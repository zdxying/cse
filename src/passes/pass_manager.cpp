#include "pass_manager.h"
#include "cse_pass.h"
#include "expr_recomb.h"
#include "algebraic_simplify.h"
#include "../ir/ir_module.h"

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
    pm.addPass(createCSEPass());
    pm.addPass(createAlgebraicSimplifyPass());
    if (enableRecombine) {
        pm.addPass(createExprRecombinePass());
    }
    return pm;
}

} // namespace cse
