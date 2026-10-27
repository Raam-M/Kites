# RISC-V Vector Extension (RVV 1.0)

Kites implements the RISC-V **V** extension, version 1.0. All 627 vector
instructions assemble, disassemble and execute, together with the standard
vector pseudo-instructions. Kites also implements the embedded profiles
Zve64d/f/x and Zve32f/x and the extensions Zvfh, Zvbb, Zvkb and Zvbc.

| Parameter | Value |
|-----------|-------|
| VLEN (register width) | 128 bits by default; 256, 512 or 1024 selectable |
| ELEN (largest element) | 64 bits (32 for the Zve32 profiles) |
| SEW (element width) | 8, 16, 32, 64 |
| LMUL (register grouping) | 1/8, 1/4, 1/2, 1, 2, 4, 8 |
| Floating point | SEW = 32 (F) and SEW = 64 (D); SEW = 16 with Zvfh |
| Processor models | All: the single-cycle processor and the four 5-stage pipelines |

The design is described in the [design document](vector_extension/design_document.pdf),
and the evidence that it works in the [validation report](vector_extension/validation_report.pdf).

## Programming model

- 32 vector registers `v0`–`v31`, each VLEN bits wide. Element 0 occupies the
  least significant bits.
- `vsetvli`/`vsetivli`/`vsetvl` choose SEW and LMUL and set
  `vl = min(AVL, VLMAX)`, where `VLMAX = LMUL × VLEN / SEW`.
- With LMUL > 1, an operand is a group of registers whose first register must be
  a multiple of LMUL.
- `v0.t` masks an instruction: element *i* is processed only if bit *i* of `v0`
  is set.
