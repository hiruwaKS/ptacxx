#include <llvm/Analysis/AliasAnalysis.h>

static inline bool mayPointTo(std::vector<const llvm::Value *> pts1, const llvm::Value * site) {
  return std::find(pts1.begin(), pts1.end(), site) != pts1.end();
}

llvm::AliasResult aliasByIntersection(std::vector<const llvm::Value *> pts1, 
    std::vector<const llvm::Value *> pts2);
