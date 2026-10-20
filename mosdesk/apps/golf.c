#include "window.h"
#include "minimalos.h"
#include "stdlib.h"
#include "string.h"
#include "../../mosWM/wm_text.h"

/* ------------------------------------------------------------------ *
 *  Greenline Mini Golf  -  Kolf (KDE golf) inspired
 *
 *  Controls (like Kolf):
 *    - Move the mouse: the putter swings around the ball and aims at
 *      the cursor.  (LEFT / RIGHT arrows also rotate the aim.)
 *    - HOLD the mouse button: the power meter charges (it swings up and
 *      down).  You can keep moving the mouse to adjust the aim.
 *    - RELEASE the mouse button: the putter hits the ball.
 *    - (SPACE also works: press to start the meter, press again to hit.)
 *    - ESC: cancel the power meter.   R: replay hole.  T: scorecard.
 *
 *  Level editor (EDIT button in the header, or press E):
 *    - PICK tool: click to select, drag to move, drag the white handles
 *      to resize, arrows nudge (SHIFT = 1px), DEL deletes, D duplicates.
 *    - Other tools: click or drag on the field to place that object.
 *    - The bottom bar edits slope push, hill amount, gate travel/phase and
 *      bumper/portal radius; SNAP cycles the grid (1/2/5/10/20 px).
 *    - Z = undo, S = save, H = hole panel (name, par, new/duplicate/
 *      delete/reorder holes, reset, export).  PLAY (or E) tests the course.
 *    - SAVE writes 0:/programs/golf/levels.dat, loaded automatically on
 *      start.  EXPORT C FILE writes levels_export.c.txt in the same folder:
 *      paste it over the zones, obstacles and courses tables below to bake
 *      the levels into the program.
 *
 *  Level building:
 *    Zones   {type, x, y, w, h, slope_x, slope_y}
 *      ZONE_SAND / ZONE_ROUGH / ZONE_WATER : plain rectangles
 *      ZONE_SLOPE : rectangle; (slope_x, slope_y) is the direction the ball
 *                   is pushed AND how hard.  {24,0} = medium push right,
 *                   {0,-8} = gentle push up, {0,40} = steep push down.
 *      ZONE_HILL  : circle hill, w == h (diameter).  slope_x is the amount:
 *                   positive = hill (ball rolls away from the middle),
 *                   negative = dip (ball rolls toward the middle).
 *                   The bigger the number, the steeper it is.
 *    Obstacles  {type, x, y, w, h, radius, travel, phase}
 *      OBSTACLE_BLOCK  : rectangular wall
 *      OBSTACLE_BUMPER : round bumper (x, y, radius)
 *      OBSTACLE_GATE   : sliding wall
 *      WALL(x1,y1,x2,y2) : free-angle Kolf-style wall between two points
  *
 *  Performance: the whole course is rendered ONCE per hole into a
 *  background buffer.  Each frame only small dirty rectangles (ball,
 *  putter, moving gate, power bar) are restored + redrawn + presented.
 *  When nothing moves, nothing is drawn or presented at all.
 * ------------------------------------------------------------------ */

#define WIDTH 900
#define HEIGHT 640
#define FIELD_X 24
#define FIELD_Y 110
#define FIELD_W 852
#define FIELD_H 478
#define FRAME 8
#define BALL_RADIUS 7
#define HOLE_RADIUS 11
#define FONT_LIMIT (4u * 1024u * 1024u)
#define SUBSTEPS 2
#define SLOPE_ACCEL 4.0f
#define WALL_THICK 9.0f
#define SLOPE_REST 12.0f   /* below this push the ball may come to rest */
#define COURSE_BTN_X 290
#define COURSE_BTN_STEP 46
#define COURSE_BTN_W 38

/* level editor limits / files */
#define MAX_HOLES 7        /* scorecard has room for 7 rows, header for 8 buttons */
#define MAX_ZONES 16
#define MAX_OBST 24
#define NAME_LEN 24
#define ED_UNDO 24
#define EDIT_BTN_X 176
#define EDIT_BTN_W 96
#define LEVEL_DIR "0:/programs/golf"
#define LEVEL_FILE "0:/programs/golf/levels.dat"
#define EXPORT_FILE "0:/programs/golf/levels_export.c.txt"
#define LEVEL_MAGIC 0x31464C47u   /* 'GLF1' */

/* If your mos_window_present() does not accept a sub-rectangle, set
 * this to 0 and the whole window will be presented (still only when
 * something changed). */
#define PARTIAL_PRESENT 1

/* background colour passed to the text renderer */
#define TEXT_BG 0x80101A15

#define MET_X (FIELD_X + FIELD_W - 250)
#define MET_Y 611
#define MET_W 210
#define MET_H 14
#define BAR_Y 604

#define BTN_NEXT_X 382
#define BTN_NEXT_Y 338
#define BTN_NEXT_W 136
#define BTN_NEXT_H 28

enum { ZONE_SAND, ZONE_ROUGH, ZONE_WATER, ZONE_SLOPE, ZONE_HILL };
enum { OBSTACLE_BLOCK, OBSTACLE_BUMPER, OBSTACLE_GATE, OBSTACLE_WALL };
enum { KEY_SPACE = 44, KEY_ENTER = 40, KEY_ESC = 41, KEY_T = 23, KEY_R = 21,
       KEY_N = 17, KEY_LEFT = 80, KEY_RIGHT = 79, KEY_UP = 82, KEY_DOWN = 81 };

typedef struct {
    int type, x, y, w, h, slope_x, slope_y;
} Zone;

typedef struct {
    int type, x, y, w, h, radius, travel, phase;
} Obstacle;

/* free-angle wall from (x1,y1) to (x2,y2); stored in x,y,w,h */
#define WALL(x1, y1, x2, y2) {OBSTACLE_WALL, x1, y1, x2, y2, 0, 0, 0}

typedef struct {
    const char* name;
    int tee_x, tee_y, hole_x, hole_y, par;
    const Zone* zones;
    int zone_count;
    const Obstacle* obstacles;
    int obstacle_count;
    int portal_ax, portal_ay, portal_bx, portal_by, portal_radius;
} Course;

/* ---- 1: First Green -------------------------------------------------- */
static const Zone zones_first[] = {
    {ZONE_ROUGH, 230, 300, 110, 100, 0, 0}
};
static const Obstacle obstacles_first[] = {
    WALL(470, 238, 575, 349),
    WALL(575, 349, 470, 460),
    {OBSTACLE_BUMPER, 690, 240, 0, 0, 24, 0, 0},
    {OBSTACLE_BUMPER, 690, 458, 0, 0, 24, 0, 0}
};

/* ---- 2: Sandbar ------------------------------------------------------- */
static const Zone zones_sand[] = {
    {ZONE_SAND, 250, 274, 170, 120, 0, 0},
    {ZONE_SAND, 540, 385, 138, 102, 0, 0},
    {ZONE_HILL, 735, 420, 110, 110, 14, 0}
};
static const Obstacle obstacles_sand[] = {
    {OBSTACLE_BLOCK, 465, 186, 22, 234, 0, 0, 0},
    WALL(487, 480, 640, 480),
    WALL(600, 150, 700, 215),
    {OBSTACLE_BUMPER, 690, 290, 0, 0, 27, 0, 0}
};

/* ---- 3: Bank Shot ----------------------------------------------------- */
static const Zone zones_bank[] = {
    {ZONE_ROUGH, 220, 145, 116, 390, 0, 0},
    {ZONE_SLOPE, 518, 145, 112, 390, 24, 0}
};
static const Obstacle obstacles_bank[] = {
    {OBSTACLE_BLOCK, 382, 154, 23, 252, 0, 0, 0},
    {OBSTACLE_BLOCK, 475, 360, 23, 219, 0, 0, 0},
    WALL(150, 450, 250, 380),
    {OBSTACLE_BUMPER, 688, 236, 0, 0, 32, 0, 0},
    {OBSTACLE_BUMPER, 693, 446, 0, 0, 28, 0, 0}
};

/* ---- 4: Waterline ----------------------------------------------------- */
static const Zone zones_water[] = {
    {ZONE_WATER, 330, 110, 250, 220, 0, 0},
    {ZONE_WATER, 330, 372, 250, 216, 0, 0},
    {ZONE_SAND, 640, 300, 110, 100, 0, 0},
    {ZONE_SLOPE, 620, 125, 150, 120, 0, -12}
};
static const Obstacle obstacles_water[] = {
    WALL(235, 235, 330, 327),
    WALL(235, 463, 330, 375),
    {OBSTACLE_BUMPER, 700, 349, 0, 0, 26, 0, 0},
    WALL(760, 270, 760, 330)
};

/* ---- 5: Clockwork ----------------------------------------------------- */
static const Zone zones_clockwork[] = {
    {ZONE_SLOPE, 184, 168, 176, 334, 14, -9},
    {ZONE_ROUGH, 420, 140, 122, 388, 0, 0},
    {ZONE_SAND, 665, 395, 110, 116, 0, 0}
};
static const Obstacle obstacles_clockwork[] = {
    {OBSTACLE_BLOCK, 390, 145, 20, 196, 0, 0, 0},
    {OBSTACLE_BLOCK, 390, 398, 20, 177, 0, 0, 0},
    {OBSTACLE_GATE, 546, 311, 18, 132, 0, 156, 310},
    {OBSTACLE_BUMPER, 720, 226, 0, 0, 30, 0, 0}
};

/* ---- 6: Hilltop ------------------------------------------------------- */
static const Zone zones_hilltop[] = {
    {ZONE_SLOPE, 470, 118, 150, 100, 0, 36},
    {ZONE_SLOPE, 470, 470, 150, 105, 10, 0},
    {ZONE_HILL, 360, 259, 180, 180, 28, 0},
    {ZONE_HILL, 741, 283, 132, 132, -14, 0}
};
static const Obstacle obstacles_hilltop[] = {
    WALL(235, 150, 330, 235),
    WALL(235, 548, 330, 463),
    WALL(640, 165, 700, 245),
    WALL(640, 533, 700, 453),
    {OBSTACLE_BUMPER, 600, 349, 0, 0, 20, 0, 0}
};

/* ---- 7: Switchback ---------------------------------------------------- */
static const Zone zones_switch[] = {
    {ZONE_SLOPE, 130, 238, 740, 104, -10, 0},
    {ZONE_SLOPE, 30, 358, 740, 104, 10, 0},
    {ZONE_HILL, 140, 500, 80, 80, -10, 0}
};
static const Obstacle obstacles_switch[] = {
    WALL(24, 230, 770, 230),
    WALL(130, 350, 876, 350),
    WALL(24, 470, 770, 470)
};

static const Course default_courses[] = {
    {"First Green",
     105, 349, 806, 349, 3, zones_first, 1, obstacles_first, 4,
     0, 0, 0, 0, 0},
    {"Sandbar",
     105, 349, 807, 349, 4, zones_sand, 3, obstacles_sand, 4,
     0, 0, 0, 0, 0},
    {"Bank Shot",
     105, 522, 806, 151, 4, zones_bank, 2, obstacles_bank, 5,
     0, 0, 0, 0, 0},
    {"Waterline",
     105, 349, 807, 200, 4, zones_water, 4, obstacles_water, 4,
     0, 0, 0, 0, 0},
    {"Clockwork",
     105, 349, 806, 349, 3, zones_clockwork, 3, obstacles_clockwork, 4,
     280, 350, 670, 350, 23},
    {"Hilltop",
     105, 349, 807, 349, 4, zones_hilltop, 4, obstacles_hilltop, 5,
     0, 0, 0, 0, 0},
    {"Switchback",
     80, 170, 180, 540, 5, zones_switch, 3, obstacles_switch, 3,
     0, 0, 0, 0, 0}
};

#define DEFAULT_COUNT ((int)(sizeof(default_courses) / sizeof(default_courses[0])))
#define COURSE_COUNT course_count

/* live (editable) copy of the course tables */
static Course courses[MAX_HOLES];
static Zone zone_store[MAX_HOLES][MAX_ZONES];
static Obstacle obs_store[MAX_HOLES][MAX_OBST];
static char name_store[MAX_HOLES][NAME_LEN];
static int course_count;

/* UI palette */
static const uint32_t C_BG = 0xFF161D1A;
static const uint32_t C_PANEL = 0xFF1E2824;
static const uint32_t C_PANEL_HI = 0xFF2B3933;
static const uint32_t C_INK = 0xFFF1F3EE;
static const uint32_t C_MUTED = 0xFFA9B8AE;
static const uint32_t C_MINT = 0xFFB9E5C8;
static const uint32_t C_GOLD = 0xFFF1C56B;
static const uint32_t C_CORAL = 0xFFE78370;
static const uint32_t C_ACCENT = 0xFF3DAEE9;

/* ------------------------------------------------------------------ */
/* state                                                              */
/* ------------------------------------------------------------------ */

typedef struct { int x0, y0, x1, y1; } Rect;
#define SLOT_BALL 0
#define SLOT_GATE0 1
#define MAX_GATES 4
#define SLOT_METER (SLOT_GATE0 + MAX_GATES)
#define SLOT_COUNT (SLOT_METER + 1)

static uint32_t window_id;
static uint32_t* pixels;      /* what is on screen                    */
static uint32_t* bg;          /* pre-rendered static scene            */
static uint32_t* grass_layer; /* cached grass texture (field only)    */
static uint8_t* regular_data;
static uint8_t* medium_data;

static uint32_t* T;           /* current drawing target               */
static int clip_x0, clip_y0, clip_x1, clip_y1;

static int current_course;
static int strokes[MAX_HOLES];
static int holed[MAX_HOLES];
static int scorecard_open;
static int ball_moving;
static int sinking, sink_t;
static int power_active;
static int charging;
static int mouse_down, mouse_x, mouse_y, last_mx = -1, last_my = -1;
static int portal_cooldown;
static int need_full = 1, header_dirty = 1, ball_dirty = 1, meter_dirty = 1;
static int was_moving;
static float meter, meter_dir = 1.0f;
static float aim_x = 1.0f, aim_y = 0.0f;
static float ball_x, ball_y, ball_vx, ball_vy;
static float last_x, last_y;
static long gate_clock;
static Rect slot_prev[SLOT_COUNT], slot_cur[SLOT_COUNT];
static int editor_mode, ed_modified, ed_panel;
static void editor_render(void);
static void editor_course_changed(void);

/* ------------------------------------------------------------------ */
/* math helpers (no libm)                                             */
/* ------------------------------------------------------------------ */

static float fsqrt(float v) {
    if (v <= 0.0f) return 0.0f;
    union { float f; uint32_t i; } u;
    u.f = v;
    u.i = 0x1fbd1df5u + (u.i >> 1);
    float y = u.f;
    y = 0.5f * (y + v / y);
    y = 0.5f * (y + v / y);
    return y;
}

static float fabs_(float v) { return v < 0.0f ? -v : v; }
static int iabs(int v) { return v < 0 ? -v : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static uint32_t hash2(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* ------------------------------------------------------------------ */
/* drawing primitives (all draw into T, clipped to the clip box)      */
/* ------------------------------------------------------------------ */

static void clip_all(void) {
    clip_x0 = 0; clip_y0 = 0; clip_x1 = WIDTH; clip_y1 = HEIGHT;
}

static void clip_rect(Rect r) {
    clip_x0 = clampi(r.x0, 0, WIDTH);
    clip_y0 = clampi(r.y0, 0, HEIGHT);
    clip_x1 = clampi(r.x1, 0, WIDTH);
    clip_y1 = clampi(r.y1, 0, HEIGHT);
}

static uint32_t rgb(int r, int g, int b) {
    return 0xFF000000u | ((uint32_t)clampi(r, 0, 255) << 16) |
           ((uint32_t)clampi(g, 0, 255) << 8) | (uint32_t)clampi(b, 0, 255);
}

static uint32_t mix(uint32_t d, uint32_t s, int a) {
    a = clampi(a, 0, 255);
    int ia = 255 - a;
    uint32_t r = ((((d >> 16) & 255u) * (uint32_t)ia) + (((s >> 16) & 255u) * (uint32_t)a) + 127u) / 255u;
    uint32_t g = ((((d >> 8) & 255u) * (uint32_t)ia) + (((s >> 8) & 255u) * (uint32_t)a) + 127u) / 255u;
    uint32_t b = ((((d) & 255u) * (uint32_t)ia) + (((s) & 255u) * (uint32_t)a) + 127u) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void put(int x, int y, uint32_t c, int a) {
    if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1) return;
    uint32_t* p = &T[(size_t)y * WIDTH + (size_t)x];
    if (a >= 255) *p = c | 0xFF000000u;
    else if (a > 0) *p = mix(*p, c, a);
}

static void fill_rect(int x, int y, int w, int h, uint32_t c, int a) {
    int x0 = x < clip_x0 ? clip_x0 : x;
    int y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w;
    int y1 = y + h > clip_y1 ? clip_y1 : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    if (a >= 255) {
        uint32_t v = c | 0xFF000000u;
        for (int py = y0; py < y1; py++) {
            uint32_t* row = &T[(size_t)py * WIDTH];
            for (int px = x0; px < x1; px++) row[px] = v;
        }
    } else {
        for (int py = y0; py < y1; py++)
            for (int px = x0; px < x1; px++) put(px, py, c, a);
    }
}

/* anti-aliased filled circle */
static void disc(float cx, float cy, float r, uint32_t c, int a) {
    int x0 = (int)(cx - r - 1.0f), x1 = (int)(cx + r + 2.0f);
    int y0 = (int)(cy - r - 1.0f), y1 = (int)(cy + r + 2.0f);
    float outer = (r + 0.5f) * (r + 0.5f);
    float inner = r > 0.5f ? (r - 0.5f) * (r - 0.5f) : 0.0f;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d2 = dx * dx + dy * dy;
            if (d2 >= outer) continue;
            if (d2 <= inner) { put(x, y, c, a); continue; }
            float cov = clampf(r + 0.5f - fsqrt(d2), 0.0f, 1.0f);
            put(x, y, c, (int)(cov * (float)a));
        }
    }
}

