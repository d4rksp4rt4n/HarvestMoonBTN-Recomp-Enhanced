/* gpu_test.c - sanity tests for runtime/gpu.c
 *
 * Build (from the repo root):
 *   clang -std=c11 -O2 -Wall -Wextra -Iruntime runtime/tests/gpu_test.c runtime/gpu.c -o gpu_test.exe
 * Run:
 *   gpu_test.exe [vram.bmp] [display.bmp]
 */
#include "../gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) {                                                   \
            g_pass++;                                                 \
        } else {                                                      \
            g_fail++;                                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

static void gp0(uint32_t w) { gpu_write_gp0(w); }
static void gp1(uint32_t w) { gpu_write_gp1(w); }
static uint16_t px(int x, int y) { return gpu_vram()[(y & 511) * 1024 + (x & 1023)]; }
static uint32_t xy(int x, int y) { return ((uint32_t)(y & 0xFFFF) << 16) | (uint32_t)(x & 0xFFFF); }
static uint16_t rgb15(int r, int g, int b) { return (uint16_t)(r | (g << 5) | (b << 10)); }

static void env_default(void)
{
    gp0(0xE1000000);
    gp0(0xE2000000);
    gp0(0xE3000000);
    gp0(0xE4000000 | (511u << 10) | 1023u);
    gp0(0xE5000000);
    gp0(0xE6000000);
}

static void fill(int x, int y, int w, int h, uint32_t rgb24)
{
    gp0(0x02000000 | (rgb24 & 0xFFFFFF));
    gp0(xy(x, y));
    gp0(xy(w, h));
}

static void clear_vram(void)
{
    fill(0, 0, 0x3FF, 256, 0); /* 3FFh rounds up to 400h */
    fill(0, 256, 0x3FF, 256, 0);
}

static void upload(int x, int y, int w, int h, const uint16_t *data)
{
    int n = w * h, i;
    gp0(0xA0000000);
    gp0(xy(x, y));
    gp0(xy(w, h));
    for (i = 0; i < n; i += 2) {
        uint32_t lo = data[i];
        uint32_t hi = (i + 1 < n) ? data[i + 1] : 0;
        gp0(lo | (hi << 16));
    }
}

/* ---------------------------------------------------------------------- */
/* reference coverage: pixel (x,y) is inside when ceil(L(y)) <= x < ceil(R(y)) */

typedef struct { int x, y; } P2;

static long long ceil_div(long long a, long long b)
{
    long long q = a / b;
    if ((a % b) != 0 && a > 0)
        q++;
    return q;
}

static int ref_cover(P2 a, P2 b, P2 c, int x, int y)
{
    P2 e[3][2];
    long long xs[3];
    int n = 0, i;
    e[0][0] = a; e[0][1] = b;
    e[1][0] = b; e[1][1] = c;
    e[2][0] = c; e[2][1] = a;
    for (i = 0; i < 3; i++) {
        P2 lo = e[i][0], hi = e[i][1];
        if (lo.y > hi.y) { P2 t = lo; lo = hi; hi = t; }
        if (lo.y == hi.y || y < lo.y || y >= hi.y)
            continue;
        xs[n++] = ceil_div((long long)lo.x * (hi.y - lo.y) + (long long)(y - lo.y) * (hi.x - lo.x),
                           hi.y - lo.y);
    }
    if (n < 2)
        return 0;
    if (xs[0] > xs[1]) { long long t = xs[0]; xs[0] = xs[1]; xs[1] = t; }
    return x >= xs[0] && x < xs[1];
}

static uint32_t rng = 12345;
static int rnd(int lo, int hi) /* inclusive */
{
    rng = rng * 1103515245u + 12345u;
    return lo + (int)((rng >> 8) % (uint32_t)(hi - lo + 1));
}

static void test_random_triangles(void)
{
    const int CX0 = 0, CY0 = 0, CX1 = 255, CY1 = 255;
    const int OX = 7, OY = -5;
    int t, x, y, bad_tris = 0, bad_px = 0;

    env_default();
    gp0(0xE3000000 | (uint32_t)(CY0 << 10) | (uint32_t)CX0);
    gp0(0xE4000000 | (uint32_t)(CY1 << 10) | (uint32_t)CX1);
    gp0(0xE5000000 | ((uint32_t)(OY & 0x7FF) << 11) | (uint32_t)(OX & 0x7FF));

    for (t = 0; t < 3000; t++) {
        P2 v[3];
        int i, bad = 0;
        int range = (t < 1000) ? 12 : 300; /* small ones stress edge cases */
        fill(0, 0, 320, 320, 0);
        for (i = 0; i < 3; i++) {
            v[i].x = rnd(-40, -40 + range);
            v[i].y = rnd(-40, -40 + range);
        }
        gp0(0x20FFFFFF);
        for (i = 0; i < 3; i++)
            gp0(xy(v[i].x, v[i].y));
        for (i = 0; i < 3; i++) {
            v[i].x += OX;
            v[i].y += OY;
        }
        for (y = 0; y < 320; y++)
            for (x = 0; x < 320; x++) {
                int exp = ref_cover(v[0], v[1], v[2], x, y) &&
                          x >= CX0 && x <= CX1 && y >= CY0 && y <= CY1;
                int got = px(x, y) != 0;
                if (exp != got) {
                    bad = 1;
                    bad_px++;
                }
            }
        if (bad && bad_tris++ < 5)
            printf("  triangle mismatch: (%d,%d) (%d,%d) (%d,%d)\n", v[0].x, v[0].y, v[1].x,
                   v[1].y, v[2].x, v[2].y);
    }
    CHECK(bad_tris == 0, "random triangle coverage: %d triangles / %d pixels differ", bad_tris, bad_px);

    /* Random quads drawn additively: shared edge must be covered exactly once */
    gp0(0xE1000020); /* abr = 1 (B+F) */
    bad_tris = 0;
    for (t = 0; t < 1000; t++) {
        P2 v[4];
        int i, bad = 0;
        fill(0, 0, 320, 320, 0);
        for (i = 0; i < 4; i++) {
            v[i].x = rnd(-20, 280);
            v[i].y = rnd(-20, 280);
        }
        gp0(0x2A080808); /* flat quad, semi-transparent, colour 1/1/1 in 5 bit */
        for (i = 0; i < 4; i++)
            gp0(xy(v[i].x, v[i].y));
        for (i = 0; i < 4; i++) {
            v[i].x += OX;
            v[i].y += OY;
        }
        for (y = 0; y < 320; y++)
            for (x = 0; x < 320; x++) {
                int in = x >= CX0 && x <= CX1 && y >= CY0 && y <= CY1;
                int exp = in ? ref_cover(v[0], v[1], v[2], x, y) + ref_cover(v[1], v[2], v[3], x, y) : 0;
                int got = px(x, y) & 31;
                if (exp != got)
                    bad = 1;
            }
        if (bad && bad_tris++ < 5)
            printf("  quad mismatch: (%d,%d) (%d,%d) (%d,%d) (%d,%d)\n", v[0].x, v[0].y, v[1].x,
                   v[1].y, v[2].x, v[2].y, v[3].x, v[3].y);
    }
    CHECK(bad_tris == 0, "random additive quads: %d mismatches", bad_tris);
    env_default();
}

/* ---------------------------------------------------------------------- */

static void test_basic_triangle(void)
{
    int x, y, ok = 1, count = 0;
    env_default();
    fill(0, 0, 16, 8, 0);
    gp0(0x200000FF); /* flat red */
    gp0(xy(0, 0));
    gp0(xy(4, 0));
    gp0(xy(0, 4));
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            int exp = (y < 4) && (x < 4 - y);
            uint16_t p = px(x, y);
            if (p)
                count++;
            if (exp != (p != 0) || (p && p != 0x001F))
                ok = 0;
        }
    CHECK(ok && count == 10, "triangle (0,0),(4,0),(0,4): coverage wrong (%d pixels)", count);
    CHECK(px(3, 0) == 0x1F && px(4, 0) == 0 && px(0, 3) == 0x1F && px(0, 4) == 0 && px(1, 3) == 0,
          "triangle edge pixels");

    /* pointy-top triangle: top vertex row is empty, sample points are integer coords */
    fill(0, 0, 16, 8, 0);
    gp0(0x2000FF00);
    gp0(xy(2, 0));
    gp0(xy(0, 4));
    gp0(xy(4, 4));
    CHECK(px(2, 0) == 0 && px(2, 1) == rgb15(0, 31, 0) && px(1, 1) == 0 && px(3, 1) == 0 &&
          px(1, 2) == rgb15(0, 31, 0) && px(3, 2) == 0 && px(0, 3) == 0 && px(1, 3) == rgb15(0, 31, 0),
          "pointy triangle coverage");

    /* too wide: |dx| = 1024 -> not drawn */
    fill(0, 0, 32, 32, 0);
    gp0(0x20FFFFFF);
    gp0(xy(-1000, 0));
    gp0(xy(24, 0));
    gp0(xy(0, 20));
    CHECK(px(1, 1) == 0, "triangle wider than 1023 must be skipped");
    gp0(0x20FFFFFF);
    gp0(xy(-999, 0));
    gp0(xy(24, 0));
    gp0(xy(0, 20));
    CHECK(px(1, 1) != 0, "triangle 1023 wide must be drawn");
}

