#include "aicore/version.h"

#include "aicore_version.h" // generated at configure time (build/generated)

namespace aicore
{
std::string buildId()     { return AICORE_BUILD_ID; }
std::string version()     { return AICORE_VERSION; }
std::string gitHash()     { return AICORE_GIT_HASH; }
std::string gitDate()     { return AICORE_GIT_DATE; }
bool gitModified()        { return AICORE_GIT_MODIFIED != 0; }
} // namespace aicore