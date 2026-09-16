#include "CallGraph.h"
#include "LLVMUtils.h"
#include "Error.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>

#include <stdexcept>

using namespace llvm;
using namespace ptacxx;

void CallGraph::rebuild() {
  _cgBuilt = false;
  _revBuilt = false;
  _CG.clear();
  _edges.clear();
  _revCG.clear();
  _callAnything.clear();
}

void CallGraph::buildCG(IndirectResolver indirectResolver) {
  if (_cgBuilt) return;
  // reset partial state so a previously failed build can be retried cleanly
  _CG.clear();
  _edges.clear();
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
            SmallVector<ResolvedTarget, 4> targets = indirectResolver(callInst, &F);
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
    for (const auto &target : indirectResolver(nullptr, &F)) {
      CallEdge resolvedEdge;
      resolvedEdge.type = target.first;
      resolvedEdge.caller = &F;
      resolvedEdge.callsite = nullptr;
      resolvedEdge.callee = target.second;
      _edges.push_back(resolvedEdge);
    }
    size_t endIdx = _edges.size();
    if (startIdx != endIdx) {
      _CG[&F] = std::make_pair(startIdx, endIdx);
    }
  }
  _cgBuilt = true;
}

void CallGraph::buildReverseCG() {
  ASSERT(_cgBuilt, "assertionviolation-reverse-cg-not-built", "buildReverseCG called before buildCG");
  if (_revBuilt) return;
  for (const CallEdge &edge : _edges) {
    if (edge.type == CallEdge::CallEdgeType::CALLANYTHING)
      _callAnything.push_back(edge);
    else _revCG[edge.callee].push_back(edge);
  }
  _revBuilt = true;
}

bool CallGraph::reachIter(llvm::Function *from, llvm::Function *to, bool ignoreUnknown, 
    std::vector<size_t> &path, std::unordered_set<llvm::Function *> &visited) const {
  auto it = _CG.find(from);
  if (it == _CG.end()) return false;
  size_t start = it->second.first;
  size_t end = it->second.second;
  for (size_t i = start; i < end; ++i) {
    const CallEdge &edge = _edges[i];
    if (ignoreUnknown && edge.type == CallEdge::CALLANYTHING) continue;
    if (edge.type == CallEdge::CALLANYTHING) {
      path.push_back(i);
      return true;
    }
    llvm::Function *callee = edge.callee;
    ASSERT(callee, "assertionviolation-call-edge-null-callee", "non-CALLANYTHING edge has null callee");
    if (callee == to) {
      path.push_back(i);
      return true;
    }
    if (visited.find(callee) == visited.end()) {
      visited.insert(callee);
      path.push_back(i);
      if (reachIter(callee, to, ignoreUnknown, path, visited)) return true;
      path.pop_back();
    }
  }
  return false;
}

std::vector<CallEdge> CallGraph::reach(llvm::Function *from, llvm::Function *to, bool ignoreUnknown) const {
  ASSERT(_cgBuilt, "assertionviolation-reach-before-build-cg", "reach called before buildCG");
  ASSERT(from && to, "assertionviolation-reach-null-args", "null from/to passed to reach");
  std::unordered_set<llvm::Function*> visited;
  std::vector<size_t> path;
  if (reachIter(from, to, ignoreUnknown, path, visited)) {
    std::vector<CallEdge> result;
    for (size_t i : path) result.push_back(_edges[i]);
    return result;
  }
  return {};
}

CallGraph::EdgesResult CallGraph::getOutEdges(llvm::Function *from) const {
  ASSERT(_cgBuilt, "assertionviolation-get-out-edges-before-build-cg", "getOutEdges called before buildCG");
  auto it = _CG.find(from);
  if (it == _CG.end()) return llvm::ArrayRef<CallEdge>();
  size_t start = it->second.first;
  size_t end = it->second.second;
  ASSERT(end > start, "assertionviolation-empty-edge-range", "empty edge range recorded in call graph");
  return llvm::ArrayRef<CallEdge>(&_edges[start], end - start);
}

CallGraph::EdgesResult CallGraph::getOutEdgesAtCallSite(llvm::CallBase *callsite) const {
  ASSERT(_cgBuilt, "assertionviolation-get-out-edges-at-callsite-before-build-cg",
         "getOutEdgesAtCallSite called before buildCG");
  ASSERT(callsite, "assertionviolation-get-out-edges-at-callsite-null",
         "null callsite passed to getOutEdgesAtCallSite");
  Function *caller = callsite->getFunction();
  ASSERT(caller, "assertionviolation-callsite-no-parent", "callsite has no parent function");
  auto it = _CG.find(caller);
  if (it == _CG.end()) return llvm::ArrayRef<CallEdge>();
  size_t start = it->second.first;
  size_t end = it->second.second;
  ASSERT(end > start, "assertionviolation-empty-edge-range-at-callsite",
         "empty edge range recorded in call graph");
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
  ASSERT(_revBuilt, "assertionviolation-reverse-cg-not-built-in-edges", "getInEdges called before buildReverseCG");
  auto it = _revCG.find(to);
  if (it == _revCG.end()) return llvm::ArrayRef<CallEdge>();
  return llvm::ArrayRef<CallEdge>(it->second);
}
