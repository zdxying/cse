#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace cse {

enum class NodeKind {
    Constant,
    Variable,
    BinaryOp,
    UnaryOp,
    ArrayAccess,   // base[index]
    MemberAccess,  // base.member
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

class DAGNode {
public:
    DAGNode(NodeKind k, uint32_t id);

    NodeKind kind;
    uint32_t id;        // unique node id
    uint64_t hash;      // structural hash for CSE

    // Constant
    double constVal = 0;
    std::string numText;

    // Variable
    std::string name;

    // BinaryOp / UnaryOp
    char op = 0;

    // Operands (children in DAG)
    std::vector<DAGNode*> operands;

    // Source location for debugging
    size_t srcLine = 0;

    // Compute hash from children
    void recomputeHash();

    // For debugging
    std::string toString() const;
};

} // namespace cse