- Masked-off and tail elements are left **undisturbed** by default, including
  when `ta`/`ma` is requested. An option fills agnostic elements with all ones
  instead (see [Configuration](#configuration)). Both behaviours are allowed by
  the spec.

### Vector CSRs

| CSR | Address | Access | Contents |
|-----|---------|--------|----------|
| `vstart` | 0x008 | read/write | first element to process (reset to 0 after every vector instruction) |
| `vxsat` | 0x009 | read/write | fixed-point saturation flag |
| `vxrm` | 0x00A | read/write | fixed-point rounding mode: 0 rnu, 1 rne, 2 rdn, 3 rod |
| `vcsr` | 0x00F | read/write | `vxrm` in bits 2:1, `vxsat` in bit 0 |
| `vl` | 0xC20 | read-only | current vector length |
| `vtype` | 0xC21 | read-only | current vector type |
| `vlenb` | 0xC22 | read-only | VLEN / 8 |

After a reset, `vtype.vill = 1` and `vl = 0`, so a `vset{i}vl{i}` must run before
other vector instructions. Read CSRs with `csrr`, for example `csrr t0, vlenb`.
Writing `vl`, `vtype` or `vlenb` is an illegal instruction.

## Assembly syntax

The syntax matches GNU `as` and LLVM:

```asm
vsetvli  t0, a0, e32, m2, ta, ma    # LMUL defaults to m1; policies default to tu, mu
vsetivli t0, 8, e8, mf2, tu, mu
vle32.v  v4, (a1)                   # unit-stride load
vle32.v  v4, (a1), v0.t             # ... masked
vlse64.v v8, (a1), t1               # strided load, stride in bytes
vluxei32.v v8, (a1), v4             # indexed load, byte offsets in v4
vlseg3e8.v v0, (a1)                 # segment load: fields to v0, v1, v2
vadd.vv  v1, v2, v3                 # vd, vs2, vs1
vadd.vx  v1, v2, a0
vadd.vi  v1, v2, -5                 # simm5: -16..15 (shift/slide/gather immediates: 0..31)
vmacc.vv v1, v2, v3                 # multiply-add: vd, vs1, vs2
vfmacc.vf v1, fa0, v2
vmerge.vvm v1, v2, v3, v0           # v0 selects, it is not a mask here
vmv.x.s  a0, v1
vror.vi  v1, v2, 40                 # Zvkb: 6-bit rotate amount, 0..63
```

Pseudo-instructions: `vneg.v`, `vnot.v`, `vwcvt(u).x.x.v`, `vncvt.x.x.w`,
`vfneg.v`, `vfabs.v`, `vmmv.m`, `vmnot.m`, `vmclr.m`, `vmset.m`,
`vmsgt(u).vv`, `vmsge(u).vv`, `vmfgt.vv`, `vmfge.vv`, `vmslt(u).vi`,
`vmsge(u).vi`, `vmsge(u).vx` (including the masked and temporary-register forms),
and `vl1r.v`/`vl2r.v`/`vl4r.v`/`vl8r.v`. Kites also accepts the CSR pseudo-instructions
`csrr`, `csrw`, `csrs`, `csrc`, `csrwi`, `csrsi` and `csrci`.

If an instruction has the wrong operands, the error message shows its expected
syntax, for example `vadd.vv vd, vs2, vs1[, v0.t]`.

Every vector instruction assembles whatever the configuration; whether it is
legal (for example a Zvbb instruction, or SEW = 64 under Zve32x) is decided when
it executes.

## Configuration

| Option | Values | GUI (*Settings → Vector Extension*) | `kites-cli` flag | REPL |
|--------|--------|------|------|------|
| VLEN | 128, 256, 512, 1024 | VLEN | `--vlen 256` | `vconfig vlen 256` |
| Agnostic elements | undisturbed, ones | Agnostic elements | `--vector-agnostic ones` | `vconfig agnostic ones` |
| Profile and extensions | see below | Profile, Extensions | `--vector-ext zve64f,zvfh` | `vconfig ext v,zvbb` |
| Timing model lanes | 1, 2, 4, …, 32 | Timing model lanes | | `vconfig lanes 8` |
| Chaining | on, off | Chain dependent instructions | | `vconfig chaining off` |

A new VLEN applies at the next reset or assemble (the REPL resets the VM when you
change it). The other options apply to the next instruction. GUI settings are
saved and restored at start-up.

| Profile | ELEN | FP element widths | `vmulh*`, `vsmul` at SEW = 64 |
|---------|------|-------------------|-------------------------------|
| `v` (default) | 64 | 32, 64 | yes |
| `zve64d` | 64 | 32, 64 | no |
| `zve64f` | 64 | 32 | no |
| `zve64x` | 64 | none | no |
| `zve32f` | 32 | 32 | – |
| `zve32x` | 32 | none | – |

| Extension | Adds |
|-----------|------|
| `zvfh` | SEW = 16 floating point (needs an F profile) |
| `zvkb` | `vandn`, `vrol`, `vror`, `vbrev8`, `vrev8` |
| `zvbb` | Zvkb plus `vbrev`, `vclz`, `vctz`, `vcpop.v`, `vwsll` |
| `zvbc` | `vclmul`, `vclmulh` (SEW = 64; needs ELEN = 64) |

## Pipelines

On the 5-stage models, vector and CSR instructions pass through IF–MEM without
effect and execute in **WB**, after every older instruction has written back.
They read their scalar operands there, so they never wait for older
instructions, and their memory accesses stay in program order with scalar loads
and stores.

A scalar instruction that reads a result of a vector or CSR instruction
(`vsetvl` rd, `vmv.x.s`, `vcpop.m`, `vfirst.m`, `vfmv.f.s`, `csrr*`) cannot get it
by forwarding:

- the models with hazard detection (`H_NF`, `H_F`) stall it until the result is
  written (two cycles for the instruction right after);
- on the models without hazard detection (`NH_NF`, `NH_F`), put two other
  instructions (or NOPs) in between, as for a load without forwarding.

A scalar floating-point instruction also waits for an older vector or CSR
instruction, so `frm` and `fflags` are used in program order. CSR instructions,
which used to be ignored on the pipelines, now execute too. The circuit view
shows a vector unit whose wires light up while a vector instruction is in WB.

## Errors at run time

Illegal vector usage stops the program with a message that names the rule
broken. The PC stays on the offending instruction, and no register, CSR or memory
location is changed. Examples:

- a vector instruction executed while `vtype.vill = 1`
- a register group not aligned to LMUL (for example `v3` with LMUL = 2)
- an EMUL outside 1/8..8, or more than 8 registers in a segment access
- forbidden overlaps: a masked destination overlapping `v0`, widening and
  narrowing overlap rules, `vrgather`/`vslideup`/`vcompress` source and destination
- a non-zero `vstart` for reductions, `vcpop`, `vfirst`, `viota`, `vmsbf`/`vmsif`/`vmsof` and `vcompress`
- a floating-point SEW the profile does not support (SEW = 8 always; SEW = 16
  without Zvfh); a reserved rounding mode in `frm`
- an instruction of an extension that is not enabled, an EEW above ELEN, or a
  64-bit `vmulh`/`vsmul` under a Zve64 profile

## Where to see the state

- **Vector tab** (sidebar): `v0`–`v31` split into elements at the current SEW
  (or a chosen one) as hex, unsigned, signed or floating point. Tail elements are
  dimmed, masked-off elements can be shaded, LMUL groups are banded, and elements
  changed by the last step are highlighted. The vector CSRs are shown above the
  table, and the statistics and timing estimate below it.
- **Register panel:** the *Vector* section shows `v0`–`v31` in hex, with
  element 0 in the rightmost digits. The *Vector CSRs* section shows `vl`,
  `vtype` (decoded, e.g. `e32, m1, ta, ma`), `vstart`, `vxrm`, `vxsat`, `vcsr`
  and `vlenb`.
- **REPL** (`kites-cli -i`): `vregs [e8|e16|e32|e64] [hex|u|s|f]`,
  `vreg v8 e32 f`, `vcsr`, `vconfig`, `vstats [json]`.
- **`registers_dump.json`:** written by the GUI and the CLI. It contains the vector
  CSRs and a `vector_registers` section: each register as one hexadecimal number,
  most significant byte first, without leading zeros (so `0x0` for an all-zero
  register).
- **`vector_stats.json`:** written by `kites-cli` after a batch run.
- **Disassembly:** vector instructions show all their operands.
- **Profiler:** vector instructions are counted as `V-Type`.
- **Undo/redo:** works for vector instructions, including vector registers,
  vector CSRs, memory written by vector stores, and the statistics.

## Statistics and timing estimate

Every processor keeps vector statistics: instructions per class and per
mnemonic, the average `vl` and `vl / VLMAX`, active and masked-off elements,
bytes loaded and stored, and SEW/LMUL use.

The timing estimate models a vector unit that issues vector instructions in
order, one per cycle, with one pipelined unit per class (integer ALU, multiply,
divide, FP add, FP multiply, FP divide, load, store, reduction, permute, mask) and
*L* lanes. An instruction occupies its unit for ⌈elements / *L*⌉ cycles (strided
and indexed accesses move one element per cycle) after a start-up latency
(add 6, multiply 7, divide 20, load/store 12 cycles, following the VMIPS example
of Hennessy & Patterson). With chaining, a dependent instruction may start at
the producer's first result; without it, after the last. The report gives the
estimated cycles, the number of convoys (chimes) and the busy cycles of each
unit. It is an estimate, separate from the processors' cycle counts.

## Custom pseudo-instructions

Expansions in *Settings → Custom Pseudo Instructions* may use vector
instructions. Arguments (`r1`…) can stand for vector, integer or FP registers and
may appear inside a memory operand:

```
vcopy32 r1, r2, r3   →   vsetvli x0 r3 e32 m1 ta ma
                         vle32.v v8 (r1)
                         vse32.v v8 (r2)
```

Each operand is checked against the instruction's syntax when the
pseudo-instruction is saved.

## Behaviour chosen where the spec allows a choice

- `vl = min(AVL, VLMAX)` for every AVL.
- Agnostic elements are left undisturbed (unless the all-ones option is selected).
- Elements are accessed in increasing order, one memory access per element.
  Indexed-unordered accesses therefore behave like ordered ones. Misaligned
  element addresses are allowed.
- `vfredusum`/`vfwredusum` add in element order, like the ordered forms. With no
  active element, a NaN scalar becomes the canonical NaN (the same choice as Spike).
- `vsetvli x0, x0, ...` that would change VLMAX sets `vill`.
- A fault-only-first load treats an address beyond the end of memory as a fault.

## Known differences from the specification

- `frm = RMM` rounds ties to even in floating-point arithmetic. Conversions to
  integer do honour RMM.
- Single- and half-precision scalars are stored in `f` registers zero-extended,
  as in Kites' F extension, rather than NaN-boxed. `vfmv.f.s` follows the same
  convention, and `.vf` operands use the low SEW bits.
- For SEW = 32 and 64, underflow detection follows the host processor's tininess
  rule. Half precision detects tininess after rounding, as RISC-V requires.

## Examples

The programs in [`examples/vector/`](../examples/vector) are also run as tests,
at every VLEN and on the pipelines:

| Program | Shows |
|---------|-------|
| `vvadd_i32.s` | strip-mined vector add |
| `memcpy.s` | byte copy with e8/m8 |
| `dot_product.s` | `vmul` + `vredsum` |
| `masked_abs.s` | masking with `vmslt` and `v0.t` |
| `gather_scatter.s` | indexed loads and stores |
| `prefix_sum.s` | log-step scan with `vslideup` |
| `matmul_i32.s` | matrix multiply, vector and scalar versions |
| `saxpy_f32.s` | `vfmacc.vf` |
| `strlen.s` | fault-only-first load, `vmseq` + `vfirst` |
| `rgb_to_grey.s` | segment loads, widening multiply-add, narrowing shift |
| `fir_q15.s` | fixed point: `vsmul`, `vsadd`, `vxrm`, `vxsat` |

## For developers

- Encodings are generated from riscv-opcodes (`rv_v`, `rv_zvbb`, `rv_zvbc`) by
  [`tools/vector/gen_vector_isa.py`](../tools/vector/gen_vector_isa.py) into
  `src/processor/vector/vector_isa_table.inc`.
- `src/processor/vector/` holds the ISA table (`vector_isa.*`), `vtype` helpers,
  the configuration (`vector_config.*`), the dispatcher, the legality checks and the
  atomic commit (`vector_unit.*`), the instruction groups
  (`vector_exec_integer.cpp`, `vector_exec_memory.cpp`, `vector_exec_float.cpp`),
  the statistics and timing model (`vector_profiler.*`) and element formatting
  (`vector_format.*`).
- `src/assembler/parse_formats/v_formats.cpp` parses every vector instruction from
  the operand pattern in the table.
- The pipelines execute vector and CSR instructions in
  `RV5StageVM_Base::pipeline_writeback`; the stall rule is
  `writeback_dependency_stall`.
- Tests are in `test/vector/`; differential testing against Spike is
  `tools/vector/spike_diff.py`. See [TESTING.md](TESTING.md#vector-extension-tests).
