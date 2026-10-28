#!/usr/bin/env bash
# Builds EscApp's Wine: Wine 11.20 with the patches in patches/, for Apple Silicon through Rosetta
# (x86_64 with the new WoW64, so 32-bit games run without 32-bit macOS code), plus Wine Mono and Wine Gecko.
#   wine/build.sh                -> $OUT_DIR/escapp-wine-<wine>-<revision>.tar.xz
# The archive holds one folder, `wine/`, laid out as Wine installs itself (bin/ lib/ share/) plus licenses/;
# the runtime manifest installs it with `"from": "wine", "into": "wine"`.
#
# Why build Wine: DXMT (Direct3D 10/11 on Metal) needs winemac's `macdrv_functions` table, which stock Wine and
# Gcenx's current builds don't export (DXMT then logs "Failed to create metal view"). patches/0001 adds it.
# patches/0002 keeps 32-bit code under Rosetta 2 from crashing at random; patches/0003 adds osync, our in-process
# synchronization (docs/osync.md; on with WINEOSYNC=1). patches/0004 lets the player stretch a fixed-size game
# window (StretchWindows): Wine scales it through the emulated display mode, and the game sees no resize;
# patches/0005 lets such a window be dragged to another display in Retina mode. patches/0006 reports directory
# changes through FSEvents (macOS has no inotify), which the Epic Games Launcher needs to install a game.
# patches/0007 keeps named CNG keys across runs, which the Epic Games Launcher binds the player's sign-in to.
# patches/0008 keeps the last frame in the back buffer of an emulated display, which Chromium-based games redraw
# only in part. patches/0009 keeps the permissions given to files and folders, which the Epic Games Launcher reads
# back before it installs a game. patches/0010 and 0011 let Apple's D3DMetal run Direct3D 12 games (the player's
# own copy, added in EscApp): ntdll exports the __wine_unix_call its DLLs import, and with WINEMACSHAREDTSD=1 its
# macOS library can be called straight from Windows threads.
# Why Wine 11.20, a development release: it passes the same probes as 11.0 (DXVK, DXMT and WineD3D, 32- and
# 64-bit) and carries ten months of fixes the stable 11.0 branch doesn't get. Development releases change every
# two weeks and winemac's internals move with them (patches/0001 had to be redone between 11.0 and 11.20), so
# bump only after the probes pass on the new version; 12.0 (stable) is due in January 2027.
#
# Trust: every download is pinned by SHA-256. Wine, Mono and Gecko come from WineHQ. The x86_64 runtime libraries
# Wine loads (FreeType, GnuTLS, SDL2 and their dependencies) are taken from Gcenx's
# Wine 11.18 build, the same pin MaPC uses, since Homebrew on Apple Silicon only has arm64 ones. MoltenVK
# (Vulkan on Metal, for DXVK) comes from Khronos' own release. GStreamer (game videos: Media Foundation and
# DirectShow go through winegstreamer) comes from GStreamer's own macOS packages.
#
# Env:
#   CACHE_DIR  downloads (default wine/.cache/dl)    WORK_DIR  build tree (default wine/.cache/work)
#   OUT_DIR    default wine/dist                     REVISION  build number of this Wine version (default 1)
#   RESUME=1   continue the build tree in WORK_DIR (after fixing a failure) instead of reconfiguring
# Needs: Xcode, Rosetta, `brew install bison pkgconf freetype gnutls sdl2 mingw-w64`.
set -euo pipefail
cd "$(dirname "$0")"
REVISION="${REVISION:-1}"
CACHE="${CACHE_DIR:-.cache/dl}"; WORK="${WORK_DIR:-.cache/work}"; OUT="${OUT_DIR:-dist}"
mkdir -p "$CACHE" "$WORK" "$OUT"
CACHE="$(cd "$CACHE" && pwd)"; WORK="$(cd "$WORK" && pwd)"; OUT="$(cd "$OUT" && pwd)"

