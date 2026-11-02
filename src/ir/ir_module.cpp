#include "ir_module.h"

#include <cassert>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>

namespace cse {

namespace {

// Format a constant for code emission with round-trip precision.
//
// Negative zero is not zero: `1.0 / -0.0` is -inf while `1.0 / 0.0` is +inf, so
// the sign has to survive emission. Without this the `val == floor(val)` branch
// below turns it into "0" -- an *integer* literal, which converts back to +0.0.
//
// `isFloat` keeps the type of an integral-valued constant. `4.0 / 3` is
// 1.333... while `4 / 3` is 1, so a floating constant must keep its decimal
// point even when its value is whole; an integer constant is emitted bare (and
// a subscript index is normalized to an integer by the backend).
std::string formatConst(double val, bool isFloat) {
  if (val == 0.0 && std::signbit(val)) return "-0.0";
  if (val == std::floor(val) && std::fabs(val) < 1e15) {
    std::string s = std::to_string(static_cast<long long>(val));
    return isFloat ? s + ".0" : s;
  }
  std::ostringstream oss;
  oss << std::setprecision(17) << val;
  return oss.str();
}

// Does a source spelling denote a floating-point literal? Used to recover the
// int/float kind of a literal the frontend kept verbatim (`2.0` vs `2`).
bool looksFloating(const std::string& text) {
  if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    return false;  // hexadecimal integer
  if (text.find('.') != std::string::npos) return true;
  if (text.find('e') != std::string::npos) return true;
  if (text.find('E') != std::string::npos) return true;
  const char last = text.empty() ? '\0' : text.back();
  return last == 'f' || last == 'F';
}

}  // namespace

NumericKind numericKindOfType(const std::string& type) {
  // A pointer's kind is not the kind of its pointee: a double* used in
  // arithmetic is not a floating expression. Templates/vectors are lowered per
  // component, so the vector name itself carries no scalar kind.
  std::string s;
  s.reserve(type.size());
  for (char ch : type) {
    if (ch == '<' || ch == '*' || ch == '[') return NumericKind::Unknown;
    if (ch == '&') continue;  // a reference denotes its referent's kind
    s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  auto has = [&](const char* word) { return s.find(word) != std::string::npos; };
  if (has("double") || has("float")) return NumericKind::Float;
  if (has("int") || has("long") || has("short") || has("char") ||
      has("unsigned") || has("size_t") || has("bool"))
    return NumericKind::Int;
  return NumericKind::Unknown;
}

// ===== DAGNode =====

NumericKind impliedNumericKind(const DAGNode* n) {
  if (!n) return NumericKind::Unknown;
  switch (n->kind) {
    case NodeKind::Constant:
      return n->isFloat ? NumericKind::Float : NumericKind::Int;
    case NodeKind::BinaryOp:
      if (n->op == OpCode::Add || n->op == OpCode::Sub ||
          n->op == OpCode::Mul || n->op == OpCode::Div) {
        if (n->operands.size() < 2) return NumericKind::Unknown;
        return joinNumericKind(n->operands[0]->numKind, n->operands[1]->numKind);
      }
      if (isComparisonOp(n->op) || n->op == OpCode::Mod ||
          n->op == OpCode::Shl || n->op == OpCode::Shr ||
          n->op == OpCode::BitAnd || n->op == OpCode::BitOr ||
          n->op == OpCode::BitXor || n->op == OpCode::LogicalAnd ||
          n->op == OpCode::LogicalOr)
        return NumericKind::Int;
      return NumericKind::Unknown;  // no rule; callers must not rely on one
    case NodeKind::UnaryOp:
      if (n->operands.empty()) return NumericKind::Unknown;
      if (n->op == OpCode::Neg || n->op == OpCode::Pos || isIncDecOp(n->op))
        return n->operands[0]->numKind;
      if (n->op == OpCode::Not || n->op == OpCode::BitNot)
        return NumericKind::Int;
      return NumericKind::Unknown;
    case NodeKind::Cast:
      return numericKindOfType(n->name);
    case NodeKind::Ternary:
      if (n->operands.size() < 3) return NumericKind::Unknown;
      return joinNumericKind(n->operands[1]->numKind, n->operands[2]->numKind);
    // A variable's kind comes from its declaration, and a load's or a call's
    // from the type of what it denotes; the DAG carries no rule for those, so
    // any kind (including Unknown) is consistent here.
    case NodeKind::Variable:
    case NodeKind::ArrayAccess:
    case NodeKind::MemberAccess:
    case NodeKind::ArrowAccess:
    case NodeKind::Call:
      return n->numKind;
  }
  return n->numKind;
}

namespace {

// One (compare, hash) helper pair per field named in CSE_DAGNODE_IDENTITY_FIELDS
// (dag_node.h). sameContentAs() and computeHash() below both drive these over
// that macro, so the identity field list is written once and the two functions
// cannot compare/hash different sets.

inline bool identityEqual_kind(const DAGNode& a, const DAGNode& b) {
  return a.kind == b.kind;
}
inline uint64_t identityHash_kind(const DAGNode& n) {
  return static_cast<uint64_t>(n.kind);
}

inline bool identityEqual_op(const DAGNode& a, const DAGNode& b) {
  return a.op == b.op;
}
inline uint64_t identityHash_op(const DAGNode& n) {
  return static_cast<uint64_t>(n.op);
}

inline bool identityEqual_name(const DAGNode& a, const DAGNode& b) {
  return a.name == b.name;
}
inline uint64_t identityHash_name(const DAGNode& n) {
  // FNV-1a over the bytes of the name. The caller folds this value into the
  // node hash, which keeps the overall fold order-sensitive.
  uint64_t h = 0xcbf29ce484222325ULL;
  for (char c : n.name) {
    h ^= static_cast<unsigned char>(c);
    h *= 0x100000001b3ULL;
  }
  return h;
}

inline bool identityEqual_pure(const DAGNode& a, const DAGNode& b) {
  return a.pure == b.pure;
}
inline uint64_t identityHash_pure(const DAGNode& n) {
  return static_cast<uint64_t>(n.pure);
}

inline bool identityEqual_postfix(const DAGNode& a, const DAGNode& b) {
  return a.postfix == b.postfix;
}
inline uint64_t identityHash_postfix(const DAGNode& n) {
  return static_cast<uint64_t>(n.postfix);
}

// Operands are compared by identity. Interning guarantees that equal content
// means the same node, so comparing the ids is exact -- and two nodes with equal
// content that must stay distinct (an unshared load, an `x++` in each of two
// iterations) are distinct nodes precisely because they were never interned.
inline bool identityEqual_operands(const DAGNode& a, const DAGNode& b) {
  if (a.operands.size() != b.operands.size()) return false;
  for (size_t i = 0; i < a.operands.size(); ++i)
    if (a.operands[i]->id != b.operands[i]->id) return false;
  return true;
}
inline uint64_t identityHash_operands(const DAGNode& n) {
  uint64_t h = 0xcbf29ce484222325ULL;
  auto step = [&h](uint64_t v) {
    h ^= v;
    h *= 0x100000001b3ULL;
  };
  step(static_cast<uint64_t>(n.operands.size()));
  for (const DAGNode* op : n.operands) step(op->hash);
  return h;
}

// A constant's value is compared by its raw bits, so `-0.0` and `+0.0` stay
// distinct, and its int/float kind is part of the identity (`2.0` and `2` are
// different constants; see dag_node.h). A non-constant node has no constant
// payload, so this field is vacuous for it.
inline bool identityEqual_constant(const DAGNode& a, const DAGNode& b) {
  if (a.kind != NodeKind::Constant) return true;
  uint64_t ab, bb;
  std::memcpy(&ab, &a.constVal, sizeof(ab));
  std::memcpy(&bb, &b.constVal, sizeof(bb));
  if (ab != bb) return false;
  return a.isFloat == b.isFloat;
}
inline uint64_t identityHash_constant(const DAGNode& n) {
  uint64_t h = 0xcbf29ce484222325ULL;
  if (n.kind != NodeKind::Constant) return h;
  uint64_t bits;
  std::memcpy(&bits, &n.constVal, sizeof(bits));
  h ^= bits;
  h *= 0x100000001b3ULL;
  h ^= static_cast<uint64_t>(n.isFloat);
  h *= 0x100000001b3ULL;
  return h;
}

}  // namespace

DAGNode::DAGNode(NodeKind k, uint32_t id) : kind(k), id(id), hash(0) {}

bool DAGNode::sameContentAs(const DAGNode& other) const {
  if (this == &other) return true;
  // The identity field list, in one place (dag_node.h): this function and
  // computeHash() expand the same macro, so the set of fields compared here is
  // the set hashed there.
#define CSE_IDENTITY_COMPARE(field) \
  if (!identityEqual_##field(*this, other)) return false;
  CSE_DAGNODE_IDENTITY_FIELDS(CSE_IDENTITY_COMPARE)
#undef CSE_IDENTITY_COMPARE
  return true;
}

uint64_t DAGNode::computeHash() const {
  // FNV-1a over an order-sensitive byte stream. std::hash<uint64_t> is the
  // identity on libstdc++, so folding child hashes with it would make the hash
  // order-insensitive and collision-prone (`a*b` and `b*a`, or unrelated
  // structures, would collide).
  uint64_t h = 0xcbf29ce484222325ULL;
  auto mix = [&h](uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (i * 8)) & 0xffULL;
      h *= 0x100000001b3ULL;
    }
  };
  // Exactly the fields sameContentAs() compares -- the same macro.
#define CSE_IDENTITY_HASH(field) mix(identityHash_##field(*this));
  CSE_DAGNODE_IDENTITY_FIELDS(CSE_IDENTITY_HASH)
#undef CSE_IDENTITY_HASH
  return h;
}

// ===== NodeEqual =====

bool NodeEqual::operator()(const DAGNode* a, const DAGNode* b) const {
  if (a == b) return true;
  // The hash is a cheap and safe first cut: equal content always hashes equal,
  // so a mismatch proves inequality.
  if (a->hash != b->hash) return false;
  return a->sameContentAs(*b);
}

// ===== IRModule =====

DAGNode* IRModule::createNode(NodeKind kind) {
  auto node = std::make_unique<DAGNode>(kind, _next_node_id++);
  DAGNode* ptr = node.get();
  _node_pool.push_back(std::move(node));
  return ptr;
}

DAGNode* IRModule::intern(DAGNode* candidate) {
  // The one place that decides what may be shared. A node whose value is not
  // referentially transparent -- an impure call, a load that may observe a
  // store, `++`/`--` -- is handed back unregistered, so it can never be
  // returned in place of a different occurrence. Centralising the policy here
  // (every factory used to spell it out, or forget to) is also what makes
  // `pure` a usable invariant for verify().
  if (!candidate->pure) return candidate;

  // Bucket by hash and confirm structural equality. A bucket (rather than a
  // single pointer) keeps distinct nodes that happen to share a hash from
  // evicting each other, which would otherwise silently disable sharing.
  std::vector<DAGNode*>& bucket = _hash_map[candidate->hash];
  NodeEqual eq;
  for (DAGNode* existing : bucket) {
    if (eq(existing, candidate)) return existing;
  }
  bucket.push_back(candidate);
  return candidate;
}

DAGNode* IRModule::createConst(double val, const std::string& text) {
  // The source spelling, when there is one, tells the int/float kind directly;
  // otherwise a whole value is an integer and anything else is floating.
  bool isFloat = text.empty() ? (val != std::floor(val)) : looksFloating(text);
  return createConstTyped(val, isFloat, text);
}

DAGNode* IRModule::createConstTyped(double val, bool isFloat,
                                    const std::string& text) {
  auto candidate = createNode(NodeKind::Constant);
  candidate->constVal = val;
  candidate->isFloat = isFloat;
  candidate->numKind = isFloat ? NumericKind::Float : NumericKind::Int;
  // A caller that knows the source spelling keeps it. Re-deriving the text from
  // the double loses information the source had: `1e16` becomes a 17-digit
  // integer, and `-0.0` becomes `-0` -- an integer literal again, i.e. +0.0 --
  // which silently changed the value of `1.0 / -0.0`. Only a constant that
  // nobody spelled is formatted here, and formatConst keeps a floating
  // constant's decimal point.
  candidate->numText = text.empty() ? formatConst(val, isFloat) : text;
  candidate->recomputeHash();
  return intern(candidate);
}

DAGNode* IRModule::createSymbolicConst(double val, const std::string& symbol) {
  auto candidate = createNode(NodeKind::Constant);
  candidate->constVal = val;
  candidate->isFloat = (val != std::floor(val));
  candidate->numKind = candidate->isFloat ? NumericKind::Float : NumericKind::Int;
  candidate->numText = formatConst(val, candidate->isFloat);
  candidate->symbol = symbol;
  candidate->recomputeHash();

  DAGNode* existing = intern(candidate);
  if (existing != candidate && existing->symbol.empty() && !symbol.empty()) {
    // `symbol` is presentation, not identity (see dag_node.h): the node keeps
    // its value and its hash, and only starts being *emitted* as the declared
    // accessor. That is why this write cannot invalidate the hash.
    existing->symbol = symbol;
  }
  return existing;
}

DAGNode* IRModule::getVar(const std::string& name) {
  auto it = _var_cache.find(name);
  if (it != _var_cache.end()) return it->second;

  auto node = createNode(NodeKind::Variable);
  node->name = name;
  node->recomputeHash();
  _var_cache[name] = node;
  _reserved_names.insert(name);
  auto kind = _var_kinds.find(name);
  if (kind != _var_kinds.end()) node->numKind = kind->second;
  return node;
}

void IRModule::reserveName(const std::string& name) {
  if (!name.empty()) _reserved_names.insert(name);
}

std::string IRModule::freshName(const std::string& base) {
  // `base` itself first, then `base_1`, `base_2`, ... . The suffix carries no
  // meaning; it only has to be unused. Reserving the winner is what makes two
  // calls with the same base return different names.
  if (base.empty()) return base;
  if (!_reserved_names.count(base)) {
    _reserved_names.insert(base);
    return base;
  }
  for (int n = 1;; ++n) {
    std::string candidate = base + "_" + std::to_string(n);
    if (!_reserved_names.count(candidate)) {
      _reserved_names.insert(candidate);
      return candidate;
    }
  }
}

void IRModule::setVarKind(const std::string& name, NumericKind kind) {
  _var_kinds[name] = kind;
  auto it = _var_cache.find(name);
  if (it != _var_cache.end()) it->second->numKind = kind;
}

DAGNode* IRModule::createBinaryOp(OpCode op, DAGNode* lhs, DAGNode* rhs) {
  auto candidate = createNode(NodeKind::BinaryOp);
  candidate->op = op;
  candidate->operands = {lhs, rhs};
  if (op == OpCode::Add || op == OpCode::Sub || op == OpCode::Mul ||
      op == OpCode::Div) {
    candidate->numKind = joinNumericKind(lhs->numKind, rhs->numKind);
  } else if (isComparisonOp(op) || op == OpCode::Mod ||
             op == OpCode::Shl || op == OpCode::Shr ||
             op == OpCode::BitAnd || op == OpCode::BitOr ||
             op == OpCode::BitXor || op == OpCode::LogicalAnd ||
             op == OpCode::LogicalOr) {
    // Comparisons and the bitwise/logical operators yield an integer (0/1 or
    // a bit pattern), never the operands' floating kind.
    candidate->numKind = NumericKind::Int;
  }
  candidate->recomputeHash();
  return intern(candidate);
}

DAGNode* IRModule::createUnaryOpImpl(OpCode op, DAGNode* operand,
                                     const std::string& name, bool postfix,
                                     bool pure) {
  auto candidate = createNode(NodeKind::UnaryOp);
  candidate->op = op;
  candidate->name = name;
  candidate->postfix = postfix;
  candidate->pure = pure;
  candidate->operands = {operand};
  if (op == OpCode::Neg || op == OpCode::Pos || isIncDecOp(op)) {
    candidate->numKind = operand->numKind;
  } else if (op == OpCode::Not || op == OpCode::BitNot) {
    candidate->numKind = NumericKind::Int;
  }
  candidate->recomputeHash();
  return intern(candidate);
}

DAGNode* IRModule::createUnaryOp(OpCode op, DAGNode* operand) {
  return createUnaryOpImpl(op, operand, "", /*postfix=*/false, /*pure=*/true);
}

DAGNode* IRModule::createPreIncDec(OpCode op, DAGNode* operand) {
  return createUnaryOpImpl(op, operand, opSpelling(op), /*postfix=*/false,
                           /*pure=*/false);
}

DAGNode* IRModule::createPostIncDec(OpCode op, DAGNode* operand) {
  return createUnaryOpImpl(op, operand, opSpelling(op), /*postfix=*/true,
                           /*pure=*/false);
}

DAGNode* IRModule::createCast(const std::string& typeName, DAGNode* operand) {
  auto candidate = createNode(NodeKind::Cast);
  candidate->name = typeName;
  candidate->numKind = numericKindOfType(typeName);
  candidate->operands = {operand};
  candidate->recomputeHash();
  return intern(candidate);
}

DAGNode* IRModule::createTernary(DAGNode* cond, DAGNode* trueExpr,
                                 DAGNode* falseExpr) {
  auto candidate = createNode(NodeKind::Ternary);
  candidate->op = OpCode::Ternary;
  candidate->numKind = joinNumericKind(trueExpr->numKind, falseExpr->numKind);
  candidate->operands = {cond, trueExpr, falseExpr};
  candidate->recomputeHash();
  return intern(candidate);
}

DAGNode* IRModule::createArrayAccess(DAGNode* base, DAGNode* index,
                                     bool shareable) {
  auto candidate = createNode(NodeKind::ArrayAccess);
  candidate->operands = {base, index};
  candidate->pure = shareable;
  candidate->recomputeHash();
  return intern(candidate);
}

DAGNode* IRModule::createMemberAccess(DAGNode* base, const std::string& member,
                                      bool shareable) {
  auto candidate = createNode(NodeKind::MemberAccess);
  candidate->name = member;
  candidate->operands = {base};
  candidate->pure = shareable;
  candidate->recomputeHash();
  return intern(candidate);
}

DAGNode* IRModule::createArrowAccess(DAGNode* base, const std::string& member,
                                     bool shareable) {
  auto candidate = createNode(NodeKind::ArrowAccess);
  candidate->name = member;
  candidate->operands = {base};
  candidate->pure = shareable;
  candidate->recomputeHash();
  return intern(candidate);
}

DAGNode* IRModule::createCall(DAGNode* callee, const std::vector<DAGNode*>& args,
                              bool pure) {
  auto candidate = createNode(NodeKind::Call);
  candidate->operands.push_back(callee);
  for (auto* a : args) candidate->operands.push_back(a);
  candidate->pure = pure;
  candidate->recomputeHash();
  // Impure calls are kept distinct: sharing them could drop or reorder effects.
  return intern(candidate);
}

void IRModule::verify() const {
#ifndef NDEBUG
  // Every AssignIR must represent its target exactly once (see
  // AssignIR::hasExactlyOneTarget). The dual representation is deliberate --
  // a bare name is cheap to compare, an expression is the general form -- but
  // "both set" or "neither set" means some pass rewrote one form and left the
  // other stale, which every consumer that checks only one of them would then
  // silently misread.
  {
    std::function<void(const StmtIR*)> check = [&](const StmtIR* s) {
      if (!s) return;
      if (s->kind == StmtIRKind::Assign) {
        assert(static_cast<const AssignIR*>(s)->hasExactlyOneTarget() &&
               "an AssignIR must carry exactly one target representation "
               "(a non-empty `target` xor a non-null `targetExpr`)");
      }
      switch (s->kind) {
        case StmtIRKind::Block:
          for (const auto& sub : static_cast<const BlockIR*>(s)->stmts)
            check(sub.get());
          break;
        case StmtIRKind::ForLoop:
          check(static_cast<const ForLoopIR*>(s)->init.get());
          check(static_cast<const ForLoopIR*>(s)->body.get());
          break;
        case StmtIRKind::IfElse: {
          const auto* ie = static_cast<const IfElseIR*>(s);
          check(ie->thenBranch.get());
          check(ie->elseBranch.get());
          break;
        }
        case StmtIRKind::ExprStmt:
        case StmtIRKind::Assign:
        case StmtIRKind::VarDecl:
        case StmtIRKind::Return:
          break;
      }
    };
    check(body.get());
  }

  // Every node the module owns must be structurally sound: no dangling
  // operand, and a hash that still describes the node. A pass that mutated an
  // interned node in place (the defect this whole file is built to prevent)
  // shows up here as a hash mismatch even when the node was never re-interned.
  for (const auto& owned : _node_pool) {
    const DAGNode* n = owned.get();
    assert(n->hash == n->computeHash() &&
           "a node's hash no longer describes it: a semantic field was written "
           "after the node was built");
    // `numKind` is derived metadata, not identity, so a mismatch does not
    // corrupt the hash -- it silently changes which rewrites fire. Every kind a
    // node's shape determines must still agree with the operands (see
    // impliedNumericKind).
    assert(n->numKind == impliedNumericKind(n) &&
           "a node's numeric kind no longer follows from its operands: a "
           "derived field was written after the node was built, or a factory "
           "forgot to compute it");
    for (const DAGNode* op : n->operands) {
      assert(op != nullptr && "a node has a null operand");
    }
  }

  for (const auto& entry : _hash_map) {
    const std::vector<DAGNode*>& bucket = entry.second;
    for (size_t i = 0; i < bucket.size(); ++i) {
      const DAGNode* n = bucket[i];
      assert(n->pure && "an unshareable node reached the hash map");
      assert(n->hash == n->computeHash() &&
             "an interned node's hash no longer describes it: a semantic field "
             "was written after the node was interned");
      for (size_t j = i + 1; j < bucket.size(); ++j) {
        assert(!n->sameContentAs(*bucket[j]) &&
               "two distinct interned nodes denote the same node: either a "
               "factory skipped interning, or the identity misses a field");
      }
    }
  }
#endif
}

}  // namespace cse
