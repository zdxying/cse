#pragma once
#include <memory>

#include "pass.h"

namespace cse {

// Additive Reassociation Pass
// Flattens `+`/`-` chains and rebuilds them ordered by how often each signed
// term occurs elsewhere in the function. Terms that are invariant across
// symmetric statements are therefore grouped into a shared prefix, so that a
// following CSE pass can extract e.g. `var0 + 4.5*uc^2` once per direction
// pair instead of recomputing it for every direction.
class ReassociatePass : public Pass {
 public:
  std::string name() const override { return "Reassociate"; }
  void run(IRModule& module) override;
};

inline std::unique_ptr<Pass> createReassociatePass() {
  return std::make_unique<ReassociatePass>();
}

}  // namespace cse
