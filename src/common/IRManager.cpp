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
#include <llvm/ADT/DenseSet.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Config/llvm-config.h>

#include <algorithm>
#include <functional>
#include <fstream>

/// print lines from a file, [start, end]
llvm::raw_ostream &printLinesFromFile(llvm::raw_ostream &os, const std::string &path, unsigned start, unsigned end);

static std::string debugFilePath(const llvm::DIFile *File) {
  assert(File);
  return (File->getDirectory() + "/" + File->getFilename()).str();
}

void IRManager::addMainModule(const std::string &irPath) {
  if (irPath.empty())
    throw std::runtime_error("IRManager: irPath is empty");
  llvm::SMDiagnostic diag;
  {
  ScopeTimer _("Load");
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
  }

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
  int idStructTypeCnt = 0;

  auto recordStructTypes = [&](llvm::Type *Ty) {
    assert(Ty);
    llvm::DenseSet<llvm::Type *> Visited;
    std::vector<llvm::Type *> Worklist{Ty};
    while (!Worklist.empty()) {
      llvm::Type *CurTy = Worklist.back(); assert(CurTy);
      Worklist.pop_back();
      if (!Visited.insert(CurTy).second) continue;
      if (auto *ST = llvm::dyn_cast<llvm::StructType>(CurTy)) {
        if (ST->hasName() && !ST->getName().empty()) {
          auto Inserted = _idStructToVidCache.try_emplace(ST, vidCnt);
          if (Inserted.second) {
            _vidToIdStructCache[vidCnt] = ST;
            auto name = ST->getName().str();
            _globalStringToIdxCache.push_back( {name, vidCnt, true} );
            std::string striped;
            if (name.starts_with("struct.")) striped = name.substr(7);
            else if (name.starts_with("class.")) striped = name.substr(6);
            else if (name.starts_with("union.")) striped = name.substr(6);
            else if (name.starts_with("enum.")) striped = name.substr(5);
            while (true) {
              if (striped != name) 
                _globalStringToIdxCache.push_back( {striped, vidCnt, false} );
              auto [ns, remain] = getNamespacePair(striped);
              if (ns == "") break;
              striped = remain;
            }
            ++vidCnt;
            ++idStructTypeCnt;
          }
        }
      }
      for (llvm::Type *SubTy : CurTy->subtypes()) Worklist.push_back(SubTy);
    }
  };

  for (auto &GV : M.globals()) {
    recordStructTypes(GV.getValueType());
    if (GV.hasInitializer())
      recordStructTypes(GV.getInitializer()->getType());

    _vidToValueCache[vidCnt] = &GV;
    _valueToVidCache[&GV] = vidCnt;
    auto mangledName = GV.getName().str();
    _globalStringToIdxCache.push_back( {mangledName, vidCnt, true} );
    {
      auto demangled = getDemangledName(mangledName);
      while (true) {
        if (demangled != mangledName) 
          _globalStringToIdxCache.push_back( {demangled, vidCnt, false} );
        auto [ns, remain] = getNamespacePair(demangled);
        if (ns == "") break;
        demangled = remain;
      }
    }
    ++vidCnt;
    if (GV.getValueType()->isPointerTy())
      ++globalPtrCnt;
    ++globalCnt;
  }
  _irStat.globalPtrCnt = globalPtrCnt;
  _irStat.globalCnt = globalCnt;

  for (auto &F : M) {
    ++funcCnt;
    // recordStructTypes(F.getFunctionType());

    _vidToValueCache[vidCnt] = &F;
    _valueToVidCache[&F] = vidCnt;
    auto mangledName = F.getName().str();
    _globalStringToIdxCache.push_back( {mangledName, vidCnt, true} );
    {
      auto demangled = getDemangledName(mangledName);
      while (true) {
        if (demangled != mangledName) 
          _globalStringToIdxCache.push_back( {demangled, vidCnt, false} );
        auto [ns, remain] = getNamespacePair(demangled);
        if (ns == "") break;
        demangled = remain;
      }
    }
    ++vidCnt;

    for (auto &Arg : F.args()) {
      // recordStructTypes(Arg.getType());

      _vidToValueCache[vidCnt] = &Arg;
      _valueToVidCache[&Arg] = vidCnt;
      ++vidCnt;
      if (Arg.getType()->isPointerTy())
        ++argPtrCnt;
      ++localIdx;
    }
    for (auto &BB : F) {
      _vidToValueCache[vidCnt] = &BB;
      _valueToVidCache[&BB] = vidCnt;
      ++vidCnt;
      for (auto &I : BB) {
        // note: number void instrument, to see where pointers are used (store)
        // if (I.getType()->isVoidTy())
        //   continue;
        // recordStructTypes(I.getType());
        if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&I))
          recordStructTypes(AI->getAllocatedType());
        if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I))
          recordStructTypes(GEP->getSourceElementType());
        for (const llvm::Use &U : I.operands())
          if (U.get())
            recordStructTypes(U.get()->getType());

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
  _irStat.idStructTypeCnt = idStructTypeCnt;
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

