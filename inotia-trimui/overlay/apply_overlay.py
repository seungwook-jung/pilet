#!/usr/bin/env python3
"""
apply_overlay.py -- apply the TrimUI overlay to a checkout of Inotia3-wasm.

    apply_overlay.py <inotia3-wasm checkout>

Copies overlay/src/*.c|*.h into <checkout>/src and makes a handful of exact,
asserted text edits to main.c, cpu.c and platform_sdl.c so that:

  * mods.txt is loaded (mods_load before patches_apply, mods_apply after
    patches_fill_vm_caches);
  * the interpreter loop calls mods_hit() for `trace` directives;
  * the SDL host routes gamepad buttons through pad_map (HUD taps, cursor
    mode) and exposes two cursor helpers.

Every edit asserts that the anchor text exists exactly once (or the stated
number of times), so an upstream change shows up as a loud failure here
rather than as a silently half-patched tree. Re-running on an already patched
tree is a no-op.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def patch(path, old, new, count=1, marker=None):
    with open(path, encoding="utf-8") as fh:
        s = fh.read()
    if marker and marker in s:
        return False  # already applied
    n = s.count(old)
    if n != count:
        sys.exit("apply_overlay: %s: expected %d occurrence(s) of anchor, found %d:\n%s"
                 % (path, count, n, old))
    s = s.replace(old, new)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(s)
    return True


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    src = os.path.join(root, "src")
    if not os.path.isfile(os.path.join(src, "cpu.c")):
        sys.exit("apply_overlay: %s does not look like an Inotia3-wasm checkout" % root)

    # 1. new sources
    for f in os.listdir(os.path.join(HERE, "src")):
        if f.endswith((".c", ".h")):
            shutil.copyfile(os.path.join(HERE, "src", f), os.path.join(src, f))
            print("  copied src/%s" % f)

    # 2. main.c: load + apply mods around the port's own patches
    main_c = os.path.join(src, "main.c")
    if patch(main_c, '#include "patches.h"\n', '#include "patches.h"\n#include "mods.h"\n',
             marker='#include "mods.h"'):
        patch(main_c, "patches_apply(&g_img);", 'mods_load("mods.txt");\npatches_apply(&g_img);')
        patch(main_c, "patches_fill_vm_caches(&g_img);", "patches_fill_vm_caches(&g_img);\nmods_apply(&g_img);")
        patch(main_c, "static guest_image g_img;\n",
              "static guest_image g_img;\n/* overlay: pad_map.c reads UI_listControl through this. */\n"
              "const guest_image *spike_image(void) { return &g_img; }\n")
        print("  patched main.c")

    # 3. cpu.c: trace hook next to the one-shot stop-at check
    cpu_c = os.path.join(src, "cpu.c")
    if patch(cpu_c, '#include "cpu.h"\n', '#include "cpu.h"\n#include "mods.h"\n', marker='#include "mods.h"'):
        patch(cpu_c, "        if (g_stop_at && pc == g_stop_at) {",
              "        if (g_mods_nhooks)\n            mods_hit(pc, R);\n        if (g_stop_at && pc == g_stop_at) {")
        print("  patched cpu.c")

    # 4. platform_sdl.c: pad mapping
    sdl_c = os.path.join(src, "platform_sdl.c")
    if patch(sdl_c, '#include "input.h"\n', '#include "input.h"\n#include "pad_map.h"\n', marker='#include "pad_map.h"'):
        patch(sdl_c, "    SDL_ShowCursor(SDL_DISABLE);      /* the port draws its own pointer */",
              '    pad_map_init("pad.txt");\n    SDL_ShowCursor(SDL_DISABLE);      /* the port draws its own pointer */')
        patch(sdl_c, "        case SDL_CONTROLLERBUTTONDOWN:\n",
              "        case SDL_CONTROLLERBUTTONDOWN:\n            pad_map_button(e.cbutton.button, 1, now);\n")
        patch(sdl_c, "        case SDL_CONTROLLERBUTTONUP:\n",
              "        case SDL_CONTROLLERBUTTONUP:\n            pad_map_button(e.cbutton.button, 0, now);\n")
        patch(sdl_c, "    cursor_nudge(dt_ms);\n    cursor_hide_if_idle();",
              "    cursor_nudge(dt_ms);\n    pad_map_tick(g_pad, dt_ms, now);\n    cursor_hide_if_idle();")
        patch(sdl_c, "    if (!g_pad)\n        return;\n    Sint16 ax = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);",
              "    if (!g_pad)\n        return;\n    if (pad_map_stick_is_dpad())\n        return;\n    Sint16 ax = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);")
        patch(sdl_c, "    cursor_draw();\n", "    pad_map_draw();\n    cursor_draw();\n")
        patch(sdl_c, "static void cursor_draw(void)\n",
              "/* pad_map.c: a filled rectangle in content coordinates (960x544), drawn\n"
              " * through the driver the way the cursor is (scissor + clear). */\n"
              "void host_sdl_fill_content_rect(int x, int y, int w, int h, float r, float g, float b)\n{\n"
              "    if (!g_gl_inited)\n        return;\n"
              "    if (!cg.ok && !cursor_gfx_init())\n        return;\n"
              "    int lx, ly, lw, lh;\n    content_target(&lx, &ly, &lw, &lh);\n"
              "    float sx = (float)lw / CONTENT_W, sy = (float)lh / CONTENT_H;\n"
              "    if (sx <= 0.0f || sy <= 0.0f)\n        return;\n"
              "    cg.BindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, 0);\n"
              "    cg.Viewport(0, 0, g_surface_w, g_surface_h);\n"
              "    cg.Disable(0x0C11 /* GL_SCISSOR_TEST */);\n"
              "    cg.ColorMask(1, 1, 1, 1);\n"
              "    cg.Enable(0x0C11 /* GL_SCISSOR_TEST */);\n"
              "    gl_rect(lx + (int)(x * sx), ly + (int)(y * sy), (int)(w * sx) + 1, (int)(h * sy) + 1, r, g, b);\n"
              "    cg.Disable(0x0C11 /* GL_SCISSOR_TEST */);\n}\n\n"
              "static void cursor_draw(void)\n")
        patch(sdl_c, "/* Window pixels -> content coordinates. */",
              "/* Cursor helpers for pad_map.c (the cursor is private to this file). */\n"
              "void host_sdl_cursor_move(float dx, float dy)\n{\n"
              "    cursor_set((int)(cur.x + dx), (int)(cur.y + dy));\n}\n\n"
              "void host_sdl_cursor_click(int down)\n{\n"
              "    double now = host_now() * 1000.0;\n"
              "    if (down == cur.down)\n        return;\n"
              "    cur.down = down;\n    cur.visible = 1;\n    cur.last_input = now;\n"
              "    push_tap(down, now);\n}\n\n"
              "/* Window pixels -> content coordinates. */")
        print("  patched platform_sdl.c")
    print("overlay applied to %s" % root)


if __name__ == "__main__":
    main()
