/**
 * @file vector_profiler.h
 * @brief Vector instruction statistics and an analytic timing model.
 *
 * Statistics: instructions per class and mnemonic, vector-length use, active and masked-off
 * elements, bytes moved, and SEW/LMUL use.
 *
 * Timing model: a decoupled vector unit that issues vector instructions in order, at most one
 * per cycle and no earlier than the cycle the scalar processor reached them. Each functional
 * unit class has one pipelined unit with `lanes` lanes. An instruction occupies its unit for
 * ceil(elements / lanes) cycles, and its first result appears after the unit's start-up
 * latency. A source register is available at the producer's first result with chaining, or
 * after its last result without. Convoys follow Hennessy & Patterson: consecutive instructions
 * form one convoy until one needs a unit already used in the convoy or (without chaining)
 * reads a register written in the convoy; the number of convoys is the chime count. The model
 * gives an estimate, not a cycle-accurate simulation: scalar/vector register dependencies and
 * memory bank conflicts are not modelled.
 */
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>

namespace Kites
{
namespace rvv
{

struct InsnInfo;

/// Functional unit classes of the timing model.
enum class Unit : uint8_t
{
    Config,  ///< vset{i}vl{i}: no unit
    Alu,     ///< integer and fixed-point arithmetic, compares, merges, bit manipulation
    Mul,     ///< integer multiply, multiply-add, vsmul, carry-less multiply
    Div,     ///< integer divide and remainder
    FpAdd,   ///< FP add/sub/min/max/sign/compare/convert/classify/estimates/moves
    FpMul,   ///< FP multiply and fused multiply-add
    FpDiv,   ///< FP divide and square root
    Load,    ///< vector loads
    Store,   ///< vector stores
    Reduce,  ///< integer and FP reductions
    Permute, ///< slides, gathers, compress, scalar moves, whole-register moves
    Mask,    ///< mask logical operations, vcpop/vfirst, vmsbf/vmsif/vmsof, viota, vid
};
inline constexpr size_t kUnitCount = 12;
std::string unitName(Unit unit);

/// Instruction classes reported by the statistics.
enum class InsnClass : uint8_t
{
    Config,
    Load,
    Store,
    Integer,
    FixedPoint,
    Float,
    Reduction,
    Mask,
    Permutation,
    BitManip,
};
inline constexpr size_t kClassCount = 10;
std::string className(InsnClass c);

InsnClass classify(const InsnInfo &info);
Unit unitOf(const InsnInfo &info);

/// Parameters of the timing model.
struct TimingConfig
{
    unsigned lanes = 4;    ///< elements each unit processes per cycle (1..32, power of two)
    bool chaining = true;  ///< dependent instructions may start at the producer's first result
    /// Start-up latency per unit (cycles to the first result), indexed by Unit. Defaults follow
    /// the VMIPS example of Hennessy & Patterson: add 6, multiply 7, divide 20, load/store 12.
    std::array<unsigned, kUnitCount> latency{1, 6, 7, 20, 6, 7, 20, 12, 12, 6, 6, 6};
};

TimingConfig &timingConfig();
bool isSupportedLaneCount(unsigned lanes);

/// What one executed vector instruction did; filled by the vector unit.
struct InstructionRecord
{
    const InsnInfo *info = nullptr;
    unsigned sew = 0;
    int lmulLog2 = 0;
    uint64_t vl = 0;
    uint64_t vlmax = 0;
    uint64_t vstart = 0;
    uint64_t bodyElements = 0;     ///< elements the instruction processes (for timing)
    uint64_t activeElements = 0;   ///< body elements not masked off
    uint64_t inactiveElements = 0; ///< body elements masked off
    uint64_t bytesLoaded = 0;
    uint64_t bytesStored = 0;
    bool unitStride = true;        ///< memory: contiguous access (lanes elements per cycle)
    uint32_t registersRead = 0;    ///< bit r set if vector register r was read
    uint32_t registersWritten = 0; ///< bit r set if vector register r was written
    uint64_t issueCycle = 0;       ///< processor cycle at which the instruction executed
};

struct VectorStatistics
{
    uint64_t instructions = 0;
    std::array<uint64_t, kClassCount> byClass{};
    std::map<std::string, uint64_t> byMnemonic;
    std::array<uint64_t, 4> bySew{};  ///< SEW 8, 16, 32, 64
    std::array<uint64_t, 7> byLmul{}; ///< LMUL 1/8 .. 8
    uint64_t vlSum = 0;               ///< over instructions that use vl
    uint64_t vlmaxSum = 0;
    uint64_t vlInstructions = 0;
    uint64_t activeElements = 0;
    uint64_t inactiveElements = 0;
    uint64_t bytesLoaded = 0;
    uint64_t bytesStored = 0;

    [[nodiscard]] double averageVl() const;
    /// Average vl / VLMAX over instructions that use vl.
    [[nodiscard]] double utilisation() const;
};

struct TimingState
{
    uint64_t lastStart = 0;
    uint64_t finish = 0; ///< cycle after the last result of any instruction
    uint64_t convoys = 0;
    uint32_t convoyUnits = 0;  ///< bit per Unit used in the current convoy
    uint32_t convoyWrites = 0; ///< vector registers written in the current convoy
    std::array<uint64_t, 32> firstReady{}; ///< per vector register
    std::array<uint64_t, 32> allReady{};
    std::array<uint64_t, kUnitCount> unitFree{};
    std::array<uint64_t, kUnitCount> unitBusy{}; ///< total busy cycles per unit
    uint64_t firstIssue = 0; ///< processor cycle of the first vector instruction
    bool started = false;
};

/// Complete profiler state (copied into step deltas so undo can restore it).
struct ProfilerState
{
    VectorStatistics stats;
    TimingState timing;
};

class VectorProfiler
{
  public:
    void record(const InstructionRecord &r);
    void reset() { state_ = {}; }

    [[nodiscard]] const ProfilerState &state() const { return state_; }
    void restore(const ProfilerState &s) { state_ = s; }

    [[nodiscard]] const VectorStatistics &statistics() const { return state_.stats; }
    /// Estimated cycles from the first vector instruction to the last vector result.
    [[nodiscard]] uint64_t estimatedCycles() const;

    /// Multi-line text report (used by the CLI and the GUI).
    [[nodiscard]] std::string report() const;
    /// JSON object with the statistics and timing estimate.
    [[nodiscard]] std::string toJson() const;

  private:
    ProfilerState state_;
};

} // namespace rvv
} // namespace Kites
