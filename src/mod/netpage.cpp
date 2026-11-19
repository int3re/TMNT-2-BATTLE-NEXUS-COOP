#include "mod/netpage.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "common/log.h"
#include "mod/gamefont.h"
#include "mod/input.h"
#include "mod/netplay.h"
#include "mod/overlay.h"
#include "mod/pageui.h"

namespace netpage {
namespace {

using pageui::Align;
using pageui::Ui;

bool g_open = false;
bool g_auto_opened = false;  // the page came up by itself for this lobby (or the session closed it)
int g_line = 0;
std::string g_last_error;

// What the page shows: by the lobby's state, with two views of its own on top (the friends to
// call, the host's code being typed).
enum class View { Main, Host, Invite, Browse, Code, Connecting, Joined, Failed };
enum class Own { None, Invite, Code };
Own g_own = Own::None;
std::string g_code;              // the code typed so far
uint8_t g_keys_before[256] = {};  // the keyboard a frame ago (typing: keys newly down)
std::set<uint64_t> g_called;     // friends invited from this page

std::wstring wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

std::wstring T(const char* ru, const char* en) { return gamefont::say(ru, en); }

std::wstring player_name(const std::string& name, int p) {
  std::wstring n = gamefont::name(wide(name));
  return n.empty() ? T(u8"ИГРОК ", "PLAYER ") + std::to_wstring(p + 1) : n;
}

View view_of(const netplay::LobbyView& v) {
  if (v.role == 0) return g_own == Own::Code ? View::Code : View::Main;
  if (v.failed) return View::Failed;
  if (v.role == 1) return g_own == Own::Invite && v.can_invite ? View::Invite : View::Host;
  if (v.joined) return View::Joined;
  return v.browsing ? View::Browse : View::Connecting;
}

int players_in(const netplay::LobbyView& v, bool* all_ready) {
  int n = 0;
  if (all_ready) *all_ready = true;
  for (int p = 0; p < 4; ++p) {
    if (!v.players[p].present) continue;
    ++n;
    if (all_ready && p != 0 && !v.players[p].ready) *all_ready = false;
  }
  return n;
}

// A row the cursor can stand on.
struct Row {
  std::wstring label;
  std::wstring about;   // a line under it (the first page's cards)
  std::wstring status;  // at the right
  uint32_t status_rgb = pageui::kDim;
  int id = 0;           // what OK does (below)
  uint64_t arg = 0;
};
enum {
  kBack = 1, kOpenHost, kOpenList, kOpenCode, kStart, kInviteList, kCopyCode, kCloseGame, kInvite, kPick, kLeave,
  kWaitReady
};

std::vector<Row> rows(View w, const netplay::LobbyView& v) {
  std::vector<Row> r;
  switch (w) {
    case View::Main:
      r.push_back({T(u8"ОТКРЫТЬ СВОЮ ИГРУ", "HOST A GAME"),
                   T(u8"ВЫ ХОСТ: ДРУЗЬЯ ВХОДЯТ К ВАМ, ДО ЧЕТЫРЁХ ИГРОКОВ", "YOU HOST: FRIENDS JOIN YOU, UP TO FOUR PLAYERS"), L"",
                   0, kOpenHost});
      r.push_back({T(u8"ВОЙТИ К ДРУГУ", "JOIN A FRIEND"),
                   T(u8"ДРУГ УЖЕ ОТКРЫЛ ИГРУ: ВЫБЕРИТЕ ЕЁ ИЗ СПИСКА (steam)", "A FRIEND HOSTS: PICK THE GAME FROM THE LIST (steam)"),
                   L"", 0, kOpenList});
      r.push_back({T(u8"ВОЙТИ ПО КОДУ", "JOIN BY CODE"),
                   T(u8"ЛОКАЛЬНАЯ СЕТЬ ИЛИ ИНТЕРНЕТ: КОД ИЛИ ip-АДРЕС ХОСТА", "LAN OR INTERNET: THE HOST'S CODE OR ip ADDRESS"), L"",
                   0, kOpenCode});
      r.push_back({T(u8"НАЗАД", "BACK"), L"", L"", 0, kBack});
      break;
    case View::Host: {
      bool ready = true;
      int n = players_in(v, &ready);
      if (!ready)
        r.push_back({T(u8"ПОДОЖДИТЕ: ИГРОК ПОЛУЧАЕТ ДАННЫЕ", "WAIT: A PLAYER GETS THE DATA"), L"", L"", 0, kWaitReady});
      else if (n <= 1)
        r.push_back({T(u8"НАЧАТЬ ИГРУ ОДНОМУ", "START ALONE"), L"", L"", 0, kStart});
      else
        r.push_back({T(u8"СТАРТ ИГРЫ", "START THE GAME"), L"", T(u8"ИГРОКОВ: ", "PLAYERS: ") + std::to_wstring(n),
                     pageui::kGood, kStart});
      if (v.can_invite) r.push_back({T(u8"ПОЗВАТЬ ДРУГА", "INVITE A FRIEND"), L"", L"steam", pageui::kDim, kInviteList});
      if (!v.code.empty())
        r.push_back({T(u8"СКОПИРОВАТЬ КОД", "COPY THE CODE"), L"", gamefont::name(wide(v.code)), pageui::kDim, kCopyCode});
      r.push_back({T(u8"ЗАКРЫТЬ ИГРУ", "CLOSE THE GAME"), L"", L"", 0, kCloseGame});
      break;
    }
    case View::Invite:
      for (size_t i = 0; i < v.friends.size() && i < 8; ++i) {
        const netplay::LobbyFriend& f = v.friends[i];
        std::wstring n = gamefont::name(wide(f.name));
        if (n.empty()) continue;
        bool called = g_called.count(f.id) != 0;
        r.push_back({n, L"", called ? T(u8"ПОЗВАЛИ", "INVITED") : f.in_game ? T(u8"В ИГРЕ", "IN A GAME") : L"",
                     called ? pageui::kGood : pageui::kDim, kInvite, f.id});
      }
      r.push_back({T(u8"НАЗАД", "BACK"), L"", L"", 0, kBack});
      break;
    case View::Browse:
      for (const netplay::LobbyGame& g : v.games) {
        if (!g.compatible) continue;
        std::wstring n = gamefont::name(wide(g.host));
        if (n.empty()) n = T(u8"ИГРА ДРУГА", "A FRIEND'S GAME");
        r.push_back({n, L"", g.running ? T(u8"УЖЕ ИГРАЕТ: МОЖНО ВОЙТИ", "PLAYING: YOU CAN JOIN") : T(u8"ЖДЁТ ИГРОКОВ", "WAITS FOR PLAYERS"),
                     g.running ? pageui::kItem : pageui::kGood, kPick, g.lobby});
      }
      r.push_back({T(u8"НАЗАД", "BACK"), L"", L"", 0, kBack});
      break;
    case View::Code:
      break;  // typed, no rows
    case View::Connecting:
      r.push_back({T(u8"ОТМЕНА", "CANCEL"), L"", L"", 0, kLeave});
      break;
    case View::Joined:
      r.push_back({T(u8"ВЫЙТИ ИЗ ИГРЫ", "LEAVE THE GAME"), L"", L"", 0, kLeave});
      break;
    case View::Failed:
      r.push_back({T(u8"НАЗАД", "BACK"), L"", L"", 0, kLeave});
      break;
  }
  return r;
}

void close_page() {
  netplay::lobby_close();
  g_open = false;
  g_line = 0;
  g_own = Own::None;
  g_auto_opened = false;
}

void leave_lobby() {
  netplay::lobby_close();
  g_line = 0;
  g_own = Own::None;
  g_called.clear();
}

void to_clipboard(const std::string& s) {
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1)) {
    std::memcpy(GlobalLock(h), s.c_str(), s.size() + 1);
    GlobalUnlock(h);
    if (!SetClipboardData(CF_TEXT, h)) GlobalFree(h);
  }
  CloseClipboard();
}

