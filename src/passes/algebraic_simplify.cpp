#include "algebraic_simplify.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "../ir/ir_module.h"
#include "../ir/statement.h"
#include "../ir/stmt_walk.h"

namespace cse {

namespace {

// Format a constant for code emission with round-trip precision.
std::string numText(double v) {
  if (v == static_cast<long long>(v) && std::abs(v) < 1e15)
    return std::to_string(static_cast<long long>(v));
  std::ostringstream oss;
  oss << std::setprecision(17) << v;
  return oss.str();
}

}  // namespace

class AlgebraicSimplifyVisitor {
 public:
  AlgebraicSimplifyVisitor(IRModule& mod, bool commutative, bool associative,
                           bool unsafeFpIdentities, bool fpReassoc)
      : module(mod),
        numeric_(commutative && associative),
        unsafeIdentities_(unsafeFpIdentities),
        fpReassoc_(fpReassoc) {}
  IRModule& module;
  int simplifications = 0;
  // Numeric reordering rules require both commutativity and associativity; the
  // pass exposes the two flags separately for callers but applies them jointly.
  bool numeric_;
  // `x/x -> 1`, `0/x -> 0`, `x-x -> 0` and `x*0 -> 0` need more than a field
  // assumption -- they also require the operands to avoid 0 / +-inf / NaN -- so
  // they carry their own opt-in.
  bool unsafeIdentities_;
  // Regrouping a multiplication chain changes floating-point rounding.
  bool fpReassoc_;

  void visitStmt(StmtIR* stmt) {
    forEachExprDeep(stmt, [&](DAGNode*& e) { e = simplify(e); });
  }

  DAGNode* simplify(DAGNode* node) {
    if (!node) return nullptr;

    // Recursively simplify children first. When a child changes, rebuild this
    // node through the factory so it stays registered under a consistent hash;
    // mutating operands in place would leave a stale hash-map entry and defeat
    // later deduplication.
    if (node->kind == NodeKind::UnaryOp && !node->operands.empty()) {
      DAGNode* child = simplify(node->operands[0]);
      if (child != node->operands[0]) node = module.createUnaryOp(node->op, child);
    } else if (node->kind == NodeKind::BinaryOp && node->operands.size() == 2) {
      DAGNode* lhs = simplify(node->operands[0]);
      DAGNode* rhs = simplify(node->operands[1]);
      if (lhs != node->operands[0] || rhs != node->operands[1]) {
        node = module.createBinaryOp(node->op, lhs, rhs);
      }
    }

    // Apply algebraic simplifications. All of these assume numeric semantics
    // (identity elements, commutativity) and are skipped unless the caller has
    // opted in via the config.
    if (numeric_ && node->kind == NodeKind::BinaryOp &&
        node->operands.size() == 2) {
      DAGNode* result = applyIdentities(node);
      if (result != node) { simplifications++; return result; }
      result = sortCommutative(node);
      if (result != node) { simplifications++; return result; }
    }
    // UnaryOp identities (--a → a)
    if (numeric_ && node->kind == NodeKind::UnaryOp && !node->operands.empty()) {
      DAGNode* result = applyIdentities(node);
      if (result != node) { simplifications++; return result; }
    }

    // Strength reduction
    if (numeric_ && node->kind == NodeKind::BinaryOp &&
        node->operands.size() == 2) {
      DAGNode* result = strengthReduce(node);
      if (result != node) { simplifications++; return result; }
    }

    // Even-power canonicalization: (-a) * (-a) -> a * a
    if (numeric_ && node->kind == NodeKind::BinaryOp &&
        node->operands.size() == 2) {
      DAGNode* result = evenPower(node);
      if (result != node) { simplifications++; return result; }
    }

    // Constant product normalization: fold constant factors, const first.
    if (numeric_ && node->kind == NodeKind::BinaryOp &&
        node->operands.size() == 2) {
      DAGNode* result = normalizeProduct(node);
      if (result != node) { simplifications++; return result; }
    }

    // Sign canonicalization: factor negations out of products so that
    // eg. `3 * (-uc)` and `3 * uc` share the same `3 * uc` subexpression.
    if (numeric_ && node->kind == NodeKind::BinaryOp &&
        node->operands.size() == 2) {
      DAGNode* result = normalizeSign(node);
      if (result != node) { simplifications++; return result; }
    }

    return node;
  }

 private:
  bool isConst(DAGNode* n, double val) {
    return n->kind == NodeKind::Constant && n->constVal == val;
  }

