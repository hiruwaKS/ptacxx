#include "QueryInterface.h"
#include "PAWrapper.h"
#include "LLVMUtils.h"
#include "Common.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/InitLLVM.h"

#include <iostream>
#include <algorithm>
#include <queue>
#include <set>
#include <string>

namespace ptacxx::options {
/// @note this will gather all options that used in PAWrapper, defined dispersedly
extern std::string CGPatchPath;
extern std::string NoteFolderPath;
extern std::string OutputIRPath;
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
  if (key == "output-ir") {
    OutputIRPath = value;
    return true;
  }
  return false;
}
} // namespace ptacxx::options

PAWrapper::~PAWrapper() {
  if (pInitLLVM) delete pInitLLVM;
}

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
          if (CB->isNoBuiltin()) continue;
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
  ASSERT(_allocSitesComputed, "assertionviolation-allocation-sites-not-computed",
         "allocation sites not computed");
  return llvm::ArrayRef<AllocationSite>(_allocationSites);
}

llvm::ArrayRef<AllocationSite> PAWrapper::getAllocationSites(llvm::Function *F) const {
  ASSERT(_allocSitesComputed, "assertionviolation-allocation-sites-not-computed-by-function",
         "allocation sites not computed");
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
  PAQuery query = parse(req, _irm);
  PAResponse response = std::visit([&](const auto &arg) -> PAResponse {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, IRParseMessage>)
        return arg;
      if constexpr (std::is_same_v<T, PtsIn>) {
        return PtsOut{ getPointsToSetCached(arg.ptr) };
      }
      if constexpr (std::is_same_v<T, PtIn>) {
        return PtOut{ mayPointTo(getPointsToSetCached(arg.ptr), arg.obj) ? ResultMay: ResultNo };
      }
      if constexpr (std::is_same_v<T, PtsTestIn>) {
        // Each dumped edge (ptr -> target) is one check: FAIL when the analyzer
        // does not report `ptr` may point to `target`, ERROR when a vid does not
        // resolve. The pair query is analyzer-specific: Inclu tests set
        // membership, Unifi uses its alias query.
        size_t passes = 0;
        size_t fails = 0;
        size_t errors = arg.unresolved;
        for (const auto &[ptr, targets] : arg.records) {
          for (VId t : targets) {
            llvm::Value *target = _irm.vidToValue(t);
            if (!target) {
              ++errors;
              continue;
            }
            const auto fast = getPointToResultCached(ptr, target);
            if (arg.consistent) {
              const auto slow = getPointToResultCachedSlow(ptr, target);
              const bool fastNo = fast == llvm::AliasResult::NoAlias;
              const bool slowNo = slow == llvm::AliasResult::NoAlias;
              if (fastNo != slowNo)
                throw ptacxx::AnalyzerError(
                    "analyzererror-pts-test-inconsistent",
                    "pts-test inconsistent: ptr " +
                        std::to_string(_irm.valueToVId(ptr)) + " target " +
                        std::to_string(t) + " fast=" + (fastNo ? "No" : "May") +
                        " slow=" + (slowNo ? "No" : "May"));
              if (fastNo)
                ++fails;
              else
                ++passes;
            } else if (fast != llvm::AliasResult::NoAlias) {
              ++passes;
            } else {
              ++fails;
            }
          }
        }
        return PtsTestOut{passes, fails, errors};
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
      if constexpr (std::is_same_v<T, TestIn>) {
        enum class Expectation {
          MayAlias,
          NoAlias,
          MustAlias,
          PartialAlias,
          MayPointsTo,
          NoPointsTo,
          MayReach
        };

        enum class TestStatus {
          Pass,
          Fail,
          PassButExpectedFail,
          FailAndExpectedFail
        };

        struct TestCase {
          Expectation expectation;
          bool expectedFail;
          llvm::Value *lhs;
          llvm::Value *rhs = nullptr;
          int idx = 0;
        };

        struct Summary {
          unsigned total = 0;
          unsigned pass = 0;
          unsigned fail = 0;
          unsigned passButExpectedFail = 0;
          unsigned failAndExpectedFail = 0;
        };

        auto aliasToString = [](llvm::AliasResult result) -> const char * {
          if (result == llvm::AliasResult::NoAlias) return "NoAlias";
          if (result == llvm::AliasResult::MayAlias) return "MayAlias";
          if (result == llvm::AliasResult::MustAlias) return "MustAlias";
          if (result == llvm::AliasResult::PartialAlias) return "PartialAlias";
          return "MayAlias";
        };

        auto pointsToToString = [](bool mayPointsTo) -> const char * {
          return mayPointsTo ? "MayPointsTo" : "NoPointsTo";
        };

        auto expectationToString = [](Expectation expectation) -> const char * {
          switch (expectation) {
          case Expectation::MayAlias:
            return "MayAlias";
          case Expectation::NoAlias:
            return "NoAlias";
          case Expectation::MustAlias:
            return "MustAlias";
          case Expectation::PartialAlias:
            return "PartialAlias";
          case Expectation::MayPointsTo:
            return "MayPointsTo";
          case Expectation::NoPointsTo:
            return "NoPointsTo";
          case Expectation::MayReach:
            return "MayReach";
          }
          return "MayAlias";
        };

        auto reachToString = [](bool mayReach) -> const char * {
          return mayReach ? "MayReach" : "NoReach";
        };

        auto statusToString = [](TestStatus status) -> const char * {
          switch (status) {
          case TestStatus::Pass:
            return "Pass";
          case TestStatus::Fail:
            return "Fail";
          case TestStatus::PassButExpectedFail:
            return "PassButExpectedFail";
          case TestStatus::FailAndExpectedFail:
            return "FailAndExpectedFail";
          }
          return "Fail";
        };

        auto matchesExpectation = [](Expectation expectation,
                                     llvm::AliasResult actual) {
          // TODO: distinguish MustAlias/PartialAlias when wrapped analyses
          // provide these results consistently.
          switch (expectation) {
          case Expectation::MayAlias:
            return actual != llvm::AliasResult::NoAlias;
          case Expectation::NoAlias:
            return actual == llvm::AliasResult::NoAlias;
          case Expectation::MustAlias:
            return actual != llvm::AliasResult::NoAlias;
          case Expectation::PartialAlias:
            return actual == llvm::AliasResult::MayAlias;
          case Expectation::MayPointsTo:
          case Expectation::NoPointsTo:
          case Expectation::MayReach:
            return false;
          }
          return false;
        };

        auto getExpectation = [](llvm::StringRef name,
                                 Expectation &expectation,
                                 bool &expectedFail) {
          expectedFail = false;
          if (name == "MAYALIAS") {
            expectation = Expectation::MayAlias;
            return true;
          }
          if (name == "NOALIAS") {
            expectation = Expectation::NoAlias;
            return true;
          }
          if (name == "MUSTALIAS") {
            expectation = Expectation::MustAlias;
            return true;
          }
          if (name == "PARTIALALIAS") {
            expectation = Expectation::PartialAlias;
            return true;
          }
          if (name == "EXPECTEDFAIL_MAYALIAS") {
            expectation = Expectation::MayAlias;
            expectedFail = true;
            return true;
          }
          if (name == "EXPECTEDFAIL_NOALIAS") {
            expectation = Expectation::NoAlias;
            expectedFail = true;
            return true;
          }
          return false;
        };

        auto getIndex = [](llvm::Value *value, int &idx) {
          value = value->stripPointerCasts();
          if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(value)) {
            idx = static_cast<int>(CI->getSExtValue());
            return true;
          }
          return false;
        };

        auto contains = [](PointsToSetView pts, llvm::Value *value) {
          if (!pts)
            return false;
          auto targets = pts.value();
          return std::find(targets.begin(), targets.end(), value) != targets.end();
        };

        llvm::DenseMap<int, llvm::Value *> marks;
        llvm::DenseMap<int, llvm::Function *> functionMarks;
        std::vector<TestCase> tests;
        for (llvm::Function &F : _irm.getModule()) {
          for (llvm::BasicBlock &BB : F) {
            for (llvm::Instruction &I : BB) {
              auto *CB = llvm::dyn_cast<llvm::CallBase>(&I);
              if (!CB)
                continue;
              llvm::Function *callee = CB->getCalledFunction();
              if (!callee)
                continue;

              const std::string mangled = callee->getName().str();
              std::string name = getDemangledName(mangled);
              name = getAllNamespaceStripped(name);

              int idx = 0;
              if (name == "MARK_FUNCTION" || mangled == "MARK_FUNCTION") {
                if (CB->arg_size() >= 1 && getIndex(CB->getArgOperand(0), idx))
                  functionMarks[idx] = I.getFunction();
                continue;
              }

              if (name == "CHECK_REACH" || mangled == "CHECK_REACH") {
                if (CB->arg_size() < 1 || !getIndex(CB->getArgOperand(0), idx))
                  continue;
                tests.push_back(TestCase{
                    Expectation::MayReach,
                    false,
                    I.getFunction(),
                    nullptr,
                    idx});
                continue;
              }

              if (CB->arg_size() < 2)
                continue;

              if (name == "MARK_AS" || mangled == "MARK_AS") {
                if (getIndex(CB->getArgOperand(1), idx))
                  marks[idx] = CB->getArgOperand(0);
                continue;
              }

              Expectation expectation;
              bool expectedFail = false;
              if (name == "CHECK_POINTS_TO" || mangled == "CHECK_POINTS_TO") {
                if (!getIndex(CB->getArgOperand(1), idx))
                  continue;
                tests.push_back(TestCase{
                    Expectation::MayPointsTo,
                    false,
                    CB->getArgOperand(0),
                    nullptr,
                    idx});
                continue;
              }
              if (name == "CHECK_NO_POINTS_TO" ||
                  mangled == "CHECK_NO_POINTS_TO") {
                if (!getIndex(CB->getArgOperand(1), idx))
                  continue;
                tests.push_back(TestCase{
                    Expectation::NoPointsTo,
                    false,
                    CB->getArgOperand(0),
                    nullptr,
                    idx});
                continue;
              }
              if (name == "CHECK_ALIAS" || mangled == "CHECK_ALIAS") {
                if (!getIndex(CB->getArgOperand(1), idx))
                  continue;
                tests.push_back(TestCase{
                    Expectation::MayAlias,
                    false,
                    CB->getArgOperand(0),
                    nullptr,
                    idx});
                continue;
              }
              if (!getExpectation(name, expectation, expectedFail) &&
                  !getExpectation(mangled, expectation, expectedFail))
                continue;

              tests.push_back(TestCase{
                  expectation,
                  expectedFail,
                  CB->getArgOperand(0),
                  CB->getArgOperand(1),
                  0});
            }
          }
        }

        Summary summary;
        std::string buf;
        llvm::raw_string_ostream os(buf);
        for (const TestCase &test : tests) {
          llvm::AliasResult actual = llvm::AliasResult::NoAlias;
          bool mayPointsTo = false;
          bool mayReach = false;
          bool isPointsToCheck = test.expectation == Expectation::MayPointsTo ||
                                 test.expectation == Expectation::NoPointsTo;
          bool isReachCheck = test.expectation == Expectation::MayReach;
          llvm::Value *rhs = test.rhs;
          if (!rhs && isReachCheck) {
            auto it = functionMarks.find(test.idx);
            if (it != functionMarks.end())
              rhs = it->second;
          } else if (!rhs) {
            auto it = marks.find(test.idx);
            if (it != marks.end())
              rhs = it->second;
          }

          bool passed = false;
          if (rhs && isPointsToCheck) {
            mayPointsTo = contains(getPointsToSetCached(test.lhs), rhs);
            passed = test.expectation == Expectation::MayPointsTo ? mayPointsTo
                                                                  : !mayPointsTo;
          } else if (rhs && isReachCheck) {
            auto *from = llvm::dyn_cast<llvm::Function>(test.lhs);
            auto *to = llvm::dyn_cast<llvm::Function>(rhs);
            if (from && to) {
              _cg.buildCG([this](llvm::CallBase *callInst,
                                 llvm::Function *caller) {
                return this->indirectCallResolver(callInst, caller);
              });
              mayReach = !_cg.reach(from, to, false).empty();
              passed = mayReach;
            }
          } else if (rhs) {
            actual = getAliasResultCached(test.lhs, rhs);
            passed = matchesExpectation(test.expectation, actual);
          }
          TestStatus status = passed ? TestStatus::Pass : TestStatus::Fail;
          if (test.expectedFail)
            status = passed ? TestStatus::PassButExpectedFail
                            : TestStatus::FailAndExpectedFail;

          ++summary.total;
          switch (status) {
          case TestStatus::Pass:
            ++summary.pass;
            break;
          case TestStatus::Fail:
            ++summary.fail;
            break;
          case TestStatus::PassButExpectedFail:
            ++summary.passButExpectedFail;
            break;
          case TestStatus::FailAndExpectedFail:
            ++summary.failAndExpectedFail;
            break;
          }

          os << expectationToString(test.expectation) << " "
             << statusToString(status) << " "
             << (isReachCheck ? reachToString(mayReach)
                 : isPointsToCheck ? pointsToToString(mayPointsTo)
                                   : aliasToString(actual))
             << " "
             << _irm.valueToVId(test.lhs) << " "
             << (rhs ? _irm.valueToVId(rhs) : VID_NOT_REGISTERED) << "\n";
        }
        os << "summary total=" << summary.total << " Pass=" << summary.pass
           << " Fail=" << summary.fail
           << " PassButExpectedFail=" << summary.passButExpectedFail
           << " FailAndExpectedFail=" << summary.failAndExpectedFail;
        os.flush();
        return TestOut{buf};
      }
      throw ptacxx::SemanticError("semanticerror-unknown-query-type",
                                  "unknown query type or not available");
    }, query);
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
    ASSERT(calledValue, "assertionviolation-indirect-call-resolver-null-operand",
           "null called operand");
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

