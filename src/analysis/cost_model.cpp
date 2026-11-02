#include "cost_model.h"

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../frontend/cse_config.h"
#include "../ir/dag_node.h"
#include "../ir/ir_module.h"
#include "../ir/statement.h"

namespace cse {

namespace {

// Safety cap on loop-nest enumeration: nests larger than this fall back to a
// product-of-trips estimate (documented approximation).
constexpr long long kMaxEnum = 1000000;

bool isFlop(OpCode op) {
  return op == OpCode::Add || op == OpCode::Sub || op == OpCode::Mul ||
         op == OpCode::Div;
}

std::string calleeName(DAGNode* call) {
  if (!call || call->operands.empty()) return "";
  DAGNode* callee = call->operands[0];
  if (callee->kind == NodeKind::Variable ||
      callee->kind == NodeKind::MemberAccess ||
      callee->kind == NodeKind::ArrowAccess) {
    return callee->name;
  }
  return "";
}

// ---- constant evaluation over loop-var bindings -------------------------

std::optional<long long> evalConst(DAGNode* n,
                                   const std::unordered_map<std::string,
                                                            long long>& env) {
  if (!n) return std::nullopt;
  switch (n->kind) {
    case NodeKind::Constant:
      return static_cast<long long>(n->constVal);
    case NodeKind::Variable: {
      auto it = env.find(n->name);
      if (it == env.end()) return std::nullopt;
      return it->second;
    }
    case NodeKind::UnaryOp: {
      if (n->operands.size() != 1) return std::nullopt;
      auto v = evalConst(n->operands[0], env);
      if (!v) return std::nullopt;
      if (n->op == OpCode::Neg) return -*v;
      if (n->op == OpCode::Pos) return *v;
      return std::nullopt;
    }
    case NodeKind::BinaryOp: {
      if (n->operands.size() != 2) return std::nullopt;
      auto a = evalConst(n->operands[0], env);
      auto b = evalConst(n->operands[1], env);
      if (!a || !b) return std::nullopt;
      switch (n->op) {
        case OpCode::Add: return *a + *b;
        case OpCode::Sub: return *a - *b;
        case OpCode::Mul: return *a * *b;
        case OpCode::Div:
          return *b == 0 ? std::nullopt : std::optional<long long>(*a / *b);
        default: return std::nullopt;
      }
    }
    default:
      return std::nullopt;
  }
}

void collectFreeVars(DAGNode* n, std::set<std::string>& out) {
  if (!n) return;
  if (n->kind == NodeKind::Variable) out.insert(n->name);
  for (auto* op : n->operands) collectFreeVars(op, out);
}

// Number of iterations of `for (v = lo; v < hi / <= hi; v += step)`, where
// `step` may be negative (descending). Returns 0 for an empty range or step 0.
long long tripCount(long long lo, long long hi, long long step, bool inclusive) {
  if (step == 0) return 0;
  if (step > 0) {
    if (inclusive) return hi < lo ? 0 : (hi - lo) / step + 1;
    return hi <= lo ? 0 : (hi - lo + step - 1) / step;
  }
  long long s = -step;
  if (inclusive) return hi > lo ? 0 : (lo - hi) / s + 1;
  return hi >= lo ? 0 : (lo - hi + s - 1) / s;
}

// ---- lane inference ------------------------------------------------------

class LaneAnalyzer {
 public:
  explicit LaneAnalyzer(const CSEConfig* cfg) : _cfg(cfg) {}

  // Lanes for compiler-introduced temporaries (whose Variable node carries no
  // declared type) are supplied by name->lanes inference.
  void setVarLanes(const std::unordered_map<std::string, int>* m) {
    _varLanes = m;
  }
  void resetMemo() { _memo.clear(); }

  int lanes(DAGNode* n) {
    if (!n) return 0;
    auto it = _memo.find(n);
    if (it != _memo.end()) return it->second;
    _memo[n] = 0;  // cycle guard
    int r = compute(n);
    _memo[n] = r;
    return r;
  }

