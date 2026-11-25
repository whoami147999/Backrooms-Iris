#include "session.h"

#include <chrono>
#include <cmath>
#include <deque>

namespace iris {
namespace {

std::vector<DrawEntity> ents_from_sim(const GameSim& s) {
    std::vector<DrawEntity> out;
    for (const Entity& e : s.ents) out.push_back({e.id, e.type, e.state, e.pos, e.yaw, e.anim, e.speed});
    return out;
}

// ======================================================================== single player
class SingleSession : public Session {
public:
    SingleSession(LevelId level, uint32_t seed, const std::string& name) : sim_(seed) {
        sim_.start_level(level, seed);
        sim_.add_player(name);
        v_.picked = &sim_.picked;
        refresh();
        v_.me_prev = v_.me;
    }
    void update(const InputCmd& cmd, double, float frame_dt, bool paused) override {
        const float dt = 1.0f / TICK_RATE;
        if (paused) { acc_ = 0; v_.me_prev = v_.me; v_.alpha = 1; return; }
        acc_ += frame_dt;
        while (acc_ >= dt) {
            PlayerState before = sim_.players[0].st;
            uint32_t epoch = sim_.epoch;
            sim_.apply_input(0, cmd);
            sim_.tick(dt);
            v_.me_prev = sim_.epoch != epoch ? sim_.players[0].st : before; // no lerp across level changes
            acc_ -= dt;
        }
        refresh();
        v_.alpha = acc_ / dt;
    }
    const GameView& view() const override { return v_; }
    std::vector<std::string> take_events() override {
        std::vector<std::string> out;
        for (const SimEvent& e : sim_.events)
            if (e.seq > last_event_) { out.push_back(e.text); last_event_ = e.seq; }
        return out;
    }
    void request_respawn() override {
        if (!sim_.players[0].st.alive) {
            sim_.respawn(0);
            v_.me_prev = sim_.players[0].st;
        }
    }
    void clear_escaped() override { sim_.escaped = false; }
    GameSim* debug_sim() override { return &sim_; }
    void resync() { refresh(); v_.me_prev = v_.me; }

private:
    GameSim sim_;
    GameView v_;
    float acc_ = 0;
    uint32_t last_event_ = 0;
    void refresh() {
        v_.world = sim_.world;
        v_.epoch = sim_.epoch;
        v_.me = sim_.players[0].st;
        v_.ents = ents_from_sim(sim_);
        v_.level_time = sim_.level_time;
        v_.escaped = sim_.escaped;
        v_.multiplayer = false;
    }
};

// ======================================================================== multiplayer
class HostThread {
public:
    std::string error;
    bool start(const ServerConfig& cfg) {
        if (!server_.start(cfg, &error)) return false;
        run_ = true;
        th_ = std::thread([this] {
            while (run_) {
                server_.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            server_.stop();
        });
        return true;
    }
    ~HostThread() {
        run_ = false;
        if (th_.joinable()) th_.join();
    }
private:
    Server server_;
    std::atomic<bool> run_{false};
    std::thread th_;
};

class NetSession : public Session {
public:
    std::unique_ptr<HostThread> host;

    bool connect(const std::string& addr, uint16_t port, const std::string& name, double now, std::string* err) {
        if (!net_.connect(addr, port, name, now)) { if (err) *err = net_.error; return false; }
        v_.multiplayer = true;
        v_.picked = &net_.picked;
        return true;
    }

