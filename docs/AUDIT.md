# Renderer-path audit — SweetFX + reshade-shaders

Method: preprocess each effect once per renderer ID, group the renderers by the
resulting source. Any effect producing more than one group is making an
API-specific decision. Then compile each group and measure what it costs.

Renderer IDs as `source/runtime.cpp` assigns them: DX9 `0x9000`, DX10 `0xa000`,
DX10.1 `0xa100`, DX11 `0xb000`, DX11.1 `0xb100`, DX12 `0xc000`, OpenGL
`0x10000`, Vulkan `0x20000`.

**33 effects checked, 4 are API-dependent, all 4 correct.** FXAA.fx and SMAA.fx
used exact equality and had gone stale; both fixed in SweetFX `407c115`.

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

## Fixed — FXAA.fx and SMAA.fx

Both used `#if (__RENDERER__ == 0xb000 || __RENDERER__ == 0xb100)`, written when
DX11 was the newest renderer. DX12, OpenGL and Vulkan failed the test and
silently took the non-gather path, despite all three supporting gather. Both now
use `>= 0xb000` like CAS.fx (SweetFX `407c115`), and a re-run of the audit shows
them splitting at DX11 like CAS.

**FXAA.fx:45** — cost 4 extra texture fetches per pixel at default settings.
`F__FXAAPixelShader`, TEX:

| renderer | before | after |
|---|---:|---:|
| DX11 | 20 | 20 |
| DX12, OpenGL, Vulkan | 24 | 20 |

**SMAA.fx:115** — latent: no difference at the default preset, 3× the fetches
with `EdgeDetectionType=2` (depth). `F__SMAAEdgeDetectionWrapPS`, performance
mode, TEX at `EdgeDetectionType=2`: DX11 1, everything newer 3 before the fix.
SMAA.fx defines no `SMAA_HLSL_*`/`SMAA_GLSL_*` level, so SMAA.fxh's own gather
defines never fire and line 115 is the only source of `SMAAGather`.

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
