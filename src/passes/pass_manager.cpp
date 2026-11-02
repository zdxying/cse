#include "pass_manager.h"

#include <iostream>

#include "../analysis/effects.h"

#include <vector>
#include "../ir/ir_module.h"
#include "../ir/ir_printer.h"
#include "algebraic_simplify.h"
#include "cleanup.h"
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

void PassManager::runAll(IRModule& module, bool verbose, bool dumpIR) {
  if (dumpIR) {
    std::cout << "[cse] ir (input)\n" << dumpModule(module) << "\n";
  }
  for (auto& pass : _passes) {
    // Snapshot the observable effects before the pass so a rewrite that drops or
    // duplicates one can be attributed to it (analysis/effects.h).
    EffectSignature before;
    std::vector<std::string> beforeSeq;
    if (_checkEffects) {
      before = effectSignature(module.body.get());
      beforeSeq = effectSequence(module.body.get());
    }

    // `verbose` also reports whether the pass actually changed anything -- a
    // pass that is wired in but never fires is either dead or mis-gated, which
    // is exactly how `-s -r` silently dropped recombination.
    std::string irBefore;
    if (verbose) irBefore = dumpModule(module);

    if (verbose) {
      std::cerr << "[cse] pass: " << pass->name();
    }
    pass->run(module);
    // A pass may not leave the DAG in a state the next one cannot reason about.
    // Free in a release build; in a debug build it asserts on the first pass
    // that breaks the interning invariant (see dag_node.h).
    module.verify();

    if (verbose) {
      std::cerr << (dumpModule(module) == irBefore ? " (no change)" : " (changed)")
                << "\n";
    }

    if (_checkEffects) {
      const std::string violation =
          effectViolation(before, effectSignature(module.body.get()),
                          /*mayDuplicate=*/!pass->preservesEffects());
      if (!violation.empty()) {
        _effectViolations.push_back(pass->name() + ":\n" + violation);
      }
      // The multiset above cannot see a *reordering*: swapping two impure calls
      // (or moving one past a read of what it writes) keeps every count and
      // still changes behavior. A pass that preserves effects must also preserve
      // their relative order; only LoopUnroll, which replicates bodies, is
      // exempt. See effectOrderViolation (analysis/effects.h).
      if (pass->preservesEffects()) {
        const std::string orderViolation = effectOrderViolation(
            beforeSeq, effectSequence(module.body.get()),
            /*mayDuplicate=*/false);
        if (!orderViolation.empty()) {
          _effectViolations.push_back(pass->name() + ":\n" + orderViolation);
        }
      }
    }
    if (dumpIR) {
      std::cout << "[cse] ir after " << pass->name() << "\n"
                << dumpModule(module) << "\n";
    }
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
                                         config.allowFpReassoc,
                                         config.assumeFloatOperands));
  if (postAlgebraPass) {
    pm.addPass(std::move(postAlgebraPass));
  }
  // Additive reassociation may change floating-point results, so it additionally
  // requires allowFpReassoc.
  if (assoc && config.allowFpReassoc) {
    pm.addPass(createReassociatePass());
  }
  pm.addPass(createCSEPass());
  // Factoring a common multiplicand out of a sum is a floating-point regrouping
  // (`a*x + a*y -> a*(x+y)` rounds differently), so like Reassociate it requires
  // allowFpReassoc. Division is excluded separately inside the pass.
  if (enableRecombine && config.allowFpReassoc) {
    pm.addPass(createExprRecombinePass(comm));
    pm.addPass(createAlgebraicSimplifyPass(comm, assoc, unsafeIdent,
                                           config.allowFpReassoc,
                                           config.assumeFloatOperands));
  }
  pm.addPass(createValuePropPass());
  pm.addPass(createDCEPass());
  // The chained-additive merge drops a leading zero, so it carries the same
  // signed-zero licence as `x + 0 -> x`.
  pm.addPass(createCleanupPass(unsafeIdent));
  return pm;
}

}  // namespace cse
