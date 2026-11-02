#include "cse_pass.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../analysis/effects.h"
#include "../ir/ir_module.h"
#include "../ir/ir_utils.h"
#include "../ir/location.h"
#include "../ir/statement.h"
#include "../ir/stmt_walk.h"

namespace cse {

// ===== Phase 1: Collect DAG nodes per statement =====

// Every node reachable from an expression root.
static void collectExprNodes(DAGNode* root, std::unordered_set<DAGNode*>& out) {
  if (!root) return;
  std::vector<DAGNode*> stack = {root};
  while (!stack.empty()) {
    DAGNode* n = stack.back();
    stack.pop_back();
    if (out.insert(n).second) {
      for (auto* op : n->operands) stack.push_back(op);
    }
  }
}

// Walk a statement tree and collect every DAGNode* that appears in an
// expression position, nested statements included. Each node is then recorded
// with the index of the top-level statement that contains it.
static void collectNodesFromStmt(StmtIR* stmt,
                                 std::unordered_set<DAGNode*>& out) {
  if (!stmt) return;
  forEachExpr(stmt, [&](DAGNode*& e) { collectExprNodes(e, out); });
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* block = static_cast<BlockIR*>(stmt);
      for (auto& s : block->stmts) collectNodesFromStmt(s.get(), out);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      collectNodesFromStmt(f->init.get(), out);
      collectNodesFromStmt(f->body.get(), out);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      collectNodesFromStmt(ie->thenBranch.get(), out);
      collectNodesFromStmt(ie->elseBranch.get(), out);
      break;
    }
    // These kinds own no nested statements. Listed explicitly so a new
    // StmtIRKind is a compile error here rather than a silent skip.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      break;
  }
}

// How many distinct non-leaf nodes does the (top-level) statement list contain?
// The structural bound for the CSE fixpoint: each iteration extracts one
// candidate and replaces it everywhere, so no node can be extracted twice.
int countNonLeafNodes(BlockIR* block) {
  std::unordered_set<DAGNode*> seen;
  for (auto& s : block->stmts) {
    std::unordered_set<DAGNode*> nodes;
    collectNodesFromStmt(s.get(), nodes);
    for (auto* n : nodes)
      if (n->kind != NodeKind::Constant && n->kind != NodeKind::Variable)
        seen.insert(n);
  }
  return static_cast<int>(seen.size());
}

// ===== Nested (conditionally executed) node collection =====
// A subexpression that appears in a nested statement (inside an if/else/loop)
// must not be hoisted to the top level by CSE: that would speculate the
// computation. Such nodes are excluded from extraction.
//
// That statement-level rule is deliberately blunt. The *expression-level* lazy
// operators need a finer answer, because "hoist it out" is safe for most
// operands and unsafe for a few:
//
//   * `cond ? then : else` -- `then`/`else` are guarded, `cond` is not;
//   * `a && b` / `a || b` -- `b` is guarded, `a` is not.
//
// Hoisting a guarded operand evaluates it unconditionally. For `a * b` that
// yields the same value, so the extraction is still worth doing; for `1 / k` or
// `arr[i]` it can trap, so it must not happen. `isSpeculatable`
// (analysis/purity.h) is exactly that distinction, and it is the predicate the
// candidate filter consults for nodes in `guardedNodes`.
//
// Missing this was a silent miscompile: `k ? 1/k : 0` written twice had its
// `1/k` extracted to the top, so it ran unconditionally and trapped at k = 0.

// Every node reached through a lazy operator's guarded operand.
static void collectGuardedExprNodes(DAGNode* e,
                                    std::unordered_set<DAGNode*>& out) {
  if (!e) return;
  std::vector<DAGNode*> stack = {e};
  while (!stack.empty()) {
    DAGNode* n = stack.back();
    stack.pop_back();
    if (n->kind == NodeKind::Ternary && n->operands.size() == 3) {
      // The condition is always evaluated; only the two arms are guarded.
      collectExprNodes(n->operands[1], out);
      collectExprNodes(n->operands[2], out);
      stack.push_back(n->operands[0]);
      continue;
    }
    if (n->kind == NodeKind::BinaryOp &&
        (n->op == OpCode::LogicalAnd || n->op == OpCode::LogicalOr) &&
        n->operands.size() == 2) {
      collectExprNodes(n->operands[1], out);
      stack.push_back(n->operands[0]);
      continue;
    }
    for (auto* op : n->operands) stack.push_back(op);
  }
}

static void collectNestedNodes(StmtIR* stmt, bool nested,
                               std::unordered_set<DAGNode*>& out) {
  if (!stmt) return;
  // A loop owns its condition and update whatever the enclosing context is --
  // they do not run once at the top level -- so those always count as nested.
  // Everything else is nested only when the caller says so.
  if (nested || stmt->kind == StmtIRKind::ForLoop) {
    forEachExpr(stmt, [&](DAGNode*& e) { collectExprNodes(e, out); });
  }
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) collectNestedNodes(s.get(), nested, out);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      collectNestedNodes(ie->thenBranch.get(), true, out);
      collectNestedNodes(ie->elseBranch.get(), true, out);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      collectNestedNodes(f->init.get(), true, out);
      collectNestedNodes(f->body.get(), true, out);
      break;
    }
    // These kinds own no nested statements (see collectNodesFromStmt).
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      break;
  }
}

