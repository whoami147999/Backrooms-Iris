#include "server.h"

#include <algorithm>
#include <vector>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <random>

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

uint32_t Server::cookie_for(const NetAddr& a) const {
    return mix32(secret_ ^ mix32(a.ip ^ mix32(a.port * 0x9e3779b9u + 0x7f4a7c15u))) | 1u; // never 0 (0 = "no cookie yet")
}

bool Server::start(const ServerConfig& cfg, std::string* error) {
    cfg_ = cfg;
    cfg_.name = clean_name(cfg_.name);
    cfg_.password = clean_password(cfg_.password);
    {
        std::random_device rd;
        secret_ = mix32(rd() ^ (uint32_t)std::time(nullptr) ^ (uint32_t)(uintptr_t)this);
    }
    for (auto& s : strikes_) s = Strike{};
    if (cfg_.max_players < 1 || cfg_.max_players > IRIS_MAX_PLAYERS) cfg_.max_players = IRIS_MAX_PLAYERS;
    if (!sock_.open(cfg_.port, true, cfg_.bind.c_str())) {
        if (error) *error = "can't open UDP port " + std::to_string(cfg_.port) + " (already in use?)";
        return false;
    }
    uint32_t seed = cfg_.seed ? cfg_.seed : (uint32_t)std::time(nullptr) * 2654435761u;
    sim = GameSim(seed);
    sim.multiplayer = true;
    sim.difficulty = cfg_.difficulty;
    sim.start_level(cfg_.lobby ? LevelId::Hub : cfg_.level, seed);
    for (auto& c : clients_) c = Client{};
    start_ = clock::now();
    next_tick_ = start_;
    next_snap_ = start_;
    tick_ = 0;
    running_ = true;
    say("listening on UDP %u | %s | %s | seed %u", (unsigned)cfg_.port, level_label(cfg_.level), diff_info(cfg_.difficulty).name, seed);
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
    // status query for the Join list. Only answered when padded to QUERY_SIZE, and the answer
    // is smaller than the question, so nobody can use a host to amplify traffic at someone.
    if (len >= 5 && std::memcmp(data, "IRIS?", 5) == 0) {
        if (len < QUERY_SIZE) return;
        char reply[QUERY_SIZE];
        int n = std::snprintf(reply, sizeof reply,
                              "IRIS! name=\"%s\" version=%s protocol=%d players=%d/%d level=\"%s\" difficulty=%s password=%d",
                              cfg_.name.c_str(), IRIS_VERSION, IRIS_PROTOCOL_VERSION, player_count(), cfg_.max_players,
                              level_label(sim.world.id()), diff_info(sim.difficulty).name, cfg_.password.empty() ? 0 : 1);
        if (n > 0) sock_.send(from, reply, std::min(n, (int)sizeof reply - 1));
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
        uint32_t cookie = r.u32();
        uint32_t proof = r.u32();
        Writer w;
        if (proto != IRIS_PROTOCOL_VERSION) {
            w.header(PKT_REJECT);
            w.str("OTHER VERSION (" IRIS_VERSION ")");
            sock_.send(from, w.buf, w.n);
            return;
        }
        if (!cl) {
            const double t = now();
            for (const auto& s : strikes_)
                if (s.ip == from.ip && t < s.until) return; // just guessed wrong: ignore for a moment
            // step 1: prove you can receive at this address
            const uint32_t expect = cookie_for(from);
            if (r.bad || cookie != expect) {
                w.header(PKT_CHALLENGE);
                w.u32(expect);
                w.u8(cfg_.password.empty() ? 0 : 1);
                sock_.send(from, w.buf, w.n);
                return;
            }
            // step 2: the password, if there is one
            if (!cfg_.password.empty() && proof != password_proof(cfg_.password, expect)) {
                Strike* slot = &strikes_[0];
                for (auto& s : strikes_) if (s.until < slot->until) slot = &s;
                *slot = {from.ip, t + 1.5};
                w.header(PKT_REJECT);
                w.str("WRONG PASSWORD");
                sock_.send(from, w.buf, w.n);
                say("wrong password from %s", net_addr_str(from).c_str());
                return;
            }
            if (player_count() >= cfg_.max_players) {
                w.header(PKT_REJECT);
                w.str("GAME IS FULL");
                sock_.send(from, w.buf, w.n);
                return;
            }
            for (auto& c : clients_)
                if (!c.used) { cl = &c; break; }
            *cl = Client{};
            cl->used = true;
            cl->addr = from;
            cl->pid = sim.add_player(clean_name(name));
            if (cl->pid < 0) { *cl = Client{}; return; }
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
            if (seq <= cl->last_seq || seq > cl->last_seq + 600 || cl->queue.size() >= 64) continue; // stale / bogus / flooding
            bool dup = false;
            for (auto& q : cl->queue) dup |= q.seq == seq;
            if (dup) continue;
            // keep the queue sorted by sequence number
            auto it = cl->queue.begin();
            while (it != cl->queue.end() && it->seq < seq) ++it;
            cl->queue.insert(it, {seq, cmd});
        }
    } else if (type == PKT_VOICE) {
        // 1.3 proximity chat: pass the frame on to whoever is close enough to hear it. Frames are
        // checked for size and rate (50 a second is real time) and never stored.
        double t = now();
        cl->voice_tokens = std::min(10.0f, cl->voice_tokens + (float)(t - cl->voice_t) * 60.0f);
        cl->voice_t = t;
        if (cl->voice_tokens < 1) return;
        cl->voice_tokens -= 1;
        uint16_t seq = r.u16();
        int16_t pred = (int16_t)r.u16();
        uint8_t index = r.u8();
        if (r.bad || index > 88 || !r.need(VOICE_BYTES)) return;
        const uint8_t* payload = r.p + r.i;
        const SimPlayer& sp = sim.players[cl->pid];
        if (!sp.active) return;
        Vec2 from{sp.st.pos.x, sp.st.pos.z};
        bool radio = sp.st.alive && sp.st.has(ItemType::Radio);
        for (auto& c : clients_) {
            if (!c.used || &c == cl || c.pid < 0) continue;
            const SimPlayer& lp = sim.players[c.pid];
            if (!lp.active) continue;
            // the dead hear each other (and nobody else hears them); the living hear the living near them
            if (sp.st.alive != lp.st.alive) continue;
            float d = (Vec2{lp.st.pos.x, lp.st.pos.z} - from).len();
            bool by_radio = radio && lp.st.has(ItemType::Radio) && d > VOICE_RANGE * 0.5f;
            if (d > VOICE_RANGE && !by_radio && sp.st.alive) continue;
            Writer w;
            w.header(PKT_VOICE);
            w.u8((uint8_t)cl->pid);
            w.u8(by_radio ? 1 : 0);
            w.u16(seq);
            w.u16((uint16_t)pred);
            w.u8(index);
            w.bytes(payload, VOICE_BYTES);
            sock_.send(c.addr, w.buf, w.n);
        }
    } else if (type == PKT_SANDBOX) {
        uint8_t action = r.u8(), arg = r.u8();
        float yaw = finite_or(r.f32());
        if (!r.bad && sim.sandbox) sim.sandbox_cmd(cl->pid, action, arg, yaw);
    } else if (type == PKT_HOSTCMD) {
        uint32_t token = r.u32();
        uint8_t level = r.u8(), diff = r.u8(), sb = r.u8();
        if (r.bad || cfg_.host_token == 0 || token != cfg_.host_token) return; // only the host's own game knows the token
        if (level >= (uint8_t)LevelId::COUNT || diff >= (uint8_t)Difficulty::COUNT) return;
        say("host starts %s", level_label((LevelId)level));
        sim.start_run((LevelId)level, (Difficulty)diff, sb != 0, mix32(secret_ ^ (uint32_t)tick_) | 1u);
    } else if (type == PKT_BYE) {
        say("%s left", sim.players[cl->pid].name.c_str());
        sim.event(sim.players[cl->pid].name + " LEFT");
        sim.remove_player(cl->pid);
        *cl = Client{};
    }
}

