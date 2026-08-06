#include "common/MemoryBuiltins.h"
#include "common/LLVMUtils.h"

using namespace llvm;

bool DynamicMemoryBuiltins::isHeapAllocationSite(CallBase *CB) {
  auto F = validDirectCall(CB);
  auto libFunc = getMemLibFunc(F);
  return libFunc != LibFn_not_registered && libFunc < DynamicMemoryBuiltins::MEMLIBFUNC_MARK_FREE;
}

Value *DynamicMemoryBuiltins::getDynamicAllocationSize(CallBase *CB) {
  auto &_M = _irm.getModule();
  auto &_Ctx = getThreadLocalContext();
  auto F = validDirectCall(CB);
  auto libFunc = getMemLibFunc(F);
  switch (libFunc) {
    case LibFn_not_registered: return nullptr;
    case LibFn_group_strdup:
    case LibFn_group_strndup: {
      Function* strlenFn = _M.getFunction("strlen");
      if (!strlenFn) {
        auto sizeTy = _M.getDataLayout().getIntPtrType(_Ctx, 0);
        strlenFn = cast<Function>(_M.getOrInsertFunction("strlen",sizeTy,
#if LLVM_VERSION_MAJOR <= 14
          Type::getInt8PtrTy(_Ctx)
#else
          PointerType::get(_Ctx, 0)
#endif
        ).getCallee());
      }
      auto str = CB->getArgOperand(0);
      auto strlen = CallInst::Create(strlenFn, {str}, "", LLVM_INS(CB->getIterator()));
      auto one = ConstantInt::get(Type::getInt64Ty(_Ctx), 1);
      if (libFunc == MemLibFunc::LibFn_group_strdup) {
        // for strdup, strlen + 1
       return ensureI64(BinaryOperator::CreateAdd(strlen, one, "", LLVM_INS(CB->getIterator())), CB->getIterator());
      } else {
        // for strndup, min(strlen(s), n) + 1
        auto n = CB->getArgOperand(1);
       auto cmp = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_ULT,
         strlen, n, "", LLVM_INS(CB->getIterator()));
       auto min = SelectInst::Create(cmp, strlen, n, "", LLVM_INS(CB->getIterator()));
       return ensureI64(BinaryOperator::CreateAdd(min, one, "", LLVM_INS(CB->getIterator())), CB->getIterator());
      }
    }
    case LibFn_valloc:
    case LibFn_malloc:
    case LibFn_vec_malloc:
    case LibFn_pvalloc:
      return ensureI64(CB->getArgOperand(0), CB->getIterator());
    case LibFn_aligned_alloc:
    case LibFn_memalign: {
      return ensureI64(CB->getArgOperand(1), CB->getIterator());
    }
    case LibFn_calloc:
    case LibFn_vec_calloc: {
      auto num = CB->getArgOperand(0);
      auto size = CB->getArgOperand(1);
      return ensureI64(BinaryOperator::CreateMul(num, size, "", LLVM_INS(CB->getIterator())), CB->getIterator());
    }
    case LibFn_group_new: 
      return CB->getArgOperand(0);
    case LibFn_group_new_array: {
      auto num = CB->getArgOperand(0);
      auto one = ConstantInt::get(Type::getInt64Ty(_Ctx), 1);
      return ensureI64(BinaryOperator::CreateAdd(num, one, "", LLVM_INS(CB->getIterator())), CB->getIterator());
    }
    case LibFn_realloc:
    case LibFn_reallocf:
    case LibFn_vec_realloc:
      return ensureI64(CB->getArgOperand(1), CB->getIterator());
    case LibFn_reallocarray: {
      auto num = CB->getArgOperand(1);
      auto size = CB->getArgOperand(2);
      return ensureI64(BinaryOperator::CreateMul(num, size, "", LLVM_INS(CB->getIterator())), CB->getIterator());
    }
    case LibFn_free:
    case LibFn_vec_free:
    case LibFn_group_delete:
    case LibFn_group_delete_array:
      return nullptr;
  }
}

