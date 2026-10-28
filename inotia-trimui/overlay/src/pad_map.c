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
#include "guest.h"      /* host_log, host_now */

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
    host_log("pad:   LEFTSTICK=%s RIGHTSTICK=%s cursor_speed=%.0f\n",
             g_left_stick == ACT_CURSOR ? "cursor" : g_left_stick == ACT_DPAD ? "dpad" : "none",
             g_right_stick == ACT_CURSOR ? "cursor" : g_right_stick == ACT_DPAD ? "dpad" : "none",
             g_cursor_speed);
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

static int handle(int b, int down, double now)
{
    if (b < 0 || b >= NB_ALL)
        return 0;
    binding *k = &g_all[b];
    int is_dpad = (b == SDL_CONTROLLER_BUTTON_DPAD_UP || b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
                   b == SDL_CONTROLLER_BUTTON_DPAD_LEFT || b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

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
        if (is_dpad) {
            int d = b == SDL_CONTROLLER_BUTTON_DPAD_UP ? 0 : b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ? 1 :
                    b == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? 2 : 3;
            g_cur_dir[d] = down;
            return 1;
        }
        if (b == SDL_CONTROLLER_BUTTON_A || k->act == ACT_CLICK) {
            host_sdl_cursor_click(down);
            return 1;
        }
        return k->act != ACT_NONE;
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
