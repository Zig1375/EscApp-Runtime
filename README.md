# EscApp-Runtime

Builds the Wine that [EscApp](https://github.com/Zig1375) uses to run Windows games on Apple Silicon Macs, and
publishes it on this repository's [Releases](../../releases). EscApp's runtime manifest pins each release archive by
URL, SHA-256 and size, so the app only installs files built here.

## Wine

`wine/build.sh` builds [Wine](https://www.winehq.org) 11.20 (development release) for x86_64 (run through Rosetta) with the new
WoW64, so 32-bit games need no 32-bit macOS code, and adds Wine Mono 11.3.0 and Wine Gecko 2.47.4 (the versions
Wine 11.20 asks for). The archive `escapp-wine-<wine>-<revision>.tar.xz` holds one folder, `wine/`, laid out as Wine
installs itself (`bin/`, `lib/`, `share/`), with every license and this build script in `wine/licenses/`.

These patches are applied (`wine/patches/`):

- `0001` exports winemac's `macdrv_functions` table, which [DXMT](https://github.com/3Shain/dxmt)
  (Direct3D 10/11 on Metal) uses to create its Metal view. Without it DXMT fails with "Failed to create metal view".
- `0002` makes wow64cpu switch between 32- and 64-bit code with far returns instead of far jumps. Under Rosetta 2
  a far jump that changes the code segment can be hit by a signal in a state Wine can't resume, and with stock Wine
  32-bit games crash at random in wow64cpu.dll (about one start in four).
- `0003` adds osync, our in-process synchronization for macOS (`docs/osync.md`): events, semaphores and mutexes
  live in memory shared with wineserver and are waited on and signalled inside the process with macOS's public
  `os_sync_wait_on_address`. Off unless wineserver starts with `WINEOSYNC=1`. Against stock Wine's wineserver path
  it is 9 to 26 times faster on our benchmarks.
- `0004` lets the player stretch a game window that can't be resized (with `StretchWindows` next to
  `EmulateModeset`): the window keeps its size for the game, and Wine scales the picture and the mouse by shrinking
  the emulated display mode. It also fixes the Mac driver bugs this exposed (OpenGL drawn at a stale size or only
  a corner of it, a Direct3D 9 hang, white bars around an emulated full screen, DXMT views not following their
  window).
- `0005` lets a stretched or fixed-size window move to another display: macOS shrank such a window to fit a smaller
  screen, or returned its frame a pixel off in Retina mode, and Wine took that for the player stretching it and put
  the window back on its old display.
- `0006` reports directory changes (ReadDirectoryChangesW, FindFirstChangeNotification) through FSEvents: Wine
  only knows inotify, which macOS doesn't have, so a program waiting for a file to appear waited forever. The Epic
  Games Launcher waits that way for its helper when it installs a game. `tests/change/run.sh` checks it.
- `0007` keeps named CNG keys (NCryptCreatePersistedKey, NCryptOpenKey, NCryptDeleteKey) across runs, in the
  prefix's registry: Wine kept them only in memory. The Epic Games Launcher binds the player's sign-in to such a
  key, made a new one at every start and was signed out each time. `tests/ncrypt/run.sh` checks it.
- `0008` keeps the last frame in an OpenGL window's back buffer when Wine emulates the display mode (EmulateModeset):
  the swap exchanged the buffers, and Chromium-based games, which redraw only what changed, flickered wherever
  they had drawn only once.
- `0009` keeps the permissions a program gives a file or folder (SetNamedSecurityInfo) in an extended attribute
  next to the Unix mode: Wine turned them into the mode and built them again from it, so a grant was gone at the
  next open. The Epic Games Launcher grants the players' group access to a game's folder from its helper, looks
  for that grant and stopped the install with DP-07 without it. `tests/acl/run.sh` checks it.
- `0010` exports `__wine_unix_call` from ntdll, the function Apple's D3DMetal DLLs (`d3d12`, `dxgi`, `d3d11`,
  `d3d10`) import for their unix calls. Wine exports only the dispatcher pointer, so the import stayed unresolved,
  `d3d12.dll` failed to initialize and no Direct3D 12 game started ("d3d12.dll failed to initialize, aborting").
- `0011` lets D3DMetal's macOS library run from the threads its Windows DLLs start: with `WINEMACSHAREDTSD=1`
  Windows code keeps the macOS thread data as its `%gs` base while it calls into that library, which reads the
  thread data there; without it `pthread_setname_np` saw a NULL `pthread_self()` and every Direct3D 12 game
  crashed at start. Nothing changes without the setting, which EscApp turns on only for D3DMetal.
