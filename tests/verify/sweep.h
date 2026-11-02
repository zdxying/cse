// Shared scaffolding for the fixture-sweep oracles (Phase A/D).
//
// Each oracle reads every fixture named on the command line, extracts its
// `//@cse` regions, and judges each region's IR. They differ only in the
// judgement (`checkRegion`) and in how the summary line is worded; the
// profile builders, file reader and the argv/region walk are the same.
//
//   verify_roundtrip          printer fidelity (source -> IR -> text -> IR)
//   verify_ir_idempotence     every pass is a projection (P(P(x)) == P(x))
//   verify_effect_preservation no pass drops or duplicates an observable effect
//   verify_pass_contracts     every wired pass can fire; effect contracts hold
//   verify_ir_invariants      IRModule::verify() + effect preservation hold
//
// This header removes the copied profile builders and file reader, and the
// walk itself where an oracle's shape matches. No judgement changes.
#pragma once

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "frontend/cse_config.h"
#include "frontend/region_extractor.h"

// The aggressive profile: commutativity, associativity, floating-point
// reassociation and the unsafe special-value/signed-zero identities, which
// together enable every optional pass and every licensed rewrite -- in
// particular CleanupPass's chain merge, gated on `allowUnsafeFpIdentities`,
// which would otherwise never fire here.
//
// It is *not* the full CLI default. The real default is `createFreeLBConfig`,
// which additionally sets `noAlias`, `isPureFunction`, `simplifyBraceInit`,
// `tokenFilter`, `resolveName` and `vectorDim`; those are FreeLB/plugin-specific
// and the scan oracles do not reproduce them. The FreeLB-flavoured pipeline is
// covered instead by the csegen smoke and the three numerical FreeLB verifiers,
// which drive the real `createFreeLBConfig` end to end.
inline cse::CSEConfig aggressive() {
  cse::CSEConfig c;
  c.assumeNumericCommutative = true;
  c.assumeNumericAssociative = true;
  c.allowFpReassoc = true;
  c.allowUnsafeFpIdentities = true;
  return c;
}

// The conservative profile: no algebraic assumptions at all.
inline cse::CSEConfig conservative() { return cse::CSEConfig{}; }

inline std::string readFile(const std::string& path, bool& ok) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    ok = false;
    return {};
  }
  std::ostringstream ss;
  ss << ifs.rdbuf();
  ok = true;
  return ss.str();
}

// The verdict for one region. `Skip` is a construct the oracle does not model
// (e.g. a struct region for the round-trip oracle), not a failure.
enum class Result { Pass, Skip, Fail };

// Drive `check(code, msg)` over every region of every fixture in argv[1..],
// print "<label>: N <unit> (S skipped, F failed)" and return the process exit
// code (0 iff nothing failed; 2 on an unreadable file).
//
//   countAllRegions  true  -> N counts every region (round-trip)
//                    false -> N counts only the regions that passed
//   printSkips       true  -> print a "skip" line per unmodelled region
template <typename Fn>
inline int sweepRegions(int argc, char** argv, const char* label,
                        const char* unit, bool countAllRegions, bool printSkips,
                        Fn check) {
  int total = 0, skipped = 0, failed = 0;
  for (int ai = 1; ai < argc; ++ai) {
    bool ok = false;
    std::string source = readFile(argv[ai], ok);
    if (!ok) {
      std::fprintf(stderr, "cannot read %s\n", argv[ai]);
      return 2;
    }
    for (const auto& r : cse::RegionExtractor().extract(source)) {
      ++total;
      // A region holding a preprocessing directive is outside the modelled
      // language (the driver passes it through unchanged; see
      // CSERegion::hasDirectives). The IR-level oracles cannot judge it, and
      // parsing it directly now raises the directive token instead of dropping
      // it, so skip it by name rather than let it look like a frontend failure.
      if (r.hasDirectives) {
        ++skipped;
        if (printSkips) {
          std::printf("skip  %s:%zu  preprocessor directive in region\n",
                      argv[ai], r.startLine);
        }
        continue;
      }
      std::string msg;
      switch (check(r.code, msg)) {
        case Result::Pass:
          break;
        case Result::Skip:
          ++skipped;
          if (printSkips) {
            std::printf("skip  %s:%zu  %s\n", argv[ai], r.startLine, msg.c_str());
          }
          break;
        case Result::Fail:
          ++failed;
          std::printf("FAIL  %s:%zu\n%s\n", argv[ai], r.startLine, msg.c_str());
          break;
      }
    }
  }
  const int counted = countAllRegions ? total : (total - skipped);
  std::printf("%s: %d %s (%d skipped, %d failed)\n", label, counted, unit,
              skipped, failed);
  return failed == 0 ? 0 : 1;
}
