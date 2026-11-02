// Did the zero identity actually fire, and only where it was licensed?
//
// This is the half `verify_zero_product_kind.cpp` cannot see. A rewrite that
// never fires leaves the value correct, so every value check passes on a build
// that silently lost the fold -- which is exactly how the FreeLB kernels lost
// half their CSE when the fold started requiring a provable numeric kind: the
// generated code stayed *correct*, it just kept every `0 * cell[i]`.
//
// The oracle is a node count over the optimized IR, driven through the library so
// both profiles are reachable:
//
//   * with `assumeFloatOperands` (the FreeLB profile) both `0.0 * c[0]` and
//     `0 * c[0]` collapse, so the multiply disappears;
//   * without it the *integer*-literal case is skipped and the multiply stays --
//     the asymmetry is precisely what the license buys.
//
// Build: g++ -std=c++17 -O2 -Isrc tests/verify/verify_zero_product_activity.cpp \
//            bin/libcse.a
#include <cstdio>
#include <string>
#include <unordered_set>

#include "frontend/cse_config.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"
#include "ir/stmt_walk.h"
#include "passes/pass_manager.h"

namespace {

int failures = 0;

// Count the multiply nodes surviving the default pipeline. A folded zero product
// contributes none; an unfolded one contributes exactly one. Counting `Mul`
// rather than every BinaryOp keeps the oracle independent of how much of the
// surrounding constant arithmetic ConstantFold happens to collapse.
int countMuls(const std::string& src, bool assumeFloatOperands) {
  cse::CSEConfig cfg;
  // The identities that rewrite in the first place are the unsafe tier; the
  // question here is only which *kind* they may substitute.
  cfg.assumeNumericCommutative = true;
  cfg.assumeNumericAssociative = true;
  cfg.allowFpReassoc = true;
  cfg.allowUnsafeFpIdentities = true;
  cfg.assumeFloatOperands = assumeFloatOperands;
  try {
    cse::Lexer lexer(src, cfg);
    auto tokens = lexer.tokenize();
    cse::Parser parser(tokens, cfg);
    auto res = parser.parseAll();
    if (res.functions.empty()) return -1;
    cse::IRModule mod;
    cse::IRBuilder builder(&mod, cfg);
    builder.buildFunction(*res.functions.front());
    auto pm = cse::PassManager::createDefault(cfg, false, nullptr, nullptr);
    pm.runAll(mod, false);
    // `forEachExprDeep` visits statement-owned *slots*, not the DAG below them:
    // a `return (0 * c[0] + 2) / 4;` hands over one slot holding the division.
    // Walk each slot's subtree as well, and keep node ids in a set so a shared
    // node is counted once rather than once per referencing slot.
    std::unordered_set<uint32_t> seen;
    int n = 0;
    auto walk = [&](const cse::DAGNode* e, auto&& self) -> void {
      if (!e || !seen.insert(e->id).second) return;
      if (e->kind == cse::NodeKind::BinaryOp && e->op == cse::OpCode::Mul) ++n;
      for (const cse::DAGNode* op : e->operands) self(op, self);
    };
    cse::forEachExprDeep(mod.body.get(), [&](cse::DAGNode*& e) {
      walk(e, walk);
    });
    return n;
  } catch (const std::exception&) {
    return -1;
  }
}

void expect(const char* what, int got, int want) {
  const bool ok = got == want;
  if (!ok) ++failures;
  std::printf("%-34s got=%d want=%d  %s\n", what, got, want, ok ? "OK" : "FAIL");
}

}  // namespace

int main() {
  // `0.0 * c[0]` needs no assumption: a floating literal fixes the product's
  // type, so the multiply must be gone in both profiles.
  const std::string floatLit =
      "double f(double* c) { return (0.0 * c[0] + 2.0) / 4.0; }";
  expect("float literal folds (licensed)", countMuls(floatLit, true), 0);
  expect("float literal folds (conservative)", countMuls(floatLit, false), 0);

  // `0 * c[0]`: the zero coefficient FreeLB's lattice tables produce. Its kind
  // is unprovable (the load) *and* the literal is, so only the license can fold
  // it. The licensed/conservative split is the regression: both used to keep the
  // multiply.
  const std::string intLit = "double f(double* c) { return (0 * c[0] + 2) / 4; }";
  expect("int literal folds (licensed)", countMuls(intLit, true), 0);
  expect("int literal kept (conservative)", countMuls(intLit, false), 1);

  // A proven integer must be left alone even under the license: folding
  // `0 * k` is fine, but the surrounding integer arithmetic must not gain a
  // floating constant. The multiply folds in both profiles; the *kind* is what
  // `verify_zero_product_kind.cpp` pins by value.
  const std::string intProven = "int f(int k) { return (0 * k + 2) / 4; }";
  expect("int proven folds (licensed)", countMuls(intProven, true), 0);
  expect("int proven folds (conservative)", countMuls(intProven, false), 0);

  if (failures == 0) {
    std::printf("\nALL ZERO-PRODUCT-ACTIVITY CHECKS PASSED\n");
  } else {
    std::printf("\n%d ZERO-PRODUCT-ACTIVITY CHECK(S) FAILED\n", failures);
  }
  return failures == 0 ? 0 : 1;
}
