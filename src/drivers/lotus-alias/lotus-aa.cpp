/*
 * See the origin (`<lotus>/tools/alias/lotus-alias-lotus-aa.cpp`)
       for documentation.
 */

#include "drivers/common/QueryInterface.h"
#include "drivers/common/Transport.h"
#include "drivers/common/Postprocess.h"
#include "common/IRManager.h"
#include "common/Common.h"

#include "Alias/InclusionBased/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/InclusionBased/LotusAA/MemoryModel/PointsToGraph.h"
#include "Alias/Infrastructure/AliasAnalysisWrapper/CLIUtils.h"

#include <llvm/IR/Dominators.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>

using namespace llvm;
using namespace lotus::alias::tools;

LLVM_CL_IGNORE_WARNINGS_BEGIN

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::init("-"),
                                          cl::value_desc("filename"), cl::Required);

static cl::opt<std::string> OutputFilename("o",
                                           cl::desc("Override output filename"),
                                           cl::value_desc("filename"));

static cl::opt<bool>
    OutputAssembly("S", cl::desc("Write LLVM assembly instead of bitcode"),
                   cl::init(false));

static cl::opt<bool> OnlyStatistics("s", cl::desc("Only output statistics"),
                                    cl::init(false));

static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false));

LLVM_CL_IGNORE_WARNINGS_END

class LotusAAQueryServer : public QueryServer<LotusAAQueryServer> {
  friend class QueryServer<LotusAAQueryServer>;
private:
  std::unique_ptr<IRManager> _irm;
  std::unique_ptr<legacy::PassManager> _passes;
  std::unique_ptr<LotusAA> _lotusaa;
private:
  void _init_impl(int argc, char **argv) {
    InitLLVM X(argc, argv);
  
    PassRegistry &Registry = *PassRegistry::getPassRegistry();
    initializeCore(Registry);
    initializeAnalysis(Registry);
  
    cl::ParseCommandLineOptions(argc, argv, "LotusAA Pointer Analysis Tool\n");
  
    // Load IR module
    _irm = std::make_unique<IRManager>();
    _irm->addMainModule(InputFilename);
    auto &M = _irm->getModule();

    // Create output file if specified
    std::unique_ptr<ToolOutputFile> Out;
    if (!OutputFilename.empty()) {
      std::error_code EC;
      Out =
          std::make_unique<ToolOutputFile>(OutputFilename, EC, sys::fs::OF_None);
      if (EC) {
        errs() << EC.message() << '\n';
        exit(1);
      }
    }
    raw_ostream &OS = Out ? Out->os() : outs();
  
    if (Verbose) {
      errs() << "Starting LotusAA Pointer Analysis...\n";
      errs() << "Input file: " << InputFilename << "\n";
      errs() << "Module: " << M.getName() << "\n";
      errs() << "Functions: " << M.getFunctionList().size() << "\n";
      errs() << "Global variables: " << M.getGlobalList().size() << "\n\n";
    }
    _passes = std::make_unique<legacy::PassManager>();
    _lotusaa = std::make_unique<LotusAA>();
    _passes->add(_lotusaa.get());
    if (Verbose) {
      errs() << "Running LotusAA analysis...\n";
    }
  
    _passes->run(M);
  
    if (Verbose) {
      errs() << "\nLotusAA analysis complete.\n";
    }
  
    // Note: Results are printed by the pass itself based on command-line flags:
    // Use -lotus-print-pts to print points-to information
    // Use -lotus-print-cg to print call graph information
  
    if (!OnlyStatistics) {
      OS << "LotusAA analysis completed successfully.\n";
      OS << "Use -lotus-print-pts to see points-to results\n";
      OS << "Use -lotus-print-cg to see call graph results\n";
    }
  
    // Write output file if specified
    if (Out) Out->keep();
  }
  static bool getPointsToSet(LotusAA* _lotusaa, 
      const Value *ptr, std::vector<const Value *> &pts) {
    /// TODO:  use  `getLoadValues` for path-sensitivity, and handle the interprocedural case
    // it ptr is a global variable, we just put itself into the pts
    if (isa<GlobalVariable>(ptr)) pts.push_back(ptr);
    if (auto *I = const_cast<Instruction *>(dyn_cast<Instruction>(ptr))) {
      auto *_intra = _lotusaa->getPtGraph(I->getFunction());
      if (!_intra) return false;
      auto *res = _intra->findPTResult(I, false);
      if (!res) return false;
      PTResultIterator it(res, _intra);
      for (auto &[loc, _cond] : it) {
        auto *obj = loc->getObj();
        if (obj->isNull() || obj->isUnknown())
          continue;
        if (obj->getKind() == MemObject::CONCRETE) {
          pts.push_back(obj->getAllocSite());
        } else {
          // TODO
        }
      }
      return true;
    }
    return false;
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
        auto succ = getPointsToSet(_lotusaa.get(), arg.ptr, pts);
        if (!succ) return PtsOut{{}};
        return PtsOut{pts};
      }
      if constexpr (std::is_same_v<T, PtIn>) {
        std::vector<const llvm::Value *> pts1;
        auto succ = getPointsToSet(_lotusaa.get(), arg.ptr, pts1);
        if (!succ) return PtOut{ResultNo};
        return PtOut{mayPointTo(pts1, arg.obj) ? ResultMay: ResultNo};
      }
      if constexpr (std::is_same_v<T, AliasIn>) {
        std::vector<const llvm::Value *> pts1;
        std::vector<const llvm::Value *> pts2;
        auto succ1 = getPointsToSet(_lotusaa.get(), arg.a, pts1);
        auto succ2 = getPointsToSet(_lotusaa.get(), arg.b, pts2);
        if (!succ1 || !succ2) return AliasOut{llvm::AliasResult::NoAlias};
        return AliasOut{ aliasByIntersection(pts1, pts2) ? 
          llvm::AliasResult::MayAlias : llvm::AliasResult::NoAlias
        };
      }
      return ErrorOut{"unknown query type or not available"};
    }, query);
    return responseToString(response, *_irm);
  }
};

int main(int argc, char **argv) {
  return LotusAAQueryServer().run(argc, argv);
}
