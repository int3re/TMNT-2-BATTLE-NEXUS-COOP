// TMNT2.exe (Battle Nexus, NA retail 2004) addresses used by tmnt2net.
// Valid only for the exe whose SHA-256 is kExeSha256; see re/notes.md for how
// each one was found. Names follow the reimplementation xntrz/tmnt2.
#pragma once
#include <cstdint>

namespace game {

// 4796416 bytes, PE timestamp 0x4124028A.
constexpr uint8_t kExeSha256[32] = {0x22, 0xf4, 0x99, 0x08, 0xbc, 0xd3, 0xb8, 0x42, 0x78, 0xbe, 0xa8,
                                    0x88, 0xb7, 0x28, 0x5e, 0x1e, 0x0b, 0xa7, 0x3a, 0x72, 0xa8, 0x97,
                                    0x64, 0xf4, 0x95, 0xac, 0xeb, 0x76, 0xa4, 0x2d, 0x8b, 0x6a};

// --- COptionData (GameData/OptionData.cpp) -----------------------------------
// ctor 0x489920: m_nGamepadNum (+0x24) = CController::Max() = 1 (keyboard) + joysticks,
// m_pGamepad (+0x28) = new CGamepadOptionData[n] (0x24 bytes each).
// Snapshot(RAWDATA*) copies 6 entries regardless of n: the "Unkown Error".
constexpr uintptr_t kOptionSnapshot = 0x4897A0;  // __thiscall(this, RAWDATA*), ret 4
constexpr uint8_t kOptionSnapshotHead[] = {0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C, 0x56, 0x57, 0x55, 0x8B, 0xF9};
constexpr int kOptionGamepadNum = 0x24;
constexpr int kOptionGamepadArray = 0x28;
constexpr int kGamepadEntrySize = 0x24;
constexpr int kSnapshotGamepads = 6;  // RAWDATA::m_aGamepad[6]

// --- CPCKeyboardController::Start (System/Pc/PCPhysicalController.cpp) ----------
// After SetCooperativeLevel(NONEXCLUSIVE|FOREGROUND) the keyboard is acquired;
// when the window is not in front (Alt+Tab while the game loads) Acquire fails,
// Start returns false, CPCInputsDevice keeps m_ppPhysicalController = NULL and
// the first Sync asserts ("m_ppPhysicalController != NULL", PCInputsDevice.cpp:56).
// The keyboard is re-acquired on every Update, so a failed first Acquire is harmless.
constexpr uintptr_t kKeyboardAcquireCheck = 0x537611;
constexpr uint8_t kKeyboardAcquireOld[] = {0x7D, 0x1E};  // jge 0x537631 (success)
constexpr uint8_t kKeyboardAcquireNew[] = {0xEB, 0x1E};  // jmp 0x537631

// --- PCSetting (System/Pc/PCSetting.cpp) ---------------------------------------
// 0x53AB20: defaults (640x480x32, window mode off) + TMNT2.ini [SCREEN].
constexpr uintptr_t kSettingInit = 0x53AB20;  // __thiscall(this), returns 1
constexpr uint8_t kSettingInitHead[] = {0xC7, 0x05, 0x3C, 0x0D, 0x6A, 0x00, 0x80, 0x02, 0x00, 0x00};
constexpr uintptr_t kScreenWidth = 0x6A0D3C;
constexpr uintptr_t kScreenHeight = 0x6A0D40;
constexpr uintptr_t kScreenDepth = 0x6A0D44;
constexpr uintptr_t kWindowMode = 0x6E0158;  // m_bWindowMode ("-wnd" in debug builds)
// 0x53A9E0: writes the current W/H/depth to TMNT2.ini; called at shutdown
// (0x534B65, 0x5352F2 via the jmp thunk 0x53AB60). __thiscall(this), plain ret.
constexpr uintptr_t kSettingSave = 0x53A9E0;
constexpr uint8_t kSettingSaveHead[] = {0x6A, 0xFF, 0x68, 0x4B, 0x17, 0x63, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};

// --- CPCGraphicsDevice (vtable 0x663D40) ---------------------------------------
constexpr uintptr_t kVideoVtable = 0x663D40;
constexpr uintptr_t kScreenWidthSlot = kVideoVtable + 0x24;  // ScreenWidth(): modes[cur].w
constexpr uintptr_t kScreenWidthFn = 0x535760;
constexpr uintptr_t kScreenHeightSlot = kVideoVtable + 0x28;  // ScreenHeight(): modes[cur].h
constexpr uintptr_t kScreenHeightFn = 0x535850;
constexpr uintptr_t kFindModeSlot = kVideoVtable + 0x58;  // FindMode(w, h, depth, bCurrentOnly) -> adapter
constexpr uintptr_t kFindMode = 0x535C70;
constexpr uintptr_t kPlaceWindowSlot = kVideoVtable + 0x5C;  // PlaceWindow(w, h)
constexpr uintptr_t kPlaceWindow = 0x535E30;
constexpr uintptr_t kPlaceFromInit = 0x5356F7;  // return address of the start-up call
constexpr int kVideoAdapterIndex = 0x28;
constexpr int kVideoAdapterCount = 0x24;

// Video mode list filter (inside the list builder): after w>=640 && h>=480,
// "w<=1024 && h<=768 unless highreso; w<=1600 && h<=1200; w*3 == h*4".
// Jumping from 0x5360A0 to the depth check at 0x5360E6 keeps every mode.
constexpr uintptr_t kModeFilter = 0x5360A0;
// The options page's РАЗРЕШЕНИЕ row walks a fixed table of 10 {w, h, depth} (0x647F68:
// 640x480 .. 1600x1200, 16 then 32 bits, all 4:3); its current row is the one equal to
// the frame (0x4837D0 with CScreen's size and depth, row 5 if none), and decide sets that
// mode (0x4838A0). tmntnet fills it with this monitor's modes.
constexpr uintptr_t kResTable = 0x647F68;
constexpr int kResRows = 10;
constexpr int kTextResFirst = 0x2D2;  // the rows' labels: texts 0x2D2.. ("1280 x  960 x 16")
// The game's change of video mode (what its РАЗРЕШЕНИЕ row calls, 0x4838A0): bool __cdecl(w, h, depth),
// then the 2D layer on the new frame (bool __cdecl()).
constexpr uintptr_t kChangeVideoMode = 0x53AF70;
constexpr uintptr_t kSystem2DReset = 0x52EE00;

// --- COptionData ([0x6D4424], CGameData::Option() 0x487670): the game's own ОПЦИИ ---------------
// Play() is the object itself: +0 autosave (byte), +4 difficulty (int 0..2). Display() is +0x1C:
// +0 player markers, +1 hit effects (the comic words), +2 help windows (bytes), +4 the row of kResTable.
// Autosave, difficulty, hit effects and help are part of the game's logic (help makes objects and
// windows that wait for a button; autosave changes which pages come): one for all in a network game.
constexpr uintptr_t kOptionDataPtr = 0x6D4424;
constexpr int kOptAutosave = 0x00, kOptDifficulty = 0x04, kOptDisplay = 0x1C;
constexpr int kOptMarkers = 0, kOptHitEffects = 1, kOptHelp = 2, kOptVideoRow = 4;  // from kOptDisplay
constexpr int kTextHitEffects = 0x2BC, kTextMarkers = 0x2BD, kTextHelp = 0x2BE;  // the ЭКРАН page's own labels

// --- the keyboard's keys (CKeyboardOptionData = COptionData + 0x30, no vtable in this build) --------
// +0: int key[13] (DirectInput scan codes) for attack A, attack B, shot, jump, dash, guard, character
// change, camera, gauge, up, down, left, right (defaults at 0x6480C8: F D R Space E S LShift F1 F2 and
// numpad 8 2 4 6; the arrows, Enter = START and Esc = SELECT are fixed keys beside them); +0x34: the pad
// button of each of the first nine. 0x483EB0 (__thiscall): the buttons from the pad's options, then
// Apply 0x483DC0 (the keys into the keyboard controller's map). The options page takes a new key with
// the first key down that is "valid": CPCSpecific::IsKeyValid 0x53AED0 = not a fixed key and
// enabled[key] (256 bytes at 0x6A1148); it shows names[key] (256 wchar_t pointers at 0x6A0D48, small
// Latin letters). COptionData::Restore(raw) 0x489800 (__thiscall, ret 4) puts back a save's options -
// its keys too - whenever a save is loaded; CGameData::Load applies them after.
constexpr int kOptKeys = 0x30, kKeyFuncs = 13;
constexpr uintptr_t kDefaultKeys = 0x6480C8;
constexpr uintptr_t kKeyboardApply = 0x483EB0;
constexpr uint8_t kKeyboardApplyHead[] = {0x56, 0x8B, 0xF1, 0xE8, 0x88, 0xFE, 0xFF, 0xFF, 0x8B, 0xCE, 0x5E, 0xE9};
constexpr uintptr_t kKeyNames = 0x6A0D48, kKeyEnabled = 0x6A1148;
constexpr uintptr_t kOptionRestore = 0x489800;
constexpr uint8_t kOptionRestoreHead[] = {0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C, 0x56, 0x55, 0x8B, 0xF1};
// tmnt2net's keys no keyboard has: the mouse's buttons, so the game's own key page can take them.
constexpr int kKeyMouseLeft = 0xF0, kKeyMouseRight = 0xF1, kKeyMouse4 = 0xF3, kKeyMouse5 = 0xF4;

// --- sound volumes ---------------------------------------------------------------------------------
// CSoundOptionData = COptionData + 8: {mode, SE, BGM, voice volume 0..10, default mode}; Apply 0x484110
// (__thiscall) gives them to CGameSound: SetSeConfig 0x51B0D0, SetBgmConfig 0x51B090, SetVoiceConfig
// 0x51B110 (__cdecl(int volume)).
constexpr int kOptSound = 0x08;
constexpr uintptr_t kSoundOptionApply = 0x484110;
constexpr uint8_t kSoundOptionApplyHead[] = {0x56, 0x8B, 0xF1, 0x8B, 0x06, 0x85, 0xC0, 0xB9, 0x0A, 0x00, 0x00, 0x00};
constexpr uintptr_t kSoundVolume[3] = {0x51B0D0, 0x51B090, 0x51B110};
constexpr uint8_t kSoundVolumeHead[] = {0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x7D, 0x10, 0x33, 0xC0};

// --- the pause menu --------------------------------------------------------------------------------
// CGameStage::CheckPauseMenu 0x48DBF0 (__thiscall, no arguments; three stage sequences ask it every
// frame the stage is not paused): on the PC it is CPCSpecific::IsKeyTrigger(DIK_ESCAPE) - the Esc KEY,
// read from the keyboard itself, the only key the game reads so. The pause menu itself
// (CGaugeInformation::PausePeriod) goes by the virtual pad: OK, CANCEL, up, down. The pad's bits of
// the fixed keys (0x64817C): up 1, down 2, left 4, right 8, START 0x200 (Enter), SELECT 0x100 (Esc).
constexpr uintptr_t kCheckPauseMenu = 0x48DBF0;
constexpr uint8_t kCheckPauseMenuHead[] = {0x6A, 0x01, 0xE8, 0xC9, 0xD2, 0x0A, 0x00, 0x83, 0xC4, 0x04};
constexpr uint32_t kDigitalStart = 0x200, kDigitalSelect = 0x100;

// --- the options sequence (Options.cpp; vtable 0x65FC1C: OnMove 0x4EB140, OnDraw 0x4E81B0) -------
// 0x4E9DA0 is its page logic, once a frame while the page is up (__thiscall, no arguments): +0x110 the
// mode (0 the list ИГРА / ЗВУК / ЭКРАН / ..., 11 ЭКРАН, 16 its ОК), +0x13C the line under the cursor.
// 0x4E8780(bool display_changed) lays the page out again (the game calls it after a new video mode).
constexpr uintptr_t kOptionsMove = 0x4E9DA0, kOptionsDraw = 0x4E81B0, kOptionsSettingInit = 0x4E8780;
constexpr uint8_t kOptionsMoveHead[] = {0x51, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8A, 0x86, 0x31, 0x01, 0x00, 0x00};
constexpr uint8_t kOptionsDrawHead[] = {0x83, 0xEC, 0x14, 0x53, 0x56, 0x57, 0x8B, 0xF9};
constexpr int kOptionsMode = 0x110, kOptionsLine = 0x13C;
constexpr int kOptionsLineDisplay = 2;  // ЭКРАН in the list
constexpr uint8_t kModeFilterOld[] = {0x8B, 0x54, 0x24, 0x14};  // mov edx, [esp+14h]
constexpr uint8_t kModeFilterNew[] = {0xEB, 0x44, 0x90, 0x90};  // jmp 0x5360E6

// --- RenderWare (same library build as TMNT1: identical RwCameraSetViewWindow) --
constexpr uintptr_t kEngineInstance = 0x6EE738;  // RwEngineInstance (-> RwGlobals 0x6EE608)
constexpr uintptr_t kSetViewWindow = 0x54AB50;   // RwCamera* __cdecl(RwCamera*, const RwV2d*)
constexpr uint8_t kSetViewWindowHead[] = {0x8B, 0x44, 0x24, 0x08, 0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x08};
constexpr int kCameraFrameBuffer = 0x60;  // RwCamera::frameBuffer (RwRaster*)
constexpr int kRasterWidth = 0x0C, kRasterHeight = 0x10;

// --- CSprite virtual screen (System/Common/Sprite.cpp) ---------------------------
// m_fVirtualScreenX/Y/W/H at 0x6A08D4.. = (-320, -240, 640, 480), written only by
// SetDefaultVirtualScreen (called from CSystem2D::Initialize, which also hands the
// values to Rt2dDeviceSetMetric). Sprites map x -> (x - X) / W * CScreen::Width().
constexpr uintptr_t kVirtualScreen = 0x6A08D4;
constexpr uintptr_t kSetDefaultVirtualScreen = 0x52E4A0;  // void __cdecl()
constexpr uint8_t kSetDefaultVirtualScreenHead[] = {0xC7, 0x05, 0xD4, 0x08, 0x6A, 0x00, 0x00, 0x00, 0xA0, 0xC3};

// Rt2dDeviceSetMetric(x, y, w, h) (RwBool __cdecl): the 2D toolkit's coordinate
// rectangle over the camera. The retail CSystem2D::Initialize (0x52ED52) passes the
// literals (-320, -240, 640, 480), not CSprite's virtual screen.
constexpr uintptr_t kRt2dDeviceSetMetric = 0x5B4C50;
constexpr uint8_t kRt2dDeviceSetMetricHead[] = {0x8B, 0x15, 0x90, 0x2A, 0x7E, 0x00, 0xA1, 0x44, 0x2B, 0x7E, 0x00};
// Rt2dDeviceSetCamera(RwCamera*): Rt2d keeps the camera's view window (x2 at 0x7E2B48) as its
// device view, the place its 2D is laid on. The game calls it for the 2D camera (CCamera* at
// 0x6DFFDC, made at the 2D's start 0x52EC80, again after a video mode change 0x4838A0 through
// 0x52EE00), outside a scene.
constexpr uintptr_t kRt2dDeviceSetCamera = 0x5B4FA0;
constexpr uintptr_t k2DCamera = 0x6DFFDC;  // CCamera*: menus, texts, gauges (no logic looks through it)
constexpr uint8_t kRt2dDeviceSetCameraHead[] = {0x8B, 0x44, 0x24, 0x04, 0x33, 0xD2, 0x3B, 0xC2};

// CScreen statics (System/Common/Screen.cpp): s_iWidth, s_iHeight (set from the device).
constexpr uintptr_t kScreenStaticWidth = 0x6DFFA4;
constexpr uintptr_t kScreenStaticHeight = 0x6DFFA8;

// --- CAnimation2D (Game/System/2d/Animation2D.cpp): Rt2d Maestro scenes (.lpac) --
// Start (0x4FA2B0, __thiscall, m_pMaestro at +4) scales the scene's bbox onto
// s_iWidth x s_iHeight: menus such as character select fill the whole frame.
constexpr uintptr_t kAnim2DStart = 0x4FA2B0;
constexpr uint8_t kAnim2DStartHead[] = {0x83, 0xEC, 0x2C, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x04, 0x85, 0xC0};
constexpr int kAnim2DMaestro = 0x04;
constexpr uintptr_t kRt2dMaestroGetBBox = 0x5BD2E0;    // Rt2dBBox* __cdecl(Rt2dMaestro*)
constexpr uintptr_t kRt2dMaestroGetScene = 0x5DE8F0;   // Rt2dObject* __cdecl(Rt2dMaestro*)
constexpr uintptr_t kRt2dObjectMTMTranslate = 0x5B6090;  // Rt2dObject* __cdecl(Rt2dObject*, float, float)

// --- the stage's gauges (Game/Component/Gauge) ------------------------------------
// The health ring CGaugeMeter ([0x6D491C]): Run 0x493CB0 (places its sprites on the first run),
// Draw 0x494010 (places the ring's pieces); both __thiscall, no args, flag "set up" at +0x14.
constexpr uintptr_t kMeterRun = 0x493CB0, kMeterDraw = 0x494010;
constexpr uint8_t kMeterHead[] = {0x56, 0x8B, 0xF1, 0x8A, 0x46, 0x14, 0x84, 0xC0};
// CGaugeStatus ([0x6D4924]): Run 0x494E10, Draw 0x496980 (flag at +0x16). The players' boxes go
// left from x [+0x340] = 270 (y [+0x344] = -200); the first player's icons (power-ups) beside the
// ring start at x [+0x348] = -93 (y [+0x34C] = -184). Set once, in the first run (0x494E5D).
constexpr uintptr_t kGaugeStatusRun = 0x494E10, kGaugeStatusDraw = 0x496980;
constexpr uint8_t kGaugeStatusHead[] = {0x56, 0x8B, 0xF1, 0x8A, 0x46, 0x16, 0x84, 0xC0};
constexpr int kGaugeBoxesX = 0x340, kGaugeIconsX = 0x348, kGaugeIconsY = 0x34C;
constexpr float kGaugeBoxesX0 = 270.0f, kGaugeIconsX0 = -93.0f, kGaugeIconsY0 = -184.0f;

// --- CPCMovie::Draw (System/Pc/PCMovie.cpp, 0x536C10, __thiscall) ---------------
// Draws m_pRenderSprite (+0x98), a CSprite placed in real pixels over the frame.
constexpr uintptr_t kMovieDraw = 0x536C10;
constexpr uint8_t kMovieDrawHead[] = {0x56, 0x57, 0x8B, 0xF1, 0xE8};
constexpr int kMovieSprite = 0x98;
// CSprite: +4 x, +8 y, +0xC z, +0x10 w, +0x14 h, +0x18 anchor x, +0x1C anchor y (pixels).
constexpr int kSpriteX = 0x04, kSpriteW = 0x10, kSpriteH = 0x14, kSpriteAnchorX = 0x18;

// --- CPCFrameTimer::Update (System/Pc/PCFrameTimer.cpp, 0x5353A0, __thiscall) -----
// Called by CPCGraphicsDevice::Flip (vtable +0x1C, 0x535710) right before the
// frame is shown: sleeps until 16 ms (1/60 s rounded down) after the last Sync,
// which comes after Present. Wait + Present add up to ~17.5 ms (57 fps).
constexpr uintptr_t kFrameTimerWait = 0x5353A0;
constexpr uint8_t kFrameTimerWaitOld[] = {0x56, 0x57, 0x8B, 0xF1};
constexpr uint8_t kFrameTimerWaitNew[] = {0xC3, 0x90, 0x90, 0x90};  // ret: tmnt2net paces at Present

// --- frame loop and determinism (stage 3, re/notes.md §10) --------------------------
// CFramework::Flip (0x52C150): ProcessDispatcher::Dispatch, graphics Flip,
// inputs Sync (vtable +0x34), file manager Sync (vtable +0xC).
// CPCInputsDevice::Sync (0x536510): Update() of every controller, then the
// virtual controllers: the moment the input of the next frame is fixed.
constexpr uintptr_t kInputsSyncSlot = 0x663CB0;  // CPCInputsDevice vtable +0x34
constexpr uintptr_t kInputsSync = 0x536510;
// CPCFrameSkipController::Sync (0x534D20): skips drawing when late; drawn
// effects draw random numbers, so skipping must be off for lockstep.
constexpr uintptr_t kFrameSkipCheck = 0x534D5C;
constexpr uint8_t kFrameSkipOld[] = {0x74, 0x16};  // je (enabled -> may skip)
constexpr uint8_t kFrameSkipNew[] = {0xEB, 0x16};  // jmp: never skip
// RenderWare's RpRandom: 31-word additive generator; state G = RwEngineInstance +
// [0x6F6E64]: G[0] table, G[1] p1, G[2] p2, G[3] table end. The game seeds it
// with 123456 when sequences start (Math::SRand 0x528BC0 -> RpRandomSeed).
constexpr uintptr_t kRpRandomSeed = 0x5B0F20;  // void __cdecl(RwUInt32)
constexpr uint8_t kRpRandomSeedHead[] = {0xA1, 0x38, 0xE7, 0x6E, 0x00};
constexpr uintptr_t kRpRandom = 0x5B0FA0;  // RwUInt32 __cdecl(void)
constexpr uint8_t kRpRandomHead[] = {0x8B, 0x0D, 0x64, 0x6E, 0x6F, 0x00};
constexpr uintptr_t kRpRandomPluginOffset = 0x6F6E64;
// CFileManager singleton [0x6E0050]: +4 state (0 idle, 1 busy), vtable [3] Sync
// (base 0x530BC0). Reads are asynchronous (CRI ADXF): one file per frame at most.
constexpr uintptr_t kFileManager = 0x6E0050;
// Things the logic asks that run on real time. Their answers must be the same on
// every copy in the same frame, so in lockstep they come with the frame's input.
// CMovie::IsEnded (0x52D540, __thiscall, bool): [this+0xB4] (handle ok) == 0 ->
// true, else Sofdec status (vtable +0x20) PLAYEND (3) or ERROR (4). Called only by
// the movie sequence's Move (0x4F93C0: OK/START skips, the end returns).
constexpr uintptr_t kMovieIsEnded = 0x52D540;
constexpr uint8_t kMovieIsEndedHead[] = {0x8A, 0x81, 0xB4, 0x00, 0x00, 0x00};
constexpr uintptr_t kMovieHandleOk = 0xB4;
// CMovieManager::PreCreateMovieInstance (around 0x512A13) stores
// new CPCMovie(640, 448, 0x200000, false) (ctor 0x536B40, __thiscall, 4 args)
// at [0x6D74C4]; DeleteMovieInstance clears it.
constexpr uintptr_t kMovieInstance = 0x6D74C4;
constexpr uintptr_t kPCMovieCtor = 0x536B40;
constexpr uint8_t kPCMovieCtorHead[] = {0x6A, 0xFF, 0x68, 0xD8, 0x15, 0x63, 0x00};
// CGameSound::IsVoicePlaying (0x519EC0, bool): SoundVoiceBusyCheck() == 1, the
// voice channels as the sound thread plays them. Callers: seesaw gimmick
// 0x4B03DC, stage sequence 0x4F83FA (waits for a voice or a timer), voice
// manager 0x51DC72 / 0x527D1D (a new voice only when none plays).
constexpr uintptr_t kVoicePlaying = 0x519EC0;
constexpr uint8_t kVoicePlayingHead[] = {0xE8, 0x3B, 0xFF, 0xFF, 0xFF};
// CGameSound::IsLoadEnd (0x519EB0, bool): SoundGetLoadInfo (0x519E50) == 0.
// Sound data loads on the sound thread; the PC start waits for it in a loop.
constexpr uintptr_t kSoundIsLoadEnd = 0x519EB0;
constexpr uint8_t kSoundIsLoadEndHead[] = {0xE8, 0x9B, 0xFF, 0xFF, 0xFF};
constexpr uintptr_t kSoundGetLoadInfo = 0x519E50;  // int __cdecl(void)

// --- 2D: the game's font, texts, controller (stage 5, re/notes.md §12) -----------------
// CGameText::GetText(id) -> const wchar_t* (UTF-16 from Text.lpac), __cdecl.
constexpr uintptr_t kGetText = 0x527E40;
constexpr uint8_t kGetTextHead[] = {0x8B, 0x44, 0x24, 0x04, 0x50, 0xB9, 0xE8, 0xFE, 0x6D, 0x00};
// CGameFont::m_pFont (CUnicodeFont*: +4 brush, +8 Rt2dFont*), loaded from fonts/mainfont.met.
constexpr uintptr_t kGameFont = 0x6D4E80;
constexpr uintptr_t kFontSetRGBA = 0x52B760;  // __thiscall(r, g, b, a), ret 0x10
constexpr uintptr_t kFontShow = 0x52B9B0;     // __thiscall(const wchar_t*, float h, float x, float y), ret 0x10
constexpr uintptr_t kRt2dFontGetStringWidth = 0x5B30D0;  // float __cdecl(Rt2dFont*, const wchar_t*, float h)
// CSystem2D [0x6DFFE0]: BeginScene / EndScene switch to the 2D camera and back (__thiscall).
constexpr uintptr_t kSystem2D = 0x6DFFE0;
constexpr uintptr_t kSystem2DBegin = 0x52A610;
constexpr uintptr_t kSystem2DEnd = 0x52A670;
// CController::GetDigitalTrigger(port, mask) (__cdecl; port -1 = any); masks: d-pad
// up 1, down 2, left 4, right 8; OK and CANCEL are variables (PC: START|cross, SELECT|square).
constexpr uintptr_t kGetDigitalTrigger = 0x52AFE0;  // int __cdecl(short port, mask): pressed this frame
constexpr uint8_t kGetDigitalTriggerHead[] = {0x8B, 0x44, 0x24, 0x04, 0x50, 0xE8};
constexpr uintptr_t kDigitalOk = 0x6A0898;
constexpr uintptr_t kDigitalCancel = 0x6A089C;
// CMenuSound: play menu sound i (0 cursor, 1 decide, 2 cancel), __cdecl.
constexpr uintptr_t kMenuSound = 0x4FAAF0;
// CProcessDispatcher::Draw (__thiscall), called by CFramework::Render (0x52C0E0)
// between the graphics device's RenderBegin and RenderEnd: every process is drawn.
constexpr uintptr_t kDispatcherDraw = 0x532910;
constexpr uint8_t kDispatcherDrawHead[] = {0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1};

// --- title menu (Game/Sequence/Menu/Title.cpp) -------------------------------------
// Items: 5 records of 16 bytes at 0x69E048 {u8 row, u8 visible, u8 enabled, u8 -,
// i32 text id, f32 y, i32 next}: new game, continue, options, arcade (hidden on PC),
// quit (made visible at start); arcade and quit share row 3 / y 170.
constexpr uintptr_t kTitleItems = 0x69E048;
constexpr int kTitleItemCount = 5;
constexpr uintptr_t kTitleGetSelect = 0x4EB1D0;  // __thiscall -> item index of the cursor row, -1
constexpr uintptr_t kTitleDrawMenu = 0x4EB380;   // __thiscall: the items, game font, inside BeginScene
constexpr uint8_t kTitleDrawMenuHead[] = {0x51, 0xA1, 0xE0, 0xFF, 0x6D, 0x00};
constexpr uintptr_t kTitleUpdateMenu = 0x4EB760;  // __thiscall: counts visible items, moves, OK
constexpr uintptr_t kTitleBranch = 0x4EB4C0;      // __thiscall: after the fade, by [this+0x38]
constexpr uint8_t kTitleBranchHead[] = {0x51, 0x56, 0x8B, 0xF1};
// The title sequence's update (vtable, __thiscall(bool, int), ret 8): phase +0x2C (1 loading,
// 2 on screen, 4 leaving), step +0x3C in phase 2 (0 "НАЖМИ СТАРТ", 1-2 the menu, 3 a question).
constexpr uintptr_t kTitleUpdate = 0x4EB8C0;
constexpr uint8_t kTitleUpdateHead[] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x8B, 0xF1};
constexpr int kTitlePhase = 0x2C, kTitleStep = 0x3C;
constexpr int kTitleIdle = 0x1A4;
// CGameData::Record ([0x6D4428], 0x968 bytes made by 0x4876A0, no vtable nor pointers): the story
// progress, unlocks and the game's settings that the save file keeps. A session in a running game
// copies the host's into every game at its frame 1 (each keeps its own and gets it back after).
constexpr uintptr_t kRecordPtr = 0x6D4428;
// Beside it, the "new game" flag (0x487390 reads it, 0x4873B0 sets it for НОВАЯ ИГРА, a loaded save
// clears it): 1 = no progress, ПРОДОЛЖИТЬ off and no "progress will be lost?" question.
// CGameData::Load(save data) 0x487340: CGameRecord::Restore(raw, true) 0x489430 (__thiscall, ret 8) with
// the record's part of a save (save data + 0x114, after the options' 0x114 bytes), the options, then the
// new-game flag from CAreaRecord (record + 0x538) ::IsNewGame 0x484850. A save file is a 12-byte header
// {version 3, size, checksum} and 0x578 bytes of save data.
constexpr uintptr_t kRecordRestore = 0x489430, kAreaIsNewGame = 0x484850;
constexpr uint8_t kRecordRestoreHead[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0x07, 0x50, 0x8B};
constexpr uint8_t kAreaIsNewGameHead[] = {0xB0, 0x01, 0x33, 0xD2, 0x85, 0xD2, 0x74, 0x0A, 0x83, 0xFA};
constexpr int kRecordArea = 0x538, kSaveHeader = 12, kSaveData = 0x578, kSaveRecord = 0x114;
constexpr uintptr_t kNewGameFlag = 0x6D442C;  // float seconds without a button: 15 in the menu -> the demo
constexpr int kTitleNext = 0x38;  // next sequence: 0 new, 1 continue, 2 options, 3 quit, 4 arcade, 5 demo
constexpr int kTitleCursor = 0x40;  // cursor row

