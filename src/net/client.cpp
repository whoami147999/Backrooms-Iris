#include "client.h"

#include <cstdlib>
#include <cstring>

#include "../core/protocol.h"

namespace iris {

bool parse_server_info(const char* t, FoundServer& out) {
    if (std::strncmp(t, "IRIS!", 5) != 0) return false;
    auto field = [&](const char* key) -> std::string {
        const char* p = std::strstr(t, key);
        if (!p) return "";
        p += std::strlen(key);
        if (*p == '"') {
            const char* e = std::strchr(p + 1, '"');
            return e ? std::string(p + 1, e) : "";
        }
        const char* e = p;
        while (*e && *e != ' ') e++;
        return std::string(p, e);
    };
    out.name = field("name=");
    out.level = field("level=");
    out.protocol = std::atoi(field("protocol=").c_str());
    std::string pl = field("players=");
    out.players = std::atoi(pl.c_str());
    size_t slash = pl.find('/');
    out.max_players = slash == std::string::npos ? 0 : std::atoi(pl.c_str() + slash + 1);
    return true;
}

bool NetClient::connect(const std::string& host, uint16_t port, const std::string& name, double now) {
    disconnect();
    error.clear();
    if (!net_resolve(host, port, server_)) {
        state = State::Failed;
        error = "CAN'T FIND " + host;
        return false;
    }
    if (!sock_.open(0)) {
        state = State::Failed;
        error = "CAN'T OPEN A SOCKET";
        return false;
    }
    name_ = name;
    state = State::Connecting;
    connect_start_ = now;
    hello_t_ = 0;
    got_snapshot = false;
    snap_count = 0;
    picked.clear();
    picked_epoch = 0;
    last_event = 0;
    my_id = -1;
    return true;
}

void NetClient::disconnect() {
    if (sock_.is_open() && state == State::Connected) {
        Writer w;
        w.header(PKT_BYE);
        for (int i = 0; i < 3; i++) sock_.send(server_, w.buf, w.n);
    }
    sock_.close();
    state = State::Idle;
}

void NetClient::send_input(uint32_t seq, const InputCmd* cmds, const uint32_t* seqs, int count) {
    (void)seq;
    if (state != State::Connected) return;
    Writer w;
    w.header(PKT_INPUT);
    w.u8((uint8_t)count);
    for (int i = 0; i < count; i++) write_cmd(w, seqs[i], cmds[i]);
    sock_.send(server_, w.buf, w.n);
}

void NetClient::handle(const uint8_t* data, int len, double now) {
    Reader r(data, len);
    uint8_t type = r.header();
    if (!type) return;
    last_heard_ = now;
    if (type == PKT_WELCOME) {
        r.u16();
        my_id = r.u8();
        if (state == State::Connecting) state = State::Connected;
        ping_ms = (float)((now - hello_t_) * 1000.0);
    } else if (type == PKT_REJECT) {
        error = r.str();
        state = State::Failed;
    } else if (type == PKT_BYE) {
        error = "THE SERVER SHUT DOWN";
        state = State::Failed;
    } else if (type == PKT_SNAPSHOT && state == State::Connected) {
        Snapshot s;
        s.tick = r.u32();
        if (got_snapshot && s.tick <= snap.tick) return; // late / duplicate
        s.level = (LevelId)r.u8();
        s.seed = r.u32();
        s.epoch = r.u32();
        s.level_time = r.f32();
        s.escaped = r.u8() != 0;
        s.my_id = r.u8();
        s.ack = r.u32();
        int np = r.u8();
        for (int k = 0; k < np && !r.bad; k++) {
            int id = r.u8();
            if (id >= IRIS_MAX_PLAYERS) { r.bad = true; break; }
            auto& p = s.players[id];
            p.active = true;
            p.name = r.str();
            p.respawn_t = r.f32();
            read_player(r, p.st);
        }
        int ne = r.u8();
        for (int k = 0; k < ne && !r.bad; k++) {
            EntSnap e;
            e.id = r.u16();
            e.type = (EntityType)r.u8();
            e.state = (EntState)r.u8();
            e.pos.x = r.f32();
            e.pos.z = r.f32();
            e.yaw = r.f32();
            e.anim = r.f32();
            e.speed = r.u8() / 20.0f;
            if ((int)e.type < (int)EntityType::COUNT) s.ents.push_back(e);
        }
        int npr = r.u8();
        for (int k = 0; k < npr && !r.bad; k++) {
            PropSnap p;
            p.id = r.u16();
            p.kind = (PropKind)r.u8();
            p.item = (ItemType)r.u8();
            p.pos.x = r.f32();
            p.pos.z = r.f32();
            p.life = r.f32();
            if ((int)p.item < ITEM_COUNT) s.props.push_back(p);
        }
        if (s.epoch != picked_epoch) { picked.clear(); picked_epoch = s.epoch; }
        int k = r.u8();
        for (int j = 0; j < k && !r.bad; j++) picked.insert(r.u32());
        s.toast_seq = r.u32();
        s.toast = r.str();
        int nev = r.u8();
        for (int j = 0; j < nev && !r.bad; j++) {
            SimEvent ev;
            ev.seq = r.u32();
            ev.text = r.str();
            if (ev.seq > last_event) { last_event = ev.seq; new_events.push_back(ev); }
        }
        if (r.bad || (int)s.level >= (int)LevelId::COUNT) return;
        prev_snap = got_snapshot ? snap : s;
        prev_snap_time = got_snapshot ? snap_time : now;
        snap = s;
        snap_time = now;
        got_snapshot = true;
        snap_count++;
    }
}

void NetClient::poll(double now) {
    if (state == State::Idle || state == State::Failed) return;
    if (state == State::Connecting) {
        if (now - hello_t_ > 0.5) { // keep knocking until the server answers
            Writer w;
            w.header(PKT_HELLO);
            w.u16(IRIS_PROTOCOL_VERSION);
            w.str(name_);
            sock_.send(server_, w.buf, w.n);
            hello_t_ = now;
        }
        if (now - connect_start_ > 6.0) {
            state = State::Failed;
            error = "NO ANSWER FROM " + net_addr_str(server_);
            sock_.close();
            return;
        }
    }
    uint8_t buf[MAX_PACKET];
    NetAddr from;
    int n;
    while ((n = sock_.recv(from, buf, sizeof buf)) >= 0)
        if (from == server_) handle(buf, n, now);
    if (state == State::Connected && now - last_heard_ > 8.0) {
        state = State::Failed;
        error = "LOST CONNECTION TO THE SERVER";
    }
}

void NetClient::scan(double now) {
    if (!scan_sock_.is_open() && !scan_sock_.open(0, true)) return;
    const char q[] = "IRIS?";
    scan_sock_.send(net_broadcast(IRIS_DEFAULT_PORT), q, 5);
    NetAddr local{0x7F000001u, IRIS_DEFAULT_PORT}; // also find a server on this machine
    scan_sock_.send(local, q, 5);
    // forget servers that stopped answering
    for (size_t i = 0; i < found.size();)
        if (now - found[i].seen > 4.0) found.erase(found.begin() + (long)i);
        else i++;
}

void NetClient::scan_poll(double now) {
    if (!scan_sock_.is_open()) return;
    char buf[512];
    NetAddr from;
    int n;
    while ((n = scan_sock_.recv(from, buf, sizeof buf - 1)) >= 0) {
        buf[n] = 0;
        FoundServer f;
        if (!parse_server_info(buf, f)) continue;
        f.addr = from;
        f.seen = now;
        bool merged = false;
        for (auto& o : found) {
            // the same server answering on loopback and on the LAN: keep one entry
            if (o.addr == from || (o.name == f.name && o.addr.port == from.port && (from.ip >> 24) == 127)) {
                if ((from.ip >> 24) != 127) o.addr = from;
                o.players = f.players; o.max_players = f.max_players; o.level = f.level; o.seen = now;
                merged = true;
            }
        }
        if (!merged) found.push_back(f);
    }
}

} // namespace iris
