#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dag_node.h"
#include "statement.h"

namespace cse {

// Function signature
struct FuncParam {
  std::string type;
  std::string name;
};

struct FuncSignature {
  std::string returnType;
  std::string name;
  std::vector<FuncParam> params;
};

// IR Module — owns all DAG nodes and holds the function body.
//
// The node pool keeps pointers stable and _hash_map implements hash-consing
// (the CSE deduplication). Every node is built through one of the create*
// factories below: they fill in all of the node's fields, compute its hash, and
// only then offer it to intern(). `createNode` and `intern` are private --
// deliberately, so that no call site can get that order wrong. The interning
// protocol itself is documented in dag_node.h.
class IRModule {
 public:
  IRModule() = default;

  // Function being processed
  FuncSignature funcSig;

  // Function body (structured statements)
  std::unique_ptr<StmtIR> body;

  // ----- Constants -----

  // Create a constant node, deduplicated by value so that expressions built
  // around the same literal share a DAG node (enables cross-statement CSE).
  // The int/float kind is inferred: from `text` when the source spelled the
  // literal, else from whether `val` is whole.
  DAGNode* createConst(double val, const std::string& text = "");

  // Create a constant node with an explicit int/float kind. The constant folder
  // uses this to carry the kind across an arithmetic fold (a whole result is
  // floating when either operand was), where no source spelling remains.
  DAGNode* createConstTyped(double val, bool isFloat,
                            const std::string& text = "");

  // Create a constant node that carries a symbolic (declared) form for code
  // emission. Deduplicated by value like createConst; if a node with the same
  // value exists without a symbol, the symbol is attached to it.
  DAGNode* createSymbolicConst(double val, const std::string& symbol);

  // ----- Leaves -----

  // Get or create variable
  DAGNode* getVar(const std::string& name);

  // ----- Generated names -----
  //
  // Every pass that *invents* a name -- the builder's alpha-renaming
  // (`x__s1`), the unroller's per-iteration locals (`t__u0`), CSE's
  // materialized temporaries (`_cse_0_7`) -- used to concatenate a suffix and
  // hope. Nothing checked the result against the names already in the function,
  // so a user variable called `_cse_0_7` silently became the *same* node as the
  // extracted temporary (variables are interned by name), and a user parameter
  // called `t__u0` collided with an unrolled iteration's renamed local.
  //
  // `freshName` is the one authority: it returns `base`, `base_1`, `base_2`, ...
  // -- the first spelling that is neither an existing variable nor reserved --
  // and reserves it, so two callers can never be handed the same name.
  std::string freshName(const std::string& base);

  // Reserve `name` so a later freshName() will not hand it out. Used for names
  // that are not (yet) variable nodes: the builder reserves every identifier it
  // sees in the source, so a generated name can never collide with a name the
  // source uses somewhere the pass has not reached yet.
  void reserveName(const std::string& name);

  // Record the declared numeric kind of a variable. Derived metadata, so this
  // updates the existing node in place (it is not part of the identity) and
  // applies to any later getVar() of the same name.
  void setVarKind(const std::string& name, NumericKind kind);

  // ----- Operators -----

  // Create a binary op node with CSE (hash-based dedup)
  DAGNode* createBinaryOp(OpCode op, DAGNode* lhs, DAGNode* rhs);

  // A plain unary operator: `-x`, `!x`. Shared when its operand is.
  DAGNode* createUnaryOp(OpCode op, DAGNode* operand);

  // `++x` / `--x`. Carries a side effect, so it is never shared and never
  // hoisted; the prefix form also yields the new value. `op` is Inc / Dec.
  DAGNode* createPreIncDec(OpCode op, DAGNode* operand);

  // `x++` / `x--`. As above, but yields the old value.
  DAGNode* createPostIncDec(OpCode op, DAGNode* operand);

  // `(typeName)operand`.
  DAGNode* createCast(const std::string& typeName, DAGNode* operand);

