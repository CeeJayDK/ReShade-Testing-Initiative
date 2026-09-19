# Renderer-path audit — SweetFX + reshade-shaders

Method: preprocess each effect once per renderer ID, group the renderers by the
resulting source. Any effect producing more than one group is making an
API-specific decision. Then compile each group and measure what it costs.

Renderer IDs as `source/runtime.cpp` assigns them: DX9 `0x9000`, DX10 `0xa000`,
DX10.1 `0xa100`, DX11 `0xb000`, DX11.1 `0xb100`, DX12 `0xc000`, OpenGL
`0x10000`, Vulkan `0x20000`.

**33 effects checked, 4 are API-dependent.** Two are correct, two use exact
equality and have gone stale.

---

## Correct

### CAS.fx — `#if __RENDERER__ >= 0xb000`

```
DX9, DX10, DX10.1                         -> non-gather path
DX11, DX11.1, DX12, OpenGL, Vulkan        -> gather path
```

`>=` degrades correctly: every renderer added after this was written lands on the
gather path automatically. Nothing to do.

### CRT.fx — `#if __RENDERER__ < 0xa000 && !__RESHADE_PERFORMANCE_MODE__`

```
DX9                                       -> DX9 path
everything else                           -> modern path
```

Also fine — `<` excludes rather than includes, so new renderer IDs fall on the
correct side.

---

## Stale

Both use `== 0xb000 || == 0xb100`, written when DX11 was the newest renderer.
DX12, OpenGL and Vulkan all fail the test and silently take the fallback path,
despite all three supporting gather.

### FXAA.fx:45 — costs 4 texture fetches per pixel, today, at default settings

```c
#if (__RENDERER__ == 0xb000 || __RENDERER__ == 0xb100)
	#define FXAA_GATHER4_ALPHA 1
	#define FxaaTexAlpha4(t, p) tex2Dgather(t, p, 3)
	#define FxaaTexOffAlpha4(t, p, o) tex2Dgatheroffset(t, p, o, 3)
	#define FxaaTexGreen4(t, p) tex2Dgather(t, p, 1)
	#define FxaaTexOffGreen4(t, p, o) tex2Dgatheroffset(t, p, o, 1)
#endif
```

`F__FXAAPixelShader`:

| renderer | gather | TEX | total |
|---|---|---:|---:|
| DX11 | yes | **20** | 477 |
| DX12 | no | **24** | 483 |
| Vulkan | no | **24** | — |
| OpenGL | no | **24** | — |

Suggested fix: `#if __RENDERER__ >= 0xb000`.

### SMAA.fx:115 — latent; costs 3× the fetches, but only under one preset

```c
#if (__RENDERER__ == 0xb000 || __RENDERER__ == 0xb100)
	#define SMAAGather(tex, coord) tex2Dgather(tex, coord, 0)
#endif
```

This one is invisible at default settings, which is why it is worth having a tool
for. `SMAAGather` is consumed by `SMAAGatherNeighbours` in SMAA.fxh:

```c
float3 SMAAGatherNeighbours(float2 texcoord, float4 offset[3], SMAATexture2D(tex)) {
    #ifdef SMAAGather
    return SMAAGather(tex, texcoord + SMAA_RT_METRICS.xy * float2(-0.5, -0.5)).grb;  // 1 fetch
    #else
    float P     = SMAASamplePoint(tex, texcoord).r;                                   // 3 fetches
    float Pleft = SMAASamplePoint(tex, offset[0].xy).r;
    float Ptop  = SMAASamplePoint(tex, offset[0].zw).r;
    ...
```

which is reached only from `SMAADepthEdgeDetectionPS` and the predication path.
`EdgeDetectionType` is a **runtime uniform**, so whether that code is compiled at
all depends on the preset:

`F__SMAAEdgeDetectionWrapPS`, performance mode:

| preset | renderer | TEX | total |
|---|---|---:|---:|
| `EdgeDetectionType=0` (luma, default) | DX11 | 7 | 50 |
| `EdgeDetectionType=0` | DX12 | 7 | 50 |
| `EdgeDetectionType=2` (depth) | DX11 | **1** | 13 |
| `EdgeDetectionType=2` | DX12 | **3** | 17 |
| `EdgeDetectionType=2` | Vulkan | **3** | 17 |
| `EdgeDetectionType=2` | OpenGL | **3** | 17 |

Three times the fetches on anything that is not DX11, and zero difference at the
default preset. Neither code inspection nor testing the default configuration
would surface this.

Suggested fix: same, `#if __RENDERER__ >= 0xb000`.

Note also that SMAA.fx defines no `SMAA_HLSL_*` or `SMAA_GLSL_*` level, so
SMAA.fxh's own gather definitions (guarded by `SMAA_HLSL_4_1` and `SMAA_GLSL_4`)
never fire. Line 115 is the only source of `SMAAGather` in this build.

---

## What this does not cover

The sweep varies `__RENDERER__` only, with default uniform values except where a
preset is named. The SMAA case shows that **path coverage is a function of the
preset as well as the API** — a branch on a runtime uniform only reaches the
compiler in performance mode with a preset that selects it.

A complete audit would sweep the combo/int uniforms that gate branches against
each renderer ID. That is a larger cross product, but it is the same method and
the same tool.

## Reproducing

```
python3 audit_paths.py --cli <reshadefx_cli> \
    -I reshade-shaders/Shaders -I SweetFX/Shaders \
    --scan-dirs SweetFX/Shaders --scan-dirs reshade-shaders/Shaders \
    SweetFX/Shaders/SweetFX/*.fx reshade-shaders/Shaders/*.fx
```

Cost measurement, per renderer:

```
fxstat --dxbc --shader-model 50 -D __RENDERER__=0xc000 \
    --preset preset.ini -I reshade-shaders/Shaders --json MyShader.fx
```
