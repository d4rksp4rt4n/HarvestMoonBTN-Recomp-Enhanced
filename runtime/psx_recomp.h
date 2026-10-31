/* Interface between the recompiled game code (generated sources) and the native runtime. */
#pragma once
#include <stdint.h>
#include <string.h>
#include "gte.h"
#include "gpu.h"

#if defined(__GNUC__)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#endif

typedef struct CPU {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t sr;            /* cop0 r12 */
    uint32_t cause;         /* cop0 r13 */
    uint32_t epc;           /* cop0 r14 */
    uint32_t cur_func;      /* address of the most recently entered function (diagnostics) */
    uint64_t cycles;        /* CPU cycles elapsed since boot (approximate: 1 per instruction) */
    uint64_t next_event;    /* cycle count at which events_run() must be called */
} CPU;

typedef void (*RecompFunc)(CPU *c);
/* w0/w1: the function's first two instruction words, used to verify that the overlay
 * holding this function is the one currently resident in RAM */
typedef struct { uint32_t addr; RecompFunc fn; uint32_t w0, w1; } FuncEntry;
typedef struct {
    const char *name;
    uint32_t base, end, code_lo, code_hi;
    uint32_t ovl_id;        /* 0xFFFFFFFF for the main executable */
    int file_index;         /* A_FILE.BIN sub-file index, -1 for main */
    const FuncEntry *funcs;
    int nfuncs;
} ModuleDesc;
extern const ModuleDesc g_modules[];
extern const int g_num_modules;

extern uint8_t g_ram[0x200000];
extern uint8_t g_scratch[0x400];

/* slow paths (I/O, scratchpad, BIOS, unmapped) */
uint32_t mem_read8_slow(uint32_t a);
uint32_t mem_read16_slow(uint32_t a);
uint32_t mem_read32_slow(uint32_t a);
void     mem_write8_slow(uint32_t a, uint32_t v);
void     mem_write16_slow(uint32_t a, uint32_t v);
void     mem_write32_slow(uint32_t a, uint32_t v);

/* RAM is 2MB mirrored 4x in the first 8MB of each segment (KUSEG/KSEG0/KSEG1). */
#define RAM_HIT(a) (((a) & 0x1F800000u) == 0)

static inline uint32_t MEM_R8(uint32_t a) {
    if (LIKELY(RAM_HIT(a))) return g_ram[a & 0x1FFFFF];
    return mem_read8_slow(a);
}
static inline uint32_t MEM_R16(uint32_t a) {
    if (LIKELY(RAM_HIT(a))) { uint16_t v; memcpy(&v, g_ram + (a & 0x1FFFFE), 2); return v; }
    return mem_read16_slow(a);
}
static inline uint32_t MEM_R32(uint32_t a) {
    if (LIKELY(RAM_HIT(a))) { uint32_t v; memcpy(&v, g_ram + (a & 0x1FFFFC), 4); return v; }
    return mem_read32_slow(a);
}
static inline void MEM_W8(uint32_t a, uint32_t v) {
    if (LIKELY(RAM_HIT(a))) { g_ram[a & 0x1FFFFF] = (uint8_t)v; return; }
    mem_write8_slow(a, v);
}
static inline void MEM_W16(uint32_t a, uint32_t v) {
    if (LIKELY(RAM_HIT(a))) { uint16_t t = (uint16_t)v; memcpy(g_ram + (a & 0x1FFFFE), &t, 2); return; }
    mem_write16_slow(a, v);
}
static inline void MEM_W32(uint32_t a, uint32_t v) {
    if (LIKELY(RAM_HIT(a))) { memcpy(g_ram + (a & 0x1FFFFC), &v, 4); return; }
    mem_write32_slow(a, v);
}

int p2_drawing(void);
/* PGXP for 2D sprites placed at GTE-projected points (hooks emitted by tools/recomp.py for the
 * object projection and the sprite builders): remember the anchor's sub-pixel offset, look it
 * up when a sprite is built at that anchor, and tag the words the builder writes. */