WINE_VERSION="11.20"
WINE_NAME="wine-$WINE_VERSION.tar.xz"
WINE_URL="https://dl.winehq.org/wine/source/11.x/$WINE_NAME"
WINE_SHA="eac8ca448f9c0087f963026b4dc276d1e2b7fbb64056edc093e59bbb7e56fe28"
# The versions Wine 11.20 asks for (MONO_VERSION and GECKO_VERSION in dlls/appwiz.cpl/addons.c); checked below.
MONO_VERSION="11.3.0"
MONO_NAME="wine-mono-$MONO_VERSION-x86.tar.xz"
MONO_URL="https://dl.winehq.org/wine/wine-mono/$MONO_VERSION/$MONO_NAME"
MONO_SHA="54a1b0111c3fe4b785eae688af94d27e64995707ad648b6fee8000381b80d298"
GECKO_VERSION="2.47.4"
GECKO_URL="https://dl.winehq.org/wine/wine-gecko/$GECKO_VERSION"
GECKO_X86_SHA="2cfc8d5c948602e21eff8a78613e1826f2d033df9672cace87fed56e8310afb6"
GECKO_X64_SHA="fd88fc7e537d058d7a8abf0c1ebc90c574892a466de86706a26d254710a82814"
RUNTIME_NAME="wine-staging-11.18-osx64.tar.xz"
RUNTIME_URL="https://github.com/Gcenx/macOS_Wine_builds/releases/download/11.18/$RUNTIME_NAME"
RUNTIME_SHA="b63704b91af269bc026a87f12bd297c4a50caaf570c322e600b6621ef918f127"
MVK_VERSION="1.4.2"
MVK_NAME="MoltenVK-macos-v$MVK_VERSION.tar"
MVK_URL="https://github.com/KhronosGroup/MoltenVK/releases/download/v$MVK_VERSION/MoltenVK-macos.tar"
MVK_SHA="f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e"
GST_VERSION="1.28.8"
GST_URL="https://gstreamer.freedesktop.org/data/pkg/osx/$GST_VERSION"
GST_NAME="gstreamer-1.0-$GST_VERSION-universal.pkg"
GST_SHA="d371ab04a7632713626836c2fc234e863cc98484c74c473eba99b0fbd9b3a3fe"
GST_DEV_NAME="gstreamer-1.0-devel-$GST_VERSION-universal.pkg"
GST_DEV_SHA="e6d380bc17be7eccf13e0154567d4718fc9321da822408c7ca0cd808b3d4a3f7"
# Parts of the GStreamer package that are used, all LGPL (the *-gpl parts are left out). effects has deinterlace,
# which winegstreamer's DirectShow path needs.
GST_PARTS="base-system-1.0 gstreamer-1.0-core gstreamer-1.0-playback gstreamer-1.0-codecs gstreamer-1.0-codecs-restricted gstreamer-1.0-libav gstreamer-1.0-effects"
# Plugins shipped: the elements winegstreamer creates by name (decodebin, audioconvert, audioresample,
# videoconvert, videoflip, deinterlace, capsfilter) and what decodebin needs to open the containers and codecs
# games use for videos and music. The rest of the package (encoders, streaming, effects) stays out.
GST_PLUGINS="coreelements app typefindfunctions playback audioconvert audioresample audiorate videoconvertscale
  videorate videofilter deinterlace volume pbtypes isomp4 matroska avi asf ogg flv wavparse mpegpsdemux mpegtsdemux
  id3demux apetag realmedia audioparsers videoparsersbad rawparse libav vorbis theora opus opusparse vpx dav1d flac
  mpg123 adpcmdec alaw mulaw jpeg"
# EscApp's oldest macOS (minMacOS in the runtime manifest).
MIN_MACOS="15.0"

