#!/usr/bin/env python3
"""
fxrender - render a ReShade .fx effect onto an image, headless, on Linux.

Drives ShaderLab (github.com/NotRayST/ShaderLab) + a MinGW build of ReShade
under Wine, then checks ReShade.log itself, because ShaderLab's `render`
command reports success even when the effect failed to compile (it just
writes the unprocessed image).

  fxrender.py LumaSharpen.fx -i shot.png -o out.png --set sharp_strength=1.5
  fxrender.py MyEffect.fx -i shot.png -o out.png --perf-mode --json

Exit code: 0 = compiled and rendered, 1 = compile error / render failure,
2 = usage or environment problem.
"""
import argparse, json, os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
APP_DIR = Path(os.environ.get("FXRENDER_APP", HERE / "runtime" / "app"))
WINEPREFIX = Path(os.environ.get("WINEPREFIX", HERE / "runtime" / "prefix"))
DEFAULT_INCLUDES = [p for p in os.environ.get("FXRENDER_INCLUDES", "").split(os.pathsep) if p]


def winpath(p):
    """Linux path -> Wine Z: path (absolute, backslashes)."""
    return "Z:" + str(Path(p).resolve()).replace("/", "\\")


def techniques_in(fx: Path):
    src = fx.read_text(errors="replace")
    src = re.sub(r"//[^\n]*|/\*.*?\*/", "", src, flags=re.S)
    seen = []
    for m in re.finditer(r"\btechnique\s+([A-Za-z_]\w*)", src):
        if m.group(1) not in seen:
            seen.append(m.group(1))
    return seen


