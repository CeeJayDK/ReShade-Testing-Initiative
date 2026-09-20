#!/usr/bin/env bash
# Sanity tests for the ShaderLab runtime: does what comes out match what the shader says?
# usage: tests/run_tests.sh   (FXRENDER_APP / WINEPREFIX as for fxrender.py; SWEETFX and
#        RESHADE_SHADERS point at checkouts of CeeJayDK/SweetFX and crosire/reshade-shaders)
set -uo pipefail
T="$(cd "$(dirname "$0")" && pwd)"
FX="$T/../fxrender.py"
SWEETFX="${SWEETFX:?set SWEETFX to a SweetFX checkout}"
export FXRENDER_INCLUDES="${RESHADE_SHADERS:?set RESHADE_SHADERS to reshade-shaders checkout}/Shaders"
OUT="$(mktemp -d)"; IMG="$T/test_pattern.png"; fails=0
check() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails+1)); fi; }

# 1. LumaSharpen against an independent numpy implementation (tolerance: 1 level of 255)
luma() { # name, fxrender args..., -- reference args...
	local name=$1; shift; local fa=() ra=()
	while [ "$1" != "--" ]; do fa+=("$1"); shift; done; shift; ra=("$@")
	"$FX" "$SWEETFX/Shaders/SweetFX/LumaSharpen.fx" -i "$IMG" -o "$OUT/$name.png" "${fa[@]}" >/dev/null
	python3 "$T/lumasharpen_reference.py" "$IMG" "$OUT/$name.png" "${ra[@]}" > "$OUT/$name.txt"; head -1 "$OUT/$name.txt" | sed "s/^/      /"
	check "LumaSharpen $name matches reference" "python3 -c \"import re,sys; m=float(re.search(r'max ([0-9.]+)', open('$OUT/$name.txt').read()).group(1)); sys.exit(0 if m <= 1.0 else 1)\""
}
luma defaults --
luma strength3_pattern2 --set sharp_strength=3.0 --set pattern=2 -- sharp_strength=3.0 pattern=2
luma perfmode --perf-mode --set pattern=0 -- pattern=0

# 2. sRGB read + write round trip must be the identity
"$FX" "$T/SRGBTest.fx" -t SRGBRoundTrip -i "$IMG" -o "$OUT/srgb.png" >/dev/null
check "sRGB read+write round trip is lossless" "python3 -c \"
from PIL import Image; import numpy as np, sys
a=np.asarray(Image.open('$IMG').convert('RGB')).astype(int); b=np.asarray(Image.open('$OUT/srgb.png').convert('RGB')).astype(int)
sys.exit(0 if np.abs(a-b).max()<=1 else 1)\""

# 3. uniform packing (float/int/bool/float2 defaults reach the shader)
"$FX" "$T/UniformTest.fx" -i "$IMG" -o "$OUT/uni.png" >/dev/null
check "uniform defaults reach the shader" "python3 -c \"
from PIL import Image; import numpy as np, sys
b=np.asarray(Image.open('$OUT/uni.png').convert('RGB')).astype(float)
got=[b[5,i*10+5,0]/255 for i in range(7)]; exp=[0.1,0.2,0.3,0.4,0.5,0.6,0.7]
sys.exit(0 if max(abs(g-e) for g,e in zip(got,exp))<0.005 else 1)\""

# 4. a syntax error must be reported with its line number, and must not produce an image
"$FX" "$T/Broken.fx" -i "$IMG" -o "$OUT/broken.png" --json > "$OUT/broken.json"; rc=$?
check "broken shader fails with a line number" "[ $rc -eq 1 ] && [ ! -f $OUT/broken.png ] && grep -q '\"line\": 5' $OUT/broken.json"

rm -rf "$OUT"
echo "$fails failure(s)"
exit $((fails > 0))
