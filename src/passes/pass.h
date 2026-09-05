#pragma once
#include <string>

namespace cse {

class IRModule;

// Base class for all optimization passes.
// Passes operate on IRModule (DAG nodes + structured statements).
// Pipeline: CSE → AlgebraicSimplify → ExprRecombine (optional).
class Pass {
 public:
  virtual ~Pass() = default;
  virtual std::string name() const = 0;
  virtual void run(IRModule& module) = 0;
};

}  // namespace cse
