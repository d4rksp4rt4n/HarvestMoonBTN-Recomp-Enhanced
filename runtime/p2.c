/* A second farmer (prototype): a second copy of the player, moved and drawn by the game's own
 * player routines.
 *
 * The player is the struct at 80071A00 (0x3FC bytes; the next object starts at 80071DFC), and the
 * farm/town code works on it by that address. Farmer 2 keeps a struct of its own outside the
 * game's RAM, and each game tick, once the player has had its turn, it is swapped in for the
 * player's and the player's routines are run once more with farmer 2's buttons:
 *   - 800C401C, the control (reads the pad, starts and runs actions: tools, ...);
 *   - 800CAADC, the walk (look-ahead, collision, sliding), when the player's state routine
 *     (800CD9A0) would let the player walk. That routine itself is not run: it is the one that
 *     takes the player through doors and to other areas.
 * Then farmer 2's struct is copied out and the player's put back, with the buttons and the few
 * globals the player's turn leaves for the rest of the tick. The entry hook of 800DB5CC, which
 * the farm's tick calls right after the player's turn, runs p2_tick.
 *
 * Farmer 2's buttons that reach the game: the directions, Square (use the tool), Circle (run),
 * Cross (look, talk, open), R1 (change tools), R2 (put away / change the item), L1 / L2 (whistle:
 * 800C9424 tests them, game bits 4 / 1), Triangle (the rucksack), Start (the farm's page) and
 * Select (the map): all of them. When farmer 2's turn opens a menu or a talk (8006C844 gets
 * 08000000: the tool chest, a person, those three), farmer 2 has the game until it closes: the
 * farm's whole tick (800C162C, p2_tick_begin / p2_tick_end) runs with farmer 2's struct, globals and
 * pad in place of the player's (and the player's drawn as the second farmer), with all farmer 2's
 * buttons.
 *
 * There is one of each tool: farmer 2 starts with nothing and takes tools from the tool chest
 * (80070EC2) with that menu. The game saves only what the player carries, so the save and load
 * screens put farmer 2's tools back in the chest.
 *
 * The farm/town code draws the player with 800CA24C (a0 = the player's position, x / y / z words
 * at 80071B00, from 800C17D8). Its entry hook calls p2_draw first: the routine is run once more
 * with farmer 2's struct swapped in, then the player's is put back and the game draws the player
 * as usual (so the projection it leaves at 80128B88 is the player's). */
#include "psx.h"
#include "psx_recomp.h"

int g_p2_enabled;               /* --p2 */
int g_p2_demo;                  /* --p2demo: farmer 2 walks around by itself (testing) */
uint16_t g_p2_buttons;          /* farmer 2's pad, set by the frontend each frame */

#define PLAYER 0x80071A00u
#define PLAYER_SIZE 0x3FC
#define POS (PLAYER + 0x100)    /* x / y / z words */
#define FLAGS (PLAYER + 0x6C)
#define PLAYER_DRAW 0x800CA24Cu
#define CONTROL 0x800C401Cu
#define WALK 0x800CAADCu
#define IDLE 0x800CA6C4u        /* counts the minutes standing still (+0x1A; the control lies down to rest) */
#define PAD 0x8005E3A8u         /* -> the pad state (port 1) */
#define BUFFER 0x8005E3A4u      /* which of the two screens is being built (0 / 1) */
#define SLOTS 0x80122F9Cu       /* per screen: VRAM places of the player's pictures */
/* The player's sprite builder (80016C08) unpacks each picture of a frame (the body, the tool in
 * hand) and loads it into VRAM at a place of its screen's list (15 places of 8 bytes: where the
 * palette goes at +0 / +2, the pixels at +4 / +6; the pixels at 960-1023 x 256-415, several tools
 * sharing a place; the palettes on rows 485-486 (the body's 256 colours, a palette per frame) and
 * 487-488 (the tools' 16)). The GPU draws a screen while the next is built, so farmer 2 needs
 * places of its own, which nothing else loads, copies to or draws from (logged at the GPU over
 * the farm, town, house, talks and menus): the pixels 256 rows up (960-1023 x 0-159; the letters'
 * page and palette are just left of it, at 896 and on row 252), the palettes 10 rows down
 * (495-498). */
#define P2_SLOTS 15
#define P2_PIXELS_DX 0
#define P2_PIXELS_DY (-256)
#define P2_PALETTE_DY 10
#define START_DX 250            /* where farmer 2 appears: beside the player */
#define GROUND 0x800BDB88u
#define GROUND_STAND 0xC080u    /* the kinds of ground the player cannot stand on */
#define BUTTONS (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_SQUARE | PAD_CIRCLE | PAD_CROSS | PAD_R1 | PAD_TRIANGLE | \
                 PAD_START | PAD_SELECT | PAD_R2 | PAD_L1 | PAD_L2)

/* what the player carries (6 bytes each): the tool in hand at +084, the item in hand at +08A,
 * 8 tools in the rucksack from +090, 8 items from +0C0 */
#define CARRIED 0x84
#define CARRIED_SIZE 0x6C
/* the stamina (halfwords): what is left at +012, the most at +014, the tiredness at +016 */
#define STAMINA 0x12
#define STAMINA_MAX 0x14
#define TIRED 0x16
/* tiredness 100: the player's state routine (800CD734) makes the player fall (+036 bit 0200: fallen)
 * and faints (800DE3A0: home, the next day). Farmer 2 does not faint (p2_faint_skipped), and with
 * tiredness 100 the game takes no button of the player at all (800C76EC answers "busy" for every
 * button, the walk too): farmer 2 stood still for ever. Fallen farmer 2 is kept at 99 instead, so
 * that it walks, and is "fainted" until its tiredness goes below that (the medicine from the clinic,
 * or the morning after the player slept: see place): no tools and no running. */
#define STATE_MARKS 0x36
#define FALLEN 0x0200
/* +036 also marks the tired animations played (0200-0800 by tiredness, 1000-8000 by stamina left),
 * so that each plays once: farmer 2 keeps its own, or it played them again in each new area */
#define TIRED_MARKS 0xFE00
/* the money (a word at +05C, saved with the player; +048 is something else) is the two farmers':
 * whoever's struct is in the player's place has the latest, and the other's copy takes it at each
 * swap */
#define MONEY 0x5C
#define FAINTED_TIRED 99
#define AREA 0x800A036Au         /* the area (halfword: 2 the farm, 1 the house by day; not always another number for another place) */
#define DAY 0x80070CF4u          /* the date: the day (from 0), then the weekday at +2, the hour at +4 */
#define HOUR 0x80070CF8u
#define WAKING_HOUR 6
#define TOOL_CHEST 0x80070EC2u  /* the tool chest: 64 places (8 pages of 8), 6 bytes each as carried */
#define TOOL_CHEST_PLACES 64
#define MENU_FLAGS 0x8006C844u  /* 08000000: a menu or talk has the game (the control skips the player) */
#define MENU_OPEN 0x08000000u
/* +06C 00800000: an event leads the player (a talk, the shipping bin): the control takes the
 * buttons the event left at +064 instead of the pad's. Farmer 2's X starts the event in its turn,
 * but the event goes on in the tick, with the player's struct: it ended there and farmer 2 was led
 * for ever (until placed again). So farmer 2 has the game while it is led, as in its menus. */
#define LED 0x00800000u
/* +06C 00008000: something held up in the arms (800C9F20 draws it), 18000000: an item (else the
 * tool at +0F0) */
#define HOLDING 0x18008000u

/* the globals the player's turn keeps for itself from tick to tick (found by what farmer 2's turn
 * changed): farmer 2 has its own copy of them too */
