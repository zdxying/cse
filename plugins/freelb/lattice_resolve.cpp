#include "lattice_resolve.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "frontend/cse_config.h"
#include "ir/dag_node.h"
#include "ir/ir_module.h"
#include "ir/statement.h"

namespace cse {
namespace freelb {

namespace {

// Hardcoded lattice tables (mirrors tests/lattice_set.h).
struct LatticeInfo {
  int dim;
  int q;
  const int* c;       // q * dim, row-major
  const double* w;    // q
};

// Direction vectors (q * dim, row-major) and weights, mirrored from
// src/lbm/lattice_set.h. Fixed 1/3 sound speed for every set.
const int kD2Q5c[] = {0, 0, 1, 0, -1, 0, 0, 1, 0, -1};
const double kD2Q5w[] = {1.0 / 3.0, 1.0 / 6.0, 1.0 / 6.0, 1.0 / 6.0, 1.0 / 6.0};

const int kD2Q9c[] = {0, 0, 1, 0, -1, 0, 0, 1, 0, -1,
                      1, 1, -1, -1, 1, -1, -1, 1};
const double kD2Q9w[] = {4.0 / 9.0,  1.0 / 9.0,  1.0 / 9.0,  1.0 / 9.0,
                         1.0 / 9.0,  1.0 / 36.0, 1.0 / 36.0, 1.0 / 36.0,
                         1.0 / 36.0};

const int kD3Q7c[] = {0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 1, 0,
                      0, -1, 0, 0, 0, 1, 0, 0, -1};
const double kD3Q7w[] = {1.0 / 4.0, 1.0 / 8.0, 1.0 / 8.0, 1.0 / 8.0,
                         1.0 / 8.0, 1.0 / 8.0, 1.0 / 8.0};

const int kD3Q15c[] = {
    0, 0, 0,   1, 0, 0,   -1, 0, 0,   0, 1, 0,    0, -1, 0,   0, 0, 1,
    0, 0, -1,  1, 1, 1,   -1, -1, -1, 1, 1, -1,   -1, -1, 1,  1, -1, 1,
    -1, 1, -1, -1, 1, 1,  1, -1, -1};
const double kD3Q15w[] = {
    2.0 / 9.0,  1.0 / 9.0,  1.0 / 9.0,  1.0 / 9.0,  1.0 / 9.0,
    1.0 / 9.0,  1.0 / 9.0,  1.0 / 72.0, 1.0 / 72.0, 1.0 / 72.0,
    1.0 / 72.0, 1.0 / 72.0, 1.0 / 72.0, 1.0 / 72.0, 1.0 / 72.0};

const int kD3Q19c[] = {
    0, 0, 0,   1, 0, 0,   -1, 0, 0,  0, 1, 0,   0, -1, 0,  0, 0, 1,  0, 0, -1,
    1, 1, 0,   -1, -1, 0, 1, 0, 1,   -1, 0, -1, 0, 1, 1,  0, -1, -1, 1, -1, 0,
    -1, 1, 0,  1, 0, -1,  -1, 0, 1, 0, 1, -1,  0, -1, 1};

const double kD3Q19w[] = {
    1.0 / 3.0,  1.0 / 18.0, 1.0 / 18.0, 1.0 / 18.0, 1.0 / 18.0, 1.0 / 18.0,
    1.0 / 18.0, 1.0 / 36.0, 1.0 / 36.0, 1.0 / 36.0, 1.0 / 36.0, 1.0 / 36.0,
    1.0 / 36.0, 1.0 / 36.0, 1.0 / 36.0, 1.0 / 36.0, 1.0 / 36.0, 1.0 / 36.0,
    1.0 / 36.0};

const int kD3Q27c[] = {
    0, 0, 0,   1, 0, 0,   -1, 0, 0,   0, 1, 0,    0, -1, 0,   0, 0, 1,
    0, 0, -1,  1, 1, 0,   -1, -1, 0,  1, 0, 1,    -1, 0, -1,  0, 1, 1,
    0, -1, -1, 1, -1, 0,  -1, 1, 0,   1, 0, -1,   -1, 0, 1,   0, 1, -1,
    0, -1, 1,  1, 1, 1,   -1, -1, -1, 1, 1, -1,   -1, -1, 1,  1, -1, 1,
    -1, 1, -1, -1, 1, 1,  1, -1, -1};
const double kD3Q27w[] = {
    8.0 / 27.0,  2.0 / 27.0,  2.0 / 27.0,  2.0 / 27.0,  2.0 / 27.0,
    2.0 / 27.0,  2.0 / 27.0,  1.0 / 54.0,  1.0 / 54.0,  1.0 / 54.0,
    1.0 / 54.0,  1.0 / 54.0,  1.0 / 54.0,  1.0 / 54.0,  1.0 / 54.0,
    1.0 / 54.0,  1.0 / 54.0,  1.0 / 54.0,  1.0 / 54.0,  1.0 / 216.0,
    1.0 / 216.0, 1.0 / 216.0, 1.0 / 216.0, 1.0 / 216.0, 1.0 / 216.0,
    1.0 / 216.0, 1.0 / 216.0};

const LatticeInfo* lookupLattice(const std::string& name) {
  static const LatticeInfo sets[] = {
      {2, 5, kD2Q5c, kD2Q5w},   {2, 9, kD2Q9c, kD2Q9w},
      {3, 7, kD3Q7c, kD3Q7w},   {3, 15, kD3Q15c, kD3Q15w},
      {3, 19, kD3Q19c, kD3Q19w}, {3, 27, kD3Q27c, kD3Q27w}};
  for (const auto& s : sets) {
    std::string tag = "D" + std::to_string(s.dim) + "Q" + std::to_string(s.q);
    if (name.find(tag) != std::string::npos) return &s;
  }
  return nullptr;
}

bool calleeIs(DAGNode* node, const char* prefix) {
  if (!node || node->kind != NodeKind::Call || node->operands.empty()) return false;
  DAGNode* callee = node->operands[0];
  return callee->kind == NodeKind::Variable &&
         callee->name.find(prefix) != std::string::npos;
}

bool isLatticeCall(DAGNode* node, const char* which) {
  if (!calleeIs(node, "latset::")) return false;
  DAGNode* callee = node->operands[0];
  return callee->name.find(which) != std::string::npos;
}

class LatticeResolveVisitor {
 public:
  LatticeResolveVisitor(IRModule& mod, const CSEConfig& cfg)
      : module(mod), config(cfg) {}
  IRModule& module;
  const CSEConfig& config;
  int resolved = 0;