  // x * 2 -> x + x. Exact in IEEE-754 (doubling only changes the exponent), and
  // overflow behaves the same way, so for *values* it needs no opt-in.
  //
  // It does duplicate the operand, though, so it may only be applied to an
  // operand that can be evaluated twice. `2 * f()` calls f once while
  // `f() + f()` calls it twice; `2 * ++y` increments y once while `++y + ++y`
  // increments it twice and returns a different number. `pure` is precisely the
  // "may be repeated or shared without changing anything" mark, so it gates the
  // rewrite.
  DAGNode* strengthReduce(DAGNode* node) {
    if (node->op != '*') return node;
    DAGNode* lhs = node->operands[0];
    DAGNode* rhs = node->operands[1];
    // x * 2 → x + x
    if (isConst(rhs, 2) && lhs->pure) return module.createBinaryOp('+', lhs, lhs);
    if (isConst(lhs, 2) && rhs->pure) return module.createBinaryOp('+', rhs, rhs);
    return node;
  }

  // If `n` represents the negation of some expression, return the inner expr.
  DAGNode* negInner(DAGNode* n) {
    if (!n) return nullptr;
    if (n->kind == NodeKind::UnaryOp && n->op == '-' && n->operands.size() == 1) {
      return n->operands[0];
    }
    if (n->kind == NodeKind::BinaryOp && n->op == '*' && n->operands.size() == 2) {
      if (isConst(n->operands[0], -1)) return n->operands[1];
      if (isConst(n->operands[1], -1)) return n->operands[0];
    }
    return nullptr;
  }

  // (-a) * (-a) → a * a  (even powers are sign-independent)
  DAGNode* evenPower(DAGNode* node) {
    if (node->kind != NodeKind::BinaryOp || node->op != '*') return node;
    if (node->operands.size() != 2) return node;
    DAGNode* li = negInner(node->operands[0]);
    DAGNode* ri = negInner(node->operands[1]);
    if (li && ri && li->id == ri->id) {
      return module.createBinaryOp('*', li, li);
    }
    return node;
  }

  void flattenMul(DAGNode* n, std::vector<DAGNode*>& factors) {
    if (n->kind == NodeKind::BinaryOp && n->op == '*' && n->operands.size() == 2) {
      flattenMul(n->operands[0], factors);
      flattenMul(n->operands[1], factors);
    } else {
      factors.push_back(n);
    }
  }

  // Normalize a multiplication chain: fold all constant factors into a single
  // leading constant and rebuild left-associatively. Makes `uc*uc*0.5*9.0`
  // and `uc*uc*4.5` share one DAG node.
  DAGNode* normalizeProduct(DAGNode* node) {
    if (node->kind != NodeKind::BinaryOp || node->op != '*') return node;
    if (node->operands.size() != 2) return node;

    std::vector<DAGNode*> factors;
    flattenMul(node, factors);

    double constProd = 1.0;
    bool hasConst = false;
    std::vector<DAGNode*> nonConst;
    for (auto* f : factors) {
      // A symbolic constant (a declared accessor kept for emission) must not be
      // folded into a numeric product: keep it so the symbol is preserved.
      if (f->kind == NodeKind::Constant && f->symbol.empty()) {
        constProd *= f->constVal;
        hasConst = true;
      } else {
        nonConst.push_back(f);
      }
    }

    // All factors are constant: always fold, this is exact.
    if (nonConst.empty()) {
      return module.createConst(constProd, numText(constProd));
    }

    // Rebuilding the chain left-associatively regroups the multiplications
    // (`a * (b * c)` becomes `(a * b) * c`), which changes floating-point
    // rounding -- so it needs the same opt-in as ReassociatePass.
    if (!fpReassoc_) return node;

    DAGNode* acc = nullptr;
    if (hasConst && constProd != 1.0) {
      acc = module.createConst(constProd, numText(constProd));
    }
    for (auto* f : nonConst) {
      acc = acc ? module.createBinaryOp('*', acc, f) : f;
    }
    return acc ? acc : node;
  }

  // Factor negations out of a multiplication so opposite signs collapse onto a
  // shared positive subexpression:
  //   a * (-b) → -(a * b),  (-a) * b → -(a * b),  (-a) * (-b) → a * b
  // Sign moves are exact for IEEE floating point, and let sign-flipped
  // subexpressions share a common factor.
  DAGNode* normalizeSign(DAGNode* node) {
    if (node->kind != NodeKind::BinaryOp || node->op != '*') return node;
    if (node->operands.size() != 2) return node;
    DAGNode* lhs = node->operands[0];
    DAGNode* rhs = node->operands[1];

    DAGNode* lhsInner = negInner(lhs);
    DAGNode* rhsInner = negInner(rhs);
    if (!lhsInner && !rhsInner) return node;

    DAGNode* l = lhsInner ? lhsInner : lhs;
    DAGNode* r = rhsInner ? rhsInner : rhs;
    DAGNode* product = module.createBinaryOp('*', l, r);
    bool oneNegated = (lhsInner != nullptr) != (rhsInner != nullptr);
    return oneNegated ? module.createUnaryOp('-', product) : product;
  }

