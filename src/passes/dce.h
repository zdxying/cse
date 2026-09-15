#pragma once
#include <memory>
#include "pass.h"

namespace cse {

class DCEPass : public Pass {
 public:
  std::string name() const override { return "dce"; }
  void run(IRModule& module) override;
};

inline std::unique_ptr<Pass> createDCEPass() {
  return std::make_unique<DCEPass>();
}

}  // namespace cse
