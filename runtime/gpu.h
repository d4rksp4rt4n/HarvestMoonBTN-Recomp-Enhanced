/* PS1 GPU: GP0/GP1 command processor with an exact software rasterizer over 1MB VRAM.
 * All commands complete instantly (the GPU is always "ready"). */
#pragma once
#include <stdint.h>
#include <stddef.h>

void     gpu_init(void);
void     gpu_write_gp0(uint32_t v);       /* write to 1F801810h */
void     gpu_write_gp1(uint32_t v);       /* write to 1F801814h */
uint32_t gpu_read_data(void);             /* read  from 1F801810h (GPUREAD) */
uint32_t gpu_read_status(void);           /* read  from 1F801814h (GPUSTAT) */

/* DMA channel 2 helpers (called by the DMA controller) */
void     gpu_dma_write(const uint32_t *src, int words);   /* block mode CPU->GPU (GP0) */
void     gpu_dma_read(uint32_t *dst, int words);          /* block mode GPU->CPU (GPUREAD) */

/* Timing hooks from the scheduler */
void     gpu_vblank(void);                /* called at start of vblank (toggles interlace field) */
void     gpu_set_line_parity(int odd);    /* GPUSTAT.31 for non-interlaced modes */

/* Display output */
typedef struct {
    int enabled;          /* display enabled (GP1(03h)) */
    int vram_x, vram_y;   /* display area start in VRAM (GP1(05h)) */
    int width, height;    /* visible resolution in pixels (from horizontal/vertical range + mode) */
    int is_24bit;
    int interlaced;
    int pal;
    int hres_mode;        /* raw GPUSTAT bits 16-18 */
} GpuDisplay;
void     gpu_get_display(GpuDisplay *d);
/* Convert the visible display area to 0xAARRGGBB pixels; out must hold 1024*512*n*n
 * entries (n = gpu_upscale()). Returns width/height actually written (row stride = *w). */
void     gpu_render_display(uint32_t *out, int *w, int *h);

/* Internal resolution: n = 2..4 also draws into an n-times larger copy of VRAM that
 * gpu_render_display shows; 1 = off (default). The native VRAM is unaffected either way. */
void     gpu_set_upscale(int n);
int      gpu_upscale(void);
/* Band workers: n threads draw the upscaled polygons, each a share of the rows (the same
 * picture as drawn by the GPU alone); 0 = none. Call after gpu_set_upscale. */
void     gpu_set_threads(int n);
int      gpu_threads(void);

/* Frame interpolation (prototype; needs upscaling and PGXP). gpu_set_camera gives the camera of
 * the frame being built (the GTE matrix at the player's projection). For every shown picture the
 * frontend calls gpu_render_interp (0: show gpu_render_display), then gpu_interp_prepare: 1 = run
 * gpu_interp_work(0 .. INTERP_WORKERS - 1) (a band of the picture each, on worker threads) before
 * the next gpu_render_interp, or call gpu_interp_late (the workers aren't done: that picture is
 * not shown). */
#define INTERP_WORKERS 3
extern int g_interp;
void     gpu_interp_enable(void);
void     gpu_set_camera(const int16_t r[9], int32_t tx, int32_t ty, int32_t tz, uint32_t h, int32_t ofx, int32_t ofy);
int      gpu_render_interp(uint32_t *out, int *w, int *h);
int      gpu_interp_prepare(void);
void     gpu_interp_work(int part);
void     gpu_interp_late(void);

/* PGXP (upscaled display only): sub-pixel positions of GTE-projected vertices. The recompiled
 * SWC2 of a screen XY register tags that RAM word with gpu_pgxp_store(); the GPU DMA sets
 * g_gp0_addr to the RAM address of each word it sends, and a polygon vertex whose word still
 * holds the tagged value is drawn at the precise position (and depth, for g_pgxp_persp). */
extern int      g_pgxp;           /* precise vertices (needs upscaling) */
extern int      g_pgxp_persp;     /* perspective-correct textures (needs g_pgxp) */
extern int      g_pgxp_cracks;    /* close the cracks between models (needs g_pgxp; set before gpu_set_upscale) */
extern uint32_t g_gp0_addr;       /* RAM address of the GP0 word being written, ~0 = none */
void     gpu_pgxp_store(uint32_t addr, uint32_t value, int ok, float x, float y, float w);

/* PGXP for 2D sprites placed at projected points: gpu_sprite_anchor records the sub-pixel
 * offset (precise - whole) of a projected anchor; gpu_sprite_begin(x, y) selects the offset of
 * that anchor (g_sprite_on = found) for the following gpu_sprite_tag calls, which tag RAM words
 * of the sprite being built (or drop an old sprite tag when no anchor was found). A sprite
 * rectangle whose position word carries a matching tag (or a quad whose corners all do) is
 * drawn shifted by the offset in the upscaled copy (rigidly, so the pixel art stays crisp).
 * With g_sprite_depth it is also scaled by the anchor's depth scale s about the anchor, and with
 * g_sprite_ws != 1 (widescreen) squeezed horizontally about it, like the GTE squeezes the world. */
extern int g_sprite_tags;         /* anchors and sprite tags are recorded (pgxp or widescreen; upscaled) */
extern int g_sprite_on;
extern int g_sprite_depth;        /* scale tagged sprites with depth (needs g_pgxp) */
extern float g_sprite_ws;         /* horizontal squeeze of tagged sprites, 1 = none */
/* s: depth scale for the anchor's own ground level, z: its depth; player: 1 = the anchor is the
 * player's, 2 = the player's at the projection centre (the camera follows it: its level is the
 * reference for every depth scale), 3 = the second farmer's (p2.c; only for what it holds) */
