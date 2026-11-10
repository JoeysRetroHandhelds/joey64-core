#include "rdp.hpp"
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "joey/texpack_ghq.h"
#include "timeline_trace_file.hpp"
#include "Gfx #1.3.h"
#include "parallel.h"
#include "z64.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string>
#include <unordered_map>
#include <map>
#include <string.h>
#include <signal.h>
#include <dlfcn.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>
#include <atomic>
#include <unordered_set>

using namespace Vulkan;
using namespace std;

extern retro_log_printf_t log_cb;
extern retro_environment_t environ_cb;
extern "C" void *g_rdram_alias;
// new_dynarec's code cache (weak: absent without the dynarec); its size is NEW_DYNAREC_CACHE_SIZE
extern "C" __attribute__((weak)) void *base_addr_rx;

namespace RDP
{
const struct retro_hw_render_interface_vulkan *vulkan;
// Joey64 N64_RDP_WAITSTATS (defined in parallel-rdp/rdp_device.cpp)
extern std::atomic<uint64_t> joey_drain_ns, joey_timeline_ns;
extern std::atomic<uint32_t> joey_timeline_waits;
bool joey_waitstats_on();

static int cmd_cur;
static int cmd_ptr;
static uint32_t cmd_data[0x00040000 >> 2];
static uint64_t pending_timeline_value, timeline_value;

static unique_ptr<Util::TimelineTraceFile> timeline_trace_file;
static unique_ptr<CommandProcessor> frontend;
static uintptr_t frontend_rdram;
static size_t frontend_offset;
static unsigned frontend_rdram_size, active_upscaling;
static bool create_frontend();
static unique_ptr<Device> device;
static unique_ptr<Context> context;
static QueryPoolHandle begin_ts, end_ts;

// Pipelines compiled by the driver, kept between runs so shaders are not recompiled (and the game
// does not stall on them) every launch. Saved when it grows and at shutdown.
static string pipeline_cache_path;
static std::atomic<size_t> pipeline_cache_saved_size;
static unsigned frames_since_cache_check;
// Save only when the game created pipelines since the last save, and space saves out (10 s, doubling to 80 s at
// 60 fps) as a game keeps adding a few (Goemon adds ~10 per 10 s standing still); shutdown always saves. Each save
// reads the whole driver cache (~230 ms of a core on the Flip at 19-22 MB). N64_RDP_CACHE_EVERY=1 checks every
// 600 frames as before.
static unsigned cache_check_interval = 600;
static size_t pipelines_at_last_save;
// The periodic save runs on its own thread (Joey64, 2026-11-08): on the Flip's Mali the cache is 18-20 MB in Goemon
// and grows ~160 KB every 10 s, and reading it out of the driver and writing it stalled the emulation thread
// ~120 ms each time (one dropped burst every 600 frames). Vulkan pipeline caches are internally synchronized
// (no EXTERNALLY_SYNCHRONIZED flag here), so the driver can be read while the game creates pipelines.
// N64_RDP_CACHE_SYNC=1 saves on the emulation thread as before.
static std::thread pipeline_cache_saver;
static std::atomic<bool> pipeline_cache_saving{false};

// Joey64 EXPERIMENT N64_RDP_GPU_SPIN=<thousands of loop iterations>: after each scanout, one small compute dispatch
// that just loops (joey/gpu_spin.comp), so the GPU's load, and with it the governor's clock, stays up between frames
// (PCSX2 "Spin GPU"; the Flip's Mali drops to 384 MHz at 60 fps). Off when unset.
static void joey_gpu_spin()
{
	static const unsigned thousands = getenv("N64_RDP_GPU_SPIN") ? unsigned(strtoul(getenv("N64_RDP_GPU_SPIN"), nullptr, 10)) : 0u;
	if (!thousands || !device)
		return;
	static Vulkan::Program *program = nullptr;
	static Vulkan::BufferHandle sink;
	if (!program)
	{
		static const uint32_t code[] = {
#include "joey/gpu_spin.inc"
		};
		Vulkan::ResourceLayout layout;
		layout.sets[0].storage_buffer_mask = 1;
		layout.sets[0].array_size[0] = 1;
		layout.push_constant_size = 4;
		program = device->request_program(code, sizeof(code), &layout);
		Vulkan::BufferCreateInfo info = {};
		info.size = 64;
		info.domain = Vulkan::BufferDomain::Device;
		info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		sink = device->create_buffer(info);
		log_cb(RETRO_LOG_WARN, "paraLLEl-RDP: GPU spin on, %u000 iterations per scanout (%s).\n", thousands,
		       program && sink ? "ok" : "FAILED");
	}
	if (!program || !sink)
		return;
	auto cmd = device->request_command_buffer();
	cmd->set_program(program);
	cmd->set_storage_buffer(0, 0, *sink);
	uint32_t iterations = thousands * 1000u;
	cmd->push_constants(&iterations, 0, sizeof(iterations));
	cmd->dispatch(1, 1, 1);
	device->submit(cmd);
}

// The compute pipelines built in earlier runs (Granite's PipelineRecord), kept next to the pipeline cache in
// parallel-rdp-pipelines.bin. At start they are built ahead on parallel-RDP's pipeline thread (from the driver
// cache, so quickly), so a game does not stall on one, or run on the slower ubershader, the first time it needs
// it. From parallel-n64 (d8fd0cb). N64_RDP_PREBUILD=0 turns the building off; the list is still kept.
struct PipelineListHeader
{
	char magic[8];
	uint32_t version;
	uint32_t record_size;
	uint32_t count;
	uint32_t reserved;
	uint64_t checksum;
};
static const char pipeline_list_magic[8] = { 'P', 'R', 'D', 'P', 'P', 'I', 'P', 'E' };
static const uint32_t pipeline_list_version = 1;
static const size_t pipeline_list_max = 8192;
static string pipeline_list_path;
static vector<PipelineRecord> pipeline_list; // as loaded, or as last saved
static size_t pipeline_records_seen;

static uint64_t pipeline_list_checksum(const vector<PipelineRecord> &list)
{
	Util::Hasher h;
	h.data(reinterpret_cast<const uint32_t *>(list.data()), list.size() * sizeof(PipelineRecord));
	return h.get();
}

static void load_pipeline_list()
{
	pipeline_list.clear();
	pipeline_records_seen = 0;
	if (pipeline_list_path.empty())
		return;
	FILE *f = fopen(pipeline_list_path.c_str(), "rb");
	if (!f)
		return;
	PipelineListHeader header = {};
	bool ok = fread(&header, sizeof(header), 1, f) == 1 &&
	          memcmp(header.magic, pipeline_list_magic, sizeof(header.magic)) == 0 &&
	          header.version == pipeline_list_version &&
	          header.record_size == sizeof(PipelineRecord) &&
	          header.count <= pipeline_list_max;
	if (ok)
	{
		pipeline_list.resize(header.count);
		ok = header.count == 0 ||
		     fread(pipeline_list.data(), sizeof(PipelineRecord), header.count, f) == header.count;
	}
	fclose(f);
	if (ok)
		ok = pipeline_list_checksum(pipeline_list) == header.checksum;
	if (!ok)
	{
		// A damaged or old list is ignored, and written again at the next save.
		pipeline_list.clear();
		log_cb(RETRO_LOG_WARN, "paraLLEl-RDP: ignoring a damaged pipeline list at %s.\n", pipeline_list_path.c_str());
	}
}

// Adds this run's new pipelines to the list and writes it, when there are any.
static void save_pipeline_list()
{
	if (pipeline_list_path.empty() || !device)
		return;
	vector<PipelineRecord> built = device->get_pipeline_records();
	if (built.size() == pipeline_records_seen)
		return;
	pipeline_records_seen = built.size();

	// The earlier ones first, then this run's that are new; past the cap the oldest go.
	unordered_set<uint64_t> known;
	for (auto &r : pipeline_list)
		known.insert(r.pipeline_hash);
	vector<PipelineRecord> merged = pipeline_list;
	for (auto &r : built)
		if (known.insert(r.pipeline_hash).second)
			merged.push_back(r);
	if (merged.size() == pipeline_list.size())
		return;
	if (merged.size() > pipeline_list_max)
		merged.erase(merged.begin(), merged.begin() + (merged.size() - pipeline_list_max));

	PipelineListHeader header = {};
	memcpy(header.magic, pipeline_list_magic, sizeof(header.magic));
	header.version = pipeline_list_version;
	header.record_size = sizeof(PipelineRecord);
	header.count = uint32_t(merged.size());
	header.checksum = pipeline_list_checksum(merged);

	// Write a temporary file and rename it, so a kill mid-write can't leave a torn list.
	string tmp = pipeline_list_path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f)
		return;
	bool ok = fwrite(&header, sizeof(header), 1, f) == 1 &&
	          fwrite(merged.data(), sizeof(PipelineRecord), merged.size(), f) == merged.size();
	ok = fclose(f) == 0 && ok;
	if (ok && rename(tmp.c_str(), pipeline_list_path.c_str()) == 0)
	{
		log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: saved pipeline list, %u pipelines (%u new).\n",
		       unsigned(merged.size()), unsigned(merged.size() - pipeline_list.size()));
		pipeline_list = std::move(merged);
	}
	else
		remove(tmp.c_str());
}

// Right after the CommandProcessor is made, before any command.
static void build_pipelines_ahead()
{
	const char *env = getenv("N64_RDP_PREBUILD");
	if ((env && env[0] == '0') || pipeline_list.empty() || !frontend)
		return;
	unsigned urgent = 0, background = 0;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	frontend->build_pipelines_ahead(pipeline_list.data(), pipeline_list.size(), urgent, background);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	const char *thr = getenv("N64_RDP_PREBUILD_THREADS");
	double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
	log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: pipeline list of %u: built %u ahead in %.1f ms, %u more in the background (threads %s).\n",
	       unsigned(pipeline_list.size()), urgent, ms, background, thr ? thr : "1");
}

static void load_pipeline_cache()
{
	const char *dir = nullptr;
	pipeline_cache_path.clear();
	pipeline_list_path.clear();
	vector<uint8_t> data;
	if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
	{
		pipeline_cache_path = string(dir) + "/Mupen64plus/parallel-rdp-pipeline-cache.bin";
		pipeline_list_path = string(dir) + "/Mupen64plus/parallel-rdp-pipelines.bin";
		if (FILE *f = fopen(pipeline_cache_path.c_str(), "rb"))
		{
			if (fseek(f, 0, SEEK_END) == 0)
			{
				long size = ftell(f);
				if (size > 0)
				{
					data.resize(size);
					rewind(f);
					if (fread(data.data(), 1, data.size(), f) != data.size())
						data.clear();
				}
			}
			fclose(f);
		}
	}
	// Granite checks the GPU's cache UUID and a hash, and starts fresh when they don't match.
	device->init_pipeline_cache(data.empty() ? nullptr : data.data(), data.size());
	pipeline_cache_saved_size = data.size();
	frames_since_cache_check = 0;
	cache_check_interval = 600;
	pipelines_at_last_save = 0;
	log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: pipeline cache %u bytes from %s.\n",
	       unsigned(data.size()), pipeline_cache_path.c_str());
	load_pipeline_list();
}

// Reads the driver's cache and writes it when it changed; on the saver thread or, at shutdown, the caller's.
static void write_pipeline_cache(Device *dev, const string &path, size_t built)
{
	auto start = std::chrono::steady_clock::now();
	size_t size = dev->get_pipeline_cache_size();
	if (!size || size == pipeline_cache_saved_size)
		return;
	vector<uint8_t> data(size);
	if (!dev->get_pipeline_cache_data(data.data(), size))
		return;
	// Write a temporary file and rename it, so a kill mid-write can't leave a torn cache.
	string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f)
		return;
	bool ok = fwrite(data.data(), 1, size, f) == size;
	ok = fclose(f) == 0 && ok;
	if (ok && rename(tmp.c_str(), path.c_str()) == 0)
	{
		pipeline_cache_saved_size = size;
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: saved pipeline cache, %u bytes, %.0f ms (%u pipelines compiled this run).\n",
		       unsigned(size), ms, unsigned(built));
	}
	else
		remove(tmp.c_str());
}

static void wait_pipeline_cache_saver()
{
	if (pipeline_cache_saver.joinable())
		pipeline_cache_saver.join();
}

// background: the periodic save (skipped while the last one still runs); else at shutdown, on this thread.
// Returns false when a background save was skipped.
static bool save_pipeline_cache(bool background = false)
{
	save_pipeline_list();
	if (pipeline_cache_path.empty() || !device)
		return true;
	static const bool sync = getenv("N64_RDP_CACHE_SYNC") && *getenv("N64_RDP_CACHE_SYNC") == '1';
	if (!background || sync)
	{
		wait_pipeline_cache_saver();
		write_pipeline_cache(device.get(), pipeline_cache_path, device->get_pipeline_records().size());
		return true;
	}
	if (pipeline_cache_saving)
		return false;
	wait_pipeline_cache_saver();
	pipeline_cache_saving = true;
	Device *dev = device.get();
	string path = pipeline_cache_path;
	size_t built = device->get_pipeline_records().size();
	pipeline_cache_saver = std::thread([dev, path, built]() {
		write_pipeline_cache(dev, path, built);
		pipeline_cache_saving = false;
	});
	return true;
}

static vector<retro_vulkan_image> retro_images;
static vector<ImageHandle> retro_image_handles;
unsigned width, height;
unsigned overscan;
unsigned upscaling = 1;
unsigned downscaling_steps = 0;
bool native_texture_lod = false;
bool native_tex_rect = true;
bool synchronous = true, divot_filter = true, gamma_dither = true;
bool vi_aa = true, vi_scale = true, dither_filter = true;
bool interlacing = true, super_sampled_read_back = false, super_sampled_dither = true;
// Whether a new picture was scanned out since the frontend last asked (take_frame_valid). Sticky: with repeat
// skipping, one retro_run can end two refreshes, a new one and then a repeat, and the repeat must not hide the new
// image (Banjo-Kazooie/Tooie on the Duo kept showing stale and black frames with the app skipping repeats).
bool frame_valid = true;

