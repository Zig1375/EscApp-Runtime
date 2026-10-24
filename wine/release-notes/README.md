# Release notes

One file per release, named after its tag: `wine-<wine version>-<revision>.md` (for example `wine-11.20-2.md`).
Commit it together with the change it describes, before running the Wine workflow: the workflow refuses to
build a revision that has no notes, and puts the file at the top of the release, followed by the component
versions, the archive's SHA-256 and size, and the runtime manifest entry, which it adds itself.

Write what changed for players and what to watch for, not how the build works; `wine/build.sh` explains that.