 private:
  int compute(DAGNode* n) {
    switch (n->kind) {
      case NodeKind::Variable: {
        if (n->vecDim > 0) return n->vecDim;
        if (_varLanes) {
          auto it = _varLanes->find(n->name);
          if (it != _varLanes->end()) return it->second;
        }
        return 0;
      }
      case NodeKind::Call: {
        std::string callee = calleeName(n);
        if (_cfg && _cfg->callResultLanes) return _cfg->callResultLanes(callee);
        if (_cfg && _cfg->isVectorProducingCall &&
            _cfg->isVectorProducingCall(callee))
          return _cfg->vectorDim;
        return 0;
      }
      case NodeKind::BinaryOp: {
        if (n->operands.size() != 2) return 0;
        long long ignored = 0;
        int a = lanes(n->operands[0]);
        int b = lanes(n->operands[1]);
        return binLanes(n->op, a, b, ignored);
      }
      case NodeKind::UnaryOp:
        return n->operands.empty() ? 0 : lanes(n->operands[0]);
      case NodeKind::Cast:
        return n->operands.empty() ? 0 : lanes(n->operands[0]);
      case NodeKind::Ternary: {
        int a = n->operands.size() > 1 ? lanes(n->operands[1]) : 0;
        int b = n->operands.size() > 2 ? lanes(n->operands[2]) : 0;
        return std::max(a, b);
      }
      default:
        return 0;
    }
  }

  int binLanes(OpCode op, int a, int b, long long& flops) {
    if (_cfg && _cfg->binaryOpCost) return _cfg->binaryOpCost(op, a, b, flops);
    flops = 1;
    return 0;
  }

  const CSEConfig* _cfg;
  const std::unordered_map<std::string, int>* _varLanes = nullptr;
  std::unordered_map<DAGNode*, int> _memo;
};

// ---- loop descriptors ----------------------------------------------------

struct LoopDesc {
  std::string var;
  DAGNode* start = nullptr;  // nullptr => 0
  DAGNode* bound = nullptr;
  long long step = 1;
  bool inclusive = false;
};

// Recognize `for (i = S; i </<=/>/>= B; ++i / i += k / --i / i -= k)` in either
// direction. The update's sign must agree with the comparison direction.
bool recognizeLoop(ForLoopIR* f, LoopDesc& d) {
  if (!f || !f->init || f->init->kind != StmtIRKind::VarDecl) return false;
  auto* decl = static_cast<VarDeclIR*>(f->init.get());
  if (decl->name.empty()) return false;
  d.var = decl->name;
  d.start = decl->init;  // may be null (=> 0)

  if (!f->cond || f->cond->kind != NodeKind::BinaryOp ||
      f->cond->operands.size() != 2)
    return false;
  // The comparison decides the direction: `<`/`<=` count up, `>`/`>=` down.
  bool descending;
  switch (f->cond->op) {
    case OpCode::Lt: descending = false; d.inclusive = false; break;
    case OpCode::Le: descending = false; d.inclusive = true; break;
    case OpCode::Gt: descending = true; d.inclusive = false; break;
    case OpCode::Ge: descending = true; d.inclusive = true; break;
    default: return false;
  }
  DAGNode* lhs = f->cond->operands[0];
  DAGNode* rhs = f->cond->operands[1];
  if (!(lhs->kind == NodeKind::Variable && lhs->name == d.var)) return false;
  d.bound = rhs;

  // update: ++i / i++ / i += k / --i / i-- / i -= k. A prefix `++i` arrives as
  // an Inc unary node; a postfix `i++` is normalized to `update = i` with
  // updateOp Inc (no rhs); a compound `i += k` is `update = i` with updateOp
  // Add and updateRhs k.
  long long step = 0;
  if (f->update && f->update->kind == NodeKind::UnaryOp &&
      (f->update->name == "++" || f->update->name == "--") &&
      !f->update->operands.empty() &&
      f->update->operands[0]->kind == NodeKind::Variable &&
      f->update->operands[0]->name == d.var) {
    step = (f->update->name == "++") ? 1 : -1;
  } else if (f->update && f->update->kind == NodeKind::Variable &&
             f->update->name == d.var &&
             (f->updateOp == OpCode::Inc || f->updateOp == OpCode::Dec ||
              f->updateOp == OpCode::Add || f->updateOp == OpCode::Sub)) {
    long long k = 1;
    if (f->updateRhs) {
      if (f->updateRhs->kind != NodeKind::Constant) return false;
      k = static_cast<long long>(f->updateRhs->constVal);
    }
    bool down = f->updateOp == OpCode::Dec || f->updateOp == OpCode::Sub;
    step = down ? -k : k;
  } else {
    return false;
  }
  if (step == 0) return false;
  if (descending != (step < 0)) return false;  // direction must match the sign
  d.step = step;
  return true;
}

// ---- cost traversal ------------------------------------------------------

class CostAnalyzer {
 public:
  explicit CostAnalyzer(const CSEConfig* cfg)
      : _cfg(cfg), _lanes(cfg) {
    _lanes.setVarLanes(&_varLanes);
  }

