#pragma once

#include "common/IRManager.h"
#include "common/CallGraph.h"
#include "common/VId.h"

#include <llvm/IR/Value.h>
#include <llvm/Analysis/AliasAnalysis.h>

#include <variant>
#include <string>
#include <vector>
#include <set>
#include <tuple>
#include <cstring>

enum ModalityResult { ResultNo, ResultMay, ResultMust };

struct AliasIn     { const llvm::Value *a; const llvm::Value *b; };
struct AliasSetIn  { const llvm::Value *ptr; };
struct PtsIn       { const llvm::Value *ptr; };
struct PtIn        { const llvm::Value *ptr; const llvm::Value *obj; };
struct ReachableIn { const llvm::Function *from; const llvm::Function *to; };
struct CallOutEdgesIn { const llvm::Function *f; };
struct CallInEdgesIn { const llvm::Function *f; };
struct CrashTestIn {};
struct IRParseMessage { std::string message; };
struct IRParseError  { std::string message; };

using IRMQuery = std::variant<IRMetadata, IRStat, IRParseMessage>;

using PAQuery = std::variant<std::monostate,
  IRMQuery, IRParseError, 
  AliasIn, AliasSetIn, PtsIn, PtIn, ReachableIn, CallOutEdgesIn, CallInEdgesIn, CrashTestIn>;

struct AliasOut     { llvm::AliasResult result; };
struct PtsOut       { std::vector<const llvm::Value *> targets; };
struct AliasSetOut  { const std::set<llvm::Value *> * ptrs; };
struct PtOut        { ModalityResult result; };
struct ReachableOut { ModalityResult result; };
struct CallOutEdgesOut { ptacxx::CallGraph::EdgesResult calledges; };
struct CallInEdgesOut { ptacxx::CallGraph::EdgesResult inCalledges; ptacxx::CallGraph::EdgesResult inCallAnything; };
struct CrashTestOut {};
struct ErrorOut     { std::string message; };

using PAResponse = std::variant<
  ErrorOut,
  IRMQuery,
  AliasOut, PtsOut, AliasSetOut, PtOut, ReachableOut, CallOutEdgesOut, CallInEdgesOut, CrashTestOut>;


int toIntStrict(const std::string &s);
std::vector<std::string> tokenize(const std::string& str, const char delimiters[] = " \n\t");
std::string join(const std::vector<std::string>& tokens, const char delimiter);
VId parseVid(const std::string& vid, IRManager &irm);
std::string modalityToString(ModalityResult modal);
PAQuery parse(const std::string &input, IRManager &irm);
std::string responseToString(const PAResponse &response, IRManager &irm);