def ensure_display(env):
    if env.get("DISPLAY"):
        return None
    disp = ":97"
    lock = Path(f"/tmp/.X{disp[1:]}-lock")
    if not lock.exists():
        subprocess.Popen(["Xvfb", disp, "-screen", "0", "1920x1080x24", "-nolisten", "tcp"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        for _ in range(50):
            if lock.exists():
                break
            time.sleep(0.1)
    env["DISPLAY"] = disp


def write_config(app: Path, fx: Path, includes, texture_dirs, preset: Path, perf_mode, defines):
    search = []
    for d in [fx.parent] + [Path(i) for i in includes]:
        w = winpath(d)
        if w.lower() not in (s.lower() for s in search):
            search.append(w)
    # Textures: the shader's own dir, explicit --textures dirs, and any "Textures" folder
    # found next to or up to three levels above the search dirs (searched recursively, so
    # SweetFX's Shaders/SweetFX + Textures/SweetFX layout works).
    textures = []
    def add_tex(w):
        if w.lower() not in (t.lower() for t in textures):
            textures.append(w)
    add_tex(winpath(fx.parent))
    for t in texture_dirs:
        add_tex(winpath(t) + "\\**")
    for d in [fx.parent] + [Path(i) for i in includes]:
        for up in (d, d.parent, d.parent.parent, d.parent.parent.parent):
            if (up / "Textures").is_dir():
                add_tex(winpath(up / "Textures") + "\\**")
    ini = f"""[GENERAL]
EffectSearchPaths={','.join(search)}
TextureSearchPaths={','.join(textures)}
PresetPath={winpath(preset)}
PerformanceMode={1 if perf_mode else 0}
PreprocessorDefinitions={','.join(defines)}
SkipLoadingDisabledEffects=1
NoReloadOnInit=0
NoEffectCache=1
IntermediateCachePath=.\\common\\ShaderCache
[INPUT]
KeyOverlay=0,0,0,0
KeyScreenshot=0,0,0,0
[OVERLAY]
TutorialProgress=4
ShowFPS=0
ShowClock=0
ShowFrameTime=0
ShowPresetName=0
ShowScreenshotMessage=0
ShowPresetTransitionMessage=0
[ShaderLab]
FirstRunDone=1
"""
    (app / "ReShade.ini").write_text(ini)


def write_preset(preset: Path, fx: Path, techs, sets):
    lines = ["Techniques=" + ",".join(f"{t}@{fx.name}" for t in techs),
             "TechniqueSorting=" + ",".join(f"{t}@{fx.name}" for t in techs), "",
             f"[{fx.name}]"]
    extra = {}
    for kv in sets:
        k, v = kv.split("=", 1)
        if ":" in k:
            sec, k = k.split(":", 1)
            extra.setdefault(sec, []).append(f"{k}={v}")
        else:
            lines.append(f"{k}={v}")
    for sec, kvs in extra.items():
        lines += ["", f"[{sec}]"] + kvs
    preset.write_text("\n".join(lines) + "\n")


LOG_DIAG = re.compile(r"^(?P<file>[A-Za-z]:\\.+?)\((?P<line>\d+), ?(?P<col>\d+)\)(?:-\d+)?: (?P<sev>(?:preprocessor )?(?:error|warning))(?: (?P<code>X\d+))?: (?P<msg>.*)$")


def parse_log(log: str, fx: Path):
    target = fx.name.lower()
    res = {"compiled": False, "failed": False, "diagnostics": [], "log_errors": []}
    for line in log.splitlines():
        low = line.lower()
        if "successfully compiled" in low and target in low:
            res["compiled"] = True
        elif "failed to compile" in low and target in low:
            res["failed"] = True
        m = LOG_DIAG.match(line.strip())
        if m:
            d = m.groupdict()
            d["line"] = int(d["line"]); d["col"] = int(d["col"]); d["code"] = d["code"] or ""
            d["file"] = d["file"].replace("Z:", "", 1).replace("\\", "/")
            res["diagnostics"].append(d)
        elif "| ERROR |" in line:
            res["log_errors"].append(line.split("| ERROR |", 1)[1].strip())
    return res


def image_stats(inp: Path, out: Path):
    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        return None
    a = np.asarray(Image.open(inp).convert("RGB")).astype(np.int16)
    b = np.asarray(Image.open(out).convert("RGB")).astype(np.int16)
    if a.shape != b.shape:
        return {"size_in": list(a.shape[1::-1]), "size_out": list(b.shape[1::-1])}
    d = np.abs(a - b)
    return {"size": [a.shape[1], a.shape[0]],
            "mean_abs_diff": round(float(d.mean()), 4),
            "max_abs_diff": int(d.max()),
            "changed_pixels_pct": round(float((d.max(2) > 0).mean() * 100), 3)}


def write_diff(inp: Path, out: Path, dst: Path, gain=8):
    from PIL import Image
    import numpy as np
    a = np.asarray(Image.open(inp).convert("RGB")).astype(np.float32)
    b = np.asarray(Image.open(out).convert("RGB")).astype(np.float32)
    d = np.clip(128 + (b - a) * gain, 0, 255).astype(np.uint8)
    Image.fromarray(d).save(dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("shader", type=Path)
    ap.add_argument("-i", "--input", type=Path, required=True, help="input image (png/jpg/bmp)")
    ap.add_argument("-o", "--output", type=Path, required=True, help="output png")
    ap.add_argument("-t", "--technique", action="append", default=[], help="technique(s) to enable (default: all in file)")
    ap.add_argument("-u", "--set", action="append", default=[], metavar="VAR=VALUE", help="uniform override (repeatable)")
    ap.add_argument("-I", "--include", action="append", default=[], help="extra effect/include search dir (e.g. reshade-shaders/Shaders)")
    ap.add_argument("-T", "--textures", action="append", default=[], help="extra texture search dir (searched recursively)")
    ap.add_argument("-D", "--define", action="append", default=[], metavar="NAME=VALUE", help="preprocessor definition")
    ap.add_argument("--perf-mode", action="store_true", help="ReShade performance mode (uniforms baked as constants)")
    ap.add_argument("-f", "--settle-frames", type=int, default=5, help="frames before capture (raise for temporal effects)")
    ap.add_argument("--diff", type=Path, help="also write an amplified difference image (128 = unchanged)")
    ap.add_argument("--json", action="store_true", help="machine-readable result on stdout")
    ap.add_argument("--keep-log", type=Path, help="copy ReShade.log here")
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()

    fx = args.shader.resolve()
    if not fx.is_file():
        print(f"shader not found: {fx}", file=sys.stderr); return 2
    if not args.input.is_file():
        print(f"input not found: {args.input}", file=sys.stderr); return 2
    if not (APP_DIR / "ShaderLab.exe").is_file():
        print(f"ShaderLab runtime not found in {APP_DIR} (run setup_runtime.sh)", file=sys.stderr); return 2

    techs = args.technique or techniques_in(fx)
    if not techs:
        print("no technique found in shader", file=sys.stderr); return 2

    env = dict(os.environ)
    env.update(WINEPREFIX=str(WINEPREFIX), WINEDEBUG="-all",
               WINEDLLOVERRIDES="dxgi=n,b;d3d11=n,b;d3d10core=n,b;d3dcompiler_47=n;winedbg.exe=d",
               DXVK_LOG_LEVEL="none", DXVK_LOG_PATH="none")
    ensure_display(env)

    out = args.output.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    work = Path(tempfile.mkdtemp(prefix="fxrender-"))
    preset = work / "preset.ini"
    write_preset(preset, fx, techs, args.set)
    write_config(APP_DIR, fx, DEFAULT_INCLUDES + args.include, args.textures, preset, args.perf_mode, args.define)
    log = APP_DIR / "ReShade.log"
    if log.exists():
        log.unlink()

    cmd = ["wine", "ShaderLab.exe", "render", "--preset", winpath(preset),
           "--input", winpath(args.input), "--output", winpath(out),
           "--settle-frames", str(args.settle_frames), "--quiet"]
    t0 = time.time()
    # Watch ReShade.log while ShaderLab runs: on a compile failure ShaderLab would otherwise
    # sit in its 30 s warm-up + 180 s capture timeouts before writing an unprocessed image.
    outf = open(work / "shaderlab.out", "w+")
    p = subprocess.Popen(cmd, cwd=APP_DIR, env=env, stdout=outf, stderr=subprocess.STDOUT)
    rc, aborted = None, False
    while True:
        try:
            rc = p.wait(timeout=0.25)
            break
        except subprocess.TimeoutExpired:
            pass
        if time.time() - t0 > args.timeout:
            aborted = True
        elif log.exists() and "failed to compile" in log.read_text(errors="replace").lower():
            time.sleep(0.5)  # let the diagnostics lines land
            aborted = True
        if aborted:
            p.kill()
            subprocess.run(["wineserver", "-k"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            p.wait(); rc = -1
            break
    outf.seek(0); stdout = outf.read(); outf.close()
    elapsed = time.time() - t0

    logtext = log.read_text(errors="replace") if log.exists() else ""
    if args.keep_log and logtext:
        shutil.copy(log, args.keep_log)
    r = parse_log(logtext, fx)
    ok = r["compiled"] and not r["failed"] and out.is_file() and rc == 0 and not r["log_errors"]
    result = {
        "shader": str(fx), "techniques": techs, "uniforms": args.set,
        "performance_mode": args.perf_mode, "defines": args.define,
        "compiled": r["compiled"] and not r["failed"],
        "rendered": out.is_file() and rc == 0,
        "output": str(out) if out.is_file() else None,
        "diagnostics": r["diagnostics"], "reshade_errors": r["log_errors"],
        "seconds": round(elapsed, 2),
    }
    if not logtext:
        result["reshade_errors"].append("ReShade.log missing - ReShade was not loaded into ShaderLab")
    if out.is_file():
        st = image_stats(args.input, out)
        if st:
            result["image"] = st
            if ok and st.get("max_abs_diff") == 0:
                result["warning"] = "output is identical to input - effect had no visible effect (or was not applied)"
        if args.diff and st and "size" in st:
            write_diff(args.input, out, args.diff); result["diff"] = str(args.diff.resolve())
    if not ok and out.is_file():
        # ShaderLab writes the unprocessed image when compilation fails; don't leave a misleading file
        out.unlink(); result["output"] = None
    if rc != 0 and not r["diagnostics"]:
        result["shaderlab_output"] = stdout[-2000:]
    shutil.rmtree(work, ignore_errors=True)

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        status = "OK" if ok else "FAILED"
        print(f"[{status}] {fx.name}  techniques={','.join(techs)}  {elapsed:.1f}s")
        for d in r["diagnostics"]:
            print(f"  {d['file']}({d['line']},{d['col']}): {d['sev']}{' ' + d['code'] if d['code'] else ''}: {d['msg']}")
        for e in r["log_errors"]:
            print(f"  reshade: {e}")
        if "image" in result:
            print(f"  image: {result['image']}")
        if result.get("warning"):
            print(f"  WARNING: {result['warning']}")
        if result["output"]:
            print(f"  -> {result['output']}")
        if result.get("diff"):
            print(f"  diff -> {result['diff']}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