// Smart sync (replaces synchronous mode when the synchronous option is on; env N64_RDP_SMART=0 gives plain
// synchronous mode again; with the option off parallel-RDP is plain async): run asynchronously, but remember
// which RDRAM pages the RDP drew into
// (color image and depth buffer rows inside the scissor). At SyncFull those pages are made inaccessible; the
// first CPU access to one (CPU load/store, dynarec, or a DMA copy, which are all CPU memcpys here) faults,
// waits for the GPU to reach that SyncFull, and unprotects them. Async speed, synchronous results for games
// that read back what the RDP drew (gopher64 does the same with interpreter hooks). Needs the RDRAM imported
// into Vulkan (VK_EXT_external_memory_host, the GPU writes the same pages) or the core's second RDRAM mapping
// (g_rdram_alias, for paraLLEl-RDP's copy-back thread). Measured: Duo (Turnip) renders RE2 and Cruis'n USA like
// sync, which async shows black, at or below sync cost; Flip (Mali) Snap 14.4 -> 5.0 ms/frame vs sync. Most games
// touch drawn pages every frame (Flip: SM64 587, OoT 599 waits per 600 SyncFulls), so it is no faster than sync
// for them and much slower than async: it replaces sync only.
extern "C" void ra_cow_drawn(uint32_t start, uint32_t end);
namespace Smart
{
static bool enabled;
static uint8_t *rdram;
static size_t rdram_size, page_shift;
static vector<uint8_t> dirty, locked;
static bool any_locked;
static volatile uint64_t sync_signal;
static struct sigaction old_action;
static bool handler_installed;
static unsigned faults, sync_count;
// N64_RDP_SMART_DEBUG=1: what the waits hit (page kind 1 = color, 2 = depth, 3 = both) and who touched it
static bool debug;
static unsigned faults_kind[4];
static uint32_t last_fault_offset;
static uintptr_t last_fault_pc;
// Byte ranges drawn since the last SyncFull (drawn) and still locked (locked_ranges): a fault outside them only
// shares a page with what the RDP drew. Precise mode (default; N64_RDP_SMART_PRECISE=0 turns it off): such a fault unlocks that page without waiting,
// if it came from dynarec code (one load or store, at most 8 bytes). Anything else (a DMA or RSP memcpy, C code)
// may run on into the drawn bytes of the same page after its first fault, so it always waits: letting those
// through left Cruis'n USA's RSP looping forever on its boot screen.
static bool precise;
static vector<pair<uint32_t, uint32_t>> drawn, locked_ranges;
static unsigned faults_inside, faults_outside, faults_copy;
// A page opened that way is locked again ~0.5 ms later by a helper thread (relock_main), so a later read of its
// drawn part faults and waits again instead of seeing stale RDRAM until the next SyncFull (on Turnip/Adreno
// without host-imported RDRAM the CPU only sees RDP results after a wait). A read inside those 0.5 ms can still
// see old pixels. guard serialises page state between the emulation thread, the handler and the helper.
static vector<uint8_t> opened;
static bool any_opened;
static std::atomic_flag guard = ATOMIC_FLAG_INIT;
static sem_t relock_sem;
static bool relock_started;
struct Guard
{
	Guard() { while (guard.test_and_set(std::memory_order_acquire)) {} }
	~Guard() { guard.clear(std::memory_order_release); }
};
static uint32_t outside_page[8];
static unsigned outside_count[8];

// RDP state that decides where draws land
static uint32_t color_addr, color_width, color_size, depth_addr;
static uint32_t scissor_yh, scissor_yl;
static bool depth_write;

static void add_range(vector<pair<uint32_t, uint32_t>> &ranges, uint32_t start, uint32_t end)
{
	for (auto &r : ranges)
		if (start <= r.second && end >= r.first)
		{
			r.first = min(r.first, start);
			r.second = max(r.second, end);
			return;
		}
	ranges.push_back({ start, end });
}

static bool touches_locked_range(uint32_t start, uint32_t end)
{
	for (auto &r : locked_ranges)
		if (start < r.second && end > r.first)
			return true;
	return false;
}

static bool from_dynarec(void *uc)
{
#if defined(__aarch64__)
	uintptr_t pc = static_cast<ucontext_t *>(uc)->uc_mcontext.pc;
	uintptr_t base = &base_addr_rx ? reinterpret_cast<uintptr_t>(base_addr_rx) : 0;
	return base && pc >= base && pc - base < (uintptr_t(1) << 25);
#else
	(void)uc;
	return false;
#endif
}

static void set_pages(uint32_t start, uint32_t end, uint8_t kind)
{
	if (start >= rdram_size)
		return;
	if (end > rdram_size)
		end = rdram_size;
	ra_cow_drawn(start, end); // before the GPU gets the draw: run-ahead keeps these pages' bytes first
	if (precise || debug)
		add_range(drawn, start, end);
	for (size_t p = start >> page_shift; p <= ((end - 1) >> page_shift); p++)
		dirty[p] |= kind;
}

static void mark_draw(bool zbuffered)
{
	uint32_t stride = color_size ? (color_width << (color_size - 1)) : (color_width >> 1);
	uint32_t y0 = scissor_yh, y1 = scissor_yl + 1;
	if (!stride || y1 <= y0)
		return;
	set_pages(color_addr + y0 * stride, color_addr + y1 * stride, 1);
	if (zbuffered && depth_write)
		set_pages(depth_addr + y0 * color_width * 2, depth_addr + y1 * color_width * 2, 2);
}

// Coalesced mprotect over runs of pages where pick(p) is set.
template <typename F>
static void protect_runs(F pick, int prot)
{
	size_t n = dirty.size();
	for (size_t p = 0; p < n;)
	{
		if (!pick(p)) { p++; continue; }
		size_t q = p;
		while (q < n && pick(q))
			q++;
		mprotect(rdram + (p << page_shift), (q - p) << page_shift, prot);
		p = q;
	}
}

static void unlock_all()
{
	Guard g;
	if (any_opened)
	{
		fill(opened.begin(), opened.end(), 0);
		any_opened = false;
	}
	if (!any_locked)
		return;
	protect_runs([](size_t p) { return locked[p] != 0; }, PROT_READ | PROT_WRITE);
	fill(locked.begin(), locked.end(), 0);
	any_locked = false;
	locked_ranges.clear();
}

static void *relock_main(void *)
{
	for (;;)
	{
		while (sem_wait(&relock_sem) != 0) {}
		usleep(500);
		Guard g;
		if (!any_opened)
			continue;
		// Mark first, then protect: the emulation thread may fault on the page the moment it is protected, and the
		// handler passes faults on unmarked pages on as crashes (OoT and Banjo-Tooie died that way on the Duo).
		any_locked = true;
		for (size_t p = 0; p < opened.size(); p++)
			if (opened[p])
			{
				locked[p] = 1;
				opened[p] = 0;
				std::atomic_thread_fence(std::memory_order_seq_cst);
				mprotect(rdram + (p << page_shift), size_t(1) << page_shift, PROT_NONE);
			}
		any_opened = false;
	}
	return nullptr;
}

static void on_sync_full()
{
	sync_signal = frontend->signal_timeline();
	if (++sync_count % 600 == 0)
	{
		log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: smart sync: %u CPU waits in the last 600 SyncFulls.\n", faults);
		faults = 0;
		if (debug)
		{
			Dl_info info = {};
			const char *who = (last_fault_pc && dladdr(reinterpret_cast<void *>(last_fault_pc), &info) && info.dli_sname)
			                      ? info.dli_sname : "JIT or unknown";
			log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: smart debug: color %u, depth %u, both %u; last at 0x%06x (color 0x%06x w %u, depth 0x%06x) by %s\n",
			       faults_kind[1], faults_kind[2], faults_kind[3], last_fault_offset, color_addr, color_width, depth_addr, who);
			faults_kind[1] = faults_kind[2] = faults_kind[3] = 0;
			char pages[160];
			int n = 0;
			for (int i = 0; i < 8 && outside_count[i] && n < int(sizeof(pages)) - 24; i++)
				n += snprintf(pages + n, sizeof(pages) - n, " 0x%06x:%u", outside_page[i], outside_count[i]);
			pages[n] = 0;
			log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: smart debug: %u faults inside drawn bytes, %u copies starting outside them, %u dynarec accesses only on a shared page;%s\n",
			       faults_inside, faults_copy, faults_outside, pages);
			faults_inside = faults_outside = faults_copy = 0;
			memset(outside_count, 0, sizeof(outside_count));
		}
	}
	Guard g;
	for (auto &r : drawn)
		add_range(locked_ranges, r.first, r.second);
	drawn.clear();
	protect_runs([](size_t p) { return dirty[p] && !locked[p]; }, PROT_NONE);
	for (size_t p = 0; p < dirty.size(); p++)
		if (dirty[p])
		{
			locked[p] |= dirty[p];
			dirty[p] = 0;
			any_locked = true;
		}
}

static void handler(int sig, siginfo_t *info, void *uc)
{
	uint8_t *addr = static_cast<uint8_t *>(info->si_addr);
	if (any_locked && addr >= rdram && addr < rdram + rdram_size && locked[size_t(addr - rdram) >> page_shift])
	{
		if (debug)
		{
			faults_kind[locked[size_t(addr - rdram) >> page_shift] & 3]++;
			last_fault_offset = uint32_t(addr - rdram);
#if defined(__aarch64__)
			last_fault_pc = static_cast<ucontext_t *>(uc)->uc_mcontext.pc;
#endif
		}
		uint32_t offset = uint32_t(addr - rdram);
		bool inside = !(precise || debug) || touches_locked_range(offset, offset + 8);
		bool single = (precise || debug) && from_dynarec(uc);
		if (debug)
		{
			if (inside)
				faults_inside++;
			else if (!single)
				faults_copy++;
			else
			{
				faults_outside++;
				uint32_t page = offset >> page_shift << page_shift;
				for (int i = 0; i < 8; i++)
					if (!outside_count[i] || outside_page[i] == page)
					{
						outside_page[i] = page;
						outside_count[i]++;
						break;
					}
			}
		}
		if (precise && !inside && single)
		{
			// Only shares a page with the drawing: let this access through without waiting for the GPU.
			size_t p = offset >> page_shift;
			{
				Guard g;
				mprotect(rdram + (p << page_shift), size_t(1) << page_shift, PROT_READ | PROT_WRITE);
				locked[p] = 0;
				opened[p] = 1;
				any_opened = true;
			}
			sem_post(&relock_sem); // async-signal-safe
			return;
		}
		frontend->wait_for_timeline(sync_signal);
		unlock_all();
		faults++;
		return; // the access is retried, now with the RDP's results in RDRAM
	}
	// Not ours: hand it on.
	if (old_action.sa_flags & SA_SIGINFO)
	{
		if (old_action.sa_sigaction)
			old_action.sa_sigaction(sig, info, uc);
	}
	else if (old_action.sa_handler == SIG_DFL)
	{
		sigaction(sig, &old_action, nullptr);
		raise(sig);
	}
	else if (old_action.sa_handler != SIG_IGN)
		old_action.sa_handler(sig);
}

// After a full GPU drain before the CommandProcessor is rebuilt: nothing is pending, and the old timeline values
// mean nothing to the new one.
static void reset_after_drain()
{
	if (!enabled)
		return;
	unlock_all();
	fill(dirty.begin(), dirty.end(), 0);
	drawn.clear();
	sync_signal = 0;
}

static bool requested()
{
	const char *env = getenv("N64_RDP_SMART");
	return !env || env[0] != '0';
}

// host_imported: the GPU writes these pages itself. Otherwise paraLLEl-RDP copies results back on a worker
// thread, which must use g_rdram_alias so it never touches a protected page.
static void init(void *rdram_ptr, size_t size, bool host_imported)
{
	enabled = false;
	if (!requested())
		return;
	if (!host_imported && !g_rdram_alias)
	{
		log_cb(RETRO_LOG_WARN, "paraLLEl-RDP: smart sync needs shared RDRAM or VK_EXT_external_memory_host, staying as configured.\n");
		return;
	}
	const char *dbg = getenv("N64_RDP_SMART_DEBUG");
	debug = dbg && dbg[0] == '1';
	const char *pre = getenv("N64_RDP_SMART_PRECISE");
	precise = !(pre && pre[0] == '0');
	drawn.clear();
	locked_ranges.clear();
	long ps = sysconf(_SC_PAGESIZE);
	page_shift = 0;
	while ((size_t(1) << page_shift) < size_t(ps))
		page_shift++;
	rdram = static_cast<uint8_t *>(rdram_ptr);
	rdram_size = size;
	dirty.assign(size >> page_shift, 0);
	locked.assign(size >> page_shift, 0);
	opened.assign(size >> page_shift, 0);
	any_opened = false;
	if (precise && !relock_started)
	{
		pthread_t t;
		sem_init(&relock_sem, 0, 0);
		relock_started = pthread_create(&t, nullptr, relock_main, nullptr) == 0;
		if (relock_started)
			pthread_detach(t);
		else
			precise = false;
	}
	any_locked = false;
	faults = 0;
	if (!handler_installed)
	{
		struct sigaction sa = {};
		sa.sa_sigaction = handler;
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
		sigemptyset(&sa.sa_mask);
		if (sigaction(SIGSEGV, &sa, &old_action) != 0)
			return;
		handler_installed = true;
	}
	enabled = true;
	log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: smart sync on (%ld-byte pages%s).\n", ps, precise ? ", precise" : "");
}

static void deinit()
{
	if (enabled)
		unlock_all();
	enabled = false;
}

static void track(RDP::Op op, uint32_t w1, uint32_t w2)
{
	switch (op)
	{
	case RDP::Op::SetColorImage:
		color_addr = w2 & 0x00FFFFFF;
		color_size = (w1 >> 19) & 3;
		color_width = (w1 & 0x3FF) + 1;
		break;
	case RDP::Op::SetMaskImage:
		depth_addr = w2 & 0x00FFFFFF;
		break;
	case RDP::Op::SetScissor:
		scissor_yh = (w1 & 0xFFF) >> 2;
		scissor_yl = (w2 & 0xFFF) >> 2;
		break;
	case RDP::Op::SetOtherModes:
		// 1/2-cycle mode with Z update writes the depth buffer
		depth_write = ((w1 >> 20) & 2) == 0 && ((w2 >> 5) & 1);
		break;
	case RDP::Op::FillTriangle:
	case RDP::Op::TextureTriangle:
	case RDP::Op::ShadeTriangle:
	case RDP::Op::ShadeTextureTriangle:
	case RDP::Op::TextureRectangle:
	case RDP::Op::TextureRectangleFlip:
	case RDP::Op::FillRectangle:
		mark_draw(false);
		break;
	case RDP::Op::FillZBufferTriangle:
	case RDP::Op::TextureZBufferTriangle:
	case RDP::Op::ShadeZBufferTriangle:
	case RDP::Op::ShadeTextureZBufferTriangle:
		mark_draw(true);
		break;
	default:
		break;
	}
}
}

bool smart_on()
{
	return Smart::enabled && synchronous && frontend;
}

// Waits for everything the RDP drew up to the last SyncFull and opens the pages again (what a fault on them does).
void smart_settle()
{
	if (!Smart::enabled || !frontend)
		return;
	if (Smart::any_locked || Smart::any_opened)
		frontend->wait_for_timeline(Smart::sync_signal);
	Smart::unlock_all();
}

