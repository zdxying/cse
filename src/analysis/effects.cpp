#include "effects.h"

#include "../ir/stmt_walk.h"

namespace cse {

namespace {

// A stable name for a call's callee. A variable or member access names the
// function; anything else is opaque. Passes never rename a callee, so this only
// has to distinguish calls from each other, not reconstruct the source.
std::string calleeAtom(const DAGNode* callee) {
  if (!callee) return "<none>";
  if (callee->kind == NodeKind::Variable || !callee->name.empty())
    return callee->name;
  return "<callee>";
}

void countExprEffects(const DAGNode* e, EffectSignature& out) {
  if (!e) return;
  if (e->kind == NodeKind::Call && !e->pure) {
    out["call:" + calleeAtom(e->operands.empty() ? nullptr : e->operands[0])]++;
  }
  if (isIncDec(e)) {
    // No target name: see the note on EffectSignature (alpha-renaming).
    out["incdec:" + e->name + (e->postfix ? ":post" : ":pre")]++;
  }
  for (const DAGNode* op : e->operands) countExprEffects(op, out);
}

void countStmtEffects(StmtIR* stmt, EffectSignature& out) {
  if (!stmt) return;
  if (stmt->kind == StmtIRKind::ForLoop) {
    auto* f = static_cast<ForLoopIR*>(stmt);
    countStmtEffects(f->init.get(), out);
    countExprEffects(f->cond, out);
    // The induction step is loop-control *only when it advances the loop's own
    // counter*: LoopUnroll removes both the loop and the counter declared in the
    // init, so `++i` on that variable is not an observable effect. An increment
    // of anything else in the update slot -- `for (i = 0; i < n; ++g)` -- names
    // no unrolled loop (a counted loop must advance its induction variable), so
    // it is a real effect and has to be counted; skipping *any* inc/dec here let
    // a pass drop it unnoticed. Anything that is not an inc/dec at all (a call
    // inside `i += g()`) is counted too.
    std::string counter;
    if (f->init && f->init->kind == StmtIRKind::VarDecl)
      counter = static_cast<VarDeclIR*>(f->init.get())->name;
    const bool counterStep =
        !counter.empty() && isIncDec(f->update) && !f->update->operands.empty() &&
        f->update->operands[0]->kind == NodeKind::Variable &&
        f->update->operands[0]->name == counter;
    if (f->update && !counterStep) countExprEffects(f->update, out);
    countExprEffects(f->updateRhs, out);
    countStmtEffects(f->body.get(), out);
    return;
  }
  // Every other statement's expression slots come from forEachExpr, the single
  // owner of that list (stmt_walk.h).
  forEachExpr(stmt, [&](DAGNode*& e) { countExprEffects(e, out); });
  switch (stmt->kind) {
    case StmtIRKind::Block:
      for (auto& s : static_cast<BlockIR*>(stmt)->stmts) countStmtEffects(s.get(), out);
      break;
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      countStmtEffects(ie->thenBranch.get(), out);
      countStmtEffects(ie->elseBranch.get(), out);
      break;
    }
    // These kinds own no nested statements: their own slots were already
    // counted by forEachExpr above.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      break;
    case StmtIRKind::ForLoop:
      // Unreachable: ForLoop is handled by the early return above (it needs the
      // counter-step exception). Listed so the switch stays exhaustive.
      break;
  }
}

}  // namespace

EffectSignature effectSignature(StmtIR* body) {
  EffectSignature sig;
  if (!body) return sig;
  countStmtEffects(body, sig);
  return sig;
}

// ---- Ordered view ----
//
// The walk mirrors countStmtEffects/countExprEffects exactly (same slots, same
// order), but appends each effect's atom to a list instead of bumping a counter.
// Keeping the two walks textually parallel is deliberate: a slot one of them
// forgets is a slot whose *count* and whose *position* both go unguarded, and
// the two oracles would then be blind in the same place.