  CostResult run(IRModule& module) {
    if (module.body) {
      inferVarLanes(module.body.get());
      _lanes.resetMemo();
      stmt(module.body.get());
      charge();
    }
    return _result;
  }

 private:
  // Infer the lane count of declared variables (including compiler-created
  // temporaries) from their initializers, in program order. CSE hoists
  // definitions before uses, so a single forward pass suffices.
  void inferVarLanes(StmtIR* s) {
    if (!s) return;
    switch (s->kind) {
      case StmtIRKind::Block: {
        auto* b = static_cast<BlockIR*>(s);
        for (auto& sub : b->stmts) inferVarLanes(sub.get());
        break;
      }
      case StmtIRKind::VarDecl: {
        auto* d = static_cast<VarDeclIR*>(s);
        if (!d->name.empty() && d->init) {
          int L = _lanes.lanes(d->init);
          if (L > 0) _varLanes[d->name] = L;
        }
        break;
      }
      case StmtIRKind::Assign: {
        auto* a = static_cast<AssignIR*>(s);
        if (!a->target.empty() && a->value) {
          int L = _lanes.lanes(a->value);
          if (L > 0) _varLanes[a->target] = L;
        }
        break;
      }
      case StmtIRKind::ForLoop: {
        auto* f = static_cast<ForLoopIR*>(s);
        inferVarLanes(f->init.get());
        inferVarLanes(f->body.get());
        break;
      }
      case StmtIRKind::IfElse: {
        auto* ie = static_cast<IfElseIR*>(s);
        inferVarLanes(ie->thenBranch.get());
        inferVarLanes(ie->elseBranch.get());
        break;
      }
      // These kinds introduce no declaration whose lane count could be
      // inferred. Listed so a new StmtIRKind is a compile error here.
      case StmtIRKind::ExprStmt:
      case StmtIRKind::Return:
        break;
    }
  }

  long long factor() { return execCount(); }

  // Count an unresolved loop extent once per loop statement. `execCount()` runs
  // per statement slot (condition, update, body, ...), and a large nest reports
  // overflow from every one of them; the field counts *distinct constructs*
  // (cost_model.h), so the innermost loop currently on the stack is remembered.
  void markUnknownLoop() {
    // The depth identifies "we are in the same loop nest"; a later, sibling
    // loop at the same depth must still be counted, so the key also carries the
    // loop's variable name (unique per statement — even when two loops reuse a
    // spelling, the unroller has already renamed them apart).
    std::string key =
        std::to_string(_stack.size()) + ":" +
        (_stack.empty() ? std::string() : _stack.back().var);
    if (_reportedUnknown.insert(key).second) _result.unknownLoops++;
  }