command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "need mingw-w64 (brew install mingw-w64)" >&2; exit 1; }
BREW="$(brew --prefix)"
[ -x "$BREW/opt/bison/bin/bison" ] || { echo "need bison >= 3 (brew install bison)" >&2; exit 1; }
arch -x86_64 /usr/bin/true 2>/dev/null || { echo "need Rosetta (softwareupdate --install-rosetta)" >&2; exit 1; }

fetch() { # name url sha
  local f="$CACHE/$1"
  if [ ! -f "$f" ]; then curl -fL --retry 3 -sS -o "$f.part" "$2"; mv "$f.part" "$f"; fi
  [ "$(shasum -a 256 "$f" | cut -d' ' -f1)" = "$3" ] || { echo "checksum mismatch: $1 (moved aside)" >&2; mv "$f" "$f.bad"; exit 1; }
}
fetch "$WINE_NAME" "$WINE_URL" "$WINE_SHA"
fetch "$MONO_NAME" "$MONO_URL" "$MONO_SHA"
fetch "wine-gecko-$GECKO_VERSION-x86.tar.xz" "$GECKO_URL/wine-gecko-$GECKO_VERSION-x86.tar.xz" "$GECKO_X86_SHA"
fetch "wine-gecko-$GECKO_VERSION-x86_64.tar.xz" "$GECKO_URL/wine-gecko-$GECKO_VERSION-x86_64.tar.xz" "$GECKO_X64_SHA"
fetch "$RUNTIME_NAME" "$RUNTIME_URL" "$RUNTIME_SHA"
fetch "$MVK_NAME" "$MVK_URL" "$MVK_SHA"
fetch "$GST_NAME" "$GST_URL/$GST_NAME" "$GST_SHA"
fetch "$GST_DEV_NAME" "$GST_URL/$GST_DEV_NAME" "$GST_DEV_SHA"

# --- 1. Runtime libraries (x86_64) from the pinned Gcenx build -----------------------------------------
rm -rf "$WORK/runtime" && mkdir -p "$WORK/runtime"
tar -xJf "$CACHE/$RUNTIME_NAME" -C "$WORK/runtime"
RLIB="$(ls -d "$WORK"/runtime/*.app/Contents/Resources/wine/lib)"
# Khronos' MoltenVK replaces Gcenx's (1.4.0). With 1.4.0, 32-bit DXVK games can't create their device: WoW64's
# winevulkan imports host memory into Vulkan buffers and MoltenVK refuses ("Only external memory handle type
# ..._MTLBUFFER_BIT_EXT and ..._MTLHEAP_BIT_EXT are supported"). 1.4.1 (WhiskyWine's) and 1.4.2 accept it.
tar -xf "$CACHE/$MVK_NAME" -C "$WORK/runtime" MoltenVK/LICENSE MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib
rm -f "$RLIB/libMoltenVK.dylib"
lipo "$WORK/runtime/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" -thin x86_64 -output "$RLIB/libMoltenVK.dylib"
# Link-time copies carry `@rpath/libX.N.dylib` ids so configure records versioned sonames. No Vulkan loader:
# with one, winevulkan loads it instead of MoltenVK, and the loader finds no driver (Gcenx ships no ICD file), so
# DXVK failed with "Required Vulkan extension VK_KHR_surface not supported". configure then falls back to MoltenVK.
DEPS="$WORK/deps"; rm -rf "$DEPS" && mkdir -p "$DEPS/lib" "$DEPS/include"
for pair in libgnutls:libgnutls.30 libfreetype:libfreetype.6 libSDL2:libSDL2-2.0.0 libMoltenVK:libMoltenVK; do
  cp "$RLIB/${pair#*:}.dylib" "$DEPS/lib/${pair%%:*}.dylib"; chmod u+w "$DEPS/lib/${pair%%:*}.dylib"
  install_name_tool -id "@rpath/${pair#*:}.dylib" "$DEPS/lib/${pair%%:*}.dylib" 2>/dev/null
