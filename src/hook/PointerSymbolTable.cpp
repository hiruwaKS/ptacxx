#include "PointerSymbolTable.h"
#include "common/Common.h"
#include "common/Error.h"

#include <stdexcept>
#include <algorithm>
#include <span>
#include <limits>
#include <csignal>
#include <cstdlib>

__attribute__((aligned(4096))) PtrRecord buffer[BUFFER_SIZE];

size_t bufferIndex = 0;

namespace {
// Flush buffered records and dump the CG trace on abnormal termination, so a
// case that aborts (e.g. SVF's validateSuccessTests assert on a failing test)
// still yields its callgraph dump.
volatile std::sig_atomic_t g_crashDumped = 0;
[[noreturn]] void crashDumpHandler(int sig) {
  if (g_crashDumped) _Exit(128 + sig);
  g_crashDumped = 1;
  PtaHook::stopAndConsume();
  PtaHook::dump();
  _Exit(128 + sig);
}
}  // namespace

void PtaHook::init(uint64_t mode) {
  auto &instance = Instance();
  std::signal(SIGABRT, crashDumpHandler);
  std::signal(SIGSEGV, crashDumpHandler);
  std::signal(SIGTERM, crashDumpHandler);
  if (!instance.mode) instance.mode = mode;

  if (const char *kEnv = std::getenv("PTACXX_K")) {
    char *end = nullptr;
    const uint64_t parsed = std::strtoull(kEnv, &end, 10);
    if (end && *end == '\0') {
      instance.K = static_cast<size_t>(parsed);
    } else {
      std::fprintf(stderr, "PtaHook: invalid PTACXX_K '%s', using 0\n", kEnv);
      instance.K = 0;
    }
  } else {
    instance.K = 0;
  }

  if (const char *bbCtxEnv = std::getenv("PTACXX_BB_CTX")) {
    char *end = nullptr;
    const uint64_t parsed = std::strtoull(bbCtxEnv, &end, 10);
    if (end && *end == '\0') {
      instance.BBCtxPlusOne = static_cast<size_t>(parsed) + 1;
    } else {
      std::fprintf(stderr, "PtaHook: invalid PTACXX_BB_CTX '%s', using 0\n", bbCtxEnv);
      instance.BBCtxPlusOne = 1;
    }
  } else {
    instance.BBCtxPlusOne = 1;
  }
  for (size_t i = 0; i < instance.BBCtxPlusOne; ++i)
    instance.bbRecent.push_back(static_cast<VId>(-1));

  if (const char *cgCtxEnv = std::getenv("PTACXX_CG_CTX")) {
    char *end = nullptr;
    const uint64_t parsed = std::strtoull(cgCtxEnv, &end, 10);
    if (end && *end == '\0') {
      instance.CGCtxPlusOne = static_cast<size_t>(parsed) + 1;
    } else {
      std::fprintf(stderr, "PtaHook: invalid PTACXX_CG_CTX '%s', using 0\n", cgCtxEnv);
      instance.CGCtxPlusOne = 1;
    }
  } else {
    instance.CGCtxPlusOne = 1;
  }
  for (size_t i = 0; i < instance.CGCtxPlusOne; ++i)
    instance.cgRecent.push_back({static_cast<VId>(-1), Slice{0, 0}});
}

