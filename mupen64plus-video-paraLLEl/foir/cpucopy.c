/* Joey64: see cpucopy.h. Built with foir (its flags and rename header). */
#include "cpucopy.h"
#include "rdp.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#define CC_PAGE_SHIFT 12
#define CC_PAGE (1u << CC_PAGE_SHIFT)

static struct rdp cc_rdp;
static bool cc_on;
static const uint8_t *cc_alias;
static uint8_t *cc_shadow;
static uint32_t cc_size;
static uint8_t *cc_owned;            /* per 4 KB page: 1 = foir's bytes are current there */

/* counters (verify runs in the fault handler: atomics, no locks) */
static _Atomic uint64_t cc_ver_pages, cc_ver_bytes, cc_ver_bad, cc_ver_badpages, cc_ver_unowned;
static _Atomic uint32_t cc_ver_first_bad = 0xffffffffu;
static uint64_t cc_cmds, cc_seed_pages, cc_tex_bytes, cc_ns, cc_released;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Make [start, end) current in the shadow for foir to draw into: pages not owned are copied in and become owned. */
static void own_range(uint32_t start, uint32_t end)
{
    if (end > cc_size) end = cc_size;
    if (start >= end) return;
    for (uint32_t p = start >> CC_PAGE_SHIFT; p <= (end - 1) >> CC_PAGE_SHIFT; p++)
        if (!cc_owned[p]) {
            memcpy(cc_shadow + ((size_t)p << CC_PAGE_SHIFT), cc_alias + ((size_t)p << CC_PAGE_SHIFT), CC_PAGE);
            cc_owned[p] = 1;
            cc_seed_pages++;
        }
}

/* A texture source [start, end): bytes on pages foir doesn't own come from real RDRAM (copied every load). */
static void fresh_range(uint32_t start, uint32_t end)
{
    if (end > cc_size) end = cc_size;
    if (start >= end) return;
    uint32_t p0 = start >> CC_PAGE_SHIFT, p1 = (end - 1) >> CC_PAGE_SHIFT;
    for (uint32_t p = p0; p <= p1; p++) {
        if (cc_owned[p]) continue;
        uint32_t a = p == p0 ? start : p << CC_PAGE_SHIFT;
        uint32_t b = p == p1 ? end : (p + 1) << CC_PAGE_SHIFT;
        memcpy(cc_shadow + a, cc_alias + a, b - a);
        cc_tex_bytes += b - a;
    }
}

static uint32_t image_bytes(int size, int width, int rows)
{
    return (uint32_t)((((uint64_t)width << size) >> 1) * (uint64_t)rows);
}

bool cpucopy_init(const uint8_t *alias, uint32_t size, int threads)
{
    cpucopy_deinit();
    if (!alias || !size) return false;
    cc_shadow = (uint8_t *)malloc(size);
    cc_owned = (uint8_t *)calloc((size >> CC_PAGE_SHIFT) + 1, 1);
    if (!cc_shadow || !cc_owned) { cpucopy_deinit(); return false; }
    cc_alias = alias;
    cc_size = size;
    struct foir_config cfg;
    foir_config_defaults(&cfg);
    cfg.relaxed_precision = false;
    cfg.no_lod = false;
    cfg.fast_persp = false;
    cfg.frame_skip = 0;
    cfg.defer_drain = false;
    cfg.neon = true;
    cfg.fill_dedup = false;    /* it skips fills it thinks are repeats: CPU writes between them would be missed */
    rdp_init(&cc_rdp, &cfg);
    rdp_attach_memory(&cc_rdp, cc_shadow, size);
    if (threads > 0) rdp_start_workers(&cc_rdp, threads);
    cc_on = true;
    return true;
}

void cpucopy_deinit(void)
{
    if (cc_on) rdp_destroy(&cc_rdp);
    cc_on = false;
    free(cc_shadow); cc_shadow = NULL;
    free(cc_owned); cc_owned = NULL;
    cc_size = 0;
}

bool cpucopy_active(void)
{
    return cc_on;
}

void cpucopy_store(uint32_t offset, uint32_t bytes, uint64_t value)
{
    if (!cc_on || offset + bytes > cc_size || !cc_owned[offset >> CC_PAGE_SHIFT]) return;
    memcpy(cc_shadow + offset, &value, bytes);   /* little-endian host: the low bytes, as the alias store wrote them */
}

void cpucopy_reset(void)
{
    if (!cc_on) return;
    rdp_sync(&cc_rdp);
    memset(cc_owned, 0, (cc_size >> CC_PAGE_SHIFT) + 1);
}

void cpucopy_release(uint32_t start, uint32_t end)
{
    if (!cc_on) return;
    if (end > cc_size) end = cc_size;
    if (start >= end) return;
    for (uint32_t p = start >> CC_PAGE_SHIFT; p <= (end - 1) >> CC_PAGE_SHIFT; p++)
        if (cc_owned[p]) { cc_owned[p] = 0; cc_released++; }
}

