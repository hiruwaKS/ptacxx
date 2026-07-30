#include "common/IRManager.h"
#include "common/Common.h"
#include "drivers/common/QueryInterface.h"
#include "drivers/common/Transport.h"

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

class IRMQueryServer : public QueryServer<IRMQueryServer> {
  friend class QueryServer<IRMQueryServer>;
private:
  std::unique_ptr<IRManager> _irm;
private:
  void _init_impl(int argc, char **argv) {
    llvm::cl::ParseCommandLineOptions(argc, argv);
    _irm = std::make_unique<IRManager>();
    if (!IRPath.empty())
      _irm->addMainModule(IRPath);
    if (IRPath.empty()) llvm::report_fatal_error("no input");
  }
  std::string _handle_query_impl(const std::string &req){
    PAQuery query = parse(req, *_irm);
    PAResponse response = std::visit([](const auto &arg) -> PAResponse {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, IRMQuery>)
        return arg;
      if constexpr (std::is_same_v<T, IRParseError>)
        return ErrorOut{arg.message};
      return ErrorOut{"unknown query type or not available"};
    }, query);
    return responseToString(response, *_irm);
  }
};

int main(int argc, char *argv[]) {
  return IRMQueryServer().run(argc, argv);
}
