#!/usr/bin/env bash
# Compares a built Wine's synchronization in several modes: tests/sync/run.sh <wine dir> [rounds]
# Builds syncprobe.exe (32- and 64-bit) and runs it <rounds> times (default 3) in each mode, in a fresh prefix.
# MODES lists the environment of each mode (default "WINEOSYNC=0 WINEOSYNC=1"). Every run must print
# "PROBE sync OK"; the bench lines give the timings. Then the differential
# fuzz: seeded sequences of calls (FUZZ_SEEDS, default 1 2 3) must give the same hash in every mode as in the first,
# which should be the server's own implementation. And in every mode, three ways to take the server down: threads
# suspended and killed at any instant (stress-suspend), processes killed with SIGKILL while they hold objects
# (holder, HOLDER_ROUNDS times, default 10), and, with osync, wineserver itself killed under waiting threads
# (orphan): nothing may hang, and no process may outlive the server. Needs mingw-w64.
set -euo pipefail
WINE_DIR="$(cd "$1" && pwd)"; ROUNDS="${2:-3}"
cd "$(dirname "$0")"
T="${TMP_DIR:-$PWD/.out}"; mkdir -p "$T"
for a in x64:x86_64 x86:i686; do
  "${a#*:}-w64-mingw32-gcc" -O2 -s -o "$T/syncprobe-${a%%:*}.exe" syncprobe.c
done
# No macOS display driver (winemac.drv=d): Wine runs with its null driver, so neither wineboot's "updating
# configuration" window nor any test window ever reaches the screen.
export WINEPREFIX="$T/prefix" WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d;winemac.drv=d"
rm -rf "$WINEPREFIX"
"$WINE_DIR/bin/wine" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
"$WINE_DIR/bin/wineserver" -w
# Wine's output goes to a file, never through a pipe: Wine's own background processes inherit the pipe and keep it
# open, so a probe killed by its timeout would leave the script waiting forever.
failed=0
rm -f "$T"/fuzz.*

restart_server() {  # also after a hang: -k ends a stuck server with SIGKILL after a while
  "$WINE_DIR/bin/wineserver" -k 2>/dev/null || true; "$WINE_DIR/bin/wineserver" -w 2>/dev/null || true
  pkill -9 -f 'syncprobe-x(64|86)\.exe' 2>/dev/null || true
}
wait_for_line() {  # file, text, seconds
  local i
  for i in $(seq $(($3 * 10))); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done
  return 1
}
server_pid() {  # wineserver of this prefix: it holds the lock file of its directory
  lsof -t "$(printf '/tmp/.wine-%d/server-%x-%x' "$(id -u)" "$(stat -f %d "$WINEPREFIX")" "$(stat -f %i "$WINEPREFIX")")/lock" 2>/dev/null | head -1
}
takedown_tests() {  # mode, exe
  local out pid i
  (cd "$T" && timeout 120 "$WINE_DIR/bin/wine" "$2" stress-suspend > "$T/out.txt" 2>&1) || true
  out="$(tr -d '\r' < "$T/out.txt")"
  echo "$1 $2 stress-suspend: $(echo "$out" | grep -E '^PROBE' || echo 'PROBE stress-suspend FAILED (hang)')"
  echo "$out" | grep -E '^stress' | sed 's/^/    /' || true
  echo "$out" | grep -q '^PROBE stress-suspend OK' || { failed=1; echo "$out" | grep FAILED | head -5 | sed 's/^/    ! /' || true; }
  restart_server

  rm -f "$T/out.txt"
  (cd "$T" && timeout 300 "$WINE_DIR/bin/wine" "$2" holder "${HOLDER_ROUNDS:-10}" > "$T/out.txt" 2>&1) &
  pid=$!
  for i in $(seq 0 $((${HOLDER_ROUNDS:-10} - 1))); do
    wait_for_line "$T/out.txt" "holder ready $i" 30 || break
    sleep "0.$(printf %03d $((RANDOM % 400)))"
    pkill -9 -f "$2 holder-child" 2>/dev/null || true
  done
  wait "$pid" 2>/dev/null || true
  out="$(tr -d '\r' < "$T/out.txt")"
  echo "$1 $2 holder: $(echo "$out" | grep -E '^PROBE' || echo "PROBE holder FAILED (hang after $(echo "$out" | grep -c 'holder ready') kills)")"
  echo "$out" | grep -q '^PROBE holder OK' || { failed=1; echo "$out" | grep FAILED | head -5 | sed 's/^/    ! /' || true; }
  restart_server

  # without osync, Wine leaves such a process behind too (seen with wineserver's own waits): only osync is held to it
  [ "$1" = WINEOSYNC=0 ] && return 0
  rm -f "$T/out.txt"
  (cd "$T" && "$WINE_DIR/bin/wine" "$2" orphan > "$T/out.txt" 2>&1 &)
  if wait_for_line "$T/out.txt" "orphan ready" 60; then
    pid="$(pgrep -f "$2 orphan" | head -1)"
    kill -9 "$(server_pid)" 2>/dev/null || true
    for i in $(seq 150); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$pid" 2>/dev/null; then
      failed=1; echo "$1 $2 orphan: FAILED, still running 15 s after wineserver was killed"
    else echo "$1 $2 orphan: exited after wineserver was killed"; fi
  else failed=1; echo "$1 $2 orphan: FAILED (no output)"; fi
  restart_server
}

