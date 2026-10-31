/*
 * gpu.c - PlayStation GPU (CXD8561, "v2") reimplementation.
 *
 *   - GP0 command FIFO / parameter assembly, polylines, VRAM transfers
 *   - GP1 display control, GPUSTAT, GPUREAD (incl. GP1(10h) info latch)
 *   - exact software rasterizer drawing into the 1024x512 16-bit VRAM
 *
 * Written from the psx-spx GPU documentation. The rasterization rules are
 * the ones of the real hardware:
 *
 *   Polygons: triangles are sorted by Y; edges are walked in 32.32 fixed
 *   point (start fraction = 1 - 2^-21, slopes rounded away from zero), so a
 *   pixel (x,y) is covered when the integer sample point (x,y) lies inside
 *   the triangle with a top-left fill convention (left/top edges inclusive,
 *   right/bottom exclusive).  Colour/UV interpolants are set up from the
 *   vertex deltas with 12 fractional bits (truncating division), stored as
 *   8.24 values starting at the "core" (left-most) vertex plus 0.5, and
 *   evaluated per pixel with wrapping 32-bit arithmetic.  Quads are drawn as
 *   triangles (v0,v1,v2) and (v1,v2,v3).
 *
 *   Lines: DDA in 32.32 fixed point over max(|dx|,|dy|)+1 pixels, both end
 *   points inclusive, colour in 20.12.
 *
 *   Rectangles: no dithering, texture coordinates stepped per pixel (8-bit
 *   wrap, then texture window), optional X/Y flip (GP0(E1h) bits 12/13).
 *
 * All commands complete instantly. All state, including VRAM, lives in one
 * static struct so it can be saved/restored with memcpy.
 */
#include "gpu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include "workers.h"
#ifdef __SSE2__
#include <emmintrin.h>
#endif

#define VRAM_W 1024
#define VRAM_H 512

/* Emulate the GPU's CLUT cache: the palette is only (re)loaded from VRAM when
 * a textured primitive uses a different CLUT/colour-depth than the previous
 * one, or after GP0(01h)/GP1(00h)/GP1(01h). Set to 0 to always reload. */
#ifndef GPU_EMULATE_CLUT_CACHE
#define GPU_EMULATE_CLUT_CACHE 1
#endif

#define CLUT_TAG_INVALID 0xFFFFFFFFu

enum {
    GP0_STATE_COMMAND = 0,   /* accumulating command words */
    GP0_STATE_VRAM_WRITE,    /* GP0(A0h) data words */
    GP0_STATE_POLYLINE       /* GP0(48h..5Fh) further vertices */
};

typedef struct {
    int32_t x, y;
    int32_t r, g, b;
} LinePt;

typedef struct {
    uint16_t vram[VRAM_W * VRAM_H];

    /* ---- GP0 command assembly ---- */
    uint32_t cmd[16];
    int32_t  cmd_count;
    int32_t  cmd_need;
    int32_t  gp0_state;

    /* polyline continuation */
    uint32_t pl_op;
    uint32_t pl_words[2];
    int32_t  pl_count;
    LinePt   pl_prev;

    /* CPU -> VRAM transfer */
    int32_t  wr_x, wr_y, wr_w, wr_h, wr_cx, wr_cy;
    /* VRAM -> CPU transfer */
    int32_t  rd_active;
    int32_t  rd_x, rd_y, rd_w, rd_h, rd_cx, rd_cy;
    uint32_t read_latch;

    /* ---- GP0(E1h) draw mode ---- */
    uint32_t tp_x;          /* texture page base X in halfwords (0..960) */
    uint32_t tp_y;          /* texture page base Y in lines (0/256) */
    uint32_t abr;           /* semi-transparency mode */
    uint32_t tex_mode;      /* 0=4bpp 1=8bpp 2,3=15bpp */
    uint32_t dither;
    uint32_t dfe;           /* drawing to display area allowed */
    uint32_t tex_disable;
    uint32_t flip_x, flip_y;

    /* ---- GP0(E2h) texture window ---- */
    uint32_t tww, twh, twx, twy;
    uint32_t tw_and_u, tw_or_u, tw_and_v, tw_or_v;

    /* ---- GP0(E3h..E5h) ---- */
    int32_t  clip_x0, clip_y0, clip_x1, clip_y1;
    int32_t  offs_x, offs_y;

    /* ---- GP0(E6h) ---- */
    uint32_t mask_set;      /* 0 or 0x8000 */
    uint32_t mask_check;    /* 0 or 0x8000 */

    /* ---- CLUT cache ---- */
    uint32_t clut_tag;
    uint16_t clut[256];

    /* ---- GP1 ---- */
    uint32_t irq;
    uint32_t display_off;
    uint32_t dma_dir;
    uint32_t disp_x, disp_y;
    uint32_t h_start, h_end;
    uint32_t v_start, v_end;
    uint32_t disp_mode;     /* GP1(08h) bits 0-7 */
    uint32_t tex_disable_allow;

    /* ---- timing ---- */
    uint32_t field;         /* interlace field currently displayed */
    uint32_t line_parity;   /* GPUSTAT.31 for non-interlaced modes */
} GpuState;

static GpuState S;

/* Draw targets. Everything is drawn into the native VRAM (exact: the game reads it back
 * and textures come from it). With upscaling, everything is drawn a second time into an
 * n-times larger copy that is only used for display; polygons are rasterized at the higher
 * resolution, sprites/lines/transfers are block-scaled. Textures always come from native VRAM. */
typedef struct {
    uint16_t *buf;
    uint32_t sh, xm, ym;            /* log2 row stride, coordinate wrap masks */
    int32_t  n;                     /* scale (1 = native VRAM) */
    uint8_t  dcol[4096];            /* dither column for x: (x / n) & 3 */
    uint16_t *tag;                  /* upscaled copy: per pixel, what drew it last (see under) */
} Target;
static Target TN = { S.vram, 10, 1023, 511, 1, { 0 } };
static Target TH;                   /* upscaled copy; buf == NULL when off */
/* Cracks: models that meet (a wall on the ground, ground tiles, a fence post on the ground) are
 * often not closed in 3D, and their corners only meet on the console's whole-pixel grid. At their
 * precise positions what was drawn before them shows through. So each pixel of the upscaled copy
 * has a tag: what drew it last (a fill, a 3D polygon or anything else, TAG_*) and when (a sequence
 * number of the commands); see-through pixels leave it. Where a fill (the screen clear) still
 * shows, a 3D polygon is drawn a second time as the console draws it, at its whole-pixel corners
 * and native resolution, each of its pixels only onto those of the n x n it covers (under,
 * plot_under): what the console shows there. They keep their tag, so a later polygon's second
 * drawing goes over them too, as on the console. Where something drawn before shows (the ground
 * through a door frame), seam_pass fills the gap when the picture is shown. */
#define TAG_FILL 0x4000u
#define TAG_3D 0x8000u
#define TAG_KIND(t) ((t) & 0xC000u)
static int under;
static uint16_t cur_tag, tag_seq;      /* the tag of what is being drawn */
/* Where textures and CLUTs are read: this GPU's own, except in a band worker's copy (gpu_band1.c
 * ...), which draws the GPU's polygons with the GPU's native VRAM and each polygon's CLUT. */
static const uint16_t *tex_vram = S.vram;
static const uint16_t *tex_clut = S.clut;
/* A band worker's copy draws the polygons' upscaled rows in stripe band_k of band_nb: stripes of
 * BAND_STRIPE rows taken in turn, so each worker gets about as much of the picture (sky or ground)
 * as the others. band_nb 0: all rows. */
#define BAND_STRIPE 8u
static int band_k, band_nb;

static inline int band_row(int32_t y)
{
    return !band_nb || (int)(((uint32_t)y / BAND_STRIPE) % (uint32_t)band_nb) == band_k;
}

/* PGXP shadow RAM: one entry per RAM word, used while the word still holds the tagged value
 * (w = 0: no precise position; w < 0: sprite tag, below). cmd_addr: RAM address of each word
 * in S.cmd. */
int g_pgxp, g_pgxp_persp;
int g_pgxp_cracks;                  /* close the cracks between models (under, seams); before gpu_set_upscale */
uint32_t g_gp0_addr = 0xFFFFFFFFu;
#if !defined(GPU_REPLAY) && !defined(GPU_BAND) && !defined(GPU_VIEW2)
/* The second view (p2view.c): what the GPU is sent goes to its copy (gpu_view2.c) too
 * (g_gpu_route 1), or only there (2: the farm's frame run once more for the second view) */
#define GPU_ROUTED 1
int g_gpu_route;
#define ROUTE(call) do { if (g_gpu_route) { view2_##call; if (g_gpu_route == 2) return; } } while (0)
/* What a frame's build tells the GPU about its lists (PGXP words, sprite tags and anchors, the
 * camera): when the second GPU draws lists its own run built (g_view2_built), the game's build of
 * the same RAM must not overwrite them (its vertices are the first view's: seams) */
int (*g_view2_built)(void);
#define ROUTE_BUILD(call) do { if (g_gpu_route == 2 || (g_gpu_route && !(g_view2_built && g_view2_built()))) \
                                   view2_##call; \
                               if (g_gpu_route == 2) return; } while (0)
#else
#define ROUTE(call) ((void)0)
#define ROUTE_BUILD(call) ((void)0)
#endif
typedef struct { uint32_t value; float x, y, w; int16_t cx, cy; float z; } PgxpWord;   /* z: sprite anchor depth */
static PgxpWord pgxp_mem[0x200000 / 4];
static uint32_t cmd_addr[16];

void gpu_pgxp_store(uint32_t addr, uint32_t value, int ok, float x, float y, float w)
{
    ROUTE_BUILD(gpu_pgxp_store(addr, value, ok, x, y, w));
    uint32_t i = (addr & 0x1FFFFCu) >> 2;
    pgxp_mem[i].value = value;
    pgxp_mem[i].x = x;
    pgxp_mem[i].y = y;
    pgxp_mem[i].w = ok ? w : 0.0f;
}

static int pgxp_lookup(uint32_t addr, uint32_t value, float *x, float *y, float *w)
{
    uint32_t i = (addr & 0x1FFFFCu) >> 2;
    if (addr == 0xFFFFFFFFu || pgxp_mem[i].w <= 0.0f || pgxp_mem[i].value != value)
        return 0;
    *x = pgxp_mem[i].x;
    *y = pgxp_mem[i].y;
    *w = pgxp_mem[i].w;
    return 1;
}

/* Sprites at projected points: anchors of the current and previous frame (by whole-pixel
 * position) with their sub-pixel offset, depth scale and scale centre (the projected point;
 * differs from x, y for an alias), the anchor selected for the sprite being built, and its
 * shift in the upscaled copy while drawing (target pixels). Sprite tags live in pgxp_mem:
 * x, y = offset, w = -scale, cx, cy = scale centre. */
typedef struct { float dx, dy, s, z; int16_t cx, cy; } SpriteMap;   /* z: the anchor's depth */
static struct { int16_t x, y; SpriteMap m; uint32_t gen; } anchors[1024];
static uint32_t anchor_gen = 1;
static SpriteMap sprite_map;
/* Depth scaling reference: the camera's distance to the player's level (the depth where the
 * projection centre's ray meets it), measured while the camera follows the player (the player's
 * anchor at the projection centre), smoothed, and kept as it is otherwise: in cutscenes, at the
 * edges of a map, while changing areas, and for the player's shadow below it while it jumps.
 * 0: not measured yet (each anchor's own level is used). */
static float ref_z;
static SpriteMap player_map;        /* the player's anchor (its own level), for what it holds */
static float player_zc;             /* ... the depth of its level, and when it was projected */
static uint32_t player_gen;
static SpriteMap farmer2_map;       /* the same for the second farmer (p2.c), drawn by the player's routine */
static float farmer2_zc;
static uint32_t farmer2_gen;
static int32_t sprite_ox, sprite_oy;
static float line_sq = 1.0f;        /* HUD line in widescreen: squeezed by line_sq about line_c */
static int32_t line_c;
/* Widescreen: a frame draws the world first and the HUD last, so once a HUD-tagged primitive was
 * drawn this frame, untagged rectangles and lines are HUD too (the dialogue arrow and the bag's
 * item slots come from shared sprite routines), except ones covering the whole width (the menus'
 * full-screen dimming). Reset at every VBlank. */
static int hud_after;
int g_sprite_tags, g_sprite_on, g_sprite_depth, g_hud_call, g_held_call, g_item_call, g_menu_tile;
#define HUD_CX INT16_MIN            /* scale centre of HUD tags: the middle of the drawing area */
#define MENU_CX (INT16_MIN + 1)     /* tag of a full-screen menu's background tile */
static const PgxpWord hud_word = { 0, 0.0f, 0.0f, -1.0f, HUD_CX, 0, 0.0f };   /* a HUD tag */
float g_sprite_ws = 1.0f;

static uint32_t anchor_slot(int x, int y)
{
    return ((uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663)) & 1023u;
}

static void anchor_set(int x, int y, const SpriteMap *m)
{
    uint32_t i = anchor_slot(x, y);
    anchors[i].x = (int16_t)x;
    anchors[i].y = (int16_t)y;
    anchors[i].m = *m;
    anchors[i].gen = anchor_gen;
}

void gpu_sprite_anchor(int x, int y, float dx, float dy, float s, float z, int player)
{
    ROUTE_BUILD(gpu_sprite_anchor(x, y, dx, dy, s, z, player));
    SpriteMap m = { dx, dy, s, z, (int16_t)x, (int16_t)y };
    if (player == 3) {                  /* the second farmer: only for what it holds */
        if (farmer2_gen != anchor_gen || z * s < farmer2_zc) {
            farmer2_map = m;
            farmer2_zc = z * s;
            farmer2_gen = anchor_gen;
        }
    } else if (player) {
        if (player_gen != anchor_gen || z * s < player_zc) {    /* the player, not its shadow */
            player_map = m;
            player_zc = z * s;
            player_gen = anchor_gen;
        }
        if (player == 2)
            ref_z = ref_z > 0.0f ? ref_z + (z * s - ref_z) * 0.125f : z * s;
    }
    anchor_set(x, y, &m);
}

/* The depth scale is the reference over the anchor's depth: the camera follows the player at a
 * fixed distance, so an object only changes size when its distance does, also when the player
 * climbs above it, and in cutscenes objects scale with the camera like the 3D world. Before the
 * reference is measured, the scale for the anchor's own ground level. */
static void begin_map(const SpriteMap *m)
{
    sprite_map = *m;
    if (ref_z > 0.0f && sprite_map.z > 0.0f) {
        float s = ref_z / sprite_map.z;
        sprite_map.s = s < 0.5f ? 0.5f : s > 2.0f ? 2.0f : s;
    }
}

void gpu_sprite_begin(int x, int y)
{
    ROUTE_BUILD(gpu_sprite_begin(x, y));
    uint32_t i = anchor_slot(x, y);
    g_sprite_on = anchors[i].gen + 1 >= anchor_gen && anchors[i].x == x && anchors[i].y == y;
    begin_map(&anchors[i].m);
}

/* What the player holds above its head is drawn at the player's position plus an offset: it is
 * placed, scaled and squeezed with the player's anchor, like the player (on = 0: drop old tags;
 * 2: what the second farmer holds, with its own anchor: with the player's it was squeezed about
 * the player and came out stretched) */
void gpu_held_begin(int on)
{
    ROUTE_BUILD(gpu_held_begin(on));
    if (on == 2) {
        g_sprite_on = farmer2_gen + 1 >= anchor_gen;
        begin_map(&farmer2_map);
        return;
    }
    g_sprite_on = on && player_gen + 1 >= anchor_gen;
    begin_map(&player_map);
}

/* Widescreen: full-screen menus (toolbox, Start menu, memory card screens) and the title screen
 * fill the wide picture: their background is repeated at the sides and the rest keeps its 4:3
 * shape in the middle (bg_wide). The game builds a frame while the previous one is shown, so what
 * decides is the picture: the frame (VBlank count) each drawing buffer (top or bottom of VRAM) was
 * last drawn in, and the last one with such a background tile in it. */
static uint32_t buf_drawn[2], buf_menu[2];
static inline int draw_buf(void)
{
    return S.clip_y0 >= 240;
}

static inline int drawing_menu(void)
{
    return buf_menu[draw_buf()] == anchor_gen;
}

int gpu_display_menu(void)
{
    int b = S.disp_y >= 240;
    return buf_menu[b] && buf_menu[b] == buf_drawn[b];
}

void gpu_menu_begin(int period)
{
    const SpriteMap menu = { 0.0f, 0.0f, 1.0f, 0.0f, MENU_CX, (int16_t)period };   /* cy: the period */
    ROUTE_BUILD(gpu_menu_begin(period));
    g_sprite_on = 1;
    sprite_map = menu;
}

void gpu_hud_begin(int on)
{
    static const SpriteMap hud = { 0.0f, 0.0f, 1.0f, 0.0f, HUD_CX, 0 };
    ROUTE_BUILD(gpu_hud_begin(on));
    g_sprite_on = on;
    sprite_map = hud;
}

void gpu_sprite_alias(int ax, int ay, int bx, int by)
{
    ROUTE_BUILD(gpu_sprite_alias(ax, ay, bx, by));
    uint32_t i = anchor_slot(ax, ay);
    if ((ax == bx && ay == by) || anchors[i].gen + 1 < anchor_gen || anchors[i].x != ax || anchors[i].y != ay)
        return;
    anchor_set(bx, by, &anchors[i].m);
}

void gpu_sprite_tag(uint32_t addr, uint32_t value)
{
    ROUTE_BUILD(gpu_sprite_tag(addr, value));
    uint32_t i = (addr & 0x1FFFFCu) >> 2;
    if (!g_sprite_on) {                     /* built without a known anchor: drop an old sprite tag */
        if (pgxp_mem[i].w < 0.0f)
            pgxp_mem[i].w = 0.0f;
        return;
    }
    pgxp_mem[i].value = value;
    pgxp_mem[i].x = sprite_map.dx;
    pgxp_mem[i].y = sprite_map.dy;
    pgxp_mem[i].w = -sprite_map.s;
    pgxp_mem[i].cx = sprite_map.cx;
    pgxp_mem[i].cy = sprite_map.cy;
    pgxp_mem[i].z = sprite_map.z;
}

