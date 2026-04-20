// 303 plugin_api_v2 entry point.
//
// Wires:
//   rosic::Open303 (monophonic 303 engine + Devilfish mods)
//     → guitarml::GuitarMLAmp (RNN overdrive, optional)
//     → dry/wet mix
//     → stereo int16 interleaved
//
// GPL-3.0, inherited from midilab/jc303 (whose Open303 core is MIT).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "open303/rosic_Open303.h"

#include "guitarml/guitarml_amp.h"

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
    guitarml::GuitarMLAmp overdrive;

    std::string module_dir;
    std::vector<std::string> model_list;  // relative paths without .json

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

    int   overdrive_switch     = 0;
    float n_overdrive_level    = 0.5f;
    float n_overdrive_dry_wet  = 0.5f;
    std::string overdrive_model;

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
// Model discovery
// ---------------------------------------------------------------------------
static void scan_models_dir(P303 *p, const std::string &dir, const std::string &prefix) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        const std::string dn(ent->d_name);
        if (!dn.empty() && dn[0] == '.') continue;
        const std::string full = dir + std::string("/") + dn;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            const std::string sub_prefix = prefix.empty() ? dn : (prefix + "/" + dn);
            scan_models_dir(p, full, sub_prefix);
        } else {
            const size_t n = dn.size();
            if (n > 5 && dn.compare(n - 5, 5, ".json") == 0) {
                const std::string name = dn.substr(0, n - 5);
                const std::string rel = prefix.empty() ? name : (prefix + "/" + name);
                p->model_list.push_back(rel);
            }
        }
    }
    closedir(d);
}

static bool load_model_by_name(P303 *p, const std::string &name) {
    if (name.empty()) return false;
    std::string path = p->module_dir + "/models/" + name + ".json";
    return p->overdrive.loadModelFromFile(path);
}

// ---------------------------------------------------------------------------
// v2 API
// ---------------------------------------------------------------------------
static void* v2_create(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;
    P303 *p = new P303();
    p->module_dir = module_dir ? module_dir : "";

    p->engine.setSampleRate(static_cast<double>(MOVE_SAMPLE_RATE));
    p->overdrive.prepare(static_cast<double>(MOVE_SAMPLE_RATE));

    apply_waveform(p, p->waveform);
    apply_cutoff(p);
    apply_resonance(p);
    apply_env_mod(p);
    apply_accent(p);
    apply_volume(p);
    apply_tuning(p);
    apply_devil_mods(p);  // sets decay range then calls apply_decay

    // Enumerate available models once; the list is stable across the session.
    if (!p->module_dir.empty()) {
        scan_models_dir(p, p->module_dir + "/models", std::string{});
        std::sort(p->model_list.begin(), p->model_list.end());
    }

    // Default model: prefer TS9_DriveKnob (classic tube screamer pairs well
    // with a 303), otherwise fall back to the first entry.
    if (!p->model_list.empty()) {
        p->overdrive_model = p->model_list.front();
        for (const auto &m : p->model_list) {
            if (m == "jc303/TS9_DriveKnob" || m == "proteus/TS9_DriveKnob") {
                p->overdrive_model = m;
                break;
            }
        }
        load_model_by_name(p, p->overdrive_model);
    }

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
    } else if (status == 0xB0 && len >= 3 && msg[1] == 123) { // CC 123 — all notes off
        p->engine.allNotesOff();
    }
    // ignore everything else (pitch bend / CC routed via set_param)
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    if (!instance || !key || !val) return;
    P303 *p = static_cast<P303 *>(instance);
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
    else if (!std::strcmp(key, "overdrive_switch"))   { p->overdrive_switch    = std::atoi(val); }
    else if (!std::strcmp(key, "overdrive_level"))    { p->n_overdrive_level   = fv; }
    else if (!std::strcmp(key, "overdrive_dry_wet"))  { p->n_overdrive_dry_wet = fv; }
    else if (!std::strcmp(key, "overdrive_model"))    {
        // Value can be either a model name (string path) or an integer index.
        std::string name;
        char *endp = nullptr;
        long idx = std::strtol(val, &endp, 10);
        if (endp != val && *endp == '\0' && idx >= 0 &&
            static_cast<size_t>(idx) < p->model_list.size()) {
            name = p->model_list[idx];
        } else {
            name = val;
        }
        if (load_model_by_name(p, name)) p->overdrive_model = name;
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    if (!instance || !key || !buf || buf_len <= 0) return 0;
    P303 *p = static_cast<P303 *>(instance);

    auto emit = [&](float f) { return std::snprintf(buf, buf_len, "%.4f", f); };
    auto emit_i = [&](int i) { return std::snprintf(buf, buf_len, "%d", i); };

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
    if (!std::strcmp(key, "overdrive_switch"))   return emit_i(p->overdrive_switch);
    if (!std::strcmp(key, "overdrive_level"))    return emit(p->n_overdrive_level);
    if (!std::strcmp(key, "overdrive_dry_wet"))  return emit(p->n_overdrive_dry_wet);
    if (!std::strcmp(key, "overdrive_model"))    return std::snprintf(buf, buf_len, "%s", p->overdrive_model.c_str());
    if (!std::strcmp(key, "overdrive_model_list")) {
        int pos = 0;
        pos += std::snprintf(buf + pos, buf_len - pos, "[");
        for (size_t i = 0; i < p->model_list.size() && pos < buf_len - 2; ++i) {
            if (i > 0) pos += std::snprintf(buf + pos, buf_len - pos, ",");
            pos += std::snprintf(buf + pos, buf_len - pos,
                "{\"label\":\"%s\",\"index\":%zu}", p->model_list[i].c_str(), i);
        }
        pos += std::snprintf(buf + pos, buf_len - pos, "]");
        return pos;
    }

    return 0;
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

    if (p->overdrive_switch && p->overdrive.isLoaded()) {
        float dry[MOVE_FRAMES_PER_BLOCK];
        std::memcpy(dry, mono, sizeof(float) * frames);

        // Pre-gain: push hotter into the RNN when level > 0.5.
        const float pre = std::pow(10.0f, (linToLin(p->n_overdrive_level, 0, 1, -12.0f, 18.0f)) / 20.0f);
        for (int i = 0; i < frames; ++i) mono[i] *= pre;

        p->overdrive.process(mono, frames, p->n_overdrive_level);

        const float wet = p->n_overdrive_dry_wet;
        const float dryg = 1.0f - wet;
        for (int i = 0; i < frames; ++i)
            mono[i] = dry[i] * dryg + mono[i] * wet;
    }

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
