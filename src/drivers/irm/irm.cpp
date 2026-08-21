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
  IRMQueryServer(IRManager &irm) : PAWrapper(irm) {}
  ~IRMQueryServer() override;
private:
  void init() override { return; }
  bool getPointsToSet(Ptr, PointsToSet &) override {
    return false;
  }
  PTAliasResult getAliasResult(Ptr, Ptr) override {
    return llvm::AliasResult::MayAlias;
  }
};

IRMQueryServer::~IRMQueryServer() = default;

int main(int argc, char *argv[]) {
  ptacxx::options::CGPatchCLIntercept().go(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv);
  IRManager irm;
  if (!IRPath.empty())
    irm.addMainModule(IRPath);
  else llvm::report_fatal_error("no input");
  return IRMQueryServer(irm).run();
}