void sequenceExprEffects(const DAGNode* e, std::vector<std::string>& out) {
  if (!e) return;
  if (e->kind == NodeKind::Call && !e->pure) {
    out.push_back("call:" + calleeAtom(e->operands.empty() ? nullptr
                                                           : e->operands[0]));
  }
  if (isIncDec(e)) {
    out.push_back("incdec:" + e->name + (e->postfix ? ":post" : ":pre"));
  }
  for (const DAGNode* op : e->operands) sequenceExprEffects(op, out);
}

void sequenceStmtEffects(StmtIR* stmt, std::vector<std::string>& out) {
  if (!stmt) return;
  if (stmt->kind == StmtIRKind::ForLoop) {
    auto* f = static_cast<ForLoopIR*>(stmt);
    sequenceStmtEffects(f->init.get(), out);
    sequenceExprEffects(f->cond, out);
    // The induction-step exception, character for character as in
    // countStmtEffects: the counter step is loop-control, everything else in the
    // update slot is observable.
    std::string counter;
    if (f->init && f->init->kind == StmtIRKind::VarDecl)
      counter = static_cast<VarDeclIR*>(f->init.get())->name;
    const bool counterStep =
        !counter.empty() && isIncDec(f->update) && !f->update->operands.empty() &&
        f->update->operands[0]->kind == NodeKind::Variable &&
        f->update->operands[0]->name == counter;
    if (f->update && !counterStep) sequenceExprEffects(f->update, out);
    sequenceExprEffects(f->updateRhs, out);
    sequenceStmtEffects(f->body.get(), out);
    return;
  }
  forEachExpr(stmt, [&](DAGNode*& e) { sequenceExprEffects(e, out); });
  switch (stmt->kind) {
    case StmtIRKind::Block:
      for (auto& s : static_cast<BlockIR*>(stmt)->stmts)
        sequenceStmtEffects(s.get(), out);
      break;
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      sequenceStmtEffects(ie->thenBranch.get(), out);
      sequenceStmtEffects(ie->elseBranch.get(), out);
      break;
    }
    // These kinds own no nested statements; their own slots were visited above.
    case StmtIRKind::ExprStmt:
    case StmtIRKind::Assign:
    case StmtIRKind::VarDecl:
    case StmtIRKind::Return:
      break;
    case StmtIRKind::ForLoop:
      // Unreachable: the early return above handles it.
      break;
  }
}

std::vector<std::string> effectSequence(StmtIR* body) {
  std::vector<std::string> seq;
  if (!body) return seq;
  sequenceStmtEffects(body, seq);
  return seq;
}

std::string effectOrderViolation(const std::vector<std::string>& before,
                                 const std::vector<std::string>& after,
                                 bool mayDuplicate) {
  // Every effect of `before` must survive (a dropped effect is caught here as
  // well as by the multiset check), and its *first* occurrence in `after` must
  // come after the first occurrence of every effect that preceded it in
  // `before`. Scanning `after` monotonically makes this a subsequence test,
  // which is exactly "the relative order is unchanged": duplication is allowed
  // (unrolling), reordering is not.
  std::string msg;
  size_t cursor = 0;
  for (const auto& atom : before) {
    size_t found = cursor;
    while (found < after.size() && after[found] != atom) ++found;
    if (found == after.size()) {
      msg += "  effect '" + atom +
             "' has no in-order occurrence after its predecessor\n";
      continue;
    }
    cursor = found + 1;
  }
  (void)mayDuplicate;  // duplication is inherent to a subsequence test
  return msg;
}

std::string effectViolation(const EffectSignature& before,
                            const EffectSignature& after, bool mayDuplicate) {
  std::string msg;
  for (const auto& kv : before) {
    auto it = after.find(kv.first);
    const int now = it == after.end() ? 0 : it->second;
    if (now < kv.second) {
      msg += "  dropped effect '" + kv.first + "' (" +
             std::to_string(kv.second) + " -> " + std::to_string(now) + ")\n";
    }
  }
  if (!mayDuplicate) {
    for (const auto& kv : after) {
      auto it = before.find(kv.first);
      const int was = it == before.end() ? 0 : it->second;
      if (kv.second > was) {
        msg += "  duplicated effect '" + kv.first + "' (" +
               std::to_string(was) + " -> " + std::to_string(kv.second) + ")\n";
      }
    }
  }
  return msg;
}

}  // namespace cse
