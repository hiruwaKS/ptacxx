#include "QueryInterface.h"
#include "LLVMUtils.h"

#include <llvm/ADT/DenseSet.h>
#include <llvm/Support/raw_ostream.h>

#include <type_traits>

static constexpr char DELIMITERS[] = " \t\r\n";

static bool detailed = true;
static bool stdSilence = true;
static bool llvmSilence = true;
static bool runtimeSilence = true;

static int toIntStrict(const std::string &s);
static std::string stripPrefix(const std::string &s, const char* deliminators = DELIMITERS);
static std::pair<std::string, std::string> eatToken(const std::string &s);
static VId parseVid(const std::string& vid);
static std::string modalityToString(ModalityResult modal);

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
          "  list | l [prefix]     list globals (including functions) or identified struct types, to see its vid, demangled prefix allowed\n"
          "  site [vid]            list allocation sites of a function or all\n"
          "  debug | d <vid> [n=1] print detailed value debug info, to locate and debug; n = number of subsequent neighbors to show\n"
          "  st <vid>              print struct type debug info\n"
          "  cg <vid> [n=5]        print call graph of a function; integer n: max depth\n"
          "  alias | a <vid> <vid> get alias result\n"
          "  aliasset <vid>        get alias set of a pointer (TODO)\n"
          "  pt <vid> <vid>        get points-to result\n"
          "  pts <vid>             get points-to set of a pointer\n"
          "  reach <vid> <vid> <i> get call-graph reachability; flag i: ignore unknown call\n"
          "  callout <vid> <i>     get outgoing calls of a function; flag i: ignore callsites, print a callee set\n"
          "  callin <vid> <i>      get incoming calls of a function; flag i: ignore callsites, print a caller set\n"
          "  detail                toggle vid printing mode\n"
          "  stds                  toggle whether to silence std:: globals\n"
          "  llvms                 toggle whether to silence llvm:: globals\n"
          "  rts                   toggle whether to silence runtime globals\n"
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
      llvm::DenseSet<VId> Seen;
      for (auto &[name, vid] : irm.listGlobal(namePrefix)) {
        if (!Seen.insert(vid).second) continue;
        if (auto *ST = irm.vidToIdStruct(vid)) {
          irm.printIdStructType(os, ST, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID)<< "\n";
        } else if (auto *value = irm.vidToValue(vid)) {
          if (stdSilence) if (auto GVal = llvm::dyn_cast<llvm::GlobalValue>(value))
            if (shouldSilence(GVal->getName().str())) continue;
          irm.printValue(os, value, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID) << "\n";
        }
      }
      os.flush();
      return PAQuery{IRParseMessage{buf}};
    }
    
    if (cmd == "site") {
      auto [vidStr, unread2] = eatToken(unread);
      if (vidStr.empty()) return PAQuery{AllAllocSitesIn{}};
      if (!unread2.empty())
        return PAQuery{SyntaxError{"site [function]"}};
      if (auto F = llvm::dyn_cast_or_null<llvm::Function>(irm.vidToValue(parseVid(vidStr))))
        return PAQuery{AllocSitesIn{F}};
      return PAQuery{IRParseError{"vid not found or not a function vid"}};
    }

    if (cmd == "cg") {
      auto [vidStr, unread2] = eatToken(unread);
      auto [nStr, unread3] = eatToken(unread2);
      if (vidStr.empty() || unread3.size())
        return PAQuery{SyntaxError{"cg <vid> [n=5]"}};
      unsigned n = 5;
      if (!nStr.empty()) n = static_cast<unsigned>(toIntStrict(nStr));
      if (auto F = llvm::dyn_cast_or_null<llvm::Function>(irm.vidToValue(parseVid(vidStr)))) {
        return PAQuery{CallGraphIn{F, n, stdSilence, llvmSilence}};
      }
      return PAQuery{IRParseError{"vid not found or not a function vid"}};
    }

    if (cmd == "d" || cmd == "debug") {
      auto [vidStr, unread2] = eatToken(unread);
      auto [nStr, unread3] = eatToken(unread2);
      if (vidStr.empty() || unread3.size())
        return PAQuery{SyntaxError{"debug <vid> [n=1]"}};
      int32_t n = 1;
      if (!nStr.empty()) n = toIntStrict(nStr);
      auto start = parseVid(vidStr);
      std::string buf;
      llvm::raw_string_ostream os{buf};
      for (int32_t i = 0; i < n; i++) {
        if (auto *V = irm.vidToValue(start+i))
          irm.printValue(os, V, IRManager::PRT_DEBUG);
      }
      os.flush();
      if (buf.size()) return PAQuery{IRParseMessage{buf}};
      return PAQuery{IRParseError{"vid not found"}};
    }

    if (cmd == "st") {
      auto [vidStr, unread2] = eatToken(unread);
      if (vidStr.empty() || unread2.size())
        return PAQuery{SyntaxError{"st <vid>"}};
      auto vid = parseVid(vidStr);
      std::string buf;
      llvm::raw_string_ostream os{buf};
      if (auto *ST = irm.vidToIdStruct(vid))
        irm.printIdStructType(os, ST, IRManager::PRT_DEBUG);
      os.flush();
      if (buf.size()) return PAQuery{IRParseMessage{buf}};
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
      auto [iStr, unread4] = eatToken(unread3);
      if (fromStr.empty() || toStr.empty() || (iStr.size() && iStr != "i"))
        return PAQuery{SyntaxError{"reach <from> <to> <i>"}};
      if (auto from = llvm::dyn_cast_or_null<llvm::Function>(irm.vidToValue(parseVid(fromStr))))
        if (auto to = llvm::dyn_cast_or_null<llvm::Function>(irm.vidToValue(parseVid(toStr))))
          return PAQuery{ReachableIn{from, to, !!iStr.size()}};
      return PAQuery{IRParseError{"vid(s) not found or not function vid(s)"}};
    }

    if (cmd == "callout") {
      auto [vidStr, unread2] = eatToken(unread);
      auto [iStr, unread3] = eatToken(unread2);
      if (vidStr.empty() || unread3.size() || (iStr.size() && iStr != "i"))
        return PAQuery{SyntaxError{"callout <vid> <i>"}};
      if (auto F = llvm::dyn_cast_or_null<llvm::Function>(irm.vidToValue(parseVid(vidStr))))
        return PAQuery{CallOutEdgesIn{F, !!iStr.size()}};
      return PAQuery{IRParseError{"vid not found or not a function vid"}};
    }

    if (cmd == "callin") {
      auto [vidStr, unread2] = eatToken(unread);
      auto [iStr, unread3] = eatToken(unread2);
      if (vidStr.empty() || unread2.size() || (iStr.size() && iStr != "i"))
        return PAQuery{SyntaxError{"callin <vid> <i>"}};
      if (auto F = llvm::dyn_cast_or_null<llvm::Function>(irm.vidToValue(parseVid(vidStr))))
        return PAQuery{CallInEdgesIn{F, !!iStr.size()}};
      return PAQuery{IRParseError{"vid not found or not a function vid"}};
    }

    if (cmd == "detail") {
      if (!unread.empty()) return PAQuery{SyntaxError{"too many arguments"}};
      detailed = !detailed;
      return PAQuery{IRParseMessage{"detailed = " + std::to_string(detailed)}};
    }

    if (cmd == "stds") {
      if (!unread.empty()) return PAQuery{SyntaxError{"too many arguments"}};
      stdSilence = !stdSilence;
      return PAQuery{IRParseMessage{"stdSilence = " + std::to_string(stdSilence)}};
    }

    if (cmd == "llvms") {
      if (!unread.empty()) return PAQuery{SyntaxError{"too many arguments"}};
      llvmSilence = !llvmSilence;
      return PAQuery{IRParseMessage{"llvmSilence = " + std::to_string(llvmSilence)}};
    }

    if (cmd == "rts") {
      if (!unread.empty()) return PAQuery{SyntaxError{"too many arguments"}};
      runtimeSilence = !runtimeSilence;
      return PAQuery{IRParseMessage{"runtimeSilence = " + std::to_string(runtimeSilence)}};
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
          irm.printValue(os, v, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID) << "\n";
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
          irm.printValue(os, v, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID) << "\n";
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
      std::unordered_set<ptacxx::CallEdgeIgnoreCS, ptacxx::CallEdgeIgnoreCSHash> edgeigns;
      auto printCallEdge = [&](const ptacxx::CallEdge &edge, bool noFrom=false, bool noTo=false, bool ignoreCS=false) {
        if (ignoreCS) {
          auto edgeign = ptacxx::CallEdgeIgnoreCS{edge.caller, edge.callee};
          if (edgeigns.find(edgeign) == edgeigns.end()) edgeigns.insert(edgeign);
          else return;
        }
        if (stdSilence && (edge.type != ptacxx::CallEdge::CALLANYTHING && shouldSilence(edge.callee->getName().str()))) return;
        os << (edge.type == ptacxx::CallEdge::DIRECT ? "direct" : 
            edge.type == ptacxx::CallEdge::INDIRECT ? "indirect" : "unknown");
        if (!noFrom) {
          os << " FROM ";
          irm.printValue(os, edge.caller, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID);
        }
        if (!noTo) {
          os << " TO ";
          if (edge.type == ptacxx::CallEdge::CALLANYTHING) {
            os << "UNKNOWN";
          } else irm.printValue(os, edge.callee, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID);
        }
        if (!ignoreCS) {
          os << " BY ";
          irm.printValue(os, edge.callsite, IRManager::PRT_VID);
        }
        os << "\n";
      };
      if constexpr (std::is_same_v<T, ReachableOut>) {
        for (auto &edge: arg.calledges) printCallEdge(edge);
      }
      else if constexpr (std::is_same_v<T, CallOutEdgesOut>) {
        for (auto edge: arg.calledges) printCallEdge(edge, true, false, arg.ignoreCS);
      } else {
        for (auto edge: arg.inCalledges) printCallEdge(edge, false, true, arg.ignoreCS);
        os << "\n";
        for (auto edge: arg.inCallAnything) {
          printCallEdge(edge, false, true, arg.ignoreCS);
          if (arg.ignoreCS) break;
        }
      }
      os.flush();
      return buf;
    }

    if constexpr (std::is_same_v<T, CallGraphOut>) {
      std::string buf;
      llvm::raw_string_ostream os(buf);
      auto &root = arg.cg.first;
      auto &tree = arg.cg.second;
      std::vector<std::string> padding;
      std::vector<llvm::Function *> ances;
      llvm::DenseSet<llvm::Function *> visited; // forward and cross
      std::function<void(llvm::Function*)> dfs = 
        [&](llvm::Function *F) {
          for (auto &p: padding) os << p;
          if (padding.size() && padding.back() == "└ ") {
            padding.pop_back();
            padding.push_back("  ");
          }
          if (F) {
            irm.printValue(os, F, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID);
            bool isBackedge = false;
            for (auto &ance: ances) if (F == ance) { isBackedge = true; break; }
            if (isBackedge) os << " (recursive)\n";
            else if (visited.find(F) != visited.end()) os << " (visited)\n";
            else {
              os << "\n";
              auto it = tree.find(F);
              if (it != tree.end()) {
                auto &callees = it->second;
                size_t count = callees.size();
                size_t idx = 0;
                if (padding.size() && padding.back() == "├ ") {
                  padding.pop_back();
                  padding.push_back("│ ");
                }
                padding.push_back("├ ");
                ances.push_back(F);
                for (auto *callee : callees) {
                  if (idx == count - 1) {
                    padding.pop_back();
                    padding.push_back("└ ");
                  }
                  dfs(callee);
                  ++idx;
                }
                ances.pop_back();
                padding.pop_back();
                if (padding.size() && padding.back() == "│ ") {
                  padding.pop_back();
                  padding.push_back("├ ");
                }
              }
            }
            visited.insert(F);
          } else os << "call to UNKNOWN\n";
        };
      dfs(root);
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
        irm.printValue(os, site.site, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID);
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

bool shouldSilence(const std::string& mangledName) {
  return shouldSilence(mangledName, stdSilence, llvmSilence, runtimeSilence);
}
