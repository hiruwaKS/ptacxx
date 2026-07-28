#include "Postprocess.h"

#include <algorithm>
#include <unordered_set>

llvm::AliasResult aliasByIntersection(std::vector<const llvm::Value *> pts1, 
    std::vector<const llvm::Value *> pts2) {
  if (pts1.size() == 0 || pts2.size() == 0) return llvm::AliasResult::NoAlias;
  bool intersect = false;
  if (pts1.size() > 20) {
    std::unordered_set<const llvm::Value *> pts2_set(pts2.begin(), pts2.end());
    for (auto site : pts1) {
      // if (site->getType()->isPointerTy()) {
      if(pts2_set.count(site)) {
        intersect = true;
        break;
      }
      // }
    }
  } else {
    for (auto site : pts1) {
      if (std::find(pts2.begin(), pts2.end(), site) != pts2.end()) {
        intersect = true;
        break;
      }
    }
  }
  return intersect ? llvm::AliasResult::MayAlias : llvm::AliasResult::NoAlias;
}
