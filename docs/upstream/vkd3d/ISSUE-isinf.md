# vkd3d-shader: `isinf` outside the entry point is not implemented (E5017)

## Summary

`isinf()` compiles in the entry point but not in any function it calls:

```
<anonymous>:29:13: E5017: Aborting due to not yet implemented feature: SM4 isinf expression.
```

At shader model 3 it fails everywhere, including the entry point:

```
E5017: Aborting due to not yet implemented feature: SM1 isinf expression of type bool.
```

`isinf` is a standard HLSL intrinsic and D3DCompiler accepts both. ReShade puts
most effect code in functions, so this is the case that matters. `lower_isinf`
(`libs/vkd3d-shader/hlsl_codegen.c`) runs on the entry function's body only, so
an `HLSL_OP1_ISINF` in a called function reaches the backend unlowered.

Separately, the SM3 template there, `1 / x <= 0`, is true for every negative
finite `x`, so it would be wrong even where it is applied.

## Repro (ps_5_0)

```hlsl
bool test(float x)
{
    return isinf(x);
}

float4 main(float4 pos : SV_Position) : SV_Target
{
    return test(1.0 / (pos.x - 100.0)) ? 1 : 0;
}
```

## Suggested fix

Run `lower_isinf` over every function body (or after inlining), as the other
lowering passes do. For SM2/3, `abs(x) > FLT_MAX` is exact: only plus or minus
infinity is larger in magnitude than the largest finite float, and NaN
compares false.

## Workaround in this repository

`cli/vkd3d_hlsl_fixups.hpp` hands vkd3d `(abs(x) > 3.40282347e+38)` in place
of `isinf(x)`; `shaderlab/patches/reshade-vkd3d-bits.patch` makes ShaderLab's
ReShade build emit the same.

## Tested

vkd3d 2.1 as bundled with Wine master 7b3fff76fa5178f6ce0141b2c776afa2a822f101
(2026-09-18), with ReShade 6.8.0. Not a ReShade bug.

Where to file: see `SUBMITTING.md` (Wine GitLab issues).
