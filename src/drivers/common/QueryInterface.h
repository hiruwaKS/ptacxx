#pragma once

#include "common/IRManager.h"
#include "common/VId.h"

#include <llvm/IR/Value.h>
#include <llvm/Analysis/AliasAnalysis.h>

#include <variant>
#include <string>
#include <vector>
#include <set>
#include <tuple>
#include <cstring>

int toIntStrict(const std::string &s);
std::vector<std::string> tokenize(const std::string& str, const char delimiters[] = " \n\t");
std::string join(const std::vector<std::string>& tokens, const char delimiter);
VId parseVid(const std::string& vid, IRManager &irm);
std::string vidToString(VId vid);

enum ModalityResult { ResultNo, ResultMay, ResultMust };

struct AliasIn     { const llvm::Value *a; const llvm::Value *b; };
struct AliasSetIn  { const llvm::Value *ptr; };
struct PtsIn       { const llvm::Value *ptr; };
struct PtIn        { const llvm::Value *ptr; const llvm::Value *obj; };
struct ReachableIn { const llvm::Value *from; const llvm::Value *to; };
struct CrashTestIn {};
struct NameToVId   { VId vid; };
struct IRParseError  { std::string message; };

using IRMQuery = std::variant<IRMetadata, IRStat, IRDebugInfo, NameToVId>;

using PAQuery = std::variant<std::monostate,
  IRMQuery, IRParseError, 
  AliasIn, AliasSetIn, PtsIn, PtIn, ReachableIn, CrashTestIn>;

struct AliasOut     { llvm::AliasResult result; };
struct PtsOut       { std::vector<const llvm::Value *> targets; };
struct AliasSetOut  { const std::set<llvm::Value *> * ptrs; };
struct PtOut        { ModalityResult result; };
struct ReachableOut { ModalityResult result; };
struct CrashTestOut {};
struct ErrorOut     { std::string message; };

using PAResponse = std::variant<
  ErrorOut,
  IRMQuery,
  AliasOut, PtsOut, AliasSetOut, PtOut, ReachableOut, CrashTestOut>;

PAQuery parse(const std::string &input, IRManager &irm);

std::string responseToString(const PAResponse &response, IRManager &irm);
