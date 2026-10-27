/**
 * @file vector_exec_integer.cpp
 * @brief Integer, fixed-point, mask, reduction and permutation vector instructions.
 */
#include "processor/registers.h"
#include "processor/vector/vector_config.h"
#include "processor/vector/vector_exec.h"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace Kites
{
namespace rvv
{
namespace detail
{

uint64_t mulHighUnsigned(uint64_t a, uint64_t b)
{
    const uint64_t aLo = a & 0xFFFFFFFFULL;
    const uint64_t aHi = a >> 32;
    const uint64_t bLo = b & 0xFFFFFFFFULL;
    const uint64_t bHi = b >> 32;
    const uint64_t p0 = aLo * bLo;
    const uint64_t p1 = aLo * bHi;
    const uint64_t p2 = aHi * bLo;
    const uint64_t p3 = aHi * bHi;
    const uint64_t middle = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
    return p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32);
}

int64_t mulHighSigned(int64_t a, int64_t b)
{
    uint64_t high = mulHighUnsigned(static_cast<uint64_t>(a), static_cast<uint64_t>(b));
    if (a < 0)
        high -= static_cast<uint64_t>(b);
    if (b < 0)
        high -= static_cast<uint64_t>(a);
    return static_cast<int64_t>(high);
}

int64_t mulHighSignedUnsigned(int64_t a, uint64_t b)
{
    uint64_t high = mulHighUnsigned(static_cast<uint64_t>(a), b);
    if (a < 0)
        high -= b;
    return static_cast<int64_t>(high);
}

namespace
{

using u64 = uint64_t;
using i64 = int64_t;

i64 sx(u64 value, unsigned bits)
{
    return signExtend(value, bits);
}

i64 signedMax(unsigned sew)
{
    return static_cast<i64>(maskBits(sew - 1));
}

i64 signedMin(unsigned sew)
{
    return -signedMax(sew) - 1;
}

// ------------------------------------------------------------------ fixed-point rounding

unsigned roundingMode(const ExecState &s)
{
    return static_cast<unsigned>(s.csr(csr_address::kVxrm) & 3U);
}

/// Rounding increment for shifting v right by d bits under vxrm (spec "roundoff" function).
u64 roundIncrement(u64 v, unsigned d, unsigned xrm)
{
    if (d == 0)
        return 0;
    const u64 bitD1 = (v >> (d - 1)) & 1U;
    const u64 lower = d >= 2 ? (v & maskBits(d - 1)) : 0;
    const u64 bitD = d < 64 ? (v >> d) & 1U : 0;
    switch (xrm)
    {
    case 0: // rnu: round to nearest, ties up
        return bitD1;
    case 1: // rne: round to nearest, ties to even
        return bitD1 & static_cast<u64>((lower != 0) || (bitD != 0));
    case 2: // rdn: truncate
        return 0;
    default: // rod: round to odd (jam)
        return static_cast<u64>(bitD == 0 && (v & maskBits(d)) != 0);
    }
}

u64 roundShiftUnsigned(u64 v, unsigned d, unsigned xrm)
{
    return (d >= 64 ? 0 : v >> d) + roundIncrement(v, d, xrm);
}

i64 roundShiftSigned(i64 v, unsigned d, unsigned xrm)
{
    return (v >> d) + static_cast<i64>(roundIncrement(static_cast<u64>(v), d, xrm));
}

// ------------------------------------------------------------------ single-width operations
// a = vs2[i], b = operand 1 (vs1[i], scalar or immediate); both truncated to SEW.

using BinaryOp = u64 (*)(ExecState &, u64 a, u64 b, unsigned sew);

u64 addOp(ExecState &, u64 a, u64 b, unsigned) { return a + b; }
u64 subOp(ExecState &, u64 a, u64 b, unsigned) { return a - b; }
u64 rsubOp(ExecState &, u64 a, u64 b, unsigned) { return b - a; }
u64 andOp(ExecState &, u64 a, u64 b, unsigned) { return a & b; }
u64 orOp(ExecState &, u64 a, u64 b, unsigned) { return a | b; }
u64 xorOp(ExecState &, u64 a, u64 b, unsigned) { return a ^ b; }
u64 sllOp(ExecState &, u64 a, u64 b, unsigned sew) { return a << (b & (sew - 1)); }
u64 srlOp(ExecState &, u64 a, u64 b, unsigned sew) { return a >> (b & (sew - 1)); }
u64 sraOp(ExecState &, u64 a, u64 b, unsigned sew)
{
    return static_cast<u64>(sx(a, sew) >> (b & (sew - 1)));
}
u64 minuOp(ExecState &, u64 a, u64 b, unsigned) { return std::min(a, b); }
u64 maxuOp(ExecState &, u64 a, u64 b, unsigned) { return std::max(a, b); }
u64 minOp(ExecState &, u64 a, u64 b, unsigned sew) { return sx(a, sew) < sx(b, sew) ? a : b; }
u64 maxOp(ExecState &, u64 a, u64 b, unsigned sew) { return sx(a, sew) > sx(b, sew) ? a : b; }
u64 mulOp(ExecState &, u64 a, u64 b, unsigned) { return a * b; }

u64 mulhOp(ExecState &, u64 a, u64 b, unsigned sew)
{
    if (sew == 64)
        return static_cast<u64>(mulHighSigned(sx(a, 64), sx(b, 64)));
    return static_cast<u64>((sx(a, sew) * sx(b, sew)) >> sew);
}

u64 mulhuOp(ExecState &, u64 a, u64 b, unsigned sew)
{
    if (sew == 64)
        return mulHighUnsigned(a, b);
    return (a * b) >> sew;
}

u64 mulhsuOp(ExecState &, u64 a, u64 b, unsigned sew)
{
    // signed(vs2) * unsigned(vs1)
    if (sew == 64)
        return static_cast<u64>(mulHighSignedUnsigned(sx(a, 64), b));
    return static_cast<u64>((sx(a, sew) * static_cast<i64>(b)) >> sew);
}

u64 divuOp(ExecState &, u64 a, u64 b, unsigned) { return b == 0 ? ~0ULL : a / b; }
u64 remuOp(ExecState &, u64 a, u64 b, unsigned) { return b == 0 ? a : a % b; }

u64 divOp(ExecState &, u64 a, u64 b, unsigned sew)
{
    const i64 sa = sx(a, sew);
    const i64 sb = sx(b, sew);
    if (sb == 0)
        return ~0ULL; // -1
    if (sa == signedMin(sew) && sb == -1)
        return a; // overflow: quotient = dividend
    return static_cast<u64>(sa / sb);
}

u64 remOp(ExecState &, u64 a, u64 b, unsigned sew)
{
    const i64 sa = sx(a, sew);
    const i64 sb = sx(b, sew);
    if (sb == 0)
        return a;
    if (sa == signedMin(sew) && sb == -1)
        return 0;
    return static_cast<u64>(sa % sb);
}

u64 saddu(ExecState &s, u64 a, u64 b, unsigned sew)
{
    const u64 r = (a + b) & maskBits(sew);
    if (r < a)
    {
        s.setVxsat = true;
        return maskBits(sew);
    }
    return r;
}

u64 sadd(ExecState &s, u64 a, u64 b, unsigned sew)
{
    const u64 r = (a + b) & maskBits(sew);
    const u64 sign = 1ULL << (sew - 1);
    if (((a ^ r) & (b ^ r) & sign) != 0)
    {
        s.setVxsat = true;
        return static_cast<u64>(sx(a, sew) < 0 ? signedMin(sew) : signedMax(sew));
    }
    return r;
}

u64 ssubu(ExecState &s, u64 a, u64 b, unsigned)
{
    if (a < b)
    {
        s.setVxsat = true;
        return 0;
    }
    return a - b;
}

u64 ssub(ExecState &s, u64 a, u64 b, unsigned sew)
{
    const u64 r = (a - b) & maskBits(sew);
    const u64 sign = 1ULL << (sew - 1);
    if (((a ^ b) & (a ^ r) & sign) != 0)
    {
        s.setVxsat = true;
        return static_cast<u64>(sx(a, sew) < 0 ? signedMin(sew) : signedMax(sew));
    }
    return r;
}

// Averaging operations compute on SEW+1 bits, then shift right by one with rounding.
u64 aaddu(ExecState &s, u64 a, u64 b, unsigned sew)
{
    const unsigned xrm = roundingMode(s);
    const u64 sum = a + b;
    if (sew < 64)
        return roundShiftUnsigned(sum, 1, xrm);
    const u64 carry = sum < a ? 1 : 0;
    return ((sum >> 1) | (carry << 63)) + roundIncrement(sum, 1, xrm);
}

u64 aadd(ExecState &s, u64 a, u64 b, unsigned sew)
{
    const unsigned xrm = roundingMode(s);
    if (sew < 64)
        return static_cast<u64>(roundShiftSigned(sx(a, sew) + sx(b, sew), 1, xrm));
    const u64 sum = a + b;
    const u64 overflow = ((a ^ sum) & (b ^ sum)) >> 63;
    const u64 top = (sum >> 63) ^ overflow;
    return ((sum >> 1) | (top << 63)) + roundIncrement(sum, 1, xrm);
}

u64 asubu(ExecState &s, u64 a, u64 b, unsigned sew)
{
    const unsigned xrm = roundingMode(s);
    if (sew < 64)
        return static_cast<u64>(
            roundShiftSigned(static_cast<i64>(a) - static_cast<i64>(b), 1, xrm));
    const u64 diff = a - b;
    const u64 borrow = a < b ? 1 : 0;
    return ((diff >> 1) | (borrow << 63)) + roundIncrement(diff, 1, xrm);
}

u64 asub(ExecState &s, u64 a, u64 b, unsigned sew)
{
    const unsigned xrm = roundingMode(s);
    if (sew < 64)
        return static_cast<u64>(roundShiftSigned(sx(a, sew) - sx(b, sew), 1, xrm));
    const u64 diff = a - b;
    const u64 overflow = ((a ^ b) & (a ^ diff)) >> 63;
    const u64 top = (diff >> 63) ^ overflow;
    return ((diff >> 1) | (top << 63)) + roundIncrement(diff, 1, xrm);
}

u64 smul(ExecState &s, u64 a, u64 b, unsigned sew)
{
    const unsigned xrm = roundingMode(s);
    const i64 sa = sx(a, sew);
    const i64 sb = sx(b, sew);
    if (sa == signedMin(sew) && sb == signedMin(sew))
    {
        s.setVxsat = true;
        return static_cast<u64>(signedMax(sew));
    }
    if (sew < 64)
        return static_cast<u64>(roundShiftSigned(sa * sb, sew - 1, xrm));
    const u64 low = a * b;
    const u64 high = static_cast<u64>(mulHighSigned(sa, sb));
    return ((high << 1) | (low >> 63)) + roundIncrement(low, 63, xrm);
}

u64 ssrl(ExecState &s, u64 a, u64 b, unsigned sew)
{
    return roundShiftUnsigned(a, static_cast<unsigned>(b & (sew - 1)), roundingMode(s));
}

u64 ssra(ExecState &s, u64 a, u64 b, unsigned sew)
{
    return static_cast<u64>(
        roundShiftSigned(sx(a, sew), static_cast<unsigned>(b & (sew - 1)), roundingMode(s)));
}

// ------------------------------------------------------------------ Zvkb / Zvbb / Zvbc

u64 andnOp(ExecState &, u64 a, u64 b, unsigned) { return a & ~b; }

u64 rotateLeft(u64 v, unsigned amount, unsigned sew)
{
    v &= maskBits(sew);
    amount &= sew - 1;
    return amount == 0 ? v : ((v << amount) | (v >> (sew - amount))) & maskBits(sew);
}

u64 rolOp(ExecState &, u64 a, u64 b, unsigned sew)
{
    return rotateLeft(a, static_cast<unsigned>(b & (sew - 1)), sew);
}

u64 rorOp(ExecState &, u64 a, u64 b, unsigned sew)
{
    return rotateLeft(a, static_cast<unsigned>((sew - (b & (sew - 1))) & (sew - 1)), sew);
}

/// Low 64 bits of the carry-less product.
u64 clmulOp(ExecState &, u64 a, u64 b, unsigned)
{
    u64 r = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (((b >> i) & 1U) != 0)
            r ^= a << i;
    return r;
}

/// High 64 bits of the carry-less product.
u64 clmulhOp(ExecState &, u64 a, u64 b, unsigned)
{
    u64 r = 0;
    for (unsigned i = 1; i < 64; ++i)
        if (((b >> i) & 1U) != 0)
            r ^= a >> (64 - i);
    return r;
}

const std::unordered_map<std::string, BinaryOp> &binaryOps()
{
    static const std::unordered_map<std::string, BinaryOp> ops = {
        {"vadd", addOp},     {"vsub", subOp},     {"vrsub", rsubOp},   {"vand", andOp},
        {"vor", orOp},       {"vxor", xorOp},     {"vsll", sllOp},     {"vsrl", srlOp},
        {"vsra", sraOp},     {"vminu", minuOp},   {"vmin", minOp},     {"vmaxu", maxuOp},
        {"vmax", maxOp},     {"vmul", mulOp},     {"vmulh", mulhOp},   {"vmulhu", mulhuOp},
        {"vmulhsu", mulhsuOp}, {"vdivu", divuOp}, {"vdiv", divOp},     {"vremu", remuOp},
        {"vrem", remOp},     {"vsaddu", saddu},   {"vsadd", sadd},     {"vssubu", ssubu},
        {"vssub", ssub},     {"vaaddu", aaddu},   {"vaadd", aadd},     {"vasubu", asubu},
        {"vasub", asub},     {"vsmul", smul},     {"vssrl", ssrl},     {"vssra", ssra},
        {"vandn", andnOp},   {"vrol", rolOp},     {"vror", rorOp},     {"vclmul", clmulOp},
        {"vclmulh", clmulhOp},
    };
    return ops;
}

void singleWidth(ExecState &s)
{
    s.requireVtype();
    const BinaryOp op = binaryOps().at(s.info.base);
    if (s.info.base == "vclmul" || s.info.base == "vclmulh")
    {
        s.require(s.sew == 64, s.info.base + " requires SEW = 64");
    }
    if (s.sew == 64 && (s.info.base.rfind("vmulh", 0) == 0 || s.info.base == "vsmul"))
    {
        s.require(config().supportsMulHigh64(), s.info.base + " with SEW = 64 is not part of " +
                                                    profileName(config().profile));
    }
    const Group d = s.group(s.vd(), s.sew);
    s.checkGroup(d, "vd");
    s.checkGroup(s.group(s.vs2(), s.sew), "vs2");
    if (s.hasVectorSrc1())
        s.checkGroup(s.group(s.vs1(), s.sew), "vs1");
    s.checkMaskOverlap(d);
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactive(d, i);
            continue;
        }
        s.write(d.reg, i, s.sew, op(s, s.read(s.vs2(), i, s.sew), s.src1(i, s.sew), s.sew));
    }
    s.tail(d);
}

// ------------------------------------------------------------------ multiply-add
// d = old vd[i], b = vs1[i]/rs1, a = vs2[i]

using MaccOp = u64 (*)(u64 d, u64 b, u64 a);

const std::unordered_map<std::string, MaccOp> &maccOps()
{
    static const std::unordered_map<std::string, MaccOp> ops = {
        {"vmacc", [](u64 d, u64 b, u64 a) { return b * a + d; }},
        {"vnmsac", [](u64 d, u64 b, u64 a) { return d - b * a; }},
        {"vmadd", [](u64 d, u64 b, u64 a) { return b * d + a; }},
        {"vnmsub", [](u64 d, u64 b, u64 a) { return a - b * d; }},
    };
    return ops;
}

void multiplyAdd(ExecState &s)
{
    s.requireVtype();
    const MaccOp op = maccOps().at(s.info.base);
    const Group d = s.group(s.vd(), s.sew);
    s.checkGroup(d, "vd");
    s.checkGroup(s.group(s.vs2(), s.sew), "vs2");
    if (s.hasVectorSrc1())
        s.checkGroup(s.group(s.vs1(), s.sew), "vs1");
    s.checkMaskOverlap(d);
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactive(d, i);
            continue;
        }
        s.write(d.reg, i, s.sew,
                op(s.read(d.reg, i, s.sew), s.src1(i, s.sew), s.read(s.vs2(), i, s.sew)));
    }
    s.tail(d);
}

