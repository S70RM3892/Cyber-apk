#include "apex/audio.hpp"

#include <algorithm>
#include <cmath>

namespace apex {

namespace {

constexpr float kTwoPi = 6.28318530718f;

float midi_hz(float note) { return 440.0f * std::exp2((note - 69.0f) / 12.0f); }

// Naive sawtooth in [-1, 1]; aliasing is buried under the pad's low-pass.
float saw(float phase) { return 2.0f * phase - 1.0f; }

// Am - F - C - G, voiced low (MIDI notes), 8 s per chord.
constexpr float kChords[4][3] = {{45, 52, 57}, {41, 48, 57}, {48, 52, 55}, {43, 50, 55}};
constexpr float kChordSeconds = 8.0f;

// Gig-complete chime: A5 C6 E6 A6.
constexpr float kChime[4] = {81, 84, 88, 93};
constexpr float kChimeStep = 0.09f;

}  // namespace

Synth::Synth(float sample_rate) : rate_(sample_rate) {}

void Synth::set_state(const AudioState& s) {
    rain_.store(s.rain, std::memory_order_relaxed);
    speed_.store(s.player_speed, std::memory_order_relaxed);
    throttle_.store(s.throttle, std::memory_order_relaxed);
    driving_.store(s.driving, std::memory_order_relaxed);
    on_ground_.store(s.on_ground, std::memory_order_relaxed);
    payouts_.store(s.payouts, std::memory_order_relaxed);
}

float Synth::noise() {
    // xorshift32 -> [-1, 1)
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

void Synth::render(float* out, int frames) {
    const float dt = 1.0f / rate_;
    const float rain = std::clamp(rain_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float speed = speed_.load(std::memory_order_relaxed);
    const float throttle = throttle_.load(std::memory_order_relaxed);
    const bool driving = driving_.load(std::memory_order_relaxed);
    const bool grounded = on_ground_.load(std::memory_order_relaxed);
    const std::uint32_t payouts = payouts_.load(std::memory_order_relaxed);
    if (payouts != heard_payouts_) {
        if (payouts > heard_payouts_) chime_time_ = 0.0f;
        heard_payouts_ = payouts;
    }

    // One-pole coefficients (cutoff -> alpha).
    auto alpha = [&](float hz) { return 1.0f - std::exp(-kTwoPi * hz * dt); };
    const float a_rain = alpha(5200.0f), a_rain2 = alpha(700.0f);
    const float a_step = alpha(400.0f), a_road = alpha(300.0f), a_engine = alpha(900.0f);

    for (int i = 0; i < frames; ++i) {
        const float t = static_cast<float>(time_);
        smooth_speed_ += (speed - smooth_speed_) * 0.0005f;
        float l = 0.0f, r = 0.0f;

        // ---- Rain: bright hiss + darker body, slowly breathing, decorrelated L/R ----
        rain_level_ += (rain - rain_level_) * 0.0002f;
        const float gust = 0.8f + 0.2f * std::sin(t * 0.31f) * std::sin(t * 0.17f + 1.0f);
        for (int c = 0; c < 2; ++c) {
            const float n = noise();
            rain_lp_[c] += (n - rain_lp_[c]) * a_rain;
            rain_lp2_[c] += (n - rain_lp2_[c]) * a_rain2;
            const float hiss = (rain_lp_[c] - rain_lp2_[c]) * 0.10f + rain_lp2_[c] * 0.07f;
            (c == 0 ? l : r) += hiss * rain_level_ * gust;
        }

        // ---- Droplets: sparse resonant ticks on nearby surfaces ----
        if (rain_level_ > 0.05f && (rng_ & 0x3FFF) < static_cast<std::uint32_t>(3.0f * rain_level_)) {
            drop_env_ = 0.5f + 0.5f * (noise() * 0.5f + 0.5f);
            drop_freq_ = 1800.0f + 3500.0f * (noise() * 0.5f + 0.5f);
            drop_pan_ = noise() * 0.5f + 0.5f;
        }
        if (drop_env_ > 1e-4f) {
            drop_phase_ += drop_freq_ * dt;
            drop_phase_ -= std::floor(drop_phase_);
            const float d = std::sin(kTwoPi * drop_phase_) * drop_env_ * 0.05f;
            l += d * (1.0f - drop_pan_);
            r += d * drop_pan_;
            drop_env_ *= 0.9985f;
        }

        // ---- Pad: detuned saws per chord note, slow crossfade, swept low-pass ----
        const float chord_pos = t / kChordSeconds;
        const int ci = static_cast<int>(chord_pos) % 4;
        const int cn = (ci + 1) % 4;
        const float xf = std::clamp((chord_pos - std::floor(chord_pos) - 0.8f) / 0.2f, 0.0f, 1.0f);
        float pad = 0.0f;
        for (int v = 0; v < 3; ++v) {
            const float note = kChords[ci][v] * (1.0f - xf) + kChords[cn][v] * xf;
            for (int d = 0; d < 2; ++d) {
                float& ph = pad_phase_[v * 2 + d];
                ph += midi_hz(note + (d ? 0.08f : -0.08f)) * dt;
                ph -= std::floor(ph);
                pad += saw(ph);
            }
        }
        pad *= 1.0f / 6.0f;
        const float cutoff = 380.0f + 260.0f * (0.5f + 0.5f * std::sin(t * 0.13f));
        const float a_pad = alpha(cutoff);
        pad_lp_[0] += (pad - pad_lp_[0]) * a_pad;
        pad_lp_[1] += (pad_lp_[0] - pad_lp_[1]) * a_pad;  // 2-pole
        const float pad_out = pad_lp_[1] * 0.16f;
        // Slight stereo movement.
        const float pan = 0.5f + 0.15f * std::sin(t * 0.21f);
        l += pad_out * (1.2f - pan);
        r += pad_out * (0.2f + pan);

        // ---- Sub pulse: chord root, eighth notes at 96 BPM, gentle ----
        const float root = kChords[ci][0] - 12.0f;
        sub_phase_ += midi_hz(root) * dt;
        sub_phase_ -= std::floor(sub_phase_);
        const float beat = std::fmod(t * (96.0f / 60.0f) * 2.0f, 1.0f);
        const float sub = std::sin(kTwoPi * sub_phase_) * std::exp(-beat * 6.0f) * 0.07f;
        l += sub;
        r += sub;

        // ---- Footsteps (on foot, moving, on the ground): thump + wet splash ----
        if (!driving && grounded && smooth_speed_ > 0.6f) {
            const float stride = smooth_speed_ > 7.0f ? 0.32f : 0.52f;
            step_clock_ += dt;
            if (step_clock_ >= stride) {
                step_clock_ -= stride;
                step_env_ = 1.0f;
                step_phase_ = 0.0f;
            }
        } else {
            step_clock_ = 0.4f;
        }
        if (step_env_ > 1e-4f) {
            step_phase_ += 70.0f * dt;
            const float thump = std::sin(kTwoPi * step_phase_) * step_env_;
            step_noise_lp_ += (noise() - step_noise_lp_) * a_step;
            const float splash = (noise() - step_noise_lp_) * step_env_ * 0.35f * rain_level_;
            const float s = (thump * 0.10f + splash * 0.12f);
            l += s;
            r += s;
            step_env_ *= 0.9975f;
        }

        // ---- Car: engine (saw + sub-harmonic, pitch follows speed) + road/tyre noise ----
        const float target_engine = driving ? 0.35f + 0.65f * std::fabs(throttle) : 0.0f;
        engine_level_ += (target_engine - engine_level_) * 0.0008f;
        if (engine_level_ > 1e-3f) {
            const float rpm_hz = 38.0f + std::fabs(smooth_speed_) * 3.4f + 12.0f * std::fabs(throttle);
            engine_phase_ += rpm_hz * dt;
            engine_phase_ -= std::floor(engine_phase_);
            const float e = saw(engine_phase_) * 0.6f + std::sin(kTwoPi * engine_phase_ * 0.5f) * 0.4f;
            engine_lp_ += (e - engine_lp_) * a_engine;
            road_lp_ += (noise() - road_lp_) * a_road;
            const float road = road_lp_ * std::min(1.0f, smooth_speed_ / 25.0f) * (0.5f + 0.5f * rain_level_);
            const float car = (engine_lp_ * 0.12f + road * 0.35f) * engine_level_;
            l += car;
            r += car;
        }

        // ---- Chime ----
        if (chime_time_ < 1.5f) {
            const int k = std::min(3, static_cast<int>(chime_time_ / kChimeStep));
            const float since = chime_time_ - static_cast<float>(k) * kChimeStep;
            chime_phase_ += midi_hz(kChime[k]) * dt;
            chime_phase_ -= std::floor(chime_phase_);
            const float env = std::exp(-since * (k == 3 ? 3.0f : 18.0f));
            const float c = (std::sin(kTwoPi * chime_phase_) + 0.3f * std::sin(2.0f * kTwoPi * chime_phase_)) * env * 0.14f;
            l += c;
            r += c;
            chime_time_ += dt;
        }

        // Master gain for phone speakers, soft-clipped so stacked layers stay bounded.
        out[2 * i] = std::tanh(l * 2.0f);
        out[2 * i + 1] = std::tanh(r * 2.0f);
        time_ += dt;
    }
}

}  // namespace apex
