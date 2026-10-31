/* SDL2 frontend: window/presentation, keyboard + game controller input (XInput/Xbox pads on
 * Windows, evdev on Linux), audio output and frame pacing.  Also implements the headless
 * test mode (scripted input + screenshots) used for automated verification. */
#include "psx.h"
#include "gpu.h"
#include "gte.h"
#include <SDL.h>
#ifndef _WIN32
#include "sdl_dyn.h"
#endif
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

uint16_t g_pad_buttons[2];
bool g_pad_connected[2];

static SDL_Window *win;
static void update_title(void);
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_AudioDeviceID audio;
static int audio_ok;
static uint32_t *frame_px;
static int fullscreen;
static uint64_t perf_next;
static int shot_request;
/* the second view (--p2 on one PC, p2view.c): a second window, following farmer 2 */
static SDL_Window *win2;
static SDL_Renderer *ren2;
static uint32_t *v2i_px;            /* farmer 2's window's in-between picture, when v2i_shown */
static int v2i_shown, v2i_w, v2i_h;
static SDL_Texture *tex2;
static uint32_t *frame2_px;
static int fullscreen2;

#define MAX_PADS 4
static SDL_GameController *pads[MAX_PADS];

/* ------------------------------------------------------------------ input bindings */
/* PS buttons in a fixed order; each can be bound to several keys and controller inputs. */
static const struct { const char *name; uint16_t bit; } ps_buttons[14] = {
    { "up", PAD_UP }, { "down", PAD_DOWN }, { "left", PAD_LEFT }, { "right", PAD_RIGHT },
    { "cross", PAD_CROSS }, { "circle", PAD_CIRCLE }, { "square", PAD_SQUARE }, { "triangle", PAD_TRIANGLE },
    { "l1", PAD_L1 }, { "r1", PAD_R1 }, { "l2", PAD_L2 }, { "r2", PAD_R2 },
    { "start", PAD_START }, { "select", PAD_SELECT },
};
#define MAX_BIND 6
static char bind_keys[14][128] = {
    "Up", "Down", "Left", "Right",
    "X,Space,K", "Z,L", "S,J", "A,I",
    "Q", "W", "1", "2",
    "Return", "Right Shift,Backspace",
};
/* SDL game controller names: a b x y back start leftshoulder rightshoulder dpup ...;
 * "lefttrigger"/"righttrigger" are analog triggers */
static char bind_pad[14][128] = {
    "dpup", "dpdown", "dpleft", "dpright",
    "a", "b", "x", "y",
    "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger",
    "start", "back",
};
static SDL_Scancode key_codes[14][MAX_BIND];
static int pad_codes[14][MAX_BIND];     /* SDL_GameControllerButton, or 1000+axis for triggers */
static int stick_as_dpad = 1;

bool platform_set_binding(const char *key, const char *val) {
    int is_key = !strncmp(key, "key_", 4), is_pad = !strncmp(key, "pad_", 4);
    if (!strcmp(key, "stick_as_dpad")) { stick_as_dpad = atoi(val); return true; }
    if (!is_key && !is_pad) return false;
    for (int i = 0; i < 14; i++) {
        if (!strcmp(key + 4, ps_buttons[i].name)) {
            snprintf(is_key ? bind_keys[i] : bind_pad[i], 128, "%s", val);
            return true;
        }
    }
    return false;
}

static void resolve_bindings(void) {
    for (int i = 0; i < 14; i++) {
        for (int k = 0; k < MAX_BIND; k++) { key_codes[i][k] = SDL_SCANCODE_UNKNOWN; pad_codes[i][k] = -1; }
        char buf[128], *save = NULL;
        snprintf(buf, sizeof buf, "%s", bind_keys[i]);
        int n = 0;
        for (char *t = strtok_r(buf, ",", &save); t && n < MAX_BIND; t = strtok_r(NULL, ",", &save)) {
            while (*t == ' ') t++;
            SDL_Scancode sc = SDL_GetScancodeFromName(t);
            if (sc == SDL_SCANCODE_UNKNOWN) LOGW("unknown key name '%s' for %s", t, ps_buttons[i].name);
            else key_codes[i][n++] = sc;
        }
        snprintf(buf, sizeof buf, "%s", bind_pad[i]);
        n = 0;
        for (char *t = strtok_r(buf, ",", &save); t && n < MAX_BIND; t = strtok_r(NULL, ",", &save)) {
            while (*t == ' ') t++;
            if (!strcmp(t, "lefttrigger")) { pad_codes[i][n++] = 1000 + SDL_CONTROLLER_AXIS_TRIGGERLEFT; continue; }
            if (!strcmp(t, "righttrigger")) { pad_codes[i][n++] = 1000 + SDL_CONTROLLER_AXIS_TRIGGERRIGHT; continue; }
            SDL_GameControllerButton gb = SDL_GameControllerGetButtonFromString(t);
            if (gb == SDL_CONTROLLER_BUTTON_INVALID) LOGW("unknown controller button '%s' for %s", t, ps_buttons[i].name);
            else pad_codes[i][n++] = gb;
        }
    }
}

/* ------------------------------------------------------------------ input script (testing) */
typedef struct { int frame, dur, end, period, p2; uint16_t btn; } ScriptEv;   /* p2: farmer 2's buttons */
/* the cheats changed while playing ("cheats@FRAME=money:stamina:clock:rucksack:materials:tools" in a
 * recording; older ones have the first four) */
#define CHEAT_EVENTS 256
static struct { int frame, v[CHEATS]; } cheat_ev[CHEAT_EVENTS];
static int n_cheat_ev;
static ScriptEv *script;
static int nscript, script_cap;

static const struct { const char *n; uint16_t b; } names[] = {
    { "UP", PAD_UP }, { "DOWN", PAD_DOWN }, { "LEFT", PAD_LEFT }, { "RIGHT", PAD_RIGHT },
    { "CROSS", PAD_CROSS }, { "X", PAD_CROSS }, { "CIRCLE", PAD_CIRCLE }, { "O", PAD_CIRCLE },
    { "SQUARE", PAD_SQUARE }, { "TRIANGLE", PAD_TRIANGLE }, { "L1", PAD_L1 }, { "R1", PAD_R1 },
    { "L2", PAD_L2 }, { "R2", PAD_R2 }, { "START", PAD_START }, { "SELECT", PAD_SELECT },
};

static uint16_t parse_buttons(const char *s) {
    uint16_t b = 0;
    char tok[32];
    while (*s) {
        int n = 0;
        while (*s && *s != '+' && n < 31) tok[n++] = (char)toupper((unsigned char)*s++);
        tok[n] = 0;
        if (*s == '+') s++;
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(tok, names[i].n)) b |= names[i].b;
    }
    return b;
}

