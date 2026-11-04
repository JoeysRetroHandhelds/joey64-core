/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - memory.c                                                *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "memory.h"

#include "api/callbacks.h"
#include "api/m64p_types.h"

#include "device/device.h"
#include "device/rcp/rsp/rsp_core.h"
#include "device/pif/pif.h"

#ifdef DBG
#include <string.h>

#include "device/r4300/r4300_core.h"

#include "debugger/dbg_breakpoints.h"
#include "debugger/dbg_memory.h"
#endif

#include <assert.h>
#ifdef __ANDROID__
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32) || defined(HAVE_LIBNX)
#include <malloc.h>
#endif


#ifdef DBG
enum
{
    BP_CHECK_READ  = 0x1,
    BP_CHECK_WRITE = 0x2,
};

void read_with_bp_checks(void* opaque, uint32_t address, uint32_t* value)
{
    struct r4300_core* r4300 = (struct r4300_core*)opaque;
    uint16_t region = address >> 16;

    /* only check bp if active */
    if (r4300->mem->bp_checks[region] & BP_CHECK_READ) {
        check_breakpoints_on_mem_access(*r4300_pc(r4300)-0x4, address, 4,
                M64P_BKP_FLAG_ENABLED | M64P_BKP_FLAG_READ);
    }

    mem_read32(&r4300->mem->saved_handlers[region], address, value);
}

void write_with_bp_checks(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct r4300_core* r4300 = (struct r4300_core*)opaque;
    uint16_t region = address >> 16;

    /* only check bp if active */
    if (r4300->mem->bp_checks[region] & BP_CHECK_WRITE) {
        check_breakpoints_on_mem_access(*r4300_pc(r4300)-0x4, address, 4,
                M64P_BKP_FLAG_ENABLED | M64P_BKP_FLAG_WRITE);
    }

    mem_write32(&r4300->mem->saved_handlers[region], address, value, mask);
}

void activate_memory_break_read(struct memory* mem, uint32_t address)
{
    uint16_t region = address >> 16;
    struct mem_handler* dbg_handler = &mem->dbg_handler;
    struct mem_handler* handler = &mem->handlers[region];
    struct mem_handler* saved_handler = &mem->saved_handlers[region];
    unsigned char* bp_check = &mem->bp_checks[region];

    /* if neither read nor write bp is active, set dbg_handler */
    if (!(*bp_check & (BP_CHECK_READ | BP_CHECK_WRITE))) {
        *saved_handler = *handler;
        *handler = *dbg_handler;
    }

    /* activate bp read */
    *bp_check |= BP_CHECK_READ;
}

void deactivate_memory_break_read(struct memory* mem, uint32_t address)
{
    uint16_t region = address >> 16;
    struct mem_handler* handler = &mem->handlers[region];
    struct mem_handler* saved_handler = &mem->saved_handlers[region];
    unsigned char* bp_check = &mem->bp_checks[region];

    /* desactivate bp read */
    *bp_check &= ~BP_CHECK_READ;

    /* if neither read nor write bp is active, restore handler */
    if (!(*bp_check & (BP_CHECK_READ | BP_CHECK_WRITE))) {
        *handler = *saved_handler;
    }
}

void activate_memory_break_write(struct memory* mem, uint32_t address)
{
    uint16_t region = address >> 16;
    struct mem_handler* dbg_handler = &mem->dbg_handler;
    struct mem_handler* handler = &mem->handlers[region];
    struct mem_handler* saved_handler = &mem->saved_handlers[region];
    unsigned char* bp_check = &mem->bp_checks[region];

    /* if neither read nor write bp is active, set dbg_handler */
    if (!(*bp_check & (BP_CHECK_READ | BP_CHECK_WRITE))) {
        *saved_handler = *handler;
        *handler = *dbg_handler;
    }

    /* activate bp write */
    *bp_check |= BP_CHECK_WRITE;
}

void deactivate_memory_break_write(struct memory* mem, uint32_t address)
{
    uint16_t region = address >> 16;
    struct mem_handler* handler = &mem->handlers[region];
    struct mem_handler* saved_handler = &mem->saved_handlers[region];
    unsigned char* bp_check = &mem->bp_checks[region];

    /* desactivate bp write */
    *bp_check &= ~BP_CHECK_WRITE;

    /* if neither read nor write bp is active, restore handler */
    if (!(*bp_check & (BP_CHECK_READ | BP_CHECK_WRITE))) {
        *handler = *saved_handler;
    }
}

int get_memory_type(struct memory* mem, uint32_t address)
{
    return mem->memtype[address >> 16];
}
#else
void read_with_bp_checks(void* opaque, uint32_t address, uint32_t* value)
{
}