static const struct { uint32_t at, size; } GLOBALS[] = {
    { 0x80123150u, 0x0C },      /* counters (80123158: the look-ahead's) */
    { 0x801231F4u, 8 },         /* the buttons */
    { 0x80128B60u, 4 },         /* the state routine's answer */
    { 0x80128C4Cu, 0x28 },      /* what the look-ahead and the actions found */
    { 0x80128CB0u, 0x1C },      /* the actions' timers */
    { 0x80128CF8u, 4 },
    { 0x80128B90u, 8 },         /* left by the control for the player's projection */
    { 0x80128D30u, 4 },         /* how long a direction has been held (walking starts after it) */
};
#define GLOBALS_SIZE 0x80       /* at least the sizes above added up */
/* the game's register of what is carried: 9 tools (halfword ids from 800A0494) and 9 items (from
 * 800A04A6), each with a record (30 bytes from 800A04B8) and its pictures loaded, found by the id:
 * the tool in hand and the rucksack window (Triangle) are drawn from it. 800D6A94 registers an id
 * (and loads its pictures), 800D666C takes it out, 800D5750 says its kind (0 tool, else item). The
 * game keeps it as the player's: the menus take out what the player no longer carries (800F9BE4),
 * so farmer 2's tool chest took out the player's tools (left out of the hand and the rucksack
 * window). It holds what both farmers carry: after a menu, what is missing goes back in (the
 * player's first, making room if needed with what the player does not carry). */
#define REGISTRY 0x800A0494u
#define REGISTRY_PLACES 9       /* of each kind */
#define REGISTER 0x800D6A94u
#define UNREGISTER 0x800D666Cu
#define KIND 0x800D5750u
/* the pad state: +00 newly pressed, +04 held, +08 pressed with repeats (after +18 ticks held,
 * every +1C), +0C held, +10 ticks since the buttons last changed, +24 / +28 as +00 / +10 */
#define PAD_SIZE 0x38
/* the view: the walk (800CAADC) aims it at the player (800A03A4, x / y / z halfwords) and
 * centres it (800A0398 / 039A: 160 - screen x, 120 - screen y). It follows the player only: farmer
 * 2's walk would turn it to farmer 2 for as long as farmer 2 walks. */
#define VIEW 0x800A0384u
#define VIEW_SIZE 0x2C
#define INPUT_LOG 0x8006C854u   /* -> the control's input log: a count at +0 that each turn moves on */
#define INPUT_LOG_SIZE 8
/* the place of the player's extra animations: some actions load a file of them there (from the
 * CD, 8007BFFC: 0000000E, then the table) and the player's animation (+25C, bound by 80015704)
 * points into it. There is one such place: when farmer 2's action loaded its file and then the
 * player's loaded another, farmer 2's animation pointed into the player's file, and drawing farmer
 * 2 looked for a picture that was not there for ever (80015B24). Farmer 2 has a copy of its own. */
#define ANIMS 0x8007BFF8u
#define ANIMS_SIZE 0x3800       /* up to 8007F7F8, where other things are kept */
/* what is thrown (an item on the ground, in the shipping bin): 3 places of 52 bytes from 80128BB0 (+2:
 * what flies, 0 = free), moved by 800CC0F8 through the farm's tick until it lands. The landing
 * (800CCBE8) ends the throw on the player's struct (+06C 00000040: until then the thrower cannot
 * move) and gives the item away from it. What farmer 2 threw landed on the player's struct, and
 * farmer 2 was left throwing for ever: it is moved with farmer 2's struct in place. */
#define THROWN 0x80128BB0u
#define THROWN_SIZE 52
#define THROWN_PLACES 3
#define THROWN_MOVE 0x800CC0F8u
/* trading: X on the other farmer (in front, near) opens the game's storage menu (the tool chest's,
 * then the cabinet's for the items) with the other farmer's things in the storage's first places,
 * as many as the other farmer has: the tool in hand and the rucksack's places (2 << +01E: 2, 4, 8).
 * The real storage is kept aside and put back when the menu closes, and what is then in those
 * places is the other farmer's. The menu (state 80126E40: 3 the tool chest's, 4 the cabinet's,
 * 800F6778 opens the cabinet's without a lid; then 0 / 1 while it is up; 800F7D34 moves between two places, a slot and a
 * page each: 0 the hand, 1-8 the rucksack, 9-16 the storage's page) takes nothing into the other
 * places, and the cabinet takes the fridge's items too (800D57A4: the storage of an item, 1 the cabinet). */
#define FACING 0x38             /* halfword: 0 down (z less), 1 left, 2 up, 3 right */
#define RUCKSACK 0x1E           /* the rucksack's size: 2 << it places */
#define REACH 200               /* how far ahead the other farmer may be ... */
#define REACH_SIDE 120          /* ... and aside */
#define MENU_STATE 0x80126E40u
#define MENU_RESET 0x800F6680u
#define MENU_SOUND 0x8001FABCu
#define MENU_TEXT 0x8002261Cu
#define TOOLS_TEXT 888
#define ITEMS_TEXT 890
#define STORE_TOOLS 0x80070EC2u
#define STORE_ITEMS 0x80071042u /* the cabinet */
#define STORE_FRIDGE 0x80070D42u
#define STORE_PLACES 64
#define ITEM_TABLE 0x8006C858u  /* -> 40 bytes an item: flags at +18 (10000000 tool, 20000000 cabinet, 40000000 fridge) */

static uint8_t me[PLAYER_SIZE], saved[PLAYER_SIZE];
static uint8_t my_globals[GLOBALS_SIZE], saved_globals[GLOBALS_SIZE], my_pad[PAD_SIZE], saved_pad[PAD_SIZE];
static uint8_t my_log[INPUT_LOG_SIZE], saved_log[INPUT_LOG_SIZE], saved_view[VIEW_SIZE];
static uint8_t my_anims[ANIMS_SIZE], saved_anims[ANIMS_SIZE];
#define ANIMS_RAM (g_ram + (ANIMS & 0x1FFFFF))

/* copies the globals out to "out", and "in" (if any) in */
static void swap_globals(uint8_t *out, const uint8_t *in) {
    uint32_t k = 0;
    for (size_t g = 0; g < sizeof GLOBALS / sizeof *GLOBALS; g++)
        for (uint32_t i = 0; i < GLOBALS[g].size; i++, k++) {
            out[k] = (uint8_t)MEM_R8(GLOBALS[g].at + i);
            if (in) MEM_W8(GLOBALS[g].at + i, in[k]);
        }
}

/* copies n bytes at "at" out to "out", and "in" (if any) in */
static void swap_at(uint32_t at, uint32_t n, uint8_t *out, const uint8_t *in) {
    for (uint32_t i = 0; i < n; i++) {
        out[i] = (uint8_t)MEM_R8(at + i);
        if (in) MEM_W8(at + i, in[i]);
    }
}

static int placed, inside, ticks;
static int owner, exchanged;    /* farmer 2 opened the menu that has the game / its turn is the tick's */
static int thrown_by_p2, flying_before; /* places (bits) farmer 2 threw / flying when its tick began */
static int landing;
static int asked;               /* farmer 2's turn pressed X (and the player did not) */
static int p1_led;              /* the player was led when farmer 2's turn began */
static uint32_t idle_clock;     /* the clock farmer 2's minutes standing still were last counted at */
static int loaded;              /* the farm's tick was not run for a while: a new area */
static int32_t seen_x, seen_z;  /* where the player was at the last tick */
static uint64_t last_tick;
static uint16_t buttons;
static uint16_t rested_day;     /* the day farmer 2 last woke up rested */
static uint16_t area;           /* where farmer 2 was placed */
static int fainted;
static int trade_who;           /* the farmer whose trade menu is up (1 the player, 2 farmer 2), 0 none */
static int trade_kind;          /* 0 the tools, 1 the items */
static int trade_next;          /* the farmer whose items' menu opens next */
static int trade_places_n;      /* the other farmer's places in the storage */
static uint8_t trade_store[STORE_PLACES * 6];
#define LENT_MAX 17             /* what farmer 2 put away for a save: where, from where, what */
static int lent_n;
static uint32_t lent_at[LENT_MAX];
static uint16_t lent_slot[LENT_MAX];
static uint8_t lent_e[LENT_MAX][6];
#define STORAGES 0x80070D42u    /* the fridge, the tool chest and the cabinet, one after the other */
#define STORAGES_SIZE (3 * STORE_PLACES * 6)
static uint8_t lent_snap[STORAGES_SIZE];
static int lent_exchanged;      /* put away while farmer 2 had the game: "me" was the player's */

