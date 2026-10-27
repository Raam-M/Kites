/**
 * @file vector_memory_test.cpp
 * @brief Vector loads and stores against a byte-level reference (VM-01..VM-05, VP-01).
 *
 * Source data lives at kSrc, stores go to kDst. Data registers start at v8, index registers at
 * v16 and the mask is v0.
 */
#include "vector_test_support.h"

#include "processor/vector/vector_unit.h"

using namespace Kites;
using namespace vtest;
namespace csr = Kites::csr_address;

namespace
{

constexpr uint64_t kSrc = 0x10000000;
constexpr uint64_t kDst = 0x10002000;
constexpr size_t kRegion = 2048;

int log2u(unsigned v)
{
    int l = 0;
    while ((1U << l) < v)
        ++l;
    return l;
}

struct MemCase
{
    unsigned sew;
    int lmul;
    bool masked;
    uint64_t vl;
    std::vector<uint8_t> src, dst, data, index, mask;
};

class MemoryMachine
{
  public:
    MemoryMachine(const std::string &body, const MemCase &c, uint64_t avl, int64_t stride = 0)
        : m(".text\nvsetvli x0, x10, e" + std::to_string(c.sew) + ", " + lmulName(c.lmul) +
            ", tu, mu\n" + body + "\n")
    {
        for (size_t i = 0; i < kRegion; ++i)
        {
            m.vm->memory_controller_.writeByte_d(kSrc + i, c.src[i]);
            m.vm->memory_controller_.writeByte_d(kDst + i, c.dst[i]);
        }
        m.setV(0, c.mask);
        m.setV(8, c.data);
        m.setV(16, c.index);
        m.setX(10, avl);
        m.setX(12, kSrc + kRegion / 2); // base address (middle, so negative strides work)
        m.setX(13, kDst + kRegion / 2);
        m.setX(14, static_cast<uint64_t>(stride));
        m.run();
    }

    std::vector<uint8_t> memory(uint64_t base) const
    {
        std::vector<uint8_t> out(kRegion);
        for (size_t i = 0; i < kRegion; ++i)
            out[i] = m.vm->memory_controller_.peekByte(base + i);
        return out;
    }

    Machine m;
};

MemCase makeCase(std::mt19937_64 &rng, unsigned sew, int lmul, bool masked)
{
    MemCase c;
    c.sew = sew;
    c.lmul = lmul;
    c.masked = masked;
    c.src = randomBytes(rng, kRegion);
    c.dst = randomBytes(rng, kRegion);
    c.data = randomBytes(rng, 8 * kVlenb); // v8..v15
    c.index = randomBytes(rng, 8 * kVlenb);
    c.mask = randomBytes(rng, kVlenb);
    return c;
}

bool activeAt(const MemCase &c, size_t i)
{
    return !c.masked || bit(c.mask, i);
}

uint64_t readBytes(const std::vector<uint8_t> &mem, int64_t offset, unsigned bytes)
{
    uint64_t v = 0;
    for (unsigned b = 0; b < bytes; ++b)
        v |= static_cast<uint64_t>(mem.at(static_cast<size_t>(offset) + b)) << (8 * b);
    return v;
}

void writeBytes(std::vector<uint8_t> &mem, int64_t offset, unsigned bytes, uint64_t value)
{
    for (unsigned b = 0; b < bytes; ++b)
        mem.at(static_cast<size_t>(offset) + b) = static_cast<uint8_t>(value >> (8 * b));
}

std::string mask(bool masked)
{
    return masked ? ", v0.t" : "";
}

} // namespace

