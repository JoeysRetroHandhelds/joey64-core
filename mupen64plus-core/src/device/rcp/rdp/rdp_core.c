/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - rdp_core.c                                              *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2014 Bobby Smiles                                       *
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

#include "rdp_core.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "api/callbacks.h"
#include "api/m64p_types.h"
#include "device/memory/memory.h"
#include "device/r4300/r4300_core.h"
#include "device/r4300/interrupt.h"
#include "device/rcp/mi/mi_controller.h"
#include "device/rcp/rsp/rsp_core.h"
#include "plugin/plugin.h"

static void update_dpc_status(struct rdp_core* dp, uint32_t w)
{
    /* clear / set xbus_dmem_dma */
    if (w & DPC_CLR_XBUS_DMEM_DMA) dp->dpc_regs[DPC_STATUS_REG] &= ~DPC_STATUS_XBUS_DMEM_DMA;
    if (w & DPC_SET_XBUS_DMEM_DMA) dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_XBUS_DMEM_DMA;

    /* clear / set freeze */
    if (w & DPC_CLR_FREEZE)
    {
        dp->dpc_regs[DPC_STATUS_REG] &= ~DPC_STATUS_FREEZE;

        if (dp->do_on_unfreeze & DELAY_DP_INT)
            signal_rcp_interrupt(dp->mi, MI_INTR_DP);
        if (dp->do_on_unfreeze & DELAY_UPDATESCREEN)
            gfx.updateScreen();
        dp->do_on_unfreeze = 0;
    }
    if (w & DPC_SET_FREEZE) dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_FREEZE;

    /* clear / set flush */
    if (w & DPC_CLR_FLUSH) dp->dpc_regs[DPC_STATUS_REG] &= ~DPC_STATUS_FLUSH;
    if (w & DPC_SET_FLUSH) dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_FLUSH;

    /* clear clock counter */
    if (w & DPC_CLR_CLOCK_CTR) dp->dpc_regs[DPC_CLOCK_REG] = 0;
}


void init_rdp(struct rdp_core* dp,
              struct rsp_core* sp,
              struct mi_controller* mi,
              struct memory* mem,
              struct rdram* rdram,
              struct r4300_core* r4300)
{
    dp->sp = sp;
    dp->mi = mi;

    init_fb(&dp->fb, mem, rdram, r4300);
}

void poweron_rdp(struct rdp_core* dp)
{
    memset(dp->dpc_regs, 0, DPC_REGS_COUNT*sizeof(uint32_t));
    memset(dp->dps_regs, 0, DPS_REGS_COUNT*sizeof(uint32_t));
    dp->dpc_regs[DPC_STATUS_REG] |= DPC_STATUS_START_GCLK;

    dp->do_on_unfreeze = 0;
    dp->pending = 0;

    poweron_fb(&dp->fb);
}


void read_dpc_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;
    uint32_t reg = dpc_reg(address);

    *value = dp->dpc_regs[reg];
}

void write_dpc_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;
    uint32_t reg = dpc_reg(address);

    switch(reg)
    {
    case DPC_STATUS_REG:
        update_dpc_status(dp, value & mask);
    case DPC_CURRENT_REG:
    case DPC_CLOCK_REG:
    case DPC_BUFBUSY_REG:
    case DPC_PIPEBUSY_REG:
    case DPC_TMEM_REG:
        return;
    }

    masked_write(&dp->dpc_regs[reg], value, mask);

    switch(reg)
    {
    case DPC_START_REG:
        dp->dpc_regs[DPC_CURRENT_REG] = dp->dpc_regs[DPC_START_REG];
        break;
    case DPC_END_REG:
        unprotect_framebuffers(&dp->fb);
        gfx.processRDPList();
        protect_framebuffers(&dp->fb);
        signal_rcp_interrupt(dp->mi, MI_INTR_DP);
        break;
    }
}


void read_dps_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;
    uint32_t reg = dps_reg(address);

    if (reg < DPS_REGS_COUNT)
    {
        *value = dp->dps_regs[reg];
    }
}

void write_dps_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;
    uint32_t reg = dps_reg(address);

    if (reg < DPS_REGS_COUNT)
    {
        masked_write(&dp->dps_regs[reg], value, mask);
    }
}

