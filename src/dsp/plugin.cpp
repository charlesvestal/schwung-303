// 303 plugin_api_v2 entry — scaffold.
//
// Implementation deferred: wires Open303 engine → GuitarML RNN overdrive.
// See docs/plans/2026-04-20-303-design.md in the schwung repo.
//
// For now this is a silent placeholder so the build scripts and install
// pipeline can be validated end-to-end before the DSP port lands.

#include <cstdint>
#include <cstring>
#include <cstdlib>

extern "C" {

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_lr, int frames);
} plugin_api_v2_t;

typedef struct host_api_v1 host_api_v1_t;

static void* p303_create(const char *, const char *) {
    // TODO: allocate Open303 + GuitarMLAmp, load default model.
    return std::calloc(1, 1);
}

static void p303_destroy(void *inst) {
    std::free(inst);
}

static void p303_on_midi(void *, const uint8_t *, int, int) {
    // TODO: route note on/off to Open303::noteOn/noteOff.
}

static void p303_set_param(void *, const char *, const char *) {
    // TODO: map keys to Open303 / GuitarMLAmp setters.
}

static int p303_get_param(void *, const char *, char *buf, int buf_len) {
    if (buf_len > 0) buf[0] = '\0';
    return 0;
}

static void p303_render_block(void *, int16_t *out_lr, int frames) {
    std::memset(out_lr, 0, sizeof(int16_t) * 2 * frames);
}

static plugin_api_v2_t g_api = {
    2,
    p303_create,
    p303_destroy,
    p303_on_midi,
    p303_set_param,
    p303_get_param,
    p303_render_block,
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *) {
    return &g_api;
}

} // extern "C"
