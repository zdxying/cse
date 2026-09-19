#include "pass_manager.h"

#include <iostream>

#include "../ir/ir_module.h"
#include "algebraic_simplify.h"
#include "constant_fold.h"
#include "counter_prop.h"
#include "cse_pass.h"
#include "dce.h"
#include "expr_recomb.h"
#include "loop_unroll.h"
#include "reassociate.h"
#include "value_prop.h"

namespace cse {

void PassManager::addPass(std::unique_ptr<Pass> pass) {
  _passes.push_back(std::move(pass));
}

void PassManager::runAll(IRModule& module, bool verbose) {
  for (auto& pass : _passes) {
    if (verbose) {
      std::cerr << "[cse] pass: " << pass->name() << "\n";
    }
    pass->run(module);
  }
}

PassManager PassManager::createDefault(const CSEConfig& config,
                                      bool enableRecombine,
                                      std::unique_ptr<Pass> resolvePass) {
  PassManager pm;
  pm.addPass(createLoopUnrollPass());
  if (resolvePass) {
    pm.addPass(std::move(resolvePass));
  }
  const bool comm = config.assumeNumericCommutative;
  const bool assoc = config.assumeNumericAssociative;
  pm.addPass(createConstantFoldPass());
  pm.addPass(createAlgebraicSimplifyPass(comm, assoc));
  // FreeLB tensor kernels index outputs with a straight-line counter; resolve
  // it and fold constant `if`s after unrolling.
  if (config.lowerVectors) {
    pm.addPass(createCounterPropPass());
  }
  // Additive reassociation may change floating-point results, so it additionally
  // requires allowFpReassoc.
  if (assoc && config.allowFpReassoc) {
    pm.addPass(createReassociatePass());
  }
  pm.addPass(createCSEPass());
  if (enableRecombine) {
    pm.addPass(createExprRecombinePass());
    pm.addPass(createAlgebraicSimplifyPass(comm, assoc));
  }
  pm.addPass(createValuePropPass());
  pm.addPass(createDCEPass());
  return pm;
}

}  // namespace cse
