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
  auto irm = IRManager();
  irm.addMainModule(IRPath);
  if (!irm.getIRStat().hasMain) throw std::runtime_error("no main function found");
  auto &M = irm.getModule();
  auto &Ctx = M.getContext();
  auto &DL = M.getDataLayout();
  auto dynMem = DynamicMemoryBuiltins(irm);

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
#if LLVM_VERSION_MAJOR <= 14
  auto *I8PtrTy = Type::getInt8PtrTy(Ctx);
#else
  auto *I8PtrTy = PointerType::get(Ctx, 0);
#endif
  auto *I16Ty = Type::getInt16Ty(Ctx);
  auto *I32Ty = Type::getInt32Ty(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);
  auto *ISizeTy = Type::getIntNTy(Ctx, sizeof(size_t) * 8);
  auto *hookInitTy = FunctionType::get(VoidTy, {ISizeTy}, false);
  auto *hookPushTy = FunctionType::get(VoidTy, {I32Ty, I16Ty, I64Ty, I64Ty}, false);
  auto *hookDumpTy = FunctionType::get(VoidTy, {I8PtrTy}, false);
  auto *registerGlobalsTy = FunctionType::get(I32Ty, {}, false);

  // 2.2 add decls

  auto *hookInitFn = declFn(M, "__hook_init", hookInitTy);
  auto *hookPushFn = declFn(M, "__hook_push", hookPushTy);
  auto *hookDumpFn = declFn(M, "__hook_dump", hookDumpTy);
  auto *registerGlobalsFn = declFn(M, "__register_globals", registerGlobalsTy);

  std::vector<Function*> registerGlobalsOthersFn;

  // 2.3 traverse all functions with definition
  {
#define CONSTI16(val) ConstantInt::get(I16Ty, static_cast<uint64_t>(static_cast<int64_t>(val)))
#define CONSTI32(val) ConstantInt::get(I32Ty, static_cast<uint64_t>(static_cast<int64_t>(val)))
#define CONSTI64(val) ConstantInt::get(I64Ty, static_cast<uint64_t>(static_cast<int64_t>(val)))
#define VID(vid) CONSTI32(vid)
    auto emitPointerProbe = [&](Value *Val, llvm::BasicBlock::iterator instPos) {
      auto vid = irm.valueToVId(Val);
      auto *ptrToIntInst = new llvm::PtrToIntInst(Val, I64Ty, "", LLVM_INS(instPos));
      CallInst::Create(hookPushFn, {VID(vid), CONSTI16(PTR_ACTION_PROBE), ptrToIntInst, CONSTI64(0)}, "", 
        LLVM_INS(instPos));
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
          auto fvid = irm.valueToVId(F);
          CallInst::Create(hookPushFn, {VID(fvid), CONSTI16(PTR_ACTION_BEGINSCOPE), CONSTI64(0), CONSTI64(0)}, 
            "", LLVM_INS(firstPt));
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
              auto allocaid = irm.valueToVId(AI);
              size_t sizeMultipiler = DL.getTypeStoreSize(AI->getAllocatedType());
              Value* varMultipiler = nullptr;
              if (AI->isArrayAllocation()) {
                if (auto *constSize = dyn_cast<ConstantInt>(AI->getArraySize())) {
                  sizeMultipiler = static_cast<size_t>(constSize->getSExtValue());
                } else varMultipiler = AI->getArraySize();
              }
              Value *sizeVal;
              if (varMultipiler) {
                auto multiplier = ConstantInt::get(I64Ty, sizeMultipiler);
                sizeVal = BinaryOperator::Create(
                  Instruction::Mul, multiplier, varMultipiler, "", LLVM_INS(instPos));
              } else {
                sizeVal = ConstantInt::get(I64Ty, sizeMultipiler);
              }
              auto *ptrVal = ensureI64(&I, instPos);
              CallInst::Create(hookPushFn, {VID(allocaid), CONSTI16(PTR_ACTION_ALLOCA), ptrVal, sizeVal}, 
                "", LLVM_INS(instPos));
            } else {
              // 2.3.4 probe case
              emitPointerProbe(&I, instPos);
              if (auto *CB = dyn_cast<CallBase>(&I)) {
                if (CB->isNoBuiltin() || !CB->getCalledFunction()) continue;
                // 2.3.5 heap alloca case
                if (auto size = dynMem.getDynamicAllocationSize(CB)) {
                  auto cbid = irm.valueToVId(CB);
                  CallInst::Create(hookPushFn, {VID(cbid), CONSTI16(PTR_ACTION_HEAP_ALLOCA), 
                    ensureI64(CB, instPos), size}, "", LLVM_INS(instPos));
                } else if (auto freedPtr = dynMem.getFreedOperand(CB)) {
                  auto cbid = irm.valueToVId(CB);
                  CallInst::Create(hookPushFn, {VID(cbid), CONSTI16(PTR_ACTION_HEAP_FREE), 
                    ensureI64(freedPtr, instPos), CONSTI64(0)}, "", LLVM_INS(instPos));
                }
              }
            }
          }
          // 2.3.6 landing pad case
          if (!handledLandingPad &&
              (isa<LandingPadInst>(I) || isa<CatchPadInst>(I) || isa<CleanupPadInst>(I))) {
            auto fvid = irm.valueToVId(F);
            CallInst::Create(hookPushFn, {VID(fvid), CONSTI16(PTR_ACTION_LANDING), CONSTI64(0), CONSTI64(0)}, 
              "", LLVM_INS(firstPt));
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
    auto *entryBB = BasicBlock::Create(Ctx, "", registerGlobalsFn);
    int cnt = 0;
    for (auto *GV : globals) {
      auto vid = irm.valueToVId(GV);
      uint64_t globalSize = 0;
      Type *Ty = GV->getValueType();
      if (Ty->isSized()) globalSize = DL.getTypeAllocSize(Ty);
      else {
        errs() << "[Warning] Global has unsized type: " << GV->getName() << "\n";
        continue;
      }
      CallInst::Create(hookPushFn, {VID(vid), CONSTI16(PTR_ACTION_REGION), 
        ConstantExpr::getPtrToInt(GV, I64Ty), ConstantInt::get(I64Ty, globalSize)}, "", entryBB);
      ++cnt;
    }
    ReturnInst::Create(Ctx, ConstantInt::get(I32Ty, static_cast<uint64_t>(cnt)), entryBB);
  }

  // 2.5 wrap main

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
  CallInst::Create(registerGlobalsFn, "", entryBB);
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

  // 3. dump
  if (verifyModule(M, &errs())) errs() << "Module verification failed!\n";
  irm.dumpModule(OutPath);
  return 0;
}