static int16_t half(const uint8_t *s, int at) { return (int16_t)(s[at] | s[at + 1] << 8); }
static void set_half(uint8_t *s, int at, int v) { s[at] = (uint8_t)v; s[at + 1] = (uint8_t)(v >> 8); }

/* farmer 2's tiredness after its turn (see FALLEN) */
static void tiredness(void) {
    int16_t tired = half(me, TIRED);
    if (tired >= 100 && (half(me, STATE_MARKS) & FALLEN)) {
        fainted = 1;
        set_half(me, TIRED, FAINTED_TIRED);
    } else if (tired < FAINTED_TIRED) {
        fainted = 0;
    }
}

/* the buttons that reach the game for farmer 2: fainted, no tools (but what is in its hand: the
 * medicine) and no running */
static uint16_t allowed(void) {
    if (owner) return 0xFFFF;
    if (!fainted) return BUTTONS;
    uint16_t b = BUTTONS & ~(PAD_CIRCLE | PAD_SQUARE);
    if (me[CARRIED + 6] | me[CARRIED + 7]) b |= PAD_SQUARE;
    return b;
}

/* the money in the player's place, into the struct copy s; and the rucksack (+01E: 0, 1, 2 for 2, 4,
 * 8 places), the bigger of the two: a rucksack bought by either farmer is both farmers' at once (it
 * was copied only when an area loaded) */
#define RUCKSACK_SIZE 0x1E
static void share(uint8_t *s) {
    for (uint32_t i = 0; i < 4; i++) s[MONEY + i] = (uint8_t)MEM_R8(PLAYER + MONEY + i);
    int16_t bag = (int16_t)MEM_R16(PLAYER + RUCKSACK_SIZE);
    if (bag > half(s, RUCKSACK_SIZE)) set_half(s, RUCKSACK_SIZE, bag);
}

static void swap_in(void) {     /* farmer 2's struct in place of the player's */
    share(me);
    for (uint32_t i = 0; i < PLAYER_SIZE; i++) {
        saved[i] = (uint8_t)MEM_R8(PLAYER + i);
        MEM_W8(PLAYER + i, me[i]);
    }
    memcpy(saved_anims, ANIMS_RAM, ANIMS_SIZE);
    memcpy(ANIMS_RAM, my_anims, ANIMS_SIZE);
}

static void swap_out(void) {    /* ... and back */
    share(saved);
    for (uint32_t i = 0; i < PLAYER_SIZE; i++) {
        me[i] = (uint8_t)MEM_R8(PLAYER + i);
        MEM_W8(PLAYER + i, saved[i]);
    }
    memcpy(my_anims, ANIMS_RAM, ANIMS_SIZE);
    memcpy(ANIMS_RAM, saved_anims, ANIMS_SIZE);
}

/* the game's ground query at x, z (on the stack below the caller's, which the routine's own frame
 * keeps clear of): 1 = the player can stand there, *y = the ground's height */
static int ground(CPU *c, uint32_t sp, int32_t x, int32_t *y, int32_t z) {
    uint32_t v = sp - 32;
    MEM_W32(v, (uint32_t)x);
    MEM_W32(v + 4, (uint32_t)*y);
    MEM_W32(v + 8, (uint32_t)z);
    MEM_W32(v + 12, 0);
    c->r[29] = sp - 64;
    c->r[4] = v;
    c->r[5] = GROUND_STAND;
    call_addr(c, GROUND);
    if (c->r[2]) return 0;
    *y = (int32_t)MEM_R32(v + 4);
    return 1;
}

/* a new day (the player slept, or saved and went on to the next; the player wakes up at 6): farmer
 * 2 wakes up rested, all its stamina back and no tiredness, however the player slept */
static void rest(void) {
    uint16_t day = (uint16_t)MEM_R16(DAY);
    if (!placed || day == rested_day || MEM_R16(HOUR) != WAKING_HOUR) return;
    rested_day = day;
    set_half(me, STAMINA, half(me, STAMINA_MAX));
    set_half(me, TIRED, 0);
    set_half(me, STATE_MARKS, half(me, STATE_MARKS) & ~TIRED_MARKS);
    fainted = 0;
}

/* new area (the farm's tick was not run for a while, and the area is another: a load; a menu or a
 * message stops the tick too): a copy of the player, beside it (on it if
 * there is no ground beside). Farmer 2 goes where it likes in the area (it has a window of its own): it
 * is no longer brought back when far from the player */
static void place(CPU *c, uint32_t sp) {
    int32_t x = (int32_t)MEM_R32(POS), y = (int32_t)MEM_R32(POS + 4), z = (int32_t)MEM_R32(POS + 8);
    rest();
    /* the area's number does not always change (on some mornings the house and the farm had the
     * same: farmer 2 stayed in the house when the player first went out); a load moves the player
     * far at once */
    int moved = abs(x - seen_x) > 1024 || abs(z - seen_z) > 1024;
    seen_x = x;
    seen_z = z;
    if (placed && !loaded) return;
    loaded = 0;
    if (placed && MEM_R16(AREA) == area && !moved) return;   /* a menu, a message: the same area */
    area = (uint16_t)MEM_R16(AREA);
    /* there is one of each tool: farmer 2 starts with nothing, takes them from the tool chest and
     * keeps what it carries */
    uint8_t carried[CARRIED_SIZE] = { 0 };
    if (placed) memcpy(carried, me + CARRIED, CARRIED_SIZE);
    /* farmer 2 keeps its stamina too (see rest) */
    int16_t stamina = half(me, STAMINA), most = half(me, STAMINA_MAX), tired = half(me, TIRED);
    int marks = half(me, STATE_MARKS) & TIRED_MARKS;
    uint32_t holding;
    memcpy(&holding, me + 0x6C, 4);
    holding &= HOLDING;
    if (!placed) rested_day = (uint16_t)MEM_R16(DAY);
    for (uint32_t i = 0; i < PLAYER_SIZE; i++) me[i] = (uint8_t)MEM_R8(PLAYER + i);
    memcpy(my_anims, ANIMS_RAM, ANIMS_SIZE);   /* the player's animation points into it */
    memcpy(me + CARRIED, carried, CARRIED_SIZE);
    {   /* an event leading the player into the area (a festival) leads the player only: copied, it
         * led farmer 2 for ever (no one ends it in farmer 2's struct; it could not move) */
        uint32_t f;
        memcpy(&f, me + 0x6C, 4);
        f &= ~LED;
        /* what farmer 2 holds up stays held (the player's flags said nothing was: the item was
         * not drawn till the rucksack was opened) */
        if (placed) f = (f & ~HOLDING) | holding;
        memcpy(me + 0x6C, &f, 4);
    }
    if (placed) {
        set_half(me, STAMINA, stamina);
        set_half(me, STAMINA_MAX, most);
        set_half(me, TIRED, tired);
        set_half(me, STATE_MARKS, (half(me, STATE_MARKS) & ~TIRED_MARKS) | marks);
    }
    swap_globals(my_globals, NULL);
    swap_at(MEM_R32(PAD), PAD_SIZE, my_pad, NULL);
    swap_at(MEM_R32(INPUT_LOG), INPUT_LOG_SIZE, my_log, NULL);
    if (ground(c, sp, x + START_DX, &y, z)) {
        x += START_DX;
        memcpy(me + 0x100, &x, 4);
        memcpy(me + 0x104, &y, 4);
    }
    placed = 1;
}

