#pragma once
#include "pass.h"
#include <memory>

namespace cse {

// Expression Recombination Pass
// Detects patterns like:
//   a*x + a*y  →  a*(x+y)
//   a*x + b*x  →  (a+b)*x
//   a*x + a*x  →  2*a*x
// and applies algebraic rewrites.
class ExprRecombinePass : public Pass {
public:
    std::string name() const override { return "ExprRecombine"; }
    void run(IRModule& module) override;
};

inline std::unique_ptr<Pass> createExprRecombinePass() {
    return std::make_unique<ExprRecombinePass>();
}

} // namespace cse