// Joey64 run-ahead (2026-11-09): with upscaling, some frames' draws are done at native size only (RDRAM stays exact;
// a later upscaled pass picks up what they drew, blown up from native). [av] is the frontend's GET_AUDIO_VIDEO_ENABLE.
// N64_RA_NATIVE_HIDDEN=1: frames without video (run-ahead's real frame). Thor: visibly blockier, since most games show
// at a frame's end what they drew in the frame before, so the real frame's drawing is what's on screen.
// N64_RA_NATIVE_HIDDEN=2: the extra frames (bit 3), whose drawing is put back afterwards. Thor: 9 of the 14 quick-set
// games show exactly the same pictures; Paper Mario, Smash, Snap, Perfect Dark and Cruis'n USA show at a frame's end
// what that same frame drew (often or sometimes), so their pictures went blocky.
// N64_RA_NATIVE_HIDDEN=3 (automatic): only the shown extra frame (bits 0 and 3), and only once 600 of them in a row
// showed rows no draw of that same frame touched (Dup's tracking); the first one that did turns it off for good.
// Mode 3 is the default (Joey 2026-11-09, after all 14 quick-set games showed the same pictures at 4x on the Thor);
// N64_RA_NATIVE_HIDDEN=0 turns it off. Without run-ahead no frame has bit 3, so nothing changes.
// Set at each retro_run, before the frame's commands, so it applies in command order.
namespace RaNative
{
static int mode = -1, cur_av = 3;
static bool off;
static unsigned clean; // shown extra frames in a row whose shown rows that frame didn't draw
// The first 60 shown frames don't count: starting from a state, GoldenEye's first shown frame looked like it drew what
// it showed and no other one in 1200 did (Thor). Native draws can't start before 600 anyway.
static unsigned warm;
// This frame's draws are at native size only (set_hidden_frame's choice for it). When such a frame turns out to have
// drawn what it shows, that picture is not shown: the last one is held one refresh instead. Spider-Man (Thor
// 2026-11-09): the first frame of its copyright screen, drawn straight into the shown buffer after 600 frames that
// weren't, came out with softer text and a faint bar; held, it shows the black frame before once more.
static bool native_now;
}
void set_hidden_frame(int av)
{
	static int allow = -1;
	static bool last;
	static unsigned frames, native;
	if (allow < 0)
	{
		const char *e = getenv("N64_RA_NATIVE_HIDDEN");
		allow = e ? atoi(e) : 3;
		if (e && allow)
			log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: run-ahead: %s frames drawn at native size\n",
			       allow == 3 ? "shown extra (automatic)" : allow == 2 ? "extra" : "hidden");
		RaNative::mode = allow;
	}
	RaNative::cur_av = av;
	if (!allow || !frontend)
		return;
	bool hidden = allow == 3 ? (av & 9) == 9 && !RaNative::off && RaNative::clean >= 600 :
	              allow == 2 ? (av & 8) != 0 : !(av & 1);
	bool skip = hidden && active_upscaling > 1 && !super_sampled_read_back;
	if (skip != last)
	{
		frontend->set_skip_upscaled(skip);
		last = skip;
	}
	RaNative::native_now = skip;
	native += skip;
	if (++frames >= 1200)
	{
		log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: run-ahead: %u of the last %u frames drawn at native size only\n", native, frames);
		frames = native = 0;
	}
}

static const unsigned cmd_len_lut[64] = {
	1, 1, 1, 1, 1, 1, 1, 1, 4, 6, 12, 14, 12, 14, 20, 22,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1,
	1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1,
};

// Repeat frames (default; env N64_RDP_SKIP_DUP=0 turns it off): most games draw 20-30 frames a second, but the VI scans one out every
// refresh. A refresh is skipped (the frontend keeps showing the last picture) when the VI registers are the same as
// last time, the RDP drew nothing into the scanned-out rows since, and those rows' RDRAM hashes the same (CPU
// writes). Interlaced output always scans out. With smart sync, RDRAM is read through the never-protected alias.
namespace Dup
{
static bool enabled;
static uint32_t color_addr, color_width, color_size, scissor_yh, scissor_yl;
static vector<std::pair<uint32_t, uint32_t>> drawn; // byte ranges the RDP drew into since the last scanout
static uint32_t last_vi[10];
static uint64_t last_hash;
static bool have_last;
static bool last_drew; // the last refresh's rows were drawn into since the scanout before (run-ahead's automatic native draws)
// The RDP's results reach RDRAM (where the hash looks for CPU drawing) only when its GPU work for the frame
// finishes: right after a draw into the scanned-out range the hash still sees the old picture. Until then
// (settle_until on the timeline) the hash is not compared, and once there it is re-baselined, so a frame the
// GPU finished late is not counted as CPU drawing (GoldenEye and Paper Mario: no repeats were found at all).
static uint64_t settle_until;
static bool settling;
static unsigned skipped, scanned;
// Why refreshes were new, over the same 600 (one refresh can count for several): VI registers changed, the RDP
// drew into the scanned-out range (overflow: more than 32 targets, so everything counted), the RDRAM hash changed.
static unsigned why_vi, why_drew, why_overflow, why_hash;

static void add_draw()
{
	uint32_t stride = color_size ? (color_width << (color_size - 1)) : (color_width >> 1);
	uint32_t y0 = scissor_yh, y1 = scissor_yl + 1;
	if (!stride || y1 <= y0)
		return;
	std::pair<uint32_t, uint32_t> r(color_addr + y0 * stride, color_addr + y1 * stride);
	for (auto &d : drawn)
		if (d == r)
			return;
	if (drawn.size() >= 32)
	{
		drawn.assign(1, std::pair<uint32_t, uint32_t>(0, 0xFFFFFFFFu)); // too many targets: everything is dirty
		why_overflow++;
	}
	else
		drawn.push_back(r);
}

static void track(RDP::Op op, uint32_t w1, uint32_t w2)
{
	switch (op)
	{
	case RDP::Op::SetColorImage:
		color_addr = w2 & 0x00FFFFFF;
		color_size = (w1 >> 19) & 3;
		color_width = (w1 & 0x3FF) + 1;
		break;
	case RDP::Op::SetScissor:
		scissor_yh = (w1 & 0xFFF) >> 2;
		scissor_yl = (w2 & 0xFFF) >> 2;
		break;
	case RDP::Op::FillTriangle:
	case RDP::Op::TextureTriangle:
	case RDP::Op::ShadeTriangle:
	case RDP::Op::ShadeTextureTriangle:
	case RDP::Op::TextureRectangle:
	case RDP::Op::TextureRectangleFlip:
	case RDP::Op::FillRectangle:
	case RDP::Op::FillZBufferTriangle:
	case RDP::Op::TextureZBufferTriangle:
	case RDP::Op::ShadeZBufferTriangle:
	case RDP::Op::ShadeTextureZBufferTriangle:
		add_draw();
		break;
	default:
		break;
	}
}

// True when this refresh would scan out the same picture as the last one.
static bool same_as_last()
{
	const uint32_t vi[10] = { *GET_GFX_INFO(VI_STATUS_REG), *GET_GFX_INFO(VI_ORIGIN_REG), *GET_GFX_INFO(VI_WIDTH_REG),
	                          *GET_GFX_INFO(VI_X_SCALE_REG), *GET_GFX_INFO(VI_Y_SCALE_REG), *GET_GFX_INFO(VI_H_START_REG),
	                          *GET_GFX_INFO(VI_V_START_REG), *GET_GFX_INFO(VI_V_SYNC_REG), *GET_GFX_INFO(VI_H_SYNC_REG),
	                          upscaling };
	// Rows the VI reads: from the origin, the active lines stepped by the Y scale (2.10 fixed point, plus its
	// subpixel start offset), and the next row when it interpolates between two. Exact, because games keep their
	// frame buffers back to back (SM64): a margin made drawing the next frame look like drawing into this one.
	uint32_t type = vi[0] & 3, bpp = type == 3 ? 4 : 2, stride = (vi[2] & 0xFFF) * bpp;
	uint32_t v0 = (vi[6] >> 16) & 0x3FF, v1 = vi[6] & 0x3FF;
	uint32_t lines = v1 > v0 ? (v1 - v0) / 2 : 0;
	uint32_t yscale = vi[4] & 0xFFF, yoff = (vi[4] >> 16) & 0xFFF;
	uint32_t last = lines ? (lines - 1) * yscale + yoff : 0;
	uint32_t rows = lines ? last / 1024 + 1 + ((last & 1023) ? 1 : 0) : 0;
	uint32_t origin = vi[1] & 0x00FFFFFF;
	uint32_t s = origin;
	uint32_t e = std::min<uint64_t>(uint64_t(origin) + uint64_t(rows) * stride, frontend_rdram_size);
	bool drew = false;
	for (auto &d : drawn)
		drew |= d.first < e && s < d.second;
	// N64_RDP_SKIP_DUP_DEBUG=1: the VI registers, the scanned-out range and what the RDP drew, every 60 refreshes
	static const bool dbg = getenv("N64_RDP_SKIP_DUP_DEBUG") && getenv("N64_RDP_SKIP_DUP_DEBUG")[0] == '1';
	static unsigned dbg_n;
	if (dbg && ++dbg_n % 60 == 0)
	{
		char buf[512];
		int n = snprintf(buf, sizeof(buf), "status %x origin %x width %u xs %x ys %x hstart %x vstart %x -> rows %u stride %u [%x,%x) drew %d; draws",
		                 vi[0], vi[1], vi[2], vi[3], vi[4], vi[5], vi[6], rows, stride, s, e, drew);
		for (size_t i = 0; i < drawn.size() && i < 8 && n < int(sizeof(buf)) - 24; i++)
			n += snprintf(buf + n, sizeof(buf) - n, " [%x,%x)", drawn[i].first, drawn[i].second);
		log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: dup debug: %s\n", buf);
	}
	drawn.clear();
	last_drew = drew;
	uint64_t hash = 1469598103934665603ull;
	if (type >= 2 && s < e)
	{
		const uint8_t *mem = g_rdram_alias ? static_cast<const uint8_t *>(g_rdram_alias) : DRAM;
		const uint64_t *w = reinterpret_cast<const uint64_t *>(mem + (s & ~7u));
		for (size_t i = 0, n = (e - (s & ~7u)) / 8; i < n; i++)
			hash = (hash ^ w[i]) * 1099511628211ull;
	}
	bool vi_same = memcmp(vi, last_vi, sizeof(vi)) == 0;
	bool hash_same = hash == last_hash;
	if (drew || !vi_same || !have_last) // a draw into the shown range, or a flip to a buffer drawn earlier
	{
		settle_until = timeline_value; // signalled for this frame in complete_frame
		settling = true;
	}
	else if (settling)
	{
		if (frontend->timeline_reached(settle_until))
			settling = false; // results in RDRAM now: this hash is the new baseline
		hash_same = true;
	}
	bool same = have_last && !drew && hash_same && vi_same &&
	            !(vi[0] & 0x40); // serrate: interlaced fields alternate
	why_vi += !vi_same; why_drew += drew; why_hash += !hash_same;
	memcpy(last_vi, vi, sizeof(vi));
	last_hash = hash;
	have_last = true;
	return same;
}
}

