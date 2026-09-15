#pragma once
#include <memory>
#include "pass.h"

namespace cse {

class ValuePropPass : public Pass {
 public:
  std::string name() const override { return "value-prop"; }
  void run(IRModule& module) override;
};

inline std::unique_ptr<Pass> createValuePropPass() {
  return std::make_unique<ValuePropPass>();
}

}  // namespace cse