    void update(const InputCmd& cmd_in, double now, float frame_dt, bool) override {
        net_.poll(now);
        if (net_.state != NetClient::State::Connected) return;
        const float dt = 1.0f / TICK_RATE;

        if (net_.got_snapshot && net_.snap_count != seen_snaps_) {
            seen_snaps_ = net_.snap_count;
            on_snapshot();
        }
        if (!have_state_) return;

        // local prediction at the same fixed rate the server uses
        acc_ += frame_dt;
        int steps = 0;
        while (acc_ >= dt && steps < 8) {
            InputCmd cmd = cmd_in;
            seq_++;
            history_.push_back({seq_, cmd});
            while (history_.size() > 240) history_.pop_front();
            prev_ = pred_;
            player_tick(pred_, cmd, world_, dt);
            // send the newest few commands every tick: losing a packet costs nothing
            InputCmd cmds[4];
            uint32_t seqs[4];
            int n = 0;
            for (int i = (int)history_.size() - 1; i >= 0 && n < 4; i--, n++) {
                cmds[3 - n] = history_[(size_t)i].cmd;
                seqs[3 - n] = history_[(size_t)i].seq;
            }
            net_.send_input(seq_, cmds + (4 - n), seqs + (4 - n), n);
            acc_ -= dt;
            steps++;
        }
        if (steps == 8) acc_ = 0;

        // remote players + entities: blend between the two newest snapshots
        double span = std::max(0.02, net_.snap_time - net_.prev_snap_time);
        float t = (float)clampf((float)((now - net_.snap_time) / span), 0.0f, 1.0f);
        const Snapshot& a = net_.prev_snap;
        const Snapshot& b = net_.snap;
        v_.others.clear();
        v_.roster.clear();
        for (int i = 0; i < IRIS_MAX_PLAYERS; i++) {
            if (!b.players[i].active) continue;
            v_.roster.push_back(b.players[i].name);
            if (i == net_.my_id) continue;
            const PlayerState& pb = b.players[i].st;
            const PlayerState& pa = a.players[i].active ? a.players[i].st : pb;
            Vec3 pos = pa.pos + (pb.pos - pa.pos) * t;
            float yaw = pa.yaw + std::remainder(pb.yaw - pa.yaw, 2 * PI) * t;
            v_.others.push_back({i, pos, yaw, pb.crouch, lerpf(pa.bob, pb.bob, t), pb.alive, b.players[i].name});
        }
        v_.ents.clear();
        for (const EntSnap& eb : b.ents) {
            EntSnap ea = eb;
            for (const EntSnap& x : a.ents) if (x.id == eb.id) ea = x;
            Vec2 p = ea.pos + (eb.pos - ea.pos) * t;
            float yaw = ea.yaw + std::remainder(eb.yaw - ea.yaw, 2 * PI) * t;
            v_.ents.push_back({eb.id, eb.type, eb.state, p, yaw, lerpf(ea.anim, eb.anim, t), eb.speed});
        }
        v_.me_prev = prev_;
        v_.me = pred_;
        v_.alpha = acc_ / dt;
        v_.ping_ms = net_.ping_ms;
    }

    const GameView& view() const override { return v_; }
    std::vector<std::string> take_events() override {
        std::vector<std::string> out;
        for (auto& e : net_.new_events) out.push_back(e.text);
        net_.new_events.clear();
        return out;
    }
    bool failed(std::string* why) const override {
        if (net_.state != NetClient::State::Failed) return false;
        if (why) *why = net_.error;
        return true;
    }
    bool ready() const override { return net_.state == NetClient::State::Connected && have_state_; }
    ~NetSession() override { net_.disconnect(); }

private:
    struct Hist { uint32_t seq; InputCmd cmd; };
    NetClient net_;
    GameView v_;
    World world_;
    PlayerState pred_, prev_;
    std::deque<Hist> history_;
    uint32_t seq_ = 0, seen_snaps_ = 0, epoch_ = 0;
    float acc_ = 0;
    bool have_state_ = false;

    void on_snapshot() {
        const Snapshot& s = net_.snap;
        if (!s.players[net_.my_id].active) return;
        bool new_level = !have_state_ || s.epoch != epoch_;
        if (new_level) {
            world_ = World(s.level, s.seed);
            epoch_ = s.epoch;
            history_.clear();
        }
        // reconcile: take the server's word for where we were, replay what it hasn't seen yet
        PlayerState server = s.players[net_.my_id].st;
        PlayerState replay = server;
        for (const Hist& h : history_)
            if (h.seq > s.ack) player_tick(replay, h.cmd, world_, 1.0f / TICK_RATE);
        while (!history_.empty() && history_.front().seq <= s.ack) history_.pop_front();
        // smooth small corrections, snap big ones (teleports, respawns, level changes)
        float err = (replay.pos - pred_.pos).len();
        if (!new_level && err < 0.4f && server.alive == pred_.alive) {
            Vec3 keep = pred_.pos + (replay.pos - pred_.pos) * 0.3f;
            replay.pos = keep;
        }
        if (new_level || err > 2.0f) prev_ = replay;
        // our own look direction stays local (no rubber-banding the camera)
        replay.yaw = pred_.yaw;
        replay.pitch = pred_.pitch;
        pred_ = replay;
        have_state_ = true;
        v_.world = world_;
        v_.epoch = s.epoch;
        v_.level_time = s.level_time;
        v_.escaped = s.escaped;
        v_.respawn_t = s.players[net_.my_id].respawn_t;
        v_.my_id = net_.my_id;
    }
};

} // namespace

std::unique_ptr<Session> make_single_player(LevelId level, uint32_t seed, const std::string& name) {
    return std::unique_ptr<Session>(new SingleSession(level, seed, name));
}

std::unique_ptr<Session> make_multiplayer(const std::string& host, uint16_t port, const std::string& name,
                                          bool start_server, LevelId level, double now, std::string* error) {
    std::unique_ptr<NetSession> s(new NetSession());
    if (start_server) {
        s->host.reset(new HostThread());
        ServerConfig cfg;
        cfg.name = name + "'S GAME";
        cfg.port = port;
        cfg.level = level;
        if (!s->host->start(cfg)) {
            if (error) *error = s->host->error;
            return nullptr;
        }
    }
    if (!s->connect(start_server ? "127.0.0.1" : host, port, name, now, error)) return nullptr;
    return std::unique_ptr<Session>(s.release());
}

} // namespace iris
