#pragma once

#include <string>

namespace aicore
{
struct SelfTestResult
{
    bool ok = false;
    std::string detail; // human-readable, ASCII (safe in Windows console logs)
};

// Minimal S0 self-test: validates the baked version string against the source
// version and checks a trivial computation. Falsifiable by construction: a
// malformed build id, a missing sha, an inconsistent .modified flag or a wrong
// calculation all yield ok == false.
SelfTestResult runSelfTest();
} // namespace aicore