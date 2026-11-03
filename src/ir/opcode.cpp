#include "opcode.h"

namespace cse {

std::string opSpelling(OpCode op) {
  switch (op) {
    case OpCode::None: return "";
    case OpCode::Add: return "+";
    case OpCode::Sub: return "-";
    case OpCode::Mul: return "*";
    case OpCode::Div: return "/";
    case OpCode::Mod: return "%";
    case OpCode::Shl: return "<<";
    case OpCode::Shr: return ">>";
    case OpCode::BitAnd: return "&";
    case OpCode::BitOr: return "|";
    case OpCode::BitXor: return "^";
    case OpCode::Eq: return "==";
    case OpCode::Ne: return "!=";
    case OpCode::Lt: return "<";
    case OpCode::Gt: return ">";
    case OpCode::Le: return "<=";
    case OpCode::Ge: return ">=";
    case OpCode::LogicalAnd: return "&&";
    case OpCode::LogicalOr: return "||";
    case OpCode::Neg: return "-";
    case OpCode::Pos: return "+";
    case OpCode::Not: return "!";
    case OpCode::BitNot: return "~";
    case OpCode::Assign: return "=";
    case OpCode::Inc: return "++";
    case OpCode::Dec: return "--";
    case OpCode::Ternary: return "?";
  }
  return "";
}

int opPrecedence(OpCode op) {
  switch (op) {
    case OpCode::Assign: return 1;
    case OpCode::Ternary: return 2;
    case OpCode::LogicalOr: return 3;
    case OpCode::LogicalAnd: return 4;
    case OpCode::BitOr: return 5;
    case OpCode::BitXor: return 6;
    case OpCode::BitAnd: return 7;
    case OpCode::Eq:
    case OpCode::Ne: return 8;
    case OpCode::Lt:
    case OpCode::Gt:
    case OpCode::Le:
    case OpCode::Ge: return 9;
    case OpCode::Shl:
    case OpCode::Shr: return 10;
    case OpCode::Add:
    case OpCode::Sub: return 11;
    case OpCode::Mul:
    case OpCode::Div:
    case OpCode::Mod: return 12;
    default: return 0;
  }
}

OpCode binaryOpCode(char c) {
  switch (c) {
    case '+': return OpCode::Add;
    case '-': return OpCode::Sub;
    case '*': return OpCode::Mul;
    case '/': return OpCode::Div;
    case '%': return OpCode::Mod;
    case '&': return OpCode::BitAnd;
    case '|': return OpCode::BitOr;
    case '^': return OpCode::BitXor;
    case '<': return OpCode::Lt;
    case '>': return OpCode::Gt;
    case 'l': return OpCode::Le;
    case 'g': return OpCode::Ge;
    case 'L': return OpCode::Shl;
    case 'R': return OpCode::Shr;
    case 'e': return OpCode::Eq;
    case 'n': return OpCode::Ne;
    case 'O': return OpCode::LogicalOr;
    case 'A': return OpCode::LogicalAnd;
    case '=': return OpCode::Assign;
    default: return OpCode::None;
  }
}

OpCode unaryOpCode(char c) {
  switch (c) {
    case '-': return OpCode::Neg;
    case '+': return OpCode::Pos;
    case '!': return OpCode::Not;
    case '~': return OpCode::BitNot;
    default: return OpCode::None;
  }
}

OpCode assignOpCode(char c) {
  if (c == '=') return OpCode::Assign;
  return binaryOpCode(c);
}

OpCode incDecOpCode(const std::string& spelling) {
  if (spelling == "++") return OpCode::Inc;
  if (spelling == "--") return OpCode::Dec;
  return OpCode::None;
}

bool isIncDecOp(OpCode op) {
  return op == OpCode::Inc || op == OpCode::Dec;
}

bool isComparisonOp(OpCode op) {
  switch (op) {
    case OpCode::Eq:
    case OpCode::Ne:
    case OpCode::Lt:
    case OpCode::Gt:
    case OpCode::Le:
    case OpCode::Ge:
      return true;
    default:
      return false;
  }
}

}  // namespace cse
