#include "ir_printer.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace cse {
namespace {

const char* kindName(NodeKind k) {
  switch (k) {
    case NodeKind::Constant: return "const";
    case NodeKind::Variable: return "var";
    case NodeKind::BinaryOp: return "binop";
    case NodeKind::UnaryOp: return "unop";
    case NodeKind::ArrayAccess: return "index";
    case NodeKind::MemberAccess: return "member";
    case NodeKind::ArrowAccess: return "arrow";
    case NodeKind::Call: return "call";
    case NodeKind::Ternary: return "ternary";
    case NodeKind::Cast: return "cast";
  }
  return "?";
}

// Print a double with full round-trip precision. `%.17g` also distinguishes
// -0.0 ("-0") from +0.0 ("0"), which the identity comparison treats as
// different constants.
std::string fmtConst(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

class Dumper {
 public:
  explicit Dumper(bool metadata) : _metadata(metadata) {}

  std::string run(const IRModule& m) {
    const FuncSignature& sig = m.funcSig;
    _out += "fn " + sig.returnType + " " + sig.name + "(";
    for (size_t i = 0; i < sig.params.size(); ++i) {
      if (i) _out += ", ";
      _out += sig.params[i].type + " " + sig.params[i].name;
    }
    _out += ")\nbody:\n";
    dumpStmt(m.body.get(), 1);
    _out += "nodes:\n";
    for (const std::string& d : _defs) _out += "  " + d + "\n";
    return _out;
  }

 private:
  void indent(int d) { _out.append(static_cast<size_t>(d) * 2, ' '); }

  static const DAGNode* operand(const DAGNode* n, size_t i) {
    return i < n->operands.size() ? n->operands[i] : nullptr;
  }

  // The `%N` label for a node, emitting its definition on first sight.
  // Children are labelled before their parent (post-order), so a definition in
  // the table never refers to a line below it.
  std::string ref(const DAGNode* n) {
    if (!n) return "_";
    auto it = _label.find(n);
    if (it != _label.end()) return "%" + std::to_string(it->second);
    // Recurse first: this assigns every operand a lower label than `n`.
    const std::string body = nodeBody(n);
    const int id = static_cast<int>(_defs.size());
    _label[n] = id;
    _defs.push_back("%" + std::to_string(id) + " = " + body);
    return "%" + std::to_string(id);
  }

  // `pure` defaults to true, so only an impure node is annotated. A node that
  // flips purity therefore shows up as the annotation appearing or vanishing.
  static std::string pureTag(const DAGNode* n) { return n->pure ? "" : " !pure"; }

  std::string nodeBody(const DAGNode* n) {
    std::string s = kindName(n->kind);
    switch (n->kind) {
      case NodeKind::Constant:
        s += " " + fmtConst(n->constVal);
        if (_metadata) {
          s += " text='" + n->numText + "'";
          if (!n->symbol.empty()) s += " sym='" + n->symbol + "'";
        }
        break;
      case NodeKind::Variable:
        s += " " + n->name;
        break;
      case NodeKind::BinaryOp:
        s += " '" + opSpelling(n->op) + "' " + ref(operand(n, 0)) + " " +
             ref(operand(n, 1));
        break;
      case NodeKind::UnaryOp: {
        // `++x`/`x--` carry their spelling in `name`; a plain unary carries it
        // in `op`.
        s += " '" + (n->name.empty() ? opSpelling(n->op) : n->name) + "'";
        if (n->postfix) s += " post";
        s += pureTag(n) + " " + ref(operand(n, 0));
        break;
      }
      case NodeKind::ArrayAccess:
        s += pureTag(n) + " " + ref(operand(n, 0)) + "[" + ref(operand(n, 1)) +
             "]";
        break;
      case NodeKind::MemberAccess:
        s += pureTag(n) + " " + ref(operand(n, 0)) + "." + n->name;
        break;
      case NodeKind::ArrowAccess:
        s += pureTag(n) + " " + ref(operand(n, 0)) + "->" + n->name;
        break;
      case NodeKind::Call: {
        s += pureTag(n) + " " + ref(operand(n, 0)) + "(";
        for (size_t i = 1; i < n->operands.size(); ++i) {
          if (i > 1) s += ", ";
          s += ref(n->operands[i]);
        }
        s += ")";
        break;
      }
      case NodeKind::Ternary:
        s += " " + ref(operand(n, 0)) + " ? " + ref(operand(n, 1)) + " : " +
             ref(operand(n, 2));
        break;
      case NodeKind::Cast:
        s += " <" + n->name + "> " + ref(operand(n, 0));
        break;
    }
    if (_metadata && n->vecDim != 0) s += " vec=" + std::to_string(n->vecDim);
    return s;
  }

  void dumpStmt(const StmtIR* s, int depth) {
    indent(depth);
    if (!s) {
      _out += "nil\n";
      return;
    }
    switch (s->kind) {
      case StmtIRKind::Block: {
        const auto* b = static_cast<const BlockIR*>(s);
        _out += "{\n";
        for (const auto& x : b->stmts) dumpStmt(x.get(), depth + 1);
        indent(depth);
        _out += "}\n";
        break;
      }
      case StmtIRKind::VarDecl: {
        const auto* d = static_cast<const VarDeclIR*>(s);
        _out += "decl " + d->type + " " + d->name + " = " + ref(d->init) + "\n";
        break;
      }
      case StmtIRKind::Assign: {
        const auto* a = static_cast<const AssignIR*>(s);
        _out += "assign ";
        _out += a->targetExpr ? ref(a->targetExpr) : ("$" + a->target);
        _out += (a->compoundOp != OpCode::None)
                    ? (" '" + opSpelling(a->compoundOp) + "'=")
                    : " =";
        _out += " " + ref(a->value) + "\n";
        break;
      }
      case StmtIRKind::ExprStmt: {
        const auto* e = static_cast<const ExprStmtIR*>(s);
        _out += "expr " + ref(e->expr) + "\n";
        break;
      }
      case StmtIRKind::Return: {
        const auto* r = static_cast<const ReturnIR*>(s);
        _out += "ret " + ref(r->value) + "\n";
        break;
      }
      case StmtIRKind::ForLoop: {
        const auto* f = static_cast<const ForLoopIR*>(s);
        _out += "for\n";
        indent(depth + 1);
        _out += "init:\n";
        dumpStmt(f->init.get(), depth + 2);
        indent(depth + 1);
        _out += "cond: " + ref(f->cond) + "\n";
        indent(depth + 1);
        _out += "upd: " + ref(f->update);
        if (f->updateOp != OpCode::None)
          _out += " '" + opSpelling(f->updateOp) + "'";
        _out += " rhs " + ref(f->updateRhs) + "\n";
        indent(depth + 1);
        _out += "body:\n";
        dumpStmt(f->body.get(), depth + 2);
        break;
      }
      case StmtIRKind::IfElse: {
        const auto* ie = static_cast<const IfElseIR*>(s);
        _out += "if";
        if (ie->isConstexpr) _out += " constexpr";
        _out += " " + ref(ie->cond) + "\n";
        indent(depth + 1);
        _out += "then:\n";
        dumpStmt(ie->thenBranch.get(), depth + 2);
        indent(depth + 1);
        _out += "else:\n";
        dumpStmt(ie->elseBranch.get(), depth + 2);
        break;
      }
    }
  }

  bool _metadata;
  std::string _out;
  std::vector<std::string> _defs;
  std::unordered_map<const DAGNode*, int> _label;
};

}  // namespace

std::string dumpModule(const IRModule& module, bool includeMetadata) {
  return Dumper(includeMetadata).run(module);
}

}  // namespace cse
