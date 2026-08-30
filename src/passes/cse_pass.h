#pragma once
#include "pass.h"
#include <memory>

namespace cse {

// CSE Pass: performs additional common subexpression elimination
// on the already-built DAG. Most CSE is done during IR building,
// but this pass handles cases across statement boundaries.
class CSEPass : public Pass {
public:
    std::string name() const override { return "CSE"; }
    void run(IRModule& module) override;
};

inline std::unique_ptr<Pass> createCSEPass() {
    return std::make_unique<CSEPass>();
}

} // namespace cse