  // Number of executions of the current loop context.
  //
  // Returns a *lower bound* when the nest is too large to enumerate: the model
  // documents `flops` as a lower bound whenever `unknownLoops` is non-zero
  // (cost_model.h), and a truncated product is exactly that. What it must not do
  // is return the truncated product *without* flagging it -- `for (i < 2e6)` was
  // reported as 2 flops with `unknownLoops = 0`, a number that is neither the
  // real cost nor labelled as an approximation.
  long long execCount() {
    if (_stack.empty()) return 1;

    // Max-product estimate, binding each loop var to its start value.
    long long est = 1;
    bool overflow = false;
    {
      std::unordered_map<std::string, long long> probe;
      for (const auto& d : _stack) {
        auto s = evalConst(d.start, probe);
        auto e = evalConst(d.bound, probe);
        if (!s) s = 0;
        if (!e) return 1;  // unresolvable (filtered by loopResolvable)
        long long n = tripCount(*s, *e, d.step, d.inclusive);
        if (n <= 0) return 0;
        if (est > kMaxEnum / std::max(1LL, n)) {
          overflow = true;
          break;
        }
        est *= n;
        probe[d.var] = *s;
      }
    }

    // Detect whether any loop extent depends on an enclosing loop variable.
    bool dependent = false;
    {
      std::set<std::string> outer;
      for (const auto& d : _stack) {
        if (referencesOuter(d.start, outer) ||
            referencesOuter(d.bound, outer)) {
          dependent = true;
          break;
        }
        outer.insert(d.var);
      }
    }
    if (!dependent) {
      if (overflow) markUnknownLoop();
      return est;
    }
    if (overflow) {
      // A dependent nest too large to enumerate. `est` is a partial product, so
      // it is a lower bound; report it as an unresolved loop so the CLI's
      // "flops is a lower bound" note fires.
      markUnknownLoop();
      return est;
    }

    // Small dependent (e.g. triangular) nest: enumerate exactly.
    std::unordered_map<std::string, long long> env;
    return execCountEnum(0, env);
  }

  long long execCountEnum(size_t i,
                          std::unordered_map<std::string, long long>& env) {
    if (i == _stack.size()) return 1;
    const LoopDesc& d = _stack[i];
    auto s = evalConst(d.start, env);
    auto e = evalConst(d.bound, env);
    if (!s) s = 0;
    if (!e) return 1;
    long long total = 0;
    for (long long v = *s; d.step > 0 ? (d.inclusive ? v <= *e : v < *e)
                                      : (d.inclusive ? v >= *e : v > *e);
         v += d.step) {
      env[d.var] = v;
      total += execCountEnum(i + 1, env);
    }
    return total;
  }

  static bool referencesOuter(DAGNode* e, const std::set<std::string>& vars) {
    std::set<std::string> free;
    collectFreeVars(e, free);
    for (const auto& v : free)
      if (vars.count(v)) return true;
    return false;
  }

  // ---- per-reference cost ------------------------------------------------
  //
  // Each *reference* to a node is charged, not each distinct node: codegen
  // re-prints a shared (unmaterialized) node at every use site, while a
  // CSE-materialized temporary reaches the IR as a single definition (its own
  // VarDecl) whose uses are cheap Variable references -- so counting by
  // reference occurrence is exact in both cases. `totalNodes` still reports
  // the distinct-node count.
  //
  // A statement's expression slot is one reference per execution of the
  // statement's loop context; a node's operand slot is one reference per
  // reference to the node. Multiplicity therefore propagates multiplicatively
  // down the DAG. The sub-DAG is walked once in topological order so a shared
  // subgraph is expanded arithmetically rather than re-traversed.

  // A statement's expression slot: `n` is referenced once per execution of the
  // statement's loop context.
  void addRoot(DAGNode* n, long long m) {
    if (!n) return;
    _rootContrib[n] += m;
  }