static void test_gouraud_and_dither(void)
{
    int x, y, ok;
    env_default();
    fill(0, 0, 64, 64, 0);

    /* gouraud quad, dither off: core (left-most) vertex pixel has its exact colour */
    gp0(0x380000FF);           /* v0 red */
    gp0(xy(10, 10));
    gp0(0x0000FF00);           /* v1 green */
    gp0(xy(50, 10));
    gp0(0x00FF0000);           /* v2 blue */
    gp0(xy(10, 50));
    gp0(0x00FFFFFF);           /* v3 white */
    gp0(xy(50, 50));
    CHECK(px(10, 10) == rgb15(31, 0, 0), "gouraud: v0 pixel = %04X", px(10, 10));
    CHECK(px(49, 49) != 0 && px(50, 50) == 0 && px(9, 10) == 0, "gouraud quad extent");

    /* dithered constant gouraud 808080h: (128 + d) >> 3 = 15 for d<0 else 16 */
    fill(0, 0, 64, 64, 0);
    gp0(0xE1000200); /* dither on */
    gp0(0x30808080);
    gp0(xy(0, 0));
    gp0(0x00808080);
    gp0(xy(32, 0));
    gp0(0x00808080);
    gp0(xy(0, 32));
    {
        static const int m[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };
        ok = 1;
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++) {
                int c = (128 + m[y & 3][x & 3]) >> 3;
                if (px(x, y) != rgb15(c, c, c))
                    ok = 0;
            }
        CHECK(ok, "gouraud dither pattern");
    }
    /* flat untextured polygons are never dithered */
    fill(0, 0, 64, 64, 0);
    gp0(0x20808080);
    gp0(xy(0, 0));
    gp0(xy(32, 0));
    gp0(xy(0, 32));
    CHECK(px(0, 0) == rgb15(16, 16, 16) && px(1, 1) == rgb15(16, 16, 16), "flat polygon not dithered");
    gp0(0xE1000000);
}

static uint16_t g_clut[16];

static void test_textures(void)
{
    uint16_t tex4[4 * 16];
    uint16_t tex15[64 * 64];
    int x, y, ok;
    const int CLUT_X = 0, CLUT_Y = 500;
    const uint32_t clut_attr = (uint32_t)((CLUT_X / 16) | (CLUT_Y << 6));

    env_default();

    /* CLUT: entry 0 transparent, entry 15 has the semi-transparency bit */
    for (x = 0; x < 16; x++)
        g_clut[x] = (uint16_t)((x * 2) | ((31 - x * 2) << 10) | ((x & 3) << 7));
    g_clut[0] = 0;
    g_clut[15] |= 0x8000;
    upload(CLUT_X, CLUT_Y, 16, 1, g_clut);

    /* 4bpp 16x16 texture at page 10 (x=640), y=256: texel(u,v) = (u+v)&15 */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 4; x++) {
            uint16_t w = 0;
            int k;
            for (k = 0; k < 4; k++)
                w |= (uint16_t)(((x * 4 + k + y) & 15) << (k * 4));
            tex4[y * 4 + x] = w;
        }
    upload(640, 256, 4, 16, tex4);

    gp0(0xE1000000 | 10u | 0x10u); /* page 10, y 256, 4bpp */

    /* raw textured 16x16 sprite at (300,20) */
    fill(288, 0, 128, 128, 0x400000); /* background: dark blue (8) */
    gp0(0x7D000000);
    gp0(xy(300, 20));
    gp0(clut_attr << 16);
    ok = 1;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int idx = (x + y) & 15;
            uint16_t exp = idx ? g_clut[idx] : rgb15(0, 0, 8);
            if (px(300 + x, 20 + y) != exp)
                ok = 0;
        }
    CHECK(ok, "4bpp raw sprite");

    /* modulated by 404040h -> each 5-bit channel halved (floor) */
    gp0(0x7C404040);
    gp0(xy(320, 20));
    gp0((clut_attr << 16));
    ok = 1;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int idx = (x + y) & 15;
            uint16_t c = g_clut[idx], exp;
            if (!idx)
                exp = rgb15(0, 0, 8);
            else
                exp = (uint16_t)(((c & 31) >> 1) | ((((c >> 5) & 31) >> 1) << 5) |
                                 ((((c >> 10) & 31) >> 1) << 10) | (c & 0x8000));
            if (px(320 + x, 20 + y) != exp)
                ok = 0;
        }
    CHECK(ok, "4bpp modulated sprite");

    /* variable size sprite with a 16x16 texture window, offset (0,0) */
    gp0(0xE2000000 | (0x1Eu << 5) | 0x1Eu);
    gp0(0x65000000);
    gp0(xy(300, 40));
    gp0(clut_attr << 16);
    gp0(xy(40, 20));
    ok = 1;
    for (y = 0; y < 20; y++)
        for (x = 0; x < 40; x++) {
            int idx = ((x & 15) + (y & 15)) & 15;
            uint16_t exp = idx ? g_clut[idx] : rgb15(0, 0, 8);
            if (px(300 + x, 40 + y) != exp)
                ok = 0;
        }
    CHECK(ok, "texture window repeat");
    gp0(0xE2000000);

    /* semi-transparent textured sprite: only texels with bit15 blend (mode 1, B+F) */
    gp0(0xE1000000 | 10u | 0x10u | 0x20u);
    gp0(0x6F000000); /* 1x1 textured raw semi */
    gp0(xy(350, 20));
    gp0((clut_attr << 16) | 15u); /* u=15 -> index 15 (bit15 set) */
    gp0(0x6F000000);
    gp0(xy(351, 20));
    gp0((clut_attr << 16) | 1u); /* index 1 (opaque) */
    {
        uint16_t c15 = g_clut[15];
        int b = ((c15 >> 10) & 31) + 8;
        uint16_t exp = (uint16_t)((c15 & 0x3FF) | ((b > 31 ? 31 : b) << 10) | 0x8000);
        CHECK(px(350, 20) == exp, "semi-transparent texel blended: %04X vs %04X", px(350, 20), exp);
        CHECK(px(351, 20) == g_clut[1], "opaque texel not blended");
    }

    /* 15bpp texture, identity UV mapping on a quad reproduces the texture */
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            tex15[y * 64 + x] = (uint16_t)(((x >> 1) & 31) | (((y >> 1) & 31) << 5) | (((x ^ y) & 31) << 10) | 1);
    upload(512, 0, 64, 64, tex15);
    gp0(0x2D000000); /* textured quad, raw */
    gp0(xy(400, 300));
    gp0(0x00000000 | (clut_attr << 16));
    gp0(xy(464, 300));
    gp0(((uint32_t)(8 | (2 << 7)) << 16) | 64u); /* page x=512, 15bpp */
    gp0(xy(400, 364));
    gp0(64u << 8);
    gp0(xy(464, 364));
    gp0((64u << 8) | 64u);
    ok = 1;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            if (px(400 + x, 300 + y) != tex15[y * 64 + x]) {
                if (ok) printf("  first mismatch at %d,%d: %04X vs %04X\n", x, y, px(400 + x, 300 + y), tex15[y * 64 + x]);
                ok = 0;
            }
    CHECK(ok, "15bpp textured quad with identity UVs");
    CHECK(((gpu_read_status() >> 7) & 3) == 2 && (gpu_read_status() & 0xF) == 8,
          "polygon texpage updates GPUSTAT");

    /* half-scale mapping: u = floor(x/2 + 0.5) */
    gp0(0x2D000000);
    gp0(xy(480, 300));
    gp0(0);
    gp0(xy(544, 300));
    gp0(((uint32_t)(8 | (2 << 7)) << 16) | 32u);
    gp0(xy(480, 364));
    gp0(32u << 8);
    gp0(xy(544, 364));
    gp0((32u << 8) | 32u);
    ok = 1;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            if (px(480 + x, 300 + y) != tex15[((y + 1) >> 1) * 64 + ((x + 1) >> 1)])
                ok = 0;
    CHECK(ok, "15bpp textured quad at half scale");

    /* textured, modulated gouraud triangle (visual only) */
    gp0(0xE1000200 | 10u | 0x10u);
    gp0(0x34FF8080);
    gp0(xy(560, 300));
    gp0((clut_attr << 16) | 0x0000);
    gp0(0x0080FF80);
    gp0(xy(620, 300));
    gp0(((uint32_t)(10 | 0x10)) << 16 | 15u);
    gp0(0x008080FF);
    gp0(xy(560, 360));
    gp0(15u << 8);
    gp0(0xE1000000);
    env_default();
}

static void test_fill_copy(void)
{
    int x, y, ok;
    env_default();
    fill(0x100, 400, 0x40, 16, 0);
    fill(0x105, 400, 17, 4, 0x00F8F8F8);
    CHECK(px(0x100, 400) == 0x7FFF && px(0x11F, 403) == 0x7FFF && px(0x120, 400) == 0 &&
          px(0x100, 404) == 0, "fill rounding (x&3F0, w rounded up to 16)");

    /* fill wraps horizontally */
    fill(0x3F0, 420, 0x20, 2, 0x0000F800);
    CHECK(px(0x3FF, 420) == rgb15(0, 31, 0) && px(0x00F, 421) == rgb15(0, 31, 0) &&
          px(0x010, 420) != rgb15(0, 31, 0), "fill wraps at x=1024");

    /* fill ignores mask settings */
    gp0(0xE6000003);
    fill(0x140, 400, 16, 1, 0x000000F8);
    CHECK(px(0x140, 400) == 0x001F, "fill not affected by mask");
    gp0(0xE6000000);

    /* copy the sprite area */
    gp0(0x80000000);
    gp0(xy(300, 20));
    gp0(xy(700, 20));
    gp0(xy(36, 16));
    ok = 1;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 36; x++)
            if (px(700 + x, 20 + y) != px(300 + x, 20 + y))
                ok = 0;
    CHECK(ok, "VRAM to VRAM copy");

    /* copy with mask check: destination pixels with bit15 survive */
    fill(760, 60, 16, 1, 0);
    gp0(0xE6000001);
    gp0(0x68FFFFFF); /* 1x1 white with mask set */
    gp0(xy(762, 60));
    gp0(0xE6000002);
    gp0(0x80000000);
    gp0(xy(300, 20));
    gp0(xy(760, 60));
    gp0(xy(4, 1));
    CHECK(px(762, 60) == 0xFFFF && px(763, 60) == px(303, 20), "copy honours mask check");
    gp0(0xE6000000);
}

