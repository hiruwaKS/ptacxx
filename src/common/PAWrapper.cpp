#include "QueryInterface.h"
#include "PAWrapper.h"
#include "LLVMUtils.h"
#include "Common.h"

#include "llvm/ADT/SmallPtrSet.h"

#include <iostream>
#include <algorithm>
#include <queue>

namespace ptacxx::options {
/// @note this will gather all options that used in PAWrapper, defined dispersedly
extern std::string CGPatchPath;
extern std::string NoteFolderPath;
bool CGPatchCLIntercept::interceptOption(const std::string &key,
                                         const std::string &value) {
  if (key == "cgpatch-path") {
    CGPatchPath = value;
    return true;
  }
  if (key == "note-folder") {
    NoteFolderPath = value;
    return true;
  }
  return false;
}
} // namespace ptacxx::options

PAWrapper::~PAWrapper() = default;

void PAWrapper::computeAllocationSites() {
  if (_allocSitesComputed) return;
  auto builtins = DynamicMemoryBuiltins(_irm);
  auto &_M = _irm.getModule();
  for (auto &GV : _M.globals()) {
    if (llvmSkip(&GV)) continue;
    AllocationSite site;
    site.type = AllocationSite::GLOBAL;
    site.site = &GV;
    _allocationSites.push_back(site);
  }
  for (auto &F : _M) {
    if (F.isDeclaration()) continue;
    if (llvmSkip(&F)) continue;
    AllocationSite funcSite;
    funcSite.type = AllocationSite::FUNCTION;
    funcSite.site = &F;
    _allocationSites.push_back(funcSite);
    for (auto &BB : F) {
      for (auto &I : BB) {
        AllocationSite site;
        if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
          auto func = CB->getCalledFunction();
          if (!func || llvmSkip(func)) continue;
          if (builtins.isHeapAllocationSite(CB)) {
            site.type = AllocationSite::HEAP;
            site.site = CB;
            _allocationSites.push_back(site);
          }
        }
        else if (llvm::isa<llvm::AllocaInst>(&I)) {
          site.type = AllocationSite::STACK;
          site.site = &I;
          _allocationSites.push_back(site);
        }
      }
    }
  }
  _allocSitesComputed = true;
}

llvm::ArrayRef<AllocationSite> PAWrapper::getAllocationSites() const {
  assert(_allocSitesComputed);
  return llvm::ArrayRef<AllocationSite>(_allocationSites);
}

llvm::ArrayRef<AllocationSite> PAWrapper::getAllocationSites(llvm::Function *F) const {
  assert(_allocSitesComputed);
  size_t start = _allocationSites.size();
  for (size_t i = 0; i < _allocationSites.size(); ++i) {
    const AllocationSite &site = _allocationSites[i];
    if (site.type == AllocationSite::STACK || site.type == AllocationSite::HEAP)
      if (auto *I = llvm::dyn_cast<llvm::Instruction>(site.site))
        if (I->getFunction() == F) { start = i; break; }
  }
  if (start == _allocationSites.size()) return llvm::ArrayRef<AllocationSite>();
  size_t end = _allocationSites.size();
  for (size_t i = start + 1; i < _allocationSites.size(); ++i) {
    const AllocationSite &site = _allocationSites[i];
    bool found = true;
    if (site.type == AllocationSite::STACK || site.type == AllocationSite::HEAP)
      if (auto *I = llvm::dyn_cast<llvm::Instruction>(site.site))
        if (I->getFunction() == F) found = false;
    if (found) { end = i; break; }
  }
  if (start == end) return llvm::ArrayRef<AllocationSite>();
  return llvm::ArrayRef<AllocationSite>(&_allocationSites[start], end - start);
}

PointsToSetView PAWrapper::getPointsToSetCached(Ptr ptr) {
  auto it = _ptsCache.find(ptr);
  if (it != _ptsCache.end()) return it->second;
  PointsToSet pts;
  bool valid = getPointsToSet(ptr, pts);
  if (valid) return _ptsCache[ptr] = pts;
  return _ptsCache[ptr] = std::nullopt;
}

