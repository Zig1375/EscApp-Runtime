Wine 11.20 for EscApp, twelfth build: games and Steam no longer crash when the Mac is slow to start them, and a
crash on a macOS thread is logged with its stack.

## What's new

- **No crash when the Mac is slow to start a game.** Each Wine process asks macOS's main thread to start the
  Mac side of its windows and waited five seconds for it, keeping what they shared on the waiting thread's stack.
  On a busy Mac (a game starting under Rosetta while Steam and other Wine processes start too) the main thread
  could come later: the process lost its windows, and the main thread then crashed on what the waiting thread had
  already freed. They now share it safely, and the wait is a minute.
- **Crashes on macOS threads say where.** A crash on a thread Wine didn't create (macOS's main thread, or one of
  Apple's) ended the process with only an address in the log. The log now lists the native stack, with libraries
  and function names. Among Us, Steam's helpers and Banished died this way in `objc_release`.

## What to watch for

- A process whose Mac side doesn't start now waits up to a minute instead of five seconds before it goes on
  without windows.
- Logs of such crashes have a few dozen more lines.