static void test_lines(void)
{
    int x, cnt;
    env_default();
    fill(576, 0, 64, 64, 0);

    gp0(0x40FFFFFF);
    gp0(xy(580, 40));
    gp0(xy(600, 40));
    for (cnt = 0, x = 576; x < 620; x++)
        cnt += px(x, 40) != 0;
    CHECK(cnt == 21 && px(580, 40) && px(600, 40), "horizontal line inclusive (%d px)", cnt);

    gp0(0x40FFFFFF);
    gp0(xy(600, 44));
    gp0(xy(580, 44));
    for (cnt = 0, x = 576; x < 620; x++)
        cnt += px(x, 44) != 0;
    CHECK(cnt == 21, "reversed line (%d px)", cnt);

    gp0(0x40FF00FF);
    gp0(xy(580, 2));
    gp0(xy(590, 30));
    CHECK(px(580, 2) != 0 && px(590, 30) != 0, "steep line endpoints");

    /* gouraud line */
    gp0(0x500000FF);
    gp0(xy(580, 50));
    gp0(0x00FF0000);
    gp0(xy(635, 55));
    CHECK(px(580, 50) == rgb15(31, 0, 0) && px(635, 55) != 0, "gouraud line endpoints");

    /* overlapping vertices: one pixel */
    gp0(0x4000FF00);
    gp0(xy(620, 10));
    gp0(xy(620, 10));
    CHECK(px(620, 10) == rgb15(0, 31, 0) && px(621, 10) == 0, "zero-length line draws 1 pixel");

    /* polyline (mono): square, then terminator, then a normal command */
    gp0(0x4800FFFF);
    gp0(xy(600, 15));
    gp0(xy(615, 15));
    gp0(xy(615, 30));
    gp0(xy(600, 30));
    gp0(xy(600, 15));
    gp0(0x55555555);
    gp0(0x68FF0000); /* 1x1 blue rect after the polyline */
    gp0(xy(608, 22));
    CHECK(px(600, 22) && px(615, 22) && px(608, 15) && px(608, 30) &&
          px(608, 22) == rgb15(0, 0, 31) && px(607, 22) == 0, "polyline + terminator");

    /* gouraud polyline: terminator is checked on the colour word */
    gp0(0x58FF0000);
    gp0(xy(580, 60));
    gp0(0x0000FF00);
    gp0(xy(600, 60));
    gp0(0x000000FF);
    gp0(xy(620, 62));
    gp0(0x50005000);
    gp0(0x68FFFFFF);
    gp0(xy(630, 63));
    CHECK(px(620, 62) == rgb15(31, 0, 0) && px(630, 63) == 0x7FFF, "gouraud polyline");
}

static void test_blend_mask(void)
{
    static const int exp[4] = { 12, 24, 8, 18 };
    int m;
    env_default();
    fill(0x200, 440, 16, 4, 0x808080); /* 16 in each channel */
    for (m = 0; m < 4; m++) {
        gp0(0xE1000000 | ((uint32_t)m << 5));
        gp0(0x6A404040); /* 1x1, semi-transparent, colour 8 */
        gp0(xy(0x200 + m, 440));
    }
    for (m = 0; m < 4; m++)
        CHECK(px(0x200 + m, 440) == rgb15(exp[m], exp[m], exp[m]), "blend mode %d: %04X", m,
              px(0x200 + m, 440));
    gp0(0xE1000000);

    /* mask set */
    gp0(0xE6000001);
    gp0(0x680000F8);
    gp0(xy(0x200, 441));
    CHECK(px(0x200, 441) == (0x8000 | 31), "mask bit set on draw");
    CHECK((gpu_read_status() >> 11 & 3) == 1, "GPUSTAT mask bits");
    /* mask check protects it */
    gp0(0xE6000002);
    gp0(0x6800F800);
    gp0(xy(0x200, 441));
    CHECK(px(0x200, 441) == (0x8000 | 31), "mask check protects pixel");
    /* CPU->VRAM honours mask check too */
    {
        uint16_t d[2] = { 0x1234, 0x0567 };
        upload(0x200, 441, 2, 1, d);
        CHECK(px(0x200, 441) == (0x8000 | 31) && px(0x201, 441) == 0x0567, "A0h honours mask check");
    }
    gp0(0xE6000000);
    gp0(0x6800F800);
    gp0(xy(0x200, 441));
    CHECK(px(0x200, 441) == rgb15(0, 31, 0), "mask cleared/overwritten");
}

static void test_transfers_and_status(void)
{
    uint16_t d[6] = { 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666 };
    uint32_t w0, w1, w2, st;

    env_default();
    upload(900, 480, 3, 2, d);
    CHECK(px(900, 480) == 0x1111 && px(902, 480) == 0x3333 && px(900, 481) == 0x4444 &&
          px(902, 481) == 0x6666, "CPU->VRAM transfer");

    /* wrapping transfer at the right edge */
    {
        uint16_t e[4] = { 0xAAAA & 0x7FFF, 0x0BBB, 0x0CCC, 0x0DDD };
        upload(1023, 470, 2, 2, e);
        CHECK(px(1023, 470) == e[0] && px(0, 470) == e[1] && px(1023, 471) == e[2] && px(0, 471) == e[3],
              "CPU->VRAM wraps horizontally");
    }

    gp0(0xC0000000);
    gp0(xy(900, 480));
    gp0(xy(3, 2));
    st = gpu_read_status();
    CHECK(st & (1u << 27), "GPUSTAT.27 set while VRAM->CPU pending");
    w0 = gpu_read_data();
    w1 = gpu_read_data();
    w2 = gpu_read_data();
    CHECK(w0 == 0x22221111u && w1 == 0x44443333u && w2 == 0x66665555u, "VRAM->CPU data %08X %08X %08X",
          w0, w1, w2);
    CHECK(!(gpu_read_status() & (1u << 27)), "GPUSTAT.27 clear after transfer");

    gp1(0x04000003);
    gp0(0xC0000000);
    gp0(xy(900, 480));
    gp0(xy(2, 1));
    CHECK((gpu_read_status() & (1u << 25)) && ((gpu_read_status() >> 29) & 3) == 3, "DMA dir 3 request");
    {
        uint32_t buf[1];
        gpu_dma_read(buf, 1);
        CHECK(buf[0] == 0x22221111u, "dma read");
    }
    CHECK(!(gpu_read_status() & (1u << 25)), "DMA dir 3 request cleared");
    gp1(0x04000000);

    /* GP1(10h) info */
    gp0(0xE2000000 | 0x12345u);
    gp0(0xE3000000 | (10u << 10) | 20u);
    gp0(0xE4000000 | (300u << 10) | 400u);
    gp0(0xE5000000 | ((uint32_t)(-8 & 0x7FF) << 11) | 16u);
    gp1(0x10000007);
    CHECK(gpu_read_data() == 2, "GPU version 2");
    gp1(0x10000002);
    CHECK(gpu_read_data() == 0x12345u, "texture window readback");
    gp1(0x10000003);
    CHECK(gpu_read_data() == ((10u << 10) | 20u), "draw area TL readback");
    gp1(0x10000004);
    CHECK(gpu_read_data() == ((300u << 10) | 400u), "draw area BR readback");
    gp1(0x10000005);
    CHECK(gpu_read_data() == (((uint32_t)(-8 & 0x7FF) << 11) | 16u), "draw offset readback");
    gp1(0x10000000);
    CHECK(gpu_read_data() == (((uint32_t)(-8 & 0x7FF) << 11) | 16u), "index 0 keeps old latch");

    gp0(0x1F000000);
    CHECK(gpu_read_status() & (1u << 24), "IRQ set");
    gp1(0x02000000);
    CHECK(!(gpu_read_status() & (1u << 24)), "IRQ ack");
    env_default();
}

static void test_display(uint32_t *out)
{
    GpuDisplay d;
    int w, h;

    gp1(0x08000001);                             /* 320x240 NTSC */
    gp1(0x06000000 | (0xC60u << 12) | 0x260u);   /* x1=260h x2=C60h */
    gp1(0x07000000 | (0x100u << 10) | 0x10u);    /* y1=10h y2=100h */
    gp1(0x05000000 | (0u << 10) | 0u);
    gp1(0x03000000);
    gpu_get_display(&d);
    CHECK(d.width == 320 && d.height == 240 && d.enabled && !d.is_24bit && d.hres_mode == 2,
          "display 320x240 (got %dx%d)", d.width, d.height);
    CHECK((gpu_read_status() & 0x007F4000u) == (1u << 17), "GPUSTAT display bits %08X", gpu_read_status());

    gp1(0x08000040); /* 368 mode */
    gp1(0x06000000 | ((0x260u + 368 * 7) << 12) | 0x260u);
    gpu_get_display(&d);
    CHECK(d.width == 368 && d.hres_mode == 1, "368 mode width %d", d.width);

    gp1(0x08000027); /* 640x480 interlaced */
    gp1(0x06000000 | ((0x260u + 640 * 4) << 12) | 0x260u);
    gpu_get_display(&d);
    CHECK(d.width == 640 && d.height == 480 && d.interlaced, "640x480i (%dx%d)", d.width, d.height);
    {
        uint32_t s0 = gpu_read_status(), s1;
        gpu_vblank();
        s1 = gpu_read_status();
        CHECK(((s0 ^ s1) & 0x80002000u) == 0x80002000u, "interlace field toggles bits 13/31");
    }

    gp1(0x07000000 | (0x10u << 10) | 0x10u); /* invalid vertical range -> nominal */
    gp1(0x08000001);
    gpu_get_display(&d);
    CHECK(d.height == 240, "nominal height fallback");

    /* 24-bit conversion */
    {
        uint16_t b[3];
        /* bytes: R0 G0 B0 R1 G1 B1 = 10 20 30 40 50 60 */
        b[0] = 0x2010;
        b[1] = 0x4030;
        b[2] = 0x6050;
        upload(800, 300, 3, 1, b);
        gp1(0x08000011);
        gp1(0x07000000 | (0x100u << 10) | 0x10u);
        gp1(0x06000000 | (0xC60u << 12) | 0x260u);
        gp1(0x05000000 | (300u << 10) | 800u);
        gpu_render_display(out, &w, &h);
        CHECK(out[0] == 0xFF102030u && out[1] == 0xFF405060u, "24-bit display %08X %08X", out[0], out[1]);
    }

    /* final: 15-bit 640x480 view of the top-left of VRAM for the screenshot */
    gp1(0x08000001);
    gp1(0x05000000);
    gpu_render_display(out, &w, &h);
    {
        uint16_t p = px(10, 10);
        uint32_t r = p & 31;
        CHECK((out[10 * w + 10] & 0xFF0000u) >> 16 == ((r << 3) | (r >> 2)), "15-bit expansion");
    }
}

