/**
 * Adapted from <SVF>/svf/lib/MemoryModel/PointerAnalysisImpl.cpp
 */

#include "drivers/common/QueryInterface.h"
#include "drivers/common/Transport.h"
#include "drivers/common/Postprocess.h"
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

class SVFWPAQueryServer : public QueryServer<SVFWPAQueryServer> {
  friend class QueryServer<SVFWPAQueryServer>;
private:
  std::unique_ptr<SVFIRBuilder> _builder;
  std::unique_ptr<SVFIR> _pag;
  std::unique_ptr<WPAPass> _wpa;
  std::unordered_map<const Value *, NodeID> _valueToNode;
  std::unordered_map<NodeID, const Value *> _nodeToValue;
  std::unordered_map<const Function *,
  std::vector<const Function *>> _callGraph;
  std::unique_ptr<IRManager> _irm;
private:
  void _init_impl(int argc, char **argv) {
    auto moduleNameVec = OptionBase::parseOptions(argc, argv,
        "Whole Program Points-to Analysis", "[options] <input-bitcode>");
    if (moduleNameVec.size() != 1) {
      errs() << "Error: exactly one input bitcode file is required.\n";
      exit(1);
    }
    std::string InputFilename = moduleNameVec[0];
    _irm = std::make_unique<IRManager>();
    _irm->addMainModule(InputFilename);
    auto &M = _irm->getModule();
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
  bool getPointsToSet(const Value *ptr, std::vector<const Value *> &pts) {
    auto it = _valueToNode.find(ptr);
    if (it == _valueToNode.end()) throw std::runtime_error("fatal");
    NodeID nodeId = it->second;
    const PointsTo& ptids = _wpa->getPts(nodeId);
    for (auto objId: ptids) {
      auto it2 = _nodeToValue.find(objId);
      if (it2 == _nodeToValue.end()) continue; // blackhole and null
      pts.push_back(it2->second);
    }
    return true;
  }
  std::string _handle_query_impl(const std::string &req){    
    PAQuery query = parse(req, *_irm);
    PAResponse response = std::visit([&](const auto &arg) -> PAResponse {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, IRMQuery>)
        return arg;
      if constexpr (std::is_same_v<T, IRParseError>)
        return ErrorOut{arg.message};
      if constexpr (std::is_same_v<T, PtsIn>) {
        std::vector<const llvm::Value *> pts;
        auto succ = getPointsToSet(arg.ptr, pts);
        if (!succ) return PtsOut{{}};
        return PtsOut{pts};
      }
      if constexpr (std::is_same_v<T, PtIn>) {
        std::vector<const llvm::Value *> pts;
        auto succ = getPointsToSet(arg.ptr, pts);
        if (!succ) return PtOut{ResultNo};
        return PtOut{mayPointTo(pts, arg.obj) ? ResultMay: ResultNo};
      }
      if constexpr (std::is_same_v<T, AliasIn>) {
        auto it_a = _valueToNode.find(arg.a);
        auto it_b = _valueToNode.find(arg.b);
        if (it_a == _valueToNode.end() || it_b == _valueToNode.end())
          throw std::runtime_error("fatal");
        const PointsTo& raw1 = _wpa->getPts(it_a->second);
        const PointsTo& raw2 = _wpa->getPts(it_b->second);
        PointsTo exp1 = expandFIObjs(raw1);
        PointsTo exp2 = expandFIObjs(raw2);
        return AliasOut{ (exp1.test(_pag->getBlackHoleNode()) || exp2.test(_pag->getBlackHoleNode())
            || exp1.intersects(exp2)) ? 
          llvm::AliasResult::MayAlias : llvm::AliasResult::NoAlias
        };
      }
      return ErrorOut{"unknown query type or not available"};
    }, query);
    return responseToString(response, *_irm);
  }
};

int main(int argc, char **argv) {
  return SVFWPAQueryServer().run(argc, argv);
}
