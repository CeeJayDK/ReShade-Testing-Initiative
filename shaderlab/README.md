# shaderlab/ — see what a shader actually outputs

The other tools in this repo tell you whether a `.fx` file compiles and what
it costs. This one tells you what it **does**: it renders the effect onto an
image, headless, on Linux, so you (or an AI agent) can look at the result
instead of guessing.

It drives [ShaderLab](https://github.com/NotRayST/ShaderLab) by RaySt, which
renders ReShade effects onto still images via a ReShade add-on, and runs the
whole thing under Wine with software Vulkan. No GPU and no Windows needed.

```
./fxrender.py LumaSharpen.fx -i shot.png -o out.png --set sharp_strength=1.5 --diff diff.png
[OK] LumaSharpen.fx  techniques=LumaSharpen  2.5s
  image: {'size': [1280, 720], 'mean_abs_diff': 0.49, 'max_abs_diff': 9, 'changed_pixels_pct': 6.1}
  -> out.png
  diff -> diff.png
```

A compile error comes back in about 2 seconds, with the file, line and column:

```
./fxrender.py Broken.fx -i shot.png -o out.png
[FAILED] Broken.fx  techniques=Broken  1.5s
  Broken.fx(5,1): error X3000: syntax error: unexpected 'return', expected ','
```

## Setup

```bash
sudo apt install wine64 mingw-w64 clang lld llvm cmake ninja-build meson glslang-tools \
     bison flex python3-pil python3-numpy xvfb mesa-vulkan-drivers libvulkan1 libvulkan-dev
./build_runtime.sh            # ~30 min on 2 cores; everything lands in ./runtime
```

Then point it at your include directories (usually reshade-shaders):

```bash
export FXRENDER_INCLUDES=/path/to/reshade-shaders/Shaders
./fxrender.py MyEffect.fx -i in.png -o out.png
```

Tested on Ubuntu 24.04 (Wine 9.0, Mesa 25.2 llvmpipe).

## fxrender.py options

| option | |
|---|---|
| `-i/-o` | input image (png/jpg/bmp), output png |
| `-u/--set VAR=VALUE` | uniform override, repeatable. `Other.fx:VAR=VALUE` targets another effect |
| `-t/--technique NAME` | technique(s) to enable. Default: every technique in the file |
| `-I DIR` | extra effect/include dir (added to `FXRENDER_INCLUDES`) |
| `-T DIR` | extra texture dir, searched recursively. `Textures` folders near the shader and include dirs are found on their own |
| `-D NAME=VALUE` | preprocessor definition |
| `--perf-mode` | ReShade performance mode (uniforms become constants) |
| `-f N` | frames rendered before capture (default 5; raise for temporal effects) |
| `--diff FILE` | difference image, 8x amplified, 128 grey = unchanged |
| `--json` | machine-readable result |
| `--keep-log FILE` | keep ReShade.log |

Exit codes: 0 = compiled and rendered, 1 = compile error / ReShade error / render
failure, 2 = usage or setup problem. On failure no output image is written. If
the output is identical to the input, you get a warning, because that usually
means the effect was not applied.

## What was needed, and why

Nothing prebuilt could be used. ShaderLab ships as a Windows zip, and the add-on
needs an unsigned ReShade build with full add-on support. So `build_runtime.sh`
builds all of it from pinned sources:

- **ReShade 6.8.0 built with clang + mingw-w64** (`patches/reshade-mingw.patch`,
  `mingw/shim/`). This is mostly mechanical MSVC-isms. One fix matters beyond
  MinGW: `ini_file` compared file times against an uninitialised
  `file_time_type`. libstdc++'s file clock has its epoch in 2174, so every real
  file looked "older than last load" and **ReShade.ini was silently never
  read**.
- **d3dcompiler_47.dll from Wine master's vkd3d-shader.** ReShade compiles
  D3D11 effects through `D3DCompile`. The d3dcompiler in Wine 9.0 is too old: it
  crashes on ReShade's own mipmap shader and can't resolve `#include`.
- **ShaderLab** with five one-line fixes (`patches/shaderlab-mingw.patch`).
- **DXVK 2.3.1** plus a one-line patch (`patches/dxvk-headless.patch`: Xvfb
  reports a 0 Hz refresh rate and DXVK divided by it). Wine's built-in wined3d
  ignores `SRGBWriteEnable`, which made CAS, SMAA and every other linear-light
  effect come out too dark. Newer DXVK needs Vulkan extensions that Wine 9.0
  doesn't pass through.

## How far can the output be trusted?

`tests/run_tests.sh` checks the pipeline against things that can be checked
independently:

- **LumaSharpen vs a numpy implementation** of the same math (default,
  `strength=3 pattern=2`, performance mode): within 1/255 everywhere.
- **Bilinear sampling, 1 px and 0.5 px offsets, BUFFER_PIXEL_SIZE**: exact.
- **sRGB read + write round trip**: lossless. Read-only and write-only give the
  expected curves.
- **Uniform packing** (float/int/bool/float2): exact.
- **Syntax errors**: reported with the correct line, and no image is written.

All 29 SweetFX effects compile and render, in about 3 s each. Five leave the
test image unchanged at their default settings (LiftGammaGain, Tonemap,
Splitscreen, Template, Layer), which is expected for those defaults.

Known limits:
- This is D3D11 through vkd3d-shader, not Microsoft's compiler. Code that FXC
  accepts and vkd3d rejects (or the reverse) will show up as a difference.
  `reshadefx_cli --dxbc` uses the same vkd3d back end, so the two agree with each
  other.
- The renderer is llvmpipe. It is fine for correctness, but the timings mean
  nothing for GPU performance. Use `reshadefx_rga` / `fxstat` for that.
- There is no depth buffer unless the input PNG carries ShaderLab's embedded
  depth (`slDp` chunk). Depth-based effects render against empty depth.
- ReShade emits `[fastopt]` for loops marked `[loop]` (shader model 4+), and
  vkd3d-shader rejects it (`E5017: Unhandled attribute 'fastopt'`; e.g. qUINT_dof,
  PD80_02_Bloom, AstrayFX Flair). `patches/reshade-loop-attribute.patch` makes this
  ReShade build emit `[loop]` instead, which means the same to the compiler.
  Reported upstream: `docs/upstream/vkd3d/ISSUE-fastopt.md`.
- vkd3d-shader has no `isnan` (`E5005`; e.g. Fubax PerfectPerspective). Report:
  `docs/upstream/vkd3d/ISSUE-isnan.md`. No workaround here.
- ReShade selects an effect by its file name. If two files with the same name are on
  the search path (e.g. an original and a modified copy in another folder), both are
  loaded and both run. To compare an original with a modified copy, render the copy
  from its own copy of the shader folder, or give it a different file name.

## Issues found in ShaderLab itself (worth reporting upstream)

- `render` reports success when the effect failed to compile, and writes the
  unprocessed image. fxrender reads ReShade.log itself because of this.
- `render --shader` adds the shader's folder to `EffectSearchPaths` in
  ReShade.ini **permanently**. If a recursive path already covers that folder,
  ReShade loads the effect twice and **applies it twice**, which looks like a
  plausible but wrong result. (fxrender writes a fresh ini each run and uses
  `--preset`.)
- It leaves `PresetPath` pointing at a temp preset it has already deleted.
- On a compile failure it waits through a 30 s warm-up and a 180 s capture
  timeout before giving up. (fxrender watches the log and stops at once.)
- `test-shader`'s compile check runs before the shader's folder is added to the
  search paths.