// ------------------------------------------------------------------ merge and move

void mergeMove(ExecState &s)
{
    s.requireVtype();
    const bool merge = s.info.base == "vmerge";
    const Group d = s.group(s.vd(), s.sew);
    s.checkGroup(d, "vd");
    if (merge)
    {
        s.checkGroup(s.group(s.vs2(), s.sew), "vs2");
        s.require(!overlaps(d, ExecState::maskGroup(0)),
                  "vmerge destination overlaps the selector register v0");
    }
    if (s.hasVectorSrc1())
        s.checkGroup(s.group(s.vs1(), s.sew), "vs1");
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        const bool takeSrc1 = !merge || s.maskBit(0, i);
        s.write(d.reg, i, s.sew, takeSrc1 ? s.src1(i, s.sew) : s.read(s.vs2(), i, s.sew));
    }
    s.tail(d);
}

// ------------------------------------------------------------------ widening

using WideOp = u64 (*)(u64 a, u64 b, u64 d, unsigned sew, bool wideA);

u64 extendA(u64 a, unsigned sew, bool wideA, bool isSigned)
{
    if (wideA)
        return a;
    return isSigned ? static_cast<u64>(sx(a, sew)) : a;
}

const std::unordered_map<std::string, WideOp> &wideOps()
{
    static const std::unordered_map<std::string, WideOp> ops = {
        {"vwaddu", [](u64 a, u64 b, u64, unsigned sew, bool w)
         { return extendA(a, sew, w, false) + b; }},
        {"vwadd", [](u64 a, u64 b, u64, unsigned sew, bool w)
         { return extendA(a, sew, w, true) + static_cast<u64>(sx(b, sew)); }},
        {"vwsubu", [](u64 a, u64 b, u64, unsigned sew, bool w)
         { return extendA(a, sew, w, false) - b; }},
        {"vwsub", [](u64 a, u64 b, u64, unsigned sew, bool w)
         { return extendA(a, sew, w, true) - static_cast<u64>(sx(b, sew)); }},
        {"vwmulu", [](u64 a, u64 b, u64, unsigned, bool) { return a * b; }},
        {"vwmul", [](u64 a, u64 b, u64, unsigned sew, bool)
         { return static_cast<u64>(sx(a, sew) * sx(b, sew)); }},
        {"vwmulsu", [](u64 a, u64 b, u64, unsigned sew, bool)
         { return static_cast<u64>(sx(a, sew) * static_cast<i64>(b)); }},
        {"vwmaccu", [](u64 a, u64 b, u64 d, unsigned, bool) { return d + b * a; }},
        {"vwmacc", [](u64 a, u64 b, u64 d, unsigned sew, bool)
         { return d + static_cast<u64>(sx(b, sew) * sx(a, sew)); }},
        {"vwmaccsu", [](u64 a, u64 b, u64 d, unsigned sew, bool)
         { return d + static_cast<u64>(sx(b, sew) * static_cast<i64>(a)); }},
        {"vwmaccus", [](u64 a, u64 b, u64 d, unsigned sew, bool)
         { return d + static_cast<u64>(static_cast<i64>(b) * sx(a, sew)); }},
        // Zvbb: zero-extended vs2 shifted left by the low log2(2*SEW) bits of the operand.
        {"vwsll", [](u64 a, u64 b, u64, unsigned sew, bool) { return a << (b & (2 * sew - 1)); }},
    };
    return ops;
}