// --- character select (Game/Sequence/Menu/CharacterSelectSequence.cpp) ---------------
// CPlayerSelectWorkPool::Move (0x4E2A00, __thiscall): every player's input. The
// pool: players at +0x1C0, 0x58 bytes each {+0 controller u16, +4 cursor 0..3,
// +8 secret character (byte), +0xC state (1 joined), +0x10 picks allowed, +0x38
// picks made, +0x4C cursor shown, +0x50 cursor animation 1 right / 2 left};
// characters at +0x180, 8 bytes each {+0 enabled, +1 taken} (slot 0..3, +4 secret).
// PushStackCharacter (0x4E1780) refuses a taken slot; the cursor still stopped there.
constexpr uintptr_t kSelectMove = 0x4E2A00;
constexpr uint8_t kSelectMoveHead[] = {0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9, 0xE8};
constexpr int kSelPlayers = 0x1C0, kSelPlayerSize = 0x58, kSelChars = 0x180;
constexpr int kSelCursor = 0x04, kSelSecret = 0x08, kSelState = 0x0C, kSelMax = 0x10, kSelPicked = 0x38,
              kSelAnimDir = 0x50;

// --- area map (Game/Sequence/Area/AreaSequence.cpp) ---------------------------------
// CAreaWorkPool::KameIconDisp (0x4DCCC0, __thiscall): the team's turtle icon at
// m_vKamePos (+0x44 x, +0x48 y) + 32, with arrows unless the icon is moving
// (m_animation +0x160 == 1) or the area is a special one; nothing while an
// area-cleared animation runs (+0x58 != 0).
constexpr uintptr_t kKameIconDisp = 0x4DCCC0;
constexpr uint8_t kKameIconDispHead[] = {0x83, 0xEC, 0x08, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x58};
constexpr int kAreaKamePos = 0x44, kAreaCleared = 0x58, kAreaAnimation = 0x160;
constexpr uint8_t kAreaAnimKameMove = 1;

