#!/usr/bin/env bash
# Checks that a built Wine plays game videos: tests/media/run.sh <wine dir> (the folder with bin/ lib/ share/).
# Builds mediaprobe.exe (32- and 64-bit), makes short clips in the formats games ship (H.264/AAC MP4, WMV2/WMA
# ASF, MPEG-1, VP8/Opus WebM), and plays each through Media Foundation and DirectShow in a fresh prefix.
# Needs mingw-w64 and ffmpeg (brew install mingw-w64 ffmpeg). Prints one PROBE line per run; exit 1 if any failed.
set -euo pipefail
WINE_DIR="$(cd "$1" && pwd)"
cd "$(dirname "$0")"
T="${TMP_DIR:-$PWD/.out}"; mkdir -p "$T"
rm -f "$T"/*.mp4 "$T"/*.wmv "$T"/*.mpg "$T"/*.webm  # made fresh each run (older ones had an audible tone)
for a in x64:x86_64 x86:i686; do
  "${a#*:}-w64-mingw32-gcc" -municode -O2 -s -o "$T/mediaprobe-${a%%:*}.exe" mediaprobe.c \
    -lmfplat -lmfreadwrite -lmfuuid -lstrmiids -lole32 -luuid
done
# The audio track is a tone at zero volume: still decoded, never heard (DirectShow plays it on the real speakers).
src=(-f lavfi -i testsrc2=size=320x240:rate=25:duration=3 -f lavfi -i "sine=frequency=440:duration=3,volume=0")
[ -f "$T/h264.mp4" ] || ffmpeg -loglevel error "${src[@]}" -c:v libx264 -pix_fmt yuv420p -c:a aac "$T/h264.mp4"
[ -f "$T/wmv2.wmv" ] || ffmpeg -loglevel error "${src[@]}" -c:v wmv2 -c:a wmav2 "$T/wmv2.wmv"
[ -f "$T/mpeg1.mpg" ] || ffmpeg -loglevel error "${src[@]}" -c:v mpeg1video -c:a mp2 "$T/mpeg1.mpg"
[ -f "$T/vp8.webm" ] || ffmpeg -loglevel error "${src[@]}" -c:v libvpx -c:a libopus "$T/vp8.webm"

# No macOS display driver (winemac.drv=d): Wine runs with its null driver, so neither wineboot's "updating
# configuration" window nor any test window ever reaches the screen.
export WINEPREFIX="$T/prefix" WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d;winemac.drv=d"
rm -rf "$WINEPREFIX"
"$WINE_DIR/bin/wineboot" -i >/dev/null 2>&1 || "$WINE_DIR/bin/wine" wineboot -i >/dev/null 2>&1
# Crashes go to the log, never to a dialog.
"$WINE_DIR/bin/wine" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
"$WINE_DIR/bin/wineserver" -w
# Wine's output goes to a file, never through a pipe: Wine's own background processes inherit the pipe and keep it
# open, so a probe killed by its timeout would leave the script waiting forever.
failed=0
for exe in mediaprobe-x64.exe mediaprobe-x86.exe; do
  for clip in h264.mp4 wmv2.wmv mpeg1.mpg vp8.webm; do
    for api in mf dshow; do
      (cd "$T" && timeout 120 "$WINE_DIR/bin/wine" "$exe" "$api" "$clip" > "$T/out.txt" 2>&1) || true
      out="$(tr -d '\r' < "$T/out.txt")"
      line="$(echo "$out" | grep '^PROBE' || echo "PROBE $api $clip FAILED (no output)")"
      echo "$exe: $line"
      case "$line" in *" OK") ;; *) failed=1; echo "$out" | grep -v '^PROBE' | tail -3 | sed 's/^/    /' ;; esac
    done
  done
done
"$WINE_DIR/bin/wineserver" -k 2>/dev/null || true
exit $failed