void widening(ExecState &s)
{
    s.requireVtype();
    s.require(2 * s.sew <= config().elen(), "widening requires 2*SEW <= ELEN");
    const WideOp op = wideOps().at(s.info.base);
    const bool wideA = !s.info.suffix.empty() && s.info.suffix[0] == 'w';
    const bool accumulate = s.info.base.rfind("vwmacc", 0) == 0;
    const unsigned wide = 2 * s.sew;
    const Group d = s.group(s.vd(), wide);
    s.checkGroup(d, "vd");
    const Group a = s.group(s.vs2(), wideA ? wide : s.sew);
    s.checkGroup(a, "vs2");
    s.checkOverlap(d, a, "vs2");
    if (s.hasVectorSrc1())
    {
        const Group b = s.group(s.vs1(), s.sew);
        s.checkGroup(b, "vs1");
        s.checkOverlap(d, b, "vs1");
    }
    s.checkMaskOverlap(d);
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactive(d, i);
            continue;
        }
        const u64 av = s.read(s.vs2(), i, wideA ? wide : s.sew);
        const u64 dv = accumulate ? s.read(d.reg, i, wide) : 0;
        s.write(d.reg, i, wide, op(av, s.src1(i, s.sew), dv, s.sew, wideA));
    }
    s.tail(d);
}