// Texture pack keys, measurement only (env N64_RDP_TEXKEYS=<file>; master list item 35, step A). Off: nothing runs.
// Computes, for every textured draw, the key GLideN64 would look a texture pack up with (TextureCache::update ->
// _calcTileSizes -> _loadHiresTexture, GLideN64/src/Textures.cpp; state from gDPSetTextureImage, gDPSetTile,
// gDPSetTileSize, gDPLoadTile, gDPLoadBlock, gDPLoadTLUT in gDP.cpp, decoded as RDP.cpp does), hashed with GLideNHQ's
// Rice CRC (TxUtil::checksum64, copied). Nothing is rendered differently. Every 60 frames the key counts go to the
// file; the pack lookup happens on the PC (tools/htsindex.py), so the pack never has to be on the device.
// GLideN64 code: GPLv2-or-later (Joey64 is GPL-3.0-or-later).
// Texture packs (env N64_RDP_TEXPACK=<folder>; master list item 35, step B). The folder holds a GLideN64 pack for this
// ROM (<ROM name>_HIRESTEXTURES.hts, or a PNG pack folder named after the ROM); GLideNHQ reads it (joey/texpack_ghq).
// TexKeys computes GLideN64's key at each textured draw; the first time a key is seen, a worker thread looks it up,
// decodes it to RGBA8 and writes it into the renderer's replacement buffer (N64_RDP_TEXPACK_MB, default 256, capped at
// the GPU's storage buffer range). Until it is there the draw keeps the N64 texture (as Dolphin, PCSX2, PPSSPP and Azahar
// do with async loading), so loading never stalls a frame. Draws whose texture has a replacement get it through
// CommandProcessor::set_tile_replace, queued just before the draw; the TEXPACK shaders sample it in place of TMEM.
// The buffer is a ring: when it is full, the worker evicts the replacements in its way that no draw has used for a
// while (first 10 s, then down to 8 frames, past what the GPU can still have in flight) and skips the ones in use; an
// evicted replacement loads again the next time a draw wants it. Where the driver allows it the replacements go into a
// 2D image instead (CommandProcessor::enable_texpack), sampled with one hardware bilinear read: placed on shelves (rows
// of one height class, 2^k + 2 for the 1-texel wrap gutter around each), each shelf its own eviction ring. Without this a whole pack (Mario Kart 64 Reloaded:
// 217 MB in 40 s) fills the buffer and new textures stop being replaced; 256 MB is also the Mali-G52's limit.
// Off: nothing runs and the stock shaders are used.
namespace TexPack
{
static bool enabled;
static uint32_t *texels;
static size_t capacity; // texels
static RDP::CommandProcessor::TexpackImage image; // width 0: the buffer path
static std::string dir, rom;

struct Key
{
	uint64_t crc;
	uint16_t fmt, siz;
	bool operator==(const Key &o) const { return crc == o.crc && fmt == o.fmt && siz == o.siz; }
};
// state: 0 queued, 1 ready, 2 none (not in the pack, or too big), 3 being evicted (worker), 4 evicted or found no room
// (a draw from frame retry_at on queues it again). last_used: the frame of the last draw that wanted it. Eviction and
// use meet as in Dekker's algorithm (both sequentially consistent): a draw stores last_used, then reads state; the
// worker moves state 1 -> 3, then reads last_used and backs out if it is recent. So either the draw sees 3 and keeps
// the N64 texture, or the worker sees the draw and leaves the replacement alone.
struct Entry
{
	std::atomic<int> state{0};
	std::atomic<uint32_t> last_used{0};
	std::atomic<uint32_t> retry_at{0};
	uint32_t offset = 0, size = 0; // offset: texels into the buffer, or x | y << 14 of the first texel in the image
	uint32_t span = 0;             // worker only: length in its ring (texels, or the image slot's width)
	bool mips = false;             // the image holds its mip chain (written before state 1)
	Key key{};
};
struct KeyHash
{
	size_t operator()(const Key &k) const { return size_t(k.crc * 0x9E3779B97F4A7C15ull) ^ (size_t(k.fmt) << 3) ^ k.siz; }
};
static std::unordered_map<Key, Entry, KeyHash> entries; // render thread only (pointers stay valid)

static std::thread worker;
static std::mutex lock;
static std::condition_variable wake;
static std::vector<std::pair<Key, Entry *>> queue;
static bool stopping;
static std::atomic<unsigned> loaded_count, loaded_kb, failed_count, evicted_count;
static bool full_logged;
static std::atomic<uint32_t> frame_no{1}; // render thread counts frames, the worker reads them
// Worker only. A ring of positions [0, len): the buffer, or one image shelf (x positions).
struct Ring
{
	size_t len = 0, cursor = 0;
	std::map<uint32_t, Entry *> resident; // ready replacements by position
};
struct Shelf
{
	uint32_t y, height;
	Ring ring;
};
static Ring buffer_ring;
static std::vector<Shelf> shelves; // image path
static uint32_t shelves_end;       // image rows below this belong to a shelf
static const uint32_t NEVER = UINT32_MAX;

static size_t texels_of(const Entry *e) { return size_t(e->size & 0xffffu) * (e->size >> 16); }

// Worker: take e out of the buffer unless a draw used it in the last `hot` frames (NEVER: leave everything).
static bool try_evict(Entry *e, uint32_t hot)
{
	if (frame_no.load() - e->last_used.load() < hot)
		return false; // recent: not even the brief state 3, which would make a draw skip it
	int one = 1;
	if (!e->state.compare_exchange_strong(one, 3))
		return false;
	if (frame_no.load() - e->last_used.load() < hot)
	{
		e->state.store(1);
		return false;
	}
	loaded_kb -= unsigned(texels_of(e) * 4 / 1024);
	evicted_count++;
	e->state.store(4);
	return true;
}

// Worker: room for n in ring r from its cursor on, evicting what is in the way and was not used in the last `hot`
// frames. Returns the position or SIZE_MAX after a full lap.
static size_t ring_place(Ring &r, size_t n, uint32_t hot)
{
	if (n > r.len)
		return SIZE_MAX;
	size_t travelled = 0;
	while (travelled <= r.len + n)
	{
		if (r.cursor + n > r.len)
		{
			travelled += r.len - r.cursor;
			r.cursor = 0;
		}
		const size_t end = r.cursor + n;
		// replacements overlapping [cursor, end): the one starting before cursor, then those starting inside
		auto it = r.resident.upper_bound(uint32_t(r.cursor));
		if (it != r.resident.begin())
		{
			auto prev = std::prev(it);
			if (prev->first + prev->second->span > r.cursor)
				it = prev;
		}
		bool blocked = false;
		while (it != r.resident.end() && it->first < end)
		{
			Entry *e = it->second;
			if (!try_evict(e, hot))
			{
				const size_t past = it->first + e->span;
				travelled += past - r.cursor;
				r.cursor = past;
				blocked = true;
				break;
			}
			it = r.resident.erase(it);
		}
		if (blocked)
			continue;
		const size_t at = r.cursor;
		r.cursor = end;
		return at;
	}
	return SIZE_MAX;
}

// Worker, buffer path: n texels. Returns the offset or SIZE_MAX.
static size_t place(size_t n)
{
	for (uint32_t hot : { 600u, 120u, 8u })
	{
		const size_t at = ring_place(buffer_ring, n, hot);
		if (at != SIZE_MAX)
			return at;
	}
	return SIZE_MAX;
}

// Worker, image path: a slot of sw x sh texels (replacement plus gutter). Free room on a shelf of its height class
// first, then a new shelf, then eviction on shelves of its class and then taller ones. Returns the shelf or -1.
static int place2d(uint32_t sw, uint32_t sh, uint32_t &x)
{
	uint32_t cls = 18;
	while (cls < sh)
		cls = (cls - 2) * 2 + 2;
	if (cls > image.height || sw > image.width)
		return -1;
	for (size_t i = 0; i < shelves.size(); i++)
		if (shelves[i].height == cls)
		{
			const size_t at = ring_place(shelves[i].ring, sw, NEVER);
			if (at != SIZE_MAX)
				return x = uint32_t(at), int(i);
		}
	if (shelves_end + cls <= image.height)
	{
		Shelf sh_new;
		sh_new.y = shelves_end;
		sh_new.height = cls;
		sh_new.ring.len = image.width;
		shelves.push_back(std::move(sh_new));
		shelves_end += cls;
		const size_t at = ring_place(shelves.back().ring, sw, NEVER);
		if (at != SIZE_MAX)
			return x = uint32_t(at), int(shelves.size() - 1);
	}
	for (uint32_t hot : { 600u, 120u, 8u })
		for (uint32_t c = cls; c <= 4098; c = (c - 2) * 2 + 2)
			for (size_t i = 0; i < shelves.size(); i++)
				if (shelves[i].height == c)
				{
					const size_t at = ring_place(shelves[i].ring, sw, hot);
					if (at != SIZE_MAX)
						return x = uint32_t(at), int(i);
				}
	return -1;
}

// Worker, image path: writes the w x h replacement with its first texel at (ox, oy) and the 1-texel gutter around it
// (the texels a repeating texture has there), then flushes those rows.
static void write2d(const uint32_t *src, uint32_t w, uint32_t h, uint32_t ox, uint32_t oy)
{
	uint8_t *base = reinterpret_cast<uint8_t *>(texels);
	for (int y = -1; y <= int(h); y++)
	{
		const uint32_t *srow = src + size_t((y + int(h)) % int(h)) * w;
		uint32_t *row = reinterpret_cast<uint32_t *>(base + size_t(int(oy) + y) * image.row_pitch) + ox;
		row[-1] = srow[w - 1];
		memcpy(row, srow, size_t(w) * 4);
		row[w] = srow[0];
	}
	const size_t first = size_t(oy - 1) * image.row_pitch;
	frontend->texpack_flush(first, size_t(h + 2) * image.row_pitch);
}

// Mip levels on the image path (N64_RDP_TEXPACK_MIPS=0 turns them off): level l is max(1, w >> l) x max(1, h >> l),
// down to 1 x 1, each with its gutter, left to right after the replacement on its shelf (texpack_level in texture.h).
// Each texel of a level averages the 2 x 2 below it (clamped at an odd edge), colour weighted by alpha so cut-out
// edges don't darken. Only replacements up to 2048 wide get them (the chain is about twice as wide).
static bool mips_on = true;
static uint32_t mip_levels(uint32_t w, uint32_t h)
{
	uint32_t n = 1;
	while ((w >> n) || (h >> n))
		n++;
	return n; // findMSB(max(w, h)) + 1
}
static uint32_t mip_chain_width(uint32_t w, uint32_t h)
{
	uint32_t sw = 0;
	for (uint32_t l = 0, n = mip_levels(w, h); l < n; l++)
		sw += std::max(1u, w >> l) + 2;
	return sw;
}
static void mip_down(const std::vector<uint32_t> &src, uint32_t w, uint32_t h, std::vector<uint32_t> &dst, uint32_t &dw, uint32_t &dh)
{
	dw = std::max(1u, w >> 1);
	dh = std::max(1u, h >> 1);
	dst.resize(size_t(dw) * dh);
	for (uint32_t y = 0; y < dh; y++)
		for (uint32_t x = 0; x < dw; x++)
		{
			uint32_t r = 0, g = 0, b = 0, a = 0;
			for (uint32_t j = 0; j < 2; j++)
				for (uint32_t i = 0; i < 2; i++)
				{
					const uint32_t c = src[size_t(std::min(2 * y + j, h - 1)) * w + std::min(2 * x + i, w - 1)];
					const uint32_t ca = c >> 24;
					r += (c & 0xff) * ca;
					g += ((c >> 8) & 0xff) * ca;
					b += ((c >> 16) & 0xff) * ca;
					a += ca;
				}
			uint32_t o = 0;
			if (a)
				o = ((r + a / 2) / a) | (((g + a / 2) / a) << 8) | (((b + a / 2) / a) << 16);
			dst[size_t(y) * dw + x] = o | (((a + 2) / 4) << 24);
		}
}

// what the renderer was last told per tile (present, offset, size, orig)
static uint32_t sent[8][4];
static unsigned frames, hits, pending_draws;

static void run()
{
	if (!texpack_ghq_init(dir.c_str(), rom.c_str()))
		log_cb(RETRO_LOG_WARN, "paraLLEl-RDP: texture packs: no pack for '%s' in %s.\n", rom.c_str(), dir.c_str());
	else
		log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: texture packs: '%s' from %s.\n", rom.c_str(), dir.c_str());
	std::vector<uint32_t> img;
	for (;;)
	{
		std::pair<Key, Entry *> job;
		{
			std::unique_lock<std::mutex> l(lock);
			wake.wait(l, [] { return stopping || !queue.empty(); });
			if (stopping)
				return;
			job = queue.back(); // newest first: what is on screen now
			queue.pop_back();
		}
		unsigned w = 0, h = 0;
		int state = 2;
		if (texpack_ghq_lookup(job.first.crc, job.first.fmt, job.first.siz, nullptr, img, w, h))
		{
			const size_t n = size_t(w) * h;
			size_t at = SIZE_MAX;
			uint32_t span = 0;
			Ring *ring = nullptr;
			if (w <= 4096 && h <= 4096)
			{
				if (image.width)
				{
					const bool mips = mips_on && w <= 2048 && h <= 2048 && (w > 1 || h > 1);
					const uint32_t sw = mips ? mip_chain_width(w, h) : w + 2;
					uint32_t x = 0;
					const int s = place2d(sw, h + 2, x);
					if (s >= 0)
					{
						const uint32_t ox = x + 1, oy = shelves[s].y + 1;
						write2d(img.data(), w, h, ox, oy);
						if (mips)
						{
							std::vector<uint32_t> cur = img, next;
							uint32_t cw = w, ch = h, lx = ox + w + 2;
							for (uint32_t l = 1, nl = mip_levels(w, h); l < nl; l++)
							{
								uint32_t nw, nh;
								mip_down(cur, cw, ch, next, nw, nh);
								write2d(next.data(), nw, nh, lx, oy);
								lx += nw + 2;
								cur.swap(next);
								cw = nw;
								ch = nh;
							}
						}
						job.second->mips = mips;
						at = ox | (oy << 14);
						span = sw;
						ring = &shelves[s].ring;
						ring->resident[x] = job.second;
					}
				}
				else if ((at = place(n)) != SIZE_MAX)
				{
					memcpy(texels + at, img.data(), n * 4);
					span = uint32_t(n);
					ring = &buffer_ring;
					ring->resident[uint32_t(at)] = job.second;
				}
			}
			if (at == SIZE_MAX)
			{
				if (w <= 4096 && h <= 4096 && !full_logged)
					log_cb(RETRO_LOG_WARN, "paraLLEl-RDP: texture packs: no room in the replacement buffer (%u MB) "
					       "for a %ux%u replacement: what is in use fills it; set N64_RDP_TEXPACK_MB higher.\n",
					       unsigned((capacity * 4) >> 20), w, h);
				if (w <= 4096 && h <= 4096)
				{
					// everything in the buffer is in use: try again in 2 s, when the scene may have moved on
					full_logged = true;
					job.second->retry_at.store(frame_no.load() + 120);
					state = 4;
				}
				failed_count++;
			}
			else
			{
				job.second->offset = uint32_t(at);
				job.second->size = w | (h << 16);
				job.second->span = span;
				state = 1;
				loaded_count++;
				loaded_kb += unsigned(n * 4 / 1024);
			}
		}
		// The texels are in the (coherent) buffer before a draw can see state 1, and that draw is submitted after.
		job.second->state.store(state, std::memory_order_release);
	}
}

static void stop()
{
	if (worker.joinable())
	{
		{
			std::lock_guard<std::mutex> l(lock);
			stopping = true;
		}
		wake.notify_all();
		worker.join();
	}
	stopping = false;
	queue.clear();
	buffer_ring = Ring();
	shelves.clear();
	shelves_end = 0;
	entries.clear(); // offsets are into the renderer's buffer, which goes with it
	enabled = false;
	texels = nullptr;
}

// Whether dir holds a pack for this ROM, named as GLideNHQ looks for it (':' and '/' in the name become '-'): the
// <name>_HIRESTEXTURES.hts cache or a <name> PNG folder. Without one the switch stays off for this game, so it keeps the
// stock shaders and skips the key hashing (both cost speed, master list 35).
static bool pack_exists(const char *d, const char *romname)
{
	std::string base = std::string(d) + "/" + romname;
	for (size_t i = strlen(d) + 1; i < base.size(); i++)
		if (base[i] == ':' || base[i] == '/')
			base[i] = '-';
	struct stat st;
	if (stat((base + "_HIRESTEXTURES.hts").c_str(), &st) == 0 && S_ISREG(st.st_mode))
		return true;
	return stat(base.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static void init(const char *romname)
{
	stop();
	const char *d = getenv("N64_RDP_TEXPACK");
	if (!d || !*d || !frontend)
		return;
	if (!pack_exists(d, romname))
	{
		log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: texture packs: no pack for '%s' in %s; off for this game.\n", romname, d);
		return;
	}
	size_t mb = 256;
	if (const char *e = getenv("N64_RDP_TEXPACK_MB"))
		mb = strtoul(e, nullptr, 0);
	{
		const char *m = getenv("N64_RDP_TEXPACK_MIPS");
		mips_on = !(m && m[0] == '0');
		const char *f = getenv("N64_RDP_TEXPACK_FMTSTATS");
		texpack_ghq_fmtstats(f && f[0] == '1');
	}
	size_t bytes = 0;
	texels = static_cast<uint32_t *>(frontend->enable_texpack(mb << 20, bytes, &image));
	if (!texels)
	{
		log_cb(RETRO_LOG_WARN, "paraLLEl-RDP: texture packs: could not make the replacement buffer.\n");
		return;
	}
	capacity = std::min(bytes / 4, size_t(1) << 28); // texel offsets are 28 bits in the tile
	if (image.width)
		log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: texture packs: replacement image %ux%u, mip levels %s.\n", image.width,
		       image.height, mips_on ? "on" : "off");
	buffer_ring = Ring();
	buffer_ring.len = capacity;
	shelves.clear();
	shelves_end = 0;
	loaded_count = 0;
	loaded_kb = 0;
	failed_count = 0;
	evicted_count = 0;
	full_logged = false;
	memset(sent, 0, sizeof(sent));
	dir = d;
	rom = romname;
	worker = std::thread(run);
	enabled = true;
}

// The entry for a key; the first time, queued for the worker.
static Entry *find(uint64_t crc, uint16_t fmt, uint16_t siz)
{
	Key k = { crc, fmt, siz };
	auto it = entries.find(k);
	if (it != entries.end())
		return &it->second;
	Entry *e = &entries[k];
	e->key = k;
	e->last_used.store(frame_no.load(std::memory_order_relaxed), std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> l(lock);
		queue.emplace_back(k, e);
	}
	wake.notify_one();
	return e;
}

// Before a textured draw: the draw's tiles get their replacement (when loaded), every other tile none.
static void apply(const uint32_t *tiles, unsigned count, Entry *const *e, const uint16_t (*orig)[2])
{
	uint32_t want[8][4] = {};
	for (unsigned t = 0; t < count; t++)
	{
		if (!e[t])
			continue;
		e[t]->last_used.store(frame_no.load(std::memory_order_relaxed)); // before reading state: see Entry
		int s = e[t]->state.load();
		if (s == 4 && int32_t(frame_no.load(std::memory_order_relaxed) - e[t]->retry_at.load(std::memory_order_relaxed)) >= 0 &&
		    e[t]->state.compare_exchange_strong(s, 0))
		{
			// evicted earlier: load it again
			{
				std::lock_guard<std::mutex> l(lock);
				queue.emplace_back(e[t]->key, e[t]);
			}
			wake.notify_one();
			s = 0;
		}
		if (s == 0)
			pending_draws++;
		if (s != 1 || orig[t][0] == 0 || orig[t][1] == 0 || orig[t][0] > 1024 || orig[t][1] > 1024)
			continue;
		if (t == 0)
			hits++;
		uint32_t *w = want[tiles[t] & 7];
		w[0] = e[t]->mips ? 3 : 1;
		w[1] = e[t]->offset;
		w[2] = e[t]->size;
		w[3] = orig[t][0] | (uint32_t(orig[t][1]) << 16);
	}
	for (unsigned i = 0; i < 8; i++)
	{
		if (memcmp(want[i], sent[i], sizeof(want[i])) == 0)
			continue;
		frontend->set_tile_replace(i, want[i][0] != 0, want[i][1], want[i][2], want[i][3], (want[i][0] & 2) != 0);
		memcpy(sent[i], want[i], sizeof(want[i]));
	}
}

static void on_frame(unsigned textured)
{
	frame_no.fetch_add(1, std::memory_order_relaxed);
	if (++frames < 600)
		return;
	log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: texture packs: %u loaded (%u MB of %u in use), %u failed, %u evicted; last "
	       "600 frames: %u of %u textured draws replaced, %u waiting.\n", loaded_count.load(), loaded_kb.load() >> 10,
	       unsigned((capacity * 4) >> 20), failed_count.load(), evicted_count.load(), hits, textured, pending_draws);
	frames = hits = pending_draws = 0;
	if (const char *f = getenv("N64_RDP_TEXPACK_FMTSTATS"))
		if (f[0] == '1')
		{
			char line[400];
			texpack_ghq_fmtstats_line(line, sizeof(line));
			log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: texture packs: formats: %s.\n", line);
		}
}
}

