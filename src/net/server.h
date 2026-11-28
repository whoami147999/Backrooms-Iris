// Backrooms Iris - the host's side of a multiplayer game. "Host game" runs this inside the
// game (on its own thread): it runs the GameSim, takes everyone's inputs over UDP and sends
// snapshots back. Friends join over LAN, Tailscale or a forwarded UDP port; there is no
// central or dedicated server.
#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>

#include "../core/sim.h"
#include "udp.h"

namespace iris {

struct ServerConfig {
    std::string name = "Backrooms Iris game";
    std::string password;    // empty: anyone who can reach the port can join
    std::string bind = "0.0.0.0";
    uint16_t port = IRIS_DEFAULT_PORT;
    int max_players = IRIS_MAX_PLAYERS;
    LevelId level = LevelId::L0;
    Difficulty difficulty = Difficulty::Medium;
    uint32_t seed = 0;       // 0 = random
    int snapshot_rate = 20;  // Hz
    uint32_t host_token = 0; // 1.2: the host's own game knows this (in memory); lobby commands must carry it
    bool lobby = false;      // start in the M.E.G. base and let the host pick a level there
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
    struct Strike { uint32_t ip = 0; double until = 0; };   // wrong password: ignored for a bit
    struct Client {
        bool used = false;
        NetAddr addr;
        int pid = -1;
        uint32_t last_seq = 0;     // newest command applied
        double last_heard = 0;
        std::deque<PendingCmd> queue;
        InputCmd held;             // repeated when the client's packets are late
        uint32_t picked_cursor = 0;
        double voice_t = 0;        // 1.3: voice rate limit (a token bucket, in frames)
        float voice_tokens = 10;
    };
    using clock = std::chrono::steady_clock;
    ServerConfig cfg_;
    UdpSocket sock_;
    Client clients_[IRIS_MAX_PLAYERS];
    clock::time_point start_, next_tick_, next_snap_;
    uint32_t tick_ = 0;
    bool running_ = false;
    uint32_t secret_ = 0;          // keys the join cookies (new every time you host)
    Strike strikes_[16];
    uint32_t cookie_for(const NetAddr& a) const;

    double now() const { return std::chrono::duration<double>(clock::now() - start_).count(); }
    void handle(const NetAddr& from, const uint8_t* data, int len);
    void send_snapshot(Client& c);
    bool send_snapshot_sized(Client& c, int max_ents, int max_props, int max_events);
    void say(const char* fmt, ...);
};

} // namespace iris