/* dither_lut[y&3][x&3][v]: (v + offset) / 8 saturated to 0..31, v = 0..511 */
static uint8_t dither_lut[4][4][512];
static int dither_lut_ready;

static const int dither_matrix[4][4] = {
    { -4,  0, -3,  1 },
    {  2, -2,  3, -1 },
    { -3,  1, -4,  0 },
    {  3, -1,  2, -2 },
};

static void build_dither_lut(void)
{
    int y, x, v;
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            for (v = 0; v < 512; v++) {
                int t = v + dither_matrix[y][x];
                if (t < 0) t = 0;
                t >>= 3;
                if (t > 31) t = 31;
                dither_lut[y][x][v] = (uint8_t)t;
            }
    for (x = 0; x < 4096; x++)          /* in every copy: band workers draw natively too (under) */
        TN.dcol[x] = (uint8_t)(x & 3);
    dither_lut_ready = 1;
}

/* LUT row used when dithering is off: offset 0 */
#define NO_DITHER_LUT (dither_lut[2][3])

/* ======================================================================= */
/* Helpers                                                                  */
/* ======================================================================= */

static inline int32_t sext11(uint32_t v)
{
    return (int32_t)((v & 0x7FFu) ^ 0x400u) - 0x400;
}

static inline int32_t fp_int(int64_t v)
{
    return (int32_t)(v >> 32);
}

static void recalc_texwin(void)
{
    S.tw_and_u = ~(S.tww << 3) & 0xFFu;
    S.tw_or_u  = (S.twx & S.tww) << 3;
    S.tw_and_v = ~(S.twh << 3) & 0xFFu;
    S.tw_or_v  = (S.twy & S.twh) << 3;
}

/* Texture page attribute (GP0(E1h) or the polygon "page" field). */
static void set_texpage(uint32_t v, int full_e1)
{
    S.tp_x     = (v & 0xFu) * 64u;
    S.tp_y     = (v & 0x10u) ? 256u : 0u;
    S.abr      = (v >> 5) & 3u;
    S.tex_mode = (v >> 7) & 3u;
    if (S.tex_disable_allow)
        S.tex_disable = (v >> 11) & 1u;
    if (full_e1) {
        S.dither = (v >> 9) & 1u;
        S.dfe    = (v >> 10) & 1u;
        S.flip_x = (v >> 12) & 1u;
        S.flip_y = (v >> 13) & 1u;
    }
}

static void clut_invalidate(void)
{
    S.clut_tag = CLUT_TAG_INVALID;
}

static void clut_update(uint32_t raw_clut)
{
    uint32_t mode = S.tex_mode;
    uint32_t cx, cy, n, i;
    const uint16_t *row;

    if (mode >= 2)
        return;
#if GPU_EMULATE_CLUT_CACHE
    {
        uint32_t tag = (raw_clut & 0x7FFFu) | (mode << 16);
        if (tag == S.clut_tag)
            return;
        S.clut_tag = tag;
    }
#endif
    cx = (raw_clut & 0x3Fu) << 4;
    cy = (raw_clut >> 6) & 0x1FFu;
    n = mode ? 256u : 16u;
    row = &S.vram[cy << 10];
    for (i = 0; i < n; i++)
        S.clut[i] = row[(cx + i) & 1023u];
}

/* Fetch a texel (texture window + page + CLUT applied). 0 = transparent. */
static inline uint32_t tex_fetch(uint32_t u, uint32_t v)
{
    uint32_t tu = (u & S.tw_and_u) | S.tw_or_u;
    uint32_t tv = (v & S.tw_and_v) | S.tw_or_v;
    const uint16_t *row = &tex_vram[((S.tp_y + tv) & 511u) << 10];
    uint32_t w;

    switch (S.tex_mode) {
    case 0:
        w = row[(S.tp_x + (tu >> 2)) & 1023u];
        return tex_clut[(w >> ((tu & 3u) << 2)) & 0xFu];
    case 1:
        w = row[(S.tp_x + (tu >> 1)) & 1023u];
        return tex_clut[(w >> ((tu & 1u) << 3)) & 0xFFu];
    default:
        return row[(S.tp_x + tu) & 1023u];
    }
}

/* Texture packs: the image the upscaled copy of the primitive being drawn reads instead of its texels
 * (NULL: none), stretched over its UV box from rep_u0, rep_v0 (rep_su, rep_sv: image pixels per
 * texel). rep_at: the image pixel at image coordinates x, y (nearest); rep_fetch: at u, v (8.24, a
 * pixel centre's); rep_mix: the texel drawn, the image pixel r with the semi-transparency bit of the
 * texture's texel t there (the game sets it on almost every texel; it blends where the game draws
 * semi-transparent, e.g. shadows). */
static const TexImage *tex_rep;
static float rep_u0, rep_v0, rep_su, rep_sv;

static inline uint32_t rep_mix(uint32_t r, uint32_t t)
{
    return r ? r | (t & 0x8000u) : 0;
}

static inline uint32_t rep_at(const TexImage *r, float fx, float fy)
{
    int32_t x = (int32_t)fx, y = (int32_t)fy;
    x = x < 0 ? 0 : x >= r->w ? r->w - 1 : x;
    y = y < 0 ? 0 : y >= r->h ? r->h - 1 : y;
    return r->px[y * r->w + x];
}

static inline uint32_t rep_fetch(const TexImage *r, uint32_t u, uint32_t v)
{
    return rep_at(r, ((float)(u >> 8) * (1.0f / 65536.0f) - rep_u0) * rep_su,
                  ((float)(v >> 8) * (1.0f / 65536.0f) - rep_v0) * rep_sv);
}

/* Texture colour modulation: (texel5 * colour8) >> 4, then dither/saturate. */
static inline uint32_t modulate(uint32_t t, uint32_t r, uint32_t g, uint32_t b,
                                const uint8_t *lut)
{
    return (uint32_t)lut[((t & 0x1Fu) * r) >> 4]
         | ((uint32_t)lut[(((t >> 5) & 0x1Fu) * g) >> 4] << 5)
         | ((uint32_t)lut[(((t >> 10) & 0x1Fu) * b) >> 4] << 10);
}

static inline uint32_t blend_px(uint32_t bk, uint32_t fr, int mode)
{
    int32_t br = (int32_t)(bk & 31u), bg = (int32_t)((bk >> 5) & 31u), bb = (int32_t)((bk >> 10) & 31u);
    int32_t fr_ = (int32_t)(fr & 31u), fg = (int32_t)((fr >> 5) & 31u), fb = (int32_t)((fr >> 10) & 31u);
    int32_t r, g, b;

    switch (mode) {
    case 0: /* B/2 + F/2 */
        r = (br + fr_) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1;
        break;
    case 1: /* B + F */
        r = br + fr_; g = bg + fg; b = bb + fb;
        if (r > 31) r = 31;
        if (g > 31) g = 31;
        if (b > 31) b = 31;
        break;
    case 2: /* B - F */
        r = br - fr_; g = bg - fg; b = bb - fb;
        if (r < 0) r = 0;
        if (g < 0) g = 0;
        if (b < 0) b = 0;
        break;
    default: /* B + F/4 */
        r = br + (fr_ >> 2); g = bg + (fg >> 2); b = bb + (fb >> 2);
        if (r > 31) r = 31;
        if (g > 31) g = 31;
        if (b > 31) b = 31;
        break;
    }
    return (uint32_t)r | ((uint32_t)g << 5) | ((uint32_t)b << 10);
}

/* Write one pixel. rgb = 15-bit colour, bit15 = value of bit 15 for the
 * written pixel (texel bit 15, or 0 for untextured), blend = -1 or mode. */
static void plot_under(uint32_t x, uint32_t y, uint32_t rgb);

/* ... with the target's tags tg (NULL: none), the tag tagv to write and under (und) as given: a
 * span reads them once (every store could change a global, so the compiler can't) */
static inline __attribute__((always_inline)) void plot_t(const Target *t, uint32_t x, uint32_t y, uint32_t rgb,
                                                         uint32_t bit15, int blend, uint16_t *tg, uint16_t tagv, int und)
{
    uint32_t i = ((y & t->ym) << t->sh) | (x & t->xm);
    uint16_t *p = &t->buf[i];
    uint32_t bk = *p;
    if (und) {
        plot_under(x, y, rgb | bit15);
        return;
    }
    if (bk & S.mask_check)
        return;
    if (tg && blend < 0)
        tg[i] = tagv;
    if (blend >= 0)
        rgb = blend_px(bk, rgb, blend);
    *p = (uint16_t)(rgb | bit15 | S.mask_set);
}

static inline void plot(const Target *t, uint32_t x, uint32_t y, uint32_t rgb, uint32_t bit15, int blend)
{
    plot_t(t, x, y, rgb, bit15, blend, t->tag, cur_tag, under);
}

/* The second drawing (under) of native row y: in this band if one of its n upscaled rows is (n is
 * less than BAND_STRIPE, so they are in one or two stripes) */
static inline int band_rows(int32_t y)
{
    return band_row(y * TH.n) || band_row(y * TH.n + TH.n - 1);
}

/* ... its native pixel (x, y) onto the marked pixels of its n x n in the upscaled copy (in this
 * band's rows) */
static void plot_under(uint32_t x, uint32_t y, uint32_t rgb)
{
    uint32_t n = (uint32_t)TH.n, hx = (x & 1023u) * n, hy = (y & 511u) * n, i, j;
    uint32_t split = BAND_STRIPE - hy % BAND_STRIPE;    /* rows from here are in the next stripe */
    int b0 = band_row((int32_t)hy), b1 = band_row((int32_t)(hy + n - 1));
    for (j = 0; j < n; j++) {
        uint32_t k = ((hy + j) << TH.sh) + hx;
        if (!(j < split ? b0 : b1))
            continue;
        for (i = 0; i < n; i++)
            if (TAG_KIND(TH.tag[k + i]) == TAG_FILL && !(TH.buf[k + i] & S.mask_check))
                TH.buf[k + i] = (uint16_t)(rgb | S.mask_set);
    }
}

/* Has native pixel (x, y) pixels in its n x n that still show a fill (under)? */
static inline int under_marked(int32_t x, int32_t y)
{
    uint32_t n = (uint32_t)TH.n, hx = ((uint32_t)x & 1023u) * n, hy = ((uint32_t)y & 511u) * n, i, j;
    for (j = 0; j < n; j++) {
        const uint16_t *b = &TH.tag[((hy + j) << TH.sh) + hx];
        for (i = 0; i < n; i++)
            if (TAG_KIND(b[i]) == TAG_FILL)
                return 1;
    }
    return 0;
}

/* Sprites and lines: the native pixel, and the n x n block it covers in the upscaled copy. */
static inline void plot2(uint32_t x, uint32_t y, uint32_t rgb, uint32_t bit15, int blend)
{
    plot(&TN, x, y, rgb, bit15, blend);
    if (TH.buf) {
        uint32_t n = (uint32_t)TH.n, hx = (x & 1023u) * n, hy = (y & 511u) * n, i, j;
        if (line_sq != 1.0f) {              /* HUD line: the pixel's squeezed span */
            int32_t px, x0 = (int32_t)lrintf(((float)line_c + (float)((int32_t)(x & 1023u) - line_c) * line_sq) * (float)n);
            int32_t x1 = (int32_t)lrintf(((float)line_c + (float)((int32_t)(x & 1023u) + 1 - line_c) * line_sq) * (float)n);
            for (j = 0; j < n; j++)
                for (px = x0; px < x1; px++)
                    plot(&TH, (uint32_t)px, hy + j, rgb, bit15, blend);
            return;
        }
        if (sprite_ox | sprite_oy) {        /* sprite at a projected point: shifted, kept in the draw area */
            int32_t sx = (int32_t)hx + sprite_ox, sy = (int32_t)hy + sprite_oy, N = (int32_t)n;
            int32_t cx0 = S.clip_x0 * N, cx1 = S.clip_x1 * N + N - 1, cy0 = S.clip_y0 * N, cy1 = S.clip_y1 * N + N - 1;
            for (j = 0; j < n; j++)
                for (i = 0; i < n; i++) {
                    int32_t px = sx + (int32_t)i, py = sy + (int32_t)j;
                    if (px >= cx0 && px <= cx1 && py >= cy0 && py <= cy1)
                        plot(&TH, (uint32_t)px, (uint32_t)py, rgb, bit15, blend);
                }
            return;
        }
        if (blend < 0 && !S.mask_check) {   /* opaque: plain stores */
            uint16_t v = (uint16_t)(rgb | bit15 | S.mask_set);
            for (j = 0; j < n; j++) {
                uint16_t *p = &TH.buf[((hy + j) << TH.sh) + hx];
                for (i = 0; i < n; i++)
                    p[i] = v;
                for (i = 0; TH.tag && i < n; i++)
                    TH.tag[((hy + j) << TH.sh) + hx + i] = cur_tag;
            }
            return;
        }
        for (j = 0; j < n; j++)
            for (i = 0; i < n; i++)
                plot(&TH, hx + i, hy + j, rgb, bit15, blend);
    }
}

/* Upscaled-copy coordinate of native coordinate v (drawing offset included) of a tagged sprite
 * with scale centre c: shifted by the anchor's offset d and scaled by s about c (unscaled sprites
 * move rigidly, so their pixel art stays crisp). */
static inline int32_t sprite_coord(int32_t v, int32_t c, float d, float s)
{
    if (s == 1.0f)
        return v * TH.n + (int32_t)lrintf(d * (float)TH.n);
    return (int32_t)lrintf(((float)c + d + (float)(v - c) * s) * (float)TH.n);
}

/* The squeeze of the HUD: the world's while the picture is shown wide, a 3D scene or a menu or
 * title screen, asked when the HUD is drawn. When it is built is too early: the game builds the
 * HUD before it projects the 3D scene, so the first frame after a black screen isn't known to be
 * 3D yet. */
int (*g_scene_3d)(void);
static inline float hud_sx(void)
{
    return !g_scene_3d || drawing_menu() || g_scene_3d() ? g_sprite_ws : 1.0f;
}

/* Scale of the sprite tagged t: its depth scale (with depth scaling), and horizontally the
 * widescreen squeeze. */
static inline float sprite_sx(const PgxpWord *t)
{
    if (t->cx == HUD_CX)
        return hud_sx();
    return (g_sprite_depth ? -t->w : 1.0f) * g_sprite_ws;
}

static inline float sprite_sy(const PgxpWord *t)
{
    return g_sprite_depth ? -t->w : 1.0f;
}

/* Horizontal scale centre of the sprite tagged t (drawing offset included): its anchor, or for
 * the HUD the middle of the screen drawn: the drawing offset (the buffer's corner) plus half the
 * display's width. Not the middle of the drawing area: the toolbox narrows that to its help box
 * (30..319) to clip the text, which then came out 5 pixels to the right. */
static inline int32_t sprite_cx(const PgxpWord *t)
{
    static const int16_t disp_w[4] = { 256, 320, 512, 640 };
    if (t->cx != HUD_CX)
        return t->cx + S.offs_x;
    return S.offs_x + ((S.disp_mode & 0x40u) ? 368 : disp_w[S.disp_mode & 3u]) / 2;
}

/* ---- Frame interpolation (prototype) ----
 * The game shows each frame for two VBlanks, and the next frame is already drawn (into the other
 * buffer) when the second one comes. For that VBlank the frontend can show an in-between picture:
 * the next frame's drawing commands (recorded per drawing buffer) redrawn into a scratch copy of
 * the upscaled VRAM with the camera half a step back towards the frame on screen.
 * replaying: such a redraw is running; ifm, ifk: the view-space change from the recorded camera to
 * the in-between one (P' = M P + K); ih, iofx, iofy, iws: the projection; ipdx, ipdy: the player's
 * own half step on screen, for sprites at the player's anchor (ipcx, ipcy). */
static int replaying;
static int32_t band_y0 = 0, band_y1 = 1024;    /* a redraw's band of VRAM rows (each worker draws one) */
static float ifm[9], ifk[3], ih, iofx, iofy, iws, ipdx, ipdy;
static int16_t ipcx = INT16_MIN + 2, ipcy;

/* A projected point (GTE screen x, y and view depth z) seen from the in-between camera. */
static void interp_point(float *x, float *y, float *z)
{
    float X = (*x - iofx) / iws * *z / ih, Y = (*y - iofy) * *z / ih, Z = *z;
    float X2 = ifm[0] * X + ifm[1] * Y + ifm[2] * Z + ifk[0];
    float Y2 = ifm[3] * X + ifm[4] * Y + ifm[5] * Z + ifk[1];
    float Z2 = ifm[6] * X + ifm[7] * Y + ifm[8] * Z + ifk[2];
    if (Z2 < 16.0f)
        return;
    *x = iofx + X2 / Z2 * ih * iws;
    *y = iofy + Y2 / Z2 * ih;
    *z = Z2;
}

/* The tag of a sprite at a projected point with its anchor moved for the in-between picture: like
 * the camera, or like the player for the player's anchor (the camera follows the player). */
static const PgxpWord *interp_tag(const PgxpWord *t, PgxpWord *tmp)
{
    float x, y, z;
    if (!replaying || !t || t->cx == HUD_CX || t->cx == MENU_CX)
        return t;
    *tmp = *t;
    if (t->cx == ipcx && t->cy == ipcy) {
        tmp->x += ipdx;
        tmp->y += ipdy;
    } else if (t->z > 0.0f) {
        x = (float)t->cx + t->x;
        y = (float)t->cy + t->y;
        z = t->z;
        interp_point(&x, &y, &z);
        tmp->x = x - (float)t->cx;
        tmp->y = y - (float)t->cy;
    }
    return tmp;
}

/* CPU->VRAM data: the n x n block for native pixel (x, y), honouring the mask bit. */
static void up_block(uint32_t x, uint32_t y, uint16_t v)
{
    uint32_t n = (uint32_t)TH.n, i, j;
    for (j = 0; j < n; j++) {
        uint32_t k = (((y & 511u) * n + j) << TH.sh) + (x & 1023u) * n;
        for (i = 0; i < n; i++)
            if (!(TH.buf[k + i] & S.mask_check)) {
                TH.buf[k + i] = v;
                if (TH.tag)
                    TH.tag[k + i] = cur_tag;
            }
    }
}

