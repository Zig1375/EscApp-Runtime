Wine 11.20 for EscApp, fifteenth build: 32-bit Direct3D 11 games no longer run out of memory.

## What's new

- **32-bit Direct3D 11 games on DXVK no longer crash after a few minutes.** A 32-bit program can only address
  4 GB, so Wine gives each graphics buffer the game can write to pages of its own below that line. When the game
  freed a buffer, Wine forgot to give those pages back, and a game that loads and drops pictures as it goes used up
  all 4 GB: the Daughter for Dessert chapters (GameMaker) crashed every ten to fifteen scenes, "out of memory" in
  the log, although the game itself needs far less. Freed buffers now give their memory back. A test that makes and
  frees a 64 MB buffer over and over failed at round 61 before and passes 400 rounds now.

## What to watch for

- 32-bit games on DXVK (most 32-bit Direct3D 11 games) are the ones affected: watch for any new graphics glitch or
  crash in them. 64-bit games don't take this path.
