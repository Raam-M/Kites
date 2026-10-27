/**
 * @file vector_exec.h
 * @brief Internal state and helpers shared by the vector instruction implementations.
 */
#pragma once

#include "processor/vector/vector_isa.h"
#include "processor/vector/vector_unit.h"
#include "processor/vector/vtype.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Kites
{
namespace rvv
{
namespace detail
{

/// A vector operand: the registers it occupies and its element width.
struct Group
{
    unsigned reg;  ///< first register
    unsigned eew;  ///< element width in bits; 1 for mask operands
    int emulLog2;  ///< log2(EMUL); masks use 0 (one register)
};

inline uint64_t maskBits(unsigned bits)
{
    return bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
}

inline int64_t signExtend(uint64_t value, unsigned bits)
{
    if (bits >= 64)
        return static_cast<int64_t>(value);
    const uint64_t sign = 1ULL << (bits - 1);
    value &= maskBits(bits);
    return static_cast<int64_t>((value ^ sign) - sign);
}

/// Number of registers a group occupies.
inline unsigned groupRegisters(const Group &g)
{
    return g.emulLog2 > 0 ? (1U << g.emulLog2) : 1U;
}

inline bool overlaps(const Group &a, const Group &b)
{
    const unsigned aEnd = a.reg + groupRegisters(a);
    const unsigned bEnd = b.reg + groupRegisters(b);
    return a.reg < bEnd && b.reg < aEnd;
}

/// One element store, applied to memory when the instruction commits.
struct PendingStore
{
    uint64_t address;
    unsigned bytes;
    uint64_t value;
};

/**
 * @brief Everything one vector instruction needs: decoded fields, vector state, a snapshot of
 * the vector registers to read from, a working copy to write to, and staged scalar/CSR/memory
 * writes.
 */
class ExecState
{
  public:
    ExecState(const InsnInfo &info, uint32_t word, ExecutionContext &context);

    const InsnInfo &info;
    uint32_t word;
    ExecutionContext &context;

    unsigned vlen;
    unsigned vlenb;
    VType vtype;
    unsigned sew;
    int sewLog2;
    int lmulLog2;
    uint64_t vl;
    uint64_t vstart;
    uint64_t vlmax;
    bool masked; ///< vm field present and 0 (operation predicated on v0.t)

    std::vector<uint8_t> source;      ///< vector registers before the instruction
    std::vector<uint8_t> destination; ///< vector registers after the instruction

    std::optional<std::pair<unsigned, uint64_t>> gprWrite;
    std::optional<std::pair<unsigned, uint64_t>> fprWrite;
    std::optional<uint64_t> newVl;
    std::optional<uint64_t> newVtype;
    bool setVxsat = false;
    uint32_t fflags = 0;
    std::vector<PendingStore> stores;

    // ---- instruction fields
    [[nodiscard]] unsigned field(uint32_t f) const { return getField(word, f); }
    [[nodiscard]] unsigned vd() const { return field(kVd); }
    [[nodiscard]] unsigned vs1() const { return field(kVs1); }
    [[nodiscard]] unsigned vs2() const { return field(kVs2); }
    [[nodiscard]] bool hasVectorSrc1() const { return info.hasField(kVs1); }

    /// The scalar operand (x[rs1], f[rs1] bits, or the immediate), not truncated.
    [[nodiscard]] uint64_t scalarOperand() const;
    /// Operand 1 of element i at width eew: vs1[i] or the scalar truncated to eew.
    [[nodiscard]] uint64_t src1(uint64_t index, unsigned eew) const;

    // ---- element access (reads come from the snapshot, writes go to the working copy)
    [[nodiscard]] uint64_t read(unsigned reg, uint64_t index, unsigned eew) const;
    void write(unsigned reg, uint64_t index, unsigned eew, uint64_t value);
    [[nodiscard]] bool maskBit(unsigned reg, uint64_t index) const;
    void writeMaskBit(unsigned reg, uint64_t index, bool value);
    [[nodiscard]] bool active(uint64_t index) const { return !masked || maskBit(0, index); }

    // ---- tail and inactive-element policies
    void inactive(const Group &dest, uint64_t index);
    void inactiveMaskBit(unsigned reg, uint64_t index);
    void tail(const Group &dest) { tailFrom(dest, vl); }
    void tailFrom(const Group &dest, uint64_t first);
    void maskTail(unsigned reg, uint64_t first);
    [[nodiscard]] uint64_t groupElements(const Group &g) const;

    // ---- scalar state
    [[nodiscard]] uint64_t gpr(unsigned reg) const;
    [[nodiscard]] uint64_t fpr(unsigned reg) const;
    [[nodiscard]] uint64_t csr(size_t address) const;

    // ---- legality
    [[noreturn]] void illegal(const std::string &reason) const;
    void require(bool condition, const std::string &reason) const;
    /// vtype must be valid and vstart in range.
    void requireVtype() const;
    void requireVstartZero() const;
    /// Group with EEW = eew and EMUL = (eew / SEW) * LMUL.
    [[nodiscard]] Group group(unsigned reg, unsigned eew) const;
    [[nodiscard]] static Group maskGroup(unsigned reg) { return {reg, 1, 0}; }
    void checkGroup(const Group &g, const char *name) const;
    /// Applies the spec's source/destination overlap rules for different EEWs.
    void checkOverlap(const Group &dest, const Group &src, const char *srcName) const;
    /// Forbids any overlap between dest and src (instructions with stricter rules).
    void forbidOverlap(const Group &dest, const Group &src, const char *srcName) const;
    /// A masked instruction's destination may not overlap v0 unless it is a mask.
    void checkMaskOverlap(const Group &dest) const;
};

void executeConfig(ExecState &s);
void executeMemory(ExecState &s);
void executeInteger(ExecState &s);
void executeFloat(ExecState &s);

bool isImplementedInteger(const InsnInfo &info);
bool isImplementedFloat(const InsnInfo &info);

/// 64 x 64 -> high 64 bits of the product (portable).
uint64_t mulHighUnsigned(uint64_t a, uint64_t b);
int64_t mulHighSigned(int64_t a, int64_t b);
int64_t mulHighSignedUnsigned(int64_t a, uint64_t b);

} // namespace detail
} // namespace rvv
} // namespace Kites