static void parse_script(const char *s) {
    /* "frame:BUTTONS[:dur],..." or repeating "start-end/period:BUTTONS[:dur]", farmer 2's (--p2)
     * with "p2 " before; "@FILE" reads the events from a file, one per line (e.g. made with --record) */
    char *buf;
    if (s[0] == '@') {
        FILE *f = fopen(s + 1, "rb");
        long n;
        if (!f) { LOGE("cannot open input script %s", s + 1); return; }
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = calloc((size_t)n + 1, 1);
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) LOGE("short read of %s", s + 1);
        fclose(f);
        for (char *p = buf; *p; p++) if (*p == '\n' || *p == '\r') *p = ',';
    } else {
        buf = strdup(s);
    }
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        ScriptEv e = { 0, 6, 0, 0, 0, 0 };
        if (!strncmp(tok, "p2 ", 3)) { e.p2 = 1; tok += 3; }
        if (!strncmp(tok, "fastcd=", 7)) { g_cfg.fast_cd = atoi(tok + 7); continue; }   /* from --record */
        if (!strncmp(tok, "widescreen=", 11)) {     /* from --record: widescreen changes what the game does */
            if (sscanf(tok + 11, "%d:%d", &g_cfg.ws_w, &g_cfg.ws_h) != 2) g_cfg.ws_w = g_cfg.ws_h = 0;
            continue;
        }
        if (!strncmp(tok, "autoslot=", 9)) { g_cfg.autoslot = atoi(tok + 9); continue; }
        if (!strncmp(tok, "quickui=", 8)) { g_cfg.quickui = atoi(tok + 8); continue; }
        if (!strncmp(tok, "skipscenes=", 11)) { g_cfg.skipscenes = atoi(tok + 11); continue; }
        if (!strncmp(tok, "textspeed=", 10)) { g_cfg.textspeed = atoi(tok + 10); continue; }
        if (!strncmp(tok, "emptybag=", 9)) { g_cfg.emptybag = atoi(tok + 9); continue; }
        if (!strncmp(tok, "cheats@", 7) && n_cheat_ev < CHEAT_EVENTS) {
            int *v = cheat_ev[n_cheat_ev].v;
            if (sscanf(tok + 7, "%d=%d:%d:%d:%d:%d:%d", &cheat_ev[n_cheat_ev].frame, &v[0], &v[1], &v[2], &v[3],
                       &v[4], &v[5]) >= 5)
                n_cheat_ev++;
            continue;
        }
        if (!strncmp(tok, "cheats=", 7)) {      /* from --record: the cheats change what the game does */
            sscanf(tok + 7, "%d:%d:%d:%d:%d:%d", &g_cfg.cheat_money, &g_cfg.cheat_stamina, &g_cfg.cheat_clock,
                   &g_cfg.cheat_bag, &g_cfg.cheat_items, &g_cfg.cheat_tools);
            continue;
        }
        if (!strncmp(tok, "overclock=", 10)) { g_cfg.overclock = atoi(tok + 10); g_cycle_scale = 256u * 100u / (uint32_t)(g_cfg.overclock < 100 ? 100 : g_cfg.overclock); continue; }
        char *c1 = strchr(tok, ':');
        if (!c1) continue;
        *c1 = 0;
        char *dash = strchr(tok, '-'), *slash = strchr(tok, '/');
        e.frame = atoi(tok);
        if (dash && slash) { e.end = atoi(dash + 1); e.period = atoi(slash + 1); }
        char *c2 = strchr(c1 + 1, ':');
        if (c2) { *c2 = 0; e.dur = atoi(c2 + 1); }
        e.btn = parse_buttons(c1 + 1);
        if (nscript == script_cap) {
            script_cap = script_cap ? script_cap * 2 : 256;
            script = realloc(script, (size_t)script_cap * sizeof *script);
        }
        script[nscript++] = e;
    }
    free(buf);
}

/* testing: --record FILE writes each run of identical pad state as "frame:BUTTONS:frames"
 * (farmer 2's, with --p2, as "p2 frame:BUTTONS:frames"), which --input @FILE replays (the
 * emulation is deterministic, so the replay is the session). The first lines are the settings
 * that change what the game does. */
static FILE *rec_file;
static struct { uint16_t btn; int start, last; } rec[2];   /* the player's, farmer 2's */

static void rec_write(int who, int end) {
    uint16_t done = 0;
    if (!rec[who].btn) return;
    fprintf(rec_file, "%s%d:", who ? "p2 " : "", rec[who].start);
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if ((rec[who].btn & names[i].b) && !(done & names[i].b)) {
            fprintf(rec_file, "%s%s", done ? "+" : "", names[i].n);
            done |= names[i].b;
        }
    fprintf(rec_file, ":%d\n", end - rec[who].start);
    fflush(rec_file);
}

static void record_pad(int who, int frame, uint16_t b) {
    if (!rec_file) return;
    rec[who].last = frame;
    if (b == rec[who].btn) return;
    rec_write(who, frame);
    rec[who].btn = b;
    rec[who].start = frame;
}

/* the cheats at a frame boundary: a recording's changes, or (playing) the ini's when the F5 page
 * saved it, which go into the recording */
static void cheat_events(int frame) {
    for (int i = 0; i < n_cheat_ev; i++)
        if (cheat_ev[i].frame == frame) cheats_set(cheat_ev[i].v);
    int v[CHEATS];
    if (g_cfg.headless || g_cfg.input_script[0] || netplay_active() || frame % 30 || !config_cheats(v)) return;
    if (v[0] == g_cfg.cheat_money && v[1] == g_cfg.cheat_stamina && v[2] == g_cfg.cheat_clock && v[3] == g_cfg.cheat_bag &&
        v[4] == g_cfg.cheat_items && v[5] == g_cfg.cheat_tools)
        return;
    cheats_set(v);
    LOGI("cheats: money %d, stamina %d, clock %d, rucksack %d, materials %d, tools %d", v[0], v[1], v[2], v[3], v[4], v[5]);
    if (rec_file) {
        fprintf(rec_file, "cheats@%d=%d:%d:%d:%d:%d:%d\n", frame, v[0], v[1], v[2], v[3], v[4], v[5]);
        fflush(rec_file);
    }
}

static void record_frame(int frame, uint16_t b) {
    record_pad(0, frame, b);
    if (g_p2_enabled) record_pad(1, frame, g_p2_buttons);
}

static void record_close(void) {
    if (!rec_file) return;
    rec_write(0, rec[0].last + 1);
    rec_write(1, rec[1].last + 1);
    fclose(rec_file);
    rec_file = NULL;
}

