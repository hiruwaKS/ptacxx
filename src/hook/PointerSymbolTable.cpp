#include "PointerSymbolTable.h"
#include "common/Common.h"

#include <stdexcept>
#include <algorithm>
#include <span>

__attribute__((aligned(4096))) PtrRecord buffer[BUFFER_SIZE];

size_t bufferIndex = 0;
size_t K;

void PtaHook::stopAndConsume(){
  if (!bufferIndex) return;
  for (size_t i = 0; i < bufferIndex; ++i) {
    std::span<PtrRecord> buffer_span(buffer, bufferIndex);
    auto &record = buffer_span[i];
    switch (record.action) {
      case PTR_ACTION_ALLOCA: {
        auto addr = static_cast<uintptr_t>(record.ptr);
        Instance().ptrToVid[addr] = {
          VId{record.moduleIdx, record.globalIdx, record.localIdx}, record.size};
        Instance().scopeStack.back().second.push_back(addr);
        break;
      }
      case PTR_ACTION_HEAP_ALLOCA:
      case PTR_ACTION_REGION: {
        auto addr = static_cast<uintptr_t>(record.ptr);
        Instance().ptrToVid[addr] = {
          VId{record.moduleIdx, record.globalIdx, record.localIdx}, record.size};
        break;
      }
      case PTR_ACTION_HEAP_FREE: {
        auto addr = static_cast<uintptr_t>(record.ptr);
        Instance().ptrToVid.erase(addr);
        break;
      }
      case PTR_ACTION_PROBE: {
        auto addr = static_cast<uintptr_t>(record.ptr);
        auto it = Instance().ptrToVid.upper_bound(addr);
        if (it == Instance().ptrToVid.begin()) continue;
        --it;
        auto base = it->first;
        auto [vid, size] = it->second;
        if (addr < base + size) {
          std::vector<VId> context;
          size_t contextSize = std::min(Instance().scopeStack.size(), K);
          context.resize(contextSize);
          for (size_t j = 0; j < contextSize; ++j)
            context[j] = Instance().scopeStack[j].first;
          Instance().pts[
            VId{record.moduleIdx, record.globalIdx, record.localIdx}]
            .insert({vid, std::move(context)});
        }
        break;
      }
      case PTR_ACTION_BEGINSCOPE: {
        Instance().scopeStack.push_back({
          VId{record.moduleIdx, record.globalIdx, record.localIdx},
          std::vector<uintptr_t>()
        });
        break;
      }
      case PTR_ACTION_ENDSCOPE: {
        for (auto addr : Instance().scopeStack.back().second)
          Instance().ptrToVid.erase(addr);
        Instance().scopeStack.pop_back();
        break;
      }
      default:
        throw std::runtime_error("Unknown action");
    }
  }
  bufferIndex = 0;
}

void PtaHook::dump(const char *dumpPath) {
  FILE *f = fopen(dumpPath, "w");
  if (!f) {
    std::fprintf(stderr, "PtaHook: cannot open dump file '%s'\n", dumpPath);
    return;
  }
  std::vector<std::pair<VId, std::set<std::pair<VId, std::vector<VId>>>>>
    sorted_vec(Instance().pts.begin(), Instance().pts.end());
  std::sort(sorted_vec.begin(), sorted_vec.end());
  for (auto &[id, targets] : sorted_vec) {
    std::fprintf(f, "%d:%d:%d", id.moduleIdx, id.globalIdx, id.localIdx);
    for (auto &t : targets)
      std::fprintf(f, " %d:%d:%d", t.first.moduleIdx, t.first.globalIdx, t.first.localIdx);
    std::fprintf(f, "\n");
  }
  fclose(f);
}