static void test_state(void)
{
    size_t n = gpu_state_size();
    void *buf = malloc(n);
    uint16_t before = px(5, 5);
    env_default();
    gpu_save_state(buf);
    fill(0, 0, 16, 16, 0x00F800);
    CHECK(px(5, 5) != before || before == rgb15(0, 31, 0), "state test fill");
    gpu_load_state(buf);
    CHECK(px(5, 5) == before, "state restore");
    free(buf);
}

/* ---------------------------------------------------------------------- */
/* upscaling: native VRAM must not change; sprites/fills/copies/lines are exact n x n blocks */

static uint32_t argb15(uint16_t p)
{
    uint32_t r = p & 31u, g = (p >> 5) & 31u, b = (p >> 10) & 31u;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
}

static void screen_320(void)
{
    clear_vram();
    env_default();
    gp0(0xE4000000 | (239u << 10) | 319u);           /* draw area 320x240 */
    gp1(0x08000001);                                 /* display 320x240 at (0,0) */
    gp1(0x06000000 | (0xC60u << 12) | 0x260u);
    gp1(0x07000000 | (0x100u << 10) | 0x10u);
    gp1(0x05000000);
    gp1(0x03000000);
}

static void upscale_scene(void)
{
    uint16_t tex[256];
    int i;
    for (i = 0; i < 256; i++)
        tex[i] = (uint16_t)(rgb15(i & 31, (i >> 3) & 31, 31 - (i & 31)) | (i % 7 == 0 ? 0x8000 : 0));
    screen_320();
    upload(640, 0, 16, 16, tex);
    gp0(0xE100032A);                                 /* dither, B+F, 15-bit page at x=640 */
    gp0(0x300000FF); gp0(xy(10, 10)); gp0(0x0000FF00); gp0(xy(200, 30)); gp0(0x00FF0000); gp0(xy(60, 200));
    gp0(0x2C906050);                                 /* textured quad, modulated */
    gp0(xy(150, 100)); gp0(0);
    gp0(xy(300, 90)); gp0(0x012A0000u | 15u);
    gp0(xy(160, 180)); gp0(15u << 8);
    gp0(xy(310, 170)); gp0((15u << 8) | 15u);
    gp0(0x2A303030); gp0(xy(40, 60)); gp0(xy(250, 50)); gp0(xy(30, 160)); gp0(xy(240, 150));
    gp0(0x40FFFF00); gp0(xy(10, 5)); gp0(xy(300, 8));
    gp0(0x65000000); gp0(xy(100, 215)); gp0(0); gp0(xy(16, 16));
    fill(288, 0, 16, 64, 0x2080F0);
    gp0(0x80000000); gp0(xy(100, 215)); gp0(xy(250, 215)); gp0(xy(16, 16));
}

/* 1:1 textured quad, marker colour just outside its UV range: at every scale the upscaled
 * copy must be the native picture enlarged, never showing the marker */
static void test_upscale_texels(void)
{
    static uint16_t tex[32 * 32];
    uint32_t *out = (uint32_t *)malloc((size_t)1024 * 512 * 16 * sizeof(uint32_t));
    int n, w, h, x, y, i;

    for (i = 0; i < 32 * 32; i++)
        tex[i] = (i % 32 < 16 && i / 32 < 16) ? rgb15(i % 32 + 8, i / 32 + 8, 5) : rgb15(0, 0, 31);
    for (n = 2; n <= 4; n++) {
        long bad = 0, marker = 0;
        gpu_set_upscale(n);
        screen_320();
        upload(640, 0, 32, 32, tex);
        gp0(0x2D000000);
        gp0(xy(100, 100)); gp0(0);
        gp0(xy(116, 100)); gp0((0x10Au << 16) | 16u);
        gp0(xy(100, 116)); gp0(16u << 8);
        gp0(xy(116, 116)); gp0((16u << 8) | 16u);
        gpu_render_display(out, &w, &h);
        for (y = 100 * n; y < 116 * n; y++)
            for (x = 100 * n; x < 116 * n; x++) {
                bad += out[y * w + x] != argb15(px(x / n, y / n));
                marker += out[y * w + x] == argb15(rgb15(0, 0, 31));
            }
        CHECK(bad == 0 && marker == 0, "upscale %d: 1:1 texels off by %ld pixels, %ld outside the UV range", n, bad, marker);

        /* the same quad with the texture magnified 4x (u, v = 12..16, marker beyond): still nothing outside */
        marker = 0;
        fill(100, 100, 16, 16, 0);
        gp0(0x2D000000);
        gp0(xy(100, 100)); gp0((12u << 8) | 12u);
        gp0(xy(116, 100)); gp0((0x10Au << 16) | (12u << 8) | 16u);
        gp0(xy(100, 116)); gp0((16u << 8) | 12u);
        gp0(xy(116, 116)); gp0((16u << 8) | 16u);
        gpu_render_display(out, &w, &h);
        for (y = 100 * n; y < 116 * n; y++)
            for (x = 100 * n; x < 116 * n; x++)
                marker += out[y * w + x] == argb15(rgb15(0, 0, 31));
        CHECK(marker == 0, "upscale %d: magnified texture shows %ld pixels outside the UV range", n, marker);
    }
    gpu_set_upscale(1);
    free(out);
}

static void test_upscale(void)
{
    static uint16_t ref[1024 * 512];
    uint32_t *out = (uint32_t *)malloc((size_t)1024 * 512 * 16 * sizeof(uint32_t));
    long native = 0;
    int n, w, h, x, y;

    gpu_set_upscale(1);
    upscale_scene();
    memcpy(ref, gpu_vram(), sizeof ref);
    for (y = 0; y < 240; y++)
        for (x = 0; x < 320; x++)
            native += (ref[y * 1024 + x] & 0x7FFF) != 0;

    for (n = 2; n <= 4; n++) {
        long hi = 0, blk_bad = 0, want = native * n * n;
        gpu_set_upscale(n);
        CHECK(gpu_upscale() == n, "upscale %d enabled", n);
        upscale_scene();
        CHECK(!memcmp(ref, gpu_vram(), sizeof ref), "upscale %d: native VRAM changed", n);
        gpu_render_display(out, &w, &h);
        CHECK(w == 320 * n && h == 240 * n, "upscale %d: display %dx%d", n, w, h);
        if (w != 320 * n || h != 240 * n)
            continue;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                int nx = x / n, ny = y / n;
                hi += (out[y * w + x] & 0xFFFFFFu) != 0;
                if ((ny >= 215 && ny < 231 && ((nx >= 100 && nx < 116) || (nx >= 250 && nx < 266))) ||
                    (nx >= 288 && nx < 304 && ny < 64))
                    blk_bad += out[y * w + x] != argb15(ref[ny * 1024 + nx]);
            }
        CHECK(blk_bad == 0, "upscale %d: %ld sprite/copy/fill/line pixels not block-scaled", n, blk_bad);
        CHECK(labs(hi - want) * 33 < want, "upscale %d: coverage %ld, expected about %ld", n, hi, want);
    }
    gpu_set_upscale(1);
    free(out);
}

/* ---------------------------------------------------------------------- */
/* PGXP: tagged vertex words use their precise position (and depth) in the upscaled copy only */

static void gp0_at(uint32_t w, uint32_t addr)
{
    g_gp0_addr = addr;
    gp0(w);
    g_gp0_addr = 0xFFFFFFFFu;
}

/* flat triangle (command cmd) whose vertex words are tagged at integer + d; bad != 0 tags vertex 0
 * with a stale value; cover: over a black rectangle drawn first instead of the screen clear */
static void pgxp_triangle(float d, uint32_t bad, uint32_t cmd, int cover)
{
    static const int P[3][2] = { { 20, 20 }, { 60, 20 }, { 20, 60 } };
    int i;
    screen_320();
    if (cover) {
        gp0(0x60000000);
        gp0(xy(0, 0));
        gp0(xy(100, 100));
    }
    for (i = 0; i < 3; i++)
        gpu_pgxp_store(0x1000u + 4u * (uint32_t)i, (bad && !i) ? bad : xy(P[i][0], P[i][1]), 1,
                       (float)P[i][0] + d, (float)P[i][1] + d, 100.0f);
    gp0(cmd);
    for (i = 0; i < 3; i++)
        gp0_at(xy(P[i][0], P[i][1]), 0x1000u + 4u * (uint32_t)i);
}

/* textured triangle A(10,100) w=1, B(210,100) w=4, C(10,160) w=1, u = 0 / 63 / 0;
 * returns the texel u seen at native (110, 101) in the 4x copy (texel red = u / 2) */
static int persp_u_at_middle(uint32_t *out)
{
    uint16_t tex[64];
    int i, w, h;
    for (i = 0; i < 64; i++)
        tex[i] = rgb15(i >> 1, 0, 0);
    screen_320();
    upload(640, 0, 64, 1, tex);
    gpu_pgxp_store(0x2000, xy(10, 100), 1, 10, 100, 1.0f);
    gpu_pgxp_store(0x2004, xy(210, 100), 1, 210, 100, 4.0f);
    gpu_pgxp_store(0x2008, xy(10, 160), 1, 10, 160, 1.0f);
    gp0(0x25000000);
    gp0_at(xy(10, 100), 0x2000); gp0(0);
    gp0_at(xy(210, 100), 0x2004); gp0((0x10Au << 16) | 63u);
    gp0_at(xy(10, 160), 0x2008); gp0(0);
    gpu_render_display(out, &w, &h);
    return (int)(((out[404 * w + 440] >> 16) & 0xFFu) >> 3) * 2;
}

