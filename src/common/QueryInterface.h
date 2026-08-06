#pragma once

#include "IRManager.h"
#include "CallGraph.h"
#include "VId.h"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Value.h>
#include <llvm/ADT/ArrayRef.h>

#include <variant>
#include <string>
#include <vector>
#include <set>
#include <tuple>
#include <cstring>

using Ptr = llvm::Value *;
using AllocSite = llvm::Value *;
using PointsToSet = llvm::SmallVector<AllocSite, 4>;
using PointsToSetView = std::optional<llvm::ArrayRef<AllocSite>>;
using AliasPair = std::pair<llvm::Value *, llvm::Value *>;
using PTAliasResult = llvm::AliasResult;

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
struct ReachableIn { llvm::Function *from; llvm::Function *to; };
struct CallOutEdgesIn { llvm::Function *f; };
struct CallInEdgesIn { llvm::Function *f; };
struct AllocSitesIn { llvm::Function *f; };
struct AllAllocSitesIn {};
struct CrashTestIn {};
struct IRParseMessage { std::string message; };
struct IRParseError  { std::string message; };

using IRMQuery = std::variant<IRMetadata, IRStat, IRParseMessage>;

using PAQuery = std::variant<std::monostate,
  IRMQuery, IRParseError, 
  AliasIn, AliasSetIn, PtsIn, PtIn, ReachableIn, CallOutEdgesIn, CallInEdgesIn, AllocSitesIn, AllAllocSitesIn, CrashTestIn>;

struct AliasOut     { PTAliasResult result; };
struct PtsOut       { PointsToSetView targets; };
struct AliasSetOut  { std::set<llvm::Value *> * ptrs; };
struct PtOut        { ModalityResult result; };
struct ReachableOut { ModalityResult result; };
struct CallOutEdgesOut { ptacxx::CallGraph::EdgesResult calledges; };
struct CallInEdgesOut { ptacxx::CallGraph::EdgesResult inCalledges; ptacxx::CallGraph::EdgesResult inCallAnything; };
struct AllocSitesOut       { llvm::ArrayRef<AllocationSite> sites; };
struct CrashTestOut {};
struct ErrorOut     { std::string message; };

using PAResponse = std::variant<
  ErrorOut,
  IRMQuery,
  AliasOut, PtsOut, AliasSetOut, PtOut, ReachableOut, CallOutEdgesOut, CallInEdgesOut, AllocSitesOut, CrashTestOut>;


int toIntStrict(const std::string &s);
std::vector<std::string> tokenize(const std::string& str, const char delimiters[] = " \n\t");
std::string join(const std::vector<std::string>& tokens, const char delimiter);
VId parseVid(const std::string& vid, IRManager &irm);
std::string modalityToString(ModalityResult modal);
PAQuery parse(const std::string &input, IRManager &irm);
std::string responseToString(const PAResponse &response, IRManager &irm);
