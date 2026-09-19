# ReShadeFXC (`tools/fxc.cpp`) diverges from the runtime and miscompiles effects

Reproduced against **v6.8.0** (18deaa5) and **master** (eeb2c76, 2026-09-12). The
patch below applies unchanged to both.

`tools/fxc.cpp` builds a preprocessor environment by hand instead of the one
`source/runtime.cpp` builds before compiling the same effects. The two have
drifted. The result is that ReShadeFXC rejects effects ReShade compiles fine,
and — worse — silently compiles the wrong branch of others.

Tested on 33 effects (the SweetFX shader set plus `reshade-shaders`):

| | stock | with patch |
|---|---|---|
| compile | 30 / 33 | **33 / 33** |
| correct `__RENDERER__` branch | 0 / 4 | **4 / 4** |

---

## 1. Missing backwards-compatibility macros — hard failure

`runtime.cpp` injects a block of conversion macros before the effect source
(`runtime.cpp:1680` on master):

```cpp
pp.append_string(
    "#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
    "#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
    "#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
    "#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
    "#define tex2Dgather0 tex2DgatherR\n"
    ...);
```

`fxc.cpp` does not. These are not intrinsics — `tex2Doffset` appears nowhere in
`effect_symbol_table_intrinsics.inl` — so without the block the effect simply
fails to compile:

```
$ reshadefx_cli --hlsl -I reshade-shaders/Shaders SweetFX/Shaders/SweetFX/CAS.fx
SweetFX/Shaders/SweetFX/CAS.fx(87, 12): error X3004: undeclared identifier or
no matching intrinsic overload for 'tex2Doffset'
```

Affected in the test set: `CAS.fx`, `SMAA.fx`.

## 2. Missing runtime macros — silent wrong-branch compilation

`fxc.cpp` defines 6 macros. `runtime.cpp` defines 14.

| defined by `runtime.cpp` | defined by `fxc.cpp` |
|---|---|
| `__RESHADE__` | yes |
| `__RESHADE_PERFORMANCE_MODE__` | yes |
| `BUFFER_WIDTH` / `BUFFER_HEIGHT` | yes |
| `BUFFER_RCP_WIDTH` / `BUFFER_RCP_HEIGHT` | yes |
| `__RENDERER__` | **no** |
| `__RESHADE_PERMUTATION__` | **no** |
| `__VENDOR__`, `__DEVICE__`, `__APPLICATION__` | **no** |
| `BUFFER_COLOR_SPACE`, `BUFFER_COLOR_FORMAT` | **no** |
| `BUFFER_COLOR_BIT_DEPTH` | **no** |

`BUFFER_COLOR_BIT_DEPTH` is used in shader expressions, so its absence is a hard
error (`Deband.fx`).

`__RENDERER__` is the serious one. An undefined identifier evaluates to `0` in
`#if`, so every renderer-conditional effect compiles its fallback path with **no
diagnostic at all**:

```c
// CAS.fx:91  -- 0 >= 0xb000 is false, so the DX11 gather path is never compiled
#if __RENDERER__ >= 0xb000

// CRT.fx:276 -- 0 < 0xa000 is true, so the DX9 path is always compiled
#if __RENDERER__ < 0xa000 && !__RESHADE_PERFORMANCE_MODE__

// SMAA.fx:115, FXAA.fx:45 -- == 0xb000 is false
```

ReShadeFXC therefore reports success on a shader that is not the shader ReShade
would build. For a tool whose purpose is validating effects, that is the most
damaging of these.

The patch derives `__RENDERER__` from the selected back end and shader model,
inverting the mapping `runtime.cpp:1762-1774` already uses. Verified: `CAS.fx`
at `--shader-model 50` now emits `GatherRed`, at `--shader-model 30` it does not.

Related: `__RESHADE_PERFORMANCE_MODE__` is hardcoded to `"0"` even when
`--spec-constants` is passed — and `--spec-constants` *is* performance mode
(`runtime.cpp` passes `_performance_mode` into that same codegen argument). So
an effect can be compiled with its uniforms folded to constants while the macro
that tells it so reads 0. `CRT.fx` branches on this.

## 3. `--invert-y` is passed in the wrong argument position

```cpp
// effect_codegen.hpp:403,421
codegen *create_codegen_glsl (bool vulkan_semantics, bool debug_info,
                              bool uniforms_to_spec_constants,
                              bool enable_16bit_types = false,
                              bool flip_vert_y = false);

// fxc.cpp:260,262
backend.reset(reshadefx::create_codegen_glsl (vulkan_semantics, debug_info, spec_constants, invert_y_axis));
backend.reset(reshadefx::create_codegen_spirv(vulkan_semantics, debug_info, spec_constants, invert_y_axis));
```

`invert_y_axis` lands in `enable_16bit_types`. So `--invert-y` does **not**
invert Y, and **does** silently switch `min16float`/`min16int`/`min16uint` to
real 16-bit types, changing the precision of the generated code.

This looks like fallout from `enable_16bit_types` being inserted into the
signature ahead of `flip_vert_y`; `runtime.cpp` passes all five arguments
explicitly and is unaffected.

## 4. `--spirv` without `-E` writes an empty file and exits 0

`codegen_spirv::finalize_code()` returns an empty string by design ("There is no
high-level text representation"). `fxc.cpp` calls it whenever `-E` is absent and
writes the result:

```
$ reshadefx_cli --spirv -Fo out.spv shader.fx ; echo $?
0
$ ls -la out.spv
-rw-r--r-- 1 user user 0 ... out.spv
```

Zero bytes, success exit code, no message. The patch reports the problem and
lists the available entry points, since ReShade mangles the names and there is
otherwise no way to discover them:

```
error: the SPIR-V back end produces one module per entry point; pass -E <name> to select one.
Available entry points:
  E__PostProcessVS
  E__CASPass
```

---

## Root cause

All four are the same thing: `fxc.cpp` duplicates setup that `runtime.cpp` owns,
and only `runtime.cpp` gets maintained. The durable fix is to factor the
preprocessor environment into one shared helper that both call — something like
`reshadefx::setup_preprocessor(pp, renderer_id, width, height, performance_mode)`
— so the next macro added to the runtime cannot go missing from the tool. The
patch attached does the minimal thing and keeps the duplication; a shared helper
would be the better change if you want it.

## 5. `-E <unknown name>` exits 1 printing nothing at all

`assemble_code_for_entry_point()` returns false and all three error strings are
empty, so fxc.cpp prints a bare newline and exits 1.

This matters because entry point names are generated and cannot be guessed. They
are also spelled differently per back end: `define_entry_point()` in
`effect_codegen_hlsl.cpp` renames the `F` prefix to `E` only when
`_shader_model < 40` or the shader is a compute shader, while the SPIR-V back end
always renames. So the same effect needs `-E F__MyPass` for `--hlsl
--shader-model 50` and `-E E__MyPass` for `--spirv`, with nothing in the tool to
tell you which.

The patch lists the available entry points on an unknown name, and adds a
`--list-entry-points` option.

## 6. The disassembly listing is computed and discarded

`assemble_code_for_entry_point()` takes an `assembly` out-parameter and the DXBC
back end fills it in via `D3DDisassemble()`. fxc.cpp declares it as a local and
never uses it. The patch writes it with `-Fc <path>`, which is FXC's spelling for
the same thing.

## Files

- `fxc-fix.py` — applies all six fixes to a checkout, idempotent
- `README.md` in this archive — the DXBC work these fixes were found during