// ===== Write / dependency analysis =====
// The IR interns variables by name (IRModule::getVar), so reassigning a
// variable does NOT create a new node: two textually identical subexpressions
// stay the same DAG node even though they denote different values. Extracting
// such a node to a single definition point would therefore bind every later use
// to the value computed at that point, which is wrong whenever one of the
// variables it reads has been written in between. `collectWriteLocations` (in
// ir/location.h, shared with ValueProp) supplies the first half of that check.

// Is it safe to hoist `node` to just before statement `insertPos`, given that it
// is used by the statements in `uses`? Only if no statement strictly between the
// definition point and a later use writes a variable the node reads.
//
// The read is resolved through reference edges by `writeAffectsRead`: a node
// reading `s3` (a reference to `b`) is changed by a write to `b` even though the
// two roots differ.
static bool usesAreStable(DAGNode* node, const std::vector<size_t>& uses,
                          size_t insertPos,
                          const std::vector<LocationSet>& writes,
                          const AliasInfo& aliases) {
  std::unordered_set<std::string> deps;
  collectReadNames(node, deps);
  if (deps.empty()) return true;
  for (size_t u : uses) {
    if (u <= insertPos) continue;
    // The use statement itself is included: a write it performs can be
    // sequenced before the read (`arr[a + i] = i++` evaluates the right-hand
    // side first in C++17), so its own write set has to count. `writeAffectsRead`
    // is wildcard-aware, so an impure call's wildcard rejects the extraction.
    for (size_t k = insertPos; k <= u && k < writes.size(); ++k) {
      for (const std::string& d : deps) {
        if (aliases.writeAffectsRead(writes[k], d)) return false;
      }
    }
  }
  return true;
}

// ===== Phase 2: Replace all references to a target node with a variable =====

// Rewrite every reference to `target` *inside* `node` -- never `node` itself.
//
// The rewrite is bottom-up and produces its results through the module's
// factories rather than assigning into an existing node's operand vector. That
// assignment was the last place where an interned node's content could change
// behind its hash: the node stayed registered under a hash that described a
// different node, so the same node could later be handed back for two different
// expressions. IRModule::verify() now catches that class of write.
static DAGNode* rewriteOperands(IRModule& module, DAGNode* node, DAGNode* target,
                                DAGNode* replacement) {
  if (!node) return nullptr;
  bool changed = false;
  std::vector<DAGNode*> ops;
  ops.reserve(node->operands.size());
  for (DAGNode* op : node->operands) {
    DAGNode* r = (op == target)
                     ? replacement
                     : rewriteOperands(module, op, target, replacement);
    ops.push_back(r);
    if (r != op) changed = true;
  }
  if (!changed) return node;
  return rebuildWithOperands(module, node, ops);
}

// Recursively walk a statement tree and replace all DAGNode* that match
// `target` with `replacement` in expression positions.
static void replaceRefsInStmt(IRModule& module, StmtIR* stmt, DAGNode* target,
                               DAGNode* replacement) {
  if (!stmt) return;

  // The lvalue of an element/member store is *written*, not evaluated: swapping
  // it for a temporary would turn the store into a store to the temporary. Only
  // its index operands -- ordinary expressions -- are rewritten, which is why
  // the root of the lvalue is passed to rewriteOperands rather than to the
  // expression-slot rewrite below.
  if (stmt->kind == StmtIRKind::Assign) {
    auto* assign = static_cast<AssignIR*>(stmt);
    if (assign->targetExpr) {
      assign->targetExpr =
          rewriteOperands(module, assign->targetExpr, target, replacement);
    }
  }
  forEachExpr(
      stmt,
      [&](DAGNode*& e) {
        e = (e == target) ? replacement
                          : rewriteOperands(module, e, target, replacement);
      },
      /*includeLvalue=*/false);

  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* block = static_cast<BlockIR*>(stmt);
      for (auto& s : block->stmts)
        replaceRefsInStmt(module, s.get(), target, replacement);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      replaceRefsInStmt(module, f->init.get(), target, replacement);
      replaceRefsInStmt(module, f->body.get(), target, replacement);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      replaceRefsInStmt(module, ie->thenBranch.get(), target, replacement);
      replaceRefsInStmt(module, ie->elseBranch.get(), target, replacement);
      break;
    }
    // These kinds own no nested statements (see collectNodesFromStmt).
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      break;
  }
}

// ===== Main CSE Pass =====

