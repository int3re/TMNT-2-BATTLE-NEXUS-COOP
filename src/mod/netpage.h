// СЕТЕВАЯ ИГРА in the title menu: a page in the game's lettering (like ДОПОЛНИТЕЛЬНО) where
// players meet in the running game: open your game (host) or pick a friend's game over Steam;
// the host's СТАРТ ИГРЫ then begins the session right at the title menu of every player's running
// game (netplay's lobby_*, no restart). Game thread only.
#pragma once

namespace netpage {

void open();
void close();  // the session began: the page goes (the title menu after it)
bool active();  // the page is up (also when a Steam invite joined a friend's game here)
void update();  // instead of the title menu's update
void draw();    // instead of the title menu's draw

}  // namespace netpage
