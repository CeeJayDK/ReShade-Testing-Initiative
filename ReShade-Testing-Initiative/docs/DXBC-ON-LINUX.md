# reshadefx-dxbc

A working `--dxbc` target for ReShadeFXC on Linux, plus the fixes the tool needs
to be trustworthy, plus DX11 instruction statistics.

crosire's `source/effect_codegen_dxbc.cpp` calls `D3DCompile()`, so upstream's
`--dxbc` exists only on Windows and only with Microsoft's redistributable.
`effect_codegen_dxbc_vkd3d.cpp` provides the same `create_codegen_dxbc()` symbol
on top of vkd3d-shader, which compiles HLSL to DXBC natively. Drop it in place of
the original and `--dxbc` works.

## Results on the SweetFX + reshade-shaders corpus (33 effects, 77 entry points)

| target | effects | entry points |
|---|---|---|
| SM 4.0 (DX10) | 33 / 33 | **77 / 77** |
| SM 4.1 (DX10.1) | 33 / 33 | **77 / 77** |
| SM 5.0 (DX11) | 33 / 33 | **77 / 77** |
| SM 5.1 (DX12) | 33 / 33 | **77 / 77** |
| SM 5.0, performance mode | 33 / 33 | **77 / 77** |
| SM 3.0 (DX9) | 0 / 33 | 36 / 77 — vertex shaders only |

DX11 — the target that matters most — is complete. DX9 is blocked by a single
vkd3d limitation, described below.

LumaSharpen at its defaults, compiled to real DX11 bytecode:

```
$ fxstat --dxbc --shader-model 50 -I reshade-shaders/Shaders LumaSharpen.fx

ENTRY POINT                  STAGE       TEX    ALU    MOV    MEM   FLOW   TOTAL
----------------------------------------------------------------------------------
F__PostProcessVS             vertex        0      6      8      0      1      15
                                                              (vs_5_0, 2 temp registers)
F__LumaSharpenPass           pixel         5     13      4      0      1      23
                                                              (ps_5_0, 3 temp registers)
```

and the same shader with performance mode off:

```
F__LumaSharpenPass           pixel        15     58     35      0      9     117
                                                              (ps_5_0, 6 temp registers)
```

5 texture fetches and no branches, against 15 fetches, 9 flow instructions and
117 total. That 117 is probably the number you remember.

## Layout

```
tools/
  build-vkd3d-shader.sh    build libvkd3d-shader.a natively on Linux
  idl2h.py                 extract the C parts of a Windows .idl (no widl needed)
  fxc-fix.py               the tools/fxc.cpp fixes (see BUG-REPORT.md)
reshadefx-cli/
  effect_codegen_dxbc_vkd3d.cpp   the DXBC back end
  coverage.cpp                    corpus coverage measurement
  build.sh                        builds reshadefx_cli + reshadefx_coverage
fxstat/                    instruction statistics; --dxbc and --spirv back ends
```

## Building

```
./reshadefx-cli/build.sh            # fetches everything, builds reshadefx_cli
./reshadefx-cli/bin/reshadefx_cli --dxbc --shader-model 50 --list-entry-points \
    -I path/to/reshade-shaders/Shaders MyShader.fx
```

Needs `gcc`/`g++`, `cmake`, `bison`, `flex`, `python3`, `git`. No Wine, no
Microsoft redistributable, no Vulkan SDK.

For fxstat with the DXBC back end:

```
./tools/build-vkd3d-shader.sh /tmp/vkd3d
cd fxstat && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DRESHADE_DIR=../reshade -DSPIRV_HEADERS_DIR=../SPIRV-Headers \
    -DVKD3D_BUILD=/tmp/vkd3d
cmake --build build
```

`-DVKD3D_BUILD` is optional; without it fxstat builds fine, minus `--dxbc`.

## How the vkd3d build works, and why it is odd

