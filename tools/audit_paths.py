#!/usr/bin/env python3
"""
Audit which code path each ReShade effect takes per graphics API.

Effects branch on __RENDERER__ (and a few other macros) to pick between
API-specific implementations. Those conditions were written at different times,
so a shader written before D3D12 or Vulkan existed can silently fall through to
a legacy path on a modern API -- with no warning, because an undefined or
unmatched comparison is simply false.

This preprocesses each effect once per renderer ID and groups the renderers by
the resulting source. Anything with more than one group is making an API-specific
decision; whether that decision is right is then a human question, but the list
is short and exact.

usage: audit_paths.py --cli <reshadefx_cli> -I <dir> [-I <dir>] <effect.fx>...
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
from collections import OrderedDict

# Renderer IDs as source/runtime.cpp assigns them.
RENDERERS = OrderedDict([
    ("DX9",     0x9000),
    ("DX10",    0xa000),
    ("DX10.1",  0xa100),
    ("DX11",    0xb000),
    ("DX11.1",  0xb100),
    ("DX12",    0xc000),
    ("OpenGL",  0x10000),
    ("Vulkan",  0x20000),
])

# Conditions of this shape exclude any renderer ID that did not exist when the
# line was written. `>=` and `<` degrade gracefully; `==` and `!=` do not.
EXACT_TEST = re.compile(r'__RENDERER__\s*(==|!=)')
RANGE_TEST = re.compile(r'__RENDERER__\s*(>=|<=|>|<)')


def preprocess(cli, effect, includes, renderer, extra_defines):
    cmd = [cli, "-D", f"__RENDERER__={renderer}"]
    for name, value in extra_defines:
        cmd += ["-D", f"{name}={value}"]
    for inc in includes:
        cmd += ["-I", inc]
    cmd += ["--hlsl", "-P", "-", effect]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return None
    if out.returncode != 0:
        return None
    # Normalise whitespace so formatting noise does not create false groups.
    text = "\n".join(line.rstrip() for line in out.stdout.splitlines() if line.strip())
    return hashlib.sha256(text.encode()).hexdigest(), len(text)


def scan_conditions(path):
    """Report the renderer-dependent preprocessor conditions in a file."""
    found = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                s = line.strip()
                if not s.startswith("#if") and not s.startswith("#elif"):
                    continue
                if "__RENDERER__" not in s:
                    continue
                kind = "exact" if EXACT_TEST.search(s) else ("range" if RANGE_TEST.search(s) else "other")
                found.append((n, kind, s))
    except OSError:
        pass
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("-I", dest="includes", action="append", default=[])
    ap.add_argument("-D", dest="defines", action="append", default=[],
                    help="extra macro, name=value")
    ap.add_argument("--scan-dirs", action="append", default=[],
                    help="also scan .fxh files in these directories for conditions")
    ap.add_argument("effects", nargs="+")
    args = ap.parse_args()

    extra = []
    for d in args.defines:
        name, _, value = d.partition("=")
        extra.append((name, value or "1"))

    divergent = []
    skipped = []

    for effect in args.effects:
        groups = OrderedDict()
        ok = True
        for name, rid in RENDERERS.items():
            result = preprocess(args.cli, effect, args.includes, hex(rid), extra)
            if result is None:
                ok = False
                break
            digest, size = result
            groups.setdefault(digest, {"renderers": [], "size": size})["renderers"].append(name)
        if not ok:
            skipped.append(effect)
            continue
        if len(groups) > 1:
            divergent.append((effect, groups))

    print(f"effects checked : {len(args.effects) - len(skipped)}")
    print(f"API-dependent   : {len(divergent)}")
    if skipped:
        print(f"skipped (preprocessor failed): {', '.join(os.path.basename(s) for s in skipped)}")
    print()

    for effect, groups in divergent:
        name = os.path.basename(effect)
        print(f"{name}")
        for digest, info in groups.items():
            rs = ", ".join(info["renderers"])
            print(f"    [{info['size']:6d} chars]  {rs}")

        # Explain it with the actual conditions, from the effect and its includes.
        conds = scan_conditions(effect)
        for d in args.scan_dirs:
            for root, _, files in os.walk(d):
                for f in files:
                    if f.endswith((".fxh", ".fx")) and f != name:
                        c = scan_conditions(os.path.join(root, f))
                        if c and os.path.splitext(f)[0] in open(effect, encoding="utf-8", errors="replace").read():
                            conds += [(f"{f}:{n}", k, s) for n, k, s in c]
        for n, kind, s in conds:
            flag = "  <-- exact match, excludes newer renderer IDs" if kind == "exact" else ""
            print(f"    line {n}: {s}{flag}")
        print()


if __name__ == "__main__":
    main()
