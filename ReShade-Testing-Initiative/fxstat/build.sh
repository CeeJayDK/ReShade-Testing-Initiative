#!/usr/bin/env bash
# Fetch dependencies, generate the opcode table and build libfxstat_core + fxstat.
#
# Optional: set VKD3D_BUILD to the output of tools/build-vkd3d-shader.sh to get
# the DXBC back end (--dxbc). Without it everything still builds, minus that.
set -euo pipefail
cd "$(dirname "$0")"

RESHADE_DIR="${RESHADE_DIR:-$PWD/../reshade}"
SPIRV_HEADERS_DIR="${SPIRV_HEADERS_DIR:-$PWD/../SPIRV-Headers}"
VKD3D_BUILD="${VKD3D_BUILD:-}"

for t in cmake g++ python3 git; do
	command -v "$t" >/dev/null || { echo "missing required tool: $t" >&2; exit 1; }
done

# SPIRV-Tools is linked, not shelled out to, so the headers are required.
if [ ! -f /usr/include/spirv-tools/optimizer.hpp ] && [ -z "${SPIRV_TOOLS_INCLUDE_DIR:-}" ]; then
	echo "SPIRV-Tools headers not found." >&2
	echo "  Debian/Ubuntu: apt-get install spirv-tools" >&2
	echo "  or set SPIRV_TOOLS_INCLUDE_DIR / SPIRV_TOOLS_OPT_LIB / SPIRV_TOOLS_LIB" >&2
	exit 1
fi

[ -d "$RESHADE_DIR" ]       || git clone --depth 1 https://github.com/crosire/reshade.git "$RESHADE_DIR"
[ -d "$SPIRV_HEADERS_DIR" ] || git clone --depth 1 https://github.com/KhronosGroup/SPIRV-Headers.git "$SPIRV_HEADERS_DIR"

python3 tools/gen_opcode_table.py \
    "$SPIRV_HEADERS_DIR/include/spirv/unified1/spirv.core.grammar.json" \
    src/core/spirv_opcodes.gen.cpp

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DRESHADE_DIR="$RESHADE_DIR" -DSPIRV_HEADERS_DIR="$SPIRV_HEADERS_DIR" \
      ${VKD3D_BUILD:+-DVKD3D_BUILD="$VKD3D_BUILD"}
cmake --build build --parallel

echo
echo "built: $PWD/build/fxstat            (command line)"
echo "       $PWD/build/libfxstat_core.a  (library -- see include/fxstat/fxstat.hpp)"
