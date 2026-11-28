// Backrooms Iris - sound. Every sound is synthesized in code at startup (no audio files, nothing to
// license): hums, drones, wind, footsteps, guns, entities, UI. A small software mixer feeds SDL's
// audio device on PC and Vita. One-shots are fire-and-forget; loops are steady channels whose
// volume and pan the game sets every frame (ambience, fire, alarms, heartbeat...).
#pragma once
#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>

namespace iris {

enum Sfx : int {
    SFX_STEP_CARPET, SFX_STEP_HARD, SFX_STEP_WATER, SFX_STEP_METAL, SFX_STEP_WOOD,
    SFX_PICKUP, SFX_DRINK, SFX_EAT, SFX_CLICK, SFX_UI_MOVE, SFX_UI_SELECT,
    SFX_PISTOL, SFX_SHOTGUN, SFX_RIFLE, SFX_SWING, SFX_HURT, SFX_DEATH, SFX_EXIT,
    SFX_BREAKER, SFX_TAPE, SFX_OBJECTIVE, SFX_FLASH, SFX_THROW,
    SFX_BACTERIA_CLICK, SFX_BACTERIA_SCREECH, SFX_HOUND_GROWL, SFX_SMILER_WHISPER, SFX_SMILER_CHARGE,
    SFX_PARTY_GIGGLE, SFX_SKIN_GROAN, SFX_STING,
    SFX_DOOR_OPEN, SFX_DOOR_CLOSE, SFX_DOOR_JAM, SFX_DRAWER, SFX_PAPER, // 1.3
    // loop sources
    SFX_HUM, SFX_DRONE, SFX_WIND, SFX_WATER, SFX_ALARM, SFX_FIRE, SFX_STATIC, SFX_HEART, SFX_MUSICBOX, SFX_WHISPERS,
    SFX_MUSIC_MENU, SFX_MUSIC_RUN, SFX_MUSIC_CALM, // 1.2: music (its own volume)
    SFX_COUNT
};

enum Loop : int { LOOP_AMB1, LOOP_AMB2, LOOP_FIRE, LOOP_STATIC, LOOP_HEART, LOOP_MUSIC, LOOP_JUICE, LOOP_BGM, LOOP_COUNT };

class Audio {
public:
    bool init();                 // opens the device and synthesizes everything; false = silent
    void shutdown();
    bool ok() const { return dev_ != 0; }
    void set_master(float v) { master_ = v; }
    void set_music(float v) { music_ = v; }  // the music channel (LOOP_BGM) on top of the master volume
    // one-shot: volume 0..1, pan -1 (left) .. 1 (right), pitch multiplier, muffle 0..1 (behind walls)
    void play(Sfx s, float vol = 1, float pan = 0, float pitch = 1, float muffle = 0);
    // a looping channel: which sound, how loud (0 = silent), pan; changes are smoothed
    void loop(Loop l, Sfx s, float vol, float pan = 0, float pitch = 1, float muffle = 0);
    void stop_loops();
    // 1.3 proximity chat. The microphone is only opened in a multiplayer game with voice on.
    bool mic_open();                         // false: no microphone (or no permission)
    void mic_close();
    bool mic_ok() const { return mic_ != 0; }
    int mic_read(int16_t* out, int max);     // 16 kHz mono samples captured since last time
    // play what player `speaker` said: 16 kHz mono, placed by vol / pan / muffle; radio = a crackly handset
    void voice_push(int speaker, const int16_t* pcm, int n, float vol, float pan, float muffle, bool radio);
    void voice_clear();
private:
    struct VoiceOut {
        std::vector<float> ring;             // 16 kHz samples (a second of them)
        uint64_t w = 0;                      // samples written
        double r = 0;                        // read position (samples)
        bool playing = false;                // false: filling up (a little jitter buffer)
        float vol = 0, target = 0, pan = 0, muffle = 0, lp = 0, hp = 0;
        bool radio = false;
    };
    VoiceOut vout_[4];
    uint32_t hiss_ = 1;
    uint32_t mic_ = 0;
    std::vector<int16_t> mic_ring_;
    uint64_t mic_w_ = 0, mic_r_ = 0;
    static void mic_callback(void* user, uint8_t* stream, int len);
    struct Voice { int sfx = -1; double pos = 0; float vol = 0, target = 0, pan = 0, pitch = 1, muffle = 0, lp = 0; bool looping = false; };
    static void callback(void* user, uint8_t* stream, int len);
    void mix(int16_t* out, int frames);
    uint32_t dev_ = 0;
    int rate_ = 44100;
    float master_ = 0.8f, music_ = 0.6f;
    std::vector<float> buf_[SFX_COUNT];
    Voice voices_[24];
    Voice loops_[LOOP_COUNT];
    std::vector<float> acc_;
    std::atomic<bool> ready_{false};
    std::thread synth_;
};

void synth_all(std::vector<float> out[SFX_COUNT]); // 22050 Hz mono (exposed for tests)
constexpr int SYNTH_RATE = 22050;

} // namespace iris