void write_with_bp_checks(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
}
#endif

void init_memory(struct memory* mem,
                 struct mem_mapping* mappings, size_t mappings_count,
                 void* base,
                 struct mem_handler* dbg_handler)
{
    size_t m;

#ifdef DBG
    memset(mem->bp_checks, 0, 0x10000*sizeof(mem->bp_checks[0]));
    memcpy(&mem->dbg_handler, dbg_handler, sizeof(*dbg_handler));
#endif

    mem->base = base;

    for(m = 0; m < mappings_count; ++m) {
        apply_mem_mapping(mem, &mappings[m]);
    }
}

static void map_region(struct memory* mem,
                       uint16_t region,
                       int type,
                       const struct mem_handler* handler)
{
#ifdef DBG
    /* set region type */
    mem->memtype[region] = type;

    /* set handler */
    if (lookup_breakpoint(((uint32_t)region << 16), 0x10000,
                          M64P_BKP_FLAG_ENABLED) != -1)
    {
        mem->saved_handlers[region] = *handler;
        mem->handlers[region] = mem->dbg_handler;
    }
    else
#endif
    {
        (void)type;
        mem->handlers[region] = *handler;
    }
}

void apply_mem_mapping(struct memory* mem, const struct mem_mapping* mapping)
{
    size_t i;
    uint16_t begin = mapping->begin >> 16;
    uint16_t end   = mapping->end   >> 16;

    for (i = begin; i <= end; ++i) {
        map_region(mem, i, mapping->type, &mapping->handler);
    }
}

/* For paraLLEl-RDP which needs to import RDRAM as a host pointer with potentially 64k of alignment. */
enum { MB_RDRAM_DRAM_ALIGNMENT_REQUIREMENT = 64 * 1024 };

enum {
    MB_RDRAM_DRAM = 0,
    MB_RSP_MEM  = MB_RDRAM_DRAM + RDRAM_MAX_SIZE,
    MB_DD_ROM   = MB_RSP_MEM    + SP_MEM_SIZE,
    MB_PIF_MEM  = MB_DD_ROM     + DD_ROM_MAX_SIZE,
    MB_MAX_SIZE = MB_PIF_MEM    + PIF_ROM_SIZE + PIF_RAM_SIZE,
    MB_MAX_SIZE_FULL = 0x20000000
};

/* Use LSB of mem_base pointer to encode mem_base mode
 * 1: compressed, 0: full
 */
#define MEM_BASE_MODE(mem_base) ((uintptr_t)(mem_base) & 0x1)
#define MEM_BASE_PTR(mem_base)  ((void*)((uintptr_t)(mem_base) & ~0x1))
#define SET_MEM_BASE_MODE(mem_base) (mem_base = (void*)((uintptr_t)(mem_base) | 0x1))

static void*    mem_rom = NULL;
static uint32_t mem_rom_size = 0;

void* g_rdram_alias = NULL;

#ifdef __ANDROID__
/* Full mem base from mmap, with RDRAM backed by a shared memory file so it can also be mapped a second time
 * (g_rdram_alias). paraLLEl-RDP's smart sync protects pages of the first view while its own copy-back
 * thread writes through the second. Returns NULL to fall back to the heap. */
static void* mmap_region;
static size_t mmap_region_size;

