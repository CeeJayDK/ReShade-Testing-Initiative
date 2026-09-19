# Submitting the vkd3d bug report — step by step

## Where it goes

**Codeberg is only a mirror.** The repo README says:

> Development of vkd3d happens on the Wine GitLab instance
> (https://gitlab.winehq.org/wine/vkd3d/). Contributors are encouraged to submit
> their patches using the merge request tool.

So an issue opened on Codeberg may sit unread. Use one of these instead:

| route | URL | notes |
|---|---|---|
| **Wine GitLab issues** (preferred) | https://gitlab.winehq.org/wine/vkd3d/-/issues | Where development happens. Needs a WineHQ account. |
| WineHQ Bugzilla | https://bugs.winehq.org | Long-standing tracker. Check whether a `vkd3d` product exists when you get there — I could not verify the product list from my sandbox. If there is no vkd3d product, file under Wine with component `vkd3d` or `directx-d3d`. |
| wine-devel mailing list | https://gitlab.winehq.org/wine/wine/-/wikis/Mailing-Lists | For discussion, or if you want to send a patch rather than a report. |

Pick one. Do not cross-post; note in the report if you have already raised it elsewhere.

## Steps

### 1. Make an account

Register at https://gitlab.winehq.org (or https://bugs.winehq.org for Bugzilla).
WineHQ GitLab registration is manual approval in some periods — if it stalls,
Bugzilla is the faster route.

### 2. Reproduce it yourself first

Do not file on my word alone. Two options.

**Option A — with `vkd3d-compiler`** (easiest, if you have a vkd3d build):

```
vkd3d-compiler --profile ps_3_0 -x hlsl -b d3dbc repro_sm3.hlsl
```

Check `vkd3d-compiler --help` for the exact spelling of the source/target flags
in your version — I could not test the CLI here, only the library API.

**Option B — with the harness in this archive** (what I actually ran):

```
# build libvkd3d-shader.a first, from the other archive:
./tools/build-vkd3d-shader.sh /tmp/vkd3d

gcc -O2 -w -I /tmp/vkd3d/wine-src/libs/vkd3d/include -I /tmp/vkd3d/shim \
    repro_harness_sm3.c /tmp/vkd3d/libvkd3d-shader.a -o repro_sm3 -lm -lpthread
./repro_sm3
```

Expected output:

```
ps_3_0: rc=-4
<anonymous>:4:24: E5002: Static variables cannot have both numeric and resource components.
```

Same for `repro_harness_sm5.c`, which shows the restriction is not shader-model
specific.

### 3. Confirm D3DCompiler accepts it

I could not run FXC. You can, on Windows:

```
fxc /T ps_3_0 /E main repro_sm3.hlsl
```

If it compiles, say so explicitly in the report — "verified against
d3dcompiler_47 version X" is much stronger than "D3DCompiler accepts this".
If it does *not* compile, stop: the report is wrong and I would want to know.

### 4. Note your versions

Replace the version block in `ISSUE.md` with what you actually tested:

- vkd3d version or commit hash
- how you built it (upstream `./configure && make`, or the Wine-vendored route)
- the FXC version from step 3

Mine was Wine-vendored vkd3d at wine commit
`14947569cf7af78471216e83f22e1f7b1bb1edc0` (2026-09-14), `PACKAGE_VERSION "2.1"`,
gcc 13.3, glibc.

### 5. File it

Title:

```
vkd3d-shader: HLSL front end rejects static structs mixing resource and numeric members (E5002)
```

Body: paste `ISSUE.md`. Attach `repro_sm3.hlsl`, `repro_sm5.hlsl` and
`workaround.hlsl`. Trim anything you did not verify yourself.

### 6. Expect the scope question

They will reasonably ask how much this matters. The strongest framing is not
"ReShade needs it" but:

> Wine's `d3dcompiler_47` is built on vkd3d-shader, so this affects any
> application that compiles HLSL at runtime and uses this pattern.

ReShade is then the concrete, widely-deployed example rather than the whole
argument. The 36/77 vs 77/77 corpus numbers in `ISSUE.md` are there to make that
measurable.

### 7. If you would rather send a patch

The workaround shows the fix is likely "split a static with both kinds of
components into its resource and numeric halves" rather than anything deep. The
check is one `if` in `libs/vkd3d-shader/hlsl.y` (around line 2707). Patches go
through GitLab merge requests per the README.

## Files in this archive

| file | what it is |
|---|---|
| `ISSUE.md` | the report, ready to paste |
| `SUBMITTING.md` | this file |
| `repro_sm3.hlsl` | minimal repro, shader model 3 |
| `repro_sm5.hlsl` | same restriction at shader model 5 |
| `workaround.hlsl` | the split form, which compiles |
| `repro_harness_sm3.c` | C harness calling `vkd3d_shader_compile` directly |
| `repro_harness_sm5.c` | same for the SM5 case |

## What I could not verify from my sandbox

Stated plainly so you do not repeat it as fact:

- **That FXC accepts the construct.** Inferred from ReShade's DX9 path working in
  production for years, not tested. Step 3 covers this.
- **Whether a `vkd3d` product exists in WineHQ Bugzilla.** Check when you get there.
- **The exact `vkd3d-compiler` CLI flags.** I tested the library API only.
