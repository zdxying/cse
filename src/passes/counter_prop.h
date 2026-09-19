#pragma once
#include <functional>
#include <memory>
#include <string>

#include "pass.h"

namespace cse {

// Propagates straight-line constant counters (e.g. `unsigned int i{}; ... ++i;`)
// so their uses become constants (`tensor[i]` -> `tensor[0]`, `tensor[1]`, ...),
// and folds `if` statements whose condition is a compile-time constant. Runs
// after loop unrolling.
//
// An optional `vectorLocalName` hook maps a lowered vector local and a now
// constant component index back to the scalar variable that holds it (e.g.
// `("unew", 1) -> "unew_1"`); it is only applied when that variable exists.
class CounterPropPass : public Pass {
 public:
  using VectorLocalName =
      std::function<std::string(const std::string&, long long)>;

  explicit CounterPropPass(VectorLocalName vectorLocalName = nullptr)
      : _vectorLocalName(std::move(vectorLocalName)) {}

  std::string name() const override { return "counter-prop"; }
  void run(IRModule& module) override;

 private:
  VectorLocalName _vectorLocalName;
};

inline std::unique_ptr<Pass> createCounterPropPass(
    CounterPropPass::VectorLocalName vectorLocalName = nullptr) {
  return std::make_unique<CounterPropPass>(std::move(vectorLocalName));
}

}  // namespace cse
