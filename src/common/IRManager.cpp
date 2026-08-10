#include "IRManager.h"
#include "MemoryBuiltins.h"
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

void IRManager::addMainModule(const std::string &irPath) {
  if (irPath.empty())
    throw std::runtime_error("IRManager: irPath is empty");
  llvm::SMDiagnostic diag;
  auto M = llvm::parseIRFile(irPath, diag, getThreadLocalContext());
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
  int globalCnt = 0, globalPtrCnt = 0;
  int funcCnt = 0, argPtrCnt = 0, instPtrCnt = 0;
  int localIdx = 0;
  VId vidCnt = 0;

  for (auto &GV : M.globals()) {
    if (GV.isDeclaration()) continue;
    if (llvmSkip(&GV)) continue;
    
    _vidToValueCache[vidCnt] = &GV;
    _valueToVidCache[&GV] = vidCnt;
    auto mangledName = GV.getName().str();
    _globalStringToIdxCache.push_back( {mangledName, vidCnt} );
    {
      auto demangled = llvm::demangle(mangledName);
      if (demangled != mangledName) 
        _globalStringToIdxCache.push_back( {demangled, vidCnt} );
    }
    ++vidCnt;
    if (GV.getValueType()->isPointerTy())
      ++globalPtrCnt;
    ++globalCnt;
  }
  _irStat.globalPtrCnt = globalPtrCnt;
  _irStat.globalCnt = globalCnt;

  for (auto &F : M) {
    if (F.isDeclaration()) continue;
    if (llvmSkip(&F)) continue;
    ++funcCnt;

    _vidToValueCache[vidCnt] = &F;
    _valueToVidCache[&F] = vidCnt;
    auto mangledName = F.getName().str();
    _globalStringToIdxCache.push_back( {mangledName, vidCnt} );
    {
      auto demangled = getDemangledName(mangledName);
      if (demangled != mangledName)
        _globalStringToIdxCache.push_back( {demangled, vidCnt} );
    }
    ++vidCnt;

    for (auto &Arg : F.args()) {
      _vidToValueCache[vidCnt] = &Arg;
      _valueToVidCache[&Arg] = vidCnt;
      ++vidCnt;
      if (Arg.getType()->isPointerTy())
        ++argPtrCnt;
      ++localIdx;
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        // note: number void instrument, to see where pointers are used (store)
        // if (I.getType()->isVoidTy())
        //   continue;
        _vidToValueCache[vidCnt] = &I;
        _valueToVidCache[&I] = vidCnt;
        ++vidCnt;
        if (I.getType()->isPointerTy())
          ++instPtrCnt;
        ++localIdx;
      }
    }
    ++globalCnt;
  }
  std::sort(_globalStringToIdxCache.begin(), _globalStringToIdxCache.end());
  _irStat.argPtrCnt = argPtrCnt;
  _irStat.instPtrCnt = instPtrCnt;
  _irStat.funcCnt = funcCnt;
  _irStat.localCnt = localIdx;

  // fill metadata
  auto extractNamed = [&](const std::string &name) -> std::string {
    auto *nmd = M.getNamedMetadata(name);
    if (!nmd)
      return {};
    std::string result;
    result.reserve(64);
    bool first = true;
    for (unsigned i = 0, e = nmd->getNumOperands(); i != e; ++i) {
      llvm::MDNode* node = nmd->getOperand(i);
      if (!node) continue;
      for (unsigned j = 0, f = node->getNumOperands(); j != f; ++j) {
        llvm::Metadata* meta = node->getOperand(j);
        if (auto* mdStr = llvm::dyn_cast<llvm::MDString>(meta)) {
          if (!first) result += "; ";
          result += mdStr->getString().str();
          first = false;
        }
      }
    }
    return result;
  };
  _irStat.moduleID = M.getModuleIdentifier();
  _irStat.sourceFileName = M.getSourceFileName();
#if LLVM_VERSION_MAJOR >= 15
  _irStat.targetTriple = M.getTargetTriple().getTriple();
#else
  _irStat.targetTriple = M.getTargetTriple();
