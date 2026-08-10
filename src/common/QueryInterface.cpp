#include "QueryInterface.h"

#include <llvm/Support/raw_ostream.h>

#include <type_traits>

static bool detailed = false;

int toIntStrict(const std::string &s) {
  std::size_t pos;
  int val = std::stoi(s, &pos);
  if (pos != s.length())
    throw std::runtime_error("Invalid integer in: " + s);
  return val;
}

std::string stripPrefix(const std::string &s, const char* delimiters) {
  size_t begin = 0;
  while (begin < s.size() && strchr(delimiters, s[begin])) ++begin;
  return s.substr(begin, s.size() - begin);
}

std::pair<std::string, std::string> eatToken(const std::string &s) {
  const std::string input = stripPrefix(s);
  const size_t pos = input.find_first_of(DELIMITERS);
  if (pos == std::string::npos) return {input, ""};
  return {input.substr(0, pos), input.substr(pos)};
}

VId parseVid(const std::string& vid) {
  return static_cast<int32_t>(toIntStrict(vid));
  // auto tokens = tokenize(vid, ":");
  // VId result={0,0};
  // if (tokens.size() > 0 && !tokens[0].empty()) {
  //   auto &global = tokens[0];
  //   if (global[0] == '@') {
  //     auto dismangled = irm.dismangleGlobalOrFunction(global.substr(1));
  //     if (!dismangled.size()) throw std::runtime_error("not found: " + global);
  //     if (dismangled.size() == 1) result.globalIdx = dismangled[0].second;
  //     else {
  //       std::string buffer = "Ambiguous global or function name, candidates:";
  //       for (const auto &pair : dismangled) {
  //         if (!buffer.empty()) buffer += "\n";
  //         buffer += std::to_string(pair.second) + " " + pair.first;
  //       }
  //       throw std::runtime_error(buffer);
  //     }
  //   }
  //   else result.globalIdx = 
  // }
  // if (tokens.size() > 1 && !tokens[1].empty()) {
  //   auto &local = tokens[1];
  //   if (local[0] == '%') {
  //     if (auto *F = llvm::dyn_cast<llvm::Function>(irm.vidToValue(VId{result.globalIdx, 0})))
  //       result.localIdx = irm.resolveLocalName(local, F);
  //     else throw std::runtime_error(
  //       "If you use a local name, the global name must be a function: " + vid);
  //   }
  //   else result.localIdx = static_cast<int16_t>(toIntStrict(local));
  // }
  // return result;
}

std::string modalityToString(ModalityResult modal) {
  return modal == ResultMust ? "Must" : modal == ResultMay ? "May" : "No";
}