void rdp_interrupt_event(void* opaque)
{
    struct rdp_core* dp = (struct rdp_core*)opaque;

    raise_rcp_interrupt(dp->mi, MI_INTR_DP);
    if (dp->pending != 0) {
        uint32_t d = dp->pending_delay[0];
        memmove(dp->pending_delay, dp->pending_delay + 1, --dp->pending * sizeof(dp->pending_delay[0]));
        cp0_update_count(dp->mi->r4300);
        add_interrupt_event(&dp->mi->r4300->cp0, DP_INT, d);
    }
}

/* ---- N64_RDP_ADAPT (dev switch, off by default), ported from mupen64plus-core-dento (beebono, GPL-2.0-or-later):
 * main.c adapt_full_sync_delay + rdp_core.c rdp_full_sync_done. N64_RDP_ADAPT=1 moves the delay between 4000 (upstream's)
 * and 900000 cycles; N64_RDP_ADAPT=<lo>:<hi> sets the range. ---- */
static int adapt_state = -1;              /* -1 not read yet, 0 off, 1 on */
static uint32_t adapt_lo = 4000, adapt_hi = 900000, adapt_delay = 4000;
static unsigned int adapt_n, adapt_slack, adapt_behind, adapt_log_vis, adapt_log_behind;
static uint32_t adapt_log_max;

int rdp_adapt_enabled(void)
{
    if (adapt_state < 0) {
        const char* e = getenv("N64_RDP_ADAPT");
        adapt_state = e && e[0] && e[0] != '0';
        if (adapt_state) {
            unsigned int lo, hi;
            if (strchr(e, ':') && sscanf(e, "%u:%u", &lo, &hi) == 2 && hi > lo) { adapt_lo = lo; adapt_hi = hi; }
            adapt_delay = adapt_lo;
            DebugMessage(M64MSG_ERROR, "rdp adapt: on, the DP delay moves between %u and %u cycles", adapt_lo, adapt_hi);
        }
    }
    return adapt_state;
}

/* dento: while behind real time (more than 2 ms) the delay rises a 75th of the range a VI towards the maximum; it falls back
 * by a 15th of it after 60 VIs with none behind and at least 54 with slack. */
void rdp_adapt_vi(double sleep_ms)
{
    uint32_t up, down;
    if (!rdp_adapt_enabled())
        return;
    up = (adapt_hi - adapt_lo) / 75 ? (adapt_hi - adapt_lo) / 75 : 1;
    down = (adapt_hi - adapt_lo) / 15 ? (adapt_hi - adapt_lo) / 15 : 1;
    if (sleep_ms < -2.0) {
        ++adapt_behind; ++adapt_log_behind;
        adapt_delay = (adapt_delay + up < adapt_hi) ? adapt_delay + up : adapt_hi;
    } else if (sleep_ms > 0.0) {
        ++adapt_slack;
    }
    if (++adapt_n >= 60) {
        if (adapt_behind == 0 && adapt_slack >= 54)
            adapt_delay = (adapt_delay > adapt_lo + down) ? adapt_delay - down : adapt_lo;
        adapt_n = adapt_slack = adapt_behind = 0;
    }
    if (adapt_delay > adapt_log_max) adapt_log_max = adapt_delay;
    if (++adapt_log_vis >= 600) {
        DebugMessage(M64MSG_ERROR, "rdp adapt: delay %u cycles now, highest %u, %u of the last 600 VIs behind",
                     adapt_delay, adapt_log_max, adapt_log_behind);
        adapt_log_vis = adapt_log_behind = 0; adapt_log_max = adapt_delay;
    }
}

void rdp_full_sync_done(struct rdp_core* dp)
{
    struct cp0* cp0 = &dp->mi->r4300->cp0;
    if (!rdp_adapt_enabled()) {
        cp0_update_count(dp->mi->r4300);
        add_interrupt_event(cp0, DP_INT, 4000);
        return;
    }
    if (get_event(&cp0->q, DP_INT)) {
        if (dp->pending < sizeof(dp->pending_delay) / sizeof(dp->pending_delay[0]))
            dp->pending_delay[dp->pending++] = adapt_delay;
        else
            dp->pending_delay[dp->pending - 1] += adapt_delay;
        return;
    }
    cp0_update_count(dp->mi->r4300);
    add_interrupt_event(cp0, DP_INT, adapt_delay);
}

