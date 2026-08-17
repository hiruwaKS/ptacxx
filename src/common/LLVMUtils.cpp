#include "LLVMUtils.h"

#include "llvm/IR/Instructions.h" 

#include <cstdlib>

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
  char *demangled = demangler.getFunctionName(nullptr, &len);
  if (!demangled || len == 0) {
    std::free(demangled);
    return mangled;
  }
  std::string result(demangled);
  std::free(demangled);
  return result;
}

std::pair<std::string, std::string> getNamespacePair(const std::string &demangled) {
  int templateDepth = 0;
  for (size_t i = 0; i < demangled.length(); ++i) {
    if (demangled[i] == '<') {
      templateDepth++;
    } else if (demangled[i] == '>') {
      if (templateDepth > 0) templateDepth--;
    } else if (templateDepth == 0 && demangled[i] == ':' && i + 1 < demangled.length() && demangled[i + 1] == ':') {
      std::string outer = demangled.substr(0, i);
      std::string inner = demangled.substr(i + 2);
      return {outer, inner};
    }
  }
  return {"", demangled};
}

bool shouldSilence(const std::string& mangledName, bool std, bool llvm, bool runtime) {
  auto demangled = getDemangledName(mangledName);
  auto ns = getNamespacePair(demangled).first;
  auto startsWith = [](const std::string& str, const std::string& prefix) {
    return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
  };
  if (std && ns == "std") return true;
  if (llvm && ns == "llvm") return true;
  if (runtime) {
    if (startsWith(demangled, "__cxa_")) return true;
    if (startsWith(demangled, "__gxx_")) return true;
    if (startsWith(demangled, "__cxx_")) return true;
    if (startsWith(demangled, "__clang_")) return true;
    if (startsWith(demangled, "__libc_")) return true;
    if (startsWith(demangled, "__glibc_")) return true;
    if (ns == "__gnu_cxx" || ns == "__cxxabiv1") return true;
    if (demangled == "__dso_handle") return true;
    if (demangled == "__tls_get_addr") return true;
  }
  return false;
}