  // Resolve `<alias>` template arguments to the configured concrete lattice.
  const LatticeInfo* latticeFor(const std::string& callee) {
    if (!config.latsetAlias.empty() &&
        callee.find(config.latsetAlias) != std::string::npos) {
      return lookupLattice(config.latsetName);
    }
    return lookupLattice(callee);
  }

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

  // Index of the lattice call, as a compile-time integer, or -1.
  int constIndex(DAGNode* call) {
    if (!call || call->operands.size() < 2) return -1;
    DAGNode* idx = call->operands[1];
    if (idx->kind != NodeKind::Constant) return -1;
    int v = static_cast<int>(idx->constVal);
    if (v < 0) return -1;
    return v;
  }

  DAGNode* rewrite(DAGNode* node) {
    if (!node) return node;

    // latset::c<...>(k)[i] -> constant component
    if (node->kind == NodeKind::ArrayAccess && node->operands.size() == 2 &&
        isLatticeCall(node->operands[0], "::c")) {
      DAGNode* call = node->operands[0];
      DAGNode* idx = node->operands[1];
      const LatticeInfo* lat = latticeFor(calleeName(call));
      int k = constIndex(call);
      if (lat && idx->kind == NodeKind::Constant && k >= 0 && k < lat->q) {
        int comp = static_cast<int>(idx->constVal);
        if (comp >= 0 && comp < lat->dim) {
          resolved++;
          double val = static_cast<double>(lat->c[k * lat->dim + comp]);
          return module.createConst(val, numText(val));
        }
      }
    }

    // u * latset::c<...>(k) -> scalar dot product
    if (node->kind == NodeKind::BinaryOp && node->op == '*' &&
        node->operands.size() == 2) {
      DAGNode* a = node->operands[0];
      DAGNode* b = node->operands[1];
      if (isLatticeCall(b, "::c") && a->kind == NodeKind::Variable) {
        if (DAGNode* dot = buildDot(a, b)) return dot;
      }
      if (isLatticeCall(a, "::c") && b->kind == NodeKind::Variable) {
        if (DAGNode* dot = buildDot(b, a)) return dot;
      }
    }

    // latset::w<...>(k) -> declared constant weight, emitted symbolically.
    if (isLatticeCall(node, "::w")) {
      const LatticeInfo* lat = latticeFor(calleeName(node));
      int k = constIndex(node);
      if (lat && k >= 0 && k < lat->q) {
        resolved++;
        // Canonical representative index for this weight value so that all
        // directions sharing a weight collapse to one node (weight grouping).
        int rep = k;
        for (int j = 0; j < k; ++j) {
          if (lat->w[j] == lat->w[k]) {
            rep = j;
            break;
          }
        }
        std::string callee = calleeName(node);
        callee.erase(std::remove(callee.begin(), callee.end(), ' '), callee.end());
        std::string sym = callee + "(" + std::to_string(rep) + ")";
        return module.createSymbolicConst(lat->w[k], sym);
      }
    }

    // Recurse into children, rebuilding as needed.
    bool changed = false;
    std::vector<DAGNode*> newOps;
    for (auto* op : node->operands) {
      DAGNode* r = rewrite(op);
      newOps.push_back(r);
      if (r != op) changed = true;
    }
    if (!changed) return node;
    return rebuild(node, newOps);
  }

