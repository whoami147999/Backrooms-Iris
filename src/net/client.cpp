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
    out.name = clean_name(field("name="));
    out.locked = field("password=") == "1";
    auto printable = [](std::string s) {
        std::string o;
        for (char c : s) if (c >= ' ' && c < 127 && o.size() < 40) o += c;
        return o;
    };
    out.level = printable(field("level="));
    std::string diff = printable(field("difficulty="));
    if (!diff.empty() && diff.size() < 12) out.level += "  " + diff;
    out.protocol = std::atoi(field("protocol=").c_str());
    std::string pl = field("players=");
    out.players = std::atoi(pl.c_str());
    size_t slash = pl.find('/');
    out.max_players = slash == std::string::npos ? 0 : std::atoi(pl.c_str() + slash + 1);
    return true;
}

bool NetClient::connect(const std::string& host, uint16_t port, const std::string& name, const std::string& password, double now) {
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
    name_ = clean_name(name);
    password_ = clean_password(password);
    cookie_ = 0;
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

void NetClient::send_hello(double now) {
    Writer w;
    w.header(PKT_HELLO);
    w.u16(IRIS_PROTOCOL_VERSION);
    w.str(name_);
    w.u32(cookie_);
    w.u32(cookie_ && !password_.empty() ? password_proof(password_, cookie_) : 0);
    sock_.send(server_, w.buf, w.n);
    hello_t_ = now;
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

void NetClient::send_sandbox(uint8_t action, uint8_t arg, float yaw) {
    if (state != State::Connected) return;
    Writer w;
    w.header(PKT_SANDBOX);
    w.u8(action);
    w.u8(arg);
    w.f32(yaw);
    sock_.send(server_, w.buf, w.n);
}

void NetClient::send_voice(uint16_t seq, AdpcmState state, const uint8_t* payload) {
    if (this->state != State::Connected) return;
    Writer w;
    w.header(PKT_VOICE);
    w.u16(seq);
    w.u16((uint16_t)state.pred);
    w.u8(state.index);
    w.bytes(payload, VOICE_BYTES);
    sock_.send(server_, w.buf, w.n);
}

void NetClient::send_hostcmd(uint32_t token, LevelId level, Difficulty d, bool sandbox) {
    if (state != State::Connected) return;
    Writer w;
    w.header(PKT_HOSTCMD);
    w.u32(token);
    w.u8((uint8_t)level);
    w.u8((uint8_t)d);
    w.u8(sandbox ? 1 : 0);
    for (int i = 0; i < 2; i++) sock_.send(server_, w.buf, w.n); // (twice: it matters)
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
    if (type == PKT_CHALLENGE && state == State::Connecting) {
        cookie_ = r.u32();
        bool locked = (r.u8() & 1) != 0;
        if (r.bad) return;
        if (locked && password_.empty()) {
            error = "THIS GAME HAS A PASSWORD";
            state = State::Failed;
            return;
        }
        send_hello(now);
    } else if (type == PKT_WELCOME) {
        r.u16();
        int id = r.u8();
        if (r.bad || id >= IRIS_MAX_PLAYERS) return;
        my_id = id;
        if (state == State::Connecting) state = State::Connected;
        ping_ms = (float)((now - hello_t_) * 1000.0);
    } else if (type == PKT_REJECT) {
        error = r.str();
        state = State::Failed;
    } else if (type == PKT_BYE) {
        error = "THE HOST LEFT THE GAME";
        state = State::Failed;
    } else if (type == PKT_VOICE && state == State::Connected) {
        VoiceFrame f;
        f.speaker = r.u8();
        f.radio = r.u8() != 0;
        f.seq = r.u16();
        AdpcmState st;
        st.pred = (int16_t)r.u16();
        st.index = r.u8();
        if (r.bad || f.speaker >= IRIS_MAX_PLAYERS || f.speaker == my_id || st.index > 88 || !r.need(VOICE_BYTES)) return;
        adpcm_decode(r.p + r.i, VOICE_FRAME, st, f.pcm);
        if (voice_in.size() < 64) voice_in.push_back(f);
    } else if (type == PKT_SNAPSHOT && state == State::Connected) {
        Snapshot s;
        s.tick = r.u32();
        if (got_snapshot && s.tick <= snap.tick) return; // late / duplicate
        s.level = (LevelId)r.u8();
        s.seed = r.u32();
        s.difficulty = (Difficulty)std::min<int>(r.u8(), (int)Difficulty::COUNT - 1);
        s.epoch = r.u32();
        s.level_time = finite_or(r.f32());
        s.escaped = r.u8() != 0;
        s.objective.type = (ObjType)std::min<int>(r.u8(), (int)ObjType::COUNT - 1);
        s.objective.have = r.u8();
        s.objective.need = r.u8();
        s.objective.done = r.u8() != 0;
        s.objective.reward = (Perk)std::min<int>(r.u8(), PERK_COUNT - 1);
        s.objective.required = r.u8() != 0;
        {
            uint8_t fl = r.u8();
            s.sandbox = fl & 1; s.god = (fl & 2) != 0; s.auto_spawn = (fl & 4) != 0; s.freeze = (fl & 8) != 0;
        }
        s.won_t = finite_or(r.f32());
        s.my_id = r.u8();
        s.ack = r.u32();
        int np = r.u8();
        for (int k = 0; k < np && !r.bad; k++) {
            int id = r.u8();
            if (id >= IRIS_MAX_PLAYERS) { r.bad = true; break; }
            auto& p = s.players[id];
            p.active = true;
            p.name = r.str();
            p.respawn_t = finite_or(r.f32());
            read_player(r, p.st);
        }
        int ne = r.u8();
        for (int k = 0; k < ne && !r.bad; k++) {
            EntSnap e;
            e.id = r.u16();
            e.type = (EntityType)r.u8();
            e.state = (EntState)std::min<int>(r.u8(), (int)EntState::Stunned);
            e.pos.x = finite_or(r.f32());
            e.pos.z = finite_or(r.f32());
            e.yaw = (int16_t)r.u16() / 32767.0f * PI;
            e.anim = finite_or(r.f32());
            e.speed = r.u8() / 20.0f;
            if ((int)e.type < (int)EntityType::COUNT) s.ents.push_back(e);
        }
        int npr = r.u8();
        for (int k = 0; k < npr && !r.bad; k++) {
            PropSnap p;
            p.id = r.u16();
            p.kind = (PropKind)std::min<int>(r.u8(), (int)PropKind::Key);
            p.item = (ItemType)r.u8();
            p.pos.x = finite_or(r.f32());
            p.pos.z = finite_or(r.f32());
            p.life = finite_or(r.f32());
            // (only real items need a valid item type: tapes, breakers and chalk marks keep their
            // rotation / side in that byte)
            if (p.kind != PropKind::Item || ((int)p.item > 0 && (int)p.item < ITEM_COUNT)) s.props.push_back(p);
        }
        if (s.epoch != picked_epoch) { picked.clear(); picked_epoch = s.epoch; }
        int k = r.u8();
        for (int j = 0; j < k && !r.bad; j++) {
            uint32_t id = r.u32();
            if (picked.size() < 200000) picked.insert(id); // (a sane level never gets near this)
        }
        {
            int nd = r.u8();
            for (int j = 0; j < nd && !r.bad; j++) s.doors.push_back(r.u32());
            int ns = r.u8();
            for (int j = 0; j < ns && !r.bad; j++) s.searched.push_back(r.u32());
        }
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
        if (now - hello_t_ > 0.5) send_hello(now); // keep knocking until the host answers
        if (now - connect_start_ > 6.0) {
            state = State::Failed;
            error = "NO ANSWER FROM " + net_addr_str(server_) + " (WRONG IP, OR UDP PORT NOT OPEN?)";
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
        error = "LOST CONNECTION TO THE HOST";
    }
}

void NetClient::scan(double now) {
    if (!scan_sock_.is_open() && !scan_sock_.open(0, true)) return;
    char q[QUERY_SIZE] = "IRIS?";
    scan_sock_.send(net_broadcast(IRIS_DEFAULT_PORT), q, QUERY_SIZE);
    NetAddr local{0x7F000001u, IRIS_DEFAULT_PORT}; // also find a game hosted on this machine
    scan_sock_.send(local, q, QUERY_SIZE);
    // forget servers that stopped answering
    for (size_t i = 0; i < found.size();)
        if (now - found[i].seen > 4.0) found.erase(found.begin() + (long)i);
        else i++;
}

void NetClient::scan_to(const NetAddr& addr, const std::string& via) {
    if (!scan_sock_.is_open() && !scan_sock_.open(0, true)) return;
    char q[QUERY_SIZE] = "IRIS?";
    scan_sock_.send(addr, q, QUERY_SIZE);
    for (auto& a : asked_) if (a.first == addr) { a.second = via; return; }
    if (asked_.size() < 64) asked_.push_back({addr, via});
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
        for (const auto& a : asked_) if (a.first == from) f.via = a.second;
        bool merged = false;
        for (auto& o : found) {
            // the same server answering on loopback and on the LAN: keep one entry
            if (o.addr == from || (o.name == f.name && o.addr.port == from.port && (from.ip >> 24) == 127)) {
                if ((from.ip >> 24) != 127) o.addr = from;
                o.players = f.players; o.max_players = f.max_players; o.level = f.level; o.seen = now;
                if (!f.via.empty()) o.via = f.via;
                merged = true;
            }
        }
        if (!merged && found.size() < 24) found.push_back(f);
    }
}

} // namespace iris
