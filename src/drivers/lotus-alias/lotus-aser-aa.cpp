/*
 * Adapted from <lotus>/include/Alias/InclusionBased/AserPTA/PTADriver.h
 *   and <lotus>/tools/alias/lotus-alias-aser-aa.cpp
 */

#include "common/PAWrapper.h"
#include "common/QueryInterface.h"
#include "common/IRManager.h"
#include "common/Common.h"

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/PointerAnalysisPass.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/KCallSite.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/KOrigin.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldInsensitive/FIMemModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/PointerAnalysisPass.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/DeepPropagation.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PartialUpdateSolver.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PointsTo/BDDPts.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PointsTo/BitVectorPTS.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PointsTo/PointsToSelector.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/WavePropagation.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/CanonicalizeGEPPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/LoweringMemCpyPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/RemoveASMInstPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/RemoveExceptionHandlerPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/StandardHeapAPIRewritePass.h"
#include "Alias/Infrastructure/AliasAnalysisWrapper/CLIUtils.h"
#include "Alias/Infrastructure/Spec/AliasSpecManager.h"

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>

#include <iostream>
#include <variant>

using namespace aser;
using namespace llvm;
using namespace std;
using namespace lotus::alias;
using namespace lotus::alias::tools;

LLVM_CL_IGNORE_WARNINGS_BEGIN

// Command-line options
static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required);

static cl::opt<std::string> AnalysisMode(
    "analysis-mode",
    cl::desc("Analysis mode: ci (context-insensitive), 1-cfa, 2-cfa, origin"),
    cl::init("ci"), cl::value_desc("mode"));

static cl::opt<std::string>
    SolverType("solver", cl::desc("Solver type: basic, wave, deep"),
               cl::init("wave"), cl::value_desc("solver"));

static cl::opt<bool>
    FieldSensitive("field-sensitive",
                   cl::desc("Use field-sensitive memory model"),
                   cl::init(true));

static cl::opt<bool> DumpStats("dump-stats",
                               cl::desc("Print analysis statistics"),
                               cl::init(true));

LLVM_CL_IGNORE_WARNINGS_END

using Origin = KOrigin<1>;

template <typename ctx, typename pts>
using FSModel = DefaultLangModel<ctx, FSMemModel<ctx>, pts>;

template <typename ctx, typename pts>
using FIModel = DefaultLangModel<ctx, FIMemModel<ctx>, pts>;

// 2*2*3*4 = 48

class PTAPass {
public:
  virtual void getPointsTo(const llvm::Value *V,
    std::vector<const llvm::Value *> &result) const = 0;
  virtual bool alias(const llvm::Value *v1, const llvm::Value *v2) const = 0;
  virtual void getCtx() = 0;
  virtual ~PTAPass();
};

 PTAPass::~PTAPass() = default;

template <typename Solver>
class PTAPassImpl: public PTAPass {
private:
  using Ctx = typename Solver::ctx;
  using ObjTy = typename Solver::ObjTy;

  PointerAnalysisPass<Solver> *_ptaPass;
  const Ctx *_ctx;
public:
  PTAPassImpl(PointerAnalysisPass<Solver>* ptaPass): _ptaPass(ptaPass) {}
  void getPointsTo(const llvm::Value *V,
      std::vector<const llvm::Value *> &result) const override {
    std::vector<const ObjTy *> raw;
    _ptaPass->getPTA()->getPointsTo(_ctx, V, raw);
    for (auto *obj : raw) if (obj) result.push_back(obj->getValue());
  }
  bool alias(const llvm::Value *v1, const llvm::Value *v2) const override {
    return _ptaPass->getPTA()->alias(_ctx, v1, _ctx, v2);
  }
  /// TODO: context sensitivity
  void getCtx() override { _ctx = CtxTrait<Ctx>::getInitialCtx(); }
};

template <typename Solver>
std::pair<std::unique_ptr<llvm::ModulePass>, std::unique_ptr<PTAPass>> getPtaPass() {
  auto pass = std::make_unique<PointerAnalysisPass<Solver>>();
  auto ptaPass = std::make_unique<PTAPassImpl<Solver>>(pass.get());
  return std::make_pair(std::move(pass), std::move(ptaPass));
}