/* the ids carried in a struct's copy "s" (what the player carries, 6 bytes each, from +084) */
static int carried_ids(const uint8_t *s, uint16_t ids[18]) {
    int n = 0;
    for (int i = 0; i < CARRIED_SIZE / 6; i++) {
        uint16_t id = (uint16_t)(s[i * 6] | s[i * 6 + 1] << 8);
        if (id) ids[n++] = id;
    }
    return n;
}

static int find(const uint16_t *ids, int n, uint16_t id) {
    for (int i = 0; i < n; i++) if (ids[i] == id) return 1;
    return 0;
}

static uint32_t call1(CPU *c, uint32_t sp, uint32_t at, uint32_t a0) {
    c->r[29] = sp;
    c->r[4] = a0;
    call_addr(c, at);
    return c->r[2];
}

/* the register's place for id: its kind's places, the one with id (or -1) and the first free */
static int registered(CPU *c, uint32_t sp, uint16_t id, int *free_place, uint32_t *places) {
    for (int i = 0; i < REGISTRY_PLACES * 2; i++)
        if (MEM_R16(REGISTRY + (uint32_t)i * 2) == id) return i;
    uint32_t kind = call1(c, sp, KIND, id);
    *places = REGISTRY + (kind ? REGISTRY_PLACES * 2 : 0);
    *free_place = -1;
    for (int i = 0; i < REGISTRY_PLACES; i++) {
        uint16_t r = (uint16_t)MEM_R16(*places + (uint32_t)i * 2);
        if (r == id) return i;
        if (!r && *free_place < 0) *free_place = i;
    }
    return -1;
}

/* the register holds what both farmers carry (see REGISTRY) */
static void keep_registry(CPU *c, uint32_t sp) {
    uint8_t mine[CARRIED_SIZE];
    uint16_t p1[18], p2[18];
    for (uint32_t i = 0; i < CARRIED_SIZE; i++) mine[i] = (uint8_t)MEM_R8(PLAYER + CARRIED + i);
    int n1 = carried_ids(mine, p1), n2 = carried_ids(me + CARRIED, p2);
    for (int who = 0; who < 2; who++)
        for (int k = 0; k < (who ? n2 : n1); k++) {
            uint16_t id = who ? p2[k] : p1[k];
            int free_place;
            uint32_t places;
            if (registered(c, sp, id, &free_place, &places) >= 0) continue;
            if (free_place < 0 && who) continue;        /* farmer 2's only where there is room */
            if (free_place < 0) {       /* the player's: in place of one the player does not carry */
                uint16_t out = 0;
                for (int i = 0; i < REGISTRY_PLACES; i++) {
                    uint16_t r = (uint16_t)MEM_R16(places + (uint32_t)i * 2);
                    if (!find(p1, n1, r) && (!out || !find(p2, n2, r))) out = r;
                }
                if (!out) continue;
                call1(c, sp, UNREGISTER, out);
            }
            call1(c, sp, REGISTER, id);
        }
}

/* trading (see FACING) */
static int rucksack(const uint8_t *s) {
    int n = 2 << half(s, RUCKSACK);
    return n > 8 || n < 2 ? 8 : n;
}

/* the places of a kind in a struct copy: the tool in hand and the tools' rucksack, or the items'
 * rucksack (not the item in hand: it is held up in the arms) */
static int trade_places(const uint8_t *s, int kind, int *at) {
    int n = 0, r = rucksack(s);
    if (!kind) at[n++] = CARRIED;
    for (int i = 0; i < r; i++) at[n++] = (kind ? 0xC0 : 0x90) + i * 6;
    return n;
}

static uint32_t trade_store_at(int kind) { return kind ? STORE_ITEMS : STORE_TOOLS; }

static int trade_carries(const uint8_t *s, int kind) {
    int at[9], n = trade_places(s, kind, at);
    for (int k = 0; k < n; k++) if (s[at[k]] | s[at[k] + 1]) return 1;
    return 0;
}

/* a in front of b, near; a can trade (no action, not led, not throwing) */
static int trade_facing(const uint8_t *a, const uint8_t *b) {
    int32_t ax, az, bx, bz;
    memcpy(&ax, a + 0x100, 4);
    memcpy(&az, a + 0x108, 4);
    memcpy(&bx, b + 0x100, 4);
    memcpy(&bz, b + 0x108, 4);
    int32_t dx = bx - ax, dz = bz - az, ahead, side;
    switch (half(a, FACING)) {
    case 0: ahead = -dz; side = dx; break;
    case 1: ahead = -dx; side = dz; break;
    case 2: ahead = dz; side = dx; break;
    case 3: ahead = dx; side = dz; break;
    default: return 0;
    }
    return ahead > 0 && ahead <= REACH && side >= -REACH_SIDE && side <= REACH_SIDE;
}

static int trade_idle(const uint8_t *s) {
    uint32_t f;
    memcpy(&f, s + 0x6C, 4);
    return !(f & (0x50 | 0x40 | LED));
}

/* the other farmer's things (struct copy s) into the storage, the real one kept aside */
static void trade_fill(uint8_t *s, int kind) {
    int at[9], n = trade_places(s, kind, at);
    uint32_t st = trade_store_at(kind);
    for (uint32_t i = 0; i < sizeof trade_store; i++) {
        trade_store[i] = (uint8_t)MEM_R8(st + i);
        MEM_W8(st + i, 0);
    }
    for (int k = 0; k < n; k++) {
        for (uint32_t i = 0; i < 6; i++) MEM_W8(st + (uint32_t)k * 6 + i, s[at[k] + i]);
        memset(s + at[k], 0, 6);
    }
    trade_kind = kind;
    trade_places_n = n;
}

/* the menu closed: what is in the storage is the other farmer's (s), the real storage goes back */
static void trade_empty(uint8_t *s) {
    int at[9], n = trade_places(s, trade_kind, at), k = 0;
    uint32_t st = trade_store_at(trade_kind);
    for (uint32_t p = 0; p < STORE_PLACES; p++) {
        uint8_t e[6];
        for (uint32_t i = 0; i < 6; i++) e[i] = (uint8_t)MEM_R8(st + p * 6 + i);
        if (!(e[0] | e[1])) continue;
        if (k < n) {
            memcpy(s + at[k++], e, 6);
            continue;
        }
        for (uint32_t q = 0; q < STORE_PLACES; q++)      /* no place left (the menu takes none): the storage's */
            if (!(trade_store[q * 6] | trade_store[q * 6 + 1])) {
                memcpy(trade_store + q * 6, e, 6);
                break;
            }
    }
    for (uint32_t i = 0; i < sizeof trade_store; i++) MEM_W8(st + i, trade_store[i]);
}

/* The tool chest and the cabinet are in the house: their menus load their pictures over a corner of
 * VRAM the farm's ground uses (320-383 x 0-255; leaving the house loads the farm's again). Opened
 * outside by a trade, the ground was drawn with the menu's pictures until the area changed: that
 * corner is kept when the trade opens and put back when it ends, in both GPUs. Not in STATE: kept
 * by the frame's first run that opens it (the second window's comes first and already loads some
 * of the menu), put back by the frame's own run. */
#define TRADE_VRAM_X 320
#define TRADE_VRAM_W 64
#define TRADE_VRAM_H 256
static uint16_t trade_vram[2][TRADE_VRAM_W * TRADE_VRAM_H];
static int trade_vram_kept;
static uint64_t trade_vram_frame;

