# Contributing

[Русский](CONTRIBUTING.md)

Thank you for wanting to help. Here is what is worth knowing before the first change.

## Reporting a bug

The best bug report is the logs. Attach `tmnt2net.log`, and for a network problem `tmnt2net.log` and
`tmnt2net-sim.log` **from every PC** that took part, plus the `tmnt2net-desync-*.log` files if the games went
apart. Say what was going on. In detail:
[How to report a bug](docs/en/TROUBLESHOOTING.md#how-to-report-a-bug).

The log holds the path of the game folder and the nicknames of the session's players. If you would rather
not show them, remove those lines before sending.

## Building and checking

- Building: [docs/en/BUILDING.md](docs/en/BUILDING.md).
- The overall picture of the code: [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md).
- Addresses, structures and evidence for `TMNT2.exe`: [re/notes.md](re/notes.md).

```bash
cmake -B build -A Win32
```

```bash
cmake --build build --config Release
```

How a change is checked:

- **The mod.** Several copies of the game on one PC, each with its own `tmnt2net.ini`; the keys of the
  `[debug]` section are described in [Testing and debugging](docs/en/ARCHITECTURE.md#testing-and-debugging).
  The main check is to compare the copies' `tmnt2net-sim.log` after a run: the lines of the session must match
  frame for frame.
- **The launcher.** Without showing its window: `--shot <screen> <file.png>` draws a screen into a file,
  `--drive "<script>"` presses the buttons with the window hidden. A script runs on a copy of the launcher and
  with its own `--settings`, `--cache` and `--pick`, so that the real settings and the real game are not
  touched. The keys are in the [launcher's page](docs/en/LAUNCHER.md#for-developers).
- **What the bench does not cover:** everything that depends on the active window — taking the keyboard,
  activating the window, the real mouse. That is seen in ordinary play only.

## Rules that are not up for discussion

They come from bugs that were found, not from taste.

1. **`TMNT2.exe` on disk is never changed.** Patches are made in the process's memory only.
2. **Every hook checks the bytes.** The addresses in `src/mod/game.h` are right for one file of the game; the
   first bytes of the function are written next to its address. If they do not match, the hook is skipped, a
   warning goes to the log, and the rest works. The game must not crash because of the mod.
3. **No files of the game in the repository or the releases**: no exe, no assets, no saves. The same goes for
   the Steamworks SDK.
4. **Everything that affects how the game plays must be the same on every PC.** A change to the game's
   behaviour in a session takes effect on every PC in the same frame and depends on neither the wall clock,
   nor the picture, nor the state of the window, nor local settings. And `kVersion` in
   `src/common/protocol.h` goes up: versions that behave differently must not connect.
5. **The game's own functions instead of the mod's own, and instead of presses "for the player".** To change
   the video mode, load the progress or open a menu, the game's own code is found and called, the same way on
   every PC.
6. **The mod's menus are the game's menus:** in its font, in its style, by its own means. No windows over the
   game.
7. **Nothing the player made is overwritten** — keys, saves, settings — without a copy being kept. The
   progress of a network game is kept apart from the players' own saves.
8. **No real names.** Screenshots and documentation use made-up nicknames only (Леонид, Вася, Петя, Коля).
   Nicknames, paths and computer names from other people's logs never go into the repository.

## Style

- C++17, MSVC, x86. Format as the code around it.
- A comment says **why**: what the game does at that place and which bug stands behind the change. What a line
  of code does can be seen anyway.
- Findings about the game go to [re/notes.md](re/notes.md): the address, how it was verified, how certain
  it is.
- A player sees text in the language of their copy of the game: every string of the mod has a Russian and an
  English variant. Logs and code are in English.

## Documentation

- `README.md` and `README.en.md`: the repository's page in two languages.
- `docs/` holds the Russian pages, `docs/en/` the English ones under the same names. Change one, change the
  other.
- `dist/README.md`: the description that is installed into the game folder as `tmnt2net-README.md` (in
  Russian). If behaviour a player can see changes, update it too.
- `docs/CHANGELOG.md`: an entry for every version, what changed and why.
- `docs/TOUR.md`: "what is added and changed" in pictures. A new row in a menu or a new screen gets a
  screenshot there.
- The launcher's pictures are drawn by the launcher itself:
  `tmnt2net-launcher.exe --shot <screen> docs/images/launcher-<screen>.png`.
- `dist/release.json`: what the launcher shows about a version. It is filled in before the build: the file is
  built into the launcher.

The order of releasing a version is in [docs/en/BUILDING.md](docs/en/BUILDING.md#a-new-version).
