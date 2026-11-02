// Node-identity drift oracle.
//
// DAGNode::sameContentAs() and DAGNode::computeHash() (src/ir/ir_module.cpp)
// both expand CSE_DAGNODE_IDENTITY_FIELDS (src/ir/dag_node.h), so the two can no
// longer compare one field set and hash another -- that half of the invariant is
// structural now.
//
// What the table does *not* force is that every field which changes what a node
// denotes is actually *listed*: a new DAGNode field that nobody adds to the
// macro would quietly stop being part of identity, re-enabling exactly the
// collapses rule 2 exists to prevent (`-x` == `--x`, `(int)x` == `(float)x`,
// `2.0` == `2`).
//
// This pins the table from the outside. For each field, two nodes that differ in
// that field alone must be distinct: sameContentAs() false (the field is
// compared) *and* computeHash() unequal (the field is hashed). A field dropped
// from the macro fails both halves; a field compared but not hashed fails the
// second. The positive control pins the other direction: two nodes with equal
// content are the same node even when their `id`s differ -- `id` is deliberately
// not part of identity.

#include <cstdio>
#include <string>

#include "ir/dag_node.h"

using namespace cse;

namespace {

int failures = 0;

void expectSame(const char* what, const DAGNode& a, const DAGNode& b) {
  const bool ok = a.sameContentAs(b) && a.computeHash() == b.computeHash();
  if (!ok) ++failures;
  std::printf("%-30s %s\n", what, ok ? "OK" : "FAIL");
}

void expectDistinct(const char* what, const DAGNode& a, const DAGNode& b) {
  const bool cmp = a.sameContentAs(b);
  const bool hsh = a.computeHash() == b.computeHash();
  const bool ok = !cmp && !hsh;
  if (!ok) ++failures;
  std::printf("%-30s %s\n", what, ok ? "OK" : "FAIL");
  if (cmp)
    std::printf("    sameContentAs() says equal: the field is missing from "
                "CSE_DAGNODE_IDENTITY_FIELDS\n");
  else if (hsh)
    std::printf("    computeHash() collided: the field is compared but not hashed\n");
}

// A node with its hash already computed, so it can stand in as an operand.
DAGNode leaf(NodeKind kind, uint32_t id, const std::string& name) {
  DAGNode n(kind, id);
  n.name = name;
  n.recomputeHash();
  return n;
}

}  // namespace

int main() {
  // Positive control: equal content, different ids -> the same node. `id` is
  // deliberately outside the identity.
  {
    DAGNode a(NodeKind::BinaryOp, 1), b(NodeKind::BinaryOp, 2);
    a.op = OpCode::Add;
    b.op = OpCode::Add;
    expectSame("equal content, different id", a, b);
  }

  {  // kind
    DAGNode a(NodeKind::BinaryOp, 1), b(NodeKind::Variable, 2);
    expectDistinct("kind differs", a, b);
  }
  {  // op
    DAGNode a(NodeKind::BinaryOp, 1), b(NodeKind::BinaryOp, 2);
    a.op = OpCode::Add;
    b.op = OpCode::Mul;
    expectDistinct("op differs", a, b);
  }
  {  // name
    DAGNode a(NodeKind::Variable, 1), b(NodeKind::Variable, 2);
    a.name = "x";
    b.name = "y";
    expectDistinct("name differs", a, b);
  }
  {  // pure: an unshared load / impure call must not be deduplicated
    DAGNode a(NodeKind::Call, 1), b(NodeKind::Call, 2);
    a.pure = true;
    b.pure = false;
    expectDistinct("pure differs", a, b);
  }
  {  // postfix: `x++` and `++x` yield different values
    DAGNode a(NodeKind::UnaryOp, 1), b(NodeKind::UnaryOp, 2);
    a.op = OpCode::Inc;
    b.op = OpCode::Inc;
    a.postfix = false;
    b.postfix = true;
    expectDistinct("postfix differs", a, b);
  }
  {  // operands: compared by node identity
    DAGNode x = leaf(NodeKind::Variable, 10, "x");
    DAGNode y = leaf(NodeKind::Variable, 11, "y");
    DAGNode a(NodeKind::BinaryOp, 1), b(NodeKind::BinaryOp, 2);
    a.op = OpCode::Add;
    b.op = OpCode::Add;
    a.operands = {&x};
    b.operands = {&y};
    expectDistinct("operands differ", a, b);
  }
  {  // operand count
    DAGNode x = leaf(NodeKind::Variable, 10, "x");
    DAGNode a(NodeKind::BinaryOp, 1), b(NodeKind::BinaryOp, 2);
    a.op = OpCode::Add;
    b.op = OpCode::Add;
    a.operands = {&x};
    expectDistinct("operand count differs", a, b);
  }
  {  // constant value
    DAGNode a(NodeKind::Constant, 1), b(NodeKind::Constant, 2);
    a.constVal = 1.0;
    b.constVal = 2.0;
    expectDistinct("constant value differs", a, b);
  }
  {  // signed zero: `1.0 / -0.0` is -inf, so -0.0 and +0.0 must stay distinct
    DAGNode a(NodeKind::Constant, 1), b(NodeKind::Constant, 2);
    a.constVal = 0.0;
    b.constVal = -0.0;
    expectDistinct("signed zero differs", a, b);
  }
  {  // int/float kind: `2.0` and `2` denote different expressions
    DAGNode a(NodeKind::Constant, 1), b(NodeKind::Constant, 2);
    a.constVal = 2.0;
    b.constVal = 2.0;
    a.isFloat = true;
    b.isFloat = false;
    expectDistinct("int/float kind differs", a, b);
  }

  if (failures == 0) {
    std::printf("\nALL NODE-IDENTITY CHECKS PASSED\n");
    return 0;
  }
  std::printf("\n%d NODE-IDENTITY CHECK(S) FAILED\n", failures);
  return 1;
}
