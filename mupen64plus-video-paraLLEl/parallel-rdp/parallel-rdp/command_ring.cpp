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

#include <chrono>
#include "command_ring.hpp"
#include "rdp_device.hpp"
#include "thread_id.hpp"
#include <assert.h>

namespace RDP
{
void CommandRing::init(
#ifdef PARALLEL_RDP_SHADER_DIR
		Granite::Global::GlobalManagersHandle global_handles_,
#endif
		CommandProcessor *processor_, unsigned count)
{
	assert((count & (count - 1)) == 0);
	teardown_thread();
	processor = processor_;
	ring.resize(count);
	write_count = 0;
	read_count = 0;
#ifdef PARALLEL_RDP_SHADER_DIR
	global_handles = std::move(global_handles_);
#endif
	thr = std::thread(&CommandRing::thread_loop, this);
}

void CommandRing::teardown_thread()
{
	if (thr.joinable())
	{
		enqueue_command(0, nullptr);
		thr.join();
	}
}

CommandRing::~CommandRing()
{
	teardown_thread();
}

void CommandRing::drain()
{
	std::unique_lock<std::mutex> holder{lock};
	waiters++;
	cond.wait(holder, [this]() {
		return write_count == completed_count;
	});
	waiters--;
}

void CommandRing::enqueue_command(unsigned num_words, const uint32_t *words)
{
	std::unique_lock<std::mutex> holder{lock};
	waiters++;
	cond.wait(holder, [this, num_words]() {
		return write_count + num_words + 1 <= read_count + ring.size();
	});
	waiters--;

	size_t mask = ring.size() - 1;
	ring[write_count++ & mask] = num_words;
	for (unsigned i = 0; i < num_words; i++)
		ring[write_count++ & mask] = words[i];

	// Only wake the worker when it sleeps: a notify per command costs a futex syscall each
	// (thousands per frame), and the worker takes everything queued per wakeup anyway.
	if (reader_waiting)
		cond.notify_all();
}

void CommandRing::thread_loop()
{
	Util::register_thread_index(0);

#ifdef PARALLEL_RDP_SHADER_DIR
	// Here to let the RDP play nice with full Granite.
	// When we move to standalone Granite, we won't need to interact with global subsystems like this.
	Granite::Global::set_thread_context(*global_handles);
	global_handles.reset();
#endif

	std::vector<uint32_t> tmp_buffer;
	tmp_buffer.reserve(64);
	size_t mask = ring.size() - 1;

	for (;;)
	{
		bool is_idle = false;
		{
			std::unique_lock<std::mutex> holder{lock};
			reader_waiting = true;
			bool has_work = cond.wait_for(holder, std::chrono::microseconds(500), [this]() { return write_count > read_count; });
			reader_waiting = false;
			if (has_work)
			{
				// Take every queued command in one go.
				tmp_buffer.resize(write_count - read_count);
				for (auto &word : tmp_buffer)
					word = ring[read_count++ & mask];
				if (waiters)
					cond.notify_all(); // room for a writer waiting on a full ring
			}
			else
			{
				// If we don't receive commands at a steady pace,
				// notify rendering thread that we should probably kick some work.
				tmp_buffer.resize(1);
				tmp_buffer[0] = uint32_t(Op::MetaIdle) << 24;
				is_idle = true;
			}
		}

		if (is_idle)
		{
			processor->enqueue_command_direct(tmp_buffer.size(), tmp_buffer.data());
			continue;
		}

		// Each command is its word count followed by its words; a count of 0 stops the thread.
		bool stop = false;
		for (size_t offset = 0; offset < tmp_buffer.size();)
		{
			uint32_t num_words = tmp_buffer[offset++];
			if (num_words == 0)
			{
				stop = true;
				break;
			}
			processor->enqueue_command_direct(num_words, tmp_buffer.data() + offset);
			offset += num_words;
		}

		{
			std::lock_guard<std::mutex> holder{lock};
			completed_count = read_count;
			if (waiters)
				cond.notify_all();
		}

		if (stop)
			break;
	}
}
}
