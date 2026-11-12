# joey64-core

The N64 core of [Joey64](https://github.com/JoeysRetroHandhelds/joey64), an N64 emulator for Android handhelds:
a fork of [libretro/mupen64plus-libretro-nx](https://github.com/libretro/mupen64plus-libretro-nx) (Mupen64Plus-Next)
with changes for Joey64's host: per-port Transfer Paks, netplay, parallel-RDP
updates and Mali/Adreno fixes, and fixes for games and dumps upstream doesn't boot.

- Branches: the release branches (`rel1`, `rel2`, `rel3`, ...) are what Joey64 bundles; the app's
  `core-version.txt` names the exact commit. Other branches are work in progress.
- Build for Android (arm64): `ndk-build` in `libretro/jni` (output `libretro/libs/arm64-v8a/libretro.so`).
- Licence: GPLv2 or later, as upstream; GNU lightning (parallel-RSP's JIT) is LGPLv3+. angrylion-rdp-plus is
  not part of this fork (removed 2026-11-07).

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
