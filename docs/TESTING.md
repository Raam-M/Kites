# Running Tests

Kites uses [GoogleTest](https://github.com/google/googletest) for unit testing, integrated with CMake/CTest. Two test targets are available:

- **`kites_test`** — plain test build, no instrumentation. Use this for everyday local testing.
- **`kites_test_cov`** — same test suite, built with coverage instrumentation (`--coverage`), plus a `coverage` target that generates an HTML report via [`gcovr`](https://gcovr.com/).

Both targets are produced from a single configure step; you choose which one to build/run afterward.

## Prerequisites

- CMake 3.24+
- Ninja
- A GCC- or Clang-style compiler (coverage instrumentation requires this — see [Notes](#notes))
- [`gcovr`](https://gcovr.com/) on your `PATH` (only required if configuring with `ENABLE_COVERAGE=ON`)

## Configure

Both test targets live under the same configure preset:

```sh
cmake --preset test
```

## Running tests (no coverage)

```sh
cmake --build --preset test-only
ctest --preset test-only
```

This builds and runs `kites_test` only — the fast path for everyday local test runs.

## Running tests (with coverage)

```sh
cmake --build --preset test-coverage
ctest --preset test-coverage
```

This builds and runs `kites_test_cov`, whose CTest test names are prefixed with `cov_` so they don't collide with the plain suite's test names.

### Generating the HTML coverage report

The `coverage` target runs the instrumented test suite via CTest and then invokes `gcovr` to produce an HTML report:

```sh
cmake --build build/coverage --target coverage
```

The report is written to `build/coverage/coverage/index.html`.

## Build options

Set via `-D<OPTION>=ON|OFF` at configure time, or in `CMakePresets.json`:

| Option              | Default | Description                                      |
|---------------------|---------|---------------------------------------------------|
| `DISABLE_GUI`        | `ON`    | Disable GUI components during tests               |
| `VM_DEBUG_PRINTS`     | `OFF`   | Enable debug prints in the VM during tests        |
| `ENABLE_COVERAGE`     | `OFF`   | Build the `kites_test_cov` target and `coverage` target |

## Vector extension tests

The vector tests are in `test/vector/` and run as part of `kites_test`. To run
only them:

```sh
cd test
../build/<preset>/test/kites_test --gtest_filter='Vec*'
```

| File | Checks |
|------|--------|
| `vector_assembler_test.cpp` | Every vector mnemonic and pseudo-instruction encodes exactly as LLVM `llvm-mc` encodes it; disassembly round trip; syntax and range errors |
| `vector_config_test.cpp` | `vset{i}vl{i}`, `vill`, reset state, vector CSRs |
| `vector_integer_test.cpp` | Integer and fixed-point arithmetic for every legal SEW/LMUL, masked and unmasked, against an independent reference model (`vector_reference.h`) |
| `vector_permute_test.cpp` | Reductions, mask instructions, slides, gathers, compress, moves |
| `vector_memory_test.cpp` | Unit-stride, strided, indexed, segment, mask, whole-register and fault-only-first accesses |
| `vector_float_test.cpp` | Floating-point arithmetic, compares, conversions, estimates, flags |
| `vector_half_test.cpp` | Half precision (Zvfh) in every rounding mode against the hardware's `_Float16`; element formatting |
| `vector_extensions_test.cpp` | Zvbb/Zvkb, Zvbc, Zve* profiles, option parsing, VLEN 128–1024, the all-ones agnostic fill |
| `vector_legality_test.cpp` | Illegal usage is rejected with state unchanged; undo/redo |
| `vector_pipeline_test.cpp` | Vector and CSR instructions on the four 5-stage models: results equal the single-cycle model, stalls, NOP rule, memory order, flushes, errors in WB, undo/redo |
| `vector_profiler_test.cpp` | Statistics and the timing model (hand-computed schedules) |
| `vector_custom_pseudo_test.cpp` | Vector instructions in custom pseudo-instructions |
| `vector_examples_test.cpp` | The programs in `examples/vector/` |
| `../rv5s_regression_test.cpp` | Scalar pipeline fixes found while running vector code (AUIPC/LUI, FP forwarding, draining) |

Test names start with the requirement ID they verify (for example
`VM01_indexed` checks requirement VM-01 of the scope document).

### Differential testing against Spike

`tools/vector/spike_diff.py` runs the same program on Kites (through `kites-cli`)
and on [Spike](https://github.com/riscv-software-src/riscv-isa-sim), then compares
data memory, vector registers, vector CSRs, `fflags` and the scalar registers
the program uses. It needs `spike`, `llvm-mc` and `ld.lld`
(`brew install riscv-isa-sim llvm lld`).

```sh
# the example programs
python3 -I tools/vector/spike_diff.py --kites build/<preset>/kites-cli examples examples/vector/*.s
# seeded random programs; --bisect reports the first diverging line of a failure
python3 -I tools/vector/spike_diff.py --kites build/<preset>/kites-cli random --count 300 --seed 1
# another processor model, VLEN and set of extensions
python3 -I tools/vector/spike_diff.py --kites build/<preset>/kites-cli --vm rv5-h-f \
    --vlen 512 --vector-ext v,zvfh,zvbb,zvbc random --count 100
```

`--keep DIR` saves failing programs. The models without hazard detection
(`rv5-nh-*`) need NOPs that random programs do not contain, so use the
single-cycle model or `rv5-h-nf`/`rv5-h-f` for random programs.

### Coverage with Apple Clang

The `coverage` target links GCC's `libgcov`, which Apple Clang does not have.
Use clang's source-based coverage instead:

```sh
cmake -S . -B build/cov -G Ninja -DENABLE_TESTS=ON -DDISABLE_GUI=ON \
    -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" \
    -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
    -DCMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
    -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate"
cmake --build build/cov --target kites_test
(cd build/cov && LLVM_PROFILE_FILE="$PWD/prof/%p.profraw" ctest)
xcrun llvm-profdata merge -sparse build/cov/prof/*.profraw -o build/cov/kites.profdata
xcrun llvm-cov report build/cov/test/kites_test -instr-profile=build/cov/kites.profdata \
    src/processor/vector src/assembler/parse_formats/v_formats.cpp
```

### Regenerating generated files

The encoding table and the golden encodings are generated from
[riscv-opcodes](https://github.com/riscv/riscv-opcodes) (`extensions/rv_v`,
`rv_zvbb`, `rv_zvbc`) and LLVM:

```sh
python3 -I tools/vector/gen_vector_isa.py src/processor/vector/vector_isa_table.inc \
    <riscv-opcodes commit> <riscv-opcodes>/extensions/rv_v \
    <riscv-opcodes>/extensions/rv_zvbb <riscv-opcodes>/extensions/rv_zvbc
python3 -I tools/vector/gen_golden_encodings.py "$(brew --prefix llvm)/bin/llvm-mc" \
    test/vector/golden/encodings.txt <riscv-opcodes>/extensions/rv_v \
    <riscv-opcodes>/extensions/rv_zvbb <riscv-opcodes>/extensions/rv_zvbc
```

The vector unit blocks in the circuit designs are added by
`tools/vector/add_vector_unit_to_circuits.py resources/circuit_designs`.

## Notes

- Coverage instrumentation (`--coverage`) requires GCC or Clang; configuring with `ENABLE_COVERAGE=ON` on another compiler will fail with a clear error.
- If you're on Windows with a Qt-bundled MinGW toolchain, confirm it actually ships `libgcov.a` before relying on coverage builds — some trimmed-down distributions omit it. You can check with:
  ```sh
  g++ -print-file-name=libgcov.a
  ```
  A real path back means it's present; the bare filename echoed back means it isn't found.