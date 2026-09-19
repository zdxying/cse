// csegen: FreeLB .ur.h specialization generator.
//
//   csegen <input.h> <output.h>
//
// Reads a `// @cse`-marked production header and emits the corresponding
// unrolled-for specializations for every supported lattice set.
#include <exception>
#include <iostream>
#include <string>

#include "ur_emit.h"

int main(int argc, char** argv) {
  if (argc == 2) {
    std::string arg = argv[1];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: csegen <input.h> <output.h>\n";
      return 0;
    }
  }
  if (argc != 3) {
    std::cerr << "Usage: csegen <input.h> <output.h>\n";
    return 1;
  }
  try {
    return cse::freelb::generateUrHeader(argv[1], argv[2]) ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "csegen: " << e.what() << "\n";
    return 1;
  }
}
