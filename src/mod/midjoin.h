// Joining a running network game straight into the fight (0.12.0, user request). The team always
// has the four turtles shared among its players (one player switches between several with the
// character change button). A player who joins later (or was in the session but not in the team)
// takes a turtle from the player who has the most: one that is asleep (not the one being played).
// - in a stage: the sleeping turtle moves over to a new player with the joiner's pad, next to its
//   old owner, with full health (no model is loaded); the team for the next stages changes too;
// - between stages: the team changes before the next stage sets its players up.
// Everything happens at the same frame of the game on every PC (the frames' player masks of the
// session decide), so the games stay the same.
#pragma once
#include <cstdint>

#include "mod/config.h"

namespace midjoin {

void install(const Config& cfg);  // InitializeASI

// netplay::frame_input, game thread: the players of this frame (bit = pad slot).
void frame_players(uint32_t frame, uint8_t mask);

}  // namespace midjoin