static uint16_t script_buttons(int frame);
static uint16_t script_pad(int frame, int p2);
/* testing: pseudo-random button presses that still let the game make progress */
static uint16_t fuzz_buttons(int frame) {
    static uint32_t rng;
    static uint16_t cur;
    static int hold;
    if (!g_cfg.fuzz_seed || frame < g_cfg.fuzz_start) return 0;
    if (!rng) rng = (uint32_t)g_cfg.fuzz_seed * 2654435761u + 1;
    if (--hold > 0) return cur;
    rng = rng * 1664525u + 1013904223u;
    uint32_t r = rng >> 8;
    static const uint16_t dirs[4] = { PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT };
    static const uint16_t acts[8] = { PAD_CROSS, PAD_CROSS, PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE, PAD_L1, PAD_R1 };
    cur = 0;
    switch (r % 10) {
    case 0: case 1: case 2: case 3: cur = dirs[(r >> 4) & 3]; hold = 10 + (int)((r >> 8) % 50); break;
    case 4: case 5: case 6: cur = acts[(r >> 4) & 7]; hold = 3; break;
    case 7: cur = dirs[(r >> 4) & 3] | PAD_CROSS; hold = 8; break;
    case 8: cur = ((r >> 4) % 23 == 0) ? PAD_START : (((r >> 4) % 31 == 0) ? PAD_SELECT : 0); hold = 4; break;
    default: cur = 0; hold = 10; break;
    }
    return cur;
}

static uint16_t script_buttons(int frame) {
    return (uint16_t)(fuzz_buttons(frame) | script_pad(frame, 0));
}

static uint16_t script_pad(int frame, int p2) {
    uint16_t b = 0;
    for (int i = 0; i < nscript; i++) {
        const ScriptEv *e = &script[i];
        if (e->p2 != p2) continue;
        if (e->period > 0) {
            if (frame >= e->frame && frame <= e->end && (frame - e->frame) % e->period < e->dur) b |= e->btn;
        } else if (frame >= e->frame && frame < e->frame + e->dur) {
            b |= e->btn;
        }
    }
    return b;
}

/* ------------------------------------------------------------------ screenshots */
static void write_bmp(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t rowb = (uint32_t)w * 3, pad = (4 - rowb % 4) % 4, img = (rowb + pad) * (uint32_t)h;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t fsz = 54 + img;
    memcpy(hdr + 2, &fsz, 4);
    uint32_t v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    int32_t ww = w, hh = h;
    memcpy(hdr + 18, &ww, 4);
    memcpy(hdr + 22, &hh, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    static uint8_t row[8192 * 3 + 4];   /* widest frame: 1024 at upscale 4, widened for widescreen */
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t p = px[y * w + x];
            row[x * 3 + 0] = (uint8_t)p;
            row[x * 3 + 1] = (uint8_t)(p >> 8);
            row[x * 3 + 2] = (uint8_t)(p >> 16);
        }
        fwrite(row, 1, rowb + pad, f);
    }
    fclose(f);
}

/* A widescreen picture as it is shown: widened by 1 / squeeze, linear between columns. */
static uint32_t *widen(const uint32_t *px, int w, int h, int *ow) {
    int W = (int)lrint((double)w * 3.0 * g_cfg.ws_w / (4.0 * g_cfg.ws_h));
    uint32_t *out;
    if (W > 8192) W = 8192;
    out = malloc((size_t)W * (size_t)h * 4);
    if (!out) return NULL;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = px + (size_t)y * w;
        for (int x = 0; x < W; x++) {
            double sx = ((double)x + 0.5) * w / W - 0.5;
            int x0 = sx < 0 ? 0 : (int)sx, x1 = x0 + 1 < w ? x0 + 1 : w - 1;
            uint32_t f = (uint32_t)((sx < 0 ? 0 : sx - x0) * 256.0), a = row[x0], b = row[x1], c = 0xFF000000u;
            for (int k = 0; k < 24; k += 8)
                c |= ((((a >> k) & 255u) * (256u - f) + ((b >> k) & 255u) * f) >> 8) << k;
            out[(size_t)y * W + x] = c;
        }
    }
    *ow = W;
    return out;
}

static void save_screenshot_as(const char *name, const uint32_t *px, int w, int h, uint64_t frame, int wide) {
    char path[1400];
    uint32_t *wpx = wide ? widen(px, w, h, &w) : NULL;
#ifdef _WIN32
    _mkdir(g_cfg.screenshot_dir);
#else
    mkdir(g_cfg.screenshot_dir, 0755);
#endif
    snprintf(path, sizeof path, "%s/%s_%06llu.bmp", g_cfg.screenshot_dir, name, (unsigned long long)frame);
    write_bmp(path, wpx ? wpx : px, w, h);
    free(wpx);
    LOGI("screenshot %s (%dx%d)", path, w, h);
}

static void save_screenshot(const uint32_t *px, int w, int h, uint64_t frame, int wide) {
    save_screenshot_as("frame", px, w, h, frame, wide);
    if (g_view2 > 0) {                  /* and farmer 2's window: frame2_... */
        static uint32_t *px2;
        static int w2, h2;
        int up = gpu_upscale();
        if (!px2) px2 = calloc((size_t)1024 * 512 * up * up, sizeof(uint32_t));
        if (!px2) return;
        if (p2view_swapped()) gpu_render_display(px2, &w2, &h2);              /* as the window */
        else if (v2i_shown) { memcpy(px2, v2i_px, (size_t)v2i_w * v2i_h * 4); w2 = v2i_w; h2 = v2i_h; }
        else if (!w2 || !p2view_held()) view2_gpu_render_display(px2, &w2, &h2);
        save_screenshot_as("frame2", px2, w2, h2, frame, wide);
    }
}

/* ------------------------------------------------------------------ init */
static void open_pad(int index) {
    if (!SDL_IsGameController(index)) return;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i]) {
            pads[i] = SDL_GameControllerOpen(index);
            if (pads[i]) LOGI("controller connected: %s", SDL_GameControllerName(pads[i]));
            return;
        }
    }
}

/* Open the default output device.  Skipped when the system has no output device at all
 * (WASAPI would otherwise stall for seconds); a device plugged in later is picked up via
 * SDL_AUDIODEVICEADDED. */
static void open_audio(void) {
    if (audio_ok) return;
    if (SDL_GetNumAudioDevices(0) <= 0) {
        LOGW("no audio output device found - running without sound until one is connected");
        return;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) {
        audio_ok = 1;
        SDL_PauseAudioDevice(audio, 0);
        LOGI("audio: %s", SDL_GetCurrentAudioDriver());
    } else {
        LOGW("cannot open audio device: %s", SDL_GetError());
    }
}

