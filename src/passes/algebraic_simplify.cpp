#include "algebraic_simplify.h"

#include <vector>

#include "../analysis/purity.h"
#include "../ir/ir_module.h"
#include "../ir/ir_utils.h"
#include "../ir/statement.h"
#include "../ir/stmt_walk.h"

namespace cse {

namespace {

}  // namespace

class AlgebraicSimplifyVisitor {
 public:
  AlgebraicSimplifyVisitor(IRModule& mod, bool commutative, bool associative,
                           bool unsafeFpIdentities, bool fpReassoc,
                           bool assumeFloatOperands)
      : module(mod),
        numeric_(commutative && associative),
        unsafeIdentities_(unsafeFpIdentities),
        fpReassoc_(fpReassoc),
        assumeFloatOperands_(assumeFloatOperands) {}
  IRModule& module;
  // Numeric reordering rules require both commutativity and associativity; the
  // pass exposes the two flags separately for callers but applies them jointly.
  // Gates only the *assumption* tier -- `sortCommutative` and `normalizeProduct`.
  // The exact tier (see `applyRules`) runs regardless of this flag.
  bool numeric_;
  // `x/x -> 1`, `0/x -> 0`, `x-x -> 0` and `x*0 -> 0` need more than a field
  // assumption -- they also require the operands to avoid 0 / +-inf / NaN -- so
  // they carry their own opt-in.
  bool unsafeIdentities_;
  // Regrouping a multiplication chain changes floating-point rounding.
  bool fpReassoc_;
  // Treat an operand of unprovable numeric kind as floating, for the purpose of
  // choosing the kind of the constant a zero identity substitutes.
  bool assumeFloatOperands_;

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
      if (child != node->operands[0]) node = rebuildWithOperands(module, node, {child});
    } else if (node->kind == NodeKind::BinaryOp && node->operands.size() == 2) {
      DAGNode* lhs = simplify(node->operands[0]);
      DAGNode* rhs = simplify(node->operands[1]);
      if (lhs != node->operands[0] || rhs != node->operands[1]) {
        node = module.createBinaryOp(node->op, lhs, rhs);
      }
    }