PTAliasResult PAWrapper::getAliasResultCached(Ptr a, Ptr b) {
  auto pair = reinterpret_cast<uint64_t>(a) > reinterpret_cast<uint64_t>(b) ? 
    std::pair<Ptr, Ptr>{b, a} : std::pair<Ptr, Ptr>{a, b};
  
  auto it = _aliasCache.find(pair);
  if (it != _aliasCache.end()) 
    return it->second;
  auto result = getAliasResult(a, b);
  auto [insertedIt, _] = _aliasCache.try_emplace(pair, result);
  return insertedIt->second;
}

std::string PAWrapper::handleQueryWrapper(const std::string &req) {
  PAResponse response;
  try {
    PAQuery query = parse(req, _irm);
    response = std::visit([&](const auto &arg) -> PAResponse {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, IRParseMessage> || std::is_same_v<T, IRParseError>
        || std::is_same_v<T, SyntaxError> || std::is_same_v<T, AnalyzerError>)
        return arg;
      if constexpr (std::is_same_v<T, PtsIn>) {
        return PtsOut{ getPointsToSetCached(arg.ptr) };
      }
      if constexpr (std::is_same_v<T, PtIn>) {
        return PtOut{ mayPointTo(getPointsToSetCached(arg.ptr), arg.obj) ? ResultMay: ResultNo };
      }
      if constexpr (std::is_same_v<T, AliasIn>) {
        return AliasOut{ getAliasResultCached(arg.a, arg.b) };
      }
      if constexpr (std::is_same_v<T, ReachableIn>) {
        _cg.buildCG([this](llvm::CallBase *callInst, llvm::Function *caller) {return this->indirectCallResolver(callInst, caller);});
        return ReachableOut{ std::move(_cg.reach(arg.from, arg.to, arg.ignoreUnknown)) };
      }
      if constexpr (std::is_same_v<T, CallOutEdgesIn>) {
        _cg.buildCG([this](llvm::CallBase *callInst, llvm::Function *caller) {return this->indirectCallResolver(callInst, caller);});
        return CallOutEdgesOut{ _cg.getOutEdges(arg.f), arg.ignoreCS };
      }
      if constexpr (std::is_same_v<T, CallInEdgesIn>) {
        _cg.buildCG([this](llvm::CallBase *callInst, llvm::Function *caller) {return this->indirectCallResolver(callInst, caller);});
        _cg.buildReverseCG();
        return CallInEdgesOut{ _cg.getInEdges(arg.f), _cg.getCallAnythingEdges(), arg.ignoreCS };
      }
      if constexpr (std::is_same_v<T, CallGraphIn>) {
        _cg.buildCG([this](llvm::CallBase *callInst, llvm::Function *caller) {return this->indirectCallResolver(callInst, caller);});
        CallTreeMap tree;
        std::queue<std::pair<unsigned, llvm::Function *>> q; // BFS to reach maxDepth
        llvm::DenseSet<llvm::Function *> visited;
        q.push({0, arg.f});
        visited.insert(arg.f);
        while (!q.empty()) {
          auto [depth, func] = q.front();
          q.pop();
          if (depth >= arg.maxDepth) continue;
          auto edges = _cg.getOutEdges(func); // llvm::ArrayRef<>
          for (auto &edge : edges) {
            auto &treeEdges = tree[func];
            if (treeEdges.find(edge.callee) != treeEdges.end()) continue;
            bool silence = edge.callee && shouldSilence(edge.callee->getName().str());
            if (!silence) treeEdges.insert(edge.callee);
            if (edge.type != ptacxx::CallEdge::CALLANYTHING && !visited.contains(edge.callee)) {
              q.push({depth+!silence, edge.callee});
              visited.insert(edge.callee);
            }
          }
        }
        return CallGraphOut{ {arg.f, std::move(tree)} };
      }
      if constexpr (std::is_same_v<T, CGReloadIn>) {
        _cg.rebuild();
        _cgPatchLoaded = false;
        return IRParseMessage{"ok"};
      }
      if constexpr (std::is_same_v<T, AllAllocSitesIn>) {
        computeAllocationSites();
        return AllocSitesOut{getAllocationSites()};
      }
      if constexpr (std::is_same_v<T, AllocSitesIn>) {
        computeAllocationSites();
        return AllocSitesOut{getAllocationSites(arg.f)};
      }
      return IRParseError{"unknown query type or not available"};
    }, query);
  } catch (const std::exception &e) {
    response = AnalyzerError{ std::string(e.what()) };
  }
  return responseToString(response, _irm);
}

