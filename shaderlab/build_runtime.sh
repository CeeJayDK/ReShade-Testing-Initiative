#!/usr/bin/env bash
#
# Build a headless ShaderLab + ReShade runtime that runs under Wine on Linux,
# so fxrender.py can render ReshadeFX effects onto images.
#
# Everything is built from source, because the prebuilt ShaderLab/ReShade
# downloads are Windows-only installers and signed builds refuse unsigned add-ons:
#
#   ReShade 6.8.0      clang + mingw-w64, with patches/reshade-mingw.patch
#                      (full add-on support, "UNOFFICIAL" build)
#   ShaderLab          clang + mingw-w64, with patches/shaderlab-mingw.patch
#   d3dcompiler_47.dll vkd3d-shader from Wine master (much newer than the
#                      one in distro Wine packages, which crashes on ReShade's
#                      own internal shaders)
#   DXVK 2.3.1         D3D11 -> Vulkan (llvmpipe/lavapipe on a GPU-less box);
#                      Wine's own wined3d ignores sRGB render targets
#                      (SRGBWriteEnable), which makes CAS, SMAA etc. come out too dark
#
# Output: $OUT/app (ShaderLab.exe, dxgi.dll = ReShade, add-on, d3dcompiler)
#         $OUT/prefix (Wine prefix with DXVK installed)
#
# usage: build_runtime.sh [out dir]      (default: ./runtime next to this script)
#
# Needs (Ubuntu 24.04 package names): wine64 mingw-w64 clang lld llvm cmake ninja-build
#   meson glslang-tools bison flex git python3 python3-pil python3-numpy xvfb
#   mesa-vulkan-drivers libvulkan1 libvulkan-dev
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mkdir -p "${1:-$HERE/runtime}" && cd "${1:-$HERE/runtime}" && pwd)"
SRC="$OUT/src"
JOBS="${JOBS:-$(nproc)}"

RESHADE_TAG="${RESHADE_TAG:-v6.8.0}"
SHADERLAB_REF="${SHADERLAB_REF:-9c3481d505c59998feed02b0a60f0fccea94d080}"
WINE_REF="${WINE_REF:-7b3fff76fa5178f6ce0141b2c776afa2a822f101}"
DXVK_REF="${DXVK_REF:-v2.3.1}"   # newer DXVK needs Vulkan extensions Wine 9.0 does not expose

MINGW_GCC=/usr/lib/gcc/x86_64-w64-mingw32/$(ls /usr/lib/gcc/x86_64-w64-mingw32 | grep posix | head -1)
[ -d "$MINGW_GCC" ] || { echo "mingw-w64 posix-thread gcc not found"; exit 1; }

export WINEPREFIX="$OUT/prefix" WINEDEBUG=-all
mkdir -p "$SRC"

fetch() { # url dir ref
	if [ ! -d "$2/.git" ]; then git clone -q "$1" "$2"; fi
	git -C "$2" fetch -q --depth 1 origin "$3" 2>/dev/null || git -C "$2" fetch -q origin
	git -C "$2" checkout -q -f FETCH_HEAD 2>/dev/null || git -C "$2" checkout -q -f "$3"
}

# ---------------------------------------------------------------------------
echo "== sources"
fetch https://github.com/crosire/reshade.git "$SRC/reshade" "refs/tags/$RESHADE_TAG"
git -C "$SRC/reshade" submodule update -q --init --depth 1
fetch https://github.com/NotRayST/ShaderLab.git "$SRC/ShaderLab" "$SHADERLAB_REF"
if [ ! -d "$SRC/dxvk/.git" ]; then
	git clone -q --depth 1 -b "$DXVK_REF" https://github.com/doitsujin/dxvk.git "$SRC/dxvk"
	# libdisplay-info lives on gitlab.freedesktop.org in the v2.3.1 .gitmodules; use DXVK's GitHub mirror
	git -C "$SRC/dxvk" config submodule.subprojects/libdisplay-info.url https://github.com/doitsujin/libdisplay-info.git
	git -C "$SRC/dxvk" submodule update -q --init
	git -C "$SRC/dxvk" apply "$HERE/patches/dxvk-headless.patch"
fi
if [ ! -d "$SRC/wine/.git" ]; then
	git clone -q --depth 1 --filter=blob:none --sparse https://github.com/wine-mirror/wine.git "$SRC/wine"
	git -C "$SRC/wine" sparse-checkout set libs/vkd3d include/wine
fi
git -C "$SRC/wine" fetch -q --depth 1 origin "$WINE_REF" && git -C "$SRC/wine" checkout -q FETCH_HEAD

