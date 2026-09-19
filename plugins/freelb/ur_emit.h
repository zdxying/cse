#pragma once
#include <string>

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

struct UrConfig {
  std::string include;  // include path emitted in the generated header
  std::string ns;       // namespace wrapping the specializations
};

// Detect the include/namespace from the input file basename (moment.h,
// equilibrium.h, force.h). Throws std::runtime_error for unknown inputs.
UrConfig detectUrConfig(const std::string& inputPath);

// Generate `outputPath` from `inputPath`. Returns false on failure.
bool generateUrHeader(const std::string& inputPath, const std::string& outputPath);

}  // namespace freelb
}  // namespace cse
