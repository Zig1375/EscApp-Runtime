#!/usr/bin/env bash
# Runs Wine's own conformance tests for synchronization against a built Wine, without and with osync:
#   tests/winetest/run.sh <wine dir> <wine source tree> <wine build tree>
# Builds kernel32:sync and ntdll:sync from the source tree (the build tree provides the generated
# headers and import libraries) with mingw-w64, and compares the failures in each mode with the server's: osync must
# not add any. A "todo" check that passes (timing-dependent ones do now and then) is reported, not counted. MODES as
# in tests/sync (default "WINEOSYNC=0 WINEOSYNC=1").
set -euo pipefail
WINE_DIR="$(cd "$1" && pwd)"; SRC="$(cd "$2" && pwd)"; BLD="$(cd "$3" && pwd)"
cd "$(dirname "$0")"
T="${TMP_DIR:-$PWD/.out}"; mkdir -p "$T"
# Wine's Windows headers with mingw's own C library (Wine's CRT headers would want Wine's CRT to link with)
CFLAGS=(-O2 -I"$BLD/include" -I"$SRC/include" -D__WINE_PE_BUILD -D_WIN32_WINNT=0x0a00 -fno-strict-aliasing
        -Wno-format -mcx16 -include windows.h -include wine/debug.h -Ddebugstr_w=wine_dbgstr_w -Ddebugstr_a=wine_dbgstr_a)
build() { # name dll test
  local name="$1" dll="$2" test="$3"
  cat > "$T/testlist-$name.c" <<C
#define STANDALONE
#include <windows.h>
#include "wine/test.h"
extern void func_$test(void);
const struct test winetest_testlist[] = { { "$test", func_$test }, { 0, 0 } };
C
  x86_64-w64-mingw32-gcc "${CFLAGS[@]}" -o "$T/$name.exe" "$SRC/dlls/$dll/tests/$test.c" "$T/testlist-$name.c" \
    -L"$BLD/dlls/ntdll/x86_64-windows" -L"$BLD/dlls/kernel32/x86_64-windows" -lntdll -lkernel32 -luser32 -ladvapi32
}
build kernel32_sync kernel32 sync
build ntdll_sync ntdll sync
# ntdll:om isn't here: built this way (mingw's C library) it crashes at start even without osync.

# No macOS display driver (winemac.drv=d): Wine runs with its null driver, so neither wineboot's "updating
# configuration" window nor any test window ever reaches the screen.
export WINEPREFIX="$T/prefix" WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d;winemac.drv=d" WINEDEBUG="${WINEDEBUG:--all}"
rm -rf "$WINEPREFIX"
"$WINE_DIR/bin/wine" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
"$WINE_DIR/bin/wineserver" -w
# Wine's output goes to a file, never through a pipe: Wine's own background processes inherit the pipe and keep it
# open, so a probe killed by its timeout would leave the script waiting forever.
failed=0; m=0
for mode in ${MODES:-WINEOSYNC=0 WINEOSYNC=1}; do
  export "${mode?}"
  : > "$T/failures.$m"
  for t in kernel32_sync ntdll_sync; do
    (cd "$T" && timeout 600 "$WINE_DIR/bin/wine" "$t.exe" > "$T/out.txt" 2>&1) || true
    out="$(tr -d '\r' < "$T/out.txt")"
    summary="$(echo "$out" | grep -E 'tests executed' | tail -1 || true)"
    echo "$mode $t: ${summary:-no summary (crashed?)}"
    { echo "$out" | grep -E 'Test failed' || true; } | sed -E 's/^[0-9a-f]+://' | sed "s/^/$t /" \
      | sort -u >> "$T/failures.$m"
    { echo "$out" | grep -E 'Test succeeded inside todo' || true; } | sed -E 's/^[0-9a-f]+://' | sed "s/^/    note: $t /"
    [ -n "$summary" ] || echo "$t crashed" >> "$T/failures.$m"
  done
  "$WINE_DIR/bin/wineserver" -k 2>/dev/null || true; "$WINE_DIR/bin/wineserver" -w 2>/dev/null || true
  if [ $m -gt 0 ]; then
    extra="$(comm -13 <(sort "$T/failures.0") <(sort "$T/failures.$m"))"
    if [ -n "$extra" ]; then failed=1; echo "$mode: failures the server doesn't have:"; echo "$extra" | sed 's/^/    /'
    else echo "$mode: no failures beyond the server's ($(wc -l < "$T/failures.$m") lines in both)"; fi
  fi
  m=$((m + 1))
done
exit $failed
