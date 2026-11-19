// Each player's own health in co-op (0.8.0, user request). The game keeps one health for the
// whole team: any hit takes from it, and at zero the game is over for everybody. With two or
// more players:
// - every player has a health of their own (the team's maximum each); hits, pizza and the
//   comeback item count for that player only;
// - a player at zero falls asleep for 10 s, invincible, and gets up with a third of the
//   maximum; a teammate's hit wakes them sooner. The game is over only when all are down;
// - the health ring shows this PC's player (the first player on a shared screen); each
//   player's box at the top right gets a bar with their health.
// The same on every PC of a session: only the ring differs per PC, and it draws, nothing more.
#pragma once
#include "mod/config.h"

namespace health {

void install(const Config& cfg);  // InitializeASI

// This PC's player in a network game (the one its ring shows), else the first.
int own_player();

}  // namespace health
