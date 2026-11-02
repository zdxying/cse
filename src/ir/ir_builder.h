#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../frontend/ast.h"
#include "../frontend/cse_config.h"
#include "ir_module.h"

namespace cse {

// AST → IR transformation.
// Converts frontend AST (Expr/Stmt) into DAG-based IR (DAGNode/StmtIR).
// This is the bridge between frontend and IR — the only file that depends on both.
//
// The builder is effect-aware: a pre-scan of the function AST determines which
// locations are read-only and which calls are pure, so that loads and calls are
// only deduplicated when it is semantically safe. It also resolves lexical
// scopes and alpha-renames shadowed variables.
class IRBuilder {
 public:
  explicit IRBuilder(IRModule* module, const CSEConfig& config = CSEConfig());

  // Build IR from a function AST
  void buildFunction(const FunctionDef& func);

  // Build a statement
  std::unique_ptr<StmtIR> buildStmt(const Stmt& stmt);

  // Build an expression (returns DAG node)
  DAGNode* buildExpr(const Expr& expr);

 private:
  // A value is either a scalar or a fixed-size vector of scalar components
  // (used to lower project vector types, see CSEConfig::lowerVectors).
  struct VecValue {
    bool vec = false;
    DAGNode* scalar = nullptr;
    std::vector<DAGNode*> comps;
    DAGNode* base = nullptr;  // underlying indexable node (var / c(k) call)
  };
  VecValue buildValue(const Expr& expr);
  VecValue valueBinary(const Expr& expr);
  VecValue valueUnary(const Expr& expr);
  VecValue valueCall(const Expr& expr);
  VecValue valueArray(const Expr& expr);
  DAGNode* buildScalarCall(const Expr& expr);
  static VecValue makeScalar(DAGNode* n);
  static VecValue makeVector(std::vector<DAGNode*> comps);

  // Expression builders
  DAGNode* buildBinaryOp(const Expr& expr);
  DAGNode* buildUnaryOp(const Expr& expr);
  DAGNode* buildArrayAccess(const Expr& expr);
  DAGNode* buildCall(const Expr& expr);

  // ---- Effect / scope analysis ----
  void prescanStmt(const Stmt& stmt);
  void prescanExpr(const Expr& expr);
  void prescanFunction(const FunctionDef& func);

  // Is a call to `callee` referentially transparent? `isMemberCall` distinguishes
  // `obj.m(...)` from `m(...)`: the builtin math whitelist matches on the bare
  // name, so it may only be consulted for a *free* call -- otherwise a user
  // method named `sin`/`max` would be treated as pure.
  bool isPureCallee(const std::string& callee, bool isMemberCall) const;
  bool isReadOnlyRoot(const std::string& name) const;
  std::string rootName(const Expr& expr) const;

  // ---- Scope handling (alpha-renaming) ----
  void pushScope();
  void popScope();
  std::string declare(const std::string& name);
  std::string resolve(const std::string& name) const;
  bool isBound(const std::string& name) const;
  DAGNode* varRef(const std::string& name);

  // Lane count (0/1 = scalar) for a declared type, via
  // CSEConfig::vectorLanes. Used to tag variable nodes for the cost model.
  int typeLanes(const std::string& type) const;

  IRModule* _module;
  CSEConfig _config;

  // Pre-scan results
  std::unordered_set<std::string> _declared;
  std::unordered_set<std::string> _pointerParams;
  std::unordered_set<std::string> _written;
  std::unordered_set<std::string> _passedToCall;

  // Scope stack: original name → internal (unique) name
  std::vector<std::unordered_map<std::string, std::string>> _scopes;
  // Every source name declared so far in the current function (plus the internal
  // names handed out). Used to alpha-rename sibling-scope shadowing too, not
  // just names that are still visible in an enclosing scope. The names
  // themselves are made unique by IRModule::freshName.
  std::unordered_set<std::string> _seenNames;
  // Names this region references but never declares -- storage that lives
  // outside the region (a global, or an enclosing function's local), whether
  // the reference is a read (varRef) or a write (an assignment target). A later
  // declaration of one of them is alpha-renamed, because variables are interned
  // by name and the two would otherwise be the same node.
  std::unordered_set<std::string> _externallyReferenced;

  // Vector lowering state (only used when config.lowerVectors is set).
  int _vecDim = 0;
  std::unordered_set<std::string> _vectorVars;
  std::unordered_map<std::string, std::vector<DAGNode*>> _vecLocalComps;
};

}  // namespace cse
