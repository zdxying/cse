#pragma once
#include <string>
#include <sstream>
#include <vector>
#include <unordered_set>
#include <cstdint>

namespace cse {

class IRModule;
class StmtIR;
class DAGNode;

class CodeGen {
public:
    std::string generate(IRModule& module);

private:
    void emitStmt(StmtIR* stmt, int indentLevel);
    std::string emitExpr(DAGNode* node);
    std::string makeIndent(int level) const;

    std::ostringstream out_;
};

} // namespace cse
