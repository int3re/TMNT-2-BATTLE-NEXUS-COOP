// The game's settings on the launcher's ГРАФИКА, ЗВУК and УПРАВЛЕНИЕ pages: what tmnt2net keeps in
// tmnt2net.ini next to TMNT2.exe - the picture ([video]), the sound levels ([audio]) and the keyboard's
// keys ([input]). The rows are those of the game's ОПЦИИ → ЭКРАН, ЗВУК, УПРАВЛЕНИЕ and of НАСТРОЙКИ
// МОДА; a change is written at once and is read by the game at its next start.
#pragma once
#include <string>

namespace lz {
namespace gset {

enum Row {
  kMode, kResolution, kAspect, kHud, kMap, kGauges, kGaugesSize,              // ГРАФИКА: the screen
  kAntialiasing, kAnisotropy, kSharpen, kColors, kFov, kOwnCamera, kVsync,   //          the picture
  kMusic, kEffects, kVoices,                                                  // ЗВУК: how loud
  kSoundMode, kIntro, kBackground,                                            //       how it sounds
  kRows
};
constexpr int kFirstPicture = kAntialiasing, kFirstSound = kMusic, kFirstGame = kSoundMode;
constexpr int kLevelMax = 10;  // a sound level: 0 (silent) .. 10

enum class Kind { Choice, Level, Pair };  // values with arrows; 0..10; two buttons

// Reads tmnt2net.ini where the game sees it (again: the game may have changed it). mod_version: the
// installed mod's, empty if not known.
void load(const std::wstring& game_dir, const std::wstring& mod_version);
// Values for a picture: no file is read or written.
void load_canned(bool two_players);

bool has_mod();  // tmnt2net.ini is there
// The first version of the mod that reads this row's key, if the installed one is older (else null): the
// sound levels came with 0.17.2, the sound mode with 0.17.3. A mod of an unknown version counts as new.
const wchar_t* needs_mod(Row r);

Kind kind(Row r);
const wchar_t* label(Row r);
const char* key(Row r);            // its name for the lab: mode, music, ...
bool usable(Row r);
int count(Row r);                  // how many values it has now
int value(Row r);                  // the one set
std::wstring name(Row r, int v);   // a value's words

// Sets a value and writes the file. False: not written, error() says why.
bool set(Row r, int v);
// The mod's defaults for the rows of a page (1 ГРАФИКА, 2 ЗВУК): those of them that can be set.
bool reset(int page);
const std::wstring& error();
std::wstring ini_file();  // where the settings are read and written (a copy Windows keeps for the game
                          // of a protected folder's file stands in for the folder's own)

// --- УПРАВЛЕНИЕ: the keyboard's keys ([input] keys) --------------------------------------------------
// The thirteen actions of the game's key table (ОПЦИИ → УПРАВЛЕНИЕ), in the page's order: a column of
// moving, a column of fighting, the rest. A key is a DirectInput code; kMouseKey.. are the mouse's
// buttons as tmnt2net gives them to the game.
constexpr int kKeyActions = 13;
constexpr int kMouseLeft = 0xF0, kMouseRight = 0xF1, kMouse4 = 0xF3, kMouse5 = 0xF4;

const wchar_t* action_label(int action);
const char* action_key(int action);  // its name for the lab: up, jump, attack, ...
int key_code(int action);
std::wstring key_name(int code);     // "Пробел", "Num 4", "ЛКМ"
bool key_allowed(int code);          // the game's key page takes it
// Gives the action that key; the action that had it takes this one's. False: not written.
bool set_key(int action, int code);

// РАСКЛАДКА: 0 the game's own keys, 1 WASD and the mouse, 2 the player's own.
int layout();
bool has_own_layout();  // keys of the player's own are kept from before a ready layout
bool set_layout(int v);
bool reset_keys();      // the game's own keys, one player at the keyboard

// КЛАВИАТУРА: one player, or two (each half of the keyboard a pad of its own, with fixed keys).
int keyboard_players();
bool set_keyboard_players(int n);
constexpr int kHalfRows = 12;
const wchar_t* half_label(int row);
const wchar_t* half_keys(int row);  // "F · Num 1": the left half's key and the right one's

}  // namespace gset
}  // namespace lz
