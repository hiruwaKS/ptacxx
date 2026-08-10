#pragma once

#include "IRManager.h"
#include "Common.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/ArrayRef.h>

#include <functional>
#include <unordered_set>

// TODO: context-sensitivity support in Call Graph

namespace ptacxx {

struct CallEdge {
  enum CallEdgeType {
    DIRECT = 0,
    INDIRECT,
    CALLANYTHING
  };
  CallEdgeType type;
  llvm::Function *caller;
  llvm::CallBase *callsite;
  llvm::Function *callee; // if CALLANYTHING, callee is ignored
};

class CallGraph {
private:
  IRManager &_irm;
  bool _cgBuilt = false;
  llvm::DenseMap<llvm::Function *, std::pair<size_t, size_t>> _CG;
  std::vector<CallEdge> _edges;
  
  bool _revBuilt = false;
  llvm::DenseMap<llvm::Function *, std::vector<CallEdge>> _revCG;
  std::vector<CallEdge> _callAnything;
public:
  using ResolvedTarget = std::pair<CallEdge::CallEdgeType, llvm::Function*>;
  using IndirectResolver = std::function<llvm::SmallVector<ResolvedTarget, 4>(llvm::CallBase*)>;
  /// @note ArrayRef is valid since vector (after built) is frozen
  using EdgesResult = llvm::ArrayRef<CallEdge>;
  CallGraph(IRManager &irm): _irm(irm) {}
  CallGraph(const CallGraph&) = delete;
  CallGraph& operator=(const CallGraph&) = delete;
  void buildCG(IndirectResolver indirectResolver);

  /// @brief reverse call graph will not be built until buildReverseCG is called
  void buildReverseCG();
  
  std::vector<CallEdge> reach(llvm::Function *from, llvm::Function *to) const;
  EdgesResult getOutEdges(llvm::Function *from) const;
  EdgesResult getOutEdgesAtCallSite(llvm::CallBase *from) const;
  EdgesResult getCallAnythingEdges() const;
  /// @note getInEdges + getCallAnythingEdges = all in edges
  EdgesResult getInEdges(llvm::Function *to) const;
private:
  bool reachIter(llvm::Function *from, llvm::Function *to, 
    std::vector<size_t> &path, std::unordered_set<llvm::Function *> &visited) const;
};
}