namespace TexKeys
{
static bool enabled;
static FILE *out;

// G_IM_SIZ_*, G_IM_FMT_*, LOADTYPE_*
enum { SIZ_4b = 0, SIZ_8b = 1, SIZ_16b = 2, SIZ_32b = 3 };
enum { FMT_CI = 2 };
enum { LOADTYPE_BLOCK = 0, LOADTYPE_TILE = 1 };

struct Tile
{
	uint32_t format, size, line, tmem, palette, cmt, cms, maskt, masks, originalMaskT, originalMaskS;
	uint32_t uls, ult, lrs, lrt, loadType, imageAddress;
	uint16_t loadWidth, loadHeight;
	uint32_t clamps() const { return (cms >> 1) & 1; }
	uint32_t clampt() const { return (cmt >> 1) & 1; }
};
struct LoadInfo
{
	uint8_t size, loadType;
	uint16_t uls, ult, lrs, lrt, width, height, texWidth;
	uint32_t texAddress, dxt, bytes;
};
static Tile tiles[8];
static LoadInfo loadInfo[512];
static uint16_t palette[512]; // gDP.TexFilterPalette
static uint32_t timg_format, timg_size, timg_width, timg_address, timg_bpl;
static uint32_t loadTileIdx;
static uint32_t textureLUT, cycleType, scissorLry;
static uint32_t gspTextureTile; // gSP.texture.tile: set by LLE triangles only
static uint32_t combine_w0, combine_w1;
static bool hackMK64;
static uint8_t const *mem;
static uint32_t memSize;

// colour images drawn this frame and last (framebuffer textures)
static uint32_t color_addr, color_width, color_size;
static vector<pair<uint32_t, uint32_t>> drawn_now, drawn_prev;

// Key record: what the PC needs to look it up and to sort the misses.
struct Rec
{
	uint64_t crc;
	uint16_t fmt, siz, w, h;
	uint8_t kind, fb;
	bool operator==(const Rec &o) const
	{
		return crc == o.crc && fmt == o.fmt && siz == o.siz && w == o.w && h == o.h && kind == o.kind && fb == o.fb;
	}
};
struct RecHash
{
	size_t operator()(const Rec &r) const
	{
		return size_t(r.crc * 0x9E3779B97F4A7C15ull) ^ (size_t(r.fmt) << 1) ^ (size_t(r.siz) << 5) ^ (size_t(r.w) << 9) ^
		       (size_t(r.h) << 20) ^ (size_t(r.kind) << 40) ^ (size_t(r.fb) << 48);
	}
};
static std::unordered_map<Rec, uint32_t, RecHash> counts;
// key log only: how each key's texture was found (L lines) and why draws got no key (F lines), to compare with GLideN64
static std::unordered_map<Rec, std::string, RecHash> detail; // kind 0, fb 0
static std::unordered_map<std::string, uint32_t> fails;
static unsigned frames, draws_tex, draws_nokey, lookups;
static uint64_t frame_total;

// cached keys per texture slot until the tile/TMEM state changes (GLideN64 only re-runs update() on CHANGED_TILE /
// CHANGED_TMEM; _calcTileSizes has side effects on loadInfo, so it must not run more often than GLideN64 runs it)
static bool dirty = true;
static bool have[2];
static Rec cached[2];
static uint32_t cached_tile[2];
static TexPack::Entry *cached_entry[2]; // texture packs: the pack entry for cached[t]

// ---- GLideNHQ TxUtil.cpp (C paths) ----
static uint32_t RiceCRC32(const uint8_t *src, int width, int height, int size, int rowStride)
{
	uint32_t crc32Ret = 0;
	const uint32_t bytesPerLine = width << size >> 1;
	int y = height - 1;
	do
	{
		uint32_t esi = 0;
		int x = int(bytesPerLine) - 4;
		do
		{
			memcpy(&esi, src + x, 4);
			esi ^= uint32_t(x);
			crc32Ret = (crc32Ret << 4) + ((crc32Ret >> 28) & 15);
			crc32Ret += esi;
			x -= 4;
		} while (x >= 0);
		esi ^= uint32_t(y);
		crc32Ret += esi;
		src += rowStride;
		--y;
	} while (y >= 0);
	return crc32Ret;
}

static uint8_t CalculateMaxCI8b(const uint8_t *src, uint32_t width, uint32_t height, uint32_t rowStride)
{
	uint8_t val = 0;
	for (uint32_t y = 0; y < height; ++y)
	{
		const uint8_t *buf = src + rowStride * y;
		for (uint32_t x = 0; x < width; ++x)
		{
			if (buf[x] > val)
				val = buf[x];
			if (val == 0xFF)
				return 0xFF;
		}
	}
	return val;
}

static uint8_t CalculateMaxCI4b(const uint8_t *src, uint32_t width, uint32_t height, uint32_t rowStride)
{
	uint8_t val = 0;
	width >>= 1;
	for (uint32_t y = 0; y < height; ++y)
	{
		const uint8_t *buf = src + rowStride * y;
		for (uint32_t x = 0; x < width; ++x)
		{
			uint8_t v1 = buf[x] >> 4, v2 = buf[x] & 0xF;
			if (v1 > val) val = v1;
			if (v2 > val) val = v2;
			if (val == 0xF)
				return 0xF;
		}
	}
	return val;
}

static uint64_t checksum64(const uint8_t *src, int width, int height, int size, int rowStride, const uint8_t *pal)
{
	uint64_t crc64Ret = 0;
	if (pal)
	{
		uint32_t crc32 = 0, cimax = 0;
		switch (size & 0xff)
		{
		case 1:
			crc32 = RiceCRC32(src, width, height, 1, rowStride);
			cimax = CalculateMaxCI8b(src, width, height, rowStride);
			crc64Ret = uint64_t(RiceCRC32(pal, cimax + 1, 1, 2, 512)) << 32 | crc32;
			break;
		case 0:
			crc32 = RiceCRC32(src, width, height, 0, rowStride);
			cimax = CalculateMaxCI4b(src, width, height, rowStride);
			crc64Ret = uint64_t(RiceCRC32(pal, cimax + 1, 1, 2, 32)) << 32 | crc32;
			break;
		}
	}
	if (!crc64Ret)
		crc64Ret = RiceCRC32(src, width, height, size, rowStride);
	return crc64Ret;
}

// ---- GLideN64 Textures.cpp ----
static const uint32_t sizeBytes[4] = { 0, 1, 2, 4 };
static uint32_t CalculateDXT(uint32_t txl2words) { return txl2words == 0 ? 1 : (2048 + txl2words - 1) / txl2words; }
static uint32_t Txl2Words(uint32_t width, uint32_t size)
{
	return size == 0 ? max(1u, width / 16) : max(1u, width * sizeBytes[size] / 8);
}
static uint32_t ReverseDXT(uint32_t val, uint32_t lrs, uint32_t width, uint32_t size)
{
	(void)lrs;
	if (val == 0x800) return 1;
	int low = 2047 / int(val);
	if (CalculateDXT(uint32_t(low)) > val) low++;
	int high = 2047 / int(val - 1);
	if (low == high) return uint32_t(low);
	for (int i = low; i <= high; i++)
		if (Txl2Words(width, size) == uint32_t(i))
			return uint32_t(i);
	return uint32_t((low + high) / 2);
}
// ImageFormat::tlp[textureLUT][size][format].maxTexels
static uint32_t maxTexels(uint32_t lut, uint32_t size, uint32_t format)
{
	if (size >= SIZ_32b) return 1024;
	if (size == SIZ_16b) return 2048;
	if (lut == 0) return size == SIZ_4b ? 8192 : 4096;
	if (format == 1) return size == SIZ_4b ? 8192 : 4096; // YUV
	return size == SIZ_4b ? 4096 : 2048;
}

struct TileSizes { uint32_t width, height, clampWidth, clampHeight, bytes; };

static void calcTileSizes(uint32_t ti, TileSizes &s, const uint32_t *rect_words)
{
	Tile &t = tiles[ti];
	Tile &lt = tiles[loadTileIdx];
	t.masks = t.originalMaskS;
	t.maskt = t.originalMaskT;
	uint32_t tileWidth = ((t.lrs - t.uls) & 0x03FF) + 1;
	uint32_t tileHeight = ((t.lrt - t.ult) & 0x03FF) + 1;
	const uint32_t tMemMask = textureLUT == 0 ? 0x1FF : 0xFF;
	LoadInfo &info = loadInfo[t.tmem & tMemMask];
	if (t.tmem == lt.tmem)
	{
		if (lt.loadWidth != 0 && lt.masks == 0)
			info.width = lt.loadWidth;
		if (lt.loadHeight != 0 && lt.maskt == 0)
		{
			info.height = lt.loadHeight;
			info.bytes = info.height * (lt.line << 3);
			if (lt.size == SIZ_32b)
				info.bytes *= 2;
		}
	}
	lt.loadWidth = lt.loadHeight = 0;
	s.bytes = info.bytes;
	if (tileWidth == 1 && tileHeight == 1 && cycleType == 2 && rect_words)
	{
		// RDP.w1 / RDP.w0 of the texrect: GLideN64's w0 is the command word, w1 the second word
		const uint32_t w0 = rect_words[0], w1 = rect_words[1];
		const uint32_t ulx = (w1 >> 14) & 0x3FF, uly = (w1 >> 2) & 0x3FF;
		const uint32_t lrx = (w0 >> 14) & 0x3FF, lry = (w0 >> 2) & 0x3FF;
		tileWidth = lrx - ulx + 1;
		tileHeight = lry - uly + 1;
	}
	uint32_t width = 0, height = 0;
	if (info.loadType == LOADTYPE_TILE)
	{
		width = min<uint32_t>(info.width, info.texWidth);
		if (width == 0) width = tileWidth;
		if (info.size > t.size) width <<= info.size - t.size;
		height = info.height;
		if (height == 0) height = tileHeight;
		if (hackMK64 && (height % 2) != 0) height--;
	}
	else
	{
		const uint32_t mt = maxTexels(textureLUT, t.size, t.format);
		int tile_width = int(t.lrs - t.uls + 1);
		int tile_height = int(t.lrt - t.ult + 1);
		int mask_width = t.masks == 0 ? tile_width : (1 << t.masks);
		int mask_height = t.maskt == 0 ? tile_height : (1 << t.maskt);
		if (t.clamps())
			width = uint32_t(min(mask_width, tile_width));
		else if (uint32_t(mask_width * mask_height) <= mt)
			width = uint32_t(mask_width);
		else
			width = tileWidth;
		if (t.clampt())
			height = uint32_t(min(mask_height, tile_height));
		else if (uint32_t(mask_width * mask_height) <= mt)
			height = uint32_t(mask_height);
		else
			height = tileHeight;
	}
	s.clampWidth = (t.clamps() && cycleType != 2) ? tileWidth : width;
	s.clampHeight = (t.clampt() && cycleType != 2) ? tileHeight : height;
	s.width = (info.loadType == LOADTYPE_TILE && t.clamps() != 0 && t.masks == 0) ? s.clampWidth : width;
	s.height = (info.loadType == LOADTYPE_TILE && t.clampt() != 0 && t.maskt == 0) ? s.clampHeight : height;
}

static bool in_framebuffer(uint32_t addr)
{
	for (auto &r : drawn_now)
		if (addr >= r.first && addr < r.second) return true;
	for (auto &r : drawn_prev)
		if (addr >= r.first && addr < r.second) return true;
	return false;
}

// _loadHiresTexture's key for texture slot t (tile index ti), or false where GLideN64 computes none.
static bool hiresKey(uint32_t t, uint32_t ti, const uint32_t *rect_words, Rec &rec)
{
	const Tile &tile = tiles[ti];
	TileSizes sz;
	calcTileSizes(ti, sz, rect_words);
	LoadInfo &info = loadInfo[tile.tmem & 511];
	char why[200];
	if (info.texAddress == 0)
	{
		if (out)
		{
			snprintf(why, sizeof(why), "noaddr t%u tile %u tmem %u fmt %u siz %u", t, ti, tile.tmem, tile.format, tile.size);
			fails[why]++;
		}
		return false;
	}
	int bpl, width, height;
	uint32_t addr = info.texAddress;
	if (info.loadType == LOADTYPE_TILE)
	{
		bpl = info.texWidth << info.size >> 1;
		addr += info.ult * bpl + (((info.uls << info.size) + 1) >> 1);
		width = min<int>(info.width, info.texWidth);
		if (info.size > tile.size) width <<= info.size - tile.size;
		height = info.height;
		if (hackMK64 && (height % 2) != 0) height--;
	}
	else
	{
		int tile_width = int(((tile.lrs - tile.uls) & 0x03FF) + 1);
		int tile_height = int(((tile.lrt - tile.ult) & 0x03FF) + 1);
		int mask_width = tile.masks == 0 ? tile_width : (1 << tile.masks);
		int mask_height = tile.maskt == 0 ? tile_height : (1 << tile.maskt);
		width = (tile.clamps() && tile_width <= 256) ? min(mask_width, tile_width) : mask_width;
		height = ((tile.clampt() && tile_height <= 256) || mask_height > 256) ? min(mask_height, tile_height) : mask_height;
		if (tile.size == SIZ_32b)
			bpl = int(tile.line << 4);
		else if (info.dxt == 0)
			bpl = int(tile.line << 3);
		else
		{
			uint32_t dxt = info.dxt;
			if (dxt > 1)
				dxt = ReverseDXT(dxt, info.width, sz.width, tile.size);
			bpl = int(dxt << 3);
		}
	}
	const uint8_t *pal = nullptr;
	if (tile.size < SIZ_16b && (textureLUT != 0 || tile.format == FMT_CI))
		pal = reinterpret_cast<const uint8_t *>(tile.size == SIZ_8b ? palette : palette + (tile.palette << 4));
	// GLideN64 reads RDRAM unchecked; skip what would run past it (and degenerate sizes the CRC can't take)
	// (a line under 4 bytes still reads 4 bytes from bytesPerLine - 4, before the line, as GLideNHQ does)
	const int bytesPerLine = width << tile.size >> 1;
	const int64_t first = int64_t(addr) + min(0, bytesPerLine - 4);
	if (width <= 0 || height <= 0 || bpl < 0 || first < 0 ||
	    uint64_t(addr) + uint64_t(bpl) * uint64_t(height - 1) + uint64_t(max(bytesPerLine, 4)) > memSize)
	{
		if (out)
		{
			snprintf(why, sizeof(why), "range t%u tile %u tmem %u fmt %u siz %u addr %06x w %d h %d bpl %d lt %u", t, ti,
			         tile.tmem, tile.format, tile.size, addr, width, height, bpl, unsigned(info.loadType));
			fails[why]++;
		}
		return false;
	}
	lookups++;
	rec.crc = checksum64(mem + addr, width, height, int(tile.size), bpl, pal);
	rec.fmt = uint16_t(tile.format);
	rec.siz = uint16_t(tile.size);
	rec.w = uint16_t(sz.width);
	rec.h = uint16_t(sz.height);
	rec.kind = 0;
	rec.fb = in_framebuffer(info.texAddress) ? 1 : 0;
	if (out)
	{
		Rec k = rec;
		k.fb = 0;
		if (detail.find(k) == detail.end())
		{
			snprintf(why, sizeof(why), "addr %06x w %d h %d bpl %d lt %u tile %u tmem %u pal %u uls %u ult %u lw %u lh %u tw %u lsz %u dxt %u",
			         addr, width, height, bpl, unsigned(info.loadType), ti, tile.tmem, tile.palette, info.uls, info.ult,
			         info.width, info.height, info.texWidth, unsigned(info.size), info.dxt);
			detail.emplace(k, why);
		}
	}
	(void)t;
	return true;
}

static bool uses_texel1(bool one_cycle)
{
	// color a/b/d = 2, c = 2 or 9 (TEXEL1_ALPHA); alpha a/b/c/d = 2. As GLideN64's Combiner_GetStages: 1-cycle mode
	// uses only the second cycle's equations, so TEXEL1 left in the first cycle does not make it update slot 1.
	const uint32_t w0 = combine_w0, w1 = combine_w1;
	const bool second = ((w0 >> 5) & 15) == 2 || ((w0 >> 0) & 31) == 2 || ((w0 >> 0) & 31) == 9 ||
	                    ((w1 >> 24) & 15) == 2 || ((w1 >> 21) & 7) == 2 || ((w1 >> 18) & 7) == 2 ||
	                    ((w1 >> 6) & 7) == 2 || ((w1 >> 3) & 7) == 2 || ((w1 >> 0) & 7) == 2;
	if (one_cycle || second)
		return second;
	return ((w0 >> 20) & 15) == 2 || ((w0 >> 15) & 31) == 2 || ((w0 >> 15) & 31) == 9 || ((w0 >> 12) & 7) == 2 ||
	       ((w0 >> 9) & 7) == 2 || ((w1 >> 28) & 15) == 2 || ((w1 >> 15) & 7) == 2 || ((w1 >> 12) & 7) == 2 ||
	       ((w1 >> 9) & 7) == 2;
}

static void count(const Rec &r) { counts[r]++; }

static void draw(uint32_t tile, uint8_t kind, const uint32_t *rect_words)
{
	draws_tex++;
	const bool two = cycleType <= 1 && uses_texel1(cycleType == 0); // combiner reads TEXEL1: GLideN64 updates slot 1
	const uint32_t ti[2] = { tile, (tile + 1) & 7 };
	for (uint32_t t = 0; t < (two ? 2u : 1u); t++)
	{
		if (dirty || !have[t] || cached_tile[t] != ti[t])
		{
			uint32_t use = ti[t];
			// update(): gSP.texture.tile == 7 && LoadBlock && same tmem -> slot 0 uses slot 1's tile
			if (t == 0 && gspTextureTile == 7 && use == loadTileIdx && tiles[loadTileIdx].loadType == LOADTYPE_BLOCK &&
			    tiles[ti[0]].tmem == tiles[ti[1]].tmem)
				use = ti[1];
			have[t] = hiresKey(t, use, rect_words, cached[t]);
			cached_tile[t] = ti[t];
			cached_entry[t] = (have[t] && TexPack::enabled && !cached[t].fb) ?
			                  TexPack::find(cached[t].crc, cached[t].fmt, cached[t].siz) : nullptr;
		}
		if (have[t] && out)
		{
			Rec r = cached[t];
			r.kind = uint8_t(kind | (t ? 8 : 0));
			count(r);
		}
		else if (t == 0 && !have[t])
			draws_nokey++;
	}
	dirty = false;
	if (TexPack::enabled)
	{
		TexPack::Entry *e[2] = { have[0] ? cached_entry[0] : nullptr, (two && have[1]) ? cached_entry[1] : nullptr };
		const uint16_t orig[2][2] = { { cached[0].w, cached[0].h }, { cached[1].w, cached[1].h } };
		TexPack::apply(ti, two ? 2u : 1u, e, orig);
	}
}

static void add_drawn()
{
	uint32_t stride = color_size ? (color_width << (color_size - 1)) : (color_width >> 1);
	uint32_t rows = scissorLry + 1;
	pair<uint32_t, uint32_t> r(color_addr, color_addr + rows * stride);
	for (auto &d : drawn_now)
		if (d == r) return;
	if (drawn_now.size() < 16)
		drawn_now.push_back(r);
}

static void track(RDP::Op op, const uint32_t *w)
{
	const uint32_t w0 = w[0], w1 = w[1];
	switch (op)
	{
	case RDP::Op::SetTextureImage:
		timg_format = (w0 >> 21) & 7;
		timg_size = (w0 >> 19) & 3;
		timg_width = (w0 & 0xFFF) + 1;
		timg_address = w1 & 0x00FFFFFF;
		timg_bpl = timg_width << timg_size >> 1;
		break;
	case RDP::Op::SetTile:
	{
		Tile &t = tiles[(w1 >> 24) & 7];
		t.format = (w0 >> 21) & 7;
		t.size = (w0 >> 19) & 3;
		t.line = (w0 >> 9) & 0x1FF;
		t.tmem = w0 & 0x1FF;
		t.palette = (w1 >> 20) & 15;
		t.cmt = (w1 >> 18) & 3;
		t.cms = (w1 >> 8) & 3;
		t.maskt = t.originalMaskT = (w1 >> 14) & 15;
		t.masks = t.originalMaskS = (w1 >> 4) & 15;
		if (!t.masks) t.cms |= 2;
		if (!t.maskt) t.cmt |= 2;
		dirty = true;
		break;
	}
	case RDP::Op::SetTileSize:
	{
		Tile &t = tiles[(w1 >> 24) & 7];
		t.uls = (w0 >> 14) & 0x3FF;
		t.ult = (w0 >> 2) & 0x3FF;
		t.lrs = (w1 >> 14) & 0x3FF;
		t.lrt = (w1 >> 2) & 0x3FF;
		dirty = true;
		break;
	}
	case RDP::Op::LoadTile:
	{
		const uint32_t ti = (w1 >> 24) & 7;
		Tile &t = tiles[ti];
		t.uls = (w0 >> 14) & 0x3FF;
		t.ult = (w0 >> 2) & 0x3FF;
		t.lrs = (w1 >> 14) & 0x3FF;
		t.lrt = (w1 >> 2) & 0x3FF;
		loadTileIdx = ti;
		t.loadType = LOADTYPE_TILE;
		t.imageAddress = timg_address;
		dirty = true;
		if (t.lrs < t.uls || t.lrt < t.ult)
			break;
		const uint32_t width = (t.lrs - t.uls + 1) & 0x03FF;
		const uint32_t height = (t.lrt - t.ult + 1) & 0x03FF;
		const uint32_t bpl = t.line << 3;
		LoadInfo &info = loadInfo[t.tmem];
		info.texAddress = t.imageAddress;
		info.uls = uint16_t(t.uls);
		info.ult = uint16_t(t.ult);
		info.lrs = uint16_t(t.lrs);
		info.lrt = uint16_t(t.lrt);
		info.width = uint16_t(t.masks != 0 ? min(width, 1u << t.masks) : width);
		info.height = uint16_t(t.maskt != 0 ? min(height, 1u << t.maskt) : height);
		info.texWidth = uint16_t(timg_width);
		info.size = uint8_t(timg_size);
		info.loadType = LOADTYPE_TILE;
		info.bytes = bpl * height;
		if (t.size == SIZ_32b)
			info.bytes *= 2;
		if (t.line == 0)
			break;
		if (t.masks == 0)
			t.loadWidth = max<uint16_t>(t.loadWidth, info.width);
		if (t.maskt == 0)
		{
			if (cycleType != 1 && t.tmem % t.line == 0)
				t.loadHeight = max<uint16_t>(t.loadHeight, uint16_t(info.height + t.tmem / t.line));
			else
				t.loadHeight = max<uint16_t>(t.loadHeight, info.height);
		}
		break;
	}
	case RDP::Op::LoadBlock:
	{
		const uint32_t ti = (w1 >> 24) & 7;
		const uint32_t uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF, lrs = (w1 >> 12) & 0xFFF, dxt = w1 & 0xFFF;
		Tile &t = tiles[ti];
		t.uls = (uls >> 2) & 0x3FF;
		t.ult = (ult >> 2) & 0x3FF;
		t.lrs = (lrs >> 2) & 0x3FF;
		t.lrt = (dxt >> 2) & 0x3FF;
		loadTileIdx = ti;
		t.loadType = LOADTYPE_BLOCK;
		t.imageAddress = timg_address;
		dirty = true;
		LoadInfo &info = loadInfo[t.tmem];
		info.texAddress = t.imageAddress;
		info.uls = uint16_t(t.uls);
		info.ult = uint16_t(t.ult);
		info.lrs = uint16_t(t.lrs);
		info.lrt = uint16_t(t.lrt);
		info.width = uint16_t(t.lrs);
		info.dxt = dxt;
		info.size = uint8_t(timg_size);
		info.loadType = LOADTYPE_BLOCK;
		const uint32_t width = (lrs - uls + 1) & 0x0FFF;
		uint32_t bytes = width << t.size >> 1;
		if ((bytes & 7) != 0)
			bytes = (bytes & ~7u) + 8;
		info.bytes = bytes;
		const uint32_t address = timg_address + ult * timg_bpl + (uls << timg_size >> 1);
		if (bytes == 0 || address + bytes > memSize)
			break;
		const uint32_t lo = t.tmem, hi = t.tmem + (bytes >> 3);
		for (uint32_t i = 0; i < ti; ++i)
			if (tiles[i].tmem >= lo && tiles[i].tmem < hi)
				loadInfo[tiles[i].tmem].loadType = LOADTYPE_BLOCK;
		break;
	}
	case RDP::Op::LoadTLut:
	{
		Tile &t = tiles[(w1 >> 24) & 7];
		t.uls = (w0 >> 14) & 0x3FF;
		t.ult = (w0 >> 2) & 0x3FF;
		t.lrs = (w1 >> 14) & 0x3FF;
		t.lrt = (w1 >> 2) & 0x3FF;
		dirty = true;
		if (t.tmem < 256)
			break;
		uint32_t count = uint16_t((t.lrs - t.uls + 1) * (t.lrt - t.ult + 1));
		const uint32_t start = t.tmem - 256;
		if (start + count > 512) count = 512 - start;
		if (uint64_t(timg_address) + count * 2 <= memSize)
			memcpy(palette + start, mem + timg_address, count * 2);
		break;
	}
	case RDP::Op::SetOtherModes:
		cycleType = (w0 >> 20) & 3;
		textureLUT = (w0 >> 14) & 3;
		break;
	case RDP::Op::SetCombine:
		combine_w0 = w0 & 0x00FFFFFF;
		combine_w1 = w1;
		break;
	case RDP::Op::SetScissor:
		scissorLry = (w1 & 0xFFF) >> 2;
		break;
	case RDP::Op::SetColorImage:
		color_addr = w1 & 0x00FFFFFF;
		color_size = (w0 >> 19) & 3;
		color_width = (w0 & 0x3FF) + 1;
		break;
	case RDP::Op::FillTriangle:
	case RDP::Op::ShadeTriangle:
	case RDP::Op::FillZBufferTriangle:
	case RDP::Op::ShadeZBufferTriangle:
	case RDP::Op::FillRectangle:
		add_drawn();
		break;
	case RDP::Op::TextureTriangle:
	case RDP::Op::ShadeTextureTriangle:
	case RDP::Op::TextureZBufferTriangle:
	case RDP::Op::ShadeTextureZBufferTriangle:
		add_drawn();
		gspTextureTile = (w0 >> 16) & 7;
		draw((w0 >> 16) & 7, 1, nullptr);
		break;
	case RDP::Op::TextureRectangle:
	case RDP::Op::TextureRectangleFlip:
		add_drawn();
		draw((w1 >> 24) & 7, uint8_t(cycleType == 2 ? 6 : 2), w);
		break;
	default:
		break;
	}
}

static void on_frame()
{
	frames++;
	frame_total++;
	drawn_prev.swap(drawn_now);
	drawn_now.clear();
	if (TexPack::enabled)
		TexPack::on_frame(draws_tex);
	if (!out)
	{
		if (frames >= 600)
			frames = draws_tex = draws_nokey = lookups = 0;
		return;
	}
	if (frames < 60)
		return;
	// window line, then one line per key record: K crc fmt siz w h kind fb count
	// kind bits: 1 triangle, 2 texrect, 4 copy mode, 8 second texture (TEXEL1)
	fprintf(out, "W %llu frames %u textured %u nokey %u keys %u lookups %u\n", (unsigned long long)frame_total, frames,
	        draws_tex, draws_nokey, unsigned(counts.size()), lookups);
	for (auto &kv : counts)
		fprintf(out, "K %016llx %u %u %u %u %u %u %u\n", (unsigned long long)kv.first.crc, kv.first.fmt, kv.first.siz,
		        kv.first.w, kv.first.h, kv.first.kind, kv.first.fb, kv.second);
	// L crc fmt siz w h : <how its texture was found>; F count <why a draw got no key>
	std::unordered_map<Rec, bool, RecHash> said;
	for (auto &kv : counts)
	{
		Rec k = kv.first;
		k.kind = 0;
		k.fb = 0;
		auto d = detail.find(k);
		if (d == detail.end() || !said.emplace(k, true).second)
			continue;
		fprintf(out, "L %016llx %u %u %u %u : %s\n", (unsigned long long)k.crc, k.fmt, k.siz, k.w, k.h, d->second.c_str());
	}
	for (auto &kv : fails)
		fprintf(out, "F %u %s\n", kv.second, kv.first.c_str());
	fails.clear();
	fflush(out);
	counts.clear();
	frames = draws_tex = draws_nokey = lookups = 0;
}

static void init(const uint8_t *rdram, uint32_t size)
{
	enabled = false;
	dirty = true;
	have[0] = have[1] = false;
	cached_entry[0] = cached_entry[1] = nullptr;
	const char *path = getenv("N64_RDP_TEXKEYS");
	if (path && *path && !out)
	{
		out = fopen(path, "w");
		if (!out)
			log_cb(RETRO_LOG_WARN, "paraLLEl-RDP: texture keys: can't open %s.\n", path);
	}
	mem = g_rdram_alias ? static_cast<const uint8_t *>(g_rdram_alias) : rdram;
	memSize = size;
	// GLideN64's hack_MK64 (RSP.cpp: ROM name "MARIOKART64"); header bytes may be word-swapped
	char a[21] = {}, b[21] = {};
	const uint8_t *hdr = GET_GFX_INFO(HEADER);
	for (int i = 0; i < 20 && hdr; i++)
	{
		a[i] = char(hdr[0x20 + i]);
		b[i] = char(hdr[(0x20 + i) ^ 3]);
	}
	hackMK64 = strstr(a, "MARIOKART64") || strstr(b, "MARIOKART64");
	// GLideN64's RSP.romname: the header name (b, byte order as RSP.cpp reads it) without trailing spaces
	char romname[21];
	memcpy(romname, b, sizeof(romname));
	for (int i = int(strlen(romname)) - 1; i >= 0 && romname[i] == ' '; i--)
		romname[i] = 0;
	TexPack::init(romname);
	if (out)
	{
		fprintf(out, "# texkeys v1 rom '%s' mk64hack %d\n", b, hackMK64 ? 1 : 0);
		log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: texture pack keys logged to %s.\n", path);
	}
	enabled = out != nullptr || TexPack::enabled;
}
}


