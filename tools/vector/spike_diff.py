#!/usr/bin/env python3
"""Differential testing of the Kites vector unit against Spike.

Each test program runs on both simulators. A shared epilogue stores the final
architectural state (x1-x31, f0-f31, v0-v31, vl, vtype, vxsat, vxrm, fflags,
frm) into a dump area next to the program's data, and the data plus the dump
are compared byte for byte:

  * Kites runs the program through kites-cli and dumps memory with --mem-dump.
  * Spike runs an ELF built with llvm-mc and ld.lld; the same region is written
    out with +signature.

Modes:
  examples <file.s>...    run existing programs (compares vector registers,
                          vector CSRs, fflags and data memory)
  random --count N        run N generated programs (also compares the scalar
                          registers the generator uses)

Usage:
  python3 -I tools/vector/spike_diff.py --kites build/mac-test/kites-cli examples examples/vector/*.s
  python3 -I tools/vector/spike_diff.py --kites build/mac-test/kites-cli random --count 300 --seed 1
  python3 -I tools/vector/spike_diff.py --kites build/mac-test/kites-cli --vm rv5-h-f random --count 100
  python3 -I tools/vector/spike_diff.py --kites build/mac-test/kites-cli --vlen 512 \
      --vector-ext v,zvfh,zvbb,zvbc random --count 100
  python3 -I tools/vector/spike_diff.py --kites build/mac-test/kites-cli --vector-ext zve32f random
"""

import argparse
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile

# Dump layout; DUMP_CSR and DUMP_SIZE depend on VLEN (see configure_layout).
VLENB = 16
DUMP_GPR = 0          # x1..x31 at 8*(i-1)
DUMP_FPR = 256        # f0..f31
DUMP_VR = 512         # v0..v31, VLENB bytes each
DUMP_CSR = 1024       # vl, vtype, vxsat, vxrm, fflags, frm
DUMP_SIZE = 1088


def configure_layout(vlen):
    global VLENB, DUMP_CSR, DUMP_SIZE
    VLENB = vlen // 8
    DUMP_CSR = DUMP_VR + 32 * VLENB
    DUMP_SIZE = DUMP_CSR + 64


class Target:
    """Vector configuration under test: VLEN, profile and extensions (as kites-cli --vector-ext)."""

    def __init__(self, vlen, ext):
        self.vlen = vlen
        self.ext = ext
        items = [x.strip().lower() for x in ext.split(",") if x.strip()]
        profiles = [x for x in items if x == "v" or x.startswith("zve")]
        self.profile = profiles[0] if profiles else "v"
        self.zvfh = "zvfh" in items
        self.zvbb = "zvbb" in items
        self.zvkb = self.zvbb or "zvkb" in items
        self.zvbc = "zvbc" in items
        self.elen = 32 if self.profile.startswith("zve32") else 64
        self.fp = set()
        if self.profile in ("v", "zve64d", "zve64f", "zve32f"):
            self.fp.add(32)
        if self.profile in ("v", "zve64d"):
            self.fp.add(64)
        if self.zvfh:
            self.fp.add(16)
        self.mulh64 = self.profile == "v"

    def spike_isa(self):
        isa = "rv64gcv" if self.profile == "v" else "rv64gc_" + self.profile
        if self.zvfh:
            isa += "_zfhmin_zvfh"
        if self.zvbb:
            isa += "_zvbb"
        elif self.zvkb:
            isa += "_zvkb"
        if self.zvbc:
            isa += "_zvbc"
        return isa + f"_zvl{self.vlen}b"


TARGET = Target(128, "v")
CSR_NAMES = ["vl", "vtype", "vxsat", "vxrm", "fflags", "frm"]
ABI = ["zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3",
       "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11",
       "t3", "t4", "t5", "t6"]
KITES_DATA_BASE = 0x10000000

LINKER_SCRIPT = """OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
  . = 0x80000000;
  .text : { *(.text.init) *(.text) }
  . = ALIGN(0x1000);
  .tohost : { *(.tohost) }
  . = ALIGN(0x1000);
  .data : { *(.data) }
  .bss : { *(.bss) }
}
"""

# ----------------------------------------------------------------------------- program assembly


def dump_epilogue():
    """Stores the architectural state at kdump (works in both assemblers)."""
    lines = ["    la s11, kdump"]
    for i in range(1, 32):
        if i != 27:  # s11 holds the dump address
            lines.append(f"    sd x{i}, {DUMP_GPR + 8 * (i - 1)}(s11)")
    for i in range(32):
        lines.append(f"    fsd f{i}, {DUMP_FPR + 8 * i}(s11)")
    lines.append(f"    addi s10, s11, {DUMP_VR}")
    for group in (0, 8, 16, 24):
        lines.append(f"    vs8r.v v{group}, (s10)")
        lines.append(f"    addi s10, s10, {8 * VLENB}")
    for i, name in enumerate(CSR_NAMES):  # s10 now points at DUMP_CSR
        lines.append(f"    csrr t0, {name}")
        lines.append(f"    sd t0, {8 * i}(s10)")
    return lines


def kites_source(data, text):
    return "\n".join([".data"] + data +
                     ["kdump: .zero %d" % DUMP_SIZE, ".text"] + text + dump_epilogue()) + "\n"