/* Rebuild the upscaled copy from native VRAM (after enabling it or loading a state). */
static void up_refresh(void)
{
    uint32_t n = (uint32_t)TH.n, x, y, i, j;
    for (y = 0; y < 512; y++)
        for (j = 0; j < n; j++) {
            uint16_t *p = &TH.buf[(y * n + j) << TH.sh];
            for (x = 0; x < 1024; x++)
                for (i = 0; i < n; i++)
                    p[x * n + i] = S.vram[(y << 10) | x];
        }
    if (TH.tag)
        memset(TH.tag, 0, ((size_t)1 << (2 * TH.sh - 1)) * sizeof *TH.tag);
}

/* 480i with drawing to the displayed field disabled: lines of the field
 * currently being displayed are not drawn. */
static inline int line_skip(int32_t y)
{
    if ((S.disp_mode & 0x24u) != 0x24u || S.dfe)
        return 0;
    return ((uint32_t)y & 1u) == S.field;
}

/* ======================================================================= */
/* Polygons                                                                 */
/* ======================================================================= */

typedef struct {
    int32_t x, y;
    int32_t r, g, b;
    int32_t u, v;
    float w;            /* PGXP view depth, 0 = unknown (upscaled pass only) */
} TriVtx;

/* Perspective-correct texturing (upscaled pass): planes a + b*x + c*y over target pixels of
 * u/w, v/w and 1/w, and the triangle's UV range (the result is clamped to it). */
typedef struct {
    double u[3], v[3], q[3];
    int32_t umin, umax, vmin, vmax;
} Persp;

typedef struct {
    uint32_t r, g, b, u, v;
} Interp;

typedef struct {
    Interp dx, dy;
} InterpDelta;

typedef struct {
    int gouraud;
    int textured;
    int raw;            /* raw texture (no modulation) */
    int dither;         /* dithering applies to this primitive */
    int blend;          /* -1 = opaque, else semi-transparency mode */
    uint32_t flat_r, flat_g, flat_b;
    uint32_t flat_rgb15;
} PolyCtx;

static inline void interp_add(Interp *ig, const Interp *d, uint32_t count)
{
    ig->r += d->r * count;
    ig->g += d->g * count;
    ig->b += d->b * count;
    ig->u += d->u * count;
    ig->v += d->v * count;
}

/* The perspective-correct textured span of draw_span (upscaled pass) for one texture mode, with
 * what stays the same along the span read once: the same arithmetic in the same order, so the
 * same pixels, only faster (the waterfall draws a lot of these). */
static inline __attribute__((always_inline)) void persp_span(const Target *t, const PolyCtx *c, uint32_t y,
                                                             int32_t x, int32_t w, Interp ig, const InterpDelta *d,
                                                             const Persp *pp, const uint8_t (*drow)[512],
                                                             const uint32_t mode, uint16_t *const tg,
                                                             const uint16_t tagv, const int und)
{
    float pu = (float)(pp->u[0] + pp->u[1] * x + pp->u[2] * (int32_t)y);
    float pv = (float)(pp->v[0] + pp->v[1] * x + pp->v[2] * (int32_t)y);
    float pq = (float)(pp->q[0] + pp->q[1] * x + pp->q[2] * (int32_t)y);
    const float du = (float)pp->u[1], dv = (float)pp->v[1], dq = (float)pp->q[1];
    const int32_t umin = pp->umin, umax = pp->umax, vmin = pp->vmin, vmax = pp->vmax;
    const uint32_t and_u = S.tw_and_u, or_u = S.tw_or_u, and_v = S.tw_and_v, or_v = S.tw_or_v;
    const uint32_t tp_x = S.tp_x, tp_y = S.tp_y;
    const uint16_t *const vram = tex_vram, *const clut = tex_clut;
#ifdef __SSE2__
    /* four pixels at a time: the same sums one after the other, then the divisions and
     * conversions of all four at once (the same IEEE results as one by one) */
    int32_t qu[4], qv[4];
    int qn = 0;
    const __m128 half = _mm_set1_ps(1.0f / 2048), one = _mm_set1_ps(1.0f);
#endif
    for (; w > 0; w--, x++) {
        int32_t tu, tv;
        uint32_t u, v, tx;
        const uint16_t *row;
#ifdef __SSE2__
        if (!qn && w >= 4) {
            float a[4], b[4], q[4];
            int k;
            for (k = 0; k < 4; k++) {
                a[k] = pu;
                b[k] = pv;
                q[k] = pq;
                pu += du;
                pv += dv;
                pq += dq;
            }
            __m128 rr = _mm_div_ps(one, _mm_loadu_ps(q));
            _mm_storeu_si128((__m128i *)qu, _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(_mm_loadu_ps(a), rr), half)));
            _mm_storeu_si128((__m128i *)qv, _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(_mm_loadu_ps(b), rr), half)));
            qn = 4;
        }
        if (qn) {
            tu = qu[4 - qn];
            tv = qv[4 - qn];
            qn--;
        } else
#endif
        {
            float r = 1.0f / pq;
            tu = (int32_t)(pu * r + (1.0f / 2048));
            tv = (int32_t)(pv * r + (1.0f / 2048));
            pu += du;
            pv += dv;
            pq += dq;
        }
        tu = tu < umin ? umin : (tu > umax ? umax : tu);
        tv = tv < vmin ? vmin : (tv > vmax ? vmax : tv);
        u = ((uint32_t)tu & and_u) | or_u;
        v = ((uint32_t)tv & and_v) | or_v;
        row = &vram[((tp_y + v) & 511u) << 10];
        if (mode == 0)
            tx = clut[(row[(tp_x + (u >> 2)) & 1023u] >> ((u & 3u) << 2)) & 0xFu];
        else if (mode == 1)
            tx = clut[(row[(tp_x + (u >> 1)) & 1023u] >> ((u & 1u) << 3)) & 0xFFu];
        else
            tx = row[(tp_x + u) & 1023u];
        if (tx) {
            uint32_t rgb;
            if (c->raw) {
                rgb = tx & 0x7FFFu;
            } else if (c->gouraud) {
                rgb = modulate(tx, ig.r >> 24, ig.g >> 24, ig.b >> 24,
                               c->dither ? drow[t->dcol[(uint32_t)x & 4095u]] : NO_DITHER_LUT);
            } else {
                rgb = modulate(tx, c->flat_r, c->flat_g, c->flat_b,
                               c->dither ? drow[t->dcol[(uint32_t)x & 4095u]] : NO_DITHER_LUT);
            }
            plot_t(t, (uint32_t)x, y, rgb, tx & 0x8000u, (tx & 0x8000u) ? c->blend : -1, tg, tagv, und);
        }
        ig.r += d->dx.r;
        ig.g += d->dx.g;
        ig.b += d->dx.b;
    }
}

/* draw_span with under as und, a constant: the usual spans have no call in their loops */
static inline __attribute__((always_inline)) void span(const Target *t, const PolyCtx *c, int32_t yi, int32_t x_start,
                                                      int32_t x_bound, Interp ig, const InterpDelta *d,
                                                      const Persp *pp, const int und)
{
    int32_t x, w, x_adj;
    int32_t cx0 = S.clip_x0 * t->n, cx1 = S.clip_x1 * t->n + t->n - 1;
    uint32_t y;
    uint16_t *const tg = t->tag;
    const uint16_t tagv = cur_tag;

    if (line_skip(yi / t->n))
        return;

    x_adj = x_start;
    w = x_bound - x_start;
    x = t->n == 1 ? sext11((uint32_t)x_start) : x_start;

    if (x < cx0) {
        int32_t delta = cx0 - x;
        x_adj += delta;
        x += delta;
        w -= delta;
    }
    if (x + w > cx1 + 1)
        w = cx1 + 1 - x;
    if (und) {          /* only native pixels over marked ones: almost none */
        while (w > 0 && !under_marked(x, yi)) {
            x++;
            x_adj++;
            w--;
        }
        while (w > 0 && !under_marked(x + w - 1, yi))
            w--;
    }
    if (w <= 0)
        return;

    interp_add(&ig, &d->dx, (uint32_t)x_adj);
    interp_add(&ig, &d->dy, (uint32_t)yi);

    y = (uint32_t)yi;

    if (c->textured) {
        const uint8_t (*drow)[512] = dither_lut[(y / (uint32_t)t->n) & 3u];
        if (pp) {
            if (S.tex_mode == 0)
                persp_span(t, c, y, x, w, ig, d, pp, drow, 0, tg, tagv, und);
            else if (S.tex_mode == 1)
                persp_span(t, c, y, x, w, ig, d, pp, drow, 1, tg, tagv, und);
            else
                persp_span(t, c, y, x, w, ig, d, pp, drow, 2, tg, tagv, und);
            return;
        }
        const TexImage *const rep = tex_rep;
        const uint32_t hu = (uint32_t)((int32_t)(d->dx.u + d->dy.u) / 2), hv = (uint32_t)((int32_t)(d->dx.v + d->dy.v) / 2);
        for (; w > 0; w--, x++) {
            uint32_t tx = tex_fetch(ig.u >> 24, ig.v >> 24);
            if (rep)
                tx = rep_mix(rep_fetch(rep, ig.u + hu, ig.v + hv), tx);
            if (tx) {
                uint32_t rgb;
                if (c->raw) {
                    rgb = tx & 0x7FFFu;
                } else if (c->gouraud) {
                    rgb = modulate(tx, ig.r >> 24, ig.g >> 24, ig.b >> 24,
                                   c->dither ? drow[t->dcol[(uint32_t)x & 4095u]] : NO_DITHER_LUT);
                } else {
                    rgb = modulate(tx, c->flat_r, c->flat_g, c->flat_b,
                                   c->dither ? drow[t->dcol[(uint32_t)x & 4095u]] : NO_DITHER_LUT);
                }
                plot_t(t, (uint32_t)x, y, rgb, tx & 0x8000u, (tx & 0x8000u) ? c->blend : -1, tg, tagv, und);
            }
            ig.u += d->dx.u;
            ig.v += d->dx.v;
            ig.r += d->dx.r;
            ig.g += d->dx.g;
            ig.b += d->dx.b;
        }
    } else if (c->gouraud) {
        const uint8_t (*drow)[512] = dither_lut[(y / (uint32_t)t->n) & 3u];
        for (; w > 0; w--, x++) {
            uint32_t r = ig.r >> 24, g = ig.g >> 24, b = ig.b >> 24, rgb;
            if (c->dither) {
                const uint8_t *l = drow[t->dcol[(uint32_t)x & 4095u]];
                rgb = (uint32_t)l[r] | ((uint32_t)l[g] << 5) | ((uint32_t)l[b] << 10);
            } else {
                rgb = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);
            }
            plot_t(t, (uint32_t)x, y, rgb, 0, c->blend, tg, tagv, und);
            ig.r += d->dx.r;
            ig.g += d->dx.g;
            ig.b += d->dx.b;
        }
    } else if (c->blend < 0 && !S.mask_check && !und) {
        uint16_t *p = &t->buf[(y & t->ym) << t->sh];
        uint16_t val = (uint16_t)(c->flat_rgb15 | S.mask_set);
        uint16_t *trow = tg ? &tg[(y & t->ym) << t->sh] : NULL;
        for (; w > 0; w--, x++) {
            p[x] = val;
            if (trow)
                trow[x] = tagv;
        }
    } else {
        for (; w > 0; w--, x++)
            plot_t(t, (uint32_t)x, y, c->flat_rgb15, 0, c->blend, tg, tagv, und);
    }
}

static void draw_span(const Target *t, const PolyCtx *c, int32_t yi, int32_t x_start, int32_t x_bound,
                      Interp ig, const InterpDelta *d, const Persp *pp)
{
    if (under)
        span(t, c, yi, x_start, x_bound, ig, d, pp, 1);
    else
        span(t, c, yi, x_start, x_bound, ig, d, pp, 0);
}

static inline void swap_vtx(TriVtx *a, TriVtx *b)
{
    TriVtx t = *a;
    *a = *b;
    *b = t;
}

static inline int64_t poly_xfp(int32_t x)
{
    return (int64_t)x * 4294967296LL + 4294967296LL - 2048;
}

static inline int64_t poly_xfp_step(int32_t dx, int32_t dy)
{
    int64_t n = (int64_t)dx * 4294967296LL;
    if (n < 0)
        n -= dy - 1;
    else if (n > 0)
        n += dy - 1;
    return n / dy;
}

static inline uint32_t idelta(const Target *t, int32_t num, int32_t den)
{
    if (t->n > 1)   /* upscaled: full precision, truncation would drift over the longer spans */
        return (uint32_t)(((int64_t)num * 16777216) / den);
    return (uint32_t)(((int64_t)num * 4096) / den) << 12;
}

/* Planes of u/w, v/w, 1/w through the three vertices; 0 when a depth is unknown. */
static int persp_setup(Persp *pp, const TriVtx *a, const TriVtx *b, const TriVtx *c)
{
    const double x1 = b->x - a->x, y1 = b->y - a->y, x2 = c->x - a->x, y2 = c->y - a->y;
    const double den = x1 * y2 - x2 * y1;
    const TriVtx *v[3] = { a, b, c };
    double *pl[3] = { pp->u, pp->v, pp->q };
    int k;
    if (a->w <= 0.0f || b->w <= 0.0f || c->w <= 0.0f || den == 0.0)
        return 0;
    for (k = 0; k < 3; k++) {
        double f[3];
        int i;
        for (i = 0; i < 3; i++)
            f[i] = (k == 0 ? v[i]->u : k == 1 ? v[i]->v : 1) / (double)v[i]->w;
        pl[k][1] = ((f[1] - f[0]) * y2 - (f[2] - f[0]) * y1) / den;
        pl[k][2] = (x1 * (f[2] - f[0]) - x2 * (f[1] - f[0])) / den;
        pl[k][0] = f[0] - pl[k][1] * a->x - pl[k][2] * a->y;
    }
    pp->umin = a->u < b->u ? (a->u < c->u ? a->u : c->u) : (b->u < c->u ? b->u : c->u);
    pp->umax = a->u > b->u ? (a->u > c->u ? a->u : c->u) : (b->u > c->u ? b->u : c->u);
    pp->vmin = a->v < b->v ? (a->v < c->v ? a->v : c->v) : (b->v < c->v ? b->v : c->v);
    pp->vmax = a->v > b->v ? (a->v > c->v ? a->v : c->v) : (b->v > c->v ? b->v : c->v);
    return 1;
}

static void draw_triangle(const Target *t, const PolyCtx *c, const TriVtx *a, const TriVtx *b, const TriVtx *cc)
{
    int32_t cy0 = S.clip_y0 * t->n, cy1 = S.clip_y1 * t->n + t->n - 1;
    Persp ps;
    const Persp *pp = (t->n > 1 && c->textured && g_pgxp_persp && !tex_rep && persp_setup(&ps, a, b, cc)) ? &ps : NULL;
    TriVtx v[3];
    unsigned cv;
    int core;
    int32_t denom;
    InterpDelta d;
    Interp ig;
    int64_t base_coord, base_step, bound_us, bound_ls;
    int right_facing;
    unsigned vo, vp, i;
    struct {
        int32_t y_coord, y_bound;
        int64_t x_coord[2], x_step[2];
        int dec;
    } tp[2];

    v[0] = *a;
    v[1] = *b;
    v[2] = *cc;

    /* Core vertex (left-most, with the hardware's tie-breaking), tracked as
     * a one-hot mask through the Y sort. */
    if (v[1].x <= v[0].x)
        cv = (v[2].x <= v[1].x) ? 4u : 2u;
    else
        cv = (v[2].x < v[0].x) ? 4u : 1u;

    if (v[2].y < v[1].y) {
        swap_vtx(&v[1], &v[2]);
        cv = ((cv >> 1) & 2u) | ((cv << 1) & 4u) | (cv & 1u);
    }
    if (v[1].y < v[0].y) {
        swap_vtx(&v[0], &v[1]);
        cv = ((cv >> 1) & 1u) | ((cv << 1) & 2u) | (cv & 4u);
    }
    if (v[2].y < v[1].y) {
        swap_vtx(&v[1], &v[2]);
        cv = ((cv >> 1) & 2u) | ((cv << 1) & 4u) | (cv & 1u);
    }
    core = (int)(cv >> 1);

    if (v[0].y == v[2].y)
        return;
    if (v[2].y - v[0].y >= 512 * t->n)
        return;
    if (abs(v[2].x - v[0].x) >= 1024 * t->n || abs(v[2].x - v[1].x) >= 1024 * t->n ||
        abs(v[1].x - v[0].x) >= 1024 * t->n)
        return;

    denom = (v[1].x - v[0].x) * (v[2].y - v[1].y) - (v[2].x - v[1].x) * (v[1].y - v[0].y);
    if (!denom)
        return;

#define CALC_DX(f) ((v[1].f - v[0].f) * (v[2].y - v[1].y) - (v[2].f - v[1].f) * (v[1].y - v[0].y))
#define CALC_DY(f) ((v[1].x - v[0].x) * (v[2].f - v[1].f) - (v[2].x - v[1].x) * (v[1].f - v[0].f))
    memset(&d, 0, sizeof(d));
    if (c->gouraud) {
        d.dx.r = idelta(t, CALC_DX(r), denom);
        d.dx.g = idelta(t, CALC_DX(g), denom);
        d.dx.b = idelta(t, CALC_DX(b), denom);
        d.dy.r = idelta(t, CALC_DY(r), denom);
        d.dy.g = idelta(t, CALC_DY(g), denom);
        d.dy.b = idelta(t, CALC_DY(b), denom);
    }
    if (c->textured) {
        d.dx.u = idelta(t, CALC_DX(u), denom);
        d.dx.v = idelta(t, CALC_DX(v), denom);
        d.dy.u = idelta(t, CALC_DY(u), denom);
        d.dy.v = idelta(t, CALC_DY(v), denom);
    }
#undef CALC_DX
#undef CALC_DY

    /* Interpolants: core vertex value + 0.5, translated back to (0,0). Upscaled, UVs get only
     * 2^-11 (more than the fixed-point drift over a span): the texel is read where coverage is
     * decided, inside the triangle, so it never comes from beyond the polygon's UV range. */
    {
        const TriVtx *cvx = &v[core];
        const uint32_t uv_bias = t->n > 1 ? (1u << 13) : (1u << 23);
        ig.r = ((uint32_t)cvx->r << 24) + (1u << 23);
        ig.g = ((uint32_t)cvx->g << 24) + (1u << 23);
        ig.b = ((uint32_t)cvx->b << 24) + (1u << 23);
        ig.u = ((uint32_t)cvx->u << 24) + uv_bias;
        ig.v = ((uint32_t)cvx->v << 24) + uv_bias;
        interp_add(&ig, &d.dx, (uint32_t)(-cvx->x));
        interp_add(&ig, &d.dy, (uint32_t)(-cvx->y));
    }

    base_coord = poly_xfp(v[0].x);
    base_step = poly_xfp_step(v[2].x - v[0].x, v[2].y - v[0].y);

    if (v[1].y == v[0].y) {
        bound_us = 0;
        right_facing = v[1].x > v[0].x;
    } else {
        bound_us = poly_xfp_step(v[1].x - v[0].x, v[1].y - v[0].y);
        right_facing = bound_us > base_step;
    }
    if (v[2].y == v[1].y)
        bound_ls = 0;
    else
        bound_ls = poly_xfp_step(v[2].x - v[1].x, v[2].y - v[1].y);

    /* The hardware walks away from the core vertex: upper half bottom-up
     * when the core vertex isn't the top one, lower half bottom-up when the
     * core vertex is the bottom one. */
    vo = core ? 1u : 0u;
    vp = (core == 2) ? 3u : 0u;

    tp[vo].y_coord = v[0 ^ vo].y;
    tp[vo].y_bound = v[1 ^ vo].y;
    tp[vo].x_coord[right_facing] = poly_xfp(v[0 ^ vo].x);
    tp[vo].x_step[right_facing] = bound_us;
    tp[vo].x_coord[!right_facing] = base_coord + (int64_t)(v[vo].y - v[0].y) * base_step;
    tp[vo].x_step[!right_facing] = base_step;
    tp[vo].dec = (int)vo;

    tp[vo ^ 1].y_coord = v[1 ^ vp].y;
    tp[vo ^ 1].y_bound = v[2 ^ vp].y;
    tp[vo ^ 1].x_coord[right_facing] = poly_xfp(v[1 ^ vp].x);
    tp[vo ^ 1].x_step[right_facing] = bound_ls;
    tp[vo ^ 1].x_coord[!right_facing] = base_coord + (int64_t)(v[1 ^ vp].y - v[0].y) * base_step;
    tp[vo ^ 1].x_step[!right_facing] = base_step;
    tp[vo ^ 1].dec = (int)vp;

    for (i = 0; i < 2; i++) {
        int32_t yi = tp[i].y_coord;
        int32_t yb = tp[i].y_bound;
        int64_t lc = tp[i].x_coord[0], ls = tp[i].x_step[0];
        int64_t rc = tp[i].x_coord[1], rs = tp[i].x_step[1];

        if (tp[i].dec) {
            while (yi > yb) {
                int32_t y;
                yi--;
                lc -= ls;
                rc -= rs;
                y = t->n == 1 ? sext11((uint32_t)yi) : yi;
                if (y < cy0)
                    break;
                if (y > cy1 || !(under ? band_rows(y) : band_row(y)))
                    continue;
                draw_span(t, c, yi, fp_int(lc), fp_int(rc), ig, &d, pp);
            }
        } else {
            while (yi < yb) {
                int32_t y = t->n == 1 ? sext11((uint32_t)yi) : yi;
                if (y > cy1)
                    break;
                if (y >= cy0 && (under ? band_rows(y) : band_row(y)))
                    draw_span(t, c, yi, fp_int(lc), fp_int(rc), ig, &d, pp);
                yi++;
                lc += ls;
                rc += rs;
            }
        }
    }
}

