#include "server.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

#include "../core/protocol.h"

namespace iris {

void Server::say(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log) log(buf);
}

bool Server::start(const ServerConfig& cfg, std::string* error) {
    cfg_ = cfg;
    if (cfg_.max_players < 1 || cfg_.max_players > IRIS_MAX_PLAYERS) cfg_.max_players = IRIS_MAX_PLAYERS;
    if (!sock_.open(cfg_.port, true, cfg_.bind.c_str())) {
        if (error) *error = "can't open UDP port " + std::to_string(cfg_.port) + " (already in use?)";
        return false;
    }
    uint32_t seed = cfg_.seed ? cfg_.seed : (uint32_t)std::time(nullptr) * 2654435761u;
    sim = GameSim(seed);
    sim.multiplayer = true;
    sim.start_level(cfg_.level, seed);
    for (auto& c : clients_) c = Client{};
    start_ = clock::now();
    next_tick_ = start_;
    next_snap_ = start_;
    tick_ = 0;
    running_ = true;
    say("listening on UDP %u | %s | seed %u", (unsigned)cfg_.port, level_label(cfg_.level), seed);
    return true;
}

void Server::stop() {
    if (!running_) return;
    running_ = false;
    for (auto& c : clients_) {
        if (!c.used) continue;
        Writer w;
        w.header(PKT_BYE);
        sock_.send(c.addr, w.buf, w.n);
    }
    sock_.close();
}

int Server::player_count() const {
    int n = 0;
    for (const auto& c : clients_) n += c.used;
    return n;
}

void Server::handle(const NetAddr& from, const uint8_t* data, int len) {
    // status query (also answered for LAN browsing and `iris_server --query`)
    if (len == 5 && std::memcmp(data, "IRIS?", 5) == 0) {
        char reply[400];
        int n = std::snprintf(reply, sizeof reply,
                              "IRIS! name=\"%s\" version=%s protocol=%d players=%d/%d level=\"%s\" seed=%u uptime=%ds",
                              cfg_.name.c_str(), IRIS_VERSION, IRIS_PROTOCOL_VERSION, player_count(), cfg_.max_players,
                              level_label(sim.world.id()), sim.world.seed(), (int)now());
        sock_.send(from, reply, n);
        return;
    }
    Reader r(data, len);
    uint8_t type = r.header();
    if (!type) return;

    Client* cl = nullptr;
    for (auto& c : clients_)
        if (c.used && c.addr == from) cl = &c;

    if (type == PKT_HELLO) {
        uint16_t proto = r.u16();
        std::string name = r.str();
        Writer w;
        if (proto != IRIS_PROTOCOL_VERSION) {
            w.header(PKT_REJECT);
            w.str("VERSION MISMATCH - SERVER IS " IRIS_VERSION);
            sock_.send(from, w.buf, w.n);
            return;
        }
        if (!cl) {
            if (player_count() >= cfg_.max_players) {
                w.header(PKT_REJECT);
                w.str("SERVER IS FULL");
                sock_.send(from, w.buf, w.n);
                return;
            }
            for (auto& c : clients_)
                if (!c.used) { cl = &c; break; }
            *cl = Client{};
            cl->used = true;
            cl->addr = from;
            cl->pid = sim.add_player(name);
            sim.event(sim.players[cl->pid].name + " JOINED");
            say("%s joined from %s", sim.players[cl->pid].name.c_str(), net_addr_str(from).c_str());
        }
        cl->last_heard = now();
        w.header(PKT_WELCOME);
        w.u16(IRIS_PROTOCOL_VERSION);
        w.u8((uint8_t)cl->pid);
        sock_.send(from, w.buf, w.n);
        return;
    }
    if (!cl) return;
    cl->last_heard = now();
    if (type == PKT_INPUT) {
        int count = r.u8();
        for (int k = 0; k < count && !r.bad; k++) {
            uint32_t seq;
            InputCmd cmd = read_cmd(r, seq);
            if (r.bad) break;
            if (seq <= cl->last_seq) continue;
            bool dup = false;
            for (auto& q : cl->queue) dup |= q.seq == seq;
            if (dup) continue;
            // keep the queue sorted by sequence number
            auto it = cl->queue.begin();
            while (it != cl->queue.end() && it->seq < seq) ++it;
            cl->queue.insert(it, {seq, cmd});
        }
    } else if (type == PKT_BYE) {
        say("%s left", sim.players[cl->pid].name.c_str());
        sim.event(sim.players[cl->pid].name + " LEFT");
        sim.remove_player(cl->pid);
        *cl = Client{};
    }
}

