#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dag_node.h"
#include "statement.h"

namespace cse {

// Function signature
struct FuncParam {
  std::string type;
  std::string name;
};

struct FuncSignature {
  std::string returnType;
  std::string name;
  std::vector<FuncParam> params;
};

// IR Module — owns all DAG nodes and holds the function body.
// Node pool ensures stable pointers; _hash_map enables CSE deduplication.
// createBinaryOp/createMemberAccess/etc. check _hash_map before creating new nodes.
class IRModule {
 public:
  IRModule() = default;

  // Function being processed
  FuncSignature funcSig;

  // Function body (structured statements)
  std::unique_ptr<StmtIR> body;

  // Create a new DAG node
  DAGNode* createNode(NodeKind kind);

  // Create a constant node
  DAGNode* createConst(double val, const std::string& text = "");

  // Create a constant node that carries a symbolic (declared) form for code
  // emission. Deduplicated by value like createConst; if a node with the same
  // value exists without a symbol, the symbol is attached to it.
  DAGNode* createSymbolicConst(double val, const std::string& symbol);

  // Create a variable node
  DAGNode* createVar(const std::string& name);

  // Create a binary op node with CSE (hash-based dedup)
  // Returns existing node if structurally identical node already exists.
  DAGNode* createBinaryOp(char op, DAGNode* lhs, DAGNode* rhs);

  // Create a unary op node
  DAGNode* createUnaryOp(char op, DAGNode* operand);

  // Create an array access node.
  // `shareable`: if true, structurally identical loads share a node. Only safe
  // for loads from read-only locations; otherwise each load is kept distinct.
  // Defaults to false so callers must opt in to deduplication explicitly.
  DAGNode* createArrayAccess(DAGNode* base, DAGNode* index, bool shareable = false);

  // Create a member access node (see createArrayAccess for `shareable`).
  DAGNode* createMemberAccess(DAGNode* base, const std::string& member,
                              bool shareable = false);

  // Create an arrow access node (see createArrayAccess for `shareable`).
  DAGNode* createArrowAccess(DAGNode* base, const std::string& member,
                             bool shareable = false);

  // Create a call node.
  // `pure`: if true, identical calls share a node. Impure calls are never
  // deduplicated (they may have side effects / observe mutable state).
  // Defaults to false so callers must opt in to deduplication explicitly.
  DAGNode* createCall(DAGNode* callee, const std::vector<DAGNode*>& args,
                      bool pure = false);

  // CSE lookup: find existing node with same structural hash.
  // If found, returns existing (dedup); otherwise registers candidate.
  DAGNode* findExistingNode(DAGNode* candidate);

  // Get or create variable
  DAGNode* getVar(const std::string& name);

 private:
  // Node pool - owns all DAG nodes
  std::vector<std::unique_ptr<DAGNode>> _node_pool;

  // Hash map for CSE: hash → nodes with that hash (bucket, so hash collisions
  // never evict each other).
  std::unordered_map<uint64_t, std::vector<DAGNode*>> _hash_map;

  // Variable cache: name → node
  std::unordered_map<std::string, DAGNode*> _var_cache;

  uint32_t _next_node_id = 0;
};

}  // namespace cse
