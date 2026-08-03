#include "IRManager.h"
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

int16_t IRManager::addMainModule(const std::string &irPath) {
  if (irPath.empty())
    throw std::runtime_error("IRManager: irPath is empty");
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

  traverseModule(std::move(M));

  if (!_irStat.hasMain)
    llvm::errs() << "IRManager: main module without main function\n";
  return 0;
}


void IRManager::traverseModule(std::unique_ptr<llvm::Module> pM) {
  _module = std::move(pM);
  auto &M = *_module;
  _targetTriple = llvm::Triple(M.getTargetTriple());
  _TLII = std::make_unique<llvm::TargetLibraryInfoImpl>(_targetTriple);
  _TLI = std::make_unique<llvm::TargetLibraryInfo>(*_TLII);
  _irStat.hasMain = M.getFunction("main") != nullptr;
  _irStat.hasGlobalCtor = M.getNamedGlobal("llvm.global_ctors") != nullptr;
  _irStat.hasGlobalDtor = M.getNamedGlobal("llvm.global_dtors") != nullptr;
  int16_t globalIdx = 0;
  int16_t funcCnt = 0;

  for (auto &GV : M.globals()) {
    if (GV.isDeclaration()) continue;
    if (llvmSkip(&GV)) continue;
    
    VId vid{globalIdx, 0};
    _vidToValueCache[vid] = &GV;
    _valueToVidCache[&GV] = vid;
    auto mangledName = GV.getName().str();
    _globalStringToIdxCache[mangledName] = {mangledName, globalIdx};
    {
      auto demangled = llvm::demangle(mangledName);
      if (demangled != mangledName)
        _manglingCache[demangled.substr(0, demangled.find('('))].push_back(std::move(mangledName));
    }
    if (GV.getValueType()->isPointerTy())
      ++_irStat.globalPtrCnt;
    ++globalIdx;
  }
  _irStat.globalCnt = globalIdx;

  for (auto &F : M) {
    if (F.isDeclaration()) continue;
    if (llvmSkip(&F)) continue;
    ++funcCnt;

    VId funcVid{globalIdx, 0};
    _vidToValueCache[funcVid] = &F;
    _valueToVidCache[&F] = funcVid;
    auto mangledName = F.getName().str();
    _globalStringToIdxCache[mangledName] = {mangledName, globalIdx};
    {
      auto demangled = llvm::demangle(mangledName);
      if (demangled != mangledName)
        _manglingCache[demangled.substr(0, demangled.find('('))].push_back(std::move(mangledName));
    }
 
    int16_t localIdx = 1;

    for (auto &Arg : F.args()) {
      VId vid{globalIdx, localIdx};
      _vidToValueCache[vid] = &Arg;
      _valueToVidCache[&Arg] = vid;
      if (Arg.getType()->isPointerTy())
        ++_irStat.argPtrCnt;
      ++localIdx;
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (I.getType()->isVoidTy())
          continue;
        VId vid{globalIdx, localIdx};
        _vidToValueCache[vid] = &I;
        _valueToVidCache[&I] = vid;
        if (I.getType()->isPointerTy())
          ++_irStat.instPtrCnt;
        ++localIdx;
      }
    }
    ++globalIdx;
  }
  _irStat.funcCnt = funcCnt;
  _metadata.metadata = getLLVMIRMetadataString(M);
}

int16_t IRManager::resolveLocalName(const std::string &name, const llvm::Function *F) const {
  std::string buffer;
  llvm::raw_string_ostream os(buffer);
  int16_t idx = 1;
  for (const auto &Arg : F->args()) {
    Arg.printAsOperand(os, false);
    os.flush();
    if (buffer == name) return idx;
    ++idx;
    buffer.clear();
  }
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (I.getType()->isVoidTy())
        continue;
      I.printAsOperand(os, false);
      os.flush();
      if (buffer == name) return idx;
      ++idx;
      buffer.clear();
    }
  }
  throw std::runtime_error("IRManager: resolveLocalName: '" + name + "' not found");
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

std::vector<std::pair<const std::string&, int16_t>>
  IRManager::dismangleGlobalOrFunction(const std::string &name) const {
  std::vector<std::pair<const std::string&, int16_t>> results;
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
    os << vid.globalIdx << ":" << vid.localIdx << " ";
    os << kind << " ";
    if (auto *Arg = llvm::dyn_cast<llvm::Argument>(Val)) {
      Arg->getParent()->printAsOperand(os, false);
      os << ":";
      Arg->printAsOperand(os, false);
      os << ": " << *Val->getType() << "\n";
    }
    else if (auto *I = llvm::dyn_cast<llvm::Instruction>(Val)) {
      I->getParent()->getParent()->printAsOperand(os, false); 
      os << ":";
      if (Val->getType()->isVoidTy()) {
        auto *BB = I->getParent();
        BB->printAsOperand(os, false);
        unsigned idx = 0;
        for (auto &Inst : *BB) {
          if (&Inst == I) break;
          idx++;
        }
        os << ":BBIdx" << idx << ": void" << "\n";
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
        os << "\t[Use] at operand " << U.getOperandNo() << "\n";
        if (auto *User = dyn_cast<llvm::Instruction>(U.getUser())) {
          os << "\t";
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