void Server::send_snapshot(Client& c) {
    // full detail first; if that doesn't fit in one packet, again with fewer entities/props
    if (!send_snapshot_sized(c, 16, 24, 3)) send_snapshot_sized(c, 8, 10, 1);
}

bool Server::send_snapshot_sized(Client& c, int max_ents, int max_props, int max_events) {
    const GameSim& s = sim;
    Writer w;
    w.header(PKT_SNAPSHOT);
    w.u32(tick_);
    w.u8((uint8_t)s.world.id());
    w.u32(s.world.seed());
    w.u8((uint8_t)s.world.difficulty());
    w.u32(s.epoch);
    w.f32(s.level_time);
    w.u8(s.escaped);
    w.u8((uint8_t)s.objective.type);
    w.u8(s.objective.have);
    w.u8(s.objective.need);
    w.u8(s.objective.done);
    w.u8((uint8_t)s.objective.reward);
    w.u8(s.objective.required);
    w.u8((uint8_t)((s.sandbox ? 1 : 0) | (s.god ? 2 : 0) | (s.auto_spawn ? 4 : 0) | (s.freeze_ents ? 8 : 0)));
    w.f32(s.won_t);
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
        if ((e.pos - me).len() > 60.0f || ne >= max_ents) continue;
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
        w.u16((uint16_t)(int)std::lround(std::remainder(e.yaw, 2 * PI) / PI * 32767.0f));
        w.f32(e.anim);
        w.u8((uint8_t)std::min(255.0f, e.speed * 20.0f));
    }
    // props near this player: the ones that matter first (items, objective things, fire), then the
    // nearest of the rest (chalk marks), so the set doesn't flicker from one snapshot to the next
    int np2 = 0;
    const Prop* pr[24];
    {
        std::vector<std::pair<float, const Prop*>> cand;
        for (const Prop& p : s.props) {
            float d = (p.pos - me).len();
            if (d > 50.0f) continue;
            float pri = p.kind == PropKind::Mark ? 1000.0f : 0.0f;
            cand.push_back({pri + d, &p});
        }
        std::sort(cand.begin(), cand.end(), [](const std::pair<float, const Prop*>& a, const std::pair<float, const Prop*>& b) { return a.first < b.first; });
        for (auto& c : cand) { if (np2 >= max_props) break; pr[np2++] = c.second; }
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
    // 1.3: doors that aren't the way they started, and furniture already searched, near this player
    {
        std::vector<uint32_t> near_doors, near_searched;
        int pcx = floor_to_int(me.x / CELL), pcz = floor_to_int(me.z / CELL);
        for (int cz = pcz - 10; cz <= pcz + 10 && near_doors.size() < 24; cz++)
            for (int cx = pcx - 10; cx <= pcx + 10 && near_doors.size() < 24; cx++) {
                Furn fs[MAX_FURN];
                bool owner = false;
                int n = s.world.furniture(cx, cz, fs, &owner);
                if (!owner) continue;
                for (int k = 0; k < n; k++) {
                    if (fs[k].kind == FurnKind::DoorLeaf && fs[k].width > 0) {
                        uint32_t id = World::door_id(fs[k]);
                        if (s.door_flip.count(id) && near_doors.size() < 24) near_doors.push_back(id);
                    } else if (World::searchable(fs[k].kind)) {
                        uint32_t id = World::furn_id(fs[k]);
                        if (s.searched.count(id) && near_searched.size() < 24) near_searched.push_back(id);
                    }
                }
            }
        w.u8((uint8_t)near_doors.size());
        for (uint32_t id : near_doors) w.u32(id);
        w.u8((uint8_t)near_searched.size());
        for (uint32_t id : near_searched) w.u32(id);
    }
    // this player's own message
    w.u32(s.players[c.pid].toast_seq);
    w.str(s.players[c.pid].toast);
    // newest events (the client drops ones it has already shown)
    int nev = (int)std::min<size_t>(s.events.size(), (size_t)max_events);
    w.u8((uint8_t)nev);
    for (int j = (int)s.events.size() - nev; j < (int)s.events.size(); j++) {
        w.u32(s.events[(size_t)j].seq);
        w.str(s.events[(size_t)j].text);
    }
    if (w.overflow) return false;
    sock_.send(c.addr, w.buf, w.n);
    return true;
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
        if (sim.escaped && sim.won_t > 14.0f) { // someone made it out: everyone back to the lobby
            sim.event("BACK TO THE M.E.G. BASE");
            sim.start_run(LevelId::Hub, sim.difficulty, false, mix32(secret_ ^ (uint32_t)tick_) | 1u);
        }
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