  DAGNode* applyIdentities(DAGNode* node) {
    // UnaryOp: --a → a
    if (node->kind == NodeKind::UnaryOp && node->op == '-' && !node->operands.empty()) {
      DAGNode* inner = node->operands[0];
      if (inner->kind == NodeKind::UnaryOp && inner->op == '-') {
        simplifications++;
        return inner->operands[0];
      }
      return node;
    }

    if (node->kind != NodeKind::BinaryOp || node->operands.size() != 2)
      return node;

    DAGNode* lhs = node->operands[0];
    DAGNode* rhs = node->operands[1];

    if (node->op == '*') {
      if (isConst(lhs, 1)) { simplifications++; return rhs; }
      if (isConst(rhs, 1)) { simplifications++; return lhs; }
      // x * 0 -> 0 is wrong for x = +-inf / NaN.
      if (unsafeIdentities_ && (isConst(lhs, 0) || isConst(rhs, 0))) {
        simplifications++;
        return module.createConst(0, "0");
      }
    }

    if (node->op == '+') {
      if (isConst(lhs, 0)) { simplifications++; return rhs; }
      if (isConst(rhs, 0)) { simplifications++; return lhs; }
      // a + (-b) → a - b
      if (rhs->kind == NodeKind::UnaryOp && rhs->op == '-') {
        simplifications++;
        return module.createBinaryOp('-', lhs, rhs->operands[0]);
      }
    }

    if (node->op == '-') {
      if (isConst(rhs, 0)) { simplifications++; return lhs; }
      if (isConst(lhs, 0)) { simplifications++; return module.createUnaryOp('-', rhs); }
      // a - a -> 0 is wrong for a = +-inf / NaN.
      if (unsafeIdentities_ && lhs->id == rhs->id) {
        simplifications++;
        return module.createConst(0, "0");
      }
      // a - (-b) → a + b
      if (rhs->kind == NodeKind::UnaryOp && rhs->op == '-') {
        simplifications++;
        return module.createBinaryOp('+', lhs, rhs->operands[0]);
      }
    }

    if (node->op == '/') {
      if (isConst(rhs, 1)) { simplifications++; return lhs; }
      // 0 / a -> 0 and a / a -> 1 are both wrong at a = 0 (NaN), and a / a is
      // also wrong for a = +-inf / NaN.
      if (unsafeIdentities_ && isConst(lhs, 0)) {
        simplifications++;
        return module.createConst(0, "0");
      }
      if (unsafeIdentities_ && lhs->id == rhs->id) {
        simplifications++;
        return module.createConst(1, "1");
      }
    }

    return node;
  }

  // Sort operands of commutative operators — local swap only
  // Only swap if both operands are leaves (variables/constants)
  DAGNode* sortCommutative(DAGNode* node) {
    if (node->op != '+' && node->op != '*') return node;
    if (node->operands.size() != 2) return node;

    DAGNode* lhs = node->operands[0];
    DAGNode* rhs = node->operands[1];

    // Only swap leaf nodes to avoid breaking sharing structure
    if (!isTrivial(lhs) || !isTrivial(rhs)) return node;

    if (isCanonicalBefore(rhs, lhs)) {
      return module.createBinaryOp(node->op, rhs, lhs);
    }

    return node;
  }

  bool isTrivial(DAGNode* n) {
    return n->kind == NodeKind::Constant || n->kind == NodeKind::Variable;
  }

  bool isCanonicalBefore(DAGNode* a, DAGNode* b) {
    int rankA = nodeRank(a);
    int rankB = nodeRank(b);
    if (rankA != rankB) return rankA < rankB;

    if (a->kind == NodeKind::Variable && b->kind == NodeKind::Variable) {
      return a->name <= b->name;
    }
    return a->id <= b->id;
  }

  int nodeRank(DAGNode* n) {
    switch (n->kind) {
      case NodeKind::Constant:
        return 0;
      case NodeKind::Variable:
        return 1;
      default:
        return 2;
    }
  }
};

void AlgebraicSimplifyPass::run(IRModule& module) {
  AlgebraicSimplifyVisitor visitor(module, _commutative, _associative,
                                   _unsafeFpIdentities, _fpReassoc);
  visitor.visitStmt(module.body.get());
}

}  // namespace cse