// ------------------------------------------------------------------ Zvbb unary bit operations

u64 reverseBits(u64 v, unsigned width)
{
    u64 r = 0;
    for (unsigned i = 0; i < width; ++i)
        r |= ((v >> i) & 1U) << (width - 1 - i);
    return r;
}

u64 bitUnaryOp(const InsnInfo &info, u64 a, unsigned sew)
{
    const std::string &base = info.base;
    a &= maskBits(sew);
    if (base == "vbrev")
        return reverseBits(a, sew);
    if (base == "vbrev8" || base == "vrev8")
    {
        u64 r = 0;
        for (unsigned byte = 0; byte < sew / 8; ++byte)
        {
            const u64 b = (a >> (8 * byte)) & 0xFFU;
            if (base == "vbrev8")
                r |= reverseBits(b, 8) << (8 * byte);
            else
                r |= b << (8 * (sew / 8 - 1 - byte));
        }
        return r;
    }
    if (base == "vclz")
    {
        unsigned n = 0;
        for (unsigned i = sew; i-- > 0 && ((a >> i) & 1U) == 0;)
            ++n;
        return n;
    }
    if (base == "vctz")
    {
        unsigned n = 0;
        for (unsigned i = 0; i < sew && ((a >> i) & 1U) == 0; ++i)
            ++n;
        return n;
    }
    u64 n = 0; // vcpop.v
    for (; a != 0; a &= a - 1)
        ++n;
    return n;
}

