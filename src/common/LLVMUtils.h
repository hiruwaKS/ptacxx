#pragma once

#include "Common.h"
#include "Error.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>

#include <string>
#include <utility>

llvm::LLVMContext &getThreadLocalContext();

static inline bool llvmStartsWith(llvm::StringRef Str, llvm::StringRef Prefix) {
#if LLVM_VERSION_MAJOR >= 15
  return Str.starts_with(Prefix);
#else
  return Str.startswith(Prefix);
#endif
}

static inline bool llvmEndsWith(llvm::StringRef Str, llvm::StringRef Suffix) {
#if LLVM_VERSION_MAJOR >= 15
  return Str.ends_with(Suffix);
#else
  return Str.endswith(Suffix);
#endif
}

/// @brief this will not skip all declarations, only the ones that start with "llvm."
static inline bool llvmSkip(llvm::Function *F) {
  ASSERT(F, "assertionviolation-llvm-skip-null-function", "null function passed to llvmSkip");
  if (F->isIntrinsic()) return true;
  llvm::StringRef Name = F->getName();
  return llvmStartsWith(Name, "llvm.");
}

static inline bool llvmSkip(llvm::GlobalVariable *GV) {
  ASSERT(GV, "assertionviolation-llvm-skip-null-global", "null global variable passed to llvmSkip");
  llvm::StringRef Name = GV->getName();
  return llvmStartsWith(Name, "llvm.");
}

static inline llvm::Function *declFn(llvm::Module &M, const llvm::Twine &name, 
    llvm::FunctionType *FT) {
  const std::string Name = name.str();
  if (llvm::GlobalValue *GV = M.getNamedValue(Name)) {
    auto *F = llvm::dyn_cast<llvm::Function>(GV);
    if (!F)
      throw ptacxx::InputBitcodeError(
          "inputbitcodeerror-declfn-name-not-function",
          "name '" + Name + "' is already used by a non-function global");
    if (F->getFunctionType() != FT)
      throw ptacxx::InputBitcodeError(
          "inputbitcodeerror-declfn-type-mismatch",
          "existing function '" + Name + "' has a different type");
    return F;
  }
  return llvm::Function::Create(FT, llvm::GlobalValue::ExternalLinkage, 
    name, &M);
}

static inline llvm::raw_ostream& printDetailedValueId(llvm::raw_ostream &os,
    const llvm::Value *V) {
  ASSERT(V, "assertionviolation-print-detailed-value-id-null", "null value passed to printDetailedValueId");
  switch (V->getValueID()) {
    case llvm::Value::ArgumentVal:
      os << "Arg";
      break;
    case llvm::Value::BasicBlockVal:
      os << "Block";
      break;
    case llvm::Value::FunctionVal:
      os << "Func";
      break;
    case llvm::Value::GlobalVariableVal:
      os << "GlobalVar";
      break;
    case llvm::Value::ConstantIntVal:
      os << "ConstInt";
      break;
    case llvm::Value::ConstantFPVal:
      os << "ConstFP";
      break;
    case llvm::Value::ConstantExprVal:
      os << "ConstExpr";
      break;
    default:
      if (V->getValueID() >= llvm::Value::InstructionVal) {
        if (auto *I = llvm::dyn_cast<llvm::Instruction>(V))
          os << "Inst";
        else os << "UnknownInst";
      } else os << "Unknown";
      break;
  }
  return os;
}

#if LLVM_VERSION_MAJOR == 14
    #define LLVM_INS(it) (&*it)
#else
    #define LLVM_INS(it) (it)
#endif

llvm::Value* ensureI64(llvm::Value *V, llvm::BasicBlock::iterator instPos);

std::string getDemangledName(const std::string &mangled);

/// @return true if F is a constructor (Itanium ABI), excluding destructors
bool isCtorFunction(llvm::Function *F);/// split the name to namespace and the remain, right-assoc (A::B::C -> (A,B::C))
std::pair<std::string, std::string> getNamespacePair(const std::string &demangled);

std::string getAllNamespaceStripped(const std::string &demangled);

bool shouldSilence(const std::string& mangledName, bool std, bool llvm, bool runtime);