/* A polygon in the upscaled copy (triangles v0 v1 v2 and, for a quad, v1 v2 v3), then, where it
 * left the background showing, as the console draws it at its corners u (NULL: none; see under). */
static void draw_up(const PolyCtx *c, const TriVtx *v, const TriVtx *u, int quad)
{
    draw_triangle(&TH, c, &v[0], &v[1], &v[2]);
    if (quad)
        draw_triangle(&TH, c, &v[1], &v[2], &v[3]);
    if (u) {
        under = 1;
        draw_triangle(&TN, c, &u[0], &u[1], &u[2]);
        if (quad)
            draw_triangle(&TN, c, &u[1], &u[2], &u[3]);
        under = 0;
    }
}

/* Seams (see under): a run of pixels across or down, narrower than a console pixel, drawn before
 * the 3D polygons on both sides of it (two different ones: not a hole in one texture) is a gap
 * between models that meet on the console, showing what is behind them: filled from its two sides.
 * The run ends at the first pixel drawn after all of it. An outline has something drawn before it
 * on one side only and stays as it is. All runs across are filled first, then all runs down, from
 * a copy of the picture as the first pass left it (snap): the picture is the same however the rows
 * are shared out among band workers. Columns x0.. of rows ya..yb - 1 of the upscaled copy buf, tag. */
static inline int tag_older(uint32_t a, uint32_t b)     /* a drawn before b */
{
    return ((b - a) & 0x3FFFu) - 1u < 0x1FFFu;
}

#define SEAM_ROW(p, yy) (&(p)[(uint32_t)(((yy) % (512 * TH.n) + 512 * TH.n) % (512 * TH.n)) << TH.sh])

/* ... across, in this band's rows, then copied to snap */
static void seam_across(uint16_t *buf, const uint16_t *tag, uint16_t *snap, int32_t x0, int32_t w, int32_t ya, int32_t yb)
{
    const int32_t n = TH.n;
    int32_t x, y, k, e;
    for (y = ya; y < yb; y++) {
        uint16_t *row = SEAM_ROW(buf, y);
        const uint16_t *tr = SEAM_ROW(tag, y);
        if (!band_row(y))
            continue;
        for (x = x0 + 1; x < x0 + w - 1; x++) {
            uint32_t l, m, r;
#ifdef __SSE2__
            if (x + 8 <= x0 + w) {      /* 8 pixels each as its left neighbour: nothing starts here */
                int same = _mm_movemask_epi8(_mm_cmpeq_epi16(_mm_loadu_si128((const __m128i *)&tr[x]),
                                                              _mm_loadu_si128((const __m128i *)&tr[x - 1])));
                if (same == 0xFFFF) {
                    x += 7;
                    continue;
                }
                x += __builtin_ctz(~same & 0xFFFF) / 2;
            }
#endif
            l = tr[x - 1];
            m = tr[x];
            if (m == l || TAG_KIND(l) != TAG_3D || !tag_older(m, l))
                continue;
            for (e = x + 1; e < x0 + w && e - x <= n && (tr[e] == m || tag_older(tr[e], m)); e++)
                ;
            if (e >= x0 + w || e - x > n)
                continue;
            r = tr[e];
            if (r != l && TAG_KIND(r) == TAG_3D)
                for (k = x; k < e; k++)
                    row[k] = k - x < (e - x + 1) / 2 ? row[x - 1] : row[e];
            x = e - 1;
        }
        memcpy(&SEAM_ROW(snap, y)[x0], &row[x0], (size_t)w * sizeof *row);
    }
}

/* ... down, filling this band's rows: each run is decided from where it starts (the rows above
 * one of this band's stripes that a run into it can start in included) */
static void seam_down(uint16_t *buf, const uint16_t *tag, const uint16_t *snap, int32_t x0, int32_t w, int32_t ya, int32_t yb)
{
    const int32_t n = TH.n;
    int32_t s, se, x, y, k, e;
    for (s = ya; s < yb; s = se) {
        se = band_nb ? (int32_t)(((uint32_t)s / BAND_STRIPE + 1u) * BAND_STRIPE) : yb;
        if (se > yb)
            se = yb;
        if (!band_row(s))
            continue;
        for (y = s - n + 1 > ya + 1 ? s - n + 1 : ya + 1; y < se; y++) {
            const uint16_t *tr = SEAM_ROW(tag, y), *up = SEAM_ROW(tag, y - 1);
            for (x = x0; x < x0 + w; x++) {
                uint32_t m, a, b;
#ifdef __SSE2__
                if (x + 8 <= x0 + w) {  /* 8 pixels each as the one above: nothing starts here */
                    int same = _mm_movemask_epi8(_mm_cmpeq_epi16(_mm_loadu_si128((const __m128i *)&tr[x]),
                                                                  _mm_loadu_si128((const __m128i *)&up[x])));
                    if (same == 0xFFFF) {
                        x += 7;
                        continue;
                    }
                    x += __builtin_ctz(~same & 0xFFFF) / 2;
                }
#endif
                m = tr[x];
                a = up[x];
                if (m == a || TAG_KIND(a) != TAG_3D || !tag_older(m, a))
                    continue;
                for (e = y + 1; e < yb && e - y <= n; e++) {
                    uint32_t t = SEAM_ROW(tag, e)[x];
                    if (t != m && !tag_older(t, m))
                        break;
                }
                if (e >= yb || e - y > n)
                    continue;
                b = SEAM_ROW(tag, e)[x];
                if (b != a && TAG_KIND(b) == TAG_3D)
                    for (k = y; k < e; k++)
                        if (k >= s && k < se)
                            SEAM_ROW(buf, k)[x] = k - y < (e - y + 1) / 2 ? SEAM_ROW(snap, y - 1)[x] : SEAM_ROW(snap, e)[x];
            }
        }
    }
}

/* ======================================================================= */
/* Upscaled polygons in bands                                               */
/* ======================================================================= */

/* With upscaling, the GPU draws each polygon into the native VRAM itself (the game reads that
 * back, and textures come from it) and queues its upscaled drawing: band workers (threads, each
 * with a copy of this file of its own: gpu_band1.c ...) draw the queue in order, each into its
 * stripes of the picture (band_row). Every upscaled pixel gets the same polygons in the same
 * order as when drawn here, so the picture is the same. The GPU waits for the queue to be drawn
 * before it draws anything else upscaled (sprites, lines, fills, copies, transfers), before the
 * picture is shown, and before it draws natively over a texture a queued polygon reads. An item
 * holds what drawing the polygon reads besides the native VRAM. */
#define BQ 4096                     /* queue size (a power of 2) */
#define BAND_MAX 4                  /* band workers at most */
typedef struct {
    PolyCtx c;
    TriVtx v[4], u[4];              /* u: the console's corners, native (under) */
    int quad, under, persp;
    uint16_t tag;
    int seam;                       /* 1, 2: not a polygon but seam_across, seam_down of the picture: */
    uint16_t *sbuf, *stag, *ssnap;  /* ... buf, tag, snap, columns sx0 .. + sw, rows sy0 .. + sh */
    int32_t sx0, sy0, sw, sh;
    uint32_t tp_x, tp_y, tex_mode, tw_and_u, tw_or_u, tw_and_v, tw_or_v;
    int32_t clip_x0, clip_y0, clip_x1, clip_y1;
    uint32_t mask_set, mask_check, disp_mode, dfe, field;
    uint16_t clut[256];
} BandItem;

#ifdef GPU_BAND
/* Draws queue items from .. to - 1 (ring[i % BQ]) into stripe k of nb (band_row). th: the GPU's
 * upscaled target (copied when gen changes). */
void GPU_BAND_RUN(const BandItem *ring, uint64_t from, uint64_t to, const Target *th, uint32_t gen,
                  const uint16_t *vram, int k, int nb)
{
    static uint32_t seen;
    if (!dither_lut_ready)
        build_dither_lut();
    if (gen != seen) {
        TH = *th;
        seen = gen;
    }
    tex_vram = vram;
    band_k = k;
    band_nb = nb;
    for (; from < to; from++) {
        const BandItem *it = &ring[from & (BQ - 1)];
        if (it->seam) {
            if (it->seam == 1)
                seam_across(it->sbuf, it->stag, it->ssnap, it->sx0, it->sw, it->sy0, it->sy0 + it->sh);
            else
                seam_down(it->sbuf, it->stag, it->ssnap, it->sx0, it->sw, it->sy0, it->sy0 + it->sh);
            continue;
        }
        S.tp_x = it->tp_x;
        S.tp_y = it->tp_y;
        S.tex_mode = it->tex_mode;
        S.tw_and_u = it->tw_and_u;
        S.tw_or_u = it->tw_or_u;
        S.tw_and_v = it->tw_and_v;
        S.tw_or_v = it->tw_or_v;
        S.clip_x0 = it->clip_x0;
        S.clip_y0 = it->clip_y0;
        S.clip_x1 = it->clip_x1;
        S.clip_y1 = it->clip_y1;
        S.mask_set = it->mask_set;
        S.mask_check = it->mask_check;
        S.disp_mode = it->disp_mode;
        S.dfe = it->dfe;
        S.field = it->field;
        tex_clut = it->clut;
        g_pgxp_persp = it->persp;
        cur_tag = it->tag;
        draw_up(&it->c, it->v, it->under ? it->u : NULL, it->quad);
    }
}
#endif

#if !defined(GPU_REPLAY) && !defined(GPU_BAND)
typedef void (*BandRun)(const BandItem *, uint64_t, uint64_t, const Target *, uint32_t, const uint16_t *, int, int);
#ifdef GPU_VIEW2
/* the second view's GPU has band workers of its own (gpu_view2_band1.c ...): the game's draw the
 * game's queue at the same time */
#define band1_run view2_band1_run
#define band2_run view2_band2_run
#define band3_run view2_band3_run
#define band4_run view2_band4_run
#endif
void band1_run(const BandItem *, uint64_t, uint64_t, const Target *, uint32_t, const uint16_t *, int, int);
void band2_run(const BandItem *, uint64_t, uint64_t, const Target *, uint32_t, const uint16_t *, int, int);
void band3_run(const BandItem *, uint64_t, uint64_t, const Target *, uint32_t, const uint16_t *, int, int);
void band4_run(const BandItem *, uint64_t, uint64_t, const Target *, uint32_t, const uint16_t *, int, int);
static const BandRun band_run[BAND_MAX] = { band1_run, band2_run, band3_run, band4_run };
void place_thread(int which, int nbands);       /* cpu_place.c */

#define BQ_WAKE 64                  /* sleeping workers are woken once this many items wait */
static BandItem *bq;
static uint64_t bq_pub;                       /* items queued (the GPU's count) */
static uint64_t bq_woke;                      /* bq_pub when the workers were last woken */
static _Atomic uint64_t bq_head;              /* items queued, for the workers */
static _Atomic uint64_t bq_done[BAND_MAX];    /* items each worker has drawn */
static _Atomic int bq_sleepers, bq_syncing, nbands;
static int band_threads, band_planned;       /* started; to be started (place_thread) */
static uint32_t th_gen = 1;                   /* changes with TH */
/* the native VRAM queued polygons read textures from: a bit per 16x16 tile, rows of 64 */
static uint64_t tex_pend[32];
static int tex_pend_any;

static inline void cpu_relax(void)
{
#ifdef __SSE2__
    _mm_pause();
#endif
}

static uint64_t band_min_done(void)
{
    uint64_t m = bq_pub;
    int k, nb = atomic_load(&nbands);
    for (k = 0; k < nb; k++) {
        uint64_t d = atomic_load(&bq_done[k]);
        if (d < m)
            m = d;
    }
    return m;
}

static int band_worker(void *arg)
{
    int k = (int)(intptr_t)arg, spin;
    uint64_t done = atomic_load(&bq_head);
    /* a performance core of its own, or where the game's thread runs (a worker on a slow core would
     * hold the others up, as every picture waits for all of them) */
    place_thread(1 + INTERP_WORKERS + k, band_planned);
    atomic_store(&bq_done[k], done);
    for (;;) {
        uint64_t head = atomic_load(&bq_head);
        if (head != done) {
            int nb = atomic_load(&nbands);
            if (k < nb)
                band_run[k](bq, done, head, &TH, th_gen, S.vram, k, nb);
            done = head;
            atomic_store(&bq_done[k], done);
            if (atomic_load(&bq_syncing)) {
                wk_lock();
                wk_wake(WK_DONE);
                wk_unlock();
            }
            continue;
        }
        for (spin = 0; spin < 256 && atomic_load_explicit(&bq_head, memory_order_relaxed) == done; spin++)
            cpu_relax();
        if (atomic_load(&bq_head) != done)
            continue;
        wk_lock();
        atomic_fetch_add(&bq_sleepers, 1);
        while (atomic_load(&bq_head) == done)
            wk_wait(WK_WORK);
        atomic_fetch_sub(&bq_sleepers, 1);
        wk_unlock();
    }
    return 0;
}

/* wait until the queue is drawn */
static void band_sync(void)
{
    if (!atomic_load(&nbands))
        return;
    if (band_min_done() < bq_pub) {
        atomic_store(&bq_syncing, 1);
        bq_woke = bq_pub;
        wk_lock();
        wk_wake(WK_WORK);
        while (band_min_done() < bq_pub)
            wk_wait(WK_DONE);
        wk_unlock();
        atomic_store(&bq_syncing, 0);
    }
    if (tex_pend_any) {
        memset(tex_pend, 0, sizeof tex_pend);
        tex_pend_any = 0;
    }
}

/* Before drawing natively over x0..x1, y0..y1 (as given to draw_triangle): wait if a queued
 * polygon reads a texture there. Coordinates beyond +-1024 wrap around: wait to be safe. */
static void band_native_draw(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint64_t bits = 0;
    int32_t tx, ty;
    if (!tex_pend_any)
        return;
    if (x0 < -1024 || x1 > 1023 || y0 < -1024 || y1 > 1023) {
        band_sync();
        return;
    }
    x0 = x0 < S.clip_x0 ? S.clip_x0 : x0;
    x1 = x1 > S.clip_x1 ? S.clip_x1 : x1;
    y0 = y0 < S.clip_y0 ? S.clip_y0 : y0;
    y1 = y1 > S.clip_y1 ? S.clip_y1 : y1;
    if (x0 > x1 || y0 > y1)
        return;
    for (tx = x0 >> 4; tx <= x1 >> 4; tx++)
        bits |= 1ull << (tx & 63);
    for (ty = y0 >> 4; ty <= y1 >> 4; ty++)
        if (tex_pend[ty & 31] & bits) {
            band_sync();
            return;
        }
}

