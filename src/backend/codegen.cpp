#include "codegen.h"

#include <cmath>
#include <unordered_set>

#include "../frontend/ast.h"
#include "../ir/ir_module.h"
#include "../ir/statement.h"

namespace cse {

void CodeGen::emitTemplateParams(const std::vector<TemplateParam>& params) {
  if (params.empty()) return;
  _out << "template<";
  for (size_t i = 0; i < params.size(); i++) {
    if (i > 0) _out << ", ";
    _out << params[i].paramType << " " << params[i].paramName;
    if (!params[i].defaultVal.empty()) {
      _out << " = " << params[i].defaultVal;
    }
  }
  _out << ">\n";
}

// Binary-operator precedence. Higher binds tighter. `O`/`A` are logical
// `||`/`&&` (their own op codes, so they can never be printed as the bitwise
// `|`/`&`). `=` is lowest. Unknown codes are treated as the loosest so their
// children are parenthesized rather than silently regrouped.
static int getPrecedence(char op) {
  switch (op) {
    case '=':
      return 1;
    case 'O':
      return 3;  // ||
    case 'A':
      return 4;  // &&
    case 'e':
    case 'n':
      return 5;  // == !=
    case '<':
    case '>':
    case 'l':
    case 'g':
      return 6;  // < > <= >=
    case '+':
    case '-':
      return 7;
    case '*':
    case '/':
    case '%':
      return 8;
    default:
      return 0;
  }
}

// Precedence of a whole expression node when it appears as an operand. Unary
// operators and casts bind tighter than any binary operator; primary
// expressions (variables, calls, subscripts, members) bind tightest.
static int exprPrecedence(const DAGNode* n) {
  switch (n->kind) {
    case NodeKind::BinaryOp:
      return getPrecedence(n->op);
    case NodeKind::Ternary:
      return 2;
    case NodeKind::Cast:
    case NodeKind::UnaryOp:
      return 9;
    default:
      return 100;
  }
}

// The base of a postfix operation (subscript / member / arrow) must be a primary
// expression; anything looser is parenthesized so `(a + b)[i]`, `(*p)[i]` and
// `(c ? x : y).f` keep their meaning.
static std::string parenthesizePostfixBase(const DAGNode* base,
                                           std::string text) {
  if (base && exprPrecedence(base) < 100) return "(" + text + ")";
  return text;
}

// Map the internal operator encoding back to C++ spelling.
static std::string opText(char op) {
  switch (op) {
    case 'e': return "==";
    case 'n': return "!=";
    case 'l': return "<=";
    case 'g': return ">=";
    case 'O': return "||";
    case 'A': return "&&";
    default: return std::string(1, op);
  }
}

// Whether `child` must be wrapped in parentheses as an operand of a binary
// operator with precedence `parentPrec`.
//
// Precedence settles the different-precedence cases. At equal precedence:
// every binary operator in this IR except `=` is left-associative, so a left
// child never needs parens while a right child does (`a - (b - c)` must not
// become `a - b - c`); `=` is right-associative, so the reverse holds. This
// also keeps `a * (b * c)` parenthesized, which matters under IEEE-754.
static bool childNeedsParens(const DAGNode* child, int parentPrec,
                             bool isRightChild, char parentOp) {
  int cp = exprPrecedence(child);
  if (cp < parentPrec) return true;
  if (cp > parentPrec) return false;
  if (parentOp == '=') return !isRightChild;
  return isRightChild;
}

std::string CodeGen::generate(IRModule& module, const std::vector<StructDef*>& structDefs,
  const std::vector<OptimizedStruct>& optStructs,
  const std::vector<TemplateParam>& funcTemplateParams) {
  _out.str("");
  _out.clear();

  // Emit structs without methods (pure data structs)
  for (auto* sd : structDefs) {
    emitTemplateParams(sd->templateParams);
    _out << "struct " << sd->name << " {\n";
    // Emit using declarations inside struct
    for (auto& usingDecl : sd->usingDecls) {
      _out << "    using " << usingDecl->aliasName << " = " << usingDecl->underlyingType << ";\n";
    }
    for (auto& field : sd->fields) {
      _out << "    " << field.type << " " << field.name << ";\n";
    }
    _out << "};\n\n";
  }

  // Emit structs with optimized methods (inline definitions)
  for (auto& os : optStructs) {
    auto* sd = os.def;
    emitTemplateParams(sd->templateParams);
    _out << "struct " << sd->name << " {\n";
    // Emit using declarations inside struct
    for (auto& usingDecl : sd->usingDecls) {
      _out << "    using " << usingDecl->aliasName << " = " << usingDecl->underlyingType << ";\n";
    }
    for (auto& field : sd->fields) {
      _out << "    " << field.type << " " << field.name << ";\n";
    }
    // Emit optimized method bodies inline
    for (size_t i = 0; i < sd->methods.size(); i++) {
      if (i < os.methodModules.size() && os.methodModules[i]) {
        auto* methodMod = os.methodModules[i].get();
        _out << "    " << methodMod->funcSig.returnType << " " << methodMod->funcSig.name
             << "(";
        for (size_t j = 0; j < methodMod->funcSig.params.size(); j++) {
          if (j > 0) _out << ", ";
          _out << methodMod->funcSig.params[j].type << " "
               << methodMod->funcSig.params[j].name;
        }
        _out << ") {\n";
        if (methodMod->body) {
          emitStmt(methodMod->body.get(), 1);
        }
        _out << "    }\n";
      }
    }
    _out << "};\n\n";
  }

  // Skip main function if funcSig is empty
  if (module.funcSig.name.empty()) {
    return _out.str();
  }

  emitTemplateParams(funcTemplateParams);
  _out << module.funcSig.returnType << " " << module.funcSig.name << "(";
  for (size_t i = 0; i < module.funcSig.params.size(); i++) {
    if (i > 0) _out << ", ";
    _out << module.funcSig.params[i].type << " " << module.funcSig.params[i].name;
  }
  _out << ") {\n";

  if (module.body) {
    emitStmt(module.body.get(), 1);
  }

  _out << "}\n";
  return _out.str();
}

std::string CodeGen::generateBody(IRModule& module, int indentLevel) {
  _out.str("");
  _out.clear();
  if (module.body) emitStmt(module.body.get(), indentLevel);
  return _out.str();
}

void CodeGen::emitStmt(StmtIR* stmt, int indentLevel) {
  if (!stmt) return;
  std::string ind = makeIndent(indentLevel);

  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* block = static_cast<BlockIR*>(stmt);
      for (auto& s : block->stmts) {
        emitStmt(s.get(), indentLevel);
      }
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* forLoop = static_cast<ForLoopIR*>(stmt);
      _out << ind << "for (";
      if (forLoop->init) {
        if (forLoop->init->kind == StmtIRKind::VarDecl) {
          auto* decl = static_cast<VarDeclIR*>(forLoop->init.get());
          _out << decl->type << " " << decl->name;
          if (decl->init) _out << " = " << emitExpr(decl->init);
        }
      }
      _out << "; ";
      if (forLoop->cond) _out << emitExpr(forLoop->cond);
      _out << "; ";
      if (forLoop->update) {
        _out << emitExpr(forLoop->update);
        if (forLoop->updateOp == '=' && forLoop->updateRhs) {
          _out << " = " << emitExpr(forLoop->updateRhs);
        } else if (forLoop->updateOp == '+' && forLoop->updateRhs) {
          _out << " += " << emitExpr(forLoop->updateRhs);
        } else if (forLoop->updateOp == '-' && forLoop->updateRhs) {
          _out << " -= " << emitExpr(forLoop->updateRhs);
        } else if (forLoop->updateOp == '*' && forLoop->updateRhs) {
          _out << " *= " << emitExpr(forLoop->updateRhs);
        } else if (forLoop->updateOp == '/' && forLoop->updateRhs) {
          _out << " /= " << emitExpr(forLoop->updateRhs);
        } else if (forLoop->updateOp == '+') {
          _out << "++";
        } else if (forLoop->updateOp == '-') {
          _out << "--";
        }
      }
      _out << ") {\n";
      emitStmt(forLoop->body.get(), indentLevel + 1);
      _out << ind << "}\n";
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ifElse = static_cast<IfElseIR*>(stmt);
      // Preserve `if constexpr (...) stmt;` without braces so downstream
      // tooling that treats `if` lines as opaque can still see the else branch.
      if (ifElse->isConstexpr && ifElse->thenBranch &&
          ifElse->thenBranch->kind != StmtIRKind::Block) {
        _out << ind << "if constexpr (" << emitExpr(ifElse->cond) << ") ";
        emitStmt(ifElse->thenBranch.get(), 0);
        if (ifElse->elseBranch) {
          if (ifElse->elseBranch->kind == StmtIRKind::Block) {
            _out << ind << "else {\n";
            emitStmt(ifElse->elseBranch.get(), indentLevel + 1);
            _out << ind << "}\n";
          } else {
            _out << ind << "else ";
            emitStmt(ifElse->elseBranch.get(), 0);
          }
        }
        break;
      }
      _out << ind << (ifElse->isConstexpr ? "if constexpr (" : "if (")
           << emitExpr(ifElse->cond) << ") {\n";
      emitStmt(ifElse->thenBranch.get(), indentLevel + 1);
      if (ifElse->elseBranch) {
        _out << ind << "} else {\n";
        emitStmt(ifElse->elseBranch.get(), indentLevel + 1);
      }
      _out << ind << "}\n";
      break;
    }
    case StmtIRKind::VarDecl: {
      auto* decl = static_cast<VarDeclIR*>(stmt);
      _out << ind << decl->type << " " << decl->name;
      if (decl->init) {
        _out << " = " << emitExpr(decl->init);
      }
      _out << ";\n";
      break;
    }
    case StmtIRKind::Assign: {
      auto* assign = static_cast<AssignIR*>(stmt);
      _out << ind;
      if (assign->targetExpr) {
        _out << emitExpr(assign->targetExpr);
      } else {
        _out << assign->target;
      }
      if (assign->compoundOp) {
        _out << " " << opText(assign->compoundOp) << "= "
             << emitExpr(assign->value) << ";\n";
      } else {
        _out << " = " << emitExpr(assign->value) << ";\n";
      }
      break;
    }
    case StmtIRKind::ExprStmt: {
      auto* exprStmt = static_cast<ExprStmtIR*>(stmt);
      if (exprStmt->expr) {
        _out << ind << emitExpr(exprStmt->expr) << ";\n";
      }
      break;
    }
    case StmtIRKind::Return: {
      auto* ret = static_cast<ReturnIR*>(stmt);
      _out << ind << "return";
      if (ret->value) _out << " " << emitExpr(ret->value);
      _out << ";\n";
      break;
    }
  }
}

