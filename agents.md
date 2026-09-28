# agents.md — GB4ME contributor guide + improvement roadmap

## Project overview

GB4ME is a Game Boy (DMG), Game Boy Color (CGB), and Game Boy Advance (GBA)
emulator in C++23 with SDL3 + Vulkan. GBA uses an internal HLE BIOS, no BIOS
file required.

* `src/core/`, `src/ppu/`, `src/apu/`, `src/cartridge/` — DMG/CGB core
  (`LR35902`, MMU, PPU `160x144`, APU 4ch, `MBC1/2/3/5`).
* `src/gba/` — GBA core (`ARM7TDMI ARM/Thumb`, bus + `WAITCNT`, PPU
  `240x160 modes 0-5`, DMA x4, timers, IRQ, HLE BIOS, `SRAM/Flash/EEPROM`,
  `GPIO/RTC`, audio FIFO).
* `src/ui/gameboy.cpp`, `src/ui/gui_console.cpp`, `src/ui/input.cpp` —
  frontend, ROM menu, settings, textured console skin.
* `src/vulkan/vulkan_renderer.cpp` (~2785 lines), `shaders/quad.*`,
  `shaders/gui.*` — Vulkan renderer + GLSL → SPIR-V via `glslc`.
* `src/selftest.cpp`, `src/main.cpp:dump_gba` — headless selftest +
  `--dump-gba` PPM/TXT diagnostics.
* Settings: `~/.config/GB4ME/settings.cfg` (`key=value`, 16 input profiles).
* Saves: `<rom>.sav` beside ROM, `GB4S v2` savestates (core only, not in UI).

## Build / run / verify