/* Joey64 (run-ahead): 64-bit words of a display list received but not yet run (a command that arrived in part). */
int pending_commands()
{
	return cmd_ptr - cmd_cur;
}

void drop_pending_commands()
{
	cmd_ptr = 0;
	cmd_cur = 0;
}

/* Joey64 (run-ahead): keep or put back the RDP's own state with the run-ahead state (CommandProcessor::ra_state). */
void ra_state(bool restore)
{
	if (frontend)
		frontend->ra_state(restore);
}

void process_commands()
{
	const uint32_t DP_CURRENT = *GET_GFX_INFO(DPC_CURRENT_REG) & 0x00FFFFF8;
	const uint32_t DP_END = *GET_GFX_INFO(DPC_END_REG) & 0x00FFFFF8;
	// This works in parallel-n64, but not this repo for some reason.
	// Angrylion does not clear this bit here.
	//*GET_GFX_INFO(DPC_STATUS_REG) &= ~DP_STATUS_FREEZE;

	int length = DP_END - DP_CURRENT;
	if (length <= 0)
		return;

	length = unsigned(length) >> 3;
	if ((cmd_ptr + length) & ~(0x0003FFFF >> 3))
		return;

	uint32_t offset = DP_CURRENT;
	if (*GET_GFX_INFO(DPC_STATUS_REG) & DP_STATUS_XBUS_DMA)
	{
		do
		{
			offset &= 0xFF8;
			cmd_data[2 * cmd_ptr + 0] = *reinterpret_cast<const uint32_t *>(SP_DMEM + offset);
			cmd_data[2 * cmd_ptr + 1] = *reinterpret_cast<const uint32_t *>(SP_DMEM + offset + 4);
			offset += sizeof(uint64_t);
			cmd_ptr++;
		} while (--length > 0);
	}
	else
	{
		if (DP_END > 0x7ffffff || DP_CURRENT > 0x7ffffff)
		{
			return;
		}
		else
		{
			do
			{
				offset &= 0xFFFFF8;
				cmd_data[2 * cmd_ptr + 0] = *reinterpret_cast<const uint32_t *>(DRAM + offset);
				cmd_data[2 * cmd_ptr + 1] = *reinterpret_cast<const uint32_t *>(DRAM + offset + 4);
				offset += sizeof(uint64_t);
				cmd_ptr++;
			} while (--length > 0);
		}
	}

	while (cmd_cur - cmd_ptr < 0)
	{
		uint32_t w1 = cmd_data[2 * cmd_cur];
		uint32_t command = (w1 >> 24) & 63;
		int cmd_length = cmd_len_lut[command];

		if (cmd_ptr - cmd_cur - cmd_length < 0)
		{
			*GET_GFX_INFO(DPC_START_REG) = *GET_GFX_INFO(DPC_CURRENT_REG) = *GET_GFX_INFO(DPC_END_REG);
			return;
		}

		// before the command is queued: texture packs queue the draw's replacements ahead of it
		if (TexKeys::enabled)
			TexKeys::track(RDP::Op(command), &cmd_data[2 * cmd_cur]);
		if (command >= 8 && frontend)
			frontend->enqueue_command(cmd_length * 2, &cmd_data[2 * cmd_cur]);
		if (Smart::enabled && synchronous)
			Smart::track(RDP::Op(command), w1, cmd_data[2 * cmd_cur + 1]);
		if (Dup::enabled)
			Dup::track(RDP::Op(command), w1, cmd_data[2 * cmd_cur + 1]);

		if (RDP::Op(command) == RDP::Op::SyncFull)
		{
			// For synchronous RDP (smart sync waits only when the CPU touches what was drawn):
			if (Smart::enabled && synchronous && frontend)
				Smart::on_sync_full();
			else if (synchronous && frontend)
				frontend->wait_for_timeline(frontend->signal_timeline());
			*gfx_info.MI_INTR_REG |= DP_INTERRUPT;
			gfx_info.CheckInterrupts();
		}

		cmd_cur += cmd_length;
	}

	cmd_ptr = 0;
	cmd_cur = 0;
	*GET_GFX_INFO(DPC_START_REG) = *GET_GFX_INFO(DPC_CURRENT_REG) = *GET_GFX_INFO(DPC_END_REG);
}

