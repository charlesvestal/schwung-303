// Multi-model saturation stage for schwung-303.
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Charles Vestal.
// Copyright (C) Dave Mollen (RAT model — ported from davemollen/dm-Rat,
//   GPL-3.0, rat/src/{clipper,op_amp,tone}.rs and supporting modules).
//
// Models:
//   Soft — tilt-EQ + 2x OS + asymmetric tanh. Generic warm clip, flat flavor.
//   RAT  — ported from davemollen/dm-Rat (GPL-3.0): 3rd-order IIR op-amp with
//          distortion-modulated coefficients → algebraic waveshaper
//          x/(1+x^4)^(1/4) around 2x oversampling → 1-pole tone stack.
//          Classic ProCo RAT character.
//
// Both models: `drive` (0..1) fully bypasses at 0 and sets saturation depth.
// `mix` (0..1) is dry/wet. 2x oversampling uses biquad LP filters (cheap; not
// FIR-steep, but adequate for 303's low-content range).
//
// Shared helpers (Biquad, filter designers) live in the `drive` namespace;
// per-model state + processing live in `drive::soft` and `drive::rat`.
// `drive::Drive` is the tagged dispatcher held by the plugin.

#pragma once

#include <algorithm>
#include <cmath>

namespace drive {

// ========================================================================
// Shared helpers
// ========================================================================

struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() { z1 = z2 = 0.0f; }

    float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

inline void set_lowpass(Biquad &bq, double fc, double fs) {
    const double q  = 0.7071067811865476;
    const double w0 = 2.0 * M_PI * fc / fs;
    const double cs = std::cos(w0);
    const double sn = std::sin(w0);
    const double alpha = sn / (2.0 * q);
    const double b0 = (1.0 - cs) * 0.5;
    const double b1 = 1.0 - cs;
    const double b2 = (1.0 - cs) * 0.5;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cs;
    const double a2 = 1.0 - alpha;
    bq.b0 = static_cast<float>(b0 / a0);
    bq.b1 = static_cast<float>(b1 / a0);
    bq.b2 = static_cast<float>(b2 / a0);
    bq.a1 = static_cast<float>(a1 / a0);
    bq.a2 = static_cast<float>(a2 / a0);
}

inline void set_low_shelf(Biquad &bq, double fc, double gain_db, double fs) {
    const double A  = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * M_PI * fc / fs;
    const double cs = std::cos(w0);
    const double sn = std::sin(w0);
    const double S  = 1.0;
    const double alpha = sn * 0.5 *
        std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
    const double sqrtA2alpha = 2.0 * std::sqrt(A) * alpha;
    const double b0 =       A * ((A + 1.0) - (A - 1.0) * cs + sqrtA2alpha);
    const double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cs);
    const double b2 =       A * ((A + 1.0) - (A - 1.0) * cs - sqrtA2alpha);
    const double a0 =           (A + 1.0) + (A - 1.0) * cs + sqrtA2alpha;
    const double a1 =    -2.0 * ((A - 1.0) + (A + 1.0) * cs);
    const double a2 =           (A + 1.0) + (A - 1.0) * cs - sqrtA2alpha;
    bq.b0 = static_cast<float>(b0 / a0);
    bq.b1 = static_cast<float>(b1 / a0);
    bq.b2 = static_cast<float>(b2 / a0);
    bq.a1 = static_cast<float>(a1 / a0);
    bq.a2 = static_cast<float>(a2 / a0);
}

// One-pole LPF with cutoff re-tuning. Used by RAT's op-amp correction and
// tone stack — matches dm-Rat's OnePoleFilter shape.
struct OnePoleLP {
    float t_neg_tau = 0.0f;
    float z = 0.0f;
    float prev_freq = -1.0f;
    float b1 = 0.0f;

    void prepare(double fs) {
        t_neg_tau = static_cast<float>(-2.0 * M_PI / fs);
        z = 0.0f;
        prev_freq = -1.0f;
        b1 = 0.0f;
    }

    void reset() { z = 0.0f; prev_freq = -1.0f; }

    float process(float x, float freq) {
        if (freq != prev_freq) {
            b1 = std::exp(freq * t_neg_tau);
            prev_freq = freq;
        }
        const float a0 = 1.0f - b1;
        z = x * a0 + z * b1;
        return z;
    }
};

// ========================================================================
// Soft model
// ========================================================================

namespace soft {

static inline float asym_tanh(float x) {
    constexpr float bias = 0.15f;
    constexpr float tanh_bias = 0.14888503f;  // tanh(0.15)
    return std::tanh(x + bias) - tanh_bias;
}

struct State {
    Biquad pre_shelf;
    Biquad post_shelf;
    Biquad up_lp;    // 2x rate
    Biquad down_lp;  // 2x rate
    float dc_x1 = 0.0f;
    float dc_y1 = 0.0f;

