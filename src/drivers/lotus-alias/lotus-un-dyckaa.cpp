/*
 * Adapted from <lotus>/tools/alias/lotus-alias-dyck-aa.cpp
 */

#include "drivers/common/QueryInterface.h"
#include "drivers/common/Transport.h"
#include "drivers/common/Postprocess.h"
#include "common/IRManager.h"
#include "common/Common.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/CLIUtils.h"
#include "Alias/UnificationBased/DyckAA/DyckAliasAnalysis.h"
#include "Alias/UnificationBased/DyckAA/DyckCallGraph.h"

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::alias::tools;

LLVM_CL_IGNORE_WARNINGS_BEGIN

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required);

static cl::opt<bool> PrintCallGraph("print-cg",
                                    cl::desc("Print call graph statistics"),
                                    cl::init(false));

static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false));
static cl::opt<bool> OnlyStatistics("s", cl::desc("Only output statistics"),
                                    cl::init(false));

LLVM_CL_IGNORE_WARNINGS_END

class DyckAAQueryServer : public QueryServer<DyckAAQueryServer> {
  friend class QueryServer<DyckAAQueryServer>;
private:
  std::unique_ptr<legacy::PassManager> _passes;
  std::unique_ptr<DyckAliasAnalysis> _DyckAA;
  std::unique_ptr<IRManager> _irm;
private:
  void _init_impl(int argc, char **argv) {
    InitLLVM X(argc, argv);
    cl::ParseCommandLineOptions(argc, argv, "DyckAA Pointer Analysis Tool\n");

    _irm = std::make_unique<IRManager>();
    _irm->addMainModule(InputFilename);
    auto &M = _irm->getModule();

    if (Verbose) {
      errs() << "Running DyckAA on " << M.getName() << " ("
            << M.getFunctionList().size() << " functions)\n";
    }

    _passes = std::make_unique<legacy::PassManager>();
    _DyckAA = std::make_unique<DyckAliasAnalysis>();
    _passes->add(_DyckAA.get());
    _passes->run(M);
    
    if (PrintCallGraph && !OnlyStatistics) {
      DyckCallGraph *CG = _DyckAA->getDyckCallGraph();
      if (CG) {
        unsigned totalIndirectCalls = 0, totalTargets = 0;
        for (auto it = CG->begin(); it != CG->end(); ++it) {
          if (auto *Node = it->second) {
            totalIndirectCalls += Node->pointer_call_size();
            for (auto pc = Node->pointer_call_begin();
                pc != Node->pointer_call_end(); ++pc) {
              if (*pc)
                totalTargets += (*pc)->size();
            }
          }
        }
        outs() << "Call graph: " << CG->size() << " nodes, " << totalIndirectCalls
              << " indirect calls, " << totalTargets << " resolved targets\n";
      }
    }

    if (OnlyStatistics || Verbose) {
      errs() << "\n=== Statistics ===\n";
      PrintStatistics(errs());
    }
  }
  std::string _handle_query_impl(const std::string &req){
    PAQuery query = parse(req, *_irm);
    PAResponse response = std::visit([&](const auto &arg) -> PAResponse {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, IRMQuery>)
        return arg;
      if constexpr (std::is_same_v<T, IRParseError>)
        return ErrorOut{arg.message};
      if constexpr (std::is_same_v<T, AliasIn>) {
        auto may = _DyckAA->mayAlias(const_cast<Value*>(arg.a), const_cast<Value*>(arg.b));
        return AliasOut{ may ? 
          llvm::AliasResult::MayAlias : llvm::AliasResult::NoAlias
        };
      }
      if constexpr (std::is_same_v<T, AliasSetIn>) {
        return AliasSetOut{ _DyckAA->getAliasSet(const_cast<Value*>(arg.ptr)) };
      }
      return ErrorOut{"unknown query type or not available"};
    }, query);
    return responseToString(response, *_irm);
  } 
};

int main(int argc, char **argv) {
  return DyckAAQueryServer().run(argc, argv);
}
