// Backrooms Iris - a running game, seen from the client. Single player runs the GameSim
// locally; multiplayer predicts the local player and renders server snapshots. The rest
// of the client (renderer, HUD, effects) only ever looks at a GameView.
#pragma once
#include "../core/voice.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../core/sim.h"
#include "renderer.h"

namespace iris {

struct GameView {
    World world;
    uint32_t epoch = 0;
    PlayerState me, me_prev;         // render me_prev -> me with `alpha`
    float alpha = 0;
    std::vector<DrawPlayer> others;
    std::vector<DrawEntity> ents;
    std::vector<DrawProp> props;
    const std::unordered_set<uint32_t>* picked = nullptr;
    const std::unordered_set<uint32_t>* searched = nullptr; // 1.3: furniture already gone through (drawers left open)
    float level_time = 0;
    bool escaped = false;
    float won_t = 0;
    bool sandbox = false, god = false, auto_spawn = true, freeze = false; // 1.2 (multiplayer sandbox)
    bool host = false;           // this machine is hosting (it picks levels in the lobby)
    Objective objective;
    float respawn_t = 0;
    bool multiplayer = false;
    int my_id = 0;
    float ping_ms = 0;
    std::vector<std::string> roster;
    std::string toast;           // your own last message ("PICKED UP ...")
    uint32_t toast_seq = 0;
};

class Session {
public:
    virtual ~Session() = default;
    // Advance by frame_dt using this frame's command (look angles already applied).
    virtual void update(const InputCmd& cmd, double now, float frame_dt, bool paused) = 0;
    virtual const GameView& view() const = 0;
    virtual std::vector<std::string> take_events() = 0;
    virtual bool failed(std::string* why) const { (void)why; return false; }
    virtual bool ready() const { return true; }        // multiplayer: connected + first snapshot
    virtual void request_respawn() {}                  // single player: retry after death
    virtual void clear_escaped() {}
    virtual GameSim* sim() { return nullptr; }  // single player / sandbox only (sandbox tools, dev hooks)
    // 1.2: sandbox actions and the lobby work the same in single player and multiplayer
    virtual void sandbox(uint8_t action, uint8_t arg, float yaw) { (void)action; (void)arg; (void)yaw; }
    virtual void host_start(LevelId level, Difficulty d, bool sandbox_mode) { (void)level; (void)d; (void)sandbox_mode; }
    virtual void resync() {}                    // after poking the sim directly
    // 1.3 proximity chat (multiplayer only)
    virtual void send_voice(uint16_t seq, AdpcmState st, const uint8_t* payload) { (void)seq; (void)st; (void)payload; }
    virtual void take_voice(std::vector<VoiceFrame>& out) { out.clear(); }
};

std::unique_ptr<Session> make_single_player(LevelId level, uint32_t seed, const std::string& name, bool sandbox = false,
                                            Difficulty diff = Difficulty::Medium);
#if IRIS_MULTIPLAYER
// start_server = true hosts the game on this machine (own thread) and joins it; friends connect
// straight to you (LAN, Tailscale or a forwarded UDP port). No dedicated server involved.
std::unique_ptr<Session> make_multiplayer(const std::string& host, uint16_t port, const std::string& name, const std::string& password,
                                          bool start_server, LevelId level, Difficulty diff, double now, std::string* error, bool lobby = false);
#endif

} // namespace iris