/* disc with a radial highlight (light from the top-left) */
static void shaded_disc(float cx, float cy, float r, uint32_t hi, uint32_t lo) {
    float lx = cx - r * 0.35f, ly = cy - r * 0.4f;
    float span = r * 1.7f;
    int x0 = (int)(cx - r - 1.0f), x1 = (int)(cx + r + 2.0f);
    int y0 = (int)(cy - r - 1.0f), y1 = (int)(cy + r + 2.0f);
    float outer = (r + 0.5f) * (r + 0.5f);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d2 = dx * dx + dy * dy;
            if (d2 >= outer) continue;
            float cov = 1.0f;
            if (d2 > (r - 0.5f) * (r - 0.5f))
                cov = clampf(r + 0.5f - fsqrt(d2), 0.0f, 1.0f);
            float hx = (float)x + 0.5f - lx, hy = (float)y + 0.5f - ly;
            float t = clampf(fsqrt(hx * hx + hy * hy) / span, 0.0f, 1.0f);
            put(x, y, mix(hi, lo, (int)(t * 255.0f)), (int)(cov * 255.0f));
        }
    }
}

static void ring(float cx, float cy, float r, float w, uint32_t c, int a) {
    float ro = r + w * 0.5f + 1.0f;
    int x0 = (int)(cx - ro), x1 = (int)(cx + ro + 1.0f);
    int y0 = (int)(cy - ro), y1 = (int)(cy + ro + 1.0f);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d = fsqrt(dx * dx + dy * dy);
            float cov = clampf(w * 0.5f + 0.5f - fabs_(d - r), 0.0f, 1.0f);
            if (cov > 0.0f) put(x, y, c, (int)(cov * (float)a));
        }
    }
}

/* anti-aliased thick segment with round caps (row-wise, cheap for long diagonals) */
static void capsule(float x0, float y0, float x1, float y1, float w,
                    uint32_t c, int a) {
    float hw = w * 0.5f;
    float pad = hw + 1.0f;
    float vx = x1 - x0, vy = y1 - y0;
    float len2 = vx * vx + vy * vy;
    float lim = pad * pad;
    float miny = (y0 < y1 ? y0 : y1) - pad, maxy = (y0 < y1 ? y1 : y0) + pad;
    for (int y = (int)(miny - 1.0f); y <= (int)(maxy + 1.0f); y++) {
        float py = (float)y + 0.5f;
        float xa, xb;
        if (fabs_(vy) < 0.001f) {
            if (fabs_(py - y0) > pad) continue;
            xa = (x0 < x1 ? x0 : x1) - pad;
            xb = (x0 < x1 ? x1 : x0) + pad;
        } else {
            float t0 = (py - pad - y0) / vy, t1 = (py + pad - y0) / vy;
            if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }
            t0 = clampf(t0, 0.0f, 1.0f);
            t1 = clampf(t1, 0.0f, 1.0f);
            float xs = x0 + t0 * vx, xe = x0 + t1 * vx;
            xa = (xs < xe ? xs : xe) - pad;
            xb = (xs < xe ? xe : xs) + pad;
        }
        for (int x = (int)(xa - 1.0f); x <= (int)(xb + 1.0f); x++) {
            float px = (float)x + 0.5f;
            float t = len2 > 0.0001f ? ((px - x0) * vx + (py - y0) * vy) / len2 : 0.0f;
            t = clampf(t, 0.0f, 1.0f);
            float qx = px - (x0 + t * vx), qy = py - (y0 + t * vy);
            float d2 = qx * qx + qy * qy;
            if (d2 > lim) continue;
            float cov = clampf(hw + 0.5f - fsqrt(d2), 0.0f, 1.0f);
            if (cov > 0.0f) put(x, y, c, (int)(cov * (float)a));
        }
    }
}

/* signed distance to a rounded rectangle (negative = inside) */
static float rr_dist(float px, float py, int x, int y, int w, int h, float r) {
    float cx = (float)x + (float)w * 0.5f, cy = (float)y + (float)h * 0.5f;
    float qx = fabs_(px - cx) - ((float)w * 0.5f - r);
    float qy = fabs_(py - cy) - ((float)h * 0.5f - r);
    float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
    float outside = (ox > 0.0f || oy > 0.0f) ? fsqrt(ox * ox + oy * oy) : 0.0f;
    float m = qx > qy ? qx : qy;
    float inside = m < 0.0f ? m : 0.0f;
    return outside + inside - r;
}

/* ------------------------------------------------------------------ */
/* text                                                               */
/* ------------------------------------------------------------------ */

static void text(int x, int y, const char* value, uint32_t color, uint32_t scale) {
    moswm_draw_text(T, WIDTH, HEIGHT, WIDTH, x, y, value, color, TEXT_BG, scale);
}

static void centered_text(int x, int y, int width, const char* value,
                          uint32_t color, uint32_t scale) {
    int text_width = (int)moswm_text_width(value, scale);
    text(x + (width - text_width) / 2, y, value, color, scale);
}

static void number_text(int x, int y, int value, uint32_t color, uint32_t scale) {
    char digits[16];
    char reverse[16];
    int count = 0;
    int negative = value < 0;
    unsigned int magnitude = negative ? 0u - (unsigned int)value : (unsigned int)value;
    do {
        reverse[count++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude && count < 14);
    int out = 0;
    if (negative) digits[out++] = '-';
    for (int i = 0; i < count; i++) digits[out++] = reverse[count - i - 1];
    digits[out] = '\0';
    text(x, y, digits, color, scale);
}

static int load_font(const char* path, uint8_t** out_data, size_t* out_size) {
    long fd = mos_open(path, 0);
    if (fd < 0) return 0;
    long size = mos_size(fd);
    if (size <= 0 || (uint64_t)size > FONT_LIMIT) {
        mos_close(fd);
        return 0;
    }
    uint8_t* data = (uint8_t*)malloc((size_t)size);
    if (!data) {
        mos_close(fd);
        return 0;
    }
    size_t read_total = 0;
    while (read_total < (size_t)size) {
        long got = mos_read(fd, data + read_total, (size_t)size - read_total);
        if (got <= 0) {
            free(data);
            mos_close(fd);
            return 0;
        }
        read_total += (size_t)got;
    }
    mos_close(fd);
    *out_data = data;
    *out_size = read_total;
    return 1;
}

/* ------------------------------------------------------------------ */
/* textures                                                           */
/* ------------------------------------------------------------------ */

static void build_grass_layer(void) {
    for (int ly = 0; ly < FIELD_H; ly++) {
        int band = (ly / 36) & 1;
        for (int lx = 0; lx < FIELD_W; lx++) {
            uint32_t h = hash2(lx, ly);
            int n = (int)(h & 15u) - 8;
            int r = 66 + band * 4 + n / 3;
            int g = 158 + band * 7 + n;
            int b = 56 + n / 3;
            uint32_t hb = hash2(lx, ly >> 2);
            if ((hb & 15u) == 0) { g -= 15; r -= 7; }
            else if (((hb >> 4) & 31u) == 0) { g += 12; r += 4; }
            grass_layer[(size_t)ly * FIELD_W + (size_t)lx] = rgb(r, g, b);
        }
    }
}

static void wall_shadow(int x, int y, int w, int h) {
    fill_rect(x + 3, y + 4, w, h, rgb(0, 30, 0), 70);
}

/* wooden wall with bevelled edges */
static void wall_body(int x, int y, int w, int h, int gate) {
    int vertical = h > w;
    int x0 = x < clip_x0 ? clip_x0 : x;
    int y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w;
    int y1 = y + h > clip_y1 ? clip_y1 : y + h;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            int g = (int)(hash2(vertical ? px : py, vertical ? (py >> 4) : (px >> 4)) & 15u) - 8;
            int r = gate ? 208 : 128, gg = gate ? 140 : 84, b = gate ? 64 : 44;
            uint32_t c = rgb(r + g * 2, gg + g * 2, b + g);
            int left = px - x, right = x + w - 1 - px;
            int top = py - y, bottom = y + h - 1 - py;
            int edge = left < right ? left : right;
            int e2 = top < bottom ? top : bottom;
            if (e2 < edge) edge = e2;
            if (edge == 0) c = gate ? rgb(110, 60, 20) : rgb(50, 30, 15);
            else if (top == 1 || left == 1) c = gate ? rgb(244, 196, 120) : rgb(188, 138, 84);
            else if (bottom == 1 || right == 1) c = gate ? rgb(160, 96, 36) : rgb(92, 58, 28);
            put(px, py, c, 255);
        }
    }
}

static void draw_bumper(int x, int y, int r) {
    disc((float)x + 3.0f, (float)y + 5.0f, (float)r, rgb(0, 30, 0), 80);
    shaded_disc((float)x, (float)y, (float)r, rgb(238, 116, 88), rgb(136, 34, 32));
    ring((float)x, (float)y, (float)r - 0.5f, 2.0f, rgb(92, 22, 20), 255);
    shaded_disc((float)x - 1.0f, (float)y - 1.0f, (float)r * 0.5f,
                rgb(255, 232, 140), rgb(212, 148, 50));
    ring((float)x, (float)y, (float)r * 0.5f, 1.2f, rgb(120, 70, 20), 200);
}

static void draw_portal(int x, int y, int r) {
    disc((float)x, (float)y, (float)r + 3.0f, rgb(40, 70, 160), 70);
    disc((float)x, (float)y, (float)r, rgb(30, 44, 104), 255);
    disc((float)x, (float)y, (float)r * 0.72f, rgb(16, 22, 60), 255);
    disc((float)x, (float)y, (float)r * 0.42f, rgb(6, 8, 24), 255);
    ring((float)x, (float)y, (float)r, 2.5f, rgb(130, 180, 255), 255);
    ring((float)x, (float)y, (float)r * 0.72f, 1.2f, rgb(96, 140, 240), 170);
    ring((float)x, (float)y, (float)r * 0.42f, 1.0f, rgb(80, 110, 210), 140);
}

static void draw_wall_segment(const Obstacle* o) {
    float x0 = (float)o->x, y0 = (float)o->y, x1 = (float)o->w, y1 = (float)o->h;
    capsule(x0, y0, x1, y1, WALL_THICK + 2.0f, rgb(50, 30, 15), 255);
    capsule(x0, y0, x1, y1, WALL_THICK - 0.5f, rgb(128, 84, 44), 255);
    capsule(x0 - 0.8f, y0 - 1.0f, x1 - 0.8f, y1 - 1.0f, WALL_THICK * 0.38f,
            rgb(184, 134, 80), 200);
    capsule(x0 + 1.2f, y0 + 1.8f, x1 + 1.2f, y1 + 1.8f, WALL_THICK * 0.22f,
            rgb(92, 58, 28), 160);
    disc(x0, y0, 2.0f, rgb(210, 170, 116), 200);
    disc(x1, y1, 2.0f, rgb(210, 170, 116), 200);
}

static void draw_hole(int x, int y) {
    float r = (float)HOLE_RADIUS;
    disc((float)x, (float)y, r + 3.0f, rgb(20, 70, 24), 80);
    disc((float)x, (float)y, r + 1.4f, rgb(176, 206, 156), 120);
    disc((float)x, (float)y, r, rgb(44, 44, 44), 255);
    disc((float)x + 0.5f, (float)y + 1.8f, r - 1.6f, rgb(8, 8, 8), 255);
}

static float zu_x, zu_y, zu_half, zu_k;

static uint32_t zone_color(const Zone* z, int x, int y, float depth) {
    uint32_t h = hash2(x, y);
    int n = (int)(h & 15u) - 8;
    uint32_t c;
    if (z->type == ZONE_SAND) {
        int r = 226 + n, g = 200 + n, b = 134 + n;
        if (((h >> 8) & 31u) == 0) { r -= 34; g -= 36; b -= 34; }
        else if (((h >> 14) & 63u) == 0) { r += 14; g += 16; b += 22; }
        c = rgb(r, g, b);
        if (depth < 3.0f) c = mix(c, rgb(160, 128, 70), (int)((3.0f - depth) * 55.0f));
    } else if (z->type == ZONE_WATER) {
        int t = (y - z->y) * 255 / (z->h > 0 ? z->h : 1);
        c = mix(rgb(58, 138, 212), rgb(28, 88, 168), t);
        int tri = iabs(((x * 2 + (y >> 2) * 3) & 63) - 32);
        int band = (y + tri / 6) % 18;
        if (band < 2) c = mix(c, rgb(150, 206, 244), 120);
        if ((h & 127u) == 0) c = mix(c, rgb(210, 236, 252), 150);
        if (depth < 2.5f) c = mix(c, rgb(196, 228, 246), (int)((2.5f - depth) * 70.0f));
    } else if (z->type == ZONE_ROUGH) {
        int r = 52 + n / 2, g = 128 + n, b = 48 + n / 2;
        uint32_t hb = hash2(x, y >> 3);
        if ((hb & 7u) == 0) { g -= 22; r -= 10; }
        else if (((hb >> 5) & 7u) == 0) { g += 16; r += 6; }
        c = rgb(r, g, b);
        if (depth < 1.5f) c = mix(c, rgb(28, 84, 32), (int)((1.5f - depth) * 110.0f));
    } else {
        float px = (float)x + 0.5f - ((float)z->x + (float)z->w * 0.5f);
        float py = (float)y + 0.5f - ((float)z->y + (float)z->h * 0.5f);
        float t = clampf((px * zu_x + py * zu_y) / zu_half, -1.0f, 1.0f);
        int r = 88 + (int)(t * zu_k * 0.85f) + n / 2;
        int g = 176 + (int)(t * zu_k) + n;
        int b = 76 + n / 2;
        c = rgb(r, g, b);
        if (depth < 1.5f) c = mix(c, rgb(50, 120, 46), (int)((1.5f - depth) * 110.0f));
    }
    return c;
}

static void chevron(float fx, float fy, float ux, float uy, float size, int alpha) {
    float px = -uy, py = ux;
    float tx = fx + ux * size * 0.9f, ty = fy + uy * size * 0.9f;
    float bx = fx - ux * size * 0.6f, by = fy - uy * size * 0.6f;
    uint32_t col = rgb(176, 232, 136);
    capsule(bx + px * size, by + py * size, tx, ty, 2.2f, col, alpha);
    capsule(bx - px * size, by - py * size, tx, ty, 2.2f, col, alpha);
}

static void hill_ring(float cx, float cy, float rad, int n, float sign,
                      float size, int alpha) {
    float c = 0.8660254f, s = 0.5f;                 /* 12 arrows */
    if (n == 4) { c = 0.0f; s = 1.0f; }
    else if (n == 6) { c = 0.5f; s = 0.8660254f; }
    else if (n == 8) { c = 0.7071068f; s = 0.7071068f; }
    float ux = 1.0f, uy = 0.0f;
    for (int i = 0; i < n; i++) {
        chevron(cx + ux * rad, cy + uy * rad, ux * sign, uy * sign, size, alpha);
        float nx = ux * c - uy * s, ny = ux * s + uy * c;
        ux = nx; uy = ny;
    }
}

/* circle hill (amount > 0) or dip (amount < 0); steeper = stronger shading */
static void paint_hill(const Zone* z) {
    float amount = (float)z->slope_x;
    float strength = clampf(fabs_(amount), 0.0f, 48.0f);
    float sign = amount >= 0.0f ? 1.0f : -1.0f;
    float r = (float)z->w * 0.5f;
    float cx = (float)z->x + r, cy = (float)z->y + r;
    float k = 12.0f + strength * 1.3f;
    for (int y = z->y - 1; y <= z->y + z->w + 1; y++) {
        for (int x = z->x - 1; x <= z->x + z->w + 1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d2 = dx * dx + dy * dy;
            if (d2 >= (r + 0.5f) * (r + 0.5f)) continue;
            float d = fsqrt(d2);
            float cov = clampf(r + 0.5f - d, 0.0f, 1.0f);
            float q = d / r;
            float rx = d > 0.01f ? dx / d : 0.0f, ry = d > 0.01f ? dy / d : 0.0f;
            float bright = -(rx + ry) * 0.7071f * sign * q * k;
            int n = (int)(hash2(x, y) & 7u) - 4;
            int cr = 90 + (int)(bright * 0.85f) + n / 2;
            int cg = 178 + (int)bright + n;
            int cb = 78 + (int)(bright * 0.6f) + n / 2;
            uint32_t c = rgb(cr, cg, cb);
            float f = q * 4.0f - (float)(int)(q * 4.0f);
            if (f < 0.07f && q > 0.05f) c = mix(c, rgb(36, 104, 42), 60);
            float depth = r - d;
            if (depth < 1.6f) c = mix(c, rgb(46, 112, 44), (int)((1.6f - depth) * 110.0f));
            put(x, y, c, (int)(cov * 255.0f));
        }
    }
    float size = 5.0f + strength * 0.12f;
    int alpha = 110 + (int)(strength * 2.0f);
    if (r >= 55.0f) hill_ring(cx, cy, r * 0.42f, 6, sign, size, alpha);
    hill_ring(cx, cy, r * 0.80f, r >= 70.0f ? 12 : 8, sign, size, alpha);
}

