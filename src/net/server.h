// Backrooms Iris - authoritative game server. Runs the GameSim, takes player inputs over
// UDP, sends snapshots back. Used by the dedicated server (FreeBSD) and by "Host game"
// inside the client (on its own thread).
#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>

#include "../core/sim.h"
#include "udp.h"

namespace iris {

struct ServerConfig {
    std::string name = "Backrooms Iris server";
    std::string bind = "0.0.0.0";
    uint16_t port = IRIS_DEFAULT_PORT;
    int max_players = IRIS_MAX_PLAYERS;
    LevelId level = LevelId::L0;
    Difficulty difficulty = Difficulty::Medium;
    uint32_t seed = 0;       // 0 = random
    int snapshot_rate = 20;  // Hz
};

class Server {
public:
    GameSim sim;
    bool start(const ServerConfig& cfg, std::string* error = nullptr);
    void stop();
    // Handle network traffic and run however many 60 Hz ticks are due. Call often.
    void poll();
    int player_count() const;
    const ServerConfig& config() const { return cfg_; }
    void (*log)(const char* line) = nullptr;

private:
    struct PendingCmd { uint32_t seq; InputCmd cmd; };
    struct Client {
        bool used = false;
        NetAddr addr;
        int pid = -1;
        uint32_t last_seq = 0;     // newest command applied
        double last_heard = 0;
        std::deque<PendingCmd> queue;
        InputCmd held;             // repeated when the client's packets are late
        uint32_t picked_cursor = 0;
    };
    using clock = std::chrono::steady_clock;
    ServerConfig cfg_;
    UdpSocket sock_;
    Client clients_[IRIS_MAX_PLAYERS];
    clock::time_point start_, next_tick_, next_snap_;
    uint32_t tick_ = 0;
    bool running_ = false;

    double now() const { return std::chrono::duration<double>(clock::now() - start_).count(); }
    void handle(const NetAddr& from, const uint8_t* data, int len);
    void send_snapshot(Client& c);
    bool send_snapshot_sized(Client& c, int max_ents, int max_props, int max_events);
    void say(const char* fmt, ...);
};

} // namespace iris
