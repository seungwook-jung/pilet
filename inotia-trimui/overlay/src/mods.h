/*
 * mods.h -- data-driven modifications to the guest (libgame.so) for the
 * TrimUI Brick port. Loaded from a text file (mods.txt) next to the binary.
 *
 * Directives, one per line ('#' starts a comment):
 *
 *   ret   <symbol|0xaddr> <int>     the function returns <int> immediately
 *   nop   <symbol|0xaddr>           same as `ret X 0`
 *   poke  <symbol|0xaddr>[+off] u8|u16|u32|f32 <value>
 *                                   write guest memory after the image is
 *                                   relocated and the port's own patches ran
 *   trace <symbol|0xaddr> [max]     log every call (r0-r3, lr) up to <max>
 *                                   times (default 50); for reverse engineering
 *   fps   <ms>                      engine frame period (66 = stock 15 fps);
 *                                   this is the game's speed, not a smoothing
 *   off   <name>                    disable a built-in port patch by name
 *                                   (currently informational only)
 *
 * Addresses are *image-relative* (what `inotia_tool.py symbols` prints): the
 * loader rebases them by the image base. Symbol names come from the .dynsym
 * of libgame.so, which Com2uS shipped un-stripped.
 */
#ifndef MODS_H
#define MODS_H

#include <stdint.h>
#include "elf.h"

/* Parse the file. Safe to call when it does not exist (no mods). Must run
 * before patches_apply() so that `fps` can set g_frame_period_ms. */
int  mods_load(const char *path);

/* Apply ret/nop/poke directives. Call after elf_load(), patches_apply() and
 * patches_fill_vm_caches(), before any guest code runs. */
void mods_apply(const guest_image *img);

/* Hot-path hook used by cpu.c: non-zero when at least one `trace` is active.
 * mods_hit() returns 1 if pc is a traced function entry (it logs and keeps
 * going; the guest is never stopped). */
extern int g_mods_nhooks;
int  mods_hit(uint32_t pc, const uint32_t *regs);

#endif /* MODS_H */