done
ln -s "$BREW/opt/freetype/include/freetype2" "$DEPS/include/freetype2"
ln -s "$BREW/opt/gnutls/include/gnutls" "$DEPS/include/gnutls"
ln -s "$BREW/include/SDL2" "$DEPS/include/SDL2"

# --- 1b. GStreamer (x86_64 slices) ---------------------------------------------------------------------
# The packages are installers; pkgutil unpacks their parts' payloads (pbzx, which tar can't read) without
# installing anything. Headers come from the devel package, libraries from the runtime one (the devel one's are
# the same plus static copies).
GST="$WORK/gst"; rm -rf "$GST" && mkdir -p "$GST/pkg" "$GST/dev" "$GST/rt"
pkgutil --expand-full "$CACHE/$GST_NAME" "$GST/pkg/rt"
pkgutil --expand-full "$CACHE/$GST_DEV_NAME" "$GST/pkg/dev"
for part in $GST_PARTS; do
  ditto "$GST/pkg/rt/$part-$GST_VERSION-universal.pkg/Payload" "$GST/rt"
  d="$GST/pkg/dev/$part-devel-$GST_VERSION-universal.pkg/Payload"
  for sub in include lib/glib-2.0/include share/licenses; do [ ! -d "$d/$sub" ] || ditto "$d/$sub" "$GST/dev/$sub"; done
done
rm -rf "$GST/pkg"
[ -f "$GST/dev/include/gstreamer-1.0/gst/gst.h" ] && [ -f "$GST/dev/lib/glib-2.0/include/x86_64/glibconfig.h" ] \
  || { echo "GStreamer headers missing from $GST_DEV_NAME" >&2; exit 1; }
# Everything the shipped plugins, the plugin scanner and winegstreamer load, by the @rpath names they use.
gst_closure() {
  local todo="$*" seen=" " f dep
  while [ -n "$todo" ]; do
    set -- $todo; f="$1"; shift; todo="$*"
    [ -f "$f" ] || { echo "GStreamer: missing $f" >&2; exit 1; }
    for dep in $(otool -arch x86_64 -L "$f" | awk 'NR > 1 && $1 ~ /^@rpath\// { sub("@rpath/", "", $1); print $1 }'); do
      case "$seen" in *" $dep "*) continue ;; esac
      [ "$dep" = "$(basename "$f")" ] && continue
      seen="$seen$dep "; todo="$todo $GST/rt/lib/$dep"
    done
  done
  echo $seen
}
GST_LINKED="gstreamer-1.0 gstvideo-1.0 gstaudio-1.0 gsttag-1.0 gobject-2.0 glib-2.0"
roots="$GST/rt/libexec/gstreamer-1.0/gst-plugin-scanner"
for p in $GST_PLUGINS; do roots="$roots $GST/rt/lib/gstreamer-1.0/libgst$p.dylib"; done
for l in $GST_LINKED; do roots="$roots $GST/rt/lib/lib$l.0.dylib"; done
GST_LIBS="$(gst_closure $roots)"
mkdir -p "$GST/link"
for l in $GST_LINKED; do lipo "$GST/rt/lib/lib$l.0.dylib" -thin x86_64 -output "$GST/link/lib$l.dylib"; done

