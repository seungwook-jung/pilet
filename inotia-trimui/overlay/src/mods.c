/*
 * mods.c -- see mods.h.
 *
 * Everything here works on the guest image *after* relocation, through the
 * same LD/ST macros the interpreter uses, so a mod is just bytes in guest
 * memory. The function-return patch mirrors patches.c's patch_return_constant
 * but encodes any 32-bit value (the port's own version only encodes 0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mods.h"
#include "guest.h"
#include "elf.h"
#include "patches.h"

#define MODS_MAX 256
#define HOOKS_MAX 32

enum { MOD_RET, MOD_POKE, MOD_TRACE, MOD_OFF };

typedef struct {
    int      kind;
    char     target[96];     /* symbol name or 0xhex */
    uint32_t off;            /* poke: +offset */
    char     width[4];       /* poke: u8/u16/u32/f32 */
    int64_t  ival;           /* ret value / poke int value / trace max */
    double   fval;           /* poke f32 */
} mod_t;

static mod_t g_mods[MODS_MAX];
static int   g_nmods;

typedef struct {
    uint32_t pc;             /* entry address with the Thumb bit cleared */
    char     name[96];
    int      left;           /* remaining log lines */
    uint64_t calls;
} hook_t;

static hook_t g_hooks[HOOKS_MAX];
int g_mods_nhooks;

static const guest_image *g_img;

/* ---- parsing ------------------------------------------------------------- */

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int parse_int(const char *s, int64_t *out)
{
    char *end = NULL;
    long long v = strtoll(s, &end, 0);
    if (end == s) return 0;
    *out = (int64_t)v;
    return 1;
}

int mods_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        host_log("mods: no %s (no mods)\n", path);
        return 0;
    }
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *s = trim(line);
        if (!*s) continue;
        char *argv[6] = {0};
        int argc = 0;
        for (char *tok = strtok(s, " \t"); tok && argc < 6; tok = strtok(NULL, " \t"))
            argv[argc++] = tok;
        if (argc < 2) {
            host_log("mods: %s:%d: incomplete directive\n", path, lineno);
            continue;
        }
        if (g_nmods >= MODS_MAX) {
            host_log("mods: too many directives (max %d)\n", MODS_MAX);
            break;
        }
        mod_t *m = &g_mods[g_nmods];
        memset(m, 0, sizeof(*m));
        if (strcmp(argv[0], "fps") == 0) {
            int64_t v = 0;
            if (parse_int(argv[1], &v) && v > 0 && v < 1000) {
                g_frame_period_ms = (int)v;
                host_log("mods: frame period %d ms (changes game speed!)\n", (int)v);
            }
            continue;
        }
        if (strcmp(argv[0], "ret") == 0 || strcmp(argv[0], "nop") == 0) {
            m->kind = MOD_RET;
            snprintf(m->target, sizeof(m->target), "%s", argv[1]);
            m->ival = 0;
            if (strcmp(argv[0], "ret") == 0) {
                if (argc < 3 || !parse_int(argv[2], &m->ival)) {
                    host_log("mods: %s:%d: ret needs a value\n", path, lineno);
                    continue;
                }
            }
        } else if (strcmp(argv[0], "poke") == 0) {
            if (argc < 4) {
                host_log("mods: %s:%d: poke <target>[+off] <width> <value>\n", path, lineno);
                continue;
            }
            m->kind = MOD_POKE;
            char *plus = strchr(argv[1], '+');
            if (plus) {
                *plus = 0;
                int64_t o = 0;
                parse_int(plus + 1, &o);
                m->off = (uint32_t)o;
            }
            snprintf(m->target, sizeof(m->target), "%s", argv[1]);
            snprintf(m->width, sizeof(m->width), "%s", argv[2]);
            if (strcmp(m->width, "f32") == 0)
                m->fval = atof(argv[3]);
            else if (!parse_int(argv[3], &m->ival)) {
                host_log("mods: %s:%d: bad value\n", path, lineno);
                continue;
            }
        } else if (strcmp(argv[0], "trace") == 0) {
            m->kind = MOD_TRACE;
            snprintf(m->target, sizeof(m->target), "%s", argv[1]);
            m->ival = 50;
            if (argc >= 3) parse_int(argv[2], &m->ival);
        } else if (strcmp(argv[0], "off") == 0) {
            m->kind = MOD_OFF;
            snprintf(m->target, sizeof(m->target), "%s", argv[1]);
        } else {
            host_log("mods: %s:%d: unknown directive '%s'\n", path, lineno, argv[0]);
            continue;
        }
        g_nmods++;
    }
    fclose(f);
    host_log("mods: %d directive(s) from %s\n", g_nmods, path);
    return g_nmods;
}

