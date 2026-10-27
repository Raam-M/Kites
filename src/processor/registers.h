/**
 * @file registers.h
 * @brief Contains the definition of the RegisterFile class for managing registers in the simulator.
 * @author Vishank Singh, https://github.com/VishankSingh
 */

#ifndef REGISTERS_H
#define REGISTERS_H

#include <QObject>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Kites
{
/**
 * @brief Represents a register file containing integer, floating-point, and vector registers.
 */
class RegisterFile : public QObject
{
    Q_OBJECT
  private:
    static constexpr size_t NUM_GPR = 32; ///< Number of General-Purpose Registers (GPR).
    static constexpr size_t NUM_FPR = 32; ///< Number of Floating-Point Registers (FPR).

    std::array<uint64_t, NUM_GPR> gpr_ = {}; ///< Array for storing GPR values.
    std::array<uint64_t, NUM_FPR> fpr_ = {}; ///< Array for storing FPR values.

    static constexpr size_t NUM_CSR = 4096; ///< Number of Control and Status Registers (CSR).

    std::array<uint64_t, NUM_CSR> csr_ = {}; ///< Array for storing CSR values.

    size_t vlenb_ = 16;          ///< Vector register length in bytes (VLEN / 8).
    std::vector<uint8_t> vr_;    ///< Vector registers v0..v31, vlenb_ bytes each, element 0 first.

  public:
    /**
     * @brief Enum representing the type of a register.
     */
    enum class RegisterType
    {
        INVALID,        ///< Invalid register type.
        INTEGER,        ///< General-purpose integer register.
        FLOATING_POINT, ///< Floating-point register.
        VECTOR,         ///< Vector register.
        CSR             ///< Control and Status Register (CSR).
    };

    explicit RegisterFile(QObject *parent = nullptr);
    virtual ~RegisterFile() = default;

    void Reset();

    /**
     * @brief Reads the value of a General-Purpose Register (GPR).
     * @param reg The index of the GPR to read.
     * @return The value of the GPR at the specified index.
     */
    [[nodiscard]] uint64_t ReadGpr(size_t reg) const;

    /**
     * @brief Writes a value to a General-Purpose Register (GPR).
     * @param reg The index of the GPR to write.
     * @param value The value to write.
     */
    void WriteGpr(size_t reg, uint64_t value);

    /**
     * @brief Reads the value of a Floating-Point Register (FPR).
     * @param reg The index of the FPR to read.
     * @return The value of the FPR at the specified index.
     */
    [[nodiscard]] uint64_t ReadFpr(size_t reg) const;

    /**
     * @brief Writes a value to a Floating-Point Register (FPR).
     * @param reg The index of the FPR to write.
     * @param value The value to write.
     */
    void WriteFpr(size_t reg, uint64_t value);

    [[nodiscard]] uint64_t ReadCsr(size_t reg) const;

    void WriteCsr(size_t reg, uint64_t value);

    /// Vector register length in bytes (VLEN / 8).
    [[nodiscard]] size_t GetVlenb() const;

    /**
     * @brief Reads the bytes of one vector register (byte 0 holds the low bits of element 0).
     * @param reg Vector register index (0..31).
     */
    [[nodiscard]] std::vector<uint8_t> ReadVr(size_t reg) const;

    /**
     * @brief Overwrites one vector register.
     * @param reg Vector register index (0..31).
     * @param bytes Exactly GetVlenb() bytes.
     */
    void WriteVr(size_t reg, const std::vector<uint8_t> &bytes);

    /// All vector registers as one contiguous byte array (v0 first).
    [[nodiscard]] const std::vector<uint8_t> &GetVrBytes() const;

    /**
     * @brief Retrieves the values of all General-Purpose Registers (GPR).
     * @return A vector containing the values of all GPRs.
     */
    [[nodiscard]] std::vector<uint64_t> GetGprValues() const;

    /**
     * @brief Retrieves the values of all Floating-Point Registers (FPR).
     * @return A vector containing the values of all FPRs.
     */
    [[nodiscard]] std::vector<uint64_t> GetFprValues() const;

    void ModifyRegister(const std::string &reg_name, uint64_t value);

  signals:
    void updateRegister(size_t regIndex, uint64_t value);
    void updateFRegister(size_t regIndex, uint64_t value);
    void updateVRegister(size_t regIndex);
    void updateCsr(size_t address, uint64_t value);
    void registerResetSignal();
};

extern const std::unordered_set<std::string> valid_general_purpose_registers;

extern const std::unordered_set<std::string> valid_floating_point_registers;

extern const std::unordered_set<std::string> valid_csr_registers;

extern const std::unordered_set<std::string> valid_vector_registers;

extern const std::unordered_map<std::string, int> csr_to_address;

/**
 * @brief Map of register aliases to their actual names.
 */
extern const std::unordered_map<std::string, std::string> reg_alias_to_name;

bool IsValidGeneralPurposeRegister(const std::string &reg);

bool IsValidFloatingPointRegister(const std::string &reg);

bool IsValidCsr(const std::string &reg);

bool IsValidVectorRegister(const std::string &reg);

/// CSR addresses whose two top bits are 11 are read-only (RISC-V CSR address convention).
inline bool IsReadOnlyCsr(size_t address)
{
    return ((address >> 10) & 0x3) == 0x3;
}

namespace csr_address
{
inline constexpr size_t kFflags = 0x001;
inline constexpr size_t kFrm = 0x002;
inline constexpr size_t kFcsr = 0x003;
inline constexpr size_t kVstart = 0x008;
inline constexpr size_t kVxsat = 0x009;
inline constexpr size_t kVxrm = 0x00A;
inline constexpr size_t kVcsr = 0x00F;
inline constexpr size_t kVl = 0xC20;
inline constexpr size_t kVtype = 0xC21;
inline constexpr size_t kVlenb = 0xC22;
} // namespace csr_address
}//namespace Kites
#endif // REGISTERS_H