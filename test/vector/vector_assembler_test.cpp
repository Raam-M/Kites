/**
 * @file vector_assembler_test.cpp
 * @brief Encoding, disassembly and parsing tests for the RVV assembler support.
 *
 * Golden encodings come from LLVM (test/vector/golden/encodings.txt, produced by
 * tools/vector/gen_golden_encodings.py), so encoder bugs are caught against an external oracle.
 */
#include "vector_test_support.h"

#include "common/instructions.h"
#include "processor/vector/vector_isa.h"
#include "processor/vector/vector_unit.h"
#include "processor/vector/vtype.h"

#include <fstream>
#include <set>

using namespace Kites;

namespace
{

struct Golden
{
    std::string text;
    uint32_t word;
};

std::vector<Golden> loadGolden()
{
    std::vector<Golden> out;
    std::ifstream in("vector/golden/encodings.txt");
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        const size_t bar = line.rfind('|');
        out.push_back({line.substr(0, bar),
                       static_cast<uint32_t>(std::stoul(line.substr(bar + 1), nullptr, 16))});
    }
    return out;
}

/// Assembles a single instruction; returns false if assembly failed.
bool assembleOne(const std::string &text, uint32_t &word)
{
    vtest::QuietOutput quiet;
    std::istringstream in(".text\n" + text + "\n");
    try
    {
        const AssembledProgram program = assemble(in);
        if (program.text_buffer.size() != 1)
            return false;
        word = program.text_buffer[0];
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool assemblyFails(const std::string &source)
{
    vtest::QuietOutput quiet;
    std::istringstream in(source);
    try
    {
        assemble(in);
        return false;
    }
    catch (const std::exception &)
    {
        return true;
    }
}

} // namespace

// AS-01: every mnemonic in every golden operand form encodes exactly as LLVM does.
TEST(VecAsm, AS01_golden_encodings_match_llvm)
{
    const std::vector<Golden> golden = loadGolden();
    ASSERT_GT(golden.size(), 1000U) << "golden file not found or empty";
    size_t checked = 0;
    for (const Golden &g : golden)
    {
        const std::string mnemonic = g.text.substr(0, g.text.find(' '));
        const rvv::InsnInfo *info = rvv::findByMnemonic(mnemonic);
        ASSERT_NE(info, nullptr) << mnemonic;
        if (!rvv::isImplemented(*info))
            continue; // checked by decode test below until implemented
        uint32_t word = 0;
        ASSERT_TRUE(assembleOne(g.text, word)) << "failed to assemble: " << g.text;
        EXPECT_EQ(word, g.word) << g.text << std::hex << " got 0x" << word << " want 0x"
                                << g.word;
        ++checked;
    }
    EXPECT_GT(checked, 600U);
}

// AS-01: decoding each golden word yields the mnemonic it was written with.
TEST(VecAsm, AS01_golden_words_decode_to_their_mnemonic)
{
    for (const Golden &g : loadGolden())
    {
        const std::string mnemonic = g.text.substr(0, g.text.find(' '));
        ASSERT_TRUE(rvv::isVectorInstruction(g.word)) << g.text;
        const rvv::InsnInfo *info = rvv::decode(g.word);
        ASSERT_NE(info, nullptr) << g.text;
        EXPECT_EQ(info->mnemonic, mnemonic) << g.text;
    }
}

// AS-02: disassembling a golden word and assembling the text gives back the same word.
TEST(VecAsm, AS02_disassembly_round_trip)
{
    for (const Golden &g : loadGolden())
    {
        const rvv::InsnInfo *info = rvv::decode(g.word);
        ASSERT_NE(info, nullptr);
        if (!rvv::isImplemented(*info))
            continue;
        const std::string text = instruction_set::disassemble(g.word);
        uint32_t word = 0;
        ASSERT_TRUE(assembleOne(text, word)) << "cannot reassemble '" << text << "'";
        EXPECT_EQ(word, g.word) << text;
    }
}

// Every table entry is covered by the golden file, and the table has the expected size.
TEST(VecAsm, table_matches_spec_inventory)
{
    const auto &table = rvv::instructionTable();
    EXPECT_EQ(table.size(), 627U); // 375 base + 252 segment variants
    std::set<std::string> covered;
    for (const Golden &g : loadGolden())
        covered.insert(g.text.substr(0, g.text.find(' ')));
    for (const rvv::InsnInfo &info : table)
    {
        EXPECT_TRUE(covered.count(info.mnemonic)) << info.mnemonic << " has no golden encoding";
        // match must decode back to the same entry (no ambiguous encodings).
        EXPECT_EQ(rvv::decode(info.match), &info) << info.mnemonic;
        EXPECT_EQ(info.match & ~info.mask, 0U) << info.mnemonic;
    }
}

TEST(VecAsm, scalar_fp_loads_are_not_vector)
{
    // flw/fld/fsw/fsd share LOAD-FP/STORE-FP with vector loads/stores.
    uint32_t word = 0;
    ASSERT_TRUE(assembleOne("flw f1, 0(x10)", word));
    EXPECT_FALSE(rvv::isVectorInstruction(word));
    ASSERT_TRUE(assembleOne("fsd f1, 8(x10)", word));
    EXPECT_FALSE(rvv::isVectorInstruction(word));
    ASSERT_TRUE(assembleOne("vle32.v v1, (x10)", word));
    EXPECT_TRUE(rvv::isVectorInstruction(word));
}

// AS-03: malformed vector instructions are rejected.
TEST(VecAsm, AS03_syntax_errors_are_reported)
{
    EXPECT_TRUE(assemblyFails(".text\nvadd.vv v1, v2\n"));                 // missing operand
    EXPECT_TRUE(assemblyFails(".text\nvadd.vv v1, v2, x3\n"));             // wrong register class
    EXPECT_TRUE(assemblyFails(".text\nvadd.vv v1, v2, v3, v1.t\n"));       // bad mask operand
    EXPECT_TRUE(assemblyFails(".text\nvadd.vi v1, v2, 16\n"));             // simm5 out of range
    EXPECT_TRUE(assemblyFails(".text\nvadd.vi v1, v2, -17\n"));            // simm5 out of range
    EXPECT_TRUE(assemblyFails(".text\nvsll.vi v1, v2, 32\n"));             // uimm5 out of range
    EXPECT_TRUE(assemblyFails(".text\nvsll.vi v1, v2, -1\n"));             // uimm5 out of range
    EXPECT_TRUE(assemblyFails(".text\nvle32.v v1, x10\n"));                // missing parentheses
    EXPECT_TRUE(assemblyFails(".text\nvsetvli t0, a0, m1, e32\n"));        // wrong field order
    EXPECT_TRUE(assemblyFails(".text\nvsetvli t0, a0, e128, m1\n"));       // unknown SEW
    EXPECT_TRUE(assemblyFails(".text\nvsetivli t0, 32, e8, m1\n"));        // AVL > 31
    EXPECT_TRUE(assemblyFails(".text\nvadc.vvm v1, v2, v3\n"));            // missing v0
    EXPECT_TRUE(assemblyFails(".text\nvadc.vvm v1, v2, v3, v4\n"));        // carry must be v0
    EXPECT_TRUE(assemblyFails(".text\nvmv.v.v v1, v2, v0.t\n"));           // not maskable
}

TEST(VecAsm, vtype_defaults_and_numeric_form)
{
    uint32_t a = 0;
    uint32_t b = 0;
    // LMUL defaults to m1 and the policies to tu, mu.
    ASSERT_TRUE(assembleOne("vsetvli t0, a0, e32", a));
    ASSERT_TRUE(assembleOne("vsetvli t0, a0, e32, m1, tu, mu", b));
    EXPECT_EQ(a, b);
    // A raw vtype immediate is accepted too.
    ASSERT_TRUE(assembleOne("vsetvli t0, a0, 208", a)); // 0xd0 = e32, m1, ta, ma
    ASSERT_TRUE(assembleOne("vsetvli t0, a0, e32, m1, ta, ma", b));
    EXPECT_EQ(a, b);
    // "0(rs1)" is accepted for vector memory operands.
    ASSERT_TRUE(assembleOne("vle8.v v1, 0(a0)", a));
    ASSERT_TRUE(assembleOne("vle8.v v1, (a0)", b));
    EXPECT_EQ(a, b);
}

// AS-04: labels named like vtype keywords still work outside vsetvli/vsetivli.
TEST(VecAsm, AS04_labels_named_like_vtype_keywords)
{
    vtest::QuietOutput quiet;
    std::istringstream in(".text\n"
                          "ta:\n"
                          "    addi x1, x1, 1\n"
                          "    vsetvli t0, a0, e8, m1, ta, ma\n"
                          "    beq x0, x1, ta\n"
                          "e32:\n"
                          "    j e32\n");
    EXPECT_NO_THROW(assemble(in));
}

TEST(VecAsm, csr_pseudo_instructions)
{
    uint32_t a = 0;
    uint32_t b = 0;
    ASSERT_TRUE(assembleOne("csrr t0, vlenb", a));
    ASSERT_TRUE(assembleOne("csrrs t0, vlenb, x0", b));
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, 0xc22022f3U); // llvm-mc: csrr t0, vlenb
    ASSERT_TRUE(assembleOne("csrw vxrm, a0", a));
    EXPECT_EQ(a, 0x00a51073U); // csrrw x0, vxrm, a0
    ASSERT_TRUE(assembleOne("csrwi vxrm, 2", a));
    EXPECT_EQ(a, 0x00a15073U); // csrrwi x0, vxrm, 2
    ASSERT_TRUE(assembleOne("csrs vcsr, a1", a));
    EXPECT_EQ(a, 0x00f5a073U); // csrrs x0, vcsr, a1
    EXPECT_TRUE(assemblyFails(".text\ncsrwi vxrm, 32\n"));
}