def spike_source(data, text):
    data = [re.sub(r"\.halfword\b", ".half", line) for line in data]
    prologue = [
        ".section .text.init", ".globl _start", "_start:",
        "    la t0, trap", "    csrw mtvec, t0",
        "    li t0, 0x6600", "    csrs mstatus, t0",   # enable FP (FS) and vector (VS) state
        "    csrwi fcsr, 0",
    ] + [f"    li x{i}, 0" for i in range(1, 32)]
    # On a trap, report mcause and the offending instruction address through the exit code.
    exit_code = ["    li t0, 1", "    la t1, tohost", "    sd t0, 0(t1)", "9:  j 9b",
                 ".balign 4", "trap:", "    csrr t0, mcause", "    slli t0, t0, 32",
                 "    csrr t2, mepc", "    la t1, _start", "    sub t2, t2, t1", "    or t0, t0, t2",
                 "    slli t0, t0, 1", "    ori t0, t0, 1", "    la t1, tohost", "    sd t0, 0(t1)",
                 "8:  j 8b"]
    tohost = [".section .tohost, \"aw\", @progbits", ".align 6", ".globl tohost",
              "tohost: .dword 0", ".align 6", ".globl fromhost", "fromhost: .dword 0"]
    body = (prologue + text + dump_epilogue() + exit_code + tohost +
            [".data", ".balign 16", ".globl begin_signature", "begin_signature:"] +
            data + ["kdump: .zero %d" % DUMP_SIZE, ".balign 16",
                    ".globl end_signature", "end_signature:"])
    return "\n".join(body) + "\n"


def split_program(path):
    """Splits a Kites program into data and text lines."""
    data, text, section = [], [], None
    for line in open(path):
        stripped = line.split("#")[0].strip()
        if stripped.startswith(".data"):
            section = "data"
            continue
        if stripped.startswith(".text"):
            section = "text"
            continue
        if section == "data" and stripped:
            data.append(line.rstrip())
        elif section == "text":
            text.append(line.rstrip())
    return data, text


def data_size(data):
    """Bytes of data before kdump in Kites' layout (natural alignment per directive)."""
    size = 0
    sizes = {".dword": 8, ".word": 4, ".float": 4, ".halfword": 2, ".byte": 1, ".double": 8}
    for line in data:
        body = line.split(":", 1)[1] if re.match(r"^\s*\w+:", line) else line
        body = body.strip()
        if not body:
            continue
        directive, _, rest = body.partition(" ")
        if directive in sizes:
            n = sizes[directive]
            for _ in [v for v in rest.split(",") if v.strip()]:
                size = (size + n - 1) // n * n + n
        elif directive == ".zero":
            size += int(rest.strip(), 0)
        elif directive == ".string":
            size += len(json.loads(rest.strip())) + 1
        else:
            raise ValueError("unsupported data directive: " + line)
    return size


# ----------------------------------------------------------------------------- running


def run_kites(kites, vm, source, length, workdir, state_dir):
    path = os.path.join(workdir, "kites.s")
    open(path, "w").write(source)
    rows = (length + 7) // 8
    result = subprocess.run([kites, path, "--vm", vm, "--vlen", str(TARGET.vlen),
                             "--vector-ext", TARGET.ext,
                             "--mem-dump", f"{KITES_DATA_BASE:x}", str(rows)],
                            capture_output=True, text=True, timeout=120)
    if result.returncode != 0:
        tail = "\n".join((result.stdout + result.stderr).splitlines()[-5:])
        return None, f"kites-cli exit {result.returncode}: {tail}"
    dump = json.load(open(os.path.join(state_dir, "memory_dump.json")))
    out = bytearray()
    for i in range(rows):
        value = int(dump[f"0x{KITES_DATA_BASE + 8 * i:016x}"], 16)
        out += struct.pack("<Q", value)
    return bytes(out[:length]), None


