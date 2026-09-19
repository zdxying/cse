#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Directed Acyclic Graph node — the core of CSE.
// Each expression is a DAGNode; structurally identical subexpressions
// share the same node (deduplication at construction time via hash).

namespace cse {

enum class NodeKind {
  Constant,
  Variable,
  BinaryOp,
  UnaryOp,
  ArrayAccess,   // base[index]
  MemberAccess,  // base.member
  ArrowAccess,   // base->member
  Call,
  Ternary,
  Cast,
};

// Forward declaration
class DAGNode;

// Hash computation for DAG nodes
struct NodeHash {
  size_t operator()(const DAGNode* node) const;
};

struct NodeEqual {
  bool operator()(const DAGNode* a, const DAGNode* b) const;
};

struct DAGNode {
  DAGNode(NodeKind k, uint32_t id);

  NodeKind kind;
  uint32_t id;    // unique node id
  uint64_t hash;  // structural hash — drives CSE deduplication

  // Constant
  double constVal = 0;
  std::string numText;
  // Optional symbolic form of a constant (e.g. a declared constexpr accessor
  // such as `latset::w<D3Q19<double>>(1)`). Used for code emission only; the
  // numeric constVal still drives folding, dedup and CSE.
  std::string symbol;

  // Variable
  std::string name;

  // BinaryOp / UnaryOp
  char op = 0;

  // Operands (children in DAG) — for BinaryOp: [lhs, rhs]; for MemberAccess: [base]
  std::vector<DAGNode*> operands;

  // Source location for debugging
  size_t srcLine = 0;

  // Compute hash from children
  void recomputeHash();

  // For debugging
  std::string toString() const;
};

}  // namespace cse
