#!/usr/bin/env bash
#
# Build libvkd3d-shader.a natively on Linux, out of Wine's vendored vkd3d copy.
#
# Why this is not just "clone vkd3d and make":
#
#  - vkd3d upstream lives on Codeberg. Wine vendors the entire library under
#    libs/vkd3d and tracks it closely, and the Wine GitHub mirror can be fetched
#    sparsely (the vkd3d subtree is a few MB of a very large repo).
#  - Wine's copy is edited for building inside the Wine tree: it includes
#    "windows.h", <wine/list.h>, "d3dcommon.h" and "d3dx9shader.h" and expects
#    Wine's own headers to satisfy them. Upstream instead ships vkd3d_windows.h
#    and IDL files. This script puts equivalents in those slots so the same
#    sources compile against glibc with plain gcc.
#  - Only vkd3d-common and vkd3d-shader are built. libs/vkd3d and
#    libs/vkd3d-utils are the D3D12 runtime and are not needed to compile
#    shaders.
#
# usage: build-vkd3d-shader.sh [out dir]
#
# Set WINE_SRC to reuse an existing Wine checkout instead of fetching one.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "${1:-$PWD/vkd3d-build}"
OUT="$(cd "${1:-$PWD/vkd3d-build}" && pwd)"
WINE_SRC="${WINE_SRC:-$OUT/wine-src}"
VKD3D_WINDOWS_H_SRC="${VKD3D_WINDOWS_H_SRC:-}"

for t in git bison flex python3 gcc ar; do
	command -v "$t" >/dev/null || { echo "missing required tool: $t" >&2; exit 1; }
done

# ---------------------------------------------------------------------------
# 1. sources
# ---------------------------------------------------------------------------

if [ ! -f "$WINE_SRC/libs/vkd3d/libs/vkd3d-shader/hlsl.y" ]; then
	echo "Fetching Wine's vkd3d subtree ..."
	rm -rf "$WINE_SRC"
	git clone --depth 1 --filter=blob:none --sparse \
		https://github.com/wine-mirror/wine.git "$WINE_SRC"
	git -C "$WINE_SRC" sparse-checkout set libs/vkd3d include
fi

V="$WINE_SRC/libs/vkd3d"
[ -f "$V/libs/vkd3d-shader/hlsl.y" ] || { echo "no vkd3d tree under $V" >&2; exit 1; }

# upstream's vkd3d_windows.h is the one piece Wine drops entirely. Any upstream
# checkout has it and it changes very rarely.
if [ -z "$VKD3D_WINDOWS_H_SRC" ]; then
	if [ ! -f "$OUT/vkd3d-upstream/include/vkd3d_windows.h" ]; then
		echo "Fetching upstream vkd3d for vkd3d_windows.h ..."
		git clone --quiet --depth 1 https://github.com/Gcenx/vkd3d.git "$OUT/vkd3d-upstream"
	fi
	VKD3D_WINDOWS_H_SRC="$OUT/vkd3d-upstream/include/vkd3d_windows.h"
fi

# ---------------------------------------------------------------------------
# 2. compatibility shim
# ---------------------------------------------------------------------------

echo "Building compatibility shim ..."
SHIM="$OUT/shim"
rm -rf "$SHIM"
mkdir -p "$SHIM/wine"

cp "$VKD3D_WINDOWS_H_SRC" "$SHIM/vkd3d_windows.h"
# Wine's own intrusive list/rbtree -- upstream's older copies lack functions
# (list_move_before) that current vkd3d-shader uses.
cp "$WINE_SRC/include/wine/list.h" "$WINE_SRC/include/wine/rbtree.h" "$SHIM/wine/"

cat > "$SHIM/windows.h" <<'EOF'
/*
 * Upstream vkd3d includes "vkd3d_windows.h" when not building for Windows.
 * Wine's vendored copy includes "windows.h" unconditionally, because inside the
 * Wine tree that resolves to Wine's headers. This puts upstream's back in place.
 */
#ifndef __VKD3D_WINDOWS_SHIM_H
#define __VKD3D_WINDOWS_SHIM_H

