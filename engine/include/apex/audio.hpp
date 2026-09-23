// Procedural soundscape: rain, droplets, synthwave pad + sub pulse, footsteps,
// car engine / road noise and a gig-complete chime. No audio assets.
//
// Threading: the game thread writes AudioState through Synth::set_state (atomics);
// the audio thread calls Synth::render. render() never allocates or locks.
#pragma once

#include <atomic>
#include <cstdint>

namespace apex {

struct AudioState {
    float rain = 1.0f;           // 0..1
    float player_speed = 0.0f;   // m/s
    bool driving = false;
    float throttle = 0.0f;       // -1..1 (driving)
    bool on_ground = true;
    std::uint32_t payouts = 0;   // monotonically increasing; each increment plays the chime
};

class Synth {
public:
    explicit Synth(float sample_rate);

    void set_state(const AudioState& s);
    // Interleaved stereo float output.
    void render(float* out, int frames);

    float sample_rate() const { return rate_; }

private:
    float rate_;
    std::uint32_t rng_ = 0x12345678u;

    // Shared state from the game thread.
    std::atomic<float> rain_{1.0f}, speed_{0.0f}, throttle_{0.0f};
    std::atomic<bool> driving_{false}, on_ground_{true};
    std::atomic<std::uint32_t> payouts_{0};

    // Audio-thread state.
    double time_ = 0.0;
    float rain_lp_[2] = {0, 0}, rain_lp2_[2] = {0, 0}, rain_level_ = 0.0f;
    float drop_env_ = 0.0f, drop_phase_ = 0.0f, drop_freq_ = 3000.0f, drop_pan_ = 0.5f;
    float pad_phase_[6] = {}, pad_lp_[2] = {0, 0};
    float sub_phase_ = 0.0f;
    float step_clock_ = 0.0f, step_env_ = 0.0f, step_phase_ = 0.0f, step_noise_lp_ = 0.0f;
    float engine_phase_ = 0.0f, engine_lp_ = 0.0f, engine_level_ = 0.0f, road_lp_ = 0.0f;
    std::uint32_t heard_payouts_ = 0;
    float chime_time_ = 100.0f;
    float chime_phase_ = 0.0f;
    float smooth_speed_ = 0.0f;

    float noise();
};

}  // namespace apex
