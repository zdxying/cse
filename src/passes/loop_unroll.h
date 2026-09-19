#pragma once
#include <memory>

#include "pass.h"

namespace cse {

// Loop Unrolling Pass
// Expands canonical counted loops of the form
//   for (i = 0; i < N; ++i) { body }
// into N copies of the body with the loop variable replaced by a constant.
// Required so that cross-statement CSE can see the individual iterations.
class LoopUnrollPass : public Pass {
 public:
  explicit LoopUnrollPass(int maxUnroll = 64) : _maxUnroll(maxUnroll) {}

  std::string name() const override { return "LoopUnroll"; }
  void run(IRModule& module) override;

 private:
  int _maxUnroll;
};

inline std::unique_ptr<Pass> createLoopUnrollPass(int maxUnroll = 64) {
  return std::make_unique<LoopUnrollPass>(maxUnroll);
}

}  // namespace cse