PAQuery parse(const std::string &input, IRManager &irm) {
  try {
    auto [cmd, unread] = eatToken(input);
    if (cmd.empty())
      return PAQuery{SyntaxError{"empty input"}};

    if (cmd == "help" || cmd == "h" || cmd == "?") {
      if (!unread.empty()) return PAQuery{SyntaxError{"too many arguments"}};
      return PAQuery{IRParseMessage{
          "commands:\n"
          "  stat | s              show IR metadata and statistics\n"
          "  global | g [prefix]   list globals (including functions), to see its vid, demangled prefix allowed\n"
          "  loc <vid> [n]         list 20 locals of a function, skip the front n\n"
          "  site [vid]            list allocation sites of a function or all\n"
          "  debug | d <vid>       print detailed value debug info, to locate and debug\n"
          "  alias | a <vid> <vid> get alias result\n"
          "  aliasset <vid>        get alias set of a pointer (TODO)\n"
          "  pt <vid> <vid>        get points-to result\n"
          "  pts <vid>             get points-to set of a pointer\n"
          "  reach <vid> <vid>     get call-graph reachability\n"
          "  callout <vid>         get outgoing calls of a function\n"
          "  callin <vid>          get incoming calls of a function\n"
          "  detail                toggle vid printing mode"
          "  crash                 run crash test (exercise all pointers) (TODO)\n"
          "  help | h              show help\n"
          "notes:\n"
          "  prefix in func/global can be a demangled name, which contains ' ' '(' ')' chars sometimes\n"
        }};
    }

    if (cmd == "stat" || cmd == "s") {
      if (!unread.empty()) return PAQuery{SyntaxError{"too many arguments"}};
      std::string buf;
      llvm::raw_string_ostream os{buf};
      irm.printStat(os).flush();
      return PAQuery{IRParseMessage{buf}};
    }

    if (cmd == "list" || cmd == "l") {
      auto namePrefix = stripPrefix(unread);
      if (namePrefix.size() && namePrefix[0] == '@') namePrefix = namePrefix.substr(1);
      std::string buf;
      llvm::raw_string_ostream os(buf);
      for (auto &[name, vid] : irm.listGlobal(namePrefix))
        irm.printValue(os, irm.vidToValue(vid), detailed, detailed, false) << "\n";
      os.flush();
      return PAQuery{IRParseMessage{buf}};
    }

    if (cmd == "local" || cmd == "loc") {
      auto [vidStr, unread2] = eatToken(unread);
      auto [skipStr, unread3] = eatToken(unread2);
      if (vidStr.empty() || !unread3.empty()) return PAQuery{SyntaxError{"local <vid> [n]"}};
      auto *F = llvm::dyn_cast<llvm::Function>(irm.vidToValue(parseVid(vidStr)));
      if (!F) return PAQuery{IRParseError{"vid not found or not a function vid"}};
      int skip = 0;
      if (!skipStr.empty()) skip = toIntStrict(skipStr);
      int cnt = 0;
      constexpr int maxLocals = 20;
      std::vector<llvm::Value *> locals;
      for (auto &Arg : F->args()) {
        if (cnt >= maxLocals) break;
        if (skip) { --skip; continue; }
        locals.push_back(&Arg);
        ++cnt;
      }
      for (auto &BB : *F) {
        for (auto &I : BB) {
          if (cnt >= maxLocals) break;
          if (skip) { --skip; continue; }
          locals.push_back(&I);
          ++cnt;
        }
      }
      std::string buf;
      llvm::raw_string_ostream os(buf);
      for (auto &local : locals)
        irm.printValue(os, local, detailed, detailed, false) << "\n";
      os.flush();
      return PAQuery{IRParseMessage{buf}};
    }
    
    if (cmd == "site") {
      auto [vidStr, unread2] = eatToken(unread);
      if (vidStr.empty()) return PAQuery{AllAllocSitesIn{}};
      if (!unread2.empty())
        return PAQuery{SyntaxError{"site [function]"}};
      if (auto F = llvm::dyn_cast<llvm::Function>(irm.vidToValue(parseVid(vidStr))))
        return PAQuery{AllocSitesIn{F}};
      return PAQuery{IRParseError{"vid not found or not a function vid"}};
    }

    if (cmd == "d" || cmd == "debug") {
      auto [vidStr, unread2] = eatToken(unread);
      if (vidStr.empty() || unread2.size())
        return PAQuery{SyntaxError{"debug <vid>"}};
      if (auto *V = irm.vidToValue(parseVid(vidStr))) {
        std::string buf;
        llvm::raw_string_ostream os{buf};
        irm.printValue(os, V, true, true, true).flush();
        return PAQuery{IRParseMessage{buf}};
      }
      return PAQuery{IRParseError{"vid not found"}};
    }

    if (cmd == "a" || cmd == "alias") {
      auto [aStr, unread2] = eatToken(unread);
      auto [bStr, unread3] = eatToken(unread2);
      if (aStr.empty() || bStr.empty() || unread3.size())
        return PAQuery{SyntaxError{"alias <vid> <vid>"}};
      auto a = irm.vidToValue(parseVid(aStr));
      auto b = irm.vidToValue(parseVid(bStr));
      if (!a || !b) return PAQuery{IRParseError{"vid not found"}};
      return PAQuery{AliasIn{a, b}};
    }

    if (cmd == "aliasset") {
      auto [ptrStr, unread2] = eatToken(unread);
      if (ptrStr.empty() || unread2.size())
        return PAQuery{SyntaxError{"aliasset <vid>"}};
      if (auto ptr = irm.vidToValue(parseVid(ptrStr)))
        return PAQuery{AliasSetIn{ptr}};
      return PAQuery{IRParseError{"vid not found"}};
    }

    if (cmd == "pts") {
      auto [ptrStr, unread2] = eatToken(unread);
      if (ptrStr.empty() || unread2.size())
        return PAQuery{SyntaxError{"pts <vid>"}};
      if (auto ptr = irm.vidToValue(parseVid(ptrStr)))
        return PAQuery{PtsIn{ptr}};
      return PAQuery{IRParseError{"vid not found"}};
    }

    if (cmd == "pt") {
      auto [ptrStr, unread2] = eatToken(unread);
      auto [objStr, unread3] = eatToken(unread2);
      if (ptrStr.empty() || objStr.empty() || unread3.size())
        return PAQuery{SyntaxError{"pt <ptr> <obj>"}};
      auto ptr = irm.vidToValue(parseVid(ptrStr));
      auto obj = irm.vidToValue(parseVid(objStr));
      if (!ptr || !obj) return PAQuery{IRParseError{"vid not found"}};
      return PAQuery{PtIn{ptr, obj}};
    }

    if (cmd == "reach") {
      auto [fromStr, unread2] = eatToken(unread);
      auto [toStr, unread3] = eatToken(unread2);
      if (fromStr.empty() || toStr.empty())
        return PAQuery{SyntaxError{"reach <from> <to>"}};
      if (auto from = llvm::dyn_cast<llvm::Function>(irm.vidToValue(parseVid(fromStr))))
        if (auto to = llvm::dyn_cast<llvm::Function>(irm.vidToValue(parseVid(toStr))))
          return PAQuery{ReachableIn{from, to}};
      return PAQuery{IRParseError{"vid(s) not found or not function vid(s)"}};
    }

    if (cmd == "callout") {
      auto [vidStr, unread2] = eatToken(unread);
      if (vidStr.empty() || unread2.size())
        return PAQuery{SyntaxError{"callout <vid>"}};
      if (auto F = llvm::dyn_cast<llvm::Function>(irm.vidToValue(parseVid(vidStr))))
        return PAQuery{CallOutEdgesIn{F}};
      return PAQuery{IRParseError{"vid not found or not a function vid"}};
    }

    if (cmd == "callin") {
      auto [vidStr, unread2] = eatToken(unread);
      if (vidStr.empty() || unread2.size())
        return PAQuery{SyntaxError{"callin <vid>"}};
      if (auto F = llvm::dyn_cast<llvm::Function>(irm.vidToValue(parseVid(vidStr))))
        return PAQuery{CallInEdgesIn{F}};
      return PAQuery{IRParseError{"vid not found or not a function vid"}};
    }

    if (cmd == "detail") {
      if (!unread.empty()) return PAQuery{SyntaxError{"too many arguments"}};
      detailed = !detailed;
      return PAQuery{IRParseMessage{"detailed = " + std::to_string(detailed)}};
    }

    if (cmd == "crash") {
      if (!unread.empty()) return PAQuery{SyntaxError{"too many arguments"}};
      return PAQuery{CrashTestIn{}};
    }

    return PAQuery{SyntaxError{"unknown command " + cmd}};
  } catch (const std::exception &e) {
    return PAQuery{IRParseError{e.what()}};
  }
}