void bitUnary(ExecState &s)
{
    s.requireVtype();
    const Group d = s.group(s.vd(), s.sew);
    s.checkGroup(d, "vd");
    s.checkGroup(s.group(s.vs2(), s.sew), "vs2");
    s.checkMaskOverlap(d);
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactive(d, i);
            continue;
        }
        s.write(d.reg, i, s.sew, bitUnaryOp(s.info, s.read(s.vs2(), i, s.sew), s.sew));
    }
    s.tail(d);
}

// ------------------------------------------------------------------ narrowing

void narrowing(ExecState &s)
{
    s.requireVtype();
    s.require(2 * s.sew <= config().elen(), "narrowing requires 2*SEW <= ELEN");
    const unsigned wide = 2 * s.sew;
    const Group d = s.group(s.vd(), s.sew);
    s.checkGroup(d, "vd");
    const Group a = s.group(s.vs2(), wide);
    s.checkGroup(a, "vs2");
    s.checkOverlap(d, a, "vs2");
    if (s.hasVectorSrc1())
        s.checkGroup(s.group(s.vs1(), s.sew), "vs1");
    s.checkMaskOverlap(d);
    const std::string &base = s.info.base;
    const unsigned xrm = roundingMode(s);
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactive(d, i);
            continue;
        }
        const u64 av = s.read(s.vs2(), i, wide);
        const unsigned shift = static_cast<unsigned>(s.src1(i, s.sew) & (wide - 1));
        u64 result = 0;
        if (base == "vnsrl")
        {
            result = av >> shift;
        }
        else if (base == "vnsra")
        {
            result = static_cast<u64>(sx(av, wide) >> shift);
        }
        else if (base == "vnclipu")
        {
            result = roundShiftUnsigned(av, shift, xrm);
            if (result > maskBits(s.sew))
            {
                result = maskBits(s.sew);
                s.setVxsat = true;
            }
        }
        else // vnclip
        {
            i64 v = roundShiftSigned(sx(av, wide), shift, xrm);
            if (v > signedMax(s.sew))
            {
                v = signedMax(s.sew);
                s.setVxsat = true;
            }
            else if (v < signedMin(s.sew))
            {
                v = signedMin(s.sew);
                s.setVxsat = true;
            }
            result = static_cast<u64>(v);
        }
        s.write(d.reg, i, s.sew, result);
    }
    s.tail(d);
}

// ------------------------------------------------------------------ integer extension

void extension(ExecState &s)
{
    s.requireVtype();
    const unsigned factor = static_cast<unsigned>(s.info.suffix.back() - '0'); // vf2/vf4/vf8
    const unsigned srcEew = s.sew / factor;
    s.require(srcEew >= 8, "source EEW = SEW/" + std::to_string(factor) + " is below 8");
    const Group d = s.group(s.vd(), s.sew);
    s.checkGroup(d, "vd");
    const Group a = s.group(s.vs2(), srcEew);
    s.checkGroup(a, "vs2");
    s.checkOverlap(d, a, "vs2");
    s.checkMaskOverlap(d);
    const bool isSigned = s.info.base == "vsext";
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactive(d, i);
            continue;
        }
        const u64 v = s.read(s.vs2(), i, srcEew);
        s.write(d.reg, i, s.sew, isSigned ? static_cast<u64>(sx(v, srcEew)) : v);
    }
    s.tail(d);
}

// ------------------------------------------------------------------ add/subtract with carry