    void prepare(double fs) {
        const double fs2 = fs * 2.0;
        set_low_shelf(pre_shelf,  400.0,  6.0, fs);
        set_low_shelf(post_shelf, 400.0, -6.0, fs);
        set_lowpass(up_lp,   19000.0, fs2);
        set_lowpass(down_lp, 19000.0, fs2);
        reset();
    }

    void reset() {
        pre_shelf.reset();
        post_shelf.reset();
        up_lp.reset();
        down_lp.reset();
        dc_x1 = dc_y1 = 0.0f;
    }
};

inline void process(State &s, float *buf, int frames,
                    float drive_amt, float mix) {
    if (drive_amt <= 0.0f || mix <= 0.0f) return;
    const float pre_gain = std::pow(10.0f, drive_amt * 24.0f / 20.0f);
    const float inv_pre  = 1.0f / pre_gain;
    const float wet = mix;
    const float dry = 1.0f - mix;
    for (int n = 0; n < frames; ++n) {
        const float x = buf[n];
        float v = s.pre_shelf.process(x) * pre_gain;
        const float up0 = 2.0f * s.up_lp.process(v);
        const float up1 = 2.0f * s.up_lp.process(0.0f);
        const float sat0 = asym_tanh(up0);
        const float sat1 = asym_tanh(up1);
        s.down_lp.process(sat0);
        float y = s.down_lp.process(sat1);
        y *= inv_pre;
        y = s.post_shelf.process(y);
        const float y_hp = y - s.dc_x1 + 0.9996f * s.dc_y1;
        s.dc_x1 = y;
        s.dc_y1 = y_hp;
        y = y_hp;
        buf[n] = dry * x + wet * y;
    }
}

} // namespace soft

// ========================================================================
// RAT model (ported from davemollen/dm-Rat, GPL-3.0)
// ========================================================================

