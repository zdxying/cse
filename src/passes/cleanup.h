#pragma once
#include <memory>
#include "pass.h"

namespace cse {

class CleanupPass : public Pass {
 public:
  // `allowUnsafeFpIdentities` licenses the chained-additive merge: it drops the
  // accumulator's leading zero (`x = 0; x = x + a; x = x + b` -> `x = a + b`),
  // which changes the sign of a zero result exactly like `x + 0 -> x` does. It
  // is the same opt-in as the other signed-zero identities, so the conservative
  // profile keeps the chain and stays IEEE-exact (see cleanup.cpp).
  explicit CleanupPass(bool allowUnsafeFpIdentities = false)
      : _allowUnsafeFpIdentities(allowUnsafeFpIdentities) {}
  std::string name() const override { return "Cleanup"; }
  void run(IRModule& module) override;
  bool crossesStatements() const override { return true; }

 private:
  bool _allowUnsafeFpIdentities;
};

inline std::unique_ptr<Pass> createCleanupPass(
    bool allowUnsafeFpIdentities = false) {
  return std::make_unique<CleanupPass>(allowUnsafeFpIdentities);
}

}  // namespace cse