/* ---- address resolution --------------------------------------------------- */

/* Returns the guest address (Thumb bit preserved for symbols), or 0. */
static uint32_t resolve(const guest_image *img, const char *target)
{
    if (target[0] == '0' && (target[1] == 'x' || target[1] == 'X')) {
        uint32_t rel = (uint32_t)strtoul(target, NULL, 16);
        return img->base + rel;
    }
    return elf_find_symbol(img, target);
}

/* ---- the function-return patch ------------------------------------------- */

static void patch_ret(uint32_t addr, uint32_t value, const char *name)
{
    int thumb = (addr & 1) != 0;
    uint32_t at = addr & ~1u;
    if (thumb) {
        if (value < 256) {
            ST16(at, 0x2000 | value);       /* MOVS r0, #imm8 */
            ST16(at + 2, 0x4770);           /* BX lr          */
        } else if ((at & 3) == 0) {
            ST16(at, 0x4800);               /* LDR r0, [pc, #0] -> literal at at+4 */
            ST16(at + 2, 0x4770);           /* BX lr */
            ST32(at + 4, value);
        } else {
            ST16(at, 0xBF00);               /* NOP (align the LDR)            */
            ST16(at + 2, 0x4800);           /* LDR r0, [pc, #0] -> literal at at+6 */
            ST16(at + 4, 0x4770);           /* BX lr */
            ST32(at + 6, value);
        }
    } else {
        ST32(at, 0xE59F0000);               /* LDR r0, [pc, #0] -> literal at at+8 */
        ST32(at + 4, 0xE12FFF1E);           /* BX lr */
        ST32(at + 8, value);
    }
    host_log("mods: %s @%08x (%s) -> return %u (0x%x)\n", name, at,
             thumb ? "Thumb" : "ARM", value, value);
}

void mods_apply(const guest_image *img)
{
    g_img = img;
    for (int i = 0; i < g_nmods; i++) {
        mod_t *m = &g_mods[i];
        if (m->kind == MOD_OFF) {
            host_log("mods: off %s (informational; built-in patches are not toggled yet)\n", m->target);
            continue;
        }
        uint32_t a = resolve(img, m->target);
        if (!a) {
            host_log("mods: '%s' not found -- skipped\n", m->target);
            continue;
        }
        switch (m->kind) {
        case MOD_RET:
            patch_ret(a, (uint32_t)m->ival, m->target);
            break;
        case MOD_POKE: {
            uint32_t at = (a & ~1u) + m->off;
            if (strcmp(m->width, "u8") == 0)        ST8(at, (uint8_t)m->ival);
            else if (strcmp(m->width, "u16") == 0)  ST16(at, (uint16_t)m->ival);
            else if (strcmp(m->width, "u32") == 0)  ST32(at, (uint32_t)m->ival);
            else if (strcmp(m->width, "f32") == 0) {
                float fv = (float)m->fval; uint32_t u; memcpy(&u, &fv, 4); ST32(at, u);
            } else {
                host_log("mods: bad width '%s' for %s\n", m->width, m->target);
                break;
            }
            host_log("mods: poke %s+%u @%08x %s = %lld\n", m->target, m->off, at,
                     m->width, (long long)m->ival);
            break;
        }
        case MOD_TRACE:
            if (g_mods_nhooks >= HOOKS_MAX) {
                host_log("mods: too many trace hooks (max %d)\n", HOOKS_MAX);
                break;
            }
            g_hooks[g_mods_nhooks].pc = a & ~1u;
            snprintf(g_hooks[g_mods_nhooks].name, sizeof(g_hooks[0].name), "%s", m->target);
            g_hooks[g_mods_nhooks].left = (int)m->ival;
            g_mods_nhooks++;
            host_log("mods: trace %s @%08x (%lld lines)\n", m->target, a & ~1u, (long long)m->ival);
            break;
        }
    }
}

/* ---- trace hook ----------------------------------------------------------- */

int mods_hit(uint32_t pc, const uint32_t *regs)
{
    for (int i = 0; i < g_mods_nhooks; i++) {
        hook_t *h = &g_hooks[i];
        if (h->pc != pc)
            continue;
        h->calls++;
        if (h->left > 0) {
            h->left--;
            host_log("trace: %s(r0=%08x r1=%08x r2=%08x r3=%08x) lr=%08x #%llu\n",
                     h->name, regs[0], regs[1], regs[2], regs[3], regs[14],
                     (unsigned long long)h->calls);
            if (h->left == 0)
                host_log("trace: %s: limit reached, silent from now on\n", h->name);
        }
        return 1;
    }
    return 0;
}
