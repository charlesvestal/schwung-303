// 303 plugin_api_v2 entry point.
//
// Wires:
//   rosic::Open303 (monophonic 303 engine + Devilfish mods)
//     → drive::Drive (Soft tanh / RAT ProCo-style; user-selectable model)
//     → stereo int16 interleaved
//
// GPL-3.0, inherited from midilab/jc303 (whose Open303 core is MIT) and
// davemollen/dm-Rat (GPL-3.0) for the RAT drive model.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "open303/rosic_Open303.h"
#include "drive.h"

// ---------------------------------------------------------------------------
// Host/plugin API (v1+v2 inlined to avoid header path coupling)
// ---------------------------------------------------------------------------
extern "C" {

#define MOVE_PLUGIN_API_VERSION 1
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128
#define MOVE_MIDI_SOURCE_INTERNAL 0
#define MOVE_MIDI_SOURCE_EXTERNAL 2

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
    void *mod_emit_value;
    void *mod_clear_source;
    void *mod_host_ctx;
    float (*get_bpm)(void);
} host_api_v1_t;

#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

} // extern "C"

static const host_api_v1_t *g_host = nullptr;

// Open303's GlobalFunctions.h provides linToLin / linToExp globally (non-static).

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline int16_t float_to_s16(float x) {
    if (x >  1.0f) x =  1.0f;
    if (x < -1.0f) x = -1.0f;
    return static_cast<int16_t>(x * 32767.0f);
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------
struct P303 {
    rosic::Open303 engine;
    drive::Drive   drive_fx;

    std::string module_dir;

    // Normalized param state (0..1). Mirrors module.json defaults.
    float n_cutoff     = 0.5f;
    float n_resonance  = 0.5f;
    float n_env_mod    = 0.5f;
    float n_decay      = 0.5f;
    float n_accent     = 0.5f;
    float n_volume     = 0.8f;
    float n_tuning     = 0.5f;
    int   waveform     = 0;  // 0=saw, 1=square

    int   devil_mod_switch = 0;
    float n_normal_decay      = 0.5f;
    float n_accent_decay      = 0.5f;
    float n_feedback_hpf      = 0.5f;
    float n_soft_attack       = 0.0f;
    float n_slide_time        = 0.3f;
    float n_tanh_shaper_drive = 0.0f;

    int   drive_model = 0;      // 0=Soft, 1=RAT
    float n_drive     = 0.0f;   // 0 = bypass
    float n_drive_mix = 1.0f;

    // Decay range depends on devil_mod_switch (Devilfish: 30-3000, stock: 200-2000).
    double decayMin = 200.0;
    double decayMax = 2000.0;
};

// ---------------------------------------------------------------------------
// Parameter application
// ---------------------------------------------------------------------------
static void apply_waveform(P303 *p, int wf) {
    p->engine.setWaveform(wf ? 1.0 : 0.0);
}
static void apply_cutoff(P303 *p)    { p->engine.setCutoff(   linToExp(p->n_cutoff,    0, 1, 314.0, 2394.0)); }
static void apply_resonance(P303 *p) { p->engine.setResonance(linToLin(p->n_resonance, 0, 1,   0.0,  100.0)); }
static void apply_env_mod(P303 *p)   { p->engine.setEnvMod(   linToLin(p->n_env_mod,   0, 1,   0.0,  100.0)); }
static void apply_decay(P303 *p)     { p->engine.setDecay(    linToExp(p->n_decay,     0, 1, p->decayMin, p->decayMax)); }
static void apply_accent(P303 *p)    { p->engine.setAccent(   linToLin(p->n_accent,    0, 1,   0.0,  100.0)); }
static void apply_volume(P303 *p)    { p->engine.setVolume(   linToLin(p->n_volume,    0, 1, -60.0,    0.0)); }
static void apply_tuning(P303 *p)    { p->engine.setTuning(   linToLin(p->n_tuning,    0, 1, 400.0,  480.0)); }

static void apply_devil_mods(P303 *p) {
    if (p->devil_mod_switch) {
        p->decayMin = 30.0;
        p->decayMax = 3000.0;
        p->engine.setAmpDecay(         linToLin(p->n_normal_decay,      0, 1,  30.0, 3000.0));
        p->engine.setAccentDecay(      linToLin(p->n_accent_decay,      0, 1,  30.0, 3000.0));
        p->engine.setFeedbackHighpass( linToExp(p->n_feedback_hpf,      0, 1, 350.0,  100.0));
        p->engine.setNormalAttack(     linToExp(p->n_soft_attack,       0, 1,   0.3, 3000.0));
        p->engine.setSlideTime(        linToLin(p->n_slide_time,        0, 1,   2.0,  360.0));
        p->engine.setTanhShaperDrive(  linToLin(p->n_tanh_shaper_drive, 0, 1,  25.0,   80.0));
    } else {
        p->decayMin = 200.0;
        p->decayMax = 2000.0;
        p->engine.setAmpDecay(1230.0);
        p->engine.setAccentDecay(200.0);
        p->engine.setFeedbackHighpass(150.0);
        p->engine.setNormalAttack(3.0);
        p->engine.setSlideTime(60.0);
        p->engine.setTanhShaperDrive(36.9);
    }
    apply_decay(p);  // decayMin/Max changed
}

// ---------------------------------------------------------------------------
// v2 API
// ---------------------------------------------------------------------------
static void* v2_create(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;
    P303 *p = new P303();
    p->module_dir = module_dir ? module_dir : "";

    p->engine.setSampleRate(static_cast<double>(MOVE_SAMPLE_RATE));
    p->drive_fx.prepare(static_cast<double>(MOVE_SAMPLE_RATE));
    p->drive_fx.set_model(p->drive_model);

    apply_waveform(p, p->waveform);
    apply_cutoff(p);
    apply_resonance(p);
    apply_env_mod(p);
    apply_accent(p);
    apply_volume(p);
    apply_tuning(p);
    apply_devil_mods(p);  // sets decay range then calls apply_decay

    return p;
}

static void v2_destroy(void *instance) {
    delete static_cast<P303 *>(instance);
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    if (!instance || !msg || len < 2) return;
    P303 *p = static_cast<P303 *>(instance);
    const uint8_t status = msg[0] & 0xF0;
    if (status == 0x90) { // note-on (velocity 0 → note-off, handled inside Open303)
        const int note = msg[1] & 0x7F;
        const int vel  = (len >= 3) ? (msg[2] & 0x7F) : 0;
        p->engine.noteOn(note, vel, 0.0);
    } else if (status == 0x80) { // note-off
        const int note = msg[1] & 0x7F;
        p->engine.noteOn(note, 0, 0.0);
    } else if (status == 0xB0 && len >= 3) {
        // CC mapping honoring GM2/MPE conventions where applicable.
        const uint8_t cc  = msg[1] & 0x7F;
        const uint8_t val = msg[2] & 0x7F;
        const float nv = val / 127.0f;
        switch (cc) {
            case 74: p->n_cutoff    = nv; apply_cutoff(p);    break;
            case 71: p->n_resonance = nv; apply_resonance(p); break;
            case 70: p->n_env_mod   = nv; apply_env_mod(p);   break;
            case 75: p->n_decay     = nv; apply_decay(p);     break;
            case 16: p->n_accent    = nv; apply_accent(p);    break;
            case 7:  p->n_volume    = nv; apply_volume(p);    break;
            case 12: p->n_drive     = nv; break;
            case 13: p->n_drive_mix = nv; break;
            case 123: p->engine.allNotesOff(); break;
            default: break;
        }
    }
}

// Minimal JSON helpers for state blob parsing. Naive strstr — sufficient for
// our flat-object state format (no nested objects with colliding keys).
static int json_extract_number(const char *json, const char *key, double *out) {
    char search[64];
    std::snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = std::strstr(json, search);
    if (!pos) return -1;
    pos += std::strlen(search);
    while (*pos == ' ' || *pos == '\t') pos++;
    char *endp = nullptr;
    double v = std::strtod(pos, &endp);
    if (endp == pos) return -1;
    *out = v;
    return 0;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    if (!instance || !key || !val) return;
    P303 *p = static_cast<P303 *>(instance);

    // State restore: flat JSON object. devil_mod_switch must be applied first
    // because it resets decayMin/decayMax — otherwise a stored decay would be
    // remapped to the wrong range.
    if (!std::strcmp(key, "state")) {
        double d;
        char vbuf[32];

        if (json_extract_number(val, "devil_mod_switch", &d) == 0) {
            std::snprintf(vbuf, sizeof(vbuf), "%d", static_cast<int>(d));
            v2_set_param(instance, "devil_mod_switch", vbuf);
        }

        static const char *const float_keys[] = {
            "cutoff", "resonance", "env_mod", "decay", "accent", "volume", "tuning",
            "normal_decay", "accent_decay", "feedback_hpf", "soft_attack",
            "slide_time", "tanh_shaper_drive", "drive", "drive_mix",
            nullptr
        };
        for (int i = 0; float_keys[i]; i++) {
            if (json_extract_number(val, float_keys[i], &d) == 0) {
                std::snprintf(vbuf, sizeof(vbuf), "%.6f", d);
                v2_set_param(instance, float_keys[i], vbuf);
            }
        }

        static const char *const int_keys[] = { "waveform", "drive_model", nullptr };
        for (int i = 0; int_keys[i]; i++) {
            if (json_extract_number(val, int_keys[i], &d) == 0) {
                std::snprintf(vbuf, sizeof(vbuf), "%d", static_cast<int>(d));
                v2_set_param(instance, int_keys[i], vbuf);
            }
        }
        return;
    }

    const float fv = static_cast<float>(std::atof(val));

    if      (!std::strcmp(key, "cutoff"))     { p->n_cutoff    = fv; apply_cutoff(p); }
    else if (!std::strcmp(key, "resonance"))  { p->n_resonance = fv; apply_resonance(p); }
    else if (!std::strcmp(key, "env_mod"))    { p->n_env_mod   = fv; apply_env_mod(p); }
    else if (!std::strcmp(key, "decay"))      { p->n_decay     = fv; apply_decay(p); }
    else if (!std::strcmp(key, "accent"))     { p->n_accent    = fv; apply_accent(p); }
    else if (!std::strcmp(key, "volume"))     { p->n_volume    = fv; apply_volume(p); }
    else if (!std::strcmp(key, "tuning"))     { p->n_tuning    = fv; apply_tuning(p); }
    else if (!std::strcmp(key, "waveform"))   { p->waveform    = std::atoi(val); apply_waveform(p, p->waveform); }
    else if (!std::strcmp(key, "devil_mod_switch")) { p->devil_mod_switch = std::atoi(val); apply_devil_mods(p); }
    else if (!std::strcmp(key, "normal_decay")     ) { p->n_normal_decay      = fv; if (p->devil_mod_switch) apply_devil_mods(p); }
    else if (!std::strcmp(key, "accent_decay")     ) { p->n_accent_decay      = fv; if (p->devil_mod_switch) apply_devil_mods(p); }
    else if (!std::strcmp(key, "feedback_hpf")     ) { p->n_feedback_hpf      = fv; if (p->devil_mod_switch) apply_devil_mods(p); }
    else if (!std::strcmp(key, "soft_attack")      ) { p->n_soft_attack       = fv; if (p->devil_mod_switch) apply_devil_mods(p); }
    else if (!std::strcmp(key, "slide_time")       ) { p->n_slide_time        = fv; if (p->devil_mod_switch) apply_devil_mods(p); }
    else if (!std::strcmp(key, "tanh_shaper_drive")) { p->n_tanh_shaper_drive = fv; if (p->devil_mod_switch) apply_devil_mods(p); }
    else if (!std::strcmp(key, "drive_model"))      { p->drive_model = std::atoi(val); p->drive_fx.set_model(p->drive_model); }
    else if (!std::strcmp(key, "drive"))            { p->n_drive     = fv; }
    else if (!std::strcmp(key, "drive_mix"))        { p->n_drive_mix = fv; }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    if (!instance || !key || !buf || buf_len <= 0) return 0;
    P303 *p = static_cast<P303 *>(instance);

    auto emit = [&](float f) { return std::snprintf(buf, buf_len, "%.4f", f); };
    auto emit_i = [&](int i) { return std::snprintf(buf, buf_len, "%d", i); };

    // State save: serialize every param to a flat JSON object. Consumed by the
    // Shadow UI autosave / preset save to capture knob positions across reboots.
    if (!std::strcmp(key, "state")) {
        int n = 0;
        #define SA(...) do { if (n < buf_len) n += std::snprintf(buf + n, static_cast<size_t>(buf_len - n), __VA_ARGS__); } while (0)
        SA("{\"cutoff\":%.6f", static_cast<double>(p->n_cutoff));
        SA(",\"resonance\":%.6f", static_cast<double>(p->n_resonance));
        SA(",\"env_mod\":%.6f", static_cast<double>(p->n_env_mod));
        SA(",\"decay\":%.6f", static_cast<double>(p->n_decay));
        SA(",\"accent\":%.6f", static_cast<double>(p->n_accent));
        SA(",\"volume\":%.6f", static_cast<double>(p->n_volume));
        SA(",\"tuning\":%.6f", static_cast<double>(p->n_tuning));
        SA(",\"waveform\":%d", p->waveform);
        SA(",\"devil_mod_switch\":%d", p->devil_mod_switch);
        SA(",\"normal_decay\":%.6f", static_cast<double>(p->n_normal_decay));
        SA(",\"accent_decay\":%.6f", static_cast<double>(p->n_accent_decay));
        SA(",\"feedback_hpf\":%.6f", static_cast<double>(p->n_feedback_hpf));
        SA(",\"soft_attack\":%.6f", static_cast<double>(p->n_soft_attack));
        SA(",\"slide_time\":%.6f", static_cast<double>(p->n_slide_time));
        SA(",\"tanh_shaper_drive\":%.6f", static_cast<double>(p->n_tanh_shaper_drive));
        SA(",\"drive_model\":%d", p->drive_model);
        SA(",\"drive\":%.6f", static_cast<double>(p->n_drive));
        SA(",\"drive_mix\":%.6f", static_cast<double>(p->n_drive_mix));
        SA("%s", "}");
        #undef SA
        return n;
    }

    if (!std::strcmp(key, "cutoff"))     return emit(p->n_cutoff);
    if (!std::strcmp(key, "resonance"))  return emit(p->n_resonance);
    if (!std::strcmp(key, "env_mod"))    return emit(p->n_env_mod);
    if (!std::strcmp(key, "decay"))      return emit(p->n_decay);
    if (!std::strcmp(key, "accent"))     return emit(p->n_accent);
    if (!std::strcmp(key, "volume"))     return emit(p->n_volume);
    if (!std::strcmp(key, "tuning"))     return emit(p->n_tuning);
    if (!std::strcmp(key, "waveform"))   return emit_i(p->waveform);
    if (!std::strcmp(key, "devil_mod_switch"))  return emit_i(p->devil_mod_switch);
    if (!std::strcmp(key, "normal_decay"))      return emit(p->n_normal_decay);
    if (!std::strcmp(key, "accent_decay"))      return emit(p->n_accent_decay);
    if (!std::strcmp(key, "feedback_hpf"))      return emit(p->n_feedback_hpf);
    if (!std::strcmp(key, "soft_attack"))       return emit(p->n_soft_attack);
    if (!std::strcmp(key, "slide_time"))        return emit(p->n_slide_time);
    if (!std::strcmp(key, "tanh_shaper_drive")) return emit(p->n_tanh_shaper_drive);
    if (!std::strcmp(key, "drive_model"))       return emit_i(p->drive_model);
    if (!std::strcmp(key, "drive"))             return emit(p->n_drive);
    if (!std::strcmp(key, "drive_mix"))         return emit(p->n_drive_mix);

    // Shadow UI queries these directly on the DSP plugin.
    if (!std::strcmp(key, "ui_hierarchy")) {
        static const char *hierarchy =
            "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"name\":\"303\","
                    "\"children\":null,"
                    "\"knobs\":[\"cutoff\",\"resonance\",\"env_mod\",\"decay\","
                               "\"accent\",\"volume\",\"drive\",\"drive_mix\"],"
                    "\"params\":["
                        "\"cutoff\",\"resonance\",\"env_mod\",\"decay\","
                        "\"accent\",\"volume\",\"waveform\",\"tuning\","
                        "{\"level\":\"devilfish\",\"label\":\"Devilfish Mods\"},"
                        "{\"level\":\"drive\",\"label\":\"Drive\"}"
                    "]"
                "},"
                "\"devilfish\":{"
                    "\"name\":\"Devilfish Mods\","
                    "\"children\":null,"
                    "\"knobs\":[\"normal_decay\",\"accent_decay\",\"feedback_hpf\","
                               "\"soft_attack\",\"slide_time\",\"tanh_shaper_drive\"],"
                    "\"params\":[\"devil_mod_switch\",\"normal_decay\",\"accent_decay\","
                                "\"feedback_hpf\",\"soft_attack\",\"slide_time\",\"tanh_shaper_drive\"],"
                    "\"navigate_to\":\"root\""
                "},"
                "\"drive\":{"
                    "\"name\":\"Drive\","
                    "\"children\":null,"
                    "\"knobs\":[\"drive\",\"drive_mix\"],"
                    "\"params\":[\"drive_model\",\"drive\",\"drive_mix\"],"
                    "\"navigate_to\":\"root\""
                "}"
            "}"
            "}";
        const int len = static_cast<int>(std::strlen(hierarchy));
        if (len >= buf_len) return -1;
        std::memcpy(buf, hierarchy, len + 1);
        return len;
    }

    if (!std::strcmp(key, "chain_params")) {
        static const char *cp =
            "["
            "{\"key\":\"cutoff\",\"name\":\"Cutoff\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
            "{\"key\":\"resonance\",\"name\":\"Resonance\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
            "{\"key\":\"env_mod\",\"name\":\"Env Mod\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
            "{\"key\":\"decay\",\"name\":\"Decay\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
            "{\"key\":\"accent\",\"name\":\"Accent\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
            "{\"key\":\"volume\",\"name\":\"Volume\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.8},"
            "{\"key\":\"waveform\",\"name\":\"Waveform\",\"type\":\"enum\",\"options\":[\"Saw\",\"Square\"],\"default\":0},"
            "{\"key\":\"tuning\",\"name\":\"Tuning\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.5},"
            "{\"key\":\"devil_mod_switch\",\"name\":\"Devilfish\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"default\":0},"
            "{\"key\":\"normal_decay\",\"name\":\"Normal Decay\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
            "{\"key\":\"accent_decay\",\"name\":\"Accent Decay\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
            "{\"key\":\"feedback_hpf\",\"name\":\"Feedback HPF\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
            "{\"key\":\"soft_attack\",\"name\":\"Soft Attack\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0},"
            "{\"key\":\"slide_time\",\"name\":\"Slide Time\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.3},"
            "{\"key\":\"tanh_shaper_drive\",\"name\":\"Shaper Drive\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0},"
            "{\"key\":\"drive_model\",\"name\":\"Model\",\"type\":\"enum\",\"options\":[\"Soft\",\"RAT\"],\"default\":0},"
            "{\"key\":\"drive\",\"name\":\"Drive\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0},"
            "{\"key\":\"drive_mix\",\"name\":\"Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":1}"
            "]";
        const int len = static_cast<int>(std::strlen(cp));
        if (len >= buf_len) return -1;
        std::memcpy(buf, cp, len + 1);
        return len;
    }

    if (!std::strcmp(key, "name")) return std::snprintf(buf, buf_len, "303");

    return -1;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    (void)instance; (void)buf; (void)buf_len;
    return 0;
}

