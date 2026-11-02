#pragma once
#include <string>

namespace cse {

class IRModule;

// Base class for all optimization passes.
// Passes mutate an IRModule (DAG nodes + structured statements) in place; the
// default pipeline's order and per-stage gating live in
// PassManager::createDefault().
class Pass {
 public:
  virtual ~Pass() = default;
  virtual std::string name() const = 0;
  virtual void run(IRModule& module) = 0;

  // Does this pass preserve the program's observable effects exactly -- the
  // same impure calls and increments/decrements, the same number of times?
  // (analysis/effects.h). True for every pass except loop unrolling, which
  // replicates the body, so an impure call inside the loop becomes one call per
  // iteration. Consumed by the effect-preservation check in
  // PassManager::runAll and reported by the pass-contract oracle.
  virtual bool preservesEffects() const { return true; }

  // Does this pass reuse computation *across* statements? A pass that answers
  // yes must consult the write set (AliasInfo::build, src/ir/location.h) at
  // entry -- variables are interned by name, so without the write-set check two
  // identical subexpressions around a store are the *same* node and any reuse
  // across the store is a miscompile. The pass-contract oracle pins the exact
  // set of default-pipeline passes that declare this, so a pass that starts
  // reusing across statements without declaring (and checking writes) fails
  // the oracle rather than waiting for a numerical differential to stumble
  // into it. Plugin-injected passes (e.g. counter-prop) declare it too; the
  // oracle sees only the default pipeline, theirs is pinned by their own
  // verifiers.
  virtual bool crossesStatements() const { return false; }
};

}  // namespace cse