def run_spike(tools, source, length, workdir):
    asm = os.path.join(workdir, "spike.s")
    obj = os.path.join(workdir, "spike.o")
    elf = os.path.join(workdir, "spike.elf")
    ld = os.path.join(workdir, "link.ld")
    sig = os.path.join(workdir, "sig.txt")
    open(asm, "w").write(source)
    open(ld, "w").write(LINKER_SCRIPT)
    for cmd in ([tools["llvm-mc"], "-triple=riscv64",
                 "-mattr=+m,+a,+f,+d,+c,+v,+zicsr,+zfhmin,+zvfh,+zvbb,+zvbc",
                 "-filetype=obj", asm, "-o", obj],
                [tools["ld.lld"], "-T", ld, obj, "-o", elf]):
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            return None, "build failed: " + result.stderr.strip()[:500]
    try:
        result = subprocess.run([tools["spike"], f"--isa={TARGET.spike_isa()}", f"+signature={sig}", elf],
                                capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired:
        return None, "spike timed out"
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip().splitlines()
        return None, f"spike trapped (exit {result.returncode}): {' '.join(detail[-2:])}"
    out = bytearray()
    for line in open(sig):
        out += bytes.fromhex(line.strip())[::-1]
    return bytes(out[:length]), None


# ----------------------------------------------------------------------------- comparison


def describe(offset, data_len):
    """Names the state element at a byte offset of the compared region."""
    if offset < data_len:
        return f"data+{offset}"
    d = offset - data_len
    if d < DUMP_FPR:
        return ABI[d // 8 + 1]
    if d < DUMP_VR:
        return f"f{(d - DUMP_FPR) // 8}"
    if d < DUMP_CSR:
        return f"v{(d - DUMP_VR) // VLENB} byte {(d - DUMP_VR) % VLENB}"
    return CSR_NAMES[(d - DUMP_CSR) // 8]


def compare(kites, spike, data_len, compare_gprs):
    diffs = []
    for i, (a, b) in enumerate(zip(kites, spike)):
        if a == b:
            continue
        d = i - data_len
        if 0 <= d < DUMP_FPR:
            reg = d // 8 + 1
            if reg not in compare_gprs:
                continue
        if DUMP_FPR <= d < DUMP_VR:
            continue  # f registers: only loaded, never written by vector code here
        diffs.append(describe(i, data_len))
    # collapse byte-level differences into element names
    seen, unique = set(), []
    for name in diffs:
        key = name.split(" byte")[0]
        if key not in seen:
            seen.add(key)
            unique.append(key)
    return unique


# ----------------------------------------------------------------------------- random programs

INT_OPS = {  # mnemonic base -> available forms
    "vadd": "vv vx vi", "vsub": "vv vx", "vrsub": "vx vi", "vand": "vv vx vi", "vor": "vv vx vi",
    "vxor": "vv vx vi", "vsll": "vv vx vi", "vsrl": "vv vx vi", "vsra": "vv vx vi",
    "vminu": "vv vx", "vmin": "vv vx", "vmaxu": "vv vx", "vmax": "vv vx", "vmul": "vv vx",
    "vmulh": "vv vx", "vmulhu": "vv vx", "vmulhsu": "vv vx", "vdivu": "vv vx", "vdiv": "vv vx",
    "vremu": "vv vx", "vrem": "vv vx", "vsaddu": "vv vx vi", "vsadd": "vv vx vi",
    "vssubu": "vv vx", "vssub": "vv vx", "vaaddu": "vv vx", "vaadd": "vv vx", "vasubu": "vv vx",
    "vasub": "vv vx", "vsmul": "vv vx", "vssrl": "vv vx vi", "vssra": "vv vx vi",
}
UNSIGNED_IMM = {"vsll", "vsrl", "vsra", "vssrl", "vssra", "vnsrl", "vnsra", "vnclipu", "vnclip",
                "vslideup", "vslidedown", "vrgather"}
COMPARES = {"vmseq": "vv vx vi", "vmsne": "vv vx vi", "vmsltu": "vv vx", "vmslt": "vv vx",
            "vmsleu": "vv vx vi", "vmsle": "vv vx vi", "vmsgtu": "vx vi", "vmsgt": "vx vi"}
FP_BINARY = {"vfadd": "vv vf", "vfsub": "vv vf", "vfrsub": "vf", "vfmul": "vv vf",
             "vfdiv": "vv vf", "vfrdiv": "vf", "vfmin": "vv vf", "vfmax": "vv vf",
             "vfsgnj": "vv vf", "vfsgnjn": "vv vf", "vfsgnjx": "vv vf"}
FP_FMA = ["vfmacc", "vfnmacc", "vfmsac", "vfnmsac", "vfmadd", "vfnmadd", "vfmsub", "vfnmsub"]
FP_CMP = {"vmfeq": "vv vf", "vmfne": "vv vf", "vmflt": "vv vf", "vmfle": "vv vf",
          "vmfgt": "vf", "vmfge": "vf"}
SCALARS = ["t1", "t2", "t3", "t4", "t5", "t6", "a2", "a3", "a4", "a5", "a6", "a7"]
# fa4/fa5 hold NaN-boxed halves (SEW = 16), fa0/fa1 NaN-boxed singles (SEW = 32), fa2/fa3
# doubles (SEW = 64), so both simulators see the same scalar: Spike requires NaN-boxing, Kites
# reads the low SEW bits.
FSCALARS = {16: ["fa4", "fa5"], 32: ["fa0", "fa1"], 64: ["fa2", "fa3"]}
ZVKB_OPS = {"vandn": "vv vx", "vrol": "vv vx", "vror": "vv vx vi"}
ZVBB_UNARY = ["vbrev.v", "vclz.v", "vctz.v", "vcpop.v"]  # plus Zvkb's vbrev8.v, vrev8.v
GROUPS = [8, 16, 24]
MASKS = [1, 2, 3]


class Generator:
    def __init__(self, rng, target=None):
        self.r = rng
        self.t = target or TARGET
        self.sew = 8
        self.lmul = 0  # log2
        self.lines = []
        self.buf_bytes = 32 * (self.t.vlen // 8)  # covers v0..v31 and an LMUL = 8 group

    # helpers
    def regs(self, emul):
        return 1 << emul if emul > 0 else 1

    def vlmax(self):
        per = self.t.vlen // self.sew
        return per << self.lmul if self.lmul >= 0 else per >> -self.lmul

    def mask(self):
        return ", v0.t" if self.r.random() < 0.3 else ""

    def imm(self, base):
        return str(self.r.randint(0, 31) if base in UNSIGNED_IMM else self.r.randint(-16, 15))

    def emit(self, line):
        self.lines.append("    " + line)

    def distinct_groups(self, n):
        return self.r.sample(GROUPS, n)

    def set_vtype(self, sew=None, lmul=None):
        self.sew = sew or self.r.choice([w for w in (8, 16, 32, 64) if w <= self.t.elen])
        choices = [l for l in range(-3, 4) if l >= 0 or self.sew * (1 << -l) <= self.t.elen]
        self.lmul = lmul if lmul is not None else self.r.choice(choices)
        lname = f"mf{1 << -self.lmul}" if self.lmul < 0 else f"m{1 << self.lmul}"
        vlmax = self.vlmax()
        if self.r.random() < 0.3:
            self.emit(f"vsetivli t0, {self.r.randint(0, 31)}, e{self.sew}, {lname}, tu, mu")
        else:
            avl = self.r.choice([vlmax, vlmax + 5, self.r.randint(0, vlmax)])
            self.emit(f"li a0, {avl}")
            self.emit(f"vsetvli t0, a0, e{self.sew}, {lname}, tu, mu")

    # instruction families
    def integer(self):
        base = self.r.choice(list(INT_OPS))
        if self.sew == 64 and not self.t.mulh64 and (base.startswith("vmulh") or base == "vsmul"):
            return
        form = self.r.choice(INT_OPS[base].split())
        vd, vs2, vs1 = (self.r.choice(GROUPS) for _ in range(3))
        src1 = f"v{vs1}" if form == "vv" else self.r.choice(SCALARS) if form == "vx" else self.imm(base)
        self.emit(f"{base}.{form} v{vd}, v{vs2}, {src1}{self.mask()}")

    def multiply_add(self):
        base = self.r.choice(["vmacc", "vnmsac", "vmadd", "vnmsub"])
        vd, vs2, vs1 = (self.r.choice(GROUPS) for _ in range(3))
        src1 = f"v{vs1}" if self.r.random() < 0.5 else self.r.choice(SCALARS)
        form = "vv" if src1.startswith("v") else "vx"
        self.emit(f"{base}.{form} v{vd}, {src1}, v{vs2}{self.mask()}")

    def compare(self):
        base = self.r.choice(list(COMPARES))
        form = self.r.choice(COMPARES[base].split())
        vs2, vs1 = self.r.choice(GROUPS), self.r.choice(GROUPS)
        vd = self.r.choice([0] + MASKS)
        src1 = f"v{vs1}" if form == "vv" else self.r.choice(SCALARS) if form == "vx" else self.imm(base)
        self.emit(f"{base}.{form} v{vd}, v{vs2}, {src1}{self.mask()}")

    def widening(self):
        if 2 * self.sew > self.t.elen or self.lmul > 2:
            return
        base = self.r.choice(["vwaddu", "vwadd", "vwsubu", "vwsub", "vwmulu", "vwmul", "vwmulsu",
                              "vwmaccu", "vwmacc", "vwmaccsu", "vwmaccus"])
        vd, vs2, vs1 = self.distinct_groups(3)
        if base.startswith("vwmacc"):
            scalar = base == "vwmaccus" or self.r.random() < 0.5
            src1 = self.r.choice(SCALARS) if scalar else f"v{vs1}"
            self.emit(f"{base}.{'vx' if scalar else 'vv'} v{vd}, {src1}, v{vs2}{self.mask()}")
            return
        forms = ["vv", "vx"] + (["wv", "wx"] if base in ("vwaddu", "vwadd", "vwsubu", "vwsub") else [])
        form = self.r.choice(forms)
        src1 = f"v{vs1}" if form[1] == "v" else self.r.choice(SCALARS)
        self.emit(f"{base}.{form} v{vd}, v{vs2}, {src1}{self.mask()}")

    def narrowing(self):
        if 2 * self.sew > self.t.elen or self.lmul > 2:
            return
        base = self.r.choice(["vnsrl", "vnsra", "vnclipu", "vnclip"])
        form = self.r.choice(["wv", "wx", "wi"])
        vd, vs2, vs1 = self.distinct_groups(3)
        src1 = f"v{vs1}" if form == "wv" else self.r.choice(SCALARS) if form == "wx" else self.imm(base)
        self.emit(f"{base}.{form} v{vd}, v{vs2}, {src1}{self.mask()}")

    def extension(self):
        f = self.r.choice([2, 4, 8])
        if self.sew // f < 8 or self.lmul - {2: 1, 4: 2, 8: 3}[f] < -3:
            return
        vd, vs2 = self.distinct_groups(2)
        self.emit(f"{self.r.choice(['vzext', 'vsext'])}.vf{f} v{vd}, v{vs2}{self.mask()}")

    def carry(self):
        base = self.r.choice(["vadc", "vsbc", "vmadc", "vmsbc"])
        forms = {"vadc": ["vvm", "vxm", "vim"], "vsbc": ["vvm", "vxm"],
                 "vmadc": ["vvm", "vxm", "vim", "vv", "vx", "vi"], "vmsbc": ["vvm", "vxm", "vv", "vx"]}
        form = self.r.choice(forms[base])
        vs2, vs1 = self.r.choice(GROUPS), self.r.choice(GROUPS)
        vd = self.r.choice(MASKS) if base.startswith("vm") else self.r.choice(GROUPS)
        src1 = f"v{vs1}" if form[1] == "v" else self.r.choice(SCALARS) if form[1] == "x" else self.imm(base)
        self.emit(f"{base}.{form} v{vd}, v{vs2}, {src1}" + (", v0" if len(form) == 3 else ""))

    def merge_move(self):
        vd, vs2, vs1 = (self.r.choice(GROUPS) for _ in range(3))
        kind = self.r.choice(["vvm", "vxm", "vim", "v.v", "v.x", "v.i"])
        if kind == "vvm":
            self.emit(f"vmerge.vvm v{vd}, v{vs2}, v{vs1}, v0")
        elif kind == "vxm":
            self.emit(f"vmerge.vxm v{vd}, v{vs2}, {self.r.choice(SCALARS)}, v0")
        elif kind == "vim":
            self.emit(f"vmerge.vim v{vd}, v{vs2}, {self.imm('vmerge')}, v0")
        elif kind == "v.v":
            self.emit(f"vmv.v.v v{vd}, v{vs1}")
        elif kind == "v.x":
            self.emit(f"vmv.v.x v{vd}, {self.r.choice(SCALARS)}")
        else:
            self.emit(f"vmv.v.i v{vd}, {self.imm('vmv')}")

    def reduction(self):
        bases = ["vredsum", "vredand", "vredor", "vredxor", "vredminu", "vredmin", "vredmaxu", "vredmax"]
        if 2 * self.sew <= self.t.elen:
            bases += ["vwredsumu", "vwredsum"]
        base = self.r.choice(bases)
        self.emit(f"{base}.vs v{self.r.choice(MASKS)}, v{self.r.choice(GROUPS)}, "
                  f"v{self.r.choice(MASKS)}{self.mask()}")

    def mask_ops(self):
        kind = self.r.randint(0, 4)
        if kind == 0:
            op = self.r.choice(["vmand", "vmnand", "vmandn", "vmxor", "vmor", "vmnor", "vmorn", "vmxnor"])
            a, b, c = (self.r.choice([0] + MASKS) for _ in range(3))
            self.emit(f"{op}.mm v{a}, v{b}, v{c}")
        elif kind == 1:
            op = self.r.choice(["vcpop.m", "vfirst.m"])
            self.emit(f"{op} {self.r.choice(SCALARS)}, v{self.r.choice(MASKS)}{self.mask()}")
        elif kind == 2:
            vd, vs2 = self.r.sample(MASKS, 2)
            self.emit(f"{self.r.choice(['vmsbf', 'vmsif', 'vmsof'])}.m v{vd}, v{vs2}{self.mask()}")
        elif kind == 3:
            self.emit(f"viota.m v{self.r.choice(GROUPS)}, v{self.r.choice(MASKS)}{self.mask()}")
        else:
            self.emit(f"vid.v v{self.r.choice(GROUPS)}{self.mask()}")

    def permutation(self):
        kind = self.r.randint(0, 6)
        vd, vs2, vs1 = self.distinct_groups(3)
        if kind == 0:
            self.emit(f"vmv.x.s {self.r.choice(SCALARS)}, v{vs2}")
        elif kind == 1:
            self.emit(f"vmv.s.x v{vd}, {self.r.choice(SCALARS)}")
        elif kind == 2:
            op = self.r.choice(["vslideup", "vslidedown"])
            if self.r.random() < 0.5:
                self.emit(f"li a1, {self.r.randint(0, self.vlmax() + 2)}")
                self.emit(f"{op}.vx v{vd}, v{vs2}, a1{self.mask()}")
            else:
                self.emit(f"{op}.vi v{vd}, v{vs2}, {self.r.randint(0, 31)}{self.mask()}")
        elif kind == 3:
            op = self.r.choice(["vslide1up", "vslide1down"])
            self.emit(f"{op}.vx v{vd}, v{vs2}, {self.r.choice(SCALARS)}{self.mask()}")
        elif kind == 4:
            form = self.r.choice(["vv", "vx", "vi", "ei16"])
            if form == "ei16":
                emul = 4 - {8: 3, 16: 4, 32: 5, 64: 6}[self.sew] + self.lmul
                if emul < -3 or emul > 3:
                    return
                self.emit(f"vrgatherei16.vv v{vd}, v{vs2}, v{vs1}{self.mask()}")
            elif form == "vv":
                self.emit(f"vrgather.vv v{vd}, v{vs2}, v{vs1}{self.mask()}")
            elif form == "vx":
                self.emit(f"li a1, {self.r.randint(0, self.vlmax() + 1)}")
                self.emit(f"vrgather.vx v{vd}, v{vs2}, a1{self.mask()}")
            else:
                self.emit(f"vrgather.vi v{vd}, v{vs2}, {self.r.randint(0, 31)}{self.mask()}")
        elif kind == 5:
            self.emit(f"vcompress.vm v{vd}, v{vs2}, v{self.r.choice(MASKS)}")
        else:
            n = self.r.choice([1, 2, 4, 8])
            self.emit(f"vmv{n}r.v v{vd}, v{vs2}")

    def fixed_point_mode(self):
        self.emit(f"csrwi vxrm, {self.r.randint(0, 3)}")

    def floating(self):
        if self.sew not in self.t.fp:
            return
        kind = self.r.randint(0, 9)
        vd, vs2, vs1 = self.distinct_groups(3)
        fs = self.r.choice(FSCALARS[self.sew])
        if kind == 0:
            self.emit(f"csrwi frm, {self.r.randint(0, 3)}")
        elif kind == 1:
            base = self.r.choice(list(FP_BINARY))
            form = self.r.choice(FP_BINARY[base].split())
            self.emit(f"{base}.{form} v{vd}, v{vs2}, {'v%d' % vs1 if form == 'vv' else fs}{self.mask()}")
        elif kind == 2:
            base = self.r.choice(FP_FMA)
            src1 = f"v{vs1}" if self.r.random() < 0.5 else fs
            self.emit(f"{base}.{'vv' if src1[0] == 'v' else 'vf'} v{vd}, {src1}, v{vs2}{self.mask()}")
        elif kind == 3:
            op = self.r.choice(["vfsqrt.v", "vfrec7.v", "vfrsqrt7.v", "vfclass.v"])
            self.emit(f"{op} v{vd}, v{vs2}{self.mask()}")
        elif kind == 4:
            base = self.r.choice(list(FP_CMP))
            form = self.r.choice(FP_CMP[base].split())
            self.emit(f"{base}.{form} v{self.r.choice([0] + MASKS)}, v{vs2}, "
                      f"{'v%d' % vs1 if form == 'vv' else fs}{self.mask()}")
        elif kind == 5:
            choice = self.r.randint(0, 3)
            if choice == 0:
                self.emit(f"vfmerge.vfm v{vd}, v{vs2}, {fs}, v0")
            elif choice == 1:
                self.emit(f"vfmv.v.f v{vd}, {fs}")
            elif choice == 2:
                self.emit(f"vfmv.s.f v{vd}, {fs}")
            else:
                op = self.r.choice(["vfslide1up", "vfslide1down"])
                self.emit(f"{op}.vf v{vd}, v{vs2}, {fs}{self.mask()}")
        elif kind == 6:
            op = self.r.choice(["xu.f.v", "x.f.v", "rtz.xu.f.v", "rtz.x.f.v", "f.xu.v", "f.x.v"])
            self.emit(f"vfcvt.{op} v{vd}, v{vs2}{self.mask()}")
        elif kind == 7:
            self.conversion(vd, vs2)
        elif kind == 8:
            bases = ["vfredosum", "vfredusum", "vfredmin", "vfredmax"]
            if 2 * self.sew in self.t.fp:
                bases += ["vfwredosum", "vfwredusum"]
            self.emit(f"{self.r.choice(bases)}.vs v{self.r.choice(MASKS)}, v{vs2}, "
                      f"v{self.r.choice(MASKS)}{self.mask()}")
        else:
            if 2 * self.sew not in self.t.fp or self.lmul > 2:
                return
            base = self.r.choice(["vfwadd", "vfwsub", "vfwmul", "vfwmacc", "vfwnmacc",
                                  "vfwmsac", "vfwnmsac"])
            if base in ("vfwadd", "vfwsub"):
                form = self.r.choice(["vv", "vf", "wv", "wf"])
            else:
                form = self.r.choice(["vv", "vf"])
            src1 = f"v{vs1}" if form[1] == "v" else fs
            if "macc" in base or "msac" in base:
                self.emit(f"{base}.{form} v{vd}, {src1}, v{vs2}{self.mask()}")
            else:
                self.emit(f"{base}.{form} v{vd}, v{vs2}, {src1}{self.mask()}")

    def conversion(self, vd, vs2):
        """vfcvt at any SEW is handled in floating(); this covers the widening and narrowing
        conversions that are legal for the current SEW and target."""
        if self.lmul > 2 or 2 * self.sew > self.t.elen:
            return
        s, w = self.sew, 2 * self.sew
        ops = []
        if w in self.t.fp:
            ops += ["vfwcvt.f.xu.v", "vfwcvt.f.x.v"]            # int s -> float 2s
            ops += ["vfncvt.xu.f.w", "vfncvt.x.f.w", "vfncvt.rtz.xu.f.w", "vfncvt.rtz.x.f.w"]
        if s in self.t.fp:
            ops += ["vfwcvt.xu.f.v", "vfwcvt.x.f.v", "vfwcvt.rtz.xu.f.v", "vfwcvt.rtz.x.f.v"]
            ops += ["vfncvt.f.xu.w", "vfncvt.f.x.w"]            # int 2s -> float s
        if s in self.t.fp and w in self.t.fp:
            ops += ["vfwcvt.f.f.v", "vfncvt.f.f.w", "vfncvt.rod.f.f.w"]
        if ops:
            self.emit(f"{self.r.choice(ops)} v{vd}, v{vs2}{self.mask()}")

    def bitmanip(self):
        if not (self.t.zvkb or self.t.zvbc):
            return
        vd, vs2, vs1 = self.distinct_groups(3)
        choices = []
        if self.t.zvkb:
            choices += ["zvkb"] * 3
        if self.t.zvbb:
            choices += ["unary", "unary", "vwsll"]
        if self.t.zvbc and self.sew == 64:
            choices += ["clmul", "clmul"]
        if not choices:
            return
        kind = self.r.choice(choices)
        if kind == "zvkb":
            base = self.r.choice(list(ZVKB_OPS))
            form = self.r.choice(ZVKB_OPS[base].split())
            src1 = (f"v{vs1}" if form == "vv" else self.r.choice(SCALARS) if form == "vx"
                    else str(self.r.randint(0, 63)))
            if self.r.random() < 0.3:
                self.emit(f"{self.r.choice(['vbrev8.v', 'vrev8.v'])} v{vd}, v{vs2}{self.mask()}")
            else:
                self.emit(f"{base}.{form} v{vd}, v{vs2}, {src1}{self.mask()}")
        elif kind == "unary":
            self.emit(f"{self.r.choice(ZVBB_UNARY)} v{vd}, v{vs2}{self.mask()}")
        elif kind == "vwsll":
            if 2 * self.sew > self.t.elen or self.lmul > 2:
                return
            form = self.r.choice(["vv", "vx", "vi"])
            src1 = (f"v{vs1}" if form == "vv" else self.r.choice(SCALARS) if form == "vx"
                    else str(self.r.randint(0, 31)))
            self.emit(f"vwsll.{form} v{vd}, v{vs2}, {src1}{self.mask()}")
        else:
            op = self.r.choice(["vclmul", "vclmulh"])
            form = self.r.choice(["vv", "vx"])
            src1 = f"v{vs1}" if form == "vv" else self.r.choice(SCALARS)
            self.emit(f"{op}.{form} v{vd}, v{vs2}, {src1}{self.mask()}")

    def memory(self):
        kind = self.r.randint(0, 5)
        eew = self.r.choice([w for w in (8, 16, 32, 64) if w <= self.t.elen])
        emul = {8: 3, 16: 4, 32: 5, 64: 6}[eew] - {8: 3, 16: 4, 32: 5, 64: 6}[self.sew] + self.lmul
        bytes_ = eew // 8
        vd = self.r.choice(GROUPS)
        if kind <= 2:  # unit-stride / fault-only-first / strided, possibly segmented
            if emul < -3 or emul > 3:
                return
            nf = self.r.choice([1, 1, 2, 3])
            if nf * self.regs(emul) > 8:
                nf = 1
            seg = "" if nf == 1 else f"seg{nf}"
            mask = self.mask()
            if kind == 0:
                ff = "ff" if self.r.random() < 0.3 else ""
                self.emit(f"vl{seg}e{eew}{ff}.v v{vd}, (s1){mask}")
                self.emit(f"vs{seg}e{eew}.v v{vd}, (s2){mask}")
            else:
                vlmax = self.vlmax()
                span = max(1, (self.buf_bytes - 112 - nf * bytes_) // max(1, vlmax - 1))
                # Element-aligned strides: Spike traps on misaligned element accesses (Kites
                # allows them; both are legal), so random programs avoid them.
                stride = self.r.randint(0, span // bytes_) * bytes_
                self.emit(f"li a1, {stride}")
                self.emit(f"vls{seg}e{eew}.v v{vd}, (s1), a1{mask}")
                self.emit(f"vss{seg}e{eew}.v v{vd}, (s2), a1{mask}")
        elif kind == 3:  # indexed with in-range offsets built from vid
            vi, vdata = self.distinct_groups(2)
            order = self.r.choice(["u", "o"])
            self.emit(f"vid.v v{vi}")
            self.emit(f"vsll.vi v{vi}, v{vi}, {({8: 0, 16: 1, 32: 2, 64: 3})[self.sew]}")
            self.emit(f"vl{order}xei{self.sew}.v v{vdata}, (s1), v{vi}{self.mask()}")
            self.emit(f"vs{order}xei{self.sew}.v v{vdata}, (s2), v{vi}{self.mask()}")
        elif kind == 4:
            m = self.r.choice(MASKS)
            self.emit(f"vlm.v v{m}, (s1)")
            self.emit(f"vsm.v v{m}, (s2)")
        else:
            n = self.r.choice([1, 2, 4, 8])
            self.emit(f"vl{n}re{eew}.v v{vd}, (s1)")
            self.emit(f"vs{n}r.v v{vd}, (s2)")

    def program(self, length):
        data = []
        specials = [0, 0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000,
                    0x7F8000017FC00000, 0x3FF0000000000000, 0x0000000000000001,
                    0x8000000080000000, 0x3F8000003F800000, 0x80000000FF800000]
        dwords = self.buf_bytes // 8
        halves = [0x3C00, 0x7E00, 0x7C01, 0x0001, 0x8000, 0x7BFF, 0x3555]
        specials += [0x3C00BC0000017C00, 0x7BFF7C0103FF8001] if self.t.zvfh else []
        words = [self.r.choice(specials) if self.r.random() < 0.2 else self.r.getrandbits(64)
                 for _ in range(dwords)]
        data.append("buf: .dword " + ", ".join(str(w) for w in words))
        data.append("out: .dword " + ", ".join("0" for _ in range(dwords)))
        singles = [self.r.choice([0x3F800000, 0x7FC00000, 0x7F800001, 0x00000001, 0x80000000,
                                  self.r.getrandbits(32)]) for _ in range(2)]
        hs = [self.r.choice(halves + [self.r.getrandbits(16)]) for _ in range(2)]
        data.append("fbox: .dword " + ", ".join([str(0xFFFFFFFF00000000 | v) for v in singles] +
                                                 [str(0xFFFFFFFFFFFF0000 | v) for v in hs]))
        self.emit("la s1, buf")
        self.emit("la s2, out")
        for g in (0, 8, 16, 24):
            self.emit(f"li t0, {g * (self.t.vlen // 8)}")  # may exceed the addi range
            self.emit("add t0, s1, t0")
            self.emit(f"vl8re8.v v{g}, (t0)")
        for i in range(32):
            self.emit(f"fld f{i}, {8 * i}(s1)")
        self.emit("la t0, fbox")
        self.emit("fld fa0, 0(t0)")
        self.emit("fld fa1, 8(t0)")
        self.emit("fld fa4, 16(t0)")
        self.emit("fld fa5, 24(t0)")
        for reg in SCALARS:
            self.emit(f"li {reg}, {self.r.randint(-2**31, 2**31 - 4097)}")
        self.set_vtype()
        self.prologue_lines = len(self.lines)
        families = [self.integer] * 6 + [self.multiply_add, self.compare, self.compare,
                    self.widening, self.narrowing, self.extension, self.carry, self.merge_move,
                    self.reduction, self.mask_ops, self.mask_ops, self.permutation,
                    self.permutation, self.fixed_point_mode] + [self.floating] * 5 + \
                   [self.memory] * 3 + [self.set_vtype] * 3 + [self.bitmanip] * 3
        for _ in range(length):
            self.r.choice(families)()
        return data, self.lines


# ----------------------------------------------------------------------------- driver


def find_tools(args):
    llvm = subprocess.run(["brew", "--prefix", "llvm"], capture_output=True, text=True).stdout.strip()
    tools = {
        "llvm-mc": shutil.which("llvm-mc") or os.path.join(llvm, "bin", "llvm-mc"),
        "ld.lld": shutil.which("ld.lld"),
        "spike": shutil.which("spike"),
    }
    for name, path in tools.items():
        if not path or not os.path.exists(path):
            sys.exit(f"{name} not found")
    return tools


def default_state_dir():
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/Kites/vm_state")
    return os.path.join(os.getcwd(), "vm_state")


def run_one(name, data, text, args, tools, compare_gprs):
    pad = -data_size(data) % 8  # the dump area must be 8-byte aligned in both layouts
    if pad:
        data = data + [f"kpad: .zero {pad}"]
    length = data_size(data) + DUMP_SIZE
    data_len = length - DUMP_SIZE
    with tempfile.TemporaryDirectory() as work:
        def keep():
            if args.keep:
                os.makedirs(args.keep, exist_ok=True)
                open(os.path.join(args.keep, f"{name}.s"), "w").write(kites_source(data, text))
                open(os.path.join(args.keep, f"{name}.spike.s"), "w").write(spike_source(data, text))

        k, err = run_kites(args.kites, args.vm, kites_source(data, text), length, work, args.state_dir)
        if err:
            keep()
            return f"{name}: kites error: {err}"
        s, err = run_spike(tools, spike_source(data, text), length, work)
        if err:
            keep()
            return f"{name}: spike error: {err}"
        diffs = compare(k, s, data_len, compare_gprs)
        if diffs:
            keep()
            return f"{name}: MISMATCH in " + ", ".join(diffs[:12])
    return None


def bisect(name, data, text, args, tools, gprs, start):
    """Finds the shortest prefix of the program whose final state differs."""
    lo, hi = start, len(text)  # the prologue passes; the whole program fails
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if mid < start or run_one(name, data, text[:mid], args, tools, gprs):
            hi = mid
        else:
            lo = mid
    context = [line.strip() for line in text[max(0, hi - 6):hi]]
    print(f"  first diverging line {hi}: {text[hi - 1].strip()}")
    print("  context: " + " | ".join(context))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--kites", required=True, help="path to kites-cli")
    parser.add_argument("--vm", default="rvss",
                        help="Kites processor model: rvss (default), rv5-h-nf or rv5-h-f. The "
                             "models without hazard detection need NOPs that the programs lack.")
    parser.add_argument("--vlen", type=int, default=128, choices=[128, 256, 512, 1024])
    parser.add_argument("--vector-ext", default="v",
                        help="profile and extensions, as kites-cli --vector-ext "
                             "(e.g. v,zvfh,zvbb,zvbc or zve32f)")
    parser.add_argument("--state-dir", default=default_state_dir())
    parser.add_argument("--keep", help="directory to save failing programs")
    sub = parser.add_subparsers(dest="mode", required=True)
    ex = sub.add_parser("examples")
    ex.add_argument("files", nargs="+")
    rnd = sub.add_parser("random")
    rnd.add_argument("--count", type=int, default=100)
    rnd.add_argument("--seed", type=int, default=1)
    rnd.add_argument("--length", type=int, default=40)
    rnd.add_argument("--bisect", action="store_true",
                     help="for each failing seed, report the first diverging line")
    args = parser.parse_args()
    global TARGET
    TARGET = Target(args.vlen, args.vector_ext)
    configure_layout(args.vlen)
    tools = find_tools(args)

    failures = []
    total = 0
    if args.mode == "examples":
        for path in args.files:
            data, text = split_program(path)
            total += 1
            msg = run_one(os.path.basename(path), data, text, args, tools, compare_gprs=set())
            print(msg or f"{os.path.basename(path)}: ok")
            if msg:
                failures.append(msg)
    else:
        gprs = {ABI.index(r) for r in SCALARS + ["t0", "a0", "a1"]}
        for i in range(args.count):
            seed = args.seed + i
            generator = Generator(random.Random(seed), TARGET)
            data, text = generator.program(args.length)
            total += 1
            msg = run_one(f"seed{seed}", data, text, args, tools, compare_gprs=gprs)
            if msg:
                failures.append(msg)
                print(msg)
                if args.bisect:
                    bisect(f"seed{seed}", data, text, args, tools, gprs, generator.prologue_lines)
    print(f"{total - len(failures)}/{total} programs match Spike")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
