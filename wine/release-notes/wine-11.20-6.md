Wine 11.20 for EscApp, sixth build: game windows that can't be resized can be stretched.

## What's new

- **Stretch any game window.** A game whose window has a fixed size (many Unity games, old games in a 640×480
  window) can now be made bigger or smaller by dragging its corner. The game doesn't notice: it keeps drawing at
  its own size, and Wine scales the picture and the mouse. The window keeps the game's proportions, can't get
  smaller than the game itself or bigger than the screen, and the size is remembered for the next start. EscApp
  turns this on with `StretchWindows` next to `EmulateModeset`.
- **When the game changes its resolution,** the picture is fitted again to the size the player chose, keeping the
  game's proportions. A game that becomes bigger than that size is shown at its own size. A full-screen mode the
  game sets itself is left as it is.
- **Fixes in the Mac driver** that stretching made common, some of which affected games before:
  - Direct3D 9 games in an emulated full screen (for example 800×600) showed a white window; they now fill the
    screen.
  - OpenGL games could be drawn at a stale size, or only their top-left corner was shown.
  - A Direct3D 9 game could hang when its window changed size while it was drawing.
  - The bars beside an emulated full screen are black on both sides with Retina mode on (one was white).
  - DXMT games' picture now follows their window when it moves or changes size.

## What to watch for

- A game that resizes its window without telling Direct3D 11 (no `ResizeBuffers`) shows a white window with DXMT;
  that was so before this build too.

## Checked

`stretchprobe` (EscApp's `Tests/Fixtures/probes`) through GDI, Direct3D 9 (WineD3D, OpenGL), Direct3D 11 through
DXVK and DXMT, also as a per-monitor DPI aware program like Unity: the player stretches ×2, ×0.8, past the screen
and below the game's size, then the game switches to 800×600, 1600×1200 and 1280×720 and back, goes full screen and
returns; the game sees its own size throughout, gets no `WM_SIZE`, and the picture fills the window in every step.
Dragging the corner by hand: the proportions held, and the mouse landed within 2 pixels. A Unity game opened at the
stretched size on the next start. EscApp's graphics probes (Direct3D 9 with WineD3D, Direct3D 11 with DXVK and
DXMT, 32- and 64-bit) pass with stretching on.
