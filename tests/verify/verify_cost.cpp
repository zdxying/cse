// Rule pin for the FLOP cost model's counting rule.
//
// The model charges each *reference* to a DAG node, not each distinct node:
// codegen re-prints a shared (unmaterialized) node at every use site, while a
// node CSE materialized reaches the IR as a single definition whose uses are
// cheap Variable references. The rule has flipped silently before -- the docs
// and the code disagreed on "per statement" vs "unique node" -- so this pins
// the observable numbers on shapes where the two rules differ. `totalNodes`
// stays the distinct-node count.
//
// The modules here are built but *not* optimized, so no CSE temporary is
// materialized: the raw hash-consed DAG is exactly the case where per-use-site
// counting and unique-node counting diverge.
//
// Build: g++ -std=c++17 -O2 -Isrc tests/verify/verify_cost.cpp bin/libcse.a
#include <cstdio>
#include <string>
#include <vector>

#include "analysis/cost_model.h"
#include "frontend/cse_config.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"

namespace {

int failures = 0;

void expect(const char* what, long long got, long long want) {
  const bool ok = got == want;
  if (!ok) failures++;
  std::printf("%-56s got %lld want %lld  %s\n", what, got, want,
              ok ? "OK" : "FAIL");
}

// Cost of the single function in `src`, built but not optimized.
cse::CostResult cost(const std::string& src) {
  cse::CSEConfig cfg;
  cse::Lexer lexer(src, cfg);
  const std::vector<cse::Token> tokens = lexer.tokenize();
  cse::Parser parser(tokens, cfg);
  cse::Parser::ParseResult parsed = parser.parseAll();
  cse::CostResult total;
  for (auto& f : parsed.functions) {
    cse::IRModule mod;
    cse::IRBuilder builder(&mod, cfg);
    builder.buildFunction(*f);
    total += cse::analyzeCost(mod, &cfg);
  }
  return total;
}

}  // namespace

int main() {
  // 1. A node shared by two statements is charged per use site. `a + b` is
  //    hash-consed to one node referenced by both declarations, so it costs 2;
  //    `x + y` costs 1. Unique-node counting would report 2.
  {
    const std::string src =
        "double f(double a, double b) {"
        " double x = a + b; double y = a + b; return x + y; }";
    const cse::CostResult r = cost(src);
    expect("shared node across statements counts per use site", r.flops, 3);
    expect("... and is still one distinct node", r.totalNodes, 6);
  }

  // 2. A materialized temporary is computed once: `t = a + b` costs 1 and the
  //    cheap `t + t` costs 1. This is the shape CSE produces, and why the rule
  //    is exact for optimized code too.
  {
    const std::string src =
        "double g(double a, double b) { double t = a + b; return t + t; }";
    expect("materialized temporary is charged once", cost(src).flops, 2);
  }

  // 3. The same subexpression twice in one statement is two references. The
  //    parser hash-conses `(a + b)` to one node, referenced twice by the outer
  //    add: 2 + 1 = 3. `totalNodes` stays 4 (a, b, a+b, outer).
  {
    const std::string src =
        "double h(double a, double b) { return (a + b) + (a + b); }";
    const cse::CostResult r = cost(src);
    expect("duplicate operand in one statement counts twice", r.flops, 3);
    expect("... and the duplicate is still one distinct node", r.totalNodes,
           4);
  }

  // 4. Loop multiplicity scales a reference by the trip count: the body's one
  //    `a + b` and the one `++i` are each charged once per iteration (4).
  {
    const std::string src =
        "double k(double a, double b) {"
        " for (int i = 0; i < 4; ++i) { a = a + b; } return a; }";
    expect("loop body is scaled by the trip count", cost(src).flops, 8);
  }

  // 5. A nest too large to enumerate is reported as unresolved. The documented
  //    contract (cost_model.h) is that a non-zero `unknownLoops` means `flops`
  //    is a *lower bound*; the old code returned the truncated product and left
  //    the counter at zero, so a 2e6-iteration loop printed "2 flops" with no
  //    indication that it was an approximation.
  {
    const std::string src =
        "double big(double a) {"
        " for (int i = 0; i < 2000000; ++i) { a = a + 1.0; } return a; }";
    const cse::CostResult r = cost(src);
    expect("an unenumerable loop is reported as unresolved", r.unknownLoops, 1);
    expect("... and its flops is a lower bound, not an overcount",
           r.flops <= 2000000 + 2, 1);
  }

  // 6. A resolvable loop is not flagged: the counter would become meaningless
  //    if every loop incremented it.
  {
    const std::string src =
        "double small(double a) {"
        " for (int i = 0; i < 1000; ++i) { a = a + 1.0; } return a; }";
    expect("a resolvable loop is not reported as unresolved",
           cost(src).unknownLoops, 0);
  }

  std::printf(failures == 0 ? "\nALL COST-MODEL CHECKS PASSED\n"
                            : "\n%d COST-MODEL CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
