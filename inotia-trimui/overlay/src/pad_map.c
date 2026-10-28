/*
 * pad_map.c -- see pad_map.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "pad_map.h"
#include "input.h"
#include "guest.h"      /* host_log, host_now, LD32 */
#include "elf.h"

/* main.c (overlay): the loaded guest image, for UI_listControl. */
const guest_image *spike_image(void);

enum { ACT_NONE, ACT_TAP, ACT_MODIFIER, ACT_CLICK, ACT_CURSOR, ACT_DPAD };

typedef struct {
    int act;
    int x, y;            /* ACT_TAP */
    int held;            /* synthetic finger currently down */
} binding;

#define NBUTTONS SDL_CONTROLLER_BUTTON_MAX
static int g_left_stick  = ACT_CURSOR;
static int g_right_stick = ACT_NONE;
static float g_cursor_speed = 900.0f;
static int g_modifier_down;
static int g_stick_dir_l = -1, g_stick_dir_r = -1;   /* 0 up 1 down 2 left 3 right */
static double g_now;

/* Synthetic pointer ids: must not collide with the cursor (slot id 0) and
 * must be stable per button (input.c keys its slots by id). */
#define ID_BUTTON(b)  (1000 + (b))
#define ID_STICK(s)   (1200 + (s))
#define ID_MENU_TAP   1300