void Server::send_snapshot(Client& c) {
    const GameSim& s = sim;
    Writer w;
    w.header(PKT_SNAPSHOT);
    w.u32(tick_);
    w.u8((uint8_t)s.world.id());
    w.u32(s.world.seed());
    w.u32(s.epoch);
    w.f32(s.level_time);
    w.u8(s.escaped);
    w.u8((uint8_t)c.pid);
    w.u32(c.last_seq);
    int np = 0;
    for (const auto& p : s.players) np += p.active;
    w.u8((uint8_t)np);
    for (int i = 0; i < IRIS_MAX_PLAYERS; i++) {
        if (!s.players[i].active) continue;
        w.u8((uint8_t)i);
        w.str(s.players[i].name);
        w.f32(s.players[i].respawn_t);
        write_player(w, s.players[i].st);
    }
    // the entities nearest this player
    Vec2 me{s.players[c.pid].st.pos.x, s.players[c.pid].st.pos.z};
    int ne = 0;
    const Entity* close_ents[16];
    for (const Entity& e : s.ents) {
        if ((e.pos - me).len() > 60.0f || ne >= 16) continue;
        close_ents[ne++] = &e;
    }
    w.u8((uint8_t)ne);
    for (int k = 0; k < ne; k++) {
        const Entity& e = *close_ents[k];
        w.u16(e.id);
        w.u8((uint8_t)e.type);
        w.u8((uint8_t)e.state);
        w.f32(e.pos.x);
        w.f32(e.pos.z);
        w.f32(e.yaw);
        w.f32(e.anim);
        w.u8((uint8_t)std::min(255.0f, e.speed * 20.0f));
    }
    // props near this player (dropped items, burning firesalt)
    int np2 = 0;
    const Prop* pr[16];
    for (const Prop& p : s.props) {
        if ((p.pos - me).len() > 50.0f || np2 >= 16) continue;
        pr[np2++] = &p;
    }
    w.u8((uint8_t)np2);
    for (int k = 0; k < np2; k++) {
        w.u16(pr[k]->id);
        w.u8((uint8_t)pr[k]->kind);
        w.u8((uint8_t)pr[k]->item);
        w.f32(pr[k]->pos.x);
        w.f32(pr[k]->pos.z);
        w.f32(pr[k]->life);
    }
    // picked-up level items: always the newest few (so nobody sees an item that's gone),
    // plus a rolling window over the rest so late joiners learn all of them
    uint32_t total = (uint32_t)s.picked_order.size();
    uint32_t newest = std::min<uint32_t>(total, 8);
    uint32_t window = total > newest ? std::min<uint32_t>(total - newest, 12) : 0;
    w.u8((uint8_t)(newest + window));
    for (uint32_t j = 0; j < newest; j++) w.u32(s.picked_order[total - 1 - j]);
    uint32_t older = total - newest;
    for (uint32_t j = 0; j < window; j++) w.u32(s.picked_order[(c.picked_cursor + j) % older]);
    if (older) c.picked_cursor = (c.picked_cursor + window) % older;
    // this player's own message
    w.u32(s.players[c.pid].toast_seq);
    w.str(s.players[c.pid].toast);
    // newest events (the client drops ones it has already shown)
    int nev = (int)std::min<size_t>(s.events.size(), 3);
    w.u8((uint8_t)nev);
    for (int j = (int)s.events.size() - nev; j < (int)s.events.size(); j++) {
        w.u32(s.events[(size_t)j].seq);
        w.str(s.events[(size_t)j].text);
    }
    if (!w.overflow) sock_.send(c.addr, w.buf, w.n);
}

void Server::poll() {
    if (!running_) return;
    uint8_t buf[MAX_PACKET];
    NetAddr from;
    int n;
    while ((n = sock_.recv(from, buf, sizeof buf)) >= 0) handle(from, buf, n);

    // drop clients that went quiet
    for (auto& c : clients_) {
        if (c.used && now() - c.last_heard > 10.0) {
            say("%s timed out", sim.players[c.pid].name.c_str());
            sim.event(sim.players[c.pid].name + " LOST CONNECTION");
            sim.remove_player(c.pid);
            c = Client{};
        }
    }

    const auto dt = std::chrono::microseconds(1000000 / (int)TICK_RATE);
    auto t = clock::now();
    int steps = 0;
    while (t >= next_tick_ && steps < 8) {
        for (auto& c : clients_) {
            if (!c.used) continue;
            // one command per tick; drain faster if the client got ahead (lag spikes)
            int take = c.queue.size() > 6 ? 2 : 1;
            for (int k = 0; k < take; k++) {
                if (!c.queue.empty()) {
                    c.held = c.queue.front().cmd;
                    c.last_seq = c.queue.front().seq;
                    c.queue.pop_front();
                    sim.apply_input(c.pid, c.held);
                }
            }
        }
        sim.tick(1.0f / TICK_RATE);
        next_tick_ += dt;
        tick_++;
        steps++;
    }
    if (t - next_tick_ > std::chrono::seconds(1)) next_tick_ = t;
    if (t >= next_snap_) {
        for (auto& c : clients_)
            if (c.used) send_snapshot(c);
        next_snap_ += std::chrono::microseconds(1000000 / cfg_.snapshot_rate);
        if (t - next_snap_ > std::chrono::seconds(1)) next_snap_ = t;
    }
}

} // namespace iris
