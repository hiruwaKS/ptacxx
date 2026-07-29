/*
 * See the origin (`<lotus>/tools/alias/lotus-alias-sparrow-aa.cpp`)
       for documentation.
 */
#include "drivers/common/QueryInterface.h"
#include "drivers/common/Transport.h"
#include "drivers/common/Postprocess.h"
#include "common/IRManager.h"
#include "common/Common.h"

#include "Alias/InclusionBased/SparrowAA/Andersen.h"
#include "Alias/InclusionBased/SparrowAA/AndersenAA.h"
#include "Alias/InclusionBased/SparrowAA/Log.h"
#include "Alias/InclusionBased/SparrowAA/ResultUtils.h"
#include "Alias/Infrastructure/AliasAnalysisWrapper/CLIUtils.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::alias::tools;

LLVM_CL_IGNORE_WARNINGS_BEGIN

static cl::OptionCategory
    SparrowAACategory("Sparrow-AA Options",
                      "Options for the Sparrow-AA pointer analysis tool");
extern cl::OptionCategory AndersenCategory;
extern cl::opt<unsigned> AndersenKContext;
extern cl::opt<bool> AndersenUseBDDPointsTo;
extern cl::opt<bool> EnableAdaptiveCS;
extern cl::opt<float> AdaptiveCSThreshold;
static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required,
                                          cl::value_desc("filename"),
                                          cl::cat(SparrowAACategory));
LLVM_CL_IGNORE_WARNINGS_END

class SparrowAAQueryServer : public QueryServer<SparrowAAQueryServer> {
  friend class QueryServer<SparrowAAQueryServer>;
private:
  std::unique_ptr<IRManager> _irm;
  std::unique_ptr<Andersen> _anders;
private:
  void _init_impl(int argc, char **argv) {
    InitLLVM X(argc, argv);
    cl::HideUnrelatedOptions({&SparrowAACategory, &AndersenCategory});
    cl::ParseCommandLineOptions(
      argc, argv,
      "Andersen's Pointer Analysis Tool\n\n"
      "Subset-based, flow-insensitive, field-sensitive pointer analysis.\n\n"
      "Context Sensitivity:\n"
      "  --andersen-k-cs=<k>      Select call-site sensitivity (0 <= k <= "
      "32):\n"
      "                            0 = context-insensitive (default)\n"
      "                            1 = 1-CFA\n"
      "                            2 = 2-CFA\n"
      "                            ...\n"
      "                           32 = 32-CFA\n"
      "                            k > 32 falls back to k=0 (NoCtx)\n");
    selectGlobalPtsSetImpl(AndersenUseBDDPointsTo ? PtsSetImpl::BDD
      : PtsSetImpl::SPARSE_BITVECTOR);
    _irm = std::make_unique<IRManager>("");
    _irm->addMainModule(InputFilename);
    auto &M = _irm->getModule(0);
    ContextPolicy policy = getSelectedAndersenContextPolicy();
    errs() << "\nRunning analysis...\n";
    _anders = std::make_unique<Andersen>(M, policy);
    errs() << "\nDone.\n";
    errs() << "Module: " << M.getName() << " (" << M.getFunctionList().size()
        << " functions, " << M.getGlobalList().size() << " globals)\n";
    if (policy.name && strcmp(policy.name, "NoCtx") != 0)
      errs() << "Context sensitivity: " << policy.name << "\n";
  }
  std::string _handle_query_impl(const std::string &req){

    // TODO: Anders.getAllAllocationSites(allocSites); not adapted
    // TODO: Anders.getContextNeedMap not adapted
    
    PAQuery query = parse(req, *_irm);
    PAResponse response = std::visit([&](const auto &arg) -> PAResponse {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, IRMetadata>)
        return arg;
      if constexpr (std::is_same_v<T, IRStat>)
        return arg;
      if constexpr (std::is_same_v<T, IRDebugInfo>)
        return arg;
      if constexpr (std::is_same_v<T, NameToVIds>)
        return arg;
      if constexpr (std::is_same_v<T, IRParseError>)
        return ErrorOut{arg.message};
      if constexpr (std::is_same_v<T, PtsIn>) {
        std::vector<const llvm::Value *> pts;
        auto succ = _anders->getPointsToSet(arg.ptr, pts);
        if (!succ) return PtsOut{{}};
        return PtsOut{pts};
      }
      if constexpr (std::is_same_v<T, PtIn>) {
        std::vector<const llvm::Value *> pts1;
        auto succ = _anders->getPointsToSet(arg.ptr, pts1);
        if (!succ) return PtOut{ResultNo};
        return PtOut{mayPointTo(pts1, arg.obj) ? ResultMay: ResultNo};
      }
      if constexpr (std::is_same_v<T, AliasIn>) {
        std::vector<const llvm::Value *> pts1;
        std::vector<const llvm::Value *> pts2;
        auto succ1 = _anders->getPointsToSet(arg.a, pts1);
        auto succ2 = _anders->getPointsToSet(arg.b, pts2);
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
  return SparrowAAQueryServer().run(argc, argv);
}