  // `cond ? trueExpr : falseExpr`.
  DAGNode* createTernary(DAGNode* cond, DAGNode* trueExpr, DAGNode* falseExpr);

  // ----- Memory access and calls -----

  // Create an array access node.
  // `shareable`: if true, structurally identical loads share a node. Only safe
  // for loads from read-only locations; otherwise each load is kept distinct.
  // Defaults to false so callers must opt in to deduplication explicitly.
  DAGNode* createArrayAccess(DAGNode* base, DAGNode* index, bool shareable = false);

  // Create a member access node (see createArrayAccess for `shareable`).
  DAGNode* createMemberAccess(DAGNode* base, const std::string& member,
                              bool shareable = false);

  // Create an arrow access node (see createArrayAccess for `shareable`).
  DAGNode* createArrowAccess(DAGNode* base, const std::string& member,
                             bool shareable = false);

  // Create a call node.
  // `pure`: if true, identical calls share a node. Impure calls are never
  // deduplicated (they may have side effects / observe mutable state).
  // Defaults to false so callers must opt in to deduplication explicitly.
  DAGNode* createCall(DAGNode* callee, const std::vector<DAGNode*>& args,
                      bool pure = false);

  // ----- Invariants -----

  // Debug-build check of the interning protocol: every interned node is
  // shareable, every interned node's hash still describes its content, and no
  // bucket holds two distinct nodes that denote the same node. A few lines that
  // catch exactly the class of defect that silently produced wrong code.
  // Compiled out under NDEBUG.
  void verify() const;

 private:
  // Allocate a node. It is NOT interned: the caller must finish filling it in,
  // call recomputeHash(), and then hand it to intern().
  DAGNode* createNode(NodeKind kind);

  // Offer a fully built node to the hash map: returns an existing
  // content-identical node when there is one, registers the candidate
  // otherwise, and returns the candidate unregistered when it is not shareable.
  DAGNode* intern(DAGNode* candidate);

  DAGNode* createUnaryOpImpl(OpCode op, DAGNode* operand,
                             const std::string& name, bool postfix, bool pure);

  // Node pool - owns all DAG nodes
  std::vector<std::unique_ptr<DAGNode>> _node_pool;

  // Hash map for CSE: hash → nodes with that hash (bucket, so hash collisions
  // never evict each other). Only shareable nodes are registered here.
  std::unordered_map<uint64_t, std::vector<DAGNode*>> _hash_map;

  // Variable cache: name → node
  std::unordered_map<std::string, DAGNode*> _var_cache;

  // Every name freshName() must not return: the variable cache plus names
  // reserved explicitly (see reserveName).
  std::unordered_set<std::string> _reserved_names;

  // Declared numeric kind per variable name (see setVarKind).
  std::unordered_map<std::string, NumericKind> _var_kinds;

  uint32_t _next_node_id = 0;
};

// Classify a declared C++ type by numeric kind. Understands the spellings the
// frontend accepts ("double", "float", "int", "unsigned", "long", "size_t",
// "bool", with const/static/inline/volatile and a trailing '&'). A pointer type
// and any template/vector type are Unknown: a pointer's kind is not the kind of
// what it points at, and a vector's elements are lowered separately.
NumericKind numericKindOfType(const std::string& type);

// The numeric kind a node's own shape implies, or its stored kind when the shape
// carries no rule (a variable's kind comes from its declaration, a load's from
// the type of what it denotes).
//
// `numKind` is derived metadata, not part of node identity (dag_node.h), so the
// factories compute it from the operands and nothing should ever *change* it
// behind the operands' backs. IRModule::verify() holds every owned node to this
// rule, which is what turns a stale kind -- a parent built before its operand's
// declaration was seen, or a renamed variable that lost its type -- into a check
// failure instead of a silent capability gap.
NumericKind impliedNumericKind(const DAGNode* n);

}  // namespace cse
