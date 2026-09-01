#pragma once
#include "dag_node.h"
#include "statement.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <string>

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

    // Raw text segments (non-CSE regions, preserved as-is)
    struct RawSegment {
        std::string text;
    };
    std::vector<RawSegment> rawSegments;

    // Create a new DAG node
    DAGNode* createNode(NodeKind kind);

    // Create a constant node
    DAGNode* createConst(double val, const std::string& text = "");

    // Create a variable node
    DAGNode* createVar(const std::string& name);

    // Create a binary op node with CSE (hash-based dedup)
    // Returns existing node if structurally identical node already exists.
    DAGNode* createBinaryOp(char op, DAGNode* lhs, DAGNode* rhs);

    // Create a unary op node
    DAGNode* createUnaryOp(char op, DAGNode* operand);

    // Create an array access node
    DAGNode* createArrayAccess(DAGNode* base, DAGNode* index);

    // Create a member access node
    DAGNode* createMemberAccess(DAGNode* base, const std::string& member);

    // Create an arrow access node
    DAGNode* createArrowAccess(DAGNode* base, const std::string& member);

    // Create a call node
    DAGNode* createCall(DAGNode* callee, const std::vector<DAGNode*>& args);

    // CSE lookup: find existing node with same structural hash.
    // If found, returns existing (dedup); otherwise registers candidate.
    DAGNode* findExistingNode(DAGNode* candidate);

    // Get all nodes (for iteration)
    const std::vector<std::unique_ptr<DAGNode>>& getNodes() const { return _node_pool; }

    // Get or create variable
    DAGNode* getVar(const std::string& name);

private:
    // Node pool - owns all DAG nodes
    std::vector<std::unique_ptr<DAGNode>> _node_pool;

    // Hash map for CSE: hash → node (first node with that hash)
    std::unordered_map<uint64_t, DAGNode*> _hash_map;

    // Variable cache: name → node
    std::unordered_map<std::string, DAGNode*> _var_cache;

    uint32_t _next_node_id = 0;
};

} // namespace cse
