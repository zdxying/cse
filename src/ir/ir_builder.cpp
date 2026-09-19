#include "ir_builder.h"

#include <sstream>
#include <stdexcept>

namespace cse {

IRBuilder::IRBuilder(IRModule* module, const CSEConfig& config)
    : _module(module), _config(config) {}

// ===== Effect / scope analysis =====

void IRBuilder::prescanFunction(const FunctionDef& func) {
  _declared.clear();
  _constParams.clear();
  _pointerParams.clear();
  _written.clear();
  _passedToCall.clear();
  _hasImpureCall = false;

  for (const auto& p : func.params) {
    _declared.insert(p.name);
    if (p.type.find("const") != std::string::npos) _constParams.insert(p.name);
    if (p.type.find('*') != std::string::npos ||
        p.type.find('[') != std::string::npos)
      _pointerParams.insert(p.name);
  }

  if (func.body) prescanStmt(*func.body);
}

void IRBuilder::prescanStmt(const Stmt& stmt) {
  switch (stmt.kind) {
    case StmtKind::Block:
      for (const auto& s : stmt.stmts) prescanStmt(*s);
      break;
    case StmtKind::Assignment:
      _written.insert(stmt.varName);
      if (stmt.rhs) prescanExpr(*stmt.rhs);
      break;
    case StmtKind::VarDecl:
      _declared.insert(stmt.varName);
      if (stmt.varType.find('*') != std::string::npos ||
          stmt.varType.find('[') != std::string::npos)
        _pointerParams.insert(stmt.varName);
      if (stmt.init) prescanExpr(*stmt.init);
      break;
    case StmtKind::ExprStmt:
      if (stmt.expr) prescanExpr(*stmt.expr);
      break;
    case StmtKind::Return:
      if (stmt.retExpr) prescanExpr(*stmt.retExpr);
      break;
    case StmtKind::ForLoop:
      if (stmt.forInit) prescanStmt(*stmt.forInit);
      if (stmt.forCond) prescanExpr(*stmt.forCond);
      if (stmt.forUpdate) prescanExpr(*stmt.forUpdate);
      if (stmt.forBody) prescanStmt(*stmt.forBody);
      break;
    case StmtKind::IfElse:
      if (stmt.ifCond) prescanExpr(*stmt.ifCond);
      if (stmt.ifThen) prescanStmt(*stmt.ifThen);
      if (stmt.ifElse) prescanStmt(*stmt.ifElse);
      break;
  }
}

namespace {
void collectVars(const Expr& e, std::unordered_set<std::string>& out) {
  if (e.kind == ExprKind::Variable) out.insert(e.name);
  if (e.base) collectVars(*e.base, out);
  if (e.lhs) collectVars(*e.lhs, out);
  if (e.rhs) collectVars(*e.rhs, out);
  if (e.operand) collectVars(*e.operand, out);
  if (e.cond) collectVars(*e.cond, out);
  if (e.trueExpr) collectVars(*e.trueExpr, out);
  if (e.falseExpr) collectVars(*e.falseExpr, out);
  for (const auto& i : e.indices)
    if (i) collectVars(*i, out);
  for (const auto& a : e.callArgs)
    if (a) collectVars(*a, out);
}
}  // namespace

void IRBuilder::prescanExpr(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::Number:
      break;
    case ExprKind::Variable:
      break;
    case ExprKind::BinaryOp: {
      // Assignment (or, conservatively, an equality comparison) to an lvalue.
      if (expr.op == '=' && expr.lhs) {
        std::string r = rootName(*expr.lhs);
        if (!r.empty()) _written.insert(r);
        if (expr.lhs->kind == ExprKind::Variable) _written.insert(expr.lhs->name);
      }
      if (expr.lhs) prescanExpr(*expr.lhs);
      if (expr.rhs) prescanExpr(*expr.rhs);
      break;
    }
    case ExprKind::UnaryOp:
    case ExprKind::PostfixOp:
      if ((expr.name == "++" || expr.name == "--") && expr.operand &&
          expr.operand->kind == ExprKind::Variable) {
        _written.insert(expr.operand->name);
      }
      if (expr.kind == ExprKind::UnaryOp && expr.op == '+' && !expr.name.empty()) {
        // ++var encoded as UnaryOp with name "++"
        if (expr.operand && expr.operand->kind == ExprKind::Variable)
          _written.insert(expr.operand->name);
      }
      if (expr.operand) prescanExpr(*expr.operand);
      break;
    case ExprKind::ArrayAccess:
      if (expr.base) prescanExpr(*expr.base);
      for (const auto& i : expr.indices)
        if (i) prescanExpr(*i);
      break;
    case ExprKind::MemberAccess:
    case ExprKind::ArrowAccess:
      if (expr.base) prescanExpr(*expr.base);
      break;
    case ExprKind::Call: {
      std::string callee;
      if (expr.base) {
        if (expr.base->kind == ExprKind::Variable) callee = expr.base->name;
        else if (expr.base->kind == ExprKind::MemberAccess ||
                 expr.base->kind == ExprKind::ArrowAccess)
          callee = expr.base->memberName;
      }
      if (!isPureCallee(callee)) _hasImpureCall = true;
      if (expr.base) prescanExpr(*expr.base);
      for (const auto& a : expr.callArgs) {
        if (!a) continue;
        collectVars(*a, _passedToCall);
        prescanExpr(*a);
      }
      break;
    }
    case ExprKind::Ternary:
      if (expr.cond) prescanExpr(*expr.cond);
      if (expr.trueExpr) prescanExpr(*expr.trueExpr);
      if (expr.falseExpr) prescanExpr(*expr.falseExpr);
      break;
    case ExprKind::Cast:
      if (expr.operand) prescanExpr(*expr.operand);
      break;
  }
}

