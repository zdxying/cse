#pragma once
#include <memory>
#include <string>
#include <vector>

#include "../frontend/cse_config.h"
#include "pass.h"

namespace cse {

class PassManager {
 public:
  PassManager() = default;

  // Add a pass (takes ownership)
  void addPass(std::unique_ptr<Pass> pass);

  // Run all passes in order. If `verbose`, print each pass name to stderr. If
  // `dumpIR`, print a structural dump of the module (input, then after each
  // pass) to stdout -- the tool for locating which pass changed something.
  void runAll(IRModule& module, bool verbose = false, bool dumpIR = false);

  // The passes in order. Exposed so the idempotence oracle can exercise each
  // pass in isolation (see tests/verify/verify_ir_idempotence.cpp).
  const std::vector<std::unique_ptr<Pass>>& passes() const { return _passes; }

  // Turn on effect-preservation checking: before and after each pass, count the
  // module's observable effects (impure calls, increments/decrements) and record
  // a violation when one is dropped, or added by a pass that declares
  // preservesEffects(). Off by default; the driver's --verify and the effect
  // oracle turn it on.
  void setCheckEffects(bool on) { _checkEffects = on; }

  // Violations found by the effect check, one string per offending pass. Empty
  // when checking is off or nothing was dropped or duplicated.
  const std::vector<std::string>& effectViolations() const {
    return _effectViolations;
  }

  // Create a default pass pipeline.
  // config: semantic/safety settings (algebraic assumptions, purity, aliasing).
  // enableRecombine: whether to enable expression recombination
  // resolvePass: optional project-specific pass run after loop unrolling and
  //              before constant folding/algebraic simplification (e.g.
  //              resolving project intrinsics to constants).
  // postAlgebraPass: optional project-specific pass run after algebraic
  //              simplification and before reassociation/CSE (e.g. propagating
  //              straight-line counters introduced by loop unrolling).
  static PassManager createDefault(const CSEConfig& config,
                                   bool enableRecombine = false,
                                   std::unique_ptr<Pass> resolvePass = nullptr,
                                   std::unique_ptr<Pass> postAlgebraPass = nullptr);

 private:
  std::vector<std::unique_ptr<Pass>> _passes;
  bool _checkEffects = false;
  std::vector<std::string> _effectViolations;
};

}  // namespace cse
