// tmnt2net wire protocol: lockstep for up to 4 players (stage 4).
// Transport (common/transport.h): ENet over UDP for LAN / direct IP, or Steam
// networking messages; connect/disconnect, RTT and timeouts are the transport's.
//
// Star around the host: every client sends the host its own input for frame N
// (sampled input_delay frames earlier); once the host has every player's input
// for N it sends all clients the frame bundle N, the input the game reads in
// frame N on every PC (sim::FrameInput). Nobody runs frame N without bundle N.
//
// Payloads are little-endian, packed, no pointers or std:: types. Bump
// kVersion on ANY change to a struct layout, channel use or field meaning, and
// whenever the mod changes what the game does (lockstep needs the same game on
// every PC): 2 = 0.7.1, the whole team dances after an area; 3 = 0.7.2, the
// whole team jumps at in-stage transitions; 4 = 0.8.0, each player has their own health;
// 5 = 0.9.0, players join a running game (Welcome.late, PT_History).
// 6 = 0.10.0, a guarding teammate wakes a sleeping player (the game goes another way than 0.9).
// 7 = 0.12.0, a player who joins a running game gets a turtle of the team at once (midjoin.cpp).
// 8 = 0.12.2, Hello.player_id: a player back after a drop is known by it (or a Steam id), any name.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tmnp {

constexpr uint32_t kMagic = 0x504E3254;  // bytes 'T','2','N','P'
constexpr uint16_t kVersion = 13;
constexpr uint16_t kDefaultPort = 7790;  // TMNT1's tmntnet uses 7777
constexpr int kMaxPlayers = 4;
constexpr int kHistory = 8;  // frames repeated in every input / bundle packet

// Channels (transport::Channel uses the same numbers).
constexpr uint8_t kChannelControl = 0;  // reliable, ordered
constexpr uint8_t kChannelInput = 1;    // unreliable: inputs, bundles, hashes
constexpr uint8_t kChannelCount = 2;

enum PacketType : uint8_t {
  PT_Hello = 1,    // client -> host once the transport link is up
  PT_Welcome = 2,  // host -> client: your player number
  PT_Reject = 3,   // host -> client, with a reason; host then disconnects
  PT_Roster = 4,   // host -> all: who is in (lobby and game)
  PT_Save = 5,     // host -> client: a piece of the host's tmntsave.dat
  PT_Ready = 6,    // client -> host: save stored, waiting at the first frame
  PT_Start = 7,    // host -> all: the game starts (input delay, players)
  PT_Input = 8,    // client -> host: own input, newest kHistory frames
  PT_Frames = 9,   // host -> clients: frame bundles, newest kHistory
  PT_Hash = 10,    // client -> host: state hash of a frame (every kHashEvery)
  PT_Desync = 11,  // host -> all: hashes differ from this frame on
  PT_Bye = 12,     // leaving on purpose
  PT_Alive = 13,   // client -> host in the lobby (header only): still here
  PT_History = 14, // host -> a player joining a running game: every frame so far
  PT_Go = 15,      // host -> all, from the network page: every game restarts into the session
  PT_Record = 16,  // host -> a player, before PT_Start: the progress every game plays with (CGameData::Record)
};

enum RejectReason : uint8_t {
  RR_Version = 1,      // protocol version mismatch
  RR_Full = 2,         // 4 players already
  RR_Started = 3,      // the game has started; nobody can join in the middle
  RR_ExeMismatch = 4,  // a different TMNT2.exe
};

constexpr uint32_t kHashEvery = 30;  // frames between state hashes

#pragma pack(push, 1)
struct PacketHeader {
  uint32_t magic;
  uint16_t version;
  uint8_t type;
  uint8_t flags;
  uint32_t seq;
};

struct HelloPacket {
  PacketHeader h;
  uint8_t exe_sha256[32];
  char mod_version[16];
  char name[32];
  uint64_t player_id;  // the PC's own id (tmnt2net.ini [net] player_id): a player is known again by it
};

struct WelcomePacket {
  PacketHeader h;
  uint8_t player;  // 0..3: the pad this client drives
  uint8_t late;    // 1: the game runs already; the client replays it from frame 1 (PT_History)
  uint8_t reserved[2];
  char name[32];   // the host's name
};

struct RejectPacket {
  PacketHeader h;
  uint8_t reason;
  char message[95];  // UTF-8, for the UI
};

