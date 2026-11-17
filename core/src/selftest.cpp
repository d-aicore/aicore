#include "aicore/selftest.h"

#include "aicore/version.h"

#include <cctype>

namespace aicore
{
namespace
{
bool isHex(const std::string& s)
{
    if (s.empty())
        return false;
    for (unsigned char c : s)
        if (!std::isxdigit(c))
            return false;
    return true;
}
} // namespace

SelfTestResult runSelfTest()
{
    // 1) Trivial computation (falsifiable).
    const int calc = 6 * 7;
    if (calc != 42)
        return { false, "calc 6*7 != 42" };

    // 2) Version format derived from the source version (no copied literal):
    //    buildId == version() + "+" + <hex sha> [+ ".modified"].
    const std::string id = buildId();
    const std::string prefix = version() + "+";
    if (id.rfind(prefix, 0) != 0)
        return { false, "build id sem prefixo " + prefix + ": " + id };

    std::string rest = id.substr(prefix.size());
    const std::string suffix = ".modified";
    bool modified = false;
    if (rest.size() > suffix.size()
        && rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) == 0)
    {
        modified = true;
        rest.erase(rest.size() - suffix.size());
    }

    if (!isHex(rest))
        return { false, "sha git invalido em: " + id };
    if (gitHash().empty() || gitHash() == "unknown")
        return { false, "sha git em falta" };
    if (rest != gitHash())
        return { false, "sha do build id difere do sha baked" };
    if (modified != gitModified())
        return { false, "flag .modified inconsistente" };

    return { true, "versao=" + id + " calc=42" };
}
} // namespace aicore