int PAWrapper::run(int argc, char **argv) {
  ptacxx::Stopwatch sw;
  try {
    ptacxx::options::CGPatchCLIntercept().go(argc, argv);
    std::string inputPath = argParseAndInitLLVM(argc, argv);
    auto parse = sw.record();
    _irm.loadMainModule(inputPath);
    auto load = sw.record();
    _irm.buildModuleIndex();
    auto index = sw.record();
    init();
    auto analysis = sw.record();
    if (!ptacxx::options::OutputIRPath.empty())
      _irm.dumpModule(ptacxx::options::OutputIRPath);
    emitInit(parse, load, index, analysis);
  } catch (const std::exception &e) {
    emitInitError(e);
    return 1;
  }
  return queryLoop();
}

int PAWrapper::queryLoop() {
  while (true) {
    std::string input;
    std::getline(std::cin, input);
    try {
      std::string output = handleQueryWrapper(input);
      std::cout << input << "\n<queryresult>\n" << output << "\n</queryresult>\n";
    } catch (const std::exception &e) {
      std::cout << input << "\n<queryerror>\n" << e.what() << "\n</queryerror>\n";
    }
    if (std::cin.eof()) break;
  }
  return 0;
}

void PAWrapper::emitInit(const ptacxx::Stopwatch::Record &parse,
                         const ptacxx::Stopwatch::Record &load,
                         const ptacxx::Stopwatch::Record &index,
                         const ptacxx::Stopwatch::Record &analysis) {
  std::cout << "<init>\n"
            << "arg_parse " << parse.split << " us\n"
            << "ir_load " << load.lap << " us\n"
            << "index " << index.lap << " us\n"
            << "analysis " << analysis.lap << " us\n"
            << "total " << analysis.split << " us\n"
            << "</init>\n";
}

