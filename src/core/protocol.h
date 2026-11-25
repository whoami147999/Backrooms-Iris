// Backrooms Iris - wire format. Little-endian, fixed layout, no allocations on the hot path.
//
// Client -> server:  HELLO, INPUT (last few commands, redundantly), BYE, and "IRIS?" queries
// Server -> client:  WELCOME / REJECT, SNAPSHOT (20 Hz), and "IRIS!" query answers
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "player.h"
#include "sim.h"

namespace iris {

enum PacketType : uint8_t {
    PKT_HELLO = 1, PKT_WELCOME = 2, PKT_REJECT = 3, PKT_INPUT = 4, PKT_SNAPSHOT = 5, PKT_BYE = 6,
};
constexpr int MAX_PACKET = 1400;

class Writer {
public:
    uint8_t buf[MAX_PACKET];
    int n = 0;
    bool overflow = false;
    void bytes(const void* p, int len) {
        if (n + len > MAX_PACKET) { overflow = true; return; }
        std::memcpy(buf + n, p, (size_t)len);
        n += len;
    }
    void u8(uint8_t v) { bytes(&v, 1); }
    void u16(uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; bytes(b, 2); }
    void u32(uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; bytes(b, 4); }
    void i8(int8_t v) { u8((uint8_t)v); }
    void f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); u32(u); }
    void str(const std::string& s) { uint8_t l = (uint8_t)std::min<size_t>(s.size(), 48); u8(l); bytes(s.data(), l); }
    void header(PacketType t) { u8('I'); u8('R'); u8(t); }
};

class Reader {
public:
    const uint8_t* p;
    int n, i = 0;
    bool bad = false;
    Reader(const void* data, int len) : p((const uint8_t*)data), n(len) {}
    bool need(int k) { if (i + k > n) { bad = true; return false; } return true; }
    uint8_t u8() { return need(1) ? p[i++] : 0; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = (uint16_t)(p[i] | (p[i + 1] << 8)); i += 2; return v; }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) | ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24);
        i += 4;
        return v;
    }
    int8_t i8() { return (int8_t)u8(); }
    float f32() { uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
    std::string str() { uint8_t l = u8(); if (!need(l)) return ""; std::string s((const char*)p + i, l); i += l; return s; }
    // returns the packet type, or 0 if this isn't one of ours
    uint8_t header() { if (n < 3 || p[0] != 'I' || p[1] != 'R') return 0; i = 3; return p[2]; }
};

inline void write_cmd(Writer& w, uint32_t seq, const InputCmd& c) {
    w.u32(seq);
    w.i8((int8_t)(clampf(c.move_x, -1, 1) * 127));
    w.i8((int8_t)(clampf(c.move_z, -1, 1) * 127));
    w.f32(c.yaw);
    w.f32(c.pitch);
    w.u8(c.buttons);
    w.u8(c.pickup_ctr);
    w.u8(c.use_ctr);
    w.u8(c.use_item);
    w.u8(c.light_ctr);
    w.u8(c.attack_ctr);
}

inline InputCmd read_cmd(Reader& r, uint32_t& seq) {
    InputCmd c;
    seq = r.u32();
    c.move_x = r.i8() / 127.0f;
    c.move_z = r.i8() / 127.0f;
    c.yaw = r.f32();
    c.pitch = r.f32();
    c.buttons = r.u8();
    c.pickup_ctr = r.u8();
    c.use_ctr = r.u8();
    c.use_item = r.u8();
    c.light_ctr = r.u8();
    c.attack_ctr = r.u8();
    return c;
}

// Full player state (the owner needs all of it to re-run prediction; others use some).
inline void write_player(Writer& w, const PlayerState& s) {
    w.f32(s.pos.x); w.f32(s.pos.z); w.f32(s.vel.x); w.f32(s.vel.z);
    w.f32(s.yaw); w.f32(s.pitch);
    w.u8((uint8_t)(s.crouch * 255)); w.f32(s.stamina); w.u8(s.exhausted);
    w.f32(s.bob); w.f32(s.speed);
    w.f32(s.health); w.f32(s.sanity); w.f32(s.boost); w.f32(s.slow); w.f32(s.lucky);
    w.f32(s.juice); w.f32(s.hurt); w.f32(s.swing); w.f32(s.battery);
    w.u8((uint8_t)((s.light_on ? 1 : 0) | (s.noclip ? 2 : 0) | (s.alive ? 4 : 0)));
    for (int i = 0; i < ITEM_COUNT; i++) w.u8(s.inv[i]);
    for (int i = 0; i < PERK_COUNT; i++) w.u8(s.perks[i]);
    w.u8((uint8_t)s.cause);
}

inline void read_player(Reader& r, PlayerState& s) {
    s.pos.x = r.f32(); s.pos.z = r.f32(); s.pos.y = 0; s.vel.x = r.f32(); s.vel.z = r.f32();
    s.yaw = r.f32(); s.pitch = r.f32();
    s.crouch = r.u8() / 255.0f; s.stamina = r.f32(); s.exhausted = r.u8() != 0;
    s.bob = r.f32(); s.speed = r.f32();
    s.health = r.f32(); s.sanity = r.f32(); s.boost = r.f32(); s.slow = r.f32(); s.lucky = r.f32();
    s.juice = r.f32(); s.hurt = r.f32(); s.swing = r.f32(); s.battery = r.f32();
    uint8_t f = r.u8();
    s.light_on = f & 1; s.noclip = (f & 2) != 0; s.alive = (f & 4) != 0;
    for (int i = 0; i < ITEM_COUNT; i++) s.inv[i] = r.u8();
    for (int i = 0; i < PERK_COUNT; i++) s.perks[i] = r.u8();
    s.cause = (DeathCause)r.u8();
}

} // namespace iris