// --- player markers (Game/Component/Gauge/GaugeStatus.cpp) ----------------------------
// CGaugeStatus_Container::PlayerMarkerDisp(player) (0x495F90, __thiscall, ret 4): the
// "P1".."P4" tag over a player: CGameProperty::Player(n) (0x48CA70, __cdecl) position
// (vtable +0xC returns RwV3d*) + 1.0 up, through CGameProperty::GetCameraViewMatrix
// (0x48CE10, __cdecl RwMatrix*), / z, * CScreen width/height; drawn 32x18 px with its
// bottom there. CController::GetPhysicalPort(virtual) (0x52AD00) = the pad a game
// player locked at character select (tmnt2net's pad n = network player n).
// The exe draws the tag as one RwIm2D quad (0x496483: RwEngineInstance +0x30
// Im2DRenderPrimitive(prim, vertices, 4); vertex 0x1C bytes: x, y, z, rhw, ARGB colour of
// the character, u, v), clamped to the frame's edge when the player is off screen; then,
// off screen only, a second quad: the arrow.
constexpr uintptr_t kPlayerMarkerDisp = 0x495F90;
constexpr int kIm2DRenderPrimitive = 0x30, kIm2DVertexSize = 0x1C, kIm2DVertexColor = 0x10;
constexpr uint8_t kPlayerMarkerDispHead[] = {0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x55, 0x8B, 0xE9};
constexpr uintptr_t kGamePlayer = 0x48CA70;
constexpr uintptr_t kGetCameraViewMatrix = 0x48CE10;
constexpr uintptr_t kGetPhysicalPort = 0x52AD00;