static inline void SPRITE_ANCHOR(uint32_t x, uint32_t y, int player) {
    float px, py, w;
    if (g_sprite_tags && gte_sxy_precise(14, &px, &py, &w)) {
        float s = gte_ground_scale(px, py);
        float cx = px - (float)(int32_t)gte_read_ctrl(24) / 65536.0f, cy = py - (float)(int32_t)gte_read_ctrl(25) / 65536.0f;
        if (player == 1 && cx > -2.0f && cx < 2.0f && cy > -2.0f && cy < 2.0f)
            player = 2;                 /* at the projection centre: the camera follows the player */
        if ((player == 1 || player == 2) && g_interp) {       /* the camera of this frame: the GTE matrix projecting the player */
            int16_t r[9];
            for (int k = 0; k < 4; k++) {
                uint32_t m = gte_read_ctrl(k);
                r[2 * k] = (int16_t)m;
                r[2 * k + 1] = (int16_t)(m >> 16);
            }
            r[8] = (int16_t)gte_read_ctrl(4);
            gpu_set_camera(r, (int32_t)gte_read_ctrl(5), (int32_t)gte_read_ctrl(6), (int32_t)gte_read_ctrl(7),
                           gte_read_ctrl(26) & 0xFFFFu, (int32_t)gte_read_ctrl(24), (int32_t)gte_read_ctrl(25));
        }
        if (!g_pgxp) {                  /* widescreen without PGXP: squeezed, but not shifted */
            px = (float)(int16_t)x;
            py = (float)(int16_t)y;
        }
        gpu_sprite_anchor((int16_t)x, (int16_t)y, px - (float)(int16_t)x, py - (float)(int16_t)y, s, w, player);
    }
}
/* Widescreen: the farm draws the field objects (weeds, rocks, branches, ...) of the tiles within
 * 3600 units of the view's centre, a square made for 4:3 (800D92FC writes the tile range x0, z0,
 * x1, z1 to a; x 0..45, z 0..25): widen it like the view. */
static inline void FIELD_RANGE(uint32_t a) {
    int32_t e = (gte_widescreen_widen(3600) - 3600 + 399) / 400;
    int32_t x0 = (int32_t)MEM_R32(a) - e, z0 = (int32_t)MEM_R32(a + 4) - e;
    int32_t x1 = (int32_t)MEM_R32(a + 8) + e, z1 = (int32_t)MEM_R32(a + 12) + e;
    if (!e) return;
    MEM_W32(a, (uint32_t)(x0 < 0 ? 0 : x0));
    MEM_W32(a + 4, (uint32_t)(z0 < 0 ? 0 : z0));
    MEM_W32(a + 8, (uint32_t)(x1 > 45 ? 45 : x1));
    MEM_W32(a + 12, (uint32_t)(z1 > 25 ? 25 : z1));
}

/* Widescreen: the HUD (windows, text, portraits, HUD icons) is tagged to keep its 4:3 shape,
 * centred, while the picture is shown wide; g_hud_call is set while the HUD icon routine calls
 * the sprite builder. */
static inline void HUD_BEGIN(void) {
    if (g_sprite_tags) gpu_hud_begin(g_sprite_ws != 1.0f);
}
static inline void SPRITE_BEGIN(uint32_t x, uint32_t y) {
    if (g_menu_tile && g_sprite_ws != 1.0f) { if (g_sprite_tags) gpu_menu_begin(g_menu_tile); }
    else if (g_hud_call && g_sprite_ws != 1.0f) HUD_BEGIN();
    else if (g_sprite_tags) gpu_sprite_begin((int16_t)x, (int16_t)y);
}
/* The field's item drawing: an item lying on or thrown to the ground (g_item_call) is drawn at its
 * projected point, so it takes that anchor; what the player holds (g_held_call) follows the
 * player's anchor; other items (menu icons) aren't tagged. */
static inline void ITEM_BEGIN(uint32_t x, uint32_t y) {
    if (g_item_call && g_sprite_tags) gpu_sprite_begin((int16_t)x, (int16_t)y);
}
static inline void HELD_BEGIN(void) {
    if (g_sprite_tags && !g_item_call) gpu_held_begin(g_held_call ? 1 + p2_drawing() : 0);
}
static inline void SPRITE_ALIAS(uint32_t ax, uint32_t ay, uint32_t bx, uint32_t by) {
    if (g_sprite_tags) gpu_sprite_alias((int16_t)ax, (int16_t)ay, (int16_t)bx, (int16_t)by);
}
static inline void SPRITE_TAG(uint32_t a) {
    if (g_sprite_tags && RAM_HIT(a)) gpu_sprite_tag(a, MEM_R32(a & ~3u));
}

/* SWC2 of a screen XY register (SXY0-2/SXYP): the store, plus tagging the word with the
 * precise position for PGXP (upscaled display only; the game sees a plain store) */
static inline void PGXP_SWC2(uint32_t a, int reg) {
    uint32_t v = gte_read_data(reg);
    MEM_W32(a, v);
    if (g_pgxp && RAM_HIT(a)) {
        float x = 0, y = 0, w = 0;
        int ok = gte_sxy_precise(reg, &x, &y, &w);
        gpu_pgxp_store(a, v, ok, x, y, w);
    }
}