namespace rat {

// Op-amp circuit constants (from dm-Rat/rat/src/op_amp.rs).
constexpr float R1         = 100000.0f;
constexpr float C1         = 1e-10f;
constexpr float Z1_B0      = 2.72149e-7f;
constexpr float Z1_B1      = 0.0027354f;
constexpr float Z1_A0      = 6.27638e-9f;
constexpr float Z1_A1      = 0.0000069f;
constexpr float MAX_GAIN_AT_1HZ = 1119360.558108f;
constexpr float MIN_DIST_GAIN   = 1.0f;
constexpr float MAX_DIST_GAIN   = 2307.231003f;

// Clipper calibration (from dm-Rat/rat/src/clipper.rs).
constexpr float CLIP_PRE_GAIN  = 1.877f;
constexpr float CLIP_POST_GAIN = 0.3204805f;

// Tone stack (from dm-Rat/rat/src/tone.rs).
constexpr float TONE_R1 = 100000.0f;
constexpr float TONE_R2 = 1500.0f;
constexpr float TONE_C1 = 3.3e-9f;

// 3rd-order IIR, direct-form-2 transposed (matches dm-Rat's ThirdOrderIIRFilter).
struct ThirdOrderIIR {
    float b[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float a[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float z0 = 0.0f, z1 = 0.0f, z2 = 0.0f;

    void reset() { z0 = z1 = z2 = 0.0f; }

    float process(float x) {
        const float y = x * b[0] + z0;
        z0 = x * b[1] - y * a[1] + z1;
        z1 = x * b[2] - y * a[2] + z2;
        z2 = x * b[3] - y * a[3];
        return y;
    }
};

// Bilinear-transform scale factors. dm-Rat's BilinearTransform.
struct BilinearT {
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;

    void prepare(double fs) {
        const double t = 1.0 / fs;
        s0 = static_cast<float>(t * 0.5);
        s1 = static_cast<float>(t * t * 0.25);
        s2 = static_cast<float>(t * t * t * 0.125);
    }

    // Transforms polynomial [x0, x1, x2, x3] (s^0..s^3 coefficients) into
    // z-domain polynomial [y0, y1, y2, y3] (z^-0..z^-3).
    void transform(const float x[4], float out[4]) const {
        const float x0 = x[0];
        const float x1 = x[1] * s0;
        const float x2 = x[2] * s1;
        const float x3 = x[3] * s2;
        out[0] =  x0 + x1 + x2 + x3;
        out[1] = -3.0f * x0 - x1 + x2 + 3.0f * x3;
        out[2] =  3.0f * x0 - x1 - x2 + 3.0f * x3;
        out[3] = -x0 + x1 - x2 + x3;
    }
};

static inline float clip(float x) {
    const float x2 = x * x;
    const float x4 = x2 * x2;
    return x / std::sqrt(std::sqrt(1.0f + x4));
}

static inline float tone_cutoff(float tone) {
    const float R = tone * TONE_R1 + TONE_R2;
    return 1.0f / (2.0f * static_cast<float>(M_PI) * R * TONE_C1);
}

struct State {
    ThirdOrderIIR op_amp;
    BilinearT     bilinear;
    OnePoleLP     op_amp_correction;
    Biquad        up_lp;
    Biquad        down_lp;
    OnePoleLP     tone;
    float         dc_x1 = 0.0f;
    float         dc_y1 = 0.0f;

    void prepare(double fs) {
        bilinear.prepare(fs);
        op_amp_correction.prepare(fs);
        tone.prepare(fs);
        const double fs2 = fs * 2.0;
        set_lowpass(up_lp,   19000.0, fs2);
        set_lowpass(down_lp, 19000.0, fs2);
        reset();
    }

    void reset() {
        op_amp.reset();
        op_amp_correction.reset();
        tone.reset();
        up_lp.reset();
        down_lp.reset();
        dc_x1 = dc_y1 = 0.0f;
    }

    // Recompute 3rd-order IIR coefficients from the distortion knob via
    // bilinear transform of the op-amp's s-domain transfer function.
    // Cheap enough to do every block (once per 128 samples in our host).
    void update_op_amp_coeffs(float distortion) {
        const float z2_b0 = std::max(distortion * R1, 1.0f);
        const float z2_a0 = z2_b0 * C1;

        const float a0 = Z1_B0 * z2_a0;
        const float a1 = Z1_B0 + Z1_B1 * z2_a0;
        const float a2 = Z1_B1 + z2_a0;
        const float b0 = a0;
        const float b1 = a1 + Z1_A0 * z2_b0;
        const float b2 = Z1_A1 * z2_b0 + Z1_B1 + z2_a0;

        const float num_s[4] = {b0, b1, b2, 1.0f};
        const float den_s[4] = {a0, a1, a2, 1.0f};
        float num_z[4], den_z[4];
        bilinear.transform(num_s, num_z);
        bilinear.transform(den_s, den_z);

        const float inv_a0 = 1.0f / den_z[0];
        for (int i = 0; i < 4; ++i) {
            op_amp.b[i] = num_z[i] * inv_a0;
            op_amp.a[i] = den_z[i] * inv_a0;
        }
    }
};

inline void process(State &s, float *buf, int frames,
                    float drive_amt, float mix) {
    if (drive_amt <= 0.0f || mix <= 0.0f) return;

    const float distortion = std::max(drive_amt, 0.001f);
    s.update_op_amp_coeffs(distortion);

    const float correction_cutoff = MAX_GAIN_AT_1HZ /
        (distortion * (MAX_DIST_GAIN - MIN_DIST_GAIN) + MIN_DIST_GAIN);
    // Fixed mid tone for now; could expose as a param later.
    const float tone_freq = tone_cutoff(0.5f);

    const float wet = mix;
    const float dry = 1.0f - mix;

    for (int n = 0; n < frames; ++n) {
        const float x = buf[n];

        // Op-amp stage — distortion-modulated 3rd-order IIR.
        float v = s.op_amp.process(x);
        v = s.op_amp_correction.process(v, correction_cutoff);

        // Push into the waveshaper.
        v *= CLIP_PRE_GAIN;

        // 2x oversampling around the clipper (cheaper than dm-Rat's 8x FIR;
        // biquad LP at ~19 kHz is adequate for a monophonic bass synth).
        const float up0 = 2.0f * s.up_lp.process(v);
        const float up1 = 2.0f * s.up_lp.process(0.0f);
        const float sat0 = clip(up0);
        const float sat1 = clip(up1);
        s.down_lp.process(sat0);
        float y = s.down_lp.process(sat1);

        y *= CLIP_POST_GAIN;

        // Tone stack.
        y = s.tone.process(y, tone_freq);

        // DC blocker.
        const float y_hp = y - s.dc_x1 + 0.9996f * s.dc_y1;
        s.dc_x1 = y;
        s.dc_y1 = y_hp;
        y = y_hp;

        buf[n] = dry * x + wet * y;
    }
}

} // namespace rat

// ========================================================================
// Dispatcher
// ========================================================================

enum class Model {
    Soft = 0,
    RAT  = 1,
    Count,
};

inline const char *model_name(Model m) {
    switch (m) {
        case Model::Soft: return "Soft";
        case Model::RAT:  return "RAT";
        default:          return "?";
    }
}

struct Drive {
    Model       model = Model::Soft;
    soft::State soft_state;
    rat::State  rat_state;

    void prepare(double fs) {
        soft_state.prepare(fs);
        rat_state.prepare(fs);
    }

    void reset() {
        soft_state.reset();
        rat_state.reset();
    }

    void set_model(int m) {
        const Model next = (m >= 0 && m < static_cast<int>(Model::Count))
            ? static_cast<Model>(m) : Model::Soft;
        if (next != model) {
            // Reset destination state so we don't hear stale filter history.
            switch (next) {
                case Model::Soft: soft_state.reset(); break;
                case Model::RAT:  rat_state.reset();  break;
                default: break;
            }
            model = next;
        }
    }

    void process(float *buf, int frames, float drive_amt, float mix) {
        switch (model) {
            case Model::Soft: soft::process(soft_state, buf, frames, drive_amt, mix); break;
            case Model::RAT:  rat::process(rat_state,   buf, frames, drive_amt, mix); break;
            default: break;
        }
    }
};

} // namespace drive
