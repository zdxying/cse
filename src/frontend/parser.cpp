#include "parser.h"

#include <sstream>
#include <stdexcept>

namespace cse {

Parser::Parser(const std::vector<Token>& tokens) : _tokens(tokens) {}

Token Parser::peek() const { return _tokens[_pos]; }

Token Parser::advance() { return _tokens[_pos++]; }

bool Parser::check(TokenType type) const { return peek().type == type; }

bool Parser::match(TokenType type) {
  if (check(type)) {
    advance();
    return true;
  }
  return false;
}

Token Parser::expect(TokenType type) {
  if (check(type)) return advance();
  std::ostringstream oss;
  oss << "Line " << peek().line << ":" << peek().col << " expected "
      << tokenTypeName(type) << " but got " << tokenTypeName(peek().type) << " '"
      << peek().text << "'";
  throw std::runtime_error(oss.str());
}

SourceLoc Parser::currentLoc() const {
  auto t = peek();
  return {t.line, t.col};
}

bool Parser::isTypeKeyword() const {
  return check(TokenType::Int) || check(TokenType::Double) || check(TokenType::Float) ||
         check(TokenType::Void);
}

std::string Parser::parseType() {
  std::string type;
  if (check(TokenType::Struct)) {
    type = advance().text;
    type += " ";
    type += expect(TokenType::Identifier).text;
  } else if (isTypeKeyword()) {
    type = advance().text;
  } else {
    type = advance().text;  // custom type name
  }
  // Handle template arguments: vector<T>, map<string, int>, etc.
  if (check(TokenType::Less)) {
    size_t saved = _pos;
    advance();  // consume <
    type += "<";
    int depth = 1;
    while (!check(TokenType::Eof) && depth > 0) {
      if (check(TokenType::Less)) depth++;
      if (check(TokenType::Greater)) depth--;
      if (depth > 0) {
        type += advance().text;
        if (!check(TokenType::Eof) && depth > 0) type += " ";
      }
    }
    if (depth == 0) {
      advance();  // consume >
      type += ">";
    } else {
      // Not a valid template argument list, backtrack
      _pos = saved;
      type = type.substr(0, type.find('<'));
    }
  }
  // Handle pointer types: double*, int*, etc.
  while (match(TokenType::Star)) {
    type += "*";
  }
  return type;
}

// ===== Expression parsing =====

std::unique_ptr<Expr> Parser::parseExpr() { return parseAssignment(); }

std::unique_ptr<Expr> Parser::parseAssignment() {
  auto lhs = parseTernary();
  if (check(TokenType::Assign) || check(TokenType::PlusAssign) ||
      check(TokenType::MinusAssign) || check(TokenType::StarAssign) ||
      check(TokenType::SlashAssign)) {
    auto op = advance();
    auto rhs = parseAssignment();
    auto expr = std::make_unique<Expr>(ExprKind::BinaryOp, lhs->loc);
    expr->op = op.text[0];
    expr->lhs = std::move(lhs);
    expr->rhs = std::move(rhs);
    return expr;
  }
  return lhs;
}

std::unique_ptr<Expr> Parser::parseTernary() {
  auto cond = parseOr();
  if (match(TokenType::Question)) {
    auto trueExpr = parseExpr();
    expect(TokenType::Colon);
    auto falseExpr = parseTernary();
    auto expr = std::make_unique<Expr>(ExprKind::Ternary, cond->loc);
    expr->cond = std::move(cond);
    expr->trueExpr = std::move(trueExpr);
    expr->falseExpr = std::move(falseExpr);
    return expr;
  }
  return cond;
}

std::unique_ptr<Expr> Parser::parseOr() {
  auto lhs = parseAnd();
  while (match(TokenType::Or)) {
    auto rhs = parseAnd();
    auto expr = std::make_unique<Expr>(ExprKind::BinaryOp, lhs->loc);
    expr->op = '|';
    expr->lhs = std::move(lhs);
    expr->rhs = std::move(rhs);
    lhs = std::move(expr);
  }
  return lhs;
}

std::unique_ptr<Expr> Parser::parseAnd() {
  auto lhs = parseEquality();
  while (match(TokenType::And)) {
    auto rhs = parseEquality();
    auto expr = std::make_unique<Expr>(ExprKind::BinaryOp, lhs->loc);
    expr->op = '&';
    expr->lhs = std::move(lhs);
    expr->rhs = std::move(rhs);
    lhs = std::move(expr);
  }
  return lhs;
}

std::unique_ptr<Expr> Parser::parseEquality() {
  auto lhs = parseComparison();
  while (check(TokenType::Equal) || check(TokenType::NotEqual)) {
    auto op = advance();
    auto rhs = parseComparison();
    auto expr = std::make_unique<Expr>(ExprKind::BinaryOp, lhs->loc);
    expr->op = (op.type == TokenType::Equal) ? '=' : '!';
    expr->lhs = std::move(lhs);
    expr->rhs = std::move(rhs);
    lhs = std::move(expr);
  }
  return lhs;
}

std::unique_ptr<Expr> Parser::parseComparison() {
  auto lhs = parseAddSub();
  while (check(TokenType::Less) || check(TokenType::Greater) ||
         check(TokenType::LessEqual) || check(TokenType::GreaterEqual)) {
    auto op = advance();
    auto rhs = parseAddSub();
    auto expr = std::make_unique<Expr>(ExprKind::BinaryOp, lhs->loc);
    expr->op = op.text[0];
    expr->lhs = std::move(lhs);
    expr->rhs = std::move(rhs);
    lhs = std::move(expr);
  }
  return lhs;
}

std::unique_ptr<Expr> Parser::parseAddSub() {
  auto lhs = parseMulDiv();
  while (check(TokenType::Plus) || check(TokenType::Minus)) {
    auto op = advance();
    auto rhs = parseMulDiv();
    auto expr = std::make_unique<Expr>(ExprKind::BinaryOp, lhs->loc);
    expr->op = op.text[0];
    expr->lhs = std::move(lhs);
    expr->rhs = std::move(rhs);
    lhs = std::move(expr);
  }
  return lhs;
}

std::unique_ptr<Expr> Parser::parseMulDiv() {
  auto lhs = parseUnary();
  while (check(TokenType::Star) || check(TokenType::Slash) || check(TokenType::Percent)) {
    auto op = advance();
    auto rhs = parseUnary();
    auto expr = std::make_unique<Expr>(ExprKind::BinaryOp, lhs->loc);
    expr->op = op.text[0];
    expr->lhs = std::move(lhs);
    expr->rhs = std::move(rhs);
    lhs = std::move(expr);
  }
  return lhs;
}

std::unique_ptr<Expr> Parser::parseUnary() {
  if (check(TokenType::Minus) || check(TokenType::Not)) {
    auto op = advance();
    auto operand = parseUnary();
    auto expr = std::make_unique<Expr>(ExprKind::UnaryOp, currentLoc());
    expr->op = op.text[0];
    expr->operand = std::move(operand);
    expr->prefix = true;
    return expr;
  }
  // Cast: (type)expr
  if (check(TokenType::LParen)) {
    size_t saved = _pos;
    advance();  // (
    if (check(TokenType::Int) || check(TokenType::Double) || check(TokenType::Float)) {
      auto typeTok = advance();
      if (match(TokenType::RParen)) {
        auto operand = parseUnary();
        auto expr = std::make_unique<Expr>(ExprKind::Cast, currentLoc());
        expr->castType = typeTok.text;
        expr->operand = std::move(operand);
        return expr;
      }
    }
    _pos = saved;  // backtrack
  }
  return parsePostfix();
}

std::unique_ptr<Expr> Parser::parsePostfix() {
  auto expr = parsePrimary();
  while (true) {
    if (match(TokenType::LBrack)) {
      auto idx = parseExpr();
      expect(TokenType::RBrack);
      auto arr = std::make_unique<Expr>(ExprKind::ArrayAccess, expr->loc);
      arr->base = std::move(expr);
      arr->indices.push_back(std::move(idx));
      expr = std::move(arr);
    } else if (match(TokenType::Dot)) {
      auto member = expect(TokenType::Identifier);
      auto acc = std::make_unique<Expr>(ExprKind::MemberAccess, expr->loc);
      acc->base = std::move(expr);
      acc->memberName = member.text;
      expr = std::move(acc);
    } else if (match(TokenType::Arrow)) {
      auto member = expect(TokenType::Identifier);
      auto acc = std::make_unique<Expr>(ExprKind::ArrowAccess, expr->loc);
      acc->base = std::move(expr);
      acc->memberName = member.text;
      expr = std::move(acc);
    } else if (match(TokenType::LParen)) {
      auto call = std::make_unique<Expr>(ExprKind::Call, expr->loc);
      call->base = std::move(expr);
      if (!check(TokenType::RParen)) {
        do {
          call->callArgs.push_back(parseExpr());
        } while (match(TokenType::Comma));
      }
      expect(TokenType::RParen);
      expr = std::move(call);
    } else {
      break;
    }
  }
  return expr;
}

std::unique_ptr<Expr> Parser::parsePrimary() {
  if (check(TokenType::Number)) {
    auto tok = advance();
    auto expr = std::make_unique<Expr>(ExprKind::Number, currentLoc());
    expr->numVal = tok.numVal;
    expr->numText = tok.text;
    return expr;
  }
  if (check(TokenType::Identifier)) {
    auto tok = advance();
    auto expr = std::make_unique<Expr>(ExprKind::Variable, currentLoc());
    expr->name = tok.text;
    return expr;
  }
  if (match(TokenType::LParen)) {
    auto expr = parseExpr();
    expect(TokenType::RParen);
    return expr;
  }
  std::ostringstream oss;
  oss << "Line " << peek().line << ":" << peek().col << " unexpected token '"
      << peek().text << "'";
  throw std::runtime_error(oss.str());
}

// ===== Statement parsing =====

std::unique_ptr<Stmt> Parser::parseStmt() {
  if (check(TokenType::LBrace)) return parseBlock();
  if (check(TokenType::For)) return parseFor();
  if (check(TokenType::If)) return parseIf();
  if (check(TokenType::Return)) return parseReturn();
  if (check(TokenType::Int) || check(TokenType::Double) || check(TokenType::Float) ||
      check(TokenType::Struct)) {
    return parseVarDecl();
  }
  // Handle identifier as type: T x, MyStruct s, etc.
  // Heuristic: if we see Identifier followed by Identifier, treat as var decl
  if (check(TokenType::Identifier) && _pos + 1 < _tokens.size() &&
      _tokens[_pos + 1].type == TokenType::Identifier) {
    return parseVarDecl();
  }
  return parseExprStmt();
}

std::unique_ptr<Stmt> Parser::parseBlock() {
  auto loc = currentLoc();
  expect(TokenType::LBrace);
  auto block = std::make_unique<Stmt>(StmtKind::Block, loc);
  while (!check(TokenType::RBrace) && !check(TokenType::Eof)) {
    block->stmts.push_back(parseStmt());
  }
  expect(TokenType::RBrace);
  return block;
}

std::unique_ptr<Stmt> Parser::parseFor() {
  auto loc = currentLoc();
  expect(TokenType::For);
  expect(TokenType::LParen);

  auto forStmt = std::make_unique<Stmt>(StmtKind::ForLoop, loc);

  // for init
  if (check(TokenType::Int) || check(TokenType::Double) || check(TokenType::Float) ||
      check(TokenType::Struct)) {
    forStmt->forInit = parseVarDecl();
  } else {
    forStmt->forInit = parseExprStmt();
  }

  // for condition
  if (!check(TokenType::Semicolon)) {
    forStmt->forCond = parseExpr();
  }
  expect(TokenType::Semicolon);

  // for update
  if (!check(TokenType::RParen)) {
    forStmt->forUpdate = parseExpr();
  }
  expect(TokenType::RParen);

  forStmt->forBody = parseStmt();
  return forStmt;
}

std::unique_ptr<Stmt> Parser::parseIf() {
  auto loc = currentLoc();
  expect(TokenType::If);
  expect(TokenType::LParen);

  auto ifStmt = std::make_unique<Stmt>(StmtKind::IfElse, loc);
  ifStmt->ifCond = parseExpr();
  expect(TokenType::RParen);
  ifStmt->ifThen = parseStmt();

  if (match(TokenType::Else)) {
    ifStmt->ifElse = parseStmt();
  }
  return ifStmt;
}

std::unique_ptr<Stmt> Parser::parseVarDecl() {
  auto typeStr = parseType();
  auto nameTok = expect(TokenType::Identifier);
  auto decl = std::make_unique<Stmt>(StmtKind::VarDecl, currentLoc());
  decl->varType = typeStr;
  decl->varName = nameTok.text;

  if (match(TokenType::Assign)) {
    decl->init = parseExpr();
  }
  expect(TokenType::Semicolon);
  return decl;
}

std::unique_ptr<Stmt> Parser::parseReturn() {
  auto loc = currentLoc();
  expect(TokenType::Return);
  auto ret = std::make_unique<Stmt>(StmtKind::Return, loc);
  if (!check(TokenType::Semicolon)) {
    ret->retExpr = parseExpr();
  }
  expect(TokenType::Semicolon);
  return ret;
}

std::unique_ptr<Stmt> Parser::parseExprStmt() {
  auto expr = parseExpr();
  expect(TokenType::Semicolon);
  auto stmt = std::make_unique<Stmt>(StmtKind::ExprStmt, expr->loc);
  stmt->expr = std::move(expr);
  return stmt;
}

// ===== Function parsing =====

std::vector<FunctionDef::Param> Parser::parseParamList() {
  std::vector<FunctionDef::Param> params;
  expect(TokenType::LParen);
  if (!check(TokenType::RParen)) {
    do {
      FunctionDef::Param p;
      p.type = parseType();
      p.name = expect(TokenType::Identifier).text;
      params.push_back(std::move(p));
    } while (match(TokenType::Comma));
  }
  expect(TokenType::RParen);
  return params;
}

std::vector<TemplateParam> Parser::parseTemplateParams() {
  std::vector<TemplateParam> params;
  expect(TokenType::Less);
  do {
    TemplateParam tp;
    if (check(TokenType::Identifier) &&
        (peek().text == "typename" || peek().text == "class")) {
      // typename/class T
      tp.isType = true;
      tp.paramType = advance().text;
      tp.paramName = expect(TokenType::Identifier).text;
    } else if (isTypeKeyword() || check(TokenType::Identifier)) {
      // non-type: int N, size_t N, etc.
      tp.isType = false;
      tp.paramType = parseType();
      tp.paramName = expect(TokenType::Identifier).text;
      if (match(TokenType::Assign)) {
        // default value: read until comma or >
        while (!check(TokenType::Greater) && !check(TokenType::Comma) && !check(TokenType::Eof)) {
          tp.defaultVal += advance().text;
        }
      }
    } else {
      tp.isType = true;
      tp.paramType = "typename";
      tp.paramName = expect(TokenType::Identifier).text;
    }
    params.push_back(std::move(tp));
  } while (match(TokenType::Comma));
  expect(TokenType::Greater);
  return params;
}

std::unique_ptr<FunctionDef> Parser::parseFunction() {
  auto func = std::make_unique<FunctionDef>();
  func->loc = currentLoc();

  // Handle template prefix
  if (check(TokenType::Template)) {
    advance();  // consume 'template'
    func->templateParams = parseTemplateParams();
  }

  func->returnType = parseType();
  func->name = expect(TokenType::Identifier).text;
  func->params = parseParamList();
  func->body = parseBlock();

  return func;
}

std::unique_ptr<StructDef> Parser::parseStructDef() {
  auto loc = currentLoc();

  // Handle template prefix
  std::vector<TemplateParam> templateParams;
  if (check(TokenType::Template)) {
    advance();  // consume 'template'
    templateParams = parseTemplateParams();
  }

  expect(TokenType::Struct);
  auto def = std::make_unique<StructDef>();
  def->name = expect(TokenType::Identifier).text;
  def->loc = loc;
  def->templateParams = std::move(templateParams);
  expect(TokenType::LBrace);
  while (!check(TokenType::RBrace) && !check(TokenType::Eof)) {
    auto returnType = parseType();
    auto nameTok = expect(TokenType::Identifier);

    if (check(TokenType::LParen)) {
      auto method = std::make_unique<FunctionDef>();
      method->returnType = returnType;
      method->name = nameTok.text;
      method->loc = {nameTok.line, nameTok.col};
      method->params = parseParamList();
      method->body = parseBlock();
      def->methods.push_back(std::move(method));
    } else {
      // Field declaration
      StructField field;
      field.type = returnType;
      field.name = nameTok.text;
      expect(TokenType::Semicolon);
      def->fields.push_back(std::move(field));
    }
  }
  expect(TokenType::RBrace);
  match(TokenType::Semicolon);
  return def;
}

// ===== Full parse =====

Parser::ParseResult Parser::parseAll() {
  ParseResult result;

  while (!check(TokenType::Eof)) {
    // Template prefix for struct or function
    if (check(TokenType::Template)) {
      // Look ahead to see if it's template struct or template function
      // Skip past template<...> to find what follows
      size_t ahead = _pos + 1;
      // Skip template parameter list: < ... >
      if (ahead < _tokens.size() && _tokens[ahead].type == TokenType::Less) {
        int depth = 1;
        ahead++;
        while (ahead < _tokens.size() && depth > 0) {
          if (_tokens[ahead].type == TokenType::Less) depth++;
          if (_tokens[ahead].type == TokenType::Greater) depth--;
          ahead++;
        }
      }
      // Now ahead points to the token after template<...>
      if (ahead < _tokens.size() && _tokens[ahead].type == TokenType::Struct) {
        result.structDefs.push_back(parseStructDef());
        continue;
      }
      // Template function: template<typename T> T foo(...)
      result.functions.push_back(parseFunction());
      continue;
    }

    // Struct definition
    if (check(TokenType::Struct)) {
      result.structDefs.push_back(parseStructDef());
      continue;
    }

    // Parse a function if we see: type[*] name(
    bool isFuncStart = false;
    if (isTypeKeyword()) {
      isFuncStart = true;
    } else if (check(TokenType::Identifier) && _pos + 1 < _tokens.size() &&
               _tokens[_pos + 1].type == TokenType::Identifier) {
      isFuncStart = true;
    }

    if (isFuncStart) {
      // Look ahead to find identifier then LParen
      size_t ahead = _pos;
      // Skip type tokens (including pointers)
      while (ahead < _tokens.size() && (_tokens[ahead].type == TokenType::Star ||
                                         _tokens[ahead].type == TokenType::Identifier ||
                                         (_tokens[ahead].type == TokenType::Int) ||
                                         (_tokens[ahead].type == TokenType::Double) ||
                                         (_tokens[ahead].type == TokenType::Float) ||
                                         (_tokens[ahead].type == TokenType::Void))) {
        ahead++;
      }
      // Check if we have: type [*...] name (
      if (ahead >= 2 && ahead < _tokens.size() &&
          _tokens[ahead - 1].type == TokenType::Identifier &&
          _tokens[ahead].type == TokenType::LParen) {
        result.functions.push_back(parseFunction());
        continue;
      }
    }
    // Unexpected token at top level
    std::ostringstream oss;
    oss << "Line " << peek().line << ":" << peek().col << " unexpected token '"
        << peek().text << "'";
    throw std::runtime_error(oss.str());
  }

  return result;
}

}  // namespace cse