int platform_init(void) {
    int up = g_cfg.upscale > 1 ? g_cfg.upscale : 1;
    frame_px = calloc((size_t)1024 * 512 * up * up, sizeof(uint32_t));
    if (g_cfg.input_script[0]) parse_script(g_cfg.input_script);
    if (g_cfg.record_path[0]) {
        rec_file = fopen(g_cfg.record_path, "w");
        if (rec_file) {
            fprintf(rec_file, "fastcd=%d\n", g_cfg.fast_cd);   /* changes loading timing: the replay needs it */
            fprintf(rec_file, "autoslot=%d\n", g_cfg.autoslot);   /* skips a menu: the replay needs it too */
            fprintf(rec_file, "quickui=%d\n", g_cfg.quickui);     /* shortens animations: so does this */
            fprintf(rec_file, "widescreen=%d:%d\n", g_cfg.ws_w, g_cfg.ws_h);   /* changes what the game does too */
            fprintf(rec_file, "skipscenes=%d\n", g_cfg.skipscenes);
            fprintf(rec_file, "textspeed=%d\n", g_cfg.textspeed);
            fprintf(rec_file, "overclock=%d\n", g_cfg.overclock);
            fprintf(rec_file, "emptybag=%d\n", g_cfg.emptybag);
            fprintf(rec_file, "cheats=%d:%d:%d:%d:%d:%d\n", g_cfg.cheat_money, g_cfg.cheat_stamina, g_cfg.cheat_clock,
                    g_cfg.cheat_bag, g_cfg.cheat_items, g_cfg.cheat_tools);
            atexit(record_close);
        }
        else LOGE("cannot write %s", g_cfg.record_path);
    }
    if (g_cfg.headless) return 0;
#ifndef _WIN32
    const char *err = sdl_dyn_load();
    if (err) { fprintf(stderr, "%s\n", err); return 1; }
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    resolve_bindings();
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");          /* pacing is done by the emulated video clock */
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    int sc = g_cfg.scale > 0 ? g_cfg.scale : 3;
    int win_w = g_cfg.ws_w ? 240 * sc * g_cfg.ws_w / g_cfg.ws_h : 320 * sc;
    win = SDL_CreateWindow("Harvest Moon: Back to Nature", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           win_w, 240 * sc, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    if (g_view2 > 0) {
        /* farmer 2's window: the two side by side when they fit */
        SDL_Rect r;
        int x2 = SDL_WINDOWPOS_UNDEFINED, y2 = SDL_WINDOWPOS_UNDEFINED;
        if (SDL_GetDisplayUsableBounds(0, &r) == 0 && 2 * win_w + 16 <= r.w) {
            int x1 = r.x + (r.w - 2 * win_w - 16) / 2;
            y2 = r.y + (r.h - 240 * sc) / 2;
            SDL_SetWindowPosition(win, x1, y2);
            x2 = x1 + win_w + 16;
        }
        win2 = SDL_CreateWindow("Harvest Moon: Back to Nature  -  farmer 2", x2, y2, win_w, 240 * sc,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (win2) ren2 = SDL_CreateRenderer(win2, -1, SDL_RENDERER_ACCELERATED);
        if (win2 && !ren2) ren2 = SDL_CreateRenderer(win2, -1, SDL_RENDERER_SOFTWARE);
        if (ren2) tex2 = SDL_CreateTexture(ren2, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 1024 * up, 512 * up);
        frame2_px = calloc((size_t)1024 * 512 * up * up, sizeof(uint32_t));
        if (!tex2 || !frame2_px) LOGW("second view: no window (%s)", SDL_GetError());
        SDL_RaiseWindow(win);
    }
    update_title();
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 1024 * up, 512 * up);
    if (g_cfg.fullscreen) { SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP); fullscreen = 1; }

    open_audio();

    for (int i = 0; i < SDL_NumJoysticks(); i++) open_pad(i);
    perf_next = SDL_GetPerformanceCounter();
    LOGI("platform init took %.0f ms", (double)(perf_next - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
    return 0;
}

void platform_shutdown(void) {
    if (g_cfg.headless) return;
    if (audio) SDL_CloseAudioDevice(audio);
    SDL_Quit();
}

void platform_error_box(const char *title, const char *msg) {
    if (g_cfg.headless || !win) return;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, win);
}

static FILE *wav_file;
static uint32_t wav_frames;

static void wav_finish(void) {
    if (!wav_file) return;
    uint32_t data = wav_frames * 4, riff = 36 + data;
    fseek(wav_file, 4, SEEK_SET); fwrite(&riff, 4, 1, wav_file);
    fseek(wav_file, 40, SEEK_SET); fwrite(&data, 4, 1, wav_file);
    fclose(wav_file);
    wav_file = NULL;
}

void platform_audio_push(const int16_t *stereo, int frames) {
    if (g_cfg.wav_path[0]) {
        if (!wav_file) {
            wav_file = fopen(g_cfg.wav_path, "wb");
            if (wav_file) {
                static const uint8_t hdr[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                    0x44,0xAC,0,0,0x10,0xB1,2,0,4,0,16,0,'d','a','t','a',0,0,0,0 };
                fwrite(hdr, 1, 44, wav_file);
                atexit(wav_finish);
            }
        }
        if (wav_file) { fwrite(stereo, 4, (size_t)frames, wav_file); wav_frames += (uint32_t)frames; }
    }
    if (audio_ok && !g_cfg.headless && !g_cfg.unthrottled) SDL_QueueAudio(audio, stereo, (Uint32)frames * 4);
}

/* ------------------------------------------------------------------ per-frame */
/* with a second farmer (--p2): the second controller and the numeric keypad are its pad (8 4 5 6
 * the directions, 7 Square, 9 Cross, 1 Circle, 3 R1, + R2, / L1, * L2, 0 Triangle, Enter Start,
 * . Select) */
static uint16_t p2_pad;

static uint16_t read_controllers(void) {
    uint16_t b = 0, p2 = 0;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < 14; i++)
        for (int k = 0; k < MAX_BIND && key_codes[i][k] != SDL_SCANCODE_UNKNOWN; k++)
            if (ks[key_codes[i][k]]) b |= ps_buttons[i].bit;
    if (ks[SDL_SCANCODE_KP_8]) p2 |= PAD_UP;
    if (ks[SDL_SCANCODE_KP_5] || ks[SDL_SCANCODE_KP_2]) p2 |= PAD_DOWN;
    if (ks[SDL_SCANCODE_KP_4]) p2 |= PAD_LEFT;
    if (ks[SDL_SCANCODE_KP_6]) p2 |= PAD_RIGHT;
    if (ks[SDL_SCANCODE_KP_7]) p2 |= PAD_SQUARE;
    if (ks[SDL_SCANCODE_KP_9]) p2 |= PAD_CROSS;
    if (ks[SDL_SCANCODE_KP_1]) p2 |= PAD_CIRCLE;
    if (ks[SDL_SCANCODE_KP_3]) p2 |= PAD_R1;
    if (ks[SDL_SCANCODE_KP_0]) p2 |= PAD_TRIANGLE;
    if (ks[SDL_SCANCODE_KP_PLUS]) p2 |= PAD_R2;
    if (ks[SDL_SCANCODE_KP_DIVIDE]) p2 |= PAD_L1;
    if (ks[SDL_SCANCODE_KP_MULTIPLY]) p2 |= PAD_L2;
    if (ks[SDL_SCANCODE_KP_ENTER]) p2 |= PAD_START;
    if (ks[SDL_SCANCODE_KP_PERIOD]) p2 |= PAD_SELECT;
    for (int p = 0; p < MAX_PADS; p++) {
        SDL_GameController *c = pads[p];
        if (!c) continue;
        uint16_t mine = b;
        if (g_p2_enabled && p == 1) b = p2;     /* this controller's buttons go to farmer 2 */
        for (int i = 0; i < 14; i++) {
            for (int k = 0; k < MAX_BIND && pad_codes[i][k] >= 0; k++) {
                int code = pad_codes[i][k];
                if (code >= 1000) { if (SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)(code - 1000)) > 16000) b |= ps_buttons[i].bit; }
                else if (SDL_GameControllerGetButton(c, (SDL_GameControllerButton)code)) b |= ps_buttons[i].bit;
            }
        }
        if (stick_as_dpad) {
            int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
            int ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
            const int dz = 16000;
            if (lx < -dz) b |= PAD_LEFT;
            if (lx > dz) b |= PAD_RIGHT;
            if (ly < -dz) b |= PAD_UP;
            if (ly > dz) b |= PAD_DOWN;
        }
        if (g_p2_enabled && p == 1) { p2 = b; b = mine; }
    }
    p2_pad = p2;
    return b;
}

