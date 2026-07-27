// don't run any middle-end optimization after running this instrument tool

#include "common/IRManager.h"
#include "common/LLVMUtils.h"
#include "common/Common.h"
#include "common/MemoryBuiltins.h"

#include "llvm/IR/Verifier.h"
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/CommandLine.h>

using namespace llvm;

LLVM_CL_IGNORE_WARNINGS_BEGIN
static cl::opt<std::string>
    IRPath(cl::Positional, cl::desc("<ir-path>"),
           cl::Optional);
static cl::list<std::string>
    StubLibs("stubl", cl::desc("stub library names"));
static cl::opt<std::string>
    LibBasePath("libbase", cl::desc("where libs can be found"),
           cl::ValueRequired);
static cl::opt<std::string>
    OutPath("o", cl::desc("output path, end with .ll or .bc"),
           cl::ValueRequired, cl::Required);
static cl::opt<std::string>
    DumpPath("dump", cl::desc("dump file path, can end with .txt"),
           cl::ValueRequired, cl::Required);
static cl::opt<int>
    KContext("context", cl::desc("K-context sensitivity"), cl::init(0));

LLVM_CL_IGNORE_WARNINGS_END

int main(int argc, char *argv[]) {
  cl::ParseCommandLineOptions(argc, argv);
  bool mainInstrument = !IRPath.empty();
  if (!mainInstrument) {
    if (StubLibs.size() != 1) report_fatal_error("see --help, one stub lib per time if in lib mode");
    outs() << "lib instrumention\n";
  } else outs() << "main instrumention\n";
  
  int16_t moduleIdx;
  auto irm = std::make_unique<IRManager>(LibBasePath);
  if (mainInstrument) moduleIdx = irm->addMainModule(IRPath);
  else moduleIdx = irm->addLibModule(StubLibs[0]);
  auto &M = irm->getModule(moduleIdx);
  auto &TLI = *irm->getModuleDataByIdx(moduleIdx)._TLI;
  auto &Ctx = M.getContext();
  auto &DL = M.getDataLayout();
  auto dynMem = 
    std::make_unique<DynamicMemoryBuiltins>(&M, &TLI);

  // 1. record all globals and functions

  std::vector<GlobalVariable *> globals;
  for (auto &GV : M.globals()) {
    if (llvmSkip(&GV)) continue;
    if (GV.isDeclaration()) continue;
    globals.push_back(&GV);
  }
  std::vector<Function *> functions;
  for (auto &F : M.functions()) {
    if (llvmSkip(&F)) continue;
    if (F.isDeclaration()) continue;
    functions.push_back(&F);
  }

  // 2.1 used types

  auto *VoidTy = Type::getVoidTy(Ctx);
  auto *I8PtrTy = Type::getInt8PtrTy(Ctx);
  auto *I16Ty = Type::getInt16Ty(Ctx);
  auto *I32Ty = Type::getInt32Ty(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);
  auto *ISizeTy = Type::getIntNTy(Ctx, sizeof(size_t) * 8);
  auto *ptrRecordTy = StructType::get(Ctx,
    {I16Ty, I16Ty, I16Ty, I16Ty, I64Ty, I64Ty, I64Ty}, false);
  auto *hookInitTy = FunctionType::get(VoidTy, {ISizeTy}, false);
  auto *hookPushTy = FunctionType::get(VoidTy, {ptrRecordTy}, false);
  auto *hookDumpTy = FunctionType::get(VoidTy, {I8PtrTy}, false);
  auto *registerGlobalsTy = FunctionType::get(I32Ty, {}, false);

  // 2.2 add decls

  auto *hookInitFn = declFn(M, "__hook_init", hookInitTy);
  auto *hookPushFn = declFn(M, "__hook_push", hookPushTy);
  auto *hookDumpFn = declFn(M, "__hook_dump", hookDumpTy);
  auto *registerGlobalsSelfFn = declFn(M, 
    Twine("__register_globals_")+Twine(moduleIdx), registerGlobalsTy);

  std::vector<Function*> registerGlobalsOthersFn;

  for (auto stubLib: StubLibs) {
    registerGlobalsOthersFn.push_back(
      declFn(M, Twine("__register_globals_")+
      Twine(IRManager::libNameToModuleIdx(stubLib)), registerGlobalsTy)
    );
  }

  auto makePtrRecord = [&](const VId &vid, int16_t action,
      Constant *f4 = nullptr, Constant *f5 = nullptr, Constant *f6 = nullptr) -> Constant * {
    auto zero = ConstantInt::get(I64Ty, 0);
    if (!f4) f4 = zero;
    else { if(f4->getType() != I64Ty) llvm::report_fatal_error("fatal"); }
    if (!f5) f5 = zero;
    else { if(f5->getType() != I64Ty) llvm::report_fatal_error("fatal"); }
    if (!f6) f6 = zero;
    else { if(f6->getType() != I64Ty) llvm::report_fatal_error("fatal"); }
    return ConstantStruct::get(ptrRecordTy, {
      ConstantInt::get(I16Ty, static_cast<uint64_t>(static_cast<int64_t>(vid.moduleIdx))),
      ConstantInt::get(I16Ty, static_cast<uint64_t>(static_cast<int64_t>(vid.globalIdx))),
      ConstantInt::get(I16Ty, static_cast<uint64_t>(static_cast<int64_t>(vid.localIdx))),
      ConstantInt::get(I16Ty, static_cast<uint64_t>(static_cast<int64_t>(action))),
      f4, f5, f6
    });
  };

 // 2.3 traverse all functions with definition
 {
    auto setPtr = [&](Value *agg, Value *val, auto instPos) {
      return InsertValueInst::Create(agg, ensureI64(&Ctx, val, instPos), {4}, "", LLVM_INS(instPos));
    };
    auto setSize = [&](Value *agg, Value *val, auto instPos) {
      return InsertValueInst::Create(agg, ensureI64(&Ctx, val, instPos), {5}, "", LLVM_INS(instPos));
    };
    auto emitPointerProbe = [&](Value *Val, llvm::BasicBlock::iterator instPos) {
      auto vid = irm->valueToVId(Val);
      auto *ptrToIntInst = new llvm::PtrToIntInst(Val, I64Ty, "", LLVM_INS(instPos));
      auto *ptrRecord = makePtrRecord(vid, PTR_ACTION_PROBE);
      auto *ptrRecordWithPtr = setPtr(ptrRecord, ptrToIntInst, instPos);
     CallInst::Create(hookPushFn, ptrRecordWithPtr, "", LLVM_INS(instPos));
   };

    std::vector<llvm::BasicBlock::iterator> beforeFirstPt;
    for (auto F : functions) {
      auto &entryBB = F->getEntryBlock();
      for (auto &BB : *F) {
        auto firstPt = BB.getFirstInsertionPt();
        beforeFirstPt.clear();
        for (auto &I : BB) {
          if (&I == &*firstPt) break;
          beforeFirstPt.push_back(I.getIterator());
        }
        if (&BB == &entryBB) {
          // 2.3.1 begin scope
          auto fvid = irm->valueToVId(F);
          auto *fptrRecord = makePtrRecord(fvid, PTR_ACTION_BEGINSCOPE);
          CallInst::Create(hookPushFn, fptrRecord, "", LLVM_INS(firstPt));
          // 2.3.2 args
          for (auto &arg : F->args()) 
            if (arg.getType()->isPointerTy())
              emitPointerProbe(&arg, firstPt);
        }
        auto beforePtIt = beforeFirstPt.begin();
        auto afterPtIt = firstPt;
        llvm::BasicBlock::iterator it;
        bool reachFirstPt = false;
        bool handledLandingPad = false;
        do {
          reachFirstPt = reachFirstPt || beforePtIt == beforeFirstPt.end();
          it = reachFirstPt ? afterPtIt : *beforePtIt;
          if (it == BB.end()) break;
          auto &I = *it;
          if (I.getType()->isPointerTy()) {
            auto instPos = reachFirstPt ? I.getNextNode()->getIterator() : firstPt;
            if (isa<AllocaInst>(I)) {
              // 2.3.3 alloca case (no probe)
              auto *AI = cast<AllocaInst>(&I);
              auto allocaid = irm->valueToVId(AI);
              size_t sizeMultipiler = DL.getTypeStoreSize(AI->getAllocatedType());
              Value* varMultipiler = nullptr;
              if (AI->isArrayAllocation()) {
                if (auto *constSize = dyn_cast<ConstantInt>(AI->getArraySize())) {
                  sizeMultipiler = static_cast<size_t>(constSize->getSExtValue());
                } else varMultipiler = AI->getArraySize();
              }
              auto *ptrRecord = makePtrRecord(allocaid, PTR_ACTION_ALLOCA,
                nullptr,
                varMultipiler ? nullptr : ConstantInt::get(I64Ty, sizeMultipiler));
              Value* ptrRecordWithSize = ptrRecord;
              if (varMultipiler) {
                auto multiplier = ConstantInt::get(I64Ty, sizeMultipiler);
                auto mul = BinaryOperator::Create(
                Instruction::Mul, multiplier, varMultipiler, "", LLVM_INS(instPos));
                ptrRecordWithSize = setSize(ptrRecord, mul, instPos);
              }
              auto *ptrRecordWithPtrSize = setPtr(ptrRecordWithSize, &I, instPos);
              CallInst::Create(hookPushFn, ptrRecordWithPtrSize, "", LLVM_INS(instPos));
            } else {
              // 2.3.4 probe case
              emitPointerProbe(&I, instPos);
              if (auto *CB = dyn_cast<CallBase>(&I)) {
                if (CB->isNoBuiltin() || !CB->getCalledFunction()) continue;
                // 2.3.5 heap alloca case
                if (auto size = dynMem->getDynamicAllocationSize(CB)) {
                  auto cbid = irm->valueToVId(CB);
                  auto *ptrRecord = makePtrRecord(cbid, PTR_ACTION_HEAP_ALLOCA);
                  auto *ptrRecordWithSize = setSize(ptrRecord, size, instPos);
                  auto *ptrRecordWithPtrSize = setPtr(ptrRecordWithSize, CB, instPos);
                  CallInst::Create(hookPushFn, ptrRecordWithPtrSize, "", LLVM_INS(instPos));
                } else if (auto freedPtr = dynMem->getFreedOperand(CB)) {
                  auto cbid = irm->valueToVId(CB);
                  auto *ptrRecord = makePtrRecord(cbid, PTR_ACTION_HEAP_FREE);
                  auto *ptrRecordWithPtr = setPtr(ptrRecord, freedPtr, instPos);
                  CallInst::Create(hookPushFn, ptrRecordWithPtr, "", LLVM_INS(instPos));
                }
              }
            }
          }
          // 2.3.6 landing pad case
          if (!handledLandingPad &&
              (isa<LandingPadInst>(I) || isa<CatchPadInst>(I) || isa<CleanupPadInst>(I))) {
            auto fvid = irm->valueToVId(F);
            auto *ptrRecord = makePtrRecord(fvid, PTR_ACTION_LANDING);
            CallInst::Create(hookPushFn, ptrRecord, "", LLVM_INS(firstPt));
            handledLandingPad = true;
          }
          if (!reachFirstPt) { ++beforePtIt; }
          else ++afterPtIt;
        } while (true);
      }
    }
  }

  // 2.4 put globals to registerGlobals
  {
    auto *entryBB = BasicBlock::Create(Ctx, "", registerGlobalsSelfFn);
    int cnt = 0;
    for (auto *GV : globals) {
      auto vid = irm->valueToVId(GV);
      uint64_t globalSize = 0;
      Type *Ty = GV->getValueType();
      if (Ty->isSized()) globalSize = DL.getTypeAllocSize(Ty);
      else {
        errs() << "[Warning] Global has unsized type: " << GV->getName() << "\n";
        continue;
      }
      auto *ptrRecord = makePtrRecord(vid, PTR_ACTION_REGION,
        ConstantExpr::getPtrToInt(GV, I64Ty),
        ConstantInt::get(I64Ty, globalSize)
      );
      CallInst::Create(hookPushFn, ptrRecord, "", entryBB);
      ++cnt;
    }
    ReturnInst::Create(Ctx, ConstantInt::get(I32Ty, static_cast<uint64_t>(cnt)), entryBB);
  }

  // 2.5 wrap main

  if (!moduleIdx) {
    // int main(int argc, char **argv) {
    //   __hook_init(K);
    //   __registerGlobals_0();
    //   __registerGlobals_...();
    //   int result = __orig_main(argc, argv);
    //   __hook_dump(dump_path);
    //   return result;
    // }
    auto *origMainFn = M.getFunction("main");
    if (!origMainFn) report_fatal_error("no main function");
    auto *mainTy = origMainFn->getFunctionType();
    origMainFn->setName("__orig_main");
    auto *newMainFn = declFn(M, "main", mainTy);
    auto *entryBB = BasicBlock::Create(Ctx, "", newMainFn);
    auto KCon = ConstantInt::get(ISizeTy, static_cast<uint64_t>(KContext));
    CallInst::Create(hookInitFn, KCon, "", entryBB);
    CallInst::Create(registerGlobalsSelfFn, "", entryBB);
    for (auto registerGlobalsOther: registerGlobalsOthersFn) 
      CallInst::Create(registerGlobalsOther, "", entryBB);
    llvm::SmallVector<llvm::Value *> origArgs;
    for (auto &arg : newMainFn->args())
      origArgs.push_back(&arg);
    auto *result = CallInst::Create(origMainFn, origArgs, "", entryBB);
    auto *dumpPath = ConstantDataArray::getString(Ctx, DumpPath);
    auto *dumpPathGlobal = new GlobalVariable(M, dumpPath->getType(), true, 
      GlobalVariable::PrivateLinkage, dumpPath, ".dump_path");
    auto *dumpPathPtr = ConstantExpr::getBitCast(dumpPathGlobal, I8PtrTy);
    CallInst::Create(hookDumpFn, dumpPathPtr, "", entryBB);
    if (result->getType()->isVoidTy()) ReturnInst::Create(Ctx, entryBB);
    else ReturnInst::Create(Ctx, result, entryBB);
  }

  // 3. dump
  if (verifyModule(M, &errs())) errs() << "Module verification failed!\n";
  irm->dumpModule(moduleIdx, OutPath);
  return 0;
}
