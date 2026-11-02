// Library-level checks on the CSEConfig contracts that the CLI cannot isolate:
// `-s` turns every aggressive rule off and the FreeLB profile turns them all on,
// so nothing in between is reachable from `bin/cse`. The flags added for the
// unsafe floating-point identities and for multiplication regrouping are only
// observable through the library API, which is what this verifier drives.
//
// Build: g++ -std=c++17 -O2 -Isrc tests/verify/verify_config.cpp bin/libcse.a
#include <cstdio>
#include <stdexcept>
#include <string>

#include "backend/codegen.h"
#include "frontend/cse_config.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"
#include "ir/statement.h"
#include "passes/algebraic_simplify.h"
#include "passes/pass_manager.h"

namespace {

std::string optimize(const std::string& src, const cse::CSEConfig& cfg) {
  cse::Lexer lexer(src, cfg);
  std::vector<cse::Token> tokens = lexer.tokenize();
  cse::Parser parser(tokens, cfg);
  cse::Parser::ParseResult parsed = parser.parseAll();
  std::string out;
  for (auto& f : parsed.functions) {
    cse::IRModule mod;
    cse::IRBuilder builder(&mod, cfg);
    builder.buildFunction(*f);
    auto pm = cse::PassManager::createDefault(cfg, false, nullptr, nullptr);
    pm.runAll(mod, false);
    cse::CodeGen cg;
    out += cg.generate(mod, {}, {}, f->templateParams);
  }
  return out;
}

bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// The value a single-statement function returns, when its body is exactly
// `return <k>;`.
//
// Compared numerically rather than textually on purpose. A constant node carries
// one spelling for every context it is reached from -- its text is not part of
// its identity -- so the zero an identity folds to is emitted as `0` or as the
// `0.0` the source wrote, depending on which came first. What the contract
// promises is the value, not the spelling, and asserting the spelling is how a
// test stops being able to see a real change.
bool returnsValue(const std::string& out, double want) {
  const size_t p = out.find("return ");
  if (p == std::string::npos) return false;
  const size_t e = out.find(';', p);
  if (e == std::string::npos) return false;
  const std::string expr = out.substr(p + 7, e - (p + 7));
  try {
    // std::stod parses a prefix and ignores the rest, so `0.0 * x` would read as
    // the value 0. Require the whole expression to be consumed.
    size_t used = 0;
    const double v = std::stod(expr, &used);
    return v == want &&
           expr.find_first_not_of(" \t\r\n", used) == std::string::npos;
  } catch (const std::exception&) {
    return false;
  }
}

// A value collapsed to 0 or 1 means the identity fired.
bool collapsed(const std::string& out) {
  return returnsValue(out, 0.0) || returnsValue(out, 1.0);
}

int failures = 0;

void expect(const char* what, bool ok, const std::string& detail) {
  if (!ok) failures++;
  std::printf("%-46s %s\n", what, ok ? "OK" : "FAIL");
  if (!ok) std::printf("      generated: %s", detail.c_str());
}

// comm+assoc on, but neither of the FP-specific opt-ins.
cse::CSEConfig numericOnly() {
  cse::CSEConfig cfg;
  cfg.assumeNumericCommutative = true;
  cfg.assumeNumericAssociative = true;
  return cfg;
}

}  // namespace

