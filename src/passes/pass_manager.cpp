#include "pass_manager.h"

#include <iostream>

#include "../ir/ir_module.h"
#include "algebraic_simplify.h"
#include "constant_fold.h"
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
    // A pass may not leave the DAG in a state the next one cannot reason about.
    // Free in a release build; in a debug build it asserts on the first pass
    // that breaks the interning invariant (see dag_node.h).
    module.verify();
  }
}

PassManager PassManager::createDefault(const CSEConfig& config,
                                      bool enableRecombine,
                                      std::unique_ptr<Pass> resolvePass,
                                      std::unique_ptr<Pass> postAlgebraPass) {
  PassManager pm;
  pm.addPass(createLoopUnrollPass());
  if (resolvePass) {
    pm.addPass(std::move(resolvePass));
  }
  const bool comm = config.assumeNumericCommutative;
  const bool assoc = config.assumeNumericAssociative;
  const bool unsafeIdent = config.allowUnsafeFpIdentities;
  pm.addPass(createConstantFoldPass());
  pm.addPass(createAlgebraicSimplifyPass(comm, assoc, unsafeIdent,
                                         config.allowFpReassoc));
  if (postAlgebraPass) {
    pm.addPass(std::move(postAlgebraPass));
  }
  // Additive reassociation may change floating-point results, so it additionally
  // requires allowFpReassoc.
  if (assoc && config.allowFpReassoc) {
    pm.addPass(createReassociatePass());
  }
  pm.addPass(createCSEPass());
  if (enableRecombine) {
    pm.addPass(createExprRecombinePass(comm));
    pm.addPass(createAlgebraicSimplifyPass(comm, assoc, unsafeIdent,
                                           config.allowFpReassoc));
  }
  pm.addPass(createValuePropPass());
  pm.addPass(createDCEPass());
  return pm;
}

}  // namespace cse