/* kind 0 starts a trade: a copy from another frame is an old one (an items' menu that never opened) */
static void trade_vram_keep(int kind) {
    if (trade_vram_kept && (kind || trade_vram_frame == frame_count())) return;
    trade_vram_frame = frame_count();
    const uint16_t *v[2] = { gpu_vram(), view2_gpu_vram() };
    for (int g = 0; g < 2; g++)
        for (int y = 0; y < TRADE_VRAM_H; y++)
            memcpy(trade_vram[g] + y * TRADE_VRAM_W, v[g] + y * 1024 + TRADE_VRAM_X, TRADE_VRAM_W * 2);
    trade_vram_kept = 1;
}

static void trade_vram_back(void) {
    if (g_view2_run || !trade_vram_kept) return;
    trade_vram_kept = 0;
    void (*gp0[2])(uint32_t) = { gpu_write_gp0, view2_gpu_write_gp0 };
    for (int g = 0; g < (g_view2 > 0 ? 2 : 1); g++) {     /* a CPU -> VRAM transfer, as the game's loads */
        gp0[g](0xA0000000u);
        gp0[g](TRADE_VRAM_X);
        gp0[g]((uint32_t)TRADE_VRAM_H << 16 | TRADE_VRAM_W);
        for (int i = 0; i < TRADE_VRAM_W * TRADE_VRAM_H; i += 2)
            gp0[g](trade_vram[g][i] | (uint32_t)trade_vram[g][i + 1] << 16);
    }
}

/* opens the storage's menu for the farmer in the player's place, as 800F6778 opens the cabinet's
 * and the X action (800C7AC0) marks the menu open */
static void trade_open(CPU *c, uint32_t sp, int kind) {
    call1(c, sp, MENU_RESET, 0);
    MEM_W32(MENU_FLAGS, MEM_R32(MENU_FLAGS) | 0x00020000u);
    call1(c, sp, MENU_SOUND, 3);
    MEM_W32(MENU_STATE, kind ? 4 : 3);
    call1(c, sp, 0x800226D8u, 0);
    call1(c, sp, 0x80017B18u, 0);
    c->r[5] = kind ? ITEMS_TEXT : TOOLS_TEXT;
    call1(c, sp, MENU_TEXT, 3);
    MEM_W32(MENU_FLAGS, MEM_R32(MENU_FLAGS) | 0x18000000u);
}

/* the farmer in the player's place (p, its struct's copy) trades with the other (o): X just pressed
 * in front of it, or the items' menu after the tools'. 1 = opened */
static int trade_start(CPU *c, uint32_t sp, int who, const uint8_t *p, uint8_t *o, int pressed) {
    if (trade_who || (MEM_R32(MENU_FLAGS) & MENU_OPEN)) return 0;
    int kind;
    if (trade_next == who) {
        trade_next = 0;
        kind = 1;
    } else if (pressed && trade_idle(p) && trade_idle(o) && trade_facing(p, o)) {
        kind = 0;
    } else {
        return 0;
    }
    trade_fill(o, kind);
    trade_vram_keep(kind);
    trade_open(c, sp, kind);
    trade_who = who;
    return 1;
}

/* the trade's menu closed (between turns: the player's struct in place, farmer 2's in "me") */
static void trade_end(void) {
    if (!trade_who || (MEM_R32(MENU_FLAGS) & MENU_OPEN)) return;
    uint8_t p1[PLAYER_SIZE];
    for (uint32_t i = 0; i < PLAYER_SIZE; i++) p1[i] = (uint8_t)MEM_R8(PLAYER + i);
    uint8_t *opener = trade_who == 1 ? p1 : me, *other = trade_who == 1 ? me : p1;
    trade_empty(other);
    for (uint32_t i = CARRIED; i < CARRIED + CARRIED_SIZE; i++) MEM_W8(PLAYER + i, p1[i]);
    /* the items' menu next, if either has items */
    if (!trade_kind && (trade_carries(opener, 1) || trade_carries(other, 1) || opener[CARRIED + 6] | opener[CARRIED + 7]))
        trade_next = trade_who;
    else
        trade_vram_back();
    trade_who = 0;
}

/* the thing in a slot of the trade's menu (0 the hand, 1-8 the rucksack of the farmer in the
 * player's place, 9-16 the storage's page) */
static uint32_t trade_slot(int32_t slot, int32_t page) {
    if (slot >= 9) return trade_store_at(trade_kind) + (uint32_t)(page * 8 + slot - 9) * 6;
    if (slot > 0) return PLAYER + (trade_kind ? 0xC0u : 0x90u) + (uint32_t)(slot - 1) * 6;
    return PLAYER + (trade_kind ? 0x8Au : CARRIED);
}

/* entry of 800F7D34 (a0 / a2 the slot and page moved from, a1 / a3 to): in a trade's menu, nothing
 * goes to the storage's places beyond the other farmer's, and nothing is piled on the same thing
 * there (the storage piles them, x2: those places are the other farmer's rucksack) */
int p2_trade_move(CPU *c) {
    if (!trade_who) return 0;
    for (int k = 0; k < 2; k++) {
        int32_t slot = (int32_t)c->r[4 + k], page = (int16_t)c->r[6 + k];
        if (slot >= 9 && page * 8 + slot - 9 >= trade_places_n) {
            c->r[2] = 0;
            return 1;
        }
    }
    int32_t to = (int32_t)c->r[5];
    if (to >= 9) {
        uint32_t what = MEM_R16(trade_slot((int32_t)c->r[4], (int16_t)c->r[6]));
        if (what && MEM_R16(trade_slot(to, (int16_t)c->r[7])) == what) {
            c->r[2] = 0;
            return 1;
        }
    }
    return 0;
}

/* entry of 800D57A4 (a0 = an item; the storage it goes to): in a trade's items menu the fridge's
 * items go to the cabinet too */
int p2_trade_kind(CPU *c) {
    if (!trade_who || !trade_kind) return 0;
    uint32_t f = MEM_R32(MEM_R32(ITEM_TABLE) + (uint32_t)(int16_t)c->r[4] * 40 + 0x18);
    if ((f & 0x10000000u) || !(f & 0x60000000u)) return 0;
    c->r[2] = 1;
    return 1;
}

static uint16_t demo_buttons(void) {    /* a square, 2 seconds a side, then 2 seconds standing */
    static const uint16_t sides[5] = { PAD_RIGHT, PAD_UP, PAD_LEFT, PAD_DOWN, 0 };
    return sides[(ticks / 60) % 5];
}

/* the game's pad bits are the PlayStation's with the two bytes swapped */
static uint32_t game_bits(uint16_t b) { return (uint32_t)(uint16_t)(b << 8 | b >> 8); }

/* farmer 2's pad state for buttons b, as the game makes the player's */
static uint32_t held, count;
static void make_pad(uint16_t b) {
    uint32_t now = game_bits(b), fresh = now & ~held, repeat = fresh;
    count = now == held ? count + 1 : 0;
    uint32_t delay = 20, rate = 7;
    memcpy(&delay, my_pad + 0x18, 4);
    memcpy(&rate, my_pad + 0x1C, 4);
    if (!delay || !rate) { delay = 20; rate = 7; }
    if (now && count >= delay && (count - delay) % rate == 0) repeat = now;
    held = now;
    memcpy(my_pad + 0x00, &fresh, 4);
    memcpy(my_pad + 0x04, &now, 4);
    memcpy(my_pad + 0x08, &repeat, 4);
    memcpy(my_pad + 0x0C, &now, 4);
    memcpy(my_pad + 0x10, &count, 4);
    memcpy(my_pad + 0x24, &fresh, 4);
    memcpy(my_pad + 0x28, &count, 4);
}

static int flying(void) {
    int bits = 0;
    for (int k = 0; k < THROWN_PLACES; k++)
        if (MEM_R16(THROWN + (uint32_t)k * THROWN_SIZE + 2)) bits |= 1 << k;
    return bits;
}

