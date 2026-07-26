#pragma once

#include "common/VId.h"

#include <stdint.h>
#include <map>
#include <unordered_map>
#include <set>
#include <vector>

struct PtrRecord {
  int16_t moduleIdx;
  int16_t globalIdx;
  int16_t localIdx;
  int16_t action;
  int64_t ptr;
  int64_t size; // for alloca, and heap allocator
  int64_t padding; // add something here?
};

constexpr size_t BUFFER_SIZE = 16*4096/sizeof(PtrRecord);
extern PtrRecord buffer[BUFFER_SIZE];
extern size_t bufferIndex;
extern size_t K; // context length

class PtaHook {
private:
  std::map<uintptr_t, std::pair<VId, size_t>> ptrToVid;
  /// (Vid, K-context)
  std::unordered_map<VId, std::set<std::pair<VId, std::vector<VId>>>> pts;
  /// (function name, [alloca's pointers])
  std::vector<std::pair<VId, std::vector<uintptr_t>>> scopeStack;

public:
  static void init(size_t k) { K = k; }
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

void init();
