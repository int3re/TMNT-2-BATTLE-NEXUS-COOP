# TMNT2.exe (Battle Nexus) — reverse-engineering notes

All addresses are for one exe; tmnt2net checks the hash before touching anything.
Names in `Class::Method` form come from the reimplementation
[xntrz/tmnt2](https://github.com/xntrz/tmnt2) (names restored from the PS2 prototype PDB),
matched to the exe by code shape and strings. Status: **confirmed** = seen in a run or
proven byte for byte; **static** = read from the code only.

## 1. Executable
- `TMNT2.exe`, 4 796 416 bytes, PE timestamp `0x4124028A`, image base `0x400000`, x86.
- SHA-256 `22f49908bcd3b84278bea888b7285e1e0ba73a72a89764f495aceb76a42d8b6a` (NA retail 2004, from a
  2021 Inno Setup repack; `TMNT2V0x.DAT`/`VOICE*.BIN` dated 2021, rest 2004). NA: no `TMNTF/G/I/S.DAT`.
- Sections: `.text .rdata .data`, RenderWare `_rwcseg/_rwdseg`, CRI Sofdec `PSFD00/_I/_P/_B`, plus
  `.ysnkvb` (0x509000, entropy 7.8, executable) and `.cuohz` (0x5D3000): encrypted leftovers of DRM.
  The game runs without a disc and with a debugger attached (no exceptions at all) — **confirmed**.
- Imports: `d3d9!Direct3DCreate9` (IAT `0x639278`), `DINPUT8!DirectInput8Create` (`0x639010`),
  `DSOUND#1` (`0x639018`), WINMM timers, user32 (`GetClientRect 0x6391F8`, `FindWindowA 0x6391FC`,
  `MessageBoxA 0x639234`, `ShowWindow 0x639248`, `SetForegroundWindow 0x63924C`, `ShowCursor 0x639250`,
  `LoadStringA 0x639254`, `SetWindowPos 0x639258`).
- Engine: RenderWare D3D9 driver, D3DX9 linked in, CRI ADX/Sofdec. 2 of the 35 TMNT1 (tmntnet)
  signatures match: `RwCameraSetViewWindow` `0x54AB50` and the Sofdec player create `0x5E95C0`.

## 2. "Unkown Error" at start — **confirmed**
- Repro (a test copy of the game, hard links): without a debugger the dialog appears after ~4.5 s.
  Under a debugger the game runs (debug heap pads blocks). Under a debugger with `_NO_DEBUG_HEAP=1`:
  `C0000005 at 0x483A3D` (`rep movsd`), read `0x04C43000`, `esi=0x6C`, `ecx=0x04C43000`.
- `WinMain` `0x53B3D0`: `try { 0x52D1C0 } catch (CException&) { MessageBox(what) } catch (...) { SetError(1) }`,
  then `0x53B000` shows `LoadString(table[code])`: 1 → id 6 "Unkown Error", 2 → id 7 "Video Initialization
  Failed" (set at `0x535654` when RW init `0x52C330` fails), 3 → id 8 "Sound Initialization Failed".
- `COptionData` ctor `0x489920` (`GameData/OptionData.cpp`): `+0x24 m_nGamepadNum = CController::Max()`
  (`0x52A8E0` → `[0x6DFF1C]` vtbl+4; `PHYSICALCONTROLLER_MAX` = 1 + joysticks: keyboard, then one per joystick; n = 1 on the test PC), `+0x28 m_pGamepad =
  new[n * 0x24]`, each entry initialised by `CGamepadOptionData::Initialize(index)` `0x483A90`
  (`{0x20,0x40,0x10,0x80,0x400,0x2000,0x800}`, byte `+0x1C = 1`, `+0x20 = index`).
- `COptionData::Snapshot(RAWDATA*)` `0x4897A0` (`__thiscall`, `ret 4`): loop `cmp esi, 0xD8` = 6 entries,
  each 32 bytes via `0x483A30`. Reads past the array when n < 6. `Restore` `0x489800` stops at min(n, 6).
  Callers `0x48732E`, `0x48755B`, `0x487816` (GameData init `0x4877xx`).
- Why some PCs: heap layout. Here n = 1: after the 0x24-byte block there were 0x48 mapped bytes and then an
  unmapped page, so the read of entry 3 (`esi = 0x6C`) faulted. With 5+ joysticks n >= 6 and nothing is read past the end.
- Fix (tmnt2net `fixes.cpp`): detour lends a 6-entry array (real entries + Initialize defaults).

## 3. Settings: `PCSetting` (`System/Pc/PCSetting.cpp`)
- `0x53AB20` (`__thiscall`, no args, called from `0x535086`): W=640 `[0x6A0D3C]`, H=480 `[0x6A0D40]`,
  depth 32 `[0x6A0D44]`, **window mode** `[0x6E0158] = 0`, then `0x53A790` reads `<exe name>.ini`
  (`GetModuleFileNameA` + `_splitpath` + `.ini` → `TMNT2.ini`) `[SCREEN] WIDTH/HEIGHT/DEPTH`.
- `[0x6E0158]` = `m_bWindowMode` (`CConfigure::CheckArg("wnd")` in the reimplementation). The retail build
  never sets it; the rest of the window path is intact (below).

## 4. Video: `CPCGraphicsDevice` (vtable `0x663D40`)
| slot | addr | what |
|---|---|---|
| +0x04 | `0x535630` | init: RW init `0x52C330` (error 2 on failure), then PlaceWindow(mode w, h) at `0x5356F4` (returns to `0x5356F7`) |
| +0x30 | `0x5354E0` | hwnd `[0x6E0174]` |
| +0x34 | `0x535B90` | choose start mode: `+0x2C fullscreen = !m_bWindowMode`; `+0x28 = FindMode(W,H,D,0)`, assert if < 0 |
| +0x38 | `0x5354F0` | RW video mode index: fullscreen → `adapter.modes[adapter+0x58]+0x18`, window → `adapter+0x60` |
| +0x58 | `0x535C70` | `FindMode(w, h, depth, bCurrentAdapterOnly)` → adapter index (sets `adapter+0x58`), -1 if none |
| +0x5C | `0x535E30` | `PlaceWindow(w, h)` (`ret 8`): window: style `0xCA0000`, ex `0x200`, centred, `SetWindowPos(HWND_NOTOPMOST,…,SWP_NOACTIVATE)`; fullscreen: `WS_POPUP`, topmost |
- Object: `+0x20` adapters (0xA8 each), `+0x24` count, `+0x28` current adapter, `+0x2C` fullscreen, `+0x34` highreso.
- Adapter: `+0x50` modes (0x60 each: w, h, depth, flags, refresh, format, `+0x18` RW index, `+0x20`
  label `"%4d x %4d x %2d"`), `+0x54` count, `+0x58` current, `+0x60` RW window mode index, `+0x64` has window mode.
- List builder ~`0x535FA0`, **filter** `0x536079–0x5360F2`: exclusive flag; `w>=640 && h>=480`; unless highreso
  `w<=1024 && h<=768`; `w<=1600 && h<=1200`; `w*3 == h*4`; depth 16/32. tmnt2net: `0x5360A0: EB 44` (jump to
  the depth check `0x5360E6`). Same shape as TMNT1 (`0x4B0376`).
- Runtime mode change `0x535A30(w,h,depth)` (menu ГРАФИКА): FindMode(current only) → PlaceWindow `0x535ACC`
  → RW mode. Wrapper `~0x534C80`: in window mode hides the window, changes, `ShowWindow(SW_SHOW)` +
  `SetForegroundWindow`; fullscreen: `ShowCursor(0)`.
- RenderWare API used at init: `RwEngineInit 0x547FB0`, `RwEngineOpen 0x547DB0`, `RwEngineSetSubSystem
  0x547B70`, `RwEngineSetVideoMode 0x547C30`, `RwEngineStart 0x547CB0`, `GetNumVideoModes 0x547BA0`,
  `GetVideoModeInfo 0x547BD0`.

## 5. Window, message loop
- `0x53B9A0`: `FindWindowA("TMNT2","TMNT2")` → `SetForegroundWindow` + exit (single instance);
  `RegisterClassA` (WndProc `0x53B290`, black background brush), `CreateWindowExA(0, "TMNT2", "TMNT2",
  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT…)` → `[0x6E0174]`, `SetForegroundWindow`. hInstance `[0x6E0170]`.
- WndProc (jump table `0x53B39C`, index `0x53B3B0` for msg 6..0x20): `WM_ACTIVATE` → `[app+0x74] = active`
  (app object `[0x6E017C]`, ctor `0x53B060` sets it to 1) and `UpdateWindow`; `WM_PAINT` → Begin/EndPaint
  + redraw; `WM_CLOSE` → window mode: `DefWindowProc` (destroys the window but nobody posts WM_QUIT),
  fullscreen: ignored; `WM_SETCURSOR` → `SetCursor(NULL)` everywhere (title bar too).
- Message pump `0x53B4B0`: `PeekMessageA(PM_REMOVE)`; `WM_QUIT` → leave; no message and
  `[app+0x74] != 0` → run a frame.

## 5a. More start-up and shutdown bugs found with tmnt2net 0.1.0 runs — **confirmed**
- **Keyboard without focus**: `CPCKeyboardController::Start` acquires the keyboard right after
  `SetCooperativeLevel(NONEXCLUSIVE|FOREGROUND)` (`0x5375C4`); `0x537611 jge` → when the window is not in front
  `Acquire` fails, Start returns false, `CPCInputsDevice` keeps `m_ppPhysicalController = NULL`, and the first
  `Sync` asserts `m_ppPhysicalController != NULL` (`PCInputsDevice.cpp:56`, from `0x53637F`) → "Unknown error.
  Program is ended compulsorily." Real-world trigger: Alt+Tab while the game loads. Fix: `jge` → `jmp` (the
  reimplementation dropped the check too; Update re-acquires).
