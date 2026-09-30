#include "ir_module.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

namespace cse {

namespace {

// Format a constant for code emission with round-trip precision.
std::string formatConst(double val) {
  if (val == std::floor(val) && std::fabs(val) < 1e15)
    return std::to_string(static_cast<long long>(val));
  std::ostringstream oss;
  oss << std::setprecision(17) << val;
  return oss.str();
}

// The operator spelling carried by an increment/decrement node. The frontend
// encodes `++` as op '+' and `--` as op '-'.
std::string incDecSpelling(char op) {
  assert(op == '+' || op == '-');
  return op == '+' ? "++" : "--";
}

}  // namespace

// ===== DAGNode =====

DAGNode::DAGNode(NodeKind k, uint32_t id) : kind(k), id(id), hash(0) {}

bool DAGNode::sameContentAs(const DAGNode& other) const {
  if (this == &other) return true;
  // ---- the identity field list; computeHash() folds exactly this ----
  if (kind != other.kind) return false;
  if (op != other.op) return false;
  if (name != other.name) return false;
  if (pure != other.pure) return false;
  if (postfix != other.postfix) return false;
  if (operands.size() != other.operands.size()) return false;
  for (size_t i = 0; i < operands.size(); ++i) {
    // Operands are compared by identity. Interning guarantees that equal
    // content means the same node, so comparing the ids is exact -- and two
    // nodes with equal content that must stay distinct (an unshared load, an
    // `x++` in each of two iterations) are distinct nodes precisely because
    // they were never interned.
    if (operands[i]->id != other.operands[i]->id) return false;
  }
  if (kind == NodeKind::Constant && constVal != other.constVal) return false;
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
  // Exactly the fields compared by sameContentAs().
  mix(static_cast<uint64_t>(kind));
  mix(static_cast<uint64_t>(static_cast<unsigned char>(op)));
  mix(static_cast<uint64_t>(pure));
  mix(static_cast<uint64_t>(postfix));
  mix(static_cast<uint64_t>(operands.size()));
  for (auto* opNode : operands) mix(opNode->hash);
  for (char c : name) mix(static_cast<unsigned char>(c));
  if (kind == NodeKind::Constant) {
    uint64_t valBits;
    std::memcpy(&valBits, &constVal, sizeof(valBits));
    mix(valBits);
  }
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
  auto candidate = createNode(NodeKind::Constant);
  candidate->constVal = val;
  std::string t = text.empty() ? formatConst(val) : text;
  // Render integral constants without a trailing ".0" (array indices, etc.).
  if (val == std::floor(val) && std::fabs(val) < 1e15) {
    t = std::to_string(static_cast<long long>(val));
  }
  candidate->numText = t;
  candidate->recomputeHash();
  return intern(candidate);
}

DAGNode* IRModule::createSymbolicConst(double val, const std::string& symbol) {
  auto candidate = createNode(NodeKind::Constant);
  candidate->constVal = val;
  candidate->numText = formatConst(val);
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
  return node;
}

DAGNode* IRModule::createBinaryOp(char op, DAGNode* lhs, DAGNode* rhs) {
  auto candidate = createNode(NodeKind::BinaryOp);
  candidate->op = op;
  candidate->operands = {lhs, rhs};
  candidate->recomputeHash();
  return intern(candidate);
}

DAGNode* IRModule::createUnaryOpImpl(char op, DAGNode* operand,
                                     const std::string& name, bool postfix,
                                     bool pure) {
  auto candidate = createNode(NodeKind::UnaryOp);
  candidate->op = op;
  candidate->name = name;
  candidate->postfix = postfix;
  candidate->pure = pure;
  candidate->operands = {operand};
  candidate->recomputeHash();
  return intern(candidate);
}

DAGNode* IRModule::createUnaryOp(char op, DAGNode* operand) {
  return createUnaryOpImpl(op, operand, "", /*postfix=*/false, /*pure=*/true);
}

DAGNode* IRModule::createPreIncDec(char op, DAGNode* operand) {
  return createUnaryOpImpl(op, operand, incDecSpelling(op), /*postfix=*/false,
                           /*pure=*/false);
}

DAGNode* IRModule::createPostIncDec(char op, DAGNode* operand) {
  return createUnaryOpImpl(op, operand, incDecSpelling(op), /*postfix=*/true,
                           /*pure=*/false);
}

DAGNode* IRModule::createCast(const std::string& typeName, DAGNode* operand) {
  auto candidate = createNode(NodeKind::Cast);
  candidate->name = typeName;
  candidate->operands = {operand};
  candidate->recomputeHash();
  return intern(candidate);
}

DAGNode* IRModule::createTernary(DAGNode* cond, DAGNode* trueExpr,
                                 DAGNode* falseExpr) {
  auto candidate = createNode(NodeKind::Ternary);
  candidate->op = '?';
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
