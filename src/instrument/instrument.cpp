// don't run any middle-end optimization after running this instrument tool
//
// instrument: inject PTACXX hook calls into an LLVM module and wrap main().
//
// The input module must define main(). Mode-specific probes are inserted on
// functions/basic blocks; they call the hook runtime (__hook_init,
// __hook_push, __hook_dump, __register_globals), which is provided by
// libhook.so. main() is renamed to __orig_main and replaced by a wrapper
// that calls __hook_init(), runs the original main, then __hook_dump().
// The runtime mode is no longer passed in: libhook.so reads PTACXX_MODE.
//
// Usage:
//   instrument <ir-path> -o <out.ll|out.bc> [-mode-ptr] [-mode-bb] [-mode-cg]
//
// Modes are independent and can be combined:
//   -mode-ptr   pointer/points-to probes (args, allocas, loads, heap)
//   -mode-bb    basic-block coverage
//   -mode-cg    call-graph context (call arguments flattened and pushed)
//
// Build and run the instrumented module:
//   clang++ out.bc -o app $(llvm-config --libs --system-libs --ldflags) \
//       -lpthread -lm /path/to/libhook.so
//   PTACXX_DUMP_PATH=<prefix> ./app
//
// Runtime environment (read by libhook.so):
//   PTACXX_MODE       runtime mode bitmask (1=ptr, 2=bb, 4=cg)
//   PTACXX_DUMP_PATH  dump prefix; writes <prefix>.pts/.bb/.cg per mode
//   PTACXX_K          context sensitivity for -mode-ptr
//   PTACXX_BB_CTX     context window for -mode-bb
//   PTACXX_CG_CTX     context window for -mode-cg

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

#include <cstdlib>

using namespace llvm;

LLVM_CL_IGNORE_WARNINGS_BEGIN
static cl::opt<std::string>
    IRPath(cl::Positional, cl::desc("<ir-path>"),
           cl::Optional);
static cl::opt<std::string>
    OutPath("o", cl::desc("output path, end with .ll or .bc"),
           cl::ValueRequired, cl::Required);
static cl::opt<bool>
    ModePtr("mode-ptr", cl::desc("use pointer mode"), cl::init(false));
static cl::opt<bool>
    ModeBB("mode-bb", cl::desc("use bb mode"), cl::init(false));
static cl::opt<bool>
    ModeCG("mode-cg", cl::desc("use cg mode"), cl::init(false));

LLVM_CL_IGNORE_WARNINGS_END