int main() {
  const std::string selfDiv = "double f(double x) { return x / x; }";
  const std::string selfSub = "double f(double x) { return x - x; }";
  const std::string timesZero = "double f(double x) { return x * 0.0; }";
  const std::string zeroOver = "double f(double x) { return 0.0 / x; }";
  const std::string addZero = "double f(double x) { return x + 0.0; }";
  const std::string zeroMinus = "double f(double x) { return 0.0 - x; }";
  const std::string mulChain = "double f(double a, double b, double c) {"
                               " return a * (b * c); }";
  // Exact-tier shapes (bit-identical rewrites; always on).
  const std::string subZero = "double f(double x) { return x - 0.0; }";
  const std::string divOne = "double f(double x) { return x / 1.0; }";
  const std::string mulOne = "double f(double x) { return x * 1.0; }";
  const std::string doubleNeg = "double f(double x) { return -(-x); }";
  const std::string addNeg = "double f(double a, double b) { return a + (-b); }";
  const std::string twice = "double f(double x) { return x * 2.0; }";

  // 1. A bare CSEConfig keeps the *licensed* identities: those that need an
  //    operand to avoid 0 / +-inf / NaN, or that flip a signed zero. (The
  //    exact tier, checked in 1b, does fire -- "collapses nothing" is no longer
  //    true of a bare config.)
  cse::CSEConfig bare;
  expect("bare CSEConfig: x / x kept", !collapsed(optimize(selfDiv, bare)),
         optimize(selfDiv, bare));
  expect("bare CSEConfig: x * 0 kept", !collapsed(optimize(timesZero, bare)),
         optimize(timesZero, bare));
  expect("bare CSEConfig: x + 0 kept", has(optimize(addZero, bare), "0.0"),
         optimize(addZero, bare));
  expect("bare CSEConfig: 0 - x kept", has(optimize(zeroMinus, bare), "0.0"),
         optimize(zeroMinus, bare));

  // 1b. The exact tier is always on: a bare CSEConfig still applies the
  //     bit-exact rewrites, which need no commutativity/associativity premise
  //     and change no special value (so `-s` keeps them too).
  expect("bare CSEConfig: x - 0 -> x",
         has(optimize(subZero, bare), "return x;"), optimize(subZero, bare));
  expect("bare CSEConfig: x / 1 -> x",
         has(optimize(divOne, bare), "return x;"), optimize(divOne, bare));
  expect("bare CSEConfig: x * 1 -> x",
         has(optimize(mulOne, bare), "return x;"), optimize(mulOne, bare));
  expect("bare CSEConfig: -(-x) -> x",
         has(optimize(doubleNeg, bare), "return x;"), optimize(doubleNeg, bare));
  expect("bare CSEConfig: a + (-b) -> a - b",
         has(optimize(addNeg, bare), "return a - b;"), optimize(addNeg, bare));
  expect("bare CSEConfig: x * 2 -> x + x",
         has(optimize(twice, bare), "return x + x;"), optimize(twice, bare));

  // 2. Commutativity/associativity alone must NOT collapse them either: these
  //    identities need the operands to avoid 0 / inf / NaN as well.
  cse::CSEConfig n = numericOnly();
  expect("comm+assoc only: x / x kept", !collapsed(optimize(selfDiv, n)),
         optimize(selfDiv, n));
  expect("comm+assoc only: x - x kept", !collapsed(optimize(selfSub, n)),
         optimize(selfSub, n));
  expect("comm+assoc only: x * 0 kept", !collapsed(optimize(timesZero, n)),
         optimize(timesZero, n));
  expect("comm+assoc only: 0 / x kept", !collapsed(optimize(zeroOver, n)),
         optimize(zeroOver, n));
  expect("comm+assoc only: x + 0 kept", has(optimize(addZero, n), "0.0"),
         optimize(addZero, n));
  expect("comm+assoc only: 0 - x kept", has(optimize(zeroMinus, n), "0.0"),
         optimize(zeroMinus, n));

  // 3. allowUnsafeFpIdentities turns exactly those six on. The additive forms
  //    collapse to the operand itself (or its negation), not to a constant, so
  //    they are checked by shape rather than by returnsValue.
  cse::CSEConfig u = numericOnly();
  u.allowUnsafeFpIdentities = true;
  expect("unsafe identities: x / x -> 1", returnsValue(optimize(selfDiv, u), 1.0),
         optimize(selfDiv, u));
  expect("unsafe identities: x - x -> 0", returnsValue(optimize(selfSub, u), 0.0),
         optimize(selfSub, u));
  expect("unsafe identities: x * 0 -> 0",
         returnsValue(optimize(timesZero, u), 0.0), optimize(timesZero, u));
  expect("unsafe identities: 0 / x -> 0", returnsValue(optimize(zeroOver, u), 0.0),
         optimize(zeroOver, u));
  expect("unsafe identities: x + 0 -> x",
         has(optimize(addZero, u), "return x;"), optimize(addZero, u));
  expect("unsafe identities: 0 - x -> -x",
         has(optimize(zeroMinus, u), "return -x;"), optimize(zeroMinus, u));

  // 4. Regrouping a multiplication chain changes FP rounding, so it needs
  //    allowFpReassoc -- commutativity/associativity alone must not do it.
  expect("comm+assoc only: a * (b * c) kept",
         has(optimize(mulChain, n), "(b * c)"), optimize(mulChain, n));
  cse::CSEConfig r = numericOnly();
  r.allowFpReassoc = true;
  expect("allowFpReassoc: a * (b * c) regrouped",
         !has(optimize(mulChain, r), "(b * c)"), optimize(mulChain, r));

  // 5. `-0.0` and `+0.0` are different constants: they compare equal under
  //    `!=`, but they differ as values (1/-0.0 is -inf, 1/0.0 is +inf) and as
  //    bit patterns. Identity must compare the raw bits, exactly as the hash
  //    does; a `!=` here would silently re-merge them.
  {
    cse::IRModule m;
    cse::DAGNode* neg = m.createConst(-0.0, "-0.0");
    cse::DAGNode* pos = m.createConst(0.0, "0.0");
    expect("sameContentAs: -0.0 and +0.0 differ", !neg->sameContentAs(*pos), "");
  }

  // 6. The CleanupPass chained-additive merge drops the accumulator's leading
  //    zero (`y = 0.0; y = y + a; y = y + a` -> `y = a + a`), so it is a
  //    signed-zero identity and must fire only when allowUnsafeFpIdentities is
  //    on. Off, the chain (and its `0.0`) must survive.
  const std::string chain = "double f(double a) {"
                            " double y = 0.0; y = y + a; y = y + a; return y; }";
  expect("bare CSEConfig: cleanup chain kept", has(optimize(chain, bare), "0.0"),
         optimize(chain, bare));
  expect("comm+assoc only: cleanup chain kept", has(optimize(chain, n), "0.0"),
         optimize(chain, n));
  expect("unsafe identities: cleanup chain merged",
         !has(optimize(chain, u), "0.0"), optimize(chain, u));

  // 7. A *symbolic* constant (a declared accessor kept verbatim for emission,
  //    e.g. `ns::w<T>(1)`) carries a real numeric value but is not a numeric
  //    literal: `x * sym1` must NOT fold to `x`, or the symbol would vanish from
  //    the emitted code. `isConst` guards on `symbol.empty()`, mirroring
  //    normalizeProduct's own constant-fold guard.
  {
    cse::IRModule m;
    cse::DAGNode* x = m.getVar("x");
    cse::DAGNode* symOne = m.createSymbolicConst(1.0, "sym_one");
    auto ret = std::make_unique<cse::ReturnIR>();
    ret->value = m.createBinaryOp(cse::OpCode::Mul, x, symOne);
    auto body = std::make_unique<cse::BlockIR>();
    body->stmts.push_back(std::move(ret));
    m.body = std::move(body);

    // Bare config: the exact tier runs, but must not match the symbolic 1.
    cse::AlgebraicSimplifyPass pass(false, false, false, false);
    pass.run(m);
    cse::CodeGen cg;
    const std::string out = cg.generateBody(m);
    expect("symbolic 1 is not folded away", has(out, "sym_one"), out);
  }

  std::printf(failures == 0 ? "\nALL CONFIG CHECKS PASSED\n"
                            : "\n%d CONFIG CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