void carry(ExecState &s)
{
    s.requireVtype();
    const std::string &base = s.info.base;
    const bool subtract = base == "vsbc" || base == "vmsbc";
    const bool maskDest = base == "vmadc" || base == "vmsbc";
    // The carry/borrow input comes from v0 when vm is fixed to 0 (.vvm/.vxm/.vim forms).
    const bool carryIn = ((s.info.mask >> 25) & 1U) != 0 && ((s.info.match >> 25) & 1U) == 0;

    const Group a = s.group(s.vs2(), s.sew);
    s.checkGroup(a, "vs2");
    Group b{};
    if (s.hasVectorSrc1())
    {
        b = s.group(s.vs1(), s.sew);
        s.checkGroup(b, "vs1");
    }
    Group d{};
    if (maskDest)
    {
        d = ExecState::maskGroup(s.vd());
        s.checkOverlap(d, a, "vs2");
        if (s.hasVectorSrc1())
            s.checkOverlap(d, b, "vs1");
    }
    else
    {
        d = s.group(s.vd(), s.sew);
        s.checkGroup(d, "vd");
        s.require(!overlaps(d, ExecState::maskGroup(0)),
                  "destination overlaps v0, which holds the carry input");
    }

    const u64 mask = maskBits(s.sew);
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        const u64 av = s.read(s.vs2(), i, s.sew);
        const u64 bv = s.src1(i, s.sew);
        const u64 c = carryIn && s.maskBit(0, i) ? 1 : 0;
        u64 result = 0;
        bool out = false;
        if (!subtract)
        {
            const u64 partial = (av + bv) & mask;
            result = (partial + c) & mask;
            out = partial < av || result < partial;
        }
        else
        {
            result = (av - bv - c) & mask;
            out = av < bv || (av == bv && c != 0);
        }
        if (maskDest)
            s.writeMaskBit(d.reg, i, out);
        else
            s.write(d.reg, i, s.sew, result);
    }
    if (maskDest)
        s.maskTail(d.reg, s.vl);
    else
        s.tail(d);
}

// ------------------------------------------------------------------ integer compares

using CompareOp = bool (*)(u64 a, u64 b, unsigned sew);

const std::unordered_map<std::string, CompareOp> &compareOps()
{
    static const std::unordered_map<std::string, CompareOp> ops = {
        {"vmseq", [](u64 a, u64 b, unsigned) { return a == b; }},
        {"vmsne", [](u64 a, u64 b, unsigned) { return a != b; }},
        {"vmsltu", [](u64 a, u64 b, unsigned) { return a < b; }},
        {"vmslt", [](u64 a, u64 b, unsigned sew) { return sx(a, sew) < sx(b, sew); }},
        {"vmsleu", [](u64 a, u64 b, unsigned) { return a <= b; }},
        {"vmsle", [](u64 a, u64 b, unsigned sew) { return sx(a, sew) <= sx(b, sew); }},
        {"vmsgtu", [](u64 a, u64 b, unsigned) { return a > b; }},
        {"vmsgt", [](u64 a, u64 b, unsigned sew) { return sx(a, sew) > sx(b, sew); }},
    };
    return ops;
}

void compare(ExecState &s)
{
    s.requireVtype();
    const CompareOp op = compareOps().at(s.info.base);
    const Group a = s.group(s.vs2(), s.sew);
    s.checkGroup(a, "vs2");
    const Group d = ExecState::maskGroup(s.vd());
    s.checkOverlap(d, a, "vs2");
    if (s.hasVectorSrc1())
    {
        const Group b = s.group(s.vs1(), s.sew);
        s.checkGroup(b, "vs1");
        s.checkOverlap(d, b, "vs1");
    }
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactiveMaskBit(d.reg, i);
            continue;
        }
        s.writeMaskBit(d.reg, i, op(s.read(s.vs2(), i, s.sew), s.src1(i, s.sew), s.sew));
    }
    s.maskTail(d.reg, s.vl);
}

// ------------------------------------------------------------------ reductions

void reduction(ExecState &s)
{
    s.requireVtype();
    s.requireVstartZero();
    const std::string &base = s.info.base;
    const bool widen = base == "vwredsumu" || base == "vwredsum";
    const unsigned accEew = widen ? 2 * s.sew : s.sew;
    s.require(accEew <= config().elen(), "widening reduction requires 2*SEW <= ELEN");
    s.checkGroup(s.group(s.vs2(), s.sew), "vs2");
    if (s.vl == 0)
    {
        return; // no operation; vd is not updated
    }
    u64 acc = s.read(s.vs1(), 0, accEew);
    for (u64 i = 0; i < s.vl; ++i)
    {
        if (!s.active(i))
            continue;
        const u64 x = s.read(s.vs2(), i, s.sew);
        if (base == "vredsum" || base == "vwredsumu")
            acc += x;
        else if (base == "vwredsum")
            acc += static_cast<u64>(sx(x, s.sew));
        else if (base == "vredand")
            acc &= x;
        else if (base == "vredor")
            acc |= x;
        else if (base == "vredxor")
            acc ^= x;
        else if (base == "vredminu")
            acc = std::min(acc, x);
        else if (base == "vredmaxu")
            acc = std::max(acc, x);
        else if (base == "vredmin")
            acc = sx(x, s.sew) < sx(acc, s.sew) ? x : acc;
        else // vredmax
            acc = sx(x, s.sew) > sx(acc, s.sew) ? x : acc;
        acc &= maskBits(accEew);
    }
    s.write(s.vd(), 0, accEew, acc);
    s.tailFrom(Group{s.vd(), accEew, 0}, 1);
}

// ------------------------------------------------------------------ mask instructions

void maskLogical(ExecState &s)
{
    s.requireVtype();
    const std::string &base = s.info.base;
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        const bool a = s.maskBit(s.vs2(), i);
        const bool b = s.maskBit(s.vs1(), i);
        bool r = false;
        if (base == "vmand")
            r = a && b;
        else if (base == "vmnand")
            r = !(a && b);
        else if (base == "vmandn")
            r = a && !b;
        else if (base == "vmxor")
            r = a != b;
        else if (base == "vmor")
            r = a || b;
        else if (base == "vmnor")
            r = !(a || b);
        else if (base == "vmorn")
            r = a || !b;
        else // vmxnor
            r = a == b;
        s.writeMaskBit(s.vd(), i, r);
    }
    s.maskTail(s.vd(), s.vl);
}