static QueryPoolHandle refresh_begin_ts;

void profile_refresh_begin()
{
	if (device)
		refresh_begin_ts = device->write_calibrated_timestamp();
}

void profile_refresh_end()
{
	if (device)
	{
		device->register_time_interval("Emulation", refresh_begin_ts, device->write_calibrated_timestamp(), "refresh");
		refresh_begin_ts.reset();
	}
}

void begin_frame()
{
	unsigned mask = vulkan->get_sync_index_mask(vulkan->handle);
	unsigned num_frames = 0;
	for (unsigned i = 0; i < 32; i++)
		if (mask & (1u << i))
			num_frames = i + 1;

	if (num_frames != retro_images.size())
	{
		retro_images.resize(num_frames);
		retro_image_handles.resize(num_frames);
	}

	vulkan->wait_sync_index(vulkan->handle);
	if (!begin_ts)
		begin_ts = device->write_calibrated_timestamp();

	//frontend->wait_for_timeline(pending_timeline_value);
	//pending_timeline_value = timeline_value;
}

bool init()
{
	if (!context || !vulkan)
		return false;

	unsigned mask = vulkan->get_sync_index_mask(vulkan->handle);
	unsigned num_frames = 0;
	unsigned num_sync_frames = 0;
	for (unsigned i = 0; i < 32; i++)
	{
		if (mask & (1u << i))
		{
			num_frames = i + 1;
			num_sync_frames++;
		}
	}

	retro_images.resize(num_frames);
	retro_image_handles.resize(num_frames);

	device.reset(new Device);
	device->set_context(*context);
	load_pipeline_cache();
	device->init_frame_contexts(num_sync_frames);
	log_cb(RETRO_LOG_INFO, "Using %u sync frames for parallel-RDP.\n", num_sync_frames);
	device->set_queue_lock(
			[]() { vulkan->lock_queue(vulkan->handle); },
			[]() { vulkan->unlock_queue(vulkan->handle); });

	uintptr_t aligned_rdram = reinterpret_cast<uintptr_t>(gfx_info.RDRAM);
	uintptr_t offset = 0;

	// Smart sync without host import: paraLLEl-RDP's copies go through the second RDRAM mapping.
	if (Smart::requested() && g_rdram_alias && !device->get_device_features().supports_external_memory_host)
		aligned_rdram = reinterpret_cast<uintptr_t>(g_rdram_alias);

	if (device->get_device_features().supports_external_memory_host)
	{
		size_t align = device->get_device_features().host_memory_properties.minImportedHostPointerAlignment;
		offset = aligned_rdram & (align - 1);

		// Mupen64Plus allocates RDRAM on the heap, so this can be guaranteed to be 0.
		if (offset)
		{
			log_cb(RETRO_LOG_ERROR, "Host RDRAM is not aligned properly! Make sure to use align RDRAM to 64 KiB!\n");
			return false;
		}
		aligned_rdram -= offset;
	}
	else
		log_cb(RETRO_LOG_WARN, "VK_EXT_external_memory_host is not supported by this device. Application might run slower because of this.\n");

	unsigned rdram_size = 8 * 1024 * 1024;
	if (gfx_info.version >= 2 && gfx_info.RDRAM_SIZE)
		rdram_size = *gfx_info.RDRAM_SIZE;

	if (rdram_size == 0)
	{
		log_cb(RETRO_LOG_ERROR, "RDRAM size is 0, was graphics initialized too early?\n");
		return false;
	}

	frontend_rdram = aligned_rdram;
	frontend_offset = offset;
	frontend_rdram_size = rdram_size;
	if (!create_frontend())
		return false;
	build_pipelines_ahead();

	timeline_value = 0;
	pending_timeline_value = 0;
	width = 0;
	height = 0;
	Smart::init(gfx_info.RDRAM, rdram_size, device->get_device_features().supports_external_memory_host);
	// Repeat-frame skipping hashes RDRAM, which smart sync may have protected: only with its alias, or without it.
	const char *dup = getenv("N64_RDP_SKIP_DUP");
	Dup::enabled = !(dup && *dup == '0') && (!Smart::enabled || g_rdram_alias);
	return true;
}

static bool create_frontend()
{
	CommandProcessorFlags flags = 0;
	switch (upscaling)
	{
		case 2:
			flags |= COMMAND_PROCESSOR_FLAG_UPSCALING_2X_BIT;
			log_cb(RETRO_LOG_INFO, "Using 2x upscaling!\n");
			break;

		case 4:
			flags |= COMMAND_PROCESSOR_FLAG_UPSCALING_4X_BIT;
			log_cb(RETRO_LOG_INFO, "Using 4x upscaling!\n");
			break;

		case 8:
			flags |= COMMAND_PROCESSOR_FLAG_UPSCALING_8X_BIT;
			log_cb(RETRO_LOG_INFO, "Using 8x upscaling!\n");
			break;

		default:
			break;
	}

	if (upscaling > 1 && super_sampled_read_back)
		flags |= COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_READ_BACK_BIT;
	if (super_sampled_dither)
		flags |= COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;

	log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: Using RDRAM size of %u bytes.\n", frontend_rdram_size);
	frontend.reset(new CommandProcessor(*device, reinterpret_cast<void *>(frontend_rdram),
				frontend_offset, frontend_rdram_size, frontend_rdram_size / 2, flags));
	active_upscaling = upscaling;
	TexKeys::init(DRAM, frontend_rdram_size);

	if (!frontend->device_is_supported())
	{
		log_cb(RETRO_LOG_ERROR, "This device probably does not support 8/16-bit storage. Make sure you're using up-to-date drivers!\n");
		TexPack::stop();
		frontend.reset();
		return false;
	}

	RDP::Quirks quirks;
	quirks.set_native_texture_lod(native_texture_lod);
	quirks.set_native_resolution_tex_rect(native_tex_rect);
	frontend->set_quirks(quirks);
	return true;
}

// The upscaling option changed while running (fast-forward drops to 1x): finish all GPU work so RDRAM holds the
// latest results, then rebuild the CommandProcessor between frames. The first upscaled frames after a rebuild
// may show the previous picture until the game draws again.
static void apply_upscaling_change()
{
	if (!frontend || upscaling == active_upscaling)
		return;
	frontend->wait_for_timeline(frontend->signal_timeline());
	Smart::reset_after_drain();
	unsigned from = active_upscaling;
	TexPack::stop();
	frontend.reset();
	if (!create_frontend())
	{
		log_cb(RETRO_LOG_ERROR, "paraLLEl-RDP: switching upscaling %ux -> %ux failed.\n", from, upscaling);
		return;
	}
	timeline_value = 0;
	pending_timeline_value = 0;
	log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: upscaling %ux -> %ux while running.\n", from, upscaling);
}

void deinit()
{
	Smart::deinit();
	save_pipeline_cache(); // waits for a periodic save still running, then the last one here, before the device goes
	begin_ts.reset();
	end_ts.reset();
	retro_image_handles.clear();
	retro_images.clear();
	TexPack::stop();
	texpack_ghq_shutdown(); // the worker is gone; the next ROM opens its own pack
	frontend.reset();
	device.reset();
	context.reset();
	timeline_trace_file.reset();
}

