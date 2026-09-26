# vkd3d-shader: `isnan` intrinsic is not defined (E5005)

## Summary

`isnan()` fails to compile:

```
<anonymous>:3:8: E5005: Function "isnan" is not defined.
```

`isnan` is a standard HLSL intrinsic and D3DCompiler accepts it. `isinf` is already
implemented (`intrinsic_isinf`, `HLSL_OP1_ISINF`); `isnan` is missing from the
intrinsic table. Seen in ReShade effects (Fubax PerfectPerspective).

## Repro (ps_5_0)

```hlsl
float4 main(float4 pos : SV_Position) : SV_Target
{
    return isnan(pos.x) ? 0 : 1;
}
```

Harness: `repro_fastopt_isnan.c` (links `libvkd3d-shader`, compiles to DXBC TPF).

## Suggested fix

Add `isnan` next to `isinf` in `intrinsic_functions[]` (`libs/vkd3d-shader/hlsl.y`),
either as its own op or lowered to `x != x` (unordered compare). `isfinite` is
missing too and can be `!isinf(x) && !isnan(x)`.

## Tested

vkd3d 2.1 as bundled with Wine master 7b3fff76fa5178f6ce0141b2c776afa2a822f101
(2026-09-18). Found with ReShade 6.8.0 effects rendered through Wine (ShaderLab).
Not a ShaderLab or ReShade bug.

Where to file: see `SUBMITTING.md` (Wine GitLab issues).
