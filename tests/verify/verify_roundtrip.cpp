// Round-trip fidelity oracle.
//
// The backend printer is correct iff re-parsing its output reproduces the same
// IR. This test closes that loop without running any optimization pass:
//
//     source --lex/parse/build--> IR1 --codegen--> text1
//     text1  --lex/parse/build--> IR2
//     assert canonical(IR1) == canonical(IR2)
//
// A printer defect that drops a needed parenthesis (e.g. `a - (b - c)` printed
// as `a - b - c`) or mis-prints an operator (`&&` as `&`) leaves IR1 correct but
// changes how text1 re-parses, so IR2 differs. The comparison is structural and
// ignores node ids and constant spelling, so it isolates *text fidelity* from
// value-level differences.
//
// Usage: verify_roundtrip <fixture.cpp> [more fixtures...]
//   Extracts every //@cse region and checks each one. Exit 0 iff all pass.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "backend/codegen.h"
#include "frontend/cse_config.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/region_extractor.h"
#include "ir/dag_node.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"
#include "ir/statement.h"
#include "sweep.h"

using namespace cse;

namespace {

// ---- canonical structural dump: ignores node ids and constant spelling ------

void dumpNode(const DAGNode* n, std::string& out) {
  if (!n) {
    out += "_";
    return;
  }
  switch (n->kind) {
    case NodeKind::Constant: {
      char buf[64];
      std::snprintf(buf, sizeof buf, "#%.17g", n->constVal);
      out += buf;
      break;
    }
    case NodeKind::Variable:
      out += "$" + n->name;
      break;
    default: {
      out += "(";
      out += std::to_string(static_cast<int>(n->kind));
      out += "|";
      out += std::to_string(static_cast<int>(n->op));
      out += "|";
      out += n->name;  // member name / cast type / ++ -- spelling
      out += "|";
      out += n->pure ? "p" : "-";
      out += n->postfix ? "+" : "-";
      for (const DAGNode* op : n->operands) {
        out += " ";
        dumpNode(op, out);
      }
      out += ")";
      break;
    }
  }
}

// A control-flow body is dumped as a braced block regardless of whether the IR
// stores a bare statement or a Block: codegen always emits braces (except for a
// non-block `if constexpr` then-branch), so re-parsing normalizes a bare body
// into a one-statement Block. Wrapping both sides makes that difference
// invisible, so only *semantic* divergence is reported.
void dumpStmt(const StmtIR* s, std::string& out);

// Emit a statement list, splicing nested Blocks inline. IRBuilder preserves a
// source-level `{ ... }` scope as a nested Block, but codegen flattens blocks
// (it emits their statements directly). Since declarations are alpha-renamed to
// unique names, flattening is semantics-preserving; the oracle must normalize it
// away or every scoped region would look like a mismatch.
void dumpStmts(const std::vector<std::unique_ptr<StmtIR>>& list, std::string& out) {
  for (const auto& x : list) {
    if (x->kind == StmtIRKind::Block) {
      dumpStmts(static_cast<const BlockIR*>(x.get())->stmts, out);
    } else {
      dumpStmt(x.get(), out);
    }
  }
}

void dumpBody(const StmtIR* s, std::string& out) {
  out += "{";
  if (s && s->kind == StmtIRKind::Block) {
    dumpStmts(static_cast<const BlockIR*>(s)->stmts, out);
  } else if (s) {
    dumpStmt(s, out);
  }
  out += "}";
}

void dumpStmt(const StmtIR* s, std::string& out) {
  if (!s) {
    out += "nil;";
    return;
  }
  switch (s->kind) {
    case StmtIRKind::Block: {
      const auto* b = static_cast<const BlockIR*>(s);
      out += "{";
      dumpStmts(b->stmts, out);
      out += "}";
      break;
    }
    case StmtIRKind::VarDecl: {
      const auto* d = static_cast<const VarDeclIR*>(s);
      out += "decl " + d->type + " " + d->name + " = ";
      dumpNode(d->init, out);
      out += ";";
      break;
    }
    case StmtIRKind::Assign: {
      const auto* a = static_cast<const AssignIR*>(s);
      out += "assign ";
      if (a->targetExpr) {
        dumpNode(a->targetExpr, out);
      } else {
        out += a->target;
      }
      out += " op" + std::to_string(static_cast<int>(a->compoundOp)) + " = ";
      dumpNode(a->value, out);
      out += ";";
      break;
    }
    case StmtIRKind::ExprStmt: {
      const auto* e = static_cast<const ExprStmtIR*>(s);
      out += "expr ";
      dumpNode(e->expr, out);
      out += ";";
      break;
    }
    case StmtIRKind::Return: {
      const auto* r = static_cast<const ReturnIR*>(s);
      out += "ret ";
      dumpNode(r->value, out);
      out += ";";
      break;
    }
    case StmtIRKind::ForLoop: {
      const auto* f = static_cast<const ForLoopIR*>(s);
      out += "for(";
      dumpStmt(f->init.get(), out);
      out += " cond ";
      dumpNode(f->cond, out);
      out += " upd ";
      dumpNode(f->update, out);
      out += " uop" + std::to_string(static_cast<int>(f->updateOp)) + " urhs ";
      dumpNode(f->updateRhs, out);
      out += ") ";
      dumpBody(f->body.get(), out);
      break;
    }
    case StmtIRKind::IfElse: {
      const auto* ie = static_cast<const IfElseIR*>(s);
      out += "if(";
      dumpNode(ie->cond, out);
      out += ") ";
      dumpBody(ie->thenBranch.get(), out);
      out += " else ";
      dumpBody(ie->elseBranch.get(), out);
      break;
    }
  }
}

// ---- one pass through source -> IR -> text ----------------------------------

struct BuildOut {
  bool ok = false;
  bool skipped = false;      // construct the oracle does not model (structs/...)
  std::string skipReason;
  std::string err;
  std::string text;                 // emitted C++ for all functions
  std::vector<std::string> canon;   // one canonical dump per function
};

BuildOut buildAll(const std::string& code) {
  BuildOut out;
  CSEConfig cfg;
  Lexer lexer(code, cfg);
  auto tokens = lexer.tokenize();
  Parser parser(tokens, cfg);
  auto res = parser.parseAll();

  if (!res.structDefs.empty() || !res.namespaces.empty() ||
      !res.usingDecls.empty()) {
    out.skipped = true;
    out.skipReason = "structs/namespaces/using (not modelled by this oracle yet)";
    return out;
  }
  if (res.functions.empty()) {
    out.skipped = true;
    out.skipReason = "declaration-only region (no function IR to compare)";
    return out;
  }

  for (const auto& f : res.functions) {
    IRModule mod;
    IRBuilder builder(&mod, cfg);
    builder.buildFunction(*f);
    CodeGen codegen;
    out.text += codegen.generate(mod, {}, {}, f->templateParams);
    std::string d;
    dumpStmt(mod.body.get(), d);
    out.canon.push_back(std::move(d));
  }
  out.ok = true;
  return out;
}

// On Pass/Skip, `msg` is empty or the skip reason; on Fail it holds a diff.
Result checkRegion(const std::string& code, std::string& msg) {
  BuildOut a = buildAll(code);
  if (a.skipped) {
    msg = a.skipReason;
    return Result::Skip;
  }
  if (!a.ok) {
    msg = "pass 1: " + a.err;
    return Result::Fail;
  }

  BuildOut b = buildAll(a.text);
  if (!b.ok) {
    msg = "pass 2 (re-parsing emitted text): " + b.err + "\n--- emitted ---\n" + a.text;
    return Result::Fail;
  }
  if (a.canon.size() != b.canon.size()) {
    msg = "function count changed: " + std::to_string(a.canon.size()) + " -> " +
          std::to_string(b.canon.size()) + "\n--- emitted ---\n" + a.text;
    return Result::Fail;
  }
  for (size_t i = 0; i < a.canon.size(); ++i) {
    if (a.canon[i] != b.canon[i]) {
      msg = "IR mismatch after round-trip (function " + std::to_string(i) + ")\n" +
            "--- emitted ---\n" + a.text + "\n--- IR before ---\n" + a.canon[i] +
            "\n--- IR after ---\n" + b.canon[i];
      return Result::Fail;
    }
  }
  return Result::Pass;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixture.cpp> [more...]\n", argv[0]);
    return 2;
  }
  return sweepRegions(argc, argv, "round-trip", "regions",
                      /*countAllRegions=*/true, /*printSkips=*/true, checkRegion);
}