static void turn(CPU *c) {
    if (!g_p2_enabled || inside || exchanged) return;
    uint32_t regs[32], hi = c->hi, lo = c->lo, sp = c->r[29];
    memcpy(regs, c->r, sizeof regs);
    ticks++;
    place(c, sp);
    if (lent_n) p2_take_back();
    trade_end();
    uint32_t pad = MEM_R32(PAD), log = MEM_R32(INPUT_LOG);
    {   /* the player's X on farmer 2 */
        uint32_t fresh = MEM_R32(pad);
        if ((fresh & game_bits(PAD_CROSS)) || trade_next == 1) {
            uint8_t p1[PLAYER_SIZE];
            for (uint32_t i = 0; i < PLAYER_SIZE; i++) p1[i] = (uint8_t)MEM_R8(PLAYER + i);
            trade_start(c, sp, 1, p1, me, (fresh & game_bits(PAD_CROSS)) != 0);
        }
    }
    if (!(MEM_R32(MENU_FLAGS) & MENU_OPEN)) keep_registry(c, sp);
    p1_led = (MEM_R32(FLAGS) & LED) != 0;
    swap_at(pad, PAD_SIZE, saved_pad, my_pad);
    swap_at(log, INPUT_LOG_SIZE, saved_log, my_log);
    swap_globals(saved_globals, my_globals);
    swap_at(VIEW, VIEW_SIZE, saved_view, NULL);
    swap_in();
    inside = 1;
    uint32_t menu = MEM_R32(MENU_FLAGS) & MENU_OPEN, led = MEM_R32(FLAGS) & LED;
    int was_flying = flying();
    if (MEM_R32(HOUR) != idle_clock) {     /* as the clock (800C0A4C) does it for the player each minute */
        idle_clock = MEM_R32(HOUR);
        c->r[4] = PLAYER;
        c->r[29] = sp;
        call_addr(c, IDLE);
    }
    c->r[29] = sp;
    call_addr(c, CONTROL);
    {   /* farmer 2's X on the player (the player's struct is in "saved") */
        uint32_t mine;
        memcpy(&mine, my_pad, 4);
        int pressed = !menu && !led && (mine & game_bits(PAD_CROSS)) && !(MEM_R32(FLAGS) & LED);
        if (pressed || trade_next == 2) {
            uint8_t p2[PLAYER_SIZE];
            for (uint32_t i = 0; i < PLAYER_SIZE; i++) p2[i] = (uint8_t)MEM_R8(PLAYER + i);
            trade_start(c, sp, 2, p2, saved, pressed);
        }
    }
    int took = (!menu && (MEM_R32(MENU_FLAGS) & MENU_OPEN)) || (!led && (MEM_R32(FLAGS) & LED));
    if (took) owner = 1;
    /* most things farmer 2's X opens (the tool chest, a sign, a person) are a script the turn only
     * starts: the game runs it later in the frame (800F2400), and it opens the menu there */
    {
        uint32_t mine, theirs;
        memcpy(&mine, my_pad, 4);
        memcpy(&theirs, saved_pad, 4);
        asked = !menu && (mine & game_bits(PAD_CROSS)) && !(theirs & game_bits(PAD_CROSS));
    }
    /* as the player's state routine (800CD9A0) lets it walk: not in an action */
    uint32_t f = MEM_R32(FLAGS);
    if (!(f & 0x50) && (!(f & 0x20000000) || (f & 0x40000008))) {
        c->r[29] = sp;
        call_addr(c, WALK);
    }
    thrown_by_p2 |= flying() & ~was_flying;
    inside = 0;
    if (took) {
        /* farmer 2 has just started a talk or a menu: the rest of this tick is farmer 2's already.
         * Run with the player's struct, it ended the talk at once (800DBAB0 ends the lead when the
         * talk it waits for has not begun) */
        memcpy(me, saved, PLAYER_SIZE);
        memcpy(my_anims, saved_anims, ANIMS_SIZE);
        memcpy(my_globals, saved_globals, GLOBALS_SIZE);
        memcpy(my_log, saved_log, INPUT_LOG_SIZE);
        memcpy(my_pad, saved_pad, PAD_SIZE);
        exchanged = 1;
        flying_before = flying();
    } else {
        swap_out();
        tiredness();
        swap_globals(my_globals, saved_globals);
        swap_at(log, INPUT_LOG_SIZE, my_log, saved_log);
        swap_at(pad, PAD_SIZE, my_pad, saved_pad);
    }
    {
        uint8_t t[VIEW_SIZE];
        swap_at(VIEW, VIEW_SIZE, t, saved_view);
    }
    memcpy(c->r, regs, sizeof regs);
    c->hi = hi;
    c->lo = lo;
}

/* Some actions and scenes link the player's animation to the extra file (800CD000) and the
 * game links the farm's set back (800BD168) when they end. The scene of an animal's death ends
 * with the player sitting and takes it home without that: the player came home sitting on the
 * floor till a button was pressed (and farmer 2, placed as a copy, sat too). After an area loads,
 * a player standing free (not led, state 1) on the extra file is stood up. Alone too. */
#define ANIM_FILE 0x80071C74u   /* +274: the file the player's animation is linked to */
#define EXTRA_FILE ANIMS
#define STAND_UP 0x800BD168u
#define STATE_NOW 0x3C          /* halfword: 1 standing, 2 walking, more in an action */

static void stand_up(CPU *c) {
    static uint16_t last_area;
    static int32_t last_x, last_z;
    static int pending;
    if (g_view2_run) return;
    int32_t x = (int32_t)MEM_R32(POS), z = (int32_t)MEM_R32(POS + 8);
    if (MEM_R16(AREA) != last_area || abs(x - last_x) > 1024 || abs(z - last_z) > 1024) pending = 1;
    last_area = (uint16_t)MEM_R16(AREA);
    last_x = x;
    last_z = z;
    if (!pending || (MEM_R32(FLAGS) & LED)) return;
    pending = 0;
    if (MEM_R32(ANIM_FILE) != EXTRA_FILE || MEM_R16(PLAYER + STATE_NOW) != 1) return;
    uint32_t regs[32], hi = c->hi, lo = c->lo;
    memcpy(regs, c->r, sizeof regs);
    call1(c, c->r[29], STAND_UP, 0);
    memcpy(c->r, regs, sizeof regs);
    c->hi = hi;
    c->lo = lo;
}

void p2_tick(CPU *c) {
    stand_up(c);
    turn(c);
    if (g_view2_run) p2_aim_view();
}

/* the second view (p2view.c) runs the farm's frame once more with the view on the other farmer,
 * aimed at it as the walk aims it at the player (not while the view is held: 8006C844 bit
 * 00400000): farmer 2, or in farmer 2's menu (farmer 2 in the player's place) the player */
void p2_aim_view(void) {
    if (!placed || (MEM_R32(MENU_FLAGS) & 0x00400000u)) return;
    for (uint32_t k = 0; k < 3; k++)
        MEM_W16(VIEW + 0x20 + k * 2, (uint32_t)(me[0x100 + k * 4] | me[0x101 + k * 4] << 8));
}

/* ... and puts back everything farmer 2 keeps */
#define STATE(X) X(me) X(my_globals) X(my_pad) X(my_log) X(my_anims) X(placed) X(inside) X(ticks) \
    X(owner) X(exchanged) X(asked) X(thrown_by_p2) X(flying_before) X(landing) X(loaded) X(seen_x) X(seen_z) X(last_tick) X(buttons) X(rested_day) X(area) X(held) X(count) X(fainted) X(idle_clock) \
    X(trade_who) X(trade_kind) X(trade_next) X(trade_places_n) X(trade_store) \
    X(lent_n) X(lent_at) X(lent_slot) X(lent_e) X(lent_snap) X(lent_exchanged)
