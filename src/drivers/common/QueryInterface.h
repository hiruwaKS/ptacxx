#pragma once

#include "common/IRManager.h"
#include "common/VId.h"

#include <llvm/IR/Value.h>
#include <llvm/Analysis/AliasAnalysis.h>

#include <variant>
#include <string>
#include <vector>
#include <tuple>
#include <cstring>

int toIntStrict(const std::string &s);
std::vector<std::string> tokenize(const std::string& str, const char delimiters[] = " \n\t");
std::string join(const std::vector<std::string>& tokens, const char delimiter);
VId parseVid(const std::string& vid);
std::string vidToString(VId vid);


struct AliasIn     { const llvm::Value *a; const llvm::Value *b; };
struct PtsIn       { const llvm::Value *ptr; };
struct PtIn        { const llvm::Value *ptr; const llvm::Value *obj; };
struct ReachableIn { const llvm::Value *from; const llvm::Value *to; };
struct CrashTestIn {};
struct NameToVIds   { std::vector<VId> vids; };
struct ParseError  { std::string message; };

using PAQuery = std::variant<std::monostate,
  Metadata, IRStat, DebugInfo, NameToVIds, ParseError,
  AliasIn, PtsIn, PtIn, ReachableIn, CrashTestIn>;

struct AliasOut     { llvm::AliasResult result; };
struct PtsOut       { std::vector<const llvm::Value *> targets; };
struct PtOut        { bool result; };
struct ReachableOut { bool result; };
struct CrashTestOut {};
struct ErrorOut     { std::string message; };

using PAResponse = std::variant<
  ErrorOut,
  Metadata, IRStat, DebugInfo, NameToVIds,
  AliasOut, PtsOut, PtOut, ReachableOut, CrashTestOut>;

PAQuery parse(const std::string &input, IRManager &irm);

std::string responseToString(const PAResponse &response, IRManager &irm);
