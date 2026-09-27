#pragma once
#include <memory>
#include "pass.h"

namespace cse {

class CleanupPass : public Pass {
 public:
  std::string name() const override { return "Cleanup"; }
  void run(IRModule& module) override;
};

inline std::unique_ptr<Pass> createCleanupPass() {
  return std::make_unique<CleanupPass>();
}

}  // namespace cse