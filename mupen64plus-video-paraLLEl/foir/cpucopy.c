/* Joey64: see cpucopy.h. Built with foir (its flags and rename header). */
#include "cpucopy.h"
#include "rdp.h"
#include "config.h"
#include "mt.h"
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
static uint64_t *cc_drawn_at;        /* per page: the GPU timeline value before foir's last draw there (the draw is
                                        covered by the next value) */
static uint8_t *cc_suspect;          /* per page: released while foir had drawn past what the GPU had reached */
static uint64_t cc_epoch;
static const char *cc_dump_dir;
static int cc_dumped;
static uint8_t cc_badpage[2 * 4096];    /* the first differing page: foir's, then real RDRAM's */
static uint64_t cc_badpage_info[5];
static volatile int cc_badpage_held;
static bool cc_verify_strict;
static uint8_t *cc_kind;             /* per page: what foir last drew there: 1 colour 16-bit, 2 colour 32-bit, 3 colour 8-bit, 4 depth */
/* diagnosis: bad pages by kind; 16-bit pixels that differ, and of them those only a step apart in each 5-bit channel */
static _Atomic uint64_t cc_bad_kind[5], cc_px_bad, cc_px_small, cc_z_bad;
static _Atomic uint64_t cc_sample_s, cc_sample_g;
static _Atomic uint32_t cc_sample_kind;            /* the timeline value before the command being run */

/* counters (verify runs in the fault handler: atomics, no locks) */
static _Atomic uint64_t cc_ver_pages, cc_ver_bytes, cc_ver_bad, cc_ver_badpages, cc_ver_unowned, cc_ver_newer,
    cc_ver_suspect_bad;
