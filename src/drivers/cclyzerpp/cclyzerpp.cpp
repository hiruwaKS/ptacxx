#include "drivers/common/QueryInterface.h"
#include "drivers/common/Transport.h"
#include "drivers/common/Postprocess.h"
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

class CCLyzerQueryServer : public QueryServer<CCLyzerQueryServer> {
  friend class QueryServer<CCLyzerQueryServer>;
private:
  std::unique_ptr<IRManager> _irm;
  std::unique_ptr<cclyzer::LegacyPointerAnalysis> _pa;
  std::unique_ptr<llvm::legacy::PassManager> _passes;

private:
  void _init_impl(int argc, char **argv) {
    InitLLVM X(argc, argv);
    cl::ParseCommandLineOptions(argc, argv, "cclyzer++ pointer analysis\n");

    _irm = std::make_unique<IRManager>();
    _irm->addMainModule(InputFilename);

    _pa = std::make_unique<cclyzer::LegacyPointerAnalysis>();
    _passes = std::make_unique<llvm::legacy::PassManager>();
    _passes->add(_pa.get());
    _passes->run(_irm->getModule());
  }

  std::string _handle_query_impl(const std::string &req) {
    PAQuery query = parse(req, *_irm);
    PAResponse response = std::visit([&](const auto &arg) -> PAResponse {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, IRMQuery>)
        return arg;
      if constexpr (std::is_same_v<T, IRParseError>)
        return ErrorOut{arg.message};
      if constexpr (std::is_same_v<T, AliasIn>) {
        llvm::MemoryLocation locA(
          arg.a, llvm::LocationSize::beforeOrAfterPointer(), llvm::AAMDNodes());
        llvm::MemoryLocation locB(
          arg.b, llvm::LocationSize::beforeOrAfterPointer(), llvm::AAMDNodes());
        llvm::SimpleAAQueryInfo AAQI;
        return AliasOut{_pa->getResult().alias(locA, locB, AAQI)};
      }
      return ErrorOut{"unknown query type or not available"};
    }, query);
    return responseToString(response, *_irm);
  }
};

int main(int argc, char **argv) {
  return CCLyzerQueryServer().run(argc, argv);
}