 private:
  std::string calleeName(DAGNode* call) {
    if (!call || call->operands.empty()) return "";
    return call->operands[0]->name;
  }

  DAGNode* buildDot(DAGNode* vec, DAGNode* call) {
    const LatticeInfo* lat = latticeFor(calleeName(call));
    int k = constIndex(call);
    if (!lat || k < 0 || k >= lat->q) return nullptr;

    int firstNz = -1;
    for (int d = 0; d < lat->dim; ++d) {
      if (lat->c[k * lat->dim + d] != 0) {
        firstNz = d;
        break;
      }
    }
    if (firstNz < 0) {
      resolved++;
      return module.createConst(0, "0");  // rest direction
    }

    // Canonical sign: factor out a leading -1 so opposite directions become
    // exact negations (enables even-power sharing in AlgebraicSimplify).
    bool negate = lat->c[k * lat->dim + firstNz] < 0;

    DAGNode* sum = nullptr;
    for (int d = 0; d < lat->dim; ++d) {
      int coeff = lat->c[k * lat->dim + d];
      if (coeff == 0) continue;
      if (negate) coeff = -coeff;

      DAGNode* term = module.createArrayAccess(
          vec, module.createConst(d, std::to_string(d)), /*shareable=*/true);
      if (coeff == -1) term = module.createUnaryOp('-', term);

      sum = sum ? module.createBinaryOp('+', sum, term) : term;
    }
    resolved++;
    if (negate) return module.createUnaryOp('-', sum);
    return sum;
  }

  DAGNode* rebuild(DAGNode* node, const std::vector<DAGNode*>& ops) {
    switch (node->kind) {
      case NodeKind::BinaryOp:
        if (ops.size() == 2) return module.createBinaryOp(node->op, ops[0], ops[1]);
        break;
      case NodeKind::UnaryOp:
        if (ops.size() == 1) return module.createUnaryOp(node->op, ops[0]);
        break;
      case NodeKind::ArrayAccess:
        if (ops.size() == 2)
          return module.createArrayAccess(ops[0], ops[1], node->pure);
        break;
      case NodeKind::MemberAccess:
        if (ops.size() == 1)
          return module.createMemberAccess(ops[0], node->name, node->pure);
        break;
      case NodeKind::ArrowAccess:
        if (ops.size() == 1)
          return module.createArrowAccess(ops[0], node->name, node->pure);
        break;
      case NodeKind::Call: {
        std::vector<DAGNode*> args(ops.begin() + 1, ops.end());
        return module.createCall(ops[0], args, node->pure);
      }
      default:
        break;
    }
    return node;
  }

  static std::string numText(double v) {
    std::ostringstream oss;
    oss << std::setprecision(17) << v;
    return oss.str();
  }
};

class LatticeResolvePass : public Pass {
 public:
  explicit LatticeResolvePass(const CSEConfig& cfg) : config(cfg) {}
  std::string name() const override { return "LatticeResolve"; }
  void run(IRModule& module) override {
    LatticeResolveVisitor visitor(module, config);
    visitor.visitStmt(module.body.get());
  }

 private:
  CSEConfig config;
};

}  // namespace

std::unique_ptr<Pass> createLatticeResolvePass(const CSEConfig& config) {
  return std::make_unique<LatticeResolvePass>(config);
}

}  // namespace freelb
}  // namespace cse
