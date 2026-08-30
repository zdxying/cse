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

    void topoSort(DAGNode* node, std::vector<DAGNode*>& order,
                  std::unordered_set<uint32_t>& visited);

    void analyzeTemporaries(DAGNode* node, std::unordered_set<uint32_t>& needsTemp);

    bool isSimple(DAGNode* node);

    std::ostringstream out_;
    int tempCounter_ = 0;
    std::unordered_set<uint32_t> assigned_;
};

} // namespace cse
