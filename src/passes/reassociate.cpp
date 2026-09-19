#include "reassociate.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "../ir/dag_node.h"
#include "../ir/ir_module.h"
#include "../ir/statement.h"

namespace cse {

namespace {

using SignedTerm = std::pair<int, DAGNode*>;  // sign (+1/-1), term

bool isAdditive(DAGNode* n) {
  return n && n->kind == NodeKind::BinaryOp && (n->op == '+' || n->op == '-');
}

// Flatten an additive expression into signed terms.
void flatten(DAGNode* n, int sign, std::vector<SignedTerm>& out) {
  if (n->kind == NodeKind::BinaryOp && n->op == '+') {
    flatten(n->operands[0], sign, out);
    flatten(n->operands[1], sign, out);
  } else if (n->kind == NodeKind::BinaryOp && n->op == '-') {
    flatten(n->operands[0], sign, out);
    flatten(n->operands[1], -sign, out);
  } else if (n->kind == NodeKind::UnaryOp && n->op == '-') {
    flatten(n->operands[0], -sign, out);
  } else {
    out.push_back({sign, n});
  }
}

// A signed term key for frequency counting.
using TermKey = std::pair<int, uint32_t>;  // sign, node id

void collectFreq(DAGNode* n, bool parentAdditive,
                 std::set<TermKey>& seen) {
  if (!n) return;
  bool add = isAdditive(n);
  if (add && !parentAdditive) {
    std::vector<SignedTerm> terms;
    flatten(n, +1, terms);
    for (auto& t : terms) seen.insert({t.first, t.second->id});
  }
  for (auto* op : n->operands) collectFreq(op, add, seen);
}

void collectFreqStmt(StmtIR* stmt, std::map<TermKey, int>& freq) {
  if (!stmt) return;
  auto visitExpr = [&](DAGNode* e) {
    if (!e) return;
    std::set<TermKey> seen;
    collectFreq(e, false, seen);
    for (auto& k : seen) freq[k]++;
  };
  switch (stmt->kind) {
    case StmtIRKind::Block: {
      auto* b = static_cast<BlockIR*>(stmt);
      for (auto& s : b->stmts) collectFreqStmt(s.get(), freq);
      break;
    }
    case StmtIRKind::ForLoop: {
      auto* f = static_cast<ForLoopIR*>(stmt);
      collectFreqStmt(f->init.get(), freq);
      visitExpr(f->cond);
      visitExpr(f->update);
      visitExpr(f->updateRhs);
      collectFreqStmt(f->body.get(), freq);
      break;
    }
    case StmtIRKind::IfElse: {
      auto* ie = static_cast<IfElseIR*>(stmt);
      visitExpr(ie->cond);
      collectFreqStmt(ie->thenBranch.get(), freq);
      collectFreqStmt(ie->elseBranch.get(), freq);
      break;
    }
    case StmtIRKind::ExprStmt:
      visitExpr(static_cast<ExprStmtIR*>(stmt)->expr);
      break;
    case StmtIRKind::Assign:
      visitExpr(static_cast<AssignIR*>(stmt)->value);
      break;
    case StmtIRKind::VarDecl:
      visitExpr(static_cast<VarDeclIR*>(stmt)->init);
      break;
    case StmtIRKind::Return:
      visitExpr(static_cast<ReturnIR*>(stmt)->value);
      break;
  }
}

class ReassociateVisitor {
 public:
  ReassociateVisitor(IRModule& mod, const std::map<TermKey, int>& f)
      : module(mod), freq(f) {}

  IRModule& module;
  const std::map<TermKey, int>& freq;

