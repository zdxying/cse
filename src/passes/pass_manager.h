#pragma once
#include <memory>
#include <vector>

#include "pass.h"

namespace cse {

class PassManager {
 public:
  PassManager() = default;

  // Add a pass (takes ownership)
  void addPass(std::unique_ptr<Pass> pass);

  // Run all passes in order
  void runAll(IRModule& module);

  // Create a default pass pipeline
  // enableRecombine: whether to enable expression recombination
  static PassManager createDefault(bool enableRecombine = false);

  size_t passCount() const { return _passes.size(); }

 private:
  std::vector<std::unique_ptr<Pass>> _passes;
};

}  // namespace cse
