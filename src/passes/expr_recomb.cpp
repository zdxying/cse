#include "expr_recomb.h"

#include <unordered_set>
#include <utility>

#include "../ir/ir_module.h"
#include "../ir/statement.h"
#include "../ir/stmt_walk.h"

namespace cse {

class ExprRecombineVisitor {
 public:
  ExprRecombineVisitor(IRModule& mod, bool commutative)
      : module(mod), commutative_(commutative) {}

  IRModule& module;
  // Whether `+` and `*` may be assumed to commute. Needed by the rewrites that
  // have to move an operand across the operator; off by default.
  bool commutative_;

  void visitStmt(StmtIR* stmt) {
    forEachExprDeep(stmt, [&](DAGNode*& e) { e = rewrite(e); });
  }

  DAGNode* rewrite(DAGNode* node) {
    if (!node) return nullptr;

    // First, recursively rewrite children. Rebuild through the factory when a
    // child changes so the node stays registered under a consistent hash.
    if (node->kind == NodeKind::BinaryOp && node->operands.size() == 2) {
      DAGNode* lhs = rewrite(node->operands[0]);
      DAGNode* rhs = rewrite(node->operands[1]);
      if (lhs != node->operands[0] || rhs != node->operands[1]) {
        node = module.createBinaryOp(node->op, lhs, rhs);
      }
    }

    // Try to apply recombination patterns
    if (node->kind == NodeKind::BinaryOp && (node->op == OpCode::Add || node->op == OpCode::Sub)) {
      DAGNode* result = tryFactorAddSub(node);
      if (result && result != node) {
        return result;
      }
    }

    return node;
  }

 private:
  // May `a` and `b` be used as one shared factor?
  //
  // The rewrite collapses two textual occurrences into a single one, so the
  // matched subexpression must be referentially transparent: dropping one
  // evaluation of an impure node (a side-effecting call, a load from a writable
  // location) changes behaviour. A pure node is interned, so two pure
  // subexpressions are interchangeable exactly when they are the *same node* --
  // compare by identity. A structural walk used to live here and drifted: it
  // compared kind/op/operands but not the cast type, so the distinct pure Cast
  // nodes `(int)x` and `(float)x` were judged equal and one cast was dropped.
  bool samePureExpr(DAGNode* a, DAGNode* b) {
    return a && b && a->pure && b->pure && a->id == b->id;
  }

  // Factoring moves the sum inside the product. If the factors mix an integral
  // and a floating kind, the source computes (part of) the products at one
  // width and the rewrite computes the sum at another -- `a*x + a` with a
  // double and an unsigned x is a double sum, `a*(x+1)` wraps x+1. Require a
  // single *known* kind across every operand the rewrite regroups; Unknown
  // (template parameter, member) cannot be proven either way and is refused.
  bool sameKnownKind(DAGNode* a, DAGNode* b) {
    return a && b && a->numKind != NumericKind::Unknown &&
           a->numKind == b->numKind;
  }

  bool allSameKind(DAGNode* a, DAGNode* b, DAGNode* c) {
    return sameKnownKind(a, b) && sameKnownKind(a, c);
  }

  bool allSameKind(DAGNode* a, DAGNode* b, DAGNode* c, DAGNode* d) {
    return allSameKind(a, b, c) && sameKnownKind(a, d);
  }

  DAGNode* oneOfKind(DAGNode* like) {
    return module.createConstTyped(
        1.0, like->numKind == NumericKind::Float, "");
  }

  bool isMul(DAGNode* n) {
    return n && n->kind == NodeKind::BinaryOp && n->op == OpCode::Mul &&
           n->operands.size() == 2;
  }

  // The inner expression of a negation (`-t` -> t), or nullptr.
  DAGNode* negInner(DAGNode* n) {
    if (n && n->kind == NodeKind::UnaryOp && n->op == OpCode::Neg && !isIncDec(n) &&
        n->operands.size() == 1)
      return n->operands[0];
    return nullptr;
  }

