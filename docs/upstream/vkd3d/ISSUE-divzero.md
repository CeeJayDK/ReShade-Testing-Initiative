# vkd3d-shader: integer division by a constant zero in dead code is an error (E5021)

## Summary

Constant folding treats integer `/` and `%` by zero as a hard error, even when
the division sits in a branch that is folded away right after:

```
<anonymous>:9:8: E5021: Division by zero.
```

D3DCompiler only warns (X4010) and compiles. For floats vkd3d already does the
same: `W5301: Floating point division by zero.` Seen in ReShade's legacy
`DOF.fx`, whose author guarded the modulo exactly to avoid this and left a
comment saying so.

## Repro (ps_5_0)

```hlsl
float4 main(float4 pos : SV_Position) : SV_Target
{
    int cycle = 0, n = 0;
    float sum = 0;
    [unroll] for (int i = 0; i < 4; i++)
    {
        if (cycle != 0)
        {
            if (n % cycle == 0)
                sum += pos.x;
        }
        cycle += 2;
        n++;
    }
    return sum;
}
```

The modulo can never divide by zero. After unrolling, the first iteration has
`cycle == 0` and the guarded block is dead, but `n % cycle` is folded before the
block is removed. Harness: `repro_divzero.c`.

## Suggested fix

In `fold_div()` and `fold_mod()` (`libs/vkd3d-shader/hlsl_constant_ops.c`), for
`HLSL_TYPE_INT` and `HLSL_TYPE_UINT`, emit `VKD3D_SHADER_WARNING_HLSL_DIVISION_BY_ZERO`
instead of `VKD3D_SHADER_ERROR_HLSL_DIVISION_BY_ZERO` and keep returning `false`
(do not fold). Dead code is then removed as usual, and a live division stays a
run-time instruction, as with D3DCompiler. This repo carries exactly that as
`patches/vkd3d/fold-division-by-zero.patch`; on a 133-effect corpus it changes
no output that compiled before.

## Tested

vkd3d as bundled with Wine master 6880117 (2026-09-28). Found with ReShade 6.8.0
effects. Not a ReShade bug.

Where to file: see `SUBMITTING.md` (Wine GitLab issues).
