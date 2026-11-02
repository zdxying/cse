#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dag_node.h"
#include "ir_module.h"
#include "statement.h"

namespace cse {

// ===== The Location model =====
//
// Every question the passes ask about memory -- "which names does this
// statement write?", "may this read have changed across that write?", "is this
// storage private to the function?" -- used to be answered by a handful of
// independent walkers, each reducing an lvalue to a bare root-name string
// (`lvalueRoot`) and each carrying its own copy of the expression-slot list.
// They drifted: the write-set scanner once missed a declaration initializer,
// the DCE owned-name test once treated a reference parameter as a local.
//
// This module is the single authority for those questions. It answers them in
// terms of `Location` -- a root name plus how the location is reached within it
// -- so there is one place to change and one relation (`mayAlias`) to reason
// about.

// How the storage a declared type names relates to the declaring name. This is
// the one predicate behind both DCE's "is this owned local?" and the builder's
// "is this an aliasable parameter?": the two used to test the type string
// independently.
//
//  * PrivateLocal -- a value parameter or a local scalar/object. Nothing
//    outside the function can name it, so it aliases no other root.
//  * Reference    -- `T&` (parameter or local). The name is a second name for
//    another object; `double& r = x;` makes `r` and `x` the same storage.
//  * Pointer      -- `T*`. The pointer variable is private, but the pointee it
//    selects may be reachable through other pointers.
//  * Array        -- `T[]` / `T[N]`. The elements may be reached through
//    another pointer to the same buffer.
//  * Global       -- not declared in this function: storage that outlives the
//    call and can be named from anywhere.
//  * NoAlias      -- the caller guarantees this root aliases nothing else
//    (opt-in, see CSEConfig::noAlias).
enum class AliasClass {
  PrivateLocal,
  Reference,
  Pointer,
  Array,
  Global,
  NoAlias,
};

AliasClass classifyDeclaredType(const std::string& type);

// How a location is reached within its root.
enum class AccessKind {
  Direct,   // the root itself
  Indexed,  // root[...]
  Member,   // root.field
  Arrow,    // root->field
  Deref,    // *root
};

// A storage location: a root name and the access that selects within it.
// `unknown` marks a location that may name anything -- the wildcard an impure
// call (or a reference whose referent cannot be named) contributes.
//
// The root's alias class is not stored here: it is a property of the declared
// name, resolved on demand by classifyDeclaredType(), and storing it on every
// Location would only duplicate it.
struct Location {
  std::string root;
  AccessKind access = AccessKind::Direct;
  bool unknown = false;
};

bool operator==(const Location& a, const Location& b);

// May two locations denote the same object?
//
// The relation is deliberately the conservative one: two locations alias when
// they share a root (any access within a root reaches the same base storage),
// and anything may alias a location that is not nameable. Distinct roots are
// disjoint here -- write sets are already closed over reference-referent edges
// (AliasInfo::closeWrites), which is what makes two distinct roots genuinely
// independent.
bool mayAlias(const Location& a, const Location& b);

// A set of locations, plus a "may be anything" wildcard. Replaces the old
// `std::unordered_set<std::string>` whose `"*"` entry meant the same thing.
//
// The wildcard is tested separately from the locations, because callers want
// two different things from it:
//   * a *stability* check ("could this read have changed?") treats the wildcard
//     as reaching everything -- use `anything()` or `aliasesName()`;
//   * a *by-name* check ("is this exact name written?") must not -- an impure
//     call does not make `k` written just because it could write something --
//     use `containsRoot()`.
class LocationSet {
 public:
  void add(const Location& loc);
  // Add a location reached directly at `root` (no-op for an empty root, which
  // names no storage).
  void addRoot(const std::string& root, AccessKind access = AccessKind::Direct);
  void addUnknown() { _anything = true; }

  bool empty() const { return _items.empty() && !_anything; }
  bool anything() const { return _anything; }

  // Is the location `root` (reached directly) in the set? The wildcard is not a
  // match; test it with anything().
  bool containsRoot(const std::string& root) const;

  // May any location in the set denote the storage named `name`? The wildcard
  // matches everything.
  bool aliasesName(const std::string& name) const;

  const std::vector<Location>& items() const { return _items; }

