#include "codegen.h"
#include "../ir/ir_module.h"
#include "../ir/statement.h"
#include <unordered_set>
#include <algorithm>

namespace cse {

std::string CodeGen::generate(IRModule& module) {
    out_.str("");
    out_.clear();

    out_ << module.funcSig.returnType << " " << module.funcSig.name << "(";
    for (size_t i = 0; i < module.funcSig.params.size(); i++) {
        if (i > 0) out_ << ", ";
        out_ << module.funcSig.params[i].type << " " << module.funcSig.params[i].name;
    }
    out_ << ") {\n";

    if (module.body) {
        emitStmt(module.body.get(), 1);
    }

    out_ << "}\n";
    return out_.str();
}

void CodeGen::emitStmt(StmtIR* stmt, int indentLevel) {
    if (!stmt) return;
    std::string ind = makeIndent(indentLevel);

    switch (stmt->kind) {
        case StmtIRKind::Block: {
            auto* block = static_cast<BlockIR*>(stmt);
            for (auto& s : block->stmts) {
                emitStmt(s.get(), indentLevel);
            }
            break;
        }
        case StmtIRKind::ForLoop: {
            auto* forLoop = static_cast<ForLoopIR*>(stmt);
            out_ << ind << "for (";
            if (forLoop->init) {
                if (forLoop->init->kind == StmtIRKind::VarDecl) {
                    auto* decl = static_cast<VarDeclIR*>(forLoop->init.get());
                    out_ << decl->type << " " << decl->name;
                    if (decl->init) out_ << " = " << emitExpr(decl->init);
                }
            }
            out_ << "; ";
            if (forLoop->cond) out_ << emitExpr(forLoop->cond);
            out_ << "; ";
            if (forLoop->update) {
                out_ << emitExpr(forLoop->update);
                if (forLoop->updateOp == '=' && forLoop->updateRhs) {
                    out_ << " = " << emitExpr(forLoop->updateRhs);
                } else if (forLoop->updateOp == '+' && forLoop->updateRhs) {
                    out_ << " += " << emitExpr(forLoop->updateRhs);
                } else if (forLoop->updateOp == '-' && forLoop->updateRhs) {
                    out_ << " -= " << emitExpr(forLoop->updateRhs);
                } else if (forLoop->updateOp == '*' && forLoop->updateRhs) {
                    out_ << " *= " << emitExpr(forLoop->updateRhs);
                } else if (forLoop->updateOp == '/' && forLoop->updateRhs) {
                    out_ << " /= " << emitExpr(forLoop->updateRhs);
                } else if (forLoop->updateOp == '+') {
                    out_ << "++";
                } else if (forLoop->updateOp == '-') {
                    out_ << "--";
                }
            }
            out_ << ") {\n";
            emitStmt(forLoop->body.get(), indentLevel + 1);
            out_ << ind << "}\n";
            break;
        }
        case StmtIRKind::IfElse: {
            auto* ifElse = static_cast<IfElseIR*>(stmt);
            out_ << ind << "if (" << emitExpr(ifElse->cond) << ") {\n";
            emitStmt(ifElse->thenBranch.get(), indentLevel + 1);
            if (ifElse->elseBranch) {
                out_ << ind << "} else {\n";
                emitStmt(ifElse->elseBranch.get(), indentLevel + 1);
            }
            out_ << ind << "}\n";
            break;
        }
        case StmtIRKind::VarDecl: {
            auto* decl = static_cast<VarDeclIR*>(stmt);
            out_ << ind << decl->type << " " << decl->name;
            if (decl->init) {
                out_ << " = " << emitExpr(decl->init);
            }
            out_ << ";\n";
            break;
        }
        case StmtIRKind::Assign: {
            auto* assign = static_cast<AssignIR*>(stmt);
            out_ << ind << assign->target << " = " << emitExpr(assign->value) << ";\n";
            break;
        }
        case StmtIRKind::ExprStmt: {
            auto* exprStmt = static_cast<ExprStmtIR*>(stmt);
            if (exprStmt->expr) {
                out_ << ind << emitExpr(exprStmt->expr) << ";\n";
            }
            break;
        }
        case StmtIRKind::Return: {
            auto* ret = static_cast<ReturnIR*>(stmt);
            out_ << ind << "return";
            if (ret->value) out_ << " " << emitExpr(ret->value);
            out_ << ";\n";
            break;
        }
    }
}

std::string CodeGen::emitExpr(DAGNode* node) {
    if (!node) return "";

    switch (node->kind) {
        case NodeKind::Constant:
            return node->numText;

        case NodeKind::Variable:
            return node->name;

        case NodeKind::BinaryOp: {
            if (node->operands.size() != 2) return "";
            std::string lhs = emitExpr(node->operands[0]);
            std::string rhs = emitExpr(node->operands[1]);

            bool needParenL = node->operands[0]->kind == NodeKind::BinaryOp;
            bool needParenR = node->operands[1]->kind == NodeKind::BinaryOp;

            std::string result;
            if (needParenL) result += "(" + lhs + ")";
            else result += lhs;

            result += " ";
            result += node->op;
            result += " ";

            if (needParenR) result += "(" + rhs + ")";
            else result += rhs;

            return result;
        }

        case NodeKind::UnaryOp: {
            if (node->operands.empty()) return "";
            std::string operand = emitExpr(node->operands[0]);
            if (node->name == "postfix") {
                return operand + node->op;
            }
            return std::string(1, node->op) + operand;
        }

        case NodeKind::ArrayAccess: {
            if (node->operands.size() != 2) return "";
            return emitExpr(node->operands[0]) + "[" + emitExpr(node->operands[1]) + "]";
        }

        case NodeKind::MemberAccess: {
            if (node->operands.empty()) return "";
            return emitExpr(node->operands[0]) + "." + node->name;
        }

        case NodeKind::Call: {
            if (node->operands.empty()) return "";
            std::string result = emitExpr(node->operands[0]) + "(";
            for (size_t i = 1; i < node->operands.size(); i++) {
                if (i > 1) result += ", ";
                result += emitExpr(node->operands[i]);
            }
            result += ")";
            return result;
        }

        case NodeKind::Ternary: {
            if (node->operands.size() != 3) return "";
            return emitExpr(node->operands[0]) + " ? " +
                   emitExpr(node->operands[1]) + " : " +
                   emitExpr(node->operands[2]);
        }

        case NodeKind::Cast: {
            if (node->operands.empty()) return "";
            return "(" + node->name + ")" + emitExpr(node->operands[0]);
        }
    }
    return "";
}

std::string CodeGen::makeIndent(int level) const {
    return std::string(level * 4, ' ');
}

void CodeGen::topoSort(DAGNode* node, std::vector<DAGNode*>& order,
                       std::unordered_set<uint32_t>& visited) {
    if (!node || visited.count(node->id)) return;
    visited.insert(node->id);
    for (auto* op : node->operands) {
        topoSort(op, order, visited);
    }
    order.push_back(node);
}

void CodeGen::analyzeTemporaries(DAGNode* node, std::unordered_set<uint32_t>& needsTemp) {
    if (!node) return;
    if (node->kind == NodeKind::BinaryOp || node->kind == NodeKind::UnaryOp ||
        node->kind == NodeKind::ArrayAccess || node->kind == NodeKind::Call ||
        node->kind == NodeKind::Ternary) {
        needsTemp.insert(node->id);
    }
    for (auto* op : node->operands) {
        analyzeTemporaries(op, needsTemp);
    }
}

bool CodeGen::isSimple(DAGNode* node) {
    return node->kind == NodeKind::Constant || node->kind == NodeKind::Variable;
}

} // namespace cse