// --- the dance after an area (Game/Sequence/Result/EnbuProc.cpp, EnbuSequence.cpp) ------
// CEnbuProc keeps one CEnbuProc_Container (0xD4 bytes) in 0x6D4DF8: +0 PLAYERID, +4 personal
// rank (1 E .. 5 A .. 7 SS), +8 total rank, +0xC costume, +0x10 rank value (0 high, 1 normal,
// 2 low = E), +0x14 CModel* (+4 position, +0x10 rotation, +0x40 clump), +0x24 motion 1 over.
// Initialize 0x4F0C70 (new + ctor from PlayResult's MVP), Settings 0x4F0D40 (__thiscall: model
// at {0, 0.5, 0}, motions "%s_%s1/2" from the motion set "enbu"), Period 0x4F03A0 (__thiscall;
// when motion 1 ends a rank A+ turtle shouts: PlaySE call at 0x4F0517), Draw3D 0x4F0CF0 (reads
// the global; cull mode saved to 0x6DFF78), Terminate 0x4F14E0. CModel::UpdateFrame 0x50C030
// (__thiscall, the clump): position + rotation into the clump frame.
// CEnbuSequence::OnAttach 0x4F1560 (vtable slot 0x65AE2C) registers the MVP's Enbu_<chara>.lpac
// with CDataLoader::Regist(file id) 0x4FEF40; each such file makes its own motion set "enbu".
constexpr uintptr_t kEnbuContainer = 0x6D4DF8;
constexpr uintptr_t kEnbuInitialize = 0x4F0C70;
constexpr uintptr_t kEnbuSettings = 0x4F0D40;
constexpr uint8_t kEnbuSettingsHead[] = {0x6A, 0xFF, 0x68, 0x21, 0x08, 0x63, 0x00};
constexpr uintptr_t kEnbuPeriod = 0x4F03A0;
constexpr uint8_t kEnbuPeriodHead[] = {0x83, 0xEC, 0x28, 0xA1, 0x14, 0x39, 0x6B, 0x00};
constexpr uintptr_t kEnbuShout = 0x4F0517;
constexpr uintptr_t kEnbuDraw3D = 0x4F0CF0;
constexpr uint8_t kEnbuDraw3DHead[] = {0xA1, 0x38, 0xE7, 0x6E, 0x00, 0x56, 0x8B, 0x35};
constexpr uintptr_t kEnbuCullSave = 0x6DFF78;
constexpr uintptr_t kEnbuTerminate = 0x4F14E0;
constexpr uint8_t kEnbuTerminateHead[] = {0x8B, 0x0D, 0xF8, 0x4D, 0x6D, 0x00};
constexpr uintptr_t kEnbuOnAttachSlot = 0x65AE2C, kEnbuOnAttach = 0x4F1560;
constexpr uintptr_t kDataLoaderRegist = 0x4FEF40;
constexpr uintptr_t kModelUpdateFrame = 0x50C030;
constexpr int kEnbuPlayer = 0x00, kEnbuRank = 0x04, kEnbuCostume = 0x0C, kEnbuRankValue = 0x10,
              kEnbuModel = 0x14, kModelPosition = 0x04, kModelClump = 0x40;