std::string CodeGen::emitExpr(DAGNode* node) {
  if (!node) return "";

  switch (node->kind) {
    case NodeKind::Constant:
      return node->symbol.empty() ? node->numText : node->symbol;

    case NodeKind::Variable:
      return node->name;

    case NodeKind::BinaryOp: {
      if (node->operands.size() != 2) return "";
      std::string lhs = emitExpr(node->operands[0]);
      std::string rhs = emitExpr(node->operands[1]);

      int pp = getPrecedence(node->op);
      bool parenL = childNeedsParens(node->operands[0], pp, false, node->op);
      bool parenR = childNeedsParens(node->operands[1], pp, true, node->op);

      std::string result;
      if (parenL)
        result += "(" + lhs + ")";
      else
        result += lhs;

      result += " ";
      result += opText(node->op);
      result += " ";

      if (parenR)
        result += "(" + rhs + ")";
      else
        result += rhs;

      return result;
    }

    case NodeKind::UnaryOp: {
      if (node->operands.empty()) return "";
      std::string operand = emitExpr(node->operands[0]);
      if (isIncDec(node)) {
        // `x++` yields the old value and `++x` the new one. Which of the two
        // this is lives in `postfix`; the spelling alone cannot say it.
        return node->postfix ? operand + node->name : node->name + operand;
      }
      // Prefix unary: parenthesize compound operands to preserve precedence.
      // A nested unary is parenthesized too, so `+(+x)` does not print as the
      // prefix increment `++x` (and `-(-x)` does not print as `--x`).
      if (node->operands[0]->kind == NodeKind::BinaryOp ||
          node->operands[0]->kind == NodeKind::Ternary ||
          node->operands[0]->kind == NodeKind::UnaryOp) {
        operand = "(" + operand + ")";
      }
      return std::string(1, node->op) + operand;
    }

    case NodeKind::ArrayAccess: {
      if (node->operands.size() != 2) return "";
      // A subscript has to be an integral expression, and `1.0` is not one.
      // A constant's text is its *source spelling* and is deliberately not part
      // of its identity (see dag_node.h), so one node can be reached both from
      // an arithmetic operand (`1.0 * x`) and from an index slot (`feq[1]`).
      // Emitting the spelling blindly produced `feq[1.0]`, which does not
      // compile; an integral constant is written as an integer here.
      DAGNode* idx = node->operands[1];
      std::string index;
      if (idx && idx->kind == NodeKind::Constant && idx->symbol.empty() &&
          idx->constVal == std::floor(idx->constVal) &&
          std::fabs(idx->constVal) < 1e15) {
        index = std::to_string(static_cast<long long>(idx->constVal));
      } else {
        index = emitExpr(idx);
      }
      return parenthesizePostfixBase(node->operands[0],
                                     emitExpr(node->operands[0])) +
             "[" + index + "]";
    }

    case NodeKind::MemberAccess: {
      if (node->operands.empty()) return "";
      return parenthesizePostfixBase(node->operands[0],
                                     emitExpr(node->operands[0])) +
             "." + node->name;
    }

    case NodeKind::ArrowAccess: {
      if (node->operands.empty()) return "";
      return parenthesizePostfixBase(node->operands[0],
                                     emitExpr(node->operands[0])) +
             "->" + node->name;
    }

    case NodeKind::Call: {
      if (node->operands.empty()) return "";
      std::string result = emitExpr(node->operands[0]) + "(";
      for (size_t i = 1; i < node->operands.size(); i++) {
        if (i > 1) result += ", ";
        result += emitExpr(node->operands[i]);
      }
      result += ")";
      return result;
    }

    case NodeKind::Ternary: {
      if (node->operands.size() != 3) return "";
      std::string cond = emitExpr(node->operands[0]);
      std::string thenE = emitExpr(node->operands[1]);
      std::string elseE = emitExpr(node->operands[2]);
      // The condition and the middle operand are full expressions; a nested
      // conditional or assignment there must be parenthesized. The third
      // operand may be another conditional (right-associative) but not an
      // assignment.
      if (exprPrecedence(node->operands[0]) <= 2) cond = "(" + cond + ")";
      if (exprPrecedence(node->operands[1]) <= 2) thenE = "(" + thenE + ")";
      if (exprPrecedence(node->operands[2]) < 2) elseE = "(" + elseE + ")";
      return cond + " ? " + thenE + " : " + elseE;
    }

    case NodeKind::Cast: {
      if (node->operands.empty()) return "";
      std::string operand = emitExpr(node->operands[0]);
      // A cast applies to a unary-expression; a binary or conditional operand
      // has to be parenthesized or the cast binds to its first subterm
      // (`(double)(a + b)` must not print as `(double)a + b`).
      if (exprPrecedence(node->operands[0]) < 9) operand = "(" + operand + ")";
      return "(" + node->name + ")" + operand;
    }
  }
  return "";
}

std::string CodeGen::makeIndent(int level) const { return std::string(level * 4, ' '); }

}  // namespace cse