PTAliasResult PAWrapper::aliasByIntersection(PointsToSetView pts1, PointsToSetView pts2) {
  if (!pts1 || !pts2) return llvm::AliasResult::MayAlias;
  auto _pts1 = pts1.value();
  auto _pts2 = pts2.value();
  if (_pts1.size() == 0 || _pts2.size() == 0) return llvm::AliasResult::NoAlias;
  bool intersect = false;
  if (_pts1.size() > 20) {
    llvm::SmallPtrSet<const llvm::Value *, 32> pts2_set;
    pts2_set.insert(_pts2.begin(), _pts2.end());
    for (auto site : _pts1) {
      // if (site->getType()->isPointerTy()) {
      if(pts2_set.count(site)) {
        intersect = true;
        break;
      }
      // }
    }
  } else {
    for (auto site : _pts1) {
      if (std::find(_pts2.begin(), _pts2.end(), site) != _pts2.end()) {
        intersect = true;
        break;
      }
    }
  }
  return intersect ? llvm::AliasResult::MayAlias : llvm::AliasResult::NoAlias;
}

llvm::SmallVector<ptacxx::CallGraph::ResolvedTarget, 4>
PAWrapper::indirectCallResolver(llvm::CallBase *callInst, llvm::Function *caller) {
  llvm::SmallVector<ptacxx::CallGraph::ResolvedTarget, 4> targets;
  if (callInst) {
    llvm::Value *calledValue = callInst->getCalledOperand();
    assert(calledValue);
    auto pts = getPointsToSetCached(calledValue);
    if (!pts) targets.push_back(std::make_pair(ptacxx::CallEdge::CALLANYTHING, nullptr));
    else for (auto ptr : pts.value()) {
      if (auto func = llvm::dyn_cast<llvm::Function>(ptr))
        targets.push_back(std::make_pair(ptacxx::CallEdge::INDIRECT, func));
    }
  } else if (caller) {
    if (!_cgPatchLoaded) {
      loadCGPatch(_irm, _cgPatchOut);
      _cgPatchLoaded = true;
    }
    auto it = _cgPatchOut.find(caller);
    if (it != _cgPatchOut.end())
      for (llvm::Function *callee : it->second)
        targets.push_back(std::make_pair(ptacxx::CallEdge::INDIRECT, callee));
  }
  return targets;
}

int PAWrapper::run() {
  init();
  while (true) {
    std::string input;
    std::getline(std::cin, input);
    std::string output = handleQueryWrapper(input);
    std::cout << input << "\n<queryresult>\n" << output << "\n</queryresult>\n";
    if (std::cin.eof()) break;
  }
  return 0;
}

PTAliasResult IncluPAWrapper::getAliasResult(Ptr a, Ptr b) {
  return aliasByIntersection(getPointsToSetCached(a), getPointsToSetCached(b));
}

bool UnifiPAWrapper::getPointsToSet(Ptr value, PointsToSet &pts){
  assert(!pts.size());
  for (auto site: getAllocationSites()) {
    if (getAliasResultCached(value, site.site) != llvm::AliasResult::NoAlias) {
      pts.push_back(site.site);
    }
  }
  return true;
}