// CGamePlayResult 0x6D4430: the team's PLAYERIDs at +0x88 (4; 8 = nobody), GetPersonalRank(i)
// 0x488AE0 (__thiscall). CGamePlayParam 0x6D3DA8: GetCostume(i) 0x487FD0 (__thiscall).
constexpr uintptr_t kPlayResult = 0x6D4430, kPlayParam = 0x6D3DA8;
constexpr int kResultTeam = 0x88;
constexpr uintptr_t kGetPersonalRank = 0x488AE0, kGetCostume = 0x487FD0;
// CMotionManager: CreateMotionSet(name, parent) 0x50EA80 (__cdecl), SetCurrentMotionSet(name)
// 0x50EAA0, the container in 0x6D74B8 with FindSet(name) 0x50E5B0 (__thiscall, null if none).
constexpr uintptr_t kCreateMotionSet = 0x50EA80;
constexpr uint8_t kCreateMotionSetHead[] = {0x8B, 0x0D, 0xB8, 0x74, 0x6D, 0x00};
constexpr uintptr_t kSetCurrentMotionSet = 0x50EAA0, kMotionContainer = 0x6D74B8, kFindMotionSet = 0x50E5B0;
constexpr uintptr_t kPlaySE = 0x51A1C0;  // CGameSound::PlaySE(code), __cdecl
// CSequence::Call(label, param) 0x5341F0 (__thiscall, ret 8); the Enbu sequence's label is 13
// (process table at 0x6A07B0: {13, priority 4, Instance 0x5296C0}).
constexpr uintptr_t kSequenceCall = 0x5341F0;
constexpr int kProcEnbu = 13;

