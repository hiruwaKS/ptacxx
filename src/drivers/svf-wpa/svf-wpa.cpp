/**
 * Adapted from <SVF>/svf/lib/MemoryModel/PointerAnalysisImpl.cpp
 */

#include "common/PAWrapper.h"
#include "common/QueryInterface.h"
#include "common/IRManager.h"
#include "common/Common.h"

#include "SVFIR/SVFIR.h" // put in the front
#include "WPA/WPAPass.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVF-LLVM/LLVMModule.h"
#include "MemoryModel/PointerAnalysis.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/CommandLine.h>

#include <cstdlib>
#include <unordered_map>
#include <vector>

using namespace llvm;
using namespace SVF;

class SVFWPAQueryServer : public IncluPAWrapper {
private:
  std::unique_ptr<SVFIRBuilder> _builder;
  std::unique_ptr<SVFIR> _pag;
  std::unique_ptr<WPAPass> _wpa;
  std::unordered_map<const Value *, NodeID> _valueToNode;
  std::unordered_map<NodeID, const Value *> _nodeToValue;
public:
  SVFWPAQueryServer(IRManager &irm) : IncluPAWrapper(irm) {}
  ~SVFWPAQueryServer() override;
private:
  void init() override {
    auto &M = _irm.getModule();
    LLVMModuleSet::buildSVFModule(M);
    _builder = std::make_unique<SVFIRBuilder>();
    _pag.reset(_builder->build());
    _wpa = std::make_unique<WPAPass>();
    _wpa->runOnModule(_pag.get());
    // mapping ID
    auto *mset = LLVMModuleSet::getLLVMModuleSet();
    for (auto it = _pag->begin(); it != _pag->end(); ++it) {
      NodeID nid = it->first;
      const SVFVar *var = it->second;
      if (!var->hasLLVMValue()) // TODO: blackhole and null handling
        continue;
      const Value *V = mset->getLLVMValue(var);
      _valueToNode[V] = nid;
      _nodeToValue[nid] = V;
    }
  }
  PointsTo expandFIObjs(const PointsTo& pts) const {
    PointsTo expanded = pts;
    for (NodeID id : pts) {
      if (_pag->getBaseObjVarID(id) == id || _pag->getBaseObject(id)->isFieldInsensitive())
        expanded |= _pag->getAllFieldsObjVars(id);
    }
    return expanded;
  }
  bool getPointsToSet(Ptr value, PointsToSet &pts) override {
    auto it = _valueToNode.find(value);
    if (it == _valueToNode.end()) throw std::runtime_error("fatal");
    NodeID nodeId = it->second;
    const PointsTo& ptids = _wpa->getPts(nodeId);
    for (auto objId: ptids) {
      auto it2 = _nodeToValue.find(objId);
      if (it2 == _nodeToValue.end()) continue; // blackhole and null
      pts.push_back(const_cast<Value*>(it2->second));
    }
    return true;
  }

  PTAliasResult getAliasResult(Ptr a, Ptr b) override {
    auto it_a = _valueToNode.find(a);
    auto it_b = _valueToNode.find(b);
    if (it_a == _valueToNode.end() || it_b == _valueToNode.end())
      throw std::runtime_error("fatal");
    const PointsTo& raw1 = _wpa->getPts(it_a->second);
    const PointsTo& raw2 = _wpa->getPts(it_b->second);
    PointsTo exp1 = expandFIObjs(raw1);
    PointsTo exp2 = expandFIObjs(raw2);
    return (exp1.test(_pag->getBlackHoleNode()) || exp2.test(_pag->getBlackHoleNode())
        || exp1.intersects(exp2)) ? 
      llvm::AliasResult::MayAlias : llvm::AliasResult::NoAlias;
  }
};

SVFWPAQueryServer::~SVFWPAQueryServer() = default;

int main(int argc, char **argv) {
  ptacxx::options::CGPatchCLIntercept().go(argc, argv);
  auto moduleNameVec = OptionBase::parseOptions(argc, argv,
      "Whole Program Points-to Analysis", "[options] <input-bitcode>");
  if (moduleNameVec.size() != 1) {
    errs() << "Error: exactly one input bitcode file is required.\n";
    exit(1);
  }
  std::string InputFilename = moduleNameVec[0];
  auto irm = IRManager();
  irm.addMainModule(InputFilename);
  return SVFWPAQueryServer(irm).run();
}
