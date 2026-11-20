# Сборка из исходников

[← К описанию мода](../README.md)

Мод собирается под Windows компилятором MSVC. Сборка **только 32-битная**: `TMNT2.exe` — 32-битный процесс и
ничего другого загрузить не может. CMake остановится с ошибкой, если настроить 64-битную сборку.

## Что нужно

| Что | Зачем |
|---|---|
| Visual Studio с компонентом «Разработка классических приложений на C++» | компилятор MSVC, C++17 |
| CMake 3.20 или новее | система сборки |
| Исходники SDL2 | геймпады; проверено с 2.32.10 |
| Исходники ENet | сеть по UDP; проверено с 1.3.18 |
| 32-битная сборка Ultimate ASI Loader | загрузчик, попадает в установщик как `winmm.dll` |
| Steamworks SDK, по желанию | сеть через Steam; проверено с 1.65 |
| Python 3, по желанию | только чтобы пересобрать шейдер обработки кадра |

MinHook лежит в репозитории: `third_party/minhook`.

## Куда положить зависимости

SDL2, ENet и загрузчик не входят в репозиторий. CMake ищет их по трём путям, каждый можно задать при
настройке:

| Переменная CMake | По умолчанию | Что там должно быть |
|---|---|---|
| `TMNT1_DIR` | `../tmnt2003-netplay` | `third_party/SDL` — исходники SDL2 с `CMakeLists.txt`; `third_party/enet` — исходники ENet |
| `UAL_DLL` | `../tmnt2003-netplay/third_party/ultimate-asi-loader/bin/Win32/Release/dinput8.dll` | 32-битная dll загрузчика; имя файла любое |
| `STEAMWORKS_SDK_DIR` | `../steamworks_sdk_165/sdk` | папка `sdk` из Steamworks SDK: `public/steam`, `redistributable_bin` |