static double t_upload, t_present;
static int t_n;

/* sharp-bilinear: the frame enlarged by whole numbers (render target of exactly that size) */
static SDL_Texture *pre;
static int pre_w, pre_h, pre_ok = 1;

static int prescale_target(int w, int h) {
    if (pre && pre_w == w && pre_h == h) return 1;
    if (pre) SDL_DestroyTexture(pre);
    pre = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
    if (!pre) { LOGW("sharp-bilinear: %s - using smooth scaling", SDL_GetError()); pre_ok = 0; return 0; }
    SDL_SetTextureScaleMode(pre, SDL_ScaleModeLinear);
    pre_w = w;
    pre_h = h;
    return 1;
}

/* Widescreen: the GTE squeezes the 3D world into the picture, which is shown stretched to the
 * wider aspect. The title screen and the full-screen menus are wide too: the GPU repeats their
 * background at the sides and keeps the rest 4:3 in the middle (upscale 2+). Other screens the
 * game draws without 3D (videos, the map, ...) are shown as they are, at 4:3: the picture is wide
 * while the game projected a 3D scene's worth of vertices in one of the last few frames (it may
 * draw only every other frame; with a full-screen menu open it still projects a vertex or two,
 * against thousands for a room) and the display isn't 24-bit video. Leaving an area, the game
 * stops drawing 3D and fades out the last picture, then shows black or "Now Loading": all of that
 * stays wide, so going from one area to the next never flashes to 4:3. Only a picture that is
 * mostly not black, without 3D, after a black one (a screen fading in from black, from its first
 * frame) switches to 4:3, and any picture without 3D after a menu. */
static int display_dark(const GpuDisplay *d) {
    const uint16_t *vram = gpu_vram();
    int dark = 0;
    if (!d->enabled) return 1;
    for (int y = 0; y < d->height; y++) {
        const uint16_t *row = vram + (((d->vram_y + y) & 511) << 10);
        for (int x = 0; x < d->width; x++)
            dark += (row[(d->vram_x + x) & 1023] & 0x7BDE) == 0;    /* red, green and blue 0 or 1 */
    }
    return dark * 10 >= d->width * d->height * 9;
}

/* The game is showing a 3D scene: it projected 32 or more vertices since the last frame, or in
 * one of the 10 frames before. Also asked when the GPU draws the HUD, which is kept at 4:3
 * (squeezed back) only in 3D scenes, the pictures shown wide. */
static uint32_t proj_mark;          /* projections counted up to the last frame */
static int hold_3d;                 /* frames left in which the last 3D scene still counts */

int platform_3d_scene(void) {
    return gte_projections() - proj_mark >= 32 || hold_3d > 0;
}

static int display_wide(void) {
    static int was_wide, blank;      /* blank: black was shown since the 3D stopped */
    uint32_t n = gte_projections();
    GpuDisplay d;
    int wide;
    if (!g_cfg.ws_w) return 0;
    if (n - proj_mark >= 32) hold_3d = 10;
    else if (hold_3d > 0) hold_3d--;
    proj_mark = n;
    gpu_get_display(&d);
    if (d.is_24bit) wide = 0;                           /* video */
    else if (gpu_display_menu()) wide = 2;              /* a full-screen menu or the title screen */
    else if (hold_3d > 0) { wide = 1; blank = 0; }
    else if (was_wide != 1) wide = 0;                   /* not after a menu either ("Now Loading") */
    else if (display_dark(&d)) { wide = 1; blank = 1; }
    else wide = !blank;
    if (!wide != !was_wide) LOGD("display: %s", wide ? "widescreen" : "4:3 (no 3D)");
    was_wide = wide;
    return wide != 0;
}

/* width:height of the picture on screen */
static double display_aspect(int w, int h, int wide) {
    double aspect = g_cfg.aspect_4_3 ? 4.0 / 3.0 : (double)w / h;
    return wide ? aspect * (3.0 * g_cfg.ws_w) / (4.0 * g_cfg.ws_h) : aspect;
}