- `0012` makes the first XInput call return before the controller update thread touches any window. The thread
  registered its window class and message window first, and on macOS the class registration waited for the window
  lock whose holder waited for the calling thread: Among Us (Unity, Rewired) hung for good on a black window right
  after its intro, on every graphics backend. A controller plugged in meanwhile is still found by the rescan.
- `0013` keeps winemac's Cocoa start-up handshake alive until both threads are done with it. The thread that
  started the macOS driver waited five seconds for the main thread to become the Cocoa application, with the
  handshake on its own stack: on a busy Mac (a game under Rosetta while Steam and other Wine processes start) the
  main thread came later, the process lost its display, and the main thread then crashed on the freed handshake in
  `objc_release`. It now lives on the heap, and the wait is a minute.
- `0014` logs the native stack, with libraries and function names, when a thread Wine didn't create faults: a
  fault there (macOS's main thread, or one of Apple's) ended the process with only an address in the log. Among Us
  died in `objc_release` on such a thread, and Steam's helpers and Banished died the same way, with nothing saying
  whose object was released.
- `0015` keeps the suspend signal out while a Unix library is loaded or unloaded by name, so no thread can be
  paused holding the macOS loader's lock. Steam's window could stop for good at start: Chromium's stack sampler
  suspended the main thread inside `dlopen`, a thread holding `virtual_mutex` waited for the loader's lock, and
  the sampler waited for that thread. The library's own init still runs with signals open.

Every download is pinned by SHA-256. Wine, Mono and Gecko come from WineHQ; the x86_64 libraries Wine loads
(FreeType, GnuTLS, SDL2 and their dependencies) come unmodified from
[Gcenx's Wine 11.18 build](https://github.com/Gcenx/macOS_Wine_builds/releases/tag/11.18), and MoltenVK 1.4.2 from
[Khronos' release](https://github.com/KhronosGroup/MoltenVK/releases/tag/v1.4.2).

Game videos (Media Foundation and DirectShow, through winegstreamer) play with
[GStreamer](https://gstreamer.freedesktop.org) 1.28.8: the x86_64 slices of the LGPL parts of GStreamer's own macOS
package, cut down to the plugins winegstreamer and common video formats need (FFmpeg's decoders included, built
without GPL code). They sit in `lib/`, `lib/gstreamer-1.0/` and `libexec/gstreamer-1.0/`.

Build locally (Xcode, Rosetta, and `brew install bison pkgconf freetype gnutls sdl2 mingw-w64`):

```bash
wine/build.sh
```

Check that a build plays videos (needs `brew install mingw-w64 ffmpeg`): it plays H.264/AAC, WMV, MPEG-1 and
VP8/Opus clips through Media Foundation and DirectShow, 32- and 64-bit, and prints one `PROBE … OK` line each.

```bash
tests/media/run.sh wine/.cache/work/package/wine
```

Synchronization (osync) has three levels of tests:

```bash
tests/osync/run.sh
```

runs osync's algorithms natively, without Wine (arm64, x86_64 through Rosetta, ThreadSanitizer, AddressSanitizer +
UBSan): semantics, hand-off, timeouts racing signals, WaitAll, PulseEvent, cross-process use, processes killed
mid-wait or holding a lock, a check against a reference model, and multi-thread and multi-process stress.

```bash
tests/sync/run.sh wine/.cache/work/package/wine
```

runs the same semantics through the Windows API in Wine, without and with osync (32- and 64-bit), times them, and
checks that seeded random sequences of calls give the same results with osync as with wineserver.

```bash
tests/winetest/run.sh wine/.cache/work/package/wine wine/.cache/work/wine-11.20 wine/.cache/work/build
```

builds Wine's own conformance tests for synchronization (kernel32:sync, ntdll:sync) and checks that osync adds no
failures to wineserver's. All three need mingw-w64 where they build Windows programs.

Release: commit the release's description as `wine/release-notes/wine-<wine>-<revision>.md` (see the README there),
then Actions → Wine → Run workflow, with that revision number. Without the file the run stops before building. Or
build and publish from this Mac, without Actions minutes (it asks before publishing; run the checks first):

```bash
wine/release-local.sh <revision>
```

Either way the release notes give the SHA-256, the size and the manifest entry.

## License

The build scripts are under the Apache License 2.0 (`LICENSE`). The patches change Wine and are under Wine's
license, the GNU LGPL 2.1 or later. Each built component keeps its own license; see `wine/licenses/` in a release.
