/**
 * @file vector_unit.h
 * @brief Executes RVV 1.0 instructions against the architectural state of a processor.
 *
 * Every vector instruction executes atomically: all legality checks run first, results are
 * staged, and architectural state (vector/scalar registers, CSRs, memory) is only modified once
 * the whole instruction has succeeded. Each change is recorded in the step's StepDelta so it can
 * be undone and redone.
 */
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace Kites
{
class RegisterFile;
class MemoryController;
struct StepDelta;

namespace rvv
{

class VectorProfiler;

struct InsnInfo;

/// Raised for illegal or reserved vector usage. Architectural state is left unchanged.
class IllegalInstruction : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/// Raised when a vector memory access targets an address outside memory.
class AccessFault : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/// State a vector instruction reads and writes.
struct ExecutionContext
{
    RegisterFile &registers;
    MemoryController &memory;
    StepDelta &delta;                   ///< receives a record of every change
    VectorProfiler *profiler = nullptr; ///< optional: statistics and timing model
    uint64_t cycle = 0;                 ///< processor cycle, for the timing model
};

/**
 * @brief Executes one vector instruction.
 * @param word The 32-bit instruction (must satisfy isVectorInstruction()).
 * @throws IllegalInstruction for reserved encodings or illegal operand/vtype combinations.
 * @throws AccessFault for out-of-range memory accesses.
 */
void execute(uint32_t word, ExecutionContext &context);

/// True if the vector unit implements this instruction (the assembler rejects the rest).
bool isImplemented(const InsnInfo &info);

} // namespace rvv
} // namespace Kites
