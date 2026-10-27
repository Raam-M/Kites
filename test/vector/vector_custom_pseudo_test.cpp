/**
 * @file vector_custom_pseudo_test.cpp
 * @brief Custom pseudo-instructions whose expansions use vector instructions (B10): operand
 * validation against the vector ISA table, argument substitution (including memory operands),
 * and running an expanded program.
 */
#include "vector_test_support.h"

#include "common/globals.h"
#include "custom_pseudo_manager/custom_pseudo_manager.h"

#include <QFile>
#include <QTemporaryDir>

using namespace Kites;
using namespace vtest;

namespace
{

class VectorCustomPseudo : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        saved_ = globals::custom_pseudo_instructions_file_path;
        ASSERT_TRUE(dir_.isValid());
        globals::custom_pseudo_instructions_file_path =
            std::filesystem::path(dir_.path().toStdString()) / "custom_pseudo_instructions.json";
        QFile seed(QString::fromStdString(globals::custom_pseudo_instructions_file_path.string()));
        ASSERT_TRUE(seed.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        seed.write("{}");
    }
    void TearDown() override { globals::custom_pseudo_instructions_file_path = saved_; }

    static bool add(const char *pseudo, const char *expansion, QString &error)
    {
        return customPseudoManager::addCustomPseudoInstruction(pseudo, expansion, error);
    }

    QTemporaryDir dir_;
    std::filesystem::path saved_;
};

} // namespace

TEST_F(VectorCustomPseudo, accepts_vector_expansions)
{
    QString error;
    EXPECT_TRUE(add("vsquare r1, r2", "vmul.vv r1 r2 r2", error)) << error.toStdString();
    EXPECT_TRUE(add("vaxpy r1, r2, r3", "vmacc.vx r1 r3 r2", error)) << error.toStdString();
    EXPECT_TRUE(add("vcopy32 r1, r2, r3", "vsetvli x0 r3 e32 m1 ta ma\nvle32.v v8 (r1)\n"
                                          "vse32.v v8 (r2)",
                    error))
        << error.toStdString();
    EXPECT_TRUE(add("vmaskadd r1, r2", "vadd.vi r1 r2 -3 v0.t", error)) << error.toStdString();
    EXPECT_TRUE(add("vnegate r1, r2", "vneg.v r1 r2", error)) << error.toStdString();
    EXPECT_TRUE(add("vfscale r1, r2", "vfmul.vf r1 r1 r2", error)) << error.toStdString();
}

TEST_F(VectorCustomPseudo, rejects_bad_vector_operands)
{
    QString error;
    EXPECT_FALSE(add("bad1 r1, r2", "vadd.vv r1 r2 x5", error)); // x5 where a vector is needed
    EXPECT_NE(error.toStdString().find("v0..v31"), std::string::npos) << error.toStdString();
    EXPECT_FALSE(add("bad2 r1", "vadd.vi r1 r1 16", error)); // simm5 out of range
    EXPECT_FALSE(add("bad3 r1", "vsetvli x0 r1 e33 m1", error));
    EXPECT_FALSE(add("bad4 r1", "vle32.v r1 r1", error)); // memory operand needs parentheses
    EXPECT_FALSE(add("bad5 r1", "vadd.vv r1 r1", error)); // missing operand
    EXPECT_FALSE(add("bad6 r1", "vadd.vv r1 r1 r1 v0.t v0.t", error));
    EXPECT_FALSE(add("bad7 r1", "vfmul.vf r1 r1 x3", error)); // scalar must be an f register
}

TEST_F(VectorCustomPseudo, expands_and_runs)
{
    QString error;
    ASSERT_TRUE(add("vcopy32 r1, r2, r3", "vsetvli x0 r3 e32 m1 ta ma\nvle32.v v8 (r1)\n"
                                          "vse32.v v8 (r2)",
                    error))
        << error.toStdString();
    ASSERT_TRUE(add("vsquare r1, r2", "vmul.vv r1 r2 r2", error)) << error.toStdString();

    const std::string source = ".data\nsrc: .word 3, 4, 5, 6\ndst: .word 0, 0, 0, 0\n.text\n"
                               "la a0, src\nla a1, dst\nli a2, 4\nvcopy32 a0, a1, a2\n"
                               "vsquare v9, v8\n";
    const std::string expanded = customPseudoManager::expandPseudoInstruction(source);
    EXPECT_NE(expanded.find("vle32.v v8, (a0)"), std::string::npos) << expanded;
    EXPECT_NE(expanded.find("vse32.v v8, (a1)"), std::string::npos) << expanded;
    EXPECT_NE(expanded.find("vmul.vv v9, v8, v8"), std::string::npos) << expanded;

    Machine m(expanded);
    m.run();
    const auto v9 = m.getV(9);
    for (unsigned i = 0; i < 4; ++i)
        EXPECT_EQ(elem(v9, i, 32), (i + 3) * (i + 3)) << i;
}