// --- in-stage transitions (Game/Component/Gimmick/ConcreteGimmick/SystemGimmick.cpp) ----
// CCameraChangeGimmick::PostMove 0x49DE90 (__thiscall): step +0x2C (0 idle, 1 = the fade out
// starts this frame, 2 fading, 3 ReplacePlayers 0x49DD50 = everybody on a ring of 1.5 around
// +0x30, 4..6 fade in, release). The one who walked or jumped into the zone did it himself.
constexpr uintptr_t kCameraChangeMove = 0x49DE90;
constexpr uint8_t kCameraChangeMoveHead[] = {0x53, 0x8B, 0xD9, 0x8B, 0x43, 0x2C, 0x48};
constexpr int kCameraChangeStep = 0x2C, kCameraChangeFadeOut = 1;
constexpr int kCameraChangeSender = 0x4C;  // char[]: the zone that called; "" = nobody to answer
// Players: CGameProperty [0x6D48B8] +0x20 = the player container, GetPlayerNum 0x48CAB0 (__cdecl).
// IGamePlayer (CGamePlayer vtable 0x64A4C0, no virtual dtor): +0x24 GetCurrentCharacter (null
// when not alive). CPlayerCharacter: GetStatus 0x4D23F0, ChangeStatus(status) 0x4D24D0 (both
// __thiscall; RequestStatusMorphing, vtable +0x88, turns JUMP 7 into JUMP_READY 6 from IDLE 0 /
// WALK 1 / RUN 2; the state machine refuses a change the current status does not allow). The
// state machine at +0x174: +0x20 = the ground status the jump takes its speed from (set by
// JUMP_READY from the status before: 1 walk speed, 2 run speed, else straight up).
constexpr uintptr_t kGameProperty = 0x6D48B8;
constexpr int kPropPlayers = 0x20;
constexpr uintptr_t kGetPlayerNum = 0x48CAB0;
constexpr int kGamePlayerCurrentChr = 0x24;
constexpr uintptr_t kChrGetStatus = 0x4D23F0, kChrChangeStatus = 0x4D24D0;
constexpr int kChrStateMachine = 0x174, kSmJumpGround = 0x20;
constexpr int kStatusIdle = 0, kStatusRun = 2, kStatusJumpReady = 6, kStatusJump = 7;

// --- the stage camera (Game/System/Map/MapCamera.cpp, System/Common/Camera.cpp) ---------------
// CGameProperty +0x1C: CMapCamera* (+4 CCamera*, whose +4 is the RwCamera). CMapCamera: +8 eye,
// +0x14 look-at (eased), +0x44 requested zoom (CalcNiceZoom, 1..3), +0x48 mode (0 manual, 1 auto
// change, 2 introduction), +0x50 camera area name (p/e path, t set, f fixed), +0x74 introduction.
// LookAt 0x503B40 puts the matrix on the camera's frame with RwFrameTransform 0x54C950 (frame,
// matrix, 0 = replace). CCamera (+4 RwCamera, +8 the camera current before): BeginScene 0x52A610
// ends the current camera's update ([0x6DFF08]) and begins its own, EndScene 0x52A670 the
// reverse (both __thiscall, bool). The stage draws its 3D inside the map camera's scene; the 2D
// layer's CCamera nests inside it (ends and restarts the stage camera's update).
constexpr int kPropMapCamera = 0x1C, kMapCamCamera = 0x04, kCameraRw = 0x04;
constexpr int kMapCamAt = 0x14, kMapCamZoom = 0x44, kMapCamMode = 0x48, kMapCamModeAuto = 1;
constexpr int kMapCamAreaName = 0x50, kMapCamIntroduction = 0x74;
constexpr uintptr_t kMapCamLookAtFrame = 0x503B5F;  // mov ecx,[esi+4]; ...; mov edx,[ecx+4]; mov eax,[edx+4]
constexpr uint8_t kMapCamLookAtFrameBytes[] = {0x8B, 0x4E, 0x04, 0xC7, 0x44, 0x24, 0x20, 0x03, 0x00, 0x00, 0x00,
                                               0x8B, 0x51, 0x04, 0x8B, 0x42, 0x04, 0x83, 0xC4, 0x10, 0x85, 0xC0,
                                               0x5E, 0x74, 0x10, 0x6A, 0x00, 0x8D, 0x4C, 0x24, 0x04, 0x51, 0x50,
                                               0xE8, 0xCB, 0x8D, 0x04, 0x00};
constexpr uintptr_t kRwFrameTransform = 0x54C950;
constexpr int kRwObjectParent = 0x04, kRwFrameModelling = 0x10;  // RwObject.parent = the frame; RwFrame.modelling
constexpr int kRwCameraBeginUpdate = 0x18, kRwCameraEndUpdate = 0x1C;  // RwCamera: its update functions
constexpr int kRwCameraViewWindow = 0x68;  // RwCamera: RwV2d view window
constexpr uintptr_t kCamBeginScene = 0x52A610, kCamEndScene = 0x52A670;
// CGraphicsDevice::RenderBegin / RenderEnd: the frame's own camera (this+0xC) begins and ends its update
// around the whole draw of a frame.
constexpr uintptr_t kRenderBegin = 0x52C570, kRenderEnd = 0x52C5E0;
constexpr uint8_t kRenderBeginHead[] = {0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x0C, 0x85, 0xC0, 0x57, 0x75, 0x1C};
constexpr uint8_t kRenderEndHead[] = {0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x0C, 0x85, 0xC0, 0x75, 0x1C};
constexpr int kDeviceCamera = 0x0C;
constexpr uint8_t kCamBeginSceneHead[] = {0xA1, 0x08, 0xFF, 0x6D, 0x00, 0x85, 0xC0, 0x56};
constexpr uint8_t kCamEndSceneHead[] = {0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x04, 0x3B, 0x05, 0x08, 0xFF, 0x6D, 0x00};
constexpr int kGpGetPosition = 0x0C;  // IGamePlayer: const RwV3d* GetPosition()
constexpr int kGpGetCurrentCharacterId = 0x2C;  // IGamePlayer: PLAYERID of the turtle being played
// CGamePlayResult::AddTechnicalAction(character id, action) 0x4888F0 (__thiscall, ret 8): every
// technical action of a turtle (attacks 1..0x14, dash 0xF, guard impact 0x10, jumps 0x15..0x17,
// damaged 0x18, fallen 0x19); the area's evaluation and MVP come from it.
// CScreenFade's controller [0x6D74A0] (BlackOut 0x50B140 → start 0x50AC80): +0xC from, +0x10 to,
// +0x14 the colour now (RwRGBA, alpha at +0x17), +0x18 fading.
constexpr uintptr_t kScreenFade = 0x6D74A0;
constexpr int kScreenFadeColor = 0x14;
constexpr uintptr_t kAddTechAction = 0x4888F0;
constexpr uint8_t kAddTechActionHead[] = {0x55, 0x8B, 0x6C, 0x24, 0x0C, 0x85, 0xED, 0x57, 0x8B, 0xF9};
constexpr int kTecActFirstAttack = 0x01, kTecActLastAttack = 0x14, kTecActDash = 0x0F, kTecActGuardImpact = 0x10,
              kTecActDamaged = 0x18;
