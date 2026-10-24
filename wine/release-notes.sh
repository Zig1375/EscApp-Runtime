#!/usr/bin/env bash
# Prints a release's notes: wine/release-notes/<tag>.md, then what the build itself knows (component versions from
# build.sh, the archive's SHA-256 and size, the runtime manifest entry). Used by the Wine workflow.
#   wine/release-notes.sh <tag> <archive> <component.json> <commit>
set -euo pipefail
TAG="$1"; COMMIT="$4"
ARCHIVE="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; COMPONENT="$(cd "$(dirname "$3")" && pwd)/$(basename "$3")"
cd "$(dirname "$0")"
NOTES="release-notes/$TAG.md"
[ -f "$NOTES" ] || { echo "no release notes: wine/$NOTES" >&2; exit 1; }
v() { sed -n "s/^$1=\"\(.*\)\"$/\1/p" build.sh; }
runtime="$(v RUNTIME_NAME)"
cat "$NOTES"
cat <<MD

## Components

| | Version |
|---|---|
| Wine | $(v WINE_VERSION), patches: $(ls patches | sed -E 's/^([0-9]+)-.*/\1/' | paste -sd, - | sed 's/,/, /g') |
| Wine Mono | $(v MONO_VERSION) |
| Wine Gecko | $(v GECKO_VERSION) |
| GStreamer | $(v GST_VERSION) |
| MoltenVK | $(v MVK_VERSION) |
| Other runtime libraries | from Gcenx's \`${runtime}\` |

## Archive

\`$(basename "$ARCHIVE")\`, built by \`wine/build.sh\` at $COMMIT.

- sha256: \`$(shasum -a 256 "$ARCHIVE" | cut -d' ' -f1)\`
- size: $(stat -f%z "$ARCHIVE") bytes

Runtime manifest entry:

\`\`\`json
$(cat "$COMPONENT")
\`\`\`
MD
