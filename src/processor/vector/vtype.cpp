/**
 * @file vtype.cpp
 * @brief Decoding, encoding and validation of the vtype CSR (RVV 1.0).
 */
#include "processor/vector/vtype.h"

#include "processor/vector/vector_config.h"

#include <array>

namespace Kites
{
namespace rvv
{

namespace
{
constexpr int kElenLog2 = 6; // ELEN = 64
} // namespace

bool isSupportedVtype(uint64_t raw)
{
    const VType vt{raw};
    // vill and bits XLEN-2..8 are reserved in a requested vtype.
    if ((raw >> 8) != 0)
    {
        return false;
    }
    if (vt.vsew() > 3) // SEW > 64
    {
        return false;
    }
    if (vt.vlmul() == 4) // reserved
    {
        return false;
    }
    // Fractional LMUL must support SEW in [SEW_MIN, LMUL * ELEN]; larger SEW is unsupported.
    if (vt.sewLog2() > kElenLog2 + vt.lmulLog2())
    {
        return false;
    }
    return true;
}

uint64_t vlmax(unsigned vlen, int sewLog2, int lmulLog2)
{
    const uint64_t perRegister = static_cast<uint64_t>(vlen) >> sewLog2;
    return lmulLog2 >= 0 ? perRegister << lmulLog2 : perRegister >> (-lmulLog2);
}

uint64_t encodeVtype(unsigned sew, int lmulLog2, bool tailAgnostic, bool maskAgnostic)
{
    unsigned vsew = 0;
    while ((8U << vsew) < sew)
    {
        ++vsew;
    }
    const unsigned vlmul = static_cast<unsigned>(lmulLog2 < 0 ? lmulLog2 + 8 : lmulLog2);
    return (static_cast<uint64_t>(maskAgnostic) << 7) | (static_cast<uint64_t>(tailAgnostic) << 6) |
           (static_cast<uint64_t>(vsew) << 3) | vlmul;
}

std::string vtypeToString(uint64_t raw)
{
    const VType vt{raw};
    if ((raw >> 8) != 0 || vt.vsew() > 3 || vt.vlmul() == 4)
    {
        return std::to_string(raw);
    }
    static const std::array<const char *, 8> lmulNames = {"m1", "m2", "m4", "m8",
                                                           "",   "mf8", "mf4", "mf2"};
    std::string text = "e" + std::to_string(vt.sew()) + ", " + lmulNames[vt.vlmul()];
    text += vt.vta() ? ", ta" : ", tu";
    text += vt.vma() ? ", ma" : ", mu";
    return text;
}

bool isVtypeKeyword(std::string_view token)
{
    static constexpr std::array<std::string_view, 15> keywords = {
        "e8", "e16", "e32", "e64", "mf8", "mf4", "mf2", "m1",
        "m2", "m4",  "m8",  "ta",  "tu",  "ma",  "mu"};
    for (std::string_view keyword : keywords)
    {
        if (token == keyword)
        {
            return true;
        }
    }
    return false;
}

} // namespace rvv
} // namespace Kites
