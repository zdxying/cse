#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>

namespace cse {

// Where a frontend rejection happened, and why.
struct Diagnostic {
  std::string phase;  // "lex" or "parse"
  size_t line = 0;    // 1-indexed, relative to the text that was parsed
  size_t col = 0;     // 1-indexed
  std::string message;
};

// A rejection the caller is expected to recover from.
//
// The frontend used to throw std::runtime_error and the driver had no handler at
// all, so one unparseable region ran into std::terminate: the process aborted,
// and because the output file is only created at the very end, the user got no
// file at all -- not even for the regions that had already been optimized
// successfully. Report the region and keep going is only possible if the failure
// carries enough information to say *where* it was, which is what a typed
// diagnostic is for.
class CSEError : public std::runtime_error {
 public:
  explicit CSEError(Diagnostic diag)
      : std::runtime_error(describe(diag)), _diag(std::move(diag)) {}

  const Diagnostic& diagnostic() const { return _diag; }

 private:
  static std::string describe(const Diagnostic& d) {
    std::string s = d.phase;
    if (d.line != 0) {
      s += ":" + std::to_string(d.line);
      if (d.col != 0) s += ":" + std::to_string(d.col);
    }
    s += ": " + d.message;
    return s;
  }

  Diagnostic _diag;
};

}  // namespace cse
