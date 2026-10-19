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
 *    - Click (or SPACE): start the power meter.  It swings up and down.
 *    - Click (or SPACE) again: hit the ball with the current power.
 *    - ESC: cancel the power meter.   R: replay hole.  T: scorecard.
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

enum { ZONE_SAND, ZONE_ROUGH, ZONE_WATER, ZONE_SLOPE };
enum { OBSTACLE_BLOCK, OBSTACLE_BUMPER, OBSTACLE_GATE };
enum { KEY_SPACE = 44, KEY_ENTER = 40, KEY_ESC = 41, KEY_T = 23, KEY_R = 21,
       KEY_N = 17, KEY_LEFT = 80, KEY_RIGHT = 79, KEY_UP = 82, KEY_DOWN = 81 };

typedef struct {
    int type, x, y, w, h, slope_x, slope_y;
} Zone;

typedef struct {
    int type, x, y, w, h, radius, travel, phase;
} Obstacle;

typedef struct {
    const char* name;
    const char* note;
    int tee_x, tee_y, hole_x, hole_y, par;
    const Zone* zones;
    int zone_count;
    const Obstacle* obstacles;
    int obstacle_count;
    int portal_ax, portal_ay, portal_bx, portal_by, portal_radius;
} Course;

static const Zone zones_first[] = {
    {ZONE_ROUGH, 280, 270, 150, 112, 0, 0}
};
static const Obstacle obstacles_first[] = {
    {OBSTACLE_BUMPER, 420, 220, 0, 0, 25, 0, 0},
    {OBSTACLE_BUMPER, 590, 412, 0, 0, 25, 0, 0}
};

static const Zone zones_sand[] = {
    {ZONE_SAND, 250, 274, 170, 120, 0, 0},
    {ZONE_SAND, 540, 385, 138, 102, 0, 0}
};
static const Obstacle obstacles_sand[] = {
    {OBSTACLE_BLOCK, 465, 186, 22, 234, 0, 0, 0},
    {OBSTACLE_BLOCK, 465, 468, 180, 22, 0, 0, 0},
    {OBSTACLE_BUMPER, 690, 270, 0, 0, 29, 0, 0}
};

static const Zone zones_bank[] = {
    {ZONE_ROUGH, 220, 145, 116, 390, 0, 0},
    {ZONE_SLOPE, 518, 145, 112, 390, 24, 0}
};
static const Obstacle obstacles_bank[] = {
    {OBSTACLE_BLOCK, 382, 154, 23, 252, 0, 0, 0},
    {OBSTACLE_BLOCK, 475, 360, 23, 219, 0, 0, 0},
    {OBSTACLE_BUMPER, 688, 236, 0, 0, 32, 0, 0},
    {OBSTACLE_BUMPER, 693, 446, 0, 0, 28, 0, 0}
};

static const Zone zones_water[] = {
    {ZONE_WATER, 344, 256, 210, 194, 0, 0},
    {ZONE_SAND, 602, 160, 98, 128, 0, 0},
    {ZONE_SAND, 602, 417, 98, 128, 0, 0}
};
static const Obstacle obstacles_water[] = {
    {OBSTACLE_BLOCK, 305, 148, 20, 153, 0, 0, 0},
    {OBSTACLE_BLOCK, 305, 402, 20, 167, 0, 0, 0},
    {OBSTACLE_BLOCK, 562, 148, 19, 146, 0, 0, 0},
    {OBSTACLE_BLOCK, 562, 408, 19, 161, 0, 0, 0},
    {OBSTACLE_BUMPER, 715, 350, 0, 0, 30, 0, 0}
};

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

static const Course courses[] = {
    {"First Green", "A clean line and a gentle first putt.",
     105, 349, 806, 349, 3, zones_first, 1, obstacles_first, 2,
     0, 0, 0, 0, 0},
    {"Sandbar", "Sand takes speed away. Bank around the divider.",
     105, 349, 807, 349, 4, zones_sand, 2, obstacles_sand, 3,
     0, 0, 0, 0, 0},
    {"Bank Shot", "Use the rails, then ride the right-hand slope.",
     105, 522, 806, 151, 5, zones_bank, 2, obstacles_bank, 4,
     0, 0, 0, 0, 0},
    {"Waterline", "The pond is a penalty. Find the narrow crossing.",
     105, 349, 807, 349, 4, zones_water, 3, obstacles_water, 5,
     0, 0, 0, 0, 0},
    {"Clockwork", "Catch the moving gate and take the portal shortcut.",
     105, 349, 806, 349, 5, zones_clockwork, 3, obstacles_clockwork, 4,
     280, 350, 670, 350, 23}
};

#define COURSE_COUNT ((int)(sizeof(courses) / sizeof(courses[0])))

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
static int strokes[COURSE_COUNT];
static int holed[COURSE_COUNT];
static int scorecard_open;
static int ball_moving;
static int sinking, sink_t;
static int power_active;
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

