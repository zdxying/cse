#include "ir_module.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>

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

}  // namespace

// ===== DAGNode =====

DAGNode::DAGNode(NodeKind k, uint32_t id) : kind(k), id(id), hash(0) {}

void DAGNode::recomputeHash() {
  std::hash<uint64_t> hasher;
  uint64_t h = hasher(static_cast<uint64_t>(kind));
  h = hasher(h + static_cast<uint64_t>(op));
  h = hasher(h + static_cast<uint64_t>(operands.size()));
  for (auto* op : operands) {
    h = hasher(h + op->hash);
  }
  if (!name.empty()) {
    for (char c : name) h = hasher(h + c);
  }
  if (kind == NodeKind::Constant) {
    uint64_t valBits;
    std::memcpy(&valBits, &constVal, sizeof(valBits));
    h = hasher(h + valBits);
  }
  hash = h;
}

// ===== NodeEqual =====

bool NodeEqual::operator()(const DAGNode* a, const DAGNode* b) const {
  if (a->hash != b->hash) return false;
  if (a->kind != b->kind) return false;
  if (a->op != b->op) return false;
  if (a->operands.size() != b->operands.size()) return false;
  for (size_t i = 0; i < a->operands.size(); i++) {
    if (a->operands[i]->id != b->operands[i]->id) return false;
  }
  if (a->kind == NodeKind::Constant && a->constVal != b->constVal) return false;
  if (a->kind == NodeKind::Variable && a->name != b->name) return false;
  if (a->kind == NodeKind::MemberAccess && a->name != b->name) return false;
  if (a->kind == NodeKind::ArrowAccess && a->name != b->name) return false;
  return true;
}

// ===== IRModule =====

DAGNode* IRModule::createNode(NodeKind kind) {
  auto node = std::make_unique<DAGNode>(kind, _next_node_id++);
  DAGNode* ptr = node.get();
  _node_pool.push_back(std::move(node));
  return ptr;
}

DAGNode* IRModule::createConst(double val, const std::string& text) {
  auto node = createNode(NodeKind::Constant);
  node->constVal = val;
  std::string t = text.empty() ? formatConst(val) : text;
  // Render integral constants without a trailing ".0" (array indices, etc.).
  if (val == std::floor(val) && std::fabs(val) < 1e15) {
    t = std::to_string(static_cast<long long>(val));
  }
  node->numText = t;
  node->recomputeHash();
  // Deduplicate constants by value so that expressions built around the same
  // literal share a DAG node (enables cross-statement CSE).
  return findExistingNode(node);
}

DAGNode* IRModule::createSymbolicConst(double val, const std::string& symbol) {
  auto candidate = createNode(NodeKind::Constant);
  candidate->constVal = val;
  candidate->numText = formatConst(val);
  candidate->symbol = symbol;
  candidate->recomputeHash();

  DAGNode* existing = findExistingNode(candidate);
  if (existing != candidate && existing->symbol.empty() && !symbol.empty()) {
    // Keep the numeric form for analysis, but reuse the declared symbol for
    // code emission on the canonical (value-equal) node.
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
  return findExistingNode(candidate);
}

DAGNode* IRModule::createUnaryOp(char op, DAGNode* operand) {
  auto candidate = createNode(NodeKind::UnaryOp);
  candidate->op = op;
  candidate->operands = {operand};
  candidate->recomputeHash();
  return findExistingNode(candidate);
}

DAGNode* IRModule::createArrayAccess(DAGNode* base, DAGNode* index,
                                     bool shareable) {
  auto candidate = createNode(NodeKind::ArrayAccess);
  candidate->operands = {base, index};
  candidate->pure = shareable;
  candidate->recomputeHash();
  return shareable ? findExistingNode(candidate) : candidate;
}

DAGNode* IRModule::createMemberAccess(DAGNode* base, const std::string& member,
                                      bool shareable) {
  auto candidate = createNode(NodeKind::MemberAccess);
  candidate->name = member;
  candidate->operands = {base};
  candidate->pure = shareable;
  candidate->recomputeHash();
  return shareable ? findExistingNode(candidate) : candidate;
}

DAGNode* IRModule::createArrowAccess(DAGNode* base, const std::string& member,
                                     bool shareable) {
  auto candidate = createNode(NodeKind::ArrowAccess);
  candidate->name = member;
  candidate->operands = {base};
  candidate->pure = shareable;
  candidate->recomputeHash();
  return shareable ? findExistingNode(candidate) : candidate;
}

DAGNode* IRModule::createCall(DAGNode* callee, const std::vector<DAGNode*>& args,
                              bool pure) {
  auto candidate = createNode(NodeKind::Call);
  candidate->operands.push_back(callee);
  for (auto* a : args) candidate->operands.push_back(a);
  candidate->pure = pure;
  candidate->recomputeHash();
  // Impure calls are kept distinct: sharing them could drop or reorder effects.
  return pure ? findExistingNode(candidate) : candidate;
}

DAGNode* IRModule::findExistingNode(DAGNode* candidate) {
  auto it = _hash_map.find(candidate->hash);
  if (it != _hash_map.end()) {
    DAGNode* existing = it->second;
    // Verify structural equality (hash collision check)
    NodeEqual eq;
    if (eq(existing, candidate)) {
      // Remove the candidate from pool (it's a duplicate)
      // Actually, we can't easily remove from vector. Just leave it.
      // The hashMap points to the first node, which is what we want.
      return existing;
    }
  }
  // New unique node, add to hash map
  _hash_map[candidate->hash] = candidate;
  return candidate;
}

}  // namespace cse
