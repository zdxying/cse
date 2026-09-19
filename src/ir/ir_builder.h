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
  // (used to lower FreeLB `Vector<T, LatSet::d>` arithmetic).
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

  bool isPureCallee(const std::string& callee) const;
  bool isReadOnlyRoot(const std::string& name) const;
  std::string rootName(const Expr& expr) const;

  // Fold `<latsetAlias>::q/d/cs2/InvCs2/InvCs4` to a constant when a per-latset
  // instantiation context is configured. Returns nullptr otherwise.
  DAGNode* latsetConst(const std::string& name);

  // ---- Scope handling (alpha-renaming) ----
  void pushScope();
  void popScope();
  std::string declare(const std::string& name);
  std::string resolve(const std::string& name) const;
  DAGNode* varRef(const std::string& name);

  IRModule* _module;
  CSEConfig _config;

  // Pre-scan results
  std::unordered_set<std::string> _declared;
  std::unordered_set<std::string> _constParams;
  std::unordered_set<std::string> _pointerParams;
  std::unordered_set<std::string> _written;
  std::unordered_set<std::string> _passedToCall;

  // Scope stack: original name → internal (unique) name
  std::vector<std::unordered_map<std::string, std::string>> _scopes;
  std::unordered_map<std::string, int> _shadowCounters;

  // Vector lowering state (only used when config.lowerVectors is set).
  int _vecDim = 0;
  std::unordered_set<std::string> _vectorVars;
  std::unordered_map<std::string, std::vector<DAGNode*>> _vecLocalComps;
};

}  // namespace cse
