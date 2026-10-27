#!/usr/bin/env bash
# Permissions given to files and folders, against a built Wine:
#   tests/acl/run.sh <wine dir>
# Runs aclprobe the way the Epic Games Launcher prepares a game's folder: one run grants the players' group full
# access to a new folder; Wine is stopped (wineserver -k, as EscApp closes a store); a second run reads the folder's
# permissions back and writes into the folder.
set -euo pipefail
WINE_DIR="$(cd "$1" && pwd)"
cd "$(dirname "$0")"
T="${TMP_DIR:-$PWD/.out}"; mkdir -p "$T"
x86_64-w64-mingw32-gcc -municode -O2 -Wall -o "$T/aclprobe.exe" aclprobe.c

# No macOS display driver: nothing reaches the screen. Output goes to files, never through pipes (tests/winetest).
export WINEPREFIX="$T/prefix" WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d;winemac.drv=d" \
  WINEDEBUG="${WINEDEBUG:--all}"
rm -rf "$WINEPREFIX"
"$WINE_DIR/bin/wine" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
"$WINE_DIR/bin/wineserver" -w
failed=0
DIR='C:\Program Files\aclprobe'
(cd "$T" && timeout 120 "$WINE_DIR/bin/wine" aclprobe.exe grant "$DIR" > "$T/out.txt" 2>&1) || true
tr -d '\r' < "$T/out.txt" | grep -E '^(PASS|FAIL)' | sed 's/^/grant: /' || echo "grant: aclprobe crashed"
grep -q '^FAIL' "$T/out.txt" && failed=1
# Everything in the prefix stops, as when EscApp closes a store; the permissions must be there afterwards.
"$WINE_DIR/bin/wineserver" -k 2>/dev/null || true; "$WINE_DIR/bin/wineserver" -w 2>/dev/null || true
(cd "$T" && timeout 120 "$WINE_DIR/bin/wine" aclprobe.exe check "$DIR" > "$T/out.txt" 2>&1) || true
tr -d '\r' < "$T/out.txt" | grep -E '^(PASS|FAIL|ACL PROBE)' | sed 's/^/check: /' || echo "check: aclprobe crashed"
grep -q 'ACL PROBE OK' "$T/out.txt" || failed=1

"$WINE_DIR/bin/wineserver" -k 2>/dev/null || true; "$WINE_DIR/bin/wineserver" -w 2>/dev/null || true
rm -rf "$WINEPREFIX"
exit $failed
