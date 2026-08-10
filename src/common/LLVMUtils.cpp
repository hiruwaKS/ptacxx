#include "LLVMUtils.h"

#include "llvm/IR/Instructions.h" 

using namespace llvm;

LLVM_CL_IGNORE_WARNINGS_BEGIN
inline llvm::LLVMContext &getThreadLocalContext() {
  static thread_local LLVMContext ctx; return ctx;
}
LLVM_CL_IGNORE_WARNINGS_END

Value* ensureI64(Value *V, BasicBlock::iterator instPos) {
  auto i64Ty = Type::getInt64Ty(getThreadLocalContext());
  if (V->getType() == i64Ty) return V;
  if (V->getType()->isIntegerTy())
    return CastInst::CreateSExtOrBitCast(V, i64Ty, "", LLVM_INS(instPos)); // will someone pass -1?
  if (V->getType()->isPointerTy())
    return CastInst::Create(Instruction::PtrToInt, V, i64Ty, "", LLVM_INS(instPos));
  throw std::runtime_error("fatal");
}

std::string getDemangledName(const std::string &mangled) {
  llvm::ItaniumPartialDemangler demangler;
  if (demangler.partialDemangle(mangled.c_str()) != 0) return mangled;
  size_t len = 0;
  demangler.getFunctionName(nullptr, &len);
  if (len == 0) return mangled;
  std::vector<char> buffer(len);
  demangler.getFunctionName(buffer.data(), &len);
  buffer.erase(std::remove_if(buffer.begin(), buffer.end(), ::isspace), buffer.end());
  return std::string(buffer.data());
}