vkd3d upstream lives on Codeberg. This builds it from **Wine's vendored copy**
(`libs/vkd3d` in the Wine tree) instead, for two reasons: the Wine GitHub mirror
can be fetched sparsely, and Wine tracks vkd3d closely — the copy used here was
a day old at the time of writing, far newer than any packaged vkd3d (Ubuntu
24.04 ships 1.2, from 2020, which has essentially no HLSL compiler).

The cost is that Wine's copy is edited for building inside the Wine tree: it
includes `windows.h`, `<wine/list.h>`, `d3dcommon.h` and `d3dx9shader.h` and
expects Wine's own headers to satisfy them, where upstream ships
`vkd3d_windows.h` and IDL files. `build-vkd3d-shader.sh` puts equivalents in
those slots. The only piece taken from upstream is `vkd3d_windows.h`, which
changes very rarely.

**`-Werror=implicit-function-declaration` is not optional here.** `strtof_l()`
is a GNU extension; without `_GNU_SOURCE` it is declared implicitly, returns
`int`, and **every float literal in every shader silently parses as garbage** —
under `-w`, in complete silence. The first build made it all the way to
`c.rgb * 2.0 - 1.0` compiling as `mul r1.xyz, r1.xyzx, l(0,0,0,0)` before this
was caught. If you change the flags, keep that one.

## Known limitations

**DX9 / shader model 3 does not work.** All 41 pixel shader failures are the
same vkd3d error:

```
E5002: Static variables cannot have both numeric and resource components.
```

ReShade's SM3 output bundles a sampler with its pixel size in a struct:

```hlsl
struct __sampler2D { sampler2D s; float2 pixelsize; };
static const __sampler2D V__ReShade__BackBuffer = { __V__ReShade__BackBuffer_s, float2(COLOR_PIXEL_SIZE) };
```

D3DCompiler accepts this; vkd3d-shader has not implemented splitting such a
static into its resource and numeric halves. Vertex shaders, which do not sample,
compile fine. This is worth reporting to the vkd3d project — it is a small,
well-defined gap, and ReShade is not doing anything exotic.

**Optimization levels are not equivalent.** vkd3d-shader has no counterpart to
`D3DCOMPILE_OPTIMIZATION_LEVEL*`. The `optimization_level` argument is accepted
for signature compatibility and only honours `#pragma reshade skipoptimization`.
Counts from this back end will not match FXC's exactly, so do not compare a
vkd3d number against an FXC number — compare vkd3d against vkd3d, and pin the
vkd3d revision.

**DXBC is a virtual ISA.** No register allocation, no scheduling, no VMEM/VALU
distinction. These counts are the right answer to "did performance mode do its
job, and did this edit make the shader smaller". They are not a cost model. For
cost, feed the SPIR-V that `fxstat --spirv --dump` writes to `rga -s vk-offline`.

## The fxc.cpp fixes

`tools/fxc-fix.py` applies six fixes to crosire's `tools/fxc.cpp`; the
accompanying `BUG-REPORT.md` covers the first four in detail. Summary:

1. Missing backwards-compatibility macros (`tex2Doffset` and friends) — hard
   failure on CAS.fx and SMAA.fx.
2. Missing runtime macros — `BUFFER_COLOR_BIT_DEPTH` is a hard failure;
   `__RENDERER__` is worse, silently selecting the wrong branch in any effect
   that tests it.
3. `--invert-y` passed in the wrong argument position, so it does not invert Y
   and does silently enable real 16-bit types.
4. `--spirv` without `-E` writes a zero-byte file and exits 0.
5. `-E <unknown name>` exits 1 printing nothing at all. Entry point names are
   generated and spelled differently per back end — the HLSL back end keeps the
   `F` prefix at shader model 4+, SPIR-V and SM3 rename to `E` — so they are
   unguessable. Now they are listed, and `--list-entry-points` exists.
6. The disassembly listing the back ends produce was computed and discarded.
   Now written with `-Fc`, matching FXC's spelling.

## Testing

```
bash fxstat/test/run_tests.sh              # needs ../SweetFX and ../reshade-shaders
./reshadefx-cli/bin/reshadefx_coverage \
    -I path/to/reshade-shaders/Shaders --sm 50 --sm 40 --sm 30 *.fx
```
