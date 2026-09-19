# fxstat

Instruction statistics for ReShade effects, counted on the code the GPU actually
runs rather than the code ReShadeFX emits.

Cross-platform, command line, JSON output, and a baseline/diff mode so a change
to a shader can be measured instead of guessed at.

```
$ fxstat -I reshade-shaders/Shaders SweetFX/Shaders/SweetFX/LumaSharpen.fx

SweetFX/Shaders/SweetFX/LumaSharpen.fx  [SPIR-V, performance mode, optimised]

uniform values baked in:
  sharp_strength               = 0.65
  sharp_clamp                  = 0.035
  pattern                      = 1
  offset_bias                  = 1
  show_sharpen                 = false

ENTRY POINT                  STAGE       TEX    ALU    MOV    MEM   FLOW   TOTAL
----------------------------------------------------------------------------------
E__PostProcessVS             vertex        0      6      3     11      2      23
                                           0      6      3     18      5      37   (before optimisation)
E__LumaSharpenPass           pixel         5     15      7     11      2      41
                                          15     63     38    106     40     266   (before optimisation)
```

Five texture fetches and fifteen ALU instructions is what LumaSharpen actually
costs at its default settings. Fifteen fetches and sixty-three ALU is what
ReShadeFX hands the driver.

## The problem this solves

ReShade's performance mode turns uniforms into constants, so the branches those
uniforms select between become dead and can be removed. But **ReShadeFX's code
generators do no optimisation at all.** In performance mode the constants are
baked in and the dead branches are still emitted, in full. ReShade ships that to
the driver and the driver cleans it up.

So any tool that counts instructions straight out of ReShadeFX is counting code
that never executes — for LumaSharpen, three times as much of it as is real.

Worse, on the SPIR-V path the constants are not baked in at all. ReShade emits
performance-mode uniforms as `OpSpecConstant` and supplies the real values
through `VkSpecializationInfo` at pipeline creation. The values never appear in
the module. A SPIR-V file taken from the code generator always carries whatever
was written in the effect source, whatever preset is loaded.

fxstat closes both gaps: it bakes the preset values into the module, then runs
the folding, mem2reg and dead-branch passes every driver runs, and counts what
is left.

## Usage

```
fxstat [options] <effect.fx>
Input:
  -I <path>              Add an include search path (repeatable).
  -D <name>[=<value>]    Define a preprocessor macro (repeatable).
  --preset <file.ini>    ReShade preset to take uniform values from.
  --effect-name <name>   Preset section to read (default: the .fx file name).
  --width <n>            BUFFER_WIDTH  (default 1920).
  --height <n>           BUFFER_HEIGHT (default 1080).

Code generation:
  --spirv                SPIR-V backend (default).
  --dxbc                 DXBC backend (DX9/10/11/12), via vkd3d-shader.
                         Counts the exact bytecode ReShade hands the driver.
  --hlsl                 HLSL backend (generates code; no counting).
  --glsl                 GLSL backend (generates code; no counting).
  --shader-model <n>     HLSL shader model: 30, 40, 41, 50, 51 (default 50).
  --renderer <id>        Override __RENDERER__ (e.g. 0xc000 for D3D12). By
                         default it follows the backend and shader model.
                         Use this to audit which path an effect takes per API.
  --performance-mode     Uniforms become constants (default).
  --no-performance-mode  Keep uniforms as uniforms.

Analysis:
  --optimize             Fold constants and remove dead branches before counting
                         -- what the driver does. Default on. SPIR-V only.
  --no-optimize          Count raw ReShadeFX output.

Comparison:
  --baseline <file.json> Compare against a report written earlier by --json.
  --fail-on-regression   Exit 2 if any count went up. For CI.

Output:
  --json                 Machine-readable output.
  --verbose              Per-opcode breakdown.
  --dump <dir>           Write generated code, binaries and disassembly there.
  --list-passes          Print the SPIR-V optimisation pass list and exit.
  --version              Print version information and exit.
  -h, --help             This message.
```

### Measuring a change

```
$ fxstat -I Shaders --json MyShader.fx > before.json
$ vim MyShader.fx
$ fxstat -I Shaders --baseline before.json --fail-on-regression MyShader.fx

ENTRY POINT                  STAGE              TEX           ALU  ...
----------------------------------------------------------------------
E__MyShaderPass              pixel           5 +2         15 +4   ...
```

A `.` means unchanged. This is the mode worth wiring into CI, and the mode worth
handing to an agent: it turns "I think this is faster" into a number.

### Per-preset, not per-shader

Performance-mode statistics are a property of the *preset*, not the effect. On
LumaSharpen the sample pattern alone moves the cost by 40%:

| `pattern` | TEX | ALU |
|-----------|-----|-----|
| 0         | 3   | 11  |
| 1         | 5   | 15  |
| 2         | 5   | 15  |
| 3         | 5   | 15  |

Always pass `--preset` when the numbers are meant to mean something. Without
one, fxstat uses the defaults written in the effect source, which is what
ReShade does too.

## The optimisation passes

Deliberately a fixed, explicit list — not `spirv-opt -O`.

`-O` is wrong here for two reasons. It never runs `--freeze-spec-const`, so the
specialisation constants keep their symbolic form and not one dead branch is
removed. On LumaSharpen, `-O` leaves all fifteen texture fetches standing. And
it inlines and unrolls, so counts move for reasons unrelated to the change being
measured, which makes it useless for diffing.

The list fxstat uses does only what every driver does unconditionally: bake the
constants, inline (GPUs have no call stack), promote locals to SSA, fold,
remove the branches the constants made dead, and sweep up. It lives in one array at the
top of `src/core/optimize.cpp`, commented, so it can be argued with, and
`--list-passes` prints it.

