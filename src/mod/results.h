// The area's results per player (0.10.0, user request): while the team dances after an area,
// a table at the top: each player's name and turtle, hits dealt, blows blocked, hits taken,
// times down, teammates woken. Counted from the game's own record of every technical action
// (CGamePlayResult::AddTechnicalAction, the same on every PC) and from health.cpp; shown with
// two players or more. Display only.
#pragma once
#include "mod/config.h"

namespace results {

void install(const Config& cfg);  // InitializeASI

void note(int player, int character);   // health.cpp, every frame of a stage: who plays which turtle
void down(int player);                  // health.cpp: player n fell asleep
void woke(int reviver, int player);     // health.cpp: a teammate guarding by them woke them
void show();                            // coop.cpp: the dance after an area starts
void hide();                            // coop.cpp: the dance is over (the table stays 6 s more)
void tick();                            // coop.cpp, game thread, every frame
void draw();                            // coop.cpp, while the game draws: the table in the game's lettering
// The player who played `character` in the area just ended (-1: nobody); valid from show() on.
int player_of(int character);

}  // namespace results
