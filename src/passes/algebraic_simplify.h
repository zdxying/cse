#pragma once
#include "pass.h"
#include <memory>

namespace cse {

// Algebraic Simplification Pass
// Applies:
//   - Commutativity reordering: x * a * x → a * x * x
//   - Identity elimination: a * 1 → a, a + 0 → a
//   - Zero folding: a * 0 → 0
class AlgebraicSimplifyPass : public Pass {
public:
    std::string name() const override { return "AlgebraicSimplify"; }
    void run(IRModule& module) override;
};

inline std::unique_ptr<Pass> createAlgebraicSimplifyPass() {
    return std::make_unique<AlgebraicSimplifyPass>();
}

} // namespace cse
