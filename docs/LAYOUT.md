# Repository layout

```
build_reshade_testing_initiative.sh   Linux/macOS build of every tool
build_reshade_testing_initiative.bat  Windows (MinGW-w64) build of every tool
RESHADE_VERSION                       the ReShade tag every build uses

rga/                          reshadefx_rga
  reshadefx_rga.cpp             the tool
  spirv_optimize.{cpp,hpp}      SPIR-V driver-proxy passes
  json_util.hpp                 JSON string escaping

cli/                          sources for reshadefx_cli_fixed and reshadefx_coverage
  effect_codegen_dxbc_vkd3d.cpp   Linux drop-in for crosire's D3DCompiler-based one
  coverage.cpp                    corpus coverage measurement

fxstat/                       instruction statistics, as a library + CLI
  include/fxstat/fxstat.hpp     the whole public API
  src/core/                     libfxstat_core: compile, optimise, classify
  src/cli/                      args, file I/O, output formatting
  examples/minimal.cpp          using the library from memory, no files
  test/run_tests.sh             smoke tests
  tools/gen_opcode_table.py     generates the SPIR-V opcode table
  CMakeLists.txt
  README.md, WINDOWS.md

tools/                        shared build and analysis tooling
  build-vkd3d-shader.sh         builds libvkd3d-shader.a natively on Linux
  idl2h.py                      extracts the C parts of a Windows .idl
  fxc-fix.py                    six fixes to crosire's tools/fxc.cpp
  audit_paths.py                which code path each effect takes per API
  mingw-w64-toolchain.cmake     for cross-building fxstat for Windows

docs/
  CHECKLIST.md                  what is left to do, in order
  AUDIT.md                      renderer-path audit results
  DXBC-ON-LINUX.md              how the vkd3d DXBC route works and what it costs
  HANDOFF.md                    deferred work: WASM, preset sweep, RGA, bandwidth
  LAYOUT.md                     this file
  upstream/                     bug reports for other projects, ready to file
    reshade-fxc.md                the ReShade tools/fxc.cpp bugs
    vkd3d/                        the vkd3d bug: ISSUE.md, SUBMITTING.md, repros

.deps/    (gitignored) reshade, SPIR-V headers and vkd3d, fetched by the build
bin/      (gitignored) build output
```

## The five executables

| binary | source | --dxbc |
|---|---|---|
| `reshadefx_cli` | crosire's `tools/fxc.cpp`, unmodified | Windows only (D3DCompiler) |
| `reshadefx_cli_fixed` | same, with `tools/fxc-fix.py` applied | Windows: D3DCompiler, Linux: vkd3d |
| `reshadefx_rga` | `rga/` | n/a (SPIR-V) |
| `fxstat` | `fxstat/` | Windows: D3DCompiler, Linux: vkd3d |
| `reshadefx_coverage` | `cli/coverage.cpp` | Windows: D3DCompiler, Linux: vkd3d |

## reshadefx_rga and fxstat, and why both exist

**`reshadefx_rga`** counts SPIR-V. It is the Vulkan/OpenGL answer, it drives real
RGA for AMD ISA data, and `--optimize` makes its numbers reflect code a GPU
actually runs rather than the unoptimised module ReShadeFX emits.

**`fxstat`** counts DXBC as well as SPIR-V, is built as a library with a thin CLI
on top, and has a baseline/diff mode. Its DXBC numbers on Windows come from the
same D3DCompiler ReShade itself uses.

They overlap on SPIR-V. That is deliberate for now — `reshadefx_rga` is the
established tool with the RGA integration, `fxstat` is the one structured for
reuse. Merging them is a decision to make later, not a loose end to trip over.

## Path assumptions

The build scripts find everything relative to their own location, so the tree
can be moved as a whole but not rearranged internally:

- `build_reshade_testing_initiative.*` → `RESHADE_VERSION`, `rga/`, `cli/`,
  `fxstat/`, `tools/fxc-fix.py`, `tools/build-vkd3d-shader.sh`
- `tools/build-vkd3d-shader.sh` → `./idl2h.py`
- `fxstat/test/run_tests.sh` → `../bin/fxstat` (override with `FXSTAT`)
