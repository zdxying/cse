#include "cleanup.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../ir/ir_module.h"
#include "../ir/statement.h"

namespace cse {

namespace {

// Check if a DAG node is a simple variable reference with no side effects
bool isTrivialVarRef(DAGNode* node) {
  if (!node) return false;
  return node->kind == NodeKind::Variable;
}

// Check if an expression statement is just a variable reference (useless)
bool isUselessExprStmt(ExprStmtIR* stmt) {
  if (!stmt || !stmt->expr) return false;
  return isTrivialVarRef(stmt->expr);
}

// Check if an assignment is a self-assignment (target == value variable)
bool isSelfAssign(AssignIR* assign) {
  if (!assign || !assign->value) return false;
  if (assign->targetExpr) return false;  // Complex lvalue
  
  if (assign->value->kind == NodeKind::Variable) {
    return assign->value->name == assign->target;
  }
  return false;
}

// Check if a DAG node is a binary '+' operation
bool isAddOp(DAGNode* node) {
  return node && node->kind == NodeKind::BinaryOp && node->op == '+';
}

// Check if a DAG node is a zero constant
bool isZeroConstant(DAGNode* node) {
  return node && node->kind == NodeKind::Constant && node->constVal == 0;
}

// Try to extract addend from x + a or a + x where x is the target variable
DAGNode* extractAddend(IRModule& mod, const std::string& varName, DAGNode* expr) {
  if (!isAddOp(expr)) return nullptr;
  
  DAGNode* lhs = expr->operands[0];
  DAGNode* rhs = expr->operands[1];
  
  // We're looking for: x + rhs where x is the target variable
  // (order could be either: x + a or a + x)
  if (lhs->kind == NodeKind::Variable && lhs->name == varName) {
    return rhs;
  } else if (rhs->kind == NodeKind::Variable && rhs->name == varName) {
    return lhs;
  }
  return nullptr;
}

// Try to combine chained VarDecl assignments:
// Pattern: VarDecl(x, 0) -> Assign(x, x+a) -> Assign(x, x+b) -> ...
// Replace with single VarDecl: x = a + b + ...
bool combineChainedVarDeclAssignments(BlockIR* block, IRModule& mod) {
  bool changed = false;
  
  for (size_t i = 0; i + 1 < block->stmts.size(); ++i) {
    // Look for VarDecl with zero init
    if (block->stmts[i]->kind != StmtIRKind::VarDecl) continue;
    auto* decl = static_cast<VarDeclIR*>(block->stmts[i].get());
    if (!isZeroConstant(decl->init)) continue;
    
    const std::string& varName = decl->name;
    std::vector<DAGNode*> addends;
    size_t j = i + 1;
    
    // Collect consecutive assignments to same var of form x = x + expr
    while (j < block->stmts.size()) {
      if (block->stmts[j]->kind != StmtIRKind::Assign) break;
      auto* assign = static_cast<AssignIR*>(block->stmts[j].get());
      if (assign->target != varName || assign->targetExpr) break;
      
      DAGNode* addend = extractAddend(mod, varName, assign->value);
      if (!addend) break;
      
      addends.push_back(addend);
      ++j;
    }
    
    // Need at least 2 addends to make it worthwhile (0 + a -> a is handled by algebraic simplify)
    if (addends.size() >= 2) {
      // Build combined expression: a + b + c + ...
      DAGNode* combined = addends[0];
      for (size_t k = 1; k < addends.size(); ++k) {
        combined = mod.createBinaryOp('+', combined, addends[k]);
      }
      
      // Replace the VarDecl with combined init
      decl->init = combined;
      
      // Remove the now-redundant assignments
      block->stmts.erase(block->stmts.begin() + i + 1, block->stmts.begin() + j);
      changed = true;
    }
  }
  
  return changed;
}

// Try to combine chained Assign assignments (for reference parameters):
// Pattern: Assign(x, 0) -> Assign(x, x+a) -> Assign(x, x+b) -> ...
// Replace first with combined: x = a + b + ...; remove rest
bool combineChainedAssignAssignments(BlockIR* block, IRModule& mod) {
  bool changed = false;
  
  for (size_t i = 0; i + 1 < block->stmts.size(); ++i) {
    // Look for Assign with zero init (x = 0)
    if (block->stmts[i]->kind != StmtIRKind::Assign) continue;
    auto* assign = static_cast<AssignIR*>(block->stmts[i].get());
    if (assign->targetExpr || !isZeroConstant(assign->value)) continue;
    
    const std::string& varName = assign->target;
    std::vector<DAGNode*> addends;
    size_t j = i + 1;
    
    // Collect consecutive assignments to same var of form x = x + expr
    while (j < block->stmts.size()) {
      if (block->stmts[j]->kind != StmtIRKind::Assign) break;
      auto* nextAssign = static_cast<AssignIR*>(block->stmts[j].get());
      if (nextAssign->target != varName || nextAssign->targetExpr) break;
      
      DAGNode* addend = extractAddend(mod, varName, nextAssign->value);
      if (!addend) break;
      
      addends.push_back(addend);
      ++j;
    }
    
    // Need at least 2 addends
    if (addends.size() >= 2) {
      // Build combined expression: a + b + c + ...
      DAGNode* combined = addends[0];
      for (size_t k = 1; k < addends.size(); ++k) {
        combined = mod.createBinaryOp('+', combined, addends[k]);
      }
      
      // Replace first Assign with combined
      assign->value = combined;
      
      // Remove the now-redundant assignments
      block->stmts.erase(block->stmts.begin() + i + 1, block->stmts.begin() + j);
      changed = true;
    }
  }
  
  return changed;
}

// Count variable uses in a DAG subtree
void countExprUses(DAGNode* e, std::unordered_map<std::string, int>& counts) {
  if (!e) return;
  if (e->kind == NodeKind::Variable) counts[e->name]++;
  for (auto* op : e->operands) countExprUses(op, counts);
}

// Count uses across a statement tree
void countStmtUses(StmtIR* stmt, std::unordered_map<std::string, int>& counts) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) countStmtUses(s.get(), counts);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      countStmtUses(f->init.get(), counts);
      countExprUses(f->cond, counts);
      countExprUses(f->update, counts);
      countExprUses(f->updateRhs, counts);
      countStmtUses(f->body.get(), counts);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      countExprUses(ie->cond, counts);
      countStmtUses(ie->thenBranch.get(), counts);
      countStmtUses(ie->elseBranch.get(), counts);
      break;
    }
    case StmtIRKind::ExprStmt:
      countExprUses(static_cast<ExprStmtIR*>(stmt)->expr, counts);
      break;
    case StmtIRKind::Assign:
      countExprUses(static_cast<AssignIR*>(stmt)->value, counts);
      break;
    case StmtIRKind::VarDecl:
      countExprUses(static_cast<VarDeclIR*>(stmt)->init, counts);
      break;
    case StmtIRKind::Return:
      countExprUses(static_cast<ReturnIR*>(stmt)->value, counts);
      break;
  }
}

