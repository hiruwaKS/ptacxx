#include "Postprocess.h"

#include "llvm/ADT/SmallPtrSet.h"

#include <algorithm>

llvm::AliasResult aliasByIntersection(std::vector<const llvm::Value *> pts1, 
    std::vector<const llvm::Value *> pts2) {
  if (pts1.size() == 0 || pts2.size() == 0) return llvm::AliasResult::NoAlias;
  bool intersect = false;
  if (pts1.size() > 20) {
    llvm::SmallPtrSet<const llvm::Value *, 32> pts2_set;
    pts2_set.insert(pts2.begin(), pts2.end());
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
