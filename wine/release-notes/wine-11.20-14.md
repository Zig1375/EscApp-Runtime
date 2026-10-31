Wine 11.20 for EscApp, fourteenth build: a crash on a macOS thread always ends the process.

## What's new

- **A crash on a macOS thread can no longer leave the process hanging.** When a thread Wine didn't create (the
  Cocoa main thread, or one of Apple's) crashes, Wine writes its native stack into the log before ending the
  process. It named each address through macOS's library loader, which takes the loader's lock: a thread that
  crashed while holding that lock (in the middle of loading a library) would have waited for itself forever, and
  the game would have stayed open with no window instead of closing. The raw addresses are now written first and
  named afterwards, so the log has them even in that case.

## What to watch for

- A crash on a macOS thread lists its stack twice in the log: the bare addresses, then the same addresses with
  their libraries and function names.