  // The operand slots `expr` would descend into. The call callee (operands[0])
  // is skipped, matching the old traversal.
  static void collectChildren(DAGNode* n, std::vector<DAGNode*>& out) {
    switch (n->kind) {
      case NodeKind::BinaryOp:
      case NodeKind::UnaryOp:
      case NodeKind::ArrayAccess:
      case NodeKind::MemberAccess:
      case NodeKind::ArrowAccess:
      case NodeKind::Ternary:
      case NodeKind::Cast:
        for (auto* op : n->operands)
          if (op) out.push_back(op);
        break;
      case NodeKind::Call:
        for (size_t i = 1; i < n->operands.size(); ++i)
          if (n->operands[i]) out.push_back(n->operands[i]);
        break;
      default:
        break;
    }
  }

  void charge() {
    // Enumerate the reachable sub-DAG and its operand slots (edges).
    std::unordered_map<DAGNode*, std::vector<DAGNode*>> kids;
    std::unordered_map<DAGNode*, long long> indeg;
    std::vector<DAGNode*> stack;
    for (auto& kv : _rootContrib) stack.push_back(kv.first);
    while (!stack.empty()) {
      DAGNode* n = stack.back();
      stack.pop_back();
      if (kids.count(n)) continue;
      std::vector<DAGNode*> ch;
      collectChildren(n, ch);
      for (DAGNode* c : ch) {
        indeg[c]++;
        if (!kids.count(c)) stack.push_back(c);
      }
      kids[n] = std::move(ch);
    }

    // Parents-before-children order (Kahn): a node's multiplicity is final by
    // the time it is charged and propagated to its operand slots.
    std::unordered_map<DAGNode*, long long> mult;
    for (auto& kv : _rootContrib) mult[kv.first] = kv.second;
    std::vector<DAGNode*> queue;
    for (auto& kv : kids)
      if (indeg[kv.first] == 0) queue.push_back(kv.first);
    for (size_t head = 0; head < queue.size(); ++head) {
      DAGNode* n = queue[head];
      long long m = mult[n];
      for (DAGNode* c : kids[n]) {
        mult[c] += m;
        if (--indeg[c] == 0) queue.push_back(c);
      }
    }

    for (auto& kv : kids) chargeNode(kv.first, mult[kv.first]);
  }

  void chargeNode(DAGNode* n, long long m) {
    _result.totalNodes++;
    switch (n->kind) {
      case NodeKind::BinaryOp: {
        int a = _lanes.lanes(n->operands.size() > 0 ? n->operands[0] : nullptr);
        int b = _lanes.lanes(n->operands.size() > 1 ? n->operands[1] : nullptr);
        if (isFlop(n->op)) {
          long long flops = 1;
          if (_cfg && _cfg->binaryOpCost) {
            _cfg->binaryOpCost(n->op, a, b, flops);
          }
          _result.flops += flops * m;
          if (a > 0 || b > 0) _result.vectorOps += m;
        }
        break;
      }
      case NodeKind::UnaryOp: {
        // A unary `+`/`-` and an increment/decrement each cost one operation:
        // `++x` is one addition of 1, `--x` one subtraction. (`!`/`~` are not
        // floating-point work and are not charged.)
        if (n->op == OpCode::Pos || n->op == OpCode::Neg ||
            isIncDecOp(n->op)) {
          int a = _lanes.lanes(n->operands.empty() ? nullptr : n->operands[0]);
          _result.flops += (a > 0 ? a : 1) * m;
          if (a > 0) _result.vectorOps += m;
        }
        break;
      }
      case NodeKind::ArrayAccess:
      case NodeKind::MemberAccess:
      case NodeKind::ArrowAccess:
        _result.memOps += m;
        break;
      case NodeKind::Call: {
        std::vector<int> argLanes;
        for (size_t i = 1; i < n->operands.size(); ++i)
          argLanes.push_back(_lanes.lanes(n->operands[i]));
        if (_cfg && _cfg->callCost) {
          long long c = _cfg->callCost(calleeName(n), argLanes);
          if (c < 0)
            _result.unmodeledCalls++;  // distinct unmodeled call nodes
          else
            _result.flops += c * m;
        }
        break;
      }
      default:
        break;
    }
  }