/* anti-aliased thick segment with round caps */
static void capsule(float x0, float y0, float x1, float y1, float w,
                    uint32_t c, int a) {
    float hw = w * 0.5f;
    float minx = x0 < x1 ? x0 : x1, maxx = x0 < x1 ? x1 : x0;
    float miny = y0 < y1 ? y0 : y1, maxy = y0 < y1 ? y1 : y0;
    float vx = x1 - x0, vy = y1 - y0;
    float len2 = vx * vx + vy * vy;
    float lim = (hw + 1.0f) * (hw + 1.0f);
    for (int y = (int)(miny - hw - 1.0f); y <= (int)(maxy + hw + 2.0f); y++) {
        for (int x = (int)(minx - hw - 1.0f); x <= (int)(maxx + hw + 2.0f); x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
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

static void draw_hole(int x, int y) {
    float r = (float)HOLE_RADIUS;
    disc((float)x, (float)y, r + 3.0f, rgb(20, 70, 24), 80);
    disc((float)x, (float)y, r + 1.4f, rgb(176, 206, 156), 120);
    disc((float)x, (float)y, r, rgb(44, 44, 44), 255);
    disc((float)x + 0.5f, (float)y + 1.8f, r - 1.6f, rgb(8, 8, 8), 255);
}

static float zu_x, zu_y, zu_half;

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
        int r = 88 + (int)(t * 20.0f) + n / 2;
        int g = 176 + (int)(t * 24.0f) + n;
        int b = 76 + n / 2;
        c = rgb(r, g, b);
        if (depth < 1.5f) c = mix(c, rgb(50, 120, 46), (int)((1.5f - depth) * 110.0f));
    }
    return c;
}

static void paint_zone(const Zone* z) {
    float radius = z->type == ZONE_WATER ? 18.0f : (z->type == ZONE_SAND ? 16.0f : 10.0f);
    if (z->type == ZONE_SLOPE) {
        float sl = fsqrt((float)(z->slope_x * z->slope_x + z->slope_y * z->slope_y));
        if (sl < 0.01f) sl = 1.0f;
        zu_x = (float)z->slope_x / sl;
        zu_y = (float)z->slope_y / sl;
        zu_half = (float)z->w * 0.5f * fabs_(zu_x) + (float)z->h * 0.5f * fabs_(zu_y);
        if (zu_half < 1.0f) zu_half = 1.0f;
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
        float ppx = -zu_y, ppy = zu_x;
        for (int cy = z->y + 26; cy < z->y + z->h - 14; cy += 44) {
            int stagger = (((cy - z->y) / 44) & 1) ? 21 : 0;
            for (int cx = z->x + 24; cx < z->x + z->w - 16; cx += 42) {
                float fx = (float)(cx + stagger), fy = (float)cy;
                if (fx > (float)(z->x + z->w - 16)) continue;
                float tx = fx + zu_x * 7.0f, ty = fy + zu_y * 7.0f;
                float ax = fx - zu_x * 5.0f + ppx * 8.0f, ay = fy - zu_y * 5.0f + ppy * 8.0f;
                float bx = fx - zu_x * 5.0f - ppx * 8.0f, by = fy - zu_y * 5.0f - ppy * 8.0f;
                capsule(ax, ay, tx, ty, 2.2f, rgb(176, 232, 136), 175);
                capsule(bx, by, tx, ty, 2.2f, rgb(176, 232, 136), 175);
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
        draw_button(330 + i * 54, 20, 42, 36, label, i == current_course);
    }
    draw_button(624, 20, 88, 36, "REPLAY", 0);
    draw_button(722, 20, 126, 36, "SCORECARD", scorecard_open);

    const Course* course = &courses[current_course];
    text(FIELD_X, 76, course->name, C_INK, 2);
    text(250, 84, course->note, C_MUTED, 1);

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
    text(FIELD_X + 14, BAR_Y + 7, "MOUSE/ARROWS AIM   CLICK/SPACE: POWER, THEN HIT   ESC CANCEL",
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
    }
    for (int i = 0; i < course->obstacle_count; i++) {
        const Obstacle* o = &course->obstacles[i];
        if (o->type == OBSTACLE_BLOCK) wall_body(o->x, o->y, o->w, o->h, 0);
        else if (o->type == OBSTACLE_BUMPER) draw_bumper(o->x, o->y, o->radius);
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
    const Zone* slope = NULL;
    int on_slope = active_zone(ball_x, ball_y, ZONE_SLOPE, &slope);
    if (on_slope) {
        ball_vx += (float)slope->slope_x * SLOPE_ACCEL * dt;
        ball_vy += (float)slope->slope_y * SLOPE_ACCEL * dt;
    }

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
    if (!on_slope && fsqrt(ball_vx * ball_vx + ball_vy * ball_vy) < 6.0f) {
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
    if (scorecard_open) return;
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
        int y = 232 + i * 43;
        if (i == current_course) fill_rect(232, y - 3, 438, 34, 0xFF284238, 255);
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

static void toggle_scorecard(void) {
    scorecard_open = !scorecard_open;
    header_dirty = 1;
    need_full = 1;
}

static void click(int x, int y) {
    if (scorecard_open) { toggle_scorecard(); return; }
    if (y >= 20 && y < 56 && x >= 330 && x < 330 + COURSE_COUNT * 54) {
        select_course((x - 330) / 54);
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
        } else {
            fire();
        }
    }
}

static void mouse_event(const syscall_window_event_t* event) {
    int x = event->x, y = event->y;
    int left = (event->buttons & SYSCALL_MOUSE_BTN_LEFT) != 0;
    mouse_x = x;
    mouse_y = y;
    if ((x != last_mx || y != last_my) && ball_aiming() && !power_active &&
        !scorecard_open) {
        last_mx = x;
        last_my = y;
        if (x >= FIELD_X && x < FIELD_X + FIELD_W && y >= FIELD_Y && y < FIELD_Y + FIELD_H) {
            aim_at(x, y);
            ball_dirty = 1;
        }
    }
    if (left && !mouse_down) click(x, y);
    mouse_down = left;
}

static void key_event(const syscall_window_event_t* event) {
    if (!event->buttons) return;
    if (event->key == KEY_T) { toggle_scorecard(); return; }
    if (scorecard_open) return;
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
