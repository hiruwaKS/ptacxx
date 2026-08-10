#include "CallGraph.h"
#include "LLVMUtils.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <stdexcept>


using namespace llvm;
using namespace ptacxx;

void CallGraph::buildCG(IndirectResolver indirectResolver) {
  if (_cgBuilt) return;
  for (Function &F : _irm.getModule()) {
    if (llvmSkip(&F)) continue;
    if (F.isDeclaration()) continue;
    size_t startIdx = _edges.size();
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *callInst = dyn_cast<CallBase>(&I)) {
          CallEdge edge;
          edge.caller = &F;
          edge.callsite = callInst;
          if (Function *directCallee = callInst->getCalledFunction()) {
            if (llvmSkip(directCallee)) continue;
            edge.type = CallEdge::DIRECT;
            edge.callee = directCallee;
            _edges.push_back(edge);
          } else {
            SmallVector<ResolvedTarget, 4> targets = indirectResolver(callInst);
            for (const auto &target : targets) {
              CallEdge resolvedEdge = edge;
              resolvedEdge.type = target.first;
              resolvedEdge.callee = target.second;
              _edges.push_back(resolvedEdge);
            }
          }
        }
      }
    }    
    size_t endIdx = _edges.size();
    if (startIdx != endIdx) {
      _CG[&F] = std::make_pair(startIdx, endIdx);
    }
  }
  _cgBuilt = true;
}

void CallGraph::buildReverseCG() {
  assert(_cgBuilt);
  if (_revBuilt) return;
  for (const CallEdge &edge : _edges) {
    if (edge.type == CallEdge::CallEdgeType::CALLANYTHING)
      _callAnything.push_back(edge);
    else _revCG[edge.callee].push_back(edge);
  }
  _revBuilt = true;
}

bool CallGraph::reachIter(llvm::Function *from, llvm::Function *to, 
    std::vector<size_t> &path, std::unordered_set<llvm::Function *> &visited) const {
  auto it = _CG.find(from);
  if (it == _CG.end()) return false;
  size_t start = it->second.first;
  size_t end = it->second.second;
  for (size_t i = start; i < end; ++i) {
    const CallEdge &edge = _edges[i];
    if (edge.type == CallEdge::CALLANYTHING) {
      path.push_back(i);
      return true;
    }
    llvm::Function *callee = edge.callee;
    assert(callee);
    if (callee == to) {
      path.push_back(i);
      return true;
    }
    if (visited.find(callee) == visited.end()) {
      visited.insert(callee);
      path.push_back(i);
      if (reachIter(callee, to, path, visited)) return true;
      path.pop_back();
    }
  }
  return false;
}

std::vector<CallEdge> CallGraph::reach(llvm::Function *from, llvm::Function *to) const {
  assert(_cgBuilt);
  assert(from && to);
  std::unordered_set<llvm::Function*> visited;
  std::vector<size_t> path;
  if (reachIter(from, to, path, visited)) {
    std::vector<CallEdge> result;
    for (size_t i : path) result.push_back(_edges[i]);
    return result;
  }
  return {};
}

CallGraph::EdgesResult CallGraph::getOutEdges(llvm::Function *from) const {
  assert(_cgBuilt);
  auto it = _CG.find(from);
  if (it == _CG.end()) return llvm::ArrayRef<CallEdge>();
  size_t start = it->second.first;
  size_t end = it->second.second;
  assert(end > start);
  return llvm::ArrayRef<CallEdge>(&_edges[start], end - start);
}

CallGraph::EdgesResult CallGraph::getOutEdgesAtCallSite(llvm::CallBase *callsite) const {
  assert(_cgBuilt&&callsite);
  Function *caller = callsite->getFunction();
  assert(caller);
  auto it = _CG.find(caller);
  if (it == _CG.end()) return llvm::ArrayRef<CallEdge>();
  size_t start = it->second.first;
  size_t end = it->second.second;
  assert(end > start);
  size_t siteStart = end;
  for (size_t i = start; i < end; ++i) {
    if (_edges[i].callsite == callsite) {
      siteStart = i;
      break;
    }
  }
  if (siteStart == end) return llvm::ArrayRef<CallEdge>();
  size_t siteEnd = end;
  for (size_t i = siteStart+1; i < end; ++i) {
    if (_edges[i].callsite != callsite) {
      siteEnd = i;
      break;
    }
  }
  if (siteStart == siteEnd) return llvm::ArrayRef<CallEdge>();
  return llvm::ArrayRef<CallEdge>(&_edges[siteStart], siteEnd - siteStart);
}

CallGraph::EdgesResult CallGraph::getCallAnythingEdges() const {
  return llvm::ArrayRef<CallEdge>(_callAnything);
}

CallGraph::EdgesResult CallGraph::getInEdges(llvm::Function *to) const {
  assert(_revBuilt);
  auto it = _revCG.find(to);
  if (it == _revCG.end()) return llvm::ArrayRef<CallEdge>();
  return llvm::ArrayRef<CallEdge>(it->second);
}
