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
  DXMT for Direct3D 11, WineD3D for Direct3D 9, 32- and 64-bit), `tests/media/run.sh`, and for anything touching
  synchronization `tests/osync/run.sh`, `tests/sync/run.sh` and `tests/winetest/run.sh`.
- Never let Wine show a dialog: set `HKCU\Software\Wine\WineDbg ShowCrashDialog=dword:0` in a test prefix before
  running anything, and read crashes from the logs.

## Our own code only

Never copy code from other applications or their build scripts, whatever their license and even with attribution. Take the idea, then design and write the implementation
ourselves; combining the best ideas of several projects is fine, and ending up similar is fine. Patch headers
describe the problem and our fix, without naming where the idea was seen. The exceptions are libraries used as
libraries: Wine itself (the base our patches change, so the patches are LGPL) and, for now, the prebuilt runtime
libraries (Gcenx's dylibs, MoltenVK, GStreamer).
