Wine 11.20 for EscApp, tenth build: DirectX 12 games run on Apple's D3DMetal.

## What's new

- **DirectX 12 games can run on D3DMetal.** D3DMetal is Apple's translation of Direct3D 12 to Metal. EscApp
  doesn't ship it: the player downloads Apple's "Evaluation environment for Windows games" and adds it in
  EscApp's DirectX 12 settings. This build is what lets it run. D3DMetal's DLLs import a function from Wine's
  ntdll that Wine 11.20 no longer exports, so d3d12.dll failed to load at all; and its DLLs call its macOS library
  directly from Windows threads, which crashed every Direct3D 12 game at start. With `WINEMACSHAREDTSD=1`,
  which EscApp sets only for games on D3DMetal, Windows code runs with the macOS thread data, and both sides work.

## What to watch for

- Only games EscApp starts on D3DMetal get the new mode. Everything else runs exactly as in the ninth build,
  apart from Wine's own dispatchers now reading their thread data through the TEB pointer, which is the same data.
- In the new mode, Windows code reads only the TEB itself (`%gs:0x30`) and its TLS array (`%gs:0x58`) through the
  thread register; a game or protection that reads other TEB fields that way directly would get wrong values.
  Ordinary code reaches the TEB through `NtCurrentTeb()` and is unaffected.
