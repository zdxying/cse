#include "location.h"

#include "stmt_walk.h"

namespace cse {

// ===== Alias classes =====

AliasClass classifyDeclaredType(const std::string& type) {
  if (type.find('&') != std::string::npos) return AliasClass::Reference;
  if (type.find('*') != std::string::npos) return AliasClass::Pointer;
  if (type.find('[') != std::string::npos) return AliasClass::Array;
  return AliasClass::PrivateLocal;
}

// ===== Location =====

bool operator==(const Location& a, const Location& b) {
  return a.root == b.root && a.access == b.access && a.unknown == b.unknown;
}

bool mayAlias(const Location& a, const Location& b) {
  if (a.unknown || b.unknown) return true;
  // A location with no nameable root (a call result, say) cannot be shown
  // disjoint from anything.
  if (a.root.empty() || b.root.empty()) return true;
  // Any two accesses within one root reach the same base storage. Distinct
  // roots are disjoint here: reference-referent edges have already been folded
  // into the set (AliasInfo::closeWrites), so a write through a reference also
  // names the root it refers to.
  return a.root == b.root;
}

void LocationSet::add(const Location& loc) {
  if (loc.unknown) {
    _anything = true;
    return;
  }
  for (const auto& l : _items)
    if (l == loc) return;
  _items.push_back(loc);
}

void LocationSet::addRoot(const std::string& root, AccessKind access) {
  if (root.empty()) return;
  Location loc;
  loc.root = root;
  loc.access = access;
  add(loc);
}

bool LocationSet::containsRoot(const std::string& root) const {
  if (root.empty()) return false;
  for (const auto& l : _items)
    if (l.root == root) return true;
  return false;
}

bool LocationSet::aliasesName(const std::string& name) const {
  if (_anything) return true;
  if (name.empty()) return false;
  Location loc;
  loc.root = name;
  for (const auto& l : _items)
    if (mayAlias(l, loc)) return true;
  return false;
}

// ===== Root extraction =====

std::string locationRoot(DAGNode* n) {
  // Only descend through nodes whose operands[0] is the *storage base* of an
  // lvalue. A Ternary's operands[0] is its condition and a Call's is the
  // callee, so walking them blindly recorded a write to the condition variable
  // and missed the real store: `(c ? s : s).x = 5.0` was recorded as a write of
  // `c`, leaving `s` free for value propagation to read across the store.
  // An unnameable base returns "" (the caller turns that into a wildcard).
  while (n) {
    if (n->kind == NodeKind::Variable) return n->name;
    switch (n->kind) {
      case NodeKind::ArrayAccess:
      case NodeKind::MemberAccess:
      case NodeKind::ArrowAccess:
      case NodeKind::Cast:
        if (n->operands.empty()) return "";
        n = n->operands[0];
        break;
      default:
        return "";
    }
  }
  return "";
}

Location locationOf(DAGNode* n) {
  Location loc;
  loc.root = locationRoot(n);
  // A store through a base we cannot name may reach any object (a call result,
  // a conditional lvalue). Treat it as the wildcard rather than dropping the
  // write: dropping it is what let ValueProp hoist a load across the store.
  if (loc.root.empty()) loc.unknown = true;
  if (n) {
    switch (n->kind) {
      case NodeKind::ArrayAccess: loc.access = AccessKind::Indexed; break;
      case NodeKind::MemberAccess: loc.access = AccessKind::Member; break;
      case NodeKind::ArrowAccess: loc.access = AccessKind::Arrow; break;
      default: break;
    }
  }
  return loc;
}

// ===== Read collection =====

void collectReadNames(DAGNode* e, std::unordered_set<std::string>& out) {
  if (!e) return;
  if (e->kind == NodeKind::Variable && !e->name.empty()) out.insert(e->name);
  for (auto* op : e->operands) collectReadNames(op, out);
}

void collectReadNames(StmtIR* stmt, std::unordered_set<std::string>& out) {
  forEachExprDeep(stmt, [&](DAGNode*& e) { collectReadNames(e, out); });
}

void countReadNames(DAGNode* e, std::unordered_map<std::string, int>& out) {
  if (!e) return;
  if (e->kind == NodeKind::Variable) out[e->name]++;
  for (auto* op : e->operands) countReadNames(op, out);
}

std::unordered_map<std::string, int> countReadNames(StmtIR* root) {
  std::unordered_map<std::string, int> counts;
  forEachExprDeep(root, [&](DAGNode*& e) { countReadNames(e, counts); });
  return counts;
}

void collectMentionedNames(StmtIR* stmt, std::unordered_set<std::string>& out) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block:
      for (auto& s : static_cast<BlockIR*>(stmt)->stmts)
        collectMentionedNames(s.get(), out);
      break;
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      collectMentionedNames(f->init.get(), out);
      collectReadNames(f->cond, out);
      collectReadNames(f->update, out);
      collectReadNames(f->updateRhs, out);
      collectMentionedNames(f->body.get(), out);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      collectReadNames(ie->cond, out);
      collectMentionedNames(ie->thenBranch.get(), out);
      collectMentionedNames(ie->elseBranch.get(), out);
      break;
    }
    case StmtIRKind::ExprStmt:
      collectReadNames(static_cast<ExprStmtIR*>(stmt)->expr, out);
      break;
    case StmtIRKind::Assign: {
      auto* a = static_cast<AssignIR*>(stmt);
      if (!a->target.empty()) out.insert(a->target);
      collectReadNames(a->targetExpr, out);
      collectReadNames(a->value, out);
      break;
    }
    case StmtIRKind::VarDecl: {
      auto* d = static_cast<VarDeclIR*>(stmt);
      if (!d->name.empty()) out.insert(d->name);
      collectReadNames(d->init, out);
      break;
    }
    case StmtIRKind::Return:
      collectReadNames(static_cast<ReturnIR*>(stmt)->value, out);
      break;
  }
}