DebugFileInfo IRManager::getDebugInfoFile(llvm::Value *V) const {
  DebugFileInfo info;
  if (!V) return info;
  if (auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
    if (auto DL = I->getDebugLoc()) {
      if (auto *Scope = DL.getScope()) {
        if (auto *DIL = llvm::dyn_cast<llvm::DILocation>(Scope))
          info.path = debugFilePath(DIL->getFile());
        else if (auto *DIS = llvm::dyn_cast<llvm::DIScope>(Scope))
          info.path = debugFilePath(DIS->getFile());
      }
      info.lineStart = info.lineEnd = DL.getLine();
      info.columnStart = info.columnEnd = DL.getCol();
    }
  } else if (auto *F = llvm::dyn_cast<llvm::Function>(V)) {
    if (auto *SP = F->getSubprogram()) {
      info.path = debugFilePath(SP->getFile());
      info.lineStart = info.lineEnd = SP->getLine();
      for (auto &BB : *F) {
        for (auto &Inst : BB) {
          if (auto DL = Inst.getDebugLoc()) {
            unsigned instLine = DL.getLine();
            if (instLine > info.lineEnd) info.lineEnd = instLine;
          }
        }
      }
    }
  } else if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
    llvm::SmallVector<llvm::DIGlobalVariableExpression *, 4> GVs;
    GV->getDebugInfo(GVs);
    for (auto *GVExpr : GVs) {
      if (!GVExpr) continue;
      auto *DIGV = GVExpr->getVariable();
      if (!DIGV) continue;
      info.path = debugFilePath(DIGV->getFile());
      info.lineStart = info.lineEnd = DIGV->getLine();
      if (!info.path.empty()) break;
    }
  }
  return info;
}

DebugFileInfo IRManager::getDebugInfoFile(llvm::StructType *ST) const {
  DebugFileInfo info;
  if (!ST || !ST->hasName()) return info;

  std::string StructTyName = ST->getName().str();
  std::string CleanName = StructTyName;
  for (llvm::StringRef prefix : {"struct.", "class.", "union.", "enum."}) {
    if (CleanName.find(prefix) == 0) {
      CleanName = CleanName.substr(prefix.size());
      break;
    }
  }
  size_t dotPos = CleanName.find_last_of('.');
  if (dotPos != std::string::npos) CleanName = CleanName.substr(0, dotPos);
  std::string ClassNameOnly = getAllNamespaceStripped(CleanName);
  if (ClassNameOnly.empty()) return info;

  for (const llvm::Function &F : _module->functions()) {
    auto *SP = F.getSubprogram();
    if (!SP) continue;
    auto *CT = llvm::dyn_cast<llvm::DICompositeType>(SP->getScope());
    if (!CT || CT->getName().str() != ClassNameOnly) continue;

    info.path = debugFilePath(CT->getFile());
    info.lineStart = info.lineEnd = CT->getLine();
    for (auto *Element : CT->getElements()) {
      if (auto *DIE = llvm::dyn_cast<llvm::DIDerivedType>(Element))
        if (DIE->getLine() > info.lineEnd)
          info.lineEnd = DIE->getLine();
    }
    if (!info.path.empty()) break;
  }
  return info;
}

