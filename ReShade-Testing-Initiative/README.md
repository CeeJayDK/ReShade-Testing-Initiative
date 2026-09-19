# ReshadeFX-tools

[![Build](https://github.com/CeeJayDK/ReshadeFX-tools/actions/workflows/build.yml/badge.svg)](https://github.com/CeeJayDK/ReshadeFX-tools/actions/workflows/build.yml)

Command-line tools for anyone working with [ReshadeFX](https://github.com/crosire/reshade)
shaders who wants to compile, verify, or analyze them quickly — without
opening a game, and without needing Windows. Built for humans and AI
assistants alike: same tools, same output, whether you're running them by
hand or an agent is driving them as part of an automated workflow.

These use the real ReshadeFX compiler itself, not a re-implementation or
a guess at what would compile — so the answer you get is the answer ReShade
itself would give.

## What's here

`build_reshadefx_tools.sh` (Linux/macOS) and `build_reshadefx_tools.bat`
(Windows, needs [MinGW-w64](https://winlibs.com) + git on PATH) fetch a
pinned commit of [crosire/reshade](https://github.com/crosire/reshade)'s own
source (the lexer, preprocessor, parser, symbol table, and HLSL/GLSL/SPIR-V
codegen — the parts of the compiler that have no Windows dependency) and
build two CLI tools from it:

- **`reshadefx_cli`** — compiles a `.fx` file and reports the exact errors
  the real ReshadeFX compiler would give. Equivalent to the `ReShadeFXC`
  tool that ships with ReShade itself. On Windows, it also supports
  `--dxbc --shader-model <30|40|41|50|...>` for real DX9/10/11/12-equivalent
  compilation — this links crosire's own unmodified `effect_codegen_dxbc.cpp`
  against the actual Microsoft D3DCompiler (`d3dcompiler_47.dll`, which ships
  with Windows itself), so it's the real compiler ReShade itself uses on
  those APIs, not an approximation. This is Windows-only because
  D3DCompiler is a proprietary, closed-source, Windows-only library with no
  equivalent elsewhere; the Linux/macOS build only produces `--hlsl` text
  for those shader models, which is genuinely the same code ReShade would
  hand to D3DCompile, just without the actual compile-and-optimize step.
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
  - if you point it at an installed copy of RGA with `--rga <path> --asic
    <name>`, it additionally reports genuine AMD GPU ISA size and real
    VGPR/SGPR register usage for a real, named GPU — actual hardware data,
    not a heuristic;
  - can load or save a small settings file to exercise a uniform value other
    than the shader's own hardcoded default — see
    [Testing shaders under different ReShade conditions](#testing-shaders-under-different-reshade-conditions)
    below.

`reshadefx_rga` accepts `--json` for machine-readable output (structured
compile diagnostics with file/line/column/code, and named numeric fields
instead of text) — useful when something else is going to parse the result
rather than a human reading it directly. `reshadefx_cli` deliberately stays
plain text only — it's built from crosire's own unmodified `tools/fxc.cpp`,
fetched fresh every build, so patching a `--json` flag into it would mean
re-patching on every ReShade version bump.

The DXBC/DXIL backends are intentionally excluded — they call into
Microsoft's D3DCompiler and are Windows-only — but they aren't needed to
validate a shader: HLSL/GLSL/SPIR-V codegen succeeding already proves the
shader is valid.

## Usage

```bash
./build_reshadefx_tools.sh          # builds into ./bin (Windows: build_reshadefx_tools.bat)
./bin/reshadefx_cli --hlsl -I path/to/reshade-shaders/Shaders -Fo out.hlsl myshader.fx
./bin/reshadefx_rga -I path/to/reshade-shaders/Shaders myshader.fx
```

Both tools accept `-I <path>` for include directories (e.g. the standard
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

On Windows, `reshadefx_cli.exe` additionally supports real DX9/10/11/12
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

## Building only some of the tools

By default the build script builds both tools. If you only need one — e.g.
for a project like ShaderBridge where you only care whether a port compiles
correctly and have no use for the optimization-focused rga tool — pass
`--cli` or `--rga`:

```bash
./build_reshadefx_tools.sh --cli              # just the compiler
./build_reshadefx_tools.sh --rga              # just rga
```

Don't want to build anything? Every push to `main` builds and functional-tests
both platforms in CI — grab the latest binaries from the
[Actions tab](https://github.com/CeeJayDK/ReshadeFX-tools/actions/workflows/build.yml)
(click the newest run's artifacts), or from the
[Releases page](https://github.com/CeeJayDK/ReshadeFX-tools/releases) for a
tagged version.

## Staying in sync with ReShade

`RESHADE_VERSION` pins the exact upstream tag these tools are built against
(currently ReShade 6.8.0). A daily scheduled workflow checks
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
or `pacman -S mingw-w64-x86_64-gcc` via MSYS2) and git on `PATH`. The
resulting `.exe` files have been verified to run correctly (including
driving the real Windows `rga.exe`) via Wine cross-testing during
development, in addition to the build script's own logic being verified
independently. The one caveat: the script's friendly "is g++/git on PATH?"
pre-check uses `where`, which is reliable on real Windows but was observed
to give false positives under Wine — if that check ever passes incorrectly
on your machine, the build will still simply fail with a normal
"not recognized" error at the point a missing tool is actually needed.

## Credit

The compiler these tools are built from is
[crosire/reshade](https://github.com/crosire/reshade), written by Patrick
Mours and contributors, licensed under BSD 3-Clause. This repo doesn't
vendor or redistribute that source — the build scripts fetch it fresh from
a pinned commit — but the CLI tools are thin wrappers around that code, and
credit for the actual compiler belongs there. The optional GPU ISA analysis
uses [RGA](https://github.com/GPUOpen-Tools/radeon_gpu_analyzer), by AMD.

## License

BSD 3-Clause, matching ReShade itself. See `LICENSE`.
