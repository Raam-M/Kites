/**
 * @file vector_test_support.h
 * @brief Helpers for RVV tests: run short programs on the single-cycle processor with a
 * prepared register state, and inspect vector registers as byte images.
 */
#pragma once

#include "assembler/assembler.h"
#include "processor/registers.h"
#include "processor/rvss/rvss_processor.h"
#include "processor/vector/vector_config.h"
#include "utils/utils.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

namespace vtest
{

constexpr unsigned kVlen = 128;
constexpr unsigned kVlenb = kVlen / 8;

/// Silences std::cout while alive (the simulator prints every step).
class QuietOutput
{
  public:
    QuietOutput() : previous_(std::cout.rdbuf(&sink_)) {}
    ~QuietOutput() { std::cout.rdbuf(previous_); }

  private:
    class NullBuffer : public std::streambuf
    {
      protected:
        int overflow(int c) override { return c; }
    };
    NullBuffer sink_;
    std::streambuf *previous_;
};

/// A single-cycle processor loaded with a program.
class Machine
{
  public:
    explicit Machine(const std::string &source)
    {
        QuietOutput quiet;
        Kites::setupVmStateDirectory();
        vm = std::make_unique<Kites::RVSSProcessor>();
        std::istringstream in(source);
        vm->LoadProgram(Kites::assemble(in));
    }

    /// Runs to the end of the program (exceptions propagate).
    void run()
    {
        QuietOutput quiet;
        while (vm->program_counter_ < vm->program_size_)
        {
            vm->Step();
        }
    }

    /// Executes one instruction.
    void step()
    {
        QuietOutput quiet;
        vm->Step();
    }

    /// Writes consecutive registers starting at reg from a byte image.
    void setV(unsigned reg, const std::vector<uint8_t> &bytes)
    {
        for (size_t r = 0; r * kVlenb < bytes.size(); ++r)
        {
            std::vector<uint8_t> one(bytes.begin() + static_cast<std::ptrdiff_t>(r * kVlenb),
                                     bytes.begin() + static_cast<std::ptrdiff_t>((r + 1) * kVlenb));
            vm->registers_.WriteVr(reg + r, one);
        }
    }

    /// Reads count consecutive registers starting at reg as one byte image.
    [[nodiscard]] std::vector<uint8_t> getV(unsigned reg, unsigned count = 1) const
    {
        std::vector<uint8_t> out;
        for (unsigned r = 0; r < count; ++r)
        {
            const auto one = vm->registers_.ReadVr(reg + r);
            out.insert(out.end(), one.begin(), one.end());
        }
        return out;
    }

    void setX(unsigned reg, uint64_t value) { vm->registers_.WriteGpr(reg, value); }
    [[nodiscard]] uint64_t x(unsigned reg) const { return vm->registers_.ReadGpr(reg); }
    void setF(unsigned reg, uint64_t value) { vm->registers_.WriteFpr(reg, value); }
    [[nodiscard]] uint64_t f(unsigned reg) const { return vm->registers_.ReadFpr(reg); }
    void setCsr(size_t address, uint64_t value) { vm->registers_.WriteCsr(address, value); }
    [[nodiscard]] uint64_t csr(size_t address) const { return vm->registers_.ReadCsr(address); }

    std::unique_ptr<Kites::RVSSProcessor> vm;
};

// ---- byte-image element helpers (element 0 in the lowest bytes)

inline uint64_t elem(const std::vector<uint8_t> &img, size_t index, unsigned eew)
{
    uint64_t v = 0;
    for (unsigned b = 0; b < eew / 8; ++b)
        v |= static_cast<uint64_t>(img.at(index * (eew / 8) + b)) << (8 * b);
    return v;
}

inline void setElem(std::vector<uint8_t> &img, size_t index, unsigned eew, uint64_t value)
{
    for (unsigned b = 0; b < eew / 8; ++b)
        img.at(index * (eew / 8) + b) = static_cast<uint8_t>(value >> (8 * b));
}

inline bool bit(const std::vector<uint8_t> &img, size_t index)
{
    return ((img.at(index / 8) >> (index % 8)) & 1U) != 0;
}

inline void setBit(std::vector<uint8_t> &img, size_t index, bool value)
{
    if (value)
        img.at(index / 8) |= static_cast<uint8_t>(1U << (index % 8));
    else
        img.at(index / 8) &= static_cast<uint8_t>(~(1U << (index % 8)));
}

inline uint64_t truncate(uint64_t v, unsigned bits)
{
    return bits >= 64 ? v : v & ((1ULL << bits) - 1);
}

inline int64_t sext(uint64_t v, unsigned bits)
{
    if (bits >= 64)
        return static_cast<int64_t>(v);
    const uint64_t sign = 1ULL << (bits - 1);
    v = truncate(v, bits);
    return static_cast<int64_t>((v ^ sign) - sign);
}

/// Random bytes with a bias towards edge values (0, all ones, sign boundaries).
inline std::vector<uint8_t> randomBytes(std::mt19937_64 &rng, size_t count)
{
    std::vector<uint8_t> out(count);
    for (auto &b : out)
    {
        const unsigned pick = static_cast<unsigned>(rng() % 8);
        if (pick == 0)
            b = 0x00;
        else if (pick == 1)
            b = 0xFF;
        else if (pick == 2)
            b = 0x80;
        else if (pick == 3)
            b = 0x7F;
        else
            b = static_cast<uint8_t>(rng());
    }
    return out;
}

/// "mf8", "mf4", "mf2", "m1", "m2", "m4", "m8"
inline std::string lmulName(int lmulLog2)
{
    if (lmulLog2 < 0)
        return "mf" + std::to_string(1 << -lmulLog2);
    return "m" + std::to_string(1 << lmulLog2);
}

inline uint64_t vlmaxOf(unsigned sew, int lmulLog2)
{
    const uint64_t perRegister = kVlen / sew;
    return lmulLog2 >= 0 ? perRegister << lmulLog2 : perRegister >> -lmulLog2;
}

/// Legal (SEW, log2 LMUL) pairs for ELEN = 64: SEW <= LMUL * ELEN.
inline std::vector<std::pair<unsigned, int>> legalConfigs()
{
    std::vector<std::pair<unsigned, int>> out;
    for (unsigned sew : {8U, 16U, 32U, 64U})
        for (int l = -3; l <= 3; ++l)
            if (l >= 0 || sew * (1U << -l) <= 64)
                out.emplace_back(sew, l);
    return out;
}

/// Registers a group of the given EMUL occupies (fractional EMUL occupies one).
inline unsigned groupRegs(int emulLog2)
{
    return emulLog2 > 0 ? 1U << emulLog2 : 1U;
}

} // namespace vtest
