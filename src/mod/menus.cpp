#include "mod/menus.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "common/log.h"
#include "mod/coop.h"
#include "mod/game.h"
#include "mod/gamefont.h"
#include "mod/camera.h"
#include "mod/input.h"
#include "mod/netpage.h"
#include "mod/netplay.h"
#include "mod/overlay.h"
#include "mod/pageui.h"
#include "mod/patch.h"
#include "mod/postfx.h"
#include "mod/sim.h"
#include "mod/video.h"
#include "mod/widescreen.h"
#include "common/version.h"

namespace menus {
namespace {

Config g_cfg;
std::wstring g_ini;
bool g_session = false;

// --- the title menu's items ---------------------------------------------------------------
#pragma pack(push, 1)
struct Item {
  uint8_t row, visible, enabled, pad;
  int32_t text;
  float y;
  int32_t next;
};
#pragma pack(pop)
static_assert(sizeof(Item) == 16, "the game's item record");

enum { kNew, kContinue, kOptions, kNet, kExtra, kQuit, kItems };
constexpr int32_t kTextNet = 0x7F00, kTextExtra = 0x7F01;  // ids no text of the game uses
Item g_items[kItems];

void layout(bool ours) {
  // Ours: six rows 26 apart from 70; the game's own: rows at 80/110/140/170, quit on row 3.
  static const float kOurs[kItems] = {70, 96, 122, 148, 174, 200};
  static const float kGame[kItems] = {80, 110, 140, 170, 170, 170};
  int row = 0;
  for (int i = 0; i < kItems; ++i) {
    bool shown = ours || (i != kNet && i != kExtra);
    if (i == kNet || i == kExtra) g_items[i].visible = shown ? 1 : 0;
    g_items[i].row = (uint8_t)(shown ? row++ : 3);
    g_items[i].y = ours ? kOurs[i] : kGame[i];
  }
}

void init_items() {
  const Item* game = reinterpret_cast<const Item*>(game::kTitleItems);
  g_items[kNew] = game[0];
  g_items[kContinue] = game[1];
  g_items[kOptions] = game[2];
  g_items[kNet] = Item{3, 1, 1, 0, kTextNet, 0, 2};  // next: options, should the OK ever get past us
  g_items[kExtra] = Item{4, 1, 1, 0, kTextExtra, 0, 2};
  g_items[kQuit] = game[4];
  layout(true);
}

// Every place the title code names the table, its records' fields or its size.
bool move_table() {
  const uintptr_t t = reinterpret_cast<uintptr_t>(g_items), old = game::kTitleItems;
  struct Site {
    uintptr_t at;  // the immediate in the instruction
    uint32_t was, now;
    uint8_t size;
  };
  const Site sites[] = {
      {0x4EB1D4, (uint32_t)old, (uint32_t)t, 4},                      // GetSelect: mov edx, table
      {0x4EB1F5, (uint32_t)old + 80, (uint32_t)t + 16 * kItems, 4},   // ... cmp edx, end
      {0x4EB3BE, (uint32_t)old, (uint32_t)t, 4},                      // DrawMenu: mov esi, table
      {0x4EB47C, (uint32_t)old + 80, (uint32_t)t + 16 * kItems, 4},   // ... cmp esi, end
      {0x4EB6FC, (uint32_t)old + 12, (uint32_t)t + 12, 4},            // next of the chosen item
      {0x4EB82C, (uint32_t)old + 12, (uint32_t)t + 12, 4},
      {0x4EB761, (uint32_t)old + 1, (uint32_t)t + 16 * kNew + 1, 4},  // UpdateMenu counts visible rows
      {0x4EB779, (uint32_t)old + 17, (uint32_t)t + 16 * kContinue + 1, 4},
      {0x4EB782, (uint32_t)old + 33, (uint32_t)t + 16 * kOptions + 1, 4},
      {0x4EB78B, (uint32_t)old + 49, (uint32_t)t + 16 * kNet + 1, 4},
      {0x4EB794, (uint32_t)old + 65, (uint32_t)t + 16 * kExtra + 1, 4},
      {0x4EB773, 1, 2, 4},  // ... the count starts at 2: quit (always shown) is the 6th record
      {0x4EB7A1, 5, 7, 1},  // ... at most 6 rows (assert)
      {0x4EB7CA, 5, 7, 1},
      {0x4EB7FC, (uint32_t)old + 17, (uint32_t)t + 16 * kContinue + 1, 4},  // new game: warn if continue is there
      {0x4EB804, (uint32_t)old + 18, (uint32_t)t + 16 * kContinue + 2, 4},
      {0x4EB867, (uint32_t)old + 2, (uint32_t)t + 2, 4},  // skip disabled rows (up / down)
      {0x4EB89C, (uint32_t)old + 2, (uint32_t)t + 2, 4},
      {0x4EB2D3, (uint32_t)old + 65, (uint32_t)t + 16 * kQuit + 1, 4},  // PC: quit shown and enabled
      {0x4EB2D9, (uint32_t)old + 66, (uint32_t)t + 16 * kQuit + 2, 4},
      {0x4EB94C, (uint32_t)old + 65, (uint32_t)t + 16 * kQuit + 1, 4},
      {0x4EB951, (uint32_t)old + 66, (uint32_t)t + 16 * kQuit + 2, 4},
      {0x4EB2EB, (uint32_t)old + 18, (uint32_t)t + 16 * kContinue + 2, 4},  // continue: a save exists
      {0x4EB963, (uint32_t)old + 18, (uint32_t)t + 16 * kContinue + 2, 4},
  };
  for (const Site& s : sites)
    if (!patch::bytes_are(s.at, &s.was, s.size)) {
      LOGE("menus: title menu code differs at 0x%08X; the menu is left as it is", (unsigned)s.at);
      return false;
    }
  for (const Site& s : sites) patch::write(s.at, &s.now, s.size);
  LOGI("menus: title menu table moved to tmnt2net (6 rows: + СЕТЕВАЯ ИГРА, ДОПОЛНИТЕЛЬНО)");
  return true;
}

// --- texts ------------------------------------------------------------------------------
using GetText_t = const wchar_t*(__cdecl*)(int);
GetText_t oGetText = nullptr;
std::wstring g_label_net, g_label_extra;

const wchar_t* __cdecl hkGetText(int id) {
  if (id == kTextNet) {
    if (g_label_net.empty()) g_label_net = gamefont::text(u8"СЕТЕВАЯ ИГРА", "NETWORK GAME");
    return g_label_net.c_str();
  }
  if (id == kTextExtra) {
    if (g_label_extra.empty()) g_label_extra = gamefont::text(u8"НАСТРОЙКИ МОДА", "MOD SETTINGS");
    return g_label_extra.c_str();
  }
  if (id >= game::kTextResFirst && id < game::kTextResFirst + game::kResRows) {
    // The resolution rows' labels from the table itself (video.cpp fills it with this monitor's modes).
    static std::wstring labels[game::kResRows];
    int i = id - game::kTextResFirst;
    const int32_t* r = reinterpret_cast<const int32_t*>(game::kResTable) + i * 3;
    wchar_t b[48];
    swprintf(b, 48, L"%d x %d x %d", r[0], r[1], r[2]);
    labels[i] = b;
    return labels[i].c_str();
  }
  if (id == 0xE && g_session) {
    // In a network session the title menu's last item does not quit the game (on every PC at once, as
    // the session's every button does): it ends the session, each player stays in their own game.
    static std::wstring leave;
    if (leave.empty()) leave = gamefont::say(u8"ВЫЙТИ ИЗ СЕТЕВОЙ ИГРЫ", "LEAVE THE NETWORK GAME");
    return leave.c_str();
  }
  const wchar_t* t = oGetText(id);
  if (id == 0xE && t && wcscmp(t, L"YNTN") == 0) {
    // The Russian release left the PC-only quit item as "YNTN" (shown "УИТИ").
    static std::wstring quit;
    if (quit.empty()) quit = gamefont::text(u8"ВЫХОД", "QUIT");
    return quit.c_str();
  }
  return t;
}

// --- tmnt2net's settings pages ------------------------------------------------------------------
// Two pages of rows. ЭКРАН is what the game's own ОПЦИИ -> ЭКРАН opens: everything about the picture
// in one place, the game's own three switches and the resolution among tmnt2net's rows. НАСТРОЙКИ МОДА
// in the title menu keeps what is not about the picture. A value changes with left / right, is kept at
// once (tmnt2net.ini; the game's own switches in the game's options) and, where the mod can, takes
// effect at once; the others take effect at the next start of the game (orange until then). Under the
// rows: what the chosen setting does.
struct Choice {
  const char* ru;
  const char* en;
  const wchar_t* value;  // as written to tmnt2net.ini
};
struct Row {
  const char* ru;
  const char* en;
  const wchar_t* section;
  const wchar_t* key;
  const wchar_t* fallback;  // the value when the ini has none
  std::vector<Choice> choices;
  const char* hint_ru;
  const char* hint_en;
  bool live;               // takes effect at once
  int game_text = 0;       // one of the game's own switches: its label is the game's text of this id,
  int game_byte = -1;      // its value a byte of the game's display options
  int cur = 0, start = 0;  // now; what the running game started with
};
struct Group {
  const char* ru;  // null: no heading
  const char* en;
  std::vector<Row> rows;
};
struct Page {
  const char* ru = nullptr;
  const char* en = nullptr;
  std::vector<Group> groups;
  const char* note_ru = nullptr;  // a line under the rows
  const char* note_en = nullptr;
};

Page g_screen, g_mod;
Page* g_open = nullptr;
int g_line = 0, g_scroll = 0;
bool g_page = false;           // the title menu's own page (НАСТРОЙКИ МОДА) is up
uint8_t* g_options = nullptr;  // the game's options sequence (seen at its last update)

// РАЗРЕШЕНИЕ: the monitor's modes; the chosen one becomes the frame's size when the cursor leaves the
// row, on ВВОД, or as the page closes (the game's own change of video mode).
std::deque<std::string> g_res_labels;
std::deque<std::wstring> g_res_values;
Row* g_res_row = nullptr;
int g_res_applied = 0;  // the choice the running game has

int index_of(const Row& r, const std::wstring& v) {
  for (size_t i = 0; i < r.choices.size(); ++i)
    if (v == r.choices[i].value) return (int)i;
  // A number between the choices (an ini written by hand): the nearest one.
  if (!v.empty() && (iswdigit(v[0]) || v[0] == L'-')) {
    double want = _wtof(v.c_str()), best = 1e30;
    int at = -1;
    for (size_t i = 0; i < r.choices.size(); ++i) {
      if (!iswdigit(r.choices[i].value[0]) && r.choices[i].value[0] != L'-') return 0;
      double d = std::fabs(_wtof(r.choices[i].value) - want);
      if (d < best) {
        best = d;
        at = (int)i;
      }
    }
    if (at >= 0) return at;
  }
  return 0;
}

std::wstring ini_value(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
  wchar_t buf[128] = {};
  GetPrivateProfileStringW(section, key, fallback, buf, 128, g_ini.c_str());
  std::wstring v(buf);
  size_t semi = v.find(L';');
  if (semi != std::wstring::npos) v.resize(semi);
  while (!v.empty() && iswspace(v.back())) v.pop_back();
  for (auto& c : v) c = towlower(c);
  return v.empty() ? fallback : v;
}

std::vector<Row*> rows_of(Page& p) {
  std::vector<Row*> out;
  for (Group& g : p.groups)
    for (Row& r : g.rows) out.push_back(&r);
  return out;
}

void build_pages() {
  if (!g_screen.groups.empty()) return;  // once: `start` is what this run of the game began with
  const std::vector<Choice> on_off = {{u8"ВКЛ", "ON", L"1"}, {u8"ВЫКЛ", "OFF", L"0"}};
  std::vector<Choice> res = {{u8"КАК МОНИТОР", "AS THE MONITOR", L"auto"}};
  for (const video::Size& s : video::resolutions()) {
    g_res_labels.push_back(std::to_string(s.w) + " x " + std::to_string(s.h));
    g_res_values.push_back(std::to_wstring(s.w) + L"x" + std::to_wstring(s.h));
    res.push_back({g_res_labels.back().c_str(), g_res_labels.back().c_str(), g_res_values.back().c_str()});
  }
  g_screen.ru = u8"ЭКРАН";
  g_screen.en = "SCREEN";
  g_screen.groups = {
      {u8"ОКНО И РАЗРЕШЕНИЕ", "WINDOW AND RESOLUTION",
       {
           {u8"ОКНО ИГРЫ", "SCREEN MODE", L"video", L"mode", L"borderless",
            {{u8"НА ВЕСЬ МОНИТОР", "BORDERLESS", L"borderless"}, {u8"В ОКНЕ", "WINDOWED", L"windowed"}},
            u8"НА ВЕСЬ МОНИТОР: БЕЗ РАМКИ, МОЖНО СВОБОДНО ПЕРЕКЛЮЧАТЬСЯ В ДРУГИЕ ОКНА. ПОДХОДИТ ПОЧТИ ВСЕМ.",
            "BORDERLESS: NO FRAME, EASY TO SWITCH TO OTHER WINDOWS. RIGHT FOR ALMOST EVERYONE.", false},
           {u8"РАЗРЕШЕНИЕ", "RESOLUTION", L"video", L"resolution", L"auto", res,
            u8"КАК МОНИТОР: КАДР В РАЗРЕШЕНИИ МОНИТОРА, САМАЯ ЧЁТКАЯ КАРТИНКА. МЕНЬШЕ - ЕСЛИ ИГРА ИДЁТ НЕ ПЛАВНО.",
            "AS THE MONITOR: THE FRAME IN THE MONITOR'S RESOLUTION, THE CRISPEST PICTURE. LOWER IF THE GAME IS NOT SMOOTH.", true},
           {u8"ФОРМАТ КАДРА", "FRAME SHAPE", L"video", L"aspect", L"auto",
            {{u8"ПОД МОНИТОР", "AS THE MONITOR", L"auto"}, {u8"4:3 С ПОЛОСАМИ", "4:3 WITH BARS", L"4:3"},
             {u8"РАСТЯНУТЬ", "STRETCH", L"stretch"}},
            u8"ПОД МОНИТОР: НА ШИРОКОМ ЭКРАНЕ КАМЕРА ВИДИТ ШИРЕ, НИЧЕГО НЕ РАСТЯНУТО, МЕНЮ ОСТАЮТСЯ ПО ЦЕНТРУ.",
            "AS THE MONITOR: ON A WIDE SCREEN THE CAMERA SEES WIDER, NOTHING IS STRETCHED, MENUS STAY IN THE MIDDLE.", false},
           {u8"ВЕРТИКАЛЬНАЯ СИНХР.", "VSYNC", L"video", L"vsync", L"-1",
            {{u8"КАК В ИГРЕ", "AS THE GAME", L"-1"}, {u8"ВЫКЛ", "OFF", L"0"}, {u8"ВКЛ", "ON", L"1"}},
            u8"УБИРАЕТ РАЗРЫВЫ КАДРА. ИГРА В ЛЮБОМ СЛУЧАЕ ИДЁТ РОВНО 60 КАДРОВ В СЕКУНДУ.",
            "REMOVES TEARING. THE GAME RUNS AT EXACTLY 60 FRAMES A SECOND EITHER WAY.", false},
       }},
      {u8"КАРТИНКА", "PICTURE",
       {
           {u8"СГЛАЖИВАНИЕ", "ANTIALIASING", L"video", L"antialiasing", L"4",
            {{u8"ВЫКЛ", "OFF", L"0"}, {"2X", "2X", L"2"}, {"4X", "4X", L"4"}, {"8X", "8X", L"8"}},
            u8"УБИРАЕТ «ЛЕСЕНКИ» НА КРАЯХ ГЕРОЕВ И ПРЕДМЕТОВ. 4X ПОДХОДИТ ПОЧТИ ВСЕМ.",
            "REMOVES JAGGED EDGES. 4X IS RIGHT FOR ALMOST EVERYONE.", false},
           {u8"ФИЛЬТР ТЕКСТУР", "TEXTURE FILTER", L"video", L"anisotropy", L"16",
            {{u8"ВЫКЛ", "OFF", L"0"}, {"4X", "4X", L"4"}, {"8X", "8X", L"8"}, {"16X", "16X", L"16"}},
            u8"ПОЛ И СТЕНЫ ОСТАЮТСЯ ЧЁТКИМИ ВДАЛИ И ПОД УГЛОМ.", "FLOORS AND WALLS STAY CRISP FAR AWAY AND AT AN ANGLE.", false},
           {u8"РЕЗКОСТЬ", "SHARPNESS", L"video", L"sharpen", L"50",
            {{u8"ВЫКЛ", "OFF", L"0"}, {u8"СЛАБАЯ", "LOW", L"25"}, {u8"СРЕДНЯЯ", "MEDIUM", L"50"},
             {u8"СИЛЬНАЯ", "HIGH", L"75"}, {u8"МАКСИМУМ", "MAXIMUM", L"100"}},
            u8"КАРТИНКА ЧЁТЧЕ: ЛУЧШЕ ВИДНЫ ТЕКСТУРЫ И МЕЛКИЕ ДЕТАЛИ. МЕНЯЕТСЯ СРАЗУ.",
            "A CRISPER PICTURE: TEXTURES AND SMALL DETAILS SHOW BETTER. CHANGES AT ONCE.", true},
           {u8"ЦВЕТА", "COLOURS", L"video", L"colors", L"25",
            {{u8"КАК В ИГРЕ", "AS THE GAME", L"0"}, {u8"ЧУТЬ ЯРЧЕ", "A LITTLE FULLER", L"25"}, {u8"ЯРКИЕ", "FULL", L"50"},
             {u8"ОЧЕНЬ ЯРКИЕ", "VERY FULL", L"80"}},
            u8"БОЛЕЕ СОЧНЫЕ ЦВЕТА И ЧУТЬ БОЛЬШЕ КОНТРАСТА. МЕНЯЕТСЯ СРАЗУ.",
            "FULLER COLOURS AND A LITTLE MORE CONTRAST. CHANGES AT ONCE.", true},
           {u8"УГОЛ КАМЕРЫ", "FIELD OF VIEW", L"video", L"fov", L"1.0",
            {{"80", "80", L"0.8"}, {"90", "90", L"0.9"}, {"100", "100", L"1.0"}, {"110", "110", L"1.1"},
             {"120", "120", L"1.2"}, {"130", "130", L"1.3"}, {"140", "140", L"1.4"}, {"150", "150", L"1.5"}},
            u8"100 - КАК В ИГРЕ. БОЛЬШЕ - КАМЕРА ВИДИТ ШИРЕ И ГЕРОИ МЕЛЬЧЕ.",
            "100 IS THE GAME'S OWN. MORE: THE CAMERA SEES WIDER, THE HEROES ARE SMALLER.", false},
           {u8"КАМЕРА В СЕТИ", "NETWORK CAMERA", L"video", L"own_camera", L"1",
            {{u8"НА СВОЕГО ГЕРОЯ", "ON MY HERO", L"1"}, {u8"ОДНА НА ВСЕХ", "SHARED", L"0"}},
            u8"В СЕТЕВОЙ ИГРЕ КАМЕРА СЛЕДИТ ЗА ВАШИМ ГЕРОЕМ ИЛИ ДЕРЖИТ В КАДРЕ ВСЕХ. КОЛЕСО МЫШИ - БЛИЖЕ И ДАЛЬШЕ.",
            "IN A NETWORK GAME THE CAMERA FOLLOWS YOUR HERO OR KEEPS EVERYBODY IN VIEW. MOUSE WHEEL: CLOSER AND FARTHER.", true},
       }},
      {u8"НАДПИСИ И ШКАЛЫ", "TEXTS AND GAUGES",
       {
           {u8"МЕНЮ И НАДПИСИ", "MENUS AND TEXTS", L"video", L"hud", L"center",
            {{u8"ПО ЦЕНТРУ", "CENTRED", L"center"}, {u8"НА ВСЮ ШИРИНУ", "FULL WIDTH", L"stretch"}},
            u8"НА ШИРОКОМ ЭКРАНЕ: В ПРАВИЛЬНЫХ ПРОПОРЦИЯХ ПО ЦЕНТРУ ИЛИ РАСТЯНУТЫ НА ВСЮ ШИРИНУ.",
            "ON A WIDE SCREEN: IN THE RIGHT PROPORTIONS IN THE MIDDLE, OR STRETCHED OVER THE WHOLE WIDTH.", false},
           {u8"ШКАЛЫ В УРОВНЕ", "STAGE GAUGES", L"video", L"gauges", L"corners",
            {{u8"ПО УГЛАМ", "IN THE CORNERS", L"corners"}, {u8"БЛИЖЕ К ЦЕНТРУ", "NEAR THE MIDDLE", L"center"}},
            u8"КОЛЬЦО ЗДОРОВЬЯ И ТАБЛИЧКИ ИГРОКОВ: В УГЛАХ ЭКРАНА ИЛИ БЛИЖЕ К ЦЕНТРУ.",
            "THE HEALTH RING AND THE PLAYERS' BOXES: IN THE SCREEN'S CORNERS OR NEARER THE MIDDLE.", true},
           {u8"РАЗМЕР ШКАЛ", "GAUGES SIZE", L"video", L"gauges_size", L"100",
            {{"100%", "100%", L"100"}, {"85%", "85%", L"85"}, {"70%", "70%", L"70"}},
            u8"МЕНЬШЕ ШКАЛЫ - БОЛЬШЕ ВИДНО ИГРУ.", "SMALLER GAUGES SHOW MORE OF THE GAME.", true},
           {u8"ЭФФЕКТЫ УДАРОВ", "HIT EFFECTS", L"", L"", L"", on_off,
            u8"НАДПИСИ, КАК В КОМИКСЕ, ПРИ УДАРАХ. НАСТРОЙКА САМОЙ ИГРЫ; В СЕТЕВОЙ ИГРЕ ОНА У ВСЕХ КАК У ХОСТА.",
            "THE COMIC WORDS AT HITS. THE GAME'S OWN SETTING; IN A NETWORK GAME EVERYBODY HAS THE HOST'S.", true,
            game::kTextHitEffects, game::kOptHitEffects},
           {u8"МЕТКИ ИГРОКОВ", "PLAYER MARKERS", L"", L"", L"", on_off,
            u8"МЕТКИ ИГРОКОВ НАД ГЕРОЯМИ. НАСТРОЙКА САМОЙ ИГРЫ.", "THE PLAYERS' MARKS OVER THE HEROES. THE GAME'S OWN SETTING.", true,
            game::kTextMarkers, game::kOptMarkers},
           {u8"ПОДСКАЗКИ", "HELP", L"", L"", L"", on_off,
            u8"ОКНА С ПОДСКАЗКАМИ В УРОВНЯХ. НАСТРОЙКА САМОЙ ИГРЫ; В СЕТЕВОЙ ИГРЕ ОНА У ВСЕХ КАК У ХОСТА.",
            "THE HELP WINDOWS IN STAGES. THE GAME'S OWN SETTING; IN A NETWORK GAME EVERYBODY HAS THE HOST'S.", true,
            game::kTextHelp, game::kOptHelp},
       }},
  };
  g_mod.ru = u8"НАСТРОЙКИ МОДА";
  g_mod.en = "MOD SETTINGS";
  g_mod.note_ru = u8"ЭКРАН, КАРТИНКА И ШКАЛЫ - В ОПЦИЯХ ИГРЫ: ОПЦИИ → ЭКРАН";
  g_mod.note_en = "SCREEN, PICTURE AND GAUGES ARE IN THE GAME'S OPTIONS: OPTIONS → SCREEN";
  g_mod.groups = {
      {nullptr, nullptr,
       {
           {u8"КЛАВИШИ И МЫШЬ", "KEYS AND MOUSE", L"input", L"layout", L"game",
            {{u8"КАК В ИГРЕ", "AS THE GAME", L"game"}, {u8"wasd + МЫШЬ", "wasd + MOUSE", L"wasd"}},
            u8"wasd + МЫШЬ: ХОДИТЬ - w a s d, АТАКА - ЛКМ, СИЛЬНАЯ АТАКА - ПКМ, ПРЫЖОК - ПРОБЕЛ, ПОДКАТ - shift, БЛОК - q, "
            u8"СЮРИКЕН - e, СМЕНА ГЕРОЯ - f. СВОИ КЛАВИШИ И КНОПКИ МЫШИ: ОПЦИИ → УПРАВЛЕНИЕ.",
            "wasd + MOUSE: MOVE - w a s d, ATTACK - LEFT BUTTON, STRONG ATTACK - RIGHT BUTTON, JUMP - SPACE, SLIDE - shift, "
            "GUARD - q, SHURIKEN - e, CHANGE HERO - f. YOUR OWN KEYS AND MOUSE BUTTONS: OPTIONS → CONTROLS.",
            true},
           {u8"ГЕЙМПАДЫ", "GAMEPADS", L"input", L"pads", L"4",
            {{u8"КАК В ОРИГИНАЛЕ", "AS THE ORIGINAL", L"0"}, {"1", "1", L"1"}, {"2", "2", L"2"}, {"3", "3", L"3"},
             {"4", "4", L"4"}},
            u8"СКОЛЬКО ГЕЙМПАДОВ (xbox, playstation, switch) ЖДЁТ ИГРА. 4 - ДО ЧЕТЫРЁХ ИГРОКОВ НА ОДНОМ КОМПЬЮТЕРЕ.",
            "HOW MANY GAMEPADS (xbox, playstation, switch) THE GAME EXPECTS. 4: UP TO FOUR PLAYERS ON ONE PC.", false},
           {u8"КЛАВИАТУРА", "KEYBOARD", L"input", L"keyboard_players", L"1",
            {{u8"ОДИН ИГРОК", "ONE PLAYER", L"1"}, {u8"ДВА ИГРОКА", "TWO PLAYERS", L"2"}},
            u8"ДВА ИГРОКА: ЛЕВАЯ И ПРАВАЯ ПОЛОВИНЫ КЛАВИАТУРЫ - ДВА ОТДЕЛЬНЫХ ИГРОКА.",
            "TWO PLAYERS: THE LEFT AND THE RIGHT HALF OF THE KEYBOARD ARE TWO PLAYERS.", false},
           {u8"ИГРА В ФОНЕ", "IN THE BACKGROUND", L"video", L"background", L"pause",
            {{u8"ПРОДОЛЖАЕТСЯ", "RUNS", L"run"}, {u8"СТОИТ", "PAUSES", L"pause"}},
            u8"ЧТО ДЕЛАЕТ ИГРА, КОГДА ВЫ ПЕРЕКЛЮЧИЛИСЬ В ДРУГОЕ ОКНО. В СЕТЕВОЙ ИГРЕ ОНА НЕ ОСТАНАВЛИВАЕТСЯ НИКОГДА.",
            "WHAT THE GAME DOES WHILE YOU ARE IN ANOTHER WINDOW. A NETWORK GAME NEVER STOPS.", true},
           {u8"ЗАСТАВКИ ПРИ ЗАПУСКЕ", "INTRO AT START", L"video", L"intro", L"show",
            {{u8"ПОКАЗЫВАТЬ", "SHOW", L"show"}, {u8"ПРОПУСКАТЬ", "SKIP", L"skip"}},
            u8"ПОКАЗЫВАТЬ: ЛОГОТИПЫ И ВСТУПИТЕЛЬНЫЙ РОЛИК ИДУТ КАК В ИГРЕ, ЛЮБАЯ КЛАВИША ИЛИ ЩЕЛЧОК МЫШИ ПРОПУСКАЕТ ИХ. "
            u8"ПРОПУСКАТЬ: ИГРА ВСЕГДА СРАЗУ ОТКРЫВАЕТСЯ В ГЛАВНОМ МЕНЮ.",
            "SHOW: THE LOGOS AND THE OPENING MOVIE PLAY AS IN THE GAME, ANY KEY OR CLICK SKIPS THEM. "
            "SKIP: THE GAME ALWAYS OPENS IN THE MAIN MENU AT ONCE.", true},
       }},
  };
  for (Page* p : {&g_screen, &g_mod})
    for (Row* rp : rows_of(*p)) {
      Row& r = *rp;
      if (r.game_byte >= 0) continue;  // (read from the game each time the page opens)
      if (r.key == std::wstring(L"layout")) continue;  // (the keys the game has now, each time too)
      std::wstring v = ini_value(r.section, r.key, r.fallback);
      if (r.key == std::wstring(L"fov")) {  // 1 / 1.00 / 1.0 alike
        double f = _wtof(v.c_str());
        wchar_t b[8];
        swprintf(b, 8, L"%.1f", f > 0 ? f : 1.0);
        v = b;
      }
      if (r.key == std::wstring(L"aspect") && (v == L"wide" || v == L"hor+")) v = L"auto";
      if (r.key == std::wstring(L"intro") && v == L"0") v = L"show";
      r.cur = r.start = index_of(r, v);
      if (r.key == std::wstring(L"resolution")) {
        g_res_row = &r;
        g_res_applied = r.cur;
      }
    }
}

const Row* find_row(const wchar_t* key) {
  for (Page* p : {&g_screen, &g_mod})
    for (const Row* r : rows_of(*p))
      if (wcscmp(r->key, key) == 0) return r;
  return nullptr;
}

int row_number(const wchar_t* key, int fallback) {
  const Row* r = find_row(key);
  return r ? _wtoi(r->choices[r->cur].value) : fallback;
}

// A value changed: kept, and into the running game where that can be done.
void changed(Row& r) {
  if (r.game_byte >= 0) {  // the game's own switch, as its own page sets it
    if (uint8_t* o = patch::read<uint8_t*>(game::kOptionDataPtr)) o[game::kOptDisplay + r.game_byte] = r.cur == 0 ? 1 : 0;
    return;
  }
  std::wstring key = r.key;
  if (key == L"layout") {  // the keyboard's keys: tmnt2net.ini keeps the keys themselves ([input] keys)
    if (r.cur == 0 || r.cur == 1) {
      input::set_layout(r.cur);
      r.choices.resize(2);  // (the player's own keys are gone: no third value any more)
    }
    return;
  }
  WritePrivateProfileStringW(r.section, r.key, r.choices[r.cur].value, g_ini.c_str());
  if (key == L"sharpen" || key == L"colors") postfx::set(row_number(L"sharpen", 0), row_number(L"colors", 0));
  if (key == L"gauges" || key == L"gauges_size") {
    const Row* g = find_row(L"gauges");
    widescreen::set_gauges(g && g->cur == 0, row_number(L"gauges_size", 100) / 100.0f);
  }
  if (key == L"own_camera") camera::set_own(r.cur == 0);
  if (key == L"background") {
    video::set_background_run(r.cur == 0);
    input::set_background_run(r.cur == 0);
  }
}

// РАЗРЕШЕНИЕ's choice becomes the frame's size (the game's own change of video mode, then its options
// page laid out again on the new frame, as the game does after its own row).
void commit_resolution() {
  if (!g_res_row || g_res_row->cur == g_res_applied) return;
  auto size_of = [](const Row& r, int i, int& w, int& h) {
    w = h = 0;
    swscanf(r.choices[i].value, L"%dx%d", &w, &h);  // ("auto": 0 x 0)
  };
  int w = 0, h = 0;
  size_of(*g_res_row, g_res_row->cur, w, h);
  bool ok = video::set_resolution(w, h);
  if (ok) {
    g_res_applied = g_res_row->cur;
    LOGI("menus: РАЗРЕШЕНИЕ: %ls, at once", g_res_row->choices[g_res_row->cur].value);
  } else {  // the game could not: the size before, in the game and in the ini
    g_res_row->cur = g_res_applied;
    WritePrivateProfileStringW(g_res_row->section, g_res_row->key, g_res_row->choices[g_res_row->cur].value, g_ini.c_str());
    size_of(*g_res_row, g_res_row->cur, w, h);
    video::set_resolution(w, h);
  }
  int fw = 0, fh = 0;
  if (uint8_t* o = patch::read<uint8_t*>(game::kOptionDataPtr); o && video::frame_size(fw, fh)) {
    const int32_t* t = reinterpret_cast<const int32_t*>(game::kResTable);  // the game's own row, where it has one
    for (int32_t i = 0; i < game::kResRows; ++i)
      if (t[i * 3] == fw && t[i * 3 + 1] == fh) std::memcpy(o + game::kOptDisplay + game::kOptVideoRow, &i, 4);
  }
  if (g_open == &g_screen && g_options)
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int)>(game::kOptionsSettingInit)(g_options, nullptr, 1);
}

bool restart_waits() {
  for (Page* p : {&g_screen, &g_mod})
    for (const Row* r : rows_of(*p))
      if (!r->live && r->cur != r->start) return true;
  return false;
}

void page_open(Page& p) {
  build_pages();
  if (const uint8_t* o = patch::read<const uint8_t*>(game::kOptionDataPtr))
    for (Row* r : rows_of(p))
      if (r->game_byte >= 0) r->cur = r->start = o[game::kOptDisplay + r->game_byte] ? 0 : 1;
  for (Row* r : rows_of(p))
    if (r->key == std::wstring(L"layout")) {
      // The keys the game has now; keys set by hand on the game's page are a third value, shown while
      // they last (choosing one of the two sets replaces them).
      int now = input::layout();
      r->choices.resize(2);
      if (now == 2) r->choices.push_back({u8"СВОИ", "MY OWN", L"own"});
      r->cur = r->start = now;
    }
  g_open = &p;
  g_line = g_scroll = 0;
  g_page = &p == &g_mod;
  LOGI("menus: %s opened", &p == &g_mod ? "НАСТРОЙКИ МОДА" : "ОПЦИИ → ЭКРАН (tmnt2net's page)");
}

void page_close() {
  commit_resolution();
  LOGI("menus: %s closed%s", g_open == &g_mod ? "НАСТРОЙКИ МОДА" : "ОПЦИИ → ЭКРАН",
       restart_waits() ? " (some settings wait for the next start)" : "");
  g_open = nullptr;
  g_page = false;
}

void page_update() {
  if (!g_open) return;
  bool up = gamefont::pressed(1), down = gamefont::pressed(2), left = gamefont::pressed(4), right = gamefont::pressed(8);
  bool ok = gamefont::pressed(gamefont::ok_mask()), cancel = gamefont::pressed(gamefont::cancel_mask());
  std::vector<Row*> rows = rows_of(*g_open);
  int lines = (int)rows.size() + 1;  // + НАЗАД
  if (up || down) {
    commit_resolution();
    g_line = (g_line + (up ? lines - 1 : 1)) % lines;
    gamefont::sound(0);
  } else if (cancel || (ok && g_line == lines - 1)) {
    gamefont::sound(2);
    page_close();
  } else if (g_line < (int)rows.size() && (left || right || ok)) {
    Row& r = *rows[g_line];
    if (ok && &r == g_res_row && r.cur != g_res_applied) {
      gamefont::sound(1);
      commit_resolution();
      return;
    }
    int n = (int)r.choices.size();
    r.cur = (r.cur + (left ? n - 1 : 1)) % n;
    changed(r);
    gamefont::sound(0);
  }
}

void page_draw() {
  if (!g_open) return;
  using pageui::Align;
  Page& p = *g_open;
  pageui::Ui ui;
  auto T = [](const char* ru, const char* en) { return gamefont::say(ru, en); };
  pageui::Window w = pageui::window(ui, T(p.ru, p.en),
                                    T(u8"ВЛЕВО / ВПРАВО→ИЗМЕНИТЬ      esc→НАЗАД", "LEFT / RIGHT→CHANGE      esc→BACK"));
  // The lines: a group's name, its rows, ... and НАЗАД; as many as fit, the chosen one always in sight.
  struct Line {
    int kind;  // 0 a group's name, 1 a row, 2 НАЗАД
    const Group* g;
    Row* r;
  };
  std::vector<Line> lines;
  int sel = 0, n = 0;
  for (Group& g : p.groups) {
    if (g.ru) lines.push_back({0, &g, nullptr});
    for (Row& r : g.rows) {
      if (n++ == g_line) sel = (int)lines.size();
      lines.push_back({1, &g, &r});
    }
  }
  if (n == g_line) sel = (int)lines.size();
  lines.push_back({2, nullptr, nullptr});
  const float step = 25.0f, h = 15.5f, value_x = w.x1 - 136;
  const float hy = w.bottom - 84, top = w.top + 13;
  const int fit = std::max(3, (int)((hy - 6 - top) / step));
  int count = (int)lines.size();
  if (count <= fit) {
    g_scroll = 0;
  } else {
    int first = sel > 0 && lines[sel - 1].kind == 0 ? sel - 1 : sel;  // a group's first row with the group's name
    if (first < g_scroll) g_scroll = first;
    if (sel > g_scroll + fit - 1) g_scroll = sel - fit + 1;
    g_scroll = std::max(0, std::min(g_scroll, count - fit));
  }
  float y = top;
  for (int i = g_scroll; i < count && i < g_scroll + fit; ++i, y += step) {
    const Line& l = lines[i];
    if (l.kind == 0) {
      std::wstring name = T(l.g->ru, l.g->en);
      ui.text(name, w.x0 + 22, y + 3, 12.5f, pageui::kLime);
      ui.rect(w.x0 + 30 + pageui::width(name, 12.5f), y + 7, w.x1 - 26, y + 8, 0x509EDC3C);
      continue;
    }
    bool chosen = i == sel;
    if (chosen) pageui::chosen(ui, w, y, pageui::bottom_of(y, h));
    if (l.kind == 2) {
      ui.text(T(u8"НАЗАД", "BACK"), w.x0 + 34, y, h, chosen ? pageui::pulse() : pageui::kItem);
      continue;
    }
    const Row& r = *l.r;
    const wchar_t* own = r.game_text && oGetText ? oGetText(r.game_text) : nullptr;  // the game's own words
    ui.text(own && *own ? std::wstring(own) : T(r.ru, r.en), w.x0 + 34, y, h, chosen ? pageui::pulse() : pageui::kItem);
    std::wstring v = T(r.choices[r.cur].ru, r.choices[r.cur].en);
    bool waits = &r == g_res_row ? r.cur != g_res_applied : !r.live && r.cur != r.start;
    uint32_t vc = waits ? pageui::kWarn : chosen ? pageui::kText : 0xD8E0D8;
    ui.text(v, value_x, y, h, vc, Align::Center);
    if (chosen) pageui::arrows(ui, value_x, y, h, pageui::width(v, h) / 2, pageui::kLime);
  }
  if (count > fit) {  // where in the list: a track with a thumb, marks when there is more above / below
    const float tx = w.x1 - 13, t0 = top - 2, t1 = top + fit * step - 8;
    ui.rect(tx, t0, tx + 3, t1, 0x40FFFFFF);
    float a = t0 + (t1 - t0) * g_scroll / count, b = t0 + (t1 - t0) * (g_scroll + fit) / count;
    ui.rect(tx, a, tx + 3, b, 0xFF9EDC3C);
    if (g_scroll > 0) ui.tri(tx - 4, t0 - 4, tx + 7, t0 - 4, tx + 1.5f, t0 - 11, 0xFF9EDC3C);
    if (g_scroll + fit < count) ui.tri(tx - 4, t1 + 4, tx + 7, t1 + 4, tx + 1.5f, t1 + 11, 0xFF9EDC3C);
  }
  if (p.note_ru) {
    ui.text(T(p.note_ru, p.note_en), 0, hy - 46, 12.5f, pageui::kDim, Align::Center);
    ui.text(gamefont::say(L"tmnt2net " TMNT2NET_VERSION_W) + T(u8" - В СЕТЕВОЙ ИГРЕ У ВСЕХ ДОЛЖНА БЫТЬ ОДНА ВЕРСИЯ",
                                                             " - A NETWORK GAME NEEDS THE SAME VERSION ON EVERY PC"),
            0, hy - 26, 11.5f, pageui::kOff, Align::Center);
  }
  // What the chosen setting does.
  pageui::rule(ui, w, hy);
  std::vector<Row*> rows = rows_of(p);
  if (g_line < (int)rows.size()) {
    const Row& r = *rows[g_line];
    float after = ui.wrapped(T(r.hint_ru, r.hint_en), 0, hy + 10, 12.5f, (w.x1 - w.x0) - 56, 16, pageui::kText, Align::Center,
                             r.live ? 4 : 3);  // (a row that waits for a restart keeps the last line for saying so)
    float ny = std::max(after + 2, w.bottom - 20);
    if (&r == g_res_row) {
      bool waits = r.cur != g_res_applied;
      ui.text(waits ? T(u8"ВВОД→ПРИМЕНИТЬ СЕЙЧАС", "enter→APPLY NOW") : T(u8"МЕНЯЕТСЯ СРАЗУ, БЕЗ ПЕРЕЗАПУСКА ИГРЫ", "CHANGES AT ONCE"), 0,
              ny, 11.5f, waits ? pageui::kWarn : pageui::kOff, Align::Center);
    } else if (!r.live) {
      bool waits = r.cur != r.start;
      ui.text(waits ? T(u8"ВСТУПИТ В СИЛУ ПОСЛЕ ПЕРЕЗАПУСКА ИГРЫ", "APPLIES AFTER THE GAME IS RESTARTED")
                    : T(u8"МЕНЯЕТСЯ ПОСЛЕ ПЕРЕЗАПУСКА ИГРЫ", "CHANGES AFTER THE GAME IS RESTARTED"),
              0, ny, 11.5f, waits ? pageui::kWarn : pageui::kOff, Align::Center);
    }
  } else if (restart_waits()) {
    ui.text(T(u8"ЧАСТЬ НАСТРОЕК ВСТУПИТ В СИЛУ ПОСЛЕ ПЕРЕЗАПУСКА ИГРЫ", "SOME SETTINGS APPLY AFTER THE GAME IS RESTARTED"), 0, hy + 14,
            12.5f, pageui::kWarn, Align::Center);
  }
  ui.draw();
}

// --- hooks ----------------------------------------------------------------------------------
using Method_t = void(__fastcall*)(uint8_t* self, void* edx);
Method_t oUpdateMenu = nullptr, oDrawMenu = nullptr, oDispatcherDraw = nullptr;

// The title sequence: a network game's start-up runs to its menu (netplay::title_state).
using TitleUpdate_t = void(__fastcall*)(uint8_t* self, void* edx, int a, int b);
TitleUpdate_t oTitleUpdate = nullptr;
uint8_t* g_title = nullptr;  // the title sequence seen last (game thread)
DWORD g_title_seen = 0;      // when it updated last: gone once the sequence ends

void __fastcall hkTitleUpdate(uint8_t* self, void* edx, int a, int b) {
  g_title = self;
  g_title_seen = GetTickCount();
  if (self) {
    int phase = *reinterpret_cast<int32_t*>(self + game::kTitlePhase), step = *reinterpret_cast<int32_t*>(self + game::kTitleStep);
    if (phase == 2 && step >= 1) sim::title_menu_reached();
    netplay::title_state(phase, step);
  }
  oTitleUpdate(self, edx, a, b);
}
using GetSelect_t = int(__fastcall*)(uint8_t* self, void* edx);

void begin_fade_out(uint8_t* title) {
  void** vt = *reinterpret_cast<void***>(title);
  reinterpret_cast<Method_t>(vt[0x58 / 4])(title, nullptr);
}

void __fastcall hkUpdateMenu(uint8_t* self, void* edx) {
  bool ours = (!g_session && netpage::active()) || g_page;
  if (ours) *reinterpret_cast<float*>(self + game::kTitleIdle) = 0.0f;  // no demo while a page of ours is up
  if (!g_session && netpage::active()) {
    netpage::update();
    return;
  }
  if (g_open == &g_mod) {
    page_update();
    return;
  }
  if (g_session && gamefont::pressed(gamefont::ok_mask()) &&
      reinterpret_cast<GetSelect_t>(game::kTitleGetSelect)(self, nullptr) == kQuit) {
    gamefont::sound(1);
    netplay::leave_session();  // (the same frame on every PC: the session's buttons are everybody's)
    return;
  }
  if (!g_session && gamefont::pressed(gamefont::ok_mask())) {
    int item = reinterpret_cast<GetSelect_t>(game::kTitleGetSelect)(self, nullptr);
    if (item == kNet) {
      gamefont::sound(1);
      netpage::open();
      return;
    }
    if (item == kExtra) {
      gamefont::sound(1);
      page_open(g_mod);
      return;
    }
  }
  oUpdateMenu(self, edx);
}

// [debug] font_sheet: every code 33..255 with its glyph, and the codes of the
// game's own menu words in the log, to learn how the font shows letters.
void font_sheet() {
  static bool logged = false;
  if (!logged) {
    logged = true;
    const int ids[] = {0x232, 0x233, 0xD, 0xE};
    for (int id : ids) {
      const wchar_t* t = oGetText(id);
      std::string codes;
      for (const wchar_t* c = t; c && *c; ++c) codes += std::to_string((unsigned)*c) + " ";
      LOGI("menus: text 0x%X codes: %s", id, codes.c_str());
    }
  }
  gamefont::panel(-320, -240, 320, 240, 230);
  gamefont::begin();
  int col = 0, row = 0;
  for (int c = 33; c < 256; ++c) {
    if (c >= 127 && c < 160) continue;
    float x = -312.0f + col * 33.0f, y = -236.0f + row * 39.0f;
    wchar_t g[2] = {(wchar_t)c, 0};
    gamefont::color(255, 255, 0);
    gamefont::show(g, x, y, 20);
    gamefont::color(150, 220, 255);
    gamefont::show(std::to_wstring(c), x, y + 20, 11);
    if (++col == 19) {
      col = 0;
      ++row;
    }
  }
  gamefont::end();
}

void __fastcall hkDrawMenu(uint8_t* self, void* edx) {
  if (g_cfg.font_sheet) {
    font_sheet();
    return;
  }
  if (!g_session && netpage::active()) {
    netpage::draw();
    return;
  }
  if (g_open == &g_mod) {
    page_draw();
    return;
  }
  oDrawMenu(self, edx);
  pageui::Ui ui;  // which tmnt2net this is: a network game needs the same on every PC
  ui.text(gamefont::say(L"tmnt2net " TMNT2NET_VERSION_W), 0, 227, 10.5f, pageui::kOff, pageui::Align::Center);
  ui.draw();
}

// The pause menu. On the PC the game opens it by the Esc key alone, read from the keyboard itself. In a
// network session the game's keyboard reads nothing - every player's keyboard is their pad of the
// session, Esc its SELECT and Enter its START - so a session could not be paused, nor left for the menu;
// and no gamepad's START has ever paused this game on a PC. Here: START or SELECT of any pad as well
// (the pads are the session's input: the same frame on every PC).
using CheckPause_t = bool(__fastcall*)(void* self, void* edx);
CheckPause_t oCheckPause = nullptr;

bool __fastcall hkCheckPause(void* self, void* edx) {
  auto trigger = reinterpret_cast<int(__cdecl*)(int, uint32_t)>(game::kGetDigitalTrigger);
  bool key = oCheckPause(self, edx);
  // (-1: the pads locked to players, -2: the others; in a stage a session's pads are of the second kind)
  const uint32_t buttons = game::kDigitalStart | game::kDigitalSelect;
  bool pad = (trigger(-1, buttons) & 0xFF) != 0 || (trigger(-2, buttons) & 0xFF) != 0;
  if (g_cfg.log_cameras) {  // [debug]
    static int polls = 0, said = 0;
    if (polls++ % 600 == 0) LOGI("[debug] pause check: polled (%d times)", polls);
    int any = 0;
    for (int port = -2; port < 4; ++port) any |= trigger(port, 0x300) ? 1 << (port + 2) : 0;
    if ((key || pad || any) && said++ < 12)
      LOGI("[debug] pause check: key %d, pad %d, START/SELECT by port (bit 0 = -2 .. bit 5 = 3): %02X, frame %u", key, pad, any,
           sim::frame());
  }
  return key || pad;
}

// The game's own ОПЦИИ: ЭКРАН opens tmnt2net's page of everything about the picture (the game's own
// rows of that page are on it). Not in a network session: there the options page runs on every PC
// alike, from the same buttons, and stays the game's own.
using OptionsMove_t = int(__fastcall*)(uint8_t* self, void* edx);
OptionsMove_t oOptionsMove = nullptr;
Method_t oOptionsDraw = nullptr;

int __fastcall hkOptionsMove(uint8_t* self, void* edx) {
  g_options = self;
  if (g_open == &g_screen) {
    page_update();
    return 0;
  }
  if (!g_session && gamefont::ready() && *reinterpret_cast<int32_t*>(self + game::kOptionsMode) == 0 &&
      *reinterpret_cast<int32_t*>(self + game::kOptionsLine) == game::kOptionsLineDisplay &&
      gamefont::pressed(gamefont::ok_mask())) {
    gamefont::sound(1);
    page_open(g_screen);
    return 0;
  }
  return oOptionsMove(self, edx);
}

void __fastcall hkOptionsDraw(uint8_t* self, void* edx) {
  oOptionsDraw(self, edx);
  if (g_open == &g_screen) page_draw();
}

// Notices with the game's font, on top of everything the game drew.
void __fastcall hkDispatcherDraw(uint8_t* self, void* edx) {
  oDispatcherDraw(self, edx);
  if (!gamefont::ready()) return;
  coop::after_frame();
  std::wstring t;
  uint32_t c = 0;
  if (!overlay::peek_notice(&t, &c)) return;
  std::wstring w = gamefont::say(t);
  if (w.empty()) return;  // (nothing the game's font can show: the overlay writes it)
  overlay::notice_drawn();
  pageui::Ui ui;
  pageui::notice(ui, w, c & 0xFFFFFF);
  ui.draw();
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  g_ini = module_dir() + L"tmnt2net.ini";
  if (patch::bytes_are(game::kGetText, game::kGetTextHead, sizeof(game::kGetTextHead)))
    patch::hook("CGameText::GetText (tmnt2net's words)", game::kGetText, (void*)&hkGetText, (void**)&oGetText);
  if (patch::bytes_are(game::kTitleUpdate, game::kTitleUpdateHead, sizeof(game::kTitleUpdateHead)))
    patch::hook("title: update (the start-up runs at once to its menu)", game::kTitleUpdate,
                (void*)&hkTitleUpdate, (void**)&oTitleUpdate);
  if (patch::bytes_are(game::kDispatcherDraw, game::kDispatcherDrawHead, sizeof(game::kDispatcherDrawHead)))
    patch::hook("CProcessDispatcher::Draw (notices in the game's font)", game::kDispatcherDraw,
                (void*)&hkDispatcherDraw, (void**)&oDispatcherDraw);
  if (patch::bytes_are(game::kCheckPauseMenu, game::kCheckPauseMenuHead, sizeof(game::kCheckPauseMenuHead)))
    patch::hook("CGameStage::CheckPauseMenu (the pause menu by START / SELECT of a pad too)", game::kCheckPauseMenu,
                (void*)&hkCheckPause, (void**)&oCheckPause);
  else
    LOGW("menus: pause check not recognised: no pause menu in a network game");
  // The area map: its menu (the heroes, options, save, load, back to the title) opens by the guard key
  // alone, and the map answers no CANCEL: Esc did nothing there, and there was no way from the map to
  // the title for who does not know that key. SELECT - the keyboard's Esc - opens it as well; the menu
  // closes by CANCEL, the same Esc. For every PC alike, whatever its settings: the map is a part of a
  // session's game.
  patch::replace("area map: Esc (the pad's SELECT) opens the map's menu, as the guard key does", game::kAreaMenuKey,
                 game::kAreaMenuKeyOld, game::kAreaMenuKeyNew, sizeof(game::kAreaMenuKeyNew));
  if (!oGetText) return;
  if (patch::bytes_are(game::kOptionsMove, game::kOptionsMoveHead, sizeof(game::kOptionsMoveHead)) &&
      patch::bytes_are(game::kOptionsDraw, game::kOptionsDrawHead, sizeof(game::kOptionsDrawHead))) {
    patch::hook("options: page logic (ЭКРАН opens tmnt2net's page)", game::kOptionsMove, (void*)&hkOptionsMove,
                (void**)&oOptionsMove);
    patch::hook("options: draw", game::kOptionsDraw, (void*)&hkOptionsDraw, (void**)&oOptionsDraw);
  } else {
    LOGW("menus: options page not recognised, ЭКРАН stays the game's own (tmnt2net's rows: tmnt2net.ini)");
  }
  init_items();
  if (!move_table()) return;
  // UpdateMenu's first instruction reads the table: hooked after the move.
  patch::hook("title menu: update", game::kTitleUpdateMenu, (void*)&hkUpdateMenu, (void**)&oUpdateMenu);
  if (patch::bytes_are(game::kTitleDrawMenu, game::kTitleDrawMenuHead, sizeof(game::kTitleDrawMenuHead)))
    patch::hook("title menu: draw", game::kTitleDrawMenu, (void*)&hkDrawMenu, (void**)&oDrawMenu);
}

uint8_t* live_title() { return g_title && GetTickCount() - g_title_seen < 250 ? g_title : nullptr; }

// ПРОДОЛЖИТЬ is on when there is progress (as the title's start sets it, 0x4EB955): after the
// progress changed under the title (a session's, then ours again).
void continue_by_progress() {
  g_items[kContinue].enabled = patch::read<uint8_t>(game::kNewGameFlag) == 0 ? 1 : 0;
}

void on_session_end() {
  g_session = false;
  layout(true);
  continue_by_progress();
}

bool title_menu_up() {
  uint8_t* t = live_title();
  return t && *reinterpret_cast<int32_t*>(t + game::kTitlePhase) == 2 &&
         *reinterpret_cast<int32_t*>(t + game::kTitleStep) >= 1;
}

bool title_before_menu() {
  uint8_t* t = live_title();
  return t && *reinterpret_cast<int32_t*>(t + game::kTitleStep) == 0;
}

void on_session_start() {
  netpage::close();
  g_session = true;
  g_open = nullptr;
  g_page = false;
  layout(false);
  continue_by_progress();
  if (uint8_t* t = live_title(); t && *reinterpret_cast<int32_t*>(t + game::kTitlePhase) == 2) {
    g_title = t;
    // A session in a running game begins at the title menu: the same menu state on every PC. With
    // progress in the session the cursor waits on ПРОДОЛЖИТЬ (НОВАЯ ИГРА there would start it all over).
    *reinterpret_cast<int32_t*>(g_title + game::kTitleCursor) = g_items[kContinue].enabled ? 1 : 0;
    *reinterpret_cast<float*>(g_title + game::kTitleIdle) = 0.0f;
    *reinterpret_cast<float*>(g_title + game::kTitleIdle + 4) = 0.0f;
  }
  LOGI("menus: network session: the game's own title menu");
}

}  // namespace menus