Value *DynamicMemoryBuiltins::getFreedOperand(const CallBase *CB) {
  auto F = validDirectCall(CB);
  auto libFunc = getMemLibFunc(F);
  if (libFunc < MEMLIBFUNC_MARK_REALLOC || libFunc > MEMLIBFUNC_MARK_LAST)
    return nullptr;
  return CB->getArgOperand(0);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
DynamicMemoryBuiltins::MemLibFunc
  DynamicMemoryBuiltins::getMemLibFunc(const Function *callee) {
  LibFunc libFunc;
  auto &_TLI = _irm.getTLI();
  bool suc = _TLI.getLibFunc(*callee, libFunc);
  if (!suc) return MemLibFunc::LibFn_not_registered;
  switch (libFunc) {
    case LibFunc_strdup:
    case LibFunc_dunder_strdup:
      return MemLibFunc::LibFn_group_strdup;
    case LibFunc_strndup:
    case LibFunc_dunder_strndup:
      return MemLibFunc::LibFn_group_strndup;
    case LibFunc_aligned_alloc:
      return MemLibFunc::LibFn_aligned_alloc;
    case LibFunc_valloc:
      return MemLibFunc::LibFn_valloc;
    case LibFunc_malloc:
      return MemLibFunc::LibFn_malloc;
    case LibFunc_vec_malloc:
      return MemLibFunc::LibFn_vec_malloc;
#if LLVM_VERSION_MAJOR > 14
    case LibFunc_pvalloc:
      return MemLibFunc::LibFn_pvalloc;
#endif
    case LibFunc_memalign:
      return MemLibFunc::LibFn_memalign;
    case LibFunc_calloc:
      return MemLibFunc::LibFn_calloc;
    case LibFunc_vec_calloc:
      return MemLibFunc::LibFn_vec_calloc;
    case LibFunc_Znwj:
    case LibFunc_Znwm:
    case LibFunc_ZnwjRKSt9nothrow_t:
    case LibFunc_ZnwmRKSt9nothrow_t:
    case LibFunc_ZnwjSt11align_val_t:
    case LibFunc_ZnwmSt11align_val_t:
    case LibFunc_ZnwjSt11align_val_tRKSt9nothrow_t:
    case LibFunc_ZnwmSt11align_val_tRKSt9nothrow_t:
#if LLVM_VERSION_MAJOR > 14
    case LibFunc_Znwm12__hot_cold_t:
    case LibFunc_ZnwmRKSt9nothrow_t12__hot_cold_t:
    case LibFunc_ZnwmSt11align_val_t12__hot_cold_t:
    case LibFunc_ZnwmSt11align_val_tRKSt9nothrow_t12__hot_cold_t:
#endif
    case LibFunc_msvc_new_int:
    case LibFunc_msvc_new_int_nothrow:
    case LibFunc_msvc_new_longlong:
    case LibFunc_msvc_new_longlong_nothrow:
      return MemLibFunc::LibFn_group_new;
    case LibFunc_Znaj:
    case LibFunc_Znam:
    case LibFunc_ZnajRKSt9nothrow_t:
    case LibFunc_ZnamRKSt9nothrow_t:
    case LibFunc_ZnajSt11align_val_t:
    case LibFunc_ZnamSt11align_val_t:
    case LibFunc_ZnajSt11align_val_tRKSt9nothrow_t:
    case LibFunc_ZnamSt11align_val_tRKSt9nothrow_t:
#if LLVM_VERSION_MAJOR > 14
    case LibFunc_Znam12__hot_cold_t:
    case LibFunc_ZnamRKSt9nothrow_t12__hot_cold_t:
    case LibFunc_ZnamSt11align_val_t12__hot_cold_t:
    case LibFunc_ZnamSt11align_val_tRKSt9nothrow_t12__hot_cold_t:
#endif
    case LibFunc_msvc_new_array_int:
    case LibFunc_msvc_new_array_int_nothrow:
    case LibFunc_msvc_new_array_longlong:
    case LibFunc_msvc_new_array_longlong_nothrow:
        return MemLibFunc::LibFn_group_new_array;
    case LibFunc_realloc:
      return MemLibFunc::LibFn_realloc;
    case LibFunc_reallocf:
      return MemLibFunc::LibFn_reallocf;
    case LibFunc_vec_realloc:
      return MemLibFunc::LibFn_vec_realloc;
#if LLVM_VERSION_MAJOR > 14
    case LibFunc_reallocarray:
      return MemLibFunc::LibFn_reallocarray;
#endif
    case LibFunc_free:
      return MemLibFunc::LibFn_free;
    case LibFunc_vec_free:
      return MemLibFunc::LibFn_vec_free;
    case LibFunc_ZdlPv:
    case LibFunc_ZdlPvj:
    case LibFunc_ZdlPvm:
    case LibFunc_ZdlPvRKSt9nothrow_t:
    case LibFunc_ZdlPvSt11align_val_t:
    case LibFunc_ZdlPvSt11align_val_tRKSt9nothrow_t:
    case LibFunc_ZdlPvjSt11align_val_t:
    case LibFunc_ZdlPvmSt11align_val_t:
    case LibFunc_msvc_delete_ptr32:
    case LibFunc_msvc_delete_ptr64:
    case LibFunc_msvc_delete_ptr32_int:
    case LibFunc_msvc_delete_ptr64_longlong:
    case LibFunc_msvc_delete_ptr32_nothrow:
    case LibFunc_msvc_delete_ptr64_nothrow:
        return MemLibFunc::LibFn_group_delete;
    case LibFunc_ZdaPv:
    case LibFunc_ZdaPvj:
    case LibFunc_ZdaPvm:
    case LibFunc_ZdaPvRKSt9nothrow_t:
    case LibFunc_ZdaPvSt11align_val_t:
    case LibFunc_ZdaPvSt11align_val_tRKSt9nothrow_t:
    case LibFunc_ZdaPvjSt11align_val_t:
    case LibFunc_ZdaPvmSt11align_val_t:
    case LibFunc_msvc_delete_array_ptr32:
    case LibFunc_msvc_delete_array_ptr64:
    case LibFunc_msvc_delete_array_ptr32_int:
    case LibFunc_msvc_delete_array_ptr64_longlong:
    case LibFunc_msvc_delete_array_ptr32_nothrow:
    case LibFunc_msvc_delete_array_ptr64_nothrow:
      return MemLibFunc::LibFn_group_delete_array;
    default:
      return MemLibFunc::LibFn_not_registered;
  }
}

#pragma clang diagnostic pop