# --- 2. Wine -------------------------------------------------------------------------------------------
SRC="$WORK/wine-$WINE_VERSION"
CLEAN_ENV=(/usr/bin/env -i HOME="$HOME" PATH="$BREW/opt/bison/bin:$BREW/bin:/usr/bin:/bin:/usr/sbin:/sbin" MACOSX_DEPLOYMENT_TARGET="$MIN_MACOS")
if [ "${RESUME:-0}" != 1 ] || [ ! -f "$WORK/build/Makefile" ]; then
  rm -rf "$SRC" "$WORK/build"
  tar -xJf "$CACHE/$WINE_NAME" -C "$WORK"
  grep -q "define MONO_VERSION \"$MONO_VERSION\"" "$SRC/dlls/appwiz.cpl/addons.c" \
    && grep -q "define GECKO_VERSION \"$GECKO_VERSION\"" "$SRC/dlls/appwiz.cpl/addons.c" \
    || { echo "Wine $WINE_VERSION wants other Mono/Gecko versions (dlls/appwiz.cpl/addons.c)" >&2; exit 1; }
  for p in patches/*.patch; do patch -d "$SRC" -p1 --forward --quiet < "$p" || { echo "patch failed: $p" >&2; exit 1; }; done
  mkdir -p "$WORK/build"
  # No -g: debug info would double the size of the PE DLLs and nobody here reads it.
  ( cd "$WORK/build" && "${CLEAN_ENV[@]}" \
      CC="clang -arch x86_64" CXX="clang++ -arch x86_64" OBJC="clang -arch x86_64" \
      CFLAGS="-O2" CROSSCFLAGS="-O2" \
      FREETYPE_CFLAGS="-I$DEPS/include/freetype2" FREETYPE_LIBS="-L$DEPS/lib -lfreetype" \
      GNUTLS_CFLAGS="-I$DEPS/include" GNUTLS_LIBS="-L$DEPS/lib -lgnutls" \
      SDL2_CFLAGS="-I$DEPS/include/SDL2 -D_THREAD_SAFE" SDL2_LIBS="-L$DEPS/lib -lSDL2" \
      GSTREAMER_CFLAGS="-I$GST/dev/include/gstreamer-1.0 -I$GST/dev/include/glib-2.0 -I$GST/dev/lib/glib-2.0/include" \
      GSTREAMER_LIBS="-L$GST/link $(for l in $GST_LINKED; do printf -- '-l%s ' "$l"; done)" \
      LDFLAGS="-L$DEPS/lib -Wl,-rpath,@loader_path/../../" \
      ac_cv_func_pipe2=no \
      "$SRC/configure" --build=x86_64-apple-darwin --host=x86_64-apple-darwin --prefix="$WORK/stage" \
        --enable-archs=i386,x86_64 --disable-tests --disable-win16 \
        --with-mingw --with-freetype --with-gnutls --with-sdl --with-vulkan --with-coreaudio --with-opencl --with-unwind --with-gstreamer \
        --without-x --without-wayland --without-alsa --without-oss --without-udev --without-v4l2 --without-capi \
        --without-inotify --without-pulse --without-dbus --without-netapi --without-krb5 --without-gssapi \
        --without-fontconfig --without-sane --without-gphoto --without-ffmpeg --without-usb \
        > configure.out 2>&1 ) || { echo "configure failed; see $WORK/build/configure.out" >&2; exit 1; }
fi
# Build-time tools (sfnt2fon links FreeType) get the same `@loader_path/../../` rpath as Wine's modules;
# from build/tools/<tool>/ that is the build root, so the runtime libraries go there too.
cp -a "$RLIB"/*.dylib "$WORK/build/"
rm -rf "$WORK/stage"
( cd "$WORK/build" && "${CLEAN_ENV[@]}" make -j"${JOBS:-$(sysctl -n hw.ncpu)}" > make.out 2>&1 \
  && "${CLEAN_ENV[@]}" make install > install.out 2>&1 ) \
  || { echo "Wine build failed; see $WORK/build/make.out and install.out" >&2; exit 1; }

# The SDK can be newer than the oldest macOS EscApp supports. Functions only a newer macOS has are weak imports,
# NULL on older systems: with the macOS 27 SDK configure finds pipe2, and Wine then crashes at start on macOS 26
# (ntdll server_pipe -> pipe2 -> address 0; MaPC engine 2.0.1). Hence ac_cv_func_pipe2=no above, and this check:
# weak imports in Wine's modules must be ones its source guards with an availability check.
GUARDED_WEAK='^(ulock_wait2|availability_version_check|os_sync_wait_on_address|os_sync_wait_on_address_with_timeout|os_sync_wake_by_address_any|clCreateImage2D|clCreateImage3D|clEnqueueBarrier|clEnqueueMarker|clEnqueueWaitForEvents|clUnloadCompiler)$'
for bin in "$WORK"/stage/bin/wine "$WORK"/stage/bin/wineserver "$WORK"/stage/lib/wine/x86_64-unix/*.so; do
  [ -L "$bin" ] && continue
  unguarded="$(dyld_info -imports "$bin" 2>/dev/null | grep weak-import | grep -oE '_[A-Za-z0-9_]+' | sed 's/^_*//' \
    | grep -vE "$GUARDED_WEAK" || true)"
  [ -z "$unguarded" ] || { echo "$(basename "$bin") weak-imports $unguarded: check it is guarded, or pin it off in configure" >&2; exit 1; }