#include "vkd3d_windows.h"

/* DXGI result codes used by vkd3d-shader that vkd3d_windows.h does not define. */
#ifndef DXGI_ERROR_NOT_FOUND
#define DXGI_ERROR_NOT_FOUND      _HRESULT_TYPEDEF_(0x887a0002)
#endif
#ifndef DXGI_ERROR_MORE_DATA
#define DXGI_ERROR_MORE_DATA      _HRESULT_TYPEDEF_(0x887a0003)
#endif
#ifndef DXGI_ERROR_UNSUPPORTED
#define DXGI_ERROR_UNSUPPORTED    _HRESULT_TYPEDEF_(0x887a0004)
#endif
#ifndef DXGI_ERROR_ALREADY_EXISTS
#define DXGI_ERROR_ALREADY_EXISTS _HRESULT_TYPEDEF_(0x887a0036)
#endif

#endif
EOF

# d3dcommon.h: Wine ships it as IDL, and widl is not in the Wine runtime
# package. Only the enums, structs and #defines are needed, and those are
# already valid C.
python3 "$SCRIPT_DIR/idl2h.py" "$WINE_SRC/include/d3dcommon.idl" \
	"$SHIM/d3dcommon.h" __VKD3D_D3DCOMMON_SHIM_H

# The D3D shader version macros. Upstream keeps these in vkd3d_d3d9types.h;
# inside Wine they arrive through its header chain. hlsl_codegen.c includes
# d3dcommon.h, so append them there rather than inventing another header.
cat >> "$SHIM/d3dcommon.h" <<'EOF'

/* From d3d9types.h -- used by hlsl_codegen.c to stamp the SM1 version token. */
#ifndef D3DPS_VERSION
#define D3DPS_VERSION(major, minor) (0xFFFF0000 | ((major) << 8) | (minor))
#endif
#ifndef D3DVS_VERSION
#define D3DVS_VERSION(major, minor) (0xFFFE0000 | ((major) << 8) | (minor))
#endif
EOF

# d3dx9shader.h: hlsl.h needs only the SM1 constant-table reflection enums.
{
	echo '/* Minimal d3dx9shader.h: only the enums vkd3d-shader/hlsl.h needs.'
	echo ' * Taken verbatim from Wine include/d3dx9shader.h to match the D3DX ABI. */'
	echo '#ifndef __VKD3D_D3DX9SHADER_SHIM_H'
	echo '#define __VKD3D_D3DX9SHADER_SHIM_H'
	echo
	sed -n '/^typedef enum _D3DXREGISTER_SET/,/D3DXREGISTER_SET;/p'        "$WINE_SRC/include/d3dx9shader.h"
	echo
	sed -n '/^typedef enum D3DXPARAMETER_CLASS/,/LPD3DXPARAMETER_CLASS;/p' "$WINE_SRC/include/d3dx9shader.h"
	echo
	sed -n '/^typedef enum D3DXPARAMETER_TYPE/,/LPD3DXPARAMETER_TYPE;/p'   "$WINE_SRC/include/d3dx9shader.h"
	echo
	echo '#endif'
} > "$SHIM/d3dx9shader.h"

grep -q D3DXRS_SAMPLER   "$SHIM/d3dx9shader.h" || { echo "shim: D3DXREGISTER_SET missing"   >&2; exit 1; }
grep -q D3DXPC_SCALAR    "$SHIM/d3dx9shader.h" || { echo "shim: D3DXPARAMETER_CLASS missing" >&2; exit 1; }
grep -q D3DXPT_SAMPLER   "$SHIM/d3dx9shader.h" || { echo "shim: D3DXPARAMETER_TYPE missing"  >&2; exit 1; }
grep -q D3D_SVC_SCALAR   "$SHIM/d3dcommon.h"   || { echo "shim: d3dcommon enums missing"     >&2; exit 1; }

# ---------------------------------------------------------------------------
# 3. generated lexers and parsers
# ---------------------------------------------------------------------------

