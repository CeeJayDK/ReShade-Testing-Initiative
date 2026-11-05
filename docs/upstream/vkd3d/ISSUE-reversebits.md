# vkd3d-shader: `reversebits` intrinsic is not defined

## Summary

`reversebits()` fails to compile:

```
<anonymous>:23:6: E5002: Can't implicitly convert from <error type> to uint.
```

`reversebits` is a standard shader model 5 HLSL intrinsic and D3DCompiler
accepts it (`bfrev`). `countbits`, `firstbitlow` and `firstbithigh` are in
vkd3d's `intrinsic_functions[]` table (`libs/vkd3d-shader/hlsl.y`);
`reversebits` is not.

## Repro (ps_5_0)

```hlsl
float4 main(float4 pos : SV_Position) : SV_Target
{
    return float(reversebits(uint(pos.x)) >> 28);
}
```

## Suggested fix

Add `reversebits` next to `countbits`, lowered to `VSIR_OP_BFREV`. The opcode
already exists, including its TPF encoding (`VKD3D_SM5_OP_BFREV` in `tpf.c`).

## Workaround in this repository

`cli/vkd3d_hlsl_fixups.hpp` replaces `reversebits(x)` with a call to a
parallel bit swap (the one ReShade itself uses below shader model 5) that it
defines ahead of the shader. `shaderlab/patches/reshade-vkd3d-bits.patch`
makes ShaderLab's ReShade build use ReShade's own fallback at every shader model.

## Tested

vkd3d 2.1 as bundled with Wine master 7b3fff76fa5178f6ce0141b2c776afa2a822f101
(2026-09-18), with ReShade 6.8.0. Not a ReShade bug.

Where to file: see `SUBMITTING.md` (Wine GitLab issues).
