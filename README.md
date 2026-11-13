# joey64-core

The N64 core of [Joey64](https://github.com/JoeysRetroHandhelds/joey64), an N64 emulator for Android handhelds:
a fork of [libretro/mupen64plus-libretro-nx](https://github.com/libretro/mupen64plus-libretro-nx) (Mupen64Plus-Next),
changed for Joey64's host and for Android handhelds' GPUs (Adreno and Mali).

**AI disclosure:** AI was used for all of this fork's changes. They are written with Anthropic's Claude through
Claude Code, which also runs most of the device testing. I decide what goes in and play it on real handhelds.

## What's changed from upstream

- **Lower lag:** the frame is scanned out at the start of the active picture, not at the vertical interrupt,
  so each frame reaches the screen about one frame (~16 ms) sooner.
- **parallel-RDP:**
  - updated to parallel-rdp-standalone master;
  - repeat frames skipped (the VI shows the same picture again);
  - "smart sync": async, with RDRAM pages the RDP drew into page-protected, so the CPU waits for the GPU only
    when it reads them (Pokémon Snap, Resident Evil 2 and Cruis'n USA draw right without full synchronous mode);
  - a lock-free command ring with lazy waking, pipeline cache kept between runs and saved off the emulation
    thread, upscaling changeable while running, and fixes for Adreno and Mali drivers (Vulkan 1.1 loaders,
    host query reset on the Retroid Pocket Duo);
  - reports the game's own border so the host can trim it.
- **parallel-RSP:** HLE audio with LLE graphics by default, AArch64 fast paths for vector multiplies and loads,
  and a stop for RSP programs that only poll RDRAM.
- **Recompiler (new_dynarec, arm64):** a jump cache, an optional compile thread, a per-game block list, code
  in cartridge ROM compiled, and a bad address raising an exception instead of `exit(1)`.
- **GLideN64:** updated to gonetz master, Pokémon Snap's photo check fixed on GLES, NEON math and ARMv8 CRC32 on
  Android. xBRZ is built out.
- **Netplay without SDL:** mupen64plus netplay on POSIX sockets, with Joey64's room additions (input resends,
  ROM check, keyed places, placing inputs where the room says, leaving a room at once).
- **Transfer Pak:** each port its own Game Boy game and save; cartridge banks mirrored and the MBC3 clock
  started at power on, as on hardware.
- **64DD:** disk-only start, disk saves next to the other saves, the IPL BIOS looked up by the disk's region.
- **Games and dumps upstream doesn't boot:** save type from the header for ROMs not in the database, PI DMA
  alignment as M64Plus FZ does (SM64 hacks), boot checksums computed for headers that don't carry them (Wii
  Virtual Console copies, prototypes), short dumps reading zeros past their end, a VI that runs with VI
  interrupts off (libdragon homebrew), and per-game CountPerOp entries.
- **Save states:** retro_serialize and retro_unserialize return at once instead of running a frame, and
  refuse states that are cut short or from another ROM.
- **Experiments, off unless the host turns them on:** run-ahead states, texture packs on parallel-RDP.
- angrylion-rdp-plus is removed (its old MAME licence is non-commercial).

## Branches and releases

`main` is the core Joey64 bundles. Each Joey64 release is tagged (`v0.2.0`, ...) at the core it shipped; the
app's `core-version.txt` names the exact commit (0.2.0's still says its old branch name, `rel3`: the same commit,
f6ce435).

## Building for Android (arm64)

`ndk-build` in `libretro/jni` (output `libretro/libs/arm64-v8a/libretro.so`); Joey64's `devtools/bundle-core.sh`
copies it into the app.

## Licence

GPLv2 or later, as upstream. GNU lightning (parallel-RSP's JIT) is LGPLv3+.

Upstream's README follows.

---

# Mupen64Plus-Next

Mupen64Plus-Next is a N64 emulation library for the [libretro API](http://www.libretro.com/), based on Mupen64Plus (see below).

It is also the successor of the old Mupen64Plus libretro core.

> You can *always* rely on it to give you an excellent Majora's Mask experience. Seriously.

#### How is this different from any N64 libretro-core, ever?

Due to the amount of libraries that are used and are in regular need of maintenance, I have strict rules about adding dependencies.  
This allows for easy maintenance, so available time can be spent on useful improvements and lowers the burden.  
By default the experience will be very simliar to the N64 emulators you know and love with *a lot extra*.

> **Sidenote:**  
While I accept pretty much every reasonable contribution, hacks must not impact behavior by default, unless justified.  
If you need to add a dependency, please consult me first.  
Force-pushes on all branches but `develop` and `master` are fair game.
`master` has the best stability memes, if that's your *thing*.

#### Used Technologies

The following projects have been incorporated into this repository:

- [mupen64plus](https://github.com/mupen64plus/mupen64plus-core)
- [GLideN64](https://github.com/gonetz/GLideN64)
- [cxd4](https://github.com/cxd4/rsp)
- [parallel-rsp](https://github.com/Themaister/parallel-rsp)
- [angrylion-rdp-plus](https://github.com/ata4/angrylion-rdp-plus) (Currently based on it's [ParaLLel](https://github.com/libretro/parallel-n64/) variant) (upstream; not in joey64-core): removed from Joey64's core (2026-11-07), its old MAME licence is non-commercial

#### Acknowledgments

A special thanks to:

- The Mupen64Plus Team, especially Gillou68310
- gonetz and those that have worked on GLideN64, especially fzurita
- The Authors of cxd4 and angrylion-rdp-plus (ata4)
- themaister for parallel-rsp and parallel-rdp (including the Vulkan integration)
- Everyone in the libretro Team


\- m4xw