static void v2_render_block(void *instance, int16_t *out, int frames) {
    if (!instance || !out) {
        if (out) std::memset(out, 0, sizeof(int16_t) * 2 * frames);
        return;
    }
    P303 *p = static_cast<P303 *>(instance);

    // Open303 emits mono. Render into a stack buffer.
    float mono[MOVE_FRAMES_PER_BLOCK];
    if (frames > MOVE_FRAMES_PER_BLOCK) frames = MOVE_FRAMES_PER_BLOCK;
    for (int i = 0; i < frames; ++i)
        mono[i] = static_cast<float>(p->engine.getSample());

    // Drive stage (Soft tanh or RAT — fully bypassed at drive=0 or mix=0).
    p->drive_fx.process(mono, frames, p->n_drive, p->n_drive_mix);

    for (int i = 0; i < frames; ++i) {
        const int16_t s = float_to_s16(mono[i]);
        out[2 * i]     = s;
        out[2 * i + 1] = s;
    }
}

// ---------------------------------------------------------------------------
// Plugin entry
// ---------------------------------------------------------------------------
static plugin_api_v2_t g_api_v2 = {
    MOVE_PLUGIN_API_VERSION_2,
    v2_create,
    v2_destroy,
    v2_on_midi,
    v2_set_param,
    v2_get_param,
    v2_get_error,
    v2_render_block,
};

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_api_v2;
}
