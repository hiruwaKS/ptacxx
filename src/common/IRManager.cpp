#include "IRManager.h"
#include "hooklibs.h"
#include "LLVMUtils.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/ValueSymbolTable.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Demangle/Demangle.h>

void IRManager::addModuleBase(const std::string &irPath, const int16_t moduleIdx){
  llvm::SMDiagnostic diag;
  auto M = llvm::parseIRFile(irPath, diag, *_ctx);
  if (!M) {
    std::string buf;
    llvm::raw_string_ostream os(buf);
    diag.print("IRManager", os);
    os.flush();
    throw std::runtime_error(
        "IRManager: failed to load '" + irPath + "': " + buf);
  }
  auto &data = _modules[moduleIdx];

  traverseModule(data, std::move(M), moduleIdx);

  if (!data._irStat.hasMain&&!moduleIdx)
    throw std::runtime_error("IRManager: main module need main function");
  if (data._irStat.hasMain&&moduleIdx)
    throw std::runtime_error("IRManager: lib module can't have main function");
}

int16_t IRManager::addMainModule(const std::string &irPath) {
  if (irPath.empty())
    throw std::runtime_error("IRManager: irPath is empty");
  addModuleBase(irPath, 0);
  return 0;
}

int16_t IRManager::addLibModule(const int16_t moduleIdx) {
  if (!moduleIdx || moduleIdx >= static_cast<int16_t>(HOOKLIBS_SIZE)) 
    throw std::runtime_error("IRManager: invalid moduleIdx");
  
  auto irPath = _libBasePath + HOOKLIBS.at(static_cast<size_t>(moduleIdx)) + ".bc";
  addModuleBase(irPath, moduleIdx);
  return moduleIdx;
}

int16_t IRManager::libNameToModuleIdx(const std::string &libName) {
  for (size_t i = 1; i < HOOKLIBS_SIZE; ++i)
    if (libName == HOOKLIBS.at(i))
      return static_cast<int16_t>(i);
  throw std::runtime_error("IRManager: unknown lib '" + libName +
    "'. Fix: add a driver, or register in hooklibs.h (only for libs used by the tested main module)");
}

void IRManager::traverseModule(ModuleData &data, std::unique_ptr<llvm::Module> pM, const int16_t moduleIdx) {
  data._module = std::move(pM);
  auto &M = *data._module;
  data._targetTriple = llvm::Triple(M.getTargetTriple());
  data._TLII = std::make_unique<llvm::TargetLibraryInfoImpl>(data._targetTriple);
  data._TLI = std::make_unique<llvm::TargetLibraryInfo>(*data._TLII);
  data._irStat.hasMain = M.getFunction("main") != nullptr;
  data._irStat.hasGlobalCtor = M.getNamedGlobal("llvm.global_ctors") != nullptr;
  data._irStat.hasGlobalDtor = M.getNamedGlobal("llvm.global_dtors") != nullptr;
  int16_t globalIdx = 0;
  int16_t funcCnt = 0;

  for (auto &GV : M.globals()) {
    if (GV.isDeclaration()) continue;
    if (llvmSkip(&GV)) continue;
    
    VId vid{moduleIdx, globalIdx, 0};
    _vidToValueCache[vid] = &GV;
    _valueToVidCache[&GV] = vid;
    auto mangledName = GV.getName().str();
    _globalStringToIdxCache[mangledName] = VId{moduleIdx, globalIdx, 0};
    {
      auto demangled = llvm::demangle(mangledName);
      if (demangled != mangledName)
        _manglingCache[demangled.substr(0, demangled.find('('))].push_back(std::move(mangledName));
    }
    if (GV.getValueType()->isPointerTy())
      ++data._irStat.globalPtrCnt;
    ++globalIdx;
  }
  data._irStat.globalCnt = globalIdx;

  for (auto &F : M) {
    if (F.isDeclaration()) continue;
    if (llvmSkip(&F)) continue;
    ++funcCnt;

    VId funcVid{moduleIdx, globalIdx, 0};
    _vidToValueCache[funcVid] = &F;
    _valueToVidCache[&F] = funcVid;
    auto mangledName = F.getName().str();
    _globalStringToIdxCache[mangledName] = VId{moduleIdx, globalIdx, 0};
    {
      auto demangled = llvm::demangle(mangledName);
      if (demangled != mangledName)
        _manglingCache[demangled.substr(0, demangled.find('('))].push_back(std::move(mangledName));
    }
 
    int16_t localIdx = 1;

    for (auto &Arg : F.args()) {
      VId vid{moduleIdx, globalIdx, localIdx};
      _vidToValueCache[vid] = &Arg;
      _valueToVidCache[&Arg] = vid;
      if (Arg.getType()->isPointerTy())
        ++data._irStat.argPtrCnt;
      ++localIdx;
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (I.getType()->isVoidTy())
          continue;
        VId vid{moduleIdx, globalIdx, localIdx};
        _vidToValueCache[vid] = &I;
        _valueToVidCache[&I] = vid;
        if (I.getType()->isPointerTy())
          ++data._irStat.instPtrCnt;
        ++localIdx;
      }
    }
    ++globalIdx;
  }
  data._irStat.funcCnt = funcCnt;
  data._metadata.metadata = getLLVMIRMetadataString(M);
}