void PtaHook::stopAndConsume(){
  if (!bufferIndex) return;
  std::span<PtrRecord> buffer_span(buffer, bufferIndex);
  for (size_t i = 0; i < bufferIndex; ++i) {
    auto &record = buffer_span[i];
    switch (record.action) {
      case PTR_ACTION_ALLOCA: {
        if (!(Instance().mode & MODE_PTR_MASK)) break;
        auto addr = record.ptr;
        ASSERT(!Instance().scopeStack.back().second.uninitialized(),
               "assertionviolation-scope-top-uninitialized", "scope top is uninitialized");
        Instance().ptrToVid[addr] = {
          record.vid, record.size};
        auto newEnd = Instance().ScopeAllocaPool.size();
        Instance().ScopeAllocaPool.push_back(addr);
        Instance().scopeStack.back().second.end = newEnd;
        break;
      }
      case PTR_ACTION_HEAP_ALLOCA:
      case PTR_ACTION_REGION: {
        if (!(Instance().mode & MODE_PTR_MASK)) break;
        auto addr = record.ptr;
        Instance().ptrToVid[addr] = {
          record.vid, record.size};
        break;
      }
      case PTR_ACTION_HEAP_FREE: {
        if (!(Instance().mode & MODE_PTR_MASK)) break;
        auto addr = record.ptr;
        Instance().ptrToVid.erase(addr);
        break;
      }
      case PTR_ACTION_PROBE: {
        if (!(Instance().mode & MODE_PTR_MASK)) break;
        auto addr = record.ptr;
        auto it = Instance().ptrToVid.upper_bound(addr);
        if (it == Instance().ptrToVid.begin()) continue;
        --it;
        auto base = it->first;
        auto [vid, size] = it->second;
        if (addr < base + size) {
          std::vector<VId> context;
          size_t contextSize = std::min(Instance().scopeStack.size(), Instance().K);
          context.resize(contextSize);
          for (size_t j = 0; j < contextSize; ++j)
            context[j] = Instance().scopeStack[j].first;
          Instance().pts[record.vid]
            .insert({vid, std::move(context)});
        }
        break;
      }
      case PTR_ACTION_ARG: {
        if (!(Instance().mode & MODE_CG_MASK)) break;
        AbstractArg a;
        // record.size==1 marks a pointer argument
        if (record.size == 1) {
          if (record.ptr == 0) {
            a.kind = NULL_PTR;
          } else {
            a.kind = PTR;
            // a non-null pointer's vals hold all possible dynamic types at `ptr`
            a.vals.start = Instance().PtrTypePool.size();
            auto &cm = Instance().consMap;
            auto it = cm.find(record.ptr);
            if (it != cm.end())
              Instance().PtrTypePool.insert(
                  Instance().PtrTypePool.end(), it->second.begin(), it->second.end());
            a.vals.end = Instance().PtrTypePool.size();
          }
        } else {
          // scalar: classify by sign
          int64_t v = static_cast<int64_t>(record.ptr);
          a.kind = v > 0 ? POS_NUM : (v < 0 ? NEG_NUM : ZERO_NUM);
        }
        Instance().pendingArgs.push_back(std::move(a));
        break;
      }
      case PTR_ACTION_ARGCLEAR: {
        if (!(Instance().mode & MODE_CG_MASK)) break;
        Instance().pendingArgs.clear();
        break;
      }
      case PTR_ACTION_CONS: {
        if (!(Instance().mode & MODE_CG_MASK)) break;
        auto &types = Instance().consMap[record.ptr];
        // drop any existing range smaller than or equal to the new construction
        types.erase(std::remove_if(types.begin(), types.end(),
                     [s = record.size](const PtrType &t) { return t.size <= s; }),
                    types.end());
        types.push_back(PtrType{record.vid, record.size});
        break;
      }
      case PTR_ACTION_BEGINSCOPE: {
        if (Instance().mode & MODE_PTR_MASK) {
          const size_t cur = Instance().ScopeAllocaPool.size();
          Instance().scopeStack.push_back(std::make_pair(record.vid, Slice{cur, cur}));
        }
        // consume the buffered arguments as this function's parameter values
        if (Instance().mode & MODE_CG_MASK) {
          auto &inst = Instance();
          // full call context: a slice of this function's arg types in PtrTypePool
          const size_t ctxStart = inst.PtrTypePool.size();
          for (const auto &a : inst.pendingArgs)
            if (a.kind == PTR)
              for (size_t k = a.vals.start; k < a.vals.end; ++k)
                inst.PtrTypePool.push_back(inst.PtrTypePool[k]);
          inst.callContext.push_back(
              {record.vid, PtrTypeSlice{ctxStart, inst.PtrTypePool.size()}});
          // sliding window: store full AbstractArgs into ArgPool as a CgRecord
          CgRecord rec;
          rec.vid = record.vid;
          rec.args.start = inst.ArgPool.size();
          inst.ArgPool.insert(
              inst.ArgPool.end(), inst.pendingArgs.begin(), inst.pendingArgs.end());
          rec.args.end = inst.ArgPool.size();
          inst.pendingArgs.clear();
          // record the sliding call-context window (mirrors bbCoverage)
          inst.cgRecent.erase(inst.cgRecent.begin());
          inst.cgRecent.push_back(rec);
          const size_t poolSize = inst.cgPool.size();
          for (size_t j = 0; j < inst.CGCtxPlusOne; ++j)
            inst.cgPool.push_back(inst.cgRecent[j]);
          if (inst.cgCoverage.find(poolSize) == inst.cgCoverage.end())
            inst.cgCoverage.insert(poolSize);
          else inst.cgPool.resize(poolSize);
        }
        break;
      }
      case PTR_ACTION_ENDSCOPE: {
        if (Instance().mode & MODE_PTR_MASK) {
          auto &scope = Instance().scopeStack.back().second;
          for (size_t k = scope.start; k < scope.end; ++k)
            Instance().ptrToVid.erase(Instance().ScopeAllocaPool[k]);
          Instance().scopeStack.pop_back();
        }
        if (Instance().mode & MODE_CG_MASK)
          Instance().callContext.pop_back();
        break;
      }
      case PTR_ACTION_LANDING: {
        if (Instance().mode & MODE_PTR_MASK) {
          while (!Instance().scopeStack.empty() &&
                  !(Instance().scopeStack.back().first == record.vid)) {
            auto &scope = Instance().scopeStack.back().second;
            for (size_t k = scope.start; k < scope.end; ++k)
              Instance().ptrToVid.erase(Instance().ScopeAllocaPool[k]);
            Instance().scopeStack.pop_back();
          }
        }
        if (Instance().mode & MODE_CG_MASK) {
          while (!Instance().callContext.empty() &&
                  !(Instance().callContext.back().first == record.vid))
            Instance().callContext.pop_back();
        }
        break;
      }
      case PTR_ACTION_BASICBLOCK: {
        if (!(Instance().mode & MODE_BB_MASK)) break;
        auto &instance = Instance();
        const VId bbVid = static_cast<VId>(record.size);
        instance.bbRecent.erase(instance.bbRecent.begin());
        instance.bbRecent.push_back(bbVid);
        const size_t allocaSize = instance.allocaPool.size();
        for (size_t j = 0; j < instance.BBCtxPlusOne; ++j)
          instance.allocaPool.push_back(instance.bbRecent[j]);
        if (instance.bbCoverage.find(allocaSize) == instance.bbCoverage.end())
          instance.bbCoverage.insert(allocaSize);
        else instance.allocaPool.resize(allocaSize);
        break;
      }
      default:
        ASSERT(false, "assertionviolation-unknown-pointer-action", "unknown action");
    }
  }
  bufferIndex = 0;
}