static void band_push(const PolyCtx *c, const TriVtx *v, const TriVtx *u, int quad)
{
    BandItem *it;
    if (bq_pub - band_min_done() >= BQ)
        band_sync();
    it = &bq[bq_pub & (BQ - 1)];
    it->c = *c;
    memcpy(it->v, v, (quad ? 4 : 3) * sizeof *v);
    if (u)
        memcpy(it->u, u, (quad ? 4 : 3) * sizeof *u);
    it->quad = quad;
    it->under = u != NULL;
    it->tag = cur_tag;
    it->seam = 0;
    it->persp = g_pgxp_persp;
    it->tp_x = S.tp_x;
    it->tp_y = S.tp_y;
    it->tex_mode = S.tex_mode;
    it->tw_and_u = S.tw_and_u;
    it->tw_or_u = S.tw_or_u;
    it->tw_and_v = S.tw_and_v;
    it->tw_or_v = S.tw_or_v;
    it->clip_x0 = S.clip_x0;
    it->clip_y0 = S.clip_y0;
    it->clip_x1 = S.clip_x1;
    it->clip_y1 = S.clip_y1;
    it->mask_set = S.mask_set;
    it->mask_check = S.mask_check;
    it->disp_mode = S.disp_mode;
    it->dfe = S.dfe;
    it->field = S.field;
    if (c->textured) {
        /* the texture page it reads: 64, 128 or 256 halfwords wide, 256 lines */
        uint32_t w = S.tex_mode == 0 ? 64u : S.tex_mode == 1 ? 128u : 256u, i;
        uint64_t bits = 0;
        if (S.tex_mode < 2)
            memcpy(it->clut, S.clut, sizeof it->clut);
        for (i = 0; i < w / 16u; i++)
            bits |= 1ull << (((S.tp_x >> 4) + i) & 63u);
        for (i = 0; i < 16u; i++)
            tex_pend[((S.tp_y >> 4) + i) & 31u] |= bits;
        tex_pend_any = 1;
    }
    bq_pub++;
    atomic_store(&bq_head, bq_pub);
    /* waking the workers takes the lock and sends them through the scheduler: woken for every
     * polygon (workers asleep between slowly coming polygons), that cost frames tens of ms */
    if (bq_pub - bq_woke >= BQ_WAKE && atomic_load(&bq_sleepers)) {
        bq_woke = bq_pub;
        wk_lock();
        wk_wake(WK_WORK);
        wk_unlock();
    }
}

/* The band workers' seam_across (kind 1) or seam_down (2) of the picture in buf, tag */
static void band_push_seam(int kind, uint16_t *buf, uint16_t *tag, uint16_t *snap, int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    BandItem *it;
    if (bq_pub - band_min_done() >= BQ)
        band_sync();
    it = &bq[bq_pub & (BQ - 1)];
    it->seam = kind;
    it->sbuf = buf;
    it->stag = tag;
    it->ssnap = snap;
    it->sx0 = x0;
    it->sy0 = y0;
    it->sw = w;
    it->sh = h;
    bq_pub++;
    atomic_store(&bq_head, bq_pub);
}

/* n band workers draw the upscaled polygons (0: the GPU draws them itself); after gpu_set_upscale */
void gpu_set_threads(int n)
{
    band_sync();
    if (n > BAND_MAX)
        n = BAND_MAX;
    if (n > 0 && (!TH.buf || (!bq && !(bq = malloc(BQ * sizeof *bq)))))
        n = 0;
    if (n > band_planned)
        band_planned = n;
    while (band_threads < n && wk_thread(band_worker, (void *)(intptr_t)band_threads))
        band_threads++;
    atomic_store(&nbands, n < band_threads ? n : band_threads);
}

int gpu_threads(void)
{
    return atomic_load(&nbands);
}
#define BAND_SYNC() band_sync()
#else
#define BAND_SYNC() ((void)0)
#endif

/* Texture packs: the replacement for the w x h texels from u0, v0 (the current texture page and
 * CLUT), found by their key; with g_texdump, they are written out. */
static const TexImage *texpack_box(int32_t u0, int32_t v0, int32_t w, int32_t h)
{
    static uint16_t box[256 * 256];
    int32_t x, y;
    uint64_t key;
    if (S.tw_and_u != 0xFFu || S.tw_or_u || S.tw_and_v != 0xFFu || S.tw_or_v || w < 1 || h < 1 || w > 256 || h > 256)
        return NULL;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            box[y * w + x] = (uint16_t)tex_fetch((uint32_t)(u0 + x), (uint32_t)(v0 + y));
    key = texpack_key(w, h, box);
#if !defined(GPU_REPLAY) && !defined(GPU_BAND)
    if (g_texdump)
        texpack_dump(key, w, h, box);
#endif
    return g_texpack ? texpack_find(key) : NULL;
}

/* ... for a 2D sprite quad v built at a projected point (tagged t: people, animals, items), by the
 * texels of its UV box (min to max, both included: the image the game drew it from, also when
 * mirrored), stretched over the box for rep_fetch. */
static const TexImage *texpack_quad(const PgxpWord *t, const TriVtx *v, int nv)
{
    int32_t u0 = v[0].u, u1 = v[0].u, v0 = v[0].v, v1 = v[0].v, i;
    const TexImage *r;
    if (t->cx == HUD_CX || t->cx == MENU_CX)
        return NULL;
    for (i = 1; i < nv; i++) {
        u0 = v[i].u < u0 ? v[i].u : u0;
        u1 = v[i].u > u1 ? v[i].u : u1;
        v0 = v[i].v < v0 ? v[i].v : v0;
        v1 = v[i].v > v1 ? v[i].v : v1;
    }
    if (u1 == u0 || v1 == v0 || !(r = texpack_box(u0, v0, u1 - u0 + 1, v1 - v0 + 1)))
        return NULL;
    rep_u0 = (float)u0;
    rep_v0 = (float)v0;
    rep_su = (float)r->w / (float)(u1 - u0);
    rep_sv = (float)r->h / (float)(v1 - v0);
    return r;
}

static void cmd_polygon(const uint32_t *p)
{
    uint32_t op = p[0] >> 24;
    buf_drawn[draw_buf()] = anchor_gen;
    int gouraud = (op >> 4) & 1;
    int quad = (op >> 3) & 1;
    int tex = (op >> 2) & 1;
    int abe = (op >> 1) & 1;
    int raw = op & 1;
    int nv = quad ? 4 : 3;
    TriVtx v[4];
    PolyCtx c;
    uint32_t clut = 0, tpage = 0;
    int i, idx = 0, xyi[4];

    for (i = 0; i < nv; i++) {
        uint32_t col, xy;
        if (i == 0 || gouraud)
            col = p[idx++];
        else
            col = p[0];
        v[i].r = (int32_t)(col & 0xFFu);
        v[i].g = (int32_t)((col >> 8) & 0xFFu);
        v[i].b = (int32_t)((col >> 16) & 0xFFu);
        v[i].w = 0.0f;
        xyi[i] = idx;
        xy = p[idx++];
        v[i].x = sext11(xy) + S.offs_x;
        v[i].y = sext11(xy >> 16) + S.offs_y;
        if (tex) {
            uint32_t uv = p[idx++];
            v[i].u = (int32_t)(uv & 0xFFu);
            v[i].v = (int32_t)((uv >> 8) & 0xFFu);
            if (i == 0)
                clut = uv >> 16;
            else if (i == 1)
                tpage = uv >> 16;
        } else {
            v[i].u = 0;
            v[i].v = 0;
        }
    }

    if (tex) {
        set_texpage(tpage, 0);
        clut_update(clut);
    }

    c.gouraud = gouraud;
    c.textured = tex;
    c.raw = tex && raw;
    c.dither = S.dither && (gouraud || (tex && !raw));
    c.blend = abe ? (int)S.abr : -1;
    c.flat_r = (uint32_t)v[0].r;
    c.flat_g = (uint32_t)v[0].g;
    c.flat_b = (uint32_t)v[0].b;
    c.flat_rgb15 = (c.flat_r >> 3) | ((c.flat_g >> 3) << 5) | ((c.flat_b >> 3) << 10);

    /* Fast path: modulation by 808080h without dithering is the identity. */
    if (tex && !raw && !gouraud && !c.dither &&
        c.flat_r == 0x80 && c.flat_g == 0x80 && c.flat_b == 0x80)
        c.raw = 1;

    if (!replaying) {           /* a redraw only needs the upscaled copy */
#if !defined(GPU_REPLAY) && !defined(GPU_BAND)
        if (tex_pend_any) {     /* over a texture a queued polygon reads? */
            int32_t x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
            for (i = 1; i < nv; i++) {
                x0 = v[i].x < x0 ? v[i].x : x0;
                x1 = v[i].x > x1 ? v[i].x : x1;
                y0 = v[i].y < y0 ? v[i].y : y0;
                y1 = v[i].y > y1 ? v[i].y : y1;
            }
            band_native_draw(x0, y0, x1, y1);
        }
#endif
        draw_triangle(&TN, &c, &v[0], &v[1], &v[2]);
        if (quad)
            draw_triangle(&TN, &c, &v[1], &v[2], &v[3]);
    }
    if (TH.buf) {
        const PgxpWord *st = NULL;
        const TexImage *rep = NULL;
        PgxpWord stmp;
        if (g_sprite_tags) {    /* a 2D sprite quad built at a projected point: every corner tagged alike */
            const PgxpWord *t0 = &pgxp_mem[(cmd_addr[xyi[0]] & 0x1FFFFCu) >> 2];
            int same = 1;
            for (i = 0; i < nv && same; i++) {
                const PgxpWord *t = &pgxp_mem[(cmd_addr[xyi[i]] & 0x1FFFFCu) >> 2];
                same = cmd_addr[xyi[i]] != 0xFFFFFFFFu && t->w < 0.0f && t->value == p[xyi[i]] && t->x == t0->x &&
                       t->y == t0->y && t->w == t0->w && t->cx == t0->cx && t->cy == t0->cy;
            }
            if (same)
                st = t0;
            if (st && st->cx == HUD_CX && hud_sx() != 1.0f)
                hud_after = 1;
            if (!st && hud_after && hud_sx() != 1.0f) {
                /* a flat 2D polygon after the HUD is HUD too (the rucksack's selection box is four
                 * textured quads), unless it covers the whole width (fades) or is projected 3D */
                int32_t x0 = v[0].x, x1 = v[0].x;
                int flat = 1;
                for (i = 0; i < nv && flat; i++) {
                    float fx, fy, fw;
                    flat = !(g_pgxp && pgxp_lookup(cmd_addr[xyi[i]], p[xyi[i]], &fx, &fy, &fw));
                    x0 = v[i].x < x0 ? v[i].x : x0;
                    x1 = v[i].x > x1 ? v[i].x : x1;
                }
                if (flat && !(x0 <= S.clip_x0 && x1 > S.clip_x1))
                    st = &hud_word;
            }
            st = interp_tag(st, &stmp);
            if (st && tex && quad && (g_texpack || g_texdump))
                rep = texpack_quad(st, v, nv);
        }
        /* u: the console's corners, for the background a 3D polygon leaves showing (under); in an
         * in-between picture, moved as far as the precise ones, rounded (corners that meet on the
         * console move alike and still meet; rounding each moved position could split them) */
        TriVtx u[4];
        int precise = 0, moved = 0;
        for (i = 0; i < nv; i++) {
            float fx, fy, fw;
            u[i] = v[i];
            if (g_pgxp && pgxp_lookup(cmd_addr[xyi[i]], p[xyi[i]], &fx, &fy, &fw)) {
                if (replaying) {
                    float ox = fx, oy = fy;
                    interp_point(&fx, &fy, &fw);
                    u[i].x += (int32_t)lrintf(fx - ox);
                    u[i].y += (int32_t)lrintf(fy - oy);
                }
                v[i].x = (int32_t)lrintf((fx + (float)S.offs_x) * (float)TH.n);
                v[i].y = (int32_t)lrintf((fy + (float)S.offs_y) * (float)TH.n);
                v[i].w = fw;
                precise = 1;
            } else if (st) {
                v[i].x = sprite_coord(v[i].x, sprite_cx(st), st->x, sprite_sx(st));
                v[i].y = sprite_coord(v[i].y, st->cy + S.offs_y, st->y, sprite_sy(st));
            } else {
                v[i].x *= TH.n;
                v[i].y *= TH.n;
            }
            moved |= u[i].x * TH.n != v[i].x || u[i].y * TH.n != v[i].y;
        }
        if (precise && !st)
            cur_tag = (uint16_t)(TAG_3D | (cur_tag & 0x3FFFu));
        precise = precise && moved && !st && c.blend < 0 && TH.tag;   /* see-through ones would blend twice */
#if !defined(GPU_REPLAY) && !defined(GPU_BAND)
        if (atomic_load_explicit(&nbands, memory_order_relaxed)) {
            if (!rep) {
                band_push(&c, v, precise ? u : NULL, quad);
                return;
            }
            band_sync();        /* a replaced texture is drawn here, over the polygons queued before it */
        }
#endif
        tex_rep = rep;
        draw_up(&c, v, precise ? u : NULL, quad);
        tex_rep = NULL;
    }
}

/* ======================================================================= */
/* Lines                                                                    */
/* ======================================================================= */

static inline int64_t line_divide(int32_t delta, int32_t dk)
{
    int64_t n = (int64_t)delta * 4294967296LL;
    if (n < 0)
        n -= dk - 1;
    else if (n > 0)
        n += dk - 1;
    return n / dk;
}

static void draw_line(LinePt p0, LinePt p1, int gouraud, int blend)
{
    int32_t adx = abs(p1.x - p0.x);
    int32_t ady = abs(p1.y - p0.y);
    int32_t k = adx > ady ? adx : ady;
    int64_t sx = 0, sy = 0, cx, cy;
    int32_t sr = 0, sg = 0, sb = 0;
    uint32_t cr, cg, cb;
    int32_t i;

    if (adx >= 1024 || ady >= 512)
        return;

    if (p0.x > p1.x && k) {
        LinePt t = p0;
        p0 = p1;
        p1 = t;
    }

    if (k) {
        sx = line_divide(p1.x - p0.x, k);
        sy = line_divide(p1.y - p0.y, k);
        if (gouraud) {
            sr = ((p1.r - p0.r) * 4096) / k;
            sg = ((p1.g - p0.g) * 4096) / k;
            sb = ((p1.b - p0.b) * 4096) / k;
        }
    }

    cx = (int64_t)p0.x * 4294967296LL + 2147483648LL - 1024;
    cy = (int64_t)p0.y * 4294967296LL + 2147483648LL;
    if (sy < 0)
        cy -= 1024;
    cr = ((uint32_t)p0.r << 12) | 0x800u;
    cg = ((uint32_t)p0.g << 12) | 0x800u;
    cb = ((uint32_t)p0.b << 12) | 0x800u;

    for (i = 0; i <= k; i++) {
        uint32_t x = (uint32_t)((uint64_t)cx >> 32) & 2047u;
        uint32_t y = (uint32_t)((uint64_t)cy >> 32) & 2047u;

        if (!line_skip((int32_t)y) &&
            (int32_t)x >= S.clip_x0 && (int32_t)x <= S.clip_x1 &&
            (int32_t)y >= S.clip_y0 && (int32_t)y <= S.clip_y1) {
            uint32_t r, g, b, rgb;
            if (gouraud) {
                r = (cr >> 12) & 0xFFu;
                g = (cg >> 12) & 0xFFu;
                b = (cb >> 12) & 0xFFu;
            } else {
                r = (uint32_t)p0.r;
                g = (uint32_t)p0.g;
                b = (uint32_t)p0.b;
            }
            if (S.dither) {
                const uint8_t *l = dither_lut[y & 3u][x & 3u];
                rgb = (uint32_t)l[r] | ((uint32_t)l[g] << 5) | ((uint32_t)l[b] << 10);
            } else {
                rgb = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);
            }
            plot2(x, y, rgb, 0, blend);
        }

        cx += sx;
        cy += sy;
        cr += (uint32_t)sr;
        cg += (uint32_t)sg;
        cb += (uint32_t)sb;
    }
}

static inline void line_color(LinePt *pt, uint32_t col)
{
    pt->r = (int32_t)(col & 0xFFu);
    pt->g = (int32_t)((col >> 8) & 0xFFu);
    pt->b = (int32_t)((col >> 16) & 0xFFu);
}

static inline void line_xy(LinePt *pt, uint32_t xy)
{
    pt->x = sext11(xy) + S.offs_x;
    pt->y = sext11(xy >> 16) + S.offs_y;
}

static void cmd_line(const uint32_t *p)
{
    uint32_t op = p[0] >> 24;
    buf_drawn[draw_buf()] = anchor_gen;
    int gouraud = (op >> 4) & 1;
    int poly = (op >> 3) & 1;
    int abe = (op >> 1) & 1;
    LinePt a, b;

    line_color(&a, p[0]);
    line_xy(&a, p[1]);
    if (gouraud) {
        line_color(&b, p[2]);
        line_xy(&b, p[3]);
    } else {
        b.r = a.r;
        b.g = a.g;
        b.b = a.b;
        line_xy(&b, p[2]);
    }

    if (TH.buf && g_sprite_ws != 1.0f) {   /* a HUD line (window border), or a line after the HUD */
        const PgxpWord *t = cmd_addr[1] != 0xFFFFFFFFu ? &pgxp_mem[(cmd_addr[1] & 0x1FFFFCu) >> 2] : NULL;
        int tagged = t && t->w < 0.0f && t->value == p[1];
        if (tagged && t->cx == HUD_CX && hud_sx() != 1.0f)
            hud_after = 1;
        if (hud_after && (!tagged || t->cx == HUD_CX)) {
            line_sq = g_sprite_ws;
            line_c = sprite_cx(&hud_word);
        }
    }
    draw_line(a, b, gouraud, abe ? (int)S.abr : -1);
    line_sq = 1.0f;

    if (poly) {
        S.gp0_state = GP0_STATE_POLYLINE;
        S.pl_op = op;
        S.pl_count = 0;
        S.pl_prev = b;
    }
}

