//===- irHash.cpp - structural hash of LLVM IR ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// The structural-hashing approach is derived from LLVM's
// `llvm/IR/StructuralHash.cpp` (introduced in LLVM 16), backported here to the
// LLVM 14 API for this project (ptacxx).  LLVM 14 has no
// `llvm/IR/StructuralHash.h`, so the implementation below is self-contained.
//
// irHash computes a *structural* hash of an LLVM module: it covers the
// instruction structure, operand/result types, constants and globals, while
// deliberately ignoring SSA names, function/global names, debug info and
// metadata.  Two modules that differ only by naming hash the same; a module
// whose IR structure was changed by a pass hashes differently.
//
// This lets us establish *by fact* which analyzer pre-processing pipelines
// actually modify the IR: run the analyzer with --output-ir <dump>, then
// compare `irHash <original.ll>` with `irHash <dump>`.
//
// Usage:
//   irHash <ir-file> [<ir-file> ...]
//   # prints: <16-hex-hash>  <ir-file>
//
//===-===----------------------------------------------------------------===//

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/AsmParser/LLParser.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalIFunc.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdint>
#include <memory>
#include <string>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wglobal-constructors"
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#endif
static llvm::cl::list<std::string>
    InputFiles(llvm::cl::Positional, llvm::cl::desc("<ir-file>..."),
               llvm::cl::OneOrMore);
#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace {

/// order-sensitive 64-bit mix
uint64_t mix(uint64_t A, uint64_t B) {
  A ^= B + 0x9e3779b97f4a7c15 + (A << 6U) + (A >> 2U);
  return A;
}

/// avalanche the accumulated state
uint64_t finalize(uint64_t A) {
  A ^= A >> 30U;
  A *= 0xbf58476d1ce4e5b9;
  A ^= A >> 27U;
  A *= 0x94d049bb133111eb;
  A ^= A >> 31U;
  return A;
}

// operand / constant kinds
constexpr uint64_t K_GLOBAL_VALUE = 1;
constexpr uint64_t K_ARGUMENT = 2;
constexpr uint64_t K_BASIC_BLOCK = 3;
constexpr uint64_t K_INSTRUCTION = 4;
constexpr uint64_t K_CONST_INT = 5;
constexpr uint64_t K_CONST_FP = 6;
constexpr uint64_t K_CONST_N = 7;
constexpr uint64_t K_CONST_UNDEF = 8;
constexpr uint64_t K_CONST_POISON = 9;
constexpr uint64_t K_CONST_AGG_ZERO = 10;
constexpr uint64_t K_CONST_EXPR = 11;
constexpr uint64_t K_CONST_DATA_SEQ = 12;
constexpr uint64_t K_CONST_AGGREGATE = 13;
constexpr uint64_t K_BLOCK_ADDRESS = 14;
constexpr uint64_t K_CONST_OTHER = 15;
constexpr uint64_t K_OTHER = 16;

class StructuralHasher {
public:
  StructuralHasher() = default;

  uint64_t hash(const llvm::Module &M) {
    assignIds(M);
    uint64_t H = 0x9e3779b97f4a7c15;
    for (const llvm::GlobalVariable &GV : M.globals())
      H = mix(H, hashGlobal(GV));
    for (const llvm::GlobalAlias &GA : M.aliases())
      H = mix(H, hashAlias(GA));
    for (const llvm::GlobalIFunc &GI : M.ifuncs())
      H = mix(H, hashIFunc(GI));
    for (const llvm::Function &F : M) {
      if (isDebugInfoFunction(F))
        continue;
      H = mix(H, hashFunction(F));
    }
    return finalize(H);
  }

private:
  llvm::DenseMap<const llvm::Value *, uint64_t> _ids;
  llvm::DenseMap<const llvm::Type *, uint64_t> _typeDone;
  llvm::DenseSet<const llvm::Type *> _typeVisiting;
  uint64_t _nextId = 1;

  /// Debug-info intrinsics (llvm.dbg.*) only carry metadata; ignore both their
  /// declarations and their call sites so that enabling/disabling debug info
  /// does not change the structural hash.
  static bool isDebugInfoFunction(const llvm::Function &F) {
    return F.getName().startswith("llvm.dbg.");
  }

