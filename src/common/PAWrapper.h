#pragma once

#include "QueryInterface.h"
#include "CallGraph.h"
#include "MemoryBuiltins.h"
#include "IRManager.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/ArrayRef.h>

#include <optional>

// TODO: context-sensitivity support

class PAWrapper {
protected:
  IRManager &_irm;

private:
  bool _allocSitesComputed;
  std::vector<AllocationSite> _allocationSites;
  llvm::DenseMap<Ptr, std::optional<PointsToSet>> _ptsCache;
  llvm::DenseMap<AliasPair, PTAliasResult> _aliasCache;

  ptacxx::CallGraph _cg;
public:
  PAWrapper(IRManager &irm) : _irm(irm), _cg(irm) {}
  virtual ~PAWrapper();

  int run();
protected:
  static bool mayPointTo(PointsToSetView pts1, AllocSite site) {
    if (pts1) return std::find(pts1.value().begin(), pts1.value().end(), site) != pts1.value().end();
    return true;
  }
  static llvm::AliasResult aliasByIntersection(PointsToSetView pts1, PointsToSetView pts2);

  PointsToSetView getPointsToSetCached(Ptr ptr);
  PTAliasResult getAliasResultCached(Ptr a, Ptr b);

  void computeAllocationSites();
  llvm::ArrayRef<AllocationSite> getAllocationSites() const;
  llvm::ArrayRef<AllocationSite> getAllocationSites(llvm::Function *F) const;
private:
  virtual void init() = 0;
  virtual bool getPointsToSet(Ptr value, PointsToSet &pts) = 0;
  virtual PTAliasResult getAliasResult(Ptr a, Ptr b) = 0;

  std::string handleQueryWrapper(const std::string &req);

  llvm::SmallVector<ptacxx::CallGraph::ResolvedTarget, 4> indirectCallResolver(llvm::CallBase *callInst);
};

class IncluPAWrapper: public PAWrapper {
public:
  IncluPAWrapper(IRManager &irm) : PAWrapper(irm) {}
private:
  PTAliasResult getAliasResult(Ptr a, Ptr b) override;
};

class UnifiPAWrapper: public PAWrapper {
public:
  UnifiPAWrapper(IRManager &irm) : PAWrapper(irm) {}
private:
  bool getPointsToSet(Ptr value, PointsToSet &pts) override;
};
