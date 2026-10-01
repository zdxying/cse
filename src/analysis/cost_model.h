#pragma once

namespace cse {

class IRModule;
struct StmtIR;
struct CSEConfig;

// Result of a cost analysis over one IR module (function body).
//
// Semantics:
//  - flops/vectorOps/memOps count *unique* DAG nodes, scaled by the number of
//    executions of their enclosing loop context (so a computation inside a
//    loop is charged once per iteration, while a CSE temporary shared by
//    several statements is charged once).
//  - flops is a scalar (vector-weighted) count: a vector op costs its lane
//    count and a vector-vector product is a dot product (2d-1).
//  - memOps counts unique loads (array/member/arrow access) and stores.
//  - unknownLoops / unmodeledCalls report constructs the model could not
//    evaluate; a non-zero value means flops is a lower bound.
struct CostResult {
  long long flops = 0;           // scalar FLOPs (+, -, *, /), vector-weighted
  long long vectorOps = 0;       // operations with at least one vector operand
  long long memOps = 0;          // loads (unique) + stores (per assignment)
  long long totalNodes = 0;      // unique DAG nodes
  long long stmts = 0;           // executed statements (loop-scaled)
  long long vars = 0;            // executed declarations (loop-scaled)
  long long unknownLoops = 0;    // loops whose trip count was not resolvable
  long long unmodeledCalls = 0;  // calls with no cost-model entry

  CostResult& operator+=(const CostResult& o) {
    flops += o.flops;
    vectorOps += o.vectorOps;
    memOps += o.memOps;
    totalNodes += o.totalNodes;
    stmts += o.stmts;
    vars += o.vars;
    unknownLoops += o.unknownLoops;
    unmodeledCalls += o.unmodeledCalls;
    return *this;
  }

  long long savedFlops(const CostResult& after) const {
    return flops - after.flops;
  }
  double savedPercent(const CostResult& after) const {
    return flops == 0
               ? 0.0
               : static_cast<double>(flops - after.flops) * 100.0 /
                     static_cast<double>(flops);
  }
};

// Analyze the cost of `module`. `config` supplies the optional lane/call/op
// hooks (see CSEConfig); when null, the model is purely scalar with calls
// charged 0 and not flagged.
CostResult analyzeCost(IRModule& module, const CSEConfig* config = nullptr);

}  // namespace cse