Requires CMake 3.28+, C++23 compiler, SDL3, Vulkan SDK, `glslc`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./bin/GB4ME                         # ROM menu
./bin/GB4ME game.gb|game.gbc|game.gba
./bin/GB4ME --list-roms <folder>
./bin/GB4ME --selftest [game.gba]
./bin/GB4ME --dump-gba game.gba 600 dump --input "120:A,240:S" --hold 6 --snap 60
```

Style: `.clang-format` (`IndentWidth 4, UseTab ForContinuationAndIndentation,
ColumnLimit 100, Linux braces`), `.editorconfig` (tabs, `lf`), `-Wall
-Wextra -Wpedantic -Werror`. `GB4ME_RELEASE=ON` strips selftest/dumps/logs;
`GB4ME_KEEP_SELFTEST=ON` keeps harness for verification builds.

Do NOT commit ROMs (`.gb/.gbc/.gba/.sav`) or `bin/`, `build*/` outputs.

Current state: `--selftest` 654/654 (656 with a GB/GBC boot smoke test, 657
with a GBA one), Blargg `cpu_instrs` 11/11, ASan+UBSan clean, `clang-format`
clean and `clang-tidy` clean (both are CI gates now). The per-item counts
quoted below are historical snapshots taken when that item landed.

## Improvement roadmap

### P0 — Correctness bugs (cheap, high value)

1. [DONE] `src/core/mmu.cpp`: serial IRQ used bit `8` on `u8 IF`, never
   fired. Now bit `3`. Verified: `--selftest` 444/444.
2. [DONE] `src/core/cpu.cpp`: `HALT` bug emulated via `halt_bug_` (PC fails
   to increment once when `HALT` runs with `IME=0` + pending IRQ).
   Verified: `--selftest` 444/444.
3. [DONE] Split `src/vulkan/vulkan_renderer.cpp` (was 2785 lines, now
   467 bootstrap): `src/vulkan/vulkan_internal.h` (shared `vulkan_detail`
   helpers), `vulkan_renderer_textures.cpp` (~400),
   `vulkan_renderer_pipelines.cpp` (~525), `vulkan_renderer_gui.cpp`
   (~825), `vulkan_renderer_draw.cpp` (~580). Moves byte-exact via
   Read/Write/Edit only, class interface unchanged; clean build +
   `--selftest` 467/467 green after every unit.
4. [DONE] Table-drive CB + ALU decoder in `src/core/cpu.cpp`: `kAlu[8]`
   / `kRot[8]` static constexpr member-function tables (same callees,
   same order) replace both `switch (y)` decoders; -52 lines, clean
   build, `--selftest` 491/491 + full Blargg `cpu_instrs` 11/11 green
   after the change.
   [DONE Blargg: `--blargg <rom> [frames]` headless runner (direct-boot
   bring-up mirroring the frontend, serial `Passed`/`Failed` verdict,
   exit 0/1) + CI `blargg` job over `cpu_instrs/individual/*.gb`.
   All 11 subtests pass locally (01-special … 11-op a,(hl)).]
5. [DONE — already ignored] Root ROMs are untracked + `.gitignore`d
   (`*.gb/*.gbc/*.gba`); `git ls-files` shows none tracked. Keep it so.
6. [DONE] `CMakeLists.txt`: auto-detect `ccache`. `Cartridge::last_error()`
   now names unsupported mappers (`MMM01/MBC6/MBC7/TAMA5/HuC1/HuC3` + hex)
   and surfaces it in `GameBoy::load_rom` logs.

### P1 — Emulation accuracy

GB/GBC:

* [DONE] Real `MBC3 RTC`: host wall-clock backed, latch `0->1`, halt +
  sticky day-carry, `<rom>.rtc` sidecar, 23 selftest checks. Remaining:
  `MBC1M`, `MBC30`, `MMM01`, `HuC1/3` still report clean `unsupported
  mapper` errors (no emulation).
* PPU (all remaining): `STAT` mode-2/0 glitches, the OAM-corruption bug,
  mid-scanline `SCX` and per-fetch FIFO/scroll timing. HDMA/OAM-DMA bus
  blocking is done (below); the rest need a per-dot renderer.
* [DONE] APU band-limiting + the two hardware quirks: the PSG now mixes 8x
  oversampled (352800Hz) and `dsp::OutputStage` (`include/gb/audio_dsp.h`)
  decimates it through a 65-tap Blackman-windowed sinc low-pass before the DC
  blocker and volume - that replaces the "sample at 44.1kHz, *68, one-pole
  0.996" hack whose square-wave harmonics and noise LFSR aliased. The mix
  clock is a 16.16 fixed-point accumulator (4194304/352800 = 11.888118 T,
  exact long-run rate; the old 95/96 dither was fine, the 11/12 one drifted
  0.4%). Wave RAM bus conflict (mGBA src/gb/io.c: while the channel plays, an
  access lands on the byte the DAC is fetching, and blanks to $FF within 4
  T-cycles of a fetch) and the `NR52` length hazard (a power cycle clears the
  registers but *not* the four length counters - SameBoy Core/apu.c restores
  them after `GB_apu_init`; games that power-cycle between notes depend on
  it). DAC power-down no longer steps to silence: a disabled channel keeps a
  linearly decaying copy of its last emitted level for 176 mix samples
  (~0.5ms), which is the click at the end of every note - the ramp starts
  from the last *emitted* level, not the live one, or a square wave's low
  phase would skip it. 14 new selftest checks. Still not modelled: SameBoy's
  retrigger corruption (needs PC-coupled cycle-accurate APU interleaving).
* [DONE] CGB HDMA bus blocking: an HDMA block owns the VRAM/OAM bus for its
  ~2us (32 dots), so CPU reads see FF and writes are dropped whatever the PPU
  mode is (`PPU::note_hdma_block`, timed off a frame dot counter; MMU reports
  each block). 8 new selftest checks. Still open, all needing a per-dot PPU
  instead of the per-scanline renderer: `STAT` mode-2/0 glitches, the OAM
  corruption bug, mid-scanline `SCX`, per-fetch scroll/FIFO timing.
* [DONE] `STOP` speed-switch 2050 M-cycle stall (Pan Docs KEY1,
  verified against SameBoy `sm83_cpu.c`): `CPU::speed_stall_` drained in
  `step()` (1 M per step, components advance normally), `DIV` frozen via
  `Timer::set_div_frozen` (`TIMA` frozen with it), cleared on `reset()`.
  6 new selftest checks (stall length, DIV/TIMA frozen, PPU advances,
  DIV resumes) + updated double-speed test to drain the stall.
* [DONE] CGB `DIV` double-speed rate: the divider runs at 2x wall-rate
  (Pan Docs KEY1 + SameBoy `apu_bit` compensation evidence). `Timer::
  set_double_speed` (8 T/M instead of 4) wired once in `MMU::
  do_speed_switch`, cleared on reset; APU untouched (own wall-clock
  counter, already rate-correct); frontend stepping unchanged. 6 new
  selftest checks (single baselines, 2x DIV/TIMA, switch-down restore).

GBA:

* [STARTED — PPU done] Promote `GbaScheduler` from stub to a real event queue
  (DMA/timers/PPU/audio/FIFO). The PPU's two internal points are events now:
  `GbaPpu::setScheduler()` + `armNextEvent()` schedule HBlank and line-end,
  and the event bodies (`onHBlankEvent`/`onLineEndEvent`) do what the old
  polling loop did, so HBlank lands on **exactly** dot 1008 and the line ends
  at 1232 instead of whenever the current step() slice happened to stop
  (GBATEK "Timing"). `GbaPpu::step()` no longer advances anything - the
  events own the dot counter - which is the pattern timers/DMA/audio will
  follow. Save states needed the matching fix: the queue holds type-erased
  callbacks and is not serialized, so `GameBoyAdvance::loadState()` calls
  `ppu_.rearmEvents()` *after* `scheduler_.load()`; the selftest caught that
  ordering (armed against the pre-load clock, the PPU stalled and no frame
  ever completed). 8 new selftest checks, including a save/reload/continue run
  compared against an uninterrupted one. The four GBA ROMs stay
  frame-identical to the pre-change baseline.
  [DONE second producer - the 2-cycle interrupt delay: GBATEK "GBA
  Interrupts" (and mGBA's `GBAIRQDelay`) say a raised IF bit reaches the CPU
  two cycles later, which was previously delivered in the same tick. IF stays
  live the instant a source raises it - software polls it right after - but
  `GbaIrq::raise()` now defers only the *delivery* through the scheduler, and
  the deferred callback re-checks IE/IME at its deadline so a flag masked (or
  IME cleared) in the meantime simply stays latched, exactly as before. With
  no scheduler the delay collapses to "now", so headless bring-up is
  unchanged. Save states need the flag serialized even though the callback is
  not: `GbaIrq::save` writes `delay_armed_` and `loadState()` calls
  `rearmPending()` after the clock is restored, or the in-flight interrupt is
  silently dropped. 11 new selftest checks (immediate IF, deferred delivery,
  masked-stays-latched, IME re-check, save/reload keeps it). The four GBA
  ROMs stay frame-identical to the pre-change baseline.
  [DONE third producer - timers: every prescaled increment is an event
  (`GbaTimers::armChannel`/`onTimerTick`, re-armed from `rearmEvents()` on
  load). A 1/64 timer now overflows on the exact tick - measured 4194304
  ticks for a 65536-increment period, i.e. 64 ticks each, where the old
  slice-polling code could land a whole instruction late. Cascade mode
  falls out of the same path (the increment happens inside the overflow
  event, and a cascade channel arms no event of its own - a check runs 2000
  steps past an overflow and confirms the cascaded counter does not move).
  Because `GbaScheduler` has no cancel, a pending event is invalidated by a
  per-channel generation counter instead: a prescaler/enable write bumps the
  generation so the stale event is a no-op. Known limitation, documented in
  `GameBoyAdvance::step()`: the scheduler clock only advances *between*
  instructions, so the FIRST event armed by a register write can be up to one
  instruction early - steady-state periods (PPU, timers, IRQ delay) are exact
  because they re-arm from inside their own callback. Fixing that needs the
  CPU in per-cycle slices, which the prefetch/`WAITCNT` item needs anyway.
  15 new selftest checks. The four GBA ROMs stay frame-identical to the
  pre-change baseline.
  Still open: DMA completion as a producer, audio FIFO; then what they enable
  - DMA latency, prefetch, per-cycle CPU slices.]
* Prefetch buffer + full `N/S` `WAITCNT` accounting; correct open-bus
  BIOS/palette/OAM values per mGBA suite.
* [DONE] GBA save hardware accuracy: Flash byte program is AND-only
  (`FlashSave::programByte`, GBATEK "Flash Memory" programming model - bits
  clear, only an erase restores 1s; mGBA assigns unconditionally), the erase
  command set is GBATEK's (`AA 55 10` chip, `AA 55 80 AA 55 30@sector`, shared
  `eraseChip`/`eraseSector`; the old `EraseConfirm` swallowed the repeated
  `55` and dropped the `30`, and `0x10` chip erase was not decoded at all),
  EEPROM write settle time (115000 core cycles ~ 6.5ms, mGBA
  `EEPROM_SETTLE_CYCLES`; a status read reports 0 = busy, re-armed per data
  bit, clocked from the scheduler through `GbaBus::cycle()`), and the GPIO RTC
  now honours game-written date/time (Boktai's date changer) by storing a
  host-clock offset - the emulated clock keeps running from the written time,
  and the offset is battery-backed in a `<rom>.rtc` sidecar like the MBC3
  clock. 22 new selftest checks; `--selftest` 534/534, Blargg 11/11, and the
  four GBA ROMs boot frame-identical to the pre-change baseline (`--dump-gba`
  A/B against a HEAD worktree).
* [PARTIAL] `Sound*`/`MultiBoot`/SIO: the BIOS sound-driver SWIs (1Ah-1Dh,
  20h-24h, 28h-29h) and `MultiBoot` (r0=1, failure) already report
  documented results, and the serial ports (0x04000120-0x12B) now answer
  "idle" reads instead of echoing the io shadow - a game polling its own
  status word can no longer believe a transfer happened - with a one-time
  dev log. 4 new selftest checks. Still open: a real SIO/multiplayer model
  and the BIOS sound driver itself (out of scope without a mixer).
* Wire `GB4S v2` savestates into UI; run `sameboy/mgba-test-suite +
  gba-tests` in `--selftest` CI.
  [NOT STARTED - sized and ordered, so the next person does not have to
  re-derive it. The GBA core already has `GB4S` v2 (`gba/state.h` +
  per-device `save(StateBuffer&)`/`load()`, with `GameBoyAdvance::loadState`
  re-arming event producers after `scheduler_.load()`). The GB/CGB core has
  NO state serialization at all, so this is one new subsystem, not a UI
  wiring job. Field census of the private members that need saving:
  cartridge 134 (every MBC's bank/latch/RTC fields), apu 85, ppu 52, mmu 22,
  cpu 8 (incl. `ei_delay_` and `halt_bug_`, both of which change observable
  behaviour if dropped), timer 8, joypad 5 - ~314 fields, so ~1-2h of
  mechanical work. Suggested order: `gb::StateBuffer` (mirror the ~50-line
  `gba/state.h` writer; duplicating it beats threading a gb->gba dependency
  through nine GBA files that are currently frame-verified), then
  timer/joypad/cpu, then mmu, then ppu, then apu, then cartridge RAM + MBC
  state last (it is the biggest and the easiest to get subtly wrong). Test
  the way the GBA does: run N frames, snapshot, run M more, snapshot, reload,
  run M more, and require byte-identical video AND audio - a single missed
  field shows up as a desync rather than a crash. Then the UI: F5/F8 slots
  beside the ROM. The GBA test-ROM runner is BLOCKED on the SIO model - the
  serial ports currently answer "idle" reads (`readSerialIdle`), so a GBA
  test ROM has no way to report `Passed`/`Failed` over the wire that
  `--blargg` already parses for GB. SIO first, runner second.]

### P2 — GUI / UX

Custom Vulkan immediate GUI (`GuiVertex`, `gb/font.h 8x8`, single
`assets/gbc_body.png`) is charming but expensive to extend:

* Adopt `Dear ImGui` (docking) on top of existing Vulkan pass for
  settings / ROM grid / remap / debugger. Keep `quad` pipeline for
  game + photo background. `Nuklear` as single-header fallback.
  [STARTED: `gb4me_imgui` vendored (see P4); `src/vulkan/
  vulkan_renderer_imgui.cpp` inits the SDL3+Vulkan backends against the
  app render pass (backend-owned descriptor pool, auto font upload),
  draws a display-only `GB4ME Debug` window (FPS/frame/ROM/core/volume/
  filter/tex) at the end of all 3 record paths, `F3` toggles
  (`GB4ME_DEBUG_OVERLAY=1` forces it on for screenshots). Best-effort:
  init failure no-ops, overlay never captures input. Screenshot-verified
  over the ROM menu + `--selftest` 467/467. Next: interactive panels.]
* Assets: `DMG + GBA + GBC` skins (not just purple CGB), dark/light theme,
  TTF (`Inter/Noto` via `stb_truetype`; `stb_image.h` already vendored),
  SVG icons (`Lucide/Phosphor`) replacing `OPEN` text + `*` gear hack.
  [DONE typeface: `src/stb_truetype.h` vendored; `src/vulkan/
  vulkan_renderer_text.cpp` bakes a shelf-packed SDF atlas (Cousine per
  build via CMake POST_BUILD, else system DejaVu/Noto, else 8x8 fallback)
  through its own `shaders/text.*` pipeline + per-frame text pass;
  monospace-grid layout preserved, natural advances via `measure_text`
  (all centering converted). Iterations driven by screenshots: coverage
  alpha blurred at 5px -> SDF; grid cells left rivers -> measured
  advances; long footer crushed ui_scale -> hint moved to subtitle.
  Follow-up polish: gear `*` sized to its button + measured centering;
  contrast audit (row values/tabs/labels to full white, mute floor
  `0xFF71718A` -> `0xFF8E8EA3`, footer/folder hints brightened).
  `--selftest` bake checks + screenshot-verified.]
* Shaders: add `slot-mask/grille/NTSC/xBRZ/LCD-ghosting` alongside existing
  `quad.frag:0-3 Nearest/Bilinear/Smooth/Scale2x-EPX` + LCD grid.
  [DONE slot-mask, grille, xBRZ: `VideoFilter::SlotMask` (rounded subpixel
  slots, smoothstep border, depth = the LCD Grid slider), `VideoFilter::Grille`
  (Trinitron RGB triads, weights normalised by the max so nothing clips or
  goes black at the stripe seams) and `VideoFilter::XBRZ` (the four reference
  blend kernels over the 3x3 neighbourhood with its per-channel equality
  tests, then a bilinear across the four sub-blocks - the 4x box-downsample
  without 16 fetches). All are plain smooth-bilinear when the slider is 0.
  All three screenshot-verified on Wario Land. Still open: `LCD-ghosting`
  (needs a persistence history buffer, not expressible in one pass), `NTSC`
  (needs a decision on how fake a composite simulation may be).]
* Features: recent-ROMs, `rom.png` boxart sidecar, search/filter,
  save slots `F1-F8 + thumbnails`, fast-forward (`Tab`), rewind buffer,
  turbo, cheats (`GameShark/ActionReplay`), volume OSD.
  [DONE per-game configs: a `<rom>.cfg` beside the ROM - the same
  key=value parser as `settings.cfg` - is applied over the global settings
  for that game only (`Settings::load_game_override`/`clear_game_override`,
  the display keys only: palette / hardware_mode / video_filter / lcd_grid /
  video_scale / volume / console_color; input bindings stay global because
  those are profiles the user switches on purpose). Two details that make it
  safe rather than surprising: the overlay remembers the global each key
  displaced, so the `settings.save("")` that `load_rom` does for the
  recent-ROMs list cannot promote one game's palette to everyone's default
  (verified in the selftest by reloading the written file), and opening the
  settings panel drops the overlay, so panel edits are global instead of
  being silently overwritten on the next load. 23 new selftest checks.]
  [DONE volume OSD: renderer-owned transient pill (`flash_volume`, 1.5 s,
  drawn above all overlay UI), flashed from `set_volume()` + `-`/`=`
  keys, `GB4ME_VOLUME_OSD=1` forces it for screenshots. 4 selftest
  checks (headless-safe clock logic) + screenshot-verified.]
  [DONE recent-ROMs ...]
  [DONE fast-forward: hold `Tab` (reserved, focus-loss safe) or
  `GB4ME_FASTFORWARD=1` skips frame pacing in menu + game loops;
  `>> FAST-FORWARD` + live FPS in the overlay. Verified live: ~959 FPS
  vs 59.6 paced. Audio crackle during FF is the standard tradeoff.]
* Input: rumble passthrough (`MBC5`/`Pak13`), WarioWare gyro, profile OSD.
  [DONE rumble: `MBC5::rumble_` records bit 3 of a 0x4000-0x5FFF write (the
  bit is stripped from the RAM bank, as before), `Cartridge::has_rumble()`/
  `rumble()` forward it, and the frontend drives the first rumble-capable
  pad with `SDL_RumbleGamepad` - stopping on shutdown and only ever from a
  cartridge that has a motor. 6 new selftest checks. Still open: WarioWare
  gyro (needs a motion source) and a profile-switch OSD.]

### P3 — Audio/video sync + perf

* [DONE] `SDL_DelayNS(1/60-elapsed)` + `~46ms` buffer (known crackle)
  replaced by audio-clock pacing (`include/gb/audio_sync.h`): the device
  buffer is the master clock and the video follows it. `AudioSync::pace()`
  measures the level as `queued - produced_this_frame` (otherwise the loop
  is always a frame stale and oscillates), never delays while the device is
  low, and otherwise waits only for the excess above a half-buffer target.
  A long-run measurement of how the level *moves* becomes a ±1% resample
  ratio: `dsp::OutputStage` does fractional decimation for the GB PSG, and
  `GbaAudio` does a fractional sample-hold (one sample per ~100 frames at
  1%, which the DC blocker smooths - never a dropped batch, which is what
  crackled). 11 new selftest checks. The menu loop keeps the nominal 60Hz
  floor (nothing is emulated there, so the buffer is not a clock).
* [DONE] `fps/mips/underrun` debug overlay: `Emu: 98.2% of hardware
  (4.11 MHz)` from a per-frame emulated-cycle counter, plus
  `Audio: N underruns, 23.3ms target, drift +0.14%` (the pacer's target
  level and live drift correction). Underrun counters come from the APU
  drop sites in both cores.
* Still open: cache static menu verts + sub-rect texture updates instead of
  full staging upload, and a `Tracy` zone option.

### P4 — Build / platform / maintainability

* `CMake`: `find_package(SDL3 CONFIG)`, `FetchContent` fallback, `vcpkg`
  manifest, `ccache`, `clang-tidy`, `ASan/UBSan` preset, CI
  (`linux/windows/macos`) running `--selftest`. [DONE SDL3 discovery +
  vcpkg: `find_package(SDL3 CONFIG)` first (`SDL3::SDL3`), `pkg-config`
  fallback, `-DGB4ME_SDL3_PKGCONFIG=ON` to force the latter; both paths
  build and pass the suite. `vcpkg.json` lists sdl3/vulkan/glslang/imgui
  plus a `gui-tests` feature (no license field: the tree has no LICENSE
  yet).]
  [DONE clang-tidy: `.clang-tidy` with a deliberately short, behaviour-focused
  check list and `WarningsAsErrors: '*'`, `CMAKE_EXPORT_COMPILE_COMMANDS=ON`
  so the tooling has a database, and a CI `clang-tidy` job running
  `run-clang-tidy -j$(nproc)`. The tree is clean. Fixing it found a REAL
  out-of-bounds: `vulkan_renderer_text.cpp` ranged a signed `char` against
  `kFontFirst + kFontCount` (127), so any byte >= 128 passed the check and
  indexed the glyph table negatively. Also fixed: 40 uninitialized
  declarations, dead constants, 8 branch clones (a dead DMG switch, BIOS
  halt/stop arms, GBA open-bus groups), and a `streamsize` narrowing in the
  save-file I/O. Checks left out are documented in `.clang-tidy` with the
  reason (signed chars are intentional in GB data, narrowing is the
  register-mask idiom, stdio/atoi are the dump path).]
  [STARTED: `ccache`
  auto-detect in `CMakeLists.txt`; `.github/workflows/ci.yml` builds
  + runs `--selftest` on Ubuntu; `cmake/SetupImGui.cmake` implements
  find-or-fetch for Dear ImGui (`GB4ME_USE_SYSTEM_IMGUI=ON` default:
  system `imgui::imgui` core + backends built from a tag derived from
  the system headers; `OFF`: full pinned fetch, default `v1.92.9`).
  Both paths build warning-free (`-Wno-error` scoped to the third-party
  target) and pass `--selftest` 467/467. `gb4me_imgui` static lib is
  linked (unused code is dropped); first panels come next.]
* Packaging: `flatpak`, `AppStream metainfo`, shader install to
  `/usr/share/GB4ME/shaders`.
* Replace custom `settings.cfg` parser with versioned schema; audit
  `GB4ME_*_TRACE` stderr helpers; enforce `clang-format` in CI.
  [DONE audit + gate: every `GB4ME_*` env hook is now `#ifndef
  GB4ME_RELEASE` (`GB4ME_FASTFORWARD` was the last stray), and README
  documents the whole list in one table. `CMakePresets.json` adds `dev`,
  `dev-asan` (ASan+UBSan on the first-party target only) and `dev-strict`,
  with `ctest --preset selftest[-asan]` running the suite; CI gained
  `format` (pinned clang-format 22.1.8, vendored stb_* excluded) and
  `sanitizers` jobs. The tree was formatted to `.clang-format` to make the
  gate pass (647 violations fixed, formatting only - `--selftest` and
  Blargg unchanged). ASan+UBSan is clean on the selftest (with leak
  detection), on the GB/CGB boot smoke tests and on two GBA ROM dumps.
  [DONE versioned settings: `Settings::kSchemaVersion` is written as a
  `schema=` line (with a comment header) and read back; an absent line
  means schema 1, a file from a newer build still applies the keys we
  understand, unknown keys are collected in `unknown_keys()` instead of
  being fatal, and `#`/`;` comment lines are skipped. 7 new selftest checks
  (round trip, legacy file, future schema, unknown key, old key order).]
  [DONE SDL3 `FetchContent` fallback: a third discovery path after
  `find_package` and `pkg-config`, opt-in with `-DGB4ME_SDL3_FETCH=ON` so a
  normal configure never reaches for the network. `SDL_SHARED`/`SDL_STATIC`
  are deliberately NOT forced - SDL gates them with `cmake_dependent_option`,
  which rewrites the cache entry, so forcing them made its `SDL3::SDL3` alias
  name a target that was never created (configure error). Pass
  `-DSDL_SHARED=OFF -DSDL_STATIC_DEFAULT=ON` for a static fetch. Verified by
  a full configure + SDL3 build + link of the fetched `release-3.2.24`.]
  [DONE discovery-path gate: a `cmake-discovery-paths` CI job configures
  all three SDL3 branches (find_package, forced pkg-config, forced fetch
  with `CMAKE_DISABLE_FIND_PACKAGE_SDL3` so the fetch branch is provably the
  one taken) - configure-only, so FetchContent costs a clone and not a
  build. All three verified locally, not just in the job.]
  Remaining: `clang-tidy` on the GBA/Vulkan TUs is now included, and
  packaging. Windows/macOS CI is deliberately NOT written yet: known
  blockers, in the order they will bite. (1) `glslc` is a hard
  `find_program(... REQUIRED)` and is a *build-time* need only - on Windows
  `vcpkg install shaderc` plus its `installed/x64/tools` on PATH, on macOS
  `brew install shaderc`; the Vulkan SDK is not on either image. (2) The
  vcpkg manifest path has to name the toolchain file, and current runner
  images export `VCPKG_INSTALLATION_ROOT`, not the older `VCPKG_ROOT` the
  docs use. (3) `find_package(Vulkan REQUIRED)` on macOS needs help: brew has
  no Vulkan SDK layout, so `Vulkan_INCLUDE_DIR`/`Vulkan_LIBRARY` (or
  `vulkan-headers` + `vulkan-loader` plus explicit hints) will have to be
  worked out. Prefer `-DGB4ME_USE_SYSTEM_IMGUI=OFF` in those jobs so the
  pinned fetch is what gets tested. Both jobs are unverified guesswork
  until someone can run them, and a permanently red CI blocks the whole
  pipeline - so they belong in their own PR, not appended here.]
