Wine 11.20 for EscApp, fourth build: osync without its known limits.

## What's new

- **No slow handles after a killed thread.** Wine's handle cache kept a reference that a thread killed while
  waiting never dropped, so the next object given that handle value went through wineserver on every call. With
  osync on, the cache now has no reference counts (an entry is one value read and written whole), and such a handle
  is as fast as any other.
- **No mutex starvation.** A released mutex still goes to whichever thread takes it first, for speed, but a waiter
  that loses that race twice gets the next release handed over, so threads re-taking a mutex in a loop can't keep
  one out forever.
- A little faster again on some benchmarks (median of three, in Wine on an M2 Max, against stock wineserver):
  event round trip 1.2 us (25.9 us), semaphore queue 123 ms (3534 ms), wait for all of 2 35 ms (949 ms).

## Checked

osync's native tests in four builds (arm64, x86_64, ThreadSanitizer, AddressSanitizer + UBSan), including a test of
the anti-starvation rule that fails with the rule switched off; `tests/sync` with and without osync, 32- and 64-bit,
with a check that a handle reused after a killed waiter stays fast (24 times slower before) and differential fuzz
identical to wineserver; Wine's kernel32:sync and ntdll:sync with no failure wineserver doesn't have.
