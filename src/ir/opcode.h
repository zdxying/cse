#pragma once
#include <string>

namespace cse {

// The classification of an operator.
//
// It replaces the frontend's character encoding, where the meaning of an
// operator lived in a bare `char`: `'e'` meant `==`, `'l'` meant `<=`, `'O'`
// meant `||` (so it would never be confused with bitwise `'|'`), and the same
// `'-'` meant both subtract and negate depending on the node kind. Every use
// site had to know that encoding. An OpCode makes the meaning a property of the
// value, so classification and emission both read from one table.
//
// The `char` survives only in the *frontend* AST (src/frontend/ast.h), which
// the parser fills from tokens; the builder translates it once, here.
enum class OpCode : int {
  None,        // no operator: leaves, calls, casts, array/member accesses
  Add, Sub, Mul, Div, Mod,
  Shl, Shr,          // `<<` / `>>`
  BitAnd, BitOr, BitXor,
  Eq, Ne, Lt, Gt, Le, Ge,
  LogicalAnd, LogicalOr,
  Neg, Pos, Not, BitNot,
  Assign,      // the `=` of an assignment expression / a plain store
  Inc, Dec,    // `++` / `--` (the spelling also lives in DAGNode::name)
  Ternary,     // `?:`
};

// The C++ spelling of an operator, for code emission. Empty for None.
std::string opSpelling(OpCode op);

// Binary-operator precedence, higher binds tighter. A non-binary code is 0, so
// an unknown operator has its operands parenthesized rather than regrouped.
int opPrecedence(OpCode op);

// Translate the frontend's character encoding into an OpCode. The same
// character means different things per context, so the context picks the entry
// point: `'-'` is Sub for a binary op but Neg for a unary one.
OpCode binaryOpCode(char c);
OpCode unaryOpCode(char c);
// The base operator of a compound assignment / a `for` update (`+=`, `-=`, ...),
// and `=` itself.
OpCode assignOpCode(char c);
// `"++"` / `"--"` (the spelling the parser keeps for an increment/decrement).
OpCode incDecOpCode(const std::string& spelling);

bool isIncDecOp(OpCode op);
bool isComparisonOp(OpCode op);

}  // namespace cse
