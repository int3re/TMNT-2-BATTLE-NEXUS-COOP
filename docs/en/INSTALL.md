# Installing, updating and removing

[← Back to the mod's page](../../README.en.md) · [All documentation](README.md) · [Русский](../INSTALL.md)

## What you need

- Windows 10 or 11.
- An installed copy of *TMNT 2: Battle Nexus* for PC with the `TMNT2.exe` of the North American 2004 release.
  The same file is in the Russian releases of the game. Its SHA-256:

  ```
  22f49908bcd3b84278bea888b7285e1e0ba73a72a89764f495aceb76a42d8b6a
  ```

  With any other `TMNT2.exe` the mod changes nothing: the game starts as it does without the mod. The launcher
  warns you about this beforehand.
- For play over Steam: a running Steam client. The game itself is not sold on Steam and nothing has to be
  bought.

## Installing

The mod is installed by **[the launcher](LAUNCHER.md)**. It is the only file to download: the mod is inside it.

1. Download `tmnt2net-launcher.exe` from the [Releases](https://github.com/int3re/TMNT-2-BATTLE-NEXUS-COOP/releases/latest) page and run it.
2. The launcher finds the game by itself: in its own folder, by the game installer's entry in Windows, or in
   the folders next to it. If it has not found the game, point it to the folder with `TMNT2.exe`.
3. Press **УСТАНОВИТЬ МОД** (Install the mod), then **ИГРАТЬ** (Play). A line with the mod's version appears
   under the game's title menu.

> The launcher is not signed with a certificate, so Windows may show "Windows protected your PC". Click
> "More info" → "Run anyway".

These files appear next to `TMNT2.exe`:

| File | What it is |
|---|---|
| `winmm.dll` | Ultimate ASI Loader: it loads the mod into the game. If it is already there for another mod, that one is used |
| `tmnt2net.asi` | the mod itself |
| `tmnt2net.ini` | the settings. Your values are kept when you update |
| `steam_api.dll`, `steam_appid.txt` | Steam for network play |
| `tmnt2net-README.md` | a short description and the list of changes |
| `tmnt2net-install.txt` | the list of everything the launcher installed |

Files with the same names that were in the folder before are put aside by the launcher into
`tmnt2net-backup\`. It does not touch `TMNT2.exe`, the game's data or your save `tmntsave.dat`.

While the game runs, the mod creates a few more files:

| File | What it is |
|---|---|
| `tmnt2net.log` | the log of the last run |
| `tmnt2net-sim.log` | the frame log of a network game, needed to investigate desyncs |
| `tmnt2net-coop.rec`, `tmntsave-net.dat` | the shared progress of network play, see [Network play](NETPLAY.md#saves-and-progress) |

### Installing by hand

Copy the files from the table above into the game folder. They can be taken from the `build/dist/Release`
folder after a [build](BUILDING.md).

### Installing from the command line

```bash
tmnt2net-launcher.exe /install "C:\Games\TMNT 2 - Battle Nexus" /quiet
```

`/quiet` removes the window with the report: the report is written to the console. Exit code `0` means it
worked, `2` means it did not.

## Updating

In the launcher there is the **ОБНОВИТЬ** (Update) button: the launcher downloads the new version from GitHub,
checks it and installs it. If GitHub does not open from your PC, download the new `tmnt2net-launcher.exe` in a
browser: the new version of the mod is inside it. The settings in `tmnt2net.ini` are kept.

For network play **every player** has to update the mod: the versions must be compatible, see the
[compatibility table](CHANGELOG.md#version-compatibility-in-network-play).

## Removing

In the launcher: the icon with sliders at the top right → **Удалить мод** (Remove the mod); it asks once more.
From the command line:

```bash
tmnt2net-launcher.exe /uninstall "C:\Games\TMNT 2 - Battle Nexus" /quiet
```

What happens:

- exactly the files that were installed are removed (by the list in `tmnt2net-install.txt`);
- the files that were put aside come back from `tmnt2net-backup\`;
- the logs and the network copy of the save, `tmntsave-net.dat`, are erased;
- your save `tmntsave.dat`, `TMNT2.exe` and the game's data stay as they were;
- `TMNT2.ini` keeps a resolution that the game also starts with when the mod is not there.

## Checking that the mod works

- the line `tmnt2net 0.17.3` is visible under the title menu;
- the title menu has the items **NETWORK GAME** (СЕТЕВАЯ ИГРА) and **MOD SETTINGS** (НАСТРОЙКИ МОДА);
- `tmnt2net.log` has appeared in the game folder, and at its beginning is the line
  `TMNT2.exe sha256 … (NA retail 2004, supported)`.

If any of this is missing, see [Troubleshooting](TROUBLESHOOTING.md#the-mod-does-not-load).
