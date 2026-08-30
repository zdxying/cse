#include "pass_manager.h"
#include "cse_pass.h"
#include "expr_recomb.h"
#include "algebraic_simplify.h"
#include "../ir/ir_module.h"

namespace cse {

void PassManager::addPass(std::unique_ptr<Pass> pass) {
    passes_.push_back(std::move(pass));
}

void PassManager::runAll(IRModule& module) {
    for (auto& pass : passes_) {
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
