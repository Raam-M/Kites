/**
 * @file vector_reference.h
 * @brief Independent reference model ("oracle") for RVV integer and fixed-point semantics.
 *
 * Written directly from the specification text using 128-bit arithmetic, deliberately without
 * sharing code or tricks with the simulator's implementation (which avoids __int128).
 */
#pragma once

#include "vector_test_support.h"

#include <string>

namespace ref
{

using i128 = __int128;
using u128 = unsigned __int128;

inline u128 umask(unsigned bits)
{
    return (u128{1} << bits) - 1;
}

inline i128 sval(uint64_t v, unsigned bits)
{
    return static_cast<i128>(vtest::sext(v, bits));
}

/// Fixed-point rounding: v >> d rounded according to vxrm (rnu, rne, rdn, rod).
inline i128 roundShift(i128 v, unsigned d, unsigned xrm)
{
    if (d == 0)
        return v;
    const i128 one = 1;
    const i128 q = v >> d;
    const bool half = ((v >> (d - 1)) & 1) != 0;
    const bool belowHalf = d > 1 && (v & ((one << (d - 1)) - 1)) != 0;
    const bool odd = (q & 1) != 0;
    const bool inexact = (v & ((one << d) - 1)) != 0;
    switch (xrm)
    {
    case 0:
        return q + (half ? 1 : 0);
    case 1:
        return q + ((half && (belowHalf || odd)) ? 1 : 0);
    case 2:
        return q;
    default:
        return inexact ? (q | 1) : q;
    }
}

struct Result
{
    uint64_t value;
    bool saturated = false;
};

/// Single-width operation vd[i] = op(vs2[i] = a, operand1 = b).
inline Result binary(const std::string &op, uint64_t a, uint64_t b, unsigned sew, unsigned xrm)
{
    const u128 ua = a & umask(sew);
    const u128 ub = b & umask(sew);
    const i128 sa = sval(a, sew);
    const i128 sb = sval(b, sew);
    const i128 smax = (i128{1} << (sew - 1)) - 1;
    const i128 smin = -(i128{1} << (sew - 1));
    const unsigned shift = static_cast<unsigned>(ub % sew);
    bool sat = false;
    auto clamp = [&](i128 v) -> uint64_t
    {
        if (v > smax)
        {
            sat = true;
            v = smax;
        }
        if (v < smin)
        {
            sat = true;
            v = smin;
        }
        return static_cast<uint64_t>(v);
    };
    auto out = [&](u128 v) { return Result{static_cast<uint64_t>(v), sat}; };

    if (op == "vadd") return out(ua + ub);
    if (op == "vsub") return out(ua - ub);
    if (op == "vrsub") return out(ub - ua);
    if (op == "vand") return out(ua & ub);
    if (op == "vor") return out(ua | ub);
    if (op == "vxor") return out(ua ^ ub);
    if (op == "vsll") return out(ua << shift);
    if (op == "vsrl") return out(ua >> shift);
    if (op == "vsra") return out(static_cast<u128>(sa >> shift));
    if (op == "vminu") return out(ua < ub ? ua : ub);
    if (op == "vmaxu") return out(ua > ub ? ua : ub);
    if (op == "vmin") return out(static_cast<u128>(sa < sb ? sa : sb));
    if (op == "vmax") return out(static_cast<u128>(sa > sb ? sa : sb));
    if (op == "vmul") return out(ua * ub);
    if (op == "vmulh") return out(static_cast<u128>((sa * sb) >> sew));
    if (op == "vmulhu") return out((ua * ub) >> sew);
    if (op == "vmulhsu") return out(static_cast<u128>((sa * static_cast<i128>(ub)) >> sew));
    if (op == "vdivu") return out(ub == 0 ? umask(sew) : ua / ub);
    if (op == "vremu") return out(ub == 0 ? ua : ua % ub);
    if (op == "vdiv")
    {
        if (sb == 0) return out(umask(sew));
        if (sa == smin && sb == -1) return out(ua);
        return out(static_cast<u128>(sa / sb));
    }
    if (op == "vrem")
    {
        if (sb == 0) return out(ua);
        if (sa == smin && sb == -1) return out(0);
        return out(static_cast<u128>(sa % sb));
    }
    if (op == "vsaddu")
    {
        const u128 r = ua + ub;
        if (r > umask(sew))
            return Result{static_cast<uint64_t>(umask(sew)), true};
        return out(r);
    }
    if (op == "vsadd") return Result{clamp(sa + sb), sat};
    if (op == "vssubu")
    {
        const i128 r = static_cast<i128>(ua) - static_cast<i128>(ub);
        return r < 0 ? Result{0, true} : out(static_cast<u128>(r));
    }
    if (op == "vssub") return Result{clamp(sa - sb), sat};
    if (op == "vaaddu") return out(static_cast<u128>(roundShift(static_cast<i128>(ua + ub), 1, xrm)));
    if (op == "vaadd") return out(static_cast<u128>(roundShift(sa + sb, 1, xrm)));
    if (op == "vasubu")
        return out(static_cast<u128>(
            roundShift(static_cast<i128>(ua) - static_cast<i128>(ub), 1, xrm)));
    if (op == "vasub") return out(static_cast<u128>(roundShift(sa - sb, 1, xrm)));
    if (op == "vsmul")
    {
        const uint64_t v = clamp(roundShift(sa * sb, sew - 1, xrm));
        return Result{v, sat};
    }
    if (op == "vssrl") return out(static_cast<u128>(roundShift(static_cast<i128>(ua), shift, xrm)));
    if (op == "vssra") return out(static_cast<u128>(roundShift(sa, shift, xrm)));
    ADD_FAILURE() << "reference model has no operation " << op;
    return {0};
}

} // namespace ref
