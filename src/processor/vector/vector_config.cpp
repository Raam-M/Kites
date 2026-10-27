/**
 * @file vector_config.cpp
 * @brief Storage, parsing and validation of the global vector configuration.
 */
#include "processor/vector/vector_config.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace Kites
{
namespace rvv
{

VectorConfig &config()
{
    static VectorConfig instance;
    return instance;
}

unsigned VectorConfig::elen() const
{
    return (profile == Profile::Zve32f || profile == Profile::Zve32x) ? 32 : 64;
}

bool VectorConfig::supportsFloat(unsigned sew) const
{
    const bool f = profile == Profile::V || profile == Profile::Zve64d ||
                   profile == Profile::Zve64f || profile == Profile::Zve32f;
    const bool d = profile == Profile::V || profile == Profile::Zve64d;
    switch (sew)
    {
    case 16:
        return f && zvfh;
    case 32:
        return f;
    case 64:
        return d;
    default:
        return false;
    }
}

std::string VectorConfig::describe() const
{
    std::string out = profileName(profile);
    if (zvfh)
        out += ", zvfh";
    if (zvbb)
        out += ", zvbb";
    else if (zvkb)
        out += ", zvkb";
    if (zvbc)
        out += ", zvbc";
    return out;
}

bool isSupportedVlen(unsigned vlen)
{
    return vlen >= kMinVlen && vlen <= kMaxVlen && (vlen & (vlen - 1)) == 0;
}

std::string profileName(Profile profile)
{
    switch (profile)
    {
    case Profile::V:
        return "v";
    case Profile::Zve64d:
        return "zve64d";
    case Profile::Zve64f:
        return "zve64f";
    case Profile::Zve64x:
        return "zve64x";
    case Profile::Zve32f:
        return "zve32f";
    case Profile::Zve32x:
        return "zve32x";
    }
    return "v";
}

std::optional<Profile> parseProfile(const std::string &name)
{
    for (Profile p : {Profile::V, Profile::Zve64d, Profile::Zve64f, Profile::Zve64x,
                      Profile::Zve32f, Profile::Zve32x})
    {
        if (profileName(p) == name)
            return p;
    }
    return std::nullopt;
}

std::string validate(const VectorConfig &cfg)
{
    if (!isSupportedVlen(cfg.vlen))
        return "VLEN must be a power of two from " + std::to_string(kMinVlen) + " to " +
               std::to_string(kMaxVlen);
    if (cfg.zvfh && !cfg.supportsFloat(32))
        return "zvfh requires a profile with floating point (v, zve64d, zve64f or zve32f)";
    if (cfg.zvbc && cfg.elen() != 64)
        return "zvbc requires ELEN = 64";
    return {};
}

std::string applyExtensionList(VectorConfig &cfg, const std::string &list)
{
    VectorConfig next = cfg;
    next.zvfh = next.zvbb = next.zvkb = next.zvbc = false;
    std::stringstream in(list);
    std::string item;
    bool profileGiven = false;
    while (std::getline(in, item, ','))
    {
        item.erase(std::remove_if(item.begin(), item.end(),
                                  [](unsigned char c) { return std::isspace(c) != 0; }),
                   item.end());
        std::transform(item.begin(), item.end(), item.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (item.empty())
            continue;
        if (auto profile = parseProfile(item))
        {
            if (profileGiven)
                return "more than one vector profile given";
            next.profile = *profile;
            profileGiven = true;
        }
        else if (item == "zvfh")
            next.zvfh = true;
        else if (item == "zvbb")
            next.zvbb = true;
        else if (item == "zvkb")
            next.zvkb = true;
        else if (item == "zvbc")
            next.zvbc = true;
        else
            return "unknown vector extension '" + item + "'";
    }
    if (next.zvbb)
        next.zvkb = true; // Zvbb includes Zvkb
    if (std::string error = validate(next); !error.empty())
        return error;
    cfg = next;
    return {};
}

} // namespace rvv
} // namespace Kites