  void visitStmt(StmtIR* stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
      case StmtIRKind::Block: {
        auto* b = static_cast<BlockIR*>(stmt);
        for (auto& s : b->stmts) visitStmt(s.get());
        break;
      }
      case StmtIRKind::ForLoop: {
        auto* f = static_cast<ForLoopIR*>(stmt);
        visitStmt(f->init.get());
        f->cond = rewrite(f->cond);
        f->update = rewrite(f->update);
        if (f->updateRhs) f->updateRhs = rewrite(f->updateRhs);
        visitStmt(f->body.get());
        break;
      }
      case StmtIRKind::IfElse: {
        auto* ie = static_cast<IfElseIR*>(stmt);
        ie->cond = rewrite(ie->cond);
        visitStmt(ie->thenBranch.get());
        visitStmt(ie->elseBranch.get());
        break;
      }
      case StmtIRKind::ExprStmt:
        if (auto* e = static_cast<ExprStmtIR*>(stmt)->expr)
          static_cast<ExprStmtIR*>(stmt)->expr = rewrite(e);
        break;
      case StmtIRKind::Assign:
        if (auto* v = static_cast<AssignIR*>(stmt)->value)
          static_cast<AssignIR*>(stmt)->value = rewrite(v);
        break;
      case StmtIRKind::VarDecl:
        if (auto* i = static_cast<VarDeclIR*>(stmt)->init)
          static_cast<VarDeclIR*>(stmt)->init = rewrite(i);
        break;
      case StmtIRKind::Return:
        if (auto* v = static_cast<ReturnIR*>(stmt)->value)
          static_cast<ReturnIR*>(stmt)->value = rewrite(v);
        break;
    }
  }

  DAGNode* rewrite(DAGNode* node) {
    if (!node) return node;

    if (isAdditive(node)) {
      std::vector<SignedTerm> terms;
      flatten(node, +1, terms);

      struct Item {
        int sign;
        DAGNode* rewritten;
        int count;
        uint32_t id;
      };
      std::vector<Item> items;
      items.reserve(terms.size());
      for (auto& [sign, term] : terms) {
        DAGNode* rw = rewrite(term);
        auto it = freq.find({sign, term->id});
        int cnt = (it == freq.end()) ? 0 : it->second;
        items.push_back({sign, rw, cnt, term->id});
      }

      std::stable_sort(items.begin(), items.end(),
                       [](const Item& a, const Item& b) {
                         if (a.count != b.count) return a.count > b.count;
                         return a.id < b.id;
                       });

      DAGNode* acc = nullptr;
      for (auto& it : items) {
        DAGNode* term = it.rewritten;
        if (!acc) {
          acc = (it.sign > 0) ? term : module.createUnaryOp('-', term);
        } else {
          acc = module.createBinaryOp(it.sign > 0 ? '+' : '-', acc, term);
        }
      }
      return acc ? acc : node;
    }

    // Non-additive: recurse into operands and rebuild.
    bool changed = false;
    std::vector<DAGNode*> newOps;
    for (auto* op : node->operands) {
      DAGNode* r = rewrite(op);
      newOps.push_back(r);
      if (r != op) changed = true;
    }
    if (!changed) return node;

    switch (node->kind) {
      case NodeKind::BinaryOp:
        if (newOps.size() == 2)
          return module.createBinaryOp(node->op, newOps[0], newOps[1]);
        break;
      case NodeKind::UnaryOp:
        if (newOps.size() == 1) return module.createUnaryOp(node->op, newOps[0]);
        break;
      case NodeKind::ArrayAccess:
        if (newOps.size() == 2) return module.createArrayAccess(newOps[0], newOps[1]);
        break;
      case NodeKind::MemberAccess:
        if (newOps.size() == 1) return module.createMemberAccess(newOps[0], node->name);
        break;
      case NodeKind::ArrowAccess:
        if (newOps.size() == 1) return module.createArrowAccess(newOps[0], node->name);
        break;
      case NodeKind::Call: {
        std::vector<DAGNode*> args(newOps.begin() + 1, newOps.end());
        return module.createCall(newOps[0], args);
      }
      default:
        break;
    }
    return node;
  }
};

}  // namespace

void ReassociatePass::run(IRModule& module) {
  std::map<TermKey, int> freq;
  collectFreqStmt(module.body.get(), freq);
  ReassociateVisitor visitor(module, freq);
  visitor.visitStmt(module.body.get());
}

}  // namespace cse
