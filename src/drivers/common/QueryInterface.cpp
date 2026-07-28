#include "QueryInterface.h"

#include <llvm/Support/raw_ostream.h>

#include <type_traits>

int toIntStrict(const std::string &s) {
  std::size_t pos;
  int val = std::stoi(s, &pos);
  if (pos != s.length())
    throw std::runtime_error("Invalid integer: " + s);
  return val;
}

std::vector<std::string> tokenize(const std::string& str, const char delimiters[]) {
  std::vector<std::string> tokens;
  std::string token;
  for (const char c:str) {
    if (strchr(delimiters, c)) {
      if (!token.empty()) tokens.push_back(token);
      token.clear();
    }
    else token += c;
  }
  if (!token.empty()) tokens.push_back(token);
  return tokens;
}

std::string join(const std::vector<std::string>& tokens, const char delimiter) {
  std::string result;
  for (const auto& token:tokens) {
    if (!result.empty()) result += delimiter;
    result += token;
  }
  return result;
}

VId parseVid(const std::string& vid) {
  auto tokens = tokenize(vid, ":");
  VId result={0,0,0};
  if (tokens.size() > 0 && !tokens[0].empty())
    result.moduleIdx = static_cast<int16_t>(toIntStrict(tokens[0]));
  if (tokens.size() > 1 && !tokens[1].empty())
    result.globalIdx = static_cast<int16_t>(toIntStrict(tokens[1]));
  if (tokens.size() > 2 && !tokens[2].empty())
    result.localIdx  = static_cast<int16_t>(toIntStrict(tokens[2]));
  return result;
}

std::string vidToString(VId vid) {
  return join({std::to_string(vid.moduleIdx), 
    std::to_string(vid.globalIdx),
    std::to_string(vid.localIdx)}, ':');
}

PAQuery parse(const std::string &input, IRManager &irm) {
  try {
  auto tokens = tokenize(input);
  if (tokens.empty())
    return PAQuery{};

  const auto &cmd = tokens[0];

  if (cmd == "meta") {
    int16_t midx = 0;
    if (tokens.size() >= 2) midx = parseVid(tokens[1]).moduleIdx;
    return PAQuery{irm.getMetadata(midx)};
  }

  if (cmd == "stat") {
    int16_t midx = 0;
    if (tokens.size() >= 2) midx = parseVid(tokens[1]).moduleIdx;
    return PAQuery{irm.getIRStat(midx)};
  }

  if (cmd == "debug") {
    if (tokens.size() < 2) return PAQuery{};
    VId vid = parseVid(tokens[1]);
    if (const llvm::Value *V = irm.vidToValue(vid))
      return PAQuery{irm.getValueDebugInfo(V)};
    return PAQuery{IRDebugInfo{"(invalid vid)"}};
  }

  if (cmd == "alias") {
    if (tokens.size() < 3) return PAQuery{};
    const llvm::Value *a = irm.vidToValue(parseVid(tokens[1]));
    const llvm::Value *b = irm.vidToValue(parseVid(tokens[2]));
    return PAQuery{AliasIn{a, b}};
  }

  if (cmd == "pts") {
    if (tokens.size() < 2) return PAQuery{};
    const llvm::Value *ptr = irm.vidToValue(parseVid(tokens[1]));
    return PAQuery{PtsIn{ptr}};
  }

  if (cmd == "pt") {
    if (tokens.size() < 3) return PAQuery{};
    const llvm::Value *ptr = irm.vidToValue(parseVid(tokens[1]));
    const llvm::Value *obj = irm.vidToValue(parseVid(tokens[2]));
    return PAQuery{PtIn{ptr, obj}};
  }

  if (cmd == "reach") {
    if (tokens.size() < 3) return PAQuery{};
    const llvm::Value *from = irm.vidToValue(parseVid(tokens[1]));
    const llvm::Value *to   = irm.vidToValue(parseVid(tokens[2]));
    return PAQuery{ReachableIn{from, to}};
  }

  if (cmd == "name") {
    if (tokens.size() < 2) return PAQuery{};
    return PAQuery{NameToVIds{irm.globalOrFunctionToVIds(tokens[1])}};
  }

  if (cmd == "crash")
    return PAQuery{CrashTestIn{}};

  return PAQuery{};
 } catch (const std::exception &e) {
    return PAQuery{IRParseError{std::string("parse error: ") + e.what()}};
  }
}

std::string responseToString(const PAResponse &response, IRManager &irm) {
  return std::visit([&](const auto &arg) -> std::string {
    using T = std::decay_t<decltype(arg)>;

    if constexpr (std::is_same_v<T, ErrorOut>)
      return "error: " + arg.message;

    if constexpr (std::is_same_v<T, IRMetadata>)
      return arg.metadata;

    if constexpr (std::is_same_v<T, IRStat>)
      return "hasMain: "        + std::to_string(arg.hasMain)        + "\n"
           + "hasGlobalCtor: "  + std::to_string(arg.hasGlobalCtor)  + "\n"
           + "hasGlobalDtor: "  + std::to_string(arg.hasGlobalDtor)  + "\n"
           + "funcCnt: "        + std::to_string(arg.funcCnt)        + "\n"
           + "globalCnt: "      + std::to_string(arg.globalCnt)      + "\n"
           + "globalPtrCnt: "   + std::to_string(arg.globalPtrCnt)   + "\n"
           + "argPtrCnt: "      + std::to_string(arg.argPtrCnt)      + "\n"
           + "instPtrCnt: "     + std::to_string(arg.instPtrCnt);
    
    if constexpr (std::is_same_v<T, IRDebugInfo>)
      return arg.debugInfo;

    if constexpr (std::is_same_v<T, NameToVIds>) {
      std::string result;
      for (const VId &vid : arg.vids) {
        if (!result.empty()) result += "\n";
        result += vidToString(vid);
        result += " " + irm.vidToValue(vid)->getName().str();
      }
      return result;
    }

    if constexpr (std::is_same_v<T, AliasOut>) {
      std::string buf;
      llvm::raw_string_ostream os(buf);
      os << arg.result;
      return buf;
    }

    if constexpr (std::is_same_v<T, PtsOut>) {
      std::string r = "{";
      for (const llvm::Value *v : arg.targets) {
        if (!r.empty()) r += " ";
        try {
          VId vid = irm.valueToVId(v);
          r += vidToString(vid) + ",\n";
        } catch (...) {
          r += "(unknown)";
        }
      }
      return r+"}";
    }

    if constexpr (std::is_same_v<T, PtOut>)
      return std::to_string(arg.result);

    if constexpr (std::is_same_v<T, ReachableOut>)
      return std::to_string(arg.result);

    if constexpr (std::is_same_v<T, CrashTestOut>)
      return "pass";

    return "(unknown query type)";
  }, response);
}
