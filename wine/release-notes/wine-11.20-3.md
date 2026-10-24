Wine 11.20 for EscApp, third build: osync, our fast in-process synchronization, and our own fix for 32-bit games
under Rosetta 2.

## What's new

- **osync** (`docs/osync.md`): events, semaphores and mutexes are waited on and signalled inside the game's
  process, in memory shared with wineserver, using only public macOS APIs. **Off unless `WINEOSYNC=1` is set for
  wineserver**; Wine processes follow whatever the server they talk to does. Measured in Wine on an M2 Max (median
  of five, against stock Wine's wineserver path):

  | | wineserver | osync |
  |---|---|---|
  | event round trip between threads | 26.5 us | 1.4 us |
  | event round trip between processes | 26.5 us | 1.6 us |
  | semaphore queue, 200,000 items | 3554 ms | 156 ms |
  | wait for any of 4, 50,000 rounds | 1345 ms | 61 ms |
  | wait for all of 2, 20,000 rounds | 796 ms | 40 ms |
  | uncontended mutex wait + release | 19.9 us | 0.77 us |
  | mutex, 2 / 8 threads, 160,000 | ~2100 ms | 114 / 232 ms |

  PulseEvent wakes every thread waiting at that moment, and a WaitAll naming the same object twice is refused, as on
  Windows.
- **32-bit games under Rosetta 2**: wow64cpu now switches between 32- and 64-bit code with far returns instead of
  far jumps, which a signal could catch in a state Wine can't resume (random crashes in wow64cpu.dll).

## Known issues

- Wine's own handle cache (also with ntsync on Linux) keeps a reference that a thread killed while waiting never
  drops; a handle value reused after that isn't cached and its calls go through wineserver, slower but correct.

## Checked

osync's native tests in four builds (arm64, x86_64, ThreadSanitizer, AddressSanitizer + UBSan) and ten 1,000,000-
operation soak runs; `tests/sync` with and without osync, 32- and 64-bit, with differential fuzz identical to
wineserver; Wine's kernel32:sync and ntdll:sync (about 380,000 checks) with no failure wineserver doesn't have.
Direct3D 11 through DXVK and DXMT and Direct3D 9 through WineD3D, 32- and 64-bit, three rounds with osync off and
on: 36/36. Videos (Media Foundation and DirectShow, four formats, 32- and 64-bit): 16/16 in both modes. The 32-bit
Direct3D 9 probe started 80 times in a row: 80/80 (stock Wine: about one start in three crashed).