void PAWrapper::emitInitError(const std::exception &e) {
  std::cout << "<initerror>\n" << e.what() << "\n</initerror>\n";
}

PTAliasResult IncluPAWrapper::getAliasResult(Ptr a, Ptr b) {
  return aliasByIntersection(getPointsToSetCached(a), getPointsToSetCached(b));
}

ptacxx::PTResult IncluPAWrapper::getPointToResultCached(Ptr ptr, AllocSite site) {
  auto result = getPointsToSetCached(ptr);
  if (!result) return llvm::AliasResult::MayAlias;
  for (auto target : result.value()) {
    if (target == site) return llvm::AliasResult::MayAlias;
  }
  return llvm::AliasResult::NoAlias;
}

ptacxx::PTResult IncluPAWrapper::getPointToResultCachedSlow(Ptr ptr, AllocSite site) {
  return getAliasResult(ptr, site);
}

bool UnifiPAWrapper::getPointsToSet(Ptr value, PointsToSet &pts){
  ASSERT(pts.empty(), "assertionviolation-points-to-set-output-not-empty",
         "output PointsToSet is not empty");
  computeAllocationSites();
  for (auto site: getAllocationSites()) {
    if (getAliasResultCached(value, site.site) != llvm::AliasResult::NoAlias) {
      pts.push_back(site.site);
    }
  }
  return true;
}

ptacxx::PTResult UnifiPAWrapper::getPointToResultCached(Ptr ptr, AllocSite site) {
  return getAliasResultCached(ptr, site);
}

ptacxx::PTResult UnifiPAWrapper::getPointToResultCachedSlow(Ptr ptr, AllocSite site) {
  auto result = getPointsToSetCached(ptr);
  if (!result) return llvm::AliasResult::MayAlias;
  for (auto target : result.value()) {
    if (target == site) return llvm::AliasResult::MayAlias;
  }
  return llvm::AliasResult::NoAlias; 
}