static const struct { const char *name; int btn; } k_names[] = {
    {"A", SDL_CONTROLLER_BUTTON_A}, {"B", SDL_CONTROLLER_BUTTON_B},
    {"X", SDL_CONTROLLER_BUTTON_X}, {"Y", SDL_CONTROLLER_BUTTON_Y},
    {"BACK", SDL_CONTROLLER_BUTTON_BACK}, {"SELECT", SDL_CONTROLLER_BUTTON_BACK},
    {"START", SDL_CONTROLLER_BUTTON_START},
    {"GUIDE", SDL_CONTROLLER_BUTTON_GUIDE}, {"MENU", SDL_CONTROLLER_BUTTON_GUIDE},
    {"L1", SDL_CONTROLLER_BUTTON_LEFTSHOULDER}, {"R1", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    {"L3", SDL_CONTROLLER_BUTTON_LEFTSTICK}, {"R3", SDL_CONTROLLER_BUTTON_RIGHTSTICK},
    {"DPAD_UP", SDL_CONTROLLER_BUTTON_DPAD_UP}, {"DPAD_DOWN", SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    {"DPAD_LEFT", SDL_CONTROLLER_BUTTON_DPAD_LEFT}, {"DPAD_RIGHT", SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    /* L2/R2 are axes in SDL; they are exposed as pseudo-buttons below. */
    {"L2", NBUTTONS + 0}, {"R2", NBUTTONS + 1},
    {NULL, 0}
};
#define NB_ALL (NBUTTONS + 2)
static binding g_all[NB_ALL];
static int g_trig_down[2];

static const char *btn_name(int b)
{
    for (int i = 0; k_names[i].name; i++)
        if (k_names[i].btn == b) return k_names[i].name;
    return "?";
}

static int btn_by_name(const char *n)
{
    for (int i = 0; k_names[i].name; i++)
        if (strcmp(k_names[i].name, n) == 0) return k_names[i].btn;
    return -1;
}

/* The PS Vita port's HUD positions (engine 960x544), mapped positionally:
 * SDL A = south = Vita CROSS, B = east = CIRCLE, X = west = SQUARE,
 * Y = north = TRIANGLE. */
static void set_defaults(void)
{
    memset(g_all, 0, sizeof(g_all));
#define TAP(b, px, py) do { g_all[b].act = ACT_TAP; g_all[b].x = px; g_all[b].y = py; } while (0)
    TAP(SDL_CONTROLLER_BUTTON_A, 880, 470);          /* attack / confirm   */
    TAP(SDL_CONTROLLER_BUTTON_B, 917, 365);          /* skill 1            */
    TAP(SDL_CONTROLLER_BUTTON_X, 757, 427);          /* skill 2            */
    TAP(SDL_CONTROLLER_BUTTON_Y, 827, 365);          /* item / shield      */
    TAP(SDL_CONTROLLER_BUTTON_DPAD_UP,    140, 341);
    TAP(SDL_CONTROLLER_BUTTON_DPAD_DOWN,  145, 507);
    TAP(SDL_CONTROLLER_BUTTON_DPAD_LEFT,   42, 417);
    TAP(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 245, 417);
    TAP(SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  540, 499);   /* quick slot 1 */
    TAP(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 640, 499);   /* quick slot 2 */
    TAP(NBUTTONS + 0, 735, 499);                          /* L2: quick slot 3 */
    TAP(SDL_CONTROLLER_BUTTON_START, 42, 272);            /* pause          */
    g_all[SDL_CONTROLLER_BUTTON_BACK].act = ACT_MODIFIER; /* SELECT held = cursor mode */
    g_all[NBUTTONS + 1].act = ACT_CLICK;                  /* R2 = click at cursor */
#undef TAP
    g_left_stick = ACT_CURSOR;
    g_right_stick = ACT_NONE;
}

static int parse_act(const char *s)
{
    if (strcmp(s, "tap") == 0) return ACT_TAP;
    if (strcmp(s, "modifier") == 0) return ACT_MODIFIER;
    if (strcmp(s, "click") == 0) return ACT_CLICK;
    if (strcmp(s, "cursor") == 0) return ACT_CURSOR;
    if (strcmp(s, "dpad") == 0) return ACT_DPAD;
    if (strcmp(s, "none") == 0) return ACT_NONE;
    return -1;
}

/* ---- UI_listControl navigation ------------------------------------------
 *
 * The engine keeps a live array of the touch widgets of the current screen
 * (UI_listControl: count at +0xc, entries pointer at +0x10, 0x2c bytes per
 * entry with centre x/y at +0xc/+0x10, size at +0x14/+0x18 and the click
 * callback at +0x28). Widgets are laid out in a 480x320 space; the touch the
 * engine receives is in 960x544 and is scaled down (x/2, y/1.7) before the
 * hit test -- both facts established by the Vita port (wr_touch.c) and the
 * wasm port (spike_dump_ui_controls). So navigating means: pick the widget
 * nearest in the pressed direction, and on A synthesise an ordinary tap at
 * its centre scaled up into touch space. Some screens hit-test ad hoc and
 * leave the list empty; there the field mapping / cursor mode stays active.
 */
#define UICTRL_LIST_OFF_COUNT   0xc
#define UICTRL_LIST_OFF_ENTRIES 0x10
#define UICTRL_ENTRY_STRIDE     0x2c
#define UICTRL_OFF_CENTER_X     0xc
#define UICTRL_OFF_CENTER_Y     0x10
#define UICTRL_OFF_WIDTH        0x14
#define UICTRL_OFF_HEIGHT       0x18
#define UICTRL_OFF_CLICK_CB     0x28

static int g_ui_nav = 1;
static float g_ui_sx = 2.0f, g_ui_sy = 544.0f / 320.0f;
static uint32_t g_ui_list;           /* guest address of UI_listControl, 0 = none */
static int g_ui_resolved;
static int g_menu_idx = -1;
static int g_menu_was_active;

static uint32_t ui_list(void)
{
    if (!g_ui_resolved) {
        g_ui_resolved = 1;
        const guest_image *img = spike_image();
        g_ui_list = img ? elf_find_symbol(img, "UI_listControl") : 0;
        host_log("pad: UI_listControl %s\n", g_ui_list ? "found" : "not found (no d-pad menu navigation)");
    }
    return g_ui_list;
}

static int ui_count(void)
{
    uint32_t lc = ui_list();
    if (!lc) return 0;
    uint32_t n = LD32(lc + UICTRL_LIST_OFF_COUNT);
    uint32_t arr = LD32(lc + UICTRL_LIST_OFF_ENTRIES);
    if (n > 64 || !arr || arr + n * UICTRL_ENTRY_STRIDE > GUEST_MEM_SIZE)
        return 0;
    return (int)n;
}

static uint32_t ui_entry(int i)
{
    return LD32(ui_list() + UICTRL_LIST_OFF_ENTRIES) + (uint32_t)i * UICTRL_ENTRY_STRIDE;
}

static int ui_clickable(int i)
{
    return LD32(ui_entry(i) + UICTRL_OFF_CLICK_CB) != 0;
}

static void ui_rect(int i, int *cx, int *cy, int *w, int *h)
{
    uint32_t e = ui_entry(i);
    *cx = (int)LD32(e + UICTRL_OFF_CENTER_X);
    *cy = (int)LD32(e + UICTRL_OFF_CENTER_Y);
    *w  = (int)LD32(e + UICTRL_OFF_WIDTH);
    *h  = (int)LD32(e + UICTRL_OFF_HEIGHT);
}

/* Re-evaluate every call (a handful of guest reads): is there a widget list
 * right now, and is the selection still a valid, clickable entry? */
static int menu_active(void)
{
    if (!g_ui_nav)
        return 0;
    int n = ui_count();
    int active = n > 0;
    if (!active) {
        g_menu_idx = -1;
    } else if (!g_menu_was_active || g_menu_idx < 0 || g_menu_idx >= n || !ui_clickable(g_menu_idx)) {
        g_menu_idx = -1;
        for (int i = 0; i < n; i++)
            if (ui_clickable(i)) { g_menu_idx = i; break; }
    }
    g_menu_was_active = active;
    return active && g_menu_idx >= 0;
}

/* dir: 0 up, 1 down, 2 left, 3 right. Nearest widget strictly on that side,
 * penalising sideways offset so a straight neighbour beats a diagonal one. */
static int nearest_in_direction(int cur, int dir)
{
    int cx, cy, cw, ch;
    ui_rect(cur, &cx, &cy, &cw, &ch);
    int best = -1;
    long best_score = 0;
    int n = ui_count();
    for (int i = 0; i < n; i++) {
        if (i == cur || !ui_clickable(i)) continue;
        int ex, ey, ew, eh;
        ui_rect(i, &ex, &ey, &ew, &eh);
        int dx = ex - cx, dy = ey - cy, primary, secondary;
        switch (dir) {
        case 0:  if (dy >= 0) continue; primary = -dy; secondary = dx; break;
        case 1:  if (dy <= 0) continue; primary = dy;  secondary = dx; break;
        case 2:  if (dx >= 0) continue; primary = -dx; secondary = dy; break;
        default: if (dx <= 0) continue; primary = dx;  secondary = dy; break;
        }
        long score = (long)primary + (long)(secondary < 0 ? -secondary : secondary) * 2;
        if (best < 0 || score < best_score) { best = i; best_score = score; }
    }
    return best;
}

static void menu_confirm(double now)
{
    int cx, cy, w, h;
    ui_rect(g_menu_idx, &cx, &cy, &w, &h);
    int tx = (int)(cx * g_ui_sx), ty = (int)(cy * g_ui_sy);
    host_log("pad: menu confirm widget %d centre (%d,%d) -> touch (%d,%d)\n", g_menu_idx, cx, cy, tx, ty);
    input_push_down(ID_MENU_TAP, tx, ty, now);
    input_push_move(ID_MENU_TAP, tx, ty, now);
    input_push_up(ID_MENU_TAP, tx, ty, now);
}

void pad_map_draw(void)
{
    if (!menu_active())
        return;
    int cx, cy, w, h;
    ui_rect(g_menu_idx, &cx, &cy, &w, &h);
    if (w <= 0) w = 40;
    if (h <= 0) h = 40;
    /* Widget rect in content (touch) space, padded 3 px. */
    int x0 = (int)((cx - w / 2.0f) * g_ui_sx) - 3, x1 = (int)((cx + w / 2.0f) * g_ui_sx) + 3;
    int y0 = (int)((cy - h / 2.0f) * g_ui_sy) - 3, y1 = (int)((cy + h / 2.0f) * g_ui_sy) + 3;
    int t = 3;
    /* dark outline then cyan core, four bars each */
    for (int p = 0; p < 2; p++) {
        float r = p ? 0.37f : 0.05f, g = p ? 0.94f : 0.23f, b = p ? 1.0f : 0.25f;
        int e = p ? 0 : 1;
        host_sdl_fill_content_rect(x0 - e, y0 - e, x1 - x0 + 2 * e, t + 2 * e, r, g, b);          /* top    */
        host_sdl_fill_content_rect(x0 - e, y1 - t - e, x1 - x0 + 2 * e, t + 2 * e, r, g, b);      /* bottom */
        host_sdl_fill_content_rect(x0 - e, y0 - e, t + 2 * e, y1 - y0 + 2 * e, r, g, b);          /* left   */
        host_sdl_fill_content_rect(x1 - t - e, y0 - e, t + 2 * e, y1 - y0 + 2 * e, r, g, b);      /* right  */
    }
}

/* ---- configuration -------------------------------------------------------- */

void pad_map_init(const char *path)
{
    set_defaults();
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        int n = 0;
        while (fgets(line, sizeof(line), f)) {
            char *h = strchr(line, '#');
            if (h) *h = 0;
            char *tok[4] = {0};
            int argc = 0;
            for (char *t = strtok(line, " \t\r\n"); t && argc < 4; t = strtok(NULL, " \t\r\n"))
                tok[argc++] = t;
            if (argc < 2) continue;
            for (char *p = tok[0]; *p; p++) *p = (char)toupper((unsigned char)*p);
            if (strcmp(tok[0], "CURSOR_SPEED") == 0) { g_cursor_speed = (float)atof(tok[1]); n++; continue; }
            if (strcmp(tok[0], "UI_NAV") == 0) { g_ui_nav = strcmp(tok[1], "off") != 0; n++; continue; }
            if (strcmp(tok[0], "UI_SCALE") == 0 && argc >= 3) {
                g_ui_sx = (float)atof(tok[1]); g_ui_sy = (float)atof(tok[2]); n++; continue;
            }
            int act = parse_act(tok[1]);
            if (act < 0) { host_log("pad: unknown action '%s'\n", tok[1]); continue; }
            if (strcmp(tok[0], "LEFTSTICK") == 0)  { g_left_stick = act; n++; continue; }
            if (strcmp(tok[0], "RIGHTSTICK") == 0) { g_right_stick = act; n++; continue; }
            int b = btn_by_name(tok[0]);
            if (b < 0) { host_log("pad: unknown button '%s'\n", tok[0]); continue; }
            g_all[b].act = act;
            if (act == ACT_TAP) {
                if (argc < 4) { host_log("pad: %s tap needs x y\n", tok[0]); g_all[b].act = ACT_NONE; continue; }
                g_all[b].x = atoi(tok[2]);
                g_all[b].y = atoi(tok[3]);
            }
            n++;
        }
        fclose(f);
        host_log("pad: %d line(s) from %s\n", n, path);
    } else {
        host_log("pad: no %s, using the built-in TrimUI Brick table\n", path);
    }
    for (int b = 0; b < NB_ALL; b++) {
        if (g_all[b].act == ACT_TAP)
            host_log("pad:   %-10s tap %d,%d\n", btn_name(b), g_all[b].x, g_all[b].y);
        else if (g_all[b].act == ACT_MODIFIER)
            host_log("pad:   %-10s modifier (cursor mode while held)\n", btn_name(b));
        else if (g_all[b].act == ACT_CLICK)
            host_log("pad:   %-10s click\n", btn_name(b));
    }
    host_log("pad:   LEFTSTICK=%s RIGHTSTICK=%s cursor_speed=%.0f ui_nav=%s ui_scale=%.2fx%.2f\n",
             g_left_stick == ACT_CURSOR ? "cursor" : g_left_stick == ACT_DPAD ? "dpad" : "none",
             g_right_stick == ACT_CURSOR ? "cursor" : g_right_stick == ACT_DPAD ? "dpad" : "none",
             g_cursor_speed, g_ui_nav ? "on" : "off", g_ui_sx, g_ui_sy);
}

int pad_map_stick_is_dpad(void)
{
    return g_left_stick != ACT_CURSOR;
}

/* ---- synthetic fingers ---------------------------------------------------- */

static void finger(int id, int x, int y, int down, double now)
{
    if (down) {
        input_push_down(id, x, y, now);
        input_push_move(id, x, y, now);
    } else {
        input_push_up(id, x, y, now);
    }
}

static void release_all_taps(double now)
{
    for (int b = 0; b < NB_ALL; b++) {
        if (g_all[b].held) {
            finger(ID_BUTTON(b), g_all[b].x, g_all[b].y, 0, now);
            g_all[b].held = 0;
        }
    }
}

/* Cursor-mode d-pad state (which directions are held while SELECT is down). */
static int g_cur_dir[4];

static int is_dpad(int b)
{
    return b == SDL_CONTROLLER_BUTTON_DPAD_UP || b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
           b == SDL_CONTROLLER_BUTTON_DPAD_LEFT || b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
}

static int dpad_dir(int b)
{
    return b == SDL_CONTROLLER_BUTTON_DPAD_UP ? 0 : b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ? 1 :
           b == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? 2 : 3;
}

static int handle(int b, int down, double now)
{
    if (b < 0 || b >= NB_ALL)
        return 0;
    binding *k = &g_all[b];

    if (k->act == ACT_MODIFIER) {
        g_modifier_down = down;
        if (!down) {
            memset(g_cur_dir, 0, sizeof(g_cur_dir));
            host_sdl_cursor_click(0);
        } else {
            /* Entering cursor mode: lift every HUD finger so a held d-pad
             * direction does not keep walking while the menu is used. */
            release_all_taps(now);
        }
        return 1;
    }

    if (g_modifier_down) {
        /* Cursor mode: d-pad moves, A (or any `click` binding) clicks. */
        if (is_dpad(b)) {
            g_cur_dir[dpad_dir(b)] = down;
            return 1;
        }
        if (b == SDL_CONTROLLER_BUTTON_A || k->act == ACT_CLICK) {
            host_sdl_cursor_click(down);
            return 1;
        }
        return k->act != ACT_NONE;
    }

    /* Widget navigation: the screen has registered touch widgets, so the
     * d-pad walks them and A taps the selected one. Other buttons keep their
     * field mapping (B/X/Y/L/R taps mostly do nothing on menus). */
    if (menu_active()) {
        if (is_dpad(b)) {
            if (down) {
                int next = nearest_in_direction(g_menu_idx, dpad_dir(b));
                if (next >= 0) g_menu_idx = next;
            }
            return 1;
        }
        if (b == SDL_CONTROLLER_BUTTON_A) {
            if (down) menu_confirm(now);
            return 1;
        }
    }

    switch (k->act) {
    case ACT_TAP:
        if (down && !k->held) { finger(ID_BUTTON(b), k->x, k->y, 1, now); k->held = 1; }
        else if (!down && k->held) { finger(ID_BUTTON(b), k->x, k->y, 0, now); k->held = 0; }
        return 1;
    case ACT_CLICK:
        host_sdl_cursor_click(down);
        return 1;
    default:
        return 0;
    }
}

int pad_map_button(int sdl_button, int down, double now_ms)
{
    g_now = now_ms;
    return handle(sdl_button, down, now_ms);
}

/* ---- sticks and triggers -------------------------------------------------- */

static int stick_dir(Sint16 ax, Sint16 ay)
{
    float fx = ax / 32767.0f, fy = ay / 32767.0f;
    if (fabsf(fx) < 0.5f && fabsf(fy) < 0.5f)
        return -1;
    if (fabsf(fx) >= fabsf(fy))
        return fx < 0 ? 2 : 3;
    return fy < 0 ? 0 : 1;
}

static const int k_dir_btn[4] = {
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
};

static void stick_as_dpad(SDL_GameController *pad, int which, int *state, double now)
{
    Sint16 ax = SDL_GameControllerGetAxis(pad, which ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_LEFTX);
    Sint16 ay = SDL_GameControllerGetAxis(pad, which ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_LEFTY);
    int d = stick_dir(ax, ay);
    if (d == *state)
        return;
    if (*state >= 0) {
        binding *k = &g_all[k_dir_btn[*state]];
        if (k->act == ACT_TAP) finger(ID_STICK(which * 4 + *state), k->x, k->y, 0, now);
    }
    if (d >= 0) {
        binding *k = &g_all[k_dir_btn[d]];
        if (k->act == ACT_TAP) finger(ID_STICK(which * 4 + d), k->x, k->y, 1, now);
    }
    *state = d;
}

static void stick_as_cursor(SDL_GameController *pad, int which, double dt_ms)
{
    Sint16 ax = SDL_GameControllerGetAxis(pad, which ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_LEFTX);
    Sint16 ay = SDL_GameControllerGetAxis(pad, which ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_LEFTY);
    float fx = ax / 32767.0f, fy = ay / 32767.0f;
    float mag = sqrtf(fx * fx + fy * fy);
    if (mag < 0.22f)
        return;
    float k = (mag - 0.22f) / 0.78f;
    if (k > 1.0f) k = 1.0f;
    k *= k;
    float step = g_cursor_speed * k * (float)(dt_ms / 1000.0);
    host_sdl_cursor_move(fx / mag * step, fy / mag * step);
}

void pad_map_tick(SDL_GameController *pad, double dt_ms, double now_ms)
{
    g_now = now_ms;
    /* A screen with widgets just appeared while a HUD finger was held (e.g.
     * walking into a shop with the d-pad down): lift it. */
    {
        static int was;
        int now_active = menu_active();
        if (now_active && !was)
            release_all_taps(now_ms);
        was = now_active;
    }
    /* Cursor mode: d-pad held directions move the pointer. */
    if (g_modifier_down) {
        float dx = 0, dy = 0;
        if (g_cur_dir[0]) dy -= 1;
        if (g_cur_dir[1]) dy += 1;
        if (g_cur_dir[2]) dx -= 1;
        if (g_cur_dir[3]) dx += 1;
        if (dx != 0 || dy != 0) {
            float m = sqrtf(dx * dx + dy * dy);
            float step = g_cursor_speed * (float)(dt_ms / 1000.0);
            host_sdl_cursor_move(dx / m * step, dy / m * step);
        }
    }
    if (!pad)
        return;
    /* L2/R2 are axes: turn them into edges. */
    for (int t = 0; t < 2; t++) {
        Sint16 v = SDL_GameControllerGetAxis(pad, t ? SDL_CONTROLLER_AXIS_TRIGGERRIGHT : SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int down = v > 16000;
        if (down != g_trig_down[t]) {
            g_trig_down[t] = down;
            handle(NBUTTONS + t, down, now_ms);
        }
    }
    /* The right stick, when it is the cursor, is independent of the modifier;
     * the left stick as cursor is platform_sdl's own cursor_nudge. */
    if (g_left_stick == ACT_DPAD)
        stick_as_dpad(pad, 0, &g_stick_dir_l, now_ms);
    if (g_right_stick == ACT_DPAD)
        stick_as_dpad(pad, 1, &g_stick_dir_r, now_ms);
    else if (g_right_stick == ACT_CURSOR)
        stick_as_cursor(pad, 1, dt_ms);
}
