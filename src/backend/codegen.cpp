#include "codegen.h"

#include <algorithm>
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

static int getPrecedence(char op) {
  switch (op) {
    case '*':
    case '/':
    case '%':
      return 2;
    case '+':
    case '-':
      return 1;
    default:
      return 0;
  }
}

static bool isLeftAssoc(char op) {
  return op == '+' || op == '-' || op == '*' || op == '/' || op == '%';
}

// Map the internal operator encoding back to C++ spelling.
static std::string opText(char op) {
  switch (op) {
    case 'e': return "==";
    case 'n': return "!=";
    case 'l': return "<=";
    case 'g': return ">=";
    default: return std::string(1, op);
  }
}

static bool childNeedsParens(char parentOp, char childOp, bool isRightChild) {
  int pp = getPrecedence(parentOp);
  int cp = getPrecedence(childOp);
  if (cp < pp) return true;
  if (cp > pp) return false;
  if (isRightChild && !isLeftAssoc(parentOp)) return true;
  return false;
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
      // Preserve `if constexpr (...) stmt;` without braces so the FreeLB
      // verifier (which treats `if` lines as opaque) can see the else branch.
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
      _out << " = " << emitExpr(assign->value) << ";\n";
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

      char lOp =
        (node->operands[0]->kind == NodeKind::BinaryOp) ? node->operands[0]->op : 0;
      char rOp =
        (node->operands[1]->kind == NodeKind::BinaryOp) ? node->operands[1]->op : 0;

      bool parenL = lOp && childNeedsParens(node->op, lOp, false);
      bool parenR = rOp && childNeedsParens(node->op, rOp, true);

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
      if (node->name == "postfix") {
        return operand + node->op;
      }
      // Check for ++ and -- operators stored in name field
      if (node->name == "++" || node->name == "--") {
        return node->name + operand;
      }
      // Prefix unary: parenthesize compound operands to preserve precedence.
      if (node->operands[0]->kind == NodeKind::BinaryOp ||
          node->operands[0]->kind == NodeKind::Ternary) {
        operand = "(" + operand + ")";
      }
      return std::string(1, node->op) + operand;
    }

    case NodeKind::ArrayAccess: {
      if (node->operands.size() != 2) return "";
      return emitExpr(node->operands[0]) + "[" + emitExpr(node->operands[1]) + "]";
    }

    case NodeKind::MemberAccess: {
      if (node->operands.empty()) return "";
      return emitExpr(node->operands[0]) + "." + node->name;
    }

    case NodeKind::ArrowAccess: {
      if (node->operands.empty()) return "";
      return emitExpr(node->operands[0]) + "->" + node->name;
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
      return emitExpr(node->operands[0]) + " ? " + emitExpr(node->operands[1]) + " : " +
             emitExpr(node->operands[2]);
    }

    case NodeKind::Cast: {
      if (node->operands.empty()) return "";
      return "(" + node->name + ")" + emitExpr(node->operands[0]);
    }
  }
  return "";
}

std::string CodeGen::makeIndent(int level) const { return std::string(level * 4, ' '); }

}  // namespace cse
