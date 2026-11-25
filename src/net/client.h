// Backrooms Iris - client side of the network game: connect, send inputs, receive snapshots,
// browse the LAN for servers.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "../core/sim.h"
#include "udp.h"

namespace iris {

struct EntSnap {
    uint16_t id;
    EntityType type;
    EntState state;
    Vec2 pos;
    float yaw, anim, speed;
};

struct PropSnap {
    uint16_t id;
    PropKind kind;
    ItemType item;
    Vec2 pos;
    float life;
};

struct Snapshot {
    uint32_t tick = 0;
    LevelId level = LevelId::L0;
    uint32_t seed = 0, epoch = 0;
    float level_time = 0;
    bool escaped = false;
    int my_id = 0;
    uint32_t ack = 0;
    struct P { bool active = false; std::string name; PlayerState st; float respawn_t = 0; } players[IRIS_MAX_PLAYERS];
    std::vector<EntSnap> ents;
    std::vector<PropSnap> props;
    uint32_t toast_seq = 0;
    std::string toast;
};

struct FoundServer {
    NetAddr addr;
    std::string name, level;
    int players = 0, max_players = 0, protocol = 0;
    double seen = 0;
};

class NetClient {
public:
    enum class State { Idle, Connecting, Connected, Failed };
    State state = State::Idle;
    std::string error;
    int my_id = -1;

    Snapshot snap, prev_snap;    // newest two snapshots
    double snap_time = 0, prev_snap_time = 0;
    bool got_snapshot = false;
    uint32_t snap_count = 0;
    std::unordered_set<uint32_t> picked;
    uint32_t picked_epoch = 0;
    std::vector<SimEvent> new_events;   // events not shown yet (the game drains this)
    uint32_t last_event = 0;
    float ping_ms = 0;

    bool connect(const std::string& host, uint16_t port, const std::string& name, double now);
    void disconnect();
    void poll(double now);
    void send_input(uint32_t seq, const InputCmd* cmds, const uint32_t* seqs, int count);

    // LAN browser
    std::vector<FoundServer> found;
    void scan(double now);           // send a broadcast query (call about once a second)
    void scan_poll(double now);
    void scan_stop() { scan_sock_.close(); }

private:
    UdpSocket sock_, scan_sock_;
    NetAddr server_;
    std::string name_;
    double hello_t_ = 0, connect_start_ = 0, last_heard_ = 0;
    void handle(const uint8_t* data, int len, double now);
};

bool parse_server_info(const char* text, FoundServer& out);

} // namespace iris