struct RosterPacket {
  PacketHeader h;
  uint8_t present;  // bit p: player p is connected
  uint8_t ready;    // bit p: player p is waiting at the first frame
  uint8_t started;
  uint8_t waiting;  // bit p: the host has been waiting for player p's input (0.10.0; was 0)
  char names[kMaxPlayers][32];
  uint16_t ping_ms[kMaxPlayers];
};

// The host's save, split; total 0 = the host has none (the client must not load its own).
struct SavePacket {
  PacketHeader h;
  uint32_t total;
  uint32_t offset;
  uint16_t size;
  uint8_t data[1024];
};

struct ReadyPacket {
  PacketHeader h;
  uint32_t save_bytes;  // what the client stored, as a check
};

struct StartPacket {
  PacketHeader h;
  uint8_t input_delay;  // frames
  uint8_t players;      // bit p: player p takes part
  uint16_t reserved;
};

// One player's input for one frame (sim::PadInput + what the logic asks of real time).
struct PadWire {
  uint16_t buttons;
  uint8_t dpad;
  uint8_t connected;
  int16_t lx, ly, rx, ry;
};

struct InputWire {
  uint32_t frame;
  PadWire pad;
  uint16_t movie;  // movies created so far on that PC
  uint8_t flags;   // sim::FrameFlags as seen on that PC
  uint8_t reserved;
};

struct InputPacket {
  PacketHeader h;
  uint8_t player;
  uint8_t count;  // 1..kHistory, items[0] is the newest
  uint16_t reserved;
  InputWire items[kHistory];
};

// What every PC feeds the game in one frame.
struct BundleWire {
  uint32_t frame;
  PadWire pads[kMaxPlayers];
  uint16_t movie;
  uint8_t flags;
  uint8_t players;  // bit p: player p still takes part
};

struct FramesPacket {
  PacketHeader h;
  uint8_t count;  // 1..kHistory, items[0] is the newest
  uint8_t reserved[3];
  BundleWire items[kHistory];
};

struct HashPacket {
  PacketHeader h;
  uint32_t frame;
  uint32_t rng_calls;
  uint32_t rng_hash;
  uint8_t player;
  uint8_t reserved[3];
};

struct DesyncPacket {
  PacketHeader h;
  uint32_t frame;
  uint8_t player;  // whose hash differed from the host's
  uint8_t reserved[3];
};

// A player who joins a running game gets the frame bundles from frame 1 (reliable, in order),
// replays them as fast as the game goes and is in once their input reaches the newest frame.
constexpr int kHistoryItems = 18;
struct HistoryPacket {
  PacketHeader h;
  uint32_t first;  // frame of items[0]
  uint8_t count;   // 1..kHistoryItems
  uint8_t reserved[3];
  BundleWire items[kHistoryItems];
};

struct ByePacket {
  PacketHeader h;
};

constexpr uint32_t kRecordSize = 0x968;  // CGameData::Record (TMNT2.exe 2004)
struct RecordPacket {
  PacketHeader h;
  uint32_t size;
  uint8_t data[kRecordSize];
  uint8_t fresh;  // the game's "new game" flag beside the record (1: no progress: ПРОДОЛЖИТЬ off)
  // The host's options that are part of the game's logic (were reserved zeros before 0.16.0: a host
  // without them sends 0 = "not given", and every game keeps its own, as before).
  uint8_t options;     // bit 7: given; bit 0 autosave, bit 1 hit effects, bit 2 help windows
  uint8_t difficulty;  // 0..2
  uint8_t reserved;
};

struct GoPacket {
  PacketHeader h;
  uint8_t players;  // places the host's restarted game waits for
  uint8_t reserved[3];
};
#pragma pack(pop)

inline void fill_header(PacketHeader& h, PacketType type, uint32_t seq) {
  h.magic = kMagic;
  h.version = kVersion;
  h.type = type;
  h.flags = 0;
  h.seq = seq;
}

// Header check; returns the packet type or 0.
inline uint8_t check_header(const void* data, size_t size) {
  if (size < sizeof(PacketHeader)) return 0;
  PacketHeader h;
  std::memcpy(&h, data, sizeof(h));
  if (h.magic != kMagic) return 0;
  return h.type;
}

// Copies a C string into a fixed field, always terminated.
template <size_t N>
inline void put_str(char (&dst)[N], const char* src) {
  std::memset(dst, 0, N);
  if (src) std::strncpy(dst, src, N - 1);
}

}  // namespace tmnp
