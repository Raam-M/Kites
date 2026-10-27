/**
 * @file repl.cpp
 * @brief Interactive REPL loop for kites-cli.
 */
#include "repl.h"

#include "common/globals.h"
#include "processor/registers.h"
#include "processor/vector/vector_config.h"
#include "processor/vector/vector_format.h"
#include "processor/vector/vector_profiler.h"
#include "processor/vector/vtype.h"
#include "utils/utils.h"

#include <QObject>
#include <QString>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace Kites
{
namespace
{
void PrintRegisters(std::ostream &os, RegisterFile &registers)
{
    std::vector<uint64_t> gpr = registers.GetGprValues();
    os << "General-purpose registers:\n";
    for (size_t i = 0; i < gpr.size(); ++i)
    {
        os << "  x" << std::left << std::setw(3) << i << " = 0x" << std::hex << std::setw(16)
           << std::setfill('0') << gpr[i] << std::dec << std::setfill(' ') << "\n";
    }

    std::vector<uint64_t> fpr = registers.GetFprValues();
    os << "Floating-point registers:\n";
    for (size_t i = 0; i < fpr.size(); ++i)
    {
        os << "  f" << std::left << std::setw(3) << i << " = 0x" << std::hex << std::setw(16)
           << std::setfill('0') << fpr[i] << std::dec << std::setfill(' ') << "\n";
    }

    os << "Control and status registers:\n";
    for (const auto &[name, address] : csr_to_address)
    {
        os << "  " << std::left << std::setw(7) << name << " = 0x" << std::hex << std::setw(16)
           << std::setfill('0') << registers.ReadCsr(address) << std::dec << std::setfill(' ')
           << "\n";
    }
}

void PrintInfo(std::ostream &os, ProcessorManager &manager)
{
    os << "PC:                    0x" << std::hex << manager.getProgramCounter() << std::dec
       << "\n"
       << "Cycles:                " << manager.getCycles() << "\n"
       << "Instructions retired:  " << manager.getInstructionsRetired() << "\n"
       << "CPI:                   " << manager.getCPI() << "\n"
       << "IPC:                   " << manager.getIPC() << "\n"
       << "Stall cycles:          " << manager.getStallCycles() << "\n"
       << "Branch mispredictions: " << manager.getBranchMispredictions() << "\n";
}

void PrintBreakpoints(std::ostream &os, const command_handler::ReplState &state)
{
    if (state.breakpoints.empty())
    {
        os << "(no breakpoints set)\n";
        return;
    }
    os << "Breakpoints (source line numbers):\n";
    for (uint64_t line : state.breakpoints)
    {
        os << "  " << line << "\n";
    }
}

std::vector<std::string> Tokens(const std::string &line)
{
    std::istringstream iss(line);
    std::vector<std::string> out;
    std::string token;
    while (iss >> token)
        out.push_back(token);
    return out;
}

/// Current SEW from vtype, or 8 when vtype is illegal.
unsigned CurrentSew(RegisterFile &registers)
{
    const rvv::VType vt{registers.ReadCsr(csr_address::kVtype)};
    return vt.vill() ? 8U : vt.sew();
}

/// vregs [e8|e16|e32|e64] [hex|u|s|f]   and   vreg <vN> [eN] [format]
void PrintVectorRegisters(std::ostream &os, RegisterFile &registers,
                          const std::vector<std::string> &args, bool single)
{
    size_t next = 1;
    unsigned first = 0;
    unsigned last = 31;
    if (single)
    {
        if (args.size() < 2 || args[1].size() < 2 || args[1][0] != 'v')
            throw std::invalid_argument("usage: vreg <v0..v31> [e8|e16|e32|e64] [hex|u|s|f]");
        first = last = static_cast<unsigned>(std::stoul(args[1].substr(1)));
        if (first > 31)
            throw std::invalid_argument("vector registers are v0..v31");
        next = 2;
    }
    unsigned sew = CurrentSew(registers);
    rvv::ElementFormat format = rvv::ElementFormat::Hex;
    for (; next < args.size(); ++next)
    {
        const std::string &a = args[next];
        if (a == "e8" || a == "e16" || a == "e32" || a == "e64")
            sew = static_cast<unsigned>(std::stoul(a.substr(1)));
        else if (auto f = rvv::parseElementFormat(a))
            format = *f;
        else
            throw std::invalid_argument("unknown option '" + a + "' (e8..e64, hex, u, s, f)");
    }
    os << "VLEN = " << registers.GetVlenb() * 8 << ", SEW = " << sew << " (element 0 first)\n";
    for (unsigned r = first; r <= last; ++r)
        os << "  v" << std::left << std::setw(3) << r << std::right
           << rvv::formatRegister(registers.ReadVr(r), sew, format) << "\n";
}

void PrintVectorCsrs(std::ostream &os, RegisterFile &registers)
{
    const uint64_t vtype = registers.ReadCsr(csr_address::kVtype);
    os << "  vl     = " << registers.ReadCsr(csr_address::kVl) << "\n"
       << "  vtype  = 0x" << std::hex << vtype << std::dec << "  (" << rvv::vtypeToString(vtype)
       << ")\n"
       << "  vstart = " << registers.ReadCsr(csr_address::kVstart) << "\n"
       << "  vxrm   = " << registers.ReadCsr(csr_address::kVxrm) << "\n"
       << "  vxsat  = " << registers.ReadCsr(csr_address::kVxsat) << "\n"
       << "  vlenb  = " << registers.ReadCsr(csr_address::kVlenb) << "\n";
}

void PrintVectorConfig(std::ostream &os)
{
    const rvv::VectorConfig &cfg = rvv::config();
    const rvv::TimingConfig &timing = rvv::timingConfig();
    os << "  vlen      " << cfg.vlen << "\n"
       << "  agnostic  " << (cfg.agnosticFillOnes ? "ones" : "undisturbed") << "\n"
       << "  ext       " << cfg.describe() << " (ELEN = " << cfg.elen() << ")\n"
       << "  lanes     " << timing.lanes << "\n"
       << "  chaining  " << (timing.chaining ? "on" : "off") << "\n";
}

/// vconfig [vlen N] [agnostic undisturbed|ones] [ext LIST] [lanes N] [chaining on|off]
void ExecuteVectorConfig(std::ostream &os, ProcessorManager &manager,
                         const std::vector<std::string> &args)
{
    if (args.size() == 1)
    {
        PrintVectorConfig(os);
        return;
    }
    if (args.size() % 2 == 0)
        throw std::invalid_argument("usage: vconfig [vlen N] [agnostic undisturbed|ones] "
                                    "[ext LIST] [lanes N] [chaining on|off]");
    rvv::VectorConfig cfg = rvv::config();
    rvv::TimingConfig timing = rvv::timingConfig();
    for (size_t i = 1; i + 1 < args.size(); i += 2)
    {
        const std::string &key = args[i];
        const std::string &value = args[i + 1];
        if (key == "vlen")
        {
            const auto vlen = static_cast<unsigned>(std::stoul(value));
            if (!rvv::isSupportedVlen(vlen))
                throw std::invalid_argument("vlen must be 128, 256, 512 or 1024");
            cfg.vlen = vlen;
        }
        else if (key == "agnostic")
        {
            if (value != "ones" && value != "undisturbed")
                throw std::invalid_argument("agnostic must be 'undisturbed' or 'ones'");
            cfg.agnosticFillOnes = value == "ones";
        }
        else if (key == "ext")
        {
            if (std::string error = rvv::applyExtensionList(cfg, value); !error.empty())
                throw std::invalid_argument(error);
        }
        else if (key == "lanes")
        {
            const auto lanes = static_cast<unsigned>(std::stoul(value));
            if (!rvv::isSupportedLaneCount(lanes))
                throw std::invalid_argument("lanes must be a power of two from 1 to 32");
            timing.lanes = lanes;
        }
        else if (key == "chaining")
        {
            if (value != "on" && value != "off")
                throw std::invalid_argument("chaining must be 'on' or 'off'");
            timing.chaining = value == "on";
        }
        else
        {
            throw std::invalid_argument("unknown vconfig key '" + key + "'");
        }
    }
    const bool vlenChanged = cfg.vlen != rvv::config().vlen;
    rvv::config() = cfg;
    rvv::timingConfig() = timing;
    if (vlenChanged)
    {
        manager.reset(); // the register file is resized on reset
        os << "VLEN changed: the processor was reset.\n";
    }
    PrintVectorConfig(os);
}

/// vstats [json]
void PrintVectorStats(std::ostream &os, ProcessorManager &manager,
                      const std::vector<std::string> &args)
{
    const rvv::VectorProfiler *profiler = manager.getVectorProfiler();
    if (args.size() > 1 && args[1] == "json")
    {
        const auto path = globals::vm_state_directory / "vector_stats.json";
        std::ofstream(path) << profiler->toJson();
        os << "Vector statistics -> " << path.string() << "\n";
        return;
    }
    os << profiler->report();
}

void PrintDisassembly(std::ostream &os, AssembledProgram &program)
{
    DumpDisasssembly(globals::disassembly_file_path, program);
    std::ifstream in(globals::disassembly_file_path);
    os << in.rdbuf();
}

void PrintHelp(std::ostream &os)
{
    os << "Commands:\n"
       << "  load <file> [--vm <name>]     assemble and load a program\n"
       << "                                 vm: rvss (default) | rv5-nh-nf | rv5-h-nf | "
          "rv5-nh-f | rv5-h-f\n"
       << "  run                           run to completion\n"
       << "  run_debug | rd                run until the next breakpoint\n"
       << "  step [n] | s [n]              execute n instructions (default 1)\n"
       << "  stop                          request the running program to stop\n"
       << "  undo | u                      step backward through executed instructions\n"
       << "  redo | r                      step forward through undone instructions\n"
       << "  reset                         reset the VM to its initial state\n"
       << "  add_breakpoint <line>         break at a source line number\n"
       << "  remove_breakpoint <line>      remove a breakpoint\n"
       << "  breakpoints                   list current breakpoints\n"
       << "  modify_register <reg> <val> | mreg\n"
       << "  print_mem <hex_addr> <rows> | pmem\n"
       << "  dump_mem <hex_addr> <rows> [<hex_addr> <rows>]... | dmem\n"
       << "  vm_stdin <text> | vmsin       feed input to a program blocked on a read syscall\n"
       << "  regs                          print all registers\n"
       << "  disas                         print disassembly of the loaded program\n"
       << "  info                          print PC/cycles/CPI/IPC/stats\n"
       << "  vregs [eN] [hex|u|s|f]        print v0..v31 as elements (default: current SEW, hex)\n"
       << "  vreg <vN> [eN] [hex|u|s|f]    print one vector register\n"
       << "  vcsr                          print vl, vtype (decoded), vstart, vxrm, vxsat, vlenb\n"
       << "  vconfig [key value]...        show or set vector options: vlen 128..1024,\n"
       << "                                agnostic undisturbed|ones, ext <list>, lanes N,\n"
       << "                                chaining on|off (changing vlen resets the VM)\n"
       << "  vstats [json]                 vector statistics and timing estimate\n"
       << "  help                          show this message\n"
       << "  exit | quit | q               leave the REPL\n";
}

std::string FirstToken(const std::string &line)
{
    std::istringstream iss(line);
    std::string token;
    iss >> token;
    return token;
}
} // namespace

int RunRepl(ProcessorManager &manager, command_handler::ReplState &state)
{
    QObject::connect(&manager, &ProcessorManager::runErrorSignal, &manager,
                      [](const QString &message, int sourceLine)
                      {
                          std::cerr << "run error: " << message.toStdString();
                          if (sourceLine > 0)
                          {
                              std::cerr << " (line " << sourceLine << ")";
                          }
                          std::cerr << std::endl;
                      });
    QObject::connect(&manager, &ProcessorManager::processorPausedAtBreakpointSignal, &manager,
                      [&manager]()
                      {
                          std::cout << "paused at breakpoint, PC=0x" << std::hex
                                    << manager.getProgramCounter() << std::dec << std::endl;
                      });

    std::cout << "Kites CLI REPL. Type 'help' for commands, 'exit' to quit.\n";

    std::string line;
    while (true)
    {
        std::cout << "kites> " << std::flush;
        if (!std::getline(std::cin, line))
        {
            std::cout << std::endl;
            break;
        }

        std::string first = FirstToken(line);
        if (first.empty())
        {
            continue;
        }

        if (first == "help")
        {
            PrintHelp(std::cout);
            continue;
        }
        if (first == "regs")
        {
            PrintRegisters(std::cout, *manager.getRegisterFile());
            continue;
        }
        if (first == "info")
        {
            PrintInfo(std::cout, manager);
            continue;
        }
        if (first == "vregs" || first == "vreg" || first == "vcsr" || first == "vconfig" ||
            first == "vstats")
        {
            try
            {
                const std::vector<std::string> args = Tokens(line);
                RegisterFile &registers = *manager.getRegisterFile();
                if (first == "vregs" || first == "vreg")
                    PrintVectorRegisters(std::cout, registers, args, first == "vreg");
                else if (first == "vcsr")
                    PrintVectorCsrs(std::cout, registers);
                else if (first == "vconfig")
                    ExecuteVectorConfig(std::cout, manager, args);
                else
                    PrintVectorStats(std::cout, manager, args);
            }
            catch (const std::exception &e)
            {
                std::cerr << "error: " << e.what() << std::endl;
            }
            continue;
        }
        if (first == "breakpoints")
        {
            PrintBreakpoints(std::cout, state);
            continue;
        }
        if (first == "disas")
        {
            if (!state.programLoaded)
            {
                std::cerr << "error: no program loaded\n";
                continue;
            }
            PrintDisassembly(std::cout, state.currentProgram);
            continue;
        }

        command_handler::Command command = command_handler::ParseCommand(line);
        if (command.type == command_handler::CommandType::INVALID)
        {
            std::cerr << "error: unknown command '" << first << "' (try 'help')\n";
            continue;
        }
        if (command.type == command_handler::CommandType::EXIT)
        {
            break;
        }

        try
        {
            command_handler::ExecuteCommand(command, manager, state);
        }
        catch (const std::exception &e)
        {
            std::cerr << "error: " << e.what() << std::endl;
        }
    }

    return 0;
}
} // namespace Kites