echo "Generating lexers and parsers ..."
mkdir -p "$OUT/gen"
cd "$OUT"
bison -d -o gen/hlsl.tab.c    "$V/libs/vkd3d-shader/hlsl.y"
bison -d -o gen/preproc.tab.c "$V/libs/vkd3d-shader/preproc.y"
flex  -o gen/hlsl.yy.c        "$V/libs/vkd3d-shader/hlsl.l"
flex  -o gen/preproc.yy.c     "$V/libs/vkd3d-shader/preproc.l"

# ---------------------------------------------------------------------------
# 4. compile
# ---------------------------------------------------------------------------

CFLAGS=(
  -O2 -fPIC -w -std=gnu17
  # An implicitly declared function is assumed to return int. strtof_l() is a
  # GNU extension, so without _GNU_SOURCE it gets declared implicitly and every
  # float literal in every shader silently parses as garbage -- with -w, in
  # total silence. Never let that class of error be a warning here.
  -Werror=implicit-function-declaration
  -D_GNU_SOURCE
  -DLIBVKD3D_SOURCE -DLIBVKD3D_SHADER_SOURCE -DHAVE_CONFIG_H
  -DHAVE_SYNC_ADD_AND_FETCH=1 -DHAVE_SYNC_BOOL_COMPARE_AND_SWAP=1
  -DHAVE_STRTOF_L=1            # glibc spells it strtof_l, not _strtof_l
  -I "$SHIM"                   # must precede the Wine tree
  -I "$V"                      # config.h
  -I "$V/include"
  -I "$V/include/private"
  -I "$V/libs/vkd3d-shader"
  -I "$OUT/gen"
)

# blob.c is omitted: it implements ID3DBlob, pulls in d3d12.h, and is only used
# by vkd3d-utils.
SRCS=(
  "$V/libs/vkd3d-common/debug.c"
  "$V/libs/vkd3d-common/error.c"
  "$V/libs/vkd3d-common/memory.c"
  "$V/libs/vkd3d-common/utf8.c"
  "$V/libs/vkd3d-shader/checksum.c"
  "$V/libs/vkd3d-shader/d3d_asm.c"
  "$V/libs/vkd3d-shader/d3dbc.c"
  "$V/libs/vkd3d-shader/dxbc.c"
  "$V/libs/vkd3d-shader/dxil.c"
  "$V/libs/vkd3d-shader/fx.c"
  "$V/libs/vkd3d-shader/glsl.c"
  "$V/libs/vkd3d-shader/hlsl.c"
  "$V/libs/vkd3d-shader/hlsl_codegen.c"
  "$V/libs/vkd3d-shader/hlsl_constant_ops.c"
  "$V/libs/vkd3d-shader/ir.c"
  "$V/libs/vkd3d-shader/msl.c"
  "$V/libs/vkd3d-shader/spirv.c"
  "$V/libs/vkd3d-shader/tpf.c"
  "$V/libs/vkd3d-shader/vkd3d_shader_main.c"
  gen/hlsl.tab.c
  gen/hlsl.yy.c
  gen/preproc.tab.c
  gen/preproc.yy.c
)

mkdir -p obj
echo "Compiling ${#SRCS[@]} files ..."
pids=()
for s in "${SRCS[@]}"; do
  gcc "${CFLAGS[@]}" -c "$s" -o "obj/$(basename "${s%.c}").o" &
  pids+=($!)
done
fail=0
for p in "${pids[@]}"; do wait "$p" || fail=1; done
[ "$fail" -eq 0 ] || { echo "compilation failed" >&2; exit 1; }

rm -f libvkd3d-shader.a
ar rcs libvkd3d-shader.a obj/*.o

echo
echo "built: $OUT/libvkd3d-shader.a"
echo "  vkd3d from: $(git -C "$WINE_SRC" log -1 --format=%ci) (wine $(git -C "$WINE_SRC" rev-parse --short HEAD))"
echo "  headers:    -I $V/include -I $SHIM"
echo "  link:       $OUT/libvkd3d-shader.a -lm -lpthread"
