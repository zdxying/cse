#include "cleanup.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../analysis/effects.h"
#include "../analysis/purity.h"
#include "../ir/ir_module.h"
#include "../ir/ir_utils.h"
#include "../ir/location.h"
#include "../ir/statement.h"
#include "../ir/stmt_walk.h"

namespace cse {

namespace {

// How many statements does this tree contain? The structural bound for
// Cleanup's fixpoint: every productive round removes or rewrites at least one
// statement, and the tree only shrinks, so the initial statement count bounds
// the rounds. A hand-picked constant (it was 10) could stop early on a long
// chain, leaving work a second run would finish.
int countStmts(const StmtIR* stmt) {
  if (!stmt) return 0;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      int n = 1;
      for (const auto& s : static_cast<const BlockIR*>(stmt)->stmts)
        n += countStmts(s.get());
      return n;
    }
    case StmtIRKind::ForLoop: {
      const auto* f = static_cast<const ForLoopIR*>(stmt);
      return 1 + countStmts(f->init.get()) + countStmts(f->body.get());
    }
    case StmtIRKind::IfElse: {
      const auto* ie = static_cast<const IfElseIR*>(stmt);
      return 1 + countStmts(ie->thenBranch.get()) +
             countStmts(ie->elseBranch.get());
    }
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      return 1;
  }
  return 0;
}

// Check if a DAG node is a simple variable reference with no side effects
bool isTrivialVarRef(DAGNode* node) {
  if (!node) return false;
  return node->kind == NodeKind::Variable;
}

// Check if an expression statement is just a variable reference (useless).
// The caller must still exclude `break;`/`continue;`, which this IR also
// spells as a bare variable (see isLoopControl in ir/stmt_walk.h).
bool isUselessExprStmt(ExprStmtIR* stmt) {
  if (!stmt || !stmt->expr) return false;
  return isTrivialVarRef(stmt->expr);
}

// `isEffectFreeLvalue` (analysis/purity.h) owns the "is this a side-effect-free
// memory access" test used to guard the removal of a complex-lvalue
// self-assignment. It used to be a private copy here.

// Check if an assignment is a self-assignment.
//   - simple target: `x = x`
//   - complex lvalue: `a[i] = a[i]` / `p->x = p->x` (structurally identical,
//     side-effect-free). Non-shareable memory loads are distinct DAGNode*
//     even when structurally equal, so compare with NodeEqual, not pointers.
bool isSelfAssign(AssignIR* assign) {
  if (!assign || !assign->value) return false;
  // `a[i] += a[i]` has equal target and value but doubles the element; it is not
  // a no-op.
  if (assign->compoundOp != OpCode::None) return false;

  if (!assign->targetExpr) {
    return assign->value->kind == NodeKind::Variable &&
           assign->value->name == assign->target;
  }
  return isEffectFreeLvalue(assign->targetExpr) &&
         NodeEqual{}(assign->targetExpr, assign->value);
}

// Check if a DAG node is a binary '+' operation
bool isAddOp(DAGNode* node) {
  return node && node->kind == NodeKind::BinaryOp && node->op == OpCode::Add;
}

// Check if a DAG node is a zero constant
bool isZeroConstant(DAGNode* node) {
  return node && node->kind == NodeKind::Constant && node->constVal == 0;
}

// Try to extract addend from x + a or a + x where x is the target variable.
// The addend must not itself read the target: merging `x = 0; x = x + x*b;`
// into `x = x*b` would read `x` before it is initialized.
DAGNode* extractAddend(IRModule& /*mod*/, const std::string& varName, DAGNode* expr) {
  if (!isAddOp(expr)) return nullptr;
  
  DAGNode* lhs = expr->operands[0];
  DAGNode* rhs = expr->operands[1];
  
  // We're looking for: x + rhs where x is the target variable
  // (order could be either: x + a or a + x)
  DAGNode* addend = nullptr;
  if (lhs->kind == NodeKind::Variable && lhs->name == varName) {
    addend = rhs;
  } else if (rhs->kind == NodeKind::Variable && rhs->name == varName) {
    addend = lhs;
  } else {
    return nullptr;
  }
  std::unordered_set<std::string> reads;
  collectReadNames(addend, reads);
  if (reads.count(varName)) return nullptr;
  // The merge rewrites two sequenced statements into one expression, so the
  // addend must be safe to evaluate in an unsequenced operand position and must
  // not have an observable effect: `x = x + (++y); x = x + (++y);` became the
  // unsequenced `x = ++y + ++y;` (UB), and a call lost its guaranteed order.
  if (!isReferentiallyTransparent(addend)) return nullptr;
  return addend;
}

