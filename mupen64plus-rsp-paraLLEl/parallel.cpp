#ifdef DEBUG_JIT
#include "debug_rsp.hpp"
#else
#include "rsp_jit.hpp"
#endif
#include <stdint.h>
#include <stdlib.h>

#include "m64p_plugin.h"
#include "rsp_1.1.h"

#define RSP_PARALLEL_VERSION 0x0101
#define RSP_PLUGIN_API_VERSION 0x020000

namespace RSP
{
RSP_INFO rsp;
#ifdef DEBUG_JIT
RSP::CPU cpu;
#else
RSP::JIT::CPU cpu;
#endif
short MFC0_count[32];
int SP_STATUS_TIMEOUT;
unsigned DMA_poll_count; // the same DMA bringing nothing new, in a row (rsp/cp0.cpp)
bool yield_halt; // HALT was set by a poll yield (rsp/cp0.cpp), not by the task: see parallelRSPDoRspCycles
} // namespace RSP

extern "C"
{
	// HLE audio with LLE graphics (on by default; dev env N64_HLE_AUDIO=0 turns it off): audio tasks with a ucode the HLE plugin
	// knows run there, everything else on this RSP.
	void hleInitiateRSP(RSP_INFO Rsp_Info, unsigned int *CycleCount);
	int hleTryAudioTask(void);
	static bool hle_audio;

	// Joey64 (run-ahead): what this RSP keeps from one call to the next, outside the core's state (its registers,
	// vector unit and delay slot; the PC goes through SP_PC_REG). Kept with a run-ahead state so one can be taken
	// while a task is still running, and so the extra frames' register changes don't stay after the load.
	struct parallel_rsp_saved
	{
		uint32_t sr[33];
		RSP::CP2 cp2;
		uint32_t pc, has_delay_slot, branch_target;
	};
	size_t parallel_rsp_state_size(void)
	{
		return sizeof(parallel_rsp_saved);
	}
	void parallel_rsp_state_save(void *out)
	{
		auto &s = RSP::cpu.get_state();
		auto *o = static_cast<parallel_rsp_saved *>(out);
		memcpy(o->sr, s.sr, sizeof(o->sr));
		o->cp2 = s.cp2;
		o->pc = s.pc;
		o->has_delay_slot = s.has_delay_slot;
		o->branch_target = s.branch_target;
	}
	void parallel_rsp_state_load(const void *in)
	{
		auto &s = RSP::cpu.get_state();
		auto *i = static_cast<const parallel_rsp_saved *>(in);
		memcpy(s.sr, i->sr, sizeof(i->sr));
		s.cp2 = i->cp2;
		s.pc = i->pc;
		s.has_delay_slot = i->has_delay_slot;
		s.branch_target = i->branch_target;
	}

	// Hack entry point to use when loading savestates when we're tracing.
	void rsp_clear_registers()
	{
		memset(RSP::cpu.get_state().sr, 0, sizeof(uint32_t) * 32);
		memset(&RSP::cpu.get_state().cp2, 0, sizeof(RSP::cpu.get_state().cp2));
	}

#ifdef INTENSE_DEBUG
	// Need super-fast hash here.
	static uint64_t hash_imem(const uint8_t *data, size_t size)
	{
		uint64_t h = 0xcbf29ce484222325ull;
		size_t i;
		for (i = 0; i < size; i++)
			h = (h * 0x100000001b3ull) ^ data[i];
		return h;
	}

	void log_rsp_mem_parallel(void)
	{
		fprintf(stderr, "IMEM HASH: 0x%016llx\n", hash_imem(RSP::rsp.IMEM, 0x1000));
		fprintf(stderr, "DMEM HASH: 0x%016llx\n", hash_imem(RSP::rsp.DMEM, 0x1000));
	}
#endif

	EXPORT unsigned int CALL parallelRSPDoRspCycles(unsigned int cycles)
	{
		/* Joey64 (2026-11-16): only HALT keeps the RSP from running, as on the hardware, where BROKE is just the flag a
		 * BREAK left. A task the CPU starts by clearing HALT alone (old libdragon's ugfx: SP_STATUS = 0x101, no
		 * CLR_BROKE) was skipped while the last task's BROKE stood, yet reported done: ZZ Gunner drew one frame and
		 * then waited forever for an RDP list that never came. The stale flag is cleared as the task starts, so the
		 * BROKE check after the run means this task's own BREAK. */
		if (*RSP::rsp.SP_STATUS_REG & SP_STATUS_HALT)
			return 0;
		*RSP::rsp.SP_STATUS_REG &= ~SP_STATUS_BROKE;

		// Only at a task's start: a task this RSP was already running continues here.
		if (hle_audio && (*RSP::rsp.SP_PC_REG & 0xfff) == 0 && hleTryAudioTask())
			return cycles;

		// We don't know if Mupen from the outside invalidated our IMEM.
		RSP::cpu.invalidate_imem();

		// Run CPU until we either break or we need to fire an IRQ.
		RSP::cpu.get_state().pc = *RSP::rsp.SP_PC_REG & 0xfff;

#ifdef INTENSE_DEBUG
		fprintf(stderr, "RUN TASK: %u\n", RSP::cpu.get_state().pc);
		log_rsp_mem_parallel();
#endif

		for (auto &count : RSP::MFC0_count)
			count = 0;
		RSP::DMA_poll_count = 0;
		RSP::yield_halt = false;

		while (!(*RSP::rsp.SP_STATUS_REG & SP_STATUS_HALT))
		{
			auto mode = RSP::cpu.run();
			if (mode == RSP::MODE_CHECK_FLAGS && (*RSP::cpu.get_state().cp0.irq & 1))
				break;
		}

		*RSP::rsp.SP_PC_REG = 0x04001000 | (RSP::cpu.get_state().pc & 0xffc);

		// From CXD4.
		if (*RSP::rsp.SP_STATUS_REG & SP_STATUS_BROKE)
			return cycles;
		else if (*RSP::cpu.get_state().cp0.irq & 1)
			RSP::rsp.CheckInterrupts();
		/* Joey64 (2026-11-16): only a HALT the task set itself ends it here (upstream be05ef71, "RSP hang when setting
		 * HALT but not BREAK"). The poll yields set HALT too, to leave the loop when the task waits on the CPU (SP_STATUS
		 * polled 16/0x7fff times, or the same DMA 256 times); since be05ef71 that return also caught them, so a task that
		 * waits on the CPU was dropped, never resumed: Pokemon Stadium's GB Tower froze when its Game Boy game started,
		 * and Thornmarked's RSP sat at pc 0x04c. A yield falls through to the HALT clear below as before be05ef71 (cxd4's
		 * rule), and the core resumes the task from its SP event (rsp_core.c rsp_interrupt_event). */
		else if ((*RSP::rsp.SP_STATUS_REG & SP_STATUS_HALT) && !RSP::yield_halt)
			return cycles;
		else if (*RSP::rsp.SP_SEMAPHORE_REG != 0) // Semaphore lock fixes.
		{
		}
		else
		{
			// Dev (GB Tower test, 2026-11-16): N64_RSP_POLL=<n> keeps n polls per slice instead of 16.
			static int poll = -1;
			if (poll < 0)
			{
				const char *e = getenv("N64_RSP_POLL");
				poll = e && atoi(e) > 0 ? atoi(e) : 16;
			}
			RSP::SP_STATUS_TIMEOUT = poll; // From now on, wait 16 times, not 0x7fff
		}

		// CPU restarts with the correct SIGs.
		*RSP::rsp.SP_STATUS_REG &= ~SP_STATUS_HALT;

		return cycles;
	}

	EXPORT m64p_error CALL parallelRSPPluginGetVersion(m64p_plugin_type *PluginType, int *PluginVersion,
	                                                   int *APIVersion, const char **PluginNamePtr, int *Capabilities)
	{
		/* set version info */
		if (PluginType != NULL)
			*PluginType = M64PLUGIN_RSP;

		if (PluginVersion != NULL)
			*PluginVersion = RSP_PARALLEL_VERSION;

		if (APIVersion != NULL)
			*APIVersion = RSP_PLUGIN_API_VERSION;

		if (Capabilities != NULL)
			*Capabilities = 0;

		return M64ERR_SUCCESS;
	}

	EXPORT void CALL parallelRSPRomClosed(void)
	{
		*RSP::rsp.SP_PC_REG = 0x00000000;
	}

	EXPORT void CALL parallelRSPInitiateRSP(RSP_INFO Rsp_Info, unsigned int *CycleCount)
	{
		if (CycleCount)
			*CycleCount = 0;

		if (Rsp_Info.DMEM == Rsp_Info.IMEM) /* usually dummy RSP data for testing */
			return; /* DMA is not executed just because plugin initiates. */

		RSP::rsp = Rsp_Info;
		const char *env = getenv("N64_HLE_AUDIO");
		hle_audio = !(env && *env == '0');
		if (hle_audio)
			hleInitiateRSP(Rsp_Info, nullptr);
		*RSP::rsp.SP_PC_REG = 0x04001000 & 0x00000FFF; /* task init bug on Mupen64 */

		auto **cr = RSP::cpu.get_state().cp0.cr;
		cr[0x0] = RSP::rsp.SP_MEM_ADDR_REG;
		cr[0x1] = RSP::rsp.SP_DRAM_ADDR_REG;
		cr[0x2] = RSP::rsp.SP_RD_LEN_REG;
		cr[0x3] = RSP::rsp.SP_WR_LEN_REG;
		cr[0x4] = RSP::rsp.SP_STATUS_REG;
		cr[0x5] = RSP::rsp.SP_DMA_FULL_REG;
		cr[0x6] = RSP::rsp.SP_DMA_BUSY_REG;
		cr[0x7] = RSP::rsp.SP_SEMAPHORE_REG;
		cr[0x8] = RSP::rsp.DPC_START_REG;
		cr[0x9] = RSP::rsp.DPC_END_REG;
		cr[0xA] = RSP::rsp.DPC_CURRENT_REG;
		cr[0xB] = RSP::rsp.DPC_STATUS_REG;
		cr[0xC] = RSP::rsp.DPC_CLOCK_REG;
		cr[0xD] = RSP::rsp.DPC_BUFBUSY_REG;
		cr[0xE] = RSP::rsp.DPC_PIPEBUSY_REG;
		cr[0xF] = RSP::rsp.DPC_TMEM_REG;

		*cr[RSP::CP0_REGISTER_SP_STATUS] = SP_STATUS_HALT;
		RSP::cpu.get_state().cp0.irq = RSP::rsp.MI_INTR_REG;

		// From CXD4.
		RSP::SP_STATUS_TIMEOUT = 0x7fff;

		RSP::cpu.set_dmem(reinterpret_cast<uint32_t *>(Rsp_Info.DMEM));
		RSP::cpu.set_imem(reinterpret_cast<uint32_t *>(Rsp_Info.IMEM));
		RSP::cpu.set_rdram(reinterpret_cast<uint32_t *>(Rsp_Info.RDRAM));
	}
}
