#pragma once

#include "common/VId.h"

#include <stdint.h>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

struct PtrRecord {
  VId vid;
  int16_t action;
  int16_t padding;
  uint64_t ptr;
  uint64_t size; // for alloca, and heap allocator
  uint64_t padding2; // add something here?
};

constexpr size_t BUFFER_SIZE = 16*4096/sizeof(PtrRecord);
extern PtrRecord buffer[BUFFER_SIZE];
extern size_t bufferIndex;

class PtaHook {
private:
  size_t K; // context length
  uint64_t mode;
  std::map<uint64_t, std::pair<VId, size_t>> ptrToVid;
  /// (Vid, K-context)
  std::unordered_map<VId, std::set<std::pair<VId, std::vector<VId>>>> pts;
  std::unordered_set<VId> bb_coverage;
  /// (function name, [alloca's pointers])
  std::vector<std::pair<VId, std::vector<uint64_t>>> scopeStack;

public:
  static void init(size_t k, uint64_t mode) { Instance().K = k; if (!Instance().mode) Instance().mode = mode; }
  static void stopAndConsume();
  static void dump(const char *dumpPath);

private:
  PtaHook() = default;
  ~PtaHook() = default;
  PtaHook(const PtaHook &) = delete;
  PtaHook &operator=(const PtaHook &) = delete;
  static PtaHook &Instance() {
    static PtaHook *instance = new PtaHook();
    return *instance;
  }
};