void popFirst(ExecState &s)
{
    s.requireVtype();
    s.requireVstartZero();
    const bool first = s.info.base == "vfirst";
    int64_t result = first ? -1 : 0;
    for (u64 i = 0; i < s.vl; ++i)
    {
        if (!s.active(i) || !s.maskBit(s.vs2(), i))
            continue;
        if (first)
        {
            result = static_cast<int64_t>(i);
            break;
        }
        ++result;
    }
    s.gprWrite = std::make_pair(s.field(kRd), static_cast<u64>(result));
}

void setFirst(ExecState &s)
{
    s.requireVtype();
    s.requireVstartZero();
    const std::string &base = s.info.base;
    s.require(s.vd() != s.vs2(), "destination must not overlap the source mask register");
    if (s.masked)
        s.require(s.vd() != 0, "destination of a masked instruction must not be v0");
    bool found = false;
    for (u64 i = 0; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactiveMaskBit(s.vd(), i);
            continue;
        }
        bool r = false;
        if (!found)
        {
            if (s.maskBit(s.vs2(), i))
            {
                found = true;
                r = base != "vmsbf"; // vmsif and vmsof set the first element
            }
            else
            {
                r = base != "vmsof"; // vmsbf and vmsif set elements before it
            }
        }
        s.writeMaskBit(s.vd(), i, r);
    }
    s.maskTail(s.vd(), s.vl);
}

void iota(ExecState &s)
{
    s.requireVtype();
    s.requireVstartZero();
    const Group d = s.group(s.vd(), s.sew);
    s.checkGroup(d, "vd");
    s.forbidOverlap(d, ExecState::maskGroup(s.vs2()), "vs2");
    if (s.masked)
        s.forbidOverlap(d, ExecState::maskGroup(0), "the mask register v0");
    u64 sum = 0;
    for (u64 i = 0; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactive(d, i);
            continue;
        }
        s.write(d.reg, i, s.sew, sum);
        if (s.maskBit(s.vs2(), i))
            ++sum;
    }
    s.tail(d);
}

void elementIndex(ExecState &s)
{
    s.requireVtype();
    const Group d = s.group(s.vd(), s.sew);
    s.checkGroup(d, "vd");
    s.checkMaskOverlap(d);
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        if (!s.active(i))
            s.inactive(d, i);
        else
            s.write(d.reg, i, s.sew, i);
    }
    s.tail(d);
}

// ------------------------------------------------------------------ permutations

void scalarMove(ExecState &s)
{
    s.require(!s.vtype.vill(), "vtype is illegal (vill = 1)");
    if (s.info.suffix == "x.s")
    {
        // vmv.x.s ignores vl and vstart.
        s.gprWrite = std::make_pair(s.field(kRd),
                                    static_cast<u64>(sx(s.read(s.vs2(), 0, s.sew), s.sew)));
        return;
    }
    // vmv.s.x: vd is a single register; element 0 is written only if vstart < vl.
    if (s.vstart < s.vl)
    {
        s.write(s.vd(), 0, s.sew, s.scalarOperand() & maskBits(s.sew));
        s.tailFrom(Group{s.vd(), s.sew, 0}, 1);
    }
}

void slide(ExecState &s)
{
    s.requireVtype();
    const std::string &base = s.info.base;
    const Group d = s.group(s.vd(), s.sew);
    const Group a = s.group(s.vs2(), s.sew);
    s.checkGroup(d, "vd");
    s.checkGroup(a, "vs2");
    s.checkMaskOverlap(d);
    if (base == "vslideup" || base == "vslide1up" || base == "vfslide1up")
        s.forbidOverlap(d, a, "vs2");

    if (base == "vslideup")
    {
        const u64 offset = s.scalarOperand();
        for (u64 i = std::max(s.vstart, offset); i < s.vl; ++i)
        {
            if (!s.active(i))
                s.inactive(d, i);
            else
                s.write(d.reg, i, s.sew, s.read(a.reg, i - offset, s.sew));
        }
    }
    else if (base == "vslidedown")
    {
        const u64 offset = s.scalarOperand();
        for (u64 i = s.vstart; i < s.vl; ++i)
        {
            if (!s.active(i))
            {
                s.inactive(d, i);
                continue;
            }
            const bool inRange = offset < s.vlmax && i < s.vlmax - offset;
            s.write(d.reg, i, s.sew, inRange ? s.read(a.reg, i + offset, s.sew) : 0);
        }
    }
    else
    {
        const u64 scalar = s.scalarOperand() & maskBits(s.sew);
        const bool up = base == "vslide1up" || base == "vfslide1up";
        for (u64 i = s.vstart; i < s.vl; ++i)
        {
            if (!s.active(i))
            {
                s.inactive(d, i);
                continue;
            }
            u64 value = 0;
            if (up)
                value = i == 0 ? scalar : s.read(a.reg, i - 1, s.sew);
            else
                value = i + 1 == s.vl ? scalar : s.read(a.reg, i + 1, s.sew);
            s.write(d.reg, i, s.sew, value);
        }
    }
    s.tail(d);
}