    // Apply the local rewrite rules until none fires. A rule can expose work
    // for a later one: sortCommutative rewrites `x * 2` to `2 * x` (the constant
    // sorts first), which is exactly the shape strengthReduce turns into
    // `x + x`. Returning after the first change deferred that rewrite to a
    // second run of the pass, so the pass -- and the tool -- was not idempotent:
    // `x * 2.0` came out as `2.0 * x` on one run and `x + x` on the next. The
    // bound is a safety net; the rules are designed to converge in a couple of
    // steps.
    for (int guard = 0; guard < 16; ++guard) {
      DAGNode* next = applyRules(node);
      if (next == node) break;
      node = next;
    }
    return node;
  }

 private:
  // Apply the first rule that fires to a single node and return its result (the
  // node itself when nothing applies). Children are assumed already simplified.
  //
  // The rules are split into two tiers. The *exact* tier is always on: those
  // rewrites are bit-identical for IEEE-754 (including +-0.0, +-inf, NaN and
  // subnormals), so even the conservative profile keeps them -- they need no
  // commutativity/associativity premise and change no special value. The
  // *assumption* tier needs an algebraic premise or changes a special value and
  // stays behind its switch: `sortCommutative` and `normalizeProduct` behind
  // `numeric_` (unsafe for overloaded operators / rounding), and the
  // special-value identities inside `applyIdentities` behind
  // `unsafeIdentities_` (they check it themselves).
  DAGNode* applyRules(DAGNode* node) {
    if (node->kind == NodeKind::UnaryOp && !node->operands.empty()) {
      DAGNode* result = applyIdentities(node);  // --a -> a is exact
      if (result != node) return result;
    }
    if (node->kind != NodeKind::BinaryOp || node->operands.size() != 2) {
      return node;
    }
    // Exact tier: no numeric assumption required.
    DAGNode* result = applyIdentities(node);
    if (result != node) return result;
    if (numeric_) {
      result = sortCommutative(node);
      if (result != node) return result;
    }
    result = strengthReduce(node);
    if (result != node) return result;
    result = evenPower(node);
    if (result != node) return result;
    if (numeric_) {
      result = normalizeProduct(node);
      if (result != node) return result;
    }
    result = normalizeSign(node);
    if (result != node) return result;
    return node;
  }

  // Match a plain numeric literal. A *symbolic* constant (`symbol` non-empty --
  // a declared accessor such as `ns::w<T>(1)` kept verbatim for emission, see
  // dag_node.h) carries a real numeric value but must never be matched here:
  // folding `x * sym1` to `x` would drop the symbol from the emitted code.
  // `normalizeProduct` guards its constant folding the same way.
  bool isConst(DAGNode* n, double val) {
    return n->kind == NodeKind::Constant && n->symbol.empty() &&
           n->constVal == val;
  }

  // Is dropping constant `c` from a binary operation kind-preserving? The
  // result keeps `other`'s kind, while the source had join(c, other). They
  // agree unless the constant was the only floating operand -- `i * 2.0` is a
  // double multiply, `i` is not.
  bool droppingKeepsKind(const DAGNode* c, const DAGNode* other) {
    if (c->numKind == NumericKind::Int) return true;
    if (other->numKind == NumericKind::Float) return true;
    // A floating constant keeps the kind only when the other operand's kind is
    // merely *unprovable* (a load), which is what `assumeFloatOperands_`
    // licenses: `u_value[0] + 0.0` is a double sum whatever the load's type, so
    // dropping the addend keeps the kind. It must NOT apply to a proven integer:
    // `0.0 + 2` is 2.0, and returning the integer `2` would turn a later `/ 4`
    // into integer division.
    return assumeFloatOperands_ && c->numKind == NumericKind::Float &&
           other->numKind == NumericKind::Unknown;
  }

  // A constant that replaces the whole expression must carry the expression's
  // kind: `x / x` is 1.0 for a double x, and emitting the integer 1 turns a
  // later `/ 2` into integer division. Returns nullptr when the kind is
  // unknown -- then the rewrite must not fire.
  DAGNode* constOfKind(double val, NumericKind kind) {
    if (kind == NumericKind::Unknown) return nullptr;
    return module.createConstTyped(val, kind == NumericKind::Float, "");
  }

  // The numeric kind of a zero identity's result, or Unknown when the rewrite
  // must be skipped.
  //
  // joinNumericKind is deliberately conservative: a load, a call or a
  // template-dependent member has no provable kind, so it reports Unknown and
  // the identity would never fire on `0.0 * a[i]` or a lattice `0 * cell[i]`.
  // Two narrower answers are sound:
  //
  //   * a floating *literal* operand fixes the result type by the usual
  //     arithmetic conversions (`double * int` is double), whatever the other
  //     operand's type is -- no assumption needed;
  //   * `assumeFloatOperands_` says an unprovable operand is floating, which is
  //     how a project whose every field is floating recovers the fold for an
  //     *integer* zero literal like the lattice `0`.
  //
  // A proven Int (both operands integral) always wins: `(0 * k + 2) / 4` is 0
  // for an int k, and must never be folded to a floating 0.0.
  NumericKind zeroIdentityKind(const DAGNode* node, const DAGNode* lhs,
                               const DAGNode* rhs) {
    if (node->numKind != NumericKind::Unknown) return node->numKind;
    const bool lhsFloatLit =
        lhs->kind == NodeKind::Constant && lhs->numKind == NumericKind::Float;
    const bool rhsFloatLit =
        rhs->kind == NodeKind::Constant && rhs->numKind == NumericKind::Float;
    if (lhsFloatLit || rhsFloatLit) return NumericKind::Float;
    if (assumeFloatOperands_) return NumericKind::Float;
    return NumericKind::Unknown;
  }

  // x * 2 -> x + x. Exact in IEEE-754 (doubling only changes the exponent), and
  // overflow behaves the same way, so for *values* it needs no opt-in.
  //
  // It does duplicate the operand, though, so it may only be applied to an
  // operand that can be evaluated twice. `2 * f()` calls f once while
  // `f() + f()` calls it twice; `2 * ++y` increments y once while `++y + ++y`
  // increments it twice and returns a different number.
  //
  // The gate is the *deep* `isReferentiallyTransparent`, not the operand's own
  // `pure` flag: `pure` is not propagated to the parent, so `2 * (f() + 1.0)` is
  // a Mul whose operand is a BinaryOp with `pure == true` even though the call
  // inside it is impure. A shallow check duplicated the call.
  DAGNode* strengthReduce(DAGNode* node) {
    if (node->op != OpCode::Mul) return node;
    DAGNode* lhs = node->operands[0];
    DAGNode* rhs = node->operands[1];
    // x * 2 → x + x. The constant must be dropped, so it may only be an
    // integer 2: `i * 2.0` is a double multiply and `i + i` is not.
    if (isConst(rhs, 2) && droppingKeepsKind(rhs, lhs) &&
        isReferentiallyTransparent(lhs))
      return module.createBinaryOp(OpCode::Add, lhs, lhs);
    if (isConst(lhs, 2) && droppingKeepsKind(lhs, rhs) &&
        isReferentiallyTransparent(rhs))
      return module.createBinaryOp(OpCode::Add, rhs, rhs);
    return node;
  }

  // If `n` represents the negation of some expression, return the inner expr.
  DAGNode* negInner(DAGNode* n) {
    if (!n) return nullptr;
    if (n->kind == NodeKind::UnaryOp && n->op == OpCode::Neg && !isIncDec(n) &&
        n->operands.size() == 1) {
      return n->operands[0];
    }
    if (n->kind == NodeKind::BinaryOp && n->op == OpCode::Mul && n->operands.size() == 2) {
      // Peeling the factor drops it, so a floating -1.0 may only be peeled when
      // the other factor is floating too: `(a * -1.0) * b` is a double product,
      // `-(a * b)` is not.
      if (isConst(n->operands[0], -1) && droppingKeepsKind(n->operands[0], n->operands[1]))
        return n->operands[1];
      if (isConst(n->operands[1], -1) && droppingKeepsKind(n->operands[1], n->operands[0]))
        return n->operands[0];
    }
    return nullptr;
  }

  // (-a) * (-a) → a * a  (even powers are sign-independent)
  DAGNode* evenPower(DAGNode* node) {
    if (node->kind != NodeKind::BinaryOp || node->op != OpCode::Mul) return node;
    if (node->operands.size() != 2) return node;
    DAGNode* li = negInner(node->operands[0]);
    DAGNode* ri = negInner(node->operands[1]);
    if (li && ri && li->id == ri->id) {
      return module.createBinaryOp(OpCode::Mul, li, li);
    }
    return node;
  }

  void flattenMul(DAGNode* n, std::vector<DAGNode*>& factors) {
    if (n->kind == NodeKind::BinaryOp && n->op == OpCode::Mul && n->operands.size() == 2) {
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
    if (node->kind != NodeKind::BinaryOp || node->op != OpCode::Mul) return node;
    if (node->operands.size() != 2) return node;

    std::vector<DAGNode*> factors;
    flattenMul(node, factors);

    double constProd = 1.0;
    bool hasConst = false;
    bool constFloat = false;  // is the constant product floating?
    std::vector<DAGNode*> nonConst;
    for (auto* f : factors) {
      // A symbolic constant (a declared accessor kept for emission) must not be
      // folded into a numeric product: keep it so the symbol is preserved.
      if (f->kind == NodeKind::Constant && f->symbol.empty()) {
        constProd *= f->constVal;
        hasConst = true;
        constFloat = constFloat || f->isFloat;
      } else {
        nonConst.push_back(f);
      }
    }

    // All factors are constant: always fold, this is exact.
    if (nonConst.empty()) {
      // ... unless the product overflows to inf/NaN. There is no C++ literal
      // spelling for those, so emitting one produces `return inf;` -- a file
      // that no longer compiles. Leave the chain as the source wrote it: it
      // still evaluates to inf at run time, which is what the program meant.
      // (foldConst in ir_utils.h has the same guard for a single binary op;
      // a chain of constants only reaches this path through the normalizer.)
      if (!std::isfinite(constProd)) return node;
      return module.createConstTyped(constProd, constFloat, "");
    }

    // Rebuilding the chain left-associatively regroups the multiplications
    // (`a * (b * c)` becomes `(a * b) * c`), which changes floating-point
    // rounding -- so it needs the same opt-in as ReassociatePass.
    if (!fpReassoc_) return node;

    // Dropping a floating 1.0 is only safe when the remaining factors are
    // floating too: `1.0 * i` is a double expression and `i` is not. With an
    // unknown factor the source is still floating, so the constant has to stay.
    NumericKind restKind = NumericKind::Unknown;
    bool first = true;
    for (auto* f : nonConst) {
      restKind = first ? f->numKind : joinNumericKind(restKind, f->numKind);
      first = false;
    }
    const bool keepConst =
        hasConst &&
        (constProd != 1.0 || (constFloat && restKind != NumericKind::Float));

    DAGNode* acc = nullptr;
    if (keepConst) {
      acc = module.createConstTyped(constProd, constFloat, "");
    }
    for (auto* f : nonConst) {
      acc = acc ? module.createBinaryOp(OpCode::Mul, acc, f) : f;
    }
    return acc ? acc : node;
  }

  // Factor negations out of a multiplication so opposite signs collapse onto a
  // shared positive subexpression:
  //   a * (-b) → -(a * b),  (-a) * b → -(a * b),  (-a) * (-b) → a * b
  // Sign moves are exact for IEEE floating point, and let sign-flipped
  // subexpressions share a common factor.
  //
  // Exactness, measured over +-0.0 / +-inf / subnormals / NaN: every finite and
  // infinite product is bit-identical to the source. The one difference is the
  // *sign bit of a NaN* produced by 0 * inf (or inf * 0), where the source has
  // the IEEE "invalid" form (sign bit set) and the rewritten form propagates the
  // negated operand's sign instead. A NaN payload/sign is not an observable
  // value under IEEE-754 (isnan() and the comparison operators cannot see it),
  // so the exact tier keeps the rule unconditionally -- moving it behind
  // `numeric_` would cost `-s` the equilibrium kernel's opposite-direction
  // sharing (93 -> 84 flops). `verify_exact_identities.cpp` pins both halves of
  // that trade: the value is exact for every non-NaN operand, and a NaN stays a
  // NaN.
  DAGNode* normalizeSign(DAGNode* node) {
    if (node->kind != NodeKind::BinaryOp || node->op != OpCode::Mul) return node;
    if (node->operands.size() != 2) return node;
    DAGNode* lhs = node->operands[0];
    DAGNode* rhs = node->operands[1];

    DAGNode* lhsInner = negInner(lhs);
    DAGNode* rhsInner = negInner(rhs);
    if (!lhsInner && !rhsInner) return node;

    DAGNode* l = lhsInner ? lhsInner : lhs;
    DAGNode* r = rhsInner ? rhsInner : rhs;
    DAGNode* product = module.createBinaryOp(OpCode::Mul, l, r);
    bool oneNegated = (lhsInner != nullptr) != (rhsInner != nullptr);
    return oneNegated ? module.createUnaryOp(OpCode::Neg, product) : product;
  }

  DAGNode* applyIdentities(DAGNode* node) {
    // UnaryOp: --a → a. The inner operator must be a genuine unary minus, not a
    // `--x` prefix decrement (also a UnaryOp with op '-'); peeling it off would
    // delete the decrement (`-(- -x)` would lose one of the two).
    if (node->kind == NodeKind::UnaryOp && node->op == OpCode::Neg && !isIncDec(node) &&
        !node->operands.empty()) {
      DAGNode* inner = node->operands[0];
      if (inner->kind == NodeKind::UnaryOp && inner->op == OpCode::Neg && !isIncDec(inner)) {
        return inner->operands[0];
      }
      return node;
    }

    if (node->kind != NodeKind::BinaryOp || node->operands.size() != 2)
      return node;

    DAGNode* lhs = node->operands[0];
    DAGNode* rhs = node->operands[1];

    if (node->op == OpCode::Mul) {
      if (isConst(lhs, 1) && droppingKeepsKind(lhs, rhs)) { return rhs; }
      if (isConst(rhs, 1) && droppingKeepsKind(rhs, lhs)) { return lhs; }
      // x * 0 -> 0 is wrong for x = +-inf / NaN. It must also not discard an
      // observable effect: `f() * 0` may not drop the call, `(++y) * 0` may not
      // drop the increment. The zero keeps the expression's kind.
      if (unsafeIdentities_ && !hasObservableEffect(node) &&
          (isConst(lhs, 0) || isConst(rhs, 0))) {
        if (DAGNode* zero =
                constOfKind(0, zeroIdentityKind(node, lhs, rhs)))
          return zero;
        return node;
      }
    }

    if (node->op == OpCode::Add) {
      // `x + 0 -> x` / `0 + x -> x` hold for every finite x except x = -0.0,
      // where the sum is +0.0 rather than -0.0. That is the same signed-zero
      // licence the four special-value identities below carry, so they share
      // the `allowUnsafeFpIdentities` opt-in rather than being dropped: the
      // algebraic form is far too valuable to lose wholesale. The operand is
      // retained, so neither rewrite can drop a side effect.
      if (unsafeIdentities_) {
        if (isConst(lhs, 0) && droppingKeepsKind(lhs, rhs)) { return rhs; }
        if (isConst(rhs, 0) && droppingKeepsKind(rhs, lhs)) { return lhs; }
      }
      // a + (-b) → a - b
      if (rhs->kind == NodeKind::UnaryOp && rhs->op == OpCode::Neg && !isIncDec(rhs)) {
        return module.createBinaryOp(OpCode::Sub, lhs, rhs->operands[0]);
      }
    }

    if (node->op == OpCode::Sub) {
      // `x - 0 -> x` is exact for every x (including both signed zeros), so it
      // needs no opt-in.
      if (isConst(rhs, 0) && droppingKeepsKind(rhs, lhs)) { return lhs; }
      // `0 - x -> -x` holds except for x = +0.0, where the difference is +0.0
      // while -x is -0.0 -- the same signed-zero licence as `x + 0`. The
      // operand is kept (negated), so no effect is dropped.
      if (unsafeIdentities_ && isConst(lhs, 0) && droppingKeepsKind(lhs, rhs)) {
        return module.createUnaryOp(OpCode::Neg, rhs);
      }
      // a - a -> 0 is wrong for a = +-inf / NaN, and must not drop effects.
      if (unsafeIdentities_ && !hasObservableEffect(node) && lhs->id == rhs->id) {
        if (DAGNode* zero =
                constOfKind(0, zeroIdentityKind(node, lhs, rhs)))
          return zero;
        return node;
      }
      // a - (-b) → a + b
      if (rhs->kind == NodeKind::UnaryOp && rhs->op == OpCode::Neg && !isIncDec(rhs)) {
        return module.createBinaryOp(OpCode::Add, lhs, rhs->operands[0]);
      }
    }

    if (node->op == OpCode::Div) {
      if (isConst(rhs, 1) && droppingKeepsKind(rhs, lhs)) { return lhs; }
      // 0 / a -> 0 and a / a -> 1 are both wrong at a = 0 (NaN), and a / a is
      // also wrong for a = +-inf / NaN; neither may drop a side effect. The
      // replacement keeps the expression's kind, so `x / x` on a double yields
      // 1.0 -- an integer 1 would turn a later `/ 2` into integer division.
      if (unsafeIdentities_ && !hasObservableEffect(node) && isConst(lhs, 0)) {
        if (DAGNode* zero =
                constOfKind(0, zeroIdentityKind(node, lhs, rhs)))
          return zero;
        return node;
      }
      if (unsafeIdentities_ && !hasObservableEffect(node) && lhs->id == rhs->id) {
        if (DAGNode* one = constOfKind(1, zeroIdentityKind(node, lhs, rhs)))
          return one;
        return node;
      }
    }

    return node;
  }

  // Sort operands of commutative operators — local swap only
  // Only swap if both operands are leaves (variables/constants)
  DAGNode* sortCommutative(DAGNode* node) {
    if (node->op != OpCode::Add && node->op != OpCode::Mul) return node;
    if (node->operands.size() != 2) return node;

    DAGNode* lhs = node->operands[0];
    DAGNode* rhs = node->operands[1];

    // Only swap leaf nodes to avoid breaking sharing structure
    if (!isLeafValue(lhs) || !isLeafValue(rhs)) return node;

    if (isCanonicalBefore(rhs, lhs)) {
      return module.createBinaryOp(node->op, rhs, lhs);
    }

    return node;
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
                                   _unsafeFpIdentities, _fpReassoc,
                                   _assumeFloatOperands);
  visitor.visitStmt(module.body.get());
}

}  // namespace cse