static void polyline_word(uint32_t w)
{
    int gouraud = (S.pl_op >> 4) & 1;
    int abe = (S.pl_op >> 1) & 1;
    LinePt b;

    if (S.pl_count == 0 && (w & 0xF000F000u) == 0x50005000u) {
        S.gp0_state = GP0_STATE_COMMAND;
        return;
    }
    S.pl_words[S.pl_count++] = w;
    if (S.pl_count < (gouraud ? 2 : 1))
        return;
    S.pl_count = 0;

    if (gouraud) {
        line_color(&b, S.pl_words[0]);
        line_xy(&b, S.pl_words[1]);
    } else {
        b.r = S.pl_prev.r;
        b.g = S.pl_prev.g;
        b.b = S.pl_prev.b;
        line_xy(&b, S.pl_words[0]);
    }
    draw_line(S.pl_prev, b, gouraud, abe ? (int)S.abr : -1);
    S.pl_prev = b;
}

/* ======================================================================= */
/* Rectangles                                                               */
/* ======================================================================= */

static void draw_sprite(int32_t x0, int32_t y0, int32_t w, int32_t h,
                        uint32_t u0, uint32_t v0, uint32_t color,
                        int tex, int raw, int blend)
{
    uint32_t r = color & 0xFFu, g = (color >> 8) & 0xFFu, b = (color >> 16) & 0xFFu;
    uint32_t fill = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);
    int32_t x_start = x0, x_bound = x0 + w;
    int32_t y_start = y0, y_bound = y0 + h;
    uint8_t u = (uint8_t)u0, v = (uint8_t)v0;
    int u_inc = 1, v_inc = 1;
    int32_t x, y;

    if (tex) {
        if (S.flip_x) {
            u_inc = -1;
            u |= 1;
        }
        if (S.flip_y)
            v_inc = -1;
        /* modulation by 808080h is the identity (sprites are never dithered) */
        if (!raw && color == 0x808080u)
            raw = 1;
    }

    if (x_start < S.clip_x0) {
        if (tex)
            u = (uint8_t)(u + (S.clip_x0 - x_start) * u_inc);
        x_start = S.clip_x0;
    }
    if (y_start < S.clip_y0) {
        if (tex)
            v = (uint8_t)(v + (S.clip_y0 - y_start) * v_inc);
        y_start = S.clip_y0;
    }
    if (x_bound > S.clip_x1 + 1)
        x_bound = S.clip_x1 + 1;
    if (y_bound > S.clip_y1 + 1)
        y_bound = S.clip_y1 + 1;

    if (x_bound <= x_start)
        return;

    for (y = y_start; y < y_bound; y++) {
        if (!line_skip(y)) {
            if (tex) {
                uint8_t ur = u;
                for (x = x_start; x < x_bound; x++) {
                    uint32_t t = tex_fetch(ur, v);
                    if (t) {
                        uint32_t rgb = raw ? (t & 0x7FFFu) : modulate(t, r, g, b, NO_DITHER_LUT);
                        plot2((uint32_t)x, (uint32_t)y, rgb, t & 0x8000u,
                              (t & 0x8000u) ? blend : -1);
                    }
                    ur = (uint8_t)(ur + u_inc);
                }
            } else if (blend < 0 && !S.mask_check && !TH.buf) {
                uint16_t *row = &TN.buf[((uint32_t)y & 511u) << 10];
                uint16_t val = (uint16_t)(fill | S.mask_set);
                for (x = x_start; x < x_bound; x++)
                    row[x] = val;
            } else {
                for (x = x_start; x < x_bound; x++)
                    plot2((uint32_t)x, (uint32_t)y, fill, 0, blend);
            }
        }
        v = (uint8_t)(v + v_inc);
    }
}

/* The upscaled copy of a tagged sprite rectangle scaled by sx, sy (depth, widescreen): the target
 * pixels between its scaled corners (and target columns cx0..cx1 - 1), each taking the texel under
 * its centre (nearest, like the native sprite), or with tex_rep the replacement image's pixel there
 * (also for untagged ones). */
static void draw_sprite_scaled(int32_t x0, int32_t y0, int32_t w, int32_t h, uint32_t u0, uint32_t v0,
                               uint32_t color, int tex, int raw, int blend, const PgxpWord *t,
                               float sx, float sy, int32_t cx0, int32_t cx1)
{
    static int32_t col_tx[4096], col_ix[4096];
    const int32_t n = TH.n, cx = sprite_cx(t), cy = t->cy + S.offs_y;
    const float fn = (float)n;
    uint32_t r = color & 0xFFu, g = (color >> 8) & 0xFFu, b = (color >> 16) & 0xFFu;
    uint32_t fill = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);
    int32_t X0 = sprite_coord(x0, cx, t->x, sx), X1 = sprite_coord(x0 + w, cx, t->x, sx);
    int32_t Y0 = sprite_coord(y0, cy, t->y, sy), Y1 = sprite_coord(y0 + h, cy, t->y, sy);
    uint8_t us = (uint8_t)u0;
    int u_inc = 1, v_inc = 1;
    int32_t px, py;

    if (tex) {
        if (S.flip_x) {
            u_inc = -1;
            us |= 1;
        }
        if (S.flip_y)
            v_inc = -1;
        if (!raw && color == 0x808080u)
            raw = 1;
    }
    if (X0 < cx0) X0 = cx0;
    if (Y0 < S.clip_y0 * n) Y0 = S.clip_y0 * n;
    if (X1 > cx1) X1 = cx1;
    if (Y1 > (S.clip_y1 + 1) * n) Y1 = (S.clip_y1 + 1) * n;
    if (X1 - X0 > 4096)
        X1 = X0 + 4096;

    for (px = X0; px < X1; px++) {      /* each column's texel (and replacement pixel), once */
        float fx = (((float)px + 0.5f) / fn - (float)cx - t->x) / sx + (float)(cx - x0);
        int32_t tx = (int32_t)floorf(fx);
        col_tx[px - X0] = tx < 0 ? 0 : tx >= w ? w - 1 : tx;
        if (tex_rep) {
            int32_t ix = (int32_t)(fx * rep_su);
            col_ix[px - X0] = ix < 0 ? 0 : ix >= tex_rep->w ? tex_rep->w - 1 : ix;
        }
    }
    for (py = Y0; py < Y1; py++) {
        float fy = (((float)py + 0.5f) / fn - (float)cy - t->y) / sy + (float)(cy - y0);
        int32_t ty = (int32_t)floorf(fy), iy = tex_rep ? (int32_t)(fy * rep_sv) : 0;
        const uint16_t *rrow = tex_rep ? &tex_rep->px[(iy < 0 ? 0 : iy >= tex_rep->h ? tex_rep->h - 1 : iy) * tex_rep->w] : NULL;
        uint8_t vr;
        if (line_skip(py / n))
            continue;
        ty = ty < 0 ? 0 : ty >= h ? h - 1 : ty;
        vr = (uint8_t)(v0 + (uint32_t)(ty * v_inc));
        for (px = X0; px < X1; px++) {
            int32_t tx = col_tx[px - X0];
            if (tex) {
                uint32_t tt = tex_fetch((uint8_t)(us + (uint32_t)(tx * u_inc)), vr);
                if (rrow)
                    tt = rep_mix(rrow[col_ix[px - X0]], tt);
                if (tt)
                    plot(&TH, (uint32_t)px, (uint32_t)py, raw ? (tt & 0x7FFFu) : modulate(tt, r, g, b, NO_DITHER_LUT),
                         tt & 0x8000u, (tt & 0x8000u) ? blend : -1);
            } else {
                plot(&TH, (uint32_t)px, (uint32_t)py, fill, 0, blend);
            }
        }
    }
}

/* Widescreen: the upscaled copy of a tile of a full-screen 2D background (menus, title screen) that
 * repeats every period native pixels across, squeezed about the middle like the HUD so it keeps its
 * shape, and repeated to fill the wide picture: the drawing area shows it where it is, the period
 * left of the area's left edge shows it one period left, the period before that two periods left,
 * and so on, likewise to the right. Every target pixel takes one of the copies, so see-through
 * tiles aren't blended twice. */
static void bg_wide(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t u, uint32_t v, uint32_t color,
                    int tex, int raw, int blend, int32_t period)
{
    const float s = g_sprite_ws;
    const int32_t n = TH.n, c = sprite_cx(&hud_word), x0 = S.clip_x0, x1 = S.clip_x1 + 1;
    const int32_t K = (int32_t)ceilf((float)(x1 - x0) * 0.5f * (1.0f / s - 1.0f) / (float)period);
    int32_t k;
    for (k = -K; k <= K; k++) {
        /* native span of copy k: the drawing area, or the k-th period beside it */
        int32_t a = k < 0 ? x0 + k * period : k > 0 ? x1 + (k - 1) * period : x0, b = k ? a + period : x1;
        int32_t A = sprite_coord(a, c, 0.0f, s), B = sprite_coord(b, c, 0.0f, s);
        if (x + k * period < b && x + k * period + w > a)
            draw_sprite_scaled(x + k * period, y, w, h, u, v, color, tex, raw, blend, &hud_word, s, 1.0f,
                               A < x0 * n ? x0 * n : A, B > x1 * n ? x1 * n : B);
    }
}

static void cmd_rect(const uint32_t *p)
{
    uint32_t op = p[0] >> 24;
    buf_drawn[draw_buf()] = anchor_gen;
    int tex = (op >> 2) & 1;
    int abe = (op >> 1) & 1;
    int raw = op & 1;
    uint32_t size = (op >> 3) & 3u;
    uint32_t color = p[0] & 0xFFFFFFu;
    uint32_t xy, u = 0, v = 0;
    int32_t x, y, w, h;
    int idx = 1;

    xy = p[idx++];
    x = sext11(xy);
    y = sext11(xy >> 16);

    if (tex) {
        uint32_t uv = p[idx++];
        u = uv & 0xFFu;
        v = (uv >> 8) & 0xFFu;
        clut_update(uv >> 16);
    }

    switch (size) {
    case 0: {
        uint32_t wh = p[idx++];
        w = (int32_t)(wh & 0x3FFu);
        h = (int32_t)((wh >> 16) & 0x1FFu);
        break;
    }
    case 1: w = 1; h = 1; break;
    case 2: w = 8; h = 8; break;
    default: w = 16; h = 16; break;
    }

    x = sext11((uint32_t)(x + S.offs_x));
    y = sext11((uint32_t)(y + S.offs_y));

    const PgxpWord *t = NULL;
    const TexImage *rep = NULL;
    PgxpWord ttmp;
    float sx = 1.0f, sy = 1.0f;
    int32_t period = 0;
    if (TH.buf && g_sprite_tags) {      /* sprite built at a projected point, or HUD */
        t = cmd_addr[1] != 0xFFFFFFFFu ? &pgxp_mem[(cmd_addr[1] & 0x1FFFFCu) >> 2] : NULL;
        if (t && !(t->w < 0.0f && t->value == p[1]))
            t = NULL;
        if (t && t->cx == MENU_CX) {        /* a menu's or the title's background tile: bg_wide */
            buf_menu[draw_buf()] = anchor_gen;
            hud_after = 1;                  /* what is drawn over it keeps its 4:3 shape */
            period = t->cy;
            t = period > 0 ? NULL : &hud_word;  /* no background to repeat (Natsume logo): like the HUD */
        } else if (t && t->cx == HUD_CX) {
            if (hud_sx() != 1.0f)
                hud_after = 1;
        } else if (!t && hud_after && !(x <= S.clip_x0 && x + w > S.clip_x1))
            t = &hud_word;
        t = interp_tag(t, &ttmp);
        if (t) {
            sx = sprite_sx(t);
            sy = sprite_sy(t);
        }
    }
    /* texture packs: any textured rectangle (field objects, HUD, text, menus), by its texels; not
     * flipped ones (the hardware reads those from another box) */
    if (TH.buf && tex && (g_texpack || g_texdump) && !S.flip_x && !S.flip_y &&
        (rep = texpack_box((int32_t)u, (int32_t)v, w, h)) != NULL) {
        rep_su = (float)rep->w / (float)w;
        rep_sv = (float)rep->h / (float)h;
    }
    if (rep || sx != 1.0f || sy != 1.0f || period > 0) {   /* scaled or replaced: native as usual, then the copy */
        static const PgxpWord untagged = { 0, 0.0f, 0.0f, -1.0f, 0, 0, 0.0f };
        uint16_t *hb = TH.buf;
        TH.buf = NULL;
        draw_sprite(x, y, w, h, u, v, color, tex, tex && raw, abe ? (int)S.abr : -1);
        TH.buf = hb;
        tex_rep = rep;
        if (period > 0)
            bg_wide(x, y, w, h, u, v, color, tex, tex && raw, abe ? (int)S.abr : -1, period);
        else
            draw_sprite_scaled(x, y, w, h, u, v, color, tex, tex && raw, abe ? (int)S.abr : -1, t ? t : &untagged, sx, sy,
                               S.clip_x0 * TH.n, (S.clip_x1 + 1) * TH.n);
        tex_rep = NULL;
        return;
    }
    if (t) {
        sprite_ox = sprite_coord(0, 0, t->x, 1.0f);
        sprite_oy = sprite_coord(0, 0, t->y, 1.0f);
    }
    draw_sprite(x, y, w, h, u, v, color, tex, tex && raw, abe ? (int)S.abr : -1);
    sprite_ox = sprite_oy = 0;
}

/* ======================================================================= */
/* VRAM commands                                                            */
/* ======================================================================= */

static void cmd_fill(const uint32_t *p)
{
    uint32_t c = p[0];
    uint16_t val = (uint16_t)(((c & 0xFFu) >> 3) | ((((c >> 8) & 0xFFu) >> 3) << 5) |
                              ((((c >> 16) & 0xFFu) >> 3) << 10));
    int32_t x0 = (int32_t)(p[1] & 0x3F0u);
    int32_t y0 = (int32_t)((p[1] >> 16) & 0x3FFu);
    int32_t w = (int32_t)(((p[2] & 0x3FFu) + 0xFu) & ~0xFu);
    int32_t h = (int32_t)((p[2] >> 16) & 0x1FFu);
    int32_t x, y;

    for (y = 0; y < h; y++) {
        int32_t dy = (y0 + y) & 511;
        uint16_t *row;
        if (line_skip(dy) || dy < band_y0 || dy >= band_y1)
            continue;
        row = &TN.buf[dy << 10];
        for (x = 0; x < w; x++)
            row[(x0 + x) & 1023] = val;
        if (TH.buf) {
            int32_t n = TH.n, i, j;
            for (j = 0; j < n; j++) {
                uint16_t *hrow = &TH.buf[(uint32_t)(dy * n + j) << TH.sh];
                uint16_t *trow = TH.tag ? &TH.tag[(uint32_t)(dy * n + j) << TH.sh] : NULL;
                for (x = 0; x < w; x++)
                    for (i = 0; i < n; i++) {
                        hrow[((x0 + x) & 1023) * n + i] = val;
                        if (trow)
                            trow[((x0 + x) & 1023) * n + i] = (uint16_t)(TAG_FILL | cur_tag);
                    }
            }
        }
    }
}

static void cmd_copy(const uint32_t *p)
{
    int32_t sx = (int32_t)(p[1] & 0x3FFu);
    int32_t sy = (int32_t)((p[1] >> 16) & 0x3FFu);
    int32_t dx = (int32_t)(p[2] & 0x3FFu);
    int32_t dy = (int32_t)((p[2] >> 16) & 0x3FFu);
    int32_t w = (int32_t)(p[3] & 0x3FFu);
    int32_t h = (int32_t)((p[3] >> 16) & 0x1FFu);
    int32_t x, y, i;
    uint16_t tmp[128];

    if (!w)
        w = 0x400;
    if (!h)
        h = 0x200;

    /* The GPU copies through a small line buffer, in chunks of 128 pixels. */
    for (y = 0; y < h; y++) {
        const uint16_t *srow = &S.vram[((sy + y) & 511) << 10];
        uint16_t *drow = &S.vram[((dy + y) & 511) << 10];
        for (x = 0; x < w; x += 128) {
            int32_t n = w - x < 128 ? w - x : 128;
            for (i = 0; i < n; i++)
                tmp[i] = srow[(sx + x + i) & 1023];
            for (i = 0; i < n; i++) {
                uint16_t *dp = &drow[(dx + x + i) & 1023];
                if (!(*dp & S.mask_check))
                    *dp = (uint16_t)(tmp[i] | S.mask_set);
            }
        }
    }

    /* the upscaled copy: same copy at n x the size, keeping its detail */
    if (TH.buf) {
        int32_t n = TH.n, W = 1024 * n, H = 512 * n, k;
        uint16_t htmp[128 * 4];
        uint16_t ttmp[128 * 4];
        for (y = 0; y < h * n; y++) {
            int32_t ys = sy * n + y, yd = dy * n + y;
            const uint16_t *srow = &TH.buf[(uint32_t)(ys % H) << TH.sh];
            uint16_t *drow = &TH.buf[(uint32_t)(yd % H) << TH.sh];
            const uint16_t *stag = TH.tag ? &TH.tag[(uint32_t)(ys % H) << TH.sh] : NULL;
            uint16_t *dtag = TH.tag ? &TH.tag[(uint32_t)(yd % H) << TH.sh] : NULL;
            for (x = 0; x < w * n; x += 128 * n) {
                k = w * n - x < 128 * n ? w * n - x : 128 * n;
                for (i = 0; i < k; i++) {
                    int32_t xs = sx * n + x + i;
                    htmp[i] = srow[xs >= W ? xs - W : xs];
                    ttmp[i] = stag ? stag[xs >= W ? xs - W : xs] : 0;
                }
                for (i = 0; i < k; i++) {
                    int32_t xd = dx * n + x + i;
                    uint16_t *dp = &drow[xd >= W ? xd - W : xd];
                    if (!(*dp & S.mask_check)) {
                        *dp = (uint16_t)(htmp[i] | S.mask_set);
                        if (dtag)
                            dtag[xd >= W ? xd - W : xd] = ttmp[i];
                    }
                }
            }
        }
    }
}

static void vram_write_word(uint32_t w)
{
    int i;
    for (i = 0; i < 2; i++) {
        uint16_t *dp = &S.vram[((uint32_t)(S.wr_cy & 511) << 10) | (uint32_t)(S.wr_cx & 1023)];
        if (!(*dp & S.mask_check))
            *dp = (uint16_t)((w & 0xFFFFu) | S.mask_set);
        if (TH.buf)
            up_block((uint32_t)S.wr_cx, (uint32_t)S.wr_cy, (uint16_t)((w & 0xFFFFu) | S.mask_set));
        w >>= 16;
        S.wr_cx++;
        if (S.wr_cx == S.wr_x + S.wr_w) {
            S.wr_cx = S.wr_x;
            S.wr_cy++;
            if (S.wr_cy == S.wr_y + S.wr_h) {
                S.gp0_state = GP0_STATE_COMMAND;
                break;
            }
        }
    }
}

