#pragma once

#include "IRManager.h"
#include "CallGraph.h"
#include "VId.h"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Value.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>

#include <variant>
#include <string>
#include <utility>
#include <vector>
#include <set>
#include <tuple>
#include <cstring>

/// QueryInterface.h are the interface part of PAWrapper, it wrapped some details like parsing

using Ptr = llvm::Value *;
using AllocSite = llvm::Value *;
using PointsToSet = llvm::SmallVector<AllocSite, 4>;
using PointsToSetView = std::optional<llvm::ArrayRef<AllocSite>>;
using AliasPair = std::pair<llvm::Value *, llvm::Value *>;
using PTAliasResult = llvm::AliasResult;
namespace ptacxx {
using PTResult = llvm::AliasResult;
}
using CGPatchMap = llvm::DenseMap<llvm::Function *, llvm::SmallVector<llvm::Function *, 4>>;
using CallTreeMap = llvm::DenseMap<llvm::Function *, llvm::SmallPtrSet<llvm::Function*, 16>>;
using CallTree = std::pair<llvm::Function *, CallTreeMap>;

enum ModalityResult { ResultNo, ResultMay, ResultMust };

struct AllocationSite {
  enum AllocationType {
    STACK = 0,
    HEAP,
    FUNCTION, // function definition is considered as an allocation site
    GLOBAL
  };
  AllocationType type;
  llvm::Value *site;
};

struct AliasIn     { Ptr a; Ptr b; };
struct AliasSetIn  { Ptr ptr; };
struct PtsIn       { Ptr ptr; };
struct PtIn        { Ptr ptr; AllocSite obj; };
/// A PtaHook .pts dump (v2) parsed into (pointer, expected target vids).
/// File parsing lives in QueryInterface; the wrapper only consumes the array.
struct PtsTestIn   {
  std::vector<std::pair<Ptr, std::vector<VId>>> records;
  size_t unresolved = 0; // edges whose key vid did not resolve
  bool consistent = false;
};
struct ReachableIn { llvm::Function *from; llvm::Function *to; bool ignoreUnknown; };
struct CallOutEdgesIn { llvm::Function *f; bool ignoreCS; };
struct CallInEdgesIn { llvm::Function *f; bool ignoreCS; };
struct CallGraphIn { llvm::Function *f; unsigned maxDepth; bool stdSilence; bool llvmSilence; };
struct CGReloadIn {};
struct AllocSitesIn { llvm::Function *f; };
struct AllAllocSitesIn {};
struct CrashTestIn {};
struct TestIn {};
struct IRParseMessage { std::string message; };

using PAQuery = std::variant<
  IRParseMessage,
  AliasIn, AliasSetIn, PtsIn, PtIn, PtsTestIn, ReachableIn, CallOutEdgesIn, CallInEdgesIn, CallGraphIn,
  CGReloadIn, AllocSitesIn, AllAllocSitesIn, CrashTestIn, TestIn>;

struct AliasOut     { PTAliasResult result; };
struct PtsOut       { PointsToSetView targets; };
struct PtsTestOut   {
  size_t passes;
  size_t fails;
  size_t errors;
};
struct AliasSetOut  { std::set<llvm::Value *> * ptrs; };
struct PtOut        { ModalityResult result; };
struct ReachableOut { std::vector<ptacxx::CallEdge> calledges; };
struct CallOutEdgesOut { ptacxx::CallGraph::EdgesResult calledges; bool ignoreCS; };
struct CallInEdgesOut { ptacxx::CallGraph::EdgesResult inCalledges; ptacxx::CallGraph::EdgesResult inCallAnything; bool ignoreCS; };
struct CallGraphOut { CallTree cg; };
struct AllocSitesOut       { llvm::ArrayRef<AllocationSite> sites; };
struct CrashTestOut {};
struct TestOut { std::string result; };

using PAResponse = std::variant<
  IRParseMessage,
  AliasOut, PtsOut, PtsTestOut, AliasSetOut, PtOut, ReachableOut, CallOutEdgesOut, CallInEdgesOut, CallGraphOut, AllocSitesOut, CrashTestOut, TestOut>;

PAQuery parse(const std::string &input, IRManager &irm);
std::string responseToString(const PAResponse &response, IRManager &irm);
void loadCGPatch(IRManager &irm, CGPatchMap &out);

bool shouldSilence(const std::string& mangledName);
