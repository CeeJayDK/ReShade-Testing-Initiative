#!/usr/bin/env python3
"""
imgcompare - how close is one image to another?

  imgcompare.py reference.png candidate.png
  imgcompare.py reference.png candidate.png --diff diff.png --json

Reports the absolute difference (max, mean, % of pixels changed), PSNR and
SSIMULACRA 2. SSIMULACRA 2 is a perceptual score: 100 = identical, 90 = can't
be told apart at 1:1, 70 = high quality, 50 = medium, 30 = low. Use it to judge
whether an approximation is close enough, not just whether it is exact.

The SSIMULACRA 2 code is a numpy port of libjxl's tools/ssimulacra2.cc (v0.11.1)
and is checked against that tool by tests/ssimulacra2_check.sh.

Exit code: 0 = compared, 2 = usage problem (missing file, size mismatch).
"""
import argparse, json, sys
from pathlib import Path

import numpy as np
from PIL import Image


def load(path):
    """-> (HxWx3 float32 sRGB in 0..1, HxW float32 alpha or None)"""
    im = Image.open(path)
    has_alpha = im.mode in ("RGBA", "LA", "PA") or (im.mode == "P" and "transparency" in im.info)
    a = np.asarray(im.convert("RGBA" if has_alpha else "RGB")).astype(np.float32) * np.float32(1 / 255)
    return (a[..., :3], a[..., 3]) if has_alpha else (a, None)


# --- absolute difference -----------------------------------------------------

def diff_stats(a, b):
    """a, b: HxWx3 float 0..1. Differences are in 8-bit levels."""
    d = np.abs(np.round(a * 255) - np.round(b * 255))
    mse = float((d ** 2).mean())
    return {"size": [a.shape[1], a.shape[0]],
            "mean_abs_diff": round(float(d.mean()), 4),
            "max_abs_diff": int(d.max()),
            "changed_pixels_pct": round(float((d.max(2) > 0).mean() * 100), 3),
            "psnr_db": round(10 * np.log10(255 ** 2 / mse), 3) if mse else None}


def write_diff(a, b, dst, gain=8):
    """Amplified signed difference b - a; 128 grey = unchanged."""
    d = np.clip(128 + (np.round(b * 255) - np.round(a * 255)) * gain, 0, 255).astype(np.uint8)
    Image.fromarray(d).save(dst)


# --- SSIMULACRA 2 --------------------------------------------------------------

# The functions below follow libjxl's float32 arithmetic step by step, not just its maths:
# near a score of 100 the result depends on rounding (see the blur below), and matching
# the reference tool matters more than being "more exact" than it.

def _fma(a, b, c):
    """a * b + c with one rounding: the product of two floats is exact in float64."""
    return (np.float64(a) * b + c).astype(np.float32)


_SRGB_P = np.array([2.200248328e-04, 1.043637593e-02, 1.624820318e-01, 7.961564959e-01, 8.210152774e-01], np.float32)
_SRGB_Q = np.array([2.631846970e-01, 1.076976492e+00, 4.987528350e-01, -5.512498495e-02, 6.521209011e-03], np.float32)


def _srgb_to_linear(v):
    """libjxl's TF_SRGB::DisplayFromEncoded: a rational polynomial, not pow()."""
    yp, yq = np.full_like(v, _SRGB_P[4]), np.full_like(v, _SRGB_Q[4])
    for i in (3, 2, 1, 0):
        yp, yq = _fma(yp, v, _SRGB_P[i]), _fma(yq, v, _SRGB_Q[i])
    return np.where(v > np.float32(0.04045), yp / yq, v * np.float32(1 / 12.92))


_F = np.float32
_OPSIN = np.array([[_F(0.30), _F(1) - _F(0.078) - _F(0.30), _F(0.078)],
                   [_F(0.23), _F(1) - _F(0.078) - _F(0.23), _F(0.078)],
                   [_F(0.24342268924547819), _F(0.20476744424496821),
                    _F(1) - _F(0.24342268924547819) - _F(0.20476744424496821)]], np.float32)
_OPSIN_BIAS = _F(0.0037930732552754493)


