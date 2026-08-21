#pragma once

#include "VId.h"
#include "Common.h"

#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include "llvm/IR/DerivedTypes.h"
#include <llvm/Bitcode/BitcodeWriter.h>
#if LLVM_VERSION_MAJOR < 16
#include <llvm/ADT/Triple.h>
#else
#include <llvm/TargetParser/Triple.h>
#endif
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

struct IRStat {
  std::string moduleID; std::string sourceFileName; std::string targetTriple;
  std::string dataLayout;
  std::string llvmIdent; std::string clangVersion; std::string optlevel; std::string commandline; std::string llvmModuleDeps;
  int funcCnt; int globalCnt; int localCnt;
  int idStructTypeCnt;
  int globalPtrCnt; int argPtrCnt; int instPtrCnt; 
  bool hasMain; bool hasGlobalCtor; bool hasGlobalDtor;
};

struct GlobalEntry {
  /// mangled or demangled name
  std::string name;
  VId id;
  /// true for the original IR name, e.g. F.getName()/GV.getName()/struct getName()
  bool isRealName = false;
  bool operator<(const GlobalEntry &rhs) const {
    if (name != rhs.name) return name < rhs.name;
    if (id != rhs.id) return id < rhs.id;
    return !isRealName && rhs.isRealName;
  }
};

class IRManager {
private:
  std::string _mainModulePath;

  std::unique_ptr<llvm::Module> _module;
  llvm::Triple _targetTriple;
  std::unique_ptr<llvm::TargetLibraryInfoImpl> _TLII;
  std::unique_ptr<llvm::TargetLibraryInfo> _TLI;
  IRStat _irStat;

  template <typename T>
  using SortedVector = std::vector<T>;
	SortedVector<GlobalEntry> _globalStringToIdxCache;
	llvm::DenseMap<VId, llvm::Value *> _vidToValueCache;
  llvm::DenseMap<llvm::Value *, VId> _valueToVidCache;
	/// idStruct is short for "identified struct"
	llvm::DenseMap<llvm::StructType *, VId> _idStructToVidCache;
	llvm::DenseMap<VId, llvm::StructType *> _vidToIdStructCache;

public:
  enum PrintLevel {
    PRT_VID = 0,
    PRT_DETAILED = 1,
    PRT_DEBUG = 2
  };
  explicit IRManager() {}

  void addMainModule(const std::string &irPath);
  
  // you can use this to modify the module, like instrumenting
  llvm::Module &getModule() { return *_module;  }
  const llvm::Module &getModule() const { return *_module; }
  const llvm::TargetLibraryInfo& getTLI() const { return *_TLI; }
  const IRStat& getIRStat() const { return _irStat; }

  /// @note cached, safe for instrumenting
  VId valueToVId(llvm::Value *V) const {
    if (!V) throw std::runtime_error("pass nullptr to valueToVId");
    auto it = _valueToVidCache.find(V);
    return it != _valueToVidCache.end() ? it->second : VID_NOT_REGISTERED;
  }

  /// @return nullptr if not found
  llvm::Value *vidToValue(VId id) const {
    auto it = _vidToValueCache.find(id);
    return it != _vidToValueCache.end() ? it->second : nullptr;
  }

  VId idStructToVId(llvm::StructType *ST) const {
    if (!ST) throw std::runtime_error("pass nullptr to idStructToVId");
    auto it = _idStructToVidCache.find(ST);
    return it != _idStructToVidCache.end() ? it->second : VID_NOT_REGISTERED;
  }

  /// @return nullptr if not found
  llvm::StructType *vidToIdStruct(VId id) const {
    auto it = _vidToIdStructCache.find(id);
    return it != _vidToIdStructCache.end() ? it->second : nullptr;
  }

  llvm::ArrayRef<GlobalEntry> listGlobal(const std::string &prefix) const;

  /// @throw if the name is not found or maps to multiple values
  GlobalEntry getGlobal(const std::string &name) const;

  /// @note if debugInfo is printed, the result will be multi-line
  /// @warning debugInfo may be costly, don't call it in every query
  llvm::raw_ostream &printValue(llvm::raw_ostream &os, llvm::Value *V, PrintLevel pl) const;

  llvm::raw_ostream &printIdStructType(llvm::raw_ostream &os, llvm::StructType *ST, PrintLevel pl) const;

  llvm::raw_ostream &printStat(llvm::raw_ostream &os) const;

  /// both .ll and .bc are supported
  void dumpModule(const std::string &outPath) const {
    const auto &M = *_module;
    llvm::SmallString<256> path(outPath);
    llvm::sys::path::remove_filename(path);
    if (!path.empty()) llvm::sys::fs::create_directories(path);
    std::error_code EC;
    llvm::raw_fd_ostream OS(outPath, EC, llvm::sys::fs::OF_Text);
    if (EC) {
      llvm::errs() << "Cannot open " << outPath << ": " << EC.message() << "\n";
      return;
    }
    if (llvm::sys::path::extension(outPath) == ".ll")
      M.print(OS, nullptr);
    else WriteBitcodeToFile(M, OS);
  }

private:
  void traverseModule(std::unique_ptr<llvm::Module> pM);
  static const llvm::Function *parentFunction(const llvm::Value *V);
};
