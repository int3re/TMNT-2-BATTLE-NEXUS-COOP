// The launcher's picture: every screen of the design, drawn from the model. Sizes and colours are the
// design's (a 960 x 600 window: header 48, body 508, footer 44).
#include <cmath>

#include "launcher/launcher.h"

namespace lz {
namespace view {
namespace {

using gfx::Canvas;
using gfx::Font;
using gfx::Matrix;
using gfx::TextOpt;

const uint32_t BAR = 0x07060F, PANEL = 0x161230, LINE = 0x30275A, EDGE = 0x6F60B0, PAPER = 0xF2EFE2, TEXT2 = 0xD4CFEA,
               MUTED = 0xA9A2CC, GREEN = 0x8BE836, INK = 0x07100A, PINK = 0x0E0B1C, VIOLET = 0x5B3FD0, DEEP = 0x2B1F6B,
               WAIT = 0xF2C94C, RED = 0xEF5A5A, RED_TEXT = 0xFF8A8A, RED_EDGE = 0xC24A4A, BLACK = 0x000000;
const uint32_t kBandana[4] = {0x4C9AF0, 0xEF5A5A, 0xA77BF5, 0xF7941D};

Font D(float size, float spacing = 0) { return {gfx::Display, size, 400, spacing}; }
Font T(float size, int weight = 400, float spacing = 0) { return {gfx::Text, size, weight, spacing}; }
Font Mo(float size, int weight = 500) { return {gfx::Mono, size, weight, 0}; }

// icons, in a 24 x 24 box
const char kSliders[] = "M4 6h8M18 6h2M4 12h2M12 12h8M4 18h10";
const char kMin[] = "M6 17h12";
const char kClose[] = "M6 6l12 12M18 6L6 18";
const char kArrow[] = "M3 12h17M14 6l6 6-6 6";
const char kEqual[] = "M5 9h14M5 15h14";
const char kCheck[] = "M5 12.5l4.5 4.5L19 7.5";
const char kWarn[] = "M12 4l9 16H3ZM12 10v5M12 17.6v0.4";
const char kSpin[] = "M12 4a8 8 0 1 0 8 8";
const char kDownload[] = "M12 4v11M7 11l5 5 5-5M5 20h14";
const char kPlay[] = "M8 5l11 7-11 7Z";
const char kFolder[] = "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z";
const char kExternal[] = "M13 5h6v6M19 5l-9 9M10 7H5v12h12v-5";
const char kCopy[] = "M9 9h11v11H9ZM5 15H4V4h11v1";
const char kBack[] = "M20 12H5M11 6l-6 6 6 6";
const char kKatana[] = "M4 20l3-3M7 17L20 4M6 14l4 4";
const char kSai[] = "M12 3v18M7 8c0 4 2 6 5 6c3 0 5-2 5-6M9 18h6";
const char kBo[] = "M4 20L20 4M8 13l3 3M13 8l3 3";
const char kNunchaku[] = "M5 20l3-9M19 20l-3-9M8 11c0-6 8-6 8 0";
// the shell mark, in a 28 x 28 box
const char kShellOuter[] = "M14 2l10.4 6v12L14 26 3.6 20V8Z";
const char kShellInner[] = "M14 8.5l4.8 2.75v5.5L14 19.5l-4.8-2.75v-5.5Z";
const char kShellLines[] = "M14 8.5V2M18.8 11.25L24.4 8M18.8 16.75L24.4 20M14 19.5V26M9.2 16.75L3.6 20M9.2 11.25L3.6 8";
// stars: 20 x 20 and 12 x 12
const char kShuriken[] = "M10 1l2.4 6.6L19 10l-6.6 2.4L10 19l-2.4-6.6L1 10l6.6-2.4Z";
const char kStar[] = "M6 0.6l1.5 3.9L11.4 6 7.5 7.5 6 11.4 4.5 7.5 0.6 6l3.9-1.5Z";
// the arena under the window's bottom edge, in a 960 x 110 box
const char kPavilionRoof[] = "M18 34Q26 41 34 38Q40 35 43 28H81Q84 35 90 38Q98 41 106 34V37Q98 44 90 42H34Q26 44 18 37Z";
const char kTorii[] = "M164 30Q200 24 236 30L234 37H166Z";
const char kPagoda1[] = "M494 36Q502 43 510 40Q516 37 519 30H561Q564 37 570 40Q578 43 586 36V39Q578 46 570 44H510Q502 46 494 39Z";
const char kPagoda2[] = "M505 18Q512 25 520 22Q525 19 528 13H552Q555 19 560 22Q568 25 575 18V21Q568 28 560 26H520Q512 28 505 21Z";
const char kPagoda3[] = "M518 7Q526 12 532 8Q536 5 540 0Q544 5 548 8Q554 12 562 7V10Q554 15 548 12H532Q526 15 518 10Z";
const char kHallRoof[] = "M632 36Q640 43 648 40Q654 37 657 30H893Q896 37 902 40Q910 43 918 36V39Q910 46 902 44H648Q640 46 632 39Z";

struct Ctx {
  Canvas& c;
  const Model& m;
  std::vector<Hit>* hits;
  uint32_t accent;
};

void hit(Ctx& x, int id, float l, float t, float w, float h) {
  if (!x.hits) return;
  Matrix inv = x.c.current();
  if (!inv.Invert()) return;
  x.hits->push_back({id, D2D1::RectF(l, t, l + w, t + h), inv});
}

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

std::wstring megabytes(uint64_t bytes) {
  wchar_t buf[32];
  swprintf(buf, 32, L"%.1f", bytes / 1000000.0);
  for (wchar_t* p = buf; *p; ++p)
    if (*p == L'.') *p = L',';
  return buf;
}

// --- what a screen says --------------------------------------------------------------------------

struct Vals {
  bool offer = false, arrow = false, equal = false, has_new = false;
  std::wstring installed, installed_note, latest, latest_note, latest_label;
  uint32_t latest_border = LINE;
  int compat = 0;  // 1 warning (accent), 2 fine (green), 3 waiting, 4 bad (red), 5 plain
  std::wstring compat_text;
  bool notice = false, notice_bad = false;
  std::wstring notice_title, notice_text;
  std::wstring primary, secondary, hint;
  int icon = 0;  // 1 download, 2 play, 3 folder
  bool primary_on = true;
  std::wstring tab, meta, title, link, link_url;
  std::vector<std::wstring> items;
  bool copy = false;
  std::wstring net;
  int net_state = 0;
  std::wstring path;
};

std::wstring proto_note(int protocol) { return protocol ? L"протокол " + std::to_wstring(protocol) : L""; }

void news(const Release& r, Vals& v, const wchar_t* tab) {
  v.tab = tab;
  v.meta = r.version + (r.date.empty() ? L"" : L" · " + r.date);
  v.title = r.title;
  v.items = r.notes;
  if (v.items.empty()) {  // (a release without its description: an old one)
    v.title = L"Версия " + r.version;
    v.items = {L"Что в ней изменилось — на странице релиза на GitHub."};
  }
  v.link = L"Все изменения на GitHub";
  v.link_url = std::wstring(kRepoUrl) + L"/releases";
}

void how_it_works(Vals& v) {
  v.tab = L"КАК ЭТО РАБОТАЕТ";
  v.meta = L"tmnt2net";
  v.title = L"Лаунчер мода";
  v.items = {L"Мод лежит внутри лаунчера: ставится в папку игры одной кнопкой.",
             L"При запуске лаунчер спрашивает у GitHub, не вышла ли версия новее.",
             L"Игра по сети — пункт СЕТЕВАЯ ИГРА в главном меню игры."};
  v.link = L"Страница мода на GitHub";
  v.link_url = kRepoUrl;
}

// The notes of the version that is installed: GitHub's or this launcher's if they are of that version,
// else the ones kept from the day it was installed.
void installed_news(const Model& m, Vals& v) {
  if (m.inst.present && m.rel.known && m.rel.version == m.inst.version && !m.rel.notes.empty()) {
    news(m.rel, v, L"В ЭТОЙ ВЕРСИИ");
  } else if (m.inst.present && m.pack.known && m.pack.version == m.inst.version && !m.pack.notes.empty()) {
    news(m.pack, v, L"В ЭТОЙ ВЕРСИИ");
  } else if (m.inst.present && !m.set.notes.empty() && m.set.notes_version == m.inst.version) {
    v.tab = L"В ЭТОЙ ВЕРСИИ";
    v.meta = m.inst.version + L" · установлена";
    v.title = m.set.notes_title;
    v.items = m.set.notes;
    v.link = L"Все изменения на GitHub";
    v.link_url = std::wstring(kRepoUrl) + L"/releases";
  } else {
    how_it_works(v);
  }
}

// What there is to install: GitHub's version when it is newer than the one inside this launcher and its
// file can be had (it is downloaded then), else the one inside.
bool from_github(const Model& m) {
  return m.rel.known && !m.assets_blocked && (!m.pack.known || core::compare_versions(m.rel.version, m.pack.version) > 0);
}
const Release& offer(const Model& m) { return from_github(m) ? m.rel : m.pack; }

Vals vals(const Model& m) {
  Vals v;
  Screen s = m.screen == Screen::Settings ? m.back : m.screen;
  const uint32_t accent = kBandana[m.set.accent & 3];
  const Release& t = offer(m);
  const bool download = from_github(m);
  std::wstring have = m.inst.version.empty() ? L"" : m.inst.version;
  std::wstring play_on = have.empty() ? L"ИГРАТЬ" : L"ИГРАТЬ НА " + have;
  v.installed = !m.inst.present ? L"нет" : have.empty() ? L"?" : have;
  v.installed_note = !m.inst.present ? L"мод не установлен" : have.empty() ? L"версия неизвестна" : proto_note(m.inst.protocol);
  v.latest_label = L"НА GITHUB";
  v.latest = m.rel.known ? m.rel.version : L"—";
  v.latest_note = m.rel.known ? proto_note(m.rel.protocol) : L"нет ответа";
  v.path = m.has_game ? m.set.game_dir : L"(папка не выбрана)";
  std::wstring how = download ? (t.pkg_size ? megabytes(t.pkg_size) + L" МБ с GitHub" : std::wstring(L"Файл с GitHub")) +
                                    (t.pkg_sha256.empty() ? L" · в релизе нет контрольной суммы" : L" · после загрузки сверяется SHA-256")
                              : L"Версия " + t.version + L" уже внутри лаунчера: скачивать ничего не нужно.";
  std::wstring checked = m.checked_at.empty() ? L"GitHub: проверено только что"
                                             : L"GitHub: проверено в " + m.checked_at + (m.via.empty() ? L"" : L", " + m.via);
  // The footer's line where an answer of GitHub is not what the screen is about.
  auto github = [&]() {
    if (m.rechecking) {
      v.net = L"GitHub: проверяю";
      v.net_state = 1;
    } else if (m.rel.known && !m.assets_blocked) {
      v.net = checked;
    } else {
      v.net = m.assets_blocked ? L"GitHub: файлы не скачиваются" : L"GitHub: нет ответа";
      v.net_state = 2;
    }
  };
  // The right box, when the version to install is the one inside this launcher.
  auto inside = [&]() {
    if (download || (m.rel.known && m.rel.version == t.version)) return;
    v.latest_label = L"В ЭТОМ ЛАУНЧЕРЕ";
    v.latest = t.version;
    v.latest_note = proto_note(t.protocol);
  };
  switch (s) {
    case Screen::Checking:
      v.offer = v.arrow = true;
      v.latest = L"· · ·";
      v.latest_note = L"жду ответа";
      v.latest_border = EDGE;
      v.compat = 3;
      v.compat_text = L"Спрашиваю у GitHub, какая версия последняя.";
      v.primary = play_on;
      v.icon = 2;
      v.primary_on = m.inst.present;
      v.hint = L"Играть можно сразу: проверка идёт в фоне и запуск не задерживает.";
      installed_news(m, v);
      v.net = L"GitHub: проверяю";
      v.net_state = 1;
      break;
    case Screen::Update:
      v.offer = v.arrow = v.has_new = true;
      v.latest_border = accent;
      inside();
      if (m.inst.protocol && t.protocol && m.inst.protocol != t.protocol) {
        v.compat = 1;
        v.compat_text = L"Протокол новый: с " + have + L" по сети не соединяется. Обновитесь всей командой.";
      } else if (m.inst.protocol && m.inst.protocol == t.protocol) {
        v.compat = 2;
        v.compat_text = L"Протокол тот же: с друзьями на " + have + L" играть можно.";
      } else {
        v.compat = 5;
        v.compat_text = L"Есть версия мода новее установленной.";
      }
      v.primary = L"ОБНОВИТЬ ДО " + t.version;
      v.icon = 1;
      v.secondary = play_on;
      v.hint = how;
      news(t, v, L"ЧТО НОВОГО");
      v.copy = true;
      github();
      break;
    case Screen::Install:
      v.offer = v.arrow = v.has_new = true;
      v.latest_border = accent;
      inside();
      v.compat = 2;
      v.compat_text = L"Файлы игры не меняются: мод ставится рядом и убирается одной кнопкой.";
      v.primary = L"УСТАНОВИТЬ МОД " + t.version;
      v.icon = 1;
      v.hint = how;
      news(t, v, L"ЧТО НОВОГО");
      github();
      break;
    case Screen::Downloading:
      if (m.pending == Pending::Uninstall) installed_news(m, v);
      else news(m.pending == Pending::Launcher ? m.rel : t, v, L"ЧТО НОВОГО");
      v.copy = m.rel.known;
      v.net = !m.total ? L"Мод: из этого лаунчера" : m.stage == 0 ? L"GitHub: идёт загрузка" : L"GitHub: файл получен";
      if (m.pending == Pending::Uninstall) v.net = L"Мод убирается из папки игры";
      break;
    case Screen::Ready:
      v.offer = true;
      if (m.unchecked) {
        v.arrow = true;
        v.latest_note = L"не проверялось";
        v.latest_border = EDGE;
        v.compat = 5;
        v.compat_text = L"Проверка при запуске выключена в настройках.";
        v.net = L"GitHub: не проверялось";
        v.net_state = 1;
      } else {
        v.equal = true;
        v.compat = 2;
        v.compat_text = L"У вас последняя версия. Новее на GitHub нет.";
        v.net = checked;
      }
      v.primary = m.launching ? L"ИГРА ЗАПУСКАЕТСЯ" : L"ИГРАТЬ";
      v.icon = 2;
      v.secondary = m.rechecking ? L"ПРОВЕРЯЮ GITHUB" : L"ПРОВЕРИТЬ ОБНОВЛЕНИЯ";
      v.hint = L"Игра по сети — пункт СЕТЕВАЯ ИГРА в главном меню игры.";
      installed_news(m, v);
      v.copy = m.rel.known;
      if (m.rechecking) {
        v.net = L"GitHub: проверяю";
        v.net_state = 1;
      }
      break;
    case Screen::Offline:
      v.offer = v.arrow = true;
      v.latest_border = EDGE;
      v.compat = 4;
      if (m.assets_blocked) {
        v.latest_note = L"файл недоступен";
        v.compat_text = L"GitHub отвечает, но файлы релиза не отдаёт. Включите прокси в настройках.";
      } else {
        v.compat_text = L"GitHub не отвечает. Какая версия последняя — неизвестно.";
      }
      v.primary = play_on;
      v.icon = 2;
      v.secondary = L"ПОВТОРИТЬ ПРОВЕРКУ";
      v.hint = L"Проверка обновлений игре не нужна: мод работает как раньше.";
      v.tab = L"НЕТ СВЯЗИ";
      v.meta = m.assets_blocked ? L"файлы GitHub не скачиваются" : L"github.com не отвечает";
      v.title = L"Что можно сделать";
      v.items = {L"Повторить позже: лаунчер проверяет обновления при каждом запуске.",
                 L"Включить прокси в настройках — автоматически или свой.",
                 L"Скачать новый лаунчер в браузере: мод уже внутри него."};
      v.link = L"Страница релиза в браузере";
      v.link_url = std::wstring(kRepoUrl) + L"/releases/latest";
      v.net = m.assets_blocked ? L"GitHub: файлы не скачиваются" : L"GitHub: нет ответа";
      v.net_state = 2;
      break;
    case Screen::NoGame:
      v.notice = true;
      v.notice_title = L"Игра не найдена";
      v.notice_text = L"Лаунчер не нашёл игру на этом компьютере. Укажите папку, где лежит TMNT2.exe.";
      v.primary = L"ВЫБРАТЬ ПАПКУ ИГРЫ";
      v.icon = 3;
      v.hint = L"Нужна своя копия игры: файлы игры в мод не входят.";
      v.tab = L"ТРИ ШАГА";
      v.meta = L"первый запуск";
      v.title = L"Как это работает";
      v.items = {L"Укажите папку игры.", L"Лаунчер поставит мод: он уже внутри. Если на GitHub есть новее — скачает.",
                 L"Запустите игру: в главном меню появится СЕТЕВАЯ ИГРА."};
      v.link = L"Инструкция на GitHub";
      v.link_url = kRepoUrl;
      github();
      if (m.rel.known && !m.rechecking) v.net = L"GitHub: последняя версия " + m.rel.version;
      break;
    case Screen::Running:
      v.notice = true;
      v.notice_title = L"Игра сейчас открыта";
      if (m.pending == Pending::Uninstall) {
        v.notice_text = L"Пока игра открыта, мод из её папки не убрать. Закройте игру — удаление начнётся само.";
        v.primary = L"ЗАКРЫТЬ ИГРУ И УДАЛИТЬ МОД";
        v.net = L"Мод ждёт, пока игра закроется";
        v.net_state = 1;
      } else {
        v.notice_text = L"Версия " + t.version + (m.total ? L" скачана и проверена" : L" готова к установке") +
                        L". Закройте игру — установка начнётся сама.";
        v.primary = L"ЗАКРЫТЬ ИГРУ И ОБНОВИТЬ";
        v.icon = 1;
        v.net = m.total ? L"GitHub: " + t.version + L" скачана и проверена" : L"Мод " + t.version + L" ждёт, пока игра закроется";
      }
      v.secondary = L"ПОЗЖЕ";
      v.hint = L"В сетевой игре остальные останутся без вас: сначала предупредите команду.";
      v.tab = L"ПОЧЕМУ ТАК";
      v.meta = L"TMNT2.exe открыт";
      v.title = L"Мод занят игрой";
      v.items = {L"Пока игра открыта, файлы мода в её папке нельзя заменить.",
                 L"Скачивать заново не придётся: осталась только установка.", L"Настройки, клавиши и сохранения останутся."};
      v.link = L"Все изменения на GitHub";
      v.link_url = std::wstring(kRepoUrl) + L"/releases";
      break;
    case Screen::Problem:
    default:
      v.notice = v.notice_bad = true;
      v.notice_title = m.problem_title;
      v.notice_text = m.problem_text;
      v.primary = m.problem_primary;
      v.secondary = m.problem_secondary;
      v.hint = L"Игра и её файлы не тронуты: мод либо стоит прежний, либо не стоит вовсе.";
      if (t.known) news(t, v, L"ЧТО НОВОГО");
      else how_it_works(v);
      v.net = L"Не получилось";
      v.net_state = 2;
      break;
  }
  if (!m.net.empty()) {
    v.net = m.net;
    v.net_state = m.net_state;
  }
  if (m.has_game && !m.exe_ok && (s == Screen::Update || s == Screen::Install || s == Screen::Ready || s == Screen::Checking))
    v.hint = L"TMNT2.exe в этой папке другого издания: мод работает только с exe американского издания 2004 года.";
  return v;
}

// --- parts ---------------------------------------------------------------------------------------

void stripes(Canvas& c, float x, float cy, float w, float h, float gap) {
  for (int i = 0; i < 4; ++i) {
    float sx = x + i * (w + gap);
    c.push(Matrix::Skew(-24, 0, D2D1::Point2F(sx + w / 2, cy)));
    c.fill(sx, cy - h / 2, w, h, kBandana[i]);
    c.pop();
  }
}

void background(Canvas& c) {
  const uint32_t rgb[3] = {0x0C0A18, 0x100D22, 0x1A1438};
  const float pos[3] = {0, 0.55f, 1};
  c.gradient_v(0, 0, kW, kH, rgb, pos, 3);
  // the gate of the Nexus behind the banner: a disc with rings, dots thickening to its lower right
  const float cx = 830, cy = 280, r = 240;
  c.ellipse(cx, cy, r, r, 0x150F30);
  for (int j = 0; j < 48; ++j)
    for (int i = 0; i < 48; ++i) {
      float px = cx - r + 5 + 10 * i, py = cy - r + 5 + 10 * j;
      float dx = px - cx, dy = py - cy;
      if (dx * dx + dy * dy > (r - 1) * (r - 1)) continue;
      float p = (dx * 0.70711f + dy * 0.70711f) / 678.8f + 0.5f;
      float a = clamp01((p - 0.25f) / 0.70f);
      if (a > 0.01f) c.ellipse(px, py, 1.9f, 1.9f, 0x3B2D80, a);
    }
  c.ring(cx, cy, 219, 2, 0x33276E);
  c.ring(cx, cy, 196.5f, 3, 0x4A3A9A, 9, 7);
  c.ellipse(cx, cy, 148, 148, 0x33276E);
  c.ellipse(cx, cy, 146, 146, 0x1B1440);
  // dots fading from the upper left corner
  for (int j = 0; j < 32; ++j)
    for (int i = 0; i < 40; ++i) {
      float px = 4.5f + 9 * i, py = 4.5f + 9 * j;
      float d = std::sqrt((px / 509.0f) * (px / 509.0f) + (py / 396.0f) * (py / 396.0f));
      float a = clamp01(1 - d / 0.7f);
      if (a > 0.01f) c.ellipse(px, 48 + py, 1.7f, 1.7f, 0x7C5CFF, 0.26f * a);
    }
  // the arena: a pavilion, a gate, the four teams' flags, the pagoda, the long hall
  c.push(Matrix::Translation(0, kH - 44 - 110));
  const uint32_t dark = 0x06050D;
  c.fill(0, 56, kW, 54, dark);
  c.fill(36, 40, 52, 16, dark);
  c.fill_path(kPavilionRoof, dark);
  c.fill_path(kTorii, dark);
  c.fill(178, 36, 7, 20, dark);
  c.fill(215, 36, 7, 20, dark);
  c.fill(172, 42, 56, 4, dark);
  for (int i = 0; i < 4; ++i) {
    c.fill(300.0f + 32 * i, 28, 2, 28, dark);
    c.fill(302.0f + 32 * i, 30, 10, 20, kBandana[i]);
  }
  c.fill(512, 42, 56, 14, dark);
  c.fill_path(kPagoda1, dark);
  c.fill(523, 24, 34, 8, dark);
  c.fill_path(kPagoda2, dark);
  c.fill(532, 10, 16, 4, dark);
  c.fill_path(kPagoda3, dark);
  c.fill(650, 42, 250, 14, dark);
  c.fill_path(kHallRoof, dark);
  const float green[][2] = {{56, 45}, {520, 46}, {554, 46}, {668, 47}, {720, 47}, {772, 47}, {824, 47}, {876, 47}};
  const float yellow[][2] = {{70, 45}, {537, 46}, {694, 47}, {746, 47}, {798, 47}, {850, 47}};
  for (auto& g : green) c.fill(g[0], g[1], 5, 6, GREEN, 0.6f);
  for (auto& y : yellow) c.fill(y[0], y[1], 5, 6, 0xF2E08A, 0.55f);
  c.pop();
}

void header_button(Ctx& x, int id, float bx, const char* icon, uint32_t rgb, bool danger = false) {
  Canvas& c = x.c;
  bool hover = x.m.hover == id, down = x.m.pressed == id;
  if (hover || down) c.fill(bx, 1, 44, 44, danger ? RED_EDGE : PAPER, danger ? (down ? 1.0f : 0.85f) : (down ? 0.16f : 0.09f));
  c.icon(icon, bx + 12, 13, 20, danger && hover ? PAPER : rgb, 2.2f);
  if (id == H_Settings) {
    const float dots[3][2] = {{15, 6}, {9, 12}, {17, 18}};
    c.push(Matrix::Scale(20.0f / 24, 20.0f / 24) * Matrix::Translation(bx + 12, 13));
    for (auto& d : dots) c.ring(d[0], d[1], 2.4f, 2.2f, rgb);
    c.pop();
  }
  hit(x, id, bx, 1, 44, 44);
}

void header(Ctx& x) {
  Canvas& c = x.c;
  c.fill(0, 0, kW, 48, BAR);
  c.fill(0, 46, kW, 2, BLACK);
  c.push(Matrix::Scale(26.0f / 28, 26.0f / 28) * Matrix::Translation(16, 10));
  c.fill_path(kShellOuter, GREEN);
  c.stroke_path(kShellOuter, 2, BLACK);
  c.stroke_path(kShellLines, 1.6f, BLACK, false);
  c.fill_path(kShellInner, VIOLET);
  c.stroke_path(kShellInner, 1.6f, BLACK);
  c.pop();
  float tx = 52;
  Font name = D(15, 0.04f);
  c.text_mid(L"TMNT2NET", name, tx, 23, PAPER);
  tx += c.measure(L"TMNT2NET", name).width + 10;
  c.text_mid(L"ЛАУНЧЕР", T(11, 600, 0.16f), tx, 23, MUTED);
  header_button(x, H_Settings, 824, kSliders, x.m.screen == Screen::Settings ? x.accent : TEXT2);
  header_button(x, H_Min, 868, kMin, TEXT2);
  header_button(x, H_Close, 912, kClose, TEXT2, true);
}

void footer(Ctx& x, const Vals& v) {
  Canvas& c = x.c;
  const Model& m = x.m;
  const float top = kH - 44.0f, cy = top + 2 + 21;
  c.fill(0, top, kW, 44, BAR);
  c.fill(0, top, kW, 2, BLACK);
  bool flash = !m.flash.empty();
  const uint32_t dot[3] = {GREEN, WAIT, RED};
  c.ellipse(20, cy, 4, 4, dot[v.net_state % 3]);
  Font f = T(12);
  const std::wstring& line = flash ? m.flash : v.net;
  c.text_mid(line, f, 32, cy, flash ? PAPER : TEXT2);
  float px = 32 + c.measure(line, f).width + 14;
  c.fill(px, cy - 7, 1, 14, LINE);
  px += 1 + 14;
  Font mono = Mo(12, 500);
  float rw = c.measure(kRepoText, mono).width;
  c.text_mid(kRepoText, mono, px, cy, m.hover == H_Repo ? PAPER : MUTED);
  hit(x, H_Repo, px, top + 6, rw, 34);
  // the right end: the four bandanas, before them a line of small print
  const float sx = kW - 16 - (4 * 16 + 3 * 3);
  stripes(c, sx, cy, 16, 6, 3);
  if (m.swapped) {
    const wchar_t* s = L"Новый лаунчер откроется при следующем запуске";
    c.text_mid(s, f, sx - 14 - c.measure(s, f).width, cy, MUTED);
  } else if (m.rel.full && !from_github(m) && !m.rel.launcher_version.empty() && m.screen != Screen::Downloading &&
             core::compare_versions(m.rel.launcher_version, kLauncherVersion) > 0) {
    // (a newer launcher with the same mod; with a newer mod it comes along with the update)
    std::wstring s = L"Вышел лаунчер " + m.rel.launcher_version + L" — обновить";
    TextOpt u;
    u.underline = true;
    float w = c.measure(s, f).width;
    c.text_mid(s, f, sx - 14 - w, cy, m.hover == H_NewLauncher ? PAPER : x.accent, u);
    hit(x, H_NewLauncher, sx - 14 - w, top + 6, w, 34);
  } else {
    const wchar_t* s = L"Неофициальный фанатский мод · MIT";
    c.text_mid(s, f, sx - 14 - c.measure(s, f).width, cy, MUTED);
  }
}

// The big green button. Returns its width.
float primary_button(Ctx& x, const Vals& v, float bx, float by) {
  Canvas& c = x.c;
  Font f = D(17, 0.02f);
  float tw = c.measure(v.primary, f).width;
  float w = 3 + 20 + (v.icon ? 22 + 10 : 0) + tw + 24 + 3, h = 62;
  bool on = v.primary_on, hover = on && x.m.hover == H_Primary, down = on && x.m.pressed == H_Primary;
  c.push(Matrix::Skew(-7, 0, D2D1::Point2F(bx + w / 2, by + h / 2)));
  float off = down ? 3.0f : 0.0f;
  if (on) c.fill(bx + 6, by + 6, w, h, VIOLET);
  c.box(bx + off, by + off, w, h, !on ? 0x2A2350 : hover ? 0xA2F552 : GREEN, 3, BLACK);
  uint32_t ink = on ? INK : MUTED;
  float ix = bx + off + 3 + 20, cy = by + off + h / 2;
  if (v.icon == 1) c.icon(kDownload, ix, cy - 11, 22, ink, 2.8f);
  if (v.icon == 2) c.icon(kPlay, ix, cy - 11, 22, ink, 2, 24, true);
  if (v.icon == 3) c.icon(kFolder, ix, cy - 11, 22, ink, 2.6f);
  if (v.icon) ix += 22 + 10;
  c.text_mid(v.primary, f, ix, cy, ink);
  if (on) hit(x, H_Primary, bx, by, w + 6, h + 6);
  c.pop();
  return w;
}

float ghost_button(Ctx& x, int id, const std::wstring& label, float bx, float by, float h, float pad, bool skew,
                   float fixed_w = 0, uint32_t text = PAPER, uint32_t edge = EDGE, uint32_t fill = 0xFFFFFFFF) {
  Canvas& c = x.c;
  Font f = T(13, 700, (skew || pad == 18) ? 0.06f : 0);
  float tw = c.measure(label, f).width;
  float w = fixed_w > 0 ? fixed_w : 2 + pad + tw + pad + 2;
  bool hover = x.m.hover == id, down = x.m.pressed == id;
  if (skew) c.push(Matrix::Skew(-7, 0, D2D1::Point2F(bx + w / 2, by + h / 2)));
  if (fill != 0xFFFFFFFF) c.fill(bx, by, w, h, fill);
  if (hover || down) c.fill(bx, by, w, h, PAPER, down ? 0.14f : 0.07f);
  c.border(bx, by, w, h, 2, hover ? PAPER : edge);
  c.text_mid(label, f, bx + (w - tw) / 2, by + h / 2, text);
  hit(x, id, bx, by, w, h);
  if (skew) c.pop();
  return w;
}

void version_box(Ctx& x, float bx, float by, float w, float h, const wchar_t* label, const std::wstring& number,
                 const std::wstring& note, uint32_t edge) {
  Canvas& c = x.c;
  c.box(bx, by, w, h, PANEL, 2, edge);
  float tx = bx + 2 + 14, ty = by + 2 + 10;
  Font lab = T(11, 600, 0.12f);
  ty += c.text(label, lab, tx, ty, MUTED) + 4;
  TextOpt num;
  num.line_h = 26 * 1.1f;
  ty += c.text(number, Mo(26, 700), tx, ty, PAPER, num) + 4;
  c.text(note, T(12), tx, ty, MUTED);
}

void notice_box(Ctx& x, const Vals& v, float bx, float by, float w, float* out_h) {
  Canvas& c = x.c;
  const float tx = bx + 2 + 16 + 24 + 12, tw = w - (tx - bx) - 16 - 2;
  Font title = T(16, 700), body = T(13);
  TextOpt wrap;
  wrap.wrap = true;
  wrap.max_w = tw;
  wrap.line_h = 13 * 1.45f;
  float th = c.line_h(title) + 4 + c.measure(v.notice_text, body, wrap).height;
  float h = 2 + 14 + (th < 24 ? 24 : th) + 14 + 2;
  uint32_t edge = v.notice_bad ? RED_EDGE : x.accent;
  c.box(bx, by, w, h, PANEL, 2, edge);
  c.icon(kWarn, bx + 2 + 16, by + 2 + 14, 24, v.notice_bad ? RED_TEXT : x.accent, 2.2f);
  float ty = by + 2 + 14;
  ty += c.text(v.notice_title, title, tx, ty, PAPER) + 4;
  c.text(v.notice_text, body, tx, ty, TEXT2, wrap);
  *out_h = h;
}

void downloading(Ctx& x, float top) {
  Canvas& c = x.c;
  const Model& m = x.m;
  const float left = 40, right = 580;
  // the three steps
  const wchar_t* labels[3] = {L"Загрузка", L"Проверка", m.pending == Pending::Uninstall ? L"Удаление"
                                                        : m.pending == Pending::Launcher ? L"Замена лаунчера" : L"Установка"};
  float sx = left;
  Font lab = T(12, 600);
  for (int i = 0; i < 3; ++i) {
    bool done = i < m.stage, active = i == m.stage;
    float tw = c.measure(labels[i], lab).width;
    float w = 2 + 5 + 20 + 8 + tw + 12 + 2;
    c.box(sx, top, w, 34, active ? 0x1B2A2A : PANEL, 2, active ? GREEN : LINE);
    c.fill(sx + 7, top + 7, 20, 20, active || done ? GREEN : LINE);
    if (done) {
      c.icon(kCheck, sx + 9, top + 9, 16, INK, 3);
    } else {
      wchar_t n[2] = {static_cast<wchar_t>(L'1' + i), 0};
      Font nf = Mo(11, 700);
      c.text_mid(n, nf, sx + 7 + (20 - c.measure(n, nf).width) / 2, top + 17, active ? INK : TEXT2);
    }
    c.text_mid(labels[i], lab, sx + 7 + 20 + 8, top + 17, active ? PAPER : done ? TEXT2 : MUTED);
    sx += w + 8;
  }
  // how far
  float y = top + 34 + 14;
  float share = m.stage > 0 ? 1.0f : m.total ? clamp01(static_cast<float>(static_cast<double>(m.got) / m.total)) : 0.0f;
  int pct = static_cast<int>(share * 100 + 0.5f);
  TextOpt big;
  big.line_h = 40;
  c.push(Matrix::Skew(-7, 0, D2D1::Point2F(left, y + 40)));
  c.text(std::to_wstring(pct) + L"%", D(40), left, y, PAPER, big);
  c.pop();
  std::wstring stage;
  if (m.stage == 0)
    stage = m.total ? megabytes(m.got) + L" из " + megabytes(m.total) + L" МБ" : std::wstring(L"мод уже в лаунчере");
  else if (m.stage == 1)
    stage = m.total ? L"сверяю SHA-256" : L"проверяю, что внутри";
  else
    stage = m.pending == Pending::Uninstall ? L"убираю файлы мода из папки игры"
            : m.pending == Pending::Launcher ? L"меняю файл лаунчера" : L"ставлю файлы в папку игры";
  Font sf = Mo(13, 500);
  c.text(stage, sf, right - c.measure(stage, sf).width, y + 40 - c.line_h(sf), TEXT2);
  // the canister of mutagen
  y += 40 + 14;
  const float tx = left + 34, ty = y + 2, tw = right - 40 - tx, th = 46;
  c.round_box(tx + 6, ty + 6, tw, th, 8, VIOLET, 0, 0);
  c.round_box(tx, ty, tw, th, 8, 0x120E28, 3, BLACK);
  const float ix = tx + 3, iy = ty + 3, iw = tw - 6, ih = th - 6;
  for (float t = 0; t < iw; t += 44) c.fill(ix + t, iy + ih - 12, 2, 12, PAPER, 0.24f);
  float lw = iw * share;
  if (lw > 0.5f) {
    c.fill(ix, iy, lw, ih, GREEN);
    float drift = static_cast<float>(std::fmod(m.time * 14.0, 96.0));
    const float bubbles[3][4] = {{18, 27, 3, 0.6f}, {52, 16, 2, 0.45f}, {78, 30, 2.4f, 0.22f}};
    for (float t = -96 + drift; t < lw; t += 96)
      for (int b = 0; b < 3; ++b) {
        float bx = t + bubbles[b][0];
        if (bx < 4 || bx > lw - 7) continue;
        c.ellipse(ix + bx, iy + bubbles[b][1] - 3, bubbles[b][2], bubbles[b][2], b == 2 ? INK : 0xFFFFFF, bubbles[b][3]);
      }
    if (lw > 26) c.round_box(ix + 10, iy + 5 - 3 + 3, lw - 20 - 3, 5, 2.5f, 0xFFFFFF, 0, 0);
    c.fill(ix + lw - 3, iy, 3, ih, BLACK);
  }
  for (int side = 0; side < 2; ++side) {
    float cxp = side == 0 ? tx - 22 : tx + tw + 22 - 26, cyp = ty - 9;
    c.round_box(cxp, cyp, 26, 64, 5, EDGE, 3, BLACK);
    c.ellipse(cxp + 13, cyp + 13, 3, 3, BLACK);
    c.ellipse(cxp + 13, cyp + 64 - 13, 3, 3, BLACK);
  }
  // where from, and the way out
  y += 2 + 46 + 8 + 14;
  Font cf = T(13, 700, 0.06f);
  float cw = 2 + 18 + c.measure(L"ОТМЕНА", cf).width + 18 + 2;
  TextOpt url;
  url.ellipsis = true;
  url.max_w = right - left - 16 - cw;
  c.text_mid(m.total ? std::wstring(kRepoText) + L"/releases/latest" : std::wstring(L"tmnt2net-launcher.exe: мод внутри"), Mo(12, 500),
             left, y + 22, MUTED, url);
  ghost_button(x, H_Cancel, L"ОТМЕНА", right - cw, y, 44, 18, false);
  y += 44 + 14;
  TextOpt note;
  note.wrap = true;
  note.max_w = right - left;
  note.line_h = 12 * 1.45f;
  c.text(m.pending == Pending::Uninstall ? L"Файлы игры, её сохранения и настройки остаются на месте."
         : m.pending == Pending::Launcher ? L"Новый лаунчер откроется при следующем запуске. Мод в игре не меняется."
                                          : L"Перед установкой лаунчер попросит закрыть игру. Настройки и сохранения остаются.",
         T(12), left, y, MUTED, note);
}

void banner(Ctx& x, const Vals& v) {
  Canvas& c = x.c;
  const Model& m = x.m;
  const float L = 620, Tp = 70, R = 920, B = 468;
  c.push(Matrix::Rotation(-1, D2D1::Point2F((L + R) / 2, Tp)));
  auto cloth = [](float l, float t, float r, float b, float notch, D2D1_POINT_2F* p) {
    p[0] = {l, t};
    p[1] = {r, t};
    p[2] = {r, b};
    p[3] = {(l + r) / 2, b - notch};
    p[4] = {l, b};
  };
  D2D1_POINT_2F p[5];
  cloth(L + 8, Tp + 8, R + 8, B + 8, 28, p);
  c.poly(p, 5, x.accent);
  cloth(L, Tp, R, B, 28, p);
  c.poly(p, 5, BLACK);
  const float l = L + 3, t = Tp + 3, r = R - 3, b = B - 4;
  cloth(l, t, r, b, 27, p);
  c.poly(p, 5, PAPER);
  c.clip_poly(p, 5);
  for (int j = 0; j < 19; ++j)
    for (int i = 0; i < 19; ++i) {
      float px = r - 150 + 4 + 8 * i, py = b - 150 + 4 + 8 * j;
      float d = std::sqrt((r - px) * (r - px) + (b - py) * (b - py)) / 212.0f;
      float a = clamp01(1 - d / 0.72f);
      if (a > 0.01f) c.ellipse(px, py, 1.6f, 1.6f, PINK, 0.2f * a);
    }
  c.fill(l, t, r - l, 38, x.accent);
  c.fill(l, t + 35, r - l, 3, BLACK);
  c.unclip();
  c.text_mid(v.tab, D(12, 0.06f), l + 18, t + 6 + (35 - 6) / 2.0f, PINK);
  c.icon(kShuriken, r - 14 - 18, t + 6 + (35 - 6) / 2.0f - 9, 18, PINK, 0, 20, true);
  c.ellipse(r - 14 - 9, t + 6 + (35 - 6) / 2.0f, 1.8f, 1.8f, x.accent);
  // the words
  const float tx = l + 20, tw = (r - l) - 40;
  float y = t + 38 + 14;
  y += c.text(v.meta, Mo(12, 700), tx, y, 0x4A4466) + 10;
  TextOpt head;
  head.wrap = true;
  head.max_w = tw;
  head.line_h = 17 * 1.3f;
  y += c.text(v.title, D(17), tx, y, PINK, head) + 10;
  const float bottom = b - 40;
  const float links_h = 2.0f + 6 + 30 + (v.copy ? 2.0f + 30 : 0.0f);
  TextOpt item;
  item.wrap = true;
  item.max_w = tw - 11 - 9;
  item.line_h = 13 * 1.45f;
  for (const std::wstring& s : v.items) {
    float h = c.measure(s, T(13), item).height;
    if (y + h > bottom - links_h - 6) break;  // (a long list: what does not fit is on GitHub)
    c.icon(kStar, tx, y + 4, 11, VIOLET, 0, 12, true);
    c.push(Matrix::Scale(11.0f / 12, 11.0f / 12) * Matrix::Translation(tx, y + 4));
    c.stroke_path(kStar, 1, BLACK);
    c.pop();
    c.text(s, T(13), tx + 11 + 9, y, PINK, item);
    y += h + 9;
  }
  // the links, at the bottom
  float ly = bottom - links_h;
  c.dashed_h(tx, ly, tw, 2, 6, 5, PINK, 0.4f);
  ly += 2 + 6;
  Font lf = T(13, 700);
  TextOpt under;
  under.underline = true;
  float lw = c.measure(v.link, lf).width;
  bool hover = m.hover == H_PaperLink;
  c.text_mid(v.link, lf, tx, ly + 15, hover ? VIOLET : PINK, under);
  c.icon(kExternal, tx + lw + 6, ly + 8, 14, hover ? VIOLET : PINK, 2.6f);
  hit(x, H_PaperLink, tx, ly, lw + 6 + 14, 30);
  if (v.copy) {
    ly += 30 + 2;
    bool copied = m.copied_until != 0;
    const wchar_t* label = copied ? L"Ссылка скопирована" : L"Скопировать ссылку для друзей";
    float cw = c.measure(label, lf).width;
    hover = m.hover == H_Copy;
    c.text_mid(label, lf, tx, ly + 15, hover ? VIOLET : PINK, under);
    c.icon(copied ? kCheck : kCopy, tx + cw + 6, ly + 8, 14, hover ? VIOLET : PINK, 2.6f);
    hit(x, H_Copy, tx, ly, cw + 6 + 14, 30);
  }
  c.pop();
  // the rod it hangs from
  c.box(L - 14, 64, (R - L) + 28, 13, EDGE, 3, BLACK);
  for (float kx : {L - 24 + 9.5f, R + 24 - 9.5f}) {
    c.ellipse(kx, 70.5f, 9.5f, 9.5f, BLACK);
    c.ellipse(kx, 70.5f, 6.5f, 6.5f, x.accent);
  }
}

void hero(Ctx& x, const Vals& v) {
  Canvas& c = x.c;
  const Model& m = x.m;
  const float left = 40, right = 580, width = right - left;
  float y = 48 + 26;
  // the line above the title
  Font eyebrow = T(12, 600, 0.14f);
  float eh = c.line_h(eyebrow);
  stripes(c, left, y + eh / 2, 14, 6, 4);
  c.text(L"СЕТЕВОЙ КООП · ДО 4 ИГРОКОВ", eyebrow, left + 4 * 14 + 3 * 4 + 10, y, MUTED);
  y += eh + 14;
  // the title
  TextOpt l1, l2;
  l1.line_h = 56;
  l2.line_h = 38;
  c.push(Matrix::Skew(-7, 0, D2D1::Point2F(left, y + 56 + 6 + 38)));
  c.text(L"TMNT 2", D(56), left + 4, y + 4, VIOLET, l1);
  c.text(L"TMNT 2", D(56), left, y, PAPER, l1);
  c.text(L"BATTLE NEXUS", D(38), left + 3, y + 62 + 3, DEEP, l2);
  c.text(L"BATTLE NEXUS", D(38), left, y + 62, GREEN, l2);
  c.pop();
  y += 56 + 6 + 38 + 22;

  if (m.screen == Screen::Downloading) {
    downloading(x, y);
  } else {
    if (v.offer) {
      const float bw = (width - 28 - 24) / 2;
      float bh = 2 + 10 + c.line_h(T(11, 600)) + 4 + 26 * 1.1f + 4 + c.line_h(T(12)) + 12 + 2;
      version_box(x, left, y, bw, bh, L"УСТАНОВЛЕНО", v.installed, v.installed_note, LINE);
      if (v.arrow) c.icon(kArrow, left + bw + 12, y + bh / 2 - 14, 28, x.accent, 2.6f);
      if (v.equal) c.icon(kEqual, left + bw + 12, y + bh / 2 - 14, 28, GREEN, 2.6f);
      float rx = left + bw + 12 + 28 + 12;
      version_box(x, rx, y, bw, bh, v.latest_label.c_str(), v.latest, v.latest_note, v.latest_border);
      if (v.has_new) {
        Font bf = T(10, 700, 0.12f);
        float tw = c.measure(L"НОВАЯ", bf).width, bh2 = 2 + 2 + c.line_h(bf) + 2 + 2;
        float bx = rx + bw - 2 - 10 - (tw + 16 + 4);
        c.box(bx, y - 9, tw + 16 + 4, bh2, x.accent, 2, BLACK);
        c.text(L"НОВАЯ", bf, bx + 2 + 8, y - 9 + 2 + 2, PINK);
      }
      y += bh + 12;
      if (v.compat) {
        const float cy = y + 10;
        uint32_t ic = v.compat == 1 ? x.accent : v.compat == 2 ? GREEN : v.compat == 3 ? WAIT : v.compat == 4 ? RED_TEXT : MUTED;
        if (v.compat == 3) {
          c.push(Matrix::Rotation(static_cast<float>(std::fmod(m.time * 300.0, 360.0)), D2D1::Point2F(left + 9, cy)));
          c.icon(kSpin, left, cy - 9, 18, ic, 2.6f);
          c.pop();
        } else {
          c.icon(v.compat == 2 ? kCheck : v.compat == 5 ? kArrow : kWarn, left, cy - 9, 18, ic, v.compat == 2 ? 2.6f : 2.4f);
        }
        TextOpt one;
        one.ellipsis = true;
        one.max_w = width - 26;
        c.text_mid(v.compat_text, T(13), left + 18 + 8, cy, TEXT2, one);
      }
      y += 20 + 20;
    } else if (v.notice) {
      float nh = 0;
      notice_box(x, v, left, y, width, &nh);
      y += nh + 20;
    }
    // the buttons
    float pw = primary_button(x, v, left + 6, y);
    if (!v.secondary.empty()) ghost_button(x, H_Secondary, v.secondary, left + 6 + pw + 18, y, 62, 20, true);
    y += 62 + 16;
    TextOpt hint;
    hint.wrap = true;
    hint.max_w = width;
    hint.line_h = 12 * 1.45f;
    c.text(v.hint, T(12), left, y, m.has_game && !m.exe_ok && v.offer ? WAIT : MUTED, hint);
  }

  // the game folder, at the bottom
  const float cy = 48 + 508 - 28;
  c.icon(kFolder, left, cy - 9, 18, MUTED, 2.2f);
  Font lab = T(11, 600, 0.12f);
  float px = left + 18 + 10;
  c.text_mid(L"ПАПКА ИГРЫ", lab, px, cy, MUTED);
  px += c.measure(L"ПАПКА ИГРЫ", lab).width + 10;
  Font cf = T(13, 600);
  float cw = c.measure(L"Изменить", cf).width + 16;
  TextOpt path;
  path.ellipsis = true;
  path.max_w = right - px - 10 - cw;
  Font pf = Mo(12, 500);
  float pwid = c.measure(v.path, pf).width;
  if (pwid > path.max_w) pwid = path.max_w;
  c.text_mid(v.path, pf, px, cy, PAPER, path);
  px += pwid + 10;
  TextOpt under;
  under.underline = true;
  c.text_mid(L"Изменить", cf, px + 8, cy, m.hover == H_Folder ? PAPER : TEXT2, under);
  hit(x, H_Folder, px, cy - 22, cw, 44);

  banner(x, v);
}

// --- settings --------------------------------------------------------------------------------

struct Section {
  float x, y, w, cx, cw, cy;  // the box; where its content starts
};

Section section_begin(Ctx& x, float sx, float sy, float w, float h, const wchar_t* title) {
  Canvas& c = x.c;
  c.box(sx, sy, w, h, PANEL, 2, LINE);
  Section s{sx, sy, w, sx + 2 + 16, w - 4 - 32, sy + 2 + 14};
  s.cy += c.text(title, D(13, 0.06f), s.cx, s.cy, x.accent);
  return s;
}

void toggle_row(Ctx& x, int id, const wchar_t* label, bool on, float lx, float ty, float w) {
  Canvas& c = x.c;
  const float cy = ty + 22;
  c.text_mid(label, T(14), lx, cy, PAPER);
  const float bx = lx + w - 56, tx = bx, tyy = cy - 13;
  bool hover = x.m.hover == id;
  c.box(tx, tyy, 52, 26, on ? GREEN : 0x1F1840, 2, on ? BLACK : hover ? PAPER : EDGE);
  c.box(tx + 2 + (on ? 28 : 2), tyy + 2 + 2, 18, 18, PAPER, 2, BLACK);
  hit(x, id, bx, ty, 56, 44);
}

void swatch(Ctx& x, int index, float sx, float sy, const char* weapon) {
  Canvas& c = x.c;
  int id = H_Accent0 + index;
  bool on = (x.m.set.accent & 3) == index, hover = x.m.hover == id;
  c.push(Matrix::Skew(-7, 0, D2D1::Point2F(sx + 22, sy + 22)));
  if (on || hover) c.border(sx - 5, sy - 5, 54, 54, 3, PAPER, on ? 1.0f : 0.45f);
  c.box(sx, sy, 44, 44, kBandana[index], 3, BLACK);
  c.icon(weapon, sx + 8, sy + 8, 28, INK, 2.3f);
  hit(x, id, sx, sy, 44, 44);
  c.pop();
}

void settings(Ctx& x) {
  Canvas& c = x.c;
  const Model& m = x.m;
  const float left = 40, top = 48 + 20;
  // back, and the title
  Font bf = T(13, 700);
  float bw = 2 + 10 + 18 + 8 + c.measure(L"Назад", bf).width + 14 + 2;
  bool hover = m.hover == H_Back;
  c.box(left, top, bw, 44, PANEL, 2, hover ? PAPER : EDGE);
  c.icon(kBack, left + 12, top + 13, 18, PAPER, 2.4f);
  c.text_mid(L"Назад", bf, left + 12 + 18 + 8, top + 22, PAPER);
  hit(x, H_Back, left, top, bw, 44);
  TextOpt tl;
  tl.line_h = 30;
  Font tf = D(30);
  float tw = c.measure(L"НАСТРОЙКИ", tf, tl).width, tx = left + bw + 18;
  c.push(Matrix::Skew(-7, 0, D2D1::Point2F(tx + tw / 2, top + 22)));
  c.text(L"НАСТРОЙКИ", tf, tx + 3, top + 7 + 3, VIOLET, tl);
  c.text(L"НАСТРОЙКИ", tf, tx, top + 7, PAPER, tl);
  c.pop();

  const float col_w = (kW - 80 - 28) / 2.0f, x1 = left, x2 = left + col_w + 28;
  const float cw = col_w - 4 - 32;
  Font tiny = T(12);
  float h3 = c.line_h(D(13, 0.06f)), sm = c.line_h(tiny);
  TextOpt note;
  note.wrap = true;
  note.max_w = cw;
  note.line_h = 12 * 1.45f;
  std::wstring source = std::wstring(L"Источник — последний релиз в ") + kRepoText + L".";
  float note_h = c.measure(source, tiny, note).height;
  float h_updates = 2 + 14 + h3 + 2 + 6 + 44 + 6 + 44 + 6 + note_h + 16 + 2;
  float h_net = 2 + 14 + h3 + 8 + sm + 8 + 44 + 8 + sm + 8 + 40 + 16 + 2;
  float row1 = h_updates > h_net ? h_updates : h_net;
  float h_game = 2 + 14 + h3 + 8 + 44 + 8 + 44 + 8 + 44 + 16 + 2;
  float h_look = 2 + 14 + h3 + 8 + sm + 8 + 56 + 8 + 44 + 16 + 2;
  float row2 = h_game > h_look ? h_game : h_look;
  float y1 = top + 44 + 16, y2 = y1 + row1 + 18;

  // updates
  Section s = section_begin(x, x1, y1, col_w, row1, L"ОБНОВЛЕНИЯ");
  float y = s.cy + 2 + 6;
  toggle_row(x, H_Toggle1, L"Проверять при запуске", m.set.auto_check, s.cx, y, cw);
  y += 44 + 6;
  toggle_row(x, H_Toggle2, L"Ставить обновления без вопроса", m.set.auto_install, s.cx, y, cw);
  y += 44 + 6;
  c.text(source, tiny, s.cx, y, MUTED, note);

  // the network
  s = section_begin(x, x2, y1, col_w, row1, L"СЕТЬ");
  y = s.cy + 8;
  c.text(L"Прокси для GitHub — если он не открывается напрямую", tiny, s.cx, y, MUTED);
  y += sm + 8;
  const wchar_t* modes[3] = {L"Авто", L"Нет", L"Свой"};
  const float seg = (cw - 16) / 3;
  for (int i = 0; i < 3; ++i) {
    bool on = m.set.proxy == i;
    ghost_button(x, H_ProxyAuto + i, modes[i], s.cx + i * (seg + 8), y, 44, 0, false, seg, on ? INK : PAPER, on ? BLACK : EDGE,
                 on ? GREEN : PANEL);
  }
  y += 44 + 8;
  c.text(m.set.proxy == 0 ? L"Авто: как в Windows, а если не выходит — VPN-клиент на этом ПК"
                          : m.set.proxy == 1 ? L"Нет: только напрямую" : L"Адрес своего прокси (HTTP)",
         tiny, s.cx, y, MUTED);
  y += sm + 8;
  bool own = m.set.proxy == 2;
  c.box(s.cx, y, cw, 40, 0x0C0A18, 2, m.proxy_focus ? PAPER : m.hover == H_ProxyField ? TEXT2 : EDGE);
  Font pf = Mo(13, 500);
  if (m.set.proxy_addr.empty() && !m.proxy_focus) {
    c.text_mid(L"адрес:порт", pf, s.cx + 2 + 12, y + 20, MUTED, {}, 0.7f);
  } else {
    TextOpt clip;
    clip.max_w = cw - 28;
    clip.ellipsis = true;
    c.text_mid(m.set.proxy_addr, pf, s.cx + 2 + 12, y + 20, own ? PAPER : MUTED, clip);
    if (m.proxy_focus && std::fmod(m.time, 1.0) < 0.55) {
      float caret = s.cx + 2 + 12 + c.measure(m.set.proxy_addr, pf).width + 1;
      if (caret < s.cx + cw - 12) c.fill(caret, y + 11, 2, 18, PAPER);
    }
  }
  hit(x, H_ProxyField, s.cx, y, cw, 40);

  // the game and the mod
  s = section_begin(x, x1, y2, col_w, row2, L"ИГРА И МОД");
  y = s.cy + 8;
  c.icon(kFolder, s.cx, y + 13, 18, MUTED, 2.2f);
  Font cf = T(13, 600);
  float chw = c.measure(L"Изменить", cf).width + 8;
  TextOpt path;
  path.ellipsis = true;
  path.max_w = cw - 28 - 10 - chw;
  c.text_mid(m.has_game ? m.set.game_dir : L"(папка не выбрана)", Mo(12, 500), s.cx + 28, y + 22, PAPER, path);
  TextOpt under;
  under.underline = true;
  c.text_mid(L"Изменить", cf, s.cx + cw - chw + 4, y + 22, m.hover == H_FolderSettings ? PAPER : TEXT2, under);
  hit(x, H_FolderSettings, s.cx + cw - chw, y, chw, 44);
  y += 44 + 8;
  const float half = (cw - 8) / 2;
  ghost_button(x, H_OpenLog, L"Открыть журнал", s.cx, y, 44, 0, false, half);
  ghost_button(x, H_DesyncLogs, L"Журналы рассинхрона", s.cx + half + 8, y, 44, 0, false, half);
  y += 44 + 8;
  ghost_button(x, H_Reinstall, L"Переустановить мод", s.cx, y, 44, 0, false, half);
  if (m.confirm_uninstall)
    ghost_button(x, H_Uninstall, L"Точно удалить?", s.cx + half + 8, y, 44, 0, false, half, PAPER, RED_TEXT, RED_EDGE);
  else
    ghost_button(x, H_Uninstall, L"Удалить мод", s.cx + half + 8, y, 44, 0, false, half, RED_TEXT, RED_EDGE);

  // the launcher itself
  s = section_begin(x, x2, y2, col_w, row2, L"ЛАУНЧЕР");
  y = s.cy + 8;
  c.text(L"Цвет повязки — цвет акцентов лаунчера", tiny, s.cx, y, MUTED);
  y += sm + 8;
  const char* weapons[4] = {kKatana, kSai, kBo, kNunchaku};
  for (int i = 0; i < 4; ++i) swatch(x, i, s.cx + 8 + i * (44 + 18), y + 6, weapons[i]);
  y += 56 + 8;
  toggle_row(x, H_Toggle3, L"Закрывать лаунчер, когда игра запущена", m.set.close_on_play, s.cx, y, cw);
}

}  // namespace

bool primary_enabled(const Model& m) { return vals(m).primary_on; }
std::wstring link_url(const Model& m) { return vals(m).link_url; }

void draw(gfx::Canvas& c, const Model& m, std::vector<Hit>* hits) {
  if (hits) hits->clear();
  Ctx x{c, m, hits, kBandana[m.set.accent & 3]};
  c.cached(1, kW, kH, background);
  Vals v = vals(m);
  header(x);
  if (m.screen == Screen::Settings) settings(x);
  else hero(x, v);
  footer(x, v);
}

// The screens as the design shows them.
bool demo(const std::wstring& screen, Model& m) {
  m = Model{};
  m.has_game = true;
  m.set.game_dir = L"D:\\Games\\TMNT 2 - Battle Nexus";
  m.inst.present = true;
  m.inst.version = L"0.16.7";
  m.inst.protocol = 14;
  m.rel.known = m.rel.full = true;
  m.rel.version = L"0.17.0";
  m.rel.protocol = 15;
  m.rel.date = L"20 ноября 2026";
  m.rel.title = L"Ровная игра по сети";
  m.rel.notes = {L"Чужой высокий пинг больше не тормозит игру у всех.", L"Враги рядом с любым игроком дерутся как обычно.",
                 L"Alt+Tab больше не останавливает сетевую игру."};
  m.rel.pkg_name = L"tmnt2net-launcher.exe";
  m.rel.pkg_size = 5900000;
  m.rel.pkg_sha256 = "x";
  m.pack.known = true;
  m.pack.version = L"0.16.7";
  m.pack.protocol = 14;
  m.pack.title = L"Перепроверка 0.16.6";
  m.pack.notes = {L"Убран остаток прошлой сессии: игра не путает, кто вошёл посреди игры.",
                  L"Сообщение «игра идёт медленно» не появляется зря в начале сессии."};
  m.set.notes_version = L"0.16.7";
  m.set.notes_title = L"Перепроверка 0.16.6";
  m.set.notes = {L"Убран остаток прошлой сессии: игра не путает, кто вошёл посреди игры.",
                 L"Сообщение «игра идёт медленно» не появляется зря в начале сессии."};
  m.total = 5900000;
  m.got = static_cast<uint64_t>(5900000 * 0.62);
  if (screen == L"update") {
    m.screen = Screen::Update;
  } else if (screen == L"downloading") {
    m.screen = Screen::Downloading;
    m.pending = Pending::Install;
  } else if (screen == L"ready") {
    m.screen = Screen::Ready;
    m.inst.version = L"0.17.0";
    m.inst.protocol = 15;
  } else if (screen == L"checking") {
    m.screen = Screen::Checking;
    m.rel = Release{};
  } else if (screen == L"install") {
    m.screen = Screen::Install;
    m.inst = Installed{};
  } else if (screen == L"nogame") {
    m.screen = Screen::NoGame;
    m.has_game = false;
    m.inst = Installed{};
  } else if (screen == L"offline") {
    m.screen = Screen::Offline;
    m.rel = Release{};
  } else if (screen == L"blocked") {
    m.screen = Screen::Offline;
    m.rel.full = false;
    m.assets_blocked = true;
  } else if (screen == L"running") {
    m.screen = Screen::Running;
    m.pending = Pending::Install;
  } else if (screen == L"problem") {
    m.screen = Screen::Problem;
    m.problem_title = L"Файл повреждён";
    m.problem_text = L"Контрольная сумма скачанного файла не совпала с той, что указана в релизе. Файл удалён — скачайте его ещё раз.";
    m.problem_primary = L"СКАЧАТЬ ЗАНОВО";
    m.problem_secondary = L"НАЗАД";
  } else if (screen == L"settings") {
    m.screen = Screen::Settings;
    m.back = Screen::Update;
  } else {
    return false;
  }
  return true;
}

}  // namespace view
}  // namespace lz