void CSEPass::run(IRModule& module) {
  auto* root = module.body.get();
  if (!root || root->kind != StmtIRKind::Block) return;

  auto* block = static_cast<BlockIR*>(root);

  // Reference aliases for the whole function: a write through a reference local
  // also writes its referent, so an extraction reading the referent is not
  // hoisted across it (ir/location.h).
  AliasInfo aliases = AliasInfo::build(root);

  // We iterate multiple times because extracting one CSE opportunity
  // may reveal new ones (e.g., after replacing, a previously unique
  // subexpression may now be shared).
  //
  // The bound is structural: each iteration extracts exactly one candidate and
  // that node is then replaced everywhere, so it can never be a candidate
  // again -- the number of distinct non-leaf nodes bounds the extractions. The
  // old cap of 100 silently truncated a region with more independent
  // opportunities (150 shared products produced only 100 temporaries), so the
  // result depended on how many times the pipeline ran.
  const int maxIterations = 1 + countNonLeafNodes(block);
  for (int iteration = 0; iteration < maxIterations; ++iteration) {
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

    // Nodes used inside nested (conditionally executed) statements must not be
    // hoisted to the top level. Collect them once and exclude from extraction.
    std::unordered_set<DAGNode*> nestedNodes;
    for (auto& s : block->stmts) collectNestedNodes(s.get(), false, nestedNodes);

    // Nodes that are only *guarded* by a lazy operator inside a top-level
    // statement (`k ? a*b : 0`, `a && b`). Hoisting one evaluates it
    // unconditionally, which is fine when the value is the same either way and
    // unsafe when it can trap -- `isSpeculatable` (analysis/purity.h) is the
    // single authority for that. A guarded node that is itself nested at the
    // statement level stays excluded by the set above.
    std::unordered_set<DAGNode*> guardedNodes;
    for (auto& s : block->stmts) {
      if (s->kind == StmtIRKind::ForLoop || s->kind == StmtIRKind::IfElse)
        continue;  // statement-level nesting already covers these
      forEachExpr(s.get(), [&](DAGNode*& e) {
        collectGuardedExprNodes(e, guardedNodes);
      });
    }

    // Per-statement written variables, needed to reject extractions whose value
    // would be reused across a write to one of its operands.
    std::vector<LocationSet> writesPerStmt(block->stmts.size());
    for (size_t i = 0; i < block->stmts.size(); i++)
      writesPerStmt[i] = aliases.writtenBy(block->stmts[i].get());

    // Phase 2: Find nodes used in >= 2 different statements
    struct CSECandidate {
      DAGNode* node;
      std::vector<size_t> stmts;
    };
    std::vector<CSECandidate> candidates;
    for (auto& [node, stmtIndices] : nodeToStmts) {
      // Remove duplicate statement indices (a node can appear multiple times
      // in the same statement's expression tree)
      std::sort(stmtIndices.begin(), stmtIndices.end());
      stmtIndices.erase(std::unique(stmtIndices.begin(), stmtIndices.end()),
                        stmtIndices.end());
      if (stmtIndices.size() < 2) continue;
      if (nestedNodes.count(node)) continue;
      // A node reached only through a lazy operator's guarded operand would be
      // evaluated unconditionally once hoisted. That is allowed exactly when
      // the evaluation cannot trap; `1/k` and `arr[i]` are refused.
      if (guardedNodes.count(node) && !isSpeculatable(node)) continue;
      candidates.push_back({node, stmtIndices});
    }

    if (candidates.empty()) break;

    // Sort by benefit: prefer nodes that appear in more statements, breaking
    // ties by node id. The candidate vector is built by iterating an
    // unordered_map keyed on pointers, so without a deterministic tie-break the
    // chosen extraction (and thus the emitted code) would depend on heap layout.
    std::sort(candidates.begin(), candidates.end(),
              [](const CSECandidate& a, const CSECandidate& b) {
                if (a.stmts.size() != b.stmts.size())
                  return a.stmts.size() > b.stmts.size();
                return a.node->id < b.node->id;
              });

    bool changed = false;
    for (auto& cand : candidates) {
      DAGNode* target = cand.node;

      // No presence check is needed here: `candidates` was built from the nodes
      // of the current block in phase 1, and the block is not modified until the
      // single extraction at the end of this loop body, so every candidate is
      // still present.

      // Reject the candidate if any of the variables it reads is written
      // between the definition point and a later use. `stmts` is sorted and
      // deduplicated, so front() is where the definition would be inserted.
      if (!usesAreStable(target, cand.stmts, cand.stmts.front(), writesPerStmt,
                         aliases))
        continue;

      // Create variable name. `freshName` is the one authority for invented
      // names: without it a user variable spelled `_cse_0_7` would *be* the
      // extracted temporary (variables are interned by name), and the user's
      // value would be propagated into every use of the shared expression.
      std::string varName = module.freshName(
          "_cse_" + std::to_string(iteration) + "_" +
          std::to_string(target->id));

      // Create the variable node (via getVar for proper caching)
      DAGNode* varNode = module.getVar(varName);

      // Create VarDecl for the extracted expression
      auto decl = std::make_unique<VarDeclIR>();
      decl->type = "auto";
      decl->name = varName;
      decl->init = target;

      // Replace all references to target with varNode across all statements
      for (auto& stmt : block->stmts) {
        replaceRefsInStmt(module, stmt.get(), target, varNode);
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
