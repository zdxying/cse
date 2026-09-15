#pragma once
#include <string>

namespace cse {

class IRModule;
struct StmtIR;

struct CostResult {
  int flops = 0;       // +, -, *, / BinaryOp count
  int totalNodes = 0;  // all DAGNode count
  int stmts = 0;       // statement count
  int vars = 0;        // VarDecl count

  int savedFlops(const CostResult& after) const {
    return flops - after.flops;
  }
  double savedPercent(const CostResult& after) const {
    return flops == 0 ? 0.0 : (flops - after.flops) * 100.0 / flops;
  }
};

CostResult analyzeCost(IRModule& module);

}  // namespace cse