static void present(int w, int h, int wide, bool same) {
    uint64_t f = SDL_GetPerformanceFrequency(), t0 = SDL_GetPerformanceCounter();
    int ww, wh;
    SDL_Rect src = { 0, 0, w, h }, dst = { 0, 0, 0, 0 };
    SDL_Texture *show = tex;
    SDL_GetRendererOutputSize(ren, &ww, &wh);
    if (w > 0 && h > 0) {
        if (!same) SDL_UpdateTexture(tex, &src, frame_px, w * 4);   /* same: tex holds it already */
        double aspect = display_aspect(w, h, wide);
        int dw = ww, dh = (int)(ww / aspect);
        if (dh > wh) { dh = wh; dw = (int)(wh * aspect); }
        dst = (SDL_Rect){ (ww - dw) / 2, (wh - dh) / 2, dw, dh };
        SDL_SetTextureScaleMode(tex, g_cfg.scaling == SCALING_SHARP ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
        if (g_cfg.scaling == SCALING_SHARP_BILINEAR && pre_ok) {
            /* enlarge by whole numbers with sharp pixels to at least the output size, then let the
             * smooth copy do the rest: every source pixel gets the same width on screen (no uneven
             * pixels sliding over sprites when the picture scrolls), with a 1-pixel blend at edges */
            int kx = (dw + w - 1) / w, ky = (dh + h - 1) / h;
            while (kx > 1 && kx * w > 8192) kx--;
            while (ky > 1 && ky * h > 8192) ky--;
            if ((kx > 1 || ky > 1) && prescale_target(kx * w, ky * h)) {
                SDL_Rect pr = { 0, 0, kx * w, ky * h };
                SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
                SDL_SetRenderTarget(ren, pre);
                SDL_RenderCopy(ren, tex, &src, &pr);
                SDL_SetRenderTarget(ren, NULL);
                src = pr;
                show = pre;
            }
        }
    }
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (w > 0 && h > 0) SDL_RenderCopy(ren, show, &src, &dst);
    uint64_t t1 = SDL_GetPerformanceCounter();
    SDL_RenderPresent(ren);
    uint64_t t2 = SDL_GetPerformanceCounter();
    t_upload += (double)(t1 - t0) * 1000.0 / (double)f;
    t_present += (double)(t2 - t1) * 1000.0 / (double)f;
    if (++t_n == 120) {
        SDL_RendererInfo ri;
        SDL_GetRendererInfo(ren, &ri);
        static uint64_t last;
        uint64_t nowt = SDL_GetPerformanceCounter();
        double fps = last ? 120.0 * (double)f / (double)(nowt - last) : 0;
        last = nowt;
        LOGD("present: renderer=%s upload %.2f ms, present %.2f ms (avg), %.2f fps", ri.name, t_upload / t_n, t_present / t_n, fps);
        t_upload = t_present = 0;
        t_n = 0;
    }
}

/* farmer 2's window: the second GPU's picture, scaled smoothly (or sharply) to fit. A picture kept
 * (the player's menu) keeps the shape it was made with. Shown again only when it changed (the game
 * draws every other VBlank) or the window did (win2_due) */
static int win2_due = 1;            /* 2: the texture may have lost the picture too */
static void present2(int wide_now) {
    static uint32_t changes;
    static int w, h, valid, wide;
    int ww, wh, fresh = win2_due;
    if (!tex2) return;
    win2_due = 0;
    if (p2view_swapped()) {             /* farmer 2 has the game: the game's picture is farmer 2's */
        gpu_render_display(frame2_px, &w, &h);
        wide = wide_now;
        valid = 0;
        fresh = 1;
        if (w > 0 && h > 0) {
            SDL_Rect src = { 0, 0, w, h };
            SDL_UpdateTexture(tex2, &src, frame2_px, w * 4);
        }
    } else if (v2i_shown && v2i_w > 0 && v2i_h > 0) {     /* an in-between picture */
        w = v2i_w;
        h = v2i_h;
        wide = wide_now;
        valid = 0;                      /* the next one is the frame's own again */
        fresh = 1;
        SDL_Rect src = { 0, 0, w, h };
        SDL_UpdateTexture(tex2, &src, v2i_px, w * 4);
    } else if (!valid || (view2_gpu_changes() != changes && !p2view_held())) {
        view2_gpu_render_display(frame2_px, &w, &h);
        changes = view2_gpu_changes();
        wide = wide_now;
        valid = 1;
        fresh = 1;
        if (w > 0 && h > 0) {
            SDL_Rect src = { 0, 0, w, h };
            SDL_UpdateTexture(tex2, &src, frame2_px, w * 4);
        }
    }
    if (!fresh) return;
    if (fresh == 2 && w > 0 && h > 0) {
        SDL_Rect src = { 0, 0, w, h };
        SDL_UpdateTexture(tex2, &src, frame2_px, w * 4);
    }
    SDL_GetRendererOutputSize(ren2, &ww, &wh);
    SDL_SetRenderDrawColor(ren2, 0, 0, 0, 255);
    SDL_RenderClear(ren2);
    if (w > 0 && h > 0) {
        SDL_Rect src = { 0, 0, w, h };
        double aspect = display_aspect(w, h, wide);
        int dw = ww, dh = (int)(ww / aspect);
        if (dh > wh) { dh = wh; dw = (int)(wh * aspect); }
        SDL_Rect dst = { (ww - dw) / 2, (wh - dh) / 2, dw, dh };
        SDL_SetTextureScaleMode(tex2, g_cfg.scaling == SCALING_SHARP ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
        SDL_RenderCopy(ren2, tex2, &src, &dst);
    }
    SDL_RenderPresent(ren2);
}

static void dump_ram(uint64_t frame) {
    for (int k = 0; k < 8; k++) {
        if (g_cfg.dump_frame[k] && (uint64_t)g_cfg.dump_frame[k] == frame) {
            char path[1400];
#ifdef _WIN32
            _mkdir(g_cfg.screenshot_dir);
#else
            mkdir(g_cfg.screenshot_dir, 0755);
#endif
            snprintf(path, sizeof path, "%s/ram_%06llu.bin", g_cfg.screenshot_dir, (unsigned long long)frame);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(g_ram, 1, sizeof g_ram, f); fwrite(g_scratch, 1, sizeof g_scratch, f); fclose(f); }
            LOGI("RAM dump %s", path);
            /* full VRAM as a 1024x512 image */
            static uint32_t vpx[1024 * 512];
            const uint16_t *vr = gpu_vram();
            for (int i = 0; i < 1024 * 512; i++) {
                uint16_t c = vr[i];
                uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
                vpx[i] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
            }
            snprintf(path, sizeof path, "%s/vram_%06llu.bmp", g_cfg.screenshot_dir, (unsigned long long)frame);
            write_bmp(path, vpx, 1024, 512);
        }
    }
}

/* Wait until the next emulated frame is due (59.29 Hz NTSC).  When an audio device is
 * present the queue depth is used as the clock instead, so audio never drifts. */
static int fast_forward;

/* the window title shows the memory card in slot 1 (F1-F4) and the co-op version */
static void update_title(void) {
    char t[240];
    const char *net = netplay_status(NULL);
    if (net) snprintf(t, sizeof t, "Harvest Moon: Back to Nature  [Memory card %d]  [build %08x]  -  %s", memcard_bank(), netplay_build_id(), net);
    else snprintf(t, sizeof t, "Harvest Moon: Back to Nature  [Memory card %d]  [build %08x]", memcard_bank(), netplay_build_id());
    if (win) SDL_SetWindowTitle(win, t);
}

/* window events while the co-op waits for the other side (the window stays responsive) */
int platform_poll_quit(void) {
    SDL_Event ev;
    int changed;
    netplay_status(&changed);
    if (changed) update_title();
    while (SDL_PollEvent(&ev))
        if (ev.type == SDL_QUIT) return 1;
    return 0;
}
static bool frame_black;   /* the console's picture is black (fastload) */
static uint32_t px_changes;    /* gpu_changes() when frame_px was converted */
static int px_w, px_h;
static bool px_valid, shown_last;   /* frame_px holds that picture; the last frame was shown */

/* A coarse grid of the console's picture, all (nearly) black. Read from the native VRAM, not from
 * the picture on screen: it decides whether the music waits (quick_hold_music), which changes the
 * game's RAM, so it must come out alike on both co-op PCs and in replays, whatever each one shows
 * (resolution, in-between pictures, farmer 2's view) or whether it shows anything (headless). */
static bool display_black(void) {
    GpuDisplay d;
    gpu_get_display(&d);
    if (!d.enabled) return true;
    int w = d.width, h = d.height;
    if (w <= 0 || h <= 0) return false;
    const uint16_t *vram = gpu_vram();
    for (int y = h / 16; y < h; y += h / 8 + 1) {
        const uint16_t *row = vram + (size_t)((d.vram_y + y) & 511) * 1024;
        for (int x = w / 32; x < w; x += w / 16 + 1) {
            if (d.is_24bit) {
                size_t at = (size_t)(d.vram_x & 1023) * 2 + (size_t)x * 3;
                if (at + 3 > 2048) break;                   /* past the row's end */
                const uint8_t *p = (const uint8_t *)row + at;
                if (((p[0] | p[1] | p[2]) & 0xF0) != 0) return false;
            } else if ((row[(d.vram_x + x) & 1023] & 0x7BDEu) != 0) {   /* a 5-bit channel of 2 or more */
                return false;
            }
        }
    }
    return true;
}

/* running at full speed: Tab, or one of the waits the speed-up options skip */
static bool turbo(void) {
    return fast_forward || memcard_busy() || quick_loading(frame_black) || quick_skipping();
}

static void throttle(void) {
    if (g_cfg.unthrottled) return;
    if (turbo()) { perf_next = SDL_GetPerformanceCounter(); if (audio_ok) SDL_ClearQueuedAudio(audio); return; }
    uint64_t freq = SDL_GetPerformanceFrequency();
    double period = (double)freq / 59.2898;          /* NTSC PS1 frame rate */
    if (audio_ok) {
        /* keep ~3-5 frames of audio queued: nudge the frame period by up to 1% so the
           emulated clock follows the sound card's clock without audible gaps */
        double queued = (double)SDL_GetQueuedAudioSize(audio) / 4.0 / 735.0;   /* in frames */
        if (queued > 5.0) period *= 1.0 + 0.002 * (queued - 5.0 > 5.0 ? 5.0 : queued - 5.0);
        else if (queued < 3.0) period *= 1.0 - 0.003 * (3.0 - queued);
    }
    perf_next += (uint64_t)period;
    uint64_t now = SDL_GetPerformanceCounter();
    if (now > perf_next + (uint64_t)period * 4 || perf_next > now + (uint64_t)period * 4) perf_next = now;   /* resync after a stall */
    for (;;) {
        now = SDL_GetPerformanceCounter();
        if (now >= perf_next) break;
        uint64_t us = (perf_next - now) * 1000000u / freq;
        if (us > 2000) SDL_Delay((Uint32)(us / 1000 - 1));   /* coarse sleep, then spin the rest */
    }
}

/* Frame interpolation: the worker threads drawing in-between pictures (a band each) */
static SDL_Thread *interp_thread[INTERP_WORKERS];
static SDL_sem *interp_go[INTERP_WORKERS], *interp_done;
static int interp_pending;          /* workers still drawing */
static int interp_jobs;             /* 1: the game's in-between picture, 2: farmer 2's window's, 3: both */
void place_thread(int which, int nbands);   /* cpu_place.c */

static void interp_work(int k) {
    if (interp_jobs & 1) gpu_interp_work(k);
    if (interp_jobs & 2) view2_gpu_interp_work(k);
}

static int interp_worker(void *part) {
    int k = (int)(intptr_t)part;
    place_thread(1 + k, gpu_threads());     /* a performance core of its own */
    /* with few cores the workers share them with the game's thread: the game goes first, and an
     * in-between picture not done in time is skipped (gpu_interp_late) */
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for (;;) {
        SDL_SemWait(interp_go[k]);
        interp_work(k);
        SDL_SemPost(interp_done);
    }
    return 0;
}

/* The in-between pictures for the next VBlank (jobs: interp_jobs), drawn while the game goes on */
static void interp_start(int jobs) {
    interp_jobs = jobs;
    if (!interp_thread[0] && (!g_cfg.headless || getenv("HMBTN_INTERP_THREAD"))) {   /* headless: SDL may be absent */
        place_thread(0, gpu_threads());     /* the game's thread: the other performance cores */
        interp_done = SDL_CreateSemaphore(0);
        for (int k = 0; k < INTERP_WORKERS; k++) {
            interp_go[k] = SDL_CreateSemaphore(0);
            interp_thread[k] = SDL_CreateThread(interp_worker, "interp", (void *)(intptr_t)k);
        }
    }
    if (interp_thread[INTERP_WORKERS - 1]) {
        interp_pending = INTERP_WORKERS;
        for (int k = 0; k < INTERP_WORKERS; k++) SDL_SemPost(interp_go[k]);
    } else {
        for (int k = 0; k < INTERP_WORKERS; k++) interp_work(k);
    }
}

void platform_frame(void) {
    uint64_t frame = frame_count();
    dump_ram(frame);
    frame_black = g_cfg.fastload && display_black();
    int w = 0, h = 0;
    bool shot = g_cfg.screenshot_every && frame >= (uint64_t)g_cfg.screenshot_start &&
                frame % (uint64_t)g_cfg.screenshot_every == 0;
    /* at full speed only every 4th frame is shown: the rest would only slow it down */
    bool shown = !g_cfg.headless && (!turbo() || (frame & 3) == 0);
    if (!g_cfg.headless) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: g_quit = 1; break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_CLOSE) g_quit = 1;   /* either of two windows */
                if (win2 && ev.window.windowID == SDL_GetWindowID(win2)) win2_due = 1;   /* shown, resized, ... */
                /* fall through */
            case SDL_RENDER_TARGETS_RESET:
            case SDL_RENDER_DEVICE_RESET:
                px_valid = false;           /* the texture may have lost the picture: upload it again */
                if (ev.type != SDL_WINDOWEVENT) win2_due = 2;
                break;
            case SDL_CONTROLLERDEVICEADDED: open_pad(ev.cdevice.which); break;
            case SDL_AUDIODEVICEADDED: if (!ev.adevice.iscapture) open_audio(); break;
            case SDL_CONTROLLERDEVICEREMOVED:
                for (int i = 0; i < MAX_PADS; i++)
                    if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == ev.cdevice.which) {
                        SDL_GameControllerClose(pads[i]);
                        pads[i] = NULL;
                    }
                break;
            case SDL_KEYDOWN:
                if (ev.key.repeat) break;
                if (ev.key.keysym.scancode == SDL_SCANCODE_F11 ||
                    (ev.key.keysym.scancode == SDL_SCANCODE_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                    if (win2 && ev.key.windowID == SDL_GetWindowID(win2)) {
                        fullscreen2 = !fullscreen2;
                        SDL_SetWindowFullscreen(win2, fullscreen2 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        break;
                    }
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_F12) {
                    shot_request = 1;
                } else if (ev.key.keysym.scancode >= SDL_SCANCODE_F1 && !netplay_active() &&   /* co-op: same card on both sides */
                           ev.key.keysym.scancode < SDL_SCANCODE_F1 + CARD_BANKS) {
                    if (memcard_select_bank(ev.key.keysym.scancode - SDL_SCANCODE_F1)) update_title();
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_F5 && !netplay_active()) {
                    open_cheats_window();       /* co-op: the cheats are the host's from the start */
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_F9 && g_cfg.interp && g_pgxp) {
                    g_interp = !g_interp;       /* compare: in-between pictures on/off */
                    view2_g_interp = g_interp;
                    LOGI("interp %s", g_interp ? "on" : "off");
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE && fullscreen) {
                    fullscreen = 0;
                    SDL_SetWindowFullscreen(win, 0);
                }
                break;
            }
        }
        {
            const Uint8 *ks = SDL_GetKeyboardState(NULL);
            fast_forward = ks[SDL_SCANCODE_TAB] && g_cfg.allow_fast_forward;
        }
        uint16_t b = read_controllers();
        static uint16_t mine;           /* Alt held: keeps the buttons as they were */
        if (netplay_role() == 2) b |= p2_pad;   /* online guest: the numeric keypad and a 2nd controller are its too */
        if (!(SDL_GetModState() & KMOD_ALT)) mine = b | script_buttons((int)frame);
        cheat_events((int)frame);
        g_p2_buttons = p2_pad | script_pad((int)frame, 1);   /* co-op with two farmers: the guest's buttons instead */
        g_pad_buttons[0] = netplay_frame((int)frame, mine);   /* co-op: with the other player's */
        { int changed; netplay_status(&changed); if (changed) update_title(); }
        record_frame((int)frame, g_pad_buttons[0]);
        g_pad_buttons[0] = quick_skip(g_pad_buttons[0]);   /* after recording: replays skip alike */
        { static int shown_bank; if (memcard_bank() != shown_bank) { shown_bank = memcard_bank(); update_title(); } }
        /* wait until this VBlank is due, then show its picture: the pictures come at even intervals
         * however the game's work is spread over its VBlanks (drawing a frame can take longer than
         * one), and an in-between picture has until then to be drawn */
        throttle();
    }
    bool need_pixels = shown || shot || shot_request, same = false;
    /* farmer 2's talk or menu: this window keeps the player's picture (farmer 2's shows the game's) */
    bool keep = p2view_swapped() && g_view2 != 2;
    if (keep) {
        w = px_w;
        h = px_h;
    } else if (need_pixels) {
        /* workers not done yet: no in-between picture this time, rather than holding the game up */
        while (interp_pending && SDL_SemTryWait(interp_done) == 0) interp_pending--;
        if (interp_pending) {
            gpu_interp_late();
            view2_gpu_interp_late();
        }
        /* farmer 2's window has in-between pictures of its own, while it shows its own farm frames */
        int v2 = g_view2 > 0 && !turbo() && !p2view_held();
        if (v2) {
            int up = gpu_upscale();
            if (!v2i_px) v2i_px = malloc((size_t)1024 * 512 * up * up * sizeof(uint32_t));
            v2i_shown = v2i_px && view2_gpu_render_interp(v2i_px, &v2i_w, &v2i_h);
        } else {
            v2i_shown = 0;
        }
        /* an in-between picture (not at full speed): the frame on screen is still the last one,
         * so frame_black stays as it was */
        static int v2_valid;
        static uint32_t v2_changes;
        int v2main = g_view2 == 2 && !p2view_swapped();
        if (!v2main) v2_valid = 0;
        if (v2main) {
            /* online guest with two farmers: its window shows farmer 2's view (farmer 1's talk or
             * menu: the last picture stays) */
            px_valid = false;
            if (v2i_shown) {
                memcpy(frame_px, v2i_px, (size_t)v2i_w * v2i_h * 4);
                w = v2i_w;
                h = v2i_h;
                v2_valid = 0;           /* the next one is the frame's own again */
            } else if (!v2_valid || (view2_gpu_changes() != v2_changes && !p2view_held())) {
                view2_gpu_render_display(frame_px, &w, &h);
                v2_changes = view2_gpu_changes();
                v2_valid = 1;
                px_w = w;
                px_h = h;
            } else {
                w = px_w;
                h = px_h;
                same = true;
            }
        } else if (turbo() || !gpu_render_interp(frame_px, &w, &h)) {
            /* the game draws every other VBlank or less: a picture nothing changed since is
             * in frame_px (and the texture) already */
            if (px_valid && gpu_changes() == px_changes) {
                w = px_w;
                h = px_h;
                same = true;
            } else {
                gpu_render_display(frame_px, &w, &h);
                px_changes = gpu_changes();
                px_w = w;
                px_h = h;
                px_valid = true;
            }
        } else {
            px_valid = false;   /* an in-between picture in frame_px */
        }
        if (!interp_pending && !turbo()) {
            int jobs = gpu_interp_prepare() | (v2 && view2_gpu_interp_prepare()) << 1;
            if (jobs) interp_start(jobs);
        }
    }
    quick_hold_music(quick_loading(frame_black));   /* for the frame run next */
    int wide = display_wide();
    if (shot) save_screenshot(frame_px, w, h, frame, wide);

    if (g_cfg.headless) {
        cheat_events((int)frame);
        g_p2_buttons = script_pad((int)frame, 1);
        g_pad_buttons[0] = netplay_frame((int)frame, script_buttons((int)frame));
        record_frame((int)frame, g_pad_buttons[0]);
        g_pad_buttons[0] = quick_skip(g_pad_buttons[0]);
        if (g_cfg.max_frames && frame >= (uint64_t)g_cfg.max_frames) g_quit = 1;
        return;
    }

    if (shot_request) { save_screenshot(frame_px, w, h, frame, wide); shot_request = 0; }
    if (shown && !keep) present(w, h, wide, same && shown_last);
    if (shown && win2) present2(wide);
    shown_last = shown;
    if (g_cfg.max_frames && frame >= (uint64_t)g_cfg.max_frames) g_quit = 1;
}
