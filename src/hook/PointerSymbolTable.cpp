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
        auto addr = record.ptr;
        Instance().ptrToVid[addr] = {
          record.vid, record.size};
        Instance().scopeStack.back().second.push_back(addr);
        break;
      }
      case PTR_ACTION_HEAP_ALLOCA:
      case PTR_ACTION_REGION: {
        auto addr = record.ptr;
        Instance().ptrToVid[addr] = {
          record.vid, record.size};
        break;
      }
      case PTR_ACTION_HEAP_FREE: {
        auto addr = record.ptr;
        Instance().ptrToVid.erase(addr);
        break;
      }
      case PTR_ACTION_PROBE: {
        auto addr = record.ptr;
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
          Instance().pts[record.vid]
            .insert({vid, std::move(context)});
        }
        break;
      }
      case PTR_ACTION_BEGINSCOPE: {
        Instance().scopeStack.push_back({
          record.vid,
          std::vector<uint64_t>()
        });
        break;
      }
      case PTR_ACTION_ENDSCOPE: {
        for (auto addr : Instance().scopeStack.back().second)
          Instance().ptrToVid.erase(addr);
        Instance().scopeStack.pop_back();
        break;
      }
      case PTR_ACTION_LANDING: {
        while (!Instance().scopeStack.empty() &&
                !(Instance().scopeStack.back().first == record.vid)) {
          for (auto addr : Instance().scopeStack.back().second)
            Instance().ptrToVid.erase(addr);
          Instance().scopeStack.pop_back();
        }
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
  auto vidToString = [](VId v) {
    return std::to_string(v);
  };
  std::vector<VId> keys;
  keys.reserve(Instance().pts.size());
  for (auto &[id, _] : Instance().pts) keys.push_back(id);
  std::sort(keys.begin(), keys.end());
  std::string buf;
  buf.reserve(1024*1024);
  for (auto &k : keys) {
    auto it = Instance().pts.find(k);
    if (it == Instance().pts.end()) continue;
    buf += vidToString(k);
    for (auto &[t, ctx] : it->second) {
      buf += " " + vidToString(t);
      if (!ctx.empty()) {
        buf += " {";
        for (auto &c : ctx) buf += " " + vidToString(c);
        buf += " }";
      }
    }
    buf += "\n";
    if (buf.size() >= 1024*1024) {
      fwrite(buf.data(), 1, buf.size(), f);
      buf.clear();
    }
  }
  if (!buf.empty()) fwrite(buf.data(), 1, buf.size(), f);
  fclose(f);
}