- **ASSERT** `0x4019A0(expr, file, line, "Unknown error.")`: vsprintf of the last argument into `0x6C4000`, throws;
  expression/file/line are passed but never shown. tmnt2net logs them.
- **ScreenWidth/Height** (vtable `+0x24` `0x535760`, `+0x28` `0x535850`) return `adapter.modes[adapter+0x58]`;
  in window mode that is a leftover (640x480 at start) and the 2D layer drew into the top-left 640x480 of a
  960x720 frame. tmnt2net reports the frame size there.
- **TMNT2.ini save** `0x53A9E0` (`__thiscall`, plain `ret`; via thunk `0x53AB60` from `0x534B65` in Terminate
  `0x534AB0` and `0x5352F2` in the init failure path of `0x534FD0`): writes the current W/H/depth. With tmnt2net
  that would be a size the stock game cannot start with (after uninstall → assert at start); the save is
  hooked to keep a stock 4:3 size.
- **Quitting by WM_QUIT** (close button / Alt+F4, which the retail game never handles): shutdown during a movie
  faults in `ntdll+0x5FB03` (write `0x14`, critical section of a freed object), caught by WinMain → "Unkown
  Error". At the logo screens it exits cleanly. tmnt2net: no dialog on the way out, process ends with 0.
- Present with `D3DPRESENT_INTERVAL_ONE` to a window placed off the desktop is throttled by Windows to ~7 fps
  (test runs use `vsync = 0`: 50–55 fps, the game's own limiter).

## 5b. tmnt2net 0.1.0 test runs (copy `test-game2`, window off-screen, never activated, `autoquit`)
| run | result |
|---|---|
| windowed 960x720 | device windowed 960x720 (RW asks 0x0, swap COPY x1); first frame; ASSERT keyboard (no focus) |
| + keyboard fix | 40 s of logos/intro, `gamepad snapshot: n=1, padded to 6`; 2D in top-left 640x480 |
| + ScreenWidth/Height | logo fills 960x720 |
| borderless | window 1920x1080, frame 1440x1080 shown at 240,0; intro movie full frame |
| Alt+Enter x2 | windowed → borderless (frame scaled into 1440x1080 at 240,0) → windowed |

## 8. Widescreen (tmnt2net 0.2.0) — **confirmed** in guarded runs
- RenderWare: `RwEngineInstance` `[0x6EE738]` (→ RwGlobals `0x6EE608`), device table +0x20 RenderStateSet,
  +0x24 RenderStateGet, +0x30 Im2DRenderPrimitive (same layout as TMNT1). `RwCameraSetViewWindow` `0x54AB50`
  (camera +0x60 frameBuffer, +0x68 view window, +0x70 recip; raster +0xC w, +0x10 h). The game sets `(x, 0.75x)`.
- CScreen statics `s_iWidth [0x6DFFA4]`, `s_iHeight [0x6DFFA8]`, depth `[0x6DFFAC]` (from the device's
  ScreenWidth/Height). Frame buffer raster = min(RW video mode, ScreenWidth): in window mode RW's mode is
  the desktop, so a frame larger than the desktop fills only part of it (tmnt2net clamps the frame).
- CSprite: virtual screen `[0x6A08D4..E0]` (x, y, w, h), written only by `SetDefaultVirtualScreen 0x52E4A0`;
  sprite fields +4 x, +8 y, +0xC z, +0x10 w, +0x14 h, +0x18/+0x1C anchor, +0x20 RGBA, +0x24.. UV;
  Draw `0x52E960` (x0 = x - anchor*w). `CSprite::SetPositionAndSizeRealScreen` places in pixels (player markers).
- `CSystem2D::Initialize` (`~0x52ED00`): `Rt2dDeviceSetMetric(-320, -240, 640, 480)` as literals (`0x5B4C50`, only
  caller `0x52ED52`), then SetDefaultVirtualScreen, `Rt2dDeviceSetLayerDepth(1.0)` `0x5B4F70`. Text layout reads
  the metric (`Rt2dDeviceGetMetric`), so the metric and the virtual screen must be widened together.
- `CAnimation2D::Start` `0x4FA2B0` (`this+4` Rt2dMaestro): GetBBox `0x5BD2E0`, GetScene `0x5DE8F0`,
  DeviceGetStep `0x5B4B10`, ObjectMTMScale `0x5B6040`, ObjectMTMTranslate `0x5B6090` (local units),
  scale = step·s_iWidth/bbox.w. tmnt2net: s_iWidth = w43 for the call, then translate by (W−w43)/2·bbox.w/w43.
- Joining a running stage (tmnt2net 0.12.0, midjoin.cpp; addresses in game.h "joining a running stage"): the team
  is the CGamePlayParam at 0x6D3DA8 (chara infos {id, costume, player} from +0x14, count +0x10; players +0x44;
  contexts 32 bytes from +0x48). Player n always uses controller n; CController::Lock(c) 0x52AB40 moves controller
  c to the first unlocked place, so the locked controllers are the team's in order. CPlayer (CGamePlayer +0x10)
  keeps up to 4 characters {chr, manipulator, slot number} from +0x28 (count +0x58, current +0x5C); ~CPlayer
  deletes the manipulators only. A sleeping character moved from one CPlayer to a new one (AddCharacter 0x4D2090)
  keeps the games identical. The stage's camera updater (default 0x48DD60, boss 0x48DE60) runs first in
  CGameStage::Period, before the objects, and not while paused.
- Draw-only view (tmnt2net 0.11.0): RwCamera +0x68 view window; `RwCameraSetViewWindow 0x54AB50` keeps a camera's
  frustum and projection from it, so a view window set at BeginScene and put back (with the camera's update redone)
  before EndScene changes the picture only. RenderWare's D3D9 device: auto depth stencil (format 75 = D24X8?) —
  MSAA on the present parameters covers it.
- Own camera (tmnt2net 0.10.0). `CCamera::BeginScene` `0x52A610` / `EndScene` `0x52A670` (+4 RwCamera, +8 the one
  current before, static current `[0x6DFF08]`) are generic; the stage draws inside the map camera's scene
  (`CGameProperty +0x1C` → CMapCamera, +4 CCamera). RenderWare in release calls the update functions through the
  camera (+0x18 begin, +0x1C end), so `RwCameraBeginUpdate 0x54A8D0` has only 3 callers. The graphics device's
  BeginScene `0x52C570` (vtable 0x663D40 +0x14) uses another RwCamera (device +0xC). CMapCamera: +8 eye, +0x14 at,
  +0x44 requested zoom, +0x48 mode, +0x50 area name, +0x74 introduction; Update(at, zoom) `0x504D30`, LookAt
  `0x503B40` → RwFrameTransform `0x54C950`. `CWorldMap::CheckCollisionLine(from, to)` `0x507A90`, result (0x1D dwords,
  closest point first) at `0x6D73C8` (GetCollisionResultClosestPoint `0x507630`). Logic runs outside the stage's
  scene: a render-only camera (frame moved for the draw, put back and the camera's update redone before its
  EndScene) keeps two copies with different views identical.
- Player status objects: state machine `[chr+0x174]`, current status `+0x18` (id `+0xC`); CStatusDamage (sleep, stun,
  dindle, bind…) OnAttach `0x4CEB20`: end time `+0x10` = chr trouble time `+0x13C`, IDLE when the status duration
  reaches it. Guard statuses 14..17. IGamePlayer +0x2C GetCurrentCharacterID (retail ids: 0 Leo, 1 Raph, 2 Mikey,
  3 Don, 4 Splinter, 5 Casey, 6 Karai, 7 Slashuur). `CGamePlayResult::AddTechnicalAction(chr, act)` `0x4888F0`
  (callers: CGameEvent damaged `0x48AF70`, `0x48B151`, `0x48B1F5`).
- Stage gauges (tmnt2net 0.9.1, `gauges = corners`). The manager runs `0x492D10` (status Run `0x4957A0` →
  `0x494E10`, then unless mode 3 the ring Run `0x494000` → `0x493CB0` and `0x494280`) and draws `0x492D40`
  (status Draw `0x496AD0` → `0x496980`, ring Draw `0x494180` → `0x494010`, `0x494D20`). Ring `CGaugeMeter`
  `[0x6D491C]`: sprites placed with `CSprite::SetPosition 0x52E740` (virtual → pixels at the call) in the first
  Run (ring at (-224, -152), 128×128, anchor 0.5) and in Draw (pieces). `CGaugeStatus` `[0x6D4924]`: boxes go left
  from x `[+0x340]` = 270 (y `[+0x344]` = -200, set at `0x494E5D`), `StatusWakuDispSub(n, x, y) 0x495840` returns the
  next x; slot 0 of the icon rows `0x4966A0` (flags `[+0x364+i]`) starts at x `[+0x348]` = -93, y `[+0x34C]` = -184
  beside the ring, slots 1–3 at the bottom (x -194 + 200·(i−1), y 181). `0x496820` also calls PlayerMarkerDisp
  (markers in pixels), so the boxes are moved through `[+0x340]`, not the virtual screen. tmnt2net: the ring's Run and
  Draw run with the virtual screen's x moved by d = −x − 320 (106.7 at 16:9); `[+0x340]` = 270 + d, `[+0x348]` = −93 − d.
- `CPCMovie::Draw` `0x536C10`: PushRenderState `0x52D7A0`, TEXTURERASTER = movie raster, sprite `[this+0x98]`
  drawn in pixels over the frame; tmnt2net narrows it to 4:3 for the draw.
- Pacing: `CPCGraphicsDevice::Flip` (vtable +0x1C `0x535710`) → `CPCFrameTimer::Update 0x5353A0` (Sleep until
  16 ms after Sync; `[this+4]` frame time, `[this+8]` sync) → `RwCameraShowRaster 0x52C650` → Sync. With the default
  15.6 ms timer ~53 fps; the CRI sound library calls timeBeginPeriod only around its own waits (`0x5D8BF2..`).
  tmnt2net: `timeBeginPeriod(1)`, `0x5353A0: ret`, exact 60 fps limiter at Present → 60.0 fps.
- WndProc keyboard: arrows/Enter(START)/Esc(SELECT) fixed (KeyboardOptionDataWin), menu OK = START|RDOWN.
  `CPCKeyboardController::Update` treats a failed Acquire as "unplugged".

## 9. Input (tmnt2net 0.3.0) — **confirmed** in guarded runs
- `CPCPhysicalController::Initialize` `0x537F00`: `DirectInput8Create` (thunk `0x5479B0`) → `[0x6E00FC]`;
  `EnumDevices(DI8DEVCLASS_GAMECTRL, 0x5378A0, &joyinfo 0x6E0078)`; joyinfo = 32 JOYSTICKSTATE* + count `[0x6E00F8]`;
  keyboard controller (0x668 bytes, Start vtbl+0x14) last; `PHYSICALCONTROLLER_MAX` (int16) `[0x6A0D2C]` = joysticks + 1.
- Enum callback `0x5378A0`: accepts devType 0x14/0x15; CreateDevice, SetDataFormat(c_dfDIJoystick2), SetCooperativeLevel
  (EXCLUSIVE|FOREGROUND), GetCapabilities, EnumObjects(axes → SetProperty RANGE ±32767), EnumObjects(FFACTUATOR|AXIS)
  → vibration via CreateEffect (failure drops the device).
- Joystick update (`~0x537040`): **`IDirectInput8::GetDeviceStatus(guid)` (vtbl+0x14) every frame** (≠0 → unplugged),
  Poll (vtbl+0x64) / Acquire loop on DIERR_INPUTLOST, GetDeviceState(0x110 = DIJOYSTATE2). Buttons 0..11 →
  RDOWN 0x20, RLEFT 0x40, RRIGHT 0x80, RUP 0x10, L1 0x400, L2 0x800, R1 0x2000, R2 0x4000, SELECT 0x100,
  START 0x200, L3 0x1000, R3 0x8000; POV0 → d-pad; analog: lX, −1−lY, lRz, −1−rglSlider[0]; dead zone
  `[0x6A0D34]·[0x6A0D30]`.
- Pad defaults (`CGamepadOptionData`): RDOWN attack A, RLEFT attack B, RUP shuriken, RRIGHT jump, L1 dash,
  R1 guard, L2 chara change, R3 camera, L3 gauge; OK = attack A|START, cancel = jump|SELECT.
- Keyboard defaults (`CKeyboardOptionDataWin`): fixed arrows/Enter/Esc; F, D, R, Space, E, S, LShift, F1, F2,
  numpad 8/2/4/6.
- Players join at character select with START, one per controller; only connected controllers get a frame.
- tmnt2net `pads.cpp`: virtual `IDirectInputDevice8A` per slot (SDL controllers, keyboard halves, debug SDL
  virtual pads); SDL hints: DirectInput/RawInput/WGI off, XInput + HIDAPI on, positional buttons, background
  events; hidden physical pads by VID/PID (XInput: "IG_" raw input paths).

## 10. Determinism (stage 3, tmnt2net 0.4.0) — **confirmed** in guarded runs of two copies
Frame: `CFramework::Flip` `0x52C150` = Dispatch (logic of frame N) → graphics Flip → `CPCInputsDevice::Sync`
(vtable `0x663CB0` → `0x536510`: every controller's Update, Compaction, virtual controllers) → file manager Sync.
Every DirectInput read of the game happens inside that Sync, so the input of frame N+1 is fixed there.

What made two copies differ, and the fix (`sim.cpp`, `[debug] determinism = 1`):
| source | where | fix |
|---|---|---|
| drawing skipped when late (drawn effects call `RpRandom`) | `CPCFrameSkipController::Sync` `0x534D5C` | `je` → `jmp`: never skip |
| file loads take a disk-dependent number of frames (ADXF, one file per frame) | file manager `[0x6E0050]` vtable[3] `0x52FDD0`, `+4` busy | loop Sync until idle inside the frame |
| sound data loads on the sound thread | `CGameSound::IsLoadEnd` `0x519EB0` (`SoundGetLoadInfo` `0x519E50` == 0); callers `0x4E6740`, `0x4F8039`, `0x4F918A` | wait inside the frame (the PC start does the same in a loop at `0x53AE02`) |
| a movie ends on real time (Sofdec) | `CMovie::IsEnded` `0x52D540`, only caller the movie sequence `0x4F9413` | answer from the frame's input (`kMovieEnded` + movie number) |
| a voice is still playing (sound thread) | `CGameSound::IsVoicePlaying` `0x519EC0` (`SoundVoiceBusyCheck` == 1); callers seesaw `0x4B03DC`, stage sequence `0x4F83FA`, voice manager `0x51DC72`/`0x527D1D` (voice choice draws `Rand`) | answer from the frame's input (`kVoiceBusy`) |
| controllers plugged / unplugged | `GetDeviceStatus` every frame | lockstep: always 4 virtual pads, none of the PC's own; state and "connected" from the frame's input |

- Movies: `CMovieManager` keeps `new CPCMovie(640, 448, 0x200000, false)` (ctor `0x536B40`) at `[0x6D74C4]`;
  movies are numbered by constructions, so a late "ended" never ends the next movie.
- Real time elsewhere: none found in logic. Elapsed time is `CScreen::TimerStride` = 1/60 (flip interval of the
  game's device, not of D3D), the frame-timer wait is already off (`0x5353A0`).
- Seed: `RpRandomSeed(123456)` before the first frame and again when an attract demo starts; in story play
  the generator then just runs on (5 min of play: one epoch).
- **Camera is logic.** Enemy AI activity (`FrustumSphereTest || Rand <= ratio`), AI line of sight
  (`IsPosVisible`), item and event gimmicks, and the co-op zoom (`CalcNiceZoom`) use the main RwCamera's
  view matrix and frustum. The Hor+ view window therefore changes the simulation: every copy in a session must
  use the same view window (aspect and fov of the session; frames of other shapes get bars). A camera of
  its own for each player cannot be had in the logic without desync: the game has one camera that keeps all
  players in view. tmnt2net 0.10.0 draws each PC's frame from its own view and leaves the logic camera alone (§13).
  Measured: A 16:9 / B 4:3 on the same input diverge at frame 2718, the first frame of the stage (37 vs 27
  `RpRandom` calls); A 1280x720 / B 1600x900 (both 16:9) stay identical.

Test tools: `[debug] frame_script` (keys on exact frames), `record` / `replay` (FrameInput file, the replay
follows a recording that is still being written), `rng_mutant_frame`; `tmnt2net-sim.log` per frame:
`frame epoch rng_calls rng_hash input_hash`.

## 11. Network play (stage 4, tmnt2net 0.5.0) — **confirmed** with two copies over ENet
- Session start: the first `CPCInputsDevice::Sync` (frame 1) holds in `netplay::lobby()` and presents black
  frames with the overlay until the host starts. Every copy then runs frame 1 from the same boot state. Waiting
  inside Sync is safe: loads and the sound thread are synchronised on query (§10).
- Save: `0x538FD0` builds the save path for every read (`0x53968F`) and write (`0x539557`); its
  `push "tmntsave.dat"` at `0x53900B` is pointed at `"tmntsave-net.dat"` on clients before frame 1 runs.
  The save is first read after the logo, so the patch is in time.
- Input in a session: the game's keyboard reads nothing; each PC's keyboard (game defaults from
  `CKeyboardOptionDataWin::assignDefaultKey`: F/D/R/Space/E/S/LShift/F1/F2, arrows / numpad 8246,
  Enter = START, Esc = SELECT) and gamepads become that player's pad; pad p of the bundle goes to virtual pad p.
- Measured: host + client on one PC, ENet, delay 2: 5840 frames identical; the host's frame time held at
  16.7 ms (it waits in Sync for the client's input, as lockstep should).

## 12. Menus, font, texts (stage 5, tmnt2net 0.6.0) — **confirmed** with frame captures
- Title menu (`Title.cpp`): items at `0x69E048`, 5 × 16 bytes `{u8 row, u8 visible, u8 enabled, u8 -, i32 text,
  f32 y, i32 next}`: new game (0x232, y 80, next 0), continue (0x233, 110, 1), options (0xD, 140, 2), arcade
  (0x234, hidden, 170, 4), quit (0xE, 170, 3; shown at attach `0x4EB2D1`). GetSelect `0x4EB1D0`, DrawMenu
  `0x4EB380` (CSystem2D Begin/End, font height 20, colours: cursor green pulse, enabled yellow, disabled 0x60),
  UpdateMenu `0x4EB760` (counts visible rows with 5 absolute byte reads, assert count < 5, OK sets
  `[this+0x38] = next` and fades out via vtable +0x58), Branch `0x4EB4C0` (jump table `0x4EB5D0` by next:
  new game, continue, options `Ret(9)`, quit `Ret(EOL)`, arcade, demo). 24 immediates name the table (list in
  `menus.cpp`); moved into the mod with 6 records, count start 1→2 (quit is the 6th), assert limit 5→7.
- Font: `CGameFont::m_pFont` `[0x6D4E80]` (`+8` Rt2dFont), `SetRGBA` `0x52B760` (thiscall r,g,b,a),
  `Show` `0x52B9B0` (thiscall text, height, x, y; 2D space x −320..320, y −240 top..240),
  `Rt2dFontGetStringWidth` `0x5B30D0`; CSystem2D `[0x6DFFE0]` Begin `0x52A610` / End `0x52A670`.
  `CGameText::GetText` `0x527E40` (cdecl id → UTF-16 from Text.lpac via CTextData `0x6DFEE8`).
- The Russian release (this copy) is a font hack: texts are Latin capitals, `mainfont.met` draws Russian
  capitals there — A А, B В, C С, D Д, E Е, F Г, G Ж, H Н, I I, J Ь, K К, L Л, M М, N И, O О, P Р, Q Ц, R Я,
  S Ы, T Т, U Ю, V Ф, W Ш, X Х, Y У, Z П; small Latin letters stay Latin; codes 160+ are button icons.
  No Б, З, Ч, Щ, Ъ, Э. "НОВАЯ ИГРА" = `HOBAR NFPA` (detection), quit = `YNTN` ("УИТИ", a translation slip).
- Controller in menus: `GetDigitalTrigger(port, mask)` `0x52AFE0` (port −1 = any; d-pad 1/2/4/8; OK
  `[0x6A0898]`, cancel `[0x6A089C]`); menu sounds `0x4FAAF0(i)`.
- Frame drawing: `CFramework::Render` `0x52C0E0` = device RenderBegin (vt +0x14), `CProcessDispatcher::Draw`
  `0x532910`, RenderEnd (vt +0x18). Text drawn after the dispatcher's Draw is on top of every process.
- Drawn effects call `RpRandom`, so the game must never draw an extra frame in a session (a "waiting" screen
  re-presents the last frame with the overlay instead of re-rendering).

## 13. Co-op details (tmnt2net 0.7.0–0.10.0)
- Character select `CPlayerSelectWorkPool` (Move `0x4E2A00`): players at `+0x1C0`, 0x58 each (`+0` controller u16,
  `+4` cursor, `+8` secret, `+0xC` state, `+0x10` picks allowed, `+0x38` picks made, `+0x4C` cursor shown,
  `+0x50` animation dir); characters at `+0x180`, 8 each (`+0` enabled, `+1` taken). `PushStackCharacter`
  `0x4E1780` refuses a taken slot (`[+0x181 + slot*8]`); the slot → PLAYERID table is {0,1,2,3,7,5,6,4}
  (Slashuur and Splinter swapped). `PopStackCharacter` `0x4E18C0`, `UnselectCharacterSearch` `0x4E10A0`.
- Area map `CAreaWorkPool::KameIconDisp` `0x4DCCC0`: texture `[+0x7C]`, position `+0x44/+0x48`, `+0x58`
  area-cleared animation, `+0x160` animation (1 = moving: no arrows), `+0x54` current area, sprite `+0x114`.
- Player tags in the exe (0.7.3 draws the name instead): `PlayerMarkerDisp` builds the tag itself, no CSprite:
  clamp to the frame (16 / 18 px from the edges), overlap shift for off-screen tags, then one quad
  `x−16..x+16, y−18..y` (constants 16 `[0x653C88]`, 18 `[0x646474]`, 32 `[0x63D198]`, UV row 22/128 per
  player) with raster `[this+0x338]`, zwrite 1, blend 5/6, drawn by `call [RwEngineInstance+0x30]`
  (Im2DRenderPrimitive(4, vertices, 4) at `0x496483`; vertex 0x1C: x, y, z, rhw, ARGB = character colour,
  u, v); off screen a second quad, the arrow (`0x496680`), which sets its own raster and blend. Called
  from the gauge draw inside the stage camera's scene (`CCamera::BeginScene` `0x52C5xx` sets the current
  camera `[0x6DFF08]`), before the pause handler and the message windows. CSystem2D Begin `0x52A610` ends
  the current camera's update and starts the 2D camera; End `0x52A670` restores the saved one (one level).
- Health (0.8.0 splits it per player). `CGamePlayerSharedProperty` is static: `[0x6D48A4]` (> 0 with a
  secret: damage ignored), `[0x6D48AC]` refs, HP `[0x6D48B0]`, max `[0x6D48B4]`; every access is inside
  GamePlayer.cpp. CGamePlayer: player number `+8`, CPlayer `+0x10`, total damage `+0x14`; vtable `0x64A4C0`
  +0x38 GetHPMax `0x48BFD0`, +0x3C GetHP `0x48BFF0`, +0x50 AddHP `0x48C350` (invincibility `0x486560` /
  `[0x6D48A4]`, damage count, clamp, at 0: comeback `0x485960` → refill `0x48BCC0`, else
  `CGameStage::GetCurrent` `0x48CB60` vtable +0x10 NotifyPlayerDead(n) at `0x48C40E`), +0x5C SaveContext
  `0x48C4D0` (ctx+4 HP), +0x60 LoadContext `0x48C500` (HP ctx+4, max ctx+8). Setup(n, CPlayer*) `0x48C650`
  (n == 0 zeroes HP and max). The health ring `CGaugeMeter::GaugeUpdate` `0x4930F0` reads Player(0) at
  `0x493102/12B/14F/1A4`; the result's remaining HP reads Player(0) at `0x48E061`. CPlayer vtable `0x6562F4`
  (+8 Run `0x4D2310`, `+0x60` asleep, `+0x24` controller). CPlayerCharacter: `+0x13C` trouble time (damage
  statuses' length, `CStatusDamage::OnAttach` `0x4CEB20`), `+0x168` player flags, `+0x16C` attributes (8 =
  invincible, checked at `0x4D4412` in OnDamage), statuses STUN 61 / SLEEP 62 / BIND 63 (party attack wakes:
  `0x4D3D20`). Status box `StatusWakuDispSub(n, x, y)` `0x495840` (thiscall, ret 0xC, float; hidden while
  `[this+0x15]`).
- Network play, 0.9.0. Joining a running session: every bundle is kept (`g_all`), a late player gets the
  start save + `PT_History` and replays from frame 1; the frames are not presented (every 30th is) and not
  limited; CRI movies otherwise wait 1/60 s per frame in 0x5D8D60 (called from the movie server 0x5EACB0 at
  0x5EACB5, WaitForSingleObject on a 60 Hz timeSetEvent event `[0x80A938]`; the QPC branch 0x5D9530 when
  `[0x6F70AC]` is set; another caller 0x5EC6A0). Saves: `CSaveLoadDataBase` header {version, size, checksum},
  then `CGameRecord::RAWDATA` starting with `m_uTimestampInit` (wall clock): co-op saves written in lockstep
  differ only in such stamps (offsets 8, 14..15, 0x120 in a 1412-byte save).
- Secret characters (tmnt2net 0.10.0 check): a select place holds a turtle and its partner, Up/Down toggles
  (`[pool+player+8]`, pool characters index place+4): Leo/Slashuur, Raph/Casey, Mikey/Karai, Don/Splinter (retail
  character ids 7, 5, 6, 4). The partner is offered once its secret (1 Casey, 2 Splinter, 3 Karai, 4 Slashuur) is
  unlocked in the save; a pick locks the place (`[pool+0x180+place*8+1]`), so a turtle and its partner never play
  together. In a session the host's co-op save decides what is unlocked. `[debug] all_characters` (hook
  IsUnlockedSecret 0x486560) unlocks them for test runs: two copies, P2 as Splinter, identical (5186 frames), name,
  own camera, sleep, dance and results with Splinter.
- Character select costumes in the exe (differs from the reimplementation): per player bytes
  `[pool+0x320+i]` (button 0x2000 held → samurai if `IsCostumeTaken(cursor, 1)` 0x4E1510: secrets 0xC..0xF)
  and `[pool+0x324+i]` (button 0x400 → nexus, secret 0x10+turtle checked inline at 0x4E2C58, the id pushed
  before `0x487640 → 0x4896A0 → IsUnlockedSecret 0x486560`, thiscall ret 4). Panel i = controller i.
- Stage camera zoom `CMapCamera::CalcNiceZoom(positions, n)` `0x503C90` (__thiscall, ret 8, float): 1..3
  (cap `[0x65CD08]`); `[debug] camera_zoom_max` clamps it (0.3 brings the camera close enough to push
  players off screen in the first training room).
- Player tags `PlayerMarkerDisp` `0x495F90` (player number in a byte), `CGameProperty::Player` `0x48CA70`
  (vtable +0xC → RwV3d*), `GetCameraViewMatrix` `0x48CE10`; `CController::GetPhysicalPort` `0x52AD00`:
  joysticks are ports 0..n-1 in DirectInput order (tmnt2net's pads first), the keyboard is the last port.
- Text over a 3D scene needs RenderWare's Z test and Z write off (RwEngineInstance +0x20 set / +0x24 get,
  states 6 and 8), as the game's HUD does.
- In-stage transitions (reimplementation): `CPlayerAreaCheckGimmick` → `CCameraChangeGimmick`: 1 s fade
  out, `ReplacePlayers` (ring of radius 1.5), camera snap, fade in. No scripted jump: the one who walks or
  jumps into the zone does it with their own controls. Enbu (`EnbuProc`) holds one model (the MVP).
- Enbu, the dance after an area (0.7.1: the whole team dances). `CEnbuProc_Container` (0xD4) in `[0x6D4DF8]`:
  `+0` PLAYERID, `+4` personal rank (1 E … 5 A … 7 SS; motion names "%s_%s1/2" use it), `+8` total rank,
  `+0xC` costume, `+0x10` rank value (0 high, 1 normal, 2 low = E), `+0x14` CModel* (`+4` position, `+0x10`
  rotation, `+0x40` clump; UpdateFrame `0x50C030` __thiscall(clump)), `+0x18` motion wrapper, `+0x1C`
  CCharacter, `+0x20` bandana, `+0x24` motion 1 over, `+0x25` bandana on, `+0x38..` textures, `+0x64` sprite.
  The retail ctor `0x4F01E0` has no test override: PlayResult `0x6D4430` GetMVP `0x488330`,
  GetPlayerCharacter `0x4889B0`, GetPersonalRank `0x488AE0`, GetTotalRank `0x488350`, IndexOfChara
  `0x488280` (team PLAYERIDs at `+0x88`, 8 = nobody); PlayParam `0x6D3DA8` GetCostume(i) `0x487FD0`.
  Initialize `0x4F0C70` (only if the global is null), Settings member `0x4F0D40` (static `0x4F14D0`),
  Period member `0x4F03A0` (static `0x4F0CE0`; rank ≥ 5 Leo..Don shout SE 0x2000+id through the call at
  `0x4F0517` to PlaySE `0x51A1C0`), Draw3D `0x4F0CF0` (static; saves the cull mode to `0x6DFF78`, never
  restores it), Draw2D `0x4F1510`, Terminate `0x4F14E0`, GetEnbuMvp `0x4EFA30`, costume `0x4EFA50`.
  `CEnbuSequence` vtable `0x65ADF8` (ctor `0x4F1520`, Instance `0x5296C0`, process label 13): OnAttach `0x4F1560`
  (slot `0x65AE2C`) inlines LoadEnbu: file id by PLAYERID/costume (Leo..Don 0xDD+id, samurai 0xE5+id, nexus
  0xE9+id; Slashuur 0xE4, Casey 0xE2, Karai 0xE3, Splinter 0xE1; paths at `0x6A0C94` + (id−0xDD)·4) →
  `CDataLoader::Regist` `0x4FEF40`. Each Enbu lpac creates the motion set "enbu"; `CMotionManager`
  (container `[0x6D74B8]`): CreateMotionSet `0x50EA80` (called by the binary loader at `0x4FE1D7`/`0x4FE1ED`),
  SetCurrentMotionSet `0x50EAA0`, FindSet `0x50E5B0`, CreateMotion `0x50EAC0` (asserts if the motion is
  missing — a second set of the same name is never searched). CSequence::Call(label, param) `0x5341F0`,
  Ret(param) `0x5343B0`. The title's Branch case 5 (idle → demo) is where an idle test run lands.
- In-stage transitions in the exe (0.7.2: everyone on the ground jumps as the fade starts).
  `CCameraChangeGimmick::PostMove` `0x49DE90`: step `+0x2C` (1 BlackOut(1 s) `0x50B140`, 2 wait, 3
  ReplacePlayers `0x49DD50` + map camera snap + event back to the sender `+0x4C`, 4 BlackIn, 5 wait, 6
  release); `+0x30` target, `+0x3C` facing vector, `+0x48` rotation Y; ST43N (stage 0x2E) also fades the
  music. ReplacePlayers keeps the reimplementation's ring (45/315/135/225°, radius 1.5) as stack immediates.
  `CReplaceGimmick::ReplacePlayer` (respawn after a fall) is `0x4AE000` / `0x4AE120`, not a transition.
  Players: CGameProperty `[0x6D48B8]` (`+0x1C` map camera, `+0x20` player container), Player(n) `0x48CA70`,
  GetGamePlayer(n) `0x48CA90` (AddRef), GetPlayerNum `0x48CAB0`. CGamePlayer vtable `0x64A4C0` (no virtual
  dtor: +0 AddRef, +4 Release, +8 IsAlive, +0xC GetPosition, +0x14 GetRotY, +0x24 GetCurrentCharacter,
  +0x34 GetStatus, +0x4C Relocation(pos, dir, blink)). CPlayerCharacter (vtable `0x6564E0`): GetStatus
  `0x4D23F0`, ChangeStatus `0x4D24D0` (RequestStatusMorphing vtable +0x88 `0x4D30C0`: JUMP 7 → JUMP_READY 6
  from IDLE/WALK/RUN), state machine `+0x174` (`+8` statuses, `+0x14` count, `+0x18` current, `+0x1C` before,
  `+0x20` ground status for the jump, `+0x24` time; IsEnableChangeStatus `0x4D4750`, ChangeStatus
  `0x4D4910`). Status numbers match the reimplementation (0 idle … 6 jump ready, 7 jump, 9 wall jump, 11
  aerial). CJumpReady::OnAttach `0x4CF960` copies "before" into `+0x20`; CJump::OnAttach `0x4CF9B0` takes
  walk speed (`chr+0x144`) for 1, run speed (`chr+0x14C`) for 2, else 0, plus jump speed `chr+0x158`.

## 6. Simulation (for netplay, from the reimplementation, to be confirmed in the exe)
- Fixed 60 Hz logic; `CPCFrameSkipController` skips drawing only (up to 5 frames) when late.
- RNG: `Math::Rand` = `RpRandom()`, seed `RpRandomSeed` (RenderWare global).
- `GAMETYPES::PLAYERS_MAX = 4`; character select joins one player per controller (`CController::Max()`).
- Game modes: `GAMEMODE_NORMAL`, `GAMEMODE_DEMO`, `GAMEMODE_NEXUS`.

## 7. Open items
- Hor+: `RwCameraSetViewWindow 0x54AB50`; find the game's camera/FOV setter and its 2D (Im2D/Rt2d) sites.
- Input: `CPCPhysicalController` (DirectInput devices, keyboard), where the per-frame state is latched.
- Main loop / frame tick and `RpRandomSeed` call sites in the exe.