  void storeTarget(DAGNode* target) {
    if (!target) return;
    _result.memOps += factor();
    // Count address/index computations but not the store access itself.
    for (auto* op : target->operands) addRoot(op, factor());
  }

  void stmt(StmtIR* s) {
    if (!s) return;
    long long f = factor();
    _result.stmts += f;

    switch (s->kind) {
      case StmtIRKind::Block: {
        auto* b = static_cast<BlockIR*>(s);
        for (auto& sub : b->stmts) stmt(sub.get());
        break;
      }
      case StmtIRKind::VarDecl: {
        auto* d = static_cast<VarDeclIR*>(s);
        _result.vars += f;
        addRoot(d->init, f);
        break;
      }
      case StmtIRKind::Assign: {
        auto* a = static_cast<AssignIR*>(s);
        if (a->targetExpr) storeTarget(a->targetExpr);
        addRoot(a->value, f);
        break;
      }
      case StmtIRKind::ExprStmt:
        addRoot(static_cast<ExprStmtIR*>(s)->expr, f);
        break;
      case StmtIRKind::Return:
        addRoot(static_cast<ReturnIR*>(s)->value, f);
        break;
      case StmtIRKind::IfElse: {
        auto* ie = static_cast<IfElseIR*>(s);
        addRoot(ie->cond, f);
        stmt(ie->thenBranch.get());
        stmt(ie->elseBranch.get());
        break;
      }
      case StmtIRKind::ForLoop: {
        auto* fo = static_cast<ForLoopIR*>(s);
        stmt(fo->init.get());
        LoopDesc d;
        if (recognizeLoop(fo, d) && loopResolvable(d)) {
          _stack.push_back(d);
          // Count the unresolved-extent report once per loop, not once per
          // `execCount()` call: the model's fields are documented as counting
          // *distinct constructs*, and a loop's body, condition and update each
          // re-evaluate the trip count.
          const long long before = _result.unknownLoops;
          long long lf = factor();
          if (_result.unknownLoops != before) _result.unknownLoops = before + 1;
          addRoot(fo->cond, lf);
          addRoot(fo->update, lf);
          addRoot(fo->updateRhs, lf);
          stmt(fo->body.get());
          _stack.pop_back();
        } else {
          _result.unknownLoops++;
          addRoot(fo->cond, f);
          addRoot(fo->update, f);
          addRoot(fo->updateRhs, f);
          stmt(fo->body.get());
        }
        break;
      }
    }
  }

  // A recognized loop is usable only if its start/bound reference nothing
  // outside the enclosing loop variables.
  bool loopResolvable(const LoopDesc& d) {
    std::set<std::string> free;
    collectFreeVars(d.start, free);
    collectFreeVars(d.bound, free);
    for (const auto& sd : _stack)
      free.erase(sd.var);
    return free.empty();
  }

  const CSEConfig* _cfg;
  std::unordered_map<std::string, int> _varLanes;
  LaneAnalyzer _lanes;
  // Sum of loop-scaled reference counts of every statement slot pointing at a
  // node; `charge()` propagates these down the DAG.
  std::unordered_map<DAGNode*, long long> _rootContrib;
  std::vector<LoopDesc> _stack;
  // Loop nests already reported as having an unresolved extent (see
  // markUnknownLoop), so one big loop is counted once.
  std::unordered_set<std::string> _reportedUnknown;
  CostResult _result;
};

}  // namespace

CostResult analyzeCost(IRModule& module, const CSEConfig* config) {
  CostAnalyzer analyzer(config);
  return analyzer.run(module);
}

}  // namespace cse