void gather(ExecState &s)
{
    s.requireVtype();
    const bool ei16 = s.info.base == "vrgatherei16";
    const Group d = s.group(s.vd(), s.sew);
    const Group a = s.group(s.vs2(), s.sew);
    s.checkGroup(d, "vd");
    s.checkGroup(a, "vs2");
    s.forbidOverlap(d, a, "vs2");
    s.checkMaskOverlap(d);
    const unsigned indexEew = ei16 ? 16 : s.sew;
    if (s.hasVectorSrc1())
    {
        const Group idx = s.group(s.vs1(), indexEew);
        s.checkGroup(idx, "vs1");
        s.forbidOverlap(d, idx, "vs1");
    }
    const u64 scalarIndex = s.scalarOperand(); // full XLEN for .vx
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactive(d, i);
            continue;
        }
        const u64 index = s.hasVectorSrc1() ? s.read(s.vs1(), i, indexEew) : scalarIndex;
        s.write(d.reg, i, s.sew, index >= s.vlmax ? 0 : s.read(a.reg, index, s.sew));
    }
    s.tail(d);
}

void compress(ExecState &s)
{
    s.requireVtype();
    s.requireVstartZero();
    const Group d = s.group(s.vd(), s.sew);
    const Group a = s.group(s.vs2(), s.sew);
    s.checkGroup(d, "vd");
    s.checkGroup(a, "vs2");
    s.forbidOverlap(d, a, "vs2");
    s.forbidOverlap(d, ExecState::maskGroup(s.vs1()), "vs1");
    u64 packed = 0;
    for (u64 i = 0; i < s.vl; ++i)
    {
        if (s.maskBit(s.vs1(), i))
            s.write(d.reg, packed++, s.sew, s.read(a.reg, i, s.sew));
    }
    s.tailFrom(d, packed);
}

void wholeRegisterMove(ExecState &s)
{
    // vmv<nr>r.v does not depend on vtype.
    const unsigned count = static_cast<unsigned>(s.info.base[3] - '0');
    s.require(s.vd() % count == 0 && s.vs2() % count == 0,
              "vd and vs2 must be multiples of " + std::to_string(count));
    const unsigned elementBytes = s.vtype.vill() ? 1 : s.sew / 8;
    const uint64_t total = static_cast<uint64_t>(count) * s.vlenb;
    for (uint64_t byte = s.vstart * elementBytes; byte < total; ++byte)
    {
        s.write(s.vd(), byte, 8, s.read(s.vs2(), byte, 8));
    }
}

using Handler = void (*)(ExecState &);

const std::unordered_map<std::string, Handler> &handlers()
{
    static const std::unordered_map<std::string, Handler> table = []
    {
        std::unordered_map<std::string, Handler> h;
        for (const auto &[name, op] : binaryOps())
            h.emplace(name, singleWidth);
        for (const auto &[name, op] : maccOps())
            h.emplace(name, multiplyAdd);
        for (const auto &[name, op] : wideOps())
            h.emplace(name, widening);
        for (const auto &[name, op] : compareOps())
            h.emplace(name, compare);
        for (const char *name : {"vnsrl", "vnsra", "vnclipu", "vnclip"})
            h.emplace(name, narrowing);
        for (const char *name : {"vzext", "vsext"})
            h.emplace(name, extension);
        for (const char *name : {"vadc", "vmadc", "vsbc", "vmsbc"})
            h.emplace(name, carry);
        for (const char *name : {"vredsum", "vredand", "vredor", "vredxor", "vredminu", "vredmin",
                                 "vredmaxu", "vredmax", "vwredsumu", "vwredsum"})
            h.emplace(name, reduction);
        for (const char *name :
             {"vmand", "vmnand", "vmandn", "vmxor", "vmor", "vmnor", "vmorn", "vmxnor"})
            h.emplace(name, maskLogical);
        for (const char *name : {"vcpop", "vfirst"})
            h.emplace(name, popFirst);
        for (const char *name : {"vmsbf", "vmsif", "vmsof"})
            h.emplace(name, setFirst);
        for (const char *name : {"vslideup", "vslidedown", "vslide1up", "vslide1down",
                                 "vfslide1up", "vfslide1down"})
            h.emplace(name, slide);
        for (const char *name : {"vmv1r", "vmv2r", "vmv4r", "vmv8r"})
            h.emplace(name, wholeRegisterMove);
        h.emplace("vmerge", mergeMove);
        h.emplace("viota", iota);
        h.emplace("vid", elementIndex);
        h.emplace("vrgather", gather);
        h.emplace("vrgatherei16", gather);
        h.emplace("vcompress", compress);
        for (const char *name : {"vbrev", "vbrev8", "vrev8", "vclz", "vctz"})
            h.emplace(name, bitUnary);
        return h;
    }();
    return table;
}

Handler findHandler(const InsnInfo &info)
{
    if (info.base == "vcpop" && info.suffix == "v") // Zvbb element popcount, not vcpop.m
        return bitUnary;
    if (info.base == "vmv")
    {
        return (info.suffix == "x.s" || info.suffix == "s.x") ? scalarMove : mergeMove;
    }
    auto it = handlers().find(info.base);
    return it == handlers().end() ? nullptr : it->second;
}

} // namespace

void executeInteger(ExecState &s)
{
    const Handler handler = findHandler(s.info);
    if (handler == nullptr)
    {
        s.illegal("instruction is not implemented");
    }
    handler(s);
}

bool isImplementedInteger(const InsnInfo &info)
{
    return findHandler(info) != nullptr;
}

} // namespace detail
} // namespace rvv
} // namespace Kites
