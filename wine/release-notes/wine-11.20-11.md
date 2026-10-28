Wine 11.20 for EscApp, eleventh build: games no longer hang on their first game controller check.

## What's new

- **Games that check for game controllers at start no longer hang.** The first time a game asks XInput about
  controllers, Wine starts a helper thread and waits until it has listed them. That thread used to set up its
  window for device notifications first, and on macOS that window work could wait for the game's own main
  thread, which was waiting for the helper: the game hung for good. Among Us froze on a black window right after
  its intro, on every graphics backend. The helper now lists the controllers and lets the game go before any
  window work.

## What to watch for

- A controller plugged in during the moment between the first check and the helper's notification setup is found
  by the helper's rescan, which runs every 2 seconds, instead of at once.