m=0
for mode in ${MODES:-WINEOSYNC=0 WINEOSYNC=1}; do
  export "${mode?}"
  for exe in syncprobe-x64.exe syncprobe-x86.exe; do
    for i in $(seq "$ROUNDS"); do
      (cd "$T" && timeout 300 "$WINE_DIR/bin/wine" "$exe" > "$T/out.txt" 2>&1) || true
      out="$(tr -d '\r' < "$T/out.txt")"
      echo "$mode $exe #$i: $(echo "$out" | grep -E '^PROBE' || echo 'PROBE sync FAILED (no output)')"
      echo "$out" | grep -E '^(bench|note|cache)' | sed 's/^/    /'
      echo "$out" | grep -q '^PROBE sync OK' || { failed=1; echo "$out" | grep -vE '^(bench|PROBE|syncprobe)' | tail -5 | sed 's/^/    ! /'; }
    done
  done
  for exe in syncprobe-x64.exe syncprobe-x86.exe; do takedown_tests "$mode" "$exe"; done
  for exe in syncprobe-x64.exe syncprobe-x86.exe; do
    for seed in ${FUZZ_SEEDS:-1 2 3}; do
      (cd "$T" && timeout 300 "$WINE_DIR/bin/wine" "$exe" fuzz "$seed" 20000 > "$T/fuzz-out.txt" 2>/dev/null) || true
      (tr -d '\r' < "$T/fuzz-out.txt" | grep '^fuzz' \
        | sed "s/^/$exe /" || echo "$exe fuzz seed $seed FAILED") >> "$T/fuzz.$m"
    done
  done
  "$WINE_DIR/bin/wineserver" -k 2>/dev/null || true; "$WINE_DIR/bin/wineserver" -w 2>/dev/null || true
  m=$((m + 1))
done
m=0
for mode in ${MODES:-WINEOSYNC=0 WINEOSYNC=1}; do
  if [ $m -gt 0 ]; then
    if cmp -s "$T/fuzz.0" "$T/fuzz.$m"; then echo "fuzz $mode: same results as the first mode ($(wc -l < "$T/fuzz.$m") runs)"
    else failed=1; echo "fuzz $mode: DIFFERENT results"; diff "$T/fuzz.0" "$T/fuzz.$m" | sed 's/^/    /'; fi
  fi
  m=$((m + 1))
done
exit $failed