/* unaligned load/store (little endian R3000A semantics) */
static inline uint32_t psx_lwl(uint32_t addr, uint32_t cur) {
    uint32_t w = MEM_R32(addr & ~3u);
    switch (addr & 3) {
    case 0: return (cur & 0x00FFFFFFu) | (w << 24);
    case 1: return (cur & 0x0000FFFFu) | (w << 16);
    case 2: return (cur & 0x000000FFu) | (w << 8);
    default: return w;
    }
}
static inline uint32_t psx_lwr(uint32_t addr, uint32_t cur) {
    uint32_t w = MEM_R32(addr & ~3u);
    switch (addr & 3) {
    case 0: return w;
    case 1: return (cur & 0xFF000000u) | (w >> 8);
    case 2: return (cur & 0xFFFF0000u) | (w >> 16);
    default: return (cur & 0xFFFFFF00u) | (w >> 24);
    }
}
static inline void psx_swl(uint32_t addr, uint32_t v) {
    uint32_t a = addr & ~3u, w = MEM_R32(a);
    switch (addr & 3) {
    case 0: w = (w & 0xFFFFFF00u) | (v >> 24); break;
    case 1: w = (w & 0xFFFF0000u) | (v >> 16); break;
    case 2: w = (w & 0xFF000000u) | (v >> 8); break;
    default: w = v; break;
    }
    MEM_W32(a, w);
}
static inline void psx_swr(uint32_t addr, uint32_t v) {
    uint32_t a = addr & ~3u, w = MEM_R32(a);
    switch (addr & 3) {
    case 0: w = v; break;
    case 1: w = (w & 0x000000FFu) | (v << 8); break;
    case 2: w = (w & 0x0000FFFFu) | (v << 16); break;
    default: w = (w & 0x00FFFFFFu) | (v << 24); break;
    }
    MEM_W32(a, w);
}

static inline void psx_div(CPU *c, uint32_t n, uint32_t d) {
    int32_t sn = (int32_t)n, sd = (int32_t)d;
    if (sd == 0) { c->hi = n; c->lo = (sn >= 0) ? 0xFFFFFFFFu : 1u; }
    else if (n == 0x80000000u && sd == -1) { c->hi = 0; c->lo = 0x80000000u; }
    else { c->lo = (uint32_t)(sn / sd); c->hi = (uint32_t)(sn % sd); }
}
static inline void psx_divu(CPU *c, uint32_t n, uint32_t d) {
    if (d == 0) { c->hi = n; c->lo = 0xFFFFFFFFu; }
    else { c->lo = n / d; c->hi = n % d; }
}

/* control flow / system */
void call_addr(CPU *c, uint32_t addr);                         /* indirect call or tail jump */
void jumptable_miss(CPU *c, uint32_t site, uint32_t target);
void psx_syscall(CPU *c, uint32_t pc);
void psx_break(CPU *c, uint32_t pc, uint32_t code);
void psx_invalid(CPU *c, uint32_t pc, uint32_t word);
uint32_t cop0_read(CPU *c, int r);
void cop0_write(CPU *c, int r, uint32_t v);
void cop0_rfe(CPU *c);
void events_run(CPU *c);

extern int g_trace_calls;
/* CPU overclock: the cost of the compiled code in CPU cycles, scaled by g_cycle_scale/256
 * (256 = the console's speed). Video, audio, CD and timers keep the console's clock. */
extern uint32_t g_cycle_scale;
#define CYC(n) ((uint32_t)(((n) * g_cycle_scale) >> 8))
/* saveload overlay hooks (memcard.c): answer the memory card slot menu for the player */
uint32_t memcard_slot_menu(uint32_t result, int loading);
void memcard_slot_visit(void);
int memcard_slot_auto(void);
uint32_t memcard_diary_menu(uint32_t result, int loading);
uint32_t memcard_text(uint32_t text);
/* quickui.c: skip the waits of menus and windows */
void quick_window(uint32_t ra, uint32_t counter, uint32_t steps);
uint32_t quick_fade(uint32_t ticks);
void quick_text(void);
void quick_text_walked(uint32_t next, uint32_t inserted);
int quick_text_wait_kept(uint32_t ra);
int quick_music_held(void);
void quick_bag_filter(void);
/* p2.c: a second farmer, drawn by the player's drawing routine */
void p2_draw(CPU *c);
void p2_tick(CPU *c);
void p2view_frame(CPU *c);      /* entry of the farm's frame: run once more for farmer 2's window (p2view.c) */
extern int g_view2_run;         /* ... while that run goes */
void p2_tick_begin(void);
void cheats_tick(void);
void p2_tick_end(void);
int p2_faint_skipped(void);
int p2_thrown(CPU *c);
int p2_trade_move(CPU *c);
int p2_trade_kind(CPU *c);
int p2_bump(CPU *c);
uint32_t p2_x_target(uint32_t kind);
void trace_func(CPU *c, uint32_t addr);

#define EVCHK() do { if (UNLIKELY(c->cycles >= c->next_event)) events_run(c); } while (0)
#define FUNC_ENTER(a) do { c->cur_func = (a); if (UNLIKELY(g_trace_calls)) trace_func(c, (a)); EVCHK(); } while (0)
#define RETURN_CHECK(a) ((void)0)
