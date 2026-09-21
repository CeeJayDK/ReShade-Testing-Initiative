# Building and running on Windows

## What is verified, and what is not

Read this before trusting a Windows number.

| | status |
|---|---|
| Cross-compiles for Windows (MinGW-w64) | **verified** |
| Links to a single standalone `fxstat.exe`, no runtime DLLs needed | **not until now** |
| Runs, loads `d3dcompiler_47.dll`, reports it as the DXBC compiler | **verified** |
| Dispatches to the D3DCompiler path with the right flags | **verified** |
| D3DCompiler actually compiling ReShade effects | **NOT verified** |

The standalone row was wrong until CI caught it. `find_library` on MinGW
searches `.dll.a` before `.a`, so MSYS2's SPIRV-Tools import libraries were
picked up and named on the link line, which `-static` does not override. The
exe built, then failed to launch with `0xC0000135` (STATUS_DLL_NOT_FOUND) and
no output unless `msys64\mingw64\bin` was on `PATH`. `CMakeLists.txt` now
restricts the library search to `.a` on MinGW.

The last row could not be tested here. The only Windows available in this
environment was Wine, and Wine's `d3dcompiler_47.dll` is not Microsoft's — it is
a reimplementation on top of an older vkd3d-shader. It logs
`D3DCompile2 Ignoring flags 0x8800` and then segfaults on ReShade's generated
HLSL. That says nothing about Microsoft's compiler, which is the one that will
actually run on Windows, but it does mean the path is untested end to end.

**So: the first thing to do on a real Windows machine is run the test suite.** If
D3DCompiler misbehaves, `--dxbc-compiler vkd3d` is the fallback, and that path is
fully tested.

## Which DXBC compiler to use

Windows can use both. `--dxbc-compiler` chooses; `auto` prefers D3DCompiler.

| | D3DCompiler | vkd3d-shader |
|---|---|---|
| platform | Windows only | anywhere |
| is what ReShade uses | **yes** | no |
| shader model 3 pixel shaders | yes | no (see the vkd3d issue) |
| optimisation levels (`-O0`..`-O3`, `-Od`) | yes | ignored |
| needs building | no, ships with Windows | yes |

Numbers from the two are **not comparable**. vkd3d has no equivalent of
`D3DCOMPILE_OPTIMIZATION_LEVEL*`, so it produces different instruction counts for
the same shader. Every report records which compiler produced it — in the header
line and in the `dxbc_compiler` JSON field. Check that before comparing anything.

The practical rule: use D3DCompiler on Windows for real numbers, and vkd3d when
you need Windows and Linux results to line up.

## Build: the easy way

From the repository root, with MSYS2's MinGW-w64 toolchain on `PATH`:

```
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-cmake mingw-w64-x86_64-spirv-tools
set SPIRV_TOOLS_DIR=C:\msys64\mingw64
build_reshade_testing_initiative.bat --fxstat
```

This is what CI does. The sections below are for building by hand.

## Build: MSVC

`RESHADE_DIR` and `SPIRV_HEADERS_DIR` below can point at the checkout the build
script caches, `.deps\reshade-<version>` and `.deps\reshade-<version>\deps\spirv`.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ^
    -DRESHADE_DIR=..\reshade -DSPIRV_HEADERS_DIR=..\SPIRV-Headers ^
    -DSPIRV_TOOLS_INCLUDE_DIR=<spirv-tools>\include ^
    -DSPIRV_TOOLS_OPT_LIB=<spirv-tools>\build\source\opt\SPIRV-Tools-opt.lib ^
    -DSPIRV_TOOLS_LIB=<spirv-tools>\build\source\SPIRV-Tools.lib
cmake --build build --config Release
```

SPIRV-Tools has no Windows binary release worth relying on, so build it:

```
git clone --depth 1 https://github.com/KhronosGroup/SPIRV-Tools.git
git clone --depth 1 https://github.com/KhronosGroup/SPIRV-Headers.git SPIRV-Tools\external\spirv-headers
cmake -S SPIRV-Tools -B SPIRV-Tools\build -DCMAKE_BUILD_TYPE=Release ^
      -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON
cmake --build SPIRV-Tools\build --config Release --target SPIRV-Tools-opt SPIRV-Tools-static
```

D3DCompiler needs nothing: `d3dcompiler_47.dll` ships with Windows and is loaded
at run time, not linked.

## Build: MinGW-w64

Same CMake invocation with the MinGW generator. Two things are handled for you:

- **`-static-libgcc -static-libstdc++ -static`** are added automatically, so the
  result is one file rather than an exe plus `libgcc_s_seh-1.dll` and
  `libstdc++-6.dll`.
- **`-include share.h`** is forced into the reshadefx target.
  `source/effect_preprocessor.cpp` calls `_wfsopen()` with `SH_DENYWR` but never
  includes `<share.h>`; MSVC provides it transitively and MinGW does not. This is
  a small portability bug in ReShade itself, worth reporting alongside the
  `tools/fxc.cpp` ones.

## Adding vkd3d on Windows

Optional — only needed if you want cross-platform-comparable numbers or a second
opinion. `tools/build-vkd3d-shader.sh` is a bash script and needs `bison`,
`flex`, `python3` and `git`, so run it under MSYS2 or WSL, then point CMake at
its output with `-DVKD3D_BUILD=<dir>`.

Without it, `--dxbc-compiler vkd3d` reports that the back end is not in the build
rather than failing obscurely.

## Checking what a build actually has

```
> fxstat.exe --version
fxstat
SPIRV-Tools: v2026.4
DXBC compilers: D3DCompiler vkd3d-shader
```

The DXBC compilers line is the ground truth for what that binary can do.
