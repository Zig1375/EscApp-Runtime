# EscApp-Runtime

Builds the Wine that EscApp installs (`wine/build.sh`) and publishes it as GitHub releases through the Wine
workflow (`.github/workflows/wine.yml`). See `README.md` for what goes into the build.

## Release notes: every change goes into one

Each release is described by `wine/release-notes/<tag>.md` (`wine-<wine version>-<revision>.md`); the workflow
refuses a revision without one. Keep it current as you work:

- Every change to what the build produces (patches, components, versions, build options) adds a line to the
  notes of the release it will ship in, in the same commit. Say what changes for players and what to watch for.
- The release in progress is the newest notes file that has not been pushed yet. While commits stay local, keep
  adding to that file.
- Once a notes file has been pushed, its release is spoken for (the workflow may already have built it): never
  edit it for later changes. Create the next revision's file (`wine-11.20-2.md` → `wine-11.20-3.md`) and write
  the new changes there. When the Wine version changes, the revision starts again at 1.
- Find out with `git log origin/main -- wine/release-notes/` which notes files are already pushed.

## Working here

- Commit, don't push: GitHub minutes are scarce. Push or run the workflow only when the user asks.
- Check a build locally before it is released: the graphics probes (EscApp's `Tests/Fixtures/probes`, DXVK and
  DXMT for Direct3D 11, WineD3D for Direct3D 9, 32- and 64-bit), `tests/media/run.sh`, `tests/change/run.sh`, `tests/acl/run.sh`, and
  for anything touching synchronization `tests/osync/run.sh`, `tests/sync/run.sh` and `tests/winetest/run.sh`.
- Clean up after every task: Wine prefixes, test homes, unpacked packages and build trees take gigabytes each and
  have filled the disk before. Put them under one temporary folder per task (TMP_DIR for the tests), delete them as
  soon as their checks are done and never leave them for later, even if the task is interrupted. Stop the task's
  Wine processes (`wineserver -k` per prefix) first. Keep only `wine/.cache/dl` (downloads), the newest archives in
  `wine/dist` and what the next step needs, and say what was kept and why; never delete another session's folders.
- Tests never put windows on the user's screen: the headless suites run Wine without its macOS display driver
  (`WINEDLLOVERRIDES=...;winemac.drv=d`, so even wineboot's "updating configuration" window stays invisible), and
  their output goes to files, never through pipes. The graphics probes need the real display: run them only when the
  user isn't using the Mac, or ask first.
- Never let Wine show a dialog: set `HKCU\Software\Wine\WineDbg ShowCrashDialog=dword:0` in a test prefix before
  running anything, and read crashes from the logs.

## Chasing a crash on a macOS thread

A fault on a thread Wine didn't create (the Cocoa main thread) is usually `objc_release` on an object that was
already freed; patch 0014 prints the native stack into the process log, and that is where to start.

- Freed memory usually still looks like a live object, so an over-release kills the process only now and then. Run
  the same program with `MallocScribble=1` and it becomes reproducible nearly every start (the Steam client died
  five times out of five that way, and not once in eight runs after patch 0016).
- To try a change without touching the user's installed runtime: rebuild one DLL (`make
  dlls/winemac.drv/winemac.so -j$(sysctl -n hw.ncpu)` in `wine/.cache/work/build`), clone the installed tree with
  `cp -c -R`, drop the new `.so` into `<clone>/lib/wine/x86_64-unix/`, and run from the clone.
- Name the culprit by logging `__builtin_return_address(0)` at the suspect call and symbolizing it with `dladdr`,
  the way `log_native_frame` does in patch 0014. An address in an Apple framework (AppKit sending `clearDrawable`
  from `-[NSOpenGLContext dealloc]`) says the object was freed while macOS still held it.
- Never read a pointer that may be dead without checking: `mincore` answers for a mapped page even when it is not
  readable (`PROT_NONE`), so a guard based on it still faults. Print raw pointers, or check with `mach_vm_region`.

## Our own code only

Never copy code from other applications or their build scripts, whatever their license and even with attribution. Take the idea, then design and write the implementation
ourselves; combining the best ideas of several projects is fine, and ending up similar is fine. Patch headers
describe the problem and our fix, without naming where the idea was seen. The exceptions are libraries used as
libraries: Wine itself (the base our patches change, so the patches are LGPL) and, for now, the prebuilt runtime
libraries (Gcenx's dylibs, MoltenVK, GStreamer).
