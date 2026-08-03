#pragma once

#include <cstdint>
#include <functional>

/**
 * [globalIdx]
 *   the index of global value in llvm module, BEFORE instrumentation
 * [localIdx]
 *   0 for the function or global itself
 *   then Arguments
 *   then local value in function (skip void instrument)
 *   refer to `IRManager.cpp::getLocalIdx` for more info
 */

struct VId {
  int16_t  globalIdx = 0;
  int16_t  localIdx = 0;

  bool operator==(const VId &o) const {
    if (globalIdx != o.globalIdx) return false;
    return localIdx == o.localIdx;
  }
  
  bool operator<(const VId &o) const {
    if (globalIdx != o.globalIdx) return globalIdx < o.globalIdx;
    return localIdx < o.localIdx;
  }
};

namespace std {
template <>
struct hash<VId> {
  size_t operator()(const VId &v) const {
    return hash<int32_t>()(v.globalIdx) ^ (hash<int32_t>()(v.localIdx) << 1);
  }
};
}

constexpr VId VID_NOT_REGISTERED = VId{-1, -1};