// CWorldMap::CheckCollisionLine(from, to) 0x507A90 (__cdecl, bool): on a hit it copies its result
// (0x1D dwords, the closest point first) to the static 0x6D73C8, which the camera reads back.
constexpr uintptr_t kWorldCheckLine = 0x507A90, kWorldCollisionResult = 0x6D73C8;
constexpr uint8_t kWorldCheckLineHead[] = {0x83, 0xEC, 0x1C, 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x2C};
constexpr size_t kWorldCollisionResultSize = 0x1D * 4;
constexpr float kOwnZoomMin = 0.5f, kOwnZoomMax = 2.5f;

// --- health (Game/Component/GameMain/GamePlayer.cpp, re/notes.md §13) -----------------------
// The team shares one health: CGamePlayerSharedProperty in .data, HP [0x6D48B0], max [0x6D48B4].
// Only CGamePlayer's own functions touch it (every xref is in 0x48BC00..0x48C720): GetHP (vtable
// +0x3C), AddHP (+0x50: total damage at +0x14, clamp 0..max; at 0 the comeback item refills, else
// CGameStage (0x48CB60) vtable +0x10 NotifyPlayerDead(n) = game over, the call bytes at 0x48C40E),
// SaveContext (+0x5C, ctx+4 = HP), LoadContext (+0x60: HP = ctx+4, max = ctx+8), Setup(n, player)
// 0x48C650 (player number at +8; n == 0 zeroes HP and max).
constexpr uintptr_t kSharedHP = 0x6D48B0, kSharedHPMax = 0x6D48B4;
constexpr uintptr_t kGamePlayerVtable = 0x64A4C0;
constexpr int kGpIsAlive = 0x08, kGpGetPlayer = 0x1C, kGpGetHP = 0x3C, kGpAddHP = 0x50, kGpSaveContext = 0x5C,
              kGpLoadContext = 0x60;
constexpr uintptr_t kGpGetHPFn = 0x48BFF0, kGpAddHPFn = 0x48C350, kGpSaveContextFn = 0x48C4D0,
                    kGpLoadContextFn = 0x48C500;
constexpr int kGamePlayerNo = 0x08, kContextHP = 0x04;
constexpr uintptr_t kGamePlayerSetup = 0x48C650;
constexpr uint8_t kGamePlayerSetupHead[] = {0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x10};
constexpr uintptr_t kPlayerDeadCall = 0x48C40E;  // call GetCurrent; mov ecx,[edi+8]; ...; call [edx+10h]
constexpr uint8_t kPlayerDeadCallOld[] = {0xE8, 0x4D, 0x07, 0x00, 0x00, 0x8B, 0x4F, 0x08,
                                          0x8B, 0x10, 0x51, 0x8B, 0xC8, 0xFF, 0x52, 0x10};
constexpr uintptr_t kGetCurrentStage = 0x48CB60;  // CGameStage* __cdecl()

// --- joining a running stage (midjoin.cpp, re/notes.md §13) ----------------------------------------
// CGamePlayParam (0x6D3DA8; CGameData::PlayParam 0x4875F0): +0x10 chara infos {id, costume, player}
// x4 from +0x14, +0x44 players, contexts {no, HP, HPmax, damage, shuriken, shuriken max, chara,
// chara count} x4 from +0x48; AddPlayerCharacter(no, id, costume) 0x487EB0; GetStageMode 0x487980
// (__thiscall; 3 = ride). CGameStage::AddPlayers(blink) 0x48DA90 (__thiscall, ret 4): the team's
// chara infos → CGameProperty::AddPlayerCharacter 0x48CAC0 → container vtable +0xC (0x48C940:
// CGamePlayer +0x38 each from container +8, count +4; CreatePlayer(no) 0x48C760 = CreatePlayerEx(no,
// no): player n uses controller n). CPlayer (CGamePlayer +0x10): characters {chr, manipulator, slot
// number} x4 from +0x28, count +0x58, current entry +0x5C; AddCharacter(chr) 0x4D2090 (makes the
// manipulator for its controller, wakes it when there is no current one); ~CPlayer deletes only the
// manipulators (vtable +0, 1). Character id at chr +0x108. Controllers: Lock(c) 0x52AB40 moves c to
// the first unlocked place, Unlock 0x52ABB0, GetController(port) 0x52AD70. Camera updaters (first in
// the stage's Period, not while paused): default 0x48DD60 (vtable slot 0x64AA10), boss 0x48DE60
// (0x64A964), __thiscall(map camera), ret 4.
constexpr int kPpCharaInfoNum = 0x10, kPpCharaInfo = 0x14, kPpPlayerNum = 0x44, kPpContext = 0x48;
constexpr uintptr_t kPpGetStageMode = 0x487980;
constexpr int kStageModeRide = 3;
constexpr uintptr_t kPlayParamAddChara = 0x487EB0;
constexpr uint8_t kPlayParamAddCharaHead[] = {0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x10, 0x04, 0x7C};
constexpr uintptr_t kAddPlayers = 0x48DA90;
constexpr uint8_t kAddPlayersHead[] = {0x6A, 0xFF, 0x68, 0xA6, 0xDB, 0x62, 0x00};
constexpr int kContainerCount = 0x04, kContainerPlayers = 0x08, kGamePlayerSize = 0x38, kGamePlayerPlayer = 0x10;
constexpr uintptr_t kGamePlayerCreate = 0x48C760;
constexpr uintptr_t kPlayerAddCharacter = 0x4D2090;
constexpr uint8_t kPlayerAddCharacterHead[] = {0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x58, 0x85, 0xC0, 0x57};
constexpr int kPlayerCharaList = 0x28, kPlayerCharaNum = 0x58, kPlayerCharaCurrent = 0x5C, kChrPlayerId = 0x108;
constexpr uintptr_t kControllerLock = 0x52AB40, kControllerUnlock = 0x52ABB0, kGetController = 0x52AD70;
constexpr uintptr_t kDefaultUpdaterSlot = 0x64AA10, kDefaultUpdater = 0x48DD60;
constexpr uintptr_t kBossUpdaterSlot = 0x64A964, kBossUpdater = 0x48DE60;
constexpr int kGpGetRotY = 0x14, kGpRelocation = 0x4C;
constexpr int kStageNotifyPlayerDead = 0x10;
// CPlayer::Run 0x4D2310 (vtable 0x6562F4 slot +8): each player, every frame the stage runs.
constexpr uintptr_t kPlayerRunSlot = 0x6562FC, kPlayerRun = 0x4D2310;
// CPlayerCharacter: +0x13C m_attackparameter.m_fTroubleTime (how long a damage status such as SLEEP
// lasts; CStatusDamage::OnAttach 0x4CEB20), +0x16C attributes (8 = invincible: hits do nothing,
// 0x4D4412). SLEEP = status 62: ends in IDLE by itself, a teammate's hit wakes (0x4D3D20).
constexpr int kChrTroubleTime = 0x13C, kChrAttributes = 0x16C, kAttrInvincible = 8, kStatusSleep = 62;
constexpr int kStatusGuardReady = 14, kStatusGuardImpact = 17;  // GUARD_READY, GUARD, GUARD_BACK, GUARD_IMPACT
// The state machine's current status object (+0x18; its id at +0xC). CStatusDamage (sleep, stun...)
// OnAttach 0x4CEB20 copies the trouble time to its end time +0x10; at status duration >= end: IDLE.
constexpr int kSmCurrentStatus = 0x18, kStatusObjId = 0x0C, kStatusDamageEndTime = 0x10;
// HUD: CGaugeMeter::GaugeUpdate 0x4930F0 (the health ring) asks CGameProperty::Player(0) here.
constexpr uintptr_t kMeterPlayerCalls[] = {0x493102, 0x49312B, 0x49314F, 0x4931A4};
// CGaugeStatus StatusWakuDispSub(n, x, y) 0x495840 (__thiscall, ret 0xC, float): a player's box at
// the top right (right edge x, top y, 2D space), returns its left edge; boxes hidden while [+0x15].
constexpr uintptr_t kStatusBox = 0x495840;
constexpr uint8_t kStatusBoxHead[] = {0x83, 0xEC, 0x20, 0xD9, 0x44, 0x24, 0x28};
constexpr int kStatusBoxHidden = 0x15;

