#pragma once
#include <string>

#include "ir_module.h"

namespace cse {

// Deterministic, order-sensitive structural dump of a module.
//
// Nodes are printed as a linear let-table: each distinct node is assigned a
// sequential `%N` label in depth-first order from the body, and every reference
// prints that label. Shared subexpressions are therefore named once and
// referred to thereafter, so the dump is O(nodes) instead of expanding the DAG
// -- and, unlike node ids, the labels are a pure function of the program shape,
// operand order and node identity fields. Two dumps compare equal iff the two
// modules denote the same program in the same order.
//
// `includeMetadata` additionally prints the non-identity presentation slots (a
// constant's source spelling and symbolic form, a node's vector width). The
// default prints exactly the fields that make two nodes interchangeable -- the
// list DAGNode::sameContentAs() compares -- which is what an idempotence
// fingerprint wants; the metadata view is for reading a dump by hand.
std::string dumpModule(const IRModule& module, bool includeMetadata = false);

}  // namespace cse