llvm::ArrayRef<GlobalEntry> IRManager::listGlobal(const std::string &prefix) const {
  auto begin = std::lower_bound(_globalStringToIdxCache.begin(), _globalStringToIdxCache.end(), 
    GlobalEntry{prefix, 0});
  auto end = std::lower_bound(_globalStringToIdxCache.begin(), _globalStringToIdxCache.end(), 
    GlobalEntry{prefix + char(255), 0});
  return llvm::ArrayRef<GlobalEntry>(&*begin, static_cast<size_t>(std::distance(begin, end)));
}

GlobalEntry IRManager::getGlobal(const std::string &name) const {
  std::string realName = name;
  auto begin = std::lower_bound(_globalStringToIdxCache.begin(), _globalStringToIdxCache.end(),
    GlobalEntry{name, 0});
  auto end = std::lower_bound(_globalStringToIdxCache.begin(), _globalStringToIdxCache.end(),
    GlobalEntry{name + char(255), 0});
  GlobalEntry match{};
  bool matched = false;
  for (auto it = begin; it != end; ++it) {
    const GlobalEntry &entry = *it;
    if (!entry.isRealName || entry.name != name)
      continue;
    if (llvm::Value *value = vidToValue(entry.id)) {
      if (matched) {
        throw std::runtime_error("getValueFromString: ambiguous: " + name);
      } else {
        match = entry;
        matched = true;
      }
    }
  }
  if (!matched)
    throw std::runtime_error("getValueFromString: not found: " + name);
  return match;
}

llvm::raw_ostream &printLinesFromFile(llvm::raw_ostream &os, const std::string &path, 
    unsigned start, unsigned end){
  std::ifstream file(path);
  std::string line;
  unsigned lineNum = 1;
  while (lineNum < start && std::getline(file, line)) ++lineNum;
  while (lineNum <= end && std::getline(file, line)) {
    os << line << "\n";
    ++lineNum;
  }
  return os;
}

