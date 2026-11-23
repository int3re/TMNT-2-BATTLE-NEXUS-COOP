# Changelog

[← Back to the mod's page](../../README.en.md) · [All documentation](README.md) · [Русский](../CHANGELOG.md)

The current version is **0.17.3**. A detailed description of every version for players is in
[`dist/README.md`](../../dist/README.md) and is installed together with the mod.

- [Version compatibility in network play](#version-compatibility-in-network-play)
- [Launcher](#launcher)
- [0.17 — smooth play over the network](#017--smooth-play-over-the-network)
- [0.16 — everything in the game's menus](#016--everything-in-the-games-menus)
- [0.13 – 0.15 — network play without a restart](#013--015--network-play-without-a-restart)
- [0.8 – 0.12 — real co-op](#08--012--real-co-op)
- [0.5 – 0.7 — network play](#05--07--network-play)
- [0.1 – 0.4 — the foundation](#01--04--the-foundation)

## Version compatibility in network play

In a network game everyone needs a version of the mod with the same protocol. The host itself refuses a player
with another version and says why.

| Mod versions | Protocol |
|---|---|
| **0.17.1 – 0.17.3** | 16 |
| 0.17.0 | 15 |
| 0.16.6 – 0.16.7 | 14 |
| 0.16.4 – 0.16.5 | 13 |
| 0.16.3 | 12 |
| 0.16.0 – 0.16.2 | 11 |
| 0.14.0 – 0.15.0 | 10 |
| 0.13.0 | 9 |
| 0.12.2 | 8 |
| 0.12.0 – 0.12.1 | 7 |
| 0.10.0 – 0.11.0 | 6 |
| 0.9.0 – 0.9.1 | 5 |
| 0.8.0 | 4 |
| 0.7.2 – 0.7.3 | 3 |
| 0.7.1 | 2 |
| 0.5.0 – 0.7.0 | 1 |

Versions in the same row play together. Even within a row it is better for everyone to update: fixes work for
those who have them installed.

## Launcher

The launcher has versions of its own: it is updated separately from the mod.

### 1.3.1 — 21 November 2026
- **The news is on a sheet with a bandana**, as in the launcher of the first game: a sheet of paper slightly
  askew, the heading on a bandana with a knot and two tails, in the colour of the bandana chosen in the
  settings. The notes are marked with stripes in the colours of the four turtles. The sheet is as tall as what
  is written on it. The old banner on a crossbar is gone.
- Inside is the same mod 0.17.3.

### 1.3.0 — 21 November 2026
- **The launcher updates itself at start.** If a newer launcher is out on GitHub, it is downloaded, checked
  against its SHA-256, takes the place of the old one and starts by itself. The mod in the game folder does not
  change in the process: the new version of the mod is inside the new launcher and is installed with no
  download. The switch is in the settings: **Обновлять лаунчер при запуске** (Update the launcher at start).
- The link «Вышел лаунчер … — обновить» (Launcher … is out — update) in the status line also restarts the
  launcher at once.
- The **ЗВУК** (Sound) tab has a **sound mode**: stereo or mono.
- Inside is mod 0.17.3.

### 1.2.0 — 21 November 2026
- **The game's settings in the launcher.** The settings have three new tabs with the same rows as the game's
  menus:
  - **ГРАФИКА** (Graphics) — window and resolution, frame shape, menus and texts, world map, gauges,
    antialiasing, texture filter, sharpness, colours, field of view, network camera, vsync;
  - **ЗВУК** (Sound) — the volume of music, effects and voices, intro movies at start, running in the
    background;
  - **УПРАВЛЕНИЕ** (Controls) — a key or a mouse button for every action, the layouts **Как в игре** (As in the
    game) and **WASD + мышь** (WASD + mouse), one player at the keyboard or two.
- The values are written to `tmnt2net.ini` in the game folder and take effect at the game's next start. Before
  the first change the launcher puts a copy of the file aside.
- A long message in the status line no longer runs over the repository's address.
- Inside is mod 0.17.2.

### 1.1.1 — 20 November 2026
- Inside is mod 0.17.1; the launcher itself did not change.

### 1.1.0 — 20 November 2026
- **One file.** The mod is inside the launcher: there is no separate installer `tmnt2net-setup.exe` any more.
  The first install happens at once and without the internet.
- A new version of the mod arrives as a new launcher: it is downloaded, checked against its SHA-256, the mod is
  installed from it, and it takes the place of the old one.
- Installing and removing from the command line: `tmnt2net-launcher.exe /install "game folder" /quiet`.
- The headings are set in the Dela Gothic One font, as in the design mock-up.

### 1.0.0 — 20 November 2026
- The first version of [the launcher](LAUNCHER.md): it finds the game, asks GitHub for the latest version of
  the mod, downloads the installer, checks its SHA-256, installs the mod and starts the game.
- It shows whether the new version is compatible over the network with the installed one, and what is new in
  it.
- It waits for the game to close if an update arrives while the game is open.
- A proxy for GitHub: as in Windows, a VPN client on this PC, or your own address.
- Releases of the mod come with `tmnt2net-latest.json`, the release description for the launcher.

## 0.17 — smooth play over the network

### 0.17.3
- The mod remembers the sound mode (stereo or mono) in `tmnt2net.ini`, section `[audio]`, the same way as the
  volume.
- The protocol is the same, 16: over the network 0.17.3 plays with 0.17.1 and 0.17.2.

### 0.17.2
- The mod remembers the volume of music, effects and voices in `tmnt2net.ini` (section `[audio]`). The game
  itself keeps it only in the save: until a save was loaded, every start began at full volume. The volume is
  changed as before, in OPTIONS → SOUND (ОПЦИИ → ЗВУК), and on the launcher's **ЗВУК** (Sound) tab.
- The protocol is the same, 16: over the network 0.17.2 plays with 0.17.1.

### 0.17.1
- On a wide screen the world map fills the whole frame: at the sides, instead of black bars, its picture goes
  on. The clouds and the moon are drawn as they are, the planets on the space maps are completed to a circle.
  `map = center` in `tmnt2net.ini` brings the bars back.
- Esc on the world map opens the map's menu (character select, options, save, exit to the title menu), like
  the guard key; a second Esc closes it.
- Protocol 16: over the network 0.17.1 plays only with 0.17.1.

### 0.17.0
- Input delay adjusts by itself for every player to their own link: the game no longer runs slowly for
  everyone because of one player whose ping is higher than it was at the start.
- Enemies near any player behave as if in view, not only those near the middle of the team.
- The fullscreen mode AS THE ORIGINAL (КАК В ОРИГИНАЛЕ) is replaced by BORDERLESS (НА ВЕСЬ МОНИТОР): Alt+Tab
  no longer stops a network game.
- Desync: up to four automatic rejoins; if that does not help, the player leaves the session with a message
  instead of staying in a game that has gone its own way.

## 0.16 — everything in the game's menus

### 0.16.7
- One more leftover of the previous session is removed: the set of players. A game that had already played a
  session with fewer players counted the others, in the next one, as having joined in the middle of the game.
- The message "the game is running slowly" (игра идёт медленно) no longer appears for nothing at the start of
  a session.

### 0.16.6
- Fixed a desync for a player who had already played a network session since the game was started: frames of
  the previous session stayed in memory, and their game replayed them instead of waiting for the host.
- Fixed a desync for a player with the frame shape 4:3 or STRETCH (РАСТЯНУТЬ): their game computed a different
  camera. The frame shape is now only a way of showing the picture.
- Protection against calculations diverging because of foreign code in the game's process (overlays, screen
  capture).
- Players are no longer pulled towards each other: in a network game you can go as far apart in a stage as you
  like.
- Fixed the names at character select: under a selection frame is the name of the player who controls it.
- On a desync the logs of both sides are saved in `tmnt2net-desync-*.log` files.

### 0.16.5
- Joining a running game is about 4 times faster and with no "rewind" on the screen: while the game catches up
  with the others it draws nothing and is silent, and the screen shows the loading screen CONNECTING TO THE
  GAME (ПОДКЛЮЧЕНИЕ К ИГРЕ) with a percentage and the time left. Half an hour of play takes about 50 seconds
  instead of 4 minutes.

### 0.16.4
- Pause and exit to the menu work in a network game: Esc, Enter, START and BACK. The pause is shared.
- In single player a gamepad now opens the pause too.
- The session's title menu has the item LEAVE THE NETWORK GAME (ВЫЙТИ ИЗ СЕТЕВОЙ ИГРЫ): the session ends and
  everyone stays in their own game.

### 0.16.3
- Fixed: a player who had dropped out could not get back into a game that had been running for a long time. The
  game's history now goes in pieces.
- A player with a short break in the link returns to their place by themselves.
- The shared progress is not lost if the game was closed in the middle of a session.
- Fixed: the picture was corrupted while the game waited for a player.
- Every hero is unlocked from the start: Casey, Splinter, Karai and Slashuur are picked with the arrows at
  character select.
- You can see who is making the game run slowly.

### 0.16.2
- Fixed: the game stopped taking the keyboard if its window had lost focus even once.
- The intro movies at start are shown again and are skipped with any press; there is an "always skip" setting.

### 0.16.1
- Mouse buttons are assigned to actions like keys.
- A ready layout, wasd + MOUSE (wasd + МЫШЬ).
- Your own keys work in a network game and are kept in `tmnt2net.ini`.

### 0.16.0
- All the graphics are on one page in the game's OPTIONS: OPTIONS → SCREEN (ОПЦИИ → ЭКРАН).
- The resolution changes at once, without a restart.
- Every costume is unlocked and picked with the up and down arrows at character select.
- Wide screens: fixed stretched panels and text on 21:9 and 32:9 monitors.
- In a network game the settings that affect how the game plays are taken from the host.

## 0.13 – 0.15 — network play without a restart

### 0.15.0
- Sharpness and colours of the finished frame, 16x texture filter.
- The mod's pages in one style, in the game's font.
- A clear NETWORK GAME (СЕТЕВАЯ ИГРА) page: HOST A GAME (ОТКРЫТЬ СВОЮ ИГРУ), JOIN A FRIEND (ВОЙТИ К ДРУГУ),
  JOIN BY CODE (ВОЙТИ ПО КОДУ); inviting a friend from inside the game.
- Area results as a table in the game's font.
- The mod's version is shown under the title menu.

### 0.14.0
- Network play without a restart: a session starts right in the title menu.
- A latecomer joins a running game without a restart too.

### 0.13.0
- The network menu inside the game, in its style.
- Esc goes back in all of the game's menus.
- The monitor's resolutions on the screen settings page.

## 0.8 – 0.12 — real co-op

### 0.12.0 – 0.12.2
- A player who joins in the middle of a stage gets one of the team's heroes at once and goes into the fight.
- The host can start alone; friends join as the game goes.
- A player who dropped out returns to their place under another name too: the host knows them by their Steam
  ID or by the number of their PC.

### 0.11.0
- Widescreen and your own field of view in a network game.
- Gauge size, antialiasing, anisotropic filtering.

### 0.10.0 – 0.10.2
- Everyone has their own camera in a network game.
- The mouse wheel zooms the camera in and out.
- Waking a teammate who is down: hold guard next to them.
- A player who dropped out returns to their place.
- A desync no longer ends the session.
- A link icon for every player, "Waiting: …" (Ожидание: …) on everyone's screen.
- Area results per player, the name above the MVP caption.
- A Steam invitation is accepted right from inside the game.

### 0.9.0 – 0.9.1
- Joining a network game that is already running.
- A shared network save, separate from the single-player one.
- Names at character select.
- The stage gauges are in the corners of the wide frame.

### 0.8.0
- Each player has their own health. A player who goes down falls asleep for 10 seconds and gets up by
  themselves.

## 0.5 – 0.7 — network play

### 0.7.0 – 0.7.3
- The installer `tmnt2net-setup.exe`.
- Player names over the heroes.
- After an area the whole team dances; at in-stage transitions everyone jumps.
- On the area map — an icon for every player.

### 0.6.0
- The items NETWORK GAME (СЕТЕВАЯ ИГРА) and mod settings in the title menu, in the game's own letters.

### 0.5.0
- Network play for up to 4 players: everyone has their own copy of the game, only input travels over the
  network.
- Connecting through Steam or by code and IP.

## 0.1 – 0.4 — the foundation

### 0.4.0
- Determinism: given the same input the game runs the same, frame for frame. The foundation of network play.

### 0.3.0
- Xbox, PlayStation and Switch Pro gamepads through SDL2, hot-plugging.
- Up to 4 players on one PC, two players on one keyboard.

### 0.2.0
- Widescreen without stretching: a wider view, menus and texts in the right proportions.
- Exactly 60 frames per second.

### 0.1.0
- Fixed the "Unkown Error" at start-up on Windows 10/11.
- Fixed a crash when the game's window is not in focus during loading.
- Every resolution of the monitor; borderless window, ordinary window, fullscreen; Alt+Enter.
- The log `tmnt2net.log`.