class AserPTAQueryServer : public IncluPAWrapper {
private:
  std::pair<std::unique_ptr<llvm::ModulePass>, std::unique_ptr<PTAPass>> _ptaPass;
  std::unique_ptr<llvm::legacy::PassManager> _passes;
public:
  AserPTAQueryServer(IRManager &irm) : IncluPAWrapper(irm) {}
  ~AserPTAQueryServer() override;
private:
  void init() override {
    auto &M = _irm.getModule();
  
    errs() << "Loaded module: " << InputFilename << "\n";
    errs() << "Analysis mode: " << AnalysisMode << "\n";
    errs() << "Solver type: " << SolverType << "\n";
    errs() << "Field-sensitive: " << (FieldSensitive ? "yes" : "no") << "\n";
  
    // Initialize AliasSpecManager with config files
    auto specFilePaths = collectConfigFilePaths();
    auto specManager = createAliasSpecManager(specFilePaths, &M);
  
    // Display loaded config files
    printLoadedConfigFiles(*specManager);

    // Setup origin rules for origin-sensitive analysis
    Origin::setOriginRules(
      [](const Origin *, const llvm::Instruction *I) -> bool {
        if (auto *CB = llvm::dyn_cast<CallBase>(I)) {
          if (auto *F = CB->getCalledFunction()) {
            StringRef name = F->getName();
            // Track thread creation and spawn operations as origins
            return name.equals("pthread_create") || name.contains("spawn") ||
                   name.contains("thread");
          }
        }
        return false;
      });

  // Determine solver

#define determineSolver(Ctx, Pts, FieldModel)                                  \
  if (SolverType == "basic") {                                                 \
    _ptaPass = getPtaPass<PartialUpdateSolver<FieldModel<Ctx, Pts>>>();        \
  } else if (SolverType == "wave") {                                           \
    _ptaPass = getPtaPass<WavePropagation<FieldModel<Ctx, Pts>>>();            \
  } else if (SolverType == "deep") {                                           \
    _ptaPass = getPtaPass<DeepPropagation<FieldModel<Ctx, Pts>>>();            \
  } else {                                                                     \
    errs() << "Unknown solver type: " << SolverType << "\n"; exit(1);          \
  }


#define determineCtx(Pts, FieldModel)                                          \
  {                                                                            \
    if (AnalysisMode == "ci") {                                                \
      determineSolver(NoCtx, Pts, FieldModel)                                  \
    } else if (AnalysisMode == "cs1") {                                        \
      determineSolver(KCallSite<1>, Pts, FieldModel)                           \
    } else if (AnalysisMode == "cs2") {                                        \
      determineSolver(KCallSite<2>, Pts, FieldModel)                           \
    } else if (AnalysisMode == "origin") {                                     \
      determineSolver(Origin, Pts, FieldModel)                                 \
    } else {                                                                   \
      errs() << "Unknown analysis mode: " << AnalysisMode << "\n";             \
      errs() << "Valid modes: ci, 1-cfa, 2-cfa, origin\n";                     \
      exit(1);                                                                 \
    }                                                                          \
  }

#define determineFieldmodel(Pts)                                               \
  {                                                                            \
    if (FieldSensitive) {                                                      \
      determineCtx(Pts, FSModel)                                               \
    } else {                                                                   \
      determineCtx(Pts, FIModel)                                               \
    }                                                                          \
  }

#define determinePts()                                                         \
  {                                                                            \
    if (ConfigUseBDDPts) {                                                     \
      if (ConfigBDDPtsReorder) {                                               \
        const auto &methodName = ConfigBDDPtsReorderMethod.getValue();         \
        BDDAndersPtsSet::ReorderingMethod method =                             \
            BDDAndersPtsSet::ReorderingMethod::Sift;                           \
        if (!BDDAndersPtsSet::parseReorderingMethod(methodName, method)) {     \
          llvm::report_fatal_error(                                            \
              llvm::Twine("Unknown BDD reordering method: ") + methodName);    \
        }                                                                      \
        BDDAndersPtsSet::configureReordering(true, method);                    \
      } else {                                                                 \
        BDDAndersPtsSet::configureReordering(false);                           \
      }                                                                        \
      std::cout << "Using BDD-based points-to analysis\n";                     \
      determineFieldmodel(BDDPts)                                              \
    } else {                                                                   \
      std::cout << "Using BitVector-based points-to analysis\n";               \
      determineFieldmodel(BitVectorPTS)                                        \
    }                                                                          \
  }

    determinePts()
    
    // Preprocessing passes
    _passes = std::make_unique<legacy::PassManager>();
    llvm::errs() << "Preprocessing IR...\n";
    _passes->add(new CanonicalizeGEPPass());
    _passes->add(new LoweringMemCpyPass());
    _passes->add(new RemoveExceptionHandlerPass());
    _passes->add(new RemoveASMInstPass());
    _passes->add(new StandardHeapAPIRewritePass());

    // Analysis passes
    _passes->add(_ptaPass.first.get());
    if (DumpStats) llvm::ResetStatistics();
    llvm::errs() << "Running pointer analysis...\n";
    _passes->run(M);
    llvm::errs() << "Analysis completed.\n";

    // Dump if required
    if (DumpStats) llvm::PrintStatistics(llvm::outs());

    // Prepare for query
    _ptaPass.second->getCtx();
  }
  bool getPointsToSet(Ptr value, PointsToSet &pts) override {
    std::vector<const llvm::Value *> pts1;
    _ptaPass.second->getPointsTo(value, pts1);
    for (auto &site : pts1) pts.push_back(const_cast<llvm::Value *>(site));
    return true;
  }

  PTAliasResult getAliasResult(Ptr a, Ptr b) override {
    auto may = _ptaPass.second->alias(a, b);
    return may ? 
      llvm::AliasResult::MayAlias : llvm::AliasResult::NoAlias;
  }
};

AserPTAQueryServer::~AserPTAQueryServer() = default;

int main(int argc, char **argv) {
  ptacxx::options::CGPatchCLIntercept().go(argc, argv);
  InitLLVM X(argc, argv);
  // Parse command line
  cl::ParseCommandLineOptions(
      argc, argv, "AserPTA - High-Performance Pointer Analysis Tool\n");

  auto irm = IRManager();
  irm.addMainModule(InputFilename);
  return AserPTAQueryServer(irm).run();
}