SPIRV-Tools is linked in, so the version is pinned by whatever you built
against rather than by whatever is on `PATH`. It is recorded in `--json` output
and by `--version`; a diff across two versions is noise, so check it matches
before comparing numbers from different machines.

## What the categories mean

| | |
|---|---|
| **TEX** | texture sampling and image access |
| **ALU** | arithmetic, logic, compare, convert, extended instructions |
| **MOV** | composite construct/extract/insert, swizzles, copies |
| **MEM** | load / store / access chain / local variable |
| **FLOW** | branches, loops, phi, calls, returns |

Only instructions reachable from the entry point being reported are counted. A
ReShade-generated module contains every function in the effect, so counting the
raw stream counts other passes' shaders too.

The opcode-to-category mapping is generated from the Khronos SPIR-V grammar by
`tools/gen_opcode_table.py`, by rule rather than by hand.

## Honest limitations

**SPIR-V is a pre-optimisation IR, not a cost model.** One `OpImageSample` can
become several VMEM instructions; `OpFMul` + `OpFAdd` collapses to one `v_fma`.
The ALU column is inflated relative to real hardware, and by a factor that
varies per shader. These numbers are good for *diffing* and for confirming
performance mode did its job. They are not a cost estimate.

For hardware cost, feed the optimised SPIR-V that `--dump` writes to
`rga -s vk-offline`, which gives RDNA ISA with VALU/SALU/VMEM/SMEM split,
register pressure and occupancy. That runs offline on Windows and Linux with no
GPU and no driver.

**Nothing here sees bandwidth.** A 9-tap and a 49-tap blur have nearly identical
per-invocation counts. Pass count, render target format and resolution scale
decide the cost of most ReShade effects, and none of them appear in this table.

**The SPIR-V inliner sometimes gives up.** SPIRV-Tools' inliner declines certain
modules; when it does, mem2reg has nothing to work on, the load/store scaffolding
survives, and the counts come out badly inflated — on SMAA's neighborhood
blending pass, 237 instructions against DXBC's 46. fxstat detects this (an
`OpFunctionParameter` surviving optimisation), marks the row `!!` in the table and
sets `inliner_incomplete` in JSON. Treat flagged rows as unusable and use
`--dxbc` for those shaders.

**`--hlsl` and `--glsl` generate code but are not counted.** Use `--dump` and
feed the result to a native compiler.

## Structure

```
include/fxstat/fxstat.hpp   the whole public API
src/core/                   libfxstat_core -- compile, optimise, classify
src/cli/                    argument parsing, file I/O, output formatting
examples/minimal.cpp        using the library directly, from memory
```

The core does no printf, no exit(), no process spawning and no disk writes;
the only file access is the ReShadeFX preprocessor resolving `#include`. That is
what makes a WASM build possible: preload the shader pack into Emscripten MEMFS
and pass the mount point in `include_paths`, or skip files entirely with
`compile_source()`.

```cpp
fxstat::compile_options options;
options.target = fxstat::backend::dxbc;
options.shader_model = 50;
options.preset["EdgeDetectionType"] = "2";

const fxstat::compile_result r = fxstat::compile_file("SMAA.fx", options);
for (const auto &e : r.entry_points)
    printf("%s: %u fetches\n", e.name.c_str(), e.stats.get(fxstat::category::tex));
```

SPIRV-Tools is **linked**, not invoked as a subprocess. Besides working under
WASM, that pins the pass list and the SPIRV-Tools version into the binary, so two
machines cannot quietly produce different numbers, and it removes a process spawn
per entry point. `--list-passes` prints the pass list; `--version` prints the
SPIRV-Tools version; both appear in `--json` output.

## Building

Needs a C++17 compiler, CMake, Python 3 (for the opcode table) and SPIRV-Tools
headers and libraries (Debian/Ubuntu: `apt-get install spirv-tools`). Nothing is
needed on `PATH` at run time.

```
git clone --depth 1 https://github.com/crosire/reshade.git
git clone --depth 1 https://github.com/KhronosGroup/SPIRV-Headers.git

python3 tools/gen_opcode_table.py \
    SPIRV-Headers/include/spirv/unified1/spirv.core.grammar.json \
    src/spirv_opcodes.gen.cpp

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DRESHADE_DIR=../reshade -DSPIRV_HEADERS_DIR=../SPIRV-Headers
cmake --build build
```

Only the ReShadeFX front end and the three code generators without Windows-only
dependencies are compiled; `effect_codegen_dxbc.cpp` and `effect_codegen_dxil.cpp`
need `d3dcompiler.h` and are left out.

## Next

**DXBC.** The obvious gap. DXBC instruction slots are the exact bytes ReShade
hands a DX11 driver, which makes them the right number for "did performance mode
work" even though they say nothing about cost. `vkd3d-compiler` from
[vkd3d](https://codeberg.org/vkd3d/vkd3d.git) 1.16+ compiles HLSL SM1–SM5 to
`d3d-asm` natively on Linux, which would cover DX9 through DX11 with no Wine and
no Microsoft redistributable. Its HLSL front end is not complete, so it needs
testing against a real shader corpus first. Wine's bundled `d3dcompiler_47` is
the same vkd3d code, not Microsoft's.

**Real hardware numbers.** `VK_KHR_pipeline_executable_properties` from a small
headless Vulkan runner gives register counts, spills and often the ISA on AMD,
Intel and NVIDIA, on both operating systems. It is the only practical way to get
NVIDIA numbers at all.

**Bandwidth.** Reporting pass count, target format and resolution scale next to
the instruction counts would close most of the gap without needing a runtime
harness.