int main(int argc, char *argv[]) {
  cl::ParseCommandLineOptions(argc, argv);
  auto irm = IRManager();
  irm.addMainModule(IRPath);
  if (!irm.getIRStat().hasMain)
    throw ptacxx::InputBitcodeError("inputbitcodeerror-no-main-function-found",
                                    "no main function found");
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
    functions.push_back(&F);
  }

  // 2.1 used types

  auto *VoidTy = Type::getVoidTy(Ctx);
  auto *I16Ty = Type::getInt16Ty(Ctx);
  auto *I32Ty = Type::getInt32Ty(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);
  auto *hookInitTy = FunctionType::get(VoidTy, {}, false);
  auto *hookPushTy = FunctionType::get(VoidTy, {I32Ty, I16Ty, I64Ty, I64Ty}, false);
  auto *hookDumpTy = FunctionType::get(VoidTy, {}, false);
  auto *registerGlobalsTy = FunctionType::get(I32Ty, {}, false);

  // 2.2 add decls

  auto *hookInitFn = declFn(M, "__hook_init", hookInitTy);
  auto *hookPushFn = declFn(M, "__hook_push", hookPushTy);
  auto *hookDumpFn = declFn(M, "__hook_dump", hookDumpTy);
  auto *registerGlobalsFn = declFn(M, "__register_globals", registerGlobalsTy);

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
      if (F->isDeclaration()) continue;
      auto &entryBB = F->getEntryBlock();
      for (auto &BB : *F) {
        auto firstPt = BB.getFirstInsertionPt();
        if (ModePtr || ModeCG) {
          beforeFirstPt.clear();
          for (auto &I : BB) {
            if (&I == &*firstPt) break;
            beforeFirstPt.push_back(I.getIterator());
          }
          if (&BB == &entryBB) {
            // 2.3.1 begin scope (ptr: scope opener; cg: function-entry marker)
            auto fvid = irm.valueToVId(F);
            CallInst::Create(hookPushFn, {VID(fvid), CONSTI16(PTR_ACTION_BEGINSCOPE), CONSTI64(0), CONSTI64(0)}, 
              "", LLVM_INS(firstPt));
            // 2.3.2 args
            if (ModePtr)
              for (auto &arg : F->args()) 
                if (arg.getType()->isPointerTy())
                  emitPointerProbe(&arg, firstPt);
            // 2.3.x bind constructed object address to its ctor vid (dynamic type id)
            if (ModeCG && isCtorFunction(F) && F->arg_size() > 0) {
              uint64_t ctorSize = 0;
              if (auto *ST = irm.getCtorStructType(F))
                ctorSize = DL.getTypeAllocSize(ST);
              CallInst::Create(hookPushFn, {VID(fvid), CONSTI16(PTR_ACTION_CONS),
                ensureI64(F->getArg(0), firstPt), CONSTI64(ctorSize)}, "", LLVM_INS(firstPt));
            }
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
            if (ModeCG) {
              auto emitArgFlat = [&](auto &self, Value *V) -> void {
                // flatten by-value structs into number/pointer leaves
                if (auto *ST = dyn_cast<StructType>(V->getType())) {
                  if (ST->isSized()) {
                    for (unsigned i = 0; i < ST->getNumElements(); ++i)
                      self(self, ExtractValueInst::Create(V, {i}, "", LLVM_INS(I.getIterator())));
                    return;
                  }
                }
                // leaf: size==1 marks a pointer (so hook only does type lookup on pointers)
                auto isPtr = V->getType()->isPointerTy() ? 1 : 0;
                // Only integer/pointer leaves carry a real runtime value. Anything else
                // (metadata, void, label, opaque) has no addressable representation, so
                // emit a placeholder 0 so the arg stream stays positionally aligned.
                Value *argVal = nullptr;
                if (V->getType()->isIntegerTy() || V->getType()->isPointerTy())
                  argVal = ensureI64(V, I.getIterator());
                else
                  argVal = CONSTI64(0);
                CallInst::Create(hookPushFn, {CONSTI32(0), CONSTI16(PTR_ACTION_ARG),
                  argVal, CONSTI64(isPtr)}, "", LLVM_INS(I.getIterator()));
              };
              if (auto *CB = dyn_cast<CallBase>(&I)) {
                // 2.3.x clear any stale args, then push all arguments before the
                // call; hook buffers them and consumes them at callee's BEGINSCOPE.
                CallInst::Create(hookPushFn, {CONSTI32(0), CONSTI16(PTR_ACTION_ARGCLEAR),
                  CONSTI64(0), CONSTI64(0)}, "", LLVM_INS(I.getIterator()));
                for (auto &arg : CB->args())
                  emitArgFlat(emitArgFlat, arg);
              }
            }
            if (ModePtr && I.getType()->isPointerTy()) {
              llvm::BasicBlock::iterator instPos;
              if (auto *II = dyn_cast<InvokeInst>(&I))
                instPos = II->getNormalDest()->getFirstInsertionPt();
              else if (auto *CBI = dyn_cast<CallBrInst>(&I))
                instPos = CBI->getDefaultDest()->getFirstInsertionPt();
              else
                instPos = reachFirstPt ? I.getNextNode()->getIterator() : firstPt;
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
                  if (!CB->isNoBuiltin() && CB->getCalledFunction()) {
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
        if (ModeBB) {
          auto fvid = irm.valueToVId(F);
          auto bbid = irm.valueToVId(&BB);
          const auto &instPos = firstPt;
          CallInst::Create(hookPushFn, {CONSTI32(0), CONSTI16(PTR_ACTION_BASICBLOCK),
            CONSTI64(fvid), CONSTI64(bbid)}, "", LLVM_INS(instPos));
        }
      }
    }
  }

  // 2.4 put globals to registerGlobals
  if (ModePtr) {
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
    // Register every function (declarations included) as a 1-byte code region at
    // its entry address, so a probe whose value is a function pointer falls
    // inside a known region and is recorded as a points-to edge to the target
    // function. A function pointer always points at the entry, and a function's
    // size cannot be known at the IR level, hence size = 1.
    for (auto *F : functions) {
      auto vid = irm.valueToVId(F);
      CallInst::Create(hookPushFn, {VID(vid), CONSTI16(PTR_ACTION_REGION), 
        ConstantExpr::getPtrToInt(F, I64Ty), CONSTI64(1)}, "", entryBB);
      ++cnt;
    }
    ReturnInst::Create(Ctx, ConstantInt::get(I32Ty, static_cast<uint64_t>(cnt)), entryBB);
  }

  // 2.5 wrap main

  // int main(int argc, char **argv) {
  //   __hook_init();
  //   __registerGlobals();
  //   int result = __orig_main(argc, argv);
  //   __hook_dump();
  //   return result;
  // }
  auto *origMainFn = M.getFunction("main");
  if (!origMainFn)
    throw ptacxx::InputBitcodeError("inputbitcodeerror-no-main-function", "no main function");
  auto *mainTy = origMainFn->getFunctionType();
  origMainFn->setName("__orig_main");
  auto *newMainFn = declFn(M, "main", mainTy);
  auto *entryBB = BasicBlock::Create(Ctx, "", newMainFn);
  CallInst::Create(hookInitFn, {}, "", entryBB);
  if (ModePtr) CallInst::Create(registerGlobalsFn, "", entryBB);
  llvm::SmallVector<llvm::Value *> origArgs;
  for (auto &arg : newMainFn->args())
    origArgs.push_back(&arg);
  auto *result = CallInst::Create(origMainFn, origArgs, "", entryBB);
  CallInst::Create(hookDumpFn, "", entryBB);
  if (result->getType()->isVoidTy()) ReturnInst::Create(Ctx, entryBB);
  else ReturnInst::Create(Ctx, result, entryBB);

  // 3. dump
  if (verifyModule(M, &errs())) errs() << "Module verification failed!\n";
  irm.dumpModule(OutPath);
  return 0;
}
