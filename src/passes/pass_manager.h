#pragma once
#include <memory>
#include <vector>

#include "../frontend/cse_config.h"
#include "pass.h"

namespace cse {

class PassManager {
 public:
  PassManager() = default;

  // Add a pass (takes ownership)
  void addPass(std::unique_ptr<Pass> pass);

  // Run all passes in order. If `verbose`, print each pass name to stderr.
  void runAll(IRModule& module, bool verbose = false);

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
};

}  // namespace cse
