/*
 * pad_map.h -- gamepad -> engine touch mapping for handhelds without a
 * touchscreen (TrimUI Brick: d-pad, A/B/X/Y, L1/L2/R1/R2, Start/Select, no
 * analog sticks). The engine is touch-only, so every button becomes a
 * synthetic finger held at a fixed position of the engine's 960x544 HUD; the
 * positions come from the PS Vita port's wr_touch.c table.
 *
 * pad.txt (next to the binary) overrides the built-in table:
 *
 *   A         tap 880 470        # engine content coordinates (960x544)
 *   DPAD_UP   tap 140 341
 *   SELECT    modifier           # held: d-pad moves the cursor, A clicks
 *   LEFTSTICK cursor|dpad|none   # stick behaviour (Smart Pro has sticks)
 *   RIGHTSTICK cursor|dpad|none
 *   L3        click|none
 *   cursor_speed 900             # content px/s in cursor mode
 *
 * Button names: A B X Y BACK/SELECT START GUIDE/MENU L1 R1 L2 R2 L3 R3
 * DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT. SDL's A is the *south* button.
 */
#ifndef PAD_MAP_H
#define PAD_MAP_H

#include <SDL.h>

/* Read pad.txt (or the defaults) and log the table. */
void pad_map_init(const char *path);

/* A controller button edge. Call for every SDL_CONTROLLERBUTTONDOWN/UP; the
 * function synthesises the touch itself through input_push_*. Returns 1 if
 * the button had a mapping. `now_ms` is host_now()*1000. */
int pad_map_button(int sdl_button, int down, double now_ms);

/* Per-tick work: stick-as-dpad, cursor movement in modifier mode, auto
 * release. `pad` may be NULL. */
void pad_map_tick(SDL_GameController *pad, double dt_ms, double now_ms);

/* 1 when the left stick is NOT the cursor (so platform_sdl's own cursor
 * nudge must stay out of the way). */
int pad_map_stick_is_dpad(void);

/* Supplied by platform_sdl.c (the cursor lives there). */
void host_sdl_cursor_move(float dx, float dy);
void host_sdl_cursor_click(int down);

#endif /* PAD_MAP_H */