TEST(VecAsm, vector_instructions_are_classified_for_the_profiler)
{
    EXPECT_EQ(instruction_set::getInstructionType("vadd.vv"),
              instruction_set::InstructionType::V_TYPE);
    EXPECT_EQ(instruction_set::getInstructionType("vlseg2e8.v"),
              instruction_set::InstructionType::V_TYPE);
    EXPECT_EQ(instruction_set::getInstructionType("add"), instruction_set::InstructionType::R_TYPE);
}

TEST(VecAsm, expected_syntax_text)
{
    EXPECT_EQ(instruction_set::getExpectedSyntaxes("vadd.vv"), "vadd.vv vd, vs2, vs1[, v0.t]");
    EXPECT_EQ(instruction_set::getExpectedSyntaxes("vmacc.vx"), "vmacc.vx vd, rs1, vs2[, v0.t]");
    EXPECT_EQ(instruction_set::getExpectedSyntaxes("vlse32.v"), "vlse32.v vd, (rs1), rs2[, v0.t]");
    EXPECT_EQ(instruction_set::getExpectedSyntaxes("vmerge.vim"), "vmerge.vim vd, vs2, simm5, v0");
}

// AS-06: standard vector pseudo-instructions expand exactly as LLVM expands them.
TEST(VecAsm, AS06_pseudo_instructions_match_llvm)
{
    struct Case
    {
        const char *text;
        std::vector<uint32_t> words;
    };
    const std::vector<Case> cases = {
        {"vneg.v v1, v2", {0x0e2040d7}},
        {"vnot.v v1, v2, v0.t", {0x2c2fb0d7}},
        {"vwcvt.x.x.v v2, v4", {0xc6406157}},
        {"vwcvtu.x.x.v v2, v4, v0.t", {0xc0406157}},
        {"vncvt.x.x.w v1, v2", {0xb22040d7}},
        {"vfneg.v v1, v2", {0x262110d7}},
        {"vfabs.v v1, v2, v0.t", {0x282110d7}},
        {"vmmv.m v1, v2", {0x662120d7}},
        {"vmclr.m v1", {0x6e10a0d7}},
        {"vmset.m v1", {0x7e10a0d7}},
        {"vmnot.m v1, v2", {0x762120d7}},
        {"vmsgt.vv v1, v2, v3, v0.t", {0x6c3100d7}},
        {"vmsgtu.vv v1, v2, v3", {0x6a3100d7}},
        {"vmsge.vv v1, v2, v3", {0x763100d7}},
        {"vmsgeu.vv v1, v2, v3, v0.t", {0x703100d7}},
        {"vmfgt.vv v1, v2, v3", {0x6e3110d7}},
        {"vmfge.vv v1, v2, v3, v0.t", {0x643110d7}},
        {"vmslt.vi v1, v2, 16", {0x7627b0d7}},
        {"vmslt.vi v1, v2, -15", {0x762830d7}},
        {"vmsltu.vi v1, v2, 1", {0x722030d7}},
        {"vmsltu.vi v1, v2, 0", {0x662100d7}},
        {"vmsge.vi v1, v2, -15, v0.t", {0x7c2830d7}},
        {"vmsgeu.vi v1, v2, 16", {0x7a27b0d7}},
        {"vmsgeu.vi v1, v2, 0", {0x622100d7}},
        {"vmsge.vx v1, v2, a0", {0x6e2540d7, 0x7610a0d7}},
        {"vmsgeu.vx v1, v2, a0, v0.t", {0x682540d7, 0x6e1020d7}},
        {"vmsge.vx v0, v2, a0, v0.t, v3", {0x6e2541d7, 0x6201a057}},
        {"vmsgeu.vx v1, v2, a0, v0.t, v3", {0x6a2541d7, 0x6201a1d7, 0x621020d7, 0x6a30a0d7}},
        {"vl1r.v v1, (a0)", {0x02850087}},
        {"vl2r.v v2, (a0)", {0x22850107}},
        {"vl4r.v v4, (a0)", {0x62850207}},
        {"vl8r.v v8, (a0)", {0xe2850407}},
    };
    for (const Case &c : cases)
    {
        SCOPED_TRACE(c.text);
        vtest::QuietOutput quiet;
        std::istringstream in(std::string(".text\n") + c.text + "\n");
        AssembledProgram program;
        ASSERT_NO_THROW(program = assemble(in));
        EXPECT_EQ(program.text_buffer, c.words);
    }
    EXPECT_TRUE(assemblyFails(".text\nvmsge.vx v0, v2, a0, v0.t\n")); // vd = v0 needs a temp
    EXPECT_TRUE(assemblyFails(".text\nvmslt.vi v1, v2, 17\n"));
    EXPECT_TRUE(assemblyFails(".text\nvmsltu.vi v1, v2, -1\n"));
    EXPECT_TRUE(assemblyFails(".text\nvmclr.m v1, v2\n"));
}
