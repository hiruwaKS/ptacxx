/*
 * See the origin (`<lotus>/tools/alias/lotus-alias-fpa.cpp`)
       for documentation.
 */

#include "common/PAWrapper.h"
#include "common/QueryInterface.h"
#include "common/IRManager.h"
#include "common/Common.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/CLIUtils.h"
#include "Alias/Specialized/FPA/Config.h"
#include "Alias/Specialized/FPA/FLTAPass.h"
#include "Alias/Specialized/FPA/KELPPass.h"
#include "Alias/Specialized/FPA/MLTADFPass.h"
#include "Alias/Specialized/FPA/MLTAPass.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/InitLLVM.h>

#include <iostream>
#include <memory>
#include <string>

using namespace llvm;
using namespace std;
using namespace lotus::alias::tools;

LLVM_CL_IGNORE_WARNINGS_BEGIN

// TODO: multiple modules
static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::init("-"),
                                          cl::value_desc("filename"), cl::Required);

static cl::opt<int>
    AnalysisType("analysis-type",
                 cl::desc("select which analysis to use: 1 --> FLTA, 2 --> "
                          "MLTA, 3 --> Data Flow Enhanced MLTA, 4 --> Kelp"),
                 cl::NotHidden, cl::init(1));

static cl::opt<int> MaxTypeLayer(
    "max-type-layer",
    cl::desc("Multi-layer type analysis for refining indirect-call targets"),
    cl::NotHidden, cl::init(10));

static cl::opt<bool> DebugMode("fpa-debug", cl::desc("debug mode"), cl::init(false));

static cl::opt<string>
    OutputFilePath("output-file",
                   cl::desc("Output file path, better to use absolute path"),
                   cl::init(""));

static GlobalContext GlobalCtx;

LLVM_CL_IGNORE_WARNINGS_END

static void PrintResults(GlobalContext *GCtx) {
  int TotalTargets = 0;
  for (auto *IC : GCtx->IndirectCallInsts)
    TotalTargets += GCtx->Callees[IC].size();

  OP << "\n@@ Total number of final callees: " << TotalTargets << ".\n";

  OP << "############## Result Statistics ##############\n";
  OP << "# Number of virtual calls: \t\t\t" << GCtx->NumVirtualCall << "\n";
  OP << "# Number of indirect calls: \t\t\t" << GCtx->IndirectCallInsts.size()
     << "\n";
  OP << "# Number of indirect calls with targets: \t"
     << GCtx->NumValidIndirectCalls << "\n";
  OP << "# Number of indirect-call targets: \t\t"
     << GCtx->NumIndirectCallTargets << "\n";
  OP << "# Number of address-taken functions: \t\t"
     << GCtx->AddressTakenFuncs.size() << "\n";
  OP << "# Number of multi-layer calls: \t\t\t" << GCtx->NumSecondLayerTypeCalls
     << "\n";
  OP << "# Number of multi-layer targets: \t\t" << GCtx->NumSecondLayerTargets
     << "\n";
  OP << "# Number of one-layer calls: \t\t\t" << GCtx->NumFirstLayerTypeCalls
     << "\n";
  OP << "# Number of one-layer targets: \t\t\t" << GCtx->NumFirstLayerTargets
     << "\n";
  OP << "# Number of simple indirect calls: \t\t\t" << GCtx->NumSimpleIndCalls
     << "\n";
  OP << "# Number of confined functions: \t\t\t" << GCtx->NumConfinedFuncs
     << "\n";

  if (OutputFilePath.size() == 0)
    return;
  ostream &output =
      (OutputFilePath == "cout") ? cout : *(new std::ofstream(OutputFilePath));

  for (auto &curEle : GCtx->Callees) {
    if (curEle.first->isIndirectCall()) {
      // totalsize += curEle.second.size();
      FuncSet funcs = curEle.second;

      auto *Scope = cast<DIScope>(curEle.first->getDebugLoc().getScope());
      string callsiteFile = Scope->getFilename().str();
      unsigned int line = curEle.first->getDebugLoc().getLine();
      unsigned int col = curEle.first->getDebugLoc().getCol();
      string content =
          callsiteFile + ":" + itostr(line) + ":" + itostr(col) + "|";
      for (llvm::Function *func : funcs)
        content += (func->getName().str() + ",");
      content = content.substr(0, content.size() - 1);
      content += "\n";
      output << content;
    }
  }

  if (OutputFilePath.size() != 0) {
    static_cast<std::ofstream &>(output).close();
    delete &output;
  }
}

class FPAQueryServer : public IncluPAWrapper {
private:
  std::unique_ptr<CallGraphPass> _fpa;
public:
  FPAQueryServer(IRManager &irm) : IncluPAWrapper(irm) {}
  ~FPAQueryServer() override;
private:
  void init() override {
    Module &M = _irm.getModule();
    
    StringRef MName = StringRef(strdup(InputFilename.data()));
    GlobalCtx.Modules.push_back(std::make_pair(&M, MName)); // TODO: potential double free
    GlobalCtx.ModuleMaps[&M] = InputFilename;
  
    debug_mode = DebugMode;
    max_type_layer = MaxTypeLayer;

    if (AnalysisType == 1)
      _fpa = std::make_unique<FLTAPass>(&GlobalCtx);
    else if (AnalysisType == 2)
      _fpa = std::make_unique<MLTAPass>(&GlobalCtx);
    else if (AnalysisType == 3)
      _fpa = std::make_unique<MLTADFPass>(&GlobalCtx);
    else if (AnalysisType == 4)
      _fpa = std::make_unique<KELPPass>(&GlobalCtx);
    else {
      cout << "unimplemnted analysis type, break\n";
      exit(1);
    }
    _fpa->run(GlobalCtx.Modules);

    PrintResults(&GlobalCtx);
  }
  bool getPointsToSet(Ptr value, PointsToSet &pts) override {
    llvm::SmallPtrSet<Function *, 8> FuncSet;
    if (auto *CI = const_cast<CallInst *>(dyn_cast<CallInst>(value))) {
      _fpa->analyzeIndCall(CI, &FuncSet);
      for (auto *F : FuncSet) pts.push_back(F);
      return true;
    }
    throw std::runtime_error("fpa can only resolve callsite");
  }
};

FPAQueryServer::~FPAQueryServer() = default;

int main(int argc, char **argv) {
InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "global analysis\n");
  
  IRManager irm;
  irm.addMainModule(InputFilename);
  return FPAQueryServer(irm).run();
}
