/**
 * @file vector_examples_test.cpp
 * @brief Runs the programs in examples/vector on the single-cycle processor and checks their
 * results against values computed here (validation level L5).
 */
#include "vector_test_support.h"

#include "config/config.h"

#include <cmath>
#include <cstring>

using namespace Kites;
using namespace vtest;

namespace
{

class Example
{
  public:
    explicit Example(const std::string &name)
    {
        QuietOutput quiet;
        setupVmStateDirectory();
        vm = std::make_unique<RVSSProcessor>();
        program = assemble("../examples/vector/" + name);
        vm->LoadProgram(program);
        while (vm->program_counter_ < vm->program_size_)
            vm->Step();
    }

    uint64_t address(const std::string &label) const
    {
        return vm_config::config.getDataSectionStart() + program.symbol_table.at(label).address;
    }

    uint64_t load(const std::string &label, size_t index, unsigned bytes) const
    {
        uint64_t v = 0;
        for (unsigned b = 0; b < bytes; ++b)
            v |= static_cast<uint64_t>(vm->memory_controller_.peekByte(address(label) + index * bytes + b))
                 << (8 * b);
        return v;
    }

    int32_t word(const std::string &label, size_t index) const
    {
        return static_cast<int32_t>(load(label, index, 4));
    }

    uint64_t x(unsigned reg) const { return vm->registers_.ReadGpr(reg); }

    std::unique_ptr<RVSSProcessor> vm;
    AssembledProgram program;
};

} // namespace

TEST(VecExamples, vvadd_i32)
{
    Example e("vvadd_i32.s");
    for (int i = 0; i < 13; ++i)
        EXPECT_EQ(e.word("c", i), 11 * (i + 1)) << i;
}

TEST(VecExamples, memcpy)
{
    Example e("memcpy.s");
    const std::string text = "Vectors make memcpy short and simple.";
    for (size_t i = 0; i <= text.size(); ++i)
        EXPECT_EQ(e.load("dst", i, 1), i < text.size() ? static_cast<uint8_t>(text[i]) : 0U) << i;
}

TEST(VecExamples, dot_product)
{
    Example e("dot_product.s");
    EXPECT_EQ(e.x(8), 770U); // s0
}

TEST(VecExamples, masked_abs)
{
    Example e("masked_abs.s");
    const int32_t expected[] = {5, 3, 0, 7, 2, 9, 4, 1};
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(e.word("values", i), expected[i]) << i;
}

TEST(VecExamples, gather_scatter)
{
    Example e("gather_scatter.s");
    const int32_t out[] = {80, 10, 40, 40, 70, 0, 20, 50};
    const int32_t rev[] = {50, 20, 0, 70, 40, 40, 10, 80};
    for (int i = 0; i < 8; ++i)
    {
        EXPECT_EQ(e.word("out", i), out[i]) << i;
        EXPECT_EQ(e.word("rev", i), rev[i]) << i;
    }
}

TEST(VecExamples, prefix_sum)
{
    Example e("prefix_sum.s");
    int32_t sum = 0;
    for (int i = 0; i < 8; ++i)
    {
        sum += i + 1;
        EXPECT_EQ(e.word("out", i), sum) << i;
    }
}

TEST(VecExamples, matmul_vector_matches_scalar)
{
    Example e("matmul_i32.s");
    const int32_t a[4][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}};
    const int32_t b[4][4] = {{1, 0, 2, -1}, {3, 1, 0, 2}, {-2, 4, 1, 0}, {0, -3, 2, 5}};
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            int32_t c = 0;
            for (int k = 0; k < 4; ++k)
                c += a[i][k] * b[k][j];
            EXPECT_EQ(e.word("cv", i * 4 + j), c) << i << "," << j;
            EXPECT_EQ(e.word("cs", i * 4 + j), c) << i << "," << j;
        }
    }
}

TEST(VecExamples, saxpy_f32)
{
    Example e("saxpy_f32.s");
    const float x[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const float y[] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, -1, -1, -1, -1, -1};
    for (int i = 0; i < 10; ++i)
    {
        const float expected = std::fma(2.5f, x[i], y[i]);
        uint32_t bits;
        std::memcpy(&bits, &expected, 4);
        EXPECT_EQ(e.load("y", i, 4), bits) << i;
    }
}

TEST(VecExamples, strlen_fault_only_first)
{
    Example e("strlen.s");
    EXPECT_EQ(e.x(10), 43U); // a0
}

TEST(VecExamples, rgb_to_grey_segment_loads)
{
    Example e("rgb_to_grey.s");
    const unsigned rgb[] = {255, 0,   0,   0,   255, 0,  0,   0,   255, 255,
                            255, 255, 0,   0,   0,   128, 128, 128, 200, 100,
                            50,  10,  20,  30,  90,  180, 45,  33,  66,  99};
    for (int i = 0; i < 10; ++i)
    {
        const unsigned grey =
            ((77 * rgb[3 * i] + 150 * rgb[3 * i + 1] + 29 * rgb[3 * i + 2]) & 0xFFFF) >> 8;
        EXPECT_EQ(e.load("grey", i, 1), grey) << i;
    }
}

TEST(VecExamples, fir_q15_fixed_point)
{
    Example e("fir_q15.s");
    const int32_t h[] = {16384, -8192, 4096, 32767};
    const int32_t x[] = {1000, -2000, 3000, 32767, -32768, 12345, -12345, 0, 500, 600, 700};
    bool saturated = false;
    auto sat16 = [&](int64_t v)
    {
        if (v > 32767 || v < -32768)
        {
            saturated = true;
            return static_cast<int32_t>(v > 0 ? 32767 : -32768);
        }
        return static_cast<int32_t>(v);
    };
    for (int i = 0; i < 8; ++i)
    {
        int32_t acc = 0;
        for (int k = 0; k < 4; ++k)
        {
            // vsmul with rnu: (a * b + 2^14) >> 15, saturating; then a saturating add.
            const int64_t product = (static_cast<int64_t>(h[k]) * x[i + k] + (1 << 14)) >> 15;
            acc = sat16(static_cast<int64_t>(acc) + sat16(product));
        }
        EXPECT_EQ(static_cast<int16_t>(e.load("y", i, 2)), acc) << i;
    }
    EXPECT_EQ(e.vm->registers_.ReadCsr(csr_address::kVxsat), saturated ? 1U : 0U);
}