bool IRBuilder::isPureCallee(const std::string& callee) const {
  if (callee.empty()) return false;
  std::string last = callee;
  auto q = last.rfind("::");
  if (q != std::string::npos) last = last.substr(q + 2);
  auto lt = last.find('<');
  if (lt != std::string::npos) last = last.substr(0, lt);

  static const char* kBuiltin[] = {
      "sin",   "cos",  "tan",   "asin",  "acos",  "atan",  "atan2",
      "exp",   "log",  "log2",  "log10", "sqrt",  "cbrt",  "pow",
      "fabs",  "abs",  "floor", "ceil",  "round", "trunc", "fmin",
      "fmax",  "min",  "max",   "hypot", "fmod",  "copysign"};
  for (const char* w : kBuiltin) {
    if (last == w) return true;
  }
  if (_config.isPureFunction && _config.isPureFunction(callee)) return true;
  return false;
}

bool IRBuilder::isReadOnlyRoot(const std::string& name) const {
  if (name.empty()) return false;
  if (_constParams.count(name)) return true;
  if (!_declared.count(name)) return false;  // unknown/global: be conservative
  if (_written.count(name)) return false;
  if (_passedToCall.count(name)) return false;
  // Pointer/array-like roots may be aliased by writes to other roots unless the
  // caller guarantees no aliasing.
  if (_pointerParams.count(name) && !_config.noAlias) return false;
  return true;
}

std::string IRBuilder::rootName(const Expr& expr) const {
  switch (expr.kind) {
    case ExprKind::Variable:
      return expr.name;
    case ExprKind::ArrayAccess:
      return expr.base ? rootName(*expr.base) : "";
    case ExprKind::MemberAccess:
    case ExprKind::ArrowAccess:
      return expr.base ? rootName(*expr.base) : "";
    default:
      return "";
  }
}

// ===== Scope handling =====

void IRBuilder::pushScope() { _scopes.emplace_back(); }

void IRBuilder::popScope() {
  if (!_scopes.empty()) _scopes.pop_back();
}

