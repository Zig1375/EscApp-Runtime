Wine 11.20 for EscApp, eighth build: Chromium-based games no longer flicker.

## What's new

- **No flicker in NW.js and Electron games.** Games built on Chromium (NW.js, Electron) redraw only the part of the
  picture that changed and count on the rest staying as they drew it. Wine swapped two buffers instead, so
  anything drawn only once alternated with an older picture: after such a game's window was maximized on a screen
  of another shape (The Taming Massage Parlor on a MacBook's own screen), the bars around its picture flickered
  between white and the picture from before, until the game happened to redraw the whole window. Wine now keeps
  the last frame in the buffer the game draws into next. This is the OpenGL path every game takes when EscApp
  emulates its display mode.

## What to watch for

- Every OpenGL frame on that path is now copied once more inside the graphics card. The copy is cheap next to
  drawing a frame, but a game that already runs at the edge of its frame rate may lose a little.
