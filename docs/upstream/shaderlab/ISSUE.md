# ShaderLab: CLI issues found while rendering effects headless

Found while driving ShaderLab from scripts (ReShade Testing Initiative, `shaderlab/`).
ShaderLab commit 9c3481d (2026-09-17), run under Wine 9.0. Line numbers are from
that commit. Each item is independent.

## 1. `render` reports success when the effect failed to compile

A render counts as successful when an image file was written (`cli_render.cpp`,
`ok = captured_by_addon && exists(dest_file)`, else a back buffer dump). Nothing
checks whether the effect compiled, so a broken effect gives `[OK]`, exit code 0
and the **unprocessed** input image.

Suggested fix: read the compile result (as `test-shader` does, or from the add-on
through the control block) and fail the render if the effect did not compile.

## 2. `render --shader` changes `ReShade.ini` permanently

`update_ini()` (`cli_render.cpp` ~200-310) appends the shader's folder to
`EffectSearchPaths` and rewrites the ini; it is never restored. If a recursive
search path already covers that folder under another spelling, ReShade loads
the effect twice and **applies it twice**. The result looks plausible but is
wrong.

Suggested fix: restore the original ini after the run, or pass the paths without
writing the user's ini.

## 3. `PresetPath` is left pointing at a deleted file

The same function sets `PresetPath` to the temporary preset, which `preset_guard`
deletes at the end of `execute()`. The ini keeps pointing at the deleted file.
Fixed by restoring the ini (item 2).

## 4. A compile failure takes minutes to report

With a broken effect, `render` sits through the warm-up loop (up to 30 s,
`cli_render.cpp` ~527-555) and then the capture loop (180 s timeout, ~688)
before giving up: up to 3.5 minutes for a one-line syntax error.

Suggested fix: stop as soon as the compile result is known (item 1).

## 5. `test-shader` matches log lines loosely

`cli_test_shader.cpp` ~209:

```cpp
bool failed_logged  = log.find("Failed to compile")     != npos && log.find(filename) != npos;
bool success_logged = log.find("Successfully compiled") != npos && log.find(filename) != npos;
```

The two strings are searched in the **whole log**, not on the same line. If any
other effect on the search path fails to compile while the tested file's name
appears anywhere in the log, the tested effect is reported as failed.

Suggested fix: search for a single line containing both strings.

## 6. `test-shader` does not add the shader's folder to the search paths

`render --shader` adds the shader's folder to `EffectSearchPaths` (item 2);
`test-shader` does not. A shader outside the configured search paths is never
loaded, so it is reported as not compiled with no error. When `test-shader`
continues into a render (`-i` given), the folder is added only then.

Suggested fix: add the folder (temporarily) before the compile check too.

---

Workarounds, for reference: RTI's `fxrender.py` writes a fresh `ReShade.ini` for
every run, passes `--preset`, and watches `ReShade.log` itself, stopping at the
first compile result.
