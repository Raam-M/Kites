/**
 * @file vector_format.cpp
 * @brief Element splitting and formatting for vector register displays.
 */
#include "processor/vector/vector_format.h"

#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace Kites
{
namespace rvv
{

std::optional<ElementFormat> parseElementFormat(const std::string &name)
{
    if (name == "hex" || name == "x")
        return ElementFormat::Hex;
    if (name == "u" || name == "unsigned")
        return ElementFormat::Unsigned;
    if (name == "s" || name == "signed")
        return ElementFormat::Signed;
    if (name == "f" || name == "float")
        return ElementFormat::Float;
    return std::nullopt;
}

std::vector<uint64_t> splitElements(const std::vector<uint8_t> &bytes, unsigned sew)
{
    const unsigned width = sew / 8;
    std::vector<uint64_t> out;
    for (size_t i = 0; width != 0 && i + width <= bytes.size(); i += width)
    {
        uint64_t v = 0;
        for (unsigned b = 0; b < width; ++b)
            v |= static_cast<uint64_t>(bytes[i + b]) << (8 * b);
        out.push_back(v);
    }
    return out;
}

double halfToDouble(uint64_t bits)
{
    const bool negative = ((bits >> 15) & 1U) != 0;
    const unsigned exp = static_cast<unsigned>((bits >> 10) & 0x1F);
    const unsigned sig = static_cast<unsigned>(bits & 0x3FF);
    double magnitude = 0;
    if (exp == 0x1F)
        magnitude = sig == 0 ? std::numeric_limits<double>::infinity()
                             : std::numeric_limits<double>::quiet_NaN();
    else if (exp == 0)
        magnitude = std::ldexp(static_cast<double>(sig), -24);
    else
        magnitude = std::ldexp(static_cast<double>(sig | 0x400), static_cast<int>(exp) - 25);
    return negative ? -magnitude : magnitude;
}

std::string formatElement(uint64_t value, unsigned sew, ElementFormat format)
{
    std::ostringstream out;
    const uint64_t mask = sew >= 64 ? ~0ULL : ((1ULL << sew) - 1);
    value &= mask;
    switch (format)
    {
    case ElementFormat::Unsigned:
        out << value;
        break;
    case ElementFormat::Signed:
    {
        const uint64_t sign = 1ULL << (sew - 1);
        out << static_cast<int64_t>((value ^ sign) - sign);
        break;
    }
    case ElementFormat::Float:
        if (sew == 16 || sew == 32 || sew == 64)
        {
            double d = 0;
            if (sew == 16)
            {
                d = halfToDouble(value);
            }
            else if (sew == 32)
            {
                float f;
                const auto bits = static_cast<uint32_t>(value);
                std::memcpy(&f, &bits, 4);
                d = f;
            }
            else
            {
                std::memcpy(&d, &value, 8);
            }
            out << std::setprecision(sew == 64 ? 17 : sew == 32 ? 9 : 5) << d;
            break;
        }
        [[fallthrough]];
    case ElementFormat::Hex:
    default:
        out << "0x" << std::hex << std::setw(static_cast<int>(sew / 4)) << std::setfill('0')
            << value;
        break;
    }
    return out.str();
}

std::string formatRegister(const std::vector<uint8_t> &bytes, unsigned sew, ElementFormat format)
{
    std::ostringstream out;
    const auto elements = splitElements(bytes, sew);
    for (size_t i = 0; i < elements.size(); ++i)
        out << (i == 0 ? "" : "  ") << "[" << i << "] " << formatElement(elements[i], sew, format);
    return out.str();
}

} // namespace rvv
} // namespace Kites