  void assignIds(const llvm::Module &M) {
    for (const llvm::GlobalVariable &GV : M.globals())
      _ids[&GV] = _nextId++;
    for (const llvm::GlobalAlias &GA : M.aliases())
      _ids[&GA] = _nextId++;
    for (const llvm::GlobalIFunc &GI : M.ifuncs())
      _ids[&GI] = _nextId++;
    for (const llvm::Function &F : M) {
      if (isDebugInfoFunction(F))
        continue;
      _ids[&F] = _nextId++;
      for (const llvm::Argument &A : F.args())
        _ids[&A] = _nextId++;
      for (const llvm::BasicBlock &BB : F) {
        _ids[&BB] = _nextId++;
        for (const llvm::Instruction &I : BB) {
          if (isDebugInfoIntrinsic(I))
            continue;
          _ids[&I] = _nextId++;
        }
      }
    }
  }

  uint64_t idOf(const llvm::Value &V) const {
    auto It = _ids.find(&V);
    return It == _ids.end() ? 0U : It->second;
  }

  static uint64_t hashAPInt(const llvm::APInt &V) {
    uint64_t H = mix(0x11U, static_cast<uint64_t>(V.getBitWidth()));
    const unsigned Words = V.getNumWords();
    const uint64_t *Data = V.getRawData();
    H = mix(H, static_cast<uint64_t>(Words));
    for (unsigned i = 0; i < Words; ++i)
      H = mix(H, Data[i]);
    return finalize(H);
  }

  // Pointer element types are deprecated in LLVM 14 but still meaningful for
  // typed pointers; read them in a warning-suppressed helper.
  static uint64_t hashPointerType(const llvm::PointerType *PT,
                                  StructuralHasher &Self) {
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    const bool Opaque = PT->isOpaque();
    llvm::Type *Elem = Opaque ? nullptr : PT->getElementType();
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    uint64_t H = mix(static_cast<uint64_t>(PT->getAddressSpace()),
                     Opaque ? 1U : 0U);
    if (Elem)
      H = mix(H, Self.hashType(Elem));
    return H;
  }

  uint64_t hashType(const llvm::Type *T) {
    if (!T)
      return 0;
    auto Done = _typeDone.find(T);
    if (Done != _typeDone.end())
      return Done->second;
    if (!_typeVisiting.insert(T).second)
      return finalize(mix(0x2fU, static_cast<uint64_t>(T->getTypeID())));

    uint64_t H = static_cast<uint64_t>(T->getTypeID());
    switch (static_cast<unsigned>(T->getTypeID())) {
    case llvm::Type::IntegerTyID:
      H = mix(H, static_cast<uint64_t>(T->getIntegerBitWidth()));
      break;
    case llvm::Type::PointerTyID:
      H = mix(H, hashPointerType(llvm::cast<llvm::PointerType>(T), *this));
      break;
    case llvm::Type::FunctionTyID: {
      const auto *FT = llvm::cast<llvm::FunctionType>(T);
      H = mix(H, FT->isVarArg() ? 1U : 0U);
      H = mix(H, hashType(FT->getReturnType()));
      for (const llvm::Type *P : FT->params())
        H = mix(H, hashType(P));
      break;
    }
    case llvm::Type::StructTyID: {
      const auto *ST = llvm::cast<llvm::StructType>(T);
      H = mix(H, ST->isPacked() ? 1U : 0U);
      H = mix(H, ST->getNumElements());
      for (const llvm::Type *E : ST->elements())
        H = mix(H, hashType(E));
      break;
    }
    case llvm::Type::ArrayTyID: {
      const auto *AT = llvm::cast<llvm::ArrayType>(T);
      H = mix(H, AT->getNumElements());
      H = mix(H, hashType(AT->getElementType()));
      break;
    }
    case llvm::Type::FixedVectorTyID:
    case llvm::Type::ScalableVectorTyID: {
      const auto *VT = llvm::cast<llvm::VectorType>(T);
      const llvm::ElementCount EC = VT->getElementCount();
      H = mix(H, static_cast<uint64_t>(EC.getKnownMinValue()));
      H = mix(H, EC.isScalable() ? 1U : 0U);
      H = mix(H, hashType(VT->getElementType()));
      break;
    }
    default:
      break;
    }
    _typeVisiting.erase(T);
    H = finalize(H);
    _typeDone[T] = H;
    return H;
  }

  uint64_t hashOperand(const llvm::Value &V) {
    if (auto *C = llvm::dyn_cast<llvm::Constant>(&V))
      return hashConstant(*C);
    if (llvm::isa<llvm::Argument>(&V))
      return finalize(
          mix(mix(K_ARGUMENT, idOf(V)), hashType(V.getType())));
    if (llvm::isa<llvm::Instruction>(&V))
      return finalize(
          mix(mix(K_INSTRUCTION, idOf(V)), hashType(V.getType())));
    if (llvm::isa<llvm::BasicBlock>(&V))
      return finalize(mix(K_BASIC_BLOCK, idOf(V)));
    return finalize(mix(mix(K_OTHER, idOf(V)), hashType(V.getType())));
  }

