#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include <llvm/Config/llvm-config.h>

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

#if LLVM_VERSION_MAJOR == 14
    #define LLVM_INS(it) (&*it)
#else
    #define LLVM_INS(it) (it)
#endif

llvm::Value* ensureI64(llvm::LLVMContext *_Ctx, llvm::Value *V, llvm::BasicBlock::iterator instPos);