static _Atomic uint32_t cc_ver_first_bad = 0xffffffffu;
static uint64_t cc_cmds, cc_seed_pages, cc_tex_bytes, cc_ns, cc_released;
static uint64_t cc_answered, cc_answer_ns;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Make [start, end) current in the shadow for foir to draw into: pages not owned are copied in and become owned. */
static void own_range(uint32_t start, uint32_t end, uint8_t kind)
{
    if (end > cc_size) end = cc_size;
    if (start >= end) return;
    for (uint32_t p = start >> CC_PAGE_SHIFT; p <= (end - 1) >> CC_PAGE_SHIFT; p++) {
        if (!cc_owned[p]) {
            memcpy(cc_shadow + ((size_t)p << CC_PAGE_SHIFT), cc_alias + ((size_t)p << CC_PAGE_SHIFT), CC_PAGE);
            cc_owned[p] = 1;
            cc_seed_pages++;
        }
        cc_drawn_at[p] = cc_epoch;
        cc_kind[p] = kind;
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
    cc_drawn_at = (uint64_t *)calloc((size >> CC_PAGE_SHIFT) + 1, sizeof(uint64_t));
    cc_suspect = (uint8_t *)calloc((size >> CC_PAGE_SHIFT) + 1, 1);
    cc_kind = (uint8_t *)calloc((size >> CC_PAGE_SHIFT) + 1, 1);
    if (!cc_shadow || !cc_owned || !cc_drawn_at || !cc_suspect || !cc_kind) { cpucopy_deinit(); return false; }
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
    const char *sc = getenv("N64_RDP_CPUCOPY_SCALAR");   /* diagnosis: foir's reference kernels, nothing specialised */
    if (sc && sc[0] == '1') {
        cfg.neon = false;
        cfg.specialize = false;
        cfg.fast_combiner = false;
        cfg.early_z = false;
    }
    rdp_init(&cc_rdp, &cfg);
    rdp_attach_memory(&cc_rdp, cc_shadow, size);
    rdp_fb_history_reset(&cc_rdp);   /* as foir's own plugin does after attaching memory */
    if (threads > 0) rdp_start_workers(&cc_rdp, threads);
    cc_dump_dir = getenv("N64_RDP_CPUCOPY_DUMP");
    const char *vs = getenv("N64_RDP_CPUCOPY_VERIFY");   /* 2: compare only when the GPU has done all foir has */
    cc_verify_strict = vs && vs[0] == '2';
    cc_dumped = 0;
    cc_on = true;
    return true;
}

void cpucopy_deinit(void)
{
    if (cc_on) rdp_destroy(&cc_rdp);
    cc_on = false;
    free(cc_shadow); cc_shadow = NULL;
    free(cc_owned); cc_owned = NULL;
    free(cc_drawn_at); cc_drawn_at = NULL;
    free(cc_suspect); cc_suspect = NULL;
    free(cc_kind); cc_kind = NULL;
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
    memset(cc_suspect, 0, (cc_size >> CC_PAGE_SHIFT) + 1);
}

/* Stage 2: foir's 4 KB page at `page` into dst + page (real RDRAM through the alias), once foir's pending jobs that write
 * it are done. False when foir doesn't own the page (its bytes there may be stale): the caller waits for the GPU. */
bool cpucopy_answer(uint32_t page, uint8_t *dst)
{
    if (!cc_on || page + CC_PAGE > cc_size || !cc_owned[page >> CC_PAGE_SHIFT]) return false;
    uint64_t t0 = now_ns();
    mt_sync_range(&cc_rdp, page, page + CC_PAGE);
    memcpy(dst + page, cc_shadow + page, CC_PAGE);
    cc_answered++;
    cc_answer_ns += now_ns() - t0;
    return true;
}

void cpucopy_release(uint32_t start, uint32_t end)
{
    if (!cc_on) return;
    if (end > cc_size) end = cc_size;
    if (start >= end) return;
    for (uint32_t p = start >> CC_PAGE_SHIFT; p <= (end - 1) >> CC_PAGE_SHIFT; p++)
        if (cc_owned[p]) { cc_owned[p] = 0; cc_released++; }
}

void cpucopy_command(const uint32_t *a, uint64_t timeline)
{
    if (!cc_on) return;
    unsigned id = (a[0] >> 24) & 0x3f;
    /* SYNC_FULL: foir's would wait for its workers to finish the frame (mt_sync_frame) on the emulation thread, and
       run frame bookkeeping (skip, callbacks) this copy doesn't use. Whoever reads foir's copy waits for it instead. */
    if (id == 0x29) return;
    uint64_t t0 = now_ns();
    cc_epoch = timeline;
    struct rdp *r = &cc_rdp;
    bool draw = id >= 0x08 && id <= 0x0f;             /* triangles */
    draw |= id == 0x24 || id == 0x25 || id == 0x36;    /* texture rectangles, fill rectangle */
    if (draw) {
        /* the rows the primitive spans (from its y values; as parallel-RDP's smart sync tracks them) within the scissor,
           in the colour image, and in the depth image when it writes depth. Whole scissor rows (as before) took pages
           the primitive never touched, which the CPU could then change without a fault (Snap). */
        int py0, py1;
        if (id <= 0x0f) {                                 /* triangle: yh (top) .. yl (bottom), s11.2 */
            int yl = (int)(a[0] << 18) >> 18, ym = (int)(a[1] << 2) >> 18, yh = (int)(a[1] << 18) >> 18;
            int lo = yh < ym ? yh : ym, hi = yl > ym ? yl : ym;
            if (lo > hi) { int t = lo; lo = hi; hi = t; }
            py0 = lo >> 2; py1 = (hi >> 2) + 2;
        } else {                                          /* rectangles: yh (top, w1) .. yl (bottom, w0), 10.2 */
            py0 = (int)(a[1] & 0xfff) >> 2; py1 = ((int)(a[0] & 0xfff) >> 2) + 2;
        }
        int y0 = r->clip.yh >> 2, y1 = (r->clip.yl + 3) >> 2;
        if (py0 > y0) y0 = py0;
        if (py1 < y1) y1 = py1;
        if (y0 < 0) y0 = 0;
        if (y1 > 1024) y1 = 1024;
        if (y1 > y0) {
            uint32_t row = image_bytes(r->fb_size, r->fb_width, 1);
            own_range(r->fb_address + row * (uint32_t)y0, r->fb_address + row * (uint32_t)y1,
                      r->fb_size == 2 ? 1 : r->fb_size == 3 ? 2 : 3);
            bool zwrite = id <= 0x0f && (id & 1) && r->om.z_update_en &&
                          r->om.cycle_type != CYCLE_TYPE_FILL && r->om.cycle_type != CYCLE_TYPE_COPY;
            if (zwrite && r->zb_address) {
                uint32_t zrow = (uint32_t)r->fb_width * 2;
                own_range(r->zb_address + zrow * (uint32_t)y0, r->zb_address + zrow * (uint32_t)y1, 4);
            }
        }
    } else if (id == 0x30 || id == 0x33 || id == 0x34) {
        /* load tlut / block / tile: the bytes foir's tmem_load_* read (the ranges its workers sync on), one spare qword.
           A palette's texture image is often 1 texel wide, so whole rows (as before) missed most of a TLUT. */
        uint32_t sl = (a[0] >> 12) & 0xfff, tl = a[0] & 0xfff, sh = (a[1] >> 12) & 0xfff, th = a[1] & 0xfff;
        uint32_t w = (uint32_t)r->ti_width, bpp = (1u << r->ti_size) >> 1;   /* 0 for 4-bit, which foir doesn't load */
        uint32_t start = 0, end = 0;
        if (id == 0x30) {                                    /* tlut: entries s0..s1 of row t, 16-bit */
            uint32_t s0 = sl >> 2, s1 = sh >> 2, t0 = tl >> 2;
            if (s1 >= s0) { start = r->ti_address + (t0 * w + s0) * 2; end = start + (s1 - s0 + 1) * 2; }
        } else if (id == 0x33) {                             /* block: sh - sl + 1 texels from (sl, tl), at most TMEM */
            if (bpp && sh >= sl) {
                uint32_t nq = ((sh - sl + 1) * bpp + 7) >> 3;
                if (nq > 512) nq = 512;
                start = r->ti_address + ((tl & 0x3ff) * w + sl) * bpp;
                end = start + nq * 8;
            }
        } else {                                             /* tile: (s0, t0) to (s1, t1) */
            uint32_t s0 = sl >> 2, s1 = sh >> 2, t0 = tl >> 2, t1 = th >> 2;
            if (bpp && s1 >= s0 && t1 >= t0) {
                start = r->ti_address + (t0 * w + s0) * bpp;
                end = r->ti_address + (t1 * w + s1 + 1) * bpp;
            }
        }
        if (end > start) fresh_range(start & ~7u, end + 8);
    }
    rdp_run_command(r, a);
    cc_cmds++;
    cc_ns += now_ns() - t0;
}

void cpucopy_verify(uint32_t start, uint32_t end, uint64_t reached)
{
    if (!cc_on) return;
    if (end > cc_size) end = cc_size;
    if (start >= end) return;
    /* foir's workers may still be drawing what was queued: wait for them. Called from smart sync's fault handler on the
       emulation thread, which is never inside foir when it faults (foir only writes its own shadow), so no foir lock is held. */
    rdp_sync(&cc_rdp);
    for (uint32_t p = start >> CC_PAGE_SHIFT; p <= (end - 1) >> CC_PAGE_SHIFT; p++) {
        if (!cc_owned[p]) { atomic_fetch_add(&cc_ver_unowned, 1); continue; }
        if (cc_drawn_at[p] >= reached || (cc_verify_strict && reached <= cc_epoch)) {
            /* foir has drawn there past what the GPU has reached: nothing to compare, and the reseed on foir's next
               draw copies a page the GPU may still change */
            atomic_fetch_add(&cc_ver_newer, 1);
            cc_suspect[p] = 1;
            continue;
        }
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
        if (bad && cc_suspect[p]) {
            atomic_fetch_add(&cc_ver_suspect_bad, 1);
        } else if (bad) {
            atomic_fetch_add(&cc_ver_bad, bad);
            atomic_fetch_add(&cc_ver_badpages, 1);
            uint32_t want = 0xffffffffu;
            if (atomic_compare_exchange_strong(&cc_ver_first_bad, &want, first)) {
                atomic_store(&cc_sample_s, s[(first - (uint32_t)o) / 8]);
                atomic_store(&cc_sample_g, g[(first - (uint32_t)o) / 8]);
                atomic_store(&cc_sample_kind, cc_kind[p]);
                if (cc_dumped == 1) {   /* the latest differing page before the dump (past the start), both copies */
                    memcpy(cc_badpage, s, CC_PAGE);
                    memcpy(cc_badpage + CC_PAGE, g, CC_PAGE);
                    cc_badpage_info[0] = (uint64_t)o; cc_badpage_info[1] = cc_drawn_at[p];
                    cc_badpage_info[2] = reached; cc_badpage_info[3] = cc_epoch; cc_badpage_info[4] = cc_kind[p];
                    cc_badpage_held = 1;
                }
            }
            uint8_t k = cc_kind[p] < 5 ? cc_kind[p] : 0;
            atomic_fetch_add(&cc_bad_kind[k], 1);
            const uint16_t *s16 = (const uint16_t *)s, *g16 = (const uint16_t *)g;
            uint64_t pb = 0, ps = 0;
            for (uint32_t i = 0; i < CC_PAGE / 2; i++) {
                if (s16[i] == g16[i]) continue;
                pb++;
                int dr = (s16[i] >> 11) - (g16[i] >> 11), dg = ((s16[i] >> 6) & 31) - ((g16[i] >> 6) & 31);
                int db = ((s16[i] >> 1) & 31) - ((g16[i] >> 1) & 31);
                if (dr >= -1 && dr <= 1 && dg >= -1 && dg <= 1 && db >= -1 && db <= 1) ps++;
            }
            if (k == 4) atomic_fetch_add(&cc_z_bad, pb);
            else if (k == 1) { atomic_fetch_add(&cc_px_bad, pb); atomic_fetch_add(&cc_px_small, ps); }
        }
    }
}

static void dump_file(const char *name, const void *p, size_t n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", cc_dump_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(p, 1, n, f);
    fclose(f);
}

/* N64_RDP_CPUCOPY_DUMP=<dir>: at the second report, foir's copy, real RDRAM and foir's image registers, for a look on the PC */
static void maybe_dump(void)
{
    if (!cc_dump_dir || ++cc_dumped != 2) return;
    rdp_sync(&cc_rdp);
    dump_file("cc-shadow.bin", cc_shadow, cc_size);
    dump_file("cc-alias.bin", cc_alias, cc_size);
    dump_file("cc-owned.bin", cc_owned, cc_size >> CC_PAGE_SHIFT);
    char t[256];
    int len = snprintf(t, sizeof t, "fb %06x width %d size %d format %d zb %06x pixels %u\n", cc_rdp.fb_address & 0xffffff,
                       cc_rdp.fb_width, cc_rdp.fb_size, cc_rdp.fb_format, cc_rdp.zb_address & 0xffffff, rdp_pixels(&cc_rdp));
    dump_file("cc-info.txt", t, (size_t)len);
    if (cc_badpage_held) {
        dump_file("cc-badpage.bin", cc_badpage, sizeof cc_badpage);
        len = snprintf(t, sizeof t, "page %06llx drawn_at %llu reached %llu epoch %llu kind %llu\n",
                       (unsigned long long)cc_badpage_info[0], (unsigned long long)cc_badpage_info[1],
                       (unsigned long long)cc_badpage_info[2], (unsigned long long)cc_badpage_info[3],
                       (unsigned long long)cc_badpage_info[4]);
        dump_file("cc-badpage.txt", t, (size_t)len);
    }
}

void cpucopy_report(char *buf, int n)
{
    if (!cc_on) { if (n) buf[0] = 0; return; }
    maybe_dump();
    uint32_t first = atomic_exchange(&cc_ver_first_bad, 0xffffffffu);
    snprintf(buf, (size_t)n,
             "cpu copy: %llu commands, %.2f ms of foir on the emulation thread (foir %s), %llu pages seeded, %llu KB of texture copied, "
             "%llu pages released, %llu reads answered (%.2f ms); verify: %llu pages compared, %llu bytes differ on %llu pages (first 0x%06x), %llu pages not foir's, "
             "%llu drawn past the GPU, %llu differ after such a release; bad pages c16/c32/c8/z/? %llu/%llu/%llu/%llu/%llu, "
             "16-bit pixels differ %llu (%llu a step apart), depth words differ %llu; first: foir %016llx gpu %016llx (kind %u)",
             (unsigned long long)cc_cmds, cc_ns / 1e6, mt_stats_delta(&cc_rdp), (unsigned long long)cc_seed_pages, (unsigned long long)(cc_tex_bytes >> 10),
             (unsigned long long)cc_released, (unsigned long long)cc_answered, cc_answer_ns / 1e6,
             (unsigned long long)atomic_exchange(&cc_ver_pages, 0),
             (unsigned long long)atomic_exchange(&cc_ver_bad, 0), (unsigned long long)atomic_exchange(&cc_ver_badpages, 0),
             first == 0xffffffffu ? 0 : first, (unsigned long long)atomic_exchange(&cc_ver_unowned, 0),
             (unsigned long long)atomic_exchange(&cc_ver_newer, 0), (unsigned long long)atomic_exchange(&cc_ver_suspect_bad, 0),
             (unsigned long long)atomic_exchange(&cc_bad_kind[1], 0), (unsigned long long)atomic_exchange(&cc_bad_kind[2], 0),
             (unsigned long long)atomic_exchange(&cc_bad_kind[3], 0), (unsigned long long)atomic_exchange(&cc_bad_kind[4], 0),
             (unsigned long long)atomic_exchange(&cc_bad_kind[0], 0), (unsigned long long)atomic_exchange(&cc_px_bad, 0),
             (unsigned long long)atomic_exchange(&cc_px_small, 0), (unsigned long long)atomic_exchange(&cc_z_bad, 0),
             (unsigned long long)atomic_load(&cc_sample_s), (unsigned long long)atomic_load(&cc_sample_g),
             (unsigned)atomic_load(&cc_sample_kind));
    atomic_exchange(&cc_ver_bytes, 0);
    cc_cmds = cc_seed_pages = cc_tex_bytes = cc_ns = cc_released = cc_answered = cc_answer_ns = 0;
}
