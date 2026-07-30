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
struct IRMetadata    { std::string metadata; };

struct IRStat      { int16_t funcCnt; int16_t globalCnt; int32_t globalPtrCnt; int32_t argPtrCnt; int32_t instPtrCnt; bool hasMain; bool hasGlobalCtor; bool hasGlobalDtor; };

struct IRDebugInfo   { std::string debugInfo; };

class IRManager {
private:
  std::string _mainModulePath;
  std::unique_ptr<llvm::LLVMContext> _ctx;

  std::unique_ptr<llvm::Module> _module;
  llvm::Triple _targetTriple;
  std::unique_ptr<llvm::TargetLibraryInfoImpl> _TLII;
  std::unique_ptr<llvm::TargetLibraryInfo> _TLI;
  IRMetadata _metadata;
  IRStat _irStat;

	std::unordered_map<std::string, VId> _globalStringToIdxCache;
	std::unordered_map<std::string, std::vector<std::string>> _manglingCache;
	std::unordered_map<VId, const llvm::Value *> _vidToValueCache;
  std::unordered_map<const llvm::Value *, VId> _valueToVidCache;

public:
  explicit IRManager(): _ctx(std::make_unique<llvm::LLVMContext>()) {}

  int16_t addMainModule(const std::string &irPath);

  const llvm::LLVMContext &getLLVMContext() const { return *_ctx; }
  
  // you can use this to modify the module, like instrumenting
  llvm::Module &getModule() { return *_module;  }
  const llvm::Module &getModule() const { return *_module; }
  const IRMetadata& getMetadata() const { return _metadata; }
  const IRStat& getIRStat() const { return _irStat; }

  /// @note cached, safe for instrumenting
  VId valueToVId(const llvm::Value *V) const {
    if (!V) throw std::runtime_error("pass nullptr to valueToVId");
    auto it = _valueToVidCache.find(V);
    return it != _valueToVidCache.end() ? it->second : VID_NOT_REGISTERED;
  }

  const llvm::Value *vidToValue(VId id) const {
    auto it = _vidToValueCache.find(id);
    return it != _vidToValueCache.end() ? it->second : throw std::runtime_error("vid not found");
  }

  /// @return VIds for the given global or function name (multiple if overloaded).
  /// @throws std::runtime_error if not found ("name not found").
  std::vector<VId> globalOrFunctionToVIds(const std::string &name) const;

  /// @return the id, type of value, the name and the debug info (if not clipped)
  /// @warning this may be costly, don't call it in every query
  IRDebugInfo getValueDebugInfo(const llvm::Value *V) const;

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
  std::string getLLVMIRMetadataString(const llvm::Module &M) const;
};