  DAGNode* tryFactorAddSub(DAGNode* addNode) {
    DAGNode* lhs = addNode->operands[0];
    DAGNode* rhs = addNode->operands[1];
    OpCode addOp = addNode->op;

    // Normalize to "<product> ± <other term>" so that one set of patterns covers
    // both the source order and the order ReassociatePass leaves behind: it
    // rewrites `a*x + a` as `a + a*x`, and `a*x - a` as `(-a) + a*x`. Both steps
    // move an operand across the operator, so they need `+` to commute.
    if (commutative_ && addOp == OpCode::Add) {
      if (DAGNode* neg = negInner(lhs)) {
        // (-t) + p  ->  p - t
        addOp = OpCode::Sub;
        lhs = rhs;
        rhs = neg;
      } else if (!isMul(lhs) && isMul(rhs)) {
        // t + p  ->  p + t
        std::swap(lhs, rhs);
      }
    }

    // Pattern: a*x ± a*y -> a*(x ± y) and a*x ± b*x -> (a ± b)*x
    //
    // Only multiplication is factored. `a / x + b / x` has no generally valid
    // single-operator form: (a+b)/x holds for exact (field) division but NOT
    // for integer division -- 3/2 + 1/2 == 1, whereas (3+1)/2 == 2 -- and the
    // IR carries no type information to tell the two apart. Emitting `*` there
    // (an earlier defect) or even `/` would silently change integer results,
    // so both operands must be `*` for this pattern to apply.
    if (isMul(lhs) && isMul(rhs)) {
      DAGNode* la = lhs->operands[0];
      DAGNode* lb = lhs->operands[1];
      DAGNode* ra = rhs->operands[0];
      DAGNode* rb = rhs->operands[1];

      // a*x ± a*y → a*(x ± y)
      if (samePureExpr(la, ra) && allSameKind(la, lb, ra, rb)) {
        auto inner = module.createBinaryOp(addOp, lb, rb);
        return module.createBinaryOp(OpCode::Mul, la, inner);
      }
      if (samePureExpr(lb, rb) && allSameKind(la, lb, ra, rb)) {
        auto outer = module.createBinaryOp(addOp, la, ra);
        return module.createBinaryOp(OpCode::Mul, outer, lb);
      }
      // The two cross combinations regroup the factors of the products --
      // `la*lb + ra*la` becomes `la*(lb+ra)` -- which assumes `*` commutes.
      if (commutative_ && samePureExpr(la, rb) && allSameKind(la, lb, ra, rb)) {
        auto inner = module.createBinaryOp(addOp, lb, ra);
        return module.createBinaryOp(OpCode::Mul, la, inner);
      }
      if (commutative_ && samePureExpr(lb, ra) && allSameKind(la, lb, ra, rb)) {
        auto outer = module.createBinaryOp(addOp, la, rb);
        return module.createBinaryOp(OpCode::Mul, outer, lb);
      }
    }

    // Pattern: a*x + a -> a*(x + 1)
    if (addOp == OpCode::Add && isMul(lhs)) {
      DAGNode* mulA = lhs->operands[0];
      DAGNode* mulB = lhs->operands[1];
      if (samePureExpr(mulA, rhs) && allSameKind(mulA, mulB, rhs)) {
        auto one = oneOfKind(mulA);
        auto inner = module.createBinaryOp(OpCode::Add, mulB, one);
        return module.createBinaryOp(OpCode::Mul, mulA, inner);
      }
      if (samePureExpr(mulB, rhs) && allSameKind(mulA, mulB, rhs)) {
        auto one = oneOfKind(mulB);
        auto inner = module.createBinaryOp(OpCode::Add, mulA, one);
        return module.createBinaryOp(OpCode::Mul, mulB, inner);
      }
    }

    // Pattern: a*x - a -> a*(x - 1)
    if (addOp == OpCode::Sub && isMul(lhs)) {
      DAGNode* mulA = lhs->operands[0];
      DAGNode* mulB = lhs->operands[1];
      if (samePureExpr(mulA, rhs) && allSameKind(mulA, mulB, rhs)) {
        auto one = oneOfKind(mulA);
        auto inner = module.createBinaryOp(OpCode::Sub, mulB, one);
        return module.createBinaryOp(OpCode::Mul, mulA, inner);
      }
      if (samePureExpr(mulB, rhs) && allSameKind(mulA, mulB, rhs)) {
        auto one = oneOfKind(mulB);
        auto inner = module.createBinaryOp(OpCode::Sub, mulA, one);
        return module.createBinaryOp(OpCode::Mul, mulB, inner);
      }
    }

    return addNode;
  }
};

void ExprRecombinePass::run(IRModule& module) {
  ExprRecombineVisitor visitor(module, _commutative);
  visitor.visitStmt(module.body.get());
}

}  // namespace cse