/* ======================================================================= */
/* GP0                                                                      */
/* ======================================================================= */

static int gp0_cmd_len(uint32_t op)
{
    switch (op >> 5) {
    case 0:
        return op == 0x02 ? 3 : 1;
    case 1: {
        int nv = (op & 0x08) ? 4 : 3;
        int len = 1 + nv * ((op & 0x04) ? 2 : 1);
        if (op & 0x10)
            len += nv - 1;
        return len;
    }
    case 2:
        return (op & 0x10) ? 4 : 3;
    case 3: {
        int len = 2;
        if (op & 0x04)
            len++;
        if (((op >> 3) & 3u) == 0)
            len++;
        return len;
    }
    case 4:
        return 4;
    case 5:
    case 6:
        return 3;
    default:
        return 1;
    }
}

static void gp0_env(uint32_t w)
{
    switch (w >> 24) {
    case 0xE1:
        set_texpage(w, 1);
        break;
    case 0xE2:
        S.tww = w & 0x1Fu;
        S.twh = (w >> 5) & 0x1Fu;
        S.twx = (w >> 10) & 0x1Fu;
        S.twy = (w >> 15) & 0x1Fu;
        recalc_texwin();
        break;
    case 0xE3:
        S.clip_x0 = (int32_t)(w & 0x3FFu);
        S.clip_y0 = (int32_t)((w >> 10) & 0x3FFu);
        if (S.clip_y0 < band_y0)
            S.clip_y0 = band_y0;
        break;
    case 0xE4:
        S.clip_x1 = (int32_t)(w & 0x3FFu);
        S.clip_y1 = (int32_t)((w >> 10) & 0x3FFu);
        if (S.clip_y1 > band_y1 - 1)
            S.clip_y1 = band_y1 - 1;
        break;
    case 0xE5:
        S.offs_x = sext11(w);
        S.offs_y = sext11(w >> 11);
        break;
    case 0xE6:
        S.mask_set = (w & 1u) ? 0x8000u : 0u;
        S.mask_check = (w & 2u) ? 0x8000u : 0u;
        break;
    default:
        break;
    }
}

int g_interp;
static void frame_record(const uint32_t *p, uint32_t op);

static void gp0_execute(const uint32_t *p)
{
    uint32_t op = p[0] >> 24;
    cur_tag = (uint16_t)(++tag_seq & 0x3FFFu);

    if (op == 0x02 || ((op >> 5) >= 2 && (op >> 5) <= 5))   /* fill, line, rect, copy, transfer */
        BAND_SYNC();

    switch (op >> 5) {
    case 0:
        if (op == 0x01)
            clut_invalidate();
        else if (op == 0x02)
            cmd_fill(p);
        else if (op == 0x1F)
            S.irq = 1;
        break;
    case 1:
        cmd_polygon(p);
        break;
    case 2:
        cmd_line(p);
        break;
    case 3:
        cmd_rect(p);
        break;
    case 4:
        cmd_copy(p);
        break;
    case 5:
        S.wr_x = (int32_t)(p[1] & 0x3FFu);
        S.wr_y = (int32_t)((p[1] >> 16) & 0x3FFu);
        S.wr_w = (int32_t)(p[2] & 0x3FFu);
        S.wr_h = (int32_t)((p[2] >> 16) & 0x1FFu);
        if (!S.wr_w)
            S.wr_w = 0x400;
        if (!S.wr_h)
            S.wr_h = 0x200;
        S.wr_cx = S.wr_x;
        S.wr_cy = S.wr_y;
        S.gp0_state = GP0_STATE_VRAM_WRITE;
        break;
    case 6:
        S.rd_x = (int32_t)(p[1] & 0x3FFu);
        S.rd_y = (int32_t)((p[1] >> 16) & 0x3FFu);
        S.rd_w = (int32_t)(p[2] & 0x3FFu);
        S.rd_h = (int32_t)((p[2] >> 16) & 0x1FFu);
        if (!S.rd_w)
            S.rd_w = 0x400;
        if (!S.rd_h)
            S.rd_h = 0x200;
        S.rd_cx = S.rd_x;
        S.rd_cy = S.rd_y;
        S.rd_active = 1;
        break;
    default:
        gp0_env(p[0]);
        break;
    }
    if (g_interp && !replaying)
        frame_record(p, op);
}

/* Frame records: the drawing commands of the last frame drawn into each buffer (top or bottom of
 * VRAM) with the RAM address and PGXP word of each command word, the GPU state when it began, its
 * camera (world to view: r, t; projection: h, ofx, ofy) and the player's anchor in it. */
#define FR_WORDS (1 << 18)
#define STATE_OFS offsetof(GpuState, cmd)
typedef struct { float r[9], t[3], h, ofx, ofy; uint32_t gen; } Camera;
typedef struct {
    uint32_t gen, last;             /* gens of its first and last command */
    int n, full, player;
    Camera cam;
    float px, py;                   /* the player's anchor on screen */
    int16_t pcx, pcy;
    uint8_t state[sizeof(GpuState) - STATE_OFS];
    uint32_t w[FR_WORDS], a[FR_WORDS];
    PgxpWord pw[FR_WORDS];
} FrameRec;

/* A redraw for the worker thread, which has a GPU of its own (gpu_replay.c compiles this file a
 * second time): copies of the frame's commands and of what drawing them reads (native VRAM for
 * textures, the GPU state at the frame's start), the in-between camera (P' = m P + k), the
 * player's half step, and the target: th, a scratch copy of the upscaled VRAM whose rows y0..y1
 * are first copied from src. */
typedef struct {
    int n;
    uint32_t w[FR_WORDS], a[FR_WORDS];
    PgxpWord pw[FR_WORDS];
    uint8_t state[sizeof(GpuState) - STATE_OFS];
    uint16_t vram[VRAM_W * VRAM_H];
    float m[9], k[3], h, ofx, ofy, ws, pdx, pdy;
    int16_t pcx, pcy;
    int pgxp, persp, depth, scene3d, log;
    Target th;
    const uint16_t *src;
    const uint16_t *src_tag;
    uint32_t y0, y1;
    int32_t band[INTERP_WORKERS + 1];   /* worker k draws VRAM rows band[k] .. band[k + 1] */
} InterpJob;

#ifdef GPU_REPLAY
static int replay_scene3d;
static int replay_scene(void)
{
    return replay_scene3d;
}