// ===== Write collection =====

void collectWriteLocations(DAGNode* e, LocationSet& out) {
  if (!e) return;
  if (e->kind == NodeKind::Call && !e->pure) out.addUnknown();
  if (isIncDec(e) && !e->operands.empty()) {
    const std::string root = locationRoot(e->operands[0]);
    if (root.empty()) out.addUnknown();
    else out.addRoot(root);
  } else if (e->kind == NodeKind::BinaryOp && e->op == OpCode::Assign &&
             e->operands.size() == 2) {
    const std::string root = locationRoot(e->operands[0]);
    if (root.empty()) out.addUnknown();
    else out.addRoot(root);
  }
  for (auto* op : e->operands) collectWriteLocations(op, out);
}

void collectWriteLocations(StmtIR* stmt, LocationSet& out) {
  if (!stmt) return;

  // The slots this statement owns. Scanning them (rather than enumerating
  // statement kinds) is what keeps a write inside a declaration initializer --
  // `double z = ++y;` -- visible.
  forEachExpr(stmt, [&](DAGNode*& e) { collectWriteLocations(e, out); });

  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) collectWriteLocations(s.get(), out);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      collectWriteLocations(f->init.get(), out);
      collectWriteLocations(f->body.get(), out);
      // A `for` update the builder normalized out of the expression tree
      // (`i = i + 1`) is not visible as an expression at all.
      if (f->updateOp != OpCode::None && f->update &&
          f->update->kind == NodeKind::Variable)
        out.addRoot(f->update->name);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      collectWriteLocations(ie->thenBranch.get(), out);
      collectWriteLocations(ie->elseBranch.get(), out);
      break;
    }
    case StmtIRKind::Assign: {
      auto* a = static_cast<AssignIR*>(stmt);
      // The lvalue of a structured store is written, not evaluated; only its
      // root is recorded (some other pointer may alias the same object).
      if (a->targetExpr) {
        out.add(locationOf(a->targetExpr));
      } else if (!a->target.empty()) {
        out.addRoot(a->target);
      }
      break;
    }
    default:  // partial: only statement kinds that can write are listed
      break;
  }
}

// ===== AliasInfo =====

AliasInfo AliasInfo::build(const StmtIR* body) {
  AliasInfo info;
  info.scan(body);
  return info;
}

void AliasInfo::recordRefDecl(const VarDeclIR* decl) {
  if (!decl || decl->name.empty()) return;
  if (classifyDeclaredType(decl->type) != AliasClass::Reference) return;
  std::string root = locationRoot(decl->init);
  if (root.empty() || root == decl->name)
    _unknownReferent.insert(decl->name);
  else
    _referentOf[decl->name] = root;
}

void AliasInfo::scan(const StmtIR* stmt) {
  if (!stmt) return;
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* block = static_cast<const BlockIR*>(stmt);
      for (const auto& sub : block->stmts) scan(sub.get());
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<const ForLoopIR*>(stmt);
      scan(f->init.get());
      scan(f->body.get());
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<const IfElseIR*>(stmt);
      scan(ie->thenBranch.get());
      scan(ie->elseBranch.get());
      break;
    }
    case StmtIRKind::VarDecl:
      // A reference declaration anywhere -- a `for`-init as well as a block --
      // introduces an alias edge. Only the Block case used to be handled, so a
      // `for (double& r = x; ...)` was never recorded and a write through `r`
      // did not invalidate `x`.
      recordRefDecl(static_cast<const VarDeclIR*>(stmt));
      break;
    default:  // partial: only kinds that can introduce an alias edge are listed
      break;
  }
}

void AliasInfo::closeWrites(LocationSet& written) const {
  if (written.anything()) return;

  std::unordered_set<std::string> seen;
  std::vector<std::string> work;
  for (const auto& l : written.items())
    if (!l.root.empty() && seen.insert(l.root).second) work.push_back(l.root);

  for (size_t i = 0; i < work.size(); ++i) {
    const std::string& r = work[i];
    auto it = _referentOf.find(r);
    if (it != _referentOf.end()) {
      const std::string& target = it->second;
      if (seen.insert(target).second) work.push_back(target);
      written.addRoot(target);
    } else if (_unknownReferent.count(r)) {
      written.addUnknown();
      return;
    }
  }
}

LocationSet AliasInfo::writtenBy(StmtIR* stmt) const {
  LocationSet written;
  collectWriteLocations(stmt, written);
  closeWrites(written);
  return written;
}

bool AliasInfo::isAliasedReferent(const std::string& name) const {
  if (name.empty()) return false;
  for (const auto& [ref, referent] : _referentOf)
    if (referent == name) return true;
  return false;
}

bool AliasInfo::writeAffectsRead(const LocationSet& written,
                                 const std::string& name) const {
  if (name.empty()) return false;
  // Close the read over reference edges: reading `s3` observes `b` when
  // `s3 -> r2 -> b`. `written` is already referent-closed (writtenBy), so the
  // two sides now name storage the same way and `aliasesName` can match.
  std::unordered_set<std::string> seen{name};
  std::vector<std::string> work{name};
  for (size_t i = 0; i < work.size(); ++i) {
    const std::string& r = work[i];
    // A read through a reference whose referent is not a name observes
    // arbitrary storage, so any write may affect it.
    if (_unknownReferent.count(r) && !written.empty()) return true;
    if (written.aliasesName(r)) return true;
    auto it = _referentOf.find(r);
    if (it != _referentOf.end() && seen.insert(it->second).second)
      work.push_back(it->second);
  }
  return false;
}

}  // namespace cse