void     gpu_sprite_anchor(int x, int y, float dx, float dy, float s, float z, int player);
void     gpu_sprite_begin(int x, int y);
void     gpu_sprite_alias(int ax, int ay, int bx, int by);   /* point b uses anchor a (offset, scale, centre) */
void     gpu_held_begin(int on);  /* tag what the player holds with the player's anchor (on = 0: drop; 2: the second farmer's) */
extern int g_held_call;           /* the player's held object is being drawn */
extern int g_item_call;           /* an item on (or thrown to) the ground is being drawn */
/* Widescreen: g_menu_tile is set while a full-screen menu's background is tiled (or the title
 * screen's drawn) to the pattern's period across in pixels; its tiles are tagged
 * (gpu_menu_begin) and repeated to fill the wide picture, and gpu_display_menu says the picture
 * shown is such a screen. -1: a sprite of a screen with no background to repeat (the Natsume
 * logo over a plain screen clear), which keeps its 4:3 shape on the wide screen; -2: the same, but
 * the sides beside the quads at its left and right edges are white (the map, in its clouds). */
extern int g_menu_tile;
void     gpu_menu_begin(int period);
int      gpu_display_menu(void);
void     gpu_sprite_tag(uint32_t addr, uint32_t value);
/* Widescreen HUD: the following gpu_sprite_tag calls (on = 1) tag the HUD, which is squeezed by
 * g_sprite_ws about the middle of the drawing area while g_scene_3d says the picture is a 3D
 * scene (shown wide; NULL = always), or (on = 0) drop old tags. */
void     gpu_hud_begin(int on);
extern int (*g_scene_3d)(void);
extern int g_hud_call;            /* the HUD icon routine is calling the sprite builder */

/* Texture packs (texpack.c; upscaled display only): images drawn in the upscaled copy instead of
 * the texels of 2D sprites (quads built at a projected point, every textured rectangle), found by a
 * key of the texels (texpack_key: the size and the 16-bit texels of the sprite's UV box).
 * texpack_load reads every <key>.png in a folder (g_texpack:
 * any there); texpack_find is safe on any thread once loaded. With g_texdump (texpack_dump_to), the
 * GPU writes each texture it could replace to <folder>/<key>.png (texpack_dump). */
typedef struct { uint64_t key; int w, h; uint16_t *px; } TexImage;   /* px: as texels, 0 = transparent */
extern int g_texpack, g_texdump;
int      texpack_load(const char *dir);
void     texpack_dump_to(const char *dir);
uint64_t texpack_key(int w, int h, const uint16_t *px);
const TexImage *texpack_find(uint64_t key);
void     texpack_dump(uint64_t key, int w, int h, const uint16_t *px);

const uint16_t *gpu_vram(void);           /* 1024x512 16-bit VRAM (debug / screenshots) */
uint32_t gpu_changes(void);       /* changes when anything that can alter the picture shown does */

/* The second view (--p2, p2view.c): a second GPU (gpu_view2.c, the names above with view2_ before
 * them) shown in a second window. g_gpu_route: 0 = what the GPU is sent goes to the GPU only, 1 = to
 * both, 2 = to the second only. */
extern int g_gpu_route;
extern int (*g_view2_built)(void);   /* the second GPU will draw lists the second view's run built */
extern uint32_t view2_g_gp0_addr;
extern int view2_g_pgxp, view2_g_pgxp_persp, view2_g_pgxp_cracks, view2_g_sprite_tags, view2_g_sprite_depth;
extern float view2_g_sprite_ws;
extern int (*view2_g_scene_3d)(void);
void     view2_gpu_init(void);
void     view2_gpu_set_upscale(int n);
int      view2_gpu_upscale(void);
void     view2_gpu_set_threads(int n);
extern int view2_g_interp;           /* its own in-between pictures (gpu_interp_*), on as the game's */
void     view2_gpu_interp_enable(void);
int      view2_gpu_render_interp(uint32_t *out, int *w, int *h);
int      view2_gpu_interp_prepare(void);
void     view2_gpu_interp_work(int part);
void     view2_gpu_interp_late(void);
void     view2_gpu_write_gp0(uint32_t v);
void     view2_gpu_write_gp1(uint32_t v);
const uint16_t *view2_gpu_vram(void);
uint32_t view2_gpu_read_data(void);
uint32_t view2_gpu_read_status(void);
void     view2_gpu_vblank(void);
void     view2_gpu_set_line_parity(int odd);
void     view2_gpu_get_display(GpuDisplay *d);
void     view2_gpu_render_display(uint32_t *out, int *w, int *h);
uint32_t view2_gpu_changes(void);
int      view2_gpu_display_menu(void);
void     view2_gpu_pgxp_store(uint32_t addr, uint32_t value, int ok, float x, float y, float w);
void     view2_gpu_sprite_anchor(int x, int y, float dx, float dy, float s, float z, int player);
void     view2_gpu_sprite_begin(int x, int y);
void     view2_gpu_held_begin(int on);
void     view2_gpu_menu_begin(int period);
void     view2_gpu_hud_begin(int on);
void     view2_gpu_sprite_alias(int ax, int ay, int bx, int by);
void     view2_gpu_sprite_tag(uint32_t addr, uint32_t value);
void     view2_gpu_set_camera(const int16_t r[9], int32_t tx, int32_t ty, int32_t tz, uint32_t h, int32_t ofx, int32_t ofy);

size_t   gpu_state_size(void);
void     gpu_save_state(void *dst);
void     gpu_load_state(const void *src);
