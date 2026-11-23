# Building from source

[← Back to the mod's page](../../README.en.md) · [All documentation](README.md) · [Русский](../BUILDING.md)

The mod is built on Windows with the MSVC compiler. The build is **32-bit only**: `TMNT2.exe` is a 32-bit
process and cannot load anything else. CMake stops with an error if a 64-bit build is configured.

## What you need

| What | Why |
|---|---|
| Visual Studio with the "Desktop development with C++" workload | the MSVC compiler, C++17 |
| CMake 3.20 or newer | the build system |
| SDL2 sources | gamepads; tested with 2.32.10 |
| ENet sources | networking over UDP; tested with 1.3.18 |
| A 32-bit build of Ultimate ASI Loader | the loader; it goes into the installer as `winmm.dll` |
| Steamworks SDK, optional | networking over Steam; tested with 1.65 |
| Python 3, optional | only to rebuild the frame post-processing shader |

MinHook is in the repository: `third_party/minhook`.

## Where to put the dependencies

SDL2, ENet and the loader are not part of the repository. CMake looks for them at three paths; each can be set
when configuring:

| CMake variable | Default | What must be there |
|---|---|---|
| `TMNT1_DIR` | `../tmnt2003-netplay` | `third_party/SDL` — SDL2 sources with `CMakeLists.txt`; `third_party/enet` — ENet sources |
| `UAL_DLL` | `../tmnt2003-netplay/third_party/ultimate-asi-loader/bin/Win32/Release/dinput8.dll` | the loader's 32-bit dll; any file name |
| `STEAMWORKS_SDK_DIR` | `../steamworks_sdk_165/sdk` | the `sdk` folder of the Steamworks SDK: `public/steam`, `redistributable_bin` |

The default paths are what they are because the mod shares these libraries with the neighbouring project for
the first game of the series, [TMNT-2003-COOP](https://github.com/int3re/TMNT-2003-COOP). The shortest way: clone it next to this one, into the folder
`tmnt2003-netplay`, with its submodules, and build Ultimate ASI Loader in it as its `docs/BUILDING.md`
describes. Then the default paths fit without changes:

```bash
git clone --recurse-submodules https://github.com/int3re/TMNT-2003-COOP tmnt2003-netplay
```

Or put the needed layout together in any folder:

```
deps/
  third_party/
    SDL/        ← SDL2 sources (the SDL2 branch)
    enet/       ← ENet sources
```

and give it as `TMNT1_DIR`.

Without `UAL_DLL` only `tmnt2net.asi` is built: the installer and `winmm.dll` do not get into the output, and
CMake warns about it.

### Steam

The Steamworks SDK is distributed by Valve on its own terms; it is not in the repository and must not be. It
can be downloaded from [partner.steamgames.com](https://partner.steamgames.com/) after signing in.

- **With the SDK** the mod is built with Steam networking: lobbies for friends, invitations, connections
  without port forwarding. `steam_api.dll` goes into the output.
- **Without the SDK** everything else is built as usual; play over a LAN and by IP remains. CMake prints
  `Steam: OFF`.

## Building

Configuring, with every path given explicitly:

```bash
cmake -B build -A Win32 -DTMNT1_DIR=C:/dev/deps -DUAL_DLL=C:/dev/ual/dinput8.dll -DSTEAMWORKS_SDK_DIR=C:/dev/steamworks_sdk/sdk
```

Building:

```bash
cmake --build build --config Release
```

The result is in `build/dist/Release`:

| File | What it is |
|---|---|
| `tmnt2net.asi` | the mod |
| `winmm.dll` | Ultimate ASI Loader |
| `tmnt2net.ini` | the default settings, from `dist/` |
| `tmnt2net-README.md` | the description for players, from `dist/README.md` |
| `steam_api.dll` | only in a build with Steam |
| `tmnt2net-launcher.exe` | the [launcher](LAUNCHER.md): everything listed above is inside it. The only file for players |

After the build CMake itself checks that `tmnt2net.asi` is a 32-bit file (`tools/check_x86.cmake`).

The C++ runtime is linked statically: players do not need to install the Visual C++ redistributable packages.

## Build targets

| Target | What it builds |
|---|---|
| `tmnt2net` | `tmnt2net.asi` and the folder `build/dist/<configuration>` |
| `tmnt2net_common` | the shared library: log, SHA-256, transports, Steam lobby, protocol |
| `mod_payload` | the mod's files in `build/payload`, as they go into the launcher |
| `tmnt2net_launcher` | `tmnt2net-launcher.exe`: the [launcher](LAUNCHER.md) with the mod inside |
| `minhook`, `enet` | third-party libraries |

## Installing the built mod

Run the built launcher or copy the files from `build/dist/Release` into the game folder. From a command line:

```bash
build\dist\Release\tmnt2net-launcher.exe /install "C:\Games\TMNT 2 - Battle Nexus" /quiet
```

## Frame post-processing shader

Sharpness and colours are done by one pixel shader. Its source is `tools/postfx.hlsl`; the compiled bytecode
is in the repository as `src/mod/postfx_shader.h`. It has to be rebuilt only if you changed the shader:

```bash
python tools/gen_postfx.py
```

The script calls `D3DCompile` from the system's `d3dcompiler_47.dll`.

## A new version

1. The version number is in `src/common/version.h`.
2. If something that the course of the game depends on has changed, or the packet format, raise `kVersion` in
   `src/common/protocol.h`: players with different protocol versions must not play together. More on the
   [architecture page](ARCHITECTURE.md#protocol-version).
3. Describe the changes in `dist/README.md` and in the [changelog](CHANGELOG.md).
4. Fill in `dist/release.json`: the version, the protocol, the date, a heading and up to four short notes —
   the [launcher](LAUNCHER.md) shows them; add the new version to `protocols`.
5. Build `Release` and make the release description for the launcher:

   ```
   python tools/release_manifest.py build/Release/tmnt2net-launcher.exe build/Release/tmnt2net-latest.json
   ```

6. Put two files into the release: `tmnt2net-launcher.exe` (the mod is inside it) and `tmnt2net-latest.json`.
   Without the second one launchers learn only the version number and install the mod without checking its
   checksum.

## The launcher

It is built by the `tmnt2net_launcher` target and needs nothing third-party: Direct2D, DirectWrite and WinHTTP
are part of Windows. Its version is `kLauncherVersion` in `src/launcher/launcher.h`; it has to be raised at
every change to the launcher, otherwise old launchers will not replace themselves with the new one.

- The mod's files (`build/payload`), their list and `dist/release.json` are embedded in the exe as resources.
  CMake takes the mod's version and the protocol from `src/common/version.h` and `src/common/protocol.h`.

- The fonts are in `src/launcher/assets/fonts` and are embedded in the exe. They are made by
  `tools/launcher_fonts.py` (it needs `fontTools` and `brotli`) from the Google Fonts files; the licenses are
  next to them. The heading font, Dela Gothic One, is there too, cut down to Latin and Cyrillic; without its
  file the build passes as well, and the headings are drawn with Windows' Arial Black.
- The icon is made by `tools/launcher_icon.py` (it needs `Pillow`).
- Any screen can be drawn to a file without showing a window: `tmnt2net-launcher.exe --shot update out.png`.
  The other testing switches are on the [launcher page](LAUNCHER.md#for-developers).