#define SIZE(v) + sizeof v
#define SAVE(v) memcpy(o, &v, sizeof v); o += sizeof v;
#define LOAD(v) memcpy(&v, i, sizeof v); i += sizeof v;
size_t p2_state_size(void) { return 0 STATE(SIZE); }
void p2_save_state(void *dst) { uint8_t *o = dst; STATE(SAVE) }
void p2_load_state(const void *src) { const uint8_t *i = src; STATE(LOAD) }

/* farmer 2's struct, globals, pad and input log for the player's, both ways */
static void exchange(void) {
    uint8_t t[GLOBALS_SIZE > PAD_SIZE ? GLOBALS_SIZE : PAD_SIZE];
    share(me);
    for (uint32_t i = 0; i < PLAYER_SIZE; i++) {
        uint8_t v = (uint8_t)MEM_R8(PLAYER + i);
        MEM_W8(PLAYER + i, me[i]);
        me[i] = v;
    }
    swap_globals(t, my_globals);
    memcpy(my_globals, t, GLOBALS_SIZE);
    swap_at(MEM_R32(PAD), PAD_SIZE, t, my_pad);
    memcpy(my_pad, t, PAD_SIZE);
    swap_at(MEM_R32(INPUT_LOG), INPUT_LOG_SIZE, t, my_log);
    memcpy(my_log, t, INPUT_LOG_SIZE);
    memcpy(saved_anims, ANIMS_RAM, ANIMS_SIZE);
    memcpy(ANIMS_RAM, my_anims, ANIMS_SIZE);
    memcpy(my_anims, saved_anims, ANIMS_SIZE);
}

/* the farm's tick begins (entry of 800C162C): farmer 2's buttons for this tick; while a menu
 * farmer 2 opened is up, the whole tick is farmer 2's, with all its buttons */
void p2_tick_begin(void) {
    if (!g_p2_enabled) return;
    uint64_t now = frame_count();
    if (now - last_tick > 8) loaded = 1;
    last_tick = now;
    buttons = g_p2_demo ? demo_buttons() : g_p2_buttons;
    uint32_t f;
    memcpy(&f, me + 0x6C, 4);
    if (asked && (MEM_R32(MENU_FLAGS) & MENU_OPEN)) owner = 1;     /* the script farmer 2's X started */
    /* ... or it leads "the player" (a thing on a shop's shelf: the script marks the struct in the
     * player's place, the player's, the next frame; the farmer then takes the thing in its hands
     * and the menu to buy it opens): the lead is farmer 2's */
    if (asked && !owner && !p1_led && (MEM_R32(FLAGS) & LED) && !(f & LED)) {
        MEM_W32(FLAGS, MEM_R32(FLAGS) & ~LED);
        f |= LED;
        memcpy(me + 0x6C, &f, 4);
        owner = 1;
    }
    asked = 0;
    if (owner && !(MEM_R32(MENU_FLAGS) & MENU_OPEN) && !(f & LED)) owner = 0;
    /* in farmer 2's own menu all its buttons but Start and Select reach the game */
    make_pad(buttons & allowed());
    thrown_by_p2 &= flying();
    if (!owner || !placed) return;
    exchange();
    exchanged = 1;
    flying_before = flying();
}

/* ... and ends (800C1874) */
void p2_tick_end(void) {
    if (!exchanged) return;
    thrown_by_p2 |= flying() & ~flying_before;
    exchange();
    exchanged = 0;
    tiredness();
}

void p2_draw(CPU *c) {
    if (!g_p2_enabled || inside || !placed) return;
    uint32_t a = c->r[4];
    uint32_t regs[32], hi = c->hi, lo = c->lo;
    memcpy(regs, c->r, sizeof regs);
    uint32_t x = MEM_R32(a), y = MEM_R32(a + 4), z = MEM_R32(a + 8);
    uint32_t slot = SLOTS + (MEM_R32(BUFFER) & 1) * P2_SLOTS * 8;
    uint16_t places[P2_SLOTS * 4];
    for (int i = 0; i < P2_SLOTS; i++) {
        uint32_t at = slot + (uint32_t)i * 8;
        for (int k = 0; k < 4; k++) places[i * 4 + k] = (uint16_t)MEM_R16(at + (uint32_t)k * 2);
        MEM_W16(at + 2, (uint32_t)(places[i * 4 + 1] + P2_PALETTE_DY));
        MEM_W16(at + 4, (uint32_t)(places[i * 4 + 2] + P2_PIXELS_DX));
        MEM_W16(at + 6, (uint32_t)(places[i * 4 + 3] + P2_PIXELS_DY));
    }
    swap_in();
    MEM_W32(a, MEM_R32(POS));
    MEM_W32(a + 4, MEM_R32(POS + 4));
    MEM_W32(a + 8, MEM_R32(POS + 8));
    inside = 1;
    call_addr(c, PLAYER_DRAW);
    inside = 0;
    swap_out();
    for (int i = 0; i < P2_SLOTS * 4; i++) MEM_W16(slot + (uint32_t)i * 2, places[i]);
    MEM_W32(a, x);
    MEM_W32(a + 4, y);
    MEM_W32(a + 8, z);
    memcpy(c->r, regs, sizeof regs);
    c->hi = hi;
    c->lo = lo;
}

int p2_drawing(void) { return inside; }

/* the cheats (cheats.c) for farmer 2: never tired, the biggest rucksack, the tools at their most */
void p2_cheats(int rest, int bag, int tools) {
    if (!placed) return;
    if (rest) {
        set_half(me, STAMINA, half(me, STAMINA_MAX));
        set_half(me, TIRED, 0);
    }
    if (bag) set_half(me, 0x1E, 2);
    if (tools)
        for (int k = 0; k < 5; k++) set_half(me, 0x40 + k * 2, 0xFFFF);
}

/* the X action's target (800C7AC0, after 800C7B70: v1 = its kind - 1, 49 kinds): farmer 2 does not
 * use the bed (kind 2: sleeping ends the day and asks for the diary, the whole game's; farmer 2
 * was left lying in it until placed again). Anything past 49 is no target */
#define BED_KIND 2
uint32_t p2_x_target(uint32_t kind) {
    if ((inside || exchanged) && kind + 1 == BED_KIND) return 0xFFFFFFFFu;
    return kind;
}

/* farmer 2 has the game (its talk or menu): the game's window is farmer 2's, the second view the
 * player's */
int p2_owner(void) { return g_p2_enabled && placed && owner; }

/* a scene: a talk, a menu or an event leading the player that no farmer's button started (the
 * kappa, an animal that died, a visit): both windows show it. The player's own (its X on a person,
 * its rucksack: a button just before) stay in the player's window */
#define SCENE_PRESS 8           /* frames: a talk or menu this soon after the player's button is the player's */
static int scene, busy_was;
static uint64_t p1_pressed, scene_seen = ~0ull;
int p2_scene(void) {
    if (!g_p2_enabled || !placed) return 0;
    uint64_t now = frame_count();
    if (scene_seen == now) return scene;
    scene_seen = now;
    if (g_pad_buttons[0] & (PAD_CROSS | PAD_TRIANGLE | PAD_START | PAD_SELECT | PAD_SQUARE)) p1_pressed = now;
    uint32_t f;
    if (exchanged) memcpy(&f, me + 0x6C, 4);
    else f = MEM_R32(FLAGS);
    int busy = (MEM_R32(MENU_FLAGS) & MENU_OPEN) || (f & LED);
    if (!busy) scene = 0;
    else if (!busy_was) scene = !owner && !asked && now - p1_pressed > SCENE_PRESS;
    if (owner) scene = 0;
    busy_was = busy;
    return scene;
}

/* online, each player hears only its own farmer's menus and talks: the host not farmer 2's, the
 * guest not the player's (the game stands still in a menu, so its sounds are the menu's); a scene
 * is both's */
