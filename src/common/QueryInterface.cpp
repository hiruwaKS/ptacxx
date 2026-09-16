#include "QueryInterface.h"
#include "LLVMUtils.h"
#include "Error.h"

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>

LLVM_CL_IGNORE_WARNINGS_BEGIN
namespace ptacxx::options {
extern std::string CGPatchPath;
std::string CGPatchPath;
extern std::string NoteFolderPath;
std::string NoteFolderPath;
}
LLVM_CL_IGNORE_WARNINGS_END

static bool detailed = true;
static bool stdSilence = true;
static bool llvmSilence = true;
static bool runtimeSilence = true;

static int toIntStrict(const std::string &s);
static std::string stripPrefix(const std::string &s);
static std::pair<std::string, std::string> eatToken(const std::string &s);
static VId parseVid(const std::string& vid, IRManager &irm);
static std::string modalityToString(ModalityResult modal);
static std::string encode(const std::string &s);
[[maybe_unused]] static std::string encodeIfNecessary(const std::string &s);

int toIntStrict(const std::string &s) {
  std::size_t pos = 0;
  try {
    int val = std::stoi(s, &pos);
    if (pos == s.length()) return val;
  } catch (const std::exception &) {
    // fall through to the typed error below
  }
  throw ptacxx::VidNotFound("vidnotfound-invalid-integer",
                            "invalid integer in: " + s);
}

std::string stripPrefix(const std::string &s) {
  size_t begin = 0;
  while (begin < s.size() && s[begin]==' ') ++begin;
  return s.substr(begin, s.size() - begin);
}

std::pair<std::string, std::string> eatToken(const std::string &s) {
  const std::string input = stripPrefix(s);
  std::string decoded;
  decoded.reserve(input.size());
  bool inQuotes = false;
  for (size_t i = 0; i < input.size();) {
    const char c = input[i];
    if (c == '\\') {
      if (i + 1 == input.size()) {
        decoded.push_back('\\');
        ++i;
      } else {
        decoded.push_back(input[i + 1]);
        i += 2;
      }
      continue;
    }
    if (c == '"') {
      inQuotes = !inQuotes;
      ++i;
      continue;
    }
    if (c == ' ' && !inQuotes)
      return {decoded, input.substr(i)};
    decoded.push_back(c);
    ++i;
  }
  return {decoded, ""};
}

VId parseVid(const std::string& vid, IRManager &irm) {
  if (!vid.empty() && vid[0] == '@') {
    const std::string name = vid.substr(1);
    const auto entries = irm.listGlobal(name);
    llvm::SmallDenseSet<VId> notSilenced;
    for (const GlobalEntry &entry : entries) {
      if (notSilenced.count(entry.id)) continue;
      if (auto *ST = irm.vidToIdStruct(entry.id)) notSilenced.insert(entry.id);
      else if (auto *value = irm.vidToValue(entry.id)) {
        if (auto GVal = llvm::dyn_cast<llvm::GlobalValue>(value)) {
          if (shouldSilence(GVal->getName().str())) continue;
        }
        notSilenced.insert(entry.id);
      }
    }
    if (notSilenced.empty())
      throw ptacxx::VidNotFound("vidnotfound-global-name-not-found", "not found: " + vid);
    if (notSilenced.size() == 1)
      return *notSilenced.begin();
    std::string buf;
    llvm::raw_string_ostream os(buf);
    os << "ambiguous:\n";
    for (const VId vid2: notSilenced) {
      if (auto *ST = irm.vidToIdStruct(vid2)) {
        irm.printIdStructType(os, ST, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID) << "\n";
      } else if (auto *value = irm.vidToValue(vid2)) {
        if (auto GVal = llvm::dyn_cast<llvm::GlobalValue>(value))
          if (shouldSilence(GVal->getName().str())) continue;
        irm.printValue(os, value, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID) << "\n";
      }
    }
    os.flush();
    throw ptacxx::VidNotFound("vidnotfound-global-name-ambiguous", buf);  
  }
  return static_cast<int32_t>(toIntStrict(vid));
}

/// Resolve a vid string to a Value; throws a shared VidNotFound on failure.
static llvm::Value *requireValue(const std::string &vidStr, IRManager &irm,
                                 const std::string &msg = "vid not found") {
  llvm::Value *V = irm.vidToValue(parseVid(vidStr, irm));
  if (!V) throw ptacxx::VidNotFound("vidnotfound-vid-not-found", msg);
  return V;
}

/// Resolve a vid string to a Function; throws a shared VidNotFound on failure.
static llvm::Function *requireFunction(const std::string &vidStr, IRManager &irm,
                                       const std::string &msg =
                                           "vid not found or not a function vid") {
  auto *F = llvm::dyn_cast_or_null<llvm::Function>(
      irm.vidToValue(parseVid(vidStr, irm)));
  if (!F) throw ptacxx::VidNotFound("vidnotfound-vid-not-found", msg);
  return F;
}

/// Resolve a vid string to an identified StructType.
static llvm::StructType *requireStruct(const std::string &vidStr, IRManager &irm,
                                       const std::string &msg = "vid not found") {
  auto *ST = irm.vidToIdStruct(parseVid(vidStr, irm));
  if (!ST) throw ptacxx::VidNotFound("vidnotfound-vid-not-found", msg);
  return ST;
}

/// Require an already-resolved global entry to be a Function.
static llvm::Function *requireFunction(const GlobalEntry &entry, IRManager &irm,
                                       const std::string &msg =
                                           "vid not found or not a function vid") {
  auto *F = llvm::dyn_cast_or_null<llvm::Function>(irm.vidToValue(entry.id));
  if (!F) throw ptacxx::VidNotFound("vidnotfound-vid-not-found", msg);
  return F;
}

/// Resolve a vid to either a Value or an identified StructType.
static std::pair<llvm::Value *, llvm::StructType *>
requireValueOrStruct(VId vid, IRManager &irm,
                     const std::string &msg = "vid not found") {
  llvm::Value *V = irm.vidToValue(vid);
  llvm::StructType *ST = V ? nullptr : irm.vidToIdStruct(vid);
  if (!ST && !V) throw ptacxx::VidNotFound("vidnotfound-vid-not-found", msg);
  return {V, ST};
}

