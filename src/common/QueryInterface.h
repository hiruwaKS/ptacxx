#pragma once

#include "IRManager.h"
#include "CallGraph.h"
#include "VId.h"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Value.h>
#include <llvm/ADT/ArrayRef.h>

#include <variant>
#include <string>
#include <utility>
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
struct SyntaxError   { std::string message; };
struct AnalyzerError   { std::string message; };

using PAQuery = std::variant<
  SyntaxError,
  IRParseMessage, IRParseError, 
  AliasIn, AliasSetIn, PtsIn, PtIn, ReachableIn, CallOutEdgesIn, CallInEdgesIn, AllocSitesIn, AllAllocSitesIn, CrashTestIn>;

struct AliasOut     { PTAliasResult result; };
struct PtsOut       { PointsToSetView targets; };
struct AliasSetOut  { std::set<llvm::Value *> * ptrs; };
struct PtOut        { ModalityResult result; };
struct ReachableOut { std::vector<ptacxx::CallEdge> calledges; };
struct CallOutEdgesOut { ptacxx::CallGraph::EdgesResult calledges; };
struct CallInEdgesOut { ptacxx::CallGraph::EdgesResult inCalledges; ptacxx::CallGraph::EdgesResult inCallAnything; };
struct AllocSitesOut       { llvm::ArrayRef<AllocationSite> sites; };
struct CrashTestOut {};

using PAResponse = std::variant<
  IRParseMessage, IRParseError, SyntaxError, AnalyzerError,
  AliasOut, PtsOut, AliasSetOut, PtOut, ReachableOut, CallOutEdgesOut, CallInEdgesOut, AllocSitesOut, CrashTestOut>;


int toIntStrict(const std::string &s);
static constexpr char DELIMITERS[] = " \t\r\n";
std::string stripPrefix(const std::string &s, const char* deliminators = DELIMITERS);
std::pair<std::string, std::string> eatToken(const std::string &s);
VId parseVid(const std::string& vid);
std::string modalityToString(ModalityResult modal);
PAQuery parse(const std::string &input, IRManager &irm);
std::string responseToString(const PAResponse &response, IRManager &irm);
