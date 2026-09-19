# vkd3d-shader: HLSL front end rejects `static` structs mixing resource and numeric members

## Summary

vkd3d-shader's HLSL compiler rejects any `static` variable whose type contains
both a resource (sampler/texture) and a numeric member:

```
E5002: Static variables cannot have both numeric and resource components.
```

D3DCompiler accepts this construct. It is not an obscure corner: bundling a
sampler with its texel size in a struct is the standard way to carry texture
dimensions on shader model 3, where there is no `GetDimensions`, and shipping
software has used it for over a decade.

The check is in `libs/vkd3d-shader/hlsl.y` (around line 2707 as of the version
tested):

```c
if ((var->storage_modifiers & HLSL_STORAGE_STATIC) && type_has_numeric_components(var->data_type)
        && type_has_object_components(var->data_type))
{
    hlsl_error(ctx, &var->loc, VKD3D_SHADER_ERROR_HLSL_INVALID_TYPE,
            "Static variables cannot have both numeric and resource components.");
}
```

It is **not** shader-model gated — it fires on every profile, see repro 2.

## Reproduction 1 — shader model 3

```hlsl
struct Sampler2D { sampler2D s; float2 pixelsize; };

sampler2D backbuffer_s : register(s0);
static const Sampler2D backbuffer = { backbuffer_s, float2(0.001, 0.002) };

float4 main(float2 uv : TEXCOORD) : COLOR
{
    return tex2D(backbuffer.s, uv + backbuffer.pixelsize);
}
```

Compiled with `profile = "ps_3_0"`, `source_type = VKD3D_SHADER_SOURCE_HLSL`,
`target_type = VKD3D_SHADER_TARGET_D3D_BYTECODE`, and
`VKD3D_SHADER_COMPILE_OPTION_BACKWARD_COMPATIBILITY` set to
`VKD3D_SHADER_COMPILE_OPTION_BACKCOMPAT_MAP_SEMANTIC_NAMES`:

```
rc = -4 (VKD3D_ERROR_INVALID_SHADER)
<anonymous>:4:24: E5002: Static variables cannot have both numeric and resource components.
```

## Reproduction 2 — shader model 5, same failure

The restriction is independent of the profile and of the resource type:

```hlsl
struct Sampler2D { Texture2D<float4> t; SamplerState s; float2 pixelsize; };

Texture2D<float4> backbuffer_t : register(t0);
SamplerState backbuffer_ss : register(s0);
static const Sampler2D backbuffer = { backbuffer_t, backbuffer_ss, float2(0.001, 0.002) };

float4 main(float2 uv : TEXCOORD) : SV_Target
{
    return backbuffer.t.Sample(backbuffer.s, uv + backbuffer.pixelsize);
}
```

`profile = "ps_5_0"`, `target_type = VKD3D_SHADER_TARGET_DXBC_TPF`:

```
rc = -4 (VKD3D_ERROR_INVALID_SHADER)
<anonymous>:5:24: E5002: Static variables cannot have both numeric and resource components.
```

## What works

Splitting the numeric member out of the struct compiles cleanly, which suggests
the missing piece is splitting such a static into its resource and numeric halves
rather than anything deeper:

```hlsl
struct Sampler2D { sampler2D s; };
static const Sampler2D backbuffer = { backbuffer_s };
static const float2 backbuffer_pixelsize = float2(0.001, 0.002);
```

```
rc = 0
```

A struct containing only resources is also fine — `{ Texture2D<float4> t;
SamplerState s; }` compiles at ps_5_0 without complaint. It is specifically the
mix that is rejected.

## Why this matters for Wine

Wine's `d3dcompiler_47` is built on vkd3d-shader, so this affects any application
that compiles HLSL at runtime and uses the pattern — not only tools that ship
precompiled bytecode.

The concrete case that surfaced it is ReShade, which is very widely used with
games under Proton. ReShade's HLSL code generator emits this struct for **every**
shader model 3 effect, in the generated preamble
(`source/effect_codegen_hlsl.cpp`, in the `_shader_model < 40` branch):

```c
preamble +=
    "struct __sampler1D { sampler1D s; float1 pixelsize; };\n"
    "struct __sampler2D { sampler2D s; float2 pixelsize; };\n"
    "struct __sampler3D { sampler3D s; float3 pixelsize; };\n"
    "uniform float2 __TEXEL_SIZE__ : register(c255);\n";
```

and then one `static const __sampler2D name = { name_s, float2(...) };` per
sampler. Every D3D9 ReShade effect therefore fails to compile, while the same
effects build fine through D3DCompiler.

Measured on a corpus of 33 effects (SweetFX plus the standard reshade-shaders
set), 77 entry points:

| profile | entry points compiled |
|---|---|
| ps/vs_4_0 | 77 / 77 |
| ps/vs_4_1 | 77 / 77 |
| ps/vs_5_0 | 77 / 77 |
| ps/vs_5_1 | 77 / 77 |
| ps/vs_3_0 | 36 / 77 |

All 41 shader-model-3 failures are this one error. The 36 that pass are the
vertex shaders, which do not sample.

## Where this was reported

Codeberg is a mirror; development is on the Wine GitLab instance. See
SUBMITTING.md in this archive.

## Version tested

vkd3d-shader as vendored in the Wine tree at commit
`14947569cf7af78471216e83f22e1f7b1bb1edc0` (2026-09-14), `PACKAGE_VERSION "2.1"`,
built standalone against glibc with gcc 13.3.

## Note on the second repro

Repro 2 is not a construct ReShade actually emits — its shader model 4+ path uses
a resource-only struct and is unaffected. It is included because it shows the
check is a general HLSL front-end restriction rather than a shader-model-3 gap,
which may matter for how you choose to fix it.