std::string IRBuilder::declare(const std::string& name) {
  if (_scopes.empty()) pushScope();
  bool shadowed = false;
  for (const auto& s : _scopes) {
    if (s.count(name)) {
      shadowed = true;
      break;
    }
  }
  std::string internal = name;
  if (shadowed) {
    int& n = _shadowCounters[name];
    internal = name + "__s" + std::to_string(++n);
  }
  _scopes.back()[name] = internal;
  _module->getVar(internal);
  return internal;
}

std::string IRBuilder::resolve(const std::string& name) const {
  for (auto it = _scopes.rbegin(); it != _scopes.rend(); ++it) {
    auto f = it->find(name);
    if (f != it->end()) return f->second;
  }
  return name;
}

DAGNode* IRBuilder::varRef(const std::string& name) {
  return _module->getVar(resolve(name));
}

// ===== Build =====

void IRBuilder::buildFunction(const FunctionDef& func) {
  _module->funcSig.returnType = func.returnType;
  _module->funcSig.name = func.name;
  for (const auto& p : func.params) {
    _module->funcSig.params.push_back({p.type, p.name});
  }

  prescanFunction(func);

  _scopes.clear();
  _shadowCounters.clear();
  pushScope();  // parameter/base scope
  for (const auto& p : func.params) {
    _scopes.back()[p.name] = p.name;
    _module->getVar(p.name);
  }

  _module->body = buildStmt(*func.body);
}

std::unique_ptr<StmtIR> IRBuilder::buildStmt(const Stmt& stmt) {
  switch (stmt.kind) {
    case StmtKind::Block: {
      auto block = std::make_unique<BlockIR>();
      pushScope();
      for (const auto& s : stmt.stmts) {
        block->stmts.push_back(buildStmt(*s));
      }
      popScope();
      return block;
    }
    case StmtKind::ForLoop: {
      auto forIR = std::make_unique<ForLoopIR>();
      pushScope();
      if (stmt.forInit) forIR->init = buildStmt(*stmt.forInit);
      if (stmt.forCond) forIR->cond = buildExpr(*stmt.forCond);
      if (stmt.forUpdate) {
        // Try to parse update as simple: var++, var--, var+=expr, var=var+expr
        if (stmt.forUpdate->kind == ExprKind::PostfixOp) {
          forIR->updateOp = stmt.forUpdate->op;
          forIR->update = buildExpr(*stmt.forUpdate->operand);
        } else if (stmt.forUpdate->kind == ExprKind::BinaryOp && stmt.forUpdate->lhs &&
                   stmt.forUpdate->lhs->kind == ExprKind::Variable) {
          forIR->update = buildExpr(*stmt.forUpdate->lhs);
          forIR->updateOp = stmt.forUpdate->op;
          if (stmt.forUpdate->rhs) {
            forIR->updateRhs = buildExpr(*stmt.forUpdate->rhs);
          }
        } else {
          forIR->update = buildExpr(*stmt.forUpdate);
        }
      }
      forIR->body = buildStmt(*stmt.forBody);
      popScope();
      return forIR;
    }
    case StmtKind::IfElse: {
      auto ifIR = std::make_unique<IfElseIR>();
      ifIR->cond = buildExpr(*stmt.ifCond);
      ifIR->thenBranch = buildStmt(*stmt.ifThen);
      if (stmt.ifElse) ifIR->elseBranch = buildStmt(*stmt.ifElse);
      return ifIR;
    }
    case StmtKind::VarDecl: {
      auto decl = std::make_unique<VarDeclIR>();
      decl->type = stmt.varType;
      if (stmt.init) decl->init = buildExpr(*stmt.init);
      // Declare after the initializer (initializer sees the outer binding).
      decl->name = declare(stmt.varName);
      return decl;
    }
    case StmtKind::Return: {
      auto ret = std::make_unique<ReturnIR>();
      if (stmt.retExpr) ret->value = buildExpr(*stmt.retExpr);
      return ret;
    }
    case StmtKind::Assignment: {
      auto assign = std::make_unique<AssignIR>();
      assign->target = resolve(stmt.varName);
      if (stmt.rhs) assign->value = buildExpr(*stmt.rhs);
      return assign;
    }
    case StmtKind::ExprStmt: {
      auto exprStmt = std::make_unique<ExprStmtIR>();
      if (stmt.expr) exprStmt->expr = buildExpr(*stmt.expr);
      return exprStmt;
    }
  }
  return nullptr;
}

