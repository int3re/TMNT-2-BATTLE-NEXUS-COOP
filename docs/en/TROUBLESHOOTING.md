# Troubleshooting

[← Back to the mod's page](../../README.en.md) · [All documentation](README.md) · [Русский](../TROUBLESHOOTING.md)

- [Start-up](#start-up)
- [Screen and picture](#screen-and-picture)
- [Controls](#controls)
- [Network play](#network-play)
- [Saves](#saves)
- [How to report a bug](#how-to-report-a-bug)

## Start-up

### The mod does not load

Signs: there is no version line under the title menu, and the menu has no NETWORK GAME (СЕТЕВАЯ ИГРА) and
MOD SETTINGS (НАСТРОЙКИ МОДА) items.

- **There is no `tmnt2net.log` in the game folder.** The mod did not load at all. Check that `winmm.dll` and
  `tmnt2net.asi` are next to `TMNT2.exe`. An antivirus may have removed them: mod loaders stand in for a
  system library in the game folder, and some antivirus programs find that suspicious. Put the files back with
  the installer and add the game folder to the exclusions.
- **The log has the line `game patches disabled`.** You have a different `TMNT2.exe`. The mod works with one
  file, the North American 2004 release; see [installation](INSTALL.md#what-you-need). With any other it
  changes nothing, on purpose.

### "Unkown Error" at start-up

This is the game's own error on modern Windows, and the mod fixes it. If the error window still appears, the
mod did not load: see above.

### Intro movies every time

Any press skips them: Enter, Esc, Space, a mouse click, a gamepad button. To remove them altogether:
MOD SETTINGS (НАСТРОЙКИ МОДА) → INTRO AT START (ЗАСТАВКИ ПРИ ЗАПУСКЕ) → SKIP (ПРОПУСКАТЬ).

## Screen and picture

### Black bars at the sides

- At character select and in movies the bars are always there: these screens are drawn for 4:3, and the mod
  does not stretch them.
- In a stage there should be no bars. Check OPTIONS (ОПЦИИ) → SCREEN (ЭКРАН) → FRAME SHAPE (ФОРМАТ КАДРА): it
  should be AS THE MONITOR (ПОД МОНИТОР).

### Antialiasing has no effect

Antialiasing works when the frame fills the window: in the borderless mode over the whole monitor, or in a
window without scaling. If the frame's resolution is smaller than the window, antialiasing is switched off.

### The game runs too fast or too slow

The game's logic is tied to the frame, and its native speed is 60 frames per second. Leave `fps_limit = 60`.
If there are fewer than 60 frames, the game runs slower: lower ANTIALIASING (СГЛАЖИВАНИЕ) or RESOLUTION
(РАЗРЕШЕНИЕ).

### The picture is too sharp or too bright

OPTIONS (ОПЦИИ) → SCREEN (ЭКРАН) → SHARPNESS (РЕЗКОСТЬ) and COLOURS (ЦВЕТА). The value 0 switches the
processing off altogether.

## Controls

### The game does not react to the keyboard

In versions 0.14.0 – 0.16.1 the keyboard stopped working if the game window lost focus even once. Fixed in
0.16.2: update the mod.

### The mouse does not work

The mouse works only while the game window is active and the pointer is over it. Mouse buttons are assigned
like keys: OPTIONS (ОПЦИИ) → CONTROLS (УПРАВЛЕНИЕ), or a ready layout in MOD SETTINGS (НАСТРОЙКИ МОДА).

### The gamepad is not detected or its buttons are mixed up

- Check MOD SETTINGS (НАСТРОЙКИ МОДА) → GAMEPADS (ГЕЙМПАДЫ): the value must not be 0.
- A gamepad can be plugged in after the game has started; it is detected by itself.
- Buttons are reassigned in OPTIONS (ОПЦИИ) → CONTROLS (УПРАВЛЕНИЕ).

### The pause does not open in a network game

Pause in a network game works from version 0.16.4 on: Esc, Enter, START or BACK.

## Network play

### I cannot see my friend's game in the list

- Steam is running on both sides, and you are friends in Steam.
- The host really has opened a game: NETWORK GAME (СЕТЕВАЯ ИГРА) → HOST A GAME (ОТКРЫТЬ СВОЮ ИГРУ).
- The first page of NETWORK GAME says whether Steam is connected.
- If Steam is not available, play [by code or IP](NETPLAY.md#by-code-or-ip).

### The host refuses the join

The reason is written on the screen. Usually it is:

- **a different version of the mod**: update the mod for everyone, see
  [compatibility](CHANGELOG.md#version-compatibility-in-network-play);
- **a different `TMNT2.exe`**: everyone must have the same game;
- **the game is full**: it already has four players.

### No connection by code or IP

- The host has to open UDP port `7790` on the router and allow the game in the Windows firewall.
- On a LAN no port forwarding is needed, but the firewall may block the game.
- Check that everyone has the same port in `[net] port`.

### A Steam invitation does not work

The invited player's game must be running. Check that `tmnt2net.ini` has `[net] steam_invites = 1`.

### The game runs slowly or in jerks

A network game runs at the speed of the slowest PC: every frame waits for everyone's input. Since version
0.17.0 the mod evens out the difference between links by itself: every player's input delay follows their own
ping.

- If a notice at the top says whom the game is waiting for, the cause is that player's link or the speed of
  their PC.
- If your own PC cannot keep up, the mod suggests lowering antialiasing or resolution.
- The link indicator under a player's box: green is up to 80 ms, yellow up to 160, red is more.

### "Waiting: P2 …" (Ожидание: P2 …)

The game is waiting for this player's input: their link has dropped or their game has frozen. If the link does
not come back, the player drops out and the game goes on without them. What to do next: see
[If the connection drops](NETPLAY.md#if-the-connection-drops).

### Joining a running game takes a long time

The joining player's game replays everything that was played before they came: half an hour of play takes
about 50 seconds. Since version 0.16.5 this runs without being shown on the screen and about four times faster
than before. If catching up takes noticeably longer, update the mod on the joining player's PC.

### The launcher says GitHub does not answer

The mod works as before all the same: the **ИГРАТЬ** (Play) button is available, and the game does not need
the update check.

- Open the launcher's settings, the **СЕТЬ** (Network) section. **Авто** (Auto) tries a VPN client on this PC
  by itself; if you have an HTTP proxy of your own, choose **Свой** (My own) and enter `address:port`.
- If it says «GitHub отвечает, но файлы релиза не отдаёт» (GitHub answers but does not give the release's
  files), only the page opens, while the files are on another server: a proxy helps.
- The fallback: download the new `tmnt2net-launcher.exe` in a browser from the release page; the new version
  of the mod is inside it. More on the [launcher page](LAUNCHER.md#if-github-does-not-answer).

### Desync

This should not happen. The game of the player whose game went another way rejoins by itself, up to four times
in a row. If that does not help, the player leaves the session with a message.

First update everyone's mod to 0.16.6 or newer: that version fixes three causes of desync (frames of the
previous session left in memory, the "4:3" frame shape, a foreign calculation precision set by overlays).

If it happens again, send the logs that the mod saves next to the game at the moment of the desync:

- from the host's PC: `tmnt2net-desync-host.log` and `tmnt2net-desync-host-sim.log`;
- from the PC of the player whose game went its own way: `tmnt2net-desync-client.log` and
  `tmnt2net-desync-client-sim.log`.

The pair of `-sim.log` files shows the frame from which the games went different ways.

## Saves

### My progress is gone after a network game

It is not gone. In a network game everyone plays with shared progress, and it is kept separately. The mod does
not touch your single-player save `tmntsave.dat`: after you leave the session, the title menu has your own
progress again.

### No CONTINUE (ПРОДОЛЖИТЬ) in a new session

The shared progress is with whoever was in the previous session. The host must be a player who has it: their
game folder has `tmnt2net-coop.rec` or `tmntsave-net.dat` in it. More in the
[description of saves](NETPLAY.md#saves-and-progress).

## How to report a bug

Open an [issue](https://github.com/int3re/TMNT-2-BATTLE-NEXUS-COOP/issues) and attach:

1. the mod's version (the line under the title menu);
2. what you were doing and what happened;
3. the file `tmnt2net.log` from the game folder. Both logs are overwritten at every start, so copy them right
   after the error, before the next start;
4. for network play errors: `tmnt2net.log` and `tmnt2net-sim.log` from every PC, and for a desync the
   `tmnt2net-desync-*.log` files.

The log contains the path to the game folder and the nicknames of the session's players. If you do not want to
show them, remove those lines before sending.