void cpucopy_command(const uint32_t *a)
{
    if (!cc_on) return;
    uint64_t t0 = now_ns();
    struct rdp *r = &cc_rdp;
    unsigned id = (a[0] >> 24) & 0x3f;
    bool draw = id >= 0x08 && id <= 0x0f;             /* triangles */
    draw |= id == 0x24 || id == 0x25 || id == 0x36;    /* texture rectangles, fill rectangle */
    if (draw) {
        /* the colour image's scissored rows, and the depth image's (same rows, 16-bit) */
        int y0 = r->clip.yh >> 2, y1 = (r->clip.yl + 3) >> 2;
        if (y1 > 1024) y1 = 1024;
        if (y1 > y0) {
            uint32_t row = image_bytes(r->fb_size, r->fb_width, 1);
            own_range(r->fb_address + row * (uint32_t)y0, r->fb_address + row * (uint32_t)y1);
            if (r->zb_address) {
                uint32_t zrow = (uint32_t)r->fb_width * 2;
                own_range(r->zb_address + zrow * (uint32_t)y0, r->zb_address + zrow * (uint32_t)y1);
            }
        }
    } else if (id == 0x30 || id == 0x33 || id == 0x34) {
        /* load tlut / block / tile: the source rows from the texture image (conservative: whole rows, one spare) */
        uint32_t tl = (a[0] & 0xfff) >> 2, th = (a[1] & 0xfff) >> 2;
        uint32_t row = image_bytes(r->ti_size, r->ti_width, 1);
        uint32_t start, end;
        if (id == 0x33) {                                    /* block: sh texels from (sl, tl) */
            uint32_t sl = (a[0] >> 12) & 0xfff, sh = (a[1] >> 12) & 0xfff;
            start = r->ti_address + row * tl + image_bytes(r->ti_size, (int)sl, 1);
            end = start + image_bytes(r->ti_size, (int)(sh - sl + 1), 1) + 8;
        } else {
            start = r->ti_address + row * tl;
            end = r->ti_address + row * (th + 2);
        }
        if (end > start) fresh_range(start, end);
    }
    rdp_run_command(r, a);
    cc_cmds++;
    cc_ns += now_ns() - t0;
}

void cpucopy_verify(uint32_t start, uint32_t end)
{
    if (!cc_on) return;
    if (end > cc_size) end = cc_size;
    if (start >= end) return;
    for (uint32_t p = start >> CC_PAGE_SHIFT; p <= (end - 1) >> CC_PAGE_SHIFT; p++) {
        if (!cc_owned[p]) { atomic_fetch_add(&cc_ver_unowned, 1); continue; }
        size_t o = (size_t)p << CC_PAGE_SHIFT;
        const uint64_t *s = (const uint64_t *)(cc_shadow + o), *g = (const uint64_t *)(cc_alias + o);
        uint64_t bad = 0;
        uint32_t first = 0xffffffffu;
        for (uint32_t i = 0; i < CC_PAGE / 8; i++)
            if (s[i] != g[i]) {
                for (int b = 0; b < 8; b++)
                    if (((s[i] ^ g[i]) >> (b * 8)) & 0xff) bad++;
                if (first == 0xffffffffu) first = (uint32_t)o + i * 8;
            }
        atomic_fetch_add(&cc_ver_pages, 1);
        atomic_fetch_add(&cc_ver_bytes, CC_PAGE);
        if (bad) {
            atomic_fetch_add(&cc_ver_bad, bad);
            atomic_fetch_add(&cc_ver_badpages, 1);
            uint32_t want = 0xffffffffu;
            atomic_compare_exchange_strong(&cc_ver_first_bad, &want, first);
        }
    }
}

void cpucopy_report(char *buf, int n)
{
    if (!cc_on) { if (n) buf[0] = 0; return; }
    uint32_t first = atomic_exchange(&cc_ver_first_bad, 0xffffffffu);
    snprintf(buf, (size_t)n,
             "cpu copy: %llu commands, %.2f ms of foir on the emulation thread, %llu pages seeded, %llu KB of texture copied, "
             "%llu pages released; verify: %llu pages compared, %llu bytes differ on %llu pages (first 0x%06x), %llu pages not foir's",
             (unsigned long long)cc_cmds, cc_ns / 1e6, (unsigned long long)cc_seed_pages, (unsigned long long)(cc_tex_bytes >> 10),
             (unsigned long long)cc_released, (unsigned long long)atomic_exchange(&cc_ver_pages, 0),
             (unsigned long long)atomic_exchange(&cc_ver_bad, 0), (unsigned long long)atomic_exchange(&cc_ver_badpages, 0),
             first == 0xffffffffu ? 0 : first, (unsigned long long)atomic_exchange(&cc_ver_unowned, 0));
    atomic_exchange(&cc_ver_bytes, 0);
    cc_cmds = cc_seed_pages = cc_tex_bytes = cc_ns = cc_released = 0;
}
