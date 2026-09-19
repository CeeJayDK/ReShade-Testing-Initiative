# Handoff — deferred tasks

Everything here is deliberately parked. Each section says what exists, what the
next concrete step is, and what will bite. Nothing here is required for the
native tools to be finished.

Current state of the work these branch off:

| piece | state |
|---|---|
| `reshadefx_cli` with working `--dxbc` on Linux | done, 77/77 entry points SM4.0–5.1 |
| `libvkd3d-shader.a` built natively from Wine's vendored copy | done, scripted |
| `fxstat` split into `libfxstat_core` + thin CLI | done |
| SPIRV-Tools linked rather than shelled out to | done |
| six `tools/fxc.cpp` fixes | done, patch + bug report written |
| Windows build | **not done** — see the native tools work, not this file |

---

## 1. WASM build / web tool

**Why deferred:** the native tools come first, and the blocker is already gone.

**What made it possible:** the `popen("spirv-opt")` call is gone — SPIRV-Tools is
linked through `spvtools::Optimizer`. And `libfxstat_core` has no printf, no
`exit()`, no process spawning and no disk writes. `examples/minimal.cpp` compiles
an effect from an in-memory string with no filesystem involvement at all; that is
the exact shape a binding wraps.

**Why this can be a static page, unlike Compiler Explorer:** Compiler Explorer
runs native compilers as processes, so it needs a backend. Our three dependencies
are all self-contained libraries with no OS dependency beyond libc — reshadefx
(C++), vkd3d-shader (C), SPIRV-Tools (C++). All three compile to WASM. This is a
consequence of avoiding D3DCompiler, which is a Windows DLL and could never have
been ported.

**Next steps, in order:**

1. Emscripten toolchain file; build SPIRV-Tools for `wasm32` first — it is the
   dependency most likely to just work, so it validates the setup cheaply.
2. Build vkd3d-shader for `wasm32`. **Test this early.** It is C with no OS
   dependencies so it should port cleanly, but "should" is not "did", and it is
   the one that would sink the plan. `build-vkd3d-shader.sh` already parameterises
   the compiler; the shim headers are platform-neutral.
3. Build `libfxstat_core` with `emcc`.
4. `EMSCRIPTEN_BINDINGS` over `compile_source()` and `parse_preset()`. The result
   structs are plain data; `val`/`embind` handles them without ceremony.
5. Preload a shader pack into MEMFS and pass the mount point in `include_paths`
   so `#include "ReShade.fxh"` resolves.

**Gotchas:**

- `std::filesystem` under Emscripten works on MEMFS, but paths are case-sensitive
  where Windows users' shader packs may not be.
- Bundle size will be several MB of wasm. It gzips well; serve it compressed.
- **RGA/amdllpc cannot be ported.** It is LLVM-based and enormous. Hardware ISA
  stats, register pressure and occupancy stay native-only. The web tool covers
  compile + DXBC/SPIR-V counts + diffs and stops there. Do not promise more.
- Hosting: a static page on GitHub Pages. No server, no cost, no maintenance.

---

## 2. Preset × renderer sweep

**Why deferred:** the single-axis renderer sweep already found the two real bugs,
and this is a much larger cross product.

**What exists:** `audit/audit_paths.py` sweeps `__RENDERER__` across all eight
renderer IDs and groups effects by preprocessed output. `audit/AUDIT.md` has the
results — 4 of 33 effects are API-dependent, FXAA.fx and SMAA.fx both stale.

**What it misses, and why it matters:** the sweep uses default uniform values.
SMAA proved that path coverage is a function of the **preset as well as the API**
— a branch on a runtime uniform only reaches the compiler in performance mode
with a preset that selects it. SMAA's gather bug is invisible at
`EdgeDetectionType=0` and costs 3× the fetches at `EdgeDetectionType=2`.

**Next step:** enumerate the combo/int uniforms in each effect (they are in
`module().uniforms` with their `ui_items` annotation giving the value range),
then sweep each value against each renderer ID. Report any combination where the
compiled output differs from what the same preset gives on DX11.

**Gotcha:** this is a genuine cross product and most of it is noise. Filter to
uniforms that actually gate a `#ifdef`/`#if` or an `if` on a spec constant,
rather than sweeping every slider.

---

## 3. Submitting the vkd3d issue

Fully written up. See `docs/upstream/vkd3d/SUBMITTING.md` — routes, steps, and the three
things I could not verify from a sandbox (that FXC accepts the construct, whether
a `vkd3d` product exists in WineHQ Bugzilla, and the exact `vkd3d-compiler` CLI
flags).

**One correction to carry forward:** Codeberg is a mirror. Development is on
`gitlab.winehq.org/wine/vkd3d/`.

---

## 4. Hardware cost stats (RGA)

**Why deferred:** DXBC and SPIR-V counts answer "did this edit shrink it", which
is what the diffing workflow needs. Hardware cost is a different question.

**What to do when you want it:** `fxstat --spirv --dump <dir>` already writes the
optimised SPIR-V. Feed it to `rga -s vk-offline`, which runs offline on Windows
and Linux with no GPU and no driver, and gives RDNA ISA with a VALU/SALU/VMEM/SMEM
split, register pressure and occupancy. That split is the one that actually
predicts whether an effect is slow.

The natural shape is a `--rga <path>` option that shells out and merges the
result into the JSON. It is a native-only feature by definition.

---

## 5. Bandwidth

**The gap nothing here closes.** Most ReShade effects are bandwidth-bound, not
ALU-bound. A 9-tap and a 49-tap blur have nearly identical per-invocation counts.

**Cheap partial fix:** report pass count, render target format and resolution
scale alongside the instruction counts. All three are already in
`module().techniques` and `module().textures` — no new analysis needed, just
surfacing. That would stop an agent concluding a multi-pass blur is cheap because
its per-pass count is low.

**Real fix:** a headless timestamp-query benchmark. Much larger, and a different
kind of tool.
