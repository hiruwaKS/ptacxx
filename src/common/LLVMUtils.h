#pragma once

#include "Common.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Demangle/Demangle.h>

#include <utility>

llvm::LLVMContext &getThreadLocalContext();

/// @brief this will not skip all declarations, only the ones that start with "llvm."
static inline bool llvmSkip(llvm::Function *F) {
  if (F->isIntrinsic()) return true;
  llvm::StringRef Name = F->getName();
#if LLVM_VERSION_MAJOR < 15
  return Name.startswith("llvm.");
#else
  return Name.substr(0, 5) == "llvm.";
#endif
}

static inline bool llvmSkip(llvm::GlobalVariable *GV) {
  llvm::StringRef Name = GV->getName();
#if LLVM_VERSION_MAJOR < 15
  return Name.startswith("llvm.");
#else
  return Name.substr(0, 5) == "llvm.";
#endif
}

static inline llvm::Function *declFn(llvm::Module &M, const llvm::Twine &name, 
    llvm::FunctionType *FT) {
  if (auto *F = M.getFunction(name.str())) return F;
  return llvm::Function::Create(FT, llvm::GlobalValue::ExternalLinkage, 
    name, &M);
}

static inline llvm::raw_ostream& printDetailedValueId(llvm::raw_ostream &os,
    const llvm::Value *V) {
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

/// split the name to namespace and the remain, right-assoc (A::B::C -> (A,B::C))
std::pair<std::string, std::string> getNamespacePair(const std::string &demangled);

bool shouldSilence(const std::string& mangledName, bool std, bool llvm, bool runtime);
