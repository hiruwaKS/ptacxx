#pragma once

#include <cstdint>
#include <functional>

/**
 * [moduleIdx] 
 *   0: the module with main
 *   i: your i-th dynamic linking library (libc, libc++)
 *   the index is defined in `hooklibs.def`
 * [globalIdx]
 *   the index of global value in llvm module, BEFORE instrumentation
 * [localIdx]
 *   0 for the function or global itself
 *   then Arguments
 *   then local value in function (skip void instrument)
 *   refer to `IRManager.cpp::getLocalIdx` for more info
 */

struct VId {
  int16_t  moduleIdx = 0;
  int16_t  globalIdx = 0;
  int16_t  localIdx = 0;

  bool isMain() const {
    return !moduleIdx;
  }

  bool operator==(const VId &o) const {
    if (moduleIdx != o.moduleIdx) return false;
    if (globalIdx != o.globalIdx) return false;
    return localIdx == o.localIdx;
  }
  
  bool operator<(const VId &o) const {
    if (moduleIdx != o.moduleIdx) return moduleIdx < o.moduleIdx;
    if (globalIdx != o.globalIdx) return globalIdx < o.globalIdx;
    return localIdx < o.localIdx;
  }
};

namespace std {
template <>
struct hash<VId> {
  size_t operator()(const VId &v) const {
    return hash<int32_t>()(v.globalIdx) ^ (hash<int32_t>()(v.localIdx) << 1) ^ (hash<bool>()(v.moduleIdx) << 2);
  }
};
}