  uint64_t hashConstant(const llvm::Constant &C) {
    if (auto *GV = llvm::dyn_cast<llvm::GlobalValue>(&C))
      return finalize(mix(K_GLOBAL_VALUE, idOf(*GV)));
    if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(&C))
      return finalize(mix(K_CONST_INT, hashAPInt(CI->getValue())));
    if (auto *CF = llvm::dyn_cast<llvm::ConstantFP>(&C))
      return finalize(
          mix(K_CONST_FP, hashAPInt(CF->getValueAPF().bitcastToAPInt())));
    if (llvm::isa<llvm::ConstantPointerNull>(&C))
      return finalize(mix(K_CONST_N, hashType(C.getType())));
    if (llvm::isa<llvm::PoisonValue>(&C))
      return finalize(mix(K_CONST_POISON, hashType(C.getType())));
    if (llvm::isa<llvm::UndefValue>(&C))
      return finalize(mix(K_CONST_UNDEF, hashType(C.getType())));
    if (llvm::isa<llvm::ConstantAggregateZero>(&C))
      return finalize(mix(K_CONST_AGG_ZERO, hashType(C.getType())));
    if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(&C)) {
      uint64_t H = mix(K_CONST_EXPR, static_cast<uint64_t>(CE->getOpcode()));
      H = mix(H, hashType(CE->getType()));
      for (const llvm::Use &U : CE->operands())
        H = mix(H, hashConstant(*llvm::cast<llvm::Constant>(U.get())));
      return finalize(H);
    }
    if (auto *CDS = llvm::dyn_cast<llvm::ConstantDataSequential>(&C)) {
      uint64_t H = mix(K_CONST_DATA_SEQ, hashType(CDS->getElementType()));
      H = mix(H, CDS->getNumElements());
      const llvm::StringRef Raw = CDS->getRawDataValues();
      for (char Ch : Raw)
        H = mix(H, static_cast<uint64_t>(
                       static_cast<unsigned char>(Ch)));
      return finalize(H);
    }
    if (auto *CA = llvm::dyn_cast<llvm::ConstantAggregate>(&C)) {
      uint64_t H = mix(K_CONST_AGGREGATE, hashType(CA->getType()));
      for (const llvm::Use &U : CA->operands())
        H = mix(H, hashConstant(*llvm::cast<llvm::Constant>(U.get())));
      return finalize(H);
    }
    if (auto *BA = llvm::dyn_cast<llvm::BlockAddress>(&C)) {
      uint64_t H = K_BLOCK_ADDRESS;
      H = mix(H, idOf(*BA->getFunction()));
      H = mix(H, idOf(*BA->getBasicBlock()));
      return finalize(H);
    }
    return finalize(mix(mix(K_CONST_OTHER, hashType(C.getType())),
                        static_cast<uint64_t>(C.getValueID())));
  }

  uint64_t hashInstruction(const llvm::Instruction &I) {
    uint64_t H = static_cast<uint64_t>(I.getOpcode());
    H = mix(H, hashType(I.getType()));
    H = mix(H, static_cast<uint64_t>(I.getNumOperands()));
    if (auto *CI = llvm::dyn_cast<llvm::CmpInst>(&I))
      H = mix(H, static_cast<uint64_t>(CI->getPredicate()));
    if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
      H = mix(H, GEP->isInBounds() ? 1U : 0U);
      H = mix(H, hashType(GEP->getSourceElementType()));
    }
    if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
      H = mix(H, static_cast<uint64_t>(LI->getOrdering()));
      H = mix(H, LI->isVolatile() ? 1U : 0U);
    }
    if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
      H = mix(H, static_cast<uint64_t>(SI->getOrdering()));
      H = mix(H, SI->isVolatile() ? 1U : 0U);
    }
    if (auto *RMW = llvm::dyn_cast<llvm::AtomicRMWInst>(&I)) {
      H = mix(H, static_cast<uint64_t>(RMW->getOperation()));
      H = mix(H, static_cast<uint64_t>(RMW->getOrdering()));
    }
    if (auto *CX = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&I)) {
      H = mix(H, static_cast<uint64_t>(CX->getSuccessOrdering()));
      H = mix(H, static_cast<uint64_t>(CX->getFailureOrdering()));
    }
    if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
      H = mix(H, static_cast<uint64_t>(CB->getCallingConv()));
    }
    if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I)) {
      H = mix(H, static_cast<uint64_t>(Call->getTailCallKind()));
    }
    for (const llvm::Use &U : I.operands())
      H = mix(H, hashOperand(*U.get()));
    return finalize(H);
  }

  /// Debug-info intrinsics (llvm.dbg.*) only carry metadata; ignore them so
  /// that enabling/disabling debug info does not change the structural hash.
  static bool isDebugInfoIntrinsic(const llvm::Instruction &I) {
    const auto *CB = llvm::dyn_cast<llvm::CallBase>(&I);
    if (!CB)
      return false;
    const llvm::Function *F = CB->getCalledFunction();
    return F && F->getName().startswith("llvm.dbg.");
  }

  uint64_t hashFunction(const llvm::Function &F) {
    uint64_t H = 0xf0f0f0f0;
    H = mix(H, hashType(F.getFunctionType()));
    H = mix(H, static_cast<uint64_t>(F.getLinkage()));
    H = mix(H, static_cast<uint64_t>(F.getVisibility()));
    H = mix(H, F.isDeclaration() ? 1U : 0U);
    for (const llvm::BasicBlock &BB : F) {
      uint64_t Count = 0;
      uint64_t BHH = 0;
      for (const llvm::Instruction &I : BB) {
        if (isDebugInfoIntrinsic(I))
          continue;
        ++Count;
        BHH = mix(BHH, hashInstruction(I));
      }
      H = mix(H, Count);
      H = mix(H, BHH);
    }
    return finalize(H);
  }

  uint64_t hashGlobal(const llvm::GlobalVariable &GV) {
    uint64_t H = 0x6060;
    H = mix(H, hashType(GV.getValueType()));
    H = mix(H, static_cast<uint64_t>(GV.getLinkage()));
    H = mix(H, GV.isConstant() ? 1U : 0U);
    H = mix(H, GV.isThreadLocal() ? 1U : 0U);
    H = mix(H, GV.hasInitializer() ? 1U : 0U);
    if (GV.hasInitializer())
      H = mix(H, hashConstant(*GV.getInitializer()));
    return finalize(H);
  }

  uint64_t hashAlias(const llvm::GlobalAlias &GA) {
    uint64_t H = 0xa1a1;
    H = mix(H, hashType(GA.getValueType()));
    H = mix(H, static_cast<uint64_t>(GA.getLinkage()));
    H = mix(H, hashConstant(*GA.getAliasee()));
    return finalize(H);
  }

  uint64_t hashIFunc(const llvm::GlobalIFunc &GI) {
    uint64_t H = 0x1f1f;
    H = mix(H, hashType(GI.getValueType()));
    H = mix(H, hashConstant(*GI.getResolver()));
    return finalize(H);
  }
};

} // namespace