std::string IRManager::getLLVMIRMetadataString(const llvm::Module &M) const {
  auto extractNamed = [&](const std::string &name) -> std::string {
    auto *nmd = M.getNamedMetadata(name);
    if (!nmd)
      return {};
    std::string result;
    for (unsigned i = 0; i < nmd->getNumOperands(); ++i) {
      auto *tuple = llvm::dyn_cast<llvm::MDTuple>(nmd->getOperand(i));
      if (!tuple)
        continue;
      for (unsigned j = 0; j < tuple->getNumOperands(); ++j) {
        if (auto *mdStr = llvm::dyn_cast<llvm::MDString>(tuple->getOperand(j))) {
          if (!result.empty())
            result += "; ";
          result += mdStr->getString().str();
        }
      }
    }
    return result;
  };

  std::string meta;
  auto append = [&](const std::string &key, const std::string &val) {
    meta += key;
    meta += ": ";
    meta += val;
    meta += '\n';
  };

  append("moduleID", M.getModuleIdentifier());
  append("sourceFileName", M.getSourceFileName());
  #if LLVM_VERSION_MAJOR >= 15
  append("targetTriple", M.getTargetTriple().getTriple());
  #else
  append("targetTriple", M.getTargetTriple());
  #endif
  append("dataLayout", M.getDataLayout().getStringRepresentation());

  std::string s;
  s = extractNamed("llvm.ident");
  if (!s.empty()) append("llvmIdent", s);
  s = extractNamed("clang.version");
  if (!s.empty()) append("clangVersion", s);
  s = extractNamed("optlevel");
  if (!s.empty()) append("optlevel", s);
  s = extractNamed("commandline");
  if (!s.empty()) append("commandline", s);
  s = extractNamed("llvm.module.deps");
  if (!s.empty()) append("llvmModuleDeps", s);

  return meta;
}

const llvm::Function * IRManager::parentFunction(const llvm::Value *V) {
  if (auto *Arg = llvm::dyn_cast<llvm::Argument>(V))
    return Arg->getParent();
  if (auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
    auto *ParentBB = I->getParent();
    return ParentBB ? ParentBB->getParent() : nullptr;
  }
  return nullptr;
}

std::vector<VId> IRManager::globalOrFunctionToVIds(const std::string &name) const {
  std::vector<VId> results;
  auto it = _globalStringToIdxCache.find(name);
  if (it != _globalStringToIdxCache.end())
    results.push_back(it->second);
  auto mit = _manglingCache.find(name);
  if (mit != _manglingCache.end()) {
    for (const auto &mangledName : mit->second) {
      if (mangledName == name) continue;
      auto vit = _globalStringToIdxCache.find(mangledName);
      if (vit != _globalStringToIdxCache.end())
        results.push_back(vit->second);
    }
  }
  if (results.empty()) throw std::runtime_error("name not found");
  return results;
}

