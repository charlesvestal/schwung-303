// Minimal JUCE-free wrapper around the GuitarML LSTM overdrive.
// Uses RTNeural directly (Eigen backend).
//
// Supports the two model architectures shipped with jc303:
//   - LSTM40 non-conditioned (input_size=1, hidden=40)
//   - LSTM40 conditioned     (input_size=2, hidden=40)
//
// Both architectures: one LSTM layer followed by one dense layer, with a
// skip connection (output += input). Matches GuitarML's Proteus tone models.
//
// Sample-rate mismatch between model and host is handled by RTNeural's
// SampleRateCorrectionMode::LinInterp.

#pragma once

#define RTNEURAL_USE_EIGEN 1

#include "../deps/RTNeural/RTNeural.h"

#include <memory>
#include <string>
#include <vector>

namespace guitarml {

class GuitarMLAmp {
public:
    GuitarMLAmp();
    ~GuitarMLAmp();

    // Set host sample rate (Hz). Call before loadModelFromFile.
    void prepare(double sample_rate);

    // Load a model from a JSON file on disk. Returns true on success.
    // Thread-safety: not realtime-safe; call from the control thread, not audio.
    bool loadModelFromFile(const std::string &path);

    // Reset RNN state (call on "panic" or when changing models).
    void reset();

    // Process a mono buffer in place. If condition is provided (0..1), it's
    // used for conditioned models; ignored for non-conditioned models.
    void process(float *buffer, int frames, float condition = 0.5f);

    bool isLoaded() const { return arch_ != ModelArch::None; }

private:
    enum class ModelArch { None, LSTM40NoCond, LSTM40Cond };

    using LSTM_NoCond = RTNeural::ModelT<float, 1, 1,
        RTNeural::LSTMLayerT<float, 1, 40, RTNeural::SampleRateCorrectionMode::LinInterp>,
        RTNeural::DenseT<float, 40, 1>>;
    using LSTM_Cond = RTNeural::ModelT<float, 2, 1,
        RTNeural::LSTMLayerT<float, 2, 40, RTNeural::SampleRateCorrectionMode::LinInterp>,
        RTNeural::DenseT<float, 40, 1>>;

    std::unique_ptr<LSTM_NoCond> nocond_;
    std::unique_ptr<LSTM_Cond>   cond_;

    ModelArch arch_ = ModelArch::None;
    double    sample_rate_  = 44100.0;
    double    model_rate_   = 44100.0;

    // DC blocker state (one-pole).
    float dc_x1_ = 0.0f;
    float dc_y1_ = 0.0f;
};

} // namespace guitarml
