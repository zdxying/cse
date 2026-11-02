// Pass idempotence oracle.
//
// A pass must be a projection: applying it to its own output changes nothing.
// Formally P(P(x)) == P(x). A pass that keeps finding work on already-processed
// IR oscillates -- the result then depends on how many times the pipeline runs,
// and a "run it again to get the fully folded form" surprise is exactly the
// symptom.
//
// The check is per pass, not per pipeline: the pipeline as a whole is *not*
// expected to be a fixpoint (constant folding runs before value propagation, so
// propagating a constant can expose a new fold). The plan deliberately rejects
// fixpoint-driving, so only the individual passes are held to idempotence.
//
// The comparison uses the structural IR fingerprint (ir_printer.h), not the
// emitted text or the FLOP cost: reassociation legitimately reorders operands
// (so the text differs) and the cost model counts extraction temporaries (so
// the cost drifts), but neither is a semantic change. The fingerprint covers
// exactly the fields that define node identity.
//
// Usage: verify_ir_idempotence <fixture.cpp> [more fixtures...]
//   Extracts every //@cse region and checks each function under both the
//   conservative and the aggressive configuration. Exit 0 iff all pass.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "frontend/cse_config.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/region_extractor.h"
#include "ir/ir_builder.h"
#include "ir/ir_module.h"
#include "ir/ir_printer.h"
#include "passes/pass_manager.h"
#include "sweep.h"

using namespace cse;

namespace {

// Build the pipeline once, then for each pass run it twice in a row and require
// the second run to change nothing. The module accumulates the effect of every
// pass, so each pass is checked on the input it would actually see (with the
// caveat that earlier passes ran twice).
Result checkRegion(const std::string& src, const CSEConfig& cfg,
                   std::string& msg) {
  std::vector<Token> tokens;
  Parser::ParseResult res;
  try {
    Lexer lexer(src, cfg);
    tokens = lexer.tokenize();
    Parser parser(tokens, cfg);
    res = parser.parseAll();
  } catch (const std::exception& e) {
    msg = std::string("frontend rejected the region: ") + e.what();
    return Result::Skip;
  }
  if (res.functions.empty()) {
    msg = "no function to check";
    return Result::Skip;
  }

  for (const auto& f : res.functions) {
    IRModule mod;
    IRBuilder builder(&mod, cfg);
    builder.buildFunction(*f);

    auto pm = PassManager::createDefault(cfg, false, nullptr, nullptr);
    for (const auto& pass : pm.passes()) {
      pass->run(mod);
      const std::string once = dumpModule(mod);
      pass->run(mod);
      const std::string twice = dumpModule(mod);
      if (once != twice) {
        msg = "pass '" + pass->name() + "' is not idempotent for function '" +
              f->name + "'\n" + "--- after one run ---\n" + once +
              "\n--- after a second run ---\n" + twice;
        return Result::Fail;
      }
    }
  }
  return Result::Pass;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixture.cpp> [more...]\n", argv[0]);
    return 2;
  }

  int checked = 0, skipped = 0, failed = 0;
  const struct {
    const char* name;
    CSEConfig cfg;
  } profiles[] = {{"conservative", conservative()}, {"aggressive", aggressive()}};

  for (int ai = 1; ai < argc; ++ai) {
    bool ok = false;
    std::string source = readFile(argv[ai], ok);
    if (!ok) {
      std::fprintf(stderr, "cannot read %s\n", argv[ai]);
      return 2;
    }
    auto regs = RegionExtractor().extract(source);
    for (const auto& r : regs) {
      // A directive is outside the modelled language; the driver passes such a
      // region through, so there is no pass pipeline to hold to idempotence.
      if (r.hasDirectives) {
        skipped += static_cast<int>(2);
        continue;
      }
      for (const auto& prof : profiles) {
        std::string msg;
        switch (checkRegion(r.code, prof.cfg, msg)) {
          case Result::Pass:
            ++checked;
            break;
          case Result::Skip:
            ++skipped;
            break;
          case Result::Fail:
            ++failed;
            std::printf("FAIL  %s:%zu [%s]\n%s\n\n", argv[ai], r.startLine,
                        prof.name, msg.c_str());
            break;
        }
      }
    }
  }

  std::printf("ir-idempotence: %d checks (%d skipped, %d failed)\n", checked,
              skipped, failed);
  return failed == 0 ? 0 : 1;
}
