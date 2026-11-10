#ifndef PARALLEL_RDP_HPP
#define PARALLEL_RDP_HPP

#include "vulkan_headers.hpp"

#include <libretro.h>
#include <libretro_vulkan.h>
#include <memory>
#include <vector>

#include "rdp_device.hpp"
#include "context.hpp"
#include "device.hpp"

namespace RDP
{
bool init();
void deinit();
void begin_frame();

void process_commands();
int pending_commands();
// Joey64 (run-ahead copy-on-write): smart sync tracks every RDRAM page the RDP draws; settle waits for what it drew
bool smart_on();
void smart_settle();
void set_hidden_frame(int av);
void drop_pending_commands();
void ra_primitive_base(bool restore);
extern const struct retro_hw_render_interface_vulkan *vulkan;

extern unsigned width;
extern unsigned height;
extern unsigned upscaling;
extern unsigned overscan;
extern unsigned downscaling_steps;
extern bool synchronous, divot_filter, gamma_dither, vi_aa, vi_scale, dither_filter, interlacing;
extern bool native_texture_lod, native_tex_rect, super_sampled_read_back, super_sampled_dither;
extern bool frame_valid;
bool take_frame_valid();

void complete_frame();
void deinit();

void profile_refresh_begin();
void profile_refresh_end();
}

#endif