void PtaHook::dump() {
  const char *dumpPath = std::getenv("PTACXX_DUMP_PATH");
  if (!dumpPath || !dumpPath[0]) {
    std::fprintf(stderr, "PtaHook: PTACXX_DUMP_PATH is not set, skip dump\n");
    return;
  }
  auto vidToString = [](VId v) {
    return std::to_string(v);
  };
  constexpr size_t FLUSH_THRESHOLD = 1024 * 1024;

  if (Instance().mode & MODE_PTR_MASK) {
    std::string path = std::string(dumpPath) + ".pts";
    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
      std::fprintf(stderr, "PtaHook: cannot open dump file '%s'\n", path.c_str());
    } else {
      std::string buf;
      buf.reserve(1024*1024*2);
      buf += "pts\n";
      std::vector<VId> keys;
      keys.reserve(Instance().pts.size());
      for (auto &[id, _] : Instance().pts) keys.push_back(id);
      std::sort(keys.begin(), keys.end());
      for (auto &k : keys) {
        auto it = Instance().pts.find(k);
        if (it == Instance().pts.end()) continue;
        // v2: "key;target,ctx,ctx;target,..." — no spaces/braces, single-char
        // delimiters (';' separates targets, ',' separates a target's context).
        buf += vidToString(k);
        for (auto &[t, ctx] : it->second) {
          buf += ";" + vidToString(t);
          for (auto &c : ctx) buf += "," + vidToString(c);
        }
        buf += "\n";
        if (buf.size() >= FLUSH_THRESHOLD) {
          fwrite(buf.data(), 1, buf.size(), f);
          buf.clear();
        }
      }
      if (!buf.empty()) fwrite(buf.data(), 1, buf.size(), f);
      fclose(f);
    }
  }
  if (Instance().mode & MODE_BB_MASK) {
    std::string path = std::string(dumpPath) + ".bb";
    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
      std::fprintf(stderr, "PtaHook: cannot open dump file '%s'\n", path.c_str());
    } else {
      std::string buf;
      buf.reserve(1024*1024*2);
      buf += "basicBlock\n";
      std::vector<size_t> windows(Instance().bbCoverage.begin(), Instance().bbCoverage.end());
      std::sort(windows.begin(), windows.end(), BBRecordComp());
      for (const auto &window : windows) {
        auto it = Instance().bbCoverage.find(window);
        for (size_t i = 0; i < Instance().BBCtxPlusOne; ++i) {
          if (i) buf += " ";
          buf += vidToString(Instance().allocaPool[*it + i]);
        }
        buf += "\n";
        if (buf.size() >= FLUSH_THRESHOLD) {
          fwrite(buf.data(), 1, buf.size(), f);
          buf.clear();
        }
      }
      if (!buf.empty()) fwrite(buf.data(), 1, buf.size(), f);
      fclose(f);
    }
  }
  if (Instance().mode & MODE_CG_MASK) {
    std::string path = std::string(dumpPath) + ".cg";
    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
      std::fprintf(stderr, "PtaHook: cannot open dump file '%s'\n", path.c_str());
    } else {
      std::string buf;
      buf.reserve(1024*1024*2);
      buf += "callGraph\n";
      std::vector<size_t> windows(Instance().cgCoverage.begin(), Instance().cgCoverage.end());
      std::sort(windows.begin(), windows.end(), CgRecordComp());
      for (const auto &window : windows) {
        auto it = Instance().cgCoverage.find(window);
        for (size_t i = 0; i < Instance().CGCtxPlusOne; ++i) {
          if (i) buf += ", ";
          const auto &lvl = Instance().cgPool[*it + i];
          buf += vidToString(lvl.vid);
          for (size_t j = lvl.args.start; j < lvl.args.end; ++j) {
            const auto &a = Instance().ArgPool[j];
            buf += " ";
            switch (a.kind) {
              case POS_NUM:  buf += "POS";  break;
              case ZERO_NUM: buf += "ZERO"; break;
              case NEG_NUM:  buf += "NEG";  break;
              case NULL_PTR: buf += "NULL"; break;
              case PTR:
                buf += "{";
                for (size_t k = a.vals.start; k < a.vals.end; ++k)
                  buf += " " + std::to_string(Instance().PtrTypePool[k].vid);
                buf += " }";
                break;
            }
          }
        }
        buf += "\n";
        if (buf.size() >= FLUSH_THRESHOLD) {
          fwrite(buf.data(), 1, buf.size(), f);
          buf.clear();
        }
      }
      if (!buf.empty()) fwrite(buf.data(), 1, buf.size(), f);
      fclose(f);
    }
  }
}
