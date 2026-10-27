/**
 * @file vector_format.h
 * @brief Splitting vector registers into elements and formatting element values, shared by
 * the CLI and the GUI vector viewer.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Kites
{
namespace rvv
{

enum class ElementFormat
{
    Hex,
    Unsigned,
    Signed,
    Float, ///< SEW 16 (half), 32 (single) or 64 (double); other SEWs fall back to hex
};

/// "hex", "u"/"unsigned", "s"/"signed", "f"/"float".
std::optional<ElementFormat> parseElementFormat(const std::string &name);

/// Elements of a register image (element 0 in the lowest bytes), each sew bits wide.
std::vector<uint64_t> splitElements(const std::vector<uint8_t> &bytes, unsigned sew);

/// Exact value of an IEEE 754 binary16 encoding.
double halfToDouble(uint64_t bits);

std::string formatElement(uint64_t value, unsigned sew, ElementFormat format);

/// One register as "[0] value  [1] value ..." (element 0 first).
std::string formatRegister(const std::vector<uint8_t> &bytes, unsigned sew, ElementFormat format);

} // namespace rvv
} // namespace Kites
