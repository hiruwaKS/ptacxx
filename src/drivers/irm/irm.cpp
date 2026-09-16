#include "common/IRManager.h"
#include "common/QueryInterface.h"
#include "common/PAWrapper.h"
#include "common/Common.h"

#include <llvm/Support/CommandLine.h>
#include <type_traits>
#include <variant>

/**
 * IRM is short for IRManager, it is not an analyzer
 *   it helps to locate the debug info
 */
LLVM_CL_IGNORE_WARNINGS_BEGIN
static llvm::cl::opt<std::string>
    IRPath(llvm::cl::Positional, llvm::cl::desc("<ir-path>"),
           llvm::cl::Optional);
LLVM_CL_IGNORE_WARNINGS_END

class IRMQueryServer : public PAWrapper {
public:
  IRMQueryServer() = default;
  ~IRMQueryServer() override;
private:
  std::string argParseAndInitLLVM(int argc, char **argv) override {
    llvm::cl::ParseCommandLineOptions(argc, argv);
    if (IRPath.empty())
      throw ptacxx::ConfigError("configerror-no-input", "no input bitcode file");
    return IRPath;
  }
  void init() override { return; }
  bool getPointsToSet(Ptr, PointsToSet &) override {
    return false;
  }
  PTAliasResult getAliasResult(Ptr, Ptr) override {
    return llvm::AliasResult::MayAlias;
  }
  ptacxx::PTResult getPointToResultCached(Ptr, AllocSite) override {
    return llvm::AliasResult::MayAlias;
  }
  ptacxx::PTResult getPointToResultCachedSlow(Ptr, AllocSite) override {
    return llvm::AliasResult::MayAlias;
  }
};

IRMQueryServer::~IRMQueryServer() = default;

int main(int argc, char *argv[]) {
  return IRMQueryServer().run(argc, argv);
}