std::string responseToString(const PAResponse &response, IRManager &irm) {
  return std::visit([&](const auto &arg) -> std::string {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, SyntaxError>)
      return "syntax: " + arg.message;
    if constexpr (std::is_same_v<T, IRParseError>)
      return "parse error: " + arg.message;
    if constexpr (std::is_same_v<T, AnalyzerError>)
      return "analyzer error: " + arg.message;
    if constexpr (std::is_same_v<T, IRParseMessage>)
      return arg.message;
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
          irm.printValue(os, v) << "\n";
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
          irm.printValue(os, v) << "\n";
        } catch (const std::exception &e) {
          os << e.what();
        }
      }
      os.flush();
      return buf;
    }

    if constexpr (std::is_same_v<T, PtOut>)
      return modalityToString(arg.result);
    
    if constexpr (std::is_same_v<T, ReachableOut> || std::is_same_v<T, CallOutEdgesOut> || std::is_same_v<T, CallInEdgesOut>) {
      std::string buf;
      llvm::raw_string_ostream os(buf);
      auto printCallEdge = [&](const ptacxx::CallEdge &edge) {
        os << (edge.type == ptacxx::CallEdge::DIRECT ? "direct" : 
            edge.type == ptacxx::CallEdge::INDIRECT ? "indirect" : "unknown")
          << " FROM ";
        irm.printValue(os, edge.caller, detailed, detailed, false) << " BY ";
        irm.printValue(os, edge.callsite, detailed, detailed, false) << " TO ";
        if (edge.type == ptacxx::CallEdge::CALLANYTHING) {
          os << "UNKNOWN";
        } else irm.printValue(os, edge.callee, detailed, detailed, false);
        os << "\n";
      };
      if constexpr (std::is_same_v<T, ReachableOut>) {
        for (auto &edge: arg.calledges) printCallEdge(edge);
      }
      else if constexpr (std::is_same_v<T, CallOutEdgesOut>) {
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
        irm.printValue(os, site.site);
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
