#pragma once
#include <string>
#include <vector>

#include "analysis/cost_model.h"

namespace cse {
namespace freelb {

// FreeLB `.ur.h` specialization generator.
//
// Reads a production header marked with `// @cse` (e.g. src/lbm/equilibrium.h),
// instantiates every marked template struct for each supported lattice set,
// optimizes the method bodies with the generic CSE pipeline and writes a
// `*.ur.h` fragment containing one partial specialization per lattice set.
//
// The CLI mirrors the previous tool:
//   csegen <input.h> <output.h>
//   csegen --cost [--json] [--lattice NAME]... <input.h> <output.h>

struct UrConfig {
  std::string include;  // include path emitted in the generated header
  std::string ns;       // namespace wrapping the specializations
};

// One measured method specialization. Before/after are taken from the exact
// pipeline that emits the code, so the cost always matches the generated
// `.ur.h` (including vector lowering / counter-prop / recombine choices).
struct UrCostEntry {
  std::string lattice;   // e.g. "D3Q19"
  std::string function;  // e.g. "UImpl::apply"
  int component = -1;    // TLatSetD component index, or -1
  CostResult before;
  CostResult after;
};

struct UrCostReport {
  std::vector<UrCostEntry> entries;
};

struct UrGenerateOptions {
  // When non-null, measure before/after cost for every emitted method.
  UrCostReport* report = nullptr;
  // When non-empty, only these lattice set names are generated/costed
  // (e.g. {"D3Q19"}); empty means all supported sets.
  std::vector<std::string> lattices;
};

// Detect the include/namespace from the input file basename (moment.h,
// equilibrium.h, force.h). Throws std::runtime_error for unknown inputs.
UrConfig detectUrConfig(const std::string& inputPath);

// Generate `outputPath` from `inputPath`. Returns false on failure.
bool generateUrHeader(const std::string& inputPath,
                      const std::string& outputPath,
                      const UrGenerateOptions& opts = {});

}  // namespace freelb
}  // namespace cse
