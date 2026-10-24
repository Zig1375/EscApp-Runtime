Wine 11.20 for EscApp, fifth build: osync no longer hangs when threads are suspended or killed.

## What's new

- **No more hangs at start (Steam).** With osync on, a thread could be suspended (SuspendThread, GetThreadContext,
  APCs from other processes) or killed in the middle of setting or waiting on an object, holding that object's
  lock. wineserver then waited for that lock forever, and the whole prefix stopped: the program hung with no CPU
  use, and no new program could start. Threads now finish what they are doing with an object before they let
  themselves be suspended or killed, and wineserver never waits for a lock whose holder is gone.
- **Killed threads leave objects consistent.** A thread killed while running (TerminateThread, or the other
  threads of an exiting process) could take a mutex again after wineserver had abandoned it, leaving it owned by a
  dead thread forever. A killed thread now takes nothing more.
- **No program outlives wineserver.** A program whose threads were all waiting in osync kept running after
  wineserver died or was killed (`wineserver -k` left it behind). It now exits within a second (stock Wine, without
  osync, leaves such programs behind too when wineserver is killed outright).

## Checked

Three new tests take the server down the ways Steam's hang did, and all three fail on wine-11.20-4: threads
suspended, sent APCs and killed at any instant for 8 seconds (wine-11.20-4 hung, or left a mutex owned by a dead
thread), a process killed with SIGKILL while it works shared objects, and wineserver killed under waiting threads
(on wine-11.20-4 the program stayed). On this build they pass, 32- and 64-bit. osync's native tests in four builds
(arm64, x86_64, ThreadSanitizer, AddressSanitizer + UBSan) now include signals that "suspend" threads at any
instant, which may never stop one holding a lock (fails without the fix), and killed threads that a rival keeps
handing a mutex to (fails without the fix). `tests/sync` with and without osync, differential fuzz identical to
wineserver, Wine's kernel32:sync and ntdll:sync with no failure wineserver doesn't have, game videos 16 of 16 with
and without osync. Speed unchanged (event round trip 1.1 to 1.3 us, wineserver 26 us).