Пути по умолчанию такие потому, что мод делит эти библиотеки с соседним проектом для первой игры серии —
[TMNT-2003-COOP](https://github.com/int3re/TMNT-2003-COOP). Самый короткий путь: склонировать его рядом, в папку `tmnt2003-netplay`, вместе с
подмодулями и собрать в нём Ultimate ASI Loader, как описано в его `docs/BUILDING.md`. Тогда пути по умолчанию
подойдут без изменений:

```bash
git clone --recurse-submodules https://github.com/int3re/TMNT-2003-COOP tmnt2003-netplay
```

Либо собрать нужную раскладку в любой папке:

```
deps/
  third_party/
    SDL/        ← исходники SDL2 (ветка SDL2)
    enet/       ← исходники ENet
```

и указать её как `TMNT1_DIR`.

Без `UAL_DLL` собирается только `tmnt2net.asi`: установщик и `winmm.dll` в выдачу не попадают, CMake
предупредит об этом.

### Steam

Steamworks SDK распространяется Valve на своих условиях, в репозитории его нет и быть не должно. Скачать его
можно на [partner.steamgames.com](https://partner.steamgames.com/) после входа.

- **С SDK** мод собирается с сетью Steam: лобби для друзей, приглашения, соединение без проброса портов.
  В выдачу попадает `steam_api.dll`.
- **Без SDK** всё остальное собирается как обычно, остаётся игра по локальной сети и IP. CMake напишет
  `Steam: OFF`.

## Сборка

Настройка, со всеми путями явно:

```bash
cmake -B build -A Win32 -DTMNT1_DIR=C:/dev/deps -DUAL_DLL=C:/dev/ual/dinput8.dll -DSTEAMWORKS_SDK_DIR=C:/dev/steamworks_sdk/sdk
```

Сборка:

```bash
cmake --build build --config Release
```

Результат — в `build/dist/Release`:

| Файл | Что это |
|---|---|
| `tmnt2net.asi` | мод |
| `winmm.dll` | Ultimate ASI Loader |
| `tmnt2net.ini` | настройки по умолчанию, из `dist/` |
| `tmnt2net-README.md` | описание для игроков, из `dist/README.md` |
| `steam_api.dll` | только при сборке со Steam |
| `tmnt2net-setup.exe` | установщик: всё перечисленное лежит внутри него |

После сборки CMake сам проверяет, что `tmnt2net.asi` — 32-битный файл (`tools/check_x86.cmake`).

Среда выполнения C++ линкуется статически: игрокам не нужно ставить распространяемые пакеты Visual C++.

## Цели сборки

| Цель | Что собирает |
|---|---|
| `tmnt2net` | `tmnt2net.asi` и папку `build/dist/<конфигурация>` |
| `tmnt2net_common` | общая библиотека: журнал, SHA-256, транспорты, лобби Steam, протокол |
| `tmnt2net_setup` | `tmnt2net-setup.exe` |
| `tmnt2net_launcher` | `tmnt2net-launcher.exe`: [лаунчер](LAUNCHER.md) |
| `minhook`, `enet` | сторонние библиотеки |

## Установка собранного мода

Запустите собранный установщик или скопируйте файлы из `build/dist/Release` в папку игры. Из командной строки:

```bash
build\dist\Release\tmnt2net-setup.exe /install "C:\Games\TMNT 2 - Battle Nexus" /quiet
```

## Шейдер обработки кадра

Резкость и цвета делает один пиксельный шейдер. Его исходник — `tools/postfx.hlsl`, скомпилированный байт-код
лежит в репозитории как `src/mod/postfx_shader.h`. Пересобирать его нужно, только если вы меняли шейдер:

```bash
python tools/gen_postfx.py
```

Скрипт вызывает `D3DCompile` из системной `d3dcompiler_47.dll`.

## Новая версия

1. Номер версии — в `src/common/version.h`.
2. Если изменилось что-то, от чего зависит ход игры, или формат пакетов — поднять `kVersion` в
   `src/common/protocol.h`: игроки с разными версиями протокола вместе играть не должны. Подробнее — в
   [устройстве мода](ARCHITECTURE.md#версия-протокола).
3. Описать изменения в `dist/README.md` и в [истории версий](CHANGELOG.md).
4. Заполнить `dist/release.json`: версия, протокол, дата, заголовок и до четырёх коротких заметок — их
   показывает [лаунчер](LAUNCHER.md); в `protocols` добавить новую версию.
5. Собрать `Release` и сделать описание релиза для лаунчера:

   ```
   python tools/release_manifest.py build/Release/tmnt2net-setup.exe build/Release/tmnt2net-launcher.exe build/Release/tmnt2net-latest.json
   ```

6. Выложить в релиз три файла: `tmnt2net-setup.exe`, `tmnt2net-launcher.exe` и `tmnt2net-latest.json`.
   Без последнего лаунчер узнает только номер версии и поставит мод без сверки контрольной суммы.

## Лаунчер

Собирается целью `tmnt2net_launcher`, ничего стороннего ему не нужно: Direct2D, DirectWrite и WinHTTP есть
в Windows. Его версия — `kLauncherVersion` в `src/launcher/launcher.h`.

- Шрифты лежат в `src/launcher/assets/fonts` и встраиваются в exe. Их делает `tools/launcher_fonts.py`
  (нужны `fontTools` и `brotli`) из файлов Google Fonts; лицензии — рядом. Шрифт заголовков, Dela Gothic
  One, лежит там же урезанным до латиницы и кириллицы; без его файла сборка тоже проходит, и заголовки
  рисует Arial Black из Windows.
- Значок делает `tools/launcher_icon.py` (нужен `Pillow`).
- Любой экран можно нарисовать в файл, не показывая окна: `tmnt2net-launcher.exe --shot update out.png`.
  Остальные ключи проверки — на [странице лаунчера](LAUNCHER.md#разработчикам).
