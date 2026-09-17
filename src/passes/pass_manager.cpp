#include "pass_manager.h"

#include "../ir/ir_module.h"
#include "algebraic_simplify.h"
#include "constant_fold.h"
#include "cse_pass.h"
#include "dce.h"
#include "expr_recomb.h"
#include "value_prop.h"

namespace cse {

void PassManager::addPass(std::unique_ptr<Pass> pass) {
  _passes.push_back(std::move(pass));
}

void PassManager::runAll(IRModule& module) {
  for (auto& pass : _passes) {
    pass->run(module);
  }
}

PassManager PassManager::createDefault(bool enableRecombine) {
  PassManager pm;
  pm.addPass(createConstantFoldPass());
  pm.addPass(createAlgebraicSimplifyPass());
  pm.addPass(createCSEPass());
  if (enableRecombine) {
    pm.addPass(createExprRecombinePass());
  }
  pm.addPass(createValuePropPass());
  pm.addPass(createDCEPass());
  return pm;
}

}  // namespace cse
