/*
 * See the origin (`<lotus>/tools/alias/lotus-alias-sparrow-aa.cpp`)
       for documentation.
 */
#include "common/PAWrapper.h"
#include "common/QueryInterface.h"
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

class SparrowAAQueryServer : public IncluPAWrapper {
/// TODO: Anders.getAllAllocationSites(allocSites); not adapted
/// TODO: Anders.getContextNeedMap not adapted
private:
  std::unique_ptr<Andersen> _anders;
public:
  SparrowAAQueryServer(IRManager &irm) : IncluPAWrapper(irm) {}
  ~SparrowAAQueryServer() override;
private:
  void init() override {
    auto &M = _irm.getModule();
    ContextPolicy policy = getSelectedAndersenContextPolicy();
    errs() << "\nRunning analysis...\n";
    _anders = std::make_unique<Andersen>(M, policy);
    errs() << "\nDone.\n";
    errs() << "Module: " << M.getName() << " (" << M.getFunctionList().size()
        << " functions, " << M.getGlobalList().size() << " globals)\n";
    if (policy.name && strcmp(policy.name, "NoCtx") != 0)
      errs() << "Context sensitivity: " << policy.name << "\n";
  }
  bool getPointsToSet(Ptr value, PointsToSet &pts) override {
    std::vector<const llvm::Value *> pts1;
    auto succ = _anders->getPointsToSet(value, pts1);
    if (!succ) return false;
    for (auto &v : pts1) pts.push_back(const_cast<llvm::Value *>(v));
    return true;
  }
};

SparrowAAQueryServer::~SparrowAAQueryServer() = default;

int main(int argc, char **argv) {
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
  auto irm = IRManager();
  irm.addMainModule(InputFilename);
  return SparrowAAQueryServer(irm).run();
}