static void test_pgxp(void)
{
    static uint16_t ref[1024 * 512];
    uint32_t *out = (uint32_t *)malloc((size_t)1024 * 512 * 16 * sizeof(uint32_t));
    int w, h, u;
#define HI(x, y) ((out[(y) * w + (x)] & 0xFFFFFFu) != 0)

    gpu_set_upscale(4);
    g_pgxp = 0;
    pgxp_triangle(0.5f, 0, 0x20FFFFFF, 1);
    memcpy(ref, gpu_vram(), sizeof ref);
    gpu_render_display(out, &w, &h);
    CHECK(HI(81, 81), "pgxp off: corner at the integer position");

    g_pgxp = 1;
    pgxp_triangle(0.5f, 0, 0x20FFFFFF, 1);
    CHECK(!memcmp(ref, gpu_vram(), sizeof ref), "pgxp: native VRAM unchanged");
    gpu_render_display(out, &w, &h);
    CHECK(!HI(81, 81) && HI(82, 82), "pgxp: corner moved by half a native pixel (drawn black stays black)");

    /* cracks: where the precise shape leaves the screen clear showing, the console's shape fills it */
    pgxp_triangle(0.5f, 0, 0x20FFFFFF, 0);
    gpu_render_display(out, &w, &h);
    CHECK(HI(81, 81) && HI(82, 82), "pgxp: background left open filled with the console's shape");
    CHECK(!HI(79, 81) && !HI(81, 79), "pgxp: nothing filled outside the console's shape");
    pgxp_triangle(0.5f, 0, 0x22FFFFFF, 0);
    gpu_render_display(out, &w, &h);
    CHECK(!HI(81, 81) && HI(82, 82), "pgxp: see-through polygons are not drawn twice");

    pgxp_triangle(0.5f, xy(21, 20), 0x20FFFFFF, 1);
    gpu_render_display(out, &w, &h);
    CHECK(HI(81, 81), "pgxp: stale tag ignored");

    u = persp_u_at_middle(out);
    CHECK(u >= 30 && u <= 32, "affine u at the middle of the edge = %d (expected ~31)", u);
    g_pgxp_persp = 1;
    u = persp_u_at_middle(out);
    CHECK(u >= 10 && u <= 14, "perspective-correct u = %d (expected ~12.6)", u);
#undef HI
    g_pgxp = g_pgxp_persp = 0;
    gpu_set_upscale(1);
    free(out);
}

/* flat quad (colour col) at native corners P[i], tagged at precise positions F[i]; words at addr.. */
static void pgxp_quad(uint32_t col, const int P[4][2], const float F[4][2], uint32_t addr)
{
    int i;
    for (i = 0; i < 4; i++)
        gpu_pgxp_store(addr + 4u * (uint32_t)i, xy(P[i][0], P[i][1]), 1, F[i][0], F[i][1], 100.0f);
    gp0(0x28000000 | col);
    for (i = 0; i < 4; i++)
        gp0_at(xy(P[i][0], P[i][1]), addr + 4u * (uint32_t)i);
}

/* Seams: two 3D quads that meet on the console at x 40, with a gap of 3/4 pixel between their
 * precise edges (39.5 and 40.25) over an older 3D ground quad: the gap is filled from its sides;
 * the left quad's outline over the ground (20.5) stays as drawn */
static void test_pgxp_seams(void)
{
    static const int G[4][2] = { { 10, 10 }, { 90, 10 }, { 10, 90 }, { 90, 90 } };
    static const float GF[4][2] = { { 10, 10 }, { 90, 10 }, { 10, 90 }, { 90, 90 } };
    static const int B[4][2] = { { 20, 20 }, { 40, 20 }, { 20, 60 }, { 40, 60 } };
    static const float BF[4][2] = { { 20.5f, 20 }, { 39.5f, 20 }, { 20.5f, 60 }, { 39.5f, 60 } };
    static const int C[4][2] = { { 40, 20 }, { 60, 20 }, { 40, 60 }, { 60, 60 } };
    static const float CF[4][2] = { { 40.25f, 20 }, { 60, 20 }, { 40.25f, 60 }, { 60, 60 } };
    uint32_t *out = (uint32_t *)malloc((size_t)1024 * 512 * 16 * sizeof(uint32_t));
    int w, h, x, gap = 0;
    gpu_set_upscale(4);
    g_pgxp = 1;
    screen_320();
    pgxp_quad(0x00FF00, G, GF, 0x3000);         /* ground: green */
    pgxp_quad(0x0000FF, B, BF, 0x3010);         /* red */
    pgxp_quad(0xFF0000, C, CF, 0x3020);         /* blue */
    gpu_render_display(out, &w, &h);
    for (x = 156; x < 164; x++)
        gap += (out[160 * w + x] & 0xFFFFFFu) == 0x00FF00u;
    CHECK(gap == 0, "seam: no ground showing between the quads (%d px)", gap);
    CHECK((out[160 * w + 81] & 0xFFFFFFu) == 0x00FF00u, "seam: the outline over the ground stays as drawn");
    CHECK((out[160 * w + 83] & 0xFFFFFFu) == 0xFF0000u, "seam: the quad itself as drawn");
    screen_320();                               /* the right quad drawn first */
    pgxp_quad(0x00FF00, G, GF, 0x3000);
    pgxp_quad(0xFF0000, C, CF, 0x3020);
    pgxp_quad(0x0000FF, B, BF, 0x3010);
    gpu_render_display(out, &w, &h);
    for (gap = 0, x = 156; x < 164; x++)
        gap += (out[160 * w + x] & 0xFFFFFFu) == 0x00FF00u;
    CHECK(gap == 0, "seam: filled whichever side was drawn first (%d px)", gap);
    {                                           /* the same with band workers */
        uint32_t *one = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
        memcpy(one, out, (size_t)w * h * sizeof(uint32_t));
        gpu_set_threads(4);
        screen_320();
        pgxp_quad(0x00FF00, G, GF, 0x3000);
        pgxp_quad(0xFF0000, C, CF, 0x3020);
        pgxp_quad(0x0000FF, B, BF, 0x3010);
        gpu_render_display(out, &w, &h);
        gpu_set_threads(0);
        CHECK(!memcmp(one, out, (size_t)w * h * sizeof(uint32_t)), "seam: the same picture with band workers");
        free(one);
    }
    {                                           /* the same with perspective-correct textured quads */
        static uint16_t tex[16 * 16];
        int i, k;
        for (i = 0; i < 16 * 16; i++)
            tex[i] = (i % 16) < 8 ? rgb15(31, 0, 0) : rgb15(0, 0, 31);
        g_pgxp_persp = 1;
        screen_320();
        upload(640, 0, 16, 16, tex);
        pgxp_quad(0x00FF00, G, GF, 0x3000);
        for (k = 0; k < 2; k++) {               /* B from the red texels, C from the blue */
            const int (*P)[2] = k ? C : B;
            const float (*F)[2] = k ? CF : BF;
            uint32_t a = k ? 0x3020u : 0x3010u, u0 = k ? 8u : 0u;
            for (i = 0; i < 4; i++)
                gpu_pgxp_store(a + 4u * (uint32_t)i, xy(P[i][0], P[i][1]), 1, F[i][0], F[i][1], 100.0f);
            gp0(0x2D000000);                    /* textured quad, raw */
            gp0_at(xy(P[0][0], P[0][1]), a);        gp0(u0);
            gp0_at(xy(P[1][0], P[1][1]), a + 4);    gp0((0x10Au << 16) | (u0 + 7u));
            gp0_at(xy(P[2][0], P[2][1]), a + 8);    gp0((15u << 8) | u0);
            gp0_at(xy(P[3][0], P[3][1]), a + 12);   gp0((15u << 8) | (u0 + 7u));
        }
        gpu_render_display(out, &w, &h);
        g_pgxp_persp = 0;
        for (gap = 0, x = 156; x < 164; x++)
            gap += (out[160 * w + x] & 0xFFFFFFu) == 0x00FF00u;
        CHECK(gap == 0, "seam: filled between perspective-correct textured quads (%d px)", gap);
    }
    g_pgxp_cracks = 0;                          /* crackfix = 0: drawn as before */
    gpu_set_upscale(4);
    screen_320();
    pgxp_quad(0x00FF00, G, GF, 0x3000);
    pgxp_quad(0x0000FF, B, BF, 0x3010);
    pgxp_quad(0xFF0000, C, CF, 0x3020);
    gpu_render_display(out, &w, &h);
    for (gap = 0, x = 156; x < 164; x++)
        gap += (out[160 * w + x] & 0xFFFFFFu) == 0x00FF00u;
    CHECK(gap == 3, "seam: left open with the fix off (%d px)", gap);
    g_pgxp_cracks = 1;
    g_pgxp = 0;
    gpu_set_upscale(1);
    free(out);
}

/* Sprites at projected points: a tagged rectangle moves rigidly by its sub-pixel offset in the
 * upscaled copy (texels stay n x n blocks); untagged or stale ones stay put; native VRAM unchanged */
static void sprite_scene(int anchored, uint32_t tag_value, float scale, float z)
{
    uint16_t tex[16 * 16];
    int i;
    for (i = 0; i < 256; i++)
        tex[i] = rgb15(i % 16 + 4, i / 16 + 4, (i * 7) % 31 + 1);
    screen_320();
    upload(640, 0, 16, 16, tex);
    gp0(0xE100010A);                                  /* 15-bit page at x=640 */
    gpu_sprite_anchor(100, 100, 0.5f, -0.25f, scale, z, 0);
    gpu_sprite_begin(anchored ? 100 : 99, 100);      /* the builder starts at a known anchor or not */
    gpu_sprite_tag(0x3000, tag_value);               /* ... and writes the position word */
    gp0(0x65000000); gp0_at(xy(110, 120), 0x3000); gp0(0); gp0(xy(16, 16));
}

/* Widescreen HUD: an 8x12 glyph at (232,19) and a white window border line from (232,50) to
 * (303,50), tagged as HUD (on) or with the tags dropped */
static void hud_scene(int on)
{
    uint16_t tex[16 * 16];
    int i;
    for (i = 0; i < 256; i++)
        tex[i] = rgb15(i % 16 + 4, i / 16 + 4, (i * 7) % 31 + 1);
    screen_320();
    upload(640, 0, 16, 16, tex);
    gp0(0xE100010A);
    gpu_hud_begin(on);
    gpu_sprite_tag(0x3000, xy(232, 19));
    gp0(0x65000000); gp0_at(xy(232, 19), 0x3000); gp0(0); gp0(xy(8, 12));
    gpu_sprite_tag(0x3100, xy(232, 50));
    gp0(0x40FFFFFF); gp0_at(xy(232, 50), 0x3100); gp0(xy(303, 50));
}

static int no_3d_scene(void) { return 0; }

