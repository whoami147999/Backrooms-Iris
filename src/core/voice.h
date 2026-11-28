// Backrooms Iris - proximity voice chat: the codec and the wire format.
//
// Voice is IMA ADPCM, 4 bits a sample at 16 kHz mono (64 kbit/s), in 20 ms frames of 320
// samples. Each frame carries its own codec state, so a lost packet costs 20 ms of silence and
// nothing after it. Clients send frames to the host. The host passes each one on to the players
// near the speaker (or to anyone with a radio when the speaker has one too). Nothing is stored.
#pragma once
#include <cstdint>

namespace iris {

constexpr int VOICE_RATE = 16000;
constexpr int VOICE_FRAME = 320;                 // samples per frame (20 ms)
constexpr int VOICE_BYTES = VOICE_FRAME / 2;     // ADPCM payload per frame
constexpr float VOICE_RANGE = 45.0f;             // metres: past this you can't hear someone (without radios)

struct AdpcmState { int16_t pred = 0; uint8_t index = 0; };
struct VoiceFrame { int speaker = 0; bool radio = false; uint16_t seq = 0; int16_t pcm[VOICE_FRAME]; }; // a decoded frame someone said

namespace adpcm_detail {
inline const int16_t* steps() {
    static const int16_t t[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
        130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
        1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
        7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    return t;
}
inline int index_adjust(int code) {
    static const int8_t t[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    return t[code & 7];
}
inline int decode_one(AdpcmState& s, int code) {
    int step = steps()[s.index];
    int diff = step >> 3;
    if (code & 1) diff += step >> 2;
    if (code & 2) diff += step >> 1;
    if (code & 4) diff += step;
    int p = s.pred + ((code & 8) ? -diff : diff);
    p = p < -32768 ? -32768 : p > 32767 ? 32767 : p;
    s.pred = (int16_t)p;
    int ix = s.index + index_adjust(code);
    s.index = (uint8_t)(ix < 0 ? 0 : ix > 88 ? 88 : ix);
    return p;
}
} // namespace adpcm_detail

// pcm: n samples (n even) -> n/2 bytes. `s` carries on to the next frame.
inline void adpcm_encode(const int16_t* pcm, int n, AdpcmState& s, uint8_t* out) {
    using namespace adpcm_detail;
    for (int i = 0; i < n; i++) {
        int step = steps()[s.index];
        int diff = pcm[i] - s.pred;
        int code = 0;
        if (diff < 0) { code = 8; diff = -diff; }
        if (diff >= step) { code |= 4; diff -= step; }
        if (diff >= step >> 1) { code |= 2; diff -= step >> 1; }
        if (diff >= step >> 2) code |= 1;
        decode_one(s, code); // track exactly what the decoder will have
        if (i & 1) out[i >> 1] |= (uint8_t)(code << 4);
        else out[i >> 1] = (uint8_t)code;
    }
}

inline void adpcm_decode(const uint8_t* in, int n, AdpcmState s, int16_t* pcm) {
    if (s.index > 88) s.index = 88;
    for (int i = 0; i < n; i++) {
        int code = (i & 1) ? in[i >> 1] >> 4 : in[i >> 1] & 15;
        pcm[i] = (int16_t)adpcm_detail::decode_one(s, code);
    }
}

} // namespace iris
