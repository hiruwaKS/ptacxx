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

VId parseVid(const std::string& vid, IRManager &irm) {
  auto tokens = tokenize(vid, ":");
  VId result={0,0};
  if (tokens.size() > 0 && !tokens[0].empty()) {
    auto &global = tokens[0];
    if (global[0] == '@') {
      auto dismangled = irm.dismangleGlobalOrFunction(global.substr(1));
      if (!dismangled.size()) throw std::runtime_error("not found: " + global);
      if (dismangled.size() == 1) result.globalIdx = dismangled[0].second;
      else {
        std::string buffer = "Ambiguous global or function name, candidates:";
        for (const auto &pair : dismangled) {
          if (!buffer.empty()) buffer += "\n";
          buffer += std::to_string(pair.second) + " " + pair.first;
        }
        throw std::runtime_error(buffer);
      }
    }
    else result.globalIdx = static_cast<int16_t>(toIntStrict(global));
  }
  if (tokens.size() > 1 && !tokens[1].empty()) {
    auto &local = tokens[1];
    if (local[0] == '%') {
      if (auto *F = llvm::dyn_cast<llvm::Function>(irm.vidToValue(VId{result.globalIdx, 0})))
        result.localIdx = irm.resolveLocalName(local, F);
      else throw std::runtime_error(
        "If you use a local name, the global name must be a function: " + vid);
    }
    else result.localIdx = static_cast<int16_t>(toIntStrict(local));
  }
  return result;
}

std::string vidToString(VId vid) {
  return join({std::to_string(vid.globalIdx),
    std::to_string(vid.localIdx)}, ':');
}

PAQuery parse(const std::string &input, IRManager &irm) {
  try {
  auto tokens = tokenize(input);
  if (tokens.empty())
    return PAQuery{};

  const auto &cmd = tokens[0];

  if (cmd == "meta") {
    return PAQuery{IRMQuery{irm.getMetadata()}};
  }

  if (cmd == "stat") {
    return PAQuery{IRMQuery{irm.getIRStat()}};
  }

  if (cmd == "d" || cmd == "debug") {
    if (tokens.size() < 2) return PAQuery{};
    VId vid = parseVid(tokens[1], irm);
    if (const llvm::Value *V = irm.vidToValue(vid))
      return PAQuery{IRMQuery{irm.getValueDebugInfo(V)}};
    return PAQuery{IRParseError{"invalid vid"}};
  }
  
  if (cmd == "name") {
    if (tokens.size() < 2) return PAQuery{};
    return PAQuery{IRMQuery{NameToVId{parseVid(tokens[1], irm)}}};
  }

  if (cmd == "alias") {
    if (tokens.size() < 3) return PAQuery{};
    const llvm::Value *a = irm.vidToValue(parseVid(tokens[1], irm));
    const llvm::Value *b = irm.vidToValue(parseVid(tokens[2], irm));
    return PAQuery{AliasIn{a, b}};
  }

  if (cmd == "aliasset") {
    if (tokens.size() < 2) return PAQuery{};
    const llvm::Value *ptr = irm.vidToValue(parseVid(tokens[1], irm));
    return PAQuery{AliasSetIn{ptr}};
  }

  if (cmd == "pts") {
    if (tokens.size() < 2) return PAQuery{};
    const llvm::Value *ptr = irm.vidToValue(parseVid(tokens[1], irm));
    return PAQuery{PtsIn{ptr}};
  }

  if (cmd == "pt") {
    if (tokens.size() < 3) return PAQuery{};
    const llvm::Value *ptr = irm.vidToValue(parseVid(tokens[1], irm));
    const llvm::Value *obj = irm.vidToValue(parseVid(tokens[2], irm));
    return PAQuery{PtIn{ptr, obj}};
  }

  if (cmd == "reach") {
    if (tokens.size() < 3) return PAQuery{};
    const llvm::Value *from = irm.vidToValue(parseVid(tokens[1], irm));
    const llvm::Value *to   = irm.vidToValue(parseVid(tokens[2], irm));
    return PAQuery{ReachableIn{from, to}};
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
    if constexpr (std::is_same_v<T, IRMQuery>) {
      return std::visit([&](const auto& inner) -> std::string {
        using InnerT = std::decay_t<decltype(inner)>;
        if constexpr (std::is_same_v<InnerT, IRMetadata>)
          return inner.metadata;

        if constexpr (std::is_same_v<InnerT, IRStat>)
          return "hasMain: "        + std::to_string(inner.hasMain)        + "\n"
              + "hasGlobalCtor: "  + std::to_string(inner.hasGlobalCtor)  + "\n"
              + "hasGlobalDtor: "  + std::to_string(inner.hasGlobalDtor)  + "\n"
              + "funcCnt: "        + std::to_string(inner.funcCnt)        + "\n"
              + "globalCnt: "      + std::to_string(inner.globalCnt)      + "\n"
              + "globalPtrCnt: "   + std::to_string(inner.globalPtrCnt)   + "\n"
              + "argPtrCnt: "      + std::to_string(inner.argPtrCnt)      + "\n"
              + "instPtrCnt: "     + std::to_string(inner.instPtrCnt);
        
        if constexpr (std::is_same_v<InnerT, IRDebugInfo>)
          return inner.debugInfo;

        if constexpr (std::is_same_v<InnerT, NameToVId>)
          return vidToString(inner.vid);
      }, arg);
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

    if constexpr (std::is_same_v<T, AliasSetOut>) {
      std::string r = "{";
      for (llvm::Value *v : *arg.ptrs) {
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