llvm::raw_ostream &IRManager::printValue(llvm::raw_ostream &os, llvm::Value *V, PrintLevel pl) const {
  if (!V) throw std::runtime_error("fatal");
  auto vid = valueToVId(V);
  os << vid;
  if (pl == PrintLevel::PRT_VID) return os;
  if (true) {
    if (pl == PrintLevel::PRT_DEBUG) {
      os << " ";
      printDetailedValueId(os, V);
    }
    os << " ";
    if (auto *Arg = llvm::dyn_cast<llvm::Argument>(V)) {
      auto F = Arg->getParent();
      os << getDemangledName(F->getName().str());
      if (pl == PrintLevel::PRT_DEBUG) {
        os << " ";
        F->printAsOperand(os, false);
        os << ":";
        Arg->printAsOperand(os, false);
      }
    }
    else if (auto *B = llvm::dyn_cast<llvm::BasicBlock>(V)) {
      auto F = B->getParent();
      os << "block of " << getDemangledName(F->getName().str());
      if (pl == PrintLevel::PRT_DEBUG) {
        os << " ";
        F->printAsOperand(os, false);
        os << ":";
        B->printAsOperand(os, false);
      }
    }
    else if (auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
      auto F = I->getParent()->getParent();
      os << "inst of " << getDemangledName(F->getName().str());
      if (pl == PrintLevel::PRT_DEBUG) {
        os << " ";
        F->printAsOperand(os, false);
        os << ":";
        if (I->getType()->isVoidTy()) {
          auto *BB = I->getParent();
          // BB->printAsOperand(os, false);
          unsigned idx = 0;
          for (auto &Inst : *BB) {
            if (&Inst == I) break;
            idx++;
          }
          (void)idx;
          os << ":BBIdx" << idx << ":";
        }
        I->print(os, false); 
      }
    }
    else if (auto *F = llvm::dyn_cast<llvm::Function>(V)) {
      os << getDemangledName(F->getName().str());
      if (pl == PrintLevel::PRT_DEBUG) {
        os << " ";
        F->printAsOperand(os, false);
      }
    }
    else if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
      os << getDemangledName(GV->getName().str());
      if (pl == PrintLevel::PRT_DEBUG) {
        os << " ";
        GV->printAsOperand(os, false);
      }
    }
    else V->printAsOperand(os, false);
  }
  if (pl == PrintLevel::PRT_DEBUG) {
    os << " " << *V->getType();
#if LLVM_VERSION_MAJOR == 14
  // check opaque pointer
  if (auto *PT = llvm::dyn_cast<llvm::PointerType>(V->getType())) {
    if (PT->isOpaque()) os << " (opaque)";
  }
#endif
  }
  if (pl == PrintLevel::PRT_DEBUG) {
    os << "\n";
    if (!V->use_empty()) {
      if (V->getNumUses() > 10) { os << "[Used in too many places]\n"; }
      else for (auto &U : V->uses()) {
        os << "  [Use] at operand " << U.getOperandNo() << "\n";
        if (auto *User = dyn_cast<llvm::Instruction>(U.getUser()))
          printValue(os << "  ", User, PrintLevel::PRT_DETAILED) << "\n";
      }
    }
    DebugFileInfo info = getDebugInfoFile(V);
    if (auto *F = llvm::dyn_cast<llvm::Function>(V)) {
      if (auto *SP = F->getSubprogram())
        os << "Defined: " << (SP->isDefinition() ? "true" : "false") << "\n";
    }
    if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
      llvm::SmallVector<llvm::DIGlobalVariableExpression *, 4> GVs;
      GV->getDebugInfo(GVs);
      for (auto *GVExpr : GVs) {
        if (auto *DIGV = GVExpr->getVariable())
          if (!DIGV->getLinkageName().empty()) os << DIGV->getLinkageName() << "\n";
      }
    }
    if (info.valid()) {
      os << "<src LINE " << info.lineStart << ":" << info.lineEnd;
      if (info.columnEnd >= info.columnStart && info.columnEnd != 0)
        os << " COL" << info.columnStart << ":" << info.columnEnd;
      os << " in " << info.path << ">\n";
      printLinesFromFile(os, info.path, info.lineStart, info.lineEnd);
      os << "</src>\n";
    }
  }
  return os;
}
llvm::raw_ostream &IRManager::printIdStructType(llvm::raw_ostream &os, llvm::StructType *ST, PrintLevel pl) const {
  if (!ST) throw std::runtime_error("pass nullptr to printIdStructType");
  os << idStructToVId(ST);
  if (pl == PrintLevel::PRT_VID) return os;
  os << " " << (ST->hasName() ? ST->getName() : "<unnamed id struct>");
  if (pl != PrintLevel::PRT_DEBUG) return os;
  os << "\n";
  ST->print(os);
  os << "\n";

  DebugFileInfo info = getDebugInfoFile(ST);
  if (info.valid()) {
    os << "<src LINE " << info.lineStart << ":" << info.lineEnd
       << " in " << info.path << ">\n";
    printLinesFromFile(os, info.path, info.lineStart, info.lineEnd);
    os << "</src>\n";
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
     << "idStructTypeCnt: " << _irStat.idStructTypeCnt << "\n"
     << "localCnt: " << _irStat.localCnt << "\n"
     << "globalPtrCnt: " << _irStat.globalPtrCnt << "\n"
     << "argPtrCnt: " << _irStat.argPtrCnt << "\n"
     << "instPtrCnt: " << _irStat.instPtrCnt << "\n"
     << "hasMain: " << _irStat.hasMain << "\n"
     << "hasGlobalCtor: " << _irStat.hasGlobalCtor << "\n"
     << "hasGlobalDtor: " << _irStat.hasGlobalDtor;
  return os;
}