// Every VarDecl in the function, by (alpha-renamed) name, with its alias class.
// The chain merge drops the accumulator's leading zero store, which is
// unobservable only when the accumulator is a private local: for a reference
// parameter or a global, a callee can observe the zero.
void collectDeclClasses(StmtIR* stmt,
                        std::unordered_map<std::string, AliasClass>& out) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block:
      for (auto& s : static_cast<BlockIR*>(stmt)->stmts)
        collectDeclClasses(s.get(), out);
      break;
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      collectDeclClasses(f->init.get(), out);
      collectDeclClasses(f->body.get(), out);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      collectDeclClasses(ie->thenBranch.get(), out);
      collectDeclClasses(ie->elseBranch.get(), out);
      break;
    }
    case StmtIRKind::VarDecl: {
      auto* d = static_cast<VarDeclIR*>(stmt);
      if (!d->name.empty()) out[d->name] = classifyDeclaredType(d->type);
      break;
    }
    // These kinds declare nothing (VarDecl is handled above).
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::Return:
      break;
  }
}

bool isPrivateAccumulator(
    const std::string& name,
    const std::unordered_map<std::string, AliasClass>& declClasses) {
  auto it = declClasses.find(name);
  return it != declClasses.end() && it->second == AliasClass::PrivateLocal;
}

