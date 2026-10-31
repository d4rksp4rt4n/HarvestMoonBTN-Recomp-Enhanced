# Graphics enhancements

What was added on top of the original-accurate renderer, how it works, and what was learned
about the game along the way. All of it is optional (see the `[video]` settings in the
[README](../README.md#configuration) or the launcher), and apart from widescreen none of it
changes what the game itself sees: with every other option on, RAM and VRAM stay
bit-identical to a plain run (checked at several frames of a recorded play session).

| Setting | What it does | Needs |
|---|---|---|
| `upscale = 2..4` | draws everything a second time at 2–4× resolution, for display | |
| `pgxp = 1` | 3D vertices at their exact sub-pixel position (no wobble), 2D objects placed exactly and scaled with depth like the ground | upscale 2+ |
| `crackfix = 1` | closes the cracks PGXP opens between models (on unless set to 0) | pgxp |
| `perspective = 1` | perspective-correct textures (no warping on large polygons) | pgxp |
| `widescreen = 16:9` | wider 3D view (also `16:10`, `21:9`) | upscale 2+ for 2D objects |
| `scaling = sharp-bilinear` | sharp pixels that stay even at any window size | |
| `texturepack = 1` | 2D sprites drawn with replacement images from `textures/` | upscale 2+ |
| `dumptextures = 1` | 2D sprites' textures written to `textures/dump/`, to draw a pack from | upscale 2+ |

The launcher (`launcher.pyw` in the build folder, needs Python) edits all of these.

## Higher internal resolution: two renderers in one

The PlayStation GPU draws into 1 MB of VRAM, and the game reads that VRAM back (textures
live there, and some effects copy screen areas). So the original-resolution VRAM must stay
exact. The GPU therefore draws every command twice:

- into the native 1024×512 VRAM, exactly as before, and
- into an n-times larger copy that is only ever displayed.

Polygons are rasterized again at the higher resolution (that's what makes 3D edges smooth);
sprites, lines, fills, copies and uploads are mirrored as n×n blocks. Textures are always
read from the native VRAM, so they look as they did.

**Finding: black lines along polygon edges.** At high resolution a pixel can lie very close
to a polygon's edge, and sampling the texture at the pixel centre (+0.5, as the native
rasterizer does) overshoots into the next texel, which is often unused black. The upscaled
pass samples at the pixel's coverage point plus a tiny guard (2⁻¹¹ texel) instead.

**Sharp-bilinear scaling.** 320×240 (or the upscaled picture) almost never fits the screen
by a whole number, and plain nearest-pixel scaling then makes some pixels wider than others,
which shimmers when the picture scrolls. The picture is first enlarged by the largest whole
factor (nearest), then scaled the rest of the way with bilinear filtering: crisp, even
pixels at any size.

## PGXP: precise vertex positions

The GTE (the geometry coprocessor) rounds every projected vertex to a whole pixel, which is
what makes PS1 polygons wobble. The runtime keeps the unrounded result next to the GTE's
screen-coordinate FIFO. When the recompiled code stores a screen coordinate (`swc2`), the
precise value is recorded in a shadow copy of RAM for that word, together with the stored
value. When a polygon arrives at the GPU (the DMA tells the GPU the RAM address of every
word), each vertex whose RAM word still holds the recorded value is drawn at the precise
position, and with its depth for perspective-correct texturing. If the game changed the
word in between, the shadow value no longer matches and the vertex is drawn as usual.

**Finding: black hairlines between models.** With PGXP, thin black lines flickered where models
meet: ground tiles in the town square, the ground at fence posts, bench legs and doorways, the
corners of sheds. Logging every polygon corner at the frames of a recorded session showed that
over 99.5% of the corners were drawn at their precise position (no positions lost on the way),
and that the corners on either side of a line are different 3D points, 8 to 30+ units apart:
the models are not closed (corners that miss each other, and T-junctions, where a corner sits
partway along another polygon's edge). Only 3 of 178 looked like fixed-point rounding. On the
console both sides round to the same or neighbouring pixels and the hole disappears; drawn where
they really are, the screen clear (a black fill) shows through.

**Fix: the console's drawing under the gaps.** Each pixel of the upscaled copy has a tag: what
drew it last (the screen clear, a 3D polygon or anything else) and when (a sequence number of
the drawing commands); see-through pixels leave it. Every 3D polygon is drawn a second time as
the console draws it, at its whole-pixel corners and at native resolution, each of its pixels
only onto the pixels of the n×n it covers that still show the screen clear, in the same order as
everything else: exactly what the console shows there. In the session, the
background showing where the console's picture is not black went from 7537 pixels to 512 (33
frames, upscale 4, 16:9); what is still black is black on the console too (a few seams leave
pixel gaps there as well), apart from pixels next to sprites moved by their sub-pixel anchors.
Pixels drawn by anything else are untouched (the same frames, compared before and after: no
pixel changed outside the background), so textures don't move and black art stays black. It
costs about 0.5 ms per VBlank at upscale 4 (14.8 s over the 27,200 VBlanks of the session).
In an in-between picture the console's corners are moved as far as the precise ones, rounded.
Rounding each moved position instead split corners that meet on the console onto different
pixels: on the farm, every other picture had cracks at door frames and wall corners, which
flickered at 60 Hz (in-between-only hairlines: 1500 pixels in 72-89 pictures, 159 now).
Filling hairlines of background with the native pixel when the picture is shown was tried
first: it needs a guess at what a hairline is, and wherever a sprite is moved or scaled in the
upscaled copy (sub-pixel anchors, depth scaling, the widescreen HUD) it would show a ghost of
its native position.

**Finding: door frames and nameplates let the ground through.** Where parts sit on another model
(a door frame in a house wall, the doghouse's nameplate, a window frame), the gap between their
precise edges shows what was drawn before them, the ground behind the house, not the screen
clear, so the second drawing doesn't reach it. In motion it flickered as a thin line of ground
colour. The console shows some of these gaps too, as single flickering pixels.

**Fix: seams.** When the picture is shown, a run of pixels across or down, narrower than a
console pixel, drawn before the 3D polygons on both sides of it (two different ones: a hole in a
single texture stays), is filled from its two sides. An outline has something drawn before it
on one side only, so object edges stay smooth; the test is about drawing order, not colour, so
dark texels and sprites are left alone. On the farm it fills about 1,900 pixels a frame (door,
window and doghouse frames, ground-tile seams) and changes nothing else. The band workers do it,
all runs across first, then all runs down from a copy of what the first pass left, so the
picture is the same with any number of them: about 0.4 ms per picture at upscale 4 with four
workers (0.9 ms on the game's thread alone).

## 2D objects on a 3D field

Stumps, rocks, weeds, flowers, villagers and the player are not 3D models. The game
projects each object's 3D position with the GTE and draws a flat sprite (a rectangle or a
few quads) at the resulting whole-pixel position.

**Finding: why field objects "hop" or warp.** With PGXP the 3D ground scrolls smoothly, but
the sprites standing on it still move in whole-pixel steps, so they drift against the ground
by up to a pixel and then jump back. The same happens in emulators (it was reproduced in
DuckStation with every option); it is how the game works, and at native resolution the
ground wobbles too, which hides it.

**Fix: sub-pixel anchors.** Hooks emitted by the recompiler (see
[TECHNICAL.md](TECHNICAL.md#the-recompiler-toolsrecomppy)):

| Address | Routine | Hook |
|---|---|---|
| 8001DFD4 | object projection | records each anchor: whole-pixel position → sub-pixel offset (and depth scale) |
| 80016280 | rectangle sprite builder | looks its anchor up on entry and tags every word it writes |
| 800165D8 | multi-part quad builder (villagers) | same |
| 80016C08 | single-quad builder (the player) | same |
| 800D93C8 | farm field object loop | big stumps and boulders are drawn at the anchor plus a y offset; that point is registered as an alias of the anchor (800D9590) |
| 80024A4C | HUD icons | deliberately not tagged (widescreen tags them as HUD) |
| 800D7144 | item builder | what the player holds over its head (called for it by 800C9F20, the player's held item routine, at the player's position plus an offset) is tagged with the player's anchor; an item put down or thrown (drawn through 800D6F78 by the jal at 800CC3FC, at a point that routine projected) with its own anchor; item icons in menus aren't tagged |

A sprite rectangle whose position word carries a tag, or a quad whose corners all carry the
same tag, is drawn in the upscaled copy shifted by the anchor's offset, rigidly, by whole
upscaled pixels, so the pixel art stays crisp. Untagged sprites and tags that no longer
match are drawn as before.

**Finding: objects seem to grow when you walk away.** The sprites have one fixed size (a
big stump is a single 56×47 rectangle, the boulder 60×42), and the PS1 cannot scale
rectangles, but the ground under them is in perspective. Measured on the farm, a real 3D
object would be about 71% of its mid-screen size at the top of the view and 122% at the
bottom. Walking away from a stump moves it up the screen, where the ground shrinks but the
stump doesn't, so it looks bigger. With the wobbly original ground this was hard to see.

**Fix: depth scaling** (on with `pgxp`; `--no-sprite-depth` on the command line turns it off). Each tagged sprite is also scaled
about its anchor by

    s = depth where the projection centre's ray meets the player's ground level
        ---------------------------------------------------------------------------
                                  depth of the anchor

The numerator comes from the player's anchor while the camera follows the player (the
projection writes each object's screen position through a0, and a0 = 80128B88 is the player's)
and `gte_ground_scale`, which computes
from the GTE's rotation matrix the depth where the centre ray meets the horizontal plane
through a point (the plane normal in view space is row 1 of the matrix's inverse, so a scaled
matrix doesn't matter). Sprites at the centre of the view on the player's level keep their
drawn size, and everything else follows the ground. Checked against the game: for the ~150
ground objects of a farm frame, the depth where the centre ray meets their level came out at
the same value (9866) for every one, and the player's level gives 9866 on the farm, in the
village and on the beach. Scaled sprites are sampled nearest-texel, so they stay sharp; their
pixels become slightly uneven.

**Finding: objects resized when the player climbed or jumped.** The first version used the
centre ray's depth on each object's *own* level. The camera follows the player at a fixed
distance, also up and down: climbing the ladder to the hot spring lifts the camera, so the
centre ray meets the ground below much further away, and a stump down there grew and shrank
as the player went up and down; every jump made all the ground objects grow by up to 5% and
back. Measured against the player's level, which the camera keeps at the same distance, an
object only changes size when its own distance does: in a recorded farm session the
reference stayed at 9866, within 0.6% (the camera lags a jump slightly), through jumps and
the stairs to the beach, and a boulder near a jump now shrinks smoothly as it moves up the
screen instead of pulsing.

**Finding: the player and villagers snapped to their size after area changes and in
cutscenes.** A second version took the smallest reference of the player's last 8 projections
(the player's object also projects its shadow, which lies below the player while it jumps),
and fell back to each object's own level when the player hadn't been projected for a few
frames. Arriving in town, the player is first projected away from the centre with a 4% smaller
reference, which the minimum kept for 8 projections before the size jumped back; in cutscenes
the camera doesn't follow the player, and the switch to the fallback made villagers jump in
size. The reference is the camera's distance to the player's level, so it is now measured only
while the camera follows the player (its anchor within 2 pixels of the projection centre,
which the shadow never is), smoothed (1/8 per projection), and otherwise kept: in cutscenes, at
the edges of a map and while changing areas, objects scale smoothly with their own distance,
like the 3D world. Before the first measurement, each object's own level is used.

## Widescreen

Unlike the other options, widescreen changes what the game sees, because the game has to draw
more of the world. The GTE scales the screen X of every perspective transform by
(4/3) / (W/H) about the projection centre (3/4 for 16:9), so a wider view fits the 320 pixels
the game draws, and the frontend shows the picture stretched to W:H. The game culls and sorts
with the squeezed coordinates, so it draws what a wider screen would show.

- **2D objects keep their shape.** People, stumps and rocks are flat sprites of a fixed size
  at a projected point (see above): the squeeze moves their anchor but not their width, so
  stretched to 16:9 they would look a third too wide. In the upscaled copy every tagged
  sprite is squeezed by the same factor about its anchor (nearest texel, like depth scaling;
  at upscale 4 and 16:9 a texel is exactly 3 pixels wide). This needs upscale 2 or more but
  not PGXP: without PGXP the anchors are recorded with no sub-pixel offset.
- **Screens without 3D stay 4:3,** except the title screen and the full-screen menus (below).
  Videos, the map and "Now Loading" are 2D pictures. The picture is shown wide while the game projected at least 32 vertices in one of the last 10
  frames, and never for 24-bit video. "Any projection" is not enough: with the full-screen
  Start menu open the game still projects one vertex every other frame (against ~2,600 for
  the house), and the menu came out stretched.
- **Area changes stay wide.** Leaving an area, the game stops drawing 3D and fades out the
  last picture, then shows black or "Now Loading" (97% black). Switching to 4:3 as soon as the
  3D stopped squeezed the fading room for a moment. Once wide, the picture now stays wide
  until a picture without 3D that is mostly not black follows a black one (at least 90% of
  the pixels with red, green and blue at 0 or 1): the Start menu is less than 13% black from
  its first frame after the fade. In the new-game intro and the village tour the only
  switches left are for the Start menu, on its first frame and on the black frame after it.
- **Full-screen menus and the title screen fill the wide picture** (upscale 2+), from their
  first frame. Every full-screen menu (toolbox, Start menu, memory card screens) draws the same
  scrolling background of 24 × 24 animal tiles: 800223D4 tiles the screen (x −12…344, 15 tiles a
  row) through a callback that builds each tile with the sprite builder, so the tiles are tagged
  while it calls it (jalr at 80022498). The four animals take turns and each row starts one
  later, so the pattern repeats every 96 pixels across (diagonal stripes, scrolling diagonally
  one pixel every fourth frame). The title screen's background (800B944C in GF_TITLE) is a
  320 × 240 meadow drawn whole at 0,0 and twice more as see-through 3 × 3 grids of itself that
  scroll, so it repeats every 320 pixels; it is tagged the same way. The Natsume logo before it
  (800B9324) is one sprite over a white screen clear, which covers the wide picture as it is: the
  logo is tagged with no background to repeat, so the screen is wide and the logo keeps its 4:3
  shape (the black after it stays wide too, so the intro never switches). In the upscaled copy each
  background tile is squeezed about the middle, like the HUD, and drawn again a period, two
  periods, … to the left and right to fill the sides; the drawing area shows the tile where it
  is, the period left of it only the copy one period left, and so on, so every pixel gets
  exactly one copy (the title's see-through grids aren't blended twice) and the sides scroll
  with the middle without seams. Everything drawn after the background (windows, text,
  cursors, icons, selection boxes, the title's logo, flowers and animals) keeps its 4:3 shape in
  the middle, like the HUD (a full-width rectangle, such as a fade, stays full width). The
  native VRAM, and so the game, is unchanged. The game builds a frame while the previous one is
  shown, so the GPU notes, for each drawing buffer (top or bottom of VRAM), whether the frame
  last drawn there had such a background, and the picture is wide when its buffer holds one:
  the toolbox, which opens straight from the room, stays wide throughout. A picture without
  3D after a menu (black, "Now Loading") is 4:3 as before. Before, these screens were shown at
  4:3.
- **The HUD keeps its 4:3 shape, centred.** The clock, dialogue boxes, portraits and text over
  the 3D view are squeezed back by the same factor about the middle of the picture (the drawing
  offset plus half the display's width; not the middle of the drawing area, which the toolbox
  and rucksack narrow to their help box to clip its text: it came out 5 pixels right), in the
  upscaled copy (upscale 2+), so they keep their size and sit in the middle 4:3 area of the
  wide picture. They are found by the routines that build them, traced by recording which
  function wrote each GPU command in a frame with the clock and one with a dialogue: window
  background (800153BC, a gouraud quad), border lines (8001530C, `LINE_F2`), corner pixels
  (8001526C), one text glyph (80019070) and portraits (80019E80) tag everything they write;
  the clock's icon comes from the shared sprite builder, which is tagged while the HUD icon
  routine (80024A4C) calls it (jal at 80024AB8). The full-screen colour tint of the time of
  day (8001316C) is not HUD and stays full width. The same routines draw the full-screen
  menus, which are shown at 4:3, so the HUD is squeezed only while the game is in a 3D scene,
  by the same test that shows the picture wide (32 projections since the last frame, or in
  one of the 10 before), asked when the GPU draws the HUD. Asked when the HUD was built, the
  first frame after every black screen (changing areas, closing a menu) showed the clock
  stretched: the game builds its HUD before it projects the frame's 3D scene. Menus, the Start
  menu's fade in and out, an area change and the way back into 3D were checked frame by frame.
  Without upscaling the HUD stays stretched.

  Some HUD comes from the shared sprite routines that also draw the world: the arrow at the
  end of a dialogue page (the dialogue system, 800225C4, through 80015C50) and the item slots
  of the bag (the field code's bag menu, 800FAB18). Every traced frame draws the world first
  and the HUD last, as one block, so once a HUD-tagged primitive was drawn in a frame, untagged
  rectangles and lines after it are HUD too, except ones covering the whole width (the bag's
  full-screen dimming). So are flat 2D polygons (no projected vertex): the rucksack's selection
  box, shown once an item is picked, is four textured quads. The rule starts over at every
  VBlank and never applies to projected 3D polygons.
- **What the player holds keeps its shape.** An object picked up and held over the head is a
  sprite drawn at the player's position plus an offset, which nothing tagged, so it came out a
  third too wide. It now takes the player's anchor (sub-pixel offset, depth scale and the squeeze
  about the player), like the player's own sprite, so it stays in shape and centred over the
  player, and with PGXP it moves smoothly with the player instead of in whole pixels. Put down
  or thrown, the item is drawn by another routine at a point it projects, so it takes that
  point's anchor, like the objects lying in the field.
- The PGXP positions and the depth scale use the squeezed coordinates (the ground scale
  undoes the squeeze to get the view ray).

**Finding: saw-tooth gaps at the sides.** With only the squeeze, black wedges appeared at the
left and right edges of the farm (small at 16:9, a saw-tooth down both sides at 21:9). The
model renderer (8001C0D4) skips each part of a model, such as a piece of the ground, unless
its centre (a single point) passes the test at 8001C1C0: the direction from the camera, against
four planes at 80061EA8. Those planes are rebuilt every frame (8001D348) by turning the camera
matrix by four angles at 800491E0. On the farm they are 2156 and −108 (180° + 9.5° and −9.5°,
a little wider than the 4:3 view's 9.1°) for the sides, and 93 and 1962 for top and bottom.
A hook on the two loads of the side angles (8001D45C, 8001D4A4) widens them so that their
tangent grows like the view (−108 → −143 for 16:9). The polygons' own screen test (8001D21C)
is loose (x −20…398) and works on squeezed coordinates, so it needs nothing.

**Finding: field objects popping in at the top corners.** On the farm, weeds, rocks and
branches in the top corners of the wider view appeared and disappeared as the view moved. The
farm's field object loop (800D93C8) draws only the tiles within 3600 units of the view's centre
(a 19 × 19 tile square from 800D92FC, enough for 4:3). A hook where that returns (800D9414)
widens the square like the view: 3 tiles more on each side at 16:9, 7 at 21:9.

What remains black at the sides is the edge of the map (past the town wall, beyond the
ground at the farm exit), which a 4:3 camera never shows; the black areas stay the same with
the side planes opened all the way. The game draws more, but it doesn't slow down: it flips
the same number of frames (30 per second) at 4:3, 16:9 and 21:9 on the farm and through the
village tour.

## Frame interpolation (in-between pictures)

The game builds a new frame every other VBlank (30 a second) and shows each one twice. With
`interp = 1` the second VBlank shows a picture halfway to the next frame, so the camera and
the 3D scenery move every VBlank.

- **The next frame is already drawn by then.** The game flips to the frame it drew last and
  sends the next frame's drawing commands right away (DrawOTag after VSync), so on the second
  VBlank of a frame the next one is complete in the other buffer. The in-between picture is
  that next frame redrawn with the camera half a step back: no extra latency, and the real
  frames are shown exactly as before.
- **What is recorded.** For each drawing buffer, the commands of the last frame drawn into it,
  with each word's RAM address and PGXP word, the GPU state when it began, and its camera.
  The camera is the GTE matrix and translation used to project the player's anchor (8001E02C):
  that is the world-to-view transform, whose translation changes as the camera follows the
  player.
- **The redraw.** Every precise vertex (screen x, y and depth) goes back to view space, through
  P' = Rm Rc^-1 (P - Tc) + Tm with the camera halfway between the two frames, and is projected
  again. Sprites at projected points move with their anchor the same way; the player's own
  sprites (the camera follows the player) move by half of the player's step on screen.
  People, animals and animations are still drawn as in the next frame, so they move at 30
  pictures a second.
- **Not for every VBlank.** Menus, videos, camera cuts (rotation, a big jump, a different
  projection) and frames the game took longer than two VBlanks for get no in-between picture.
  When a frame stays up a third or fourth VBlank (the game slowing down), the in-between
  picture stays too, rather than stepping back.
- **Three worker threads.** Redrawing a frame costs about as much as drawing it (about 15 ms
  at upscale 4 on the farm), so three threads draw a third of the picture each while the game
  goes on. Each has a GPU of its own: `gpu_replay*.c` compile `gpu.c` again with the public
  names renamed, and keep their variables in sections of their own, aligned to whole cache
  lines (a worker's sprite offsets had ended up in the same cache line as the GTE registers).
  The game never waits for them: a picture that is not ready in time is replaced by the real
  frame.
- **Where the threads run.** On a hybrid CPU with the Balanced power plan, Windows parked all
  performance cores but one, so the game's thread and the workers took turns on one or two
  cores and every in-between picture cost the game the time of drawing it (slowdowns by the
  waterfall). Each worker now has a performance core of its own and the game's thread the
  others (`cpu_place.c`, hard affinity, which Windows honours on parked cores by waking them).
- **Pacing.** The frontend waits until a VBlank is due before it renders and shows that
  VBlank's picture (it used to show it first and wait afterwards). The pictures then come at
  even intervals however the game's work is spread over its two VBlanks (drawing a frame can
  take longer than one), and an in-between picture has a whole VBlank to be drawn.

Measured with replays paced in real time (the waterfall west of the farm, the heaviest place
found): the game keeps its full speed with `interp` on, and the in-between pictures are ready
in time at upscale 2 and 3. At upscale 4 by the waterfall the game's own drawing already takes
more than a frame's time, so there it shows 30 pictures a second.

## Texture packs

A texture pack is a folder of PNG images, `textures/` next to the game, that the upscaled picture
uses in place of the game's own texels. It covers the 2D sprites: the player, villagers, animals and
items (quads built at a projected point), and every textured rectangle (stumps, rocks, weeds,
branches, water drops, the clock, text, icons, menus, the title screen). The 3D world (ground,
buildings, trees) is not covered. The native VRAM is drawn as before (the game reads it back and
samples it), so the game sees no difference: with a pack the RAM and VRAM stayed identical to a run
without one, and with no pack the pictures are the same as without the feature.

**Finding: the player's frames move around VRAM.** The player is one textured quad (`POLY_FT4`,
8-bit, opaque, not tinted) built by the single-quad builder (80016C08) and tagged with the player's
anchor. The game uploads the current animation frame for every frame it draws, into one of two
slots of the texture page at 960,256 (x 960 or 992, one per drawing buffer), with its palette in
row 485 or 486. The quad's UVs span the uploaded image, last texel included (`0..35 × 0..44` for a
36×45 frame); facing left swaps the U of the corners. The VRAM address and the palette row change
every frame, so a frame can only be recognised by what it looks like. Field objects are plain
rectangles from texture pages loaded with the area (rocks 32×26, weeds 32×24, branches 24×23 at
704,0 on the farm), text is one 8×12 rectangle per glyph.

**The key.** The GPU reads the texels a sprite is drawn from through its palette and hashes their
size and the 16-bit texels (FNV-1a, 64 bits):

- a quad tagged with a sprite anchor (sprite quads at projected points; recorded whenever upscaling
  and PGXP, widescreen or a pack is on): its UV box, smallest to largest UV, both included;
- a textured rectangle: its `w × h` texels from its UV (not flipped ones, see the limits).

The same picture gets the same key wherever it is in VRAM, with whichever palette row holds its
colours, and mirrored or not. Every rock shares one key. A replacement is `textures/<key>.png`, e.g.
`textures/74c3cd670f0ebe99.png`. The recorded session (title screen, house, farm) used 120 different
textures: 33 frames of the player, 43 glyphs, the farm's objects, tools, icons, menu tiles and the
title screen's pictures.

**Making a pack.**

1. Play with `dumptextures = 1` (or `--dumptextures`). Each texture not seen before is written once
   to `textures/dump/<key>.png`: an RGBA image at its original size, transparent where the texel is
   0. Files already there are not written again.
2. Draw the images anew at any size, keeping the shape (4× the dump is exactly one image pixel per
   screen pixel at `upscale = 4`). Pixels with alpha below 128 are transparent. Save as 8-bit RGB or
   RGBA PNG in `textures/`, under the dump's name. Leave out what you don't redraw.
3. Play with `texturepack = 1`. The images are read when the game starts; the log says how many.

`tools/texpack_sample.py` makes a sample pack from dumps: each image enlarged 4× with Scale2x applied
twice (smooth diagonals instead of 4×4 blocks). It is made from your own dump, so no game art is
in the repository: `python tools/texpack_sample.py dist/HarvestMoonBTN-Windows-x64/textures/dump/*.png`.

**How it's drawn.** When the key has an image, the upscaled copy of the sprite is drawn as before,
with the same corners (sub-pixel anchor, depth scale, widescreen squeeze, the HUD's squeeze), colour
and blending; only the texel each pixel takes comes from the image, nearest: for a quad the UV at the
pixel's centre mapped from the UV box onto the image (a mirrored quad shows the image mirrored), for
a rectangle the position in it. The UV box is stretched over the whole image: the original 36-texel
frame, drawn at UV 0..35 on a 36-pixel quad, shows 35 texels with one of them twice, while its
replacement shows every one of its columns.

- The drawing threads don't draw a replaced quad: the GPU waits for the polygons queued before it
  and draws it itself (the pictures came out identical with 0 and 4 threads). Rectangles are always
  drawn by the GPU.
- The in-between pictures redraw the recorded frame with the same keys, so they show the
  replacements too.
- A replaced rectangle's upscaled copy is drawn like a depth-scaled one (each target pixel takes the
  image pixel under its centre). That drawing now works out each column's texel once per rectangle
  instead of for every pixel (the same arithmetic, so the same pictures), which halved its cost.

**Finding: almost every texel is "semi-transparent".** The game sets the semi-transparency bit
(bit 15) on nearly every texel of its sprites; it only matters for the primitives it draws
semi-transparent, like the shadows. A dump that kept the bit (as alpha) would look half see-through
in every image editor. So the images carry colour and transparency only, and a replaced pixel takes
bit 15 from the texture's own texel under it: the shadows stay see-through, with or without a pack.

**What it costs.** Measured in the recorded session at upscale 4 with every sprite replaced: the
hashing takes about 0.05 ms per frame on the farm and 2 ms on the title screen (its pictures are two
full-screen rectangles, hashed again every frame); drawing the replaced sprites about 0.6–0.9 ms per
frame on the farm and 14 ms on the title screen.

**Limits.**

- Only 2D sprites: the 3D world's textures come as one big upload per area (128×255 halfwords at
  320,0 and 576,0 for the house and the farm), which every 3D polygon samples a piece of. Those need
  a key of their own (the uploaded image and its palette) and a high-resolution copy every polygon
  reading from it samples.
- Quads count as sprites only when tagged with an anchor, so 2D quads drawn without one (the
  rucksack's selection box) aren't replaced.
- Flipped rectangles (`GP0(E1h)` bits 12/13) read from another box and are drawn as usual, and so is
  anything drawn with a texture window (`GP0(E2h)`).
- Upscaled picture only (upscale 2+), nearest sampling; widescreen squeezes a 4× image onto 3 pixels per
  texel, so columns of it are dropped.
- Replacement colours are 15-bit, like the VRAM; opaque black is drawn as the darkest grey (0 would be
  transparent). Where an image is opaque over a transparent texel (a smoother edge), that pixel is
  never blended.
- The same picture with another palette is another key (a recoloured sprite needs its own image).
- All images are read at start and kept in memory.

## Testing method

- `--record FILE` saves the pad input of a play session; `--input @FILE` replays it
  deterministically (`--headless --unthrottled --shots N --shotstart F --dumpram F` for
  screenshots and RAM/VRAM dumps). The recording starts with `fastcd=` because faster
  loading changes timing: an early wrong diagnosis came from replaying a recording made
  with `fastcd = 1` without it, so the replay went somewhere else entirely.
- Every option was checked for game-state identity (RAM + VRAM dumps compared with a plain
  run) and with unit tests in `runtime/tests/` (upscaled texel sampling, PGXP vertices,
  perspective, sprite shifting and depth scaling, the ground-scale formula, the widescreen
  projection, sprite squeeze and frustum angles, texture pack PNGs, a replaced quad and
  rectangle). Widescreen changes the game state on purpose; with it off the game state is
  unchanged.
- Interpolation only changes what is shown. The in-between pictures were compared frame by
  frame across 1, 2 and 3 worker bands (identical) and checked against the frames before and
  after (the scenery moves halfway); speed was measured with replays paced in real time from a
  given frame, since an unthrottled replay never leaves the game idle and hides when pictures
  are due.