int main(int argc, char *argv[]) {
  llvm::cl::ParseCommandLineOptions(
      argc, argv, "Structural hash of LLVM IR (LLVM 14)\n");
  int Rc = 0;
  for (const std::string &Path : InputFiles) {
    llvm::LLVMContext Ctx;
    llvm::SMDiagnostic Diag;
    auto BufOrErr = llvm::MemoryBuffer::getFile(Path);
    if (!BufOrErr) {
      llvm::errs() << "irHash: cannot open " << Path << "\n";
      Rc = 1;
      continue;
    }
    llvm::MemoryBufferRef Buf = BufOrErr.get()->getMemBufferRef();
    std::unique_ptr<llvm::Module> M;
    if (llvm::isBitcode(
            reinterpret_cast<const unsigned char *>(Buf.getBufferStart()),
            reinterpret_cast<const unsigned char *>(Buf.getBufferEnd()))) {
      auto MOrErr = llvm::parseBitcodeFile(Buf, Ctx);
      if (!MOrErr) {
        llvm::errs() << "irHash: " << llvm::toString(MOrErr.takeError())
                     << "\n";
        Rc = 1;
        continue;
      }
      M = std::move(*MOrErr);
    } else {
      // Parse without the debug-info upgrade: the upgrade runs the verifier,
      // which aborts the process on the (possibly invalid) modules produced by
      // some analyzers' pre-processing passes.  We only need the structure.
      M = std::make_unique<llvm::Module>(Path, Ctx);
      llvm::SourceMgr SM;
      llvm::LLParser Parser(Buf.getBuffer(), SM, Diag, M.get(), nullptr, Ctx);
      if (Parser.Run(/*UpgradeDebugInfo=*/false)) {
        M.reset();
      }
    }
    if (!M) {
      Diag.print("irHash", llvm::errs());
      Rc = 1;
      continue;
    }
    StructuralHasher Hasher;
    const uint64_t H = Hasher.hash(*M);
    llvm::outs() << llvm::format_hex_no_prefix(H, 16) << "  " << Path << "\n";
  }
  return Rc;
}
