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

std::string modalityToString(ModalityResult modal) {
  return modal == ResultMust ? "Must" : modal == ResultMay ? "May" : "No";
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

  if (cmd == "d" || cmd == "debug" || cmd == "name") {
    if (tokens.size() < 2) return PAQuery{};
    VId vid = parseVid(tokens[1], irm);
    if (auto *V = irm.vidToValue(vid)) {
      std::string buf;
      llvm::raw_string_ostream os{buf};
      (cmd == "name" ? irm.printValueDebugName(os, V) :
        irm.printValueDebugInfo(os, V)).flush();
      return PAQuery{IRMQuery{IRParseMessage{buf}}};
    }
    return PAQuery{IRParseError{"debug or name command: fatal"}};
  }

  if (cmd == "alias") {
    if (tokens.size() < 3) return PAQuery{};
    auto a = irm.vidToValue(parseVid(tokens[1], irm));
    auto b = irm.vidToValue(parseVid(tokens[2], irm));
    return PAQuery{AliasIn{a, b}};
  }

  if (cmd == "aliasset") {
    if (tokens.size() < 2) return PAQuery{};
    auto ptr = irm.vidToValue(parseVid(tokens[1], irm));
    return PAQuery{AliasSetIn{ptr}};
  }

  if (cmd == "pts") {
    if (tokens.size() < 2) return PAQuery{};
    auto ptr = irm.vidToValue(parseVid(tokens[1], irm));
    return PAQuery{PtsIn{ptr}};
  }

  if (cmd == "pt") {
    if (tokens.size() < 3) return PAQuery{};
    auto ptr = irm.vidToValue(parseVid(tokens[1], irm));
    auto obj = irm.vidToValue(parseVid(tokens[2], irm));
    return PAQuery{PtIn{ptr, obj}};
  }

  if (cmd == "reach") {
    if (tokens.size() < 3) return PAQuery{};
    if (auto from = llvm::dyn_cast<llvm::Function>(irm.vidToValue(parseVid(tokens[1], irm))))
      if (auto to = llvm::dyn_cast<llvm::Function>(irm.vidToValue(parseVid(tokens[2], irm))))
        return PAQuery{ReachableIn{from, to}};
    return PAQuery{IRParseError{"invalid function vid(s)"}};
  }

  if (cmd == "callout") {
    if (tokens.size() < 2) return PAQuery{};
    if (auto F = llvm::dyn_cast<llvm::Function>(irm.vidToValue(parseVid(tokens[1], irm))))
      return PAQuery{CallOutEdgesIn{F}};
    return PAQuery{IRParseError{"invalid function vid"}};
  }

  if (cmd == "callin") {
    if (tokens.size() < 2) return PAQuery{};
    if (auto F = llvm::dyn_cast<llvm::Function>(irm.vidToValue(parseVid(tokens[1], irm))))
      return PAQuery{CallInEdgesIn{F}};
    return PAQuery{IRParseError{"invalid function vid"}};
  }

  if (cmd == "site") {
    if (tokens.size() > 2) return PAQuery{};
    if (tokens.size() == 1) return PAQuery{AllAllocSitesIn{}};
    if (auto F = llvm::dyn_cast<llvm::Function>(irm.vidToValue(parseVid(tokens[1], irm))))
      return PAQuery{AllocSitesIn{F}};
    return PAQuery{IRParseError{"invalid function vid"}};
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
        
        if constexpr (std::is_same_v<InnerT, IRParseMessage>)
          return inner.message;
      }, arg);
    }

    if constexpr (std::is_same_v<T, AliasOut>) {
      std::string buf;
      llvm::raw_string_ostream os(buf);
      os << arg.result;
      os.flush();
      return buf;
    }

    if constexpr (std::is_same_v<T, PtsOut>) {
      std::string buf;
      llvm::raw_string_ostream os(buf);
      if (!arg.targets) os << "unknown";
      else for (auto v : arg.targets.value()) {
        try {
          irm.printValueDebugName(os, v) << "\n";
        } catch (const std::exception &e) {
          os << e.what();
        }
      }
      os.flush();
      return buf;
    }

    if constexpr (std::is_same_v<T, AliasSetOut>) {
      std::string buf;
      llvm::raw_string_ostream os(buf);
      for (llvm::Value *v : *arg.ptrs) {
        try {
          irm.printValueDebugName(os, v) << "\n";
        } catch (const std::exception &e) {
          os << e.what();
        }
      }
      os.flush();
      return buf;
    }

    if constexpr (std::is_same_v<T, PtOut>)
      return modalityToString(arg.result);

    if constexpr (std::is_same_v<T, ReachableOut>)
      return modalityToString(arg.result);
    
    if constexpr (std::is_same_v<T, CallOutEdgesOut> || std::is_same_v<T, CallInEdgesOut>) {
      std::string buf;
      llvm::raw_string_ostream os(buf);
      auto printCallEdge = [&](const ptacxx::CallEdge &edge) {
        irm.printValueDebugName(
          irm.printValueDebugName(
            irm.printValueDebugName(
              os << (edge.type == ptacxx::CallEdge::DIRECT ? "direct" : 
                edge.type == ptacxx::CallEdge::INDIRECT ? "indirect" : "callanything")
              << ", ", edge.caller
            ) << ", ", edge.callsite
          ), edge.callee
        );
      };
      if constexpr (std::is_same_v<T, CallOutEdgesOut>) {
        for (auto edge: arg.calledges) printCallEdge(edge);
      } else {
        for (auto edge: arg.inCalledges) printCallEdge(edge);
        os << "\n";
        for (auto edge: arg.inCallAnything) printCallEdge(edge);
      }
      os.flush();
      return buf;
    }

    if constexpr (std::is_same_v<T, AllocSitesOut>) {
      std::string buf;
      llvm::raw_string_ostream os(buf);
      for (auto site: arg.sites) {
        os << (site.type == AllocationSite::STACK ? "stack" : 
               site.type == AllocationSite::HEAP ? "heap" :
               site.type == AllocationSite::FUNCTION ? "function" : "global") << " ";
        irm.printValueDebugName(os, site.site);
        os << "\n";
      }
      os.flush();
      return buf;
    }

    if constexpr (std::is_same_v<T, CrashTestOut>)
      return "pass";

    return "(unknown query type)";
  }, response);
}