/* What the player holds: the player's anchor at (160,120) + (0.5,-0.25), a 16x16 item at
 * (145,83) built as the held object (on) or as another item (off) */
static void held_scene(int on)
{
    uint16_t tex[16 * 16];
    int i;
    for (i = 0; i < 256; i++)
        tex[i] = rgb15(i % 16 + 4, i / 16 + 4, (i * 7) % 31 + 1);
    screen_320();
    upload(640, 0, 16, 16, tex);
    gp0(0xE100010A);
    gpu_sprite_anchor(160, 120, 0.5f, -0.25f, 1.0f, 1000.0f, 1);
    gpu_held_begin(on);
    gpu_sprite_tag(0x3000, xy(145, 83));
    gp0(0x65000000); gp0_at(xy(145, 83), 0x3000); gp0(0); gp0(xy(16, 16));
}

static void test_pgxp_sprite(void)
{
    static uint16_t ref[1024 * 512];
    uint32_t *out = (uint32_t *)malloc((size_t)1024 * 512 * 16 * sizeof(uint32_t));
    const int n = 4;
    int w, h, x, y, i, j;
    long bad = 0;

    gpu_set_upscale(n);
    g_pgxp = g_sprite_tags = 0;
    sprite_scene(1, xy(110, 120), 1.0f, 0.0f);
    memcpy(ref, gpu_vram(), sizeof ref);
    g_pgxp = g_sprite_tags = 1;
    sprite_scene(1, xy(110, 120), 1.0f, 0.0f);
    CHECK(g_sprite_on, "anchor found");
    CHECK(!memcmp(ref, gpu_vram(), sizeof ref), "sprite shift: native VRAM unchanged");
    gpu_render_display(out, &w, &h);
    for (y = 120; y < 136; y++)
        for (x = 110; x < 126; x++)
            for (j = 0; j < n; j++)
                for (i = 0; i < n; i++)
                    bad += out[(y * n + j - 1) * w + (x * n + i + 2)] != argb15(px(x, y));
    CHECK(bad == 0, "tagged sprite: %ld pixels not shifted by (+2,-1) as exact 4x4 texel blocks", bad);

    sprite_scene(0, xy(110, 120), 1.0f, 0.0f);                     /* rebuilt without a known anchor */
    gpu_render_display(out, &w, &h);
    CHECK(out[120 * n * w + 110 * n] == argb15(px(110, 120)), "sprite without anchor not shifted");
    sprite_scene(1, xy(111, 120), 1.0f, 0.0f);                     /* tag no longer matches the word */
    gpu_render_display(out, &w, &h);
    CHECK(out[120 * n * w + 110 * n] == argb15(px(110, 120)), "stale sprite tag ignored");
    gpu_sprite_begin(99, 100);
    CHECK(!g_sprite_on, "unknown anchor not found");

    /* a 2D sprite quad (multi-part sprites): moves when all four corners carry the same tag */
    {
        static const int Q[4][2] = { { 150, 150 }, { 166, 150 }, { 150, 166 }, { 166, 166 } };
        static const uint32_t UV[4] = { 0, 0x010A0000u | 16u, 16u << 8, (16u << 8) | 16u };
        int k, tagged;
        for (tagged = 4; tagged >= 3; tagged--) {
            long badq = 0;
            sprite_scene(1, xy(110, 120), 1.0f, 0.0f);
            for (k = 0; k < 4; k++)
                gpu_sprite_tag(0x5000u + 8u * (uint32_t)k, k < tagged ? xy(Q[k][0], Q[k][1]) : 0);
            gp0(0x2D000000);
            for (k = 0; k < 4; k++) {
                gp0_at(xy(Q[k][0], Q[k][1]), 0x5000u + 8u * (uint32_t)k);
                gp0(UV[k]);
            }
            gpu_render_display(out, &w, &h);
            for (y = 150; y < 166; y++)
                for (x = 150; x < 166; x++)
                    for (j = 0; j < n; j++)
                        for (i = 0; i < n; i++)
                            badq += out[(y * n + j - (tagged == 4)) * w + (x * n + i + 2 * (tagged == 4))] != argb15(px(x, y));
            CHECK(badq == 0, "sprite quad with %d tagged corners: %ld pixels not where expected", tagged, badq);
        }
    }

    /* depth scaling, scale 0.5 about the anchor (100,100) + offset (0.5,-0.25): the 16x16 rect at
     * (110,120) covers target pixels 422..453 x 439..470, 2x2 per texel; native VRAM unchanged */
    {
        long bads = 0;
        g_sprite_depth = 1;
        sprite_scene(1, xy(110, 120), 0.5f, 0.0f);
        CHECK(!memcmp(ref, gpu_vram(), sizeof ref), "depth-scaled sprite: native VRAM unchanged");
        gpu_render_display(out, &w, &h);
        for (y = 439; y < 471; y++)
            for (x = 422; x < 454; x++)
                bads += out[y * w + x] != argb15(px(110 + (x - 422) / 2, 120 + (y - 439) / 2));
        CHECK(bads == 0, "depth-scaled sprite: %ld pixels not the half-size sprite", bads);
        CHECK(out[439 * w + 421] != out[439 * w + 422] && out[471 * w + 422] != out[470 * w + 422] &&
              out[135 * n * w + 125 * n] != argb15(px(125, 135)), "depth-scaled sprite: nothing drawn outside");
        sprite_scene(1, xy(110, 120), 1.0f, 0.0f);              /* scale 1 stays a rigid shift */
        gpu_render_display(out, &w, &h);
        CHECK(out[(120 * n - 1) * w + 110 * n + 2] == argb15(px(110, 120)), "depth scaling at scale 1: rigid shift");

        /* with the player projected, the scale is the depth of the player's ground level over the
         * anchor's: scale 1 at depth 1000 there, the sprite (scale 1 on its own level) at 2000 */
        bads = 0;
        gpu_sprite_anchor(160, 120, 0.0f, 0.0f, 1.0f, 1000.0f, 2);
        sprite_scene(1, xy(110, 120), 1.0f, 2000.0f);
        gpu_render_display(out, &w, &h);
        for (y = 439; y < 471; y++)
            for (x = 422; x < 454; x++)
                bads += out[y * w + x] != argb15(px(110 + (x - 422) / 2, 120 + (y - 439) / 2));
        CHECK(bads == 0, "depth scale against the player's level: %ld pixels not the half-size sprite", bads);
        /* kept while the camera doesn't follow the player (off centre) or it isn't projected */
        gpu_sprite_anchor(60, 200, 0.0f, 0.0f, 1.0f, 500.0f, 1);
        for (i = 0; i < 5; i++)
            gpu_vblank();
        bads = 0;
        sprite_scene(1, xy(110, 120), 1.0f, 2000.0f);
        gpu_render_display(out, &w, &h);
        for (y = 439; y < 471; y++)
            for (x = 422; x < 454; x++)
                bads += out[y * w + x] != argb15(px(110 + (x - 422) / 2, 120 + (y - 439) / 2));
        CHECK(bads == 0, "reference kept: %ld pixels not the half-size sprite", bads);
        g_sprite_depth = 0;
    }

    /* widescreen 16:9: squeezed by 3/4 horizontally about the anchor: the rect covers target pixels
     * 432..479 x 479..542, 3x4 per texel; native VRAM unchanged */
    {
        long badw = 0;
        g_sprite_ws = 0.75f;
        sprite_scene(1, xy(110, 120), 1.0f, 0.0f);
        CHECK(!memcmp(ref, gpu_vram(), sizeof ref), "widescreen sprite: native VRAM unchanged");
        gpu_render_display(out, &w, &h);
        for (y = 479; y < 543; y++)
            for (x = 432; x < 480; x++)
                badw += out[y * w + x] != argb15(px(110 + (x - 432) / 3, 120 + (y - 479) / 4));
        CHECK(badw == 0, "widescreen sprite: %ld pixels not the squeezed sprite", badw);
        CHECK(out[479 * w + 431] != out[479 * w + 432] && out[479 * w + 480] != out[479 * w + 479],
              "widescreen sprite: nothing drawn beside it");
        g_sprite_ws = 1.0f;
    }

    /* widescreen HUD: squeezed by 3/4 about the middle of the drawing area (x 160) so it keeps its
     * 4:3 shape: the glyph covers target pixels 856..879 x 76..123, 3x4 per texel */
    {
        long badh = 0;
        g_sprite_ws = 0.75f;
        g_sprite_depth = 1;                            /* HUD tags have no depth scale */
        hud_scene(1);
        gpu_render_display(out, &w, &h);
        for (y = 76; y < 124; y++)
            for (x = 856; x < 880; x++)
                badh += out[y * w + x] != argb15(px(232 + (x - 856) / 3, 19 + (y - 76) / 4));
        CHECK(badh == 0, "widescreen HUD: %ld pixels not the glyph squeezed about the centre", badh);
        CHECK(out[76 * w + 855] != out[76 * w + 856] && out[76 * w + 880] != out[76 * w + 879],
              "widescreen HUD: nothing drawn beside it");
        CHECK(out[200 * w + 856] == 0xFFFFFFFFu && out[203 * w + 1071] == 0xFFFFFFFFu &&
              out[200 * w + 855] != 0xFFFFFFFFu && out[200 * w + 1072] != 0xFFFFFFFFu &&
              out[200 * w + 1215] != 0xFFFFFFFFu, "widescreen HUD line: target pixels 856..1071, squeezed");
        /* after HUD in this frame, an untagged rectangle is HUD too (the glyph again at (232,60)),
         * but not one covering the whole width (green, rows 100..107) */
        gp0(0x65000000); gp0(xy(232, 60)); gp0(0); gp0(xy(8, 12));
        gp0(0x6000FF00); gp0(xy(0, 100)); gp0(xy(320, 8));
        gpu_render_display(out, &w, &h);
        badh = 0;
        for (y = 240; y < 288; y++)
            for (x = 856; x < 880; x++)
                badh += out[y * w + x] != argb15(px(232 + (x - 856) / 3, 60 + (y - 240) / 4));
        CHECK(badh == 0, "untagged rectangle after the HUD: %ld pixels not squeezed about the centre", badh);
        CHECK(out[400 * w] == argb15(px(0, 100)) && out[431 * w + 1279] == argb15(px(319, 107)) &&
              (px(0, 100) & 0x3E0u), "full-width rectangle after the HUD: not squeezed");
        /* ... and so is a flat polygon (the rucksack's selection box is textured quads): red,
         * x 232..240 -> target 856..879; one covering the whole width (blue, rows 120..123) is not */
        gp0(0x280000FF); gp0(xy(232, 140)); gp0(xy(240, 140)); gp0(xy(232, 148)); gp0(xy(240, 148));
        gp0(0x28FF0000); gp0(xy(0, 120)); gp0(xy(320, 120)); gp0(xy(0, 124)); gp0(xy(320, 124));
        gpu_render_display(out, &w, &h);
        CHECK(out[570 * w + 856] == argb15(px(232, 142)) && out[570 * w + 879] == argb15(px(239, 142)) &&
              out[570 * w + 855] != out[570 * w + 856] && out[570 * w + 880] != out[570 * w + 879] &&
              (px(232, 142) & 0x1Fu), "flat polygon after the HUD: squeezed about the centre");
        CHECK(out[482 * w] == argb15(px(0, 120)) && out[482 * w + 1279] == argb15(px(319, 120)) &&
              (px(0, 120) & 0x7C00u), "full-width polygon after the HUD: not squeezed");
        gpu_vblank();
        g_scene_3d = no_3d_scene;                        /* tagged, but the picture isn't a 3D scene */
        hud_scene(1);
        gpu_render_display(out, &w, &h);
        CHECK(out[76 * w + 928] == argb15(px(232, 19)) && out[203 * w + 1215] == 0xFFFFFFFFu,
              "HUD while the picture is not a 3D scene: not squeezed");
        g_scene_3d = NULL;
        gpu_vblank();                                    /* the next frame starts with the world */
        hud_scene(0);
        gpu_render_display(out, &w, &h);
        CHECK(out[76 * w + 928] == argb15(px(232, 19)) && out[123 * w + 959] == argb15(px(239, 30)) &&
              out[203 * w + 1215] == 0xFFFFFFFFu, "HUD outside 3D scenes: not squeezed");
        g_sprite_ws = 1.0f;
        g_sprite_depth = 0;
    }

    /* widescreen menus (and the title screen): a frame with a background tile makes its buffer a
     * menu (shown wide). A row of see-through 16x16 tiles at x -8..328, every other one darker
     * (period 32), is squeezed about the middle and repeated to fill the sides: each target column
     * shows the native picture at the place a whole number of periods away that is on screen,
     * blended once. What is drawn over it keeps its 4:3 shape: the HUD glyph, an untagged one (target
     * 856..879), not a rectangle covering the whole width. The next frame without one is no menu. */
    {
        uint16_t tex[16 * 16];
        long badm = 0, badg = 0;
        g_sprite_ws = 0.75f;
        gpu_vblank();
        screen_320();
        for (i = 0; i < 256; i++)
            tex[i] = (uint16_t)(0x8000u | rgb15(i % 16 + 4, i / 16 + 4, (i * 7) % 31 + 1));
        upload(640, 0, 16, 16, tex);
        gp0(0xE100010A);                                /* 15-bit page at x=640, (B + F) / 2 */
        for (i = 0; i < 21; i++) {
            gpu_menu_begin(32);
            gpu_sprite_tag(0x3200u + 16u * (uint32_t)i, xy(i * 16 - 8, 100));
            gp0(0x66000000 | ((i & 1) ? 0x404040u : 0x808080u));
            gp0_at(xy(i * 16 - 8, 100), 0x3200u + 16u * (uint32_t)i); gp0(0); gp0(xy(16, 16));
        }
        gpu_hud_begin(1);
        gpu_sprite_tag(0x3000, xy(232, 19));
        gp0(0x65000000); gp0_at(xy(232, 19), 0x3000); gp0(0); gp0(xy(8, 12));
        gp0(0x65000000); gp0(xy(232, 60)); gp0(0); gp0(xy(8, 12));
        gp0(0x6000FF00); gp0(xy(0, 140)); gp0(xy(320, 8));
        gp0(0xE3000000 | 30u);                          /* drawing area 30..319, as the toolbox's help box */
        gpu_sprite_tag(0x3010, xy(232, 170));
        gp0(0x65000000); gp0_at(xy(232, 170), 0x3010); gp0(0); gp0(xy(8, 12));
        gp0(0xE3000000);
        gpu_render_display(out, &w, &h);
        CHECK(gpu_display_menu(), "frame with a menu background: shown as a menu");
        for (y = 400; y < 464; y++)
            for (x = 0; x < w; x++) {
                int t = 2 * x + 1 - 1280, p = 160 + (t >= 0 ? t / 6 : -((5 - t) / 6));   /* native column */
                badm += out[y * w + x] != argb15(px(((p % 32) + 32) % 32, y / 4));
            }
        CHECK(badm == 0, "menu background: %ld pixels not the tiles squeezed and repeated", badm);
        for (y = 76; y < 124; y++)
            for (x = 856; x < 880; x++)
                badg += (out[y * w + x] != argb15(px(232 + (x - 856) / 3, 19 + (y - 76) / 4))) +
                        (out[(y + 164) * w + x] != argb15(px(232 + (x - 856) / 3, 60 + (y - 76) / 4))) +
                        (out[(y + 604) * w + x] != argb15(px(232 + (x - 856) / 3, 170 + (y - 76) / 4)));
        CHECK(badg == 0, "menu: %ld pixels of the HUD, the untagged glyph and the HUD in a narrowed drawing "
              "area not squeezed about the centre", badg);
        CHECK(out[560 * w] == argb15(px(0, 140)) && out[591 * w + 1279] == argb15(px(319, 147)) &&
              (px(0, 140) & 0x3E0u), "menu: full-width rectangle not squeezed");
        gpu_vblank();
        hud_scene(0);
        CHECK(!gpu_display_menu(), "next frame without a menu background: no menu");
        g_sprite_ws = 1.0f;
    }

    /* the held object in widescreen: shifted by the player's offset and squeezed about the
     * player's anchor, so it stays over the player: target pixels 597..644 x 331..394 */
    {
        long badp = 0;
        g_sprite_ws = 0.75f;
        held_scene(1);
        gpu_render_display(out, &w, &h);
        for (y = 331; y < 395; y++)
            for (x = 597; x < 645; x++)
                badp += out[y * w + x] != argb15(px(145 + (x - 597) / 3, 83 + (y - 331) / 4));
        CHECK(badp == 0, "held object: %ld pixels not squeezed about the player's anchor", badp);
        held_scene(0);
        gpu_render_display(out, &w, &h);
        CHECK(out[332 * w + 580] == argb15(px(145, 83)) && out[395 * w + 643] == argb15(px(160, 98)),
              "other items: not tagged, drawn as they are");
        g_sprite_ws = 1.0f;
    }

    g_pgxp = g_sprite_tags = 0;
    gpu_set_upscale(1);
    free(out);
}