def _cbrt_add(x, add):
    """libjxl's CubeRootAndAdd: cbrt(x) + add, via an exponent trick + Newton-Raphson."""
    xa_3 = x * _F(1 / 3)
    m = x.view(np.int32)
    r = np.where(m == 0, 0, 0x54800000 - (m >> 23) * 0x002AAAAA).astype(np.int32).view(np.float32)
    for _ in range(3):  # r -> x^(-1/3)
        r2 = r * r
        r = (np.float64(r * _F(4 / 3)) - np.float64(xa_3) * (r2 * r2)).astype(np.float32)
    r2 = r * r
    r = _fma(_F(1 / 3), (np.float64(r) - np.float64(x) * (r2 * r2)).astype(np.float32), r)
    return _fma(r * r, x, add)  # r = x^(-1/3), so x^(1/3) = r^2 * x


def _xyb(lin):
    """Linear sRGB HxWx3 -> SSIMULACRA 2's 'positive' XYB, as 3 planes."""
    r, g, b = lin[..., 0], lin[..., 1], lin[..., 2]
    add = -_F(np.cbrt(np.float64(_OPSIN_BIAS)))
    m = []
    for row in _OPSIN:
        mixed = np.maximum(_fma(row[0], r, _fma(row[1], g, _fma(row[2], b, _OPSIN_BIAS))), 0)
        m.append(_cbrt_add(mixed, add))
    x, y = _F(0.5) * (m[0] - m[1]), _F(0.5) * (m[0] + m[1])
    return np.stack([x * _F(14) + _F(0.42), y + _F(0.01), (m[2] - y) + _F(0.55)])


# libjxl blurs with a recursive (IIR) Gaussian in float32. Its rounding error is part of
# the score: for near-identical images it can move the result by several points (an image
# off by one level everywhere scores 93.6 in libjxl, 96.5 with an exact Gaussian). So the
# blur below repeats libjxl's arithmetic step by step, including its fused multiply-adds
# (AVX2/AVX-512 build) and the 4-lane unrolled form of the horizontal pass.

def _gauss_coeffs(sigma=1.5):
    """CreateRecursiveGaussian: Charalampidis 2016, three cosine terms."""
    radius = round(3.2795 * sigma + 0.2546)
    w = np.pi / (2 * radius) * np.array([1.0, 3.0, 5.0])
    p = np.array([1, -1, 1]) / np.tan(0.5 * w)
    r = np.array([1, -1, 1]) * p * p / np.sin(w)
    rho = np.exp(-0.5 * sigma * sigma * w * w) / radius
    d13 = p[0] * r[1] - r[0] * p[1]
    d35 = p[1] * r[2] - r[1] * p[2]
    d51 = p[2] * r[0] - r[2] * p[0]
    z15, z35 = d35 / d13, d51 / d13
    A = np.linalg.inv(np.array([p, r, [z15, z35, 1.0]]))
    beta = A @ np.array([1.0, radius * radius - sigma * sigma, z15 * rho[0] + z35 * rho[1] + rho[2]])
    n2 = -beta * np.cos(w * (radius + 1.0))
    d1 = -2.0 * np.cos(w)
    d2 = d1 * d1
    f = lambda *cols: np.stack(cols, 1).astype(np.float32)
    return (radius, n2.astype(np.float32), d1.astype(np.float32),
            f(n2, -d1 * n2, d2 * n2 - n2, -d2 * d1 * n2 + 2 * d1 * n2),  # mul_in
            f(-d1, d2 - 1, -d2 * d1 + 2 * d1, d2 * d2 - 3 * d2 + 1),      # mul_prev
            f(-np.ones(3), d1, -d2 + 1, d2 * d1 - 2 * d1))                # mul_prev2


_R, _N2, _D1, _MUL_IN, _MUL_PREV, _MUL_PREV2 = _gauss_coeffs()