done
grep -q '^#define SONAME_LIBVULKAN "libMoltenVK.dylib"' "$WORK/build/include/config.h" \
  || { echo "winevulkan isn't linked to MoltenVK; DXVK would find no Vulkan driver" >&2; exit 1; }
# Checks read all their input (grep >/dev/null, not -q): with pipefail, grep -q quitting early sent objdump a
# SIGPIPE and failed the ntdll check of build 12 although the export was there.
nm -gU "$WORK/stage/lib/wine/x86_64-unix/winemac.so" | grep " _macdrv_functions$" >/dev/null \
  || { echo "winemac.so doesn't export macdrv_functions; DXMT can't create its Metal view" >&2; exit 1; }
x86_64-w64-mingw32-objdump -p "$WORK/stage/lib/wine/x86_64-windows/ntdll.dll" | grep -E " __wine_unix_call$" >/dev/null \
  || { echo "ntdll.dll doesn't export __wine_unix_call; D3DMetal's d3d12.dll can't initialize" >&2; exit 1; }
[ -f "$WORK/stage/lib/wine/x86_64-unix/winegstreamer.so" ] \
  || { echo "winegstreamer.so wasn't built; game videos wouldn't play (see configure.out)" >&2; exit 1; }

# --- 3. Assemble ---------------------------------------------------------------------------------------
PKG="$WORK/package"; rm -rf "$PKG" && mkdir -p "$PKG"
cp -R "$WORK/stage" "$PKG/wine"
W="$PKG/wine"
rm -rf "$W/include" "$W/share/man" "$W/share/applications"      # build-time only
find "$W/lib/wine" -name "*.a" -delete
cp -a "$RLIB"/*.dylib "$W/lib/"
# Wine dlopens these by leaf name, which dyld resolves through the calling module's rpaths. Wine adds
# `@loader_path/`; LDFLAGS above adds `@loader_path/../../` (= wine/lib). A missing rpath would only show up later
# as broken fonts, TLS or Vulkan, so check every module that names one of them (which modules do moves between
# Wine versions: Vulkan is in winevulkan.so in 11.0 and in win32u.so in 11.20).
loaders=0
for so in "$W"/lib/wine/x86_64-unix/*.so; do
  strings -a "$so" | grep -E 'lib(gnutls\.30|freetype\.6|SDL2-2\.0\.0|MoltenVK)\.dylib' >/dev/null || continue
  loaders=$((loaders + 1))
  otool -l "$so" | grep "path @loader_path/../../ " >/dev/null || { echo "no rpath in $(basename "$so")" >&2; exit 1; }
done
[ "$loaders" -ge 4 ] || { echo "only $loaders modules load the runtime libraries; did their names change?" >&2; exit 1; }
for lib in libfreetype.6.dylib libgnutls.30.dylib libSDL2-2.0.0.dylib libMoltenVK.dylib; do
  [ -f "$W/lib/$lib" ] || { echo "runtime library missing: $lib" >&2; exit 1; }
done
otool -l "$W/lib/wine/x86_64-unix/winegstreamer.so" | grep "path @loader_path/../../ " >/dev/null \
  || { echo "no rpath in winegstreamer.so" >&2; exit 1; }

# GStreamer next to them: libraries in lib/, plugins in lib/gstreamer-1.0/, the plugin scanner in
# libexec/gstreamer-1.0/. GStreamer finds both from where libgstreamer is, and their own rpaths
# (@loader_path/.., @loader_path/../../lib) point at lib/. A few libraries (libintl, zlib, bzip2) are in Gcenx's set
# too; one copy each must serve both, so Gcenx's stays, provided it has every symbol GStreamer takes from it.
mkdir -p "$W/lib/gstreamer-1.0" "$W/libexec/gstreamer-1.0"
for p in $GST_PLUGINS; do lipo "$GST/rt/lib/gstreamer-1.0/libgst$p.dylib" -thin x86_64 -output "$W/lib/gstreamer-1.0/libgst$p.dylib"; done
lipo "$GST/rt/libexec/gstreamer-1.0/gst-plugin-scanner" -thin x86_64 -output "$W/libexec/gstreamer-1.0/gst-plugin-scanner"
gst_users="$W/lib/gstreamer-1.0/*.dylib $W/libexec/gstreamer-1.0/gst-plugin-scanner"
for l in $GST_LIBS; do gst_users="$gst_users $GST/rt/lib/$l"; done
for l in $GST_LIBS; do
  if [ ! -e "$W/lib/$l" ]; then lipo "$GST/rt/lib/$l" -thin x86_64 -output "$W/lib/$l"; continue; fi
  from="$(echo "$l" | sed -E 's/(\.[0-9]+)*\.dylib$//')"
  missing="$(comm -23 \
    <(for f in $gst_users; do nm -m -arch x86_64 "$f" 2>/dev/null; done | grep "(undefined).*(from $from)" \
        | sed -E 's/.* (_[^ ]+) \(from.*/\1/' | sort -u) \
    <(nm -gU -arch x86_64 "$W/lib/$l" | awk '{ print $3 }' | sort -u))"
  [ -z "$missing" ] || { echo "Gcenx's $l lacks symbols GStreamer needs:" $missing >&2; exit 1; }
