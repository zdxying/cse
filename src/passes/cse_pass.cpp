#include "cse_pass.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../ir/ir_module.h"
#include "../ir/statement.h"

namespace cse {

// ===== Phase 1: Collect DAG nodes per statement =====

// Walk a statement tree and collect all DAGNode* pointers that appear in
// expression positions. Each node is recorded with the "root statement index"
// (the index of the top-level statement in the block that contains it).
static void collectNodesFromStmt(StmtIR* stmt,
                                 std::unordered_set<DAGNode*>& out) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* block = static_cast<BlockIR*>(stmt);
      for (auto& s : block->stmts) collectNodesFromStmt(s.get(), out);
      break;
    }
    case StmtIRKind::VarDecl: {
      auto* decl = static_cast<VarDeclIR*>(stmt);
      if (decl->init) {
        // Collect all nodes in the init expression
        std::vector<DAGNode*> stack = {decl->init};
        while (!stack.empty()) {
          DAGNode* n = stack.back();
          stack.pop_back();
          if (out.insert(n).second) {
            for (auto* op : n->operands) stack.push_back(op);
          }
        }
      }
      break;
    }
    case StmtIRKind::Assign: {
      auto* assign = static_cast<AssignIR*>(stmt);
      if (assign->value) {
        std::vector<DAGNode*> stack = {assign->value};
        while (!stack.empty()) {
          DAGNode* n = stack.back();
          stack.pop_back();
          if (out.insert(n).second) {
            for (auto* op : n->operands) stack.push_back(op);
          }
        }
      }
      break;
    }
    case StmtIRKind::ExprStmt: {
      auto* exprStmt = static_cast<ExprStmtIR*>(stmt);
      if (exprStmt->expr) {
        std::vector<DAGNode*> stack = {exprStmt->expr};
        while (!stack.empty()) {
          DAGNode* n = stack.back();
          stack.pop_back();
          if (out.insert(n).second) {
            for (auto* op : n->operands) stack.push_back(op);
          }
        }
      }
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      collectNodesFromStmt(f->init.get(), out);
      if (f->cond) {
        std::vector<DAGNode*> stack = {f->cond};
        while (!stack.empty()) {
          DAGNode* n = stack.back();
          stack.pop_back();
          if (out.insert(n).second) {
            for (auto* op : n->operands) stack.push_back(op);
          }
        }
      }
      if (f->update) {
        std::vector<DAGNode*> stack = {f->update};
        while (!stack.empty()) {
          DAGNode* n = stack.back();
          stack.pop_back();
          if (out.insert(n).second) {
            for (auto* op : n->operands) stack.push_back(op);
          }
        }
      }
      if (f->updateRhs) {
        std::vector<DAGNode*> stack = {f->updateRhs};
        while (!stack.empty()) {
          DAGNode* n = stack.back();
          stack.pop_back();
          if (out.insert(n).second) {
            for (auto* op : n->operands) stack.push_back(op);
          }
        }
      }
      collectNodesFromStmt(f->body.get(), out);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      if (ie->cond) {
        std::vector<DAGNode*> stack = {ie->cond};
        while (!stack.empty()) {
          DAGNode* n = stack.back();
          stack.pop_back();
          if (out.insert(n).second) {
            for (auto* op : n->operands) stack.push_back(op);
          }
        }
      }
      collectNodesFromStmt(ie->thenBranch.get(), out);
      collectNodesFromStmt(ie->elseBranch.get(), out);
      break;
    }
    case StmtIRKind::Return: {
      auto* ret = static_cast<ReturnIR*>(stmt);
      if (ret->value) {
        std::vector<DAGNode*> stack = {ret->value};
        while (!stack.empty()) {
          DAGNode* n = stack.back();
          stack.pop_back();
          if (out.insert(n).second) {
            for (auto* op : n->operands) stack.push_back(op);
          }
        }
      }
      break;
    }
  }
}

// ===== Phase 2: Replace all references to a target node with a variable =====

// Recursively walk a statement tree and replace all DAGNode* that match
// `target` with `replacement` in expression positions.
static void replaceRefsInStmt(StmtIR* stmt, DAGNode* target,
                               DAGNode* replacement) {
  if (!stmt) return;

  // Recursive replacement on operands vectors
  std::function<void(DAGNode*&)> replaceInExprTree = [&](DAGNode*& node) {
    if (!node) return;
    if (node == target) {
      node = replacement;
      return;
    }
    // Recurse into operands (need to handle the vector elements)
    for (size_t i = 0; i < node->operands.size(); i++) {
      if (node->operands[i] == target) {
        node->operands[i] = replacement;
      } else if (node->operands[i]) {
        replaceInExprTree(node->operands[i]);
      }
    }
  };

  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* block = static_cast<BlockIR*>(stmt);
      for (auto& s : block->stmts) replaceRefsInStmt(s.get(), target, replacement);
      break;
    }
    case StmtIRKind::VarDecl: {
      auto* decl = static_cast<VarDeclIR*>(stmt);
      replaceInExprTree(decl->init);
      break;
    }
    case StmtIRKind::Assign: {
      auto* assign = static_cast<AssignIR*>(stmt);
      replaceInExprTree(assign->value);
      break;
    }
    case StmtIRKind::ExprStmt: {
      auto* exprStmt = static_cast<ExprStmtIR*>(stmt);
      replaceInExprTree(exprStmt->expr);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      replaceRefsInStmt(f->init.get(), target, replacement);
      replaceInExprTree(f->cond);
      replaceInExprTree(f->update);
      replaceInExprTree(f->updateRhs);
      replaceRefsInStmt(f->body.get(), target, replacement);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      replaceInExprTree(ie->cond);
      replaceRefsInStmt(ie->thenBranch.get(), target, replacement);
      replaceRefsInStmt(ie->elseBranch.get(), target, replacement);
      break;
    }
    case StmtIRKind::Return: {
      auto* ret = static_cast<ReturnIR*>(stmt);
      replaceInExprTree(ret->value);
      break;
    }
  }
}

