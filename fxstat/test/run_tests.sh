#!/usr/bin/env bash
# Smoke tests. Needs SweetFX and reshade-shaders checkouts alongside this repo.
set -uo pipefail
cd "$(dirname "$0")/.."
FXSTAT=build/fxstat
SWEETFX="${SWEETFX:-../SweetFX}"
SHADERS="${SHADERS:-../reshade-shaders}"
fail=0

fxstat_has_d3dcompiler() {
  $FXSTAT --version 2>/dev/null | grep -q "D3DCompiler" && echo yes || echo no
}

check() { # name expected actual
  if [ "$2" = "$3" ]; then printf "  ok    %-42s %s\n" "$1" "$3"
  else printf "  FAIL  %-42s expected %s, got %s\n" "$1" "$2" "$3"; fail=1; fi
}

get() { # preset field
  $FXSTAT -I "$SHADERS/Shaders" ${1:+--preset "$1"} --json \
    "$SWEETFX/Shaders/SweetFX/LumaSharpen.fx" 2>/dev/null |
    python3 -c "import json,sys;e=[x for x in json.load(sys.stdin)['entry_points'] if x['stage']=='pixel'][0];print(e['$2'])"
}

echo "LumaSharpen, default preset:"
check "pixel TEX" 5 "$(get '' tex)"
check "pixel ALU" 15 "$(get '' alu)"

for p in 0 1 2 3; do
  printf "[LumaSharpen.fx]\npattern=%s\n" "$p" > /tmp/fxstat_test_p$p.ini
done
echo "sample pattern changes the cost:"
check "pattern=0 TEX" 3 "$(get /tmp/fxstat_test_p0.ini tex)"
check "pattern=1 TEX" 5 "$(get /tmp/fxstat_test_p1.ini tex)"
check "pattern=2 TEX" 5 "$(get /tmp/fxstat_test_p2.ini tex)"
check "pattern=3 TEX" 5 "$(get /tmp/fxstat_test_p3.ini tex)"

echo "whole corpus compiles:"
ok=0; bad=0
for f in "$SWEETFX"/Shaders/SweetFX/*.fx "$SHADERS"/Shaders/*.fx; do
  if $FXSTAT -I "$SHADERS/Shaders" -I "$SWEETFX/Shaders" --json "$f" >/dev/null 2>&1
  then ok=$((ok+1)); else bad=$((bad+1)); echo "  FAIL  $(basename "$f")"; fi
done
check "effects compiled" "$((ok+bad))" "$ok"


# --- DXBC back end (only if this build has it) ---
if $FXSTAT --dxbc --shader-model 50 -I "$SHADERS/Shaders" --json \
     "$SWEETFX/Shaders/SweetFX/LumaSharpen.fx" >/dev/null 2>&1; then
  getd() { # shader-model extra-args field
    $FXSTAT --dxbc --shader-model "$1" ${2:+$2} -I "$SHADERS/Shaders" --json \
      "$SWEETFX/Shaders/SweetFX/LumaSharpen.fx" 2>/dev/null |
      python3 -c "import json,sys;e=[x for x in json.load(sys.stdin)['entry_points'] if x['stage']=='pixel'][0];print(e['$3'])"
  }
  echo "DXBC (DX11, shader model 5):"
  check "perf mode TEX"      5   "$(getd 50 '' tex)"
  check "no perf mode TEX"   15  "$(getd 50 --no-performance-mode tex)"
  check "no perf mode TOTAL" 117 "$(getd 50 --no-performance-mode total)"

  echo "whole corpus compiles to DXBC:"
  ok=0; bad=0
  for f in "$SWEETFX"/Shaders/SweetFX/*.fx "$SHADERS"/Shaders/*.fx; do
    if $FXSTAT --dxbc --shader-model 50 -I "$SHADERS/Shaders" -I "$SWEETFX/Shaders" --json "$f" >/dev/null 2>&1
    then ok=$((ok+1)); else bad=$((bad+1)); echo "  FAIL  $(basename "$f")"; fi
  done
  check "effects compiled to DXBC" "$((ok+bad))" "$ok"
else
  echo "DXBC: not built into this fxstat, skipping"
fi


# --- library split: the CLI must not need spirv-opt on PATH ---
echo "no external spirv-opt dependency:"
if PATH=/nonexistent $FXSTAT -I "$SHADERS/Shaders" --json \
     "$SWEETFX/Shaders/SweetFX/LumaSharpen.fx" >/dev/null 2>&1; then
  printf "  ok    %-42s %s\n" "runs with an empty PATH" "yes"
else
  printf "  FAIL  %-42s %s\n" "runs with an empty PATH" "no"; fail=1
fi

echo "inliner-incomplete guard:"
flagged=$($FXSTAT --spirv -I "$SHADERS/Shaders" -I "$SWEETFX/Shaders" --json \
  "$SWEETFX/Shaders/SweetFX/SMAA.fx" 2>/dev/null | grep -c inliner_incomplete)
check "SMAA rows flagged as inflated" 2 "$flagged"

echo "reproducibility fields present:"
has=$($FXSTAT -I "$SHADERS/Shaders" --json "$SWEETFX/Shaders/SweetFX/LumaSharpen.fx" 2>/dev/null \
  | grep -cE '"optimizer_version"|"renderer"')
check "optimizer_version + renderer in json" 2 "$has"


# --- DXBC compiler identity must be recorded, or numbers can't be compared ---
echo "dxbc compiler recorded in output:"
if $FXSTAT --dxbc --shader-model 50 -I "$SHADERS/Shaders" --json \
     "$SWEETFX/Shaders/SweetFX/LumaSharpen.fx" 2>/dev/null | grep -q '"dxbc_compiler"'; then
  printf "  ok    %-42s %s\n" "dxbc_compiler in json" "yes"
else
  printf "  FAIL  %-42s %s\n" "dxbc_compiler in json" "no"; fail=1
fi
if $FXSTAT --dxbc --shader-model 50 -I "$SHADERS/Shaders" \
     "$SWEETFX/Shaders/SweetFX/LumaSharpen.fx" 2>/dev/null | head -1 | grep -qE "vkd3d-shader|d3dcompiler"; then
  printf "  ok    %-42s %s\n" "dxbc compiler in header line" "yes"
else
  printf "  FAIL  %-42s %s\n" "dxbc compiler in header line" "no"; fail=1
fi

echo "unavailable compiler fails clearly, not obscurely:"
msg=$($FXSTAT --dxbc --dxbc-compiler d3dcompiler -I "$SHADERS/Shaders" \
        "$SWEETFX/Shaders/SweetFX/LumaSharpen.fx" 2>&1 | grep -c "not available in this build" || true)
if [ "$(fxstat_has_d3dcompiler)" = "yes" ]; then
  printf "  skip  %-42s %s\n" "d3dcompiler refusal" "this build has D3DCompiler"
else
  check "d3dcompiler refusal message" 1 "$msg"
fi

exit $fail
