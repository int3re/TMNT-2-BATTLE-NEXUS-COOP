// The game's frame for lockstep (stage 3). With [debug] determinism = 1:
// - drawing is never skipped (drawn effects draw random numbers);
// - file and sound loading finish within the frame that waits for them (the
//   number of frames a load takes no longer depends on the disk);
// - every frame is numbered at CPCInputsDevice::Sync, where the input of the
//   next frame is fixed: the keyboard, four pads and the answers to what the
//   logic asks of real time (movie ended, a voice still playing). The game reads
//   only that FrameInput during the frame;
// - [debug] record writes each FrameInput to a file, [debug] replay feeds a
//   recording back (it may still be written by another copy of the game);
// - tmnt2net-sim.log gets one line per frame: frame, seed epoch, RpRandom calls,
//   a hash of the generator state and of the input. Two copies fed the same input
//   must write the same lines.
// Stage 4 fills the FrameInput from every player over the network at the same point.
#pragma once
#include <cstdint>

#include "mod/config.h"

namespace sim {

constexpr int kPads = 4;  // joysticks the game sees in lockstep, always these four

struct PadInput {
  uint16_t buttons = 0;  // bit b: the game's button b (cross, square, circle, triangle, L1, L2, R1, R2,
                         // select, start, L3, R3)
  uint8_t dpad = 0;      // 1 up, 2 down, 4 left, 8 right
  uint8_t connected = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;  // -32767..32767, y down positive
};

enum FrameFlags : uint8_t {
  kMovieEnded = 1,  // the movie numbered FrameInput::movie has played to its end
  kVoiceBusy = 2,   // a voice is playing
  kBoot = 4,        // network game: the game's start-up (logos, the opening) before the title, run at once
};

struct FrameInput {
  uint8_t keys[32] = {};  // DIK bitset
  PadInput pads[kPads];
  uint16_t movie = 0;  // movies created since the start (the one kMovieEnded is about)
  uint8_t flags = 0;
  uint8_t reserved = 0;
};
static_assert(sizeof(FrameInput) == 32 + 4 * 12 + 4, "FrameInput is a file record");

void install(const Config& cfg);  // InitializeASI, after the exe hash matched
bool active();

uint32_t frame();  // frames fixed so far (0 before the first)

// The keyboard buffer the game reads this frame: scripted keys are added.
void apply_frame_keys(uint8_t keys[256]);

// The game's start-up (logos, the opening, "НАЖМИ СТАРТ") run at once up to the title menu: Enter
// pressed for the player now and then, every movie over at once, frames not shown nor held to 60.
bool starting_up();
void title_menu_reached();  // the title's menu is up: the start-up is over (game thread)
void run_to_title_menu();   // the title's opening again at once to its menu (a session waits there)
void dump_data(int n);      // [debug] the game's data objects to files

// What the game's devices report this frame (only while active()).
void keyboard(uint8_t keys[256]);
const PadInput& pad(int slot);

}  // namespace sim
