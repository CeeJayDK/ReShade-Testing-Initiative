# ReShade Testing Initiative

[![Build](https://github.com/CeeJayDK/ReShade-Testing-Initiative/actions/workflows/build.yml/badge.svg)](https://github.com/CeeJayDK/ReShade-Testing-Initiative/actions/workflows/build.yml)

Command-line tools for anyone working with [ReshadeFX](https://github.com/crosire/reshade)
shaders who wants to compile, verify, or analyze them quickly — without
opening a game, and without needing Windows. Built for humans and AI
assistants alike: same tools, same output, whether you're running them by
hand or an agent is driving them as part of an automated workflow.

These use the real ReshadeFX compiler itself, not a re-implementation or
a guess at what would compile — so the answer you get is the answer ReShade
itself would give.

(This repo was previously called ReshadeFX-tools.)

## What's here

`build_reshade_testing_initiative.sh` (Linux/macOS) and
`build_reshade_testing_initiative.bat` (Windows, MinGW-w64) fetch
[crosire/reshade](https://github.com/crosire/reshade)'s own source at the tag in
`RESHADE_VERSION` and build five tools from it:

| tool | what it does |
|---|---|
| `reshadefx_cli` | crosire's own `ReShadeFXC`, unmodified. Compiles a `.fx` file and reports the exact errors ReShade would. |
| `reshadefx_cli_fixed` | the same tool with six bug fixes applied ([docs/upstream/reshade-fxc.md](docs/upstream/reshade-fxc.md)), and `--dxbc` on Linux too. |
| `reshadefx_rga` | per-stage SPIR-V instruction cost; optionally drives AMD's RGA for real GPU ISA and register data. |
| `fxstat` | SPIR-V and DXBC instruction statistics, lane-weighted ALU and transcendental counts, optional real AMD GPU ISA via RGA (`--rga`), JSON output, baseline/diff mode. See [fxstat/README.md](fxstat/README.md). |
| `reshadefx_coverage` | how much of a shader corpus compiles to DXBC, per shader model. |

More detail on each:

- **`reshadefx_cli`** — equivalent to the `ReShadeFXC` tool that ships with
  ReShade, built from crosire's unmodified `tools/fxc.cpp`. On Windows it
  also supports `--dxbc --shader-model <30|40|41|50|...>` for real
  DX9/10/11/12-equivalent compilation — this links crosire's own unmodified
  `effect_codegen_dxbc.cpp` against the actual Microsoft D3DCompiler
  (`d3dcompiler_47.dll`, which ships with Windows itself), so it's the real
  compiler ReShade uses on those APIs. On Linux/macOS this build only produces
  `--hlsl` text for those shader models, which is the same code ReShade would
  hand to D3DCompile, just without the compile-and-optimize step.
- **`reshadefx_cli_fixed`** — `reshadefx_cli` with
  [`tools/fxc-fix.py`](tools/fxc-fix.py) applied. Upstream's tool sets up a
  different preprocessor environment than the runtime does, so it rejects
  some effects ReShade compiles fine and silently compiles the wrong branch of
  others. The fixed build also adds `--list-entry-points` and `-Fc`. On Linux
  its `--dxbc` works, via vkd3d-shader
  ([docs/DXBC-ON-LINUX.md](docs/DXBC-ON-LINUX.md)); on Windows it uses
  D3DCompiler like the upstream one. Use this one unless you specifically need
  upstream's exact behaviour.
- **`reshadefx_rga`** — makes [RGA (Radeon GPU Analyzer)](https://github.com/GPUOpen-Tools/radeon_gpu_analyzer)
  understand ReshadeFX shaders directly. RGA only speaks raw HLSL/GLSL/SPIR-V
  and has no idea what a `technique`/`pass` is or how to resolve
  `ReShade.fxh`; this tool compiles the `.fx` file with the real reshadefx
  frontend, extracts each shader stage as SPIR-V, and:
  - always reports a real-instruction-based cost breakdown per shader stage
    (cheap ALU / transcendental / texture / control-flow / memory), classified
    directly from the actual SPIR-V opcode stream — a vendor-general proxy
    for "which parts of this shader are expensive," useful with no other
    tools installed, and enough on its own to confirm an optimization
    actually reduced instruction count rather than just looking leaner;
  - with `--optimize`, first runs the folding and dead-branch passes a GPU
    driver runs, so the counts reflect code that actually executes (needs
    SPIRV-Tools at build time);
  - if you point it at an installed copy of RGA with `--rga <path> --asic
    <name>`, it additionally reports genuine AMD GPU ISA size and real
    VGPR/SGPR register usage for a real, named GPU;
  - can load or save a small settings file to exercise a uniform value other
    than the shader's own hardcoded default — see
    [Testing shaders under different ReShade conditions](#testing-shaders-under-different-reshade-conditions)
    below.
- **`fxstat`** — overlaps with `reshadefx_rga` on SPIR-V, adds DXBC, and is
  built as a library with a thin CLI on top. Why both exist:
  [docs/LAYOUT.md](docs/LAYOUT.md).
- **`reshadefx_coverage`** — walks every effect, every entry point and every
  requested shader model and reports exactly what fails to compile to DXBC
  and why.

`reshadefx_rga` and `fxstat` accept `--json` for machine-readable output.
`reshadefx_cli` deliberately stays plain text only — it's built from crosire's
own unmodified `tools/fxc.cpp`, fetched fresh every build, so patching a
`--json` flag into it would mean re-patching on every ReShade version bump.

## Usage

```bash
./build_reshade_testing_initiative.sh     # builds everything into ./bin (Windows: .bat)
./bin/reshadefx_cli --hlsl -I path/to/reshade-shaders/Shaders -Fo out.hlsl myshader.fx
./bin/reshadefx_rga -I path/to/reshade-shaders/Shaders myshader.fx
```

All the tools accept `-I <path>` for include directories (e.g. the standard
[reshade-shaders](https://github.com/crosire/reshade-shaders) repo, if your
effect includes `ReShade.fxh`) and `-D name=value` for preprocessor macros.

For real GPU ISA instead of just the built-in instruction classification,
point `reshadefx_rga` at an installed copy of RGA (download from its
[releases page](https://github.com/GPUOpen-Tools/radeon_gpu_analyzer/releases)
— not bundled here, it's a large AMD binary under AMD's own EULA):

```bash
./bin/reshadefx_rga -I path/to/reshade-shaders/Shaders --rga path/to/rga --asic gfx1100 myshader.fx
```

Add `--json` for machine-readable output instead of the human-readable text
shown above:

```bash
./bin/reshadefx_rga --json -I path/to/reshade-shaders/Shaders myshader.fx
```

On Windows, `reshadefx_cli.exe` and `reshadefx_cli_fixed.exe` support real DX9/10/11/12
compilation via the actual Microsoft D3DCompiler (`--shader-model` maps
directly to the profile: `30`=DX9, `40`=DX10, `41`/`50`=DX11/12):

```powershell
reshadefx_cli.exe --dxbc --shader-model 50 -I path\to\reshade-shaders\Shaders -E MyEntryPoint -Fo out.cso myshader.fx
```

## Testing shaders under different ReShade conditions

`reshadefx_rga` accepts four flags for the ReShade-specific macros a shader
might branch on — a shader would not normally need to check these, but some
do to work around known bugs or adjust for capabilities, so it's worth being
able to test both branches:

```bash
--reshade-version <num>   # override __RESHADE__ (default: the version these
                           # tools were built against; MAJOR*10000+MINOR*100+REVISION)
--perf                     # set __RESHADE_PERFORMANCE_MODE__ to 1 (default: 0)
--width <n>                # override BUFFER_WIDTH (default: 1920)
--height <n>               # override BUFFER_HEIGHT (default: 1080)
```

`--perf` is the one worth understanding, not just using. ReShade
recompiles a shader with its uniform variables (normally used for editable
UI settings) turned into `static const` values when performance mode is on.
The compiler can sometimes fold and simplify the resulting math further than
it could with a genuinely variable value — so the *same* shader can compile
to meaningfully different, and sometimes faster, code in performance mode.
That means a real performance picture has two numbers, not one: how it
behaves in normal/edit mode (settings variable) and how it behaves in
performance mode (settings locked to static) — and not every shader author
remembers to check both. Occasionally a compiler bug surfaces in one mode
but not the other, too, particularly in larger, more complex effects.

By default, performance mode exercises whatever values the shader's own
`uniform` declarations already default to. To test a *different* value — the
other side of an `if` a uniform guards, for instance — use:

```bash
--load-settings[=<path>]   # read plain-numeric uniform overrides from a
                             # ReShade-preset-format file (a real ReShade
                             # preset works too) and apply them before
                             # compiling. Implies --perf, since
                             # that's the only mode where a uniform's value
                             # becomes part of the compiled code rather than
                             # a runtime-editable buffer entry.
--save-settings[=<path>]   # scan the shader's own uniform defaults and
                             # write them out in that same format, then
                             # continue to compile as normal.
```

Leaving off `=<path>` for either flag defaults to `<effect-name>.ini`
alongside the shader — so `reshadefx_rga --save-settings levels.fx` writes
`levels.ini` next to it, and a later `reshadefx_rga --load-settings
levels.fx` picks that same file back up. Hand-edit the saved file (or point
`--load-settings` at a real ReShade preset) to try a different value:

```bash
./bin/reshadefx_rga --save-settings levels.fx    # writes levels.ini
# edit levels.ini, e.g. change some_mode=0 to some_mode=1
./bin/reshadefx_rga --load-settings levels.fx    # compiles with that override
```

## Building

With no arguments the build script builds all five tools into `./bin`. Pass
any of `--cli`, `--cli-fixed`, `--rga`, `--fxstat`, `--coverage` to build only
those — e.g. for a project like ShaderBridge, where you only care whether a
port compiles:

```bash
./build_reshade_testing_initiative.sh --cli-fixed     # just the compiler
./build_reshade_testing_initiative.sh out --rga       # just rga, into ./out
```

What each needs besides git and g++:

| | Linux/macOS | Windows |
|---|---|---|
| `reshadefx_cli` | — | — |
| `reshadefx_cli_fixed` | python3, bison, flex | python |
| `reshadefx_rga` | SPIRV-Tools (optional, for `--optimize`) | same, via `SPIRV_TOOLS_DIR` |
| `fxstat` | cmake, SPIRV-Tools, bison, flex | cmake, SPIRV-Tools via `SPIRV_TOOLS_DIR` |
| `reshadefx_coverage` | bison, flex | — |

bison and flex are for vkd3d-shader, which is what gives `--dxbc` on Linux.
Debian/Ubuntu: `apt-get install spirv-tools bison flex cmake`.

Downloaded sources (ReShade, the SPIR-V headers that ReShade tag pins, and
vkd3d) are cached in `.deps/`. The first full build takes a few minutes, mostly
vkd3d; after that it is quick. Delete `.deps/` to start clean.

Don't want to build anything? Every push to `main` builds and tests both
platforms in CI — grab the latest binaries from the
[Actions tab](https://github.com/CeeJayDK/ReShade-Testing-Initiative/actions/workflows/build.yml)
(click the newest run's artifacts), or from the
[Releases page](https://github.com/CeeJayDK/ReShade-Testing-Initiative/releases) for a
tagged version.

## Staying in sync with ReShade

`RESHADE_VERSION` pins the exact upstream tag these tools are built against
(currently ReShade 6.8.0), and every tool in both build scripts is built against
it. A daily scheduled workflow checks
[crosire/reshade](https://github.com/crosire/reshade)'s tags directly via
git (no REST API, no rate limits) for the newest clean `vMAJOR.MINOR.REVISION`
tag — pre-releases like `v6.9.0-rc1` are ignored, matching how crosire
actually marks an official release. When a new version appears, it bumps
`RESHADE_VERSION`, tags this repo to match, and that tag push triggers the
build workflow above, which builds, tests, and publishes a matching release
automatically. No manual step required to stay current.

## Notes on the Windows build

The Windows build uses MinGW-w64 rather than MSVC, so no Visual Studio
install is required — just MinGW-w64 (e.g. from [winlibs.com](https://winlibs.com),
or via MSYS2) and git on `PATH`. The `.exe` files are statically linked and need
no MinGW runtime DLLs. The simplest full setup is MSYS2, which is also what CI
uses:

```
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-cmake mingw-w64-x86_64-spirv-tools
set SPIRV_TOOLS_DIR=C:\msys64\mingw64
build_reshade_testing_initiative.bat
```

vkd3d is not used on Windows; every `--dxbc` there goes through the real
`d3dcompiler_47.dll`. See [fxstat/WINDOWS.md](fxstat/WINDOWS.md) for what has
and hasn't been verified on real Windows.

## Repository layout

See [docs/LAYOUT.md](docs/LAYOUT.md). Open work is in
[docs/CHECKLIST.md](docs/CHECKLIST.md).

## Credit

The compiler these tools are built from is
[crosire/reshade](https://github.com/crosire/reshade), written by Patrick
Mours and contributors, licensed under BSD 3-Clause. This repo doesn't
vendor or redistribute that source — the build scripts fetch it from
the pinned tag — but the CLI tools are thin wrappers around that code, and
credit for the actual compiler belongs there. The optional GPU ISA analysis
uses [RGA](https://github.com/GPUOpen-Tools/radeon_gpu_analyzer), by AMD.

## License

BSD 3-Clause, matching ReShade itself. See `LICENSE`.
