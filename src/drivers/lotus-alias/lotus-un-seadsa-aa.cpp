/*
 * Adapted from <lotus>/tools/alias/lotus-alias-seadsa-tool.cpp
 */

#include "common/PAWrapper.h"
#include "common/QueryInterface.h"
#include "common/IRManager.h"
#include "common/Common.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/CLIUtils.h"
#include "Alias/UnificationBased/seadsa/CompleteCallGraph.hh"
#include "Alias/UnificationBased/seadsa/DsaAnalysis.hh"
#include "Alias/UnificationBased/seadsa/DsaLibFuncInfo.hh"
#include "Alias/UnificationBased/seadsa/InitializePasses.hh"
#include "Alias/UnificationBased/seadsa/SeaDsaAliasAnalysis.hh"
#include "Alias/UnificationBased/seadsa/support/RemovePtrToInt.hh"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/InitLLVM.h>

using namespace llvm;

LLVM_CL_IGNORE_WARNINGS_BEGIN

static cl::opt<std::string>
    InputFilename(cl::Positional,
                  cl::desc("<input LLVM bitcode file>"),
                  cl::Required, cl::value_desc("filename"));

static cl::opt<std::string> OutputDir("outdir",
                                            cl::desc("Output directory"),
                                            cl::init(""),
                                            cl::value_desc("DIR"));

static cl::opt<std::string>
    AsmOutputFilename("output", cl::desc("Output analyzed bitcode"),
                      cl::init(""), cl::value_desc("filename"));

static cl::opt<bool> MemDot(
    "sea-dsa-dot",
    cl::desc("Print SeaDsa memory graph of each function to dot format"),
    cl::init(false));

// TODO: add CallGraphDot and AAEval if possible

// Register passes manually since INITIALIZE_PASS macros are commented out
static RegisterPass<seadsa::DsaAnalysis>
    X("seadsa-dsa", "SeaHorn Dsa analysis: entry point for all clients");
static RegisterPass<seadsa::RemovePtrToInt>
    Y("seadsa-remove-ptrtoint",
      "Convert ptrtoint/inttoptr pairs to bitcasts when possible");
static RegisterPass<seadsa::AllocWrapInfo>
    Z("seadsa-alloc-wrap", "Identifies allocation wrappers");
static RegisterPass<seadsa::DsaLibFuncInfo>
    W("seadsa-lib-functions-info",
      "Identifies library functions for special handling");
LLVM_CL_IGNORE_WARNINGS_END

class SeadsaQueryServer : public UnifiPAWrapper {
private:
  std::unique_ptr<legacy::PassManager> _passes;
  std::unique_ptr<seadsa::SeaDsaAAWrapperPass> _seadsa;
  std::unique_ptr<ToolOutputFile> _asmOutput;
public:
  SeadsaQueryServer(IRManager &irm) : UnifiPAWrapper(irm) {}
  ~SeadsaQueryServer() override;
private:
  void init() override {
    auto &M = _irm.getModule();
  
    // Set up output file if requested
    if (!AsmOutputFilename.empty()) {
      std::error_code error_code;
      std::string outputPath = AsmOutputFilename;
      if (!OutputDir.empty()) {
        if (!llvm::sys::fs::create_directories(OutputDir)) {
          auto fname = llvm::sys::path::filename(AsmOutputFilename);
          outputPath = OutputDir + "/" + fname.str();
        }
      }
      _asmOutput = std::make_unique<llvm::ToolOutputFile>(
        outputPath, error_code, llvm::sys::fs::OF_Text);
      if (error_code) {
        errs() << "error: Could not open " << AsmOutputFilename << ": "
                     << error_code.message() << "\n";
        exit(3);
      }
    }

    _passes = std::make_unique<legacy::PassManager>();

    PassRegistry &Registry = *PassRegistry::getPassRegistry();
    initializeCore(Registry);
    seadsa::initializeAnalysisPasses(Registry);
  
    _passes->add(new llvm::TargetLibraryInfoWrapperPass());
    _passes->add(new seadsa::RemovePtrToInt());
    _passes->add(new seadsa::AllocWrapInfo());
    _passes->add(new seadsa::DsaLibFuncInfo());
    _seadsa = std::make_unique<seadsa::SeaDsaAAWrapperPass>(); // warning: immutable pass
    _passes->add(_seadsa.get());
  
    if (MemDot) _passes->add(seadsa::createDsaPrinterPass());
  
    if (!AsmOutputFilename.empty())
      errs() << "Warning: Cannot add module printing pass in this LLVM version\n";
  
    _passes->run(M);

    if (!AsmOutputFilename.empty()) _asmOutput->keep();
  }
  PTAliasResult getAliasResult(Ptr a, Ptr b) override {
    SimpleAAQueryInfo AAQI;
    auto mkLoc = [](const llvm::Value *v) {
        return llvm::MemoryLocation(v, llvm::LocationSize::beforeOrAfterPointer());
    };
    auto result = _seadsa->getResult().alias(mkLoc(a), mkLoc(b), AAQI);
    return result;
  }
};

SeadsaQueryServer::~SeadsaQueryServer() = default;

int main(int argc, char **argv) {
  EnableDebugBuffering = true;
  InitLLVM X2(argc, argv);
  cl::ParseCommandLineOptions(
      argc, argv, "Sea-DSA Advanced Memory Graph Analysis Tool");

  auto irm = IRManager();
  irm.addMainModule(InputFilename);
  return SeadsaQueryServer(irm).run();
}
