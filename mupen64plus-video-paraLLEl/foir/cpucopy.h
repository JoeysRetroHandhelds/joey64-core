/* Joey64: N64_RDP_CPUCOPY, a CPU copy of the RDP's drawing (foir) beside parallel-RDP (joey64-notes
 * docs/foir-readback-study.md). foir draws every command into a private shadow of RDRAM. Pages foir draws into
 * are its own ("owned") from then on; a page smart sync gives back to the CPU (unlocked, or a store done through
 * the alias) is no longer owned, and is copied in again from real RDRAM before foir next uses it. Texture load
 * sources not owned are copied in from real RDRAM at each load.
 *
 * Stage 1 (this file): compare only. Every read-back still waits for the GPU; then cpucopy_verify compares
 * foir's owned bytes of that page with what the GPU wrote. Nothing the game sees changes. */
#ifndef JOEY64_CPUCOPY_H
#define JOEY64_CPUCOPY_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* alias: a never-protected view of RDRAM (g_rdram_alias). threads: foir workers (0 = none). */
__attribute__((visibility("default"))) bool cpucopy_init(const uint8_t *alias, uint32_t size, int threads);
__attribute__((visibility("default"))) void cpucopy_deinit(void);
/* One RDP command (its words, as given to parallel-RDP), in order; timeline: parallel-RDP's last signalled timeline value. */
__attribute__((visibility("default"))) void cpucopy_command(const uint32_t *words, uint64_t timeline);
/* The CPU may change [start, end) from now on (smart sync unlocked it, or a store went through the alias). */
__attribute__((visibility("default"))) void cpucopy_release(uint32_t start, uint32_t end);
/* A CPU store done through the alias while the page stays locked (smart sync's emulate path): the same bytes go
 * into the shadow when foir owns the page. Async-signal-safe. */
__attribute__((visibility("default"))) void cpucopy_store(uint32_t offset, uint32_t bytes, uint64_t value);
/* On (cpucopy_init succeeded). */
__attribute__((visibility("default"))) bool cpucopy_active(void);
/* Forget everything (a state load, a reset, an upscale change): every page is copied in again when next used. */
__attribute__((visibility("default"))) void cpucopy_reset(void);
/* After the GPU reached timeline value reached: compare foir's owned bytes in [start, end) with real RDRAM.
 * Async-signal-safe (counters only). */
__attribute__((visibility("default"))) void cpucopy_verify(uint32_t start, uint32_t end, uint64_t reached);
/* A log line's worth of counters since the last call (writes up to n bytes into buf). */
__attribute__((visibility("default"))) void cpucopy_report(char *buf, int n);

#ifdef __cplusplus
}
#endif
#endif
