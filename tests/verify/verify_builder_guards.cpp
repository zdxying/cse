// The builder must reject a malformed AST with a positioned diagnostic rather
// than dereference an empty statement slot.
//
// Every slot checked here is filled by the parser, so none of these paths is
// reachable from the CLI -- which is exactly why they are worth a guard and a
// test. `*nullptr` is undefined behaviour that no build reports: the process
// simply reads whatever is at that address. A `CSEError` instead becomes "this
// region is passed through unchanged" with a line and a column, which is the
// outcome the driver already knows how to produce.
//
// Build: g++ -std=c++17 -O2 -Isrc tests/verify/verify_builder_guards.cpp bin/libcse.a
#include <cstdio>
#include <memory>

#include "frontend/ast.h"
#include "frontend/cse_config.h"
#include "frontend/diagnostics.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"

namespace {

int failures = 0;

void expect(const char* what, bool ok) {
  if (!ok) failures++;
  std::printf("%-46s %s\n", what, ok ? "OK" : "FAIL");
}

std::unique_ptr<cse::Stmt> makeBlock(unsigned line) {
  return std::make_unique<cse::Stmt>(cse::StmtKind::Block, cse::SourceLoc{line, 1});
}

std::unique_ptr<cse::Expr> makeNumber(double v) {
  auto e = std::make_unique<cse::Expr>(cse::ExprKind::Number, cse::SourceLoc{0, 0});
  e->numVal = v;
  e->numText = "1";
  return e;
}

// Build the function and report whether it was rejected with a CSEError.
bool rejected(cse::FunctionDef& fn) {
  cse::CSEConfig cfg;
  cse::IRModule mod;
  cse::IRBuilder builder(&mod, cfg);
  try {
    builder.buildFunction(fn);
  } catch (const cse::CSEError&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

cse::FunctionDef shell() {
  cse::FunctionDef fn;
  fn.returnType = "double";
  fn.name = "f";
  fn.loc = {1, 1};
  fn.body = makeBlock(1);
  return fn;
}

}  // namespace

int main() {
  {  // `if` with no condition
    cse::FunctionDef fn = shell();
    auto ifs = std::make_unique<cse::Stmt>(cse::StmtKind::IfElse, cse::SourceLoc{2, 5});
    ifs->ifThen = makeBlock(2);
    fn.body->stmts.push_back(std::move(ifs));
    expect("if without a condition is rejected", rejected(fn));
  }
  {  // `if` with no then-branch
    cse::FunctionDef fn = shell();
    auto ifs = std::make_unique<cse::Stmt>(cse::StmtKind::IfElse, cse::SourceLoc{2, 5});
    ifs->ifCond = makeNumber(1);
    fn.body->stmts.push_back(std::move(ifs));
    expect("if without a branch is rejected", rejected(fn));
  }
  {  // `for` with no body
    cse::FunctionDef fn = shell();
    auto f = std::make_unique<cse::Stmt>(cse::StmtKind::ForLoop, cse::SourceLoc{2, 5});
    fn.body->stmts.push_back(std::move(f));
    expect("for without a body is rejected", rejected(fn));
  }
  {  // function with no body
    cse::FunctionDef fn = shell();
    fn.body.reset();
    expect("function without a body is rejected", rejected(fn));
  }
  {  // the well-formed shape still builds
    cse::FunctionDef fn = shell();
    auto rs = std::make_unique<cse::Stmt>(cse::StmtKind::Return, cse::SourceLoc{2, 3});
    rs->retExpr = makeNumber(1);
    fn.body->stmts.push_back(std::move(rs));
    expect("a well-formed function still builds", !rejected(fn));
  }

  std::printf(failures == 0 ? "\nALL BUILDER-GUARD CHECKS PASSED\n"
                            : "\n%d BUILDER-GUARD CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