std::string modalityToString(ModalityResult modal) {
  return modal == ResultMust ? "Must" : modal == ResultMay ? "May" : "No";
}

static std::string encode(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    if (c == '"' || c == '\\')
      out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

[[maybe_unused]] static std::string encodeIfNecessary(const std::string &s) {
  if (s.find_first_of(" \"\\") == std::string::npos)
    return s;
  return encode(s);
}

static std::optional<std::string_view> isKeyPoint(const std::string &buf, size_t pos,
                                                  const std::string &prefix) {
  if (pos == buf.size()) return std::string_view("\xFF");
  if (pos != 0 && buf[pos - 1] != '\n') return std::nullopt;
  size_t end = buf.find('\n', pos);
  if (end == std::string::npos) end = buf.size();
  auto line = std::string_view(buf).substr(pos, end - pos);
  if (line.starts_with(std::string_view(prefix))) return line;
  if (pos == 0) return std::string_view("");
  return std::nullopt;
}

static std::string_view readRecord(const std::string &buf, size_t pos,
                                   const std::string &prefix) {
  if (!isKeyPoint(buf, pos, prefix).has_value())
    throw ptacxx::NoteRelated("noterelated-record-pos-not-key-point",
                              "pos is not a key point");
  if (pos == buf.size()) return std::string_view(buf).substr(pos, 0);
  size_t end = pos + 1;
  while (end < buf.size() && !isKeyPoint(buf, end, prefix).has_value()) ++end;
  return std::string_view(buf).substr(pos, end - pos);
}

static std::string_view keyAt(const std::string &buf, size_t pos,
                              const std::string &prefix) {
  size_t p = pos;
  while (true) if (auto key = isKeyPoint(buf, p--, prefix)) return *key;
}

static bool findLowerboundWithLinePrefix(const std::string &buf,
                                         size_t &out,
                                         const std::string &prefix,
                                         const std::string &key) {
  size_t left = 0;
  size_t right = buf.size();
  const std::string &fullKey = prefix + key;

  while (left < right) {
    size_t mid = left + (right - left) / 2;
    if (keyAt(buf, mid, prefix).compare(fullKey) < 0) left = mid + 1;
    else right = mid;
  }
  out = left;
  return left != buf.size() && keyAt(buf, left, prefix).compare(fullKey) == 0;
}

static std::string readAll(std::ifstream &in) {
  in.clear();
  in.seekg(0, std::ios::end);
  const std::streamoff size = static_cast<std::streamoff>(in.tellg());
  in.clear();
  in.seekg(0, std::ios::beg);
  if (size <= 0) return {};

  std::string buf(static_cast<size_t>(size), '\0');
  in.read(buf.data(), static_cast<std::streamsize>(size));
  in.clear();
  return buf;
}

static unsigned getLineno(const std::string &buf, size_t pos) {
  unsigned lineno = 1;
  for (size_t i = 0; i < pos; ++i)
    if (buf[i] == '\n') ++lineno;
  return lineno;
}

enum class ProofStatus {
  Proven,
  ConditionalProven,
  Unproven
};

struct NodeKey {
  std::string func;
  std::string pp;
  int tag = 0;

  bool operator==(const NodeKey &RHS) const {
    return tag == RHS.tag && func == RHS.func && pp == RHS.pp;
  }
};

struct NodeKeyInfo {
  static NodeKey getEmptyKey() {
    return NodeKey{"", "", 1};
  }
  static NodeKey getTombstoneKey() {
    return NodeKey{"", "", 2};
  }
  static unsigned getHashValue(const NodeKey &K) {
    return static_cast<unsigned>(llvm::hash_combine(K.tag, K.func, K.pp));
  }
  static bool isEqual(const NodeKey &LHS, const NodeKey &RHS) {
    return LHS == RHS;
  }
};

static std::vector<std::string> splitNoteLines(std::string_view s) {
  std::vector<std::string> lines;
  for (size_t start = 0; start < s.size();) {
    const size_t end = s.find('\n', start);
    if (end == std::string_view::npos) {
      lines.emplace_back(s.substr(start));
      break;
    }
    lines.emplace_back(s.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

static std::string getNoteFile(const std::string &debugPath, bool &found,
                               bool createIfMissing = false) {
  found = false;
  if (ptacxx::options::NoteFolderPath.empty() || debugPath.empty()) return {};

  const std::string sourceName = std::filesystem::path(debugPath).filename().string();
  std::string path;
  for (unsigned i = 1;; ++i) {
    std::string noteName = sourceName + ".md";
    if (i > 1) noteName = sourceName + "." + std::to_string(i) + ".md";
    path = ptacxx::options::NoteFolderPath + "/" + noteName;

    std::ifstream in(path);
    if (!in) {
      if (createIfMissing) {
        std::ofstream out(path);
        if (!out)
          throw ptacxx::FileSystemError("filesystemerror-note-file-create-failed",
                                        "cannot create " + path);
        out << debugPath << "\n";
        out.close();
        if (!out)
          throw ptacxx::FileSystemError("filesystemerror-note-file-write-failed",
                                        "cannot write " + path);
        found = true;
      }
      break;
    }
    std::string firstLine;
    if (std::getline(in, firstLine) && firstLine == debugPath) {
      found = true;
      break;
    }
  }
  return path;
}

static std::optional<bool> isTheorem(std::string_view record,
                                     const std::string &pp) {
  const size_t headingEnd = record.find('\n');
  const std::string_view body =
      headingEnd == std::string_view::npos ? std::string_view{} :
                                             record.substr(headingEnd + 1);
  const std::string conjectureLine = "- Conjecture: " + pp;
  const std::string theoremLine = "- Theorem: " + pp;
  for (const std::string &line : splitNoteLines(body)) {
    if (line == conjectureLine) return false;
    if (line == theoremLine) return true;
  }
  return std::nullopt;
}

static llvm::SmallVector<NodeKey, 4> getWhens(std::string_view record,
                                              const std::string &pp) {
  const size_t headingEnd = record.find('\n');
  const std::string_view body =
      headingEnd == std::string_view::npos ? std::string_view{} :
                                             record.substr(headingEnd + 1);
  const std::vector<std::string> lines = splitNoteLines(body);

  const std::string conjectureLine = "- Conjecture: " + pp;
  const std::string theoremLine = "- Theorem: " + pp;
  size_t target = std::string::npos;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i] == conjectureLine || lines[i] == theoremLine) {
      target = i;
      break;
    }
  }
  if (target == std::string::npos) return {};

  llvm::SmallVector<NodeKey, 4> whens;
  constexpr std::string_view whenPrefix = "  - When: ";
  constexpr std::string_view childPrefix = "    - ";
  for (size_t i = target + 1; i < lines.size(); ++i) {
    const std::string &line = lines[i];
    if (line.empty() || (line[0] != ' ' && line[0] != '\t')) break;
    if (!std::string_view(line).starts_with(whenPrefix)) continue;

    const std::string func(line.substr(whenPrefix.size()));
    if (func.empty()) continue;
    for (size_t j = i + 1;
         j < lines.size() && std::string_view(lines[j]).starts_with(childPrefix);
         ++j) {
      const std::string ppName(
          std::string_view(lines[j]).substr(childPrefix.size()));
      if (!ppName.empty())
        whens.push_back(NodeKey{func, ppName});
    }
  }
  return whens;
}

void loadCGPatch(IRManager &irm, CGPatchMap &out) {
  if (ptacxx::options::CGPatchPath.empty()) return;
  std::ifstream in(ptacxx::options::CGPatchPath);
  if (!in)
    throw ptacxx::FileSystemError("filesystemerror-cgpatch-open-failed",
                                  "cannot open " + ptacxx::options::CGPatchPath);
  out.clear();
  std::string caller, callee;
  while (std::getline(in, caller)) {
    if (caller.empty()) continue;
    if (!std::getline(in, callee) || !callee.size())
      throw ptacxx::SemanticError("semanticerror-cgpatch-missing-callee", "missing callee");
    auto *f1 = requireFunction(irm.getGlobal(caller), irm);
    auto *f2 = requireFunction(irm.getGlobal(callee), irm);
    out[f1].push_back(f2);
  }
}

PAQuery parse(const std::string &input, IRManager &irm) {
  {
    auto [cmd, unread] = eatToken(input);
    if (cmd.empty())
      throw ptacxx::SyntaxError("syntaxerror-empty-input", "empty input");

    if (cmd == "help" || cmd == "h" || cmd == "?") {
      if (!unread.empty())
        throw ptacxx::SyntaxError("syntaxerror-help-too-many-arguments", "too many arguments");
      return PAQuery{IRParseMessage{
          "commands:\n"
          "  stat | s                  show IR metadata and statistics\n"
          "  <vid> [<vid> ...]         print detailed value debug info for one or more vids\n"
          "  <vid> -c <i>              print detailed value debug info for vid and the next i-1 vids\n"
          "  site [vid]                list allocation sites of a function or all\n"
          "  st <vid>                  print struct type debug info\n"
          "  note/n <vid> [note/-d/-f] note - view/append/delete; -f prints note file location\n"
          "  n <vid> -c <con> <dsc>    note - add a conjecture (e.g. main-not-fail) to a function with description\n"
          "  n <vid> -p <con> <prf>    note - claim to prove a conjecture of a function, then it becomes a theorem\n"
          "  n <vid> -a <con> \n"
          "                <vid> <pp>  note - add a proposition (conjecture or theorem, of a function) as premise to a conjecture\n"
          "  n <vid> -r <con> \n"
          "                <vid> <pp>  note - remove a premise from a conjecture\n"
          "  tv <vid> <pp>             tree view of a proposition\n"
          "  cg <vid> [n=5]            print call graph of a function; integer n: max depth\n"
          "  alias | a <vid> <vid>     get alias result\n"
          "  aliasset <vid>            get alias set of a pointer (TODO)\n"
          "  pt <vid> <vid>            get points-to result\n"
          "  pts <vid>                 get points-to set of a pointer\n"
          "  reach <vid> <vid> <i>     get call-graph reachability; flag i: ignore unknown call\n"
          "  callout <vid> <i>         get outgoing calls of a function; flag i: ignore callsites, print a callee set\n"
          "  callin <vid> <i>          get incoming calls of a function; flag i: ignore callsites, print a caller set\n"
          "  cgpatch <vid> <vid>       append a call graph patch edge to CGPatch file\n"
          "  cgreload                  rebuild call graph\n"
          "  detail                    toggle vid printing mode\n"
          "  stds                      toggle whether to silence std:: globals\n"
          "  llvms                     toggle whether to silence llvm:: globals\n"
          "  rts                       toggle whether to silence runtime globals\n"
          "  test                      run probe tests\n"
          "  help | h                  show help\n"
          "notes:\n"
          "  prefix in func/global/identified structType can be a demangled name, which contains ' ' '(' ')' chars sometimes\n"
          "  vid can be number or @[prefix], like @main, @foo::foo, @_ZTVN4llvm11raw_ostreamE, as long as it is unique\n"
        }};
    }

    if (cmd == "stat" || cmd == "s") {
      if (!unread.empty())
        throw ptacxx::SyntaxError("syntaxerror-stat-too-many-arguments", "too many arguments");
      std::string buf;
      llvm::raw_string_ostream os{buf};
      irm.printStat(os).flush();
      return PAQuery{IRParseMessage{buf}};
    }

    if (cmd[0] == '@' || isdigit(cmd[0])) {
      std::string buf;
      llvm::raw_string_ostream os{buf};
      bool found = false;
      unsigned silenced = 0;
      auto silenceName = [](llvm::Value *V) -> std::string {
        if (auto *F = llvm::dyn_cast<llvm::Function>(V)) return F->getName().str();
        if (auto *GV = llvm::dyn_cast<llvm::GlobalValue>(V)) return GV->getName().str();
        if (auto *I = llvm::dyn_cast<llvm::Instruction>(V))
          if (I->getFunction()) return I->getFunction()->getName().str();
        if (auto *BB = llvm::dyn_cast<llvm::BasicBlock>(V))
          if (BB->getParent()) return BB->getParent()->getName().str();
        return "";
      };
      auto printSilence = [&](unsigned cnt) {
        if (cnt == 0) return;
        os << "(silence " << cnt << " query)\n";
      };
      auto handleVid = [&](int32_t vid, bool &any) {
        if (auto *V = irm.vidToValue(vid)) {
          std::string name = silenceName(V);
          if (name.empty() || !shouldSilence(name)) {
            printSilence(silenced);
            irm.printValue(os, V, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID);
            any = true;
          } else {
            ++silenced;
            any = true;
          }
        }
        if (auto *ST = irm.vidToIdStruct(vid)) {
          printSilence(silenced);
          irm.printIdStructType(os, ST, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID);
          any = true;
        }
      };
      auto [arg1, unread2] = eatToken(unread);
      if (arg1 == "-c") {
        auto [countStr, unread3] = eatToken(unread2);
        if (countStr.empty() || stripPrefix(unread3).size())
          throw ptacxx::SyntaxError("syntaxerror-vid-range-usage", "<vid> -c <i>");
        int32_t n = toIntStrict(countStr);
        auto start = parseVid(cmd, irm);
        for (int32_t i = 0; i < n; i++) handleVid(start + i, found);
      } else {
        std::string vidStr = cmd;
        std::string remaining = unread;
        while (!vidStr.empty()) {
          auto vid = parseVid(vidStr, irm);
          handleVid(vid, found);
          vidStr = arg1;
          remaining = unread2;
          std::tie(arg1, unread2) = eatToken(remaining);
          if (vidStr == "-c")
            throw ptacxx::SyntaxError("syntaxerror-vid-range-usage-in-list", "<vid> -c <i>");
        }
      }
      printSilence(silenced);
      os.flush();
      if (buf.size() && found) return PAQuery{IRParseMessage{buf}};
      throw ptacxx::VidNotFound("vidnotfound-vid-not-found", "vid not found");
    }
    
    if (cmd == "site") {
      auto [vidStr, unread2] = eatToken(unread);
      if (vidStr.empty()) return PAQuery{AllAllocSitesIn{}};
      if (!unread2.empty())
        throw ptacxx::SyntaxError("syntaxerror-site-usage", "site [function]");
      auto *F = requireFunction(vidStr, irm);
      return PAQuery{AllocSitesIn{F}};
    }

    if (cmd == "cg") {
      auto [vidStr, unread2] = eatToken(unread);
      auto [nStr, unread3] = eatToken(unread2);
      if (vidStr.empty() || unread3.size())
        throw ptacxx::SyntaxError("syntaxerror-cg-usage", "cg <vid> [n=5]");
      unsigned n = 5;
      if (!nStr.empty()) n = static_cast<unsigned>(toIntStrict(nStr));
      auto *F = requireFunction(vidStr, irm);
      return PAQuery{CallGraphIn{F, n, stdSilence, llvmSilence}};
    }

    if (cmd == "st") {
      auto [vidStr, unread2] = eatToken(unread);
      if (vidStr.empty() || unread2.size())
        throw ptacxx::SyntaxError("syntaxerror-st-usage", "st <vid>");
      auto *ST = requireStruct(vidStr, irm);
      std::string buf;
      llvm::raw_string_ostream os{buf};
      irm.printIdStructType(os, ST, IRManager::PRT_DEBUG);
      os.flush();
      return PAQuery{IRParseMessage{buf}};
    }

    if (cmd == "a" || cmd == "alias") {
      auto [aStr, unread2] = eatToken(unread);
      auto [bStr, unread3] = eatToken(unread2);
      if (aStr.empty() || bStr.empty() || unread3.size())
        throw ptacxx::SyntaxError("syntaxerror-alias-usage", "alias <vid> <vid>");
      auto *a = requireValue(aStr, irm);
      auto *b = requireValue(bStr, irm);
      return PAQuery{AliasIn{a, b}};
    }

    if (cmd == "aliasset") {
      auto [ptrStr, unread2] = eatToken(unread);
      if (ptrStr.empty() || unread2.size())
        throw ptacxx::SyntaxError("syntaxerror-aliasset-usage", "aliasset <vid>");
      auto *ptr = requireValue(ptrStr, irm);
      return PAQuery{AliasSetIn{ptr}};
    }

    if (cmd == "pts") {
      auto [ptrStr, unread2] = eatToken(unread);
      if (ptrStr.empty() || unread2.size())
        throw ptacxx::SyntaxError("syntaxerror-pts-usage", "pts <vid>");
      auto *ptr = requireValue(ptrStr, irm);
      return PAQuery{PtsIn{ptr}};
    }

    if (cmd == "pt") {
      auto [ptrStr, unread2] = eatToken(unread);
      auto [objStr, unread3] = eatToken(unread2);
      if (ptrStr.empty() || objStr.empty() || unread3.size())
        throw ptacxx::SyntaxError("syntaxerror-pt-usage", "pt <ptr> <obj>");
      auto *ptr = requireValue(ptrStr, irm);
      auto *obj = requireValue(objStr, irm);
      return PAQuery{PtIn{ptr, obj}};
    }

    if (cmd == "pts-test") {
      auto [pathStr, unread2] = eatToken(unread);
      if (pathStr.empty())
        throw ptacxx::SyntaxError("syntaxerror-pts-test-usage",
                                  "pts-test <dump-file-path> [-c]");
      auto [flagStr, unread3] = eatToken(unread2);
      bool consistent = false;
      if (flagStr == "-c") {
        if (stripPrefix(unread3).size())
          throw ptacxx::SyntaxError("syntaxerror-pts-test-usage",
                                    "pts-test <dump-file-path> [-c]");
        consistent = true;
      } else if (!flagStr.empty()) {
        throw ptacxx::SyntaxError("syntaxerror-pts-test-usage",
                                  "pts-test <dump-file-path> [-c]");
      }
      std::ifstream in(pathStr);
      if (!in)
        throw ptacxx::FileSystemError("filesystemerror-pts-dump-open-failed",
                                      "cannot open pts dump '" + pathStr + "'");
      std::vector<std::pair<Ptr, std::vector<VId>>> records;
      std::string line;
      // The dump starts with a "pts" magic line; check and ignore it.
      if (std::getline(in, line) && line != "pts")
        in.seekg(0);
      while (std::getline(in, line)) {
        if (line.empty()) continue;
        // v2 record: "key;target,ctx,...;target,..."
        std::vector<std::string> segs;
        size_t start = 0;
        while (true) {
          const size_t pos = line.find(';', start);
          segs.push_back(line.substr(
              start, pos == std::string::npos ? std::string::npos : pos - start));
          if (pos == std::string::npos) break;
          start = pos + 1;
        }
        if (segs.empty() || segs[0].empty()) continue;
        Ptr ptr = irm.vidToValue(parseVid(segs[0], irm));
        if (!ptr) continue;
        std::vector<VId> targets;
        for (size_t i = 1; i < segs.size(); ++i) {
          if (segs[i].empty()) continue;
          const size_t comma = segs[i].find(',');
          const std::string tstr = segs[i].substr(0, comma);
          targets.push_back(parseVid(tstr, irm));
        }
        records.emplace_back(ptr, std::move(targets));
      }
      return PAQuery{PtsTestIn{std::move(records), consistent}};
    }

    if (cmd == "reach") {
      auto [fromStr, unread2] = eatToken(unread);
      auto [toStr, unread3] = eatToken(unread2);
      auto [iStr, unread4] = eatToken(unread3);
      if (fromStr.empty() || toStr.empty() || (iStr.size() && iStr != "i"))
        throw ptacxx::SyntaxError("syntaxerror-reach-usage", "reach <from> <to> <i>");
      const char *notFn = "vid(s) not found or not function vid(s)";
      auto *from = requireFunction(fromStr, irm, notFn);
      auto *to = requireFunction(toStr, irm, notFn);
      return PAQuery{ReachableIn{from, to, !!iStr.size()}};
    }

    if (cmd == "callout") {
      auto [vidStr, unread2] = eatToken(unread);
      auto [iStr, unread3] = eatToken(unread2);
      if (vidStr.empty() || unread3.size() || (iStr.size() && iStr != "i"))
        throw ptacxx::SyntaxError("syntaxerror-callout-usage", "callout <vid> <i>");
      auto *F = requireFunction(vidStr, irm);
      return PAQuery{CallOutEdgesIn{F, !!iStr.size()}};
    }

    if (cmd == "callin") {
      auto [vidStr, unread2] = eatToken(unread);
      auto [iStr, unread3] = eatToken(unread2);
      if (vidStr.empty() || unread2.size() || (iStr.size() && iStr != "i"))
        throw ptacxx::SyntaxError("syntaxerror-callin-usage", "callin <vid> <i>");
      auto *F = requireFunction(vidStr, irm);
      return PAQuery{CallInEdgesIn{F, !!iStr.size()}};
    }

    if (cmd == "cgpatch") {
      if (ptacxx::options::CGPatchPath.empty())
        throw ptacxx::ConfigError("configerror-cgpatch-path-empty", "cgpatch path is empty");
      auto [fromStr, unread2] = eatToken(unread);
      auto [toStr, unread3] = eatToken(unread2);
      if (fromStr.empty() || toStr.empty() || unread3.size())
        throw ptacxx::SyntaxError("syntaxerror-cgpatch-usage", "cgpatch <from-vid> <to-vid>");
      const char *notFn = "vid(s) not found or not function vid(s)";
      auto *caller = requireFunction(fromStr, irm, notFn);
      auto *callee = requireFunction(toStr, irm, notFn);
      std::ofstream out(ptacxx::options::CGPatchPath, std::ios::app);
      if (!out)
        throw ptacxx::FileSystemError("filesystemerror-cgpatch-append-open-failed",
                                      "cannot open " + ptacxx::options::CGPatchPath);
      out << caller->getName().str() << "\n" << callee->getName().str() << "\n\n";
      return PAQuery{IRParseMessage{"ok"}};
    }

    if (cmd == "cgreload") {
      if (!unread.empty())
        throw ptacxx::SyntaxError("syntaxerror-cgreload-usage", "cgreload");
      return PAQuery{CGReloadIn{}};
    }

    if (cmd == "detail") {
      if (!unread.empty())
        throw ptacxx::SyntaxError("syntaxerror-detail-too-many-arguments", "too many arguments");
      detailed = !detailed;
      return PAQuery{IRParseMessage{"detailed = " + std::to_string(detailed)}};
    }

    if (cmd == "stds") {
      if (!unread.empty())
        throw ptacxx::SyntaxError("syntaxerror-stds-too-many-arguments", "too many arguments");
      stdSilence = !stdSilence;
      return PAQuery{IRParseMessage{"stdSilence = " + std::to_string(stdSilence)}};
    }

    if (cmd == "llvms") {
      if (!unread.empty())
        throw ptacxx::SyntaxError("syntaxerror-llvms-too-many-arguments", "too many arguments");
      llvmSilence = !llvmSilence;
      return PAQuery{IRParseMessage{"llvmSilence = " + std::to_string(llvmSilence)}};
    }

    if (cmd == "rts") {
      if (!unread.empty())
        throw ptacxx::SyntaxError("syntaxerror-rts-too-many-arguments", "too many arguments");
      runtimeSilence = !runtimeSilence;
      return PAQuery{IRParseMessage{"runtimeSilence = " + std::to_string(runtimeSilence)}};
    }

    if (cmd == "test") {
      if (!unread.empty())
        throw ptacxx::SyntaxError("syntaxerror-test-too-many-arguments", "too many arguments");
      return PAQuery{TestIn{}};
    }

    if (cmd == "crash") {
      if (!unread.empty())
        throw ptacxx::SyntaxError("syntaxerror-crash-too-many-arguments", "too many arguments");
      return PAQuery{CrashTestIn{}};
    }

    if (cmd == "note" || cmd == "n") {
      // suppose note file is ordered by llvm name ascending
      llvm::StringMap<std::string> ifstreamBufHolder;
      if (ptacxx::options::NoteFolderPath.empty())
        throw ptacxx::ConfigError("configerror-note-folder-empty", "note folder is empty");
      auto [vidStr, unread2] = eatToken(unread);
      if (vidStr.empty())
        throw ptacxx::SyntaxError("syntaxerror-note-usage",
                                  "note <vid> [note|-d|-f|-c...|-p...|-a...|-r...]");
      const VId vid = parseVid(vidStr, irm);
      auto [optionStr, unread3] = eatToken(unread2);
      std::string noteText;
      std::string conStr;
      std::string prfStr;
      std::string premiseName;
      std::string ppStr;
      if (optionStr == "-c") {
        auto [parsedCon, unread4] = eatToken(unread3);
        const std::string dsc = stripPrefix(unread4);
        if (parsedCon.empty() || dsc.empty())
          throw ptacxx::SyntaxError("syntaxerror-note-conjecture-usage",
                                    "note <vid> -c <con> <dsc>");
        noteText = "- Conjecture: " + parsedCon + "\n  - " + dsc;
      } else if (optionStr == "-p") {
        auto [parsedCon, unread4] = eatToken(unread3);
        conStr = parsedCon;
        prfStr = stripPrefix(unread4);
        if (conStr.empty() || prfStr.empty())
          throw ptacxx::SyntaxError("syntaxerror-note-proof-usage",
                                    "note <vid> -p <con> <prf>");
      } else if (optionStr == "-a" || optionStr == "-r") {
        auto [parsedCon, unread4] = eatToken(unread3);
        conStr = parsedCon;
        auto [premiseVidStr, unread5] = eatToken(unread4);
        ppStr = stripPrefix(unread5);
        if (conStr.empty() || premiseVidStr.empty() || ppStr.empty())
          throw ptacxx::SyntaxError("syntaxerror-note-premise-usage",
                                    "note <vid> -a|-r <con> <vid> <pp>");
        llvm::Value *premise = requireValue(premiseVidStr, irm,
                                            "premise vid not found");
        premiseName = premise->getName().str();
      } else {
        noteText = stripPrefix(unread2);
      }

      auto [V, ST] = requireValueOrStruct(vid, irm);

      std::string heading;
      if (ST) {
        heading = ST->getName().str();
        if (heading.size() && heading[0] == '%') heading = heading.substr(1);
      } else {
        if (!llvm::isa<llvm::GlobalValue>(V))
          throw ptacxx::NoteRelated("noterelated-note-local-value",
                                    "local values are not allowed for notes");
        heading = V->getName().str();
        if (heading.size() && heading[0] == '@') heading = heading.substr(1);
      }

      enum NoteMode {
        MODE_FINDFILE,
        MODE_FIND,
        MODE_MODIFY
      };
      NoteMode mode;
      if (optionStr == "-p" || optionStr == "-a" || optionStr == "-r") mode = MODE_MODIFY;
      else if (noteText == "-f") mode = MODE_FINDFILE;
      else if (noteText.empty()) mode = MODE_FIND;
      else mode = MODE_MODIFY;

      std::error_code ec;
      std::filesystem::create_directories(ptacxx::options::NoteFolderPath, ec);
      if (ec)
        throw ptacxx::FileSystemError("filesystemerror-note-folder-create-failed",
                                      "cannot create note folder: " + ec.message());

      DebugFileInfo info;
      if (ST)
        info = irm.getDebugInfoFile(ST);
      else
        info = irm.getDebugInfoFile(V);
      if (!info.valid())
        throw ptacxx::NoteRelated("noterelated-note-no-debug-info", "no debug info");

      const bool appendMode =
          optionStr != "-p" && optionStr != "-a" && optionStr != "-r" && noteText != "-d";
      const bool createIfNotFound = mode == MODE_MODIFY && appendMode;
      const bool copyWhileFinding = mode == MODE_MODIFY;

      std::string path;
      {
      bool fileFound = false;
      path = getNoteFile(info.path, fileFound, createIfNotFound);
      std::ifstream in;
      if (fileFound) {
        in.open(path);
        if (!in)
          throw ptacxx::FileSystemError("filesystemerror-note-file-open-failed",
                                        "cannot open " + path);
      } else throw ptacxx::NoteRelated("noterelated-note-not-found", "note not found");
      ifstreamBufHolder[path] = readAll(in);
      in.close();
      }
      std::string outBuffer;
      llvm::raw_string_ostream out(outBuffer);
      size_t headingPos = 0;
      bool found;

      const std::string &buffer = ifstreamBufHolder[path];
      found = findLowerboundWithLinePrefix(buffer, headingPos, "## ", heading);
      if (copyWhileFinding) out.write(buffer.data(), headingPos);
      if (!createIfNotFound && !found)
        throw ptacxx::NoteRelated("noterelated-note-not-found-on-modify", "note not found");

      switch (mode) {
      case MODE_FINDFILE:
        return PAQuery{IRParseMessage{path + ":" + std::to_string(getLineno(buffer, headingPos))}};
      case MODE_FIND:
        return PAQuery{IRParseMessage{std::string(readRecord(buffer, headingPos, "## "))}};
      case MODE_MODIFY: {
        std::string_view oldStringView;
        if (!found) {
          oldStringView = "";
          out << "## " << heading << "\n";
          out << noteText << "\n";
        } else {
          oldStringView = readRecord(buffer, headingPos, "## ");
          if (noteText == "-d") {
            // newString stays empty, deleting the current record.
          } else if (appendMode) {
            out << oldStringView;
            if (!oldStringView.ends_with('\n')) out << "\n";
            out << noteText << "\n";
          } else {
            const size_t headingEnd = oldStringView.find('\n');
            out.write(oldStringView.data(),
                         headingEnd == std::string_view::npos ? oldStringView.size() : headingEnd + 1);

            auto splitLines = [](std::string_view s) {
              std::vector<std::string> lines;
              for (size_t start = 0; start < s.size();) {
                const size_t end = s.find('\n', start);
                if (end == std::string_view::npos) {
                  lines.emplace_back(s.substr(start));
                  break;
                }
                lines.emplace_back(s.substr(start, end - start));
                start = end + 1;
              }
              return lines;
            };
            const std::string_view body =
                headingEnd == std::string_view::npos ? std::string_view{} : oldStringView.substr(headingEnd + 1);
            std::vector<std::string> noteLines = splitLines(body);

            auto isIndented = [](const std::string &s) {
              return !s.empty() && (s[0] == ' ' || s[0] == '\t');
            };

            size_t conIdx = std::string::npos;
            for (size_t i = 0; i < noteLines.size(); ++i) {
              if (noteLines[i] == "- Conjecture: " + conStr) {
                conIdx = i;
                break;
              }
              if ((optionStr == "-a" || optionStr == "-r") &&
                  noteLines[i] == "- Theorem: " + conStr)
                throw ptacxx::NoteRelated("noterelated-note-theorem-premise",
                                          "theorem cannot add/remove premise");
            }
            if (conIdx == std::string::npos)
              throw ptacxx::NoteRelated("noterelated-note-conjecture-not-found",
                                        "conjecture not found");

            std::vector<std::string> modified;
            if (optionStr == "-p") {
              bool proofInserted = false;
              for (size_t i = 0; i < noteLines.size(); ++i) {
                if (i == conIdx) {
                  modified.push_back("- Theorem: " + conStr);
                  continue;
                }
                if (i > conIdx && !proofInserted && !isIndented(noteLines[i])) {
                  modified.push_back("  - Proof: " + prfStr);
                  proofInserted = true;
                }
                modified.push_back(noteLines[i]);
              }
              if (!proofInserted) modified.push_back("  - Proof: " + prfStr);
            } else if (optionStr == "-a") {
              size_t whenIdx = std::string::npos;
              for (size_t i = conIdx + 1; i < noteLines.size(); ++i) {
                if (noteLines[i] == "  - When: " + premiseName) {
                  whenIdx = i;
                  break;
                }
              }

              size_t insertionIdx;
              std::vector<std::string> inserted;
              if (whenIdx != std::string::npos) {
                insertionIdx = whenIdx + 1;
                while (insertionIdx < noteLines.size() && isIndented(noteLines[insertionIdx]))
                  ++insertionIdx;
                inserted.push_back("    - " + ppStr);
              } else {
                insertionIdx = conIdx + 1;
                while (insertionIdx < noteLines.size() && isIndented(noteLines[insertionIdx]))
                  ++insertionIdx;
                inserted.push_back("  - When: " + premiseName);
                inserted.push_back("    - " + ppStr);
              }

              for (size_t i = 0; i < noteLines.size(); ++i) {
                if (i == insertionIdx)
                  for (const auto &ins : inserted) modified.push_back(ins);
                modified.push_back(noteLines[i]);
              }
              if (insertionIdx == noteLines.size())
                for (const auto &ins : inserted) modified.push_back(ins);
            } else if (optionStr == "-r") {
              size_t whenIdx = std::string::npos;
              for (size_t i = conIdx + 1; i < noteLines.size(); ++i) {
                if (noteLines[i] == "  - When: " + premiseName) {
                  whenIdx = i;
                  break;
                }
              }
              if (whenIdx == std::string::npos)
                throw ptacxx::NoteRelated("noterelated-note-premise-when-not-found",
                                          "premise when not found");

              size_t removedIdx = std::string::npos;
              size_t childEnd = whenIdx + 1;
              while (childEnd < noteLines.size() && isIndented(noteLines[childEnd]))
                ++childEnd;
              for (size_t i = whenIdx + 1; i < childEnd; ++i) {
                if (noteLines[i] == "    - " + ppStr) {
                  removedIdx = i;
                  break;
                }
              }
              if (removedIdx == std::string::npos)
                throw ptacxx::NoteRelated("noterelated-note-premise-not-found",
                                          "premise not found");

              bool hasOtherChild = false;
              for (size_t i = whenIdx + 1; i < childEnd; ++i) {
                if (i != removedIdx && isIndented(noteLines[i])) {
                  hasOtherChild = true;
                  break;
                }
              }
              for (size_t i = 0; i < noteLines.size(); ++i) {
                if (i == removedIdx) continue;
                if (i == whenIdx && !hasOtherChild) continue;
                modified.push_back(noteLines[i]);
              }
            }

            for (const auto &noteLine : modified) out << noteLine << "\n";
          }
        }
        out.write(std::string_view(buffer).substr(headingPos + oldStringView.size()).data(),
                  buffer.size() - headingPos - oldStringView.size());
        out.flush();
        std::ofstream file(path);
        if (!file)
          throw ptacxx::FileSystemError("filesystemerror-note-file-write-failed-open",
                                        "cannot write " + path);
        file << outBuffer;
        file.close();
        if (!file)
          throw ptacxx::FileSystemError("filesystemerror-note-file-write-failed-close",
                                        "cannot write " + path);
        return PAQuery{IRParseMessage{"ok"}};
      }
      }
    }

    if (cmd == "tv") {
      auto [vidStr, unread2] = eatToken(unread);
      auto [ppStr, unread3] = eatToken(unread2);
      if (vidStr.empty() || ppStr.empty() || !stripPrefix(unread3).empty())
        throw ptacxx::SyntaxError("syntaxerror-tv-usage", "tv <vid> <pp>");
      if (ptacxx::options::NoteFolderPath.empty())
        throw ptacxx::ConfigError("configerror-tv-note-folder-empty", "note folder is empty");

      const VId vid = parseVid(vidStr, irm);
      auto [V, ST] = requireValueOrStruct(vid, irm);

      std::string heading;
      if (ST) {
        heading = ST->getName().str();
        if (!heading.empty() && heading[0] == '%') heading = heading.substr(1);
      } else {
        if (!llvm::isa<llvm::GlobalValue>(V))
          throw ptacxx::SemanticError("semanticerror-tv-local-value",
                                      "local values are not allowed for tv");
        heading = V->getName().str();
        if (!heading.empty() && heading[0] == '@') heading = heading.substr(1);
      }

      GlobalEntry rootEntry = irm.getGlobal(heading);
      V = irm.vidToValue(rootEntry.id);
      ST = V ? nullptr : irm.vidToIdStruct(rootEntry.id);

      DebugFileInfo rootInfo;
      if (ST)
        rootInfo = irm.getDebugInfoFile(ST);
      else
        rootInfo = irm.getDebugInfoFile(V);
      if (!rootInfo.valid())
        throw ptacxx::NoteRelated("noterelated-tv-no-debug-info", "no debug info");

      bool rootFound = false;
      const std::string rootNoteFile = getNoteFile(rootInfo.path, rootFound);
      if (!rootFound)
        throw ptacxx::NoteRelated("noterelated-tv-note-not-found", "note not found");

      std::ifstream rootIn(rootNoteFile);
      const std::string rootBuf = readAll(rootIn);
      size_t rootHeadingPos = 0;
      if (!findLowerboundWithLinePrefix(rootBuf, rootHeadingPos, "## ", heading))
        throw ptacxx::NoteRelated("noterelated-tv-root-note-not-found", "note not found");
      const std::string rootRecord =
          std::string(readRecord(rootBuf, rootHeadingPos, "## "));
      if (!isTheorem(rootRecord, ppStr).has_value())
        throw ptacxx::NoteRelated("noterelated-tv-proposition-not-found",
                                  "proposition not found");

      llvm::SmallVector<NodeKey, 8> ances;
      llvm::DenseMap<NodeKey, ProofStatus, NodeKeyInfo> visited;

      auto resolve = [&](auto &&self, const NodeKey &key) -> ProofStatus {
        bool inAnces = false;
        for (const NodeKey &ance : ances)
          if (ance == key) {
            inAnces = true;
            break;
          }
        if (inAnces)
          throw ptacxx::SemanticError(
              "semanticerror-tv-recursive-dependency",
              key.func + " " + key.pp + " cannot be proven: recursive dependency");
        if (auto it = visited.find(key); it != visited.end())
          return it->second;

        ances.push_back(key);
        ProofStatus status = ProofStatus::Unproven;

        GlobalEntry entry = irm.getGlobal(key.func);
        llvm::Value *value = irm.vidToValue(entry.id);
        llvm::StructType *structType =
            value ? nullptr : irm.vidToIdStruct(entry.id);
        DebugFileInfo info;
        if (structType)
          info = irm.getDebugInfoFile(structType);
        else if (value)
          info = irm.getDebugInfoFile(value);
        bool found = info.valid();
        const std::string noteFile = found ? getNoteFile(info.path, found) : std::string{};
        if (!found) {
          visited[key] = status;
          ances.pop_back();
          return status;
        }

        std::ifstream in(noteFile);
        const std::string buf = readAll(in);
        size_t headingPos = 0;
        if (!findLowerboundWithLinePrefix(buf, headingPos, "## ", key.func)) {
          visited[key] = status;
          ances.pop_back();
          return status;
        }
        const std::string record = std::string(readRecord(buf, headingPos, "## "));
        const std::optional<bool> theorem = isTheorem(record, key.pp);
        if (!theorem) {
          visited[key] = status;
          ances.pop_back();
          return status;
        }

        if (*theorem) {
          status = ProofStatus::Proven;
          for (const NodeKey &child : getWhens(record, key.pp)) {
            if (self(self, child) != ProofStatus::Proven)
              status = ProofStatus::ConditionalProven;
          }
        } else {
          for (const NodeKey &child : getWhens(record, key.pp))
            self(self, child);
        }

        visited[key] = status;
        ances.pop_back();
        return status;
      };

      const NodeKey rootKey{heading, ppStr};
      resolve(resolve, rootKey);

      auto statusName = [](ProofStatus s) -> std::string_view {
        switch (s) {
        case ProofStatus::Proven:
          return "proven";
        case ProofStatus::ConditionalProven:
          return "conditional-proven";
        case ProofStatus::Unproven:
          return "unproven";
        }
        return "unproven";
      };

      std::string buf;
      llvm::raw_string_ostream os(buf);
      auto printNode = [&](auto &&self, const NodeKey &key,
                           unsigned depth) -> void {
        auto statusIt = visited.find(key);
        std::string status = "unproven";
        if (statusIt != visited.end())
          status = std::string(statusName(statusIt->second));
        os << std::string(depth * 2, ' ') << "- " << key.func << " " << key.pp
           << " " << status << "\n";

        bool inAnces = false;
        for (const NodeKey &ance : ances)
          if (ance == key) {
            inAnces = true;
            break;
          }
        if (inAnces) return;

        ances.push_back(key);
        GlobalEntry entry = irm.getGlobal(key.func);
        llvm::Value *value = irm.vidToValue(entry.id);
        llvm::StructType *structType =
            value ? nullptr : irm.vidToIdStruct(entry.id);
        DebugFileInfo info;
        if (structType)
          info = irm.getDebugInfoFile(structType);
        else if (value)
          info = irm.getDebugInfoFile(value);
        bool found = info.valid();
        const std::string noteFile = found ? getNoteFile(info.path, found) : std::string{};
        if (!found) {
          ances.pop_back();
          return;
        }
        std::ifstream in(noteFile);
        const std::string recordBuf = readAll(in);
        size_t headingPos = 0;
        if (!findLowerboundWithLinePrefix(recordBuf, headingPos, "## ", key.func)) {
          ances.pop_back();
          return;
        }
        const std::string record =
            std::string(readRecord(recordBuf, headingPos, "## "));
        for (const NodeKey &child : getWhens(record, key.pp))
          self(self, child, depth + 1);
        ances.pop_back();
      };

      printNode(printNode, rootKey, 0);
      os.flush();
      return PAQuery{IRParseMessage{buf}};
    }
    throw ptacxx::SyntaxError("syntaxerror-unknown-command", "unknown command " + cmd);
  }
}

std::string responseToString(const PAResponse &response, IRManager &irm) {
  return std::visit([&](const auto &arg) -> std::string {
    using T = std::decay_t<decltype(arg)>;
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
        irm.printValue(os, v, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID) << "\n";
      }
      os.flush();
      return buf;
    }

    if constexpr (std::is_same_v<T, PtsTestOut>) {
      const unsigned rateHundredths =
          arg.total ? static_cast<unsigned>(
                          10000.0 * static_cast<double>(arg.fails) /
                              static_cast<double>(arg.total) + 0.5)
                    : 0;
      std::string buf;
      llvm::raw_string_ostream os(buf);
      os << "Fail=" << arg.fails << "/" << arg.total << " ("
         << (rateHundredths / 100) << "."
         << (rateHundredths % 100 < 10 ? "0" : "") << (rateHundredths % 100)
         << "%)\n";
      for (const auto &[key, target] : arg.firstFails)
        os << key << " " << target << "\n";
      os.flush();
      return buf;
    }

    if constexpr (std::is_same_v<T, AliasSetOut>) {
      std::string buf;
      llvm::raw_string_ostream os(buf);
      for (llvm::Value *v : *arg.ptrs) {
        irm.printValue(os, v, detailed ? IRManager::PRT_DETAILED : IRManager::PRT_VID) << "\n";
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
          if (edge.callsite)
            irm.printValue(os, edge.callsite, IRManager::PRT_VID);
          else
            os << "-";
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

    if constexpr (std::is_same_v<T, TestOut>)
      return arg.result;

    return "(unknown query type)";
  }, response);
}

bool shouldSilence(const std::string& mangledName) {
  return shouldSilence(mangledName, stdSilence, llvmSilence, runtimeSilence);
}
