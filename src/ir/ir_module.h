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
//
// The node pool keeps pointers stable and _hash_map implements hash-consing
// (the CSE deduplication). Every node is built through one of the create*
// factories below: they fill in all of the node's fields, compute its hash, and
// only then offer it to intern(). `createNode` and `intern` are private --
// deliberately, so that no call site can get that order wrong. The interning
// protocol itself is documented in dag_node.h.
class IRModule {
 public:
  IRModule() = default;

  // Function being processed
  FuncSignature funcSig;

  // Function body (structured statements)
  std::unique_ptr<StmtIR> body;

  // ----- Constants -----

  // Create a constant node, deduplicated by value so that expressions built
  // around the same literal share a DAG node (enables cross-statement CSE).
  DAGNode* createConst(double val, const std::string& text = "");

  // Create a constant node that carries a symbolic (declared) form for code
  // emission. Deduplicated by value like createConst; if a node with the same
  // value exists without a symbol, the symbol is attached to it.
  DAGNode* createSymbolicConst(double val, const std::string& symbol);

  // ----- Leaves -----

  // Get or create variable
  DAGNode* getVar(const std::string& name);

  // ----- Operators -----

  // Create a binary op node with CSE (hash-based dedup)
  DAGNode* createBinaryOp(char op, DAGNode* lhs, DAGNode* rhs);

  // A plain unary operator: `-x`, `!x`. Shared when its operand is.
  DAGNode* createUnaryOp(char op, DAGNode* operand);

  // `++x` / `--x`. Carries a side effect, so it is never shared and never
  // hoisted; the prefix form also yields the new value.
  DAGNode* createPreIncDec(char op, DAGNode* operand);

  // `x++` / `x--`. As above, but yields the old value.
  DAGNode* createPostIncDec(char op, DAGNode* operand);

  // `(typeName)operand`.
  DAGNode* createCast(const std::string& typeName, DAGNode* operand);

  // `cond ? trueExpr : falseExpr`.
  DAGNode* createTernary(DAGNode* cond, DAGNode* trueExpr, DAGNode* falseExpr);

  // ----- Memory access and calls -----

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

  // ----- Invariants -----

  // Debug-build check of the interning protocol: every interned node is
  // shareable, every interned node's hash still describes its content, and no
  // bucket holds two distinct nodes that denote the same node. A few lines that
  // catch exactly the class of defect that silently produced wrong code.
  // Compiled out under NDEBUG.
  void verify() const;

 private:
  // Allocate a node. It is NOT interned: the caller must finish filling it in,
  // call recomputeHash(), and then hand it to intern().
  DAGNode* createNode(NodeKind kind);

  // Offer a fully built node to the hash map: returns an existing
  // content-identical node when there is one, registers the candidate
  // otherwise, and returns the candidate unregistered when it is not shareable.
  DAGNode* intern(DAGNode* candidate);

  DAGNode* createUnaryOpImpl(char op, DAGNode* operand, const std::string& name,
                             bool postfix, bool pure);

  // Node pool - owns all DAG nodes
  std::vector<std::unique_ptr<DAGNode>> _node_pool;

  // Hash map for CSE: hash → nodes with that hash (bucket, so hash collisions
  // never evict each other). Only shareable nodes are registered here.
  std::unordered_map<uint64_t, std::vector<DAGNode*>> _hash_map;

  // Variable cache: name → node
  std::unordered_map<std::string, DAGNode*> _var_cache;

  uint32_t _next_node_id = 0;
};

}  // namespace cse
