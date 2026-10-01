// csegen: FreeLB .ur.h specialization generator.
//
//   csegen <input.h> <output.h>
//   csegen --cost [--json] [--lattice NAME]... <input.h> <output.h>
//
// Reads a `// @cse`-marked production header and emits the corresponding
// unrolled-for specializations for every supported lattice set. With `--cost`
// it also reports the before/after FLOP cost measured on the exact pipeline
// that produced the output, so the numbers always match the generated `.ur.h`.
#include <algorithm>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "ur_emit.h"

namespace {

void printUsage() {
  std::cout
      << "Usage: csegen [--cost] [--json] [--lattice NAME]... <input.h> <output.h>\n"
      << "  --cost           report before/after FLOP cost of the emitted methods\n"
      << "  --json           emit the cost report as JSON (with --cost)\n"
      << "  --lattice NAME   restrict to a lattice set (repeatable; default all)\n"
      << "  -h, --help       show this help\n";
}

std::vector<std::string> latticeOrder(const cse::freelb::UrCostReport& r) {
  std::vector<std::string> order;
  for (const auto& e : r.entries)
    if (std::find(order.begin(), order.end(), e.lattice) == order.end())
      order.push_back(e.lattice);
  return order;
}

void printCostText(const std::string& input,
                   const cse::freelb::UrCostReport& r) {
  std::cout << "\n=== CSE cost (csegen pipeline): " << input << " ===\n";
  for (const auto& lat : latticeOrder(r)) {
    cse::CostResult tb, ta;
    std::cout << "\n--- [" << lat << "] ---\n";
    std::cout << "  " << std::left << std::setw(35) << "Function" << std::right
              << std::setw(10) << "Before" << std::setw(10) << "After"
              << std::setw(10) << "Saved" << std::setw(8) << "%" << "\n";
    std::cout << "  " << std::string(73, '-') << "\n";
    for (const auto& e : r.entries) {
      if (e.lattice != lat) continue;
      tb += e.before;
      ta += e.after;
      std::string name = e.function;
      if (e.component >= 0) name += "/d=" + std::to_string(e.component);
      long long saved = e.before.savedFlops(e.after);
      double pct = e.before.savedPercent(e.after);
      std::cout << "  " << std::left << std::setw(35) << name << std::right
                << std::setw(10) << e.before.flops << std::setw(10)
                << e.after.flops << std::setw(10) << saved << std::setw(7)
                << std::fixed << std::setprecision(1) << pct << "%\n";
    }
    std::cout << "  " << std::string(73, '-') << "\n";
    long long saved = tb.savedFlops(ta);
    double pct = tb.savedPercent(ta);
    std::cout << "  Total: Before " << tb.flops << " flops, After " << ta.flops
              << " flops, Saved " << saved << " ("
              << std::round(pct * 10.0) / 10.0 << "%)  [vector-ops "
              << tb.vectorOps << "->" << ta.vectorOps << ", mem-ops "
              << tb.memOps << "->" << ta.memOps << "]\n";
    long long ul = tb.unknownLoops + ta.unknownLoops;
    long long uc = tb.unmodeledCalls + ta.unmodeledCalls;
    if (ul || uc) {
      std::cout << "  Coverage: " << ul << " unresolved loops, " << uc
                << " unmodeled calls (flops is a lower bound)\n";
    }
  }
}

void printCostJson(const std::string& input,
                   const cse::freelb::UrCostReport& r) {
  auto side = [](const cse::CostResult& c) {
    std::cout << "{\"flops\":" << c.flops << ",\"vectorOps\":" << c.vectorOps
              << ",\"memOps\":" << c.memOps << ",\"nodes\":" << c.totalNodes
              << ",\"unknownLoops\":" << c.unknownLoops
              << ",\"unmodeledCalls\":" << c.unmodeledCalls << "}";
  };
  std::cout << "{\n  \"input\": \"" << input << "\",\n  \"lattices\": [\n";
  auto order = latticeOrder(r);
  for (size_t li = 0; li < order.size(); ++li) {
    const auto& lat = order[li];
    cse::CostResult tb, ta;
    for (const auto& e : r.entries) {
      if (e.lattice != lat) continue;
      tb += e.before;
      ta += e.after;
    }
    std::cout << "    {\"lattice\": \"" << lat << "\", \"functions\": [\n";
    bool first = true;
    for (const auto& e : r.entries) {
      if (e.lattice != lat) continue;
      if (!first) std::cout << ",\n";
      first = false;
      std::cout << "      {\"name\": \"" << e.function << "\"";
      if (e.component >= 0) std::cout << ", \"component\": " << e.component;
      std::cout << ", \"before\": ";
      side(e.before);
      std::cout << ", \"after\": ";
      side(e.after);
      std::cout << "}";
    }
    std::cout << "\n    ], \"total\": {\"before\": ";
    side(tb);
    std::cout << ", \"after\": ";
    side(ta);
    std::cout << "}}";
    if (li + 1 < order.size()) std::cout << ",";
    std::cout << "\n";
  }
  std::cout << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
  bool cost = false;
  bool json = false;
  std::vector<std::string> lattices;
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printUsage();
      return 0;
    } else if (a == "--cost") {
      cost = true;
    } else if (a == "--json") {
      json = true;
    } else if (a == "--lattice") {
      if (i + 1 >= argc) {
        std::cerr << "csegen: --lattice requires a name\n";
        return 1;
      }
      lattices.push_back(argv[++i]);
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "csegen: unknown option " << a << "\n";
      printUsage();
      return 1;
    } else {
      positional.push_back(a);
    }
  }

  if (positional.size() != 2) {
    printUsage();
    return 1;
  }

  cse::freelb::UrCostReport report;
  cse::freelb::UrGenerateOptions opts;
  opts.lattices = lattices;
  if (cost) opts.report = &report;

  try {
    if (!cse::freelb::generateUrHeader(positional[0], positional[1], opts)) {
      return 1;
    }
  } catch (const std::exception& e) {
    std::cerr << "csegen: " << e.what() << "\n";
    return 1;
  }

  if (cost) {
    if (json)
      printCostJson(positional[0], report);
    else
      printCostText(positional[0], report);
  }
  return 0;
}