int g_p2_online;                /* online: this side's role (1 the host, 2 the guest; set by netplay.c) */
int p2_sfx_quiet(void) {
    int r = g_p2_online, m = p2_menu();
    return r && m >= 0 && m == (r == 1) && !p2_scene();
}

/* a talk or a menu is open: the player's (0) or farmer 2's (1; also the one farmer 2's X just
 * opened), -1 = none. An X that opened nothing is no menu: the windows swapped for that frame
 * (the picture jumped at each press) */
int p2_menu(void) {
    if (!g_p2_enabled || !placed) return -1;
    uint32_t open = MEM_R32(MENU_FLAGS) & MENU_OPEN;
    if (owner || (asked && open)) return 1;
    return open ? 0 : -1;
}

/* entry of 800CC0F8 (a0 = a place of THROWN): what the farmer not in the player's place threw is
 * moved with that farmer's struct in place (1 = done here) */
int p2_thrown(CPU *c) {
    uint32_t at = c->r[4] - THROWN;
    if (!g_p2_enabled || !placed || inside || landing || at >= THROWN_PLACES * THROWN_SIZE || at % THROWN_SIZE)
        return 0;
    int k = (int)(at / THROWN_SIZE);
    if ((thrown_by_p2 >> k & 1) == exchanged) return 0;
    exchange();
    landing = 1;
    call_addr(c, THROWN_MOVE);
    landing = 0;
    exchange();
    return 1;
}

/* entry of 800BE4A8, the collision test's part for the player (a0: the query; +0 -> the point x / y
 * / z words, +4 -> its floor at +2, as the player's +FE): a box 218 wide each way in x and z, 20 in
 * y, around the player's position. Animals walking (the sheep, the cows, the horse) ask for it, so
 * they went through farmer 2: it is asked for both farmers. A hit puts the player's struct in +08 ->
 * (what was hit) and +18 in +10 -> */
static int bumps(uint32_t q, int16_t floor, int16_t x, int16_t y, int16_t z) {
    uint32_t p = MEM_R32(q);
    int32_t px = (int32_t)MEM_R32(p), py = (int32_t)MEM_R32(p + 4), pz = (int32_t)MEM_R32(p + 8);
    if (floor != (int16_t)MEM_R16(MEM_R32(q + 4) + 2)) return 0;
    return (int16_t)(x - 218) < px && px < (int16_t)(x + 218) && (int16_t)(z - 218) < pz && pz < (int16_t)(z + 218)
        && (int16_t)(y - 20) < py && py < (int16_t)(y + 20);
}

int p2_bump(CPU *c) {
    if (!g_p2_enabled || !placed || inside) return 0;
    uint32_t q = c->r[4];
    int hit = bumps(q, (int16_t)MEM_R16(PLAYER + 0xFE), (int16_t)MEM_R16(POS), (int16_t)MEM_R16(POS + 4), (int16_t)MEM_R16(POS + 8))
        || bumps(q, (int16_t)(me[0xFE] | me[0xFF] << 8), (int16_t)(me[0x100] | me[0x101] << 8),
                 (int16_t)(me[0x104] | me[0x105] << 8), (int16_t)(me[0x108] | me[0x109] << 8));
    if (hit) {
        MEM_W32(MEM_R32(q + 0x10), MEM_R32(q + 0x18));
        MEM_W32(MEM_R32(q + 8), PLAYER);
    }
    c->r[2] = (uint32_t)hit;
    return 1;
}

/* the player's state routine (800CD734) faints at tiredness 100 (800DE3A0): the screen fades and the
 * game takes the player home, frames long, from inside the turn. In farmer 2's turn that ran with
 * farmer 2's struct, globals and animations in the player's place, and putting the player's back
 * afterwards wrote over what the faint had loaded (a crash a few frames later). Farmer 2 does not
 * faint: it only falls (the routine's animation), as exhausted as before */
int p2_faint_skipped(void) { return inside || exchanged; }

/* the storage a carried thing goes to: the tool chest, the cabinet or the fridge (0: none, the
 * flags of 800D57A4) */
static uint32_t storage_of(uint16_t id, int tool) {
    if (tool) return STORE_TOOLS;
    uint32_t f = MEM_R32(MEM_R32(ITEM_TABLE) + (uint32_t)(int16_t)id * 40 + 0x18);
    if (f & 0x10000000u) return STORE_TOOLS;
    if (f & 0x20000000u) return STORE_ITEMS;
    if (f & 0x40000000u) return STORE_FRIDGE;
    return 0;
}

/* farmer 2's tools (in hand and in the rucksack) and the items in its rucksack go to the storages,
 * each to the first free place; lend: noted, to be taken back (p2_take_back) */
static void put_away(int lend) {
    if (!placed) return;
    for (int k = 0; k < 17; k++) {
        int at = k == 0 ? CARRIED : k < 9 ? 0x90 + (k - 1) * 6 : 0xC0 + (k - 9) * 6;
        uint8_t *e = me + at;
        if (!(e[0] | e[1])) continue;
        uint32_t st = storage_of((uint16_t)(e[0] | e[1] << 8), k < 9);
        if (!st) continue;              /* nowhere to keep it: farmer 2 keeps it, unsaved */
        for (uint32_t p = st; p < st + STORE_PLACES * 6; p += 6)
            if (!MEM_R16(p)) {
                for (uint32_t i = 0; i < 6; i++) MEM_W8(p + i, e[i]);
                if (lend && lent_n < LENT_MAX) {
                    lent_at[lent_n] = p;
                    lent_slot[lent_n] = (uint16_t)at;
                    memcpy(lent_e[lent_n++], e, 6);
                }
                memset(e, 0, 6);
                break;
            }
    }
}

/* farmer 2's player left (online, the guest): its things go to the storages and farmer 2 leaves
 * the game. Between ticks: the player's struct is the player's */
void p2_leave(void) {
    if (!g_p2_enabled) return;
    put_away(0);
    lent_n = 0;
    g_p2_enabled = 0;
    g_p2_buttons = 0;
    placed = owner = 0;
}

/* the save and load screens (the slot question; the title's screens also when they open): the game
 * saves only what the player carries, so farmer 2's tools and rucksack items go to the storages
 * (the tool chest, the cabinet, the fridge), which are saved. Back on the farm farmer 2 takes them
 * back if the storages are as they were left (a save, or a screen left); a load changed them and
 * keeps what it loaded, so nothing is made twice */
static int storages_as_left(void) {
    for (uint32_t i = 0; i < STORAGES_SIZE; i++)
        if ((uint8_t)MEM_R8(STORAGES + i) != lent_snap[i]) return 0;
    return 1;
}

void p2_return_tools(void) {
    if (lent_n && storages_as_left()) return;      /* asked again in the same screen */
    p2_take_back();
    put_away(1);
    lent_exchanged = exchanged;
    for (uint32_t i = 0; i < STORAGES_SIZE; i++) lent_snap[i] = (uint8_t)MEM_R8(STORAGES + i);
}

void p2_take_back(void) {
    if (lent_n && !storages_as_left()) lent_n = 0;  /* a diary was loaded */
    /* farmer 2 had the game when they were put away and no longer has (or the other way round):
     * the farmer they came from is the one in the player's place */
    int in_place = lent_exchanged != exchanged;
    for (int k = 0; k < lent_n; k++) {
        uint32_t p = lent_at[k], to = PLAYER + lent_slot[k];
        uint8_t *e = me + lent_slot[k];
        if (in_place ? MEM_R16(to) != 0 : (e[0] | e[1]) != 0) continue;
        for (uint32_t i = 0; i < 6; i++) {
            MEM_W8(p + i, 0);
            if (in_place) MEM_W8(to + i, lent_e[k][i]);
            else e[i] = lent_e[k][i];
        }
    }
    lent_n = 0;
}

