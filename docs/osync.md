# osync: in-process synchronization for Wine on macOS

Status: implemented (`wine/patches/0003`). Off unless wineserver starts with `WINEOSYNC=1`.
Code: `include/wine/osync.h` (layout and every operation, shared by wineserver and ntdll), `server/osync.c` (the
region, cells, cleanup after dead threads), hooks in `server/inproc_sync.c`, `server/thread.c` and
`dlls/ntdll/unix/{sync,server,thread}.c`.

## Why

With stock Wine on macOS every wait on, and every signal of, an event, mutex or semaphore is a request to
wineserver. Games do this thousands of times a second. Wine 11 added an interface for doing these operations
inside the process (`inproc_sync`, used by Linux's ntsync driver); macOS has no ntsync, so osync provides the same
operations in shared memory.

## Goals

- Only public macOS APIs: `os_sync_wait_on_address` and `os_sync_wake_by_address_*` (macOS 14.4 and later), shared
  between processes.
- No wineserver round trip in the steady state, including waits on several objects and across processes.
- Exact Windows semantics: PulseEvent wakes every thread waiting at that moment, WaitAll takes all objects at once,
  abandoned mutexes are reported.
- Few wakeups: an object goes to one chosen waiter, and only a sleeping waiter costs a system call.
- One switch, on wineserver: processes use whatever the server they talk to provides.

## Design

**Shared memory.** wineserver creates one region per prefix (an unlinked file, 64 MB of address space, pages
allocated on use) and hands its fd to every process as the `inproc_device` fd that Wine 11 already passes at
startup. The region is an array of 64-byte cells, one cache line each. Cells refer to each other by index, never
by pointer, since every process maps the region at its own address. Only the server allocates and frees cells.

**Objects.** One cell per event, semaphore, mutex or internal (server-signalled) event: a lock word, type,
generation, state (signalled flag, count, or recursion count), maximum, mutex owner and abandoned flag, the head
and tail of its waiter list, and a count of WaitAll waiters. A process refers to an object by `generation << 24 |
cell`; every operation checks the generation under the lock, so an operation racing with the object's destruction
and the cell's reuse fails cleanly instead of touching the new object.

**Locks.** Per-object lock word holding the owner's thread id (bit 0 = contended): compare-and-swap, a short spin,
then `os_sync_wait_on_address` (shared) on the lock word. Holding one object lock never waits for another; taking
several requires the region's WaitAll lock first, which rules out deadlocks. The server never waits on a lock held
by a dead thread: it takes it over.

**Waiting.** Each thread has a wait block (cells aligned to 32) with a status word and one entry per waited object
(up to 64, plus the APC alert event). Fast path: lock the object, and if it can be acquired, claim the block and
acquire it; no system call. Otherwise the entry is linked into the object's waiter list, then the thread spins
briefly and sleeps on its own status word with `os_sync_wait_on_address_with_timeout`.

**Hand-off.** Whoever makes an event or semaphore acquirable (set, release, the server) walks its waiter list under
the lock, waiters still spinning first, and claims waiters by compare-and-swap of their status from WAITING to
"signalled by entry i", acquiring the object on the waiter's behalf (decrement the semaphore, reset the auto event)
and unlinking the entry. A timeout is the same compare-and-swap to CANCELLED, so a timeout racing with a hand-off
has exactly one winner. Wakes happen after the lock is released (a woken thread would otherwise sleep again on that
lock) and only for waiters that set their "sleeping" flag: the waker changes the status before reading the flag,
the waiter sets the flag before the kernel compares the status, both sequentially consistent, so no wakeup is lost.

**Mutexes** are not handed over: a sleeping new owner would hold up every running thread (a lock convoy). A released
mutex becomes free and one waiter is claimed with RETRY: it tries to take it, and if another thread was quicker,
that thread wakes the next waiter when it releases it. The status stays RETRY until the attempt is over, so if the
thread dies in between, the server passes the wake on.

**Spinning.** A waiter spins up to 2 us before sleeping (time-based: a pause instruction costs next to nothing
under Rosetta), and only one thread at a time spins on an object, so a crowd of waiters doesn't take the
processors from the thread that would release it.

**PulseEvent** sets the event, hands it off to the waiters registered at that moment and resets it, all under the
lock: exact. **WaitAll** takes the WaitAll lock and all object locks in index order, acquires all objects
atomically or registers on all; a signaller of an object with WaitAll waiters takes the WaitAll lock and checks
whether such a waiter can now take everything.

**Server.** Creates and destroys cells, signals and resets internal events (process and thread exit, APC alerts),
marks a dead thread's mutexes abandoned and hands them off, unlinks its wait entries and frees its wait block. A
destroyed object with waiters left keeps working until they are gone (as on Windows, a wait keeps its object
alive: a mutex whose handles were all closed is still abandoned when its owner dies). osync is on when wineserver
starts with `WINEOSYNC=1`; processes use it whenever the server passes them the region.

## Results

In Wine on an M2 Max, median of five interleaved runs (`tests/sync` benchmarks and a mutex test):

| | wineserver | osync |
|---|---|---|
| event round trip between threads | 26.5 us | 1.4 us |
| event round trip between processes | 26.5 us | 1.6 us |
| semaphore queue, 200,000 items | 3554 ms | 156 ms |
| wait for any of 4, 50,000 rounds | 1345 ms | 61 ms |
| wait for all of 2, 20,000 rounds | 796 ms | 40 ms |
| uncontended mutex wait + release | 19.9 us | 0.77 us |
| mutex, 2 / 4 / 8 threads, 160,000 | ~2100 ms | 114 / 188 / 232 ms |

## Known limits

- Wine's handle cache in ntdll (shared with ntsync) keeps a reference that a thread killed while waiting never
  drops; a handle value reused after that isn't cached, and calls on it fetch the object from the server each time.
- A WaitAll naming one object twice fails, as on Windows and with ntsync; wineserver accepts it.
- Mutexes are not first-come first-served (neither are Windows' in practice).

## Tests

- `tests/osync/run.sh`: the header built natively for arm64, x86_64 (Rosetta), ThreadSanitizer and AddressSanitizer
  + UBSan; semantics of every operation, hand-off, PulseEvent, WaitAll, alerts, 64-object waits, stale names, a
  reference model (500,000 random single-threaded steps compared result by result and state by state), mutex
  contention, waiters woken to retry that die, cross-process ping-pong, processes killed mid-wait or holding a lock,
  8-thread and 4-process stress with conservation checks, and a watchdog against hangs.
- `tests/sync/run.sh`: the same semantics through the Windows API in Wine, 32- and 64-bit, with and without osync,
  including threads terminated holding or waiting on objects and a mutex outliving its handles; benchmarks; and a
  differential fuzz: seeded call sequences must hash the same with osync as with wineserver.
- `tests/winetest/run.sh`: Wine's kernel32:sync and ntdll:sync; osync may not add failures to wineserver's.

These found, before release: wakeups under a held lock (a 3 us penalty), two reuse races after a hand-off
(ThreadSanitizer), a lost wakeup when a thread woken to retry died before trying, and a mutex never abandoned
after its last handle closed (Wine's own tests).