std::string from_clipboard() {
  std::string s;
  if (!OpenClipboard(nullptr)) return s;
  if (HANDLE h = GetClipboardData(CF_TEXT)) {
    if (const char* p = static_cast<const char*>(GlobalLock(h))) {
      s = p;
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return s;
}

bool code_char(char c) { return std::isalnum((unsigned char)c) || c == '.' || c == ':' || c == '-'; }

void type(const std::string& more) {
  for (char c : more)
    if (code_char(c) && g_code.size() < 40) g_code += (char)std::toupper((unsigned char)c);
}

// The host's code is typed on the keyboard itself (the game's buttons are letters there): keys
// newly down this frame. Returns 1 for Enter, -1 for Esc.
int typing() {
  static const struct {
    uint8_t dik;
    char c;
  } kKeys[] = {{DIK_1, '1'}, {DIK_2, '2'}, {DIK_3, '3'}, {DIK_4, '4'}, {DIK_5, '5'}, {DIK_6, '6'}, {DIK_7, '7'},
               {DIK_8, '8'}, {DIK_9, '9'}, {DIK_0, '0'}, {DIK_NUMPAD1, '1'}, {DIK_NUMPAD2, '2'}, {DIK_NUMPAD3, '3'},
               {DIK_NUMPAD4, '4'}, {DIK_NUMPAD5, '5'}, {DIK_NUMPAD6, '6'}, {DIK_NUMPAD7, '7'}, {DIK_NUMPAD8, '8'},
               {DIK_NUMPAD9, '9'}, {DIK_NUMPAD0, '0'}, {DIK_A, 'A'}, {DIK_B, 'B'}, {DIK_C, 'C'}, {DIK_D, 'D'},
               {DIK_E, 'E'}, {DIK_F, 'F'}, {DIK_G, 'G'}, {DIK_H, 'H'}, {DIK_I, 'I'}, {DIK_J, 'J'}, {DIK_K, 'K'},
               {DIK_L, 'L'}, {DIK_M, 'M'}, {DIK_N, 'N'}, {DIK_O, 'O'}, {DIK_P, 'P'}, {DIK_Q, 'Q'}, {DIK_R, 'R'},
               {DIK_S, 'S'}, {DIK_T, 'T'}, {DIK_U, 'U'}, {DIK_V, 'V'}, {DIK_W, 'W'}, {DIK_X, 'X'}, {DIK_Y, 'Y'},
               {DIK_Z, 'Z'}, {DIK_MINUS, '-'}, {DIK_SUBTRACT, '-'}, {DIK_PERIOD, '.'}, {DIK_DECIMAL, '.'},
               {DIK_SEMICOLON, ':'}};
  uint8_t keys[256] = {};
  if (!input::read_keyboard(keys)) return 0;
  auto hit = [&](int dik) { return (keys[dik] & 0x80) && !(g_keys_before[dik] & 0x80); };
  int done = 0;
  bool ctrl = (keys[DIK_LCONTROL] & 0x80) || (keys[DIK_RCONTROL] & 0x80);
  if (hit(DIK_RETURN) || hit(DIK_NUMPADENTER)) {
    done = 1;
  } else if (hit(DIK_ESCAPE)) {
    done = -1;
  } else if (hit(DIK_BACK)) {
    if (!g_code.empty()) g_code.pop_back();
  } else if (ctrl && hit(DIK_V)) {
    type(from_clipboard());
  } else if (!ctrl) {
    for (const auto& k : kKeys)
      if (hit(k.dik)) type(std::string(1, k.c));
  }
  std::memcpy(g_keys_before, keys, sizeof(keys));
  return done;
}

void act(const Row& row, const netplay::LobbyView& v) {
  switch (row.id) {
    case kBack:
      gamefont::sound(2);
      if (g_own != Own::None) {
        g_own = Own::None;
        g_line = 0;
      } else if (v.role == 0) {
        close_page();
      } else {
        leave_lobby();
      }
      break;
    case kOpenHost:
      gamefont::sound(1);
      netplay::lobby_open_host();
      g_line = 0;
      break;
    case kOpenList:
      gamefont::sound(1);
      netplay::lobby_open_client("list");
      g_line = 0;
      break;
    case kOpenCode:
      gamefont::sound(1);
      g_own = Own::Code;
      g_code.clear();
      std::memset(g_keys_before, 0x80, sizeof(g_keys_before));  // (the Enter that opened it is not typed)
      break;
    case kStart:
      gamefont::sound(1);
      netplay::lobby_start();
      break;
    case kWaitReady:
      gamefont::sound(2);
      break;
    case kInviteList:
      gamefont::sound(1);
      g_own = Own::Invite;
      g_line = 0;
      break;
    case kCopyCode:
      gamefont::sound(1);
      to_clipboard(v.code);
      overlay::notify(L"Код скопирован: отправьте его другу.", 4.0, 0xFF8CFF8C);
      break;
    case kInvite:
      gamefont::sound(1);
      netplay::lobby_invite(row.arg);
      g_called.insert(row.arg);
      overlay::notify(L"Приглашение отправлено: " + row.label, 4.0, 0xFF8CFF8C);
      break;
    case kPick:
      gamefont::sound(1);
      netplay::lobby_pick(row.arg);
      break;
    case kCloseGame:
    case kLeave:
      gamefont::sound(2);
      leave_lobby();
      break;
    default:
      break;
  }
}

// --- drawing --------------------------------------------------------------------------------------

std::wstring dots() { return std::wstring(1 + (GetTickCount() / 400) % 3, L'.'); }

// The four places of a game: who is in, who is ready.
float slots(Ui& ui, const pageui::Window& w, const netplay::LobbyView& v, float y) {
  for (int p = 0; p < 4; ++p) {
    const netplay::LobbyPlayer& pl = v.players[p];
    ui.rect(w.x0 + 22, y - 6, w.x1 - 22, y + 18, pl.present ? 0x60204818 : 0x40101810);
    ui.rect(w.x0 + 22, y - 6, w.x0 + 50, y + 18, pl.present ? (pl.me ? 0xFF4E9A1E : 0xFF2E5A18) : 0xFF1C2A18);
    ui.text(std::to_wstring(p + 1), w.x0 + 36, y, 17, pl.present ? pageui::kText : pageui::kOff, Align::Center);
    if (!pl.present) {
      ui.text(T(u8"СВОБОДНО", "FREE"), w.x0 + 62, y + 1, 15, pageui::kOff);
    } else {
      std::wstring n = player_name(pl.name, p);
      if (pl.me) n += L"  " + T(u8"(ВЫ)", "(YOU)");
      ui.text(n, w.x0 + 62, y, 17, pl.me ? pageui::kGood : pageui::kText);
      std::wstring st;
      uint32_t c = pageui::kGood;
      if (p == 0) {
        st = T(u8"ХОСТ", "HOST");
        c = pageui::kItem;
      } else if (pl.ready) {
        st = T(u8"ГОТОВ", "READY");
        if (pl.ping) st += L"  " + std::to_wstring(pl.ping) + T(u8" МС", " ms");
      } else {
        st = T(u8"ПОЛУЧАЕТ ДАННЫЕ", "GETS THE DATA") + dots();
        c = pageui::kWarn;
      }
      ui.text(st, w.x1 - 32, y + 1, 14, c, Align::Right);
    }
    y += 28;
  }
  return y;
}

void draw_rows(Ui& ui, const pageui::Window& w, const std::vector<Row>& r, float y, float step) {
  for (size_t i = 0; i < r.size(); ++i) {
    bool sel = (int)i == g_line;
    bool card = !r[i].about.empty();
    float h = 19.0f;
    if (sel) pageui::chosen(ui, w, y, card ? pageui::bottom_of(y + 22, 12.5f) : pageui::bottom_of(y, h));
    ui.text(r[i].label, w.x0 + 34, y, h, sel ? pageui::pulse() : pageui::kItem);
    if (!r[i].status.empty()) ui.text(r[i].status, w.x1 - 32, y + 2, 14, r[i].status_rgb, Align::Right);
    if (card) ui.text(r[i].about, w.x0 + 34, y + 22, 12.5f, sel ? pageui::kText : pageui::kDim);
    y += card ? step + 24 : step;
  }
}

}  // namespace

void open() {
  g_open = true;
  g_auto_opened = false;
  g_line = 0;
  g_own = Own::None;
  g_called.clear();
  g_last_error.clear();
  LOGI("netpage: СЕТЕВАЯ ИГРА opened");
}

void close() {
  g_open = false;
  g_line = 0;
  g_own = Own::None;
  g_auto_opened = true;  // (not again for this lobby)
}

bool active() {
  if (!netplay::lobby_running()) g_auto_opened = false;
  if (!g_open && !g_auto_opened && netplay::lobby_running()) {  // a Steam invite joined a friend's game here
    g_open = true;
    g_line = 0;
    g_own = Own::None;
  }
  return g_open;
}

void update() {
  netplay::LobbyView v = netplay::lobby_view();
  View w = view_of(v);
  if (w == View::Code) {
    int done = typing();
    if (done < 0) {
      gamefont::sound(2);
      g_own = Own::None;
    } else if (done > 0 && !g_code.empty()) {
      gamefont::sound(1);
      g_own = Own::None;
      g_line = 0;
      netplay::lobby_open_client(g_code);
    }
    return;
  }
  if (w == View::Failed && v.error != g_last_error) {
    g_last_error = v.error;
    LOGW("netpage: %s", v.error.c_str());
  }
  std::vector<Row> r = rows(w, v);
  int n = (int)r.size();
  if (n == 0) return;
  if (g_line >= n) g_line = n - 1;
  if (gamefont::pressed(1)) {  // up
    g_line = (g_line + n - 1) % n;
    gamefont::sound(0);
  } else if (gamefont::pressed(2)) {  // down
    g_line = (g_line + 1) % n;
    gamefont::sound(0);
  } else if (gamefont::pressed(gamefont::cancel_mask())) {
    Row back;
    back.id = w == View::Main || w == View::Invite ? kBack : kLeave;
    act(back, v);
  } else if (gamefont::pressed(gamefont::ok_mask())) {
    act(r[g_line], v);
  }
}

void draw() {
  netplay::LobbyView v = netplay::lobby_view();
  View view = view_of(v);
  Ui ui;
  std::wstring hints = view == View::Code ? T(u8"ВВОД→ВОЙТИ      esc→НАЗАД      ctrl+v→ВСТАВИТЬ", "enter→JOIN      esc→BACK      ctrl+v→PASTE")
                                          : T(u8"ВВОД→ВЫБОР      esc→НАЗАД", "enter→OK      esc→BACK");
  pageui::Window w = pageui::window(ui, T(u8"СЕТЕВАЯ ИГРА", "NETWORK GAME"), hints);
  std::vector<Row> r = rows(view, v);
  if (g_line >= (int)r.size()) g_line = std::max(0, (int)r.size() - 1);
  float y = w.top + 12;
  auto line = [&](const std::wstring& s, uint32_t rgb, float h = 15.0f, float gap = 8.0f) {
    ui.text(s, 0, y, h, rgb, Align::Center);
    y += h + gap;
  };
  const float text_w = (w.x1 - w.x0) - 60;
  switch (view) {
    case View::Main: {
      netplay::SteamInfo st = netplay::steam_info();
      if (st.state == 1)
        line(L"steam: " + gamefont::name(wide(st.name)) + T(u8" - НА СВЯЗИ", " - ON LINE"), pageui::kGood, 14);
      else if (st.state == 2)
        line(T(u8"steam НЕДОСТУПЕН: ", "NO steam: ") + gamefont::say(wide(st.reason)), pageui::kWarn, 13);
      else
        line(T(u8"ИГРАЙТЕ ВМЕСТЕ: ЧЕРЕЗ steam ИЛИ ПО СЕТИ", "PLAY TOGETHER: OVER steam OR A NETWORK"), pageui::kDim, 14);
      pageui::rule(ui, w, y);
      draw_rows(ui, w, r, y + 16, 30);
      // How playing together goes, in three steps.
      float sy = w.bottom - 84;
      pageui::rule(ui, w, sy);
      ui.text(T(u8"КАК ИГРАТЬ ВМЕСТЕ", "HOW TO PLAY TOGETHER"), w.x0 + 34, sy + 10, 13, pageui::kLime);
      const std::wstring steps[3] = {T(u8"ОДИН ИГРОК ОТКРЫВАЕТ СВОЮ ИГРУ", "ONE PLAYER HOSTS A GAME"),
                                     T(u8"ДРУЗЬЯ ВХОДЯТ К НЕМУ", "THE FRIENDS JOIN IT"),
                                     T(u8"ХОСТ НАЖИМАЕТ «СТАРТ ИГРЫ» - И ВСЕ В ИГРЕ", "THE HOST STARTS - AND EVERYBODY IS IN")};
      for (int i = 0; i < 3; ++i) {
        ui.text(std::to_wstring(i + 1), w.x0 + 40, sy + 29 + 16.0f * i, 12.5f, pageui::kItem, Align::Center);
        ui.text(steps[i], w.x0 + 54, sy + 29 + 16.0f * i, 12.5f, pageui::kDim);
      }
      break;
    }
    case View::Host: {
      line(T(u8"ВАША ИГРА ОТКРЫТА: ЖДЁМ ИГРОКОВ", "YOUR GAME IS OPEN: WAITING FOR PLAYERS"), pageui::kItem, 15, 14);
      y = slots(ui, w, v, y) + 2;
      pageui::rule(ui, w, y);
      y += 9;
      if (v.can_invite)
        y = ui.wrapped(T(u8"ДРУГ ВХОДИТ САМ: СЕТЕВАЯ ИГРА → ВОЙТИ К ДРУГУ → ВАШЕ ИМЯ. ИЛИ ПОЗОВИТЕ ЕГО ОТСЮДА. НАЧАТЬ МОЖНО И ОДНОМУ: ДРУЗЬЯ ВОЙДУТ ПО ХОДУ ИГРЫ.",
                         "A FRIEND JOINS: NETWORK GAME → JOIN A FRIEND → YOUR NAME. OR INVITE THEM FROM HERE."),
                       0, y, 12.5f, text_w, 16, pageui::kDim, Align::Center, 3);
      else if (!v.code.empty())
        y = ui.wrapped(T(u8"steam НЕ НА СВЯЗИ. ДРУГ ВХОДИТ ПО КОДУ: СЕТЕВАЯ ИГРА → ВОЙТИ ПО КОДУ.",
                         "NO steam. A FRIEND JOINS BY THE CODE: NETWORK GAME → JOIN BY CODE."),
                       0, y, 12.5f, text_w, 16, pageui::kWarn, Align::Center, 2);
      draw_rows(ui, w, r, std::max(y + 10, w.bottom - 16 - 27.0f * (float)r.size()), 27);
      break;
    }
    case View::Invite: {
      line(T(u8"КОГО ПОЗВАТЬ? ДРУЗЬЯ В СЕТИ steam", "WHOM TO INVITE? steam FRIENDS ON LINE"), pageui::kItem, 15, 10);
      if (r.size() <= 1) line(T(u8"СЕЙЧАС НИКОГО НЕТ В СЕТИ", "NOBODY IS ON LINE NOW"), pageui::kDim, 14);
      draw_rows(ui, w, r, y + 4, 26);
      ui.wrapped(T(u8"ДРУГ ПРИНИМАЕТ ПРИГЛАШЕНИЕ В steam, И ЕГО ИГРА САМА ВХОДИТ К ВАМ. ИГРА У НЕГО ДОЛЖНА БЫТЬ ЗАПУЩЕНА.",
                   "THE FRIEND ACCEPTS THE INVITE IN steam AND THEIR GAME JOINS BY ITSELF. THEIR GAME MUST BE RUNNING."),
                 0, w.bottom - 42, 12.5f, text_w, 16, pageui::kDim, Align::Center, 2);
      break;
    }
    case View::Browse: {
      line(T(u8"ОТКРЫТЫЕ ИГРЫ ДРУЗЕЙ", "FRIENDS' OPEN GAMES"), pageui::kItem, 15, 10);
      if (r.size() <= 1) {
        line(T(u8"ПОКА НИКТО ИЗ ДРУЗЕЙ НЕ ОТКРЫЛ ИГРУ", "NO FRIEND HAS OPENED A GAME YET"), pageui::kText, 14, 6);
        line(T(u8"ИЩЕМ", "LOOKING") + dots(), pageui::kDim, 14);
      }
      draw_rows(ui, w, r, y + 4, 27);
      float yy = y + 4 + 27.0f * (float)r.size() + 6;
      for (const netplay::LobbyGame& g : v.games) {  // the ones that cannot be joined, and why
        if (g.compatible) continue;
        std::wstring n = gamefont::name(wide(g.host));
        std::wstring why = gamefont::say(wide(g.why_not));
        ui.text((n.empty() ? T(u8"ИГРА", "A GAME") : n) + L": " + (why.empty() ? T(u8"ДРУГАЯ ВЕРСИЯ", "ANOTHER VERSION") : why),
                0, yy, 12.5f, pageui::kWarn, Align::Center);
        yy += 17;
      }
      ui.wrapped(T(u8"НЕТ В СПИСКЕ? ПУСТЬ ДРУГ ВЫБЕРЕТ У СЕБЯ: СЕТЕВАЯ ИГРА → ОТКРЫТЬ СВОЮ ИГРУ. НУЖНО БЫТЬ ДРУЗЬЯМИ В steam.",
                   "NOT LISTED? YOUR FRIEND PICKS: NETWORK GAME → HOST A GAME. YOU MUST BE steam FRIENDS."),
                 0, w.bottom - 58, 12.5f, text_w, 16, pageui::kDim, Align::Center, 3);
      break;
    }
    case View::Code: {
      y += 34;
      line(T(u8"КОД ХОСТА ИЛИ ЕГО ip-АДРЕС", "THE HOST'S CODE OR ip ADDRESS"), pageui::kItem, 16, 18);
      ui.rect(-190, y - 10, 190, y + 28, 0xC0000000);
      ui.rect(-190, y + 28, 190, y + 30, 0xFF9EDC3C);
      std::wstring typed = gamefont::name(wide(g_code));
      float tw = pageui::width(typed, 24);
      ui.text(typed, -tw / 2, y, 24, pageui::kText);
      if ((GetTickCount() / 350) % 2) ui.rect(tw / 2 + 4, y - 1, tw / 2 + 15, y + 19, 0xFF9EDC3C);
      y += 56;
      ui.wrapped(T(u8"КОД ВИДЕН У ХОСТА НА ЕГО СТРАНИЦЕ «СЕТЕВАЯ ИГРА», НАПРИМЕР tmnt-30ag-00c. ПО ИНТЕРНЕТУ ХОСТУ НУЖЕН ОТКРЫТЫЙ ПОРТ 7790 udp.",
                   "THE HOST SEES THE CODE ON THEIR NETWORK GAME PAGE, LIKE tmnt-30ag-00c. OVER THE INTERNET THE HOST NEEDS PORT 7790 udp OPEN."),
                 0, y, 12.5f, text_w, 16, pageui::kDim, Align::Center, 3);
      break;
    }
    case View::Connecting: {
      y += 50;
      line(T(u8"СОЕДИНЕНИЕ", "CONNECTING") + dots(), pageui::kItem, 18, 14);
      ui.wrapped(gamefont::say(wide(v.note)), 0, y, 13, text_w, 17, pageui::kDim, Align::Center, 3);
      draw_rows(ui, w, r, w.bottom - 44, 27);
      break;
    }
    case View::Joined: {
      line(T(u8"ВЫ В ИГРЕ: ", "YOU ARE IN THE GAME OF ") + player_name(v.host_name, 0), pageui::kItem, 15, 14);
      y = slots(ui, w, v, y) + 2;
      pageui::rule(ui, w, y);
      y += 14;
      ui.text(T(u8"ЖДЁМ, КОГДА ХОСТ НАЖМЁТ «СТАРТ ИГРЫ»", "WAITING FOR THE HOST TO START") + dots(), 0, y, 15,
              pageui::kText, Align::Center, (uint8_t)(170 + 85 * pageui::breath()));
      y += 24;
      ui.wrapped(T(u8"ИГРА НАЧНЁТСЯ ПРЯМО ЗДЕСЬ, В ГЛАВНОМ МЕНЮ, У ВСЕХ СРАЗУ.", "THE GAME BEGINS RIGHT HERE, IN THE MAIN MENU, FOR EVERYBODY AT ONCE."),
                 0, y, 12.5f, text_w, 16, pageui::kDim, Align::Center, 2);
      draw_rows(ui, w, r, w.bottom - 44, 27);
      break;
    }
    case View::Failed: {
      y += 40;
      line(T(u8"НЕ ПОЛУЧИЛОСЬ", "IT DID NOT WORK"), pageui::kBad, 20, 16);
      ui.wrapped(gamefont::say(wide(v.error)), 0, y, 14, text_w, 19, pageui::kText, Align::Center, 4);
      draw_rows(ui, w, r, w.bottom - 44, 27);
      break;
    }
  }
  ui.draw();
}

}  // namespace netpage
