# Compile speed of `reshadefx_cli_fixed`

Where the time goes when `reshadefx_cli_fixed` compiles an effect, what was done
about it, and what is left. The rule for every change: **the output must not
change**. An effect has to compile here exactly as it does in ReShade.

Measured 2026-09-29 on 133 effects (SweetFX, reshade-shaders slim and legacy,
qUINT, iMMERSE, prod80, AstrayFX, Fubax) with 529 entry points, on 4 cores.

## Where the time goes

| mode | total | spent in |
|---|---|---|
| `--hlsl`, `--glsl` | 0.7 s | ReShade's front end: preprocessor, parser, code generation. 5 ms per effect, 20 ms for the largest (iMMERSE LAUNCHPAD) |
| `--spirv` | 3.9 s | the same front end, once per entry point, because each `-E` was a separate run. LAUNCHPAD alone: 52 entry points × 20 ms = 1 s |
| `--dxbc` | 50 s | vkd3d-shader, the HLSL-to-DXBC compiler behind `--dxbc` on Linux. The front end is under 1% of it |

For DXBC a few shaders take nearly all of the time, and each is a single entry
point: prod80 `PD80_01B_RT_Correct_Color` `PS_MinMax_1x1` 16.4 s, iMMERSE
`MartysMods_SMAA` `SMAABlendingWeightCalculationWrapCS` 8 s, legacy `Denoise`
5 s. Callgrind on the SMAA one: 98% of it is inside vkd3d's loop unroller
(`unroll_loops` → `loop_unrolling_unroll_loop` → `copy_propagation_transform_block`
and `clone_block`).

## What was done

### 1. Several entry points per run (`tools/fxc-fix.py`)

`-E` can be repeated, and `--all-entry-points` assembles every entry point. The
effect is preprocessed and parsed once, then the entry points are assembled on
`-j` threads (default: all cores). `-Fo out.cso` becomes `out.<entry point>.cso`.

| | one `-E` per run | `--all-entry-points` |
|---|---|---|
| `--spirv`, all 133 effects | 3.9 s | **0.8 s** |
| `--dxbc`, all 133 effects | 50 s | 43 s |

DXBC gains little because its time is in single huge shaders, which threads
can't split (see *Left for later*). All 529 entry points were compared byte for
byte against separate `-E` runs of the previous build, in `--hlsl`, `--glsl`,
`--spirv` and `--dxbc`: identical, including which ones fail.

This one is a tool change. ReShade itself already parses an effect once and
compiles its entry points from that, so there is nothing to carry over.

### 2. Token reuse in the front end (`patches/reshade-token-reuse.patch`)

Every token the lexer produces carries a `location`, and `location.source` is a
`std::string` holding the file path. Paths are longer than the 15 characters
libstdc++ stores inline, so each token cost a heap allocation plus a copy, then
a free when the token was discarded. The preprocessor also made one more
`substr()` copy per token.

The patch adds `lexer::lex(token &)`, which fills an existing token. The parser
and preprocessor keep two tokens and swap them instead of moving a new token in
each time, so the strings reuse their buffers. The substring copy becomes
`assign()` into the existing string. `token lex()` stays for other callers.

| | before | after |
|---|---|---|
| instructions, LAUNCHPAD `--hlsl` (callgrind) | 100.9 M | **87.1 M** (−14%) |
| wall time, LAUNCHPAD `--hlsl` (median of 30) | 22.2 ms | 20.3 ms |
| wall time, all 133 effects `--hlsl` | 0.65 s | 0.62 s |

Output is identical in all four modes on all 133 effects, for both
`reshadefx_cli_fixed` and `reshadefx_cli`.

**For ReShade itself:** this patch applies to ReShade's own source and would
work there unchanged. The gain is real but small: a few ms per effect, while
ReShade's loading time is dominated by D3DCompiler (or the driver), not by this
front end. After the patch the profile is flat: string handling, macro hash
lookups and memcpy, with no single hot spot left.

Both build scripts apply it. The Windows `.bat` applies `patches/` with
`git apply --ignore-whitespace`, because with `core.autocrlf` the patch or the
ReShade checkout can have CRLF line endings.

## Left for later

### vkd3d's loop unroller (the big DXBC win)

vkd3d unrolls a loop by cloning its body once per iteration and running copy
propagation again after each one. The work grows much faster than the loop does:
a loop of a few dozen iterations with a big body takes seconds. This is where
nearly all of the 50 s goes.

A fix belongs in vkd3d (and Wine, which vendors it), not in ReShade: for example
propagating copies once per unrolled iteration instead of over the whole
unrolled block each time. It would have to produce identical DXBC, which the
benchmark below can check. Harder than the two changes above, and a patch to
upstream vkd3d.

Separately: vkd3d rejects `[fastopt]`, which ReShade emits for `[loop]` at shader
model 4+. The vkd3d back end now passes it on as `[loop]`; see
`docs/DXBC-ON-LINUX.md`. The DXBC total above (50 s) was measured before that;
with 21 more effects compiling it is 54 s.

## How it was checked

A throwaway harness, not in the repo: for every effect and entry point, record
the time and a hash of the output file plus stdout and stderr, once with the
previous build and once with the new one, then compare. The multi-entry-point
check ran the previous build once per `-E` and the new one with
`--all-entry-points`, and compared each output file.
