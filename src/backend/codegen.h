#pragma once
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace cse {

class IRModule;
class StmtIR;
class DAGNode;
struct StructDef;
struct TemplateParam;

// Holds optimized IRModules for struct methods (one per method).
struct OptimizedStruct {
  const StructDef* def;
  std::vector<std::unique_ptr<IRModule>> methodModules;
};

// Backend: converts optimized IR back to C++ source text.
// Handles operator precedence for parentheses and emits struct definitions.
class CodeGen {
 public:
  std::string generate(IRModule& module, const std::vector<StructDef*>& structDefs = {},
    const std::vector<OptimizedStruct>& optStructs = {},
    const std::vector<TemplateParam>& funcTemplateParams = {});

 private:
  void emitStmt(StmtIR* stmt, int indentLevel);
  std::string emitExpr(DAGNode* node);
  std::string makeIndent(int level) const;
  void emitTemplateParams(const std::vector<TemplateParam>& params);

  std::ostringstream _out;
};

}  // namespace cse