// Try to combine chained VarDecl assignments:
// Pattern: VarDecl(x, 0) -> Assign(x, x+a) -> Assign(x, x+b) -> ...
// Replace with single VarDecl: x = a + b + ...
bool combineChainedVarDeclAssignments(
    BlockIR* block, IRModule& mod,
    const std::unordered_map<std::string, AliasClass>& declClasses) {
  bool changed = false;
  
  for (size_t i = 0; i + 1 < block->stmts.size(); ++i) {
    // Look for VarDecl with zero init
    if (block->stmts[i]->kind != StmtIRKind::VarDecl) continue;
    auto* decl = static_cast<VarDeclIR*>(block->stmts[i].get());
    if (!isZeroConstant(decl->init)) continue;
    if (!isPrivateAccumulator(decl->name, declClasses)) continue;
    
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
        combined = mod.createBinaryOp(OpCode::Add, combined, addends[k]);
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
bool combineChainedAssignAssignments(
    BlockIR* block, IRModule& mod,
    const std::unordered_map<std::string, AliasClass>& declClasses) {
  bool changed = false;
  
  for (size_t i = 0; i + 1 < block->stmts.size(); ++i) {
    // Look for Assign with zero init (x = 0)
    if (block->stmts[i]->kind != StmtIRKind::Assign) continue;
    auto* assign = static_cast<AssignIR*>(block->stmts[i].get());
    if (assign->targetExpr || !isZeroConstant(assign->value)) continue;
    if (!isPrivateAccumulator(assign->target, declClasses)) continue;
    
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
        combined = mod.createBinaryOp(OpCode::Add, combined, addends[k]);
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

// Check if a DAG node is a constant (or unary plus of a constant)
bool isEffectivelyConstant(DAGNode* node) {
  if (!node) return false;
  if (node->kind == NodeKind::Constant) return true;
  // Handle unary plus: +constant
  if (node->kind == NodeKind::UnaryOp && node->op == OpCode::Pos &&
      node->operands.size() == 1 && node->operands[0]->kind == NodeKind::Constant) {
    return true;
  }
  return false;
}

// Remove useless statements from a block
bool pruneBlock(BlockIR* block, IRModule& mod, bool allowUnsafeFpIdentities);

// Recurse into a nested statement so its blocks get the same cleanup. A nested
// `if` / `for` body / bare `{ }` is a separate scope that the top-level pass
// never visited; its own statements are cleaned when that scope's block is
// pruned. `allowUnsafeFpIdentities` (the chain-merge licence) is threaded so the
// gate holds in every scope.
bool pruneNested(StmtIR* s, IRModule& mod, bool allowUnsafeFpIdentities) {
  if (!s) return false;
  switch (s->kind) {
    case StmtIRKind::Block:
      return pruneBlock(static_cast<BlockIR*>(s), mod, allowUnsafeFpIdentities);
    case StmtIRKind::ForLoop:
      return pruneNested(static_cast<ForLoopIR*>(s)->body.get(), mod,
                         allowUnsafeFpIdentities);
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(s);
      bool changed =
          pruneNested(ie->thenBranch.get(), mod, allowUnsafeFpIdentities);
      if (pruneNested(ie->elseBranch.get(), mod, allowUnsafeFpIdentities)) {
        changed = true;
      }
      return changed;
    }
    // These kinds introduce no nested scope, so there is nothing to recurse
    // into. Listed explicitly so a new StmtIRKind is a compile error here.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      return false;
  }
  // Unreachable: every StmtIRKind returns inside the switch. Kept so the
  // function stays well-formed if a kind is ever added without being handled
  // here (the compiler reports that separately via -Wswitch).
  return false;
}

bool pruneBlock(BlockIR* block, IRModule& mod, bool allowUnsafeFpIdentities) {
  bool changed = false;

  // First: combine chained additive assignments (both VarDecl and Assign chains).
  //
  // This merge drops the accumulator's leading zero, so it changes the sign of
  // a zero result (`x = 0; x = x + a; x = x + a` yields +0.0 at a = -0.0, while
  // `x = a + a` yields -0.0) -- the same signed-zero licence as `x + 0 -> x`.
  // Under the conservative profile it must not fire, or `-s` stops being
  // IEEE-exact. It is deliberately not deleted: the default FreeLB profile
  // licenses it and relies on the merge.
  if (allowUnsafeFpIdentities) {
    std::unordered_map<std::string, AliasClass> declClasses;
    collectDeclClasses(mod.body.get(), declClasses);
    // A by-value parameter is a private copy of the caller's argument: writing
    // it (and dropping a zero store to it) is unobservable. A reference or
    // pointer parameter is not -- that is the aliasing case this gate exists
    // for.
    for (const auto& param : mod.funcSig.params) {
      if (classifyDeclaredType(param.type) == AliasClass::PrivateLocal)
        declClasses[param.name] = AliasClass::PrivateLocal;
    }
    if (combineChainedVarDeclAssignments(block, mod, declClasses)) {
      changed = true;
    }
    if (combineChainedAssignAssignments(block, mod, declClasses)) {
      changed = true;
    }
  }
  
  auto it = block->stmts.begin();
  while (it != block->stmts.end()) {
    bool remove = false;
    
    if ((*it)->kind == StmtIRKind::ExprStmt) {
      // Remove useless expression statements (just variable refs). A `break;` /
      // `continue;` is *also* a bare-variable ExprStmt in this IR, so it must be
      // excluded: erasing it silently changes control flow.
      if (!isLoopControl(it->get()) &&
          isUselessExprStmt(static_cast<ExprStmtIR*>(it->get()))) {
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
  // Count remaining uses. The shared `countReadNames` (ir/location.h) walks
  // every expression slot through forEachExprDeep, so a variable read only in a
  // store lvalue -- `a[x] = v;`, whose index is ordinary arithmetic -- is seen
  // here. The private kind-by-kind counter this replaces listed only the
  // *value* of an Assign, so such a declaration looked unused and was deleted,
  // leaving the store referring to a name that no longer existed.
  std::unordered_map<std::string, int> uses = countReadNames(block);
  // `countReadNames` counts *reads* only. A declaration that is still written --
  // `double unused = 0.0; unused = y++;` -- has zero reads but must be kept:
  // dropping it leaves the store referring to a variable that no longer exists,
  // and the emitted code does not compile. DCE applies the same rule, so the two
  // passes have to agree on it.
  LocationSet written = AliasInfo::build(mod.body.get()).writtenBy(block);
  
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
        if ((uit == uses.end() || uit->second == 0) &&
            !written.containsRoot(decl->name)) {
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

  // Recurse into nested blocks (`if`/`for` bodies, bare `{ }`) so their
  // statements get the same treatment. The analyses above are already deep
  // (`countReadNames` / `collectWriteLocations` walk nested scopes), but the
  // erasures only ever touched this block's own statements -- a nested
  // self-assignment or dead zero-init declaration was never removed.
  for (auto& s : block->stmts) {
    if (pruneNested(s.get(), mod, allowUnsafeFpIdentities)) changed = true;
  }

  return changed;
}

}  // namespace

void CleanupPass::run(IRModule& module) {
  // Iterate until no more cleanup opportunities. The bound is structural: every
  // productive round removes or rewrites at least one statement, and the tree
  // only shrinks, so the initial statement count bounds the rounds.
  const int maxRounds = 1 + countStmts(module.body.get());
  for (int iter = 0; iter < maxRounds; ++iter) {
    if (module.body && module.body->kind == StmtIRKind::Block) {
      auto* block = static_cast<BlockIR*>(module.body.get());
      if (pruneBlock(block, module, _allowUnsafeFpIdentities)) {
        continue;
      }
    }
    break;
  }
}

}  // namespace cse