done
# No library reference or rpath may point back into this build machine's folders. (Wine's loader keeps the
# configure prefix as text, only to compute relative paths; that is harmless.)
for bin in "$W"/bin/wine "$W"/bin/wineserver "$W"/lib/wine/x86_64-unix/*; do
  [ -L "$bin" ] && continue
  if { otool -L "$bin" | tail -n +2; otool -l "$bin" | grep -A2 LC_RPATH || true; } | grep -F "$WORK" >/dev/null; then
    echo "build path leaked into $bin" >&2; exit 1
  fi
done

# Wine Mono and Gecko where Wine looks for them first (share/wine/{mono,gecko}), so a new bottle installs them
# without a download prompt.
mkdir -p "$W/share/wine/mono" "$W/share/wine/gecko"
tar -xJf "$CACHE/$MONO_NAME" -C "$W/share/wine/mono"
tar -xJf "$CACHE/wine-gecko-$GECKO_VERSION-x86.tar.xz" -C "$W/share/wine/gecko"
tar -xJf "$CACHE/wine-gecko-$GECKO_VERSION-x86_64.tar.xz" -C "$W/share/wine/gecko"

# Licenses, and how this build was made: the LGPL's "scripts used to control compilation" ship with the binaries.
L="$W/licenses"; mkdir -p "$L/wine-libs" "$L/runtime-libs" "$L/build"
cp "$SRC/COPYING.LIB" "$L/Wine-COPYING.LIB.txt"; cp "$SRC/LICENSE" "$L/Wine-LICENSE.txt"
# Libraries Wine builds into its own DLLs (libs/ in Wine's source).
for f in "$SRC"/libs/*/{COPYING,LICENSE,COPYRIGHT}*; do
  [ -f "$f" ] && cp "$f" "$L/wine-libs/$(basename "$(dirname "$f")")-$(basename "$f").txt"