IRDebugInfo IRManager::getValueDebugInfo(const llvm::Value *V) const {
  if (!V) throw std::runtime_error("fatal");

  std::string buf;
  llvm::raw_string_ostream os(buf);

  auto pr = [&](const llvm::Value *Val, const char *kind) {
    auto vid = valueToVId(Val);
    os << "vid: " << vid.moduleIdx << ":" << vid.globalIdx << ":" << vid.localIdx << "\n";
    os << kind << " ";
    if (auto *Arg = llvm::dyn_cast<llvm::Argument>(Val)) {
      Arg->getParent()->printAsOperand(os, false);
      os << ".";
      Arg->printAsOperand(os, false);
      os << ": " << *Val->getType() << "\n";
    }
    else if (auto *I = llvm::dyn_cast<llvm::Instruction>(Val)) {
      I->getParent()->getParent()->printAsOperand(os, false); 
      os << ".";
      if (Val->getType()->isVoidTy()) {
        auto *BB = I->getParent();
        BB->printAsOperand(os, false);
        unsigned idx = 0;
        for (auto &Inst : *BB) {
          if (&Inst == I) break;
          idx++;
        }
        os << ".BBIdx" << idx << ": void" << "\n";
      } else {
        Val->printAsOperand(os, false);
        os << ": " << *Val->getType() << "\n";
      }
    }
    else {
      Val->printAsOperand(os, false);
      os << ": " << *Val->getType() << "\n";
    }
#if LLVM_VERSION_MAJOR == 14
    // check opaque pointer
    if (auto *PT = llvm::dyn_cast<llvm::PointerType>(Val->getType())) {
      if (PT->isOpaque()) os << " (opaque)";
    }
#endif
  };
  if (auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
    pr(I, "Instruction");
    if (I->getOpcodeName()) os << I->getOpcodeName() << "\n";
    if (auto DL = I->getDebugLoc()) {
      unsigned Line = DL.getLine();
      unsigned Col = DL.getCol();
      if (Line != 0 && Col != 0) os << "LINE " << Line << ":" << Col << "\n";
      if (auto *Scope = DL.getScope()) {
        llvm::StringRef Filename = "";
        llvm::StringRef Directory = "";
        llvm::StringRef FuncName = "";
        if (auto *DIL = dyn_cast<llvm::DILocation>(Scope)) {
          if (auto *File = DIL->getFile()) {
            Filename = File->getFilename();
            Directory = File->getDirectory();
          }
          else if (auto *SP = DIL->getScope()->getSubprogram())
            FuncName = SP->getName();
        } else if (auto *DIS = dyn_cast<llvm::DIScope>(Scope)) {
          if (auto *File = DIS->getFile()) {
            Filename = File->getFilename();
            Directory = File->getDirectory();
          }
          else if (auto *SP = DIL->getScope()->getSubprogram())
            FuncName = SP->getName();
        }
        if (!Filename.empty()&&!Directory.empty()) os << Directory << "/" << Filename << "\n";
        if (!FuncName.empty()) os << FuncName << "\n";
      }
    }
    if (!I->use_empty()) {
      if (I->getNumUses() > 10) { os << "[Used in too many places]\n"; }
      else for (auto &U : I->uses()) {
        os << "[Used at operand " << U.getOperandNo() << "]\n";
        if (auto *User = dyn_cast<llvm::Instruction>(U.getUser())) {
          pr(User, "Instruction");
        }
      }
    }
  }
  else if (auto *F = llvm::dyn_cast<llvm::Function>(V)) {
    pr(F, "Function");
    if (auto *SP = F->getSubprogram()) {
      os << "Defined: " << (SP->isDefinition() ? "true" : "false") << "\n";
      if (SP->getLine() && SP->getScopeLine()) os << "LINE " << SP->getLine() << ":" << SP->getScopeLine() << "\n";
      if (auto *File = SP->getFile()) {
        llvm::StringRef Filename = File->getFilename(); 
        llvm::StringRef Directory = File->getDirectory();
        if (!Filename.empty()&&!Directory.empty()) os << Directory << "/" << Filename << "\n";
      }
    }
  }
  else if (auto *Arg = llvm::dyn_cast<llvm::Argument>(V)) {
    pr(Arg, "Argument");
  }
  else if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
    pr(GV, "GlobalVarible");
    llvm::SmallVector<llvm::DIGlobalVariableExpression *, 4> GVs;
    GV->getDebugInfo(GVs);
    for (auto *GVExpr : GVs) {
      if (auto *DIGV = GVExpr->getVariable()) {
        if (DIGV->getLine()) os << "LINE " << DIGV->getLine() << "\n";
        if (auto *File = DIGV->getFile()) {
        llvm::StringRef Filename = File->getFilename(); 
        llvm::StringRef Directory = File->getDirectory();
          if (!Filename.empty()&&!Directory.empty()) os << Directory << "/" << Filename << "\n";
        }
        if (!DIGV->getLinkageName().empty()) os << DIGV->getLinkageName() << "\n";
      }
    }
  }
  return IRDebugInfo{buf};
}
