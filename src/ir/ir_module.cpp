#include "ir_module.h"
#include <functional>
#include <sstream>
#include <cstring>

namespace cse {

// ===== DAGNode =====

DAGNode::DAGNode(NodeKind k, uint32_t id)
    : kind(k), id(id), hash(0) {}

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

std::string DAGNode::toString() const {
    std::ostringstream oss;
    oss << "N" << id << "(";
    switch (kind) {
        case NodeKind::Constant:
            oss << numText;
            break;
        case NodeKind::Variable:
            oss << name;
            break;
        case NodeKind::BinaryOp:
            oss << operands[0]->toString() << " " << op << " " << operands[1]->toString();
            break;
        case NodeKind::UnaryOp:
            oss << op << operands[0]->toString();
            break;
        case NodeKind::ArrayAccess:
            oss << operands[0]->toString() << "[" << operands[1]->toString() << "]";
            break;
        case NodeKind::MemberAccess:
            oss << operands[0]->toString() << "." << name;
            break;
        case NodeKind::ArrowAccess:
            oss << operands[0]->toString() << "->" << name;
            break;
        case NodeKind::Call:
            oss << operands[0]->toString() << "()";
            break;
        case NodeKind::Ternary:
            oss << operands[0]->toString() << "?" << operands[1]->toString()
                << ":" << operands[2]->toString();
            break;
        case NodeKind::Cast:
            oss << "(" << name << ")" << operands[0]->toString();
            break;
    }
    oss << ")";
    return oss.str();
}

// ===== NodeHash / NodeEqual =====

size_t NodeHash::operator()(const DAGNode* node) const {
    return node->hash;
}

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
    node->numText = text.empty() ? std::to_string(val) : text;
    node->recomputeHash();
    return node;
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

DAGNode* IRModule::createArrayAccess(DAGNode* base, DAGNode* index) {
    auto candidate = createNode(NodeKind::ArrayAccess);
    candidate->operands = {base, index};
    candidate->recomputeHash();
    return findExistingNode(candidate);
}

DAGNode* IRModule::createMemberAccess(DAGNode* base, const std::string& member) {
    auto candidate = createNode(NodeKind::MemberAccess);
    candidate->name = member;
    candidate->operands = {base};
    candidate->recomputeHash();
    return findExistingNode(candidate);
}

DAGNode* IRModule::createArrowAccess(DAGNode* base, const std::string& member) {
    auto candidate = createNode(NodeKind::ArrowAccess);
    candidate->name = member;
    candidate->operands = {base};
    candidate->recomputeHash();
    return findExistingNode(candidate);
}

DAGNode* IRModule::createCall(DAGNode* callee, const std::vector<DAGNode*>& args) {
    auto candidate = createNode(NodeKind::Call);
    candidate->operands.push_back(callee);
    for (auto* a : args) candidate->operands.push_back(a);
    candidate->recomputeHash();
    return findExistingNode(candidate);
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

} // namespace cse