 private:
  std::vector<Location> _items;
  bool _anything = false;
};

// Root name of an lvalue expression: `a[i].m` -> "a"; an expression that names
// no variable (a call result) -> "". The one definition of "the base name".
std::string locationRoot(DAGNode* n);

// The location an lvalue expression denotes: its root plus how it is reached.
Location locationOf(DAGNode* n);

// ===== Read / write collection =====

// Every variable name read by an expression / statement (deduplicated).
void collectReadNames(DAGNode* e, std::unordered_set<std::string>& out);
void collectReadNames(StmtIR* stmt, std::unordered_set<std::string>& out);

// Every variable occurrence, counted. The read-counting walk (an element
// store's index counts as a use of the root, which keeps a declaration that is
// only ever stored into from looking unused).
void countReadNames(DAGNode* e, std::unordered_map<std::string, int>& out);
std::unordered_map<std::string, int> countReadNames(StmtIR* root);

// Every name a statement mentions: reads, write targets and the names a
// declaration introduces. (CounterProp -- the FreeLB plugin's post-algebra
// pass -- needs the declarations for its vector-local resolver, which a pure
// read walk does not report.)
void collectMentionedNames(StmtIR* stmt, std::unordered_set<std::string>& out);

// Every location an expression / statement writes. An impure call contributes
// the wildcard: it may write any global and anything reachable from an
// argument. A structured store (`a[i] = v;`, `p->f = v;`) records the location
// of its lvalue, and a `for` update the builder normalized out of the
// expression tree is added explicitly.
void collectWriteLocations(DAGNode* e, LocationSet& out);
void collectWriteLocations(StmtIR* stmt, LocationSet& out);

// ===== Alias information for one function =====

// Reference-referent tracking for a function body.
//
// A reference local names another object (`double& r = x;`); a write through
// `r` writes `x`. AliasInfo records those edges and closes a written set over
// them, so a consumer that asks "is `x` written?" sees the write through `r`.
// A reference whose referent is not a plain name (`T& r = f();`) contributes
// the wildcard instead.
//
// Reference *parameters* are not tracked: their referent is the caller's object
// and resolving it needs call-site knowledge the tool does not have. They are
// handled by the builder's root rules (a load through a reference/pointer
// parameter is not shareable).
class AliasInfo {
 public:
  static AliasInfo build(const StmtIR* body);

  // Add to `written` every root reachable from a root already in it by a
  // reference chain, to a fixed point. A root whose referent is unknown makes
  // the whole set "anything".
  void closeWrites(LocationSet& written) const;

  // The write set of `stmt`, closed over reference-referent edges. This is the
  // entry point every pass should use: assembling the set by hand
  // (`collectWriteLocations` + `closeWrites`) is what let a pass forget the
  // closure -- or a whole statement shape -- and forward a stale value.
  LocationSet writtenBy(StmtIR* stmt) const;

  // Is `name` the referent of some reference declaration in this body -- i.e.
  // the object a `T& r = name;` binds to?
  //
  // Such a name is an *lvalue binding target*, not a value, and must not be
  // value-propagated. Copying its initializer to `T& r = name;` would bind the
  // reference to an rvalue: ill-formed for a non-const `T&`, and a silently
  // different object (a temporary) for a `const T&`. Even rewriting only its
  // other uses and dropping its declaration leaves the binding dangling. A
  // pass that inlines locals must therefore treat a referent as non-inlinable.
  bool isAliasedReferent(const std::string& name) const;

  // May the writes in `written` change the value observed by reading `name`?
  //
  // The mirror of the closure `writtenBy` applies to writes: reading a
  // reference local observes its referent's storage (`s3 -> r2 -> b` means
  // reading `s3` observes `b`), so `name` is closed over reference edges before
  // the comparison. Without this a consumer comparing a raw read name against
  // the referent-closed write set sees two disjoint roots where the storage is
  // in fact the same -- and reuses a value across a write that changed it. A
  // read through a reference whose referent is not a name observes arbitrary
  // storage, so any write may affect it.
  bool writeAffectsRead(const LocationSet& written,
                        const std::string& name) const;

 private:
  void scan(const StmtIR* stmt);
  // Record a declaration that introduces a reference (`T& r = x;`) -- the edge
  // `r -> x` (or a wildcard when the referent is not a plain name). Called for a
  // VarDecl wherever it appears, including a `for`-init.
  void recordRefDecl(const VarDeclIR* decl);

  // reference name -> the root it names (the root may itself be a reference).
  std::unordered_map<std::string, std::string> _referentOf;
  // reference names whose referent is not a plain name: a write through one may
  // reach any name.
  std::unordered_set<std::string> _unknownReferent;
};

}  // namespace cse
