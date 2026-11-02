#pragma once
#include <map>
#include <string>
#include <vector>

#include "../ir/statement.h"

namespace cse {

// The alias relation -- which locations a statement reads and writes, and
// whether two may denote the same object -- lives in src/ir/location.h
// (`Location`, `LocationSet`, `AliasInfo`). This header is only about effects.

// A multiset of the observable effects a body performs, one entry per impure
// call and per increment/decrement, counted per *occurrence* -- a node shared by
// CSE and printed at three use sites counts three times, because the question is
// how many effects the emitted code performs.
//
// An atom names what the effect *is*, not where it lands:
//   "call:<callee>"        an impure call to <callee>
//   "incdec:++:pre"        any prefix increment
//   "incdec:--:post"       any postfix decrement
// The increment/decrement atom deliberately omits its target: the loop unroller
// alpha-renames body locals (`t` -> `t__u0`), so a name-keyed atom would report
// a rename as a dropped effect. Two effects are likewise excluded as
// loop-control rather than observable: the loop's own induction step
// (`for (...; ++i)`) is not counted, and stores are excluded entirely, since
// dead-store elimination legitimately removes a store to private storage. An
// increment/decrement is kept even when its target is otherwise dead (see DCE),
// so its disappearance is always a violation.
using EffectSignature = std::map<std::string, int>;

// Count the effects in `body`, across every statement and every expression slot
// except a loop's induction step.
EffectSignature effectSignature(StmtIR* body);

// The effects of `body` in **program order**, as an ordered list of atoms.
//
// `effectSignature` answers "how many of each effect"; this answers "in what
// order do they happen". The distinction matters because reordering two effects
// (an impure call past a read of what it writes, two `++` across a read of their
// operand) changes behavior while leaving the multiset untouched -- which is how
// a reassociation of `state + f() + state` could compile into
// `state + state + f()` with `f` writing `state` (see tests/fixtures/
// effect_order.cpp). A pass that reorders observable effects must declare it,
// the way it declares mayDuplicateEffects.
//
// The order is the IR's evaluation order: statements in sequence, an
// expression's operands left to right (the builder builds them in a fixed
// order), a for-loop's init/cond/update/body in that order.
std::vector<std::string> effectSequence(StmtIR* body);

// Compare two ordered effect sequences and describe any violation, or return ""
// when `after` keeps the same effects in the same relative order. `mayDuplicate`
// allows each effect of `before` to appear more often in `after` (loop
// unrolling), but the *first* occurrence of each atom must stay in order, so
// unrolling `f(); g();` inside a loop still reads f-before-g in every iteration.
std::string effectOrderViolation(const std::vector<std::string>& before,
                                 const std::vector<std::string>& after,
                                 bool mayDuplicate);

// Compare two effect signatures and describe any violation, or return "" when
// `after` preserves `before`. Dropping an effect is always a violation; adding
// one is a violation unless `mayDuplicate` (loop unrolling legitimately
// replicates a body, so an impure call inside the loop becomes one per
// iteration).
std::string effectViolation(const EffectSignature& before,
                            const EffectSignature& after, bool mayDuplicate);

}  // namespace cse
