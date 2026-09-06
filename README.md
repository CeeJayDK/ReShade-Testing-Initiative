# ReShade AI Tools

[![Build](https://github.com/CeeJayDK/ReshadeFX-tools/actions/workflows/build.yml/badge.svg)](https://github.com/CeeJayDK/ReshadeFX-tools/actions/workflows/build.yml)

Tools that let an AI assistant (or anyone without a Windows machine) compile
and verify [ReShade FX](https://github.com/crosire/reshade) shaders directly,
using the real ReShade FX compiler — not a re-implementation or a guess at
what would compile.

This exists because AI-assisted shader work (porting, writing, or optimizing
ReShade FX effects) is much faster and more reliable when the assistant can
see real compiler output and iterate on its own, instead of a human relaying
error messages back and forth from a Windows machine running ReShade.

This isn't specific to any one project — any ReShade FX shader work benefits
from being able to compile and check instruction counts on the spot.

## What's here

`build_reshadefx_tools.sh` (Linux/macOS) and `build_reshadefx_tools.bat`
(Windows, needs [MinGW-w64](https://winlibs.com) + git on PATH) fetch a
pinned commit of [crosire/reshade](https://github.com/crosire/reshade)'s own
source (the lexer, preprocessor, parser, symbol table, and HLSL/GLSL/SPIR-V
codegen — the parts of the compiler that have no Windows dependency) and
build three CLI tools from it:

- **`reshadefx_cli`** — compiles a `.fx` file and reports the exact errors
  the real ReShade FX compiler would give. Equivalent to the `ReShadeFXC`
  tool that ships with ReShade itself.
- **`reshadefx_stats`** — compiles a `.fx` file and additionally reports the
  real SPIR-V instruction count per shader stage (vertex/pixel/compute),
  useful for confirming an optimization actually reduced instruction count
  rather than just looking leaner.
- **`reshadefx_rga`** — makes [RGA (Radeon GPU Analyzer)](https://github.com/GPUOpen-Tools/radeon_gpu_analyzer)
  understand ReShade FX shaders directly. RGA only speaks raw HLSL/GLSL/SPIR-V
  and has no idea what a `technique`/`pass` is or how to resolve
  `ReShade.fxh`; this tool compiles the `.fx` file with the real reshadefx
  frontend, extracts each shader stage as SPIR-V, and:
  - always reports a real-instruction-based cost breakdown per shader stage
    (cheap ALU / transcendental / texture / control-flow / memory), classified
    directly from the actual SPIR-V opcode stream — a vendor-general proxy
    for "which parts of this shader are expensive," useful with no other
    tools installed;
  - if you point it at an installed copy of RGA with `--rga <path> --asic
    <name>`, it additionally reports genuine AMD GPU ISA size and real
    VGPR/SGPR register usage for a real, named GPU — actual hardware data,
    not a heuristic.

The DXBC/DXIL backends are intentionally excluded — they call into
Microsoft's D3DCompiler and are Windows-only — but they aren't needed to
validate a shader: HLSL/GLSL/SPIR-V codegen succeeding already proves the
shader is valid.

## Usage

```bash
./build_reshadefx_tools.sh          # builds into ./bin (Windows: build_reshadefx_tools.bat)
./bin/reshadefx_cli --hlsl -I path/to/reshade-shaders/Shaders -Fo out.hlsl myshader.fx
./bin/reshadefx_stats -I path/to/reshade-shaders/Shaders myshader.fx
./bin/reshadefx_rga -I path/to/reshade-shaders/Shaders myshader.fx
```

Don't want to build anything? Every push to `main` builds and functional-tests
both platforms in CI — grab the latest binaries from the
[Actions tab](https://github.com/CeeJayDK/ReshadeFX-tools/actions/workflows/build.yml)
(click the newest run's artifacts), or from the
[Releases page](https://github.com/CeeJayDK/ReshadeFX-tools/releases) for a
tagged version (`git tag v1.0.0 && git push --tags` cuts a new one).

With real GPU ISA via RGA (download RGA from its
[releases page](https://github.com/GPUOpen-Tools/radeon_gpu_analyzer/releases)
— not bundled here, it's a large AMD binary under AMD's own EULA):

```bash
./bin/reshadefx_rga -I path/to/reshade-shaders/Shaders --rga path/to/rga --asic gfx1100 myshader.fx
```

All three tools accept `-I <path>` for include directories (e.g. the
standard [reshade-shaders](https://github.com/crosire/reshade-shaders) repo,
if your effect includes `ReShade.fxh`) and `-D name=value` for preprocessor
macros.

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