// VM-01, VM-02: unit-stride and strided loads and stores (with segments) for every legal
// SEW/LMUL/EEW combination.
TEST(VecMemory, VM01_VM02_unit_stride_and_strided)
{
    std::mt19937_64 rng(31);
    size_t cases = 0;
    for (const auto &[sew, lmul] : legalConfigs())
    {
        for (unsigned eew : {8U, 16U, 32U, 64U})
        {
            const int emul = log2u(eew) - log2u(sew) + lmul;
            if (emul < -3 || emul > 3)
                continue;
            const unsigned fieldRegs = groupRegs(emul);
            const unsigned bytes = eew / 8;
            for (unsigned nf : {1U, 2U, 3U})
            {
                if (nf * fieldRegs > 8)
                    continue;
                for (int mode = 0; mode < 5; ++mode) // 0 unit, 1..4 strided variants
                {
                    const bool masked = (rng() & 1) != 0;
                    MemCase c = makeCase(rng, sew, lmul, masked);
                    const uint64_t vlmax = vlmaxOf(sew, lmul);
                    const uint64_t avl = rng() % 2 ? vlmax : rng() % (vlmax + 1);
                    const uint64_t vl = std::min(avl, vlmax);
                    const int64_t strides[] = {0, static_cast<int64_t>(bytes * nf),
                                               static_cast<int64_t>(3 * bytes + 1),
                                               -static_cast<int64_t>(2 * bytes * nf), 0};
                    const int64_t stride = mode == 0 ? static_cast<int64_t>(bytes * nf)
                                                     : strides[mode == 4 ? 0 : mode];
                    const std::string seg = nf == 1 ? "" : "seg" + std::to_string(nf);
                    const std::string e = "e" + std::to_string(eew) + ".v";
                    std::string body;
                    if (mode == 0)
                        body = "vl" + seg + e + " v8, (x12)" + mask(masked) + "\nvs" + seg + e +
                               " v8, (x13)" + mask(masked);
                    else
                        body = "vl" + std::string(nf == 1 ? "s" : "s" + seg) + e +
                               " v8, (x12), x14" + mask(masked) + "\nvs" +
                               std::string(nf == 1 ? "s" : "s" + seg) + e + " v8, (x13), x14" +
                               mask(masked);
                    SCOPED_TRACE(body + "  e" + std::to_string(sew) + " " + lmulName(lmul) +
                                 " vl=" + std::to_string(vl) + " stride=" + std::to_string(stride));
                    MemoryMachine mm(body, c, avl, stride);

                    std::vector<uint8_t> expectedRegs = c.data;
                    std::vector<uint8_t> expectedDst = c.dst;
                    const int64_t middle = kRegion / 2;
                    for (size_t i = 0; i < vl; ++i)
                    {
                        if (!activeAt(c, i))
                            continue;
                        for (unsigned f = 0; f < nf; ++f)
                        {
                            const int64_t offset =
                                middle + static_cast<int64_t>(i) * stride + f * bytes;
                            const uint64_t v = readBytes(c.src, offset, bytes);
                            setElem(expectedRegs, f * fieldRegs * kVlenb / bytes + i, eew, v);
                        }
                    }
                    // Stores write the loaded registers back, element by element in order.
                    for (size_t i = 0; i < vl; ++i)
                    {
                        if (!activeAt(c, i))
                            continue;
                        for (unsigned f = 0; f < nf; ++f)
                        {
                            const int64_t offset =
                                middle + static_cast<int64_t>(i) * stride + f * bytes;
                            writeBytes(expectedDst, offset, bytes,
                                       elem(expectedRegs, f * fieldRegs * kVlenb / bytes + i, eew));
                        }
                    }
                    ASSERT_EQ(mm.m.getV(8, 8), expectedRegs);
                    ASSERT_EQ(mm.memory(kDst), expectedDst);
                    ++cases;
                }
            }
        }
    }
    EXPECT_GT(cases, 300U);
}

// VM-01: indexed loads/stores, ordered and unordered, every index EEW, with segments.
TEST(VecMemory, VM01_indexed)
{
    std::mt19937_64 rng(32);
    for (const auto &[sew, lmul] : legalConfigs())
    {
        for (unsigned ieew : {8U, 16U, 32U, 64U})
        {
            const int indexEmul = log2u(ieew) - log2u(sew) + lmul;
            if (indexEmul < -3 || indexEmul > 3)
                continue;
            const unsigned dataRegs = groupRegs(lmul);
            const unsigned bytes = sew / 8;
            for (unsigned nf : {1U, 2U})
            {
                if (nf * dataRegs > 8)
                    continue;
                for (const char *order : {"u", "o"})
                {
                    const bool masked = (rng() & 1) != 0;
                    MemCase c = makeCase(rng, sew, lmul, masked);
                    const uint64_t vlmax = vlmaxOf(sew, lmul);
                    const uint64_t vl = rng() % 2 ? vlmax : rng() % (vlmax + 1);
                    // Offsets within the region; ordered stores may repeat an offset.
                    for (size_t i = 0; i < vlmax; ++i)
                    {
                        const uint64_t offset = std::string(order) == "o" ? rng() % 64
                                                                          : i * bytes * nf + (rng() % 2);
                        setElem(c.index, i, ieew, truncate(offset, ieew));
                    }
                    const std::string seg = nf == 1 ? "" : "seg" + std::to_string(nf);
                    const std::string suffix = "ei" + std::to_string(ieew) + ".v";
                    const std::string body = std::string("vl") + order + "x" + seg + suffix +
                                             " v8, (x12), v16" + mask(masked) + "\nvs" + order +
                                             "x" + seg + suffix + " v8, (x13), v16" + mask(masked);
                    SCOPED_TRACE(body + "  e" + std::to_string(sew) + " " + lmulName(lmul) +
                                 " vl=" + std::to_string(vl));
                    MemoryMachine mm(body, c, vl);
                    std::vector<uint8_t> expectedRegs = c.data;
                    std::vector<uint8_t> expectedDst = c.dst;
                    const int64_t middle = kRegion / 2;
                    for (int pass = 0; pass < 2; ++pass)
                    {
                        for (size_t i = 0; i < vl; ++i)
                        {
                            if (!activeAt(c, i))
                                continue;
                            for (unsigned f = 0; f < nf; ++f)
                            {
                                const int64_t offset =
                                    middle + static_cast<int64_t>(elem(c.index, i, ieew)) + f * bytes;
                                const size_t element = f * dataRegs * kVlenb / bytes + i;
                                if (pass == 0)
                                    setElem(expectedRegs, element, sew, readBytes(c.src, offset, bytes));
                                else
                                    writeBytes(expectedDst, offset, bytes,
                                               elem(expectedRegs, element, sew));
                            }
                        }
                    }
                    ASSERT_EQ(mm.m.getV(8, 8), expectedRegs);
                    ASSERT_EQ(mm.memory(kDst), expectedDst);
                }
            }
        }
    }
}