// CGameStage vtable +0: NotifyGameClear(sub) (CGameEndGimmick 0x4B1E62: sub 1 = ending B).
constexpr int kStageNotifyGameClear = 0x00;
// Character select costumes (re/notes.md §13): CPlayerSelectWorkPool::IsCostumeTaken(cursor,
// costume) 0x4E1510 (__thiscall, ret 8: costume 1 samurai = secrets 0xC..0xF, 2 nexus = 0x10..0x13,
// IsUnlockedSecret 0x486560); Move asks the secret directly at 0x4E2C58 too (call 0x486560).
// CSecretRecord::IsUnlockedSecret(id) 0x486560 (__thiscall, ret 4, bool): secrets 1..4 = the playable
// Casey, Splinter, Karai, Slashuur (character select offers a character once unlocked).
constexpr uintptr_t kIsUnlockedSecret = 0x486560;
constexpr uint8_t kIsUnlockedSecretHead[] = {0x53, 0x56, 0x57, 0x8B, 0xF1, 0x32, 0xDB};
constexpr int kSecretFirstCharacter = 1, kSecretLastCharacter = 4;
// CAreaRecord (CGameData::Record 0x487640 +0x538, Area() 0x489690): GetCurrentSelectedArea 0x484870
// (field +0x3D8). The area map's CAreaWorkPool::Attach reads it at 0x4DBA92 (back at 0x4DBA97) to
// put its cursor (Home 3 → the station 0x3D); OK on the station enters Home.
constexpr uintptr_t kGetSelectedArea = 0x484870, kMapAttachReadsArea = 0x4DBA97;
constexpr uint8_t kGetSelectedAreaHead[] = {0x8B, 0x81, 0xD8, 0x03, 0x00, 0x00, 0xC3};
// Per player: [pool+0x320+i] samurai (game button 0x2000 held), [pool+0x324+i] nexus (0x400).
constexpr uintptr_t kIsCostumeTaken = 0x4E1510;
constexpr uint8_t kIsCostumeTakenHead[] = {0x8B, 0x44, 0x24, 0x08, 0x83, 0xEC, 0x20};
constexpr uintptr_t kSelectNexusSecret = 0x4E2C58;
constexpr uint8_t kSelectNexusSecretOld[] = {0xE8, 0x03, 0x39, 0xFA, 0xFF};
// The costume icons' draw (0x4E2810: samurai at x = 140 p - 201, nexus at 140 p - 244, y = -30, 32 x 32,
// white when chosen) asks the nexus secret directly too.
constexpr uintptr_t kSelectNexusIconSecret = 0x4E2918;
constexpr uint8_t kSelectNexusIconSecretOld[] = {0xE8, 0x43, 0x3C, 0xFA, 0xFF};
constexpr int kSelSamurai = 0x320, kSelNexus = 0x324;  // pool: a byte per player

// CRI Sofdec: CPCMovie::Update (vtable +4, 0x536AD0) runs the movie server 0x5EACB0, which waits
// for the next 1/60 s in 0x5D8D60 (__cdecl, no args: WaitForSingleObject on the event a 60 Hz
// timeSetEvent sets, [0x80A938]; with a time source [0x6F70AC] a QPC sleep 0x5D9530 instead).
constexpr uintptr_t kMovieWaitCall = 0x5EACB5, kMovieWait = 0x5D8D60;
constexpr uint8_t kMovieWaitCallOld[] = {0xE8, 0xA6, 0xE0, 0xFE, 0xFF};

// CGamePlayParam::SetStartArea(area, stage index) 0x4878A0 (__thiscall, ret 8): +4 area, +8 index,
// +0xC stage (GetStageID 0x48A330). Callers: area map 0x4D9302 (after ClearArea 0x487840), new game
// 0x4EC2E9, 0x4DF838, 0x4F9037. Areas with two stages: 0-2 (2), 1-1 (4), 4-3, 4-5, 4-6, 5-6.
constexpr uintptr_t kSetStartArea = 0x4878A0;
constexpr uint8_t kSetStartAreaHead[] = {0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1};

// --- stage camera (Game/System/Map/MapCamera.cpp) -----------------------------------
// CMapCamera::CalcNiceZoom(positions, count) 0x503C90 (__thiscall, ret 8, float in st0): how far
// the camera backs off to keep every player in view, 1..3 (3 = [0x65CD08]). Past that the players
// walk off screen and their tags stick to the edge with an arrow.
constexpr uintptr_t kCalcNiceZoom = 0x503C90;
constexpr uint8_t kCalcNiceZoomHead[] = {0x83, 0xEC, 0x18, 0x8B, 0x41, 0x04, 0x57};

// --- save file (System/Pc/PCSaveLoadManager.cpp) ---------------------------------
// 0x538FD0 builds "<exe folder>\tmntsave.dat" (esi = buffer) for every read (0x53968F)
// and write (0x539557): sprintf("%s%s%s", drive, dir, name) with the name pushed here.
constexpr uintptr_t kSaveNamePush = 0x53900B;
constexpr uint8_t kSaveNamePushOld[] = {0x68, 0x48, 0x44, 0x66, 0x00};  // push "tmntsave.dat"

// --- diagnostics -------------------------------------------------------------
// ASSERT failure: __cdecl(expr, file, line, "Unknown error."), formats the last
// argument and throws; WinMain shows it with "Program is ended compulsorily."
constexpr uintptr_t kAssert = 0x4019A0;
constexpr uint8_t kAssertHead[] = {0x6A, 0xFF, 0x68, 0xBB, 0x66, 0x62, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};

// --- window ------------------------------------------------------------------
constexpr uintptr_t kHwnd = 0x6E0174;
constexpr const char* kWindowClass = "TMNT2";

}  // namespace game