void GPU_REPLAY_RUN(const InterpJob *j, int32_t y0, int32_t y1)
{
    clock_t t0 = clock();
    uint32_t y, k;
    int i, len;
    if (!dither_lut_ready)
        build_dither_lut();
    band_y0 = y0;
    band_y1 = y1;
    for (y = j->y0; y < j->y1; y++) {
        if (y < (uint32_t)y0 * (uint32_t)j->th.n || y >= (uint32_t)y1 * (uint32_t)j->th.n)
            continue;
        uint32_t row = (y % (512u * (uint32_t)j->th.n)) << j->th.sh;
        memcpy(&j->th.buf[row], &j->src[row], (size_t)1024 * (size_t)j->th.n * sizeof(uint16_t));
        if (j->th.tag)
            memcpy(&j->th.tag[row], &j->src_tag[row], (size_t)1024 * (size_t)j->th.n * sizeof *j->th.tag);
    }
    clock_t t1 = clock();
    memcpy(S.vram, j->vram, sizeof S.vram);
    memcpy((uint8_t *)&S + STATE_OFS, j->state, sizeof j->state);
    if (S.clip_y0 < band_y0)
        S.clip_y0 = band_y0;
    if (S.clip_y1 > band_y1 - 1)
        S.clip_y1 = band_y1 - 1;
    clut_invalidate();
    TH = j->th;
    g_pgxp = j->pgxp;
    g_pgxp_persp = j->persp;
    g_sprite_depth = j->depth;
    g_sprite_tags = 1;
    g_sprite_ws = j->ws;
    replay_scene3d = j->scene3d;
    g_scene_3d = replay_scene;
    memcpy(ifm, j->m, sizeof ifm);
    memcpy(ifk, j->k, sizeof ifk);
    ih = j->h;
    iofx = j->ofx;
    iofy = j->ofy;
    iws = j->ws;
    ipdx = j->pdx;
    ipdy = j->pdy;
    ipcx = j->pcx;
    ipcy = j->pcy;
    hud_after = 0;
    memset(buf_menu, 0, sizeof buf_menu);
    replaying = 1;
    for (k = 0; k < (uint32_t)j->n; k += (uint32_t)len) {
        len = gp0_cmd_len(j->w[k] >> 24);
        for (i = 0; i < len; i++) {         /* the PGXP words as they were */
            uint32_t a = cmd_addr[i] = j->a[k + i];
            if (a != 0xFFFFFFFFu)
                pgxp_mem[(a & 0x1FFFFCu) >> 2] = j->pw[k + i];
        }
        gp0_execute(&j->w[k]);
    }
    replaying = 0;
    if (j->log)
        fprintf(stderr, "interp: copy %.2f ms ", (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC);
    if (j->log)
        fprintf(stderr, "interp: drawn %.2f ms\n", (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
}
#endif
void gpu_replay_run(const InterpJob *j, int32_t y0, int32_t y1);      /* gpu_replay.c */
void gpu_replay_run2(const InterpJob *j, int32_t y0, int32_t y1);     /* gpu_replay2.c */
void gpu_replay_run3(const InterpJob *j, int32_t y0, int32_t y1);     /* gpu_replay3.c */
#ifdef GPU_VIEW2
/* the second view's GPU redraws its in-between pictures with copies of its own (gpu_view2_replay1.c
 * ...): the game's may be drawing the game's at the same time */
void view2_replay_run1(const InterpJob *j, int32_t y0, int32_t y1);
void view2_replay_run2(const InterpJob *j, int32_t y0, int32_t y1);
void view2_replay_run3(const InterpJob *j, int32_t y0, int32_t y1);
#define gpu_replay_run view2_replay_run1
#define gpu_replay_run2 view2_replay_run2
#define gpu_replay_run3 view2_replay_run3
#endif

static FrameRec *frec;              /* [2] */
static Camera cam_cur;
static InterpJob *job;
static uint16_t *interp_buf;        /* the in-between picture: a scratch copy of the upscaled VRAM */
static uint16_t *interp_tags;       /* and of its tags (Target.tag) */
static uint32_t job_gen;            /* the next frame the in-between picture is made for */
static int since_flip, last_y = -1, disp_y[2] = { -1, -1 }, ilog;

static void frame_record(const uint32_t *p, uint32_t op)
{
    FrameRec *r;
    int len, k;
    if (!frec || !(op == 0x02 || (op >= 0x20 && op < 0x80) || (op >= 0xE1 && op <= 0xE6)))
        return;
    if ((op >> 5) == 2 && (op & 0x08))          /* polylines go on in polyline_word: not recorded */
        return;
    r = &frec[draw_buf()];
    if (anchor_gen - r->last > 1) {             /* a new frame in this buffer */
        r->gen = anchor_gen;
        r->n = r->full = 0;
        memcpy(r->state, (uint8_t *)&S + STATE_OFS, sizeof r->state);
        r->cam = cam_cur;
        if (anchor_gen - cam_cur.gen > 3)
            r->cam.gen = 0;
#ifdef GPU_VIEW2
        /* the second view follows the second farmer: its half step, not the player's */
        r->player = farmer2_gen + 3 >= anchor_gen;
        r->pcx = farmer2_map.cx;
        r->pcy = farmer2_map.cy;
        r->px = (float)farmer2_map.cx + farmer2_map.dx;
        r->py = (float)farmer2_map.cy + farmer2_map.dy;
#else
        r->player = player_gen + 3 >= anchor_gen;
        r->pcx = player_map.cx;
        r->pcy = player_map.cy;
        r->px = (float)player_map.cx + player_map.dx;
        r->py = (float)player_map.cy + player_map.dy;
#endif
    }
    r->last = anchor_gen;
    len = gp0_cmd_len(op);
    if (r->full || r->n + len > FR_WORDS) {
        r->full = 1;
        return;
    }
    for (k = 0; k < len; k++) {
        uint32_t a = cmd_addr[k];
        r->w[r->n + k] = p[k];
        r->a[r->n + k] = a;
        if (a != 0xFFFFFFFFu)
            r->pw[r->n + k] = pgxp_mem[(a & 0x1FFFFCu) >> 2];
    }
    r->n += len;
}

void gpu_set_camera(const int16_t r[9], int32_t tx, int32_t ty, int32_t tz, uint32_t h, int32_t ofx, int32_t ofy)
{
    ROUTE_BUILD(gpu_set_camera(r, tx, ty, tz, h, ofx, ofy));
    int k;
    for (k = 0; k < 9; k++)
        cam_cur.r[k] = (float)r[k] / 4096.0f;
    cam_cur.t[0] = (float)tx;
    cam_cur.t[1] = (float)ty;
    cam_cur.t[2] = (float)tz;
    cam_cur.h = (float)h;
    cam_cur.ofx = (float)ofx / 65536.0f;
    cam_cur.ofy = (float)ofy / 65536.0f;
    cam_cur.gen = anchor_gen;
}

void gpu_interp_enable(void)
{
    if (!TH.buf || frec)
        return;
    frec = calloc(2, sizeof *frec);
    job = malloc(sizeof *job);
    interp_buf = malloc(((size_t)1 << (2 * TH.sh - 1)) * sizeof(uint16_t));
    interp_tags = TH.tag ? malloc(((size_t)1 << (2 * TH.sh - 1)) * sizeof *interp_tags) : NULL;
    g_interp = frec && job && interp_buf && (interp_tags || !TH.tag);
    ilog = getenv("HMBTN_INTERP_LOG") != NULL;
}

/* inv = m^-1 (3x3, row-major); 0 when singular */
static int inv3(const float *m, float *inv)
{
    float d = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (fabsf(d) < 1e-9f)
        return 0;
    inv[0] = (m[4] * m[8] - m[5] * m[7]) / d;
    inv[1] = (m[2] * m[7] - m[1] * m[8]) / d;
    inv[2] = (m[1] * m[5] - m[2] * m[4]) / d;
    inv[3] = (m[5] * m[6] - m[3] * m[8]) / d;
    inv[4] = (m[0] * m[8] - m[2] * m[6]) / d;
    inv[5] = (m[2] * m[3] - m[0] * m[5]) / d;
    inv[6] = (m[3] * m[7] - m[4] * m[6]) / d;
    inv[7] = (m[1] * m[6] - m[0] * m[7]) / d;
    inv[8] = (m[0] * m[4] - m[1] * m[3]) / d;
    return 1;
}

/* Every shown picture: notes buffer flips, and on the second VBlank of a frame (also on a third
 * and fourth when the game slowed down, rather than stepping back) shows the in-between picture
 * made for it. 0: none, show gpu_render_display. */
int gpu_render_interp(uint32_t *out, int *w, int *h)
{
    GpuDisplay d;
    uint32_t disp_saved;
    uint16_t *hb, *ht;
    int b;

    BAND_SYNC();
    if (!frec)
        return 0;
    gpu_get_display(&d);
    since_flip = d.vram_y == last_y ? since_flip + 1 : 1;
    last_y = d.vram_y;
    b = d.vram_y >= 240;
    disp_y[b] = d.vram_y;
    if (!g_interp || since_flip < 2 || since_flip > 4 || !job_gen || job_gen != frec[!b].gen || !d.enabled ||
        d.is_24bit || disp_y[!b] < 0)
        return 0;
    if (ilog && since_flip > 2)
        fprintf(stderr, "interp: held\n");
    hb = TH.buf;
    ht = TH.tag;
    TH.buf = interp_buf;
    TH.tag = interp_tags;
    disp_saved = S.disp_y;              /* the next frame's buffer, from the in-between picture */
    S.disp_y = (uint32_t)disp_y[!b];
    gpu_render_display(out, w, h);
    S.disp_y = disp_saved;
    TH.buf = hb;
    TH.tag = ht;
    return 1;
}

/* After the first VBlank of a frame is shown: the next frame is drawn already. Sets up its redraw
 * with the camera halfway back to this frame's, for gpu_interp_work (the worker thread) to do
 * while the game goes on. 0: none (a menu, a camera cut, a slowdown...). */
int gpu_interp_prepare(void)
{
    const FrameRec *cur, *prev;
    GpuDisplay d;
    float rm[9], ri[9], tm[3], diff = 0.0f, dt;
    int b, i, j, k;

    BAND_SYNC();

    if (!g_interp || !frec || !TH.buf || since_flip != 1)
        return 0;
    job_gen = 0;
    gpu_get_display(&d);
    b = d.vram_y >= 240;
    cur = &frec[!b];                    /* the next frame */
    prev = &frec[b];                    /* the frame on screen */
    if (ilog)
        fprintf(stderr, "interp: y %d gen %u/%u n %d/%d cam %u/%u t %.0f %.0f %.0f p %.1f %.1f\n", d.vram_y,
                cur->gen, prev->gen, cur->n, prev->n, cur->cam.gen, prev->cam.gen, cur->cam.t[0], cur->cam.t[1],
                cur->cam.t[2], cur->px, cur->py);
    if (!d.enabled || d.is_24bit || disp_y[!b] < 0 || gpu_display_menu() || (buf_menu[!b] && buf_menu[!b] == buf_drawn[!b]))
        return 0;
    if (!cur->n || cur->full || !cur->cam.gen || !prev->cam.gen || cur->gen - prev->gen != 2)
        return 0;
    for (k = 0; k < 9; k++)
        diff += fabsf(cur->cam.r[k] - prev->cam.r[k]);
    dt = fabsf(cur->cam.t[0] - prev->cam.t[0]) + fabsf(cur->cam.t[1] - prev->cam.t[1]) + fabsf(cur->cam.t[2] - prev->cam.t[2]);
    if (diff > 0.1f || dt > 600.0f || cur->cam.h != prev->cam.h || !inv3(cur->cam.r, ri))     /* a camera cut */
        return 0;

    /* P' = Rm Rc^-1 (P - Tc) + Tm, with Rm, Tm halfway between the two cameras */
    for (k = 0; k < 9; k++)
        rm[k] = 0.5f * (cur->cam.r[k] + prev->cam.r[k]);
    for (k = 0; k < 3; k++)
        tm[k] = 0.5f * (cur->cam.t[k] + prev->cam.t[k]);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            job->m[i * 3 + j] = rm[i * 3] * ri[j] + rm[i * 3 + 1] * ri[3 + j] + rm[i * 3 + 2] * ri[6 + j];
    for (i = 0; i < 3; i++)
        job->k[i] = tm[i] - (job->m[i * 3] * cur->cam.t[0] + job->m[i * 3 + 1] * cur->cam.t[1] + job->m[i * 3 + 2] * cur->cam.t[2]);
    job->h = cur->cam.h;
    job->ofx = cur->cam.ofx;
    job->ofy = cur->cam.ofy;
    job->ws = g_sprite_ws;
    job->pcx = INT16_MIN + 2;
    /* the player's own half step; not when "the player" jumped (farmer 2 took the player's place
     * for its menu, and back: its sprite was drawn halfway to the player's) */
    if (cur->player && prev->player && fabsf(prev->px - cur->px) + fabsf(prev->py - cur->py) < 24.0f) {
        job->pcx = cur->pcx;
        job->pcy = cur->pcy;
        job->pdx = 0.5f * (prev->px - cur->px);
        job->pdy = 0.5f * (prev->py - cur->py);
    }
    job->n = cur->n;
    memcpy(job->w, cur->w, (size_t)cur->n * sizeof *job->w);
    memcpy(job->a, cur->a, (size_t)cur->n * sizeof *job->a);
    memcpy(job->pw, cur->pw, (size_t)cur->n * sizeof *job->pw);
    memcpy(job->state, cur->state, sizeof job->state);
    memcpy(job->vram, S.vram, sizeof job->vram);
    job->pgxp = g_pgxp;
    job->persp = g_pgxp_persp;
    job->depth = g_sprite_depth;
    job->scene3d = !g_scene_3d || g_scene_3d();
    job->log = ilog;
    job->th = TH;
    job->th.buf = interp_buf;
    job->th.tag = interp_tags;
    job->src = TH.buf;
    job->src_tag = TH.tag;
    job->y0 = (uint32_t)disp_y[!b] * (uint32_t)TH.n;
    job->y1 = (uint32_t)(disp_y[!b] + d.height) * (uint32_t)TH.n;
    for (k = 1; k < INTERP_WORKERS; k++)
        job->band[k] = disp_y[!b] + d.height * k / INTERP_WORKERS;
    job->band[0] = 0;
    job->band[INTERP_WORKERS] = 1024;
    job_gen = cur->gen;
    return 1;
}

void gpu_interp_work(int part)
{
    static void (*const run[INTERP_WORKERS])(const InterpJob *, int32_t, int32_t) = {
        gpu_replay_run, gpu_replay_run2, gpu_replay_run3
    };
    run[part](job, job->band[part], job->band[part + 1]);
}

void gpu_interp_late(void)
{
    job_gen = 0;
}

/* counts everything that can change the picture shown (gpu_changes) */
static uint32_t changes;

uint32_t gpu_changes(void)
{
    return changes;
}

void gpu_write_gp0(uint32_t w)
{
#ifdef GPU_ROUTED
    if (g_gpu_route) {
        view2_g_gp0_addr = g_gp0_addr;
        view2_gpu_write_gp0(w);
        if (g_gpu_route == 2)
            return;
    }
#endif
    changes++;
    switch (S.gp0_state) {
    case GP0_STATE_VRAM_WRITE:
        vram_write_word(w);
        return;
    case GP0_STATE_POLYLINE:
        polyline_word(w);
        return;
    default:
        break;
    }

    if (S.cmd_count == 0)
        S.cmd_need = gp0_cmd_len(w >> 24);
    cmd_addr[S.cmd_count] = g_gp0_addr;
    S.cmd[S.cmd_count++] = w;
    if (S.cmd_count < S.cmd_need)
        return;
    S.cmd_count = 0;
    gp0_execute(S.cmd);
}

void gpu_dma_write(const uint32_t *src, int words)
{
    int i;
    for (i = 0; i < words; i++)
        gpu_write_gp0(src[i]);
}

uint32_t gpu_read_data(void)
{
#ifdef GPU_ROUTED
    if (g_gpu_route == 2)
        return view2_gpu_read_data();
    if (g_gpu_route)
        view2_gpu_read_data();
#endif
    if (S.rd_active) {
        uint32_t v = 0;
        int i;
        for (i = 0; i < 2; i++) {
            v |= (uint32_t)S.vram[((uint32_t)(S.rd_cy & 511) << 10) | (uint32_t)(S.rd_cx & 1023)]
                 << (16 * i);
            S.rd_cx++;
            if (S.rd_cx == S.rd_x + S.rd_w) {
                if (S.rd_cy + 1 == S.rd_y + S.rd_h) {
                    S.rd_active = 0;
                } else {
                    S.rd_cy++;
                    S.rd_cx = S.rd_x;
                }
            }
        }
        S.read_latch = v;
    }
    return S.read_latch;
}

void gpu_dma_read(uint32_t *dst, int words)
{
    int i;
    for (i = 0; i < words; i++)
        dst[i] = gpu_read_data();
}

/* ======================================================================= */
/* GP1                                                                      */
/* ======================================================================= */

static void reset_command_buffer(void)
{
    S.gp0_state = GP0_STATE_COMMAND;
    S.cmd_count = 0;
    S.cmd_need = 0;
    S.pl_count = 0;
    S.rd_active = 0;
    clut_invalidate();
}

static void soft_reset(void)
{
    reset_command_buffer();
    S.irq = 0;
    S.display_off = 1;
    S.dma_dir = 0;
    S.disp_x = 0;
    S.disp_y = 0;
    S.h_start = 0x200;
    S.h_end = 0x200 + 256 * 10;
    S.v_start = 0x10;
    S.v_end = 0x10 + 240;
    S.disp_mode = 0;
    S.field = 0;

    S.tp_x = S.tp_y = 0;
    S.abr = S.tex_mode = 0;
    S.dither = S.dfe = 0;
    S.tex_disable = 0;
    S.flip_x = S.flip_y = 0;
    S.tww = S.twh = S.twx = S.twy = 0;
    recalc_texwin();
    S.clip_x0 = S.clip_y0 = S.clip_x1 = S.clip_y1 = 0;
    S.offs_x = S.offs_y = 0;
    S.mask_set = S.mask_check = 0;
}

void gpu_write_gp1(uint32_t v)
{
    ROUTE(gpu_write_gp1(v));
    changes++;
    uint32_t cmd = (v >> 24) & 0x3Fu;
    if (cmd <= 0x01)            /* resets */
        BAND_SYNC();

    switch (cmd) {
    case 0x00:
        soft_reset();
        break;
    case 0x01:
        reset_command_buffer();
        break;
    case 0x02:
        S.irq = 0;
        break;
    case 0x03:
        S.display_off = v & 1u;
        break;
    case 0x04:
        S.dma_dir = v & 3u;
        break;
    case 0x05:
        S.disp_x = v & 0x3FEu;  /* bit 0 is ignored by the hardware */
        S.disp_y = (v >> 10) & 0x1FFu;
        break;
    case 0x06:
        S.h_start = v & 0xFFFu;
        S.h_end = (v >> 12) & 0xFFFu;
        break;
    case 0x07:
        S.v_start = v & 0x3FFu;
        S.v_end = (v >> 10) & 0x3FFu;
        break;
    case 0x08:
        S.disp_mode = v & 0xFFu;
        if (!(S.disp_mode & 0x20u))
            S.field = 0;
        break;
    case 0x09:
        S.tex_disable_allow = v & 1u;
        break;
    default:
        if (cmd >= 0x10 && cmd <= 0x1F) {
            switch (v & 0xFu) {
            case 0x2:
                S.read_latch = S.tww | (S.twh << 5) | (S.twx << 10) | (S.twy << 15);
                break;
            case 0x3:
                S.read_latch = (uint32_t)S.clip_x0 | ((uint32_t)S.clip_y0 << 10);
                break;
            case 0x4:
                S.read_latch = (uint32_t)S.clip_x1 | ((uint32_t)S.clip_y1 << 10);
                break;
            case 0x5:
                S.read_latch = ((uint32_t)S.offs_x & 0x7FFu) | (((uint32_t)S.offs_y & 0x7FFu) << 11);
                break;
            case 0x7:
                S.read_latch = 2;
                break;
            case 0x8:
                S.read_latch = 0;
                break;
            default:
                break;  /* old latch value remains */
            }
        }
        break;
    }
}

uint32_t gpu_read_status(void)
{
    uint32_t s = 0;
#ifdef GPU_ROUTED
    if (g_gpu_route == 2)
        return view2_gpu_read_status();
#endif
    int interlaced = (S.disp_mode & 0x20u) != 0;

    s |= (S.tp_x / 64u) & 0xFu;
    s |= (S.tp_y >= 256u) ? (1u << 4) : 0u;
    s |= S.abr << 5;
    s |= S.tex_mode << 7;
    s |= S.dither << 9;
    s |= S.dfe << 10;
    if (S.mask_set)
        s |= 1u << 11;
    if (S.mask_check)
        s |= 1u << 12;
    if (!interlaced || !S.field)
        s |= 1u << 13;
    /* bit 14 ("reverse") always 0 */
    s |= S.tex_disable << 15;
    s |= ((S.disp_mode >> 6) & 1u) << 16;
    s |= (S.disp_mode & 3u) << 17;
    s |= (S.disp_mode & 0x3Cu) << 17;     /* bits 2..5 -> 19..22 */
    s |= S.display_off << 23;
    s |= S.irq << 24;
    switch (S.dma_dir) {
    case 1:
    case 2:
        s |= 1u << 25;
        break;
    case 3:
        if (S.rd_active)
            s |= 1u << 25;
        break;
    default:
        break;
    }
    s |= 1u << 26;
    if (S.rd_active)
        s |= 1u << 27;
    s |= 1u << 28;
    s |= S.dma_dir << 29;
    if (interlaced ? S.field : S.line_parity)
        s |= 1u << 31;
    return s;
}

/* ======================================================================= */
/* Timing                                                                   */
/* ======================================================================= */

void gpu_vblank(void)
{
    ROUTE(gpu_vblank());
    hud_after = 0;
    anchor_gen++;
    if (S.disp_mode & 0x20u)
        S.field ^= 1u;
    else
        S.field = 0;
}

void gpu_set_line_parity(int odd)
{
    ROUTE(gpu_set_line_parity(odd));
    S.line_parity = odd ? 1u : 0u;
}

/* ======================================================================= */
/* Display                                                                  */
/* ======================================================================= */

void gpu_get_display(GpuDisplay *d)
{
    int div, nominal_w, w, h;
    int pal = (S.disp_mode >> 3) & 1;
    int interlaced = (S.disp_mode >> 5) & 1;
    int is480 = (S.disp_mode & 0x24u) == 0x24u;

    if (S.disp_mode & 0x40u) {
        div = 7;
        nominal_w = 368;
    } else {
        switch (S.disp_mode & 3u) {
        case 0: div = 10; nominal_w = 256; break;
        case 1: div = 8; nominal_w = 320; break;
        case 2: div = 5; nominal_w = 512; break;
        default: div = 4; nominal_w = 640; break;
        }
    }

    w = 0;
    if (S.h_end > S.h_start)
        w = ((int)(S.h_end - S.h_start) / div + 2) & ~3;
    if (w <= 0)
        w = nominal_w;
    if (w > VRAM_W)
        w = VRAM_W;

    if (S.v_end > S.v_start)
        h = (int)(S.v_end - S.v_start);
    else
        h = pal ? 288 : 240;
    if (is480)
        h *= 2;
    if (h > VRAM_H)
        h = VRAM_H;

    d->enabled = !S.display_off;
    d->vram_x = (int)S.disp_x;
    d->vram_y = (int)S.disp_y;
    d->width = w;
    d->height = h;
    d->is_24bit = (S.disp_mode >> 4) & 1;
    d->interlaced = interlaced;
    d->pal = pal;
    d->hres_mode = (int)(((S.disp_mode >> 6) & 1u) | ((S.disp_mode & 3u) << 1));
}

/* 15-bit VRAM colour -> 0xFFRRGGBB (5 bits widened to 8: v << 3 | v >> 2) */
static uint32_t rgb_lut[32768];

static void build_rgb_lut(void)
{
    uint32_t p;
    for (p = 0; p < 32768; p++) {
        uint32_t r = p & 31u, g = (p >> 5) & 31u, b = (p >> 10) & 31u;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        rgb_lut[p] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

static void convert_row(uint32_t *o, const uint16_t *row, int n)
{
    int x;
    for (x = 0; x < n; x++)
        o[x] = rgb_lut[row[x] & 0x7FFFu];
}


/* Seams of the picture at x0, y0 (w x h upscaled pixels): by the band workers if there are any */
static uint16_t *seam_snap;         /* the size of TH.buf */

static void seam_pass(int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    if (!seam_snap && !(seam_snap = malloc(((size_t)1 << (2 * TH.sh - 1)) * sizeof *seam_snap)))
        return;
#if !defined(GPU_REPLAY) && !defined(GPU_BAND)
    if (atomic_load(&nbands)) {
        band_push_seam(1, TH.buf, TH.tag, seam_snap, x0, y0, w, h);
        band_sync();
        band_push_seam(2, TH.buf, TH.tag, seam_snap, x0, y0, w, h);
        band_sync();
        return;
    }
#endif
    seam_across(TH.buf, TH.tag, seam_snap, x0, w, y0, y0 + h);
    seam_down(TH.buf, TH.tag, seam_snap, x0, w, y0, y0 + h);
}

void gpu_render_display(uint32_t *out, int *w, int *h)
{
    GpuDisplay d;
    int x, y, n;

    BAND_SYNC();
    gpu_get_display(&d);
    n = (TH.buf && !d.is_24bit) ? TH.n : 1;    /* 24-bit (video) is shown at native size */
    *w = d.width * n;
    *h = d.height * n;

    if (!d.enabled) {
        for (y = 0; y < *w * *h; y++)
            out[y] = 0xFF000000u;
        return;
    }

    if (!rgb_lut[0])
        build_rgb_lut();
    if (n > 1) {
        int W = 1024 * n, x0 = d.vram_x * n, n1 = *w < W - x0 ? *w : W - x0;   /* n1: before the wrap */
        if (g_pgxp && TH.tag && n1 == *w)
            seam_pass(x0, d.vram_y * n, *w, *h);
        for (y = 0; y < *h; y++) {
            const uint16_t *row = &TH.buf[(uint32_t)((d.vram_y * n + y) % (512 * n)) << TH.sh];
            uint32_t *o = out + (size_t)y * (size_t)*w;
            convert_row(o, row + x0, n1);
            convert_row(o + n1, row, *w - n1);
        }
        return;
    }

    for (y = 0; y < d.height; y++) {
        const uint16_t *row = &S.vram[(uint32_t)((d.vram_y + y) & 511) << 10];
        uint32_t *o = out + (size_t)y * (size_t)d.width;
        if (d.is_24bit) {
            uint32_t bx = (uint32_t)d.vram_x * 2u;
            for (x = 0; x < d.width; x++, bx += 3) {
                uint32_t c[3];
                int k;
                for (k = 0; k < 3; k++) {
                    uint32_t ba = (bx + (uint32_t)k) & 2047u;
                    c[k] = (row[ba >> 1] >> ((ba & 1u) * 8u)) & 0xFFu;
                }
                o[x] = 0xFF000000u | (c[0] << 16) | (c[1] << 8) | c[2];
            }
        } else {
            for (x = 0; x < d.width; x++)
                o[x] = rgb_lut[row[(d.vram_x + x) & 1023] & 0x7FFFu];
        }
    }
}

const uint16_t *gpu_vram(void)
{
    return S.vram;
}

/* ======================================================================= */
/* Init / state                                                             */
/* ======================================================================= */

void gpu_init(void)
{
    BAND_SYNC();
    changes++;
    if (!dither_lut_ready)
        build_dither_lut();
    memset(&S, 0, sizeof(S));
    S.tex_disable_allow = 0;
    S.read_latch = 0;
    S.line_parity = 0;
    soft_reset();
    if (TH.buf)
        up_refresh();
}

void gpu_set_upscale(int n)
{
    uint32_t sh, x;
    BAND_SYNC();
#if !defined(GPU_REPLAY) && !defined(GPU_BAND)
    atomic_store(&nbands, 0);   /* gpu_set_threads again for the new size */
    th_gen++;
#endif
    changes++;
    free(TH.buf);
    free(TH.tag);
    free(seam_snap);
    TH.buf = NULL;
    TH.tag = NULL;
    seam_snap = NULL;
    if (n < 2)
        return;
    if (n > 4)
        n = 4;
    sh = n == 2 ? 11u : 12u;   /* power-of-two row stride: 2048 or 4096 halfwords */
    TH.buf = (uint16_t *)calloc((size_t)1 << (2 * sh - 1), sizeof(uint16_t));
    TH.tag = g_pgxp_cracks ? (uint16_t *)calloc((size_t)1 << (2 * sh - 1), sizeof(uint16_t)) : NULL;
    if (!TH.buf || (g_pgxp_cracks && !TH.tag)) {
        free(TH.buf);
        free(TH.tag);
        TH.buf = NULL;
        TH.tag = NULL;
        return;
    }
    TH.sh = sh;
    TH.xm = (1u << sh) - 1u;
    TH.ym = (1u << (sh - 1u)) - 1u;
    TH.n = n;
    for (x = 0; x < 4096; x++)
        TH.dcol[x] = (uint8_t)((x / (uint32_t)n) & 3u);
    up_refresh();
}

int gpu_upscale(void)
{
    return TH.buf ? TH.n : 1;
}

size_t gpu_state_size(void)
{
    return sizeof(S);
}

void gpu_save_state(void *dst)
{
    BAND_SYNC();
    memcpy(dst, &S, sizeof(S));
}

void gpu_load_state(const void *src)
{
    BAND_SYNC();
    changes++;
    if (!dither_lut_ready)
        build_dither_lut();
    memcpy(&S, src, sizeof(S));
    if (TH.buf)
        up_refresh();
}