def _blur_rows(x):
    """FastGaussian1D along the last axis of a KxHxW float32 stack (zero padding)."""
    x = np.ascontiguousarray(np.moveaxis(x, -1, 0))  # columns contiguous: much faster slicing
    W = x.shape[0]; z = np.zeros(x.shape[1:], np.float32); out = np.empty_like(x)
    at = lambda i: x[i] if 0 <= i < W else z
    prev, prev2 = [z] * 3, [z] * 3

    def one(n):
        nonlocal prev, prev2
        s = at(n - _R - 1) + at(n + _R - 1)
        o = [_fma(_MUL_PREV[k, 0], prev[k], _fma(_MUL_PREV2[k, 0], prev2[k], s * _MUL_IN[k, 0])) for k in range(3)]
        prev, prev2 = o, prev
        if n >= 0:
            out[n] = o[0] + (o[1] + o[2])

    n = -_R + 1
    while n < min(-(-(_R + 1) // 4) * 4, W):  # scalar up to RoundUpTo(radius + 1, 4)
        one(n); n += 1
    while n < W - _R + 1 - 3:                 # four outputs per step
        s = [at(n - _R - 1 + j) + at(n + _R - 1 + j) for j in range(4)]
        o = []
        for k in range(3):
            lanes = []
            for lane in range(4):
                v = s[0] * _MUL_IN[k, lane]
                for j in range(1, lane + 1):
                    v = _fma(_MUL_IN[k, lane - j], s[j], v)
                lanes.append(_fma(_MUL_PREV[k, lane], prev[k], _fma(_MUL_PREV2[k, lane], prev2[k], v)))
            o.append(lanes)
        for lane in range(4):
            out[n + lane] = o[0][lane] + (o[1][lane] + o[2][lane])
        prev, prev2 = [o[k][3] for k in range(3)], [o[k][2] for k in range(3)]
        n += 4
    while n < W:
        one(n); n += 1
    return np.moveaxis(out, 0, -1)


def _blur_cols(x):
    """FastGaussianVertical along axis 1 of a KxHxW float32 stack (zero padding)."""
    H = x.shape[1]; z = np.zeros((x.shape[0], x.shape[2]), np.float32); out = np.empty_like(x)
    at = lambda i: x[:, i] if 0 <= i < H else z
    y1, y2 = [z] * 3, [z] * 3
    for n in range(-_R + 1, H):
        s = at(n - _R - 1) + at(n + _R - 1)
        # y = n2 * sum + NegMulSub(d1, y[n-1], y[n-2])
        y = [_fma(_N2[k], s, (-(np.float64(_D1[k]) * y1[k]) - y2[k]).astype(np.float32)) for k in range(3)]
        y1, y2 = y, y1
        if n >= 0:
            out[:, n] = y[0] + (y[1] + y[2])
    return out


def _blur(planes):
    return _blur_cols(_blur_rows(planes.astype(np.float32)))


def _downsample(lin):
    """2x2 box average in linear RGB; odd edges repeat the last row/column."""
    h, w = lin.shape[:2]
    if h % 2: lin = np.concatenate([lin, lin[-1:]], 0)
    if w % 2: lin = np.concatenate([lin, lin[:, -1:]], 1)
    return ((lin[0::2, 0::2] + lin[0::2, 1::2] + lin[1::2, 0::2] + lin[1::2, 1::2]) * 0.25).astype(np.float32)


def _norms(d):
    """1-norm and 4-norm per plane of a 3xHxW error map."""
    d = d.astype(np.float64).reshape(3, -1)
    return d.mean(1), (d ** 4).mean(1) ** 0.25


_WEIGHTS = [
    0.0, 0.0007376606707406586, 0.0, 0.0, 0.0007793481682867309, 0.0, 0.0, 0.0004371155730107379,
    0.0, 1.1041726426657346, 0.00066284834129271, 0.00015231632783718752, 0.0, 0.0016406437456599754,
    0.0, 1.8422455520539298, 11.441172603757666, 0.0, 0.0007989109436015163, 0.000176816438078653,
    0.0, 1.8787594979546387, 10.94906990605142, 0.0, 0.0007289346991508072, 0.9677937080626833,
    0.0, 0.00014003424285435884, 0.9981766977854967, 0.00031949755934435053, 0.0004550992113792063,
    0.0, 0.0, 0.0013648766163243398, 0.0, 0.0, 0.0, 0.0, 0.0, 7.466890328078848, 0.0,
    17.445833984131262, 0.0006235601634041466, 0.0, 0.0, 6.683678146179332, 0.00037724407979611296,
    1.027889937768264, 225.20515300849274, 0.0, 0.0, 19.213238186143016, 0.0011401524586618361,
    0.001237755635509985, 176.39317598450694, 0.0, 0.0, 24.43300999870476, 0.28520802612117757,
    0.0004485436923833408, 0.0, 0.0, 0.0, 34.77906344483772, 44.835625328877896, 0.0, 0.0, 0.0,
    0.0, 0.0, 0.0, 0.0, 0.0, 0.0008680556573291698, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0005313191874358747,
    0.0, 0.00016533814161379112, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0004179171803251336,
    0.0017290828234722833, 0.0, 0.0020827005846636437, 0.0, 0.0, 8.826982764996862,
    23.19243343998926, 0.0, 95.1080498811086, 0.9863978034400682, 0.9834382792465353,
    0.0012286405048278493, 171.2667255897307, 0.9807858872435379, 0.0, 0.0, 0.0,
    0.0005130064588990679, 0.0, 0.00010854057858411537]


def _subscores(ref, dist):
    lin1, lin2 = _srgb_to_linear(ref), _srgb_to_linear(dist)
    scales = []
    for scale in range(6):
        if lin1.shape[0] < 8 or lin1.shape[1] < 8:
            break
        if scale:
            lin1, lin2 = _downsample(lin1), _downsample(lin2)
        i1, i2 = _xyb(lin1), _xyb(lin2)
        # all five blurs in one pass: the per-pixel loop overhead dominates
        mu1, mu2, s11, s22, s12 = np.split(_blur(np.concatenate([i1, i2, i1 * i1, i2 * i2, i1 * i2])), 5)
        # SSIM without the luma denominator (see libjxl's comment), as an error: 1 - SSIM'
        num_m = 1.0 - (mu1 - mu2) ** 2
        num_s = 2 * (s12 - mu1 * mu2) + 0.0009
        den_s = (s11 - mu1 * mu1) + (s22 - mu2 * mu2) + 0.0009
        ssim = _norms(np.maximum(1.0 - num_m * num_s / den_s, 0))
        # > 0: distorted has edges the original lacks (ringing, banding, blocking)
        # < 0: original has edges the distorted lacks (blur, smearing)
        d1 = (1.0 + np.abs(i2 - mu2)) / (1.0 + np.abs(i1 - mu1)) - 1.0
        scales.append((ssim, _norms(np.maximum(d1, 0)), _norms(np.maximum(-d1, 0))))
    # 108 sub-scores in libjxl's order: channel, scale, norm, (ssim, ringing, blur)
    return [abs(m[n][c]) for c in range(3) for sc in scales for n in range(2) for m in sc]


def _ssimulacra2_opaque(ref, dist):
    s = sum(w * v for w, v in zip(_WEIGHTS, _subscores(ref, dist)))
    s *= 0.9562382616834844
    s = 2.326765642916932 * s - 0.020884521182843837 * s * s + 6.248496625763138e-05 * s * s * s
    return 100.0 - 10.0 * s ** 0.6276336467831387 if s > 0 else 100.0


def ssimulacra2(ref, dist, ref_alpha=None, dist_alpha=None):
    """SSIMULACRA 2 score of dist against ref (HxWx3 float sRGB 0..1). With alpha, both
    are blended onto dark and light grey and the worse score wins, like libjxl."""
    if ref.shape[0] < 8 or ref.shape[1] < 8:
        raise ValueError("SSIMULACRA 2 needs images of at least 8x8 pixels")
    if ref_alpha is None:
        return _ssimulacra2_opaque(ref, dist)
    blend = lambda c, a, bg: c * a[..., None] + (1 - a[..., None]) * bg if a is not None else c
    return min(_ssimulacra2_opaque(blend(ref, ref_alpha, bg), blend(dist, dist_alpha, bg)) for bg in (0.1, 0.9))


def compare(ref_path, dist_path, diff=None):
    """Everything imgcompare reports, as a dict. Raises ValueError on a size mismatch."""
    a, aa = load(ref_path)
    b, ba = load(dist_path)
    if a.shape != b.shape:
        raise ValueError(f"size mismatch: {a.shape[1]}x{a.shape[0]} vs {b.shape[1]}x{b.shape[0]}")
    r = diff_stats(a, b)
    r["ssimulacra2"] = round(ssimulacra2(a, b, aa, ba), 4) if min(a.shape[:2]) >= 8 else None
    if diff:
        write_diff(a, b, diff)
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference", type=Path)
    ap.add_argument("candidate", type=Path)
    ap.add_argument("--diff", type=Path, help="write an 8x amplified difference image (128 = unchanged)")
    ap.add_argument("--json", action="store_true", help="machine-readable result on stdout")
    args = ap.parse_args()
    for p in (args.reference, args.candidate):
        if not p.is_file():
            print(f"not found: {p}", file=sys.stderr); return 2
    try:
        r = compare(args.reference, args.candidate, args.diff)
    except ValueError as e:
        print(str(e), file=sys.stderr); return 2
    if args.json:
        print(json.dumps(r, indent=2))
    else:
        psnr = "inf (identical)" if r["psnr_db"] is None else f"{r['psnr_db']} dB"
        print(f"ssimulacra2 {r['ssimulacra2']}  psnr {psnr}  max_abs_diff {r['max_abs_diff']}  "
              f"mean_abs_diff {r['mean_abs_diff']}  changed {r['changed_pixels_pct']}%")
        if args.diff:
            print(f"  diff -> {args.diff}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