DAGNode* IRBuilder::buildExpr(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::Number:
      return _module->createConst(expr.numVal, expr.numText);

    case ExprKind::Variable:
      return varRef(expr.name);

    case ExprKind::BinaryOp:
      return buildBinaryOp(expr);

    case ExprKind::UnaryOp:
      return buildUnaryOp(expr);

    case ExprKind::ArrayAccess:
      return buildArrayAccess(expr);

    case ExprKind::MemberAccess: {
      DAGNode* base = buildExpr(*expr.base);
      std::string root = rootName(expr);
      bool shareable = isReadOnlyRoot(root);
      return _module->createMemberAccess(base, expr.memberName, shareable);
    }

    case ExprKind::ArrowAccess: {
      DAGNode* base = buildExpr(*expr.base);
      std::string root = rootName(expr);
      bool shareable = isReadOnlyRoot(root);
      return _module->createArrowAccess(base, expr.memberName, shareable);
    }

    case ExprKind::Call:
      return buildCall(expr);

    case ExprKind::Ternary: {
      DAGNode* cond = buildExpr(*expr.cond);
      DAGNode* trueExpr = buildExpr(*expr.trueExpr);
      DAGNode* falseExpr = buildExpr(*expr.falseExpr);
      auto node = _module->createNode(NodeKind::Ternary);
      node->op = '?';
      node->operands = {cond, trueExpr, falseExpr};
      return _module->findExistingNode(node);
    }

    case ExprKind::Cast: {
      DAGNode* operand = buildExpr(*expr.operand);
      auto node = _module->createNode(NodeKind::Cast);
      node->name = expr.castType;
      node->operands = {operand};
      return _module->findExistingNode(node);
    }

    case ExprKind::PostfixOp: {
      DAGNode* operand = buildExpr(*expr.operand);
      auto node = _module->createNode(NodeKind::UnaryOp);
      node->op = expr.op;
      node->operands = {operand};
      node->name = "postfix";
      return _module->findExistingNode(node);
    }
  }
  return nullptr;
}

DAGNode* IRBuilder::buildBinaryOp(const Expr& expr) {
  DAGNode* lhs = buildExpr(*expr.lhs);
  DAGNode* rhs = buildExpr(*expr.rhs);
  return _module->createBinaryOp(expr.op, lhs, rhs);
}

DAGNode* IRBuilder::buildUnaryOp(const Expr& expr) {
  DAGNode* operand = buildExpr(*expr.operand);
  auto node = _module->createUnaryOp(expr.op, operand);
  // For ++ and -- operators, store the full operator in name field
  if (expr.name == "++" || expr.name == "--") {
    node->name = expr.name;
  }
  return node;
}

DAGNode* IRBuilder::buildArrayAccess(const Expr& expr) {
  DAGNode* base = buildExpr(*expr.base);
  DAGNode* result = base;
  std::string root = rootName(expr);
  bool shareable = isReadOnlyRoot(root);
  for (const auto& idx : expr.indices) {
    DAGNode* index = buildExpr(*idx);
    result = _module->createArrayAccess(result, index, shareable);
  }
  return result;
}

DAGNode* IRBuilder::buildCall(const Expr& expr) {
  DAGNode* callee = buildExpr(*expr.base);
  std::string calleeText;
  if (expr.base) {
    if (expr.base->kind == ExprKind::Variable) calleeText = expr.base->name;
    else if (expr.base->kind == ExprKind::MemberAccess ||
             expr.base->kind == ExprKind::ArrowAccess)
      calleeText = expr.base->memberName;
  }
  bool pure = isPureCallee(calleeText);
  std::vector<DAGNode*> args;
  for (const auto& arg : expr.callArgs) {
    args.push_back(buildExpr(*arg));
  }
  return _module->createCall(callee, args, pure);
}

}  // namespace cse