#endif
  _irStat.dataLayout = M.getDataLayout().getStringRepresentation();
  _irStat.llvmIdent = extractNamed("llvm.ident");
  _irStat.clangVersion = extractNamed("clang.version");
  _irStat.optlevel = extractNamed("optlevel");
  _irStat.commandline = extractNamed("commandline");
  _irStat.llvmModuleDeps = extractNamed("llvm.module.deps");
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

llvm::ArrayRef<GlobalEntry> IRManager::listGlobal(const std::string &prefix) const {
  auto begin = std::lower_bound(_globalStringToIdxCache.begin(), _globalStringToIdxCache.end(), 
    GlobalEntry{prefix, 0});
  auto end = std::lower_bound(_globalStringToIdxCache.begin(), _globalStringToIdxCache.end(), 
    GlobalEntry{prefix + char(255), 0});
  return llvm::ArrayRef<GlobalEntry>(&*begin, static_cast<size_t>(std::distance(begin, end)));
}

llvm::raw_ostream &IRManager::printValue(llvm::raw_ostream &os, llvm::Value *V, 
    bool demangle, bool type, bool debugInfo) const {
  if (!V) throw std::runtime_error("fatal");
  auto vid = valueToVId(V);
  os << vid;
  os << " ";
  printDetailedValueId(os, V);
  os << " ";
  if (auto *Arg = llvm::dyn_cast<llvm::Argument>(V)) {
    Arg->getParent()->printAsOperand(os, false);
    os << ":";
    Arg->printAsOperand(os, false);
  }
  else if (auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
    I->getParent()->getParent()->printAsOperand(os, false); 
    os << ":";
    if (V->getType()->isVoidTy()) {
      auto *BB = I->getParent();
      BB->printAsOperand(os, false);
      unsigned idx = 0;
      for (auto &Inst : *BB) {
        if (&Inst == I) break;
        idx++;
      }
      os << ":BBIdx" << idx;
    } else V->printAsOperand(os, false);
  }
  else if (auto *F = llvm::dyn_cast<llvm::Function>(V)) {
    if (demangle) os << getDemangledName(F->getName().str()) << " ";
    F->printAsOperand(os, false);
  }
  else if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
    if (demangle) os << getDemangledName(GV->getName().str()) << " ";
    GV->printAsOperand(os, false);
  }
  else V->printAsOperand(os, false);
  if (type) {
    os << " " << *V->getType();
#if LLVM_VERSION_MAJOR == 14
  // check opaque pointer
  if (auto *PT = llvm::dyn_cast<llvm::PointerType>(V->getType())) {
    if (PT->isOpaque()) os << " (opaque)";
  }
#endif
  }
  if (debugInfo) {
    os << "\n";
    if (auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
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
          os << "  [Use] at operand " << U.getOperandNo() << "\n";
          if (auto *User = dyn_cast<llvm::Instruction>(U.getUser()))
            printValue(os << "  ", User) << "\n";
        }
      }
    }
    else if (auto *F = llvm::dyn_cast<llvm::Function>(V)) {
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
      // no debug info for arguments
    }
    else if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
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
  }
  return os;
}

llvm::raw_ostream &IRManager::printStat(llvm::raw_ostream &os) const {
  os << "moduleID: " << _irStat.moduleID << "\n"
     << "sourceFileName: " << _irStat.sourceFileName << "\n"
     << "targetTriple: " << _irStat.targetTriple << "\n"
     << "dataLayout: " << _irStat.dataLayout << "\n"
     << "llvmIdent: " << _irStat.llvmIdent << "\n"
     << "clangVersion: " << _irStat.clangVersion << "\n"
     << "optlevel: " << _irStat.optlevel << "\n"
     << "commandline: " << _irStat.commandline << "\n"
     << "llvmModuleDeps: " << _irStat.llvmModuleDeps << "\n"
     << "funcCnt: " << _irStat.funcCnt << "\n"
     << "globalCnt: " << _irStat.globalCnt << "\n"
     << "globalPtrCnt: " << _irStat.globalPtrCnt << "\n"
     << "argPtrCnt: " << _irStat.argPtrCnt << "\n"
     << "instPtrCnt: " << _irStat.instPtrCnt << "\n"
     << "hasMain: " << _irStat.hasMain << "\n"
     << "hasGlobalCtor: " << _irStat.hasGlobalCtor << "\n"
     << "hasGlobalDtor: " << _irStat.hasGlobalDtor;
  return os;
}
