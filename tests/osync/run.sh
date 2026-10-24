#!/usr/bin/env bash
# Builds and runs osync's unit and stress tests natively (no Wine): tests/osync/run.sh [iterations] [header dir]
# Variants: arm64, x86_64 (through Rosetta, as Wine runs), ThreadSanitizer, AddressSanitizer + UBSan.
# The header comes from <header dir>/wine/osync.h, by default extracted from wine/patches (the osync patch).
set -euo pipefail
cd "$(dirname "$0")"
ITER="${1:-200000}"
T="${TMP_DIR:-$PWD/.out}"; mkdir -p "$T"
INC="${2:-}"
if [ -z "$INC" ]; then
  INC="$T/include"; mkdir -p "$INC/wine"
  python3 - "$INC/wine/osync.h" ../../wine/patches/*osync*.patch <<'PY'
import sys
out, patch = sys.argv[1], sys.argv[2]
lines, take = [], False
for l in open(patch):
    if l.startswith('+++ '): take = l.strip().endswith('include/wine/osync.h'); continue
    if take:
        if l.startswith('diff --git'): break
        if l.startswith('+'): lines.append(l[1:])
open(out, 'w').writelines(lines)
PY
fi
CFLAGS=(-std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Werror -I"$INC" -mmacosx-version-min=15.0)
failed=0
run() { # name, then compiler flags
  local name="$1"; shift
  clang "${CFLAGS[@]}" "$@" -o "$T/osync_test-$name" osync_test.c -lpthread
  echo "== $name"
  if ! "$T/osync_test-$name" "$ITER"; then failed=1; fi
}
run arm64 -arch arm64
run x86_64 -arch x86_64
run tsan -arch arm64 -fsanitize=thread
run asan -arch arm64 -fsanitize=address,undefined -fno-sanitize-recover=undefined
exit $failed
