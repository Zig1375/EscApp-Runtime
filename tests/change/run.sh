#!/usr/bin/env bash
# Directory change notifications (ReadDirectoryChangesW, FindFirstChangeNotification) against a built Wine:
#   tests/change/run.sh <wine dir> <wine source tree> <wine build tree> [<older wine dir>]
# Runs changeprobe (the Epic Games Launcher's case: one process waits for a file another one writes, then the kinds
# of records one by one) and Wine's own kernel32:change and ntdll:change. With an older Wine, the same runs there
# too and the Wine tests' failures are compared: there must not be more of them. Some remain on macOS: a few checks
# wait 0 or 100 ms right after a change, and FSEvents reports through fseventsd, later than that; one gets a removal
# its directory saw a moment before the watch began (the test deletes and recreates the same folder).
set -euo pipefail
WINE_DIR="$(cd "$1" && pwd)"; SRC="$(cd "$2" && pwd)"; BLD="$(cd "$3" && pwd)"
OLD_DIR="${4:+$(cd "$4" && pwd)}"
cd "$(dirname "$0")"
T="${TMP_DIR:-$PWD/.out}"; mkdir -p "$T"
x86_64-w64-mingw32-gcc -municode -O2 -Wall -o "$T/changeprobe.exe" changeprobe.c
# Wine's tests as in tests/winetest: Wine's Windows headers with mingw's own C library
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
build kernel32_change kernel32 change
build ntdll_change ntdll change

# No macOS display driver: nothing reaches the screen. Output goes to files, never through pipes (tests/winetest).
export WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d;winemac.drv=d" WINEDEBUG="${WINEDEBUG:--all}"
run() { # wine dir, label; failures of Wine's tests go to $T/failures.<label>
  local wine="$1" label="$2" out summary
  export WINEPREFIX="$T/prefix-$label"
  rm -rf "$WINEPREFIX"
  "$wine/bin/wine" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
  "$wine/bin/wineserver" -w
  (cd "$T" && timeout 120 "$wine/bin/wine" changeprobe.exe > "$T/out.txt" 2>&1) || true
  tr -d '\r' < "$T/out.txt" | grep -E '^(PASS|FAIL|CHANGE PROBE)' | sed "s/^/$label: /" || echo "$label: changeprobe crashed"
  grep -q 'CHANGE PROBE OK' "$T/out.txt" && probe_ok=1 || probe_ok=0
  : > "$T/failures.$label"
  for t in kernel32_change ntdll_change; do
    (cd "$T" && timeout 600 "$wine/bin/wine" "$t.exe" > "$T/out.txt" 2>&1) || true
    out="$(tr -d '\r' < "$T/out.txt")"
    summary="$(echo "$out" | grep -E 'tests executed' | tail -1 || true)"
    echo "$label $t: ${summary:-no summary (crashed?)}"
    { echo "$out" | grep -E 'Test failed' || true; } | sed -E 's/^[0-9a-f]+://' | sed "s/^/$t /" \
      | sort -u >> "$T/failures.$label"
    [ -n "$summary" ] || echo "$t crashed" >> "$T/failures.$label"
  done
  "$wine/bin/wineserver" -k 2>/dev/null || true; "$wine/bin/wineserver" -w 2>/dev/null || true
  rm -rf "$WINEPREFIX"
}
failed=0
run "$WINE_DIR" new; [ "$probe_ok" = 1 ] || failed=1
if [ -n "$OLD_DIR" ]; then
  run "$OLD_DIR" old
  new="$(wc -l < "$T/failures.new" | tr -d ' ')"; old="$(wc -l < "$T/failures.old" | tr -d ' ')"
  # a check the older Wine never reached (it timed out before) can fail here, so the count decides
  extra="$(comm -13 <(sort "$T/failures.old") <(sort "$T/failures.new"))"
  echo "Wine's tests: $new failures, $old with the older Wine"
  [ -z "$extra" ] || { echo "failing here only:"; echo "$extra" | sed 's/^/    /'; }
  [ "$new" -le "$old" ] || failed=1
else
  sed 's/^/    /' "$T/failures.new"
fi
exit $failed
