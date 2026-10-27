Wine 11.20 for EscApp, seventh build: the Epic Games Launcher installs games and keeps the player signed in, and a
stretched game window can be moved to another display.

## What's new

- **Epic Games Launcher installs games.** Choosing a folder and pressing Install closed the dialog and nothing
  downloaded (the launcher logged error DP-06). The launcher prepares the folder in a second copy of itself and
  waits for it to write a file into a folder it watches; Wine learns of such changes only through inotify, which
  macOS doesn't have, so the launcher never heard back. Wine now gets them from FSEvents, and every program
  that waits for changes in a folder (ReadDirectoryChangesW, FindFirstChangeNotification) is told of them.
- **Epic keeps the player signed in.** After every start the launcher asked to sign in again: it binds the sign-in
  to a key of its own that it keeps in Windows' key storage (a named CNG key), and Wine kept such keys only while
  the program ran. The launcher made a new key at each start and Epic's server refused the saved sign-in
  (`invalid_dpop_proof`). Wine now keeps named keys in the bottle's registry and opens them again by name.
- **Stretched windows move between displays.** With Retina mode on, a game window that had been stretched jumped
  back as soon as the player dragged it towards another display (from a MacBook's own screen to an external one,
  for example). macOS gives such a window's frame in points, so a window an odd number of pixels wide or tall came
  back a pixel off after a plain move; Wine took the move for a resize, and a resize keeps a stretched window on
  the screen it is on. A frame within a pixel of the window's size is now a move.

## What to watch for

- macOS reports a change a few milliseconds after it happened, where Windows reports it at once: a program that
  checks for a change right after making it may not see it yet (some of Wine's own tests do). A program that
  watches a folder it has just deleted and created again can also be told of a file removed just before.
- A named key is kept in the bottle's registry (`HKCU\Software\Wine\NCrypt\Keys`) as plain key data, where
  Windows protects it with the user's login; anyone who can read the bottle can read the key. RSA and ECDSA P-256
  keys only, the kinds Wine creates. A player already signed out has to sign in once more.
- The stretching belongs to the display the window was stretched on. On the other display the game is shown at
  its own size until the player stretches it there.