// ===== Main CSE Pass =====

void CSEPass::run(IRModule& module) {
  auto* root = module.body.get();
  if (!root || root->kind != StmtIRKind::Block) return;

  auto* block = static_cast<BlockIR*>(root);

  // We iterate multiple times because extracting one CSE opportunity
  // may reveal new ones (e.g., after replacing, a previously unique
  // subexpression may now be shared).
  for (int iteration = 0; iteration < 100; ++iteration) {
    // Phase 1: For each top-level statement, collect all DAG nodes
    // nodeToStmts: maps node -> set of statement indices that use it
    std::unordered_map<DAGNode*, std::vector<size_t>> nodeToStmts;

    for (size_t i = 0; i < block->stmts.size(); i++) {
      std::unordered_set<DAGNode*> nodesInStmt;
      collectNodesFromStmt(block->stmts[i].get(), nodesInStmt);
      for (auto* node : nodesInStmt) {
        // Only track non-leaf nodes (Constant, Variable are leaves)
        if (node->kind != NodeKind::Constant && node->kind != NodeKind::Variable) {
          nodeToStmts[node].push_back(i);
        }
      }
    }

    // Phase 2: Find nodes used in >= 2 different statements
    struct CSECandidate {
      DAGNode* node;
      size_t stmtCount;
    };
    std::vector<CSECandidate> candidates;
    for (auto& [node, stmtIndices] : nodeToStmts) {
      // Remove duplicate statement indices (a node can appear multiple times
      // in the same statement's expression tree)
      std::sort(stmtIndices.begin(), stmtIndices.end());
      stmtIndices.erase(std::unique(stmtIndices.begin(), stmtIndices.end()),
                        stmtIndices.end());
      if (stmtIndices.size() >= 2) {
        candidates.push_back({node, stmtIndices.size()});
      }
    }

    if (candidates.empty()) break;

    // Sort by benefit: prefer nodes that appear in more statements
    // and have larger subtrees (more savings)
    std::sort(candidates.begin(), candidates.end(),
              [](const CSECandidate& a, const CSECandidate& b) {
                return a.stmtCount > b.stmtCount;
              });

    bool changed = false;
    for (auto& cand : candidates) {
      DAGNode* target = cand.node;

      // Verify the node is still in the DAG (may have been replaced
      // by a previous extraction in this iteration)
      bool stillPresent = false;
      for (auto& stmt : block->stmts) {
        std::unordered_set<DAGNode*> nodes;
        collectNodesFromStmt(stmt.get(), nodes);
        if (nodes.count(target)) {
          stillPresent = true;
          break;
        }
      }
      if (!stillPresent) continue;

      // Create variable name
      std::string varName = "_cse_" + std::to_string(iteration) + "_" +
                            std::to_string(target->id);

      // Create the variable node (via getVar for proper caching)
      DAGNode* varNode = module.getVar(varName);

      // Create VarDecl for the extracted expression
      auto decl = std::make_unique<VarDeclIR>();
      decl->type = "auto";
      decl->name = varName;
      decl->init = target;

      // Replace all references to target with varNode across all statements
      for (auto& stmt : block->stmts) {
        replaceRefsInStmt(stmt.get(), target, varNode);
      }

      // Find the first statement that uses the extracted expression
      // and insert the VarDecl right before it
      size_t insertPos = block->stmts.size(); // default: insert at end
      for (size_t i = 0; i < block->stmts.size(); i++) {
        std::unordered_set<DAGNode*> nodes;
        collectNodesFromStmt(block->stmts[i].get(), nodes);
        // Check if this statement uses the varNode (which replaced target)
        if (nodes.count(varNode)) {
          insertPos = i;
          break;
        }
      }

      // Insert the VarDecl at the computed position
      block->stmts.insert(block->stmts.begin() + insertPos, std::move(decl));
      changed = true;

      // Only extract one candidate per iteration to keep things simple
      break;
    }

    if (!changed) break;
  }
}

}  // namespace cse
