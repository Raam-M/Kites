/**
 * @file vector_isa.h
 * @brief Table-driven description of the RVV 1.0 instruction set.
 *
 * The table is the single source of truth for vector encodings. The assembler, the disassembler
 * and the vector unit all look instructions up here. Encodings are generated from riscv-opcodes
 * (see tools/vector/gen_vector_isa.py); segment load/store variants are derived from their base
 * unit-stride, strided and indexed forms.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Kites
{
namespace rvv
{

/// Variable operand fields of a vector instruction word.
enum FieldBits : uint32_t
{
    kVd = 1U << 0,      ///< bits 11..7, vector destination
    kVs3 = 1U << 1,     ///< bits 11..7, vector store data
    kRd = 1U << 2,      ///< bits 11..7, scalar destination
    kVs1 = 1U << 3,     ///< bits 19..15
    kVs2 = 1U << 4,     ///< bits 24..20
    kRs1 = 1U << 5,     ///< bits 19..15
    kRs2 = 1U << 6,     ///< bits 24..20
    kVm = 1U << 7,      ///< bit 25, 0 = masked by v0.t
    kNf = 1U << 8,      ///< bits 31..29
    kSimm5 = 1U << 9,   ///< bits 19..15, sign-extended immediate
    kZimm5 = 1U << 10,  ///< bits 19..15, zero-extended immediate
    kZimm10 = 1U << 11, ///< bits 29..20, vsetivli vtype immediate
    kZimm11 = 1U << 12, ///< bits 30..20, vsetvli vtype immediate
};

/// Kind of an assembly operand.
enum class OperandKind
{
    VectorRegister, ///< v0..v31
    IntRegister,    ///< x0..x31
    FloatRegister,  ///< f0..f31
    Memory,         ///< (rs1)
    Immediate,      ///< simm5 / zimm5
    VtypeImmediate, ///< e32, m1, ta, ma
    MaskV0,         ///< literal v0 (carry-in / merge selector)
    OptionalMask,   ///< optional ", v0.t"
};

struct Operand
{
    OperandKind kind;
    uint32_t field; ///< one FieldBits value
};

/// Major opcodes used by vector instructions.
inline constexpr uint32_t kOpcodeOpV = 0x57;
inline constexpr uint32_t kOpcodeLoadFp = 0x07;
inline constexpr uint32_t kOpcodeStoreFp = 0x27;

/// OP-V funct3 operand categories.
enum Funct3 : uint32_t
{
    kOpIVV = 0,
    kOpFVV = 1,
    kOpMVV = 2,
    kOpIVI = 3,
    kOpIVX = 4,
    kOpFVF = 5,
    kOpMVX = 6,
    kOpCfg = 7,
};

/**
 * @brief Static description of one vector instruction mnemonic.
 */
struct InsnInfo
{
    std::string mnemonic; ///< e.g. "vadd.vv", "vlseg3e8.v"
    std::string base;     ///< text before the first '.', e.g. "vadd"
    std::string suffix;   ///< text after the first '.', e.g. "vv"
    uint32_t match = 0;   ///< fixed bits
    uint32_t mask = 0;    ///< which bits are fixed
    uint32_t fields = 0;  ///< variable fields (FieldBits)
    std::vector<Operand> operands; ///< assembly operands in source order
    uint32_t opcode = 0;  ///< major opcode (bits 6..0)
    uint32_t funct3 = 0;  ///< bits 14..12 (OP-V category or memory width)
    unsigned nfields = 1; ///< segment field count (NFIELDS) for loads/stores

    [[nodiscard]] bool isConfig() const { return opcode == kOpcodeOpV && funct3 == kOpCfg; }
    [[nodiscard]] bool isLoad() const { return opcode == kOpcodeLoadFp; }
    [[nodiscard]] bool isStore() const { return opcode == kOpcodeStoreFp; }
    [[nodiscard]] bool hasField(uint32_t f) const { return (fields & f) != 0; }
};

/// All vector instructions: the 375 base encodings plus the 252 segment variants.
const std::vector<InsnInfo> &instructionTable();

/// Looks up an instruction by mnemonic; nullptr if it is not a vector mnemonic.
const InsnInfo *findByMnemonic(std::string_view mnemonic);

/// True if the mnemonic is a vector instruction.
bool isVectorMnemonic(std::string_view mnemonic);

/// True if the word lies in the vector encoding space (OP-V, or LOAD-FP/STORE-FP with a vector
/// width). Reserved encodings in that space also return true so they can be rejected.
bool isVectorInstruction(uint32_t word);

/// Decodes a word; nullptr if it is not a defined vector instruction.
const InsnInfo *decode(uint32_t word);

/// Extracts a field (one FieldBits value) from an instruction word.
uint32_t getField(uint32_t word, uint32_t field);

/// Returns the word with the given field set to value (value is truncated to the field width).
uint32_t setField(uint32_t word, uint32_t field, uint32_t value);

/// Disassembles a vector instruction. Registers use x/f/v numbering.
std::string disassemble(uint32_t word);

/// Expected assembly syntax, e.g. "vadd.vv vd, vs2, vs1[, v0.t]".
std::string expectedSyntax(const InsnInfo &info);

/// Standard vector pseudo-instructions accepted by the assembler (vneg.v, vmsge.vx, ...).
const std::vector<std::string> &pseudoMnemonics();
bool isVectorPseudo(std::string_view mnemonic);
/// Expected syntax of a pseudo-instruction; empty if the mnemonic is not one.
std::string pseudoSyntax(std::string_view mnemonic);

/// Inclusive immediate range accepted for an immediate field.
void immediateRange(const InsnInfo &info, uint32_t field, int64_t &min, int64_t &max);

} // namespace rvv
} // namespace Kites
