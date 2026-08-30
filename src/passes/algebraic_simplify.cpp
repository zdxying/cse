#include "algebraic_simplify.h"
#include "../ir/ir_module.h"
#include "../ir/statement.h"
#include <algorithm>
#include <vector>

namespace cse {

class AlgebraicSimplifyVisitor {
public:
    explicit AlgebraicSimplifyVisitor(IRModule& mod) : module(mod) {}
    IRModule& module;
    int simplifications = 0;

    void visitStmt(StmtIR* stmt) {
        if (!stmt) return;
        switch (stmt->kind) {
            case StmtIRKind::Block: {
                auto* block = static_cast<BlockIR*>(stmt);
                for (auto& s : block->stmts) visitStmt(s.get());
                break;
            }
            case StmtIRKind::ForLoop: {
                auto* forLoop = static_cast<ForLoopIR*>(stmt);
                visitStmt(forLoop->init.get());
                forLoop->cond = simplify(forLoop->cond);
                forLoop->update = simplify(forLoop->update);
                if (forLoop->updateRhs) forLoop->updateRhs = simplify(forLoop->updateRhs);
                visitStmt(forLoop->body.get());
                break;
            }
            case StmtIRKind::IfElse: {
                auto* ifElse = static_cast<IfElseIR*>(stmt);
                ifElse->cond = simplify(ifElse->cond);
                visitStmt(ifElse->thenBranch.get());
                visitStmt(ifElse->elseBranch.get());
                break;
            }
            case StmtIRKind::ExprStmt: {
                auto* exprStmt = static_cast<ExprStmtIR*>(stmt);
                if (exprStmt->expr) exprStmt->expr = simplify(exprStmt->expr);
                break;
            }
            case StmtIRKind::Assign: {
                auto* assign = static_cast<AssignIR*>(stmt);
                if (assign->value) assign->value = simplify(assign->value);
                break;
            }
            case StmtIRKind::VarDecl: {
                auto* decl = static_cast<VarDeclIR*>(stmt);
                if (decl->init) decl->init = simplify(decl->init);
                break;
            }
            case StmtIRKind::Return: {
                auto* ret = static_cast<ReturnIR*>(stmt);
                if (ret->value) ret->value = simplify(ret->value);
                break;
            }
        }
    }

    DAGNode* simplify(DAGNode* node) {
        if (!node) return nullptr;

        // Recursively simplify children first
        if (node->kind == NodeKind::BinaryOp && node->operands.size() == 2) {
            node->operands[0] = simplify(node->operands[0]);
            node->operands[1] = simplify(node->operands[1]);
            node->recomputeHash();
        }

        // Apply algebraic simplifications
        if (node->kind == NodeKind::BinaryOp && node->operands.size() == 2) {
            DAGNode* result = applyIdentities(node);
            if (result != node) {
                simplifications++;
                return result;
            }

            result = sortCommutative(node);
            if (result != node) {
                simplifications++;
                return result;
            }
        }

        return node;
    }

private:
    bool isConst(DAGNode* n, double val) {
        return n->kind == NodeKind::Constant && n->constVal == val;
    }

    // a * 1 → a, a + 0 → a, a * 0 → 0
    DAGNode* applyIdentities(DAGNode* node) {
        DAGNode* lhs = node->operands[0];
        DAGNode* rhs = node->operands[1];

        if (node->op == '*') {
            if (isConst(lhs, 1)) { simplifications++; return rhs; }
            if (isConst(rhs, 1)) { simplifications++; return lhs; }
            if (isConst(lhs, 0) || isConst(rhs, 0)) {
                simplifications++;
                return module.createConst(0, "0");
            }
        }

        if (node->op == '+') {
            if (isConst(lhs, 0)) { simplifications++; return rhs; }
            if (isConst(rhs, 0)) { simplifications++; return lhs; }
        }

        if (node->op == '-') {
            if (isConst(rhs, 0)) { simplifications++; return lhs; }
        }

        if (node->op == '/') {
            if (isConst(rhs, 1)) { simplifications++; return lhs; }
            if (isConst(lhs, 0)) { simplifications++; return module.createConst(0, "0"); }
        }

        return node;
    }

    // Sort operands of commutative operators — local swap only
    // Only swap if both operands are leaves (variables/constants)
    DAGNode* sortCommutative(DAGNode* node) {
        if (node->op != '+' && node->op != '*') return node;
        if (node->operands.size() != 2) return node;

        DAGNode* lhs = node->operands[0];
        DAGNode* rhs = node->operands[1];

        // Only swap leaf nodes to avoid breaking sharing structure
        if (!isTrivial(lhs) || !isTrivial(rhs)) return node;

        if (isCanonicalBefore(rhs, lhs)) {
            return module.createBinaryOp(node->op, rhs, lhs);
        }

        return node;
    }

    bool isTrivial(DAGNode* n) {
        return n->kind == NodeKind::Constant || n->kind == NodeKind::Variable;
    }

    bool isCanonicalBefore(DAGNode* a, DAGNode* b) {
        int rankA = nodeRank(a);
        int rankB = nodeRank(b);
        if (rankA != rankB) return rankA < rankB;

        if (a->kind == NodeKind::Variable && b->kind == NodeKind::Variable) {
            return a->name <= b->name;
        }
        return a->id <= b->id;
    }

    int nodeRank(DAGNode* n) {
        switch (n->kind) {
            case NodeKind::Constant: return 0;
            case NodeKind::Variable: return 1;
            default: return 2;
        }
    }
};

void AlgebraicSimplifyPass::run(IRModule& module) {
    AlgebraicSimplifyVisitor visitor(module);
    visitor.visitStmt(module.body.get());
}

} // namespace cse
