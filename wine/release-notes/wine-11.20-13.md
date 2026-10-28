Wine 11.20 for EscApp, thirteenth build: Steam no longer hangs at start now and then.

## What's new

- **Steam no longer hangs at start.** Steam's built-in browser could stop for good while starting: a thread was
  paused while it held macOS's library loader lock, and the thread that paused it waited for a lock held by one
  that needed the loader. Steam then never came up and never started the game it was asked to. Wine no longer
  pauses a thread while it loads a library.

## What to watch for

- Pausing a thread (debuggers, crash reporters, Chromium's profiler) waits until any library load it is doing
  finishes, a few milliseconds at most.
