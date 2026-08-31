#pragma once

#include "common/VId.h"

#include <stdint.h>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

class PtaHook;

struct PtrRecord {
  VId vid;
  int16_t action;
  int16_t padding;
  uint64_t ptr;
  uint64_t size; // for alloca, heap allocator, and basicblock metadata
  uint64_t padding2; // add something here?
};

constexpr size_t BUFFER_SIZE = 16*4096/sizeof(PtrRecord);
extern PtrRecord buffer[BUFFER_SIZE];
extern size_t bufferIndex;

struct BBRecordComp {
  bool operator()(const size_t &a, const size_t &b) const;
};

struct BBRecordHash {
  bool operator()(const size_t &a, const size_t &b) const;
  std::size_t operator()(const size_t &p) const;
};

/// one level of a call-context record: a function and its argument values
/// Arg: how an argument is abstracted. Numbers are classified by sign, pointers
/// by null-ness; a non-null pointer's vals hold all possible dynamic types (GEP-0)
enum ArgKind : uint8_t {
  POS_NUM, ZERO_NUM, NEG_NUM, NULL_PTR, PTR
};

/// a half-open range [start, end) into one of the flat memory pools
struct Slice {
  size_t start = static_cast<size_t>(-1); // -1 marks "uninitialized"
  size_t end = 0;
  bool uninitialized() const { return start == static_cast<size_t>(-1); }
};
using PtrTypeSlice = Slice;
using ArgSlice = Slice;
using CgRecordSlice = Slice;
using ScopeAllocaSlice = Slice;

/// a pointed-to object type: object id + size (was pair<VId, uint64_t>)
struct PtrType {
  VId vid;
  uint64_t size;
};

struct AbstractArg {
  ArgKind kind = POS_NUM;
  /// [start, end) into PtrTypePool holding this argument's u64 type/vals payloads
  Slice vals;
};

struct CgRecord {
  VId vid;
  /// [start, end) into ArgPool of AbstractArg objects
  ArgSlice args;
};

struct CgRecordComp {
  bool operator()(const size_t &a, const size_t &b) const;
};

struct CgRecordHash {
  bool operator()(const size_t &a, const size_t &b) const;
  std::size_t operator()(const size_t &p) const;
};

class PtaHook {
private:
  size_t K; // context length
  size_t BBCtxPlusOne; // bb window context length + 1
  size_t CGCtxPlusOne; // cg window context length + 1
  uint64_t mode;
  std::map<uint64_t, std::pair<VId, size_t>> ptrToVid;
  /// (Vid, K-context)
  std::unordered_map<VId, std::set<std::pair<VId, std::vector<VId>>>> pts;
  std::vector<VId> bbRecent;
  std::unordered_set<size_t, BBRecordHash, BBRecordHash> bbCoverage;
  std::vector<VId> allocaPool;
  /// sliding window of recent call-context records
  std::vector<CgRecord> cgRecent;
  std::unordered_set<size_t, CgRecordHash, CgRecordHash> cgCoverage;
  std::vector<std::pair<VId, ScopeAllocaSlice>> scopeStack;
  std::vector<AbstractArg> pendingArgs;
  std::vector<std::pair<VId, PtrTypeSlice>> callContext;
  std::unordered_map<uint64_t, std::vector<PtrType>> consMap;

  std::vector<CgRecord> cgPool;
  std::vector<AbstractArg> ArgPool;
  std::vector<PtrType> PtrTypePool;
  std::vector<uint64_t> ScopeAllocaPool;

public:
  static void init(uint64_t mode);
  static void stopAndConsume();
  static void dump();

private:
  PtaHook() = default;
  ~PtaHook() = default;
  PtaHook(const PtaHook &) = delete;
  PtaHook &operator=(const PtaHook &) = delete;
  static PtaHook &Instance() {
    static PtaHook *instance = new PtaHook();
    return *instance;
  }
private:
  friend struct BBRecordComp;
  friend struct BBRecordHash;
  friend struct CgRecordComp;
  friend struct CgRecordHash;
};

inline bool BBRecordComp::operator()(const size_t &a, const size_t &b) const {
  for (size_t i = 0; i < PtaHook::Instance().BBCtxPlusOne; ++i) {
    auto aa = PtaHook::Instance().allocaPool[a + i];
    auto bb = PtaHook::Instance().allocaPool[b + i];
    if (aa != bb) return aa < bb; // p.s. VId is signed
  }
  return false; // a == b
}

