#pragma once
#include <memory>
#include "pass.h"

namespace cse {

class ConstantFoldPass : public Pass {
 public:
  std::string name() const override { return "constant-fold"; }
  void run(IRModule& module) override;
};

inline std::unique_ptr<Pass> createConstantFoldPass() {
  return std::make_unique<ConstantFoldPass>();
}

}  // namespace cse
