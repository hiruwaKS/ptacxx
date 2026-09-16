#include "common/PAWrapper.h"
#include "common/QueryInterface.h"
#include "common/IRManager.h"
#include "common/Common.h"

#include "cclyzerpp/src/PointerAnalysis.h"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>

#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

using namespace llvm;

LLVM_CL_IGNORE_WARNINGS_BEGIN

static cl::opt<std::string> InputFilename(
    cl::Positional,
    cl::desc("<input bitcode file>"),
    cl::init("-"),
    cl::value_desc("filename"),
    cl::Required);

LLVM_CL_IGNORE_WARNINGS_END

class CCLyzerQueryServer : public UnifiPAWrapper {
private:

  cclyzer::LegacyPointerAnalysis *_pa = nullptr;
  std::unique_ptr<llvm::legacy::PassManager> _passes;
public:
  CCLyzerQueryServer() = default;
  ~CCLyzerQueryServer() override;
private:
  std::string argParseAndInitLLVM(int argc, char **argv) override {
    pInitLLVM = new InitLLVM(argc, argv);
    cl::ParseCommandLineOptions(argc, argv, "cclyzer++ pointer analysis\n");
    return InputFilename;
  }
  void init() override {
    _pa = new cclyzer::LegacyPointerAnalysis();
    _passes = std::make_unique<llvm::legacy::PassManager>();
    _passes->add(_pa);
    _passes->run(_irm.getModule());
  }

  PTAliasResult getAliasResult(Ptr a, Ptr b) override {
    llvm::MemoryLocation locA(
      a, llvm::LocationSize::beforeOrAfterPointer(), llvm::AAMDNodes());
    llvm::MemoryLocation locB(
      b, llvm::LocationSize::beforeOrAfterPointer(), llvm::AAMDNodes());
    llvm::SimpleAAQueryInfo AAQI;
    return _pa->getResult().alias(locA, locB, AAQI);
  }
};

CCLyzerQueryServer::~CCLyzerQueryServer() = default;

int main(int argc, char **argv) {
  return CCLyzerQueryServer().run(argc, argv);
}
