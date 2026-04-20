#include "guitarml_amp.h"

#include "../deps/json/json.hpp"

#include <cmath>
#include <fstream>

namespace guitarml {

GuitarMLAmp::GuitarMLAmp() = default;
GuitarMLAmp::~GuitarMLAmp() = default;

void GuitarMLAmp::prepare(double sample_rate) {
    sample_rate_ = sample_rate;
}

void GuitarMLAmp::reset() {
    if (nocond_) nocond_->reset();
    if (cond_)   cond_->reset();
    dc_x1_ = dc_y1_ = 0.0f;
}

bool GuitarMLAmp::loadModelFromFile(const std::string &path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    nlohmann::json model_json;
    try {
        in >> model_json;
    } catch (...) { return false; }

    if (!model_json.contains("model_data") || !model_json.contains("state_dict"))
        return false;

    const auto &md = model_json["model_data"];
    const int  num_inputs  = md.value("input_size", 1);
    const int  hidden_size = md.value("hidden_size", 0);
    const auto unit_type   = md.value("unit_type", std::string{"LSTM"});

    if (hidden_size != 40 || unit_type != "LSTM") return false;

    model_rate_ = md.value("sample_rate", 44100.0);
    // RTNeural's LinInterp delaySamples: ratio of host to model rate.
    const float delay_samples = std::max(1.0f,
        static_cast<float>(sample_rate_ / model_rate_));

    const auto &state_dict = model_json["state_dict"];

    try {
        if (num_inputs == 1) {
            nocond_ = std::make_unique<LSTM_NoCond>();
            RTNeural::torch_helpers::loadLSTM<float>(state_dict, "rec.",
                nocond_->get<0>());
            RTNeural::torch_helpers::loadDense<float>(state_dict, "lin.",
                nocond_->get<1>());
            nocond_->get<0>().prepare(delay_samples);
            nocond_->reset();
            arch_ = ModelArch::LSTM40NoCond;
            cond_.reset();
        } else if (num_inputs == 2) {
            cond_ = std::make_unique<LSTM_Cond>();
            RTNeural::torch_helpers::loadLSTM<float>(state_dict, "rec.",
                cond_->get<0>());
            RTNeural::torch_helpers::loadDense<float>(state_dict, "lin.",
                cond_->get<1>());
            cond_->get<0>().prepare(delay_samples);
            cond_->reset();
            arch_ = ModelArch::LSTM40Cond;
            nocond_.reset();
        } else {
            return false;
        }
    } catch (...) {
        arch_ = ModelArch::None;
        nocond_.reset();
        cond_.reset();
        return false;
    }

    // Pre-warm the RNN so transients on first audio don't click.
    constexpr int warmup_frames = 2048;
    std::vector<float> silence(warmup_frames, 0.0f);
    process(silence.data(), warmup_frames, 0.5f);
    dc_x1_ = dc_y1_ = 0.0f;

    return true;
}

void GuitarMLAmp::process(float *buffer, int frames, float condition) {
    if (arch_ == ModelArch::None) return;

    if (arch_ == ModelArch::LSTM40NoCond) {
        for (int i = 0; i < frames; ++i) {
            const float x = buffer[i];
            const float y = nocond_->forward(&x);
            buffer[i] = y + x;  // residual (skip=1 in model_data)
        }
    } else { // LSTM40Cond
        const float c = condition;
        for (int i = 0; i < frames; ++i) {
            const float x = buffer[i];
            const float in[2] = { x, c };
            const float y = cond_->forward(in);
            buffer[i] = y + x;
        }
    }

    // Simple one-pole DC blocker, R=0.995.
    constexpr float R = 0.995f;
    float x1 = dc_x1_, y1 = dc_y1_;
    for (int i = 0; i < frames; ++i) {
        const float x = buffer[i];
        const float y = x - x1 + R * y1;
        buffer[i] = y;
        x1 = x; y1 = y;
    }
    dc_x1_ = x1; dc_y1_ = y1;
}

} // namespace guitarml
