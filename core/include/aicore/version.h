#pragma once

#include <string>

namespace aicore
{
// Baked at configure time from the git HEAD (cmake/aicore_version.cmake).
// buildId() == "0.1.0+<sha10>" and gains ".modified" when the tracked tree was
// dirty at configure. This is the exact string printed by `--version`.
std::string buildId();
std::string version();
std::string gitHash();
std::string gitDate();
bool gitModified();
} // namespace aicore