static void complete_frame_error()
{
	static const char error_tex[] =
		"ooooooooooooooooooooooooo"
		"ooXXXXXoooXXXXXoooXXXXXoo"
		"ooXXooooooXoooXoooXoooXoo"
		"ooXXXXXoooXXXXXoooXXXXXoo"
		"ooXXXXXoooXoXoooooXoXoooo"
		"ooXXooooooXooXooooXooXooo"
		"ooXXXXXoooXoooXoooXoooXoo"
		"ooooooooooooooooooooooooo";

	auto info = Vulkan::ImageCreateInfo::immutable_2d_image(50, 16, VK_FORMAT_R8G8B8A8_UNORM, false);
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	info.misc = IMAGE_MISC_MUTABLE_SRGB_BIT;

	Vulkan::ImageInitialData data = {};

	uint32_t tex_data[16][50];
	for (unsigned y = 0; y < 16; y++)
		for (unsigned x = 0; x < 50; x++)
			tex_data[y][x] = error_tex[25 * (y >> 1) + (x >> 1)] != 'o' ? 0xffffffffu : 0u;
	data.data = tex_data;
	auto image = device->create_image(info, &data);

	unsigned index = vulkan->get_sync_index(vulkan->handle);
	assert(index < retro_images.size());

	retro_images[index].image_view = image->get_view().get_view();
	retro_images[index].image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	retro_images[index].create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	retro_images[index].create_info.image = image->get_image();
	retro_images[index].create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	retro_images[index].create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	retro_images[index].create_info.subresourceRange.baseMipLevel = 0;
	retro_images[index].create_info.subresourceRange.baseArrayLayer = 0;
	retro_images[index].create_info.subresourceRange.levelCount = 1;
	retro_images[index].create_info.subresourceRange.layerCount = 1;
	retro_images[index].create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	retro_images[index].create_info.components.r = VK_COMPONENT_SWIZZLE_R;
	retro_images[index].create_info.components.g = VK_COMPONENT_SWIZZLE_G;
	retro_images[index].create_info.components.b = VK_COMPONENT_SWIZZLE_B;
	retro_images[index].create_info.components.a = VK_COMPONENT_SWIZZLE_A;

	vulkan->set_image(vulkan->handle, &retro_images[index], 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
	width = image->get_width();
	height = image->get_height();
	retro_image_handles[index] = image;

	device->flush_frame();
}

bool take_frame_valid()
{
	bool v = frame_valid;
	if (Dup::enabled)
		frame_valid = false;
	return v;
}

void complete_frame()
{
	if (!frontend)
	{
		complete_frame_error();
		device->next_frame_context();
		return;
	}

	timeline_value = frontend->signal_timeline();
	if (TexKeys::enabled)
		TexKeys::on_frame();

	// New pipelines show up as the game reaches new effects; check every ~10 s, save only when there are new ones.
	if (++frames_since_cache_check >= cache_check_interval)
	{
		frames_since_cache_check = 0;
		static const bool every = getenv("N64_RDP_CACHE_EVERY") && *getenv("N64_RDP_CACHE_EVERY") == '1';
		size_t built = device->get_pipelines_built();
		if ((every || built != pipelines_at_last_save) && save_pipeline_cache(true))
		{
			pipelines_at_last_save = built;
			if (!every && cache_check_interval < 4800)
				cache_check_interval *= 2;
		}
	}

	// PARALLEL_RDP_BENCH=1|2: GPU/CPU time per frame for each timestamp tag, every 300 frames
	static const bool bench = getenv("PARALLEL_RDP_BENCH") != nullptr;
	static unsigned bench_frames;
	if (bench && ++bench_frames >= 300)
	{
		bench_frames = 0;
		std::string line;
		device->timestamp_log([&](const std::string &tag, const Vulkan::TimestampIntervalReport &r) {
			char buf[96];
			snprintf(buf, sizeof(buf), " %s %.3f ms (x%.1f)", tag.c_str(),
			         1000.0 * r.time_per_frame_context, r.accumulations_per_frame_context);
			line += buf;
		});
		device->timestamp_log_reset();
		log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: bench per frame:%s\n", line.c_str());
	}

	if (Dup::enabled)
	{
		bool same = Dup::same_as_last();
		// Run-ahead's automatic native draws: did the shown extra frame show rows it drew itself?
		// N64_RA_NATIVE_DEBUG=1: count shown extra frames that drew what they showed, per 600, without turning off.
		static const bool ra_dbg = getenv("N64_RA_NATIVE_DEBUG") && getenv("N64_RA_NATIVE_DEBUG")[0] == '1';
		if (ra_dbg && (RaNative::cur_av & 9) == 9)
		{
			static unsigned shown, drew, first = ~0u;
			if (Dup::last_drew && first == ~0u)
				first = shown;
			drew += Dup::last_drew;
			if (++shown % 600 == 0)
			{
				log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: run-ahead debug: %u of the last 600 shown frames drew what they showed (first at shown frame %u)\n", drew, first);
				drew = 0;
			}
		}
		else if (RaNative::mode == 3 && (RaNative::cur_av & 9) == 9 && !RaNative::off && active_upscaling > 1)
		{
			if (RaNative::warm < 60)
				RaNative::warm++;
			else if (Dup::last_drew)
			{
				RaNative::off = true;
				log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: run-ahead: a shown frame drew what it showed (after %u that didn't): extra frames upscaled from now on%s\n",
				       RaNative::clean, RaNative::native_now ? "; it was drawn at native size, so the last picture is held for it" : "");
				if (RaNative::native_now)
				{
					same = true;              // not scanned out: the last picture stays
					Dup::have_last = false;   // and the next refresh is scanned out whatever it holds
				}
			}
			else if (RaNative::clean < 600 && ++RaNative::clean == 600)
				log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: run-ahead: 600 shown frames showed what the frame before drew: extra frames at native size from now on\n");
		}
		if (++(same ? Dup::skipped : Dup::scanned), Dup::skipped + Dup::scanned >= 600)
		{
			log_cb(RETRO_LOG_INFO, "paraLLEl-RDP: %u of the last 600 refreshes were repeats (not scanned out); new because: VI %u, drew %u (overflow %u), hash %u\n",
			       Dup::skipped, Dup::why_vi, Dup::why_drew, Dup::why_overflow, Dup::why_hash);
			Dup::skipped = Dup::scanned = Dup::why_vi = Dup::why_drew = Dup::why_overflow = Dup::why_hash = 0;
		}
		if (same)
			return;
	}
	frame_valid = true;

	frontend->set_vi_register(VIRegister::Control, *GET_GFX_INFO(VI_STATUS_REG));
	frontend->set_vi_register(VIRegister::Origin, *GET_GFX_INFO(VI_ORIGIN_REG));
	frontend->set_vi_register(VIRegister::Width, *GET_GFX_INFO(VI_WIDTH_REG));
	frontend->set_vi_register(VIRegister::Intr, *GET_GFX_INFO(VI_INTR_REG));
	frontend->set_vi_register(VIRegister::VCurrentLine, *GET_GFX_INFO(VI_V_CURRENT_LINE_REG));
	frontend->set_vi_register(VIRegister::Timing, *GET_GFX_INFO(VI_V_BURST_REG));
	frontend->set_vi_register(VIRegister::VSync, *GET_GFX_INFO(VI_V_SYNC_REG));
	frontend->set_vi_register(VIRegister::HSync, *GET_GFX_INFO(VI_H_SYNC_REG));
	frontend->set_vi_register(VIRegister::Leap, *GET_GFX_INFO(VI_LEAP_REG));
	frontend->set_vi_register(VIRegister::HStart, *GET_GFX_INFO(VI_H_START_REG));
	frontend->set_vi_register(VIRegister::VStart, *GET_GFX_INFO(VI_V_START_REG));
	frontend->set_vi_register(VIRegister::VBurst, *GET_GFX_INFO(VI_V_BURST_REG));
	frontend->set_vi_register(VIRegister::XScale, *GET_GFX_INFO(VI_X_SCALE_REG));
	frontend->set_vi_register(VIRegister::YScale, *GET_GFX_INFO(VI_Y_SCALE_REG));

	ScanoutOptions opts;
	opts.persist_frame_on_invalid_input = true;
	opts.vi.aa = vi_aa;
	opts.vi.scale = vi_scale;
	opts.vi.dither_filter = dither_filter;
	opts.vi.divot_filter = divot_filter;
	opts.vi.gamma_dither = gamma_dither;
	opts.blend_previous_frame = interlacing;
	opts.upscale_deinterlacing = !interlacing;
	opts.downscale_steps = downscaling_steps;
	opts.crop_overscan_pixels = overscan;
	const bool joey_ws = RDP::joey_waitstats_on();
	const auto joey_t0 = std::chrono::steady_clock::now();
	auto image = frontend->scanout(opts);
	if (joey_ws)
	{
		static uint64_t scan_ns = 0;
		static unsigned scans = 0;
		scan_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - joey_t0).count());
		if (++scans == 300)
		{
			const uint64_t drain = RDP::joey_drain_ns.exchange(0), tl = RDP::joey_timeline_ns.exchange(0);
			const unsigned waits = RDP::joey_timeline_waits.exchange(0);
			log_cb(RETRO_LOG_WARN, "paraLLEl-RDP: wait stats per scanout (300): scanout %.3f ms (drain %.3f ms), GPU timeline waits %.3f ms (%.2f waits).\n",
			       scan_ns / 300e6, drain / 300e6, tl / 300e6, waits / 300.0);
			scan_ns = 0;
			scans = 0;
		}
	}
	RDP::joey_gpu_spin();
	unsigned index = vulkan->get_sync_index(vulkan->handle);

	if (!image)
	{
		auto info = Vulkan::ImageCreateInfo::immutable_2d_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM);
		info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		info.misc = IMAGE_MISC_MUTABLE_SRGB_BIT;
		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		image = device->create_image(info);

		auto cmd = device->request_command_buffer();
		cmd->image_barrier(*image,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		cmd->clear_image(*image, {});
		cmd->image_barrier(*image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
		device->submit(cmd);
	}

	assert(index < retro_images.size());

	retro_images[index].image_view = image->get_view().get_view();
	retro_images[index].image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	retro_images[index].create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	retro_images[index].create_info.image = image->get_image();
	retro_images[index].create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	retro_images[index].create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	retro_images[index].create_info.subresourceRange.baseMipLevel = 0;
	retro_images[index].create_info.subresourceRange.baseArrayLayer = 0;
	retro_images[index].create_info.subresourceRange.levelCount = 1;
	retro_images[index].create_info.subresourceRange.layerCount = 1;
	retro_images[index].create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	retro_images[index].create_info.components.r = VK_COMPONENT_SWIZZLE_R;
	retro_images[index].create_info.components.g = VK_COMPONENT_SWIZZLE_G;
	retro_images[index].create_info.components.b = VK_COMPONENT_SWIZZLE_B;
	retro_images[index].create_info.components.a = VK_COMPONENT_SWIZZLE_A;

	vulkan->set_image(vulkan->handle, &retro_images[index], 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
	width = image->get_width();
	height = image->get_height();
	retro_image_handles[index] = image;

	end_ts = device->write_calibrated_timestamp();
	device->register_time_interval("Emulation", begin_ts, end_ts, "frame");
	begin_ts.reset();
	end_ts.reset();

	RDP::Quirks quirks;
	quirks.set_native_texture_lod(native_texture_lod);
	quirks.set_native_resolution_tex_rect(native_tex_rect);
	frontend->set_quirks(quirks);

	apply_upscaling_change();
	if (frontend)
		frontend->begin_frame_context();
}
}

bool parallel_create_device(struct retro_vulkan_context *frontend_context, VkInstance instance, VkPhysicalDevice gpu,
                            VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                            const char **required_device_extensions, unsigned num_required_device_extensions,
                            const char **required_device_layers, unsigned num_required_device_layers,
                            const VkPhysicalDeviceFeatures *required_features)
{
	if (!Vulkan::Context::init_loader(get_instance_proc_addr))
		return false;

	::RDP::context.reset(new Vulkan::Context);

	::Vulkan::Context::SystemHandles handles;

	if (const char *env = getenv("PARALLEL_RDP_TIMELINE_TRACE"))
	{
		::RDP::timeline_trace_file.reset(new Util::TimelineTraceFile(env));
		handles.timeline_trace_file = ::RDP::timeline_trace_file.get();
	}

	::RDP::context->set_system_handles(handles);
	// The frontend made the instance with our application info: tell Granite which version it asked for, but never above
	// what the loader supports. A Vulkan 1.1 loader accepts a 1.2 apiVersion and gives a 1.1 instance, while Granite then
	// refuses ("Vulkan loader does not support required Vulkan version"): every game failed to start on the Flip's
	// stock Mali-G52 (loader 1.1) with the 1.2 request (2026-11-09).
	VkApplicationInfo app_info = *parallel_get_application_info();
	uint32_t loader_version = volkGetInstanceVersion();
	if (loader_version < app_info.apiVersion)
		app_info.apiVersion = loader_version > VK_API_VERSION_1_1 ? loader_version : VK_API_VERSION_1_1;
	::RDP::context->set_application_info(&app_info);

	if (!::RDP::context->init_device_from_instance(
				instance, gpu, surface, required_device_extensions, num_required_device_extensions,
				required_features, 0))
	{
		::RDP::context.reset();
		return false;
	}

	frontend_context->gpu = ::RDP::context->get_gpu();
	frontend_context->device = ::RDP::context->get_device();
	frontend_context->queue = ::RDP::context->get_queue_info().queues[Vulkan::QUEUE_INDEX_GRAPHICS];
	frontend_context->queue_family_index = ::RDP::context->get_queue_info().family_indices[Vulkan::QUEUE_INDEX_GRAPHICS];
	frontend_context->presentation_queue = ::RDP::context->get_queue_info().queues[Vulkan::QUEUE_INDEX_GRAPHICS];
	frontend_context->presentation_queue_family_index = ::RDP::context->get_queue_info().family_indices[Vulkan::QUEUE_INDEX_GRAPHICS];

	// Frontend owns the device.
	::RDP::context->release_device();
	return true;
}

// Joey64: ask for a Vulkan 1.2 instance. Some drivers offer the 8-bit storage parallel-RDP needs only as a Vulkan 1.2
// core feature, without VK_KHR_8bit_storage (PurpleVK on the Adreno 650, Snapdragon 865); a 1.1 instance cannot
// use it. Granite still uses only what each GPU's own version allows. N64_VK11=1 asks for 1.1 as before.
static VkApplicationInfo parallel_app_info = {
	VK_STRUCTURE_TYPE_APPLICATION_INFO,
	nullptr,
	"paraLLEl-RDP",
	0,
	"Granite",
	0,
	VK_API_VERSION_1_2,
};

const VkApplicationInfo *parallel_get_application_info(void)
{
	const char *v11 = getenv("N64_VK11");
	parallel_app_info.apiVersion = (v11 && *v11 == '1') ? VK_API_VERSION_1_1 : VK_API_VERSION_1_2;
	return &parallel_app_info;
}

