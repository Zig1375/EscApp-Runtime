#!/usr/bin/env bash
# Named CNG keys (ncrypt) against a built Wine:
#   tests/ncrypt/run.sh <wine dir> <wine source tree> <wine build tree>
# Runs keyprobe the way the Epic Games Launcher uses its key: one run creates a named key and signs with it; Wine is
# stopped (wineserver -k, as EscApp closes a store); a second run opens the key by name and checks it is the same.
# Then Wine's own ncrypt conformance test, which has a test for named keys (test_named_key).
set -euo pipefail
WINE_DIR="$(cd "$1" && pwd)"; SRC="$(cd "$2" && pwd)"; BLD="$(cd "$3" && pwd)"
cd "$(dirname "$0")"
T="${TMP_DIR:-$PWD/.out}"; mkdir -p "$T"
x86_64-w64-mingw32-gcc -O2 -Wall -o "$T/keyprobe.exe" keyprobe.c -lncrypt -lbcrypt
# Wine's test as in tests/winetest: Wine's Windows headers with mingw's own C library
CFLAGS=(-O2 -I"$BLD/include" -I"$SRC/include" -D__WINE_PE_BUILD -D_WIN32_WINNT=0x0a00 -fno-strict-aliasing
        -Wno-format -mcx16 -include windows.h -include wine/debug.h -Ddebugstr_w=wine_dbgstr_w -Ddebugstr_a=wine_dbgstr_a)
cat > "$T/testlist-ncrypt.c" <<C
#define STANDALONE
#include <windows.h>
#include "wine/test.h"
extern void func_ncrypt(void);
const struct test winetest_testlist[] = { { "ncrypt", func_ncrypt }, { 0, 0 } };
C
x86_64-w64-mingw32-gcc "${CFLAGS[@]}" -o "$T/ncrypt_test.exe" "$SRC/dlls/ncrypt/tests/ncrypt.c" "$T/testlist-ncrypt.c" \
  -L"$BLD/dlls/ncrypt/x86_64-windows" -L"$BLD/dlls/bcrypt/x86_64-windows" -L"$BLD/dlls/ntdll/x86_64-windows" \
  -L"$BLD/dlls/kernel32/x86_64-windows" -lncrypt -lbcrypt -lntdll -lkernel32 -luser32 -ladvapi32

# No macOS display driver: nothing reaches the screen. Output goes to files, never through pipes (tests/winetest).
export WINEPREFIX="$T/prefix" WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d;winemac.drv=d" \
  WINEDEBUG="${WINEDEBUG:--all}"
rm -rf "$WINEPREFIX"
"$WINE_DIR/bin/wine" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
"$WINE_DIR/bin/wineserver" -w
failed=0
mkdir -p "$T/results"
(cd "$T" && timeout 120 "$WINE_DIR/bin/wine" keyprobe.exe create 'Z:'"$T/results" > "$T/out.txt" 2>&1) || true
tr -d '\r' < "$T/out.txt" | grep -E '^(PASS|FAIL)' | sed 's/^/create: /' || echo "create: keyprobe crashed"
grep -q '^FAIL' "$T/out.txt" && failed=1
grep -q '^PASS results written' "$T/out.txt" || failed=1
# Everything in the prefix stops, as when EscApp closes a store; the key must be there for the next start.
"$WINE_DIR/bin/wineserver" -k 2>/dev/null || true; "$WINE_DIR/bin/wineserver" -w 2>/dev/null || true
(cd "$T" && timeout 120 "$WINE_DIR/bin/wine" keyprobe.exe check 'Z:'"$T/results" > "$T/out.txt" 2>&1) || true
tr -d '\r' < "$T/out.txt" | grep -E '^(PASS|FAIL|KEY PROBE)' | sed 's/^/check: /' || echo "check: keyprobe crashed"
grep -q 'KEY PROBE OK' "$T/out.txt" || failed=1

(cd "$T" && timeout 300 "$WINE_DIR/bin/wine" ncrypt_test.exe > "$T/out.txt" 2>&1) || true
out="$(tr -d '\r' < "$T/out.txt")"
summary="$(echo "$out" | grep -E 'tests executed' | tail -1 || true)"
echo "ncrypt: ${summary:-no summary (crashed?)}"
{ echo "$out" | grep -E 'Test failed' || true; } | sed -E 's/^[0-9a-f]+://' | sed 's/^/    /'
echo "$out" | grep -qE 'Test failed' && failed=1
[ -n "$summary" ] || failed=1

"$WINE_DIR/bin/wineserver" -k 2>/dev/null || true; "$WINE_DIR/bin/wineserver" -w 2>/dev/null || true
rm -rf "$WINEPREFIX"
exit $failed