static void* init_mem_base_shared(void)
{
    int fd = (int)syscall(__NR_memfd_create, "n64-rdram", 0);
    if (fd < 0)
        return NULL;
    if (ftruncate(fd, RDRAM_MAX_SIZE) != 0) {
        close(fd);
        return NULL;
    }
    size_t size = MB_MAX_SIZE_FULL + MB_RDRAM_DRAM_ALIGNMENT_REQUIREMENT;
    uint8_t* region = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (region == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    uint8_t* base = (uint8_t*)(((uintptr_t)region + MB_RDRAM_DRAM_ALIGNMENT_REQUIREMENT - 1)
        & ~(uintptr_t)(MB_RDRAM_DRAM_ALIGNMENT_REQUIREMENT - 1));
    void* alias = mmap(NULL, RDRAM_MAX_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mmap(base + MB_RDRAM_DRAM, RDRAM_MAX_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED
        || alias == MAP_FAILED) {
        if (alias != MAP_FAILED)
            munmap(alias, RDRAM_MAX_SIZE);
        munmap(region, size);
        close(fd);
        return NULL;
    }
    close(fd);
    mmap_region = region;
    mmap_region_size = size;
    g_rdram_alias = alias;
    return base;
}
#endif

void* init_mem_base(void)
{
    void* mem_base;

#ifdef __ANDROID__
    if ((mem_base = init_mem_base_shared()) != NULL) {
        DebugMessage(M64MSG_INFO, "Using full mem base, RDRAM in shared memory");
        return mem_base;
    }
#endif

    /* First try the full mem base alloc */
#ifdef _WIN32
    mem_base = _aligned_malloc(MB_MAX_SIZE_FULL, MB_RDRAM_DRAM_ALIGNMENT_REQUIREMENT);
#else
#ifdef HAVE_LIBNX
    if (!(mem_base = memalign(MB_RDRAM_DRAM_ALIGNMENT_REQUIREMENT, MB_MAX_SIZE_FULL)))
        mem_base = NULL;
#else
    if (posix_memalign(&mem_base, MB_RDRAM_DRAM_ALIGNMENT_REQUIREMENT, MB_MAX_SIZE_FULL) != 0)
        mem_base = NULL;
#endif // HAVE_LIBNX
#endif
    if (mem_base == NULL) {
        /* if it failed, try the compressed mem base alloc */
        mem_base = malloc(MB_MAX_SIZE);
        if (mem_base != NULL) {
            /* Compressed mem base mode has LSB = 1 */
            assert(MEM_BASE_MODE(mem_base) == 0);
            SET_MEM_BASE_MODE(mem_base);
            DebugMessage(M64MSG_INFO, "Using compressed mem base");
        }
    }
    else {
        /* Full mem base mode has LSB = 0 */
        assert(MEM_BASE_MODE(mem_base) == 0);
        DebugMessage(M64MSG_INFO, "Using full mem base");
    }

    return mem_base;
}

void release_mem_base(void* mem_base)
{
#ifdef __ANDROID__
    if (mmap_region != NULL && MEM_BASE_PTR(mem_base) != NULL
        && (uint8_t*)mem_base >= (uint8_t*)mmap_region && (uint8_t*)mem_base < (uint8_t*)mmap_region + mmap_region_size) {
        munmap(g_rdram_alias, RDRAM_MAX_SIZE);
        munmap(mmap_region, mmap_region_size);
        g_rdram_alias = NULL;
        mmap_region = NULL;
        return;
    }
#endif
#ifdef _WIN32
    if (MEM_BASE_MODE(mem_base) == 0)
        _aligned_free(MEM_BASE_PTR(mem_base));
    else
#endif
        free(MEM_BASE_PTR(mem_base));
}

void* init_mem_rom(uint32_t size)
{
    if (size > mem_rom_size) {
        mem_rom = realloc(mem_rom, size);
        if (mem_rom == NULL)
            mem_rom_size = 0;
        else
            mem_rom_size = size;
    }

    return mem_rom;
}

void release_mem_rom(void)
{
    if (mem_rom != NULL) {
        free(mem_rom);
        mem_rom = NULL;
    }

    mem_rom_size = 0;
}

uint32_t* mem_base_u32(void* mem_base, uint32_t address)
{
    uint32_t* mem;

    if (MEM_BASE_MODE(mem_base) == 0) {
        /* In full mem base mode, use simple pointer arithmetic
         * except for the rom, which is dynamically allocated 
         */
        if (address >= MM_CART_ROM && (address & UINT32_C(0xfff00000)) != MM_PIF_MEM) {
            mem = (uint32_t*)((uint8_t*)mem_rom + (address - MM_CART_ROM));
        } else {
            mem = (uint32_t*)((uint8_t*)mem_base + address);
        }
    }
    else {
        /* In compressed mem base mode, select appropriate mem_base offset */
        mem_base = MEM_BASE_PTR(mem_base);

        if (address < RDRAM_MAX_SIZE) {
            mem = (uint32_t*)((uint8_t*)mem_base + (address - MM_RDRAM_DRAM + MB_RDRAM_DRAM));
        }
        else if (address >= MM_CART_ROM) {
            if ((address & UINT32_C(0xfff00000)) == MM_PIF_MEM) {
                mem = (uint32_t*)((uint8_t*)mem_base + (address - MM_PIF_MEM + MB_PIF_MEM));
            } else {
                mem = (uint32_t*)((uint8_t*)mem_rom + (address - MM_CART_ROM));
            }
        }
        else if ((address & UINT32_C(0xfe000000)) == MM_DD_ROM) {
            mem = (uint32_t*)((uint8_t*)mem_base + (address - MM_DD_ROM + MB_DD_ROM));
        }
        else if ((address & UINT32_C(0xffffe000)) == MM_RSP_MEM) {
            mem = (uint32_t*)((uint8_t*)mem_base + (address - MM_RSP_MEM + MB_RSP_MEM));
        }
        else {
            mem = NULL;
        }
    }

    return mem;
}
