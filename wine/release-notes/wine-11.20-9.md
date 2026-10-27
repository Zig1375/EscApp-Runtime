Wine 11.20 for EscApp, ninth build: the Epic Games Launcher installs games.

## What's new

- **Epic Games Launcher installs games.** Pressing Install stopped at once with "Missing permissions to install to
  the folder" (error DP-07). The launcher prepares a game's folder by giving the players' group full access to it
  from a helper, then checks that the folder has that access. Wine didn't keep permissions given to a file or
  folder: it turned them into the Mac's own permissions and built them again from those, so the access the helper
  gave was gone when the launcher looked. Wine now keeps them with the file and reports them as they were set.

## What to watch for

- Permissions are kept in an extended attribute of the file (`user.wine.sd`), together with the Mac permissions
  they were set with. When the Mac permissions change outside Wine, the kept ones are dropped and the file reports
  permissions built from the Mac's again, as before.
- Only what a program set itself is kept. A folder or file created inside doesn't take on its folder's permissions
  as on Windows, and what may be done with a file is still decided by the Mac's permissions alone.
