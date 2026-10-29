Wine 11.20 for EscApp, thirteenth build: Steam comes up more reliably now.

## What's new

- **Steam no longer hangs at start.** Steam's built-in browser could stop for good while starting: a thread was
  paused while it held macOS's library loader lock, and the thread that paused it waited for a lock held by one
  that needed the loader. Steam then never came up and never started the game it was asked to. Wine no longer
  pauses a thread while it loads a library.

- **Steam no longer dies while it opens and closes its windows.** Steam was killed by a crash in AppKit's own
  cleanup of an OpenGL context. AppKit deallocates a context by asking it to detach from its view, and Wine
  answered by handing that work to the main thread in a block that held the context — one that was already being
  torn down. The block was left with freed memory, and releasing it crashed the process, taking the store client
  with it. Wine now does that detach on the thread that is deallocating the context, which is what AppKit is
  asking for. Steam's helpers and games died the same way in our older logs (Banished and Among Us in
  objc_release, Steam's assistant processes in the 28 October log).

## What to watch for

- Pausing a thread (debuggers, crash reporters, Chromium's profiler) waits until any library load it is doing
  finishes, a few milliseconds at most.

- Windows of the store client and of games open, resize and close as before: the new code only runs while a GL
  context is being destroyed, and it does the same detach, just on the thread that is destroying it.
