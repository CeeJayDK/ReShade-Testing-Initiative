#!/usr/bin/env bash
# Checks imgcompare.py's SSIMULACRA 2 against libjxl's own ssimulacra2 tool on a set of
# distorted versions of test_pattern.png (JPEG, blur, noise, gamma, odd sizes, alpha) and
# on LumaSharpen outputs that differ by a setting (the "is my rewrite close enough" case).
# usage: SSIMULACRA2=/path/to/ssimulacra2 tests/ssimulacra2_check.sh
#
# Building the reference tool (libjxl v0.11.1, lcms instead of skcms because skcms's
# googlesource host is often unreachable; for sRGB input libjxl uses neither):
#   git clone --depth 1 --branch v0.11.1 https://github.com/libjxl/libjxl && cd libjxl
#   git submodule update --init --depth 1 third_party/highway third_party/lcms
#   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
#     -DJPEGXL_ENABLE_SKCMS=OFF -DJPEGXL_ENABLE_DEVTOOLS=ON -DJPEGXL_ENABLE_JPEGLI=OFF \
#     -DJPEGXL_ENABLE_SJPEG=OFF -DJPEGXL_ENABLE_OPENEXR=OFF -DJPEGXL_FORCE_SYSTEM_BROTLI=ON
#   ninja -C build ssimulacra2          # -> build/tools/ssimulacra2
set -uo pipefail
T="$(cd "$(dirname "$0")" && pwd)"
REF="${SSIMULACRA2:?set SSIMULACRA2 to libjxl\'s ssimulacra2 binary}"
TOL=0.001  # score points; imgcompare repeats libjxl's float32 arithmetic, so this is tight
OUT="$(mktemp -d)"; fails=0

PYTHONPATH="$T" python3 - "$T/test_pattern.png" "$OUT" <<'EOF'
import io, sys, numpy as np
from lumasharpen_reference import lumasharpen
from PIL import Image, ImageFilter
src, out = Image.open(sys.argv[1]).convert("RGB"), sys.argv[2]
rng = np.random.default_rng(1)
def pair(name, a, b): a.save(f"{out}/{name}_a.png"); b.save(f"{out}/{name}_b.png")
def jpeg(im, q):
    buf = io.BytesIO(); im.save(buf, "JPEG", quality=q); return Image.open(buf).convert("RGB")
def arr(a): return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))
px = np.asarray(src).astype(float)
pair("identical", src, src)
for q in (5, 20, 50, 80, 95): pair(f"jpeg{q}", src, jpeg(src, q))
for r in (0.5, 1, 3): pair(f"blur{r}", src, src.filter(ImageFilter.GaussianBlur(r)))
pair("sharpen", src, src.filter(ImageFilter.UnsharpMask(2, 150, 0)))
for s in (2, 10): pair(f"noise{s}", src, arr(px + rng.normal(0, s, px.shape)))
pair("gamma", src, arr((px / 255) ** (1 / 1.1) * 255 + 0.5))
pair("offby1", src, arr(px + 1))
odd = src.crop((3, 5, 336, 256)); pair("odd333x251_jpeg30", odd, jpeg(odd, 30))
sm = src.crop((600, 300, 617, 309)); pair("small17x9_blur", sm, sm.filter(ImageFilter.GaussianBlur(1)))
sm = src.crop((100, 100, 140, 140)); pair("small40_noise", sm, arr(np.asarray(sm) + rng.normal(0, 8, (40, 40, 3))))
rgb = np.asarray(src.crop((0, 0, 256, 256))); al = np.tile(np.arange(256, dtype=np.uint8), (256, 1))
pair("alpha_jpeg25", Image.fromarray(np.dstack([rgb, al])),
     Image.fromarray(np.dstack([np.asarray(jpeg(Image.fromarray(rgb), 25)), al])))
ls = lambda **kw: arr(lumasharpen(px / 255, **kw) * 255 + 0.5)
base = ls()
for name, kw in {"strength0.60": dict(strength=0.60), "strength1.0": dict(strength=1.0),
                 "pattern0": dict(pattern=0), "pattern2": dict(pattern=2)}.items():
    pair(f"lumasharpen_{name}", base, ls(**kw))
EOF

printf "%-26s %12s %12s %9s\n" pair libjxl imgcompare delta
for a in "$OUT"/*_a.png; do
	n=$(basename "$a" _a.png); b="$OUT/${n}_b.png"
	r=$("$REF" "$a" "$b")
	m=$(python3 "$T/../imgcompare.py" "$a" "$b" --json | python3 -c "import json,sys; print(json.load(sys.stdin)['ssimulacra2'])")
	d=$(python3 -c "print(f'{$m - $r:+.4f}')")
	if python3 -c "import sys; sys.exit(0 if abs($m - $r) <= $TOL else 1)"; then s=PASS; else s=FAIL; fails=$((fails+1)); fi
	printf "%-26s %12.4f %12.4f %9s  %s\n" "$n" "$r" "$m" "$d" "$s"
done

rm -rf "$OUT"
echo "$fails failure(s) (tolerance $TOL)"
exit $((fails > 0))
