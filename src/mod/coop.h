// Co-op touches (stage 6 requests), the same on every PC of a session:
// - character select: a player's cursor passes over turtles another player
//   has taken (the game already refuses them, but the cursor used to stop
//   there and the pick looked possible), and leaves a turtle someone just took;
// - area map: one turtle icon per player instead of one for the team;
// - network game: each player's name instead of their "P1".."P4" tag;
// - the dance after an area: the whole team dances, not only the MVP (the
//   MVP in front; the others behind, without their shouts);
// - in-stage transitions (2+ players): everyone standing, walking or running
//   jumps straight up as the screen starts to fade.
#pragma once
#include "mod/config.h"

namespace coop {

void install(const Config& cfg);  // InitializeASI

// Players who joined at the last character select (1..4).
int players();

// Game thread, after the game drew its frame (menus.cpp's draw hook).
void after_frame();

}  // namespace coop
