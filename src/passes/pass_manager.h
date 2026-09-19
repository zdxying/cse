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

  // Run all passes in order
  void runAll(IRModule& module);

  // Create a default pass pipeline.
  // config: semantic/safety settings (algebraic assumptions, purity, aliasing).
  // enableRecombine: whether to enable expression recombination
  // resolvePass: optional project-specific pass run after loop unrolling and
  //              before the generic algebraic/CSE pipeline (e.g. lattice
  //              constant resolution).
  static PassManager createDefault(const CSEConfig& config,
                                   bool enableRecombine = false,
                                   std::unique_ptr<Pass> resolvePass = nullptr);

  size_t passCount() const { return _passes.size(); }

 private:
  std::vector<std::unique_ptr<Pass>> _passes;
};

}  // namespace cse
