#include "constant_fold.h"

#include "../ir/ir_module.h"
#include "../ir/ir_utils.h"
#include "../ir/statement.h"
#include "../ir/stmt_walk.h"

namespace cse {

class ConstantFoldVisitor {
 public:
  explicit ConstantFoldVisitor(IRModule& mod) : module(mod) {}
  IRModule& module;
  int folds = 0;

  // Every expression slot the statement owns -- including the lvalue of an
  // element store, whose index is ordinary arithmetic and folds like any other
  // expression.
  void visitStmt(StmtIR* stmt) {
    forEachExprDeep(stmt, [&](DAGNode*& e) { e = fold(e); });
  }

  DAGNode* fold(DAGNode* node) {
    DAGNode* result = foldConst(module, node);
    if (result != node) folds++;
    return result;
  }
};

void ConstantFoldPass::run(IRModule& module) {
  ConstantFoldVisitor visitor(module);
  visitor.visitStmt(module.body.get());
}

}  // namespace cse