# ---------------------------------------------------------------------------
echo "== toolchain (clang targeting mingw-w64)"
SHIM="$OUT/shim"
mkdir -p "$SHIM"
cp "$HERE"/mingw/shim/*.h "$SHIM/"
# ReShade includes Windows SDK headers with SDK capitalisation; mingw-w64's are lowercase
for h in DbgHelp OAIdl OCIdl Psapi Shellapi ShlObj Unknwn WinInet Windows Winsock2 Xinput; do
	ln -sf "/usr/x86_64-w64-mingw32/include/$(echo $h | tr A-Z a-z).h" "$SHIM/$h.h"
done
TC="$OUT/clang-mingw.cmake"
cat > "$TC" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-w64-mingw32)
set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-mingw32)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_C_FLAGS_INIT "-fms-extensions -Wno-everything --gcc-install-dir=$MINGW_GCC -isystem $SHIM")
set(CMAKE_CXX_FLAGS_INIT "-fms-extensions -Wno-everything -nostdinc++ -isystem $MINGW_GCC/include/c++ -isystem $MINGW_GCC/include/c++/x86_64-w64-mingw32 -isystem $MINGW_GCC/include/c++/backward -isystem $SHIM -include $SHIM/rs_prelude.h")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -static -pthread -static-libgcc -static-libstdc++ -L$MINGW_GCC")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -static -pthread -static-libgcc -static-libstdc++ -L$MINGW_GCC")
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
STDLIBS="-luuid -lole32 -lshell32 -lkernel32 -luser32 -lgdi32 -ladvapi32 -lcomdlg32 -lstdc++ -lpthread"

# ---------------------------------------------------------------------------
echo "== d3dcompiler_47.dll from vkd3d-shader"
V="$SRC/wine/libs/vkd3d"
D="$OUT/build/d3dcompiler"
mkdir -p "$D/obj" "$D/inc/wine"
cp "$SRC/wine/include/wine/list.h" "$SRC/wine/include/wine/rbtree.h" "$D/inc/wine/"
cp -r /usr/include/vulkan /usr/include/vk_video "$D/inc/"
echo '#include <vulkan/vulkan.h>' > "$D/inc/wine/vulkan.h"
grep -v HAVE__STRTOF_L "$V/config.h" > "$D/inc/config.h"
cp "$HERE/d3dcompiler/rti_guids.h" "$D/inc/"
for y in hlsl preproc; do
	bison -d -o "$D/obj/$y.tab.c" "$V/libs/vkd3d-shader/$y.y"
	flex -o "$D/obj/$y.yy.c" "$V/libs/vkd3d-shader/$y.l"
done
CC=x86_64-w64-mingw32-gcc-posix
CF="-O2 -w -DLIBVKD3D_SOURCE -DLIBVKD3D_SHADER_SOURCE -DLIBVKD3D_UTILS_SOURCE -I$D/inc -I$V -I$V/include -I$V/include/private -I$V/libs/vkd3d -I$V/libs/vkd3d-shader -I$D/obj"
for f in "$V"/libs/vkd3d-common/{blob,debug,error,memory,utf8}.c \
         "$V"/libs/vkd3d-shader/{checksum,d3d_asm,d3dbc,dxbc,dxil,fx,glsl,hlsl,hlsl_codegen,hlsl_constant_ops,ir,msl,spirv,tpf,vkd3d_shader_main}.c \
         "$D"/obj/{hlsl.tab,preproc.tab,preproc.yy}.c "$V/libs/vkd3d-utils/vkd3d_utils_main.c"; do
	$CC $CF -c "$f" -o "$D/obj/$(basename "$f" .c).o" &
	while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do sleep 0.2; done
done
$CC $CF -include d3dx9shader.h -c "$D/obj/hlsl.yy.c" -o "$D/obj/hlsl.yy.o"
$CC $CF -DWINE_NO_NAMELESS_EXTENSION -include "$D/inc/rti_guids.h" -c "$V/libs/vkd3d-utils/reflection.c" -o "$D/obj/reflection.o"
$CC -O2 -c "$HERE/d3dcompiler/stubs.c" -o "$D/obj/stubs.o"
wait
$CC -shared -o "$D/d3dcompiler_47.dll" "$D"/obj/*.o "$HERE/d3dcompiler/d3dcompiler_47.def" -static -static-libgcc -luuid -lole32

# ---------------------------------------------------------------------------
echo "== ReShade's internal shaders (.cso) via the new d3dcompiler under Wine"
wineboot -i >/dev/null 2>&1 || true
mkdir -p "$OUT/build/fxcw"
cp "$D/d3dcompiler_47.dll" "$OUT/build/fxcw/"
x86_64-w64-mingw32-gcc -O2 "$HERE/mingw/fxcw.c" -o "$OUT/build/fxcw/fxcw.exe" -ld3dcompiler_47
R="$SRC/reshade"
( cd "$R" && git apply --check "$HERE/patches/reshade-mingw.patch" 2>/dev/null && git apply "$HERE/patches/reshade-mingw.patch" ) || echo "   (ReShade patch already applied)"
# vkd3d-shader does not implement [fastopt], which ReShade emits for [loop] loops at
# SM 4+ ("E5017: Unhandled attribute 'fastopt'"); [loop] means the same to the compiler.
( cd "$R" && git apply --check "$HERE/patches/reshade-loop-attribute.patch" 2>/dev/null && git apply "$HERE/patches/reshade-loop-attribute.patch" ) || echo "   (loop attribute patch already applied)"
sed -i 's/defined(_MSC_VER) || !defined(_WIN32)/1/' "$R"/deps/d3d12/include/directx/*.h
# vkd3d does not support the RootSignature attribute or expressions in [numthreads]; D3D11 ignores the former
python3 - "$R/res/shaders/mipmap_cs_5_0.hlsl" "$OUT/build/fxcw/mipmap_cs.hlsl" <<'EOF'
import re, sys
s = open(sys.argv[1]).read()
s = re.sub(r'\[RootSignature\((.|\n)*?\)\]\n', '', s).replace('[numthreads(16 * 16, 1, 1)]', '[numthreads(256, 1, 1)]')
open(sys.argv[2], 'w').write(s)
EOF
fxc() { WINEDLLOVERRIDES="d3dcompiler_47=n" wine "$OUT/build/fxcw/fxcw.exe" "$1" main "$2" "$3" 2>&1 | grep -v fixme; }
( cd "$R/res/shaders"
  fxc copy_ps.hlsl ps_4_0 copy_ps.cso
  fxc fullscreen_vs.hlsl vs_4_0 fullscreen_vs.cso
  fxc imgui_ps_4_0.hlsl ps_4_0 imgui_ps_4_0.cso
  fxc imgui_vs_4_0.hlsl vs_4_0 imgui_vs_4_0.cso
  fxc imgui_ps_3_0.hlsl ps_3_0 imgui_ps_3_0.cso
  fxc imgui_vs_3_0.hlsl vs_3_0 imgui_vs_3_0.cso
  fxc "Z:$(echo "$OUT/build/fxcw/mipmap_cs.hlsl" | tr / '\\')" cs_5_0 mipmap_cs_5_0.cso )

echo "== ReShade (dxgi.dll)"
cat > "$R/res/version.h" <<'EOF'
#pragma once
#define VERSION_FULL 6.8.0.1
#define VERSION_MAJOR 6
#define VERSION_MINOR 8
#define VERSION_REVISION 0
#define VERSION_BUILD 1
#define VERSION_STRING_FILE "6.8.0.1"
#define VERSION_STRING_PRODUCT "6.8.0 UNOFFICIAL"
EOF
cmake -S "$R" -B "$OUT/build/reshade" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C "$OUT/build/reshade" -j"$JOBS" ReShade

echo "== ShaderLab"
S="$SRC/ShaderLab"
( cd "$S" && git apply --check "$HERE/patches/shaderlab-mingw.patch" 2>/dev/null && git apply "$HERE/patches/shaderlab-mingw.patch" ) || echo "   (ShaderLab patch already applied)"
cmake -S "$S" -B "$OUT/build/shaderlab" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
	-DBUILD_GAME_CAPTURE=OFF -DBUILD_UNDO_REDO=OFF -DCMAKE_CXX_STANDARD_LIBRARIES="$STDLIBS" >/dev/null
ninja -C "$OUT/build/shaderlab" -j"$JOBS"

echo "== DXVK (with patches/dxvk-headless.patch: Xvfb reports 0 Hz, DXVK divided by it)"
sed -e "s/x86_64-w64-mingw32-gcc'/x86_64-w64-mingw32-gcc-posix'/; s/x86_64-w64-mingw32-g++'/x86_64-w64-mingw32-g++-posix'/" \
	"$SRC/dxvk/build-win64.txt" > "$OUT/build/dxvk-cross.txt"
[ -d "$OUT/build/dxvk" ] || meson setup "$OUT/build/dxvk" "$SRC/dxvk" --cross-file "$OUT/build/dxvk-cross.txt" \
	--buildtype release -Denable_d3d9=false >/dev/null
ninja -C "$OUT/build/dxvk" -j"$JOBS"

# ---------------------------------------------------------------------------
echo "== assemble $OUT/app"
APP="$OUT/app"
mkdir -p "$APP"
cp "$OUT/build/shaderlab/host_app/ShaderLab.exe" "$OUT/build/shaderlab/addon/ShaderLab.addon" "$APP/"
cp "$OUT/build/reshade/ReShade64.dll" "$APP/dxgi.dll"
cp "$D/d3dcompiler_47.dll" "$APP/"
# DXVK goes into the prefix's system32: ReShade (app-local dxgi.dll) loads the "system" dxgi/d3d11 from there
SYS="$WINEPREFIX/drive_c/windows/system32"
cp "$OUT/build/dxvk/src/dxgi/dxgi.dll" "$OUT/build/dxvk/src/d3d11/d3d11.dll" "$SYS/"
cp "$OUT/build/dxvk/src/d3d10/d3d10core.dll" "$SYS/" 2>/dev/null || true
echo "done. Try:"
echo "  FXRENDER_APP=$APP WINEPREFIX=$WINEPREFIX $HERE/fxrender.py <effect.fx> -i in.png -o out.png"