// VM-03: mask loads/stores move ceil(vl/8) bytes; whole-register forms ignore vl and vtype.
TEST(VecMemory, VM03_mask_and_whole_register)
{
    std::mt19937_64 rng(33);
    for (const auto &[sew, lmul] : legalConfigs())
    {
        MemCase c = makeCase(rng, sew, lmul, false);
        const uint64_t vlmax = vlmaxOf(sew, lmul);
        const uint64_t vl = rng() % (vlmax + 1);
        SCOPED_TRACE("vlm/vsm e" + std::to_string(sew) + " " + lmulName(lmul) +
                     " vl=" + std::to_string(vl));
        MemoryMachine mm("vlm.v v8, (x12)\nvsm.v v8, (x13)", c, vl);
        std::vector<uint8_t> expectedRegs = c.data;
        std::vector<uint8_t> expectedDst = c.dst;
        for (size_t i = 0; i < (vl + 7) / 8; ++i)
        {
            expectedRegs[i] = c.src[kRegion / 2 + i];
            expectedDst[kRegion / 2 + i] = c.src[kRegion / 2 + i];
        }
        ASSERT_EQ(mm.m.getV(8, 8), expectedRegs);
        ASSERT_EQ(mm.memory(kDst), expectedDst);
    }
    for (unsigned count : {1U, 2U, 4U, 8U})
    {
        for (unsigned eew : {8U, 16U, 32U, 64U})
        {
            MemCase c = makeCase(rng, 8, 0, false);
            const std::string n = std::to_string(count);
            SCOPED_TRACE("vl" + n + "re" + std::to_string(eew));
            // vl = 1 to show that whole-register transfers ignore vl.
            MemoryMachine mm("vl" + n + "re" + std::to_string(eew) + ".v v8, (x12)\nvs" + n +
                                 "r.v v8, (x13)",
                             c, 1);
            std::vector<uint8_t> expectedRegs = c.data;
            std::vector<uint8_t> expectedDst = c.dst;
            for (size_t i = 0; i < count * kVlenb; ++i)
            {
                expectedRegs[i] = c.src[kRegion / 2 + i];
                expectedDst[kRegion / 2 + i] = c.src[kRegion / 2 + i];
            }
            ASSERT_EQ(mm.m.getV(8, 8), expectedRegs);
            ASSERT_EQ(mm.memory(kDst), expectedDst);
        }
    }
}

// VM-05: fault-only-first trims vl at a faulting element > 0 and traps on element 0.
TEST(VecMemory, VM05_fault_only_first)
{
    const uint64_t top = ~0ULL - 11; // elements 0 and 1 fit below 2^64, element 2 wraps
    {
        Machine m(".text\nvsetivli x0, 4, e32, m1, tu, mu\nvle32ff.v v8, (x12)\n");
        m.setX(12, top);
        m.run();
        EXPECT_EQ(m.csr(csr::kVl), 2U);
    }
    {
        Machine m(".text\nvsetivli x0, 4, e32, m1, tu, mu\nvle32.v v8, (x12)\n");
        m.setX(12, top);
        EXPECT_THROW(m.run(), rvv::AccessFault);
        EXPECT_EQ(m.csr(csr::kVl), 4U); // unchanged
    }
    {
        Machine m(".text\nvsetivli x0, 4, e32, m1, tu, mu\nvle32ff.v v8, (x12)\n");
        m.setX(12, ~0ULL - 1); // element 0 already faults
        EXPECT_THROW(m.run(), rvv::AccessFault);
    }
    {
        // No fault: vl is unchanged and data is loaded.
        Machine m(".text\nvsetivli x0, 3, e16, m1, tu, mu\nvle16ff.v v8, (x12)\n");
        m.setX(12, kSrc);
        for (int i = 0; i < 6; ++i)
            m.vm->memory_controller_.writeByte_d(kSrc + i, static_cast<uint8_t>(i + 1));
        m.run();
        EXPECT_EQ(m.csr(csr::kVl), 3U);
        EXPECT_EQ(elem(m.getV(8), 2, 16), 0x0605U);
    }
}

// VM-04: vector accesses go through the data cache, one access per element.
TEST(VecMemory, VM04_accesses_go_through_the_cache)
{
    Machine m(".text\nvsetivli x0, 4, e32, m1, tu, mu\nvle32.v v8, (x12)\nvse32.v v8, (x13)\n");
    m.setX(12, kSrc);
    m.setX(13, kDst);
    Kites::Cache *l1 = m.vm->memory_controller_.getL1Cache();
    const size_t before = l1->getHitCount() + l1->getMissCount();
    m.run();
    EXPECT_EQ(l1->getHitCount() + l1->getMissCount() - before, 8U);
}
