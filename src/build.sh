#!/usr/bin/env bash
# Build the 32-bit Windows proxy dinput8.dll (FF13-SpeedrunPractice).
# Compiles the mod source together with the FF13 DLL modules (ff13_hook = SteamStub-aware
# inline hooks, ff13_crypt = save-file crypto). zig cross-compiles, so this runs anywhere.
#
# Two layouts, one script (kept identical in both repos on purpose):
#   private monorepo  ../../common/lib holds the modules, ../../common/re/zig the toolchain
#   public repo       ../lib holds the modules, zig comes from $ZIG or PATH (CI installs it)
#
# Default = release build (features only: one-hit-kill / enemy-group suppression /
# overlay). Set FF13_DIAG=1 to also compile the RE investigation aids (AI-territory
# setter hook, activate-variant probes, verbose applyHPDelta logger):
#     FF13_DIAG=1 ./build.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"            # <repo>/sr_practice/src or <repo>/src
if [ -d "$here/../../common/lib" ]; then
    lib="$(cd "$here/../../common/lib" && pwd)"
    libspec="../../common"                       # what the dirty-tree check watches
    ZIG="${ZIG:-$here/../../common/re/zig/zig}"  # env ZIG wins (CI); else the vendored toolchain
else
    lib="$(cd "$here/../lib" && pwd)"
    libspec="../lib"
    ZIG="${ZIG:-zig}"                            # env ZIG wins; else PATH
fi
out="$here/out"
mkdir -p "$out"
# Diagnostics are compiled in only when FF13_DIAG is set (any non-empty value).
EXTRA=""
[ -n "${FF13_DIAG:-}" ] && EXTRA="-DFF13_DIAG"
# Which source this DLL was built from, printed at the top of the log. __DATE__/__TIME__ are not
# an option here -- the build is kept reproducible and clang refuses them -- and the commit is the
# more useful answer anyway. "+" marks a working tree with uncommitted changes.
rev="$(cd "$here" && git rev-parse --short HEAD 2>/dev/null || echo unknown)"
(cd "$here" && git diff --quiet HEAD -- . "$libspec" 2>/dev/null) || rev="$rev+"
EXTRA="$EXTRA -DFF13_BUILD_ID=\"$rev\""
# Version info resource (ProductName etc. in the DLL's file properties).
"$ZIG" rc "$here/version.rc" "$out/version.res"
"$ZIG" cc -target x86-windows-gnu -shared -O2 $EXTRA \
    -I"$here" -I"$lib" \
    -o "$out/dinput8.dll" \
    "$here/dllmain.c" "$here/srp_proxy.c" "$here/srp_core.c" "$here/srp_speed.c" \
    "$here/srp_battle.c" "$here/srp_picker.c" "$here/srp_warp.c" "$here/srp_overlay.c" \
    "$here/srp_save.c" "$here/srp_crash.c" \
    "$lib/ff13_hook.c" "$lib/ff13_crypt.c" "$here/dinput8.def" \
    "$out/version.res" \
    -lkernel32 -luser32 -lgdi32
echo "built: $out/dinput8.dll${EXTRA:+ (with $EXTRA)}"
