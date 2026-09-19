#pragma once
#include <memory>

#include "pass.h"

namespace cse {

// Propagates straight-line constant counters (e.g. `unsigned int i{}; ... ++i;`)
// so their uses become constants (`tensor[i]` -> `tensor[0]`, `tensor[1]`, ...),
// and folds `if` statements whose condition is a compile-time constant. Used
// after loop unrolling for the FreeLB tensor kernels.
class CounterPropPass : public Pass {
 public:
  std::string name() const override { return "counter-prop"; }
  void run(IRModule& module) override;
};

inline std::unique_ptr<Pass> createCounterPropPass() {
  return std::make_unique<CounterPropPass>();
}

}  // namespace cse
