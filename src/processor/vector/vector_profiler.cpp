/**
 * @file vector_profiler.cpp
 * @brief Vector statistics and the analytic timing model (see vector_profiler.h).
 */
#include "processor/vector/vector_profiler.h"

#include "processor/vector/vector_config.h"
#include "processor/vector/vector_isa.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace Kites
{
namespace rvv
{

namespace
{
bool startsWith(const std::string &s, const char *prefix)
{
    return s.rfind(prefix, 0) == 0;
}

bool isOneOf(const std::string &s, std::initializer_list<const char *> names)
{
    for (const char *n : names)
        if (s == n)
            return true;
    return false;
}

unsigned log2Lanes(unsigned lanes)
{
    unsigned n = 0;
    while ((1U << n) < lanes)
        ++n;
    return n;
}
} // namespace

std::string unitName(Unit unit)
{
    static const char *names[kUnitCount] = {"config", "integer ALU", "integer multiply",
                                             "integer divide", "FP add", "FP multiply",
                                             "FP divide", "load", "store", "reduction",
                                             "permute", "mask"};
    return names[static_cast<size_t>(unit)];
}

std::string className(InsnClass c)
{
    static const char *names[kClassCount] = {"configuration", "load",      "store",
                                             "integer",       "fixed-point", "floating-point",
                                             "reduction",     "mask",       "permutation",
                                             "bit manipulation"};
    return names[static_cast<size_t>(c)];
}

InsnClass classify(const InsnInfo &info)
{
    const std::string &b = info.base;
    if (info.isConfig())
        return InsnClass::Config;
    if (info.isLoad())
        return InsnClass::Load;
    if (info.isStore())
        return InsnClass::Store;
    if (info.extension != kExtV)
        return InsnClass::BitManip;
    if (b.find("red") != std::string::npos)
        return InsnClass::Reduction;
    if (startsWith(b, "vf") || startsWith(b, "vmf"))
        return (b == "vfslide1up" || b == "vfslide1down" || (b == "vfmv" && info.suffix != "v.f"))
                   ? InsnClass::Permutation
                   : InsnClass::Float;
    if (isOneOf(b, {"vsaddu", "vsadd", "vssubu", "vssub", "vaaddu", "vaadd", "vasubu", "vasub",
                    "vsmul", "vssrl", "vssra", "vnclipu", "vnclip"}))
        return InsnClass::FixedPoint;
    if ((startsWith(b, "vm") && info.suffix == "mm") ||
        isOneOf(b, {"vcpop", "vfirst", "vmsbf", "vmsif", "vmsof", "viota", "vid"}))
        return InsnClass::Mask;
    if (startsWith(b, "vslide") || startsWith(b, "vrgather") || b == "vcompress" ||
        (b == "vmv" && (info.suffix == "x.s" || info.suffix == "s.x")) ||
        isOneOf(b, {"vmv1r", "vmv2r", "vmv4r", "vmv8r"}))
        return InsnClass::Permutation;
    return InsnClass::Integer;
}

Unit unitOf(const InsnInfo &info)
{
    const std::string &b = info.base;
    switch (classify(info))
    {
    case InsnClass::Config:
        return Unit::Config;
    case InsnClass::Load:
        return Unit::Load;
    case InsnClass::Store:
        return Unit::Store;
    case InsnClass::Reduction:
        return Unit::Reduce;
    case InsnClass::Mask:
        return Unit::Mask;
    case InsnClass::Permutation:
        return Unit::Permute;
    case InsnClass::Float:
        if (isOneOf(b, {"vfdiv", "vfrdiv", "vfsqrt"}))
            return Unit::FpDiv;
        if (b == "vfmul" || b == "vfwmul" || b.find("macc") != std::string::npos ||
            b.find("msac") != std::string::npos || b.find("madd") != std::string::npos ||
            b.find("msub") != std::string::npos)
            return Unit::FpMul;
        return Unit::FpAdd;
    default:
        break;
    }
    if (isOneOf(b, {"vdiv", "vdivu", "vrem", "vremu"}))
        return Unit::Div;
    if (startsWith(b, "vmul") || startsWith(b, "vwmul") || b.find("macc") != std::string::npos ||
        isOneOf(b, {"vnmsac", "vmadd", "vnmsub", "vsmul", "vclmul", "vclmulh"}))
        return Unit::Mul;
    return Unit::Alu;
}

TimingConfig &timingConfig()
{
    static TimingConfig instance;
    return instance;
}

bool isSupportedLaneCount(unsigned lanes)
{
    return lanes >= 1 && lanes <= 32 && (lanes & (lanes - 1)) == 0;
}

double VectorStatistics::averageVl() const
{
    return vlInstructions == 0 ? 0.0 : static_cast<double>(vlSum) / static_cast<double>(vlInstructions);
}

double VectorStatistics::utilisation() const
{
    return vlmaxSum == 0 ? 0.0 : static_cast<double>(vlSum) / static_cast<double>(vlmaxSum);
}

void VectorProfiler::record(const InstructionRecord &r)
{
    if (r.info == nullptr)
        return;
    VectorStatistics &s = state_.stats;
    const InsnClass cls = classify(*r.info);
    ++s.instructions;
    ++s.byClass[static_cast<size_t>(cls)];
    ++s.byMnemonic[r.info->mnemonic];
    const bool usesVl = cls != InsnClass::Config && r.vlmax != 0;
    if (usesVl)
    {
        switch (r.sew)
        {
        case 8:
            ++s.bySew[0];
            break;
        case 16:
            ++s.bySew[1];
            break;
        case 32:
            ++s.bySew[2];
            break;
        default:
            ++s.bySew[3];
            break;
        }
        if (r.lmulLog2 >= -3 && r.lmulLog2 <= 3)
            ++s.byLmul[static_cast<size_t>(r.lmulLog2 + 3)];
        s.vlSum += r.vl;
        s.vlmaxSum += r.vlmax;
        ++s.vlInstructions;
    }
    s.activeElements += r.activeElements;
    s.inactiveElements += r.inactiveElements;
    s.bytesLoaded += r.bytesLoaded;
    s.bytesStored += r.bytesStored;

    // ---- timing
    const TimingConfig &cfg = timingConfig();
    TimingState &t = state_.timing;
    const Unit unit = unitOf(*r.info);
    const auto u = static_cast<size_t>(unit);
    const unsigned lanes = std::max(1U, cfg.lanes);
    uint64_t busy = 0;
    if (unit != Unit::Config)
    {
        const uint64_t perCycle = ((unit == Unit::Load || unit == Unit::Store) && !r.unitStride)
                                      ? 1
                                      : (cls == InsnClass::Mask && r.info->suffix == "mm")
                                            ? 64ULL * lanes // packed mask bits
                                            : lanes;
        busy = std::max<uint64_t>(1, (r.bodyElements + perCycle - 1) / perCycle);
        if (unit == Unit::Reduce)
            busy += log2Lanes(lanes); // combine the per-lane partial results
    }
    const uint64_t latency = cfg.latency[u];

    uint64_t start = std::max(r.issueCycle, t.started ? t.lastStart + 1 : r.issueCycle);
    if (unit != Unit::Config)
        start = std::max(start, t.unitFree[u]);
    for (unsigned reg = 0; reg < 32; ++reg)
    {
        if ((r.registersRead >> reg) & 1U)
            start = std::max(start, cfg.chaining ? t.firstReady[reg] : t.allReady[reg]);
    }
    const uint64_t end = start + latency + busy; // cycle after the last result
    for (unsigned reg = 0; reg < 32; ++reg)
    {
        if ((r.registersWritten >> reg) & 1U)
        {
            t.firstReady[reg] = start + latency;
            t.allReady[reg] = end;
        }
    }
    if (unit != Unit::Config)
    {
        t.unitFree[u] = start + busy;
        t.unitBusy[u] += busy;
        // A new convoy starts on a structural hazard, or on a dependency without chaining.
        const uint32_t unitBit = 1U << u;
        if (t.convoys == 0 || (t.convoyUnits & unitBit) != 0 ||
            (!cfg.chaining && (r.registersRead & t.convoyWrites) != 0))
        {
            ++t.convoys;
            t.convoyUnits = 0;
            t.convoyWrites = 0;
        }
        t.convoyUnits |= unitBit;
        t.convoyWrites |= r.registersWritten;
    }
    if (!t.started)
    {
        t.firstIssue = r.issueCycle; // the estimate starts at the first vector instruction
    }
    t.started = true;
    t.lastStart = start;
    t.finish = std::max(t.finish, end);
}

uint64_t VectorProfiler::estimatedCycles() const
{
    const TimingState &t = state_.timing;
    return t.started ? t.finish - t.firstIssue : 0;
}

std::string VectorProfiler::report() const
{
    const VectorStatistics &s = state_.stats;
    const TimingState &t = state_.timing;
    const TimingConfig &cfg = timingConfig();
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "Vector instructions: " << s.instructions << "\n";
    for (size_t c = 0; c < kClassCount; ++c)
        if (s.byClass[c] != 0)
            out << "  " << std::left << std::setw(18) << className(static_cast<InsnClass>(c))
                << std::right << s.byClass[c] << "\n";
    out << "Average vl: " << s.averageVl() << "  (vl / VLMAX: " << 100.0 * s.utilisation()
        << "%)\n";
    out << "Elements: " << s.activeElements << " active, " << s.inactiveElements
        << " masked off\n";
    out << "Memory: " << s.bytesLoaded << " bytes loaded, " << s.bytesStored << " bytes stored\n";
    static const char *sewNames[] = {"e8", "e16", "e32", "e64"};
    static const char *lmulNames[] = {"mf8", "mf4", "mf2", "m1", "m2", "m4", "m8"};
    out << "SEW use:";
    for (size_t i = 0; i < 4; ++i)
        if (s.bySew[i] != 0)
            out << " " << sewNames[i] << "=" << s.bySew[i];
    out << "\nLMUL use:";
    for (size_t i = 0; i < 7; ++i)
        if (s.byLmul[i] != 0)
            out << " " << lmulNames[i] << "=" << s.byLmul[i];
    out << "\nTiming estimate (" << cfg.lanes << " lanes, chaining " << (cfg.chaining ? "on" : "off")
        << "): " << estimatedCycles() << " cycles, " << t.convoys << " convoys (chimes)\n";
    for (size_t u = 1; u < kUnitCount; ++u)
        if (t.unitBusy[u] != 0)
            out << "  " << std::left << std::setw(18) << unitName(static_cast<Unit>(u)) << std::right
                << t.unitBusy[u] << " busy cycles\n";
    return out.str();
}

std::string VectorProfiler::toJson() const
{
    const VectorStatistics &s = state_.stats;
    const TimingState &t = state_.timing;
    const TimingConfig &cfg = timingConfig();
    std::ostringstream out;
    out << "{\n  \"vlen\": " << config().vlen << ",\n  \"extensions\": \"" << config().describe()
        << "\",\n  \"instructions\": " << s.instructions << ",\n  \"by_class\": {";
    bool first = true;
    for (size_t c = 0; c < kClassCount; ++c)
    {
        out << (first ? "" : ",") << "\n    \"" << className(static_cast<InsnClass>(c))
            << "\": " << s.byClass[c];
        first = false;
    }
    out << "\n  },\n  \"by_mnemonic\": {";
    first = true;
    for (const auto &[name, count] : s.byMnemonic)
    {
        out << (first ? "" : ",") << "\n    \"" << name << "\": " << count;
        first = false;
    }
    out << "\n  },\n  \"average_vl\": " << s.averageVl() << ",\n  \"vl_utilisation\": "
        << s.utilisation() << ",\n  \"active_elements\": " << s.activeElements
        << ",\n  \"inactive_elements\": " << s.inactiveElements
        << ",\n  \"bytes_loaded\": " << s.bytesLoaded << ",\n  \"bytes_stored\": " << s.bytesStored
        << ",\n  \"timing\": {\n    \"lanes\": " << cfg.lanes << ",\n    \"chaining\": "
        << (cfg.chaining ? "true" : "false") << ",\n    \"estimated_cycles\": " << estimatedCycles()
        << ",\n    \"convoys\": " << t.convoys << ",\n    \"unit_busy_cycles\": {";
    first = true;
    for (size_t u = 1; u < kUnitCount; ++u)
    {
        out << (first ? "" : ",") << "\n      \"" << unitName(static_cast<Unit>(u))
            << "\": " << t.unitBusy[u];
        first = false;
    }
    out << "\n    }\n  }\n}\n";
    return out.str();
}

} // namespace rvv
} // namespace Kites
