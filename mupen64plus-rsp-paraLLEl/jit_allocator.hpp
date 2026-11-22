#pragma once

#include <vector>
#include <stdint.h>

namespace RSP
{
namespace JIT
{
class Allocator
{
public:
	Allocator() = default;
	~Allocator();
	void operator=(const Allocator &) = delete;
	Allocator(const Allocator &) = delete;

	void *allocate_code(size_t size);
	static bool commit_code(void *code, size_t size);
	// Gives back the unused tail of the most recent allocation (only effective when code is packed).
	void shrink_last(void *code, size_t used);
	// Pack code regions 64-byte aligned instead of one page each (Linux-like systems only; off by default).
	void set_pack(bool enable);

private:
	struct Block
	{
		uint8_t *code = nullptr;
		size_t size = 0;
		size_t offset = 0;
	};
	std::vector<Block> blocks;
	uint8_t *last_code = nullptr;
	size_t last_size = 0;
	bool pack = false;

	static Block reserve_block(size_t size);
};
}
}
