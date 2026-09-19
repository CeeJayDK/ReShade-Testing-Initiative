# Repository layout

What lives where, after the ReShade Testing Initiative rename.

```
reshadefx_rga.cpp             the RGA/stats tool          (root, unchanged place)
spirv_optimize.{cpp,hpp}      SPIR-V driver-proxy passes  (root, used by the above)
json_util.hpp                 (root)
build_reshadefx_tools.{sh,bat}

fxstat/                       instruction statistics, as a library + CLI
  include/fxstat/fxstat.hpp     the whole public API
  src/core/                     libfxstat_core: compile, optimise, classify
  src/cli/                      args, file I/O, output formatting
  examples/minimal.cpp          using the library from memory, no files
  test/run_tests.sh             17 tests
  tools/gen_opcode_table.py     generates the SPIR-V opcode table
  CMakeLists.txt, build.sh
  README.md, WINDOWS.md

reshadefx-cli/                makes reshadefx_cli --dxbc work on Linux
  effect_codegen_dxbc_vkd3d.cpp   drop-in for crosire's D3DCompiler-based one
  coverage.cpp                    corpus coverage measurement
  build.sh                        builds reshadefx_cli + reshadefx_coverage

tools/                        shared build and analysis tooling
  build-vkd3d-shader.sh         builds libvkd3d-shader.a natively on Linux
  idl2h.py                      extracts the C parts of a Windows .idl
  fxc-fix.py                    six fixes to crosire's tools/fxc.cpp
  audit_paths.py                which code path each effect takes per API
  mingw-w64-toolchain.cmake     for cross-building fxstat for Windows

docs/
  CHECKLIST.md                  what is left to do, in order
  BUG-REPORT.md                 the ReShade tools/fxc.cpp bugs, ready to file
  AUDIT.md                      renderer-path audit results
  DXBC-ON-LINUX.md              how the vkd3d DXBC route works and what it costs
  HANDOFF.md                    deferred work: WASM, preset sweep, RGA, bandwidth
  LAYOUT.md                     this file

vkd3d-issue/                  the vkd3d bug, ready to submit
  ISSUE.md, SUBMITTING.md
  repro_sm3.hlsl, repro_sm5.hlsl, workaround.hlsl
  repro_harness_sm3.c, repro_harness_sm5.c
```

## The two tools, and why both exist

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

Scripts resolve each other relative to their own location, so the tree can be
moved as a whole but not rearranged internally:

- `reshadefx-cli/build.sh` → `../tools/build-vkd3d-shader.sh`, `../tools/fxc-fix.py`
- `tools/build-vkd3d-shader.sh` → `./idl2h.py`
- `fxstat/build.sh` → `./tools/gen_opcode_table.py`
