/*
 * See the origin (`<lotus>/tools/alias/lotus-alias-lotus-aa.cpp`)
       for documentation.
 */

#include "common/PAWrapper.h"
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

class LotusAAQueryServer : public IncluPAWrapper {
private:
  std::unique_ptr<legacy::PassManager> _passes;
  LotusAA *_lotusaa = nullptr;
public:
  LotusAAQueryServer() = default;
  ~LotusAAQueryServer() override;
private:
  std::string argParseAndInitLLVM(int argc, char **argv) override {
    pInitLLVM = new InitLLVM(argc, argv);
    PassRegistry &Registry = *PassRegistry::getPassRegistry();
    initializeCore(Registry);
    initializeAnalysis(Registry);
    cl::ParseCommandLineOptions(argc, argv, "LotusAA Pointer Analysis Tool\n");
    return InputFilename;
  }
  void init() override {
    auto &M = _irm.getModule();

    // Create output file if specified
    std::unique_ptr<ToolOutputFile> Out;
    if (!OutputFilename.empty()) {
      std::error_code EC;
      Out =
          std::make_unique<ToolOutputFile>(OutputFilename, EC, sys::fs::OF_None);
      if (EC) {
        errs() << EC.message() << '\n';
        throw ptacxx::FileSystemError("filesystemerror-output-file-open-failed", EC.message());
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
    _lotusaa = new LotusAA();
    _passes->add(_lotusaa);
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
  bool getPointsToSet(Ptr value, PointsToSet &pts) override {
    /// TODO:  use  `getLoadValues` for path-sensitivity, and handle the interprocedural case
    // it ptr is a global variable, we just put itself into the pts
    if (isa<GlobalValue>(value)) {
      pts.push_back(value);
      return true;
    }
    if (auto *I = dyn_cast<Instruction>(value)) {
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
};

LotusAAQueryServer::~LotusAAQueryServer() = default;

int main(int argc, char **argv) {
  return LotusAAQueryServer().run(argc, argv);
}