static void write_bmp(const char *path, const uint32_t *argb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    int stride = (w * 3 + 3) & ~3, y, x;
    uint32_t size = 54u + (uint32_t)(stride * h);
    unsigned char hdr[54];
    unsigned char *row;
    if (!f) {
        printf("cannot write %s\n", path);
        return;
    }
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (unsigned char)size; hdr[3] = (unsigned char)(size >> 8);
    hdr[4] = (unsigned char)(size >> 16); hdr[5] = (unsigned char)(size >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (unsigned char)w; hdr[19] = (unsigned char)(w >> 8);
    hdr[22] = (unsigned char)h; hdr[23] = (unsigned char)(h >> 8);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    row = (unsigned char *)calloc((size_t)stride, 1);
    for (y = h - 1; y >= 0; y--) {
        for (x = 0; x < w; x++) {
            uint32_t c = argb[y * w + x];
            row[x * 3 + 0] = (unsigned char)c;
            row[x * 3 + 1] = (unsigned char)(c >> 8);
            row[x * 3 + 2] = (unsigned char)(c >> 16);
        }
        fwrite(row, 1, (size_t)stride, f);
    }
    free(row);
    fclose(f);
}

/* ---------------------------------------------------------------------- */
/* Frame interpolation: the next frame redrawn with the camera halfway back */

/* A frame drawn into the buffer at VRAM row y0: a triangle at view space (tx, 0), (tx + 80, 0),
 * (tx, 80), depth 1000, projected with h 500 about (160, 120); the camera's translation is tx. */
static void interp_frame(int y0, int tx)
{
    static const int16_t rot[9] = { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 };
    static const int V[3][2] = { { 0, 0 }, { 80, 0 }, { 0, 80 } };
    int i;
    gpu_set_camera(rot, tx, 0, 1000, 500, 160 << 16, 120 << 16);
    gp0(0xE3000000 | ((uint32_t)y0 << 10));
    gp0(0xE4000000 | ((uint32_t)(y0 + 239) << 10) | 319u);
    gp0(0xE5000000 | ((uint32_t)y0 << 11));
    fill(0, y0, 320, 240, 0);
    gp0(0x20FFFFFF);
    for (i = 0; i < 3; i++) {
        int sx = 160 + (V[i][0] + tx) / 2, sy = 120 + V[i][1] / 2;
        gpu_pgxp_store(0x3000u + 4u * (uint32_t)i, xy(sx, sy), 1, (float)sx, (float)sy, 1000.0f);
        gp0_at(xy(sx, sy), 0x3000u + 4u * (uint32_t)i);
    }
}

/* One VBlank as the frontend runs it (workers on this thread): 1 = an in-between picture */
static int interp_present(uint32_t *out, int *w, int *h)
{
    int shown, k;
    gpu_vblank();
    shown = gpu_render_interp(out, w, h);
    if (!shown)
        gpu_render_display(out, w, h);
    if (gpu_interp_prepare())
        for (k = 0; k < INTERP_WORKERS; k++)
            gpu_interp_work(k);
    return shown;
}

static void test_interp(void)
{
    uint32_t *out = (uint32_t *)malloc((size_t)1024 * 512 * 4 * sizeof(uint32_t));
    int w, h;
#define LIT(x, y) ((out[(y) * 2 * w + (x) * 2] & 0xFFFFFFu) != 0)    /* native (x, y) in the 2x picture */

    screen_320();
    gpu_set_upscale(2);
    g_pgxp = 1;
    gpu_interp_enable();
    CHECK(g_interp, "interp: enabled");
    gp1(0x05000000 | (240u << 10));             /* both buffers shown once */
    interp_present(out, &w, &h);
    gp1(0x05000000);
    interp_present(out, &w, &h);
    interp_frame(240, 0);                       /* frame 1 into the bottom buffer */
    interp_present(out, &w, &h);
    interp_present(out, &w, &h);
    gp1(0x05000000 | (240u << 10));             /* frame 1 shown, frame 2 drawn: the camera moved by -40 */
    interp_frame(0, -40);
    CHECK(!interp_present(out, &w, &h) && LIT(165, 125) && !LIT(155, 125), "interp: frame 1 shown first");
    CHECK(interp_present(out, &w, &h), "interp: in-between picture on the second VBlank");
    CHECK(LIT(155, 125) && !LIT(145, 125) && LIT(180, 125) && !LIT(188, 125),
          "interp: the triangle halfway (x 150..190 instead of 160..200 and 140..180)");
    gp1(0x05000000);                            /* frame 2 shown, frame 3 drawn (camera at -80) */
    interp_frame(240, -80);
    CHECK(!interp_present(out, &w, &h) && LIT(145, 125) && !LIT(135, 125), "interp: frame 2 as drawn");
    gpu_interp_late();                          /* its in-between picture not ready in time: frame 2 again */
    CHECK(!interp_present(out, &w, &h) && LIT(145, 125) && !LIT(135, 125), "interp: late picture not shown");
#undef LIT
    g_interp = g_pgxp = 0;
    gpu_set_upscale(1);
    free(out);
}

/* ---------------------------------------------------------------------- */
/* Texture packs: the player's sprite quad (an 8x8 texture at (150,150), UV 0..7, mirrored: 7..0)
 * dumped, read back, and drawn with a replacement in the upscaled copy only */

static void player_quad(const uint16_t *tex, int mirrored)
{
    static const int Q[4][2] = { { 150, 150 }, { 158, 150 }, { 150, 158 }, { 158, 158 } };
    int k;
    screen_320();
    upload(640, 0, 8, 8, tex);
    gpu_sprite_anchor(154, 158, 0.0f, 0.0f, 1.0f, 0.0f, 1);  /* the player's anchor */
    gpu_sprite_begin(154, 158);
    for (k = 0; k < 4; k++)
        gpu_sprite_tag(0x5000u + 8u * (uint32_t)k, xy(Q[k][0], Q[k][1]));
    gp0(0x2D000000);
    for (k = 0; k < 4; k++) {
        uint32_t u = (uint32_t)((k & 1) ^ mirrored) * 7u, v = (uint32_t)(k >> 1) * 7u;
        gp0_at(xy(Q[k][0], Q[k][1]), 0x5000u + 8u * (uint32_t)k);
        gp0((k == 1 ? 0x010A0000u : 0u) | (v << 8) | u);       /* 15-bit page at x=640 */
    }
}

/* an untagged 8x8 textured rectangle at (100,100) */
static void rect_scene(const uint16_t *tex)
{
    screen_320();
    upload(640, 0, 8, 8, tex);
    gp0(0xE100010A);
    gp0(0x65000000); gp0(xy(100, 100)); gp0(0); gp0(xy(8, 8));
}

static void test_texpack(void)
{
    static uint16_t ref[1024 * 512];
    uint16_t t1[64], t2[64], rep[16 * 16];
    uint32_t *out = (uint32_t *)malloc((size_t)1024 * 512 * 16 * sizeof(uint32_t));
    const TexImage *im;
    uint64_t k1, k2;
    char path[64];
    FILE *f;
    int i, j, w, h, mirrored;

    gpu_vblank();
    for (i = 0; i < 64; i++) {
        t1[i] = (i == 5 ? 0x8000 : 0) | rgb15(i % 8 + 3, i / 8 + 3, 9);    /* with a semi-transparent texel */
        t2[i] = i == 9 ? 0x8000 : rgb15(20, i % 8 + 1, i / 8 + 1);   /* with opaque black */
    }
    for (i = 0; i < 256; i++)
        rep[i] = i == 0 ? 0 : rgb15(i % 16 + 1, i / 16 + 1, 25);     /* 2x, first pixel transparent */
    k1 = texpack_key(8, 8, t1);
    k2 = texpack_key(8, 8, t2);
    snprintf(path, sizeof path, "texpack_test/%016llx.png", (unsigned long long)k1);
    remove(path);
    snprintf(path, sizeof path, "texpack_test/%016llx.png", (unsigned long long)k2);
    remove(path);
    gpu_set_upscale(4);
    g_sprite_tags = 1;

    texpack_dump_to("texpack_test");                     /* drawing the quad dumps its texture */
    player_quad(t1, 0);
    snprintf(path, sizeof path, "texpack_test/%016llx.png", (unsigned long long)k1);
    f = fopen(path, "rb");
    CHECK(f != NULL, "texpack: %s dumped", path);
    if (f)
        fclose(f);
    texpack_dump(k2, 16, 16, rep);                       /* the replacement for t2 */
    CHECK(texpack_load("texpack_test") == 2 && g_texpack, "texpack: 2 images loaded");
    g_texdump = 0;                                       /* (the test's later textures aren't dumped) */
    im = texpack_find(k1);
    for (i = 0, j = 0; im && i < 64; i++)                /* (bit 15 isn't kept: it comes from the texture) */
        j += im->px[i] != (t1[i] & 0x7FFF);
    CHECK(im && im->w == 8 && im->h == 8 && !j, "texpack: PNG read back as dumped");
    im = texpack_find(k2);
    CHECK(im && im->w == 16 && im->h == 16 && !memcmp(im->px, rep, sizeof rep), "texpack: replacement read");

    for (mirrored = 0; mirrored < 2; mirrored++) {
        long bad = 0;
        g_texpack = 0;
        player_quad(t2, mirrored);
        memcpy(ref, gpu_vram(), sizeof ref);
        g_texpack = 1;
        player_quad(t2, mirrored);
        CHECK(!memcmp(ref, gpu_vram(), sizeof ref), "texpack: native VRAM unchanged (mirrored %d)", mirrored);
        gpu_render_display(out, &w, &h);
        for (j = 0; j < 32; j++)                         /* each replacement pixel is 2x2 target pixels */
            for (i = 0; i < 32; i++)
                bad += out[(600 + j) * w + 600 + i] != argb15(rep[(j / 2) * 16 + (mirrored ? 15 - i / 2 : i / 2)]);
        CHECK(bad == 0, "texpack: %ld upscaled pixels not the replacement's (mirrored %d)", bad, mirrored);
    }
    {                                                    /* the same texels in a rectangle */
        long bad = 0;
        g_texpack = 0;
        rect_scene(t2);
        memcpy(ref, gpu_vram(), sizeof ref);
        g_texpack = 1;
        rect_scene(t2);
        CHECK(!memcmp(ref, gpu_vram(), sizeof ref), "texpack: rectangle: native VRAM unchanged");
        gpu_render_display(out, &w, &h);
        for (j = 0; j < 32; j++)
            for (i = 0; i < 32; i++)
                bad += out[(400 + j) * w + 400 + i] != argb15(rep[(j / 2) * 16 + i / 2]);
        CHECK(bad == 0, "texpack: rectangle: %ld upscaled pixels not the replacement's", bad);
    }
    t1[63] = 0;                                          /* a texture without replacement: drawn as usual */
    player_quad(t1, 0);
    gpu_render_display(out, &w, &h);
    CHECK(out[601 * w + 605] == argb15(t1[1]) && out[631 * w + 631] == argb15(t1[54]), "texpack: other textures unchanged");

    g_texpack = g_texdump = g_sprite_tags = 0;
    gpu_set_upscale(1);
    free(out);
}

/* the frontend's (main.c) */
void log_msg(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void fatal(const char *fmt, ...) { (void)fmt; printf("FATAL\n"); exit(1); }

int main(int argc, char **argv)
{
    const char *vram_path = argc > 1 ? argv[1] : "gpu_test_vram.bmp";
    const char *disp_path = argc > 2 ? argv[2] : "gpu_test_display.bmp";
    uint32_t *img = (uint32_t *)malloc(1024 * 512 * sizeof(uint32_t));
    int x, y, w, h;

    gpu_init();
    CHECK(gpu_read_status() == 0x14802000u, "GPUSTAT after reset = %08X", gpu_read_status());
    gp1(0x00000000);
    CHECK(gpu_read_status() == 0x14802000u, "GPUSTAT after GP1(00h) = %08X", gpu_read_status());

    test_random_triangles();
    clear_vram();
    test_basic_triangle();
    test_gouraud_and_dither();
    test_textures();
    test_fill_copy();
    test_lines();
    test_blend_mask();
    test_transfers_and_status();
    test_state();

    /* some extra visual content: a big dithered gouraud triangle + semi-transparent overlay */
    env_default();
    gp0(0xE1000200);
    gp0(0x300000FF);
    gp0(xy(20, 120));
    gp0(0x0000FF00);
    gp0(xy(260, 140));
    gp0(0x00FF0000);
    gp0(xy(80, 280));
    gp0(0xE1000000);
    gp0(0x62FFFFFF);
    gp0(xy(60, 160));
    gp0(xy(120, 60));

    for (y = 0; y < 512; y++)
        for (x = 0; x < 1024; x++) {
            uint16_t p = px(x, y);
            uint32_t r = p & 31, g = (p >> 5) & 31, b = (p >> 10) & 31;
            img[y * 1024 + x] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) |
                                (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
        }
    write_bmp(vram_path, img, 1024, 512);

    test_display(img);
    gpu_render_display(img, &w, &h);
    write_bmp(disp_path, img, w, h);

    test_upscale();
    test_upscale_texels();
    g_pgxp_cracks = 1;
    test_pgxp();
    test_pgxp_seams();
    test_pgxp_sprite();
    test_interp();
    test_texpack();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    free(img);
    return g_fail ? 1 : 0;
}