static void paint_zone(const Zone* z) {
    if (z->type == ZONE_HILL) { paint_hill(z); return; }
    float radius = z->type == ZONE_WATER ? 18.0f : (z->type == ZONE_SAND ? 16.0f : 10.0f);
    if (z->type == ZONE_SLOPE) {
        float sl = fsqrt((float)(z->slope_x * z->slope_x + z->slope_y * z->slope_y));
        if (sl < 0.01f) sl = 1.0f;
        zu_x = (float)z->slope_x / sl;
        zu_y = (float)z->slope_y / sl;
        zu_half = (float)z->w * 0.5f * fabs_(zu_x) + (float)z->h * 0.5f * fabs_(zu_y);
        if (zu_half < 1.0f) zu_half = 1.0f;
        zu_k = 4.0f + clampf(sl, 0.0f, 48.0f) * 0.7f;
    }
    for (int y = z->y - 1; y <= z->y + z->h; y++) {
        for (int x = z->x - 1; x <= z->x + z->w; x++) {
            float d = rr_dist((float)x + 0.5f, (float)y + 0.5f, z->x, z->y, z->w, z->h, radius);
            if (d > 0.5f) continue;
            float cov = clampf(0.5f - d, 0.0f, 1.0f);
            put(x, y, zone_color(z, x, y, -d), (int)(cov * 255.0f));
        }
    }
    if (z->type == ZONE_SLOPE) {
        float sl = fsqrt((float)(z->slope_x * z->slope_x + z->slope_y * z->slope_y));
        float strength = clampf(sl, 0.0f, 48.0f);
        float size = 5.0f + strength * 0.12f;
        int alpha = 110 + (int)(strength * 2.0f);
        int pitch = clampi(62 - (int)strength, 34, 56);     /* steeper = denser arrows */
        int row = 0;
        for (int cy = z->y + pitch / 2 + 4; cy < z->y + z->h - 10; cy += pitch, row++) {
            int stagger = (row & 1) ? pitch / 2 : 0;
            for (int cx = z->x + 20; cx < z->x + z->w - 14; cx += pitch) {
                float fx = (float)(cx + stagger);
                if (fx > (float)(z->x + z->w - 14)) continue;
                chevron(fx, (float)cy, zu_x, zu_y, size, alpha);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* static HUD                                                         */
/* ------------------------------------------------------------------ */

static void draw_button(int x, int y, int w, int h, const char* label, int selected) {
    uint32_t face = selected ? C_ACCENT : C_PANEL_HI;
    fill_rect(x, y, w, h, face, 255);
    fill_rect(x, y, w, 1, selected ? 0xFF8AD0F5 : 0xFF40514A, 255);
    fill_rect(x, y + h - 2, w, 2, selected ? 0xFF1F7FB3 : 0xFF121A16, 255);
    centered_text(x, y + (h - 16) / 2, w, label, selected ? 0xFF0E1A14 : C_INK, 1);
}

static void draw_header(void) {
    fill_rect(0, 0, WIDTH, 72, C_PANEL, 255);
    fill_rect(0, 70, WIDTH, 2, 0xFF33453C, 255);
    fill_rect(0, 72, WIDTH, FIELD_Y - FRAME - 72, C_BG, 255);
    text(24, 12, "GREENLINE", C_MINT, 2);
    text(26, 42, "MINI GOLF CLUB", C_MUTED, 1);
    for (int i = 0; i < COURSE_COUNT; i++) {
        char label[3] = {(char)('0' + i + 1), '\0', '\0'};
        draw_button(COURSE_BTN_X + i * COURSE_BTN_STEP, 20, COURSE_BTN_W, 36, label, i == current_course);
    }
    draw_button(EDIT_BTN_X, 20, EDIT_BTN_W, 36, editor_mode ? "PLAY" : "EDIT", editor_mode);
    if (editor_mode) {
        draw_button(624, 20, 88, 36, ed_modified ? "SAVE *" : "SAVE", 0);
        draw_button(722, 20, 126, 36, "HOLE", ed_panel);
        return;
    }
    draw_button(624, 20, 88, 36, "REPLAY", 0);
    draw_button(722, 20, 126, 36, "SCORECARD", scorecard_open);

    const Course* course = &courses[current_course];
    text(FIELD_X, 76, course->name, C_INK, 2);

    char parline[8] = "PAR ";
    parline[4] = (char)('0' + course->par);
    parline[5] = '\0';
    text(720, 84, parline, C_GOLD, 1);
    text(795, 84, "STROKES", C_MUTED, 1);
    number_text(858, 84, strokes[current_course], C_INK, 1);
}

static void draw_statusbar(void) {
    fill_rect(FIELD_X, BAR_Y, FIELD_W, 30, C_PANEL, 255);
    fill_rect(FIELD_X, BAR_Y, FIELD_W, 1, 0xFF33453C, 255);
    text(FIELD_X + 14, BAR_Y + 7, "AIM WITH MOUSE   HOLD TO CHARGE   RELEASE TO PUTT   ESC CANCEL",
         C_MUTED, 1);
    text(MET_X - 62, BAR_Y + 7, "POWER", C_MINT, 1);
    fill_rect(MET_X - 2, MET_Y - 2, MET_W + 4, MET_H + 4, 0xFF0C120F, 255);
    fill_rect(MET_X - 1, MET_Y - 1, MET_W + 2, MET_H + 2, 0xFF3A4A42, 255);
    fill_rect(MET_X, MET_Y, MET_W, MET_H, 0xFF1A2420, 255);
}

/* ------------------------------------------------------------------ */
/* static scene (rendered once per hole)                              */
/* ------------------------------------------------------------------ */

static void build_bg(void) {
    T = bg;
    clip_all();
    fill_rect(0, 0, WIDTH, HEIGHT, C_BG, 255);
    draw_header();
    draw_statusbar();

    int fx = FIELD_X - FRAME, fy = FIELD_Y - FRAME;
    int fw = FIELD_W + 2 * FRAME, fh = FIELD_H + 2 * FRAME;
    fill_rect(fx + 4, fy + 6, fw, fh, rgb(0, 0, 0), 90);
    fill_rect(fx, fy, fw, fh, rgb(128, 84, 44), 255);
    for (int i = 0; i < FRAME; i++) {
        int g = (int)(hash2(i, 7) & 7u) - 4;
        uint32_t c = rgb(128 + g * 3, 84 + g * 3, 44 + g * 2);
        fill_rect(fx, fy + i, fw, 1, c, 255);
        fill_rect(fx, fy + fh - 1 - i, fw, 1, c, 255);
        fill_rect(fx + i, fy + FRAME, 1, fh - 2 * FRAME, c, 255);
        fill_rect(fx + fw - 1 - i, fy + FRAME, 1, fh - 2 * FRAME, c, 255);
    }
    fill_rect(fx + 1, fy + 1, fw - 2, 1, rgb(188, 138, 84), 255);
    fill_rect(fx + 1, fy + 1, 1, fh - 2, rgb(188, 138, 84), 255);
    fill_rect(fx + 1, fy + fh - 2, fw - 2, 1, rgb(92, 58, 28), 255);
    fill_rect(fx + fw - 2, fy + 1, 1, fh - 2, rgb(92, 58, 28), 255);
    fill_rect(fx, fy, fw, 1, rgb(50, 30, 15), 255);
    fill_rect(fx, fy + fh - 1, fw, 1, rgb(50, 30, 15), 255);
    fill_rect(fx, fy, 1, fh, rgb(50, 30, 15), 255);
    fill_rect(fx + fw - 1, fy, 1, fh, rgb(50, 30, 15), 255);

    for (int y = 0; y < FIELD_H; y++)
        for (int x = 0; x < FIELD_W; x++)
            bg[(size_t)(FIELD_Y + y) * WIDTH + (size_t)(FIELD_X + x)] =
                grass_layer[(size_t)y * FIELD_W + (size_t)x];
    for (int i = 0; i < 5; i++) {
        int a = 80 - i * 15;
        fill_rect(FIELD_X, FIELD_Y + i, FIELD_W, 1, rgb(0, 30, 0), a);
        fill_rect(FIELD_X + i, FIELD_Y, 1, FIELD_H, rgb(0, 30, 0), a);
    }

    const Course* course = &courses[current_course];
    for (int i = 0; i < course->zone_count; i++) paint_zone(&course->zones[i]);

    /* tee marker */
    disc((float)course->tee_x, (float)course->tee_y, 15.0f, rgb(255, 255, 255), 26);
    ring((float)course->tee_x, (float)course->tee_y, 15.0f, 1.5f, rgb(255, 255, 255), 110);

    if (course->portal_radius) {
        draw_portal(course->portal_ax, course->portal_ay, course->portal_radius);
        draw_portal(course->portal_bx, course->portal_by, course->portal_radius);
    }

    draw_hole(course->hole_x, course->hole_y);

    for (int i = 0; i < course->obstacle_count; i++) {
        const Obstacle* o = &course->obstacles[i];
        if (o->type == OBSTACLE_BLOCK) wall_shadow(o->x, o->y, o->w, o->h);
        else if (o->type == OBSTACLE_WALL)
            capsule((float)o->x + 3.0f, (float)o->y + 4.0f, (float)o->w + 3.0f,
                    (float)o->h + 4.0f, WALL_THICK, rgb(0, 30, 0), 70);
    }
    for (int i = 0; i < course->obstacle_count; i++) {
        const Obstacle* o = &course->obstacles[i];
        if (o->type == OBSTACLE_BLOCK) wall_body(o->x, o->y, o->w, o->h, 0);
        else if (o->type == OBSTACLE_BUMPER) draw_bumper(o->x, o->y, o->radius);
        else if (o->type == OBSTACLE_WALL) draw_wall_segment(o);
    }
}

/* ------------------------------------------------------------------ */
/* game helpers                                                       */
/* ------------------------------------------------------------------ */

static int object_x(const Obstacle* obstacle, long clock) {
    if (obstacle->type != OBSTACLE_GATE || obstacle->travel <= 0)
        return obstacle->x;
    int period = obstacle->travel * 2;
    int phase = (int)((clock / 10 + obstacle->phase) % period);
    int offset = phase <= obstacle->travel ? phase : period - phase;
    return obstacle->x + offset;
}

static int inside_rect(float x, float y, int rx, int ry, int rw, int rh) {
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static int active_zone(float x, float y, int type, const Zone** found) {
    const Course* course = &courses[current_course];
    for (int i = 0; i < course->zone_count; i++) {
        const Zone* zone = &course->zones[i];
        if (zone->type == type && inside_rect(x, y, zone->x, zone->y, zone->w, zone->h)) {
            if (found) *found = zone;
            return 1;
        }
    }
    return 0;
}

static void aim_at(int x, int y) {
    float dx = (float)x - ball_x, dy = (float)y - ball_y;
    float l = fsqrt(dx * dx + dy * dy);
    if (l < 4.0f) return;
    aim_x = dx / l;
    aim_y = dy / l;
}

static void rotate_aim(int dir) {
    const float c = 0.99863f, s = 0.05234f * (float)dir;
    float nx = aim_x * c - aim_y * s;
    float ny = aim_x * s + aim_y * c;
    float l = fsqrt(nx * nx + ny * ny);
    aim_x = nx / l;
    aim_y = ny / l;
    ball_dirty = 1;
}

static void set_ball_to_tee(void) {
    const Course* course = &courses[current_course];
    ball_x = (float)course->tee_x;
    ball_y = (float)course->tee_y;
    last_x = ball_x;
    last_y = ball_y;
    ball_vx = ball_vy = 0.0f;
    ball_moving = 0;
    sinking = 0;
    power_active = 0;
    meter = 0.0f;
    portal_cooldown = 0;
    aim_x = (float)(course->hole_x - course->tee_x);
    aim_y = (float)(course->hole_y - course->tee_y);
    float l = fsqrt(aim_x * aim_x + aim_y * aim_y);
    if (l < 1.0f) { aim_x = 1.0f; aim_y = 0.0f; l = 1.0f; }
    aim_x /= l;
    aim_y /= l;
    ball_dirty = meter_dirty = 1;
}

static void select_course(int index) {
    if (index < 0 || index >= COURSE_COUNT) return;
    current_course = index;
    scorecard_open = 0;
    gate_clock = 0;
    set_ball_to_tee();
    build_bg();
    need_full = 1;
    editor_course_changed();
}

static void restart_course(void) {
    strokes[current_course] = 0;
    holed[current_course] = 0;
    gate_clock = 0;
    set_ball_to_tee();
    header_dirty = 1;
    need_full = 1;
}

/* ------------------------------------------------------------------ */
/* physics                                                            */
/* ------------------------------------------------------------------ */

/* total push on the ball from every slope / hill it is on (px/s^2) */
static float slope_accel(float x, float y, float* ax, float* ay) {
    const Course* course = &courses[current_course];
    *ax = *ay = 0.0f;
    for (int i = 0; i < course->zone_count; i++) {
        const Zone* z = &course->zones[i];
        if (z->type == ZONE_SLOPE) {
            if (inside_rect(x, y, z->x, z->y, z->w, z->h)) {
                *ax += (float)z->slope_x * SLOPE_ACCEL;
                *ay += (float)z->slope_y * SLOPE_ACCEL;
            }
        } else if (z->type == ZONE_HILL) {
            float r = (float)z->w * 0.5f;
            float dx = x - ((float)z->x + r), dy = y - ((float)z->y + r);
            float d2 = dx * dx + dy * dy;
            if (d2 < r * r && d2 > 0.0001f) {
                float d = fsqrt(d2);
                float mag = (float)z->slope_x * SLOPE_ACCEL * (d / r);  /* flat on top, steeper at the rim */
                *ax += dx / d * mag;
                *ay += dy / d * mag;
            }
        }
    }
    return fsqrt(*ax * *ax + *ay * *ay);
}

static void hit_wall(const Obstacle* o, float* vx, float* vy) {
    float x0 = (float)o->x, y0 = (float)o->y;
    float sx = (float)o->w - x0, sy = (float)o->h - y0;
    float len2 = sx * sx + sy * sy;
    float t = len2 > 0.0001f ? ((ball_x - x0) * sx + (ball_y - y0) * sy) / len2 : 0.0f;
    t = clampf(t, 0.0f, 1.0f);
    float cx = x0 + t * sx, cy = y0 + t * sy;
    float dx = ball_x - cx, dy = ball_y - cy;
    float min_d = (float)BALL_RADIUS + WALL_THICK * 0.5f;
    float d2 = dx * dx + dy * dy;
    if (d2 >= min_d * min_d) return;
    float nx, ny;
    if (d2 > 0.0001f) {
        float d = fsqrt(d2);
        nx = dx / d; ny = dy / d;
    } else {
        float l = fsqrt(len2);
        nx = l > 0.01f ? -sy / l : 1.0f;
        ny = l > 0.01f ? sx / l : 0.0f;
    }
    ball_x = cx + nx * min_d;
    ball_y = cy + ny * min_d;
    float normal_speed = *vx * nx + *vy * ny;
    if (normal_speed < 0.0f) {
        *vx -= 1.8f * normal_speed * nx;
        *vy -= 1.8f * normal_speed * ny;
    }
}

static void hit_rect(float rx0, float ry0, float rw, float rh, float svx,
                     float* vx, float* vy) {
    float x = ball_x, y = ball_y;
    float px = x < rx0 ? rx0 : (x > rx0 + rw ? rx0 + rw : x);
    float py = y < ry0 ? ry0 : (y > ry0 + rh ? ry0 + rh : y);
    float dx = x - px, dy = y - py;
    float d2 = dx * dx + dy * dy;
    if (d2 >= (float)(BALL_RADIUS * BALL_RADIUS)) return;
    float nx, ny, distance;
    if (d2 < 0.01f) {
        float left = x - rx0, right = rx0 + rw - x;
        float top = y - ry0, bottom = ry0 + rh - y;
        float shortest = left;
        nx = -1.0f; ny = 0.0f;
        if (right < shortest) { shortest = right; nx = 1.0f; ny = 0.0f; }
        if (top < shortest) { shortest = top; nx = 0.0f; ny = -1.0f; }
        if (bottom < shortest) { shortest = bottom; nx = 0.0f; ny = 1.0f; }
        distance = -shortest;
    } else {
        distance = fsqrt(d2);
        nx = dx / distance;
        ny = dy / distance;
    }
    float overlap = (float)BALL_RADIUS - distance;
    ball_x += nx * overlap;
    ball_y += ny * overlap;
    float rvx = *vx - svx;
    float normal_speed = rvx * nx + *vy * ny;
    if (normal_speed < 0.0f) {
        *vx -= 1.8f * normal_speed * nx;
        *vy -= 1.8f * normal_speed * ny;
    }
}

static void hit_bumper(const Obstacle* o, float* vx, float* vy) {
    float dx = ball_x - (float)o->x, dy = ball_y - (float)o->y;
    float min_d = (float)(BALL_RADIUS + o->radius);
    float d2 = dx * dx + dy * dy;
    if (d2 >= min_d * min_d) return;
    float d = fsqrt(d2);
    float nx = d > 0.01f ? dx / d : 1.0f;
    float ny = d > 0.01f ? dy / d : 0.0f;
    ball_x = (float)o->x + nx * min_d;
    ball_y = (float)o->y + ny * min_d;
    float normal_speed = *vx * nx + *vy * ny;
    if (normal_speed < 0.0f) {
        /* perfectly bouncy, plus a little kick like a pinball bumper */
        *vx -= 2.0f * normal_speed * nx;
        *vy -= 2.0f * normal_speed * ny;
        *vx += nx * 55.0f;
        *vy += ny * 55.0f;
    }
}

static void finish_hole(void) {
    ball_moving = 0;
    sinking = 0;
    ball_vx = ball_vy = 0.0f;
    holed[current_course] = 1;
    power_active = 0;
    header_dirty = 1;
    need_full = 1;
}

static void water_penalty(void) {
    strokes[current_course]++;
    ball_x = last_x;
    ball_y = last_y;
    ball_vx = ball_vy = 0.0f;
    ball_moving = 0;
    header_dirty = 1;
    ball_dirty = 1;
}

static void step_ball(void) {
    const Course* course = &courses[current_course];
    const float dt = 1.0f / (60.0f * (float)SUBSTEPS);
    float sax, say;
    float slope_mag = slope_accel(ball_x, ball_y, &sax, &say);
    ball_vx += sax * dt;
    ball_vy += say * dt;

    ball_x += ball_vx * dt;
    ball_y += ball_vy * dt;

    float min_x = FIELD_X + BALL_RADIUS, max_x = FIELD_X + FIELD_W - BALL_RADIUS;
    float min_y = FIELD_Y + BALL_RADIUS, max_y = FIELD_Y + FIELD_H - BALL_RADIUS;
    if (ball_x < min_x) { ball_x = min_x; if (ball_vx < 0) ball_vx = -ball_vx * 0.8f; }
    if (ball_x > max_x) { ball_x = max_x; if (ball_vx > 0) ball_vx = -ball_vx * 0.8f; }
    if (ball_y < min_y) { ball_y = min_y; if (ball_vy < 0) ball_vy = -ball_vy * 0.8f; }
    if (ball_y > max_y) { ball_y = max_y; if (ball_vy > 0) ball_vy = -ball_vy * 0.8f; }

    for (int i = 0; i < course->obstacle_count; i++) {
        const Obstacle* o = &course->obstacles[i];
        if (o->type == OBSTACLE_BUMPER) {
            hit_bumper(o, &ball_vx, &ball_vy);
        } else if (o->type == OBSTACLE_GATE) {
            float gx = (float)object_x(o, gate_clock);
            float gv = (float)(object_x(o, gate_clock + 16) - object_x(o, gate_clock)) / 0.016f;
            hit_rect(gx, (float)o->y, (float)o->w, (float)o->h, gv, &ball_vx, &ball_vy);
        } else if (o->type == OBSTACLE_WALL) {
            hit_wall(o, &ball_vx, &ball_vy);
        } else {
            hit_rect((float)o->x, (float)o->y, (float)o->w, (float)o->h, 0.0f,
                     &ball_vx, &ball_vy);
        }
    }

    if (active_zone(ball_x, ball_y, ZONE_WATER, NULL)) {
        water_penalty();
        return;
    }

    if (course->portal_radius && portal_cooldown == 0) {
        float ax = ball_x - (float)course->portal_ax, ay = ball_y - (float)course->portal_ay;
        float bx = ball_x - (float)course->portal_bx, by = ball_y - (float)course->portal_by;
        float rsq = (float)(course->portal_radius * course->portal_radius);
        if (ax * ax + ay * ay < rsq) {
            ball_x = (float)course->portal_bx;
            ball_y = (float)course->portal_by;
            portal_cooldown = 24 * SUBSTEPS;
        } else if (bx * bx + by * by < rsq) {
            ball_x = (float)course->portal_ax;
            ball_y = (float)course->portal_ay;
            portal_cooldown = 24 * SUBSTEPS;
        }
    }
    if (portal_cooldown > 0) portal_cooldown--;

    /* the cup: slow balls get pulled in, fast balls roll over it */
    float hx = (float)course->hole_x - ball_x, hy = (float)course->hole_y - ball_y;
    float hd2 = hx * hx + hy * hy;
    float speed = fsqrt(ball_vx * ball_vx + ball_vy * ball_vy);
    if (hd2 < (float)((HOLE_RADIUS + 3) * (HOLE_RADIUS + 3)) && speed < 280.0f) {
        float hd = fsqrt(hd2);
        if (hd > 0.1f) {
            ball_vx += hx / hd * 520.0f * dt;
            ball_vy += hy / hd * 520.0f * dt;
        }
        if (hd2 < 64.0f && speed < 260.0f) {
            sinking = 1;
            sink_t = 0;
            ball_moving = 0;
            ball_vx = ball_vy = 0.0f;
            return;
        }
    }

    float friction = 0.99298f;                                 /* grass */
    if (active_zone(ball_x, ball_y, ZONE_SAND, NULL)) friction = 0.97724f;
    else if (active_zone(ball_x, ball_y, ZONE_ROUGH, NULL)) friction = 0.98742f;
    ball_vx *= friction;
    ball_vy *= friction;
    if (slope_mag < SLOPE_REST && fsqrt(ball_vx * ball_vx + ball_vy * ball_vy) < 6.0f) {
        ball_vx = ball_vy = 0.0f;
        ball_moving = 0;
    }
}

static void start_power(void) {
    if (ball_moving || sinking || holed[current_course] || scorecard_open) return;
    power_active = 1;
    meter = 0.0f;
    meter_dir = 1.0f;
    meter_dirty = ball_dirty = 1;
}

static void cancel_power(void) {
    if (!power_active) return;
    power_active = 0;
    charging = 0;
    meter = 0.0f;
    meter_dirty = ball_dirty = 1;
}

static void fire(void) {
    if (!power_active) return;
    float m = meter;
    float speed = 30.0f + 610.0f * (0.55f * m + 0.45f * m * m);
    ball_vx = aim_x * speed;
    ball_vy = aim_y * speed;
    last_x = ball_x;
    last_y = ball_y;
    ball_moving = 1;
    power_active = 0;
    meter = 0.0f;
    strokes[current_course]++;
    header_dirty = 1;
    meter_dirty = ball_dirty = 1;
}

static void tick(void) {
    if (scorecard_open || editor_mode) return;
    if (!holed[current_course]) gate_clock += 16;

    if (power_active) {
        meter += meter_dir * 0.017f;
        if (meter >= 1.0f) { meter = 1.0f; meter_dir = -1.0f; }
        if (meter <= 0.0f) { meter = 0.0f; meter_dir = 1.0f; }
        meter_dirty = ball_dirty = 1;
    }
    if (sinking) {
        const Course* course = &courses[current_course];
        sink_t++;
        ball_x += ((float)course->hole_x - ball_x) * 0.35f;
        ball_y += ((float)course->hole_y - ball_y) * 0.35f;
        ball_dirty = 1;
        if (sink_t >= 12) finish_hole();
        return;
    }
    if (ball_moving) {
        for (int s = 0; s < SUBSTEPS && ball_moving && !sinking; s++) step_ball();
    }
}

/* ------------------------------------------------------------------ */
/* dynamic layer                                                      */
/* ------------------------------------------------------------------ */

static Rect make_rect(int x0, int y0, int x1, int y1, int limit_to_play) {
    Rect r;
    int lx0 = 0, ly0 = 0, lx1 = WIDTH, ly1 = HEIGHT;
    if (limit_to_play) {
        lx0 = FIELD_X - FRAME; ly0 = FIELD_Y - FRAME;
        lx1 = FIELD_X + FIELD_W + FRAME; ly1 = FIELD_Y + FIELD_H + FRAME;
    }
    r.x0 = clampi(x0, lx0, lx1);
    r.y0 = clampi(y0, ly0, ly1);
    r.x1 = clampi(x1, lx0, lx1);
    r.y1 = clampi(y1, ly0, ly1);
    return r;
}

static int rect_empty(Rect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }

static Rect rect_union(Rect a, Rect b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    Rect r;
    r.x0 = a.x0 < b.x0 ? a.x0 : b.x0;
    r.y0 = a.y0 < b.y0 ? a.y0 : b.y0;
    r.x1 = a.x1 > b.x1 ? a.x1 : b.x1;
    r.y1 = a.y1 > b.y1 ? a.y1 : b.y1;
    return r;
}

static int ball_aiming(void) {
    return !ball_moving && !sinking && !holed[current_course];
}

static void compute_slots(void) {
    const Course* course = &courses[current_course];
    for (int i = 0; i < SLOT_COUNT; i++) {
        slot_cur[i].x0 = slot_cur[i].y0 = slot_cur[i].x1 = slot_cur[i].y1 = 0;
    }
    int bx = (int)ball_x, by = (int)ball_y;
    int pad = ball_aiming() ? 104 : 14;
    slot_cur[SLOT_BALL] = make_rect(bx - pad, by - pad, bx + pad + 1, by + pad + 1, 1);

    int g = 0;
    for (int i = 0; i < course->obstacle_count && g < MAX_GATES; i++) {
        const Obstacle* o = &course->obstacles[i];
        if (o->type != OBSTACLE_GATE) continue;
        int gx = object_x(o, gate_clock);
        slot_cur[SLOT_GATE0 + g] = make_rect(gx - 1, o->y - 1, gx + o->w + 6, o->y + o->h + 7, 1);
        g++;
    }
    slot_cur[SLOT_METER] = make_rect(MET_X - 3, MET_Y - 3, MET_X + MET_W + 3,
                                     MET_Y + MET_H + 3, 0);
}

static void draw_ball(float x, float y, float r) {
    disc(x + 2.5f, y + 3.5f, r, rgb(0, 30, 0), 80);
    shaded_disc(x, y, r, rgb(255, 255, 255), rgb(190, 198, 210));
    ring(x, y, r - 0.5f, 1.0f, rgb(140, 146, 160), 90);
}

static void draw_putter(void) {
    float pull = meter * 36.0f;
    float back = (float)BALL_RADIUS + 6.0f + pull;
    float hx = ball_x - aim_x * back, hy = ball_y - aim_y * back;
    float px = -aim_y, py = aim_x;

    /* aiming dots */
    for (int i = 0; i < 6; i++) {
        float d = (float)BALL_RADIUS + 10.0f + (float)i * 14.0f;
        disc(ball_x + aim_x * d, ball_y + aim_y * d, 1.8f, rgb(255, 255, 255), 170 - i * 16);
    }
    /* shaft (angled slightly, like a real putter) + head */
    float sx = hx - aim_x * 40.0f + px * 6.0f, sy = hy - aim_y * 40.0f + py * 6.0f;
    capsule(hx + 2.0f, hy + 3.0f, sx + 2.0f, sy + 3.0f, 3.0f, rgb(0, 30, 0), 70);
    capsule(hx, hy, sx, sy, 2.6f, rgb(120, 124, 132), 255);
    capsule(hx - px * 8.0f + 2.0f, hy - py * 8.0f + 3.0f,
            hx + px * 8.0f + 2.0f, hy + py * 8.0f + 3.0f, 5.0f, rgb(0, 30, 0), 70);
    capsule(hx - px * 8.0f, hy - py * 8.0f, hx + px * 8.0f, hy + py * 8.0f,
            5.0f, rgb(52, 54, 60), 255);
    capsule(hx - px * 7.0f + aim_x * 1.6f, hy - py * 7.0f + aim_y * 1.6f,
            hx + px * 7.0f + aim_x * 1.6f, hy + py * 7.0f + aim_y * 1.6f,
            1.4f, rgb(190, 194, 204), 255);
}

static void draw_meter_fill(void) {
    int w = (int)(meter * (float)MET_W);
    for (int i = 0; i < w; i++) {
        int t = i * 255 / MET_W;
        uint32_t c = t < 128 ? mix(rgb(88, 200, 90), rgb(240, 212, 80), t * 2)
                             : mix(rgb(240, 212, 80), rgb(232, 88, 68), (t - 128) * 2);
        fill_rect(MET_X + i, MET_Y, 1, MET_H, c, 255);
    }
    if (w > 0) fill_rect(MET_X, MET_Y, w, 2, rgb(255, 255, 255), 60);
}

static void draw_dynamic(void) {
    const Course* course = &courses[current_course];
    T = pixels;
    int g = 0;
    for (int i = 0; i < course->obstacle_count && g < MAX_GATES; i++) {
        const Obstacle* o = &course->obstacles[i];
        if (o->type != OBSTACLE_GATE) continue;
        clip_rect(slot_cur[SLOT_GATE0 + g]);
        int gx = object_x(o, gate_clock);
        wall_shadow(gx, o->y, o->w, o->h);
        wall_body(gx, o->y, o->w, o->h, 1);
        g++;
    }

    clip_rect(slot_cur[SLOT_BALL]);
    if (!holed[current_course]) {
        if (sinking) {
            float r = (float)BALL_RADIUS * (1.0f - (float)sink_t / 16.0f);
            draw_ball(ball_x, ball_y, r);
        } else {
            if (ball_aiming()) draw_putter();
            draw_ball(ball_x, ball_y, (float)BALL_RADIUS);
        }
    }

    clip_rect(slot_cur[SLOT_METER]);
    draw_meter_fill();
    clip_all();
}

/* ------------------------------------------------------------------ */
/* overlays (drawn only when they appear / change)                    */
/* ------------------------------------------------------------------ */

static void draw_banner(void) {
    const Course* course = &courses[current_course];
    int diff = strokes[current_course] - course->par;
    const char* title;
    if (strokes[current_course] == 1) title = "HOLE IN ONE!";
    else if (diff <= -3) title = "ALBATROSS!";
    else if (diff == -2) title = "EAGLE!";
    else if (diff == -1) title = "BIRDIE!";
    else if (diff == 0) title = "PAR";
    else if (diff == 1) title = "BOGEY";
    else if (diff == 2) title = "DOUBLE BOGEY";
    else title = "OVER PAR";

    fill_rect(270, 240, 360, 140, rgb(20, 32, 27), 238);
    fill_rect(270, 240, 360, 3, C_GOLD, 255);
    centered_text(270, 258, 360, title, C_MINT, 2);
    text(330, 296, "STROKES", C_MUTED, 1);
    number_text(412, 296, strokes[current_course], C_INK, 1);
    text(470, 296, "PAR", C_MUTED, 1);
    number_text(510, 296, course->par, C_GOLD, 1);
    draw_button(BTN_NEXT_X, BTN_NEXT_Y, BTN_NEXT_W, BTN_NEXT_H,
                current_course + 1 < COURSE_COUNT ? "NEXT HOLE" : "SCORECARD", 1);
}

static void draw_scorecard(void) {
    fill_rect(210, 142, 480, 372, rgb(20, 32, 27), 245);
    fill_rect(210, 142, 480, 2, C_GOLD, 255);
    text(238, 165, "ROUND SCORECARD", C_MINT, 2);
    text(242, 202, "HOLE", C_MUTED, 1);
    text(420, 202, "PAR", C_MUTED, 1);
    text(504, 202, "SCORE", C_MUTED, 1);
    text(590, 202, "+/-", C_MUTED, 1);
    int total = 0, par_total = 0;
    for (int i = 0; i < COURSE_COUNT; i++) {
        int y = 226 + i * 32;
        if (i == current_course) fill_rect(232, y - 4, 438, 28, 0xFF284238, 255);
        text(246, y, courses[i].name, C_INK, 1);
        number_text(432, y, courses[i].par, C_MUTED, 1);
        if (holed[i]) {
            number_text(512, y, strokes[i], C_INK, 1);
            number_text(592, y, strokes[i] - courses[i].par,
                        strokes[i] <= courses[i].par ? C_MINT : C_CORAL, 1);
        } else {
            text(512, y, "-", C_MUTED, 1);
            text(592, y, "-", C_MUTED, 1);
        }
        total += strokes[i];
        par_total += courses[i].par;
    }
    fill_rect(238, 457, 424, 1, 0xFF496453, 255);
    text(246, 472, "TOTAL", C_GOLD, 1);
    number_text(432, 472, par_total, C_MUTED, 1);
    number_text(512, 472, total, C_INK, 1);
    number_text(592, 472, total - par_total, total <= par_total ? C_MINT : C_CORAL, 1);
    centered_text(224, 494, 452, "T OR CLICK TO CLOSE", C_MUTED, 1);
}

/* ------------------------------------------------------------------ */
/* presenting                                                         */
/* ------------------------------------------------------------------ */

static void present_rect(int x, int y, int w, int h) {
#if PARTIAL_PRESENT
    int x1 = clampi(x + w, 0, WIDTH), y1 = clampi(y + h, 0, HEIGHT);
    x = clampi(x, 0, WIDTH);
    y = clampi(y, 0, HEIGHT);
    if (x1 <= x || y1 <= y) return;
    mos_window_present(window_id, pixels, WIDTH * 4, x, y, x1 - x, y1 - y);
#else
    (void)x; (void)y; (void)w; (void)h;
    mos_window_present(window_id, pixels, WIDTH * 4, 0, 0, WIDTH, HEIGHT);
#endif
}

static void present_slot_rect(Rect r) {
    if (rect_empty(r)) return;
    present_rect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
}

static void restore_rect(Rect r) {
    r.x0 = clampi(r.x0, 0, WIDTH);
    r.x1 = clampi(r.x1, 0, WIDTH);
    r.y0 = clampi(r.y0, 0, HEIGHT);
    r.y1 = clampi(r.y1, 0, HEIGHT);
    if (rect_empty(r)) return;
    for (int y = r.y0; y < r.y1; y++) {
        const uint32_t* src = &bg[(size_t)y * WIDTH];
        uint32_t* dst = &pixels[(size_t)y * WIDTH];
        for (int x = r.x0; x < r.x1; x++) dst[x] = src[x];
    }
}

static void remember_slots(void) {
    for (int i = 0; i < SLOT_COUNT; i++) slot_prev[i] = slot_cur[i];
}

static void full_render(void) {
    if (header_dirty) {
        T = bg;
        clip_all();
        draw_header();
        header_dirty = 0;
    }
    for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; i++) pixels[i] = bg[i];
    compute_slots();
    draw_dynamic();
    T = pixels;
    clip_all();
    if (holed[current_course]) draw_banner();
    if (scorecard_open) draw_scorecard();
    present_rect(0, 0, WIDTH, HEIGHT);
    remember_slots();
    need_full = 0;
    ball_dirty = meter_dirty = 0;
}

static void render(void) {
    if (editor_mode) { editor_render(); return; }
    if (need_full) { full_render(); return; }

    if (header_dirty) {
        T = bg;
        clip_all();
        draw_header();
        header_dirty = 0;
        int hh = FIELD_Y - FRAME;
        for (int y = 0; y < hh; y++)
            for (int x = 0; x < WIDTH; x++)
                pixels[(size_t)y * WIDTH + (size_t)x] = bg[(size_t)y * WIDTH + (size_t)x];
        present_rect(0, 0, WIDTH, hh);
    }
    if (scorecard_open || holed[current_course]) return;

    const Course* course = &courses[current_course];
    int has_gate = 0;
    for (int i = 0; i < course->obstacle_count; i++)
        if (course->obstacles[i].type == OBSTACLE_GATE) has_gate = 1;

    if (!ball_dirty && !meter_dirty && !has_gate) return;

    compute_slots();
    for (int i = 0; i < SLOT_COUNT; i++)
        restore_rect(rect_union(slot_prev[i], slot_cur[i]));
    draw_dynamic();

    if (ball_dirty)
        present_slot_rect(rect_union(slot_prev[SLOT_BALL], slot_cur[SLOT_BALL]));
    for (int i = SLOT_GATE0; i < SLOT_METER; i++)
        if (!rect_empty(slot_cur[i]) || !rect_empty(slot_prev[i]))
            present_slot_rect(rect_union(slot_prev[i], slot_cur[i]));
    if (meter_dirty)
        present_slot_rect(rect_union(slot_prev[SLOT_METER], slot_cur[SLOT_METER]));

    remember_slots();
    ball_dirty = meter_dirty = 0;
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */

static int in_button(int x, int y, int bx, int by, int bw, int bh) {
    return x >= bx && y >= by && x < bx + bw && y < by + bh;
}

/* ------------------------------------------------------------------ */
/* level editor                                                       */
/* ------------------------------------------------------------------ */

enum { TOOL_SELECT, TOOL_TEE, TOOL_HOLE, TOOL_SAND, TOOL_ROUGH, TOOL_WATER,
       TOOL_SLOPE, TOOL_HILL, TOOL_BLOCK, TOOL_WALL, TOOL_BUMPER, TOOL_GATE,
       TOOL_PORTAL, TOOL_COUNT };
enum { SEL_NONE, SEL_TEE, SEL_HOLE, SEL_PORTAL_A, SEL_PORTAL_B, SEL_ZONE, SEL_OBS };
enum { DRAG_NONE, DRAG_MOVE, DRAG_HANDLE, DRAG_CREATE };
enum { H_NW, H_NE, H_SW, H_SE, H_HILL, H_BUMPER, H_P1, H_P2, H_PORTAL };
enum { KEY_D = 7, KEY_E = 8, KEY_H = 11, KEY_S = 22, KEY_Z = 29,
       KEY_BKSP = 42, KEY_MINUS = 45, KEY_PLUS = 46, KEY_DEL = 76 };

static const char* const tool_names[TOOL_COUNT] = {
    "PICK", "TEE", "HOLE", "SAND", "ROUGH", "WATER", "SLOPE", "HILL",
    "BLOCK", "WALL", "BUMP", "GATE", "PORT"
};
static const char* const tool_hints[TOOL_COUNT] = {
    "CLICK TO SELECT", "CLICK TO PLACE TEE", "CLICK TO PLACE CUP",
    "DRAG A RECTANGLE", "DRAG A RECTANGLE", "DRAG A RECTANGLE",
    "DRAG A RECTANGLE", "DRAG A CIRCLE", "DRAG A RECTANGLE",
    "DRAG END TO END", "CLICK TO PLACE", "DRAG A RECTANGLE", "CLICK TO PLACE"
};
static const char* const zone_names[5] = {"SAND", "ROUGH", "WATER", "SLOPE", "HILL"};
static const char* const obstacle_names[4] = {"BLOCK", "BUMPER", "GATE", "WALL"};
static const int ed_snap_vals[5] = {1, 2, 5, 10, 20};

typedef struct {
    char name[NAME_LEN];
    int32_t tee_x, tee_y, hole_x, hole_y, par, zone_count, obstacle_count;
    int32_t portal[5];
    Zone zones[MAX_ZONES];
    Obstacle obstacles[MAX_OBST];
} LevelRec;

typedef struct { int id, x, y; } Handle;
typedef struct { const char* label; int* value; int step, lo, hi; } EdParam;
typedef struct { char* p; size_t n, cap; } Out;

static LevelRec ed_undo[ED_UNDO], ed_tmp, ed_tmp2;
static int ed_undo_n, ed_tool, ed_snap_idx = 2, ed_sel_kind, ed_sel_i;
static int ed_bg_dirty = 1, ed_dirty = 1, ed_name_focus, ed_confirm;
static char ed_msg[32];
static char ed_panel_msg[64];
static long ed_msg_until;
static struct {
    int mode, hnd, px, py, cx, cy, pushed;
    int otx, oty, op[5];
    Zone oz;
    Obstacle oo;
} ed_drag;

/* ---- small helpers ------------------------------------------------ */

static void ed_copy_str(char* d, const char* s, int cap) {
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static int ed_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void ed_say(const char* m) {
    ed_copy_str(ed_msg, m, (int)sizeof ed_msg);
    ed_msg_until = mos_uptime() + 2500;
    ed_dirty = 1;
}

static void ed_panel_say(const char* m) {
    ed_copy_str(ed_panel_msg, m, (int)sizeof ed_panel_msg);
    ed_dirty = 1;
}

static int snapv(int v) {
    int s = ed_snap_vals[ed_snap_idx];
    if (s <= 1) return v;
    if (v < 0) return -(((-v) + s / 2) / s) * s;
    return ((v + s / 2) / s) * s;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ---- level storage <-> records ------------------------------------ */

static void level_bind(int i) {
    courses[i].name = name_store[i];
    courses[i].zones = zone_store[i];
    courses[i].obstacles = obs_store[i];
}

static void level_save(int i, LevelRec* r) {
    const Course* c = &courses[i];
    ed_copy_str(r->name, name_store[i], NAME_LEN);
    r->tee_x = c->tee_x; r->tee_y = c->tee_y;
    r->hole_x = c->hole_x; r->hole_y = c->hole_y;
    r->par = c->par;
    r->zone_count = c->zone_count;
    r->obstacle_count = c->obstacle_count;
    r->portal[0] = c->portal_ax; r->portal[1] = c->portal_ay;
    r->portal[2] = c->portal_bx; r->portal[3] = c->portal_by;
    r->portal[4] = c->portal_radius;
    for (int k = 0; k < MAX_ZONES; k++) r->zones[k] = zone_store[i][k];
    for (int k = 0; k < MAX_OBST; k++) r->obstacles[k] = obs_store[i][k];
}

static void level_load(int i, const LevelRec* r) {
    Course* c = &courses[i];
    ed_copy_str(name_store[i], r->name, NAME_LEN);
    c->tee_x = r->tee_x; c->tee_y = r->tee_y;
    c->hole_x = r->hole_x; c->hole_y = r->hole_y;
    c->par = clampi(r->par, 1, 9);
    c->zone_count = clampi(r->zone_count, 0, MAX_ZONES);
    c->obstacle_count = clampi(r->obstacle_count, 0, MAX_OBST);
    c->portal_ax = r->portal[0]; c->portal_ay = r->portal[1];
    c->portal_bx = r->portal[2]; c->portal_by = r->portal[3];
    c->portal_radius = r->portal[4];
    for (int k = 0; k < MAX_ZONES; k++) zone_store[i][k] = r->zones[k];
    for (int k = 0; k < MAX_OBST; k++) obs_store[i][k] = r->obstacles[k];
    level_bind(i);
}

static void level_blank(LevelRec* r) {
    char* raw = (char*)r;
    for (size_t k = 0; k < sizeof(LevelRec); k++) raw[k] = 0;
    ed_copy_str(r->name, "New Hole", NAME_LEN);
    r->tee_x = 105; r->tee_y = 349;
    r->hole_x = 806; r->hole_y = 349;
    r->par = 3;
}

static void load_defaults(void) {
    course_count = DEFAULT_COUNT > MAX_HOLES ? MAX_HOLES : DEFAULT_COUNT;
    for (int i = 0; i < course_count; i++) {
        const Course* d = &default_courses[i];
        Course* c = &courses[i];
        ed_copy_str(name_store[i], d->name, NAME_LEN);
        c->tee_x = d->tee_x; c->tee_y = d->tee_y;
        c->hole_x = d->hole_x; c->hole_y = d->hole_y;
        c->par = d->par;
        c->zone_count = imin(d->zone_count, MAX_ZONES);
        c->obstacle_count = imin(d->obstacle_count, MAX_OBST);
        for (int k = 0; k < c->zone_count; k++) zone_store[i][k] = d->zones[k];
        for (int k = 0; k < c->obstacle_count; k++) obs_store[i][k] = d->obstacles[k];
        c->portal_ax = d->portal_ax; c->portal_ay = d->portal_ay;
        c->portal_bx = d->portal_bx; c->portal_by = d->portal_by;
        c->portal_radius = d->portal_radius;
        level_bind(i);
    }
    for (int i = 0; i < MAX_HOLES; i++) { strokes[i] = 0; holed[i] = 0; }
}

/* ---- saving / loading --------------------------------------------- */

static int write_file(const char* path, const void* data, size_t len) {
    mos_mkdir(LEVEL_DIR);             /* fails harmlessly if it exists */
    mos_delete(path);                 /* fails harmlessly if it doesn't */
    if (mos_create(path, NULL, NULL) < 0) return 0;
    long fd = mos_open(path, SYS_O_RDWR);
    if (fd < 0) return 0;
    size_t done = 0;
    while (done < len) {
        long n = mos_fwrite(fd, (const char*)data + done, len - done);
        if (n <= 0) { mos_close(fd); return 0; }
        done += (size_t)n;
    }
    mos_close(fd);
    return 1;
}

static int editor_save_file(void) {
    size_t total = 12 + (size_t)course_count * sizeof(LevelRec);
    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) return 0;
    uint32_t* hdr = (uint32_t*)buf;
    hdr[0] = LEVEL_MAGIC;
    hdr[1] = (uint32_t)sizeof(LevelRec);
    hdr[2] = (uint32_t)course_count;
    for (int i = 0; i < course_count; i++)
        level_save(i, (LevelRec*)(buf + 12 + (size_t)i * sizeof(LevelRec)));
    int ok = write_file(LEVEL_FILE, buf, total);
    free(buf);
    if (ok) ed_modified = 0;
    return ok;
}

/* returns 1 when a valid saved level file was loaded */
static int editor_load_file(void) {
    long fd = mos_open(LEVEL_FILE, 0);
    if (fd < 0) return 0;
    long size = mos_size(fd);
    if (size < 12 || (uint64_t)size > 1024u * 1024u) { mos_close(fd); return 0; }
    uint8_t* buf = (uint8_t*)malloc((size_t)size);
    if (!buf) { mos_close(fd); return 0; }
    size_t got = 0;
    while (got < (size_t)size) {
        long n = mos_read(fd, buf + got, (size_t)size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    mos_close(fd);
    uint32_t* hdr = (uint32_t*)buf;
    int ok = got == (size_t)size && hdr[0] == LEVEL_MAGIC &&
             hdr[1] == (uint32_t)sizeof(LevelRec) &&
             hdr[2] >= 1 && hdr[2] <= (uint32_t)MAX_HOLES &&
             (size_t)size == 12 + (size_t)hdr[2] * sizeof(LevelRec);
    if (ok) {
        course_count = (int)hdr[2];
        for (int i = 0; i < course_count; i++)
            level_load(i, (const LevelRec*)(buf + 12 + (size_t)i * sizeof(LevelRec)));
        for (int i = 0; i < MAX_HOLES; i++) { strokes[i] = 0; holed[i] = 0; }
    }
    free(buf);
    return ok;
}

/* ---- C export (same layout as the tables at the top of this file) -- */

static void o_ch(Out* o, char c) { if (o->n + 1 < o->cap) o->p[o->n++] = c; }
static void o_str(Out* o, const char* s) { while (*s) o_ch(o, *s++); }
static void o_int(Out* o, int v) {
    char tmp[16];
    int k = 0;
    unsigned int u = v < 0 ? 0u - (unsigned int)v : (unsigned int)v;
    if (v < 0) o_ch(o, '-');
    do { tmp[k++] = (char)('0' + u % 10u); u /= 10u; } while (u && k < 14);
    while (k) o_ch(o, tmp[--k]);
}
static void o_sep(Out* o, int v) { o_str(o, ", "); o_int(o, v); }

static void ed_make_id(int i, Out* id) {
    o_ch(id, 'h');
    o_int(id, i + 1);
    o_ch(id, '_');
    int last_us = 1;
    for (const char* s = name_store[i]; *s; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { o_ch(id, c); last_us = 0; }
        else if (!last_us) { o_ch(id, '_'); last_us = 1; }
    }
    id->p[id->n] = '\0';
}

static void ed_export_c(Out* o) {
    for (int i = 0; i < course_count; i++) {
        const Course* c = &courses[i];
        char idbuf[64];
        Out id = {idbuf, 0, sizeof idbuf};
        ed_make_id(i, &id);
        o_str(o, "/* ---- ");
        o_int(o, i + 1);
        o_str(o, ": ");
        o_str(o, name_store[i]);
        o_str(o, " ---- */\nstatic const Zone zones_");
        o_str(o, idbuf);
        o_str(o, "[] = {\n");
        if (!c->zone_count) o_str(o, "    {ZONE_ROUGH, 0, 0, 0, 0, 0, 0}   /* placeholder, zone_count = 0 */\n");
        for (int k = 0; k < c->zone_count; k++) {
            const Zone* z = &zone_store[i][k];
            o_str(o, "    {ZONE_");
            o_str(o, zone_names[z->type]);
            o_sep(o, z->x); o_sep(o, z->y); o_sep(o, z->w); o_sep(o, z->h);
            o_sep(o, z->slope_x); o_sep(o, z->slope_y);
            o_str(o, k + 1 < c->zone_count ? "},\n" : "}\n");
        }
        o_str(o, "};\nstatic const Obstacle obstacles_");
        o_str(o, idbuf);
        o_str(o, "[] = {\n");
        if (!c->obstacle_count) o_str(o, "    {OBSTACLE_BLOCK, 0, 0, 0, 0, 0, 0, 0}   /* placeholder, obstacle_count = 0 */\n");
        for (int k = 0; k < c->obstacle_count; k++) {
            const Obstacle* b = &obs_store[i][k];
            if (b->type == OBSTACLE_WALL) {
                o_str(o, "    WALL(");
                o_int(o, b->x); o_sep(o, b->y); o_sep(o, b->w); o_sep(o, b->h);
                o_str(o, k + 1 < c->obstacle_count ? "),\n" : ")\n");
            } else {
                o_str(o, "    {OBSTACLE_");
                o_str(o, b->type == OBSTACLE_BLOCK ? "BLOCK" : b->type == OBSTACLE_BUMPER ? "BUMPER" : "GATE");
                o_sep(o, b->x); o_sep(o, b->y); o_sep(o, b->w); o_sep(o, b->h);
                o_sep(o, b->radius); o_sep(o, b->travel); o_sep(o, b->phase);
                o_str(o, k + 1 < c->obstacle_count ? "},\n" : "}\n");
            }
        }
        o_str(o, "};\n\n");
    }
    o_str(o, "static const Course courses[] = {\n");
    for (int i = 0; i < course_count; i++) {
        const Course* c = &courses[i];
        char idbuf[64];
        Out id = {idbuf, 0, sizeof idbuf};
        ed_make_id(i, &id);
        o_str(o, "    {\"");
        o_str(o, name_store[i]);
        o_str(o, "\",\n     ");
        o_int(o, c->tee_x); o_sep(o, c->tee_y); o_sep(o, c->hole_x); o_sep(o, c->hole_y); o_sep(o, c->par);
        o_str(o, ", zones_"); o_str(o, idbuf); o_sep(o, c->zone_count);
        o_str(o, ", obstacles_"); o_str(o, idbuf); o_sep(o, c->obstacle_count);
        o_str(o, ",\n     ");
        o_int(o, c->portal_ax); o_sep(o, c->portal_ay); o_sep(o, c->portal_bx);
        o_sep(o, c->portal_by); o_sep(o, c->portal_radius);
        o_str(o, i + 1 < course_count ? "},\n" : "}\n");
    }
    o_str(o, "};\n");
}

static int editor_export_file(void) {
    size_t cap = 96 * 1024;
    char* mem = (char*)malloc(cap);
    if (!mem) return 0;
    Out o = {mem, 0, cap};
    ed_export_c(&o);
    int ok = write_file(EXPORT_FILE, mem, o.n);
    free(mem);
    return ok;
}

/* ---- undo --------------------------------------------------------- */

static void ed_push_undo(void) {
    if (ed_undo_n == ED_UNDO) {
        for (int i = 0; i < ED_UNDO - 1; i++) ed_undo[i] = ed_undo[i + 1];
        ed_undo_n--;
    }
    level_save(current_course, &ed_undo[ed_undo_n++]);
    ed_modified = 1;
    header_dirty = 1;
}

static void ed_undo_pop(void) {
    if (!ed_undo_n) { ed_say("NOTHING TO UNDO"); return; }
    level_load(current_course, &ed_undo[--ed_undo_n]);
    ed_sel_kind = SEL_NONE;
    set_ball_to_tee();
    ed_bg_dirty = 1;
    ed_dirty = 1;
}

/* ---- hit testing / handles ---------------------------------------- */

static float seg_dist(float px, float py, float x0, float y0, float x1, float y1) {
    float sx = x1 - x0, sy = y1 - y0, l2 = sx * sx + sy * sy;
    float t = l2 > 0.0001f ? ((px - x0) * sx + (py - y0) * sy) / l2 : 0.0f;
    t = clampf(t, 0.0f, 1.0f);
    float dx = px - (x0 + t * sx), dy = py - (y0 + t * sy);
    return fsqrt(dx * dx + dy * dy);
}

static int ed_hit(int x, int y, int* kind, int* idx) {
    const Course* c = &courses[current_course];
    int dx = x - c->tee_x, dy = y - c->tee_y;
    if (dx * dx + dy * dy <= 14 * 14) { *kind = SEL_TEE; *idx = 0; return 1; }
    dx = x - c->hole_x; dy = y - c->hole_y;
    if (dx * dx + dy * dy <= (HOLE_RADIUS + 4) * (HOLE_RADIUS + 4)) { *kind = SEL_HOLE; *idx = 0; return 1; }
    if (c->portal_radius) {
        int r2 = c->portal_radius * c->portal_radius;
        dx = x - c->portal_ax; dy = y - c->portal_ay;
        if (dx * dx + dy * dy <= r2) { *kind = SEL_PORTAL_A; *idx = 0; return 1; }
        dx = x - c->portal_bx; dy = y - c->portal_by;
        if (dx * dx + dy * dy <= r2) { *kind = SEL_PORTAL_B; *idx = 0; return 1; }
    }
    for (int i = c->obstacle_count - 1; i >= 0; i--) {
        const Obstacle* o = &obs_store[current_course][i];
        int hitit = 0;
        if (o->type == OBSTACLE_BUMPER) {
            dx = x - o->x; dy = y - o->y;
            hitit = dx * dx + dy * dy <= o->radius * o->radius;
        } else if (o->type == OBSTACLE_WALL) {
            hitit = seg_dist((float)x, (float)y, (float)o->x, (float)o->y,
                             (float)o->w, (float)o->h) <= WALL_THICK * 0.5f + 3.0f;
        } else {
            hitit = x >= o->x && x <= o->x + o->w && y >= o->y && y <= o->y + o->h;
        }
        if (hitit) { *kind = SEL_OBS; *idx = i; return 1; }
    }
    for (int i = c->zone_count - 1; i >= 0; i--) {
        const Zone* z = &zone_store[current_course][i];
        int hitit;
        if (z->type == ZONE_HILL) {
            int r = z->w / 2;
            dx = x - (z->x + r); dy = y - (z->y + r);
            hitit = dx * dx + dy * dy <= r * r;
        } else {
            hitit = x >= z->x && x <= z->x + z->w && y >= z->y && y <= z->y + z->h;
        }
        if (hitit) { *kind = SEL_ZONE; *idx = i; return 1; }
    }
    return 0;
}

static void add_handle(Handle* h, int* n, int id, int x, int y) {
    h[*n].id = id; h[*n].x = x; h[*n].y = y;
    (*n)++;
}

static void add_rect_handles(Handle* h, int* n, int x, int y, int w, int hh) {
    add_handle(h, n, H_NW, x, y);
    add_handle(h, n, H_NE, x + w, y);
    add_handle(h, n, H_SW, x, y + hh);
    add_handle(h, n, H_SE, x + w, y + hh);
}

static int ed_handles(Handle* h) {
    int n = 0;
    const Course* c = &courses[current_course];
    if (ed_sel_kind == SEL_ZONE && ed_sel_i < c->zone_count) {
        const Zone* z = &zone_store[current_course][ed_sel_i];
        if (z->type == ZONE_HILL) add_handle(h, &n, H_HILL, z->x + z->w, z->y + z->h / 2);
        else add_rect_handles(h, &n, z->x, z->y, z->w, z->h);
    } else if (ed_sel_kind == SEL_OBS && ed_sel_i < c->obstacle_count) {
        const Obstacle* o = &obs_store[current_course][ed_sel_i];
        if (o->type == OBSTACLE_BUMPER) add_handle(h, &n, H_BUMPER, o->x + o->radius, o->y);
        else if (o->type == OBSTACLE_WALL) {
            add_handle(h, &n, H_P1, o->x, o->y);
            add_handle(h, &n, H_P2, o->w, o->h);
        } else add_rect_handles(h, &n, o->x, o->y, o->w, o->h);
    } else if (ed_sel_kind == SEL_PORTAL_A && c->portal_radius) {
        add_handle(h, &n, H_PORTAL, c->portal_ax + c->portal_radius, c->portal_ay);
    } else if (ed_sel_kind == SEL_PORTAL_B && c->portal_radius) {
        add_handle(h, &n, H_PORTAL, c->portal_bx + c->portal_radius, c->portal_by);
    }
    return n;
}

/* ---- drag operations ---------------------------------------------- */

static void ed_touch(void) {
    if (!ed_drag.pushed) { ed_push_undo(); ed_drag.pushed = 1; }
}

static void ed_drag_begin(int mode, int hnd, int x, int y) {
    Course* c = &courses[current_course];
    ed_drag.mode = mode;
    ed_drag.hnd = hnd;
    ed_drag.px = x; ed_drag.py = y;
    ed_drag.cx = snapv(x); ed_drag.cy = snapv(y);
    ed_drag.pushed = 0;
    if (ed_sel_kind == SEL_TEE) { ed_drag.otx = c->tee_x; ed_drag.oty = c->tee_y; }
    if (ed_sel_kind == SEL_HOLE) { ed_drag.otx = c->hole_x; ed_drag.oty = c->hole_y; }
    ed_drag.op[0] = c->portal_ax; ed_drag.op[1] = c->portal_ay;
    ed_drag.op[2] = c->portal_bx; ed_drag.op[3] = c->portal_by;
    ed_drag.op[4] = c->portal_radius;
    if (ed_sel_kind == SEL_ZONE) ed_drag.oz = zone_store[current_course][ed_sel_i];
    if (ed_sel_kind == SEL_OBS) ed_drag.oo = obs_store[current_course][ed_sel_i];
}

static void resize_rect(int* x, int* y, int* w, int* h, int ox, int oy, int ow,
                        int oh, int id, int mx, int my) {
    int fx = (id == H_NW || id == H_SW) ? ox + ow : ox;
    int fy = (id == H_NW || id == H_NE) ? oy + oh : oy;
    int x0 = imin(fx, mx), x1 = imax(fx, mx);
    int y0 = imin(fy, my), y1 = imax(fy, my);
    if (x1 - x0 < 4) x1 = x0 + 4;
    if (y1 - y0 < 4) y1 = y0 + 4;
    *x = x0; *w = x1 - x0; *y = y0; *h = y1 - y0;
}

static void ed_apply_handle(int rx, int ry) {
    int sx = snapv(rx), sy = snapv(ry);
    Course* c = &courses[current_course];
    if (ed_sel_kind == SEL_ZONE) {
        Zone* z = &zone_store[current_course][ed_sel_i];
        const Zone* o = &ed_drag.oz;
        if (ed_drag.hnd == H_HILL) {
            int d = snapv(rx - o->x);
            if (d < 10) d = 10;
            z->w = z->h = d;
        } else {
            resize_rect(&z->x, &z->y, &z->w, &z->h, o->x, o->y, o->w, o->h, ed_drag.hnd, sx, sy);
        }
    } else if (ed_sel_kind == SEL_OBS) {
        Obstacle* z = &obs_store[current_course][ed_sel_i];
        const Obstacle* o = &ed_drag.oo;
        if (ed_drag.hnd == H_BUMPER) z->radius = clampi(rx - o->x, 6, 120);
        else if (ed_drag.hnd == H_P1) { z->x = sx; z->y = sy; }
        else if (ed_drag.hnd == H_P2) { z->w = sx; z->h = sy; }
        else resize_rect(&z->x, &z->y, &z->w, &z->h, o->x, o->y, o->w, o->h, ed_drag.hnd, sx, sy);
    } else if (ed_sel_kind == SEL_PORTAL_A || ed_sel_kind == SEL_PORTAL_B) {
        int cx = ed_sel_kind == SEL_PORTAL_A ? ed_drag.op[0] : ed_drag.op[2];
        c->portal_radius = clampi(iabs(rx - cx), 8, 80);
    }
}

static void ed_apply_move(int dx, int dy) {
    Course* c = &courses[current_course];
    if (ed_sel_kind == SEL_TEE || ed_sel_kind == SEL_HOLE) {
        int nx = clampi(snapv(ed_drag.otx + dx), FIELD_X + BALL_RADIUS, FIELD_X + FIELD_W - BALL_RADIUS);
        int ny = clampi(snapv(ed_drag.oty + dy), FIELD_Y + BALL_RADIUS, FIELD_Y + FIELD_H - BALL_RADIUS);
        if (ed_sel_kind == SEL_TEE) { c->tee_x = nx; c->tee_y = ny; }
        else { c->hole_x = nx; c->hole_y = ny; }
    } else if (ed_sel_kind == SEL_PORTAL_A) {
        c->portal_ax = snapv(ed_drag.op[0] + dx); c->portal_ay = snapv(ed_drag.op[1] + dy);
    } else if (ed_sel_kind == SEL_PORTAL_B) {
        c->portal_bx = snapv(ed_drag.op[2] + dx); c->portal_by = snapv(ed_drag.op[3] + dy);
    } else if (ed_sel_kind == SEL_ZONE) {
        Zone* z = &zone_store[current_course][ed_sel_i];
        const Zone* o = &ed_drag.oz;
        z->x = snapv(o->x + dx); z->y = snapv(o->y + dy);
    } else if (ed_sel_kind == SEL_OBS) {
        Obstacle* z = &obs_store[current_course][ed_sel_i];
        const Obstacle* o = &ed_drag.oo;
        int ax = snapv(o->x + dx) - o->x, ay = snapv(o->y + dy) - o->y;
        z->x = o->x + ax; z->y = o->y + ay;
        if (o->type == OBSTACLE_WALL) { z->w = o->w + ax; z->h = o->h + ay; }
    }
}

/* returns 0 = nothing, 1 = zone, 2 = obstacle */
static int ed_shape(int tool, int ax, int ay, int bx, int by, Zone* z, Obstacle* o) {
    int dx = bx - ax, dy = by - ay;
    int w = iabs(dx), h = iabs(dy);
    if (tool == TOOL_SAND || tool == TOOL_ROUGH || tool == TOOL_WATER ||
        tool == TOOL_SLOPE || tool == TOOL_BLOCK || tool == TOOL_GATE) {
        if (w < 6 || h < 6) return 0;
        int x = imin(ax, bx), y = imin(ay, by);
        if (tool == TOOL_BLOCK || tool == TOOL_GATE) {
            o->type = tool == TOOL_BLOCK ? OBSTACLE_BLOCK : OBSTACLE_GATE;
            o->x = x; o->y = y; o->w = w; o->h = h;
            o->radius = 0; o->travel = tool == TOOL_GATE ? 100 : 0; o->phase = 0;
            return 2;
        }
        z->type = tool == TOOL_SAND ? ZONE_SAND : tool == TOOL_ROUGH ? ZONE_ROUGH :
                  tool == TOOL_WATER ? ZONE_WATER : ZONE_SLOPE;
        z->x = x; z->y = y; z->w = w; z->h = h;
        z->slope_x = tool == TOOL_SLOPE ? 20 : 0; z->slope_y = 0;
        return 1;
    }
    if (tool == TOOL_HILL) {
        int s = imax(w, h);
        if (s < 10) return 0;
        z->type = ZONE_HILL;
        z->x = dx >= 0 ? ax : ax - s;
        z->y = dy >= 0 ? ay : ay - s;
        z->w = z->h = s;
        z->slope_x = 20; z->slope_y = 0;
        return 1;
    }
    if (tool == TOOL_WALL) {
        if (fsqrt((float)(dx * dx + dy * dy)) < 6.0f) return 0;
        o->type = OBSTACLE_WALL;
        o->x = ax; o->y = ay; o->w = bx; o->h = by;
        o->radius = o->travel = o->phase = 0;
        return 2;
    }
    return 0;
}

static void ed_add_zone(const Zone* z) {
    Course* c = &courses[current_course];
    if (c->zone_count >= MAX_ZONES) { ed_say("TOO MANY ZONES"); return; }
    ed_push_undo();
    zone_store[current_course][c->zone_count] = *z;
    ed_sel_kind = SEL_ZONE;
    ed_sel_i = c->zone_count++;
    ed_bg_dirty = 1;
}

static void ed_add_obstacle(const Obstacle* o) {
    Course* c = &courses[current_course];
    if (c->obstacle_count >= MAX_OBST) { ed_say("TOO MANY OBSTACLES"); return; }
    ed_push_undo();
    obs_store[current_course][c->obstacle_count] = *o;
    ed_sel_kind = SEL_OBS;
    ed_sel_i = c->obstacle_count++;
    ed_bg_dirty = 1;
}

static void ed_delete_sel(void) {
    Course* c = &courses[current_course];
    if (ed_sel_kind == SEL_ZONE && ed_sel_i < c->zone_count) {
        ed_push_undo();
        for (int i = ed_sel_i; i < c->zone_count - 1; i++) zone_store[current_course][i] = zone_store[current_course][i + 1];
        c->zone_count--;
    } else if (ed_sel_kind == SEL_OBS && ed_sel_i < c->obstacle_count) {
        ed_push_undo();
        for (int i = ed_sel_i; i < c->obstacle_count - 1; i++) obs_store[current_course][i] = obs_store[current_course][i + 1];
        c->obstacle_count--;
    } else if ((ed_sel_kind == SEL_PORTAL_A || ed_sel_kind == SEL_PORTAL_B) && c->portal_radius) {
        ed_push_undo();
        c->portal_ax = c->portal_ay = c->portal_bx = c->portal_by = c->portal_radius = 0;
    } else {
        return;
    }
    ed_sel_kind = SEL_NONE;
    ed_bg_dirty = 1; ed_dirty = 1;
}

static void ed_duplicate_sel(void) {
    Course* c = &courses[current_course];
    if (ed_sel_kind == SEL_ZONE && ed_sel_i < c->zone_count) {
        Zone z = zone_store[current_course][ed_sel_i];
        z.x += 20; z.y += 20;
        ed_add_zone(&z);
    } else if (ed_sel_kind == SEL_OBS && ed_sel_i < c->obstacle_count) {
        Obstacle o = obs_store[current_course][ed_sel_i];
        o.x += 20; o.y += 20;
        if (o.type == OBSTACLE_WALL) { o.w += 20; o.h += 20; }
        ed_add_obstacle(&o);
    }
}

static void ed_nudge(int dx, int dy) {
    Course* c = &courses[current_course];
    if (ed_sel_kind == SEL_NONE) return;
    ed_push_undo();
    if (ed_sel_kind == SEL_TEE) { c->tee_x += dx; c->tee_y += dy; }
    else if (ed_sel_kind == SEL_HOLE) { c->hole_x += dx; c->hole_y += dy; }
    else if (ed_sel_kind == SEL_PORTAL_A) { c->portal_ax += dx; c->portal_ay += dy; }
    else if (ed_sel_kind == SEL_PORTAL_B) { c->portal_bx += dx; c->portal_by += dy; }
    else if (ed_sel_kind == SEL_ZONE) { zone_store[current_course][ed_sel_i].x += dx; zone_store[current_course][ed_sel_i].y += dy; }
    else if (ed_sel_kind == SEL_OBS) {
        Obstacle* o = &obs_store[current_course][ed_sel_i];
        o->x += dx; o->y += dy;
        if (o->type == OBSTACLE_WALL) { o->w += dx; o->h += dy; }
    }
    ed_bg_dirty = 1; ed_dirty = 1;
}

/* ---- parameter bar ------------------------------------------------ */

static void ed_param(EdParam* p, const char* label, int* v, int step, int lo, int hi) {
    p->label = label; p->value = v; p->step = step; p->lo = lo; p->hi = hi;
}

static int ed_params(EdParam* p) {
    int n = 0;
    Course* c = &courses[current_course];
    if (ed_sel_kind == SEL_ZONE && ed_sel_i < c->zone_count) {
        Zone* z = &zone_store[current_course][ed_sel_i];
        if (z->type == ZONE_SLOPE) {
            ed_param(&p[n++], "PUSH X", &z->slope_x, 2, -48, 48);
            ed_param(&p[n++], "PUSH Y", &z->slope_y, 2, -48, 48);
        } else if (z->type == ZONE_HILL) {
            ed_param(&p[n++], "AMOUNT", &z->slope_x, 2, -48, 48);
        }
    } else if (ed_sel_kind == SEL_OBS && ed_sel_i < c->obstacle_count) {
        Obstacle* o = &obs_store[current_course][ed_sel_i];
        if (o->type == OBSTACLE_GATE) {
            ed_param(&p[n++], "TRAVEL", &o->travel, 4, 0, 600);
            ed_param(&p[n++], "PHASE", &o->phase, 10, 0, 1200);
        } else if (o->type == OBSTACLE_BUMPER) {
            ed_param(&p[n++], "RADIUS", &o->radius, 1, 6, 120);
        }
    } else if ((ed_sel_kind == SEL_PORTAL_A || ed_sel_kind == SEL_PORTAL_B) && c->portal_radius) {
        ed_param(&p[n++], "RADIUS", &c->portal_radius, 1, 8, 80);
    }
    return n;
}

static void ed_param_step(int idx, int dir) {
    EdParam p[2];
    int n = ed_params(p);
    if (idx >= n) return;
    ed_push_undo();
    *p[idx].value = clampi(*p[idx].value + dir * p[idx].step, p[idx].lo, p[idx].hi);
    ed_bg_dirty = 1; ed_dirty = 1;
}

#define ED_PARAM_X(i) (FIELD_X + 230 + (i) * 176)
#define ED_DEL_X (FIELD_X + FIELD_W - 228)
#define ED_UNDO_X (FIELD_X + FIELD_W - 174)
#define ED_SNAP_X (FIELD_X + FIELD_W - 112)
#define PANEL_X 210
#define PANEL_Y 142
#define PANEL_W 480
#define PANEL_H 372

/* ---- structure changes (new / duplicate / delete / reorder) ---------- */

static void ed_after_structure(int idx) {
    for (int i = 0; i < MAX_HOLES; i++) { strokes[i] = 0; holed[i] = 0; }
    ed_undo_n = 0;
    ed_sel_kind = SEL_NONE;
    ed_modified = 1;
    ed_confirm = 0;
    select_course(clampi(idx, 0, course_count - 1));
    header_dirty = 1; ed_bg_dirty = 1; ed_dirty = 1;
}

static void ed_new_hole(int dup) {
    if (course_count >= MAX_HOLES) { ed_panel_say("MAXIMUM 7 HOLES"); return; }
    if (dup) {
        level_save(current_course, &ed_tmp);
        int len = ed_strlen(ed_tmp.name);
        if (len + 2 < NAME_LEN) { ed_tmp.name[len] = ' '; ed_tmp.name[len + 1] = '2'; ed_tmp.name[len + 2] = '\0'; }
    } else {
        level_blank(&ed_tmp);
    }
    for (int i = course_count; i > current_course + 1; i--) {
        level_save(i - 1, &ed_tmp2);
        level_load(i, &ed_tmp2);
    }
    level_load(current_course + 1, &ed_tmp);
    course_count++;
    ed_after_structure(current_course + 1);
    ed_panel_say(dup ? "HOLE DUPLICATED" : "NEW HOLE ADDED");
}

static void ed_delete_hole(void) {
    if (course_count <= 1) { ed_panel_say("CANNOT DELETE THE LAST HOLE"); return; }
    for (int i = current_course; i < course_count - 1; i++) {
        level_save(i + 1, &ed_tmp);
        level_load(i, &ed_tmp);
    }
    course_count--;
    ed_after_structure(current_course);
    ed_panel_say("HOLE DELETED");
}

static void ed_move_hole(int dir) {
    int j = current_course + dir;
    if (j < 0 || j >= course_count) return;
    level_save(current_course, &ed_tmp);
    level_save(j, &ed_tmp2);
    level_load(current_course, &ed_tmp2);
    level_load(j, &ed_tmp);
    ed_after_structure(j);
}

static void ed_reset_defaults(void) {
    load_defaults();
    ed_after_structure(0);
    ed_modified = 1;
    ed_panel_say("RESET TO BUILT-IN COURSES");
}

/* ---- editor <-> game switching ------------------------------------ */

static void editor_course_changed(void) {
    ed_sel_kind = SEL_NONE;
    ed_undo_n = 0;
    ed_drag.mode = DRAG_NONE;
    ed_name_focus = 0;
    ed_bg_dirty = 1;
    ed_dirty = 1;
}

static void toggle_editor(void) {
    editor_mode = !editor_mode;
    scorecard_open = 0;
    ed_panel = 0;
    ed_name_focus = 0;
    ed_confirm = 0;
    ed_drag.mode = DRAG_NONE;
    ed_sel_kind = SEL_NONE;
    for (int i = 0; i < MAX_HOLES; i++) { strokes[i] = 0; holed[i] = 0; }
    gate_clock = 0;
    set_ball_to_tee();
    build_bg();
    header_dirty = 1;
    need_full = 1;
    ed_bg_dirty = 1;
    ed_dirty = 1;
}

/* ---- drawing ------------------------------------------------------ */

static void rect_outline(int x, int y, int w, int h, uint32_t c) {
    fill_rect(x, y, w, 1, c, 255);
    fill_rect(x, y + h - 1, w, 1, c, 255);
    fill_rect(x, y, 1, h, c, 255);
    fill_rect(x + w - 1, y, 1, h, c, 255);
}

static void ed_draw_selection(void) {
    const Course* c = &courses[current_course];
    if (ed_sel_kind == SEL_TEE) ring((float)c->tee_x, (float)c->tee_y, 19.0f, 2.0f, C_ACCENT, 255);
    else if (ed_sel_kind == SEL_HOLE) ring((float)c->hole_x, (float)c->hole_y, (float)HOLE_RADIUS + 8.0f, 2.0f, C_ACCENT, 255);
    else if (ed_sel_kind == SEL_PORTAL_A && c->portal_radius)
        ring((float)c->portal_ax, (float)c->portal_ay, (float)c->portal_radius + 5.0f, 2.0f, C_ACCENT, 255);
    else if (ed_sel_kind == SEL_PORTAL_B && c->portal_radius)
        ring((float)c->portal_bx, (float)c->portal_by, (float)c->portal_radius + 5.0f, 2.0f, C_ACCENT, 255);
    else if (ed_sel_kind == SEL_ZONE && ed_sel_i < c->zone_count) {
        const Zone* z = &zone_store[current_course][ed_sel_i];
        if (z->type == ZONE_HILL) ring((float)z->x + (float)z->w * 0.5f, (float)z->y + (float)z->w * 0.5f, (float)z->w * 0.5f + 3.0f, 2.0f, C_ACCENT, 255);
        else rect_outline(z->x - 2, z->y - 2, z->w + 4, z->h + 4, C_ACCENT);
    } else if (ed_sel_kind == SEL_OBS && ed_sel_i < c->obstacle_count) {
        const Obstacle* o = &obs_store[current_course][ed_sel_i];
        if (o->type == OBSTACLE_BUMPER) ring((float)o->x, (float)o->y, (float)o->radius + 4.0f, 2.0f, C_ACCENT, 255);
        else if (o->type == OBSTACLE_WALL)
            capsule((float)o->x, (float)o->y, (float)o->w, (float)o->h, WALL_THICK + 9.0f, C_ACCENT, 90);
        else rect_outline(o->x - 2, o->y - 2, o->w + 4, o->h + 4, C_ACCENT);
    }
    Handle h[4];
    int n = ed_handles(h);
    for (int i = 0; i < n; i++) {
        fill_rect(h[i].x - 4, h[i].y - 4, 9, 9, 0xFFFFFFFF, 255);
        rect_outline(h[i].x - 4, h[i].y - 4, 9, 9, C_ACCENT);
    }
}

static void ed_draw_world_overlay(void) {
    const Course* c = &courses[current_course];
    /* faint 50px grid */
    for (int x = FIELD_X + 50 - (FIELD_X % 50); x < FIELD_X + FIELD_W; x += 50)
        fill_rect(x, FIELD_Y, 1, FIELD_H, 0xFFFFFFFF, 18);
    for (int y = FIELD_Y + 50 - (FIELD_Y % 50); y < FIELD_Y + FIELD_H; y += 50)
        fill_rect(FIELD_X, y, FIELD_W, 1, 0xFFFFFFFF, 18);

    /* gates: base position + ghost of the far end */
    for (int i = 0; i < c->obstacle_count; i++) {
        const Obstacle* o = &obs_store[current_course][i];
        if (o->type != OBSTACLE_GATE) continue;
        if (o->travel > 0) {
            fill_rect(o->x, o->y, o->w + o->travel, o->h, C_GOLD, 28);
            rect_outline(o->x + o->travel, o->y, o->w, o->h, C_GOLD);
        }
        wall_shadow(o->x, o->y, o->w, o->h);
        wall_body(o->x, o->y, o->w, o->h, 1);
    }
    centered_text(c->tee_x - 30, c->tee_y - 36, 60, "TEE", 0xFFFFFFFF, 1);
    centered_text(c->hole_x - 30, c->hole_y - 38, 60, "HOLE", 0xFFFFFFFF, 1);
    if (c->portal_radius) {
        centered_text(c->portal_ax - 10, c->portal_ay - 8, 20, "A", 0xFFFFFFFF, 1);
        centered_text(c->portal_bx - 10, c->portal_by - 8, 20, "B", 0xFFFFFFFF, 1);
    }

    /* shape being dragged out */
    if (ed_drag.mode == DRAG_CREATE) {
        Zone z; Obstacle o;
        int kind = ed_shape(ed_tool, ed_drag.px, ed_drag.py,
                            ed_drag.cx, ed_drag.cy, &z, &o);
        if (kind == 1) paint_zone(&z);
        else if (kind == 2) {
            if (o.type == OBSTACLE_BLOCK) { wall_shadow(o.x, o.y, o.w, o.h); wall_body(o.x, o.y, o.w, o.h, 0); }
            else if (o.type == OBSTACLE_GATE) { wall_shadow(o.x, o.y, o.w, o.h); wall_body(o.x, o.y, o.w, o.h, 1); }
            else draw_wall_segment(&o);
        }
    }
    ed_draw_selection();
}

static void ed_draw_toolbar(void) {
    for (int i = 0; i < TOOL_COUNT; i++)
        draw_button(FIELD_X + i * 66, 74, 64, 24, tool_names[i], i == ed_tool);
}

static void ed_draw_bar(void) {
    fill_rect(FIELD_X, BAR_Y, FIELD_W, 30, C_PANEL, 255);
    fill_rect(FIELD_X, BAR_Y, FIELD_W, 1, 0xFF33453C, 255);
    int tx = FIELD_X + 10, ty = BAR_Y + 7;
    if (ed_msg[0]) {
        text(tx, ty, ed_msg, C_GOLD, 1);
    } else if (ed_sel_kind != SEL_NONE && ed_tool == TOOL_SELECT) {
        const char* name = "OBJECT";
        const Course* c = &courses[current_course];
        if (ed_sel_kind == SEL_TEE) name = "TEE";
        else if (ed_sel_kind == SEL_HOLE) name = "CUP";
        else if (ed_sel_kind == SEL_PORTAL_A || ed_sel_kind == SEL_PORTAL_B) name = "PORTAL";
        else if (ed_sel_kind == SEL_ZONE && ed_sel_i < c->zone_count) name = zone_names[zone_store[current_course][ed_sel_i].type];
        else if (ed_sel_kind == SEL_OBS && ed_sel_i < c->obstacle_count) name = obstacle_names[obs_store[current_course][ed_sel_i].type];
        text(tx, ty, "SEL", C_MUTED, 1);
        text(tx + 42, ty, name, C_INK, 1);
    } else {
        text(tx, ty, tool_hints[ed_tool], C_MUTED, 1);
    }
    EdParam p[2];
    int n = ed_params(p);
    for (int i = 0; i < n; i++) {
        int bx = ED_PARAM_X(i);
        text(bx, ty, p[i].label, C_MINT, 1);
        draw_button(bx + 70, BAR_Y + 5, 20, 20, "-", 0);
        number_text(bx + 98, ty, *p[i].value, C_INK, 1);
        draw_button(bx + 142, BAR_Y + 5, 20, 20, "+", 0);
    }
    draw_button(ED_DEL_X, BAR_Y + 5, 48, 20, "DEL", 0);
    draw_button(ED_UNDO_X, BAR_Y + 5, 56, 20, "UNDO", 0);
    char snap_label[12] = "SNAP ";
    int sv = ed_snap_vals[ed_snap_idx];
    int len = 5;
    if (sv >= 10) snap_label[len++] = (char)('0' + sv / 10);
    snap_label[len++] = (char)('0' + sv % 10);
    snap_label[len] = '\0';
    draw_button(ED_SNAP_X, BAR_Y + 5, 104, 20, snap_label, 0);
}

static void ed_draw_panel(void) {
    const Course* c = &courses[current_course];
    fill_rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, rgb(20, 32, 27), 245);
    fill_rect(PANEL_X, PANEL_Y, PANEL_W, 2, C_GOLD, 255);
    text(238, 160, "HOLE SETTINGS", C_MINT, 2);
    text(238, 206, "NAME", C_MUTED, 1);
    fill_rect(310, 198, 356, 30, ed_name_focus ? 0xFF2B3933 : 0xFF121A16, 255);
    rect_outline(310, 198, 356, 30, ed_name_focus ? C_ACCENT : 0xFF40514A);
    {
        char shown[NAME_LEN + 2];
        int len = ed_strlen(name_store[current_course]);
        ed_copy_str(shown, name_store[current_course], NAME_LEN);
        if (ed_name_focus && len < NAME_LEN - 1) { shown[len] = '_'; shown[len + 1] = '\0'; }
        text(318, 205, shown, C_INK, 1);
    }
    text(238, 250, "PAR", C_MUTED, 1);
    draw_button(310, 242, 30, 30, "-", 0);
    number_text(354, 250, c->par, C_GOLD, 1);
    draw_button(384, 242, 30, 30, "+", 0);

    static const char* const labels[4][2] = {
        {"NEW HOLE", "DUPLICATE HOLE"}, {"MOVE EARLIER", "MOVE LATER"},
        {"DELETE HOLE", "RESET ALL"}, {"RELOAD SAVED", "EXPORT C FILE"}
    };
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 2; col++) {
            const char* label = labels[r][col];
            int armed = (r == 2 && col == 0 && ed_confirm == 1) || (r == 2 && col == 1 && ed_confirm == 2);
            if (armed) label = "CLICK AGAIN";
            draw_button(238 + col * 220, 296 + r * 36, 204, 30, label, armed);
        }
    }
    text(238, 450, ed_panel_msg, C_GOLD, 1);
    centered_text(PANEL_X + 4, 486, PANEL_W - 8, "H OR CLICK OUTSIDE TO CLOSE", C_MUTED, 1);
}

static void editor_render(void) {
    if (ed_msg[0] && mos_uptime() > ed_msg_until) { ed_msg[0] = '\0'; ed_dirty = 1; }
    if (!ed_dirty && !ed_bg_dirty && !need_full && !header_dirty) return;
    if (ed_bg_dirty || need_full || header_dirty) {
        build_bg();
        ed_bg_dirty = 0;
        need_full = 0;
        header_dirty = 0;
    }
    for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; i++) pixels[i] = bg[i];
    T = pixels;
    clip_all();
    ed_draw_world_overlay();
    ed_draw_toolbar();
    ed_draw_bar();
    if (ed_panel) ed_draw_panel();
    present_rect(0, 0, WIDTH, HEIGHT);
    ed_dirty = 0;
}

/* ---- input -------------------------------------------------------- */

static void ed_field_press(int x, int y) {
    Course* c = &courses[current_course];
    int sx = snapv(x), sy = snapv(y);
    ed_name_focus = 0;
    ed_confirm = 0;
    if (ed_tool == TOOL_SELECT) {
        Handle h[4];
        int n = ed_handles(h);
        for (int i = 0; i < n; i++) {
            if (iabs(x - h[i].x) <= 7 && iabs(y - h[i].y) <= 7) {
                ed_drag_begin(DRAG_HANDLE, h[i].id, x, y);
                return;
            }
        }
        int kind, idx;
        if (ed_hit(x, y, &kind, &idx)) {
            ed_sel_kind = kind; ed_sel_i = idx;
            ed_drag_begin(DRAG_MOVE, 0, x, y);
        } else {
            ed_sel_kind = SEL_NONE;
        }
        return;
    }
    if (ed_tool == TOOL_TEE || ed_tool == TOOL_HOLE) {
        ed_push_undo();
        int nx = clampi(sx, FIELD_X + BALL_RADIUS, FIELD_X + FIELD_W - BALL_RADIUS);
        int ny = clampi(sy, FIELD_Y + BALL_RADIUS, FIELD_Y + FIELD_H - BALL_RADIUS);
        if (ed_tool == TOOL_TEE) { c->tee_x = nx; c->tee_y = ny; ed_sel_kind = SEL_TEE; }
        else { c->hole_x = nx; c->hole_y = ny; ed_sel_kind = SEL_HOLE; }
        set_ball_to_tee();
        ed_bg_dirty = 1;
        return;
    }
    if (ed_tool == TOOL_BUMPER) {
        Obstacle o;
        o.type = OBSTACLE_BUMPER; o.x = sx; o.y = sy; o.w = o.h = 0;
        o.radius = 24; o.travel = 0; o.phase = 0;
        ed_add_obstacle(&o);
        return;
    }
    if (ed_tool == TOOL_PORTAL) {
        if (!c->portal_radius) {
            ed_push_undo();
            c->portal_ax = sx; c->portal_ay = sy;
            c->portal_bx = clampi(sx + 120, FIELD_X + 30, FIELD_X + FIELD_W - 30);
            c->portal_by = sy;
            c->portal_radius = 23;
            ed_sel_kind = SEL_PORTAL_A;
            ed_bg_dirty = 1;
        } else {
            int kind, idx;
            if (ed_hit(x, y, &kind, &idx) && (kind == SEL_PORTAL_A || kind == SEL_PORTAL_B)) ed_sel_kind = kind;
            else ed_say("ONLY ONE PORTAL PAIR");
        }
        return;
    }
    /* drag-to-create tools */
    ed_drag_begin(DRAG_CREATE, 0, x, y);
    ed_drag.px = sx; ed_drag.py = sy;      /* anchor stored snapped */
}

static void ed_course_buttons(int x, int y) {
    if (y >= 20 && y < 56 && x >= COURSE_BTN_X && x < COURSE_BTN_X + COURSE_COUNT * COURSE_BTN_STEP)
        select_course((x - COURSE_BTN_X) / COURSE_BTN_STEP);
}

static void ed_bar_click(int x, int y) {
    if (y < BAR_Y + 5 || y >= BAR_Y + 25) return;
    EdParam p[2];
    int n = ed_params(p);
    for (int i = 0; i < n; i++) {
        int bx = ED_PARAM_X(i);
        if (in_button(x, y, bx + 70, BAR_Y + 5, 20, 20)) { ed_param_step(i, -1); return; }
        if (in_button(x, y, bx + 142, BAR_Y + 5, 20, 20)) { ed_param_step(i, 1); return; }
    }
    if (in_button(x, y, ED_DEL_X, BAR_Y + 5, 48, 20)) ed_delete_sel();
    else if (in_button(x, y, ED_UNDO_X, BAR_Y + 5, 56, 20)) ed_undo_pop();
    else if (in_button(x, y, ED_SNAP_X, BAR_Y + 5, 104, 20)) { ed_snap_idx = (ed_snap_idx + 1) % 5; ed_dirty = 1; }
}

static void ed_save_clicked(void) {
    if (editor_save_file()) { ed_say("SAVED"); ed_panel_say("SAVED TO LEVELS.DAT"); }
    else { ed_say("SAVE FAILED"); ed_panel_say("SAVE FAILED - CHECK THE DISK"); }
    header_dirty = 1;
}

static void ed_panel_click(int x, int y) {
    Course* c = &courses[current_course];
    ed_dirty = 1;
    if (!in_button(x, y, PANEL_X, PANEL_Y, PANEL_W, PANEL_H)) {
        ed_panel = 0; ed_name_focus = 0; ed_confirm = 0; header_dirty = 1;
        return;
    }
    if (in_button(x, y, 310, 198, 356, 30)) {
        if (!ed_name_focus) ed_push_undo();
        ed_name_focus = 1;
        return;
    }
    ed_name_focus = 0;
    if (in_button(x, y, 310, 242, 30, 30)) { ed_push_undo(); c->par = clampi(c->par - 1, 1, 9); ed_confirm = 0; return; }
    if (in_button(x, y, 384, 242, 30, 30)) { ed_push_undo(); c->par = clampi(c->par + 1, 1, 9); ed_confirm = 0; return; }
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 2; col++) {
            if (!in_button(x, y, 238 + col * 220, 296 + r * 36, 204, 30)) continue;
            int armed_before = ed_confirm;
            ed_confirm = 0;
            if (r == 0 && col == 0) ed_new_hole(0);
            else if (r == 0 && col == 1) ed_new_hole(1);
            else if (r == 1 && col == 0) ed_move_hole(-1);
            else if (r == 1 && col == 1) ed_move_hole(1);
            else if (r == 2 && col == 0) { if (armed_before == 1) ed_delete_hole(); else { ed_confirm = 1; ed_panel_say("CLICK DELETE AGAIN TO CONFIRM"); } }
            else if (r == 2 && col == 1) { if (armed_before == 2) ed_reset_defaults(); else { ed_confirm = 2; ed_panel_say("CLICK RESET AGAIN TO CONFIRM"); } }
            else if (r == 3 && col == 0) {
                if (editor_load_file()) { ed_after_structure(clampi(current_course, 0, course_count - 1)); ed_modified = 0; ed_panel_say("RELOADED LEVELS.DAT"); }
                else ed_panel_say("NO VALID LEVELS.DAT FOUND");
            } else if (r == 3 && col == 1) {
                ed_panel_say(editor_export_file() ? "WROTE LEVELS_EXPORT.C.TXT" : "EXPORT FAILED");
            }
            return;
        }
    }
    ed_confirm = 0;
}

static void ed_press(int x, int y) {
    ed_dirty = 1;
    if (ed_panel) { ed_panel_click(x, y); return; }
    if (y < 72) {
        if (in_button(x, y, EDIT_BTN_X, 20, EDIT_BTN_W, 36)) toggle_editor();
        else if (in_button(x, y, 624, 20, 88, 36)) ed_save_clicked();
        else if (in_button(x, y, 722, 20, 126, 36)) { ed_panel = 1; ed_confirm = 0; ed_panel_msg[0] = '\0'; header_dirty = 1; }
        else ed_course_buttons(x, y);
        return;
    }
    if (y >= 74 && y < 98 && x >= FIELD_X) {
        int i = (x - FIELD_X) / 66;
        if (i >= 0 && i < TOOL_COUNT && (x - FIELD_X) % 66 < 64) ed_tool = i;
        return;
    }
    if (y >= BAR_Y) { ed_bar_click(x, y); return; }
    if (x >= FIELD_X && x < FIELD_X + FIELD_W && y >= FIELD_Y && y < FIELD_Y + FIELD_H) ed_field_press(x, y);
}

static void ed_drag_event(int x, int y) {
    if (ed_drag.mode == DRAG_NONE || ed_panel) return;
    int rx = clampi(x, FIELD_X, FIELD_X + FIELD_W), ry = clampi(y, FIELD_Y, FIELD_Y + FIELD_H);
    if (ed_drag.mode == DRAG_CREATE) {
        int nx = snapv(rx), ny = snapv(ry);
        if (nx != ed_drag.cx || ny != ed_drag.cy) { ed_drag.cx = nx; ed_drag.cy = ny; ed_dirty = 1; }
        return;
    }
    if (ed_drag.mode == DRAG_MOVE) {
        if (x == ed_drag.px && y == ed_drag.py && !ed_drag.pushed) return;
        ed_touch();
        ed_apply_move(x - ed_drag.px, y - ed_drag.py);
    } else {
        if (rx == ed_drag.px && ry == ed_drag.py && !ed_drag.pushed) return;
        ed_touch();
        ed_apply_handle(rx, ry);
    }
    if (ed_sel_kind == SEL_TEE || ed_sel_kind == SEL_HOLE) set_ball_to_tee();
    ed_bg_dirty = 1;
}

static void ed_release(int x, int y) {
    (void)x; (void)y;
    if (ed_drag.mode == DRAG_CREATE) {
        Zone z; Obstacle o;
        int kind = ed_shape(ed_tool, ed_drag.px, ed_drag.py, ed_drag.cx, ed_drag.cy, &z, &o);
        if (kind == 1) ed_add_zone(&z);
        else if (kind == 2) ed_add_obstacle(&o);
    }
    ed_drag.mode = DRAG_NONE;
    ed_bg_dirty = 1;
    ed_dirty = 1;
}

static void editor_mouse(const syscall_window_event_t* event) {
    int x = event->x, y = event->y;
    int left = (event->buttons & SYSCALL_MOUSE_BTN_LEFT) != 0;
    mouse_x = x;
    mouse_y = y;
    if (left && !mouse_down) ed_press(x, y);
    else if (left && mouse_down) ed_drag_event(x, y);
    else if (!left && mouse_down) ed_release(x, y);
    mouse_down = left;
}

static char key_char(uint32_t k, int shift) {
    if (k >= 4 && k <= 29) return (char)((shift ? 'A' : 'a') + (int)(k - 4));
    if (k >= 30 && k <= 38) return (char)('1' + (int)(k - 30));
    if (k == 39) return '0';
    if (k == 44) return ' ';
    if (k == 45) return '-';
    return 0;
}

static void editor_key(const syscall_window_event_t* event) {
    int shift = (event->mods & SYSCALL_INPUT_MOD_SHIFT) != 0;
    uint32_t k = event->key;
    ed_dirty = 1;
    if (ed_name_focus) {
        char* name = name_store[current_course];
        int len = ed_strlen(name);
        if (k == KEY_ENTER || k == KEY_ESC) { ed_name_focus = 0; return; }
        if (k == KEY_BKSP) { if (len > 0) name[len - 1] = '\0'; return; }
        char ch = key_char(k, shift);
        if (ch && len < NAME_LEN - 1) { name[len] = ch; name[len + 1] = '\0'; }
        return;
    }
    if (k == KEY_E) { toggle_editor(); return; }
    if (k == KEY_H) { ed_panel = !ed_panel; ed_confirm = 0; header_dirty = 1; return; }
    if (k == KEY_ESC) {
        if (ed_panel) { ed_panel = 0; header_dirty = 1; }
        else { ed_sel_kind = SEL_NONE; ed_drag.mode = DRAG_NONE; }
        return;
    }
    if (ed_panel) return;
    if (k == KEY_S) { ed_save_clicked(); return; }
    if (k == KEY_Z) { ed_undo_pop(); return; }
    if (k == KEY_DEL || k == KEY_BKSP) { ed_delete_sel(); return; }
    if (k == KEY_D) { ed_duplicate_sel(); return; }
    if (k == KEY_PLUS) { ed_param_step(0, 1); return; }
    if (k == KEY_MINUS) { ed_param_step(0, -1); return; }
    int step = shift ? 1 : ed_snap_vals[ed_snap_idx];
    if (k == KEY_LEFT) ed_nudge(-step, 0);
    else if (k == KEY_RIGHT) ed_nudge(step, 0);
    else if (k == KEY_UP) ed_nudge(0, -step);
    else if (k == KEY_DOWN) ed_nudge(0, step);
}

static void toggle_scorecard(void) {
    scorecard_open = !scorecard_open;
    header_dirty = 1;
    need_full = 1;
}

static void click(int x, int y) {
    if (scorecard_open) { toggle_scorecard(); return; }
    if (in_button(x, y, EDIT_BTN_X, 20, EDIT_BTN_W, 36)) { toggle_editor(); return; }
    if (y >= 20 && y < 56 && x >= COURSE_BTN_X &&
        x < COURSE_BTN_X + COURSE_COUNT * COURSE_BTN_STEP) {
        select_course((x - COURSE_BTN_X) / COURSE_BTN_STEP);
    } else if (in_button(x, y, 624, 20, 88, 36)) {
        restart_course();
    } else if (in_button(x, y, 722, 20, 126, 36)) {
        toggle_scorecard();
    } else if (holed[current_course] &&
               in_button(x, y, BTN_NEXT_X, BTN_NEXT_Y, BTN_NEXT_W, BTN_NEXT_H)) {
        if (current_course + 1 < COURSE_COUNT) select_course(current_course + 1);
        else toggle_scorecard();
    } else if (ball_aiming() && x >= FIELD_X && x < FIELD_X + FIELD_W &&
               y >= FIELD_Y && y < FIELD_Y + FIELD_H) {
        if (!power_active) {
            aim_at(x, y);
            start_power();
            charging = power_active;
        } else {
            fire();
        }
    }
}

static void mouse_event(const syscall_window_event_t* event) {
    if (editor_mode) { editor_mouse(event); return; }
    int x = event->x, y = event->y;
    int left = (event->buttons & SYSCALL_MOUSE_BTN_LEFT) != 0;
    mouse_x = x;
    mouse_y = y;
    if ((x != last_mx || y != last_my) && ball_aiming() && !scorecard_open) {
        last_mx = x;
        last_my = y;
        if (x >= FIELD_X && x < FIELD_X + FIELD_W && y >= FIELD_Y && y < FIELD_Y + FIELD_H) {
            aim_at(x, y);
            ball_dirty = 1;
        }
    }
    if (left && !mouse_down) click(x, y);
    if (!left && mouse_down) {
        if (charging && power_active) fire();      /* release = hit the ball */
        charging = 0;
    }
    mouse_down = left;
}

static void key_event(const syscall_window_event_t* event) {
    if (!event->buttons) return;
    if (editor_mode) { editor_key(event); return; }
    if (event->key == KEY_T) { toggle_scorecard(); return; }
    if (scorecard_open) return;
    if (event->key == KEY_E) { toggle_editor(); return; }
    if (event->key == KEY_R) { restart_course(); return; }
    if (event->key == KEY_N) {
        if (holed[current_course] && current_course + 1 < COURSE_COUNT)
            select_course(current_course + 1);
        return;
    }
    if (event->key == KEY_ESC) { cancel_power(); return; }
    if (!ball_aiming()) return;
    if (event->key == KEY_LEFT && !power_active) rotate_aim(-1);
    if (event->key == KEY_RIGHT && !power_active) rotate_aim(1);
    if (event->key == KEY_SPACE || event->key == KEY_ENTER) {
        if (!power_active) start_power();
        else fire();
    }
}

int main(void) {
    size_t regular_size = 0, medium_size = 0;
    int regular_loaded = load_font("0:/programs/mosdesk/fonts/Inter-Regular.otf",
                                   &regular_data, &regular_size);
    int medium_loaded = load_font("0:/programs/mosdesk/fonts/Inter-Medium.otf",
                                  &medium_data, &medium_size);
    if (regular_loaded || medium_loaded)
        moswm_text_load_fonts(regular_loaded ? regular_data : NULL,
                              regular_size,
                              medium_loaded ? medium_data : NULL,
                              medium_size);

    window_id = mos_window_create(0, 130, 75, WIDTH, HEIGHT, "Greenline Mini Golf");
    if (!window_id) return 1;
    mos_window_set_resizable(window_id, false);
    syscall_window_style_t style = {
        .title_bar = 0xFF1E2824, .title_text = 0xFFF1F3EE,
        .close_button = 0xFFE78370, .minimize_button = 0xFFF1C56B,
        .maximize_button = 0xFFB9E5C8, .close_icon = 0xFFFFFFFF,
        .minimize_icon = 0xFF17211D, .maximize_icon = 0xFF17211D,
        .border = 0xFF365344, .content_background = 0xFF161D1A
    };
    mos_window_set_style(window_id, style);

    pixels = (uint32_t*)malloc((size_t)WIDTH * HEIGHT * sizeof(uint32_t));
    bg = (uint32_t*)malloc((size_t)WIDTH * HEIGHT * sizeof(uint32_t));
    grass_layer = (uint32_t*)malloc((size_t)FIELD_W * FIELD_H * sizeof(uint32_t));
    if (!pixels || !bg || !grass_layer) { mos_window_close(window_id); return 1; }

    build_grass_layer();
    load_defaults();
    editor_load_file();          /* saved levels replace the built-in ones */
    select_course(0);

    long last_frame = mos_uptime();
    long accumulator = 0;
    for (;;) {
        syscall_window_event_t event;
        while (mos_window_next_event(window_id, &event) == SYS_SUCCESS) {
            if (event.type == SYSCALL_WINDOW_EVENT_CLOSE_REQUEST) {
                mos_window_close(window_id);
                return 0;
            }
            if (event.type == SYSCALL_WINDOW_EVENT_MOUSE) mouse_event(&event);
            if (event.type == SYSCALL_WINDOW_EVENT_KEY) key_event(&event);
        }

        long now = mos_uptime();
        long elapsed = now - last_frame;
        if (elapsed > 100) elapsed = 100;
        accumulator += elapsed;
        last_frame = now;
        while (accumulator >= 16) {
            tick();
            accumulator -= 16;
        }

        if (ball_moving || was_moving) ball_dirty = 1;
        was_moving = ball_moving;

        render();
        mos_sleep(8);
    }
}
