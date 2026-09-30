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
  const std::string mulChain = "double f(double a, double b, double c) {"
                               " return a * (b * c); }";

  // 1. The conservative default (a bare CSEConfig) collapses nothing.
  cse::CSEConfig bare;
  expect("bare CSEConfig: x / x kept", !collapsed(optimize(selfDiv, bare)),
         optimize(selfDiv, bare));
  expect("bare CSEConfig: x * 0 kept", !collapsed(optimize(timesZero, bare)),
         optimize(timesZero, bare));

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

  // 3. allowUnsafeFpIdentities turns exactly those four on.
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

  // 4. Regrouping a multiplication chain changes FP rounding, so it needs
  //    allowFpReassoc -- commutativity/associativity alone must not do it.
  expect("comm+assoc only: a * (b * c) kept",
         has(optimize(mulChain, n), "(b * c)"), optimize(mulChain, n));
  cse::CSEConfig r = numericOnly();
  r.allowFpReassoc = true;
  expect("allowFpReassoc: a * (b * c) regrouped",
         !has(optimize(mulChain, r), "(b * c)"), optimize(mulChain, r));

  std::printf(failures == 0 ? "\nALL CONFIG CHECKS PASSED\n"
                            : "\n%d CONFIG CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
