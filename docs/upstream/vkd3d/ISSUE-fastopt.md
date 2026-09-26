# vkd3d-shader: `[fastopt]` loop attribute aborts compilation (E5017)

## Summary

A loop marked `[fastopt]` fails to compile:

```
<anonymous>:4:13: E5017: Aborting due to not yet implemented feature: Unhandled attribute 'fastopt'.
```

D3DCompiler accepts it. ReShade's HLSL code generator emits `[fastopt]` for every loop
an effect marks `[loop]` at shader model 4 and later, so every such ReShade effect
fails under vkd3d (e.g. qUINT_dof, PD80_02_Bloom, AstrayFX Flair).

## Repro (ps_5_0)

```hlsl
float4 main(float4 pos : SV_Position) : SV_Target
{
    float s = 0;
    [fastopt] for (int i = 0; i < 4; ++i)
        s += pos.x * i;
    return s;
}
```

Harness: `repro_fastopt_isnan.c` (links `libvkd3d-shader`, compiles to DXBC TPF).

## Where

`libs/vkd3d-shader/hlsl.y`, `create_loop()`: `fastopt` (and `allow_uav_condition`)
go to `hlsl_fixme()`, which aborts.

## Suggested fix

`[fastopt]` only asks for faster compilation; per Microsoft's HLSL docs the compiler
then does not unroll the loop. Treating it like `[loop]` (`HLSL_LOOP_FORCE_LOOP`), or
ignoring it with a warning, gives correct code.

## Tested

vkd3d 2.1 as bundled with Wine master 7b3fff76fa5178f6ce0141b2c776afa2a822f101
(2026-09-18). Found with ReShade 6.8.0 effects rendered through Wine (ShaderLab).
Not a ShaderLab or ReShade bug. Workaround used: ReShade built to emit `[loop]`
(`shaderlab/patches/reshade-loop-attribute.patch`).

Where to file: see `SUBMITTING.md` (Wine GitLab issues).
