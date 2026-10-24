#!/usr/bin/env bash
# Builds a release on this Mac and publishes it on GitHub, as the Wine workflow does, without Actions minutes:
#   wine/release-local.sh <revision>
# The tag is wine-<wine version>-<revision>, pointing at HEAD, which must be clean and pushed; the release notes come
# from wine/release-notes/<tag>.md like the workflow's. Run the checks (README) on the build before publishing: the
# script stops after building and asks.
set -euo pipefail
cd "$(dirname "$0")"
REVISION="${1:?usage: wine/release-local.sh <revision>}"
[[ "$REVISION" =~ ^[0-9]+$ ]] || { echo "revision must be a number" >&2; exit 1; }
REPO="$(gh repo view --json nameWithOwner -q .nameWithOwner)"
WINE_VERSION="$(sed -n 's/^WINE_VERSION="\(.*\)"$/\1/p' build.sh)"
TAG="wine-$WINE_VERSION-$REVISION"

[ -f "release-notes/$TAG.md" ] || { echo "Add wine/release-notes/$TAG.md and commit it first." >&2; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "Commit your changes first: the release must match a commit." >&2; exit 1; }
git fetch -q origin
HEAD_SHA="$(git rev-parse HEAD)"
git merge-base --is-ancestor "$HEAD_SHA" "origin/$(git rev-parse --abbrev-ref HEAD)" \
  || { echo "Push $HEAD_SHA first: the release points at it." >&2; exit 1; }
! gh release view "$TAG" -R "$REPO" >/dev/null 2>&1 || { echo "$TAG already exists." >&2; exit 1; }

REVISION="$REVISION" BASE_URL="https://github.com/$REPO/releases/download/$TAG" ./build.sh
ARCHIVE="dist/escapp-wine-$WINE_VERSION-$REVISION.tar.xz"
COMPONENT="dist/escapp-wine-$WINE_VERSION-$REVISION.component.json"
read -r -p "Built $ARCHIVE. Publish it as $TAG? [y/N] " answer
[ "$answer" = y ] || { echo "Not published."; exit 0; }

./release-notes.sh "$TAG" "$ARCHIVE" "$COMPONENT" "$HEAD_SHA" > "dist/$TAG-notes.md"
gh release create "$TAG" "$ARCHIVE" "$COMPONENT" -R "$REPO" --target "$HEAD_SHA" \
  --title "Wine for EscApp: $TAG" --notes-file "dist/$TAG-notes.md"
echo "published $TAG; manifest entry: $COMPONENT"
