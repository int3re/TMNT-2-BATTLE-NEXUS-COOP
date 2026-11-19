// tmnt2net in the game's own menus (stage 5), with the game's font, colours,
// sounds and buttons:
// - the title menu gets СЕТЕВАЯ ИГРА and ДОПОЛНИТЕЛЬНО between ПАРАМЕТРЫ and
//   ВЫХОД (its item table is moved into tmnt2net and grown to 6 rows);
// - СЕТЕВАЯ ИГРА restarts the game straight into the network menu (a session
//   has to start at the game's first frame);
// - ДОПОЛНИТЕЛЬНО opens a page of tmnt2net's settings (screen, widescreen,
//   controls, network), written to tmnt2net.ini for the next start;
// - notices (a player joined or left, desync) are written with the game's font
//   at the top of the frame.
// In a network session the title menu is the game's own (every PC the same).
#pragma once
#include "mod/config.h"

namespace menus {

void install(const Config& cfg);  // InitializeASI, before the title runs
void on_session_start();
void on_session_end();  // a session in a running game is over: our title menu again
bool title_menu_up();  // the title's menu is on screen (game thread)
bool title_before_menu();  // the title is up before its menu (the opening, НАЖМИ СТАРТ)

}  // namespace menus
