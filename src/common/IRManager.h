#pragma once

#include "VId.h"

#include "llvm/Analysis/TargetLibraryInfo.h"
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#if LLVM_VERSION_MAJOR < 16
#include <llvm/ADT/Triple.h>
#else
#include <llvm/TargetParser/Triple.h>
#endif
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <stdexcept>
#include <vector>

/**
 * Manage multiple modules.
 * Add bitcode sources -> get metadatas -> conversion to vid
 */

/// Contains: Module::ModuleID, Module::SourceFileName, Module::TargetTriple
///   Module::DataLayout
/// !llvm.ident, !clang.version, !optlevel, !commandline, !llvm.module.deps
struct Metadata    { std::string metadata; };

struct IRStat      { int16_t funcCnt; int16_t globalCnt; int32_t globalPtrCnt; int32_t argPtrCnt; int32_t instPtrCnt; bool hasMain; bool hasGlobalCtor; bool hasGlobalDtor; };

struct DebugInfo   { std::string debugInfo; };

struct ModuleData {
  std::unique_ptr<llvm::Module> _module;
  llvm::Triple _targetTriple;
  std::unique_ptr<llvm::TargetLibraryInfoImpl> _TLII;
  std::unique_ptr<llvm::TargetLibraryInfo> _TLI;
  Metadata _metadata;
  IRStat _irStat;
  int16_t _moduleIdx;
};

class IRManager {
private:
  std::string _libBasePath;
  std::string _mainModulePath;
  std::vector<int16_t> _libModuleIdxs;
  std::unique_ptr<llvm::LLVMContext> _ctx;

  std::unordered_map<int16_t, ModuleData> _modules;
	std::unordered_map<std::string, VId> _globalStringToIdxCache;
	std::unordered_map<std::string, std::vector<std::string>> _manglingCache;
	std::unordered_map<VId, const llvm::Value *> _vidToValueCache;
  std::unordered_map<const llvm::Value *, VId> _valueToVidCache;

public:
  explicit IRManager(const std::string &libBasePath):
    _libBasePath(libBasePath), 
    _ctx(std::make_unique<llvm::LLVMContext>()) {}

  int16_t addMainModule(const std::string &irPath);

  static int16_t libNameToModuleIdx(const std::string &libName);

  /// See `VId.h` to know more about moduleIdx
  int16_t addLibModule(const int16_t moduleIdx);
  int16_t addLibModule(const std::string &libName) {
    return addLibModule(libNameToModuleIdx(libName));
  }

  const llvm::LLVMContext &getLLVMContext() const { return *_ctx; }
  
  /// @throw std::runtime_error if not found
  const ModuleData &getModuleDataByIdx(int16_t moduleIdx) const {
    auto it = _modules.find(moduleIdx);
    if (it == _modules.end())
      throw std::runtime_error("module not found");
    return it->second;
  }
  // you can use this to modify the module, like instrumenting
  llvm::Module &getModule(int16_t moduleIdx) { return *getModuleDataByIdx(moduleIdx)._module;  }
  const llvm::Module &getModule(int16_t moduleIdx) const 
    { return *getModuleDataByIdx(moduleIdx)._module; }
  const Metadata& getMetadata(int16_t moduleIdx) const 
    { return getModuleDataByIdx(moduleIdx)._metadata; }
  const IRStat& getIRStat(int16_t moduleIdx) const
    { return getModuleDataByIdx(moduleIdx)._irStat; }

  /// @note cached, safe for instrumenting
  VId valueToVId(const llvm::Value *V) const {
    if (!V) throw std::runtime_error("pass nullptr to valueToVId");
    auto it = _valueToVidCache.find(V);
    return it != _valueToVidCache.end() ? it->second : 
      throw std::runtime_error("fatal");
  }

  const llvm::Value *vidToValue(VId id) const {
    auto it = _vidToValueCache.find(id);
    return it != _vidToValueCache.end() ? it->second : nullptr;
  }

  /// @return VIds for the given global or function name (multiple if overloaded).
  /// @throws std::runtime_error if not found ("name not found").
  std::vector<VId> globalOrFunctionToVIds(const std::string &name) const;

  /// @return the id, type of value, the name and the debug info (if not clipped)
  /// @warning this may be costly, don't call it in every query
  DebugInfo getValueDebugInfo(const llvm::Value *V) const;

  /// both .ll and .bc are supported
  void dumpModule(int16_t moduleIdx, const std::string &outPath) const {
    const auto &M = getModule(moduleIdx);
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
  void addModuleBase(const std::string &irPath, const int16_t moduleIdx);
  void traverseModule(ModuleData &data, std::unique_ptr<llvm::Module> pM, const int16_t moduleIdx);
  static const llvm::Function *parentFunction(const llvm::Value *V);
  std::string getLLVMIRMetadataString(const llvm::Module &M) const;
};
