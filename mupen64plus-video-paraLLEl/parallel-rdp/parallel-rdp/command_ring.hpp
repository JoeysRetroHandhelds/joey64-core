/* Copyright (c) 2020 Themaister
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>

#ifdef PARALLEL_RDP_SHADER_DIR
#include "global_managers.hpp"
#endif

namespace RDP
{
class CommandProcessor;
class CommandRing
{
public:
	void init(
#ifdef PARALLEL_RDP_SHADER_DIR
			Granite::Global::GlobalManagersHandle global_handles,
#endif
			CommandProcessor *processor, unsigned count);
	~CommandRing();
	void drain();

	void enqueue_command(unsigned num_words, const uint32_t *words);
	// Wakes the worker if it sleeps: at the points where the queued commands must run soon.
	void kick();
	// The emulation thread is about to wait for the worker: wake eagerly for a while (see eager_until).
	void note_wait();

private:
	CommandProcessor *processor = nullptr;
	std::thread thr;
	std::mutex lock;
	std::condition_variable cond;

	// Single producer (the emulation thread), single consumer (the worker). The counters are
	// atomics, so neither side takes the lock unless it has to sleep: the ring empty (worker),
	// full (writer), or not yet completed (drain). A side about to sleep raises its flag before
	// checking again, and the other side, after publishing a counter, wakes it under the lock if
	// the flag is up (both sequentially consistent, so one of them always sees the other).
	std::vector<uint32_t> ring;
	std::atomic<uint64_t> write_count{0};
	std::atomic<uint64_t> read_count{0};
	std::atomic<uint64_t> completed_count{0};
	std::atomic<bool> reader_waiting{false};
	std::atomic<bool> writer_waiting{false};
	std::atomic<bool> drain_waiting{false};

	// Lazy waking (default; env N64_RDP_RING_LAZY=0 turns it off): the worker empties the ring faster than
	// the emulation thread fills it, so it slept after almost every command and each one woke it again
	// (Duo, SM64: ~20,000 wakeups a second, the emulation thread's 12-20% kernel time). Now a sleeping
	// worker is woken once lazy_words are queued or at a kick; its 0.5 ms timed wait covers the rest.
	bool lazy = true;
	static constexpr uint64_t lazy_words = 1024;
	// Adaptive (default; env N64_RDP_RING_ADAPT=0 turns it off): a game that waits on the worker (CPU reads of
	// RDP output: Pokemon Snap, OoT's lens flare check) pays for every command the worker has not started yet,
	// so lazy waking cost those games 3-15% on the Portal. After a wait the writer wakes eagerly until another
	// eager_words have been written (about two frames of a busy game), so only games that keep waiting stay eager.
	// Written and read on the emulation thread only.
	bool adapt = true;
	uint64_t eager_until = 0;
	static constexpr uint64_t eager_words = 1 << 16;

	void wake();
	void thread_loop();
	void teardown_thread();
#ifdef PARALLEL_RDP_SHADER_DIR
	Granite::Global::GlobalManagersHandle global_handles;
#endif
};
}