// Check if a DAG node is a constant (or unary plus of a constant)
bool isEffectivelyConstant(DAGNode* node) {
  if (!node) return false;
  if (node->kind == NodeKind::Constant) return true;
  // Handle unary plus: +constant
  if (node->kind == NodeKind::UnaryOp && node->op == '+' && 
      node->operands.size() == 1 && node->operands[0]->kind == NodeKind::Constant) {
    return true;
  }
  return false;
}

// Remove useless statements from a block
bool pruneBlock(BlockIR* block, IRModule& mod) {
  bool changed = false;
  
  // First: combine chained additive assignments (both VarDecl and Assign chains)
  if (combineChainedVarDeclAssignments(block, mod)) {
    changed = true;
  }
  if (combineChainedAssignAssignments(block, mod)) {
    changed = true;
  }
  
  auto it = block->stmts.begin();
  while (it != block->stmts.end()) {
    bool remove = false;
    
    if ((*it)->kind == StmtIRKind::ExprStmt) {
      // Remove useless expression statements (just variable refs)
      if (isUselessExprStmt(static_cast<ExprStmtIR*>(it->get()))) {
        remove = true;
      }
    } else if ((*it)->kind == StmtIRKind::Assign) {
      // Remove self-assignments
      if (isSelfAssign(static_cast<AssignIR*>(it->get()))) {
        remove = true;
      }
    } else if ((*it)->kind == StmtIRKind::VarDecl) {
      auto* decl = static_cast<VarDeclIR*>(it->get());
      // Remove zero-initialized vars with no real uses (only self-assignments/expr stmts)
      bool isZeroInit = !decl->init ||
                        (decl->init->kind == NodeKind::Constant && decl->init->constVal == 0);
      if (isZeroInit) {
        // We'll check uses after counting
        // Mark for potential removal
        remove = false; // Handle in second pass
      }
    }
    
    if (remove) {
      it = block->stmts.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  
  // Second pass: remove zero-init vars and unused const vars
  // Count remaining uses
  std::unordered_map<std::string, int> uses;
  countStmtUses(block, uses);
  
  it = block->stmts.begin();
  while (it != block->stmts.end()) {
    bool remove = false;
    if ((*it)->kind == StmtIRKind::VarDecl) {
      auto* decl = static_cast<VarDeclIR*>(it->get());
      bool isZeroInit = !decl->init ||
                        (decl->init->kind == NodeKind::Constant && decl->init->constVal == 0);
      // Also check for const variable declarations with effectively constant initializers that are never used
      bool isConstInit = decl->init && isEffectivelyConstant(decl->init);
      if (isZeroInit || isConstInit) {
        auto uit = uses.find(decl->name);
        if (uit == uses.end() || uit->second == 0) {
          remove = true;
        }
      }
    }
    if (remove) {
      it = block->stmts.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  
  return changed;
}

}  // namespace

void CleanupPass::run(IRModule& module) {
  // Iterate until no more cleanup opportunities
  for (int iter = 0; iter < 10; ++iter) {
    if (module.body && module.body->kind == StmtIRKind::Block) {
      auto* block = static_cast<BlockIR*>(module.body.get());
      if (pruneBlock(block, module)) {
        continue;
      }
    }
    break;
  }
}

}  // namespace cse