done
cp licenses/* "$L/runtime-libs/"
cp "$WORK/runtime/MoltenVK/LICENSE" "$L/runtime-libs/MoltenVK-LICENSE.txt"
mkdir -p "$L/gstreamer"; cp -R "$GST/dev/share/licenses/" "$L/gstreamer/"
cp build.sh "$L/build/"; cp -R patches "$L/build/"
cat > "$L/NOTICE.txt" <<TXT
Wine for EscApp, Wine $WINE_VERSION build $REVISION.

Wine $WINE_VERSION (LGPL-2.1-or-later): $WINE_URL (sha256 $WINE_SHA),
  with the patches in build/patches applied, built by build/build.sh. Wine's own licenses: Wine-*.txt;
  libraries built into Wine's DLLs: wine-libs/.
Wine Mono $MONO_VERSION (share/wine/mono; MIT, LGPL, GPL with exceptions, MS-PL and others, listed in its source):
  $MONO_URL; source: https://dl.winehq.org/wine/wine-mono/$MONO_VERSION/wine-mono-$MONO_VERSION-src.tar.xz
Wine Gecko $GECKO_VERSION (share/wine/gecko; MPL-2.0): $GECKO_URL/
  source: $GECKO_URL/wine-gecko-$GECKO_VERSION-src.tar.xz
MoltenVK $MVK_VERSION (lib/libMoltenVK.dylib; Apache-2.0): the x86_64 slice of Khronos' release,
  $MVK_URL (sha256 $MVK_SHA).
Other runtime libraries in lib/ (FreeType, GnuTLS and its dependencies, SDL2, the Vulkan loader and others):
  unmodified from Gcenx's Wine 11.18 build, $RUNTIME_URL (sha256 $RUNTIME_SHA), built with MacPorts.
  Their licenses: runtime-libs/.
GStreamer $GST_VERSION (lib/gstreamer-1.0, libexec/gstreamer-1.0 and its libraries in lib/: GStreamer, GLib,
  FFmpeg built without GPL parts, codec libraries; LGPL-2.1-or-later and more liberal licenses): x86_64 slices,
  unmodified, of GStreamer's macOS package $GST_URL/$GST_NAME (sha256 $GST_SHA).
  Their licenses: gstreamer/. Sources: https://gstreamer.freedesktop.org/src/ and
  https://gitlab.freedesktop.org/gstreamer/cerbero (tag $GST_VERSION, which builds the package).
TXT
cat > "$W/BUILD.json" <<JSON
{ "wine": "$WINE_VERSION", "revision": $REVISION, "mono": "$MONO_VERSION", "gecko": "$GECKO_VERSION", "gstreamer": "$GST_VERSION", "minMacOS": "$MIN_MACOS" }
JSON

# --- 4. Package ----------------------------------------------------------------------------------------
ARCHIVE="$OUT/escapp-wine-$WINE_VERSION-$REVISION.tar.xz"
rm -f "$ARCHIVE"
XZ_OPT="-T0 -9" tar -cJf "$ARCHIVE" -C "$PKG" wine
SHA="$(shasum -a 256 "$ARCHIVE" | cut -d' ' -f1)"; SIZE="$(stat -f%z "$ARCHIVE")"
echo "archive: $ARCHIVE ($((SIZE / 1048576)) MB)"
echo "sha256:  $SHA"
echo "size:    $SIZE"
cat > "$OUT/escapp-wine-$WINE_VERSION-$REVISION.component.json" <<JSON
{
  "kind": "wine",
  "version": "$WINE_VERSION (EscApp build $REVISION)",
  "url": "${BASE_URL:-file://$OUT}/escapp-wine-$WINE_VERSION-$REVISION.tar.xz",
  "sha256": "$SHA",
  "size": $SIZE,
  "license": "LGPL-2.1-or-later",
  "from": "wine",
  "into": "wine"
}
JSON
echo "manifest component: $OUT/escapp-wine-$WINE_VERSION-$REVISION.component.json"
