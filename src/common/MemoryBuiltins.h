#pragma once

#include "IRManager.h"

#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>

/// Dynamic memory buitins info through instrumentation

/// Adapted from llvm MemoryBuiltins.cpp and llvm BuildLibCalls.cpp
///   actually, llvm don't provide stable apis so I need to copy and adapt them
/// Make sure this module fit the instrumentation convention where it is used
/// This module is experimental, not fully tested
class DynamicMemoryBuiltins {
private:
  IRManager &_irm;
public:
  DynamicMemoryBuiltins(IRManager &irm) : _irm(irm) {}
  ~DynamicMemoryBuiltins() = default;
  DynamicMemoryBuiltins(const DynamicMemoryBuiltins&) = delete;
  DynamicMemoryBuiltins& operator=(const DynamicMemoryBuiltins&) = delete;

  bool isHeapAllocationSite(llvm::CallBase *CB);
  
  /// @note realloc is also considered as a free (and an allocation)
  /// @return nullptr if not allocation call, will do instrument, ensure i64
  llvm::Value *getDynamicAllocationSize(llvm::CallBase *CB);
  /// @note realloc is also considered as a free (and an allocation)
  /// @return nullptr if not free call
  llvm::Value *getFreedOperand(const llvm::CallBase *CB);

private:
  enum MemLibFunc {
    LibFn_not_registered = 0,
// strdup/__strdup: %0 for the given string (const char*); [size] = strlen(%0) + 1
    LibFn_group_strdup = 1,
// strndup/__strndup: %0 for const char* and %1 for the minimum size; [size] = min(strlen(%0), %1) + 1
    LibFn_group_strndup,
// aligned_alloc, [size] = %1
    LibFn_aligned_alloc,
// malloc, [size] = %0
    LibFn_valloc, LibFn_malloc, LibFn_vec_malloc, LibFn_pvalloc,
// memalign, [size] = %1
    LibFn_memalign,
// calloc, vec_calloc, [size] = %0*%1
    LibFn_calloc, LibFn_vec_calloc,
// new: [size] = %0
//   other arguments: nothrow, align, hot_cold params
//   note: "new" has many mangled version
    LibFn_group_new,
// new[]: similar to new, [size] = %0 (+ 1)
    LibFn_group_new_array,
// realloc, reallocf, vec_realloc, [pointer] = %0, [size] = %1
    LibFn_realloc, LibFn_reallocf, LibFn_vec_realloc,
// reallocarray, [pointer] = %0, [size] = %1*%2
    LibFn_reallocarray,
// free, vec_free
    LibFn_free, LibFn_vec_free,
// delete: [pointer] = %0
//   other arguments: nothrow, align and the "delete size"
//   note: delete also has many mangled version
    LibFn_group_delete,
// delete[]: similar to delete
    LibFn_group_delete_array
  };
  static constexpr MemLibFunc MEMLIBFUNC_MARK_MALLOC = LibFn_group_strdup;
  static constexpr MemLibFunc MEMLIBFUNC_MARK_REALLOC = LibFn_realloc;
  static constexpr MemLibFunc MEMLIBFUNC_MARK_FREE = LibFn_free;
  static constexpr MemLibFunc MEMLIBFUNC_MARK_LAST = LibFn_group_delete_array;
  static llvm::Function* validDirectCall(const llvm::CallBase *CB) {
    if (CB->isNoBuiltin()) throw std::runtime_error("fatal");
    auto *F = CB->getCalledFunction();
    if (!F) throw std::runtime_error("fatal");
    return F;
  }
  MemLibFunc getMemLibFunc(const llvm::Function *callee);
};