inline bool BBRecordHash::operator()(const size_t &a, const size_t &b) const {
  for (size_t i = 0; i < PtaHook::Instance().BBCtxPlusOne; ++i)
    if (PtaHook::Instance().allocaPool[a + i] != PtaHook::Instance().allocaPool[b + i]) return false;
  return true;
}

inline std::size_t BBRecordHash::operator()(const size_t &p) const {
  std::size_t h = 0;
  for (size_t i = 0; i < PtaHook::Instance().BBCtxPlusOne; ++i) {
    h = h * 131 + static_cast<std::size_t>(PtaHook::Instance().allocaPool[p + i]);
  }
  return h;
}

inline bool CgRecordComp::operator()(const size_t &a, const size_t &b) const {
  auto &pool = PtaHook::Instance().cgPool;
  auto &apool = PtaHook::Instance().ArgPool;
  auto &tpool = PtaHook::Instance().PtrTypePool;
  const size_t W = PtaHook::Instance().CGCtxPlusOne;
  for (size_t i = 0; i < W; ++i)
    if (pool[a + i].vid != pool[b + i].vid)
      return pool[a + i].vid < pool[b + i].vid;
  for (size_t i = 0; i < W; ++i) {
    const auto &la = pool[a + i];
    const auto &lb = pool[b + i];
    const size_t na = la.args.end - la.args.start;
    const size_t nb = lb.args.end - lb.args.start;
    if (na != nb) return na < nb;
    for (size_t j = 0; j < na; ++j) {
      const auto &aa = apool[la.args.start + j];
      const auto &bb = apool[lb.args.start + j];
      if (aa.kind != bb.kind) return aa.kind < bb.kind;
      if (aa.kind == PTR) {
        const size_t vsa = aa.vals.end - aa.vals.start;
        const size_t vsb = bb.vals.end - bb.vals.start;
        if (vsa != vsb) return vsa < vsb;
        for (size_t k = 0; k < vsa; ++k) {
          const auto &x = tpool[aa.vals.start + k];
          const auto &y = tpool[bb.vals.start + k];
          if (x.vid != y.vid) return x.vid < y.vid;
          if (x.size != y.size) return x.size < y.size;
        }
      }
    }
  }
  return false; // a == b
}

inline bool CgRecordHash::operator()(const size_t &a, const size_t &b) const {
  auto &pool = PtaHook::Instance().cgPool;
  auto &apool = PtaHook::Instance().ArgPool;
  auto &tpool = PtaHook::Instance().PtrTypePool;
  for (size_t i = 0; i < PtaHook::Instance().CGCtxPlusOne; ++i) {
    const auto &la = pool[a + i];
    const auto &lb = pool[b + i];
    if (la.vid != lb.vid) return false;
    const size_t na = la.args.end - la.args.start;
    const size_t nb = lb.args.end - lb.args.start;
    if (na != nb) return false;
    for (size_t j = 0; j < na; ++j) {
      const auto &aa = apool[la.args.start + j];
      const auto &bb = apool[lb.args.start + j];
      if (aa.kind != bb.kind) return false;
      if (aa.kind == PTR) {
        if (aa.vals.end - aa.vals.start != bb.vals.end - bb.vals.start) return false;
        for (size_t k = 0; k < aa.vals.end - aa.vals.start; ++k) {
          const auto &x = tpool[aa.vals.start + k];
          const auto &y = tpool[bb.vals.start + k];
          if (x.vid != y.vid || x.size != y.size) return false;
        }
      }
    }
  }
  return true;
}

inline std::size_t CgRecordHash::operator()(const size_t &p) const {
  std::size_t h = 0;
  auto &pool = PtaHook::Instance().cgPool;
  auto &apool = PtaHook::Instance().ArgPool;
  auto &tpool = PtaHook::Instance().PtrTypePool;
  for (size_t i = 0; i < PtaHook::Instance().CGCtxPlusOne; ++i) {
    const auto &lvl = pool[p + i];
    h = h * 131 + static_cast<std::size_t>(lvl.vid);
    // iterate the AbstractArg objects in ArgPool
    for (size_t j = lvl.args.start; j < lvl.args.end; ++j) {
      const auto &a = apool[j];
      h = h * 131 + static_cast<std::size_t>(a.kind);
      // numeric/null args hash by kind alone; PTR additionally hashes its type set
      if (a.kind == PTR)
        for (size_t k = a.vals.start; k < a.vals.end; ++k) {
          h = h * 131 + static_cast<std::size_t>(tpool[k].vid);
          h = h * 131 + static_cast<std::size_t>(tpool[k].size);
        }
    }
  }
  return h;
}
