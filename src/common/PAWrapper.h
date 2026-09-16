#pragma once

#include "CommandLine.h"
#include "QueryInterface.h"
#include "CallGraph.h"
#include "MemoryBuiltins.h"
#include "IRManager.h"
#include "Stopwatch.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallVector.h>

#include <optional>

namespace llvm {
class InitLLVM;
}

namespace ptacxx::options {
class CGPatchCLIntercept : public CLIntercept {
private:
  bool interceptOption(const std::string &key, const std::string &value) override;
};
} // namespace ptacxx::options

// TODO: context-sensitivity support

class PAWrapper {
protected:
  /// Optional LLVM runtime initializer, set by drivers that need it (e.g. via
  /// `pInitLLVM = new llvm::InitLLVM(argc, argv);`).  It is kept alive until
  /// this server is destroyed so that llvm_shutdown() (run by InitLLVM's
  /// destructor) happens only after all pass scheduling and queries are done.
  llvm::InitLLVM *pInitLLVM = nullptr;
  IRManager _irm;

private:
  bool _allocSitesComputed;
  std::vector<AllocationSite> _allocationSites;
  llvm::DenseMap<Ptr, std::optional<PointsToSet>> _ptsCache;
  llvm::DenseMap<AliasPair, PTAliasResult> _aliasCache;
  llvm::DenseMap<llvm::Function *, llvm::SmallVector<llvm::Function *, 4>> _cgPatchOut;

  ptacxx::CallGraph _cg;
  bool _cgPatchLoaded;
public:
  PAWrapper() : _cg(_irm), _cgPatchLoaded(false) {}
  virtual ~PAWrapper();

  int run(int argc, char **argv);
protected:
  static bool mayPointTo(PointsToSetView pts1, AllocSite site) {
    if (pts1) return std::find(pts1.value().begin(), pts1.value().end(), site) != pts1.value().end();
    return true;
  }
  static llvm::AliasResult aliasByIntersection(PointsToSetView pts1, PointsToSetView pts2);

  PointsToSetView getPointsToSetCached(Ptr ptr);
  PTAliasResult getAliasResultCached(Ptr a, Ptr b);
  virtual ptacxx::PTResult getPointToResultCached(Ptr ptr, AllocSite site) = 0;
  virtual ptacxx::PTResult getPointToResultCachedSlow(Ptr ptr, AllocSite site) = 0;
  virtual llvm::SmallVector<ptacxx::CallGraph::ResolvedTarget, 4>
    indirectCallResolver(llvm::CallBase *callInst, llvm::Function *caller);

  void computeAllocationSites();
  llvm::ArrayRef<AllocationSite> getAllocationSites() const;
  llvm::ArrayRef<AllocationSite> getAllocationSites(llvm::Function *F) const;
private:
  /// parse the command line and initialize LLVM; @return the input IR path
  virtual std::string argParseAndInitLLVM(int argc, char **argv) = 0;
  virtual void init() = 0;
  virtual bool getPointsToSet(Ptr value, PointsToSet &pts) = 0;
  virtual PTAliasResult getAliasResult(Ptr a, Ptr b) = 0;

  int queryLoop();
  void emitInit(const ptacxx::Stopwatch::Record &parse,
                const ptacxx::Stopwatch::Record &load,
                const ptacxx::Stopwatch::Record &index,
                const ptacxx::Stopwatch::Record &analysis);
  void emitInitError(const std::exception &e);
  std::string handleQueryWrapper(const std::string &req);
};

class IncluPAWrapper: public PAWrapper {
public:
  IncluPAWrapper() = default;
  ptacxx::PTResult getPointToResultCached(Ptr ptr, AllocSite site) override;
  ptacxx::PTResult getPointToResultCachedSlow(Ptr ptr, AllocSite site) override;
private:
  PTAliasResult getAliasResult(Ptr a, Ptr b) override;
};

class UnifiPAWrapper: public PAWrapper {
public:
  UnifiPAWrapper() = default;
  ptacxx::PTResult getPointToResultCached(Ptr ptr, AllocSite site) override;
  ptacxx::PTResult getPointToResultCachedSlow(Ptr ptr, AllocSite site) override;
private:
  bool getPointsToSet(Ptr value, PointsToSet &pts) override;
};
