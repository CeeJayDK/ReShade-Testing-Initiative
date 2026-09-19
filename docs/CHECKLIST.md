# Checklist

Ordered so each step is useful on its own. Nothing here depends on a later item.

---

## 1. Verify the build

Everything is in the repo now — no patch to apply, no files to copy.

- [ ] `./build_reshade_testing_initiative.sh` (or the `.bat` on Windows) —
      it prints whether SPIRV-Tools was found
- [ ] `reshadefx_rga --optimize -I <shaders> LumaSharpen.fx` -> expect
      `cheap_alu=21 texture=5 control_flow=0`, with a `before optimization:`
      line showing `cheap_alu=116 texture=16 control_flow=20`

If the `before optimization:` line is missing, the binary was built without
SPIRV-Tools and `--optimize` is a no-op.

## 2. On Windows specifically

- [ ] Build with the `.bat` and confirm the `-Os -fno-exceptions ... -s` flags
      still produce a working binary — the handoff flagged this as unverified
      and it still is
- [ ] `reshadefx_cli --dxbc --shader-model 50 -E <entry> <shader>.fx` against one
      real shader, confirm the output is sane. Still the biggest untested thing.
      Wine cannot substitute — I confirmed that a third time this session
      (its `d3dcompiler_47.dll` segfaulted on ReShade's generated HLSL)
- [ ] To find the entry point names: they are **mangled and differ per backend**
      — `F__` prefix for HLSL/DXBC at shader model 4+, `E__` for SPIR-V and SM3

## 3. File the vkd3d bug

Files are in `docs/upstream/vkd3d/` (`ISSUE.md` to paste, `SUBMITTING.md` for the steps).

**URL: https://gitlab.winehq.org/wine/vkd3d/-/issues**

Codeberg (`codeberg.org/vkd3d/vkd3d`) is only a **mirror** — their README says
development happens on the Wine GitLab instance. An issue opened on Codeberg may
sit unread.

Fallback: WineHQ Bugzilla at https://bugs.winehq.org — I could not check whether
a `vkd3d` product exists there (the site blocks automated fetching), so verify
when you get there; if there is none, file under Wine with a vkd3d component.

- [ ] Register at gitlab.winehq.org (manual approval in some periods — if it
      stalls, Bugzilla is faster)
- [ ] **Reproduce it yourself** with `repro_sm3.hlsl` before filing
- [ ] **Run `fxc /T ps_3_0 /E main repro_sm3.hlsl` on Windows.** I could not test
      FXC. The report claims D3DCompiler accepts the construct, inferred from
      your DX9 path working in production. If FXC actually rejects it, the report
      is wrong and I want to know
- [ ] Replace the version block with what you actually tested
- [ ] File it

## 4. Report the ReShade upstream bugs

`docs/upstream/reshade-fxc.md` and `tools/fxc-fix.py`. Six fixes to
`tools/fxc.cpp`, applies cleanly to v6.8.0 and master.

- [ ] Decide whether to file as one issue or a merge request
- [ ] Mention the seventh, separate one: `source/effect_preprocessor.cpp` calls
      `_wfsopen()` with `SH_DENYWR` but never includes `<share.h>`. MSVC provides
      it transitively, MinGW does not — your `.bat` already works around it with
      `-include share.h`

---

## What changed in reshadefx_rga

**`--optimize`** — the handoff's #1 open thread, now done. Runs the folding,
inlining, mem2reg and dead-branch passes a driver runs, before counting.
SPIRV-Tools is **linked, not shelled out to**, so the pass list and the
SPIRV-Tools version are pinned into the binary and two machines cannot quietly
produce different numbers. Builds fine without SPIRV-Tools; the flag then reports
it is unavailable.

The pass list is deliberately **not `spirv-opt -O`**. `-O` never runs
`--freeze-spec-const`, so the spec constants stay symbolic and not one dead
branch is removed — on LumaSharpen it leaves all 15 fetches standing. It also
inlines and unrolls, which moves counts for reasons unrelated to the edit being
measured.

**Environment fix** — `reshadefx_rga` defined only 6 of the 14 macros the runtime
defines, and none of the backwards-compatibility ones. That is why CAS.fx,
SMAA.fx and Deband.fx failed. **Corpus went 30/33 → 33/33.** Both preprocessor
setups in the file now call one shared helper so they cannot drift apart again.

**`--renderer <id>`** — the tool hardcoded OpenGL-semantics SPIR-V and never
defined `__RENDERER__` at all, so every renderer-conditional effect silently
compiled its fallback path. Default is now Vulkan (`0x20000`), overridable. This
is what makes the path audit possible:

```
CAS.fx, texture fetches:  DX10 → 9,  DX11 → 8,  DX12 → 8,  Vulkan → 8
```

**Inliner guard** — SPIRV-Tools' inliner declines some modules; when it does,
mem2reg has nothing to work on and counts come out inflated (on SMAA, 5×). The
tool now detects a surviving `OpFunctionParameter` and prints a WARNING that the
numbers are unusable, rather than reporting them as if they were real. Currently
fires on CRT.fx, FXAA.fx and DisplayDepth.fx.

### One thing I did not change

Your `apply_settings_to_source()` rewrites uniform defaults **in the source text**
before parsing, with a comment explaining that mutating
`module().spec_constants` afterwards has no effect. That analysis is correct and
the approach works — I verified `--load-settings` drives dead-branch elimination
properly (`pattern=0` → 3 fetches, `pattern=1..3` → 5).

There is a more robust alternative: patch the `OpSpecConstant` literal in the
**compiled SPIR-V binary** instead. It needs no source parsing, so it is immune
to expressions, macros and formatting, and it handles
`OpSpecConstantTrue`/`False` which text rewriting has to special-case. I left
your working code alone rather than swapping it out unasked — say the word if
you want it.
