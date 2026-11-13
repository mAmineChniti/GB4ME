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
* Saves: `<rom>.sav` beside ROM, `GB4S v2` savestates wired into the UI
* (8 slots per game + rewind ring + menu thumbnails).

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
./bin/GB4ME --dump-gb game.gb 600 dump        # headless DMG/CGB PPM + serial
./bin/GB4ME --blargg test.gb                 # serial *and* cart-RAM verdicts
GB4ME_FORCE_DMG=1 ./bin/GB4ME --blargg oam_bug.gb   # DMG-only test ROMs
./bin/GB4ME --trace-gba game.gba 200          # boot triage: first PC outside
                                           # every legal GBA region + context
```

Style: `.clang-format` (`IndentWidth 4, UseTab ForContinuationAndIndentation,
ColumnLimit 100, Linux braces`), `.editorconfig` (tabs, `lf`), `-Wall
-Wextra -Wpedantic -Werror`. `GB4ME_RELEASE=ON` strips selftest/dumps/logs;
`GB4ME_KEEP_SELFTEST=ON` keeps harness for verification builds.

Do NOT commit ROMs (`.gb/.gbc/.gba/.sav`) or `bin/`, `build*/` outputs.

Current state: `--selftest` 1355/1355, Blargg `cpu_instrs` 11/11 +
`mem_timing` 6/6 + `instr_timing` + `halt_bug` + `interrupt_time` (the last
two only became reportable with the on-screen verdict reader), `oam_bug` 6/8,
`dmg_sound` 8/12, `cgb_sound` 8/12 (01-06, 08, 11), ASan+UBSan clean,
`clang-format` clean and `clang-tidy` clean (both are CI gates now). The
per-item counts quoted below are historical snapshots taken when that item
landed.

## Improvement roadmap

Every item carries a status tag: `[DONE]` landed and verified, `[PARTIAL]`
landed with a named remainder (listed right under the tag), `[STARTED]` in
flight, `[BLOCKED]` waiting on something outside the tree. The counts quoted
inside an item are the snapshot taken when it landed; the authoritative
totals are in "Current state" above.

### Open items (everything not `[DONE]` above, in dependency order)

1. **T-resolution stepping in the CPU hook** - the single enabling change
   behind most of the rest, and the reason the three bullets below are one
   project rather than three. It is the fix for `dmg_sound` 07 #5 + 09/10/12 -
   SameBoy passes 07 and 09 with the same chunked 4 T APU advance we use (see
   the wave-window entry), so whatever it does is expressible inside this
   refactor - and for the per-dot PPU and the conflict-register offsets. Today `CPU::tick()` advances timer/PPU/APU one whole
   M-cycle (4 T) at a time and every consumer that needs to know *where inside*
   the M-cycle something happened has to guess. Verified this session that the
   gap is real and shared:
   * **Wave RAM phase** (`dmg_sound` 07 #5, 09/10/12) needs the fetch grid to sit
     at an odd offset from the CPU's access grid - `09` walks the period 2 T per
     iteration precisely to walk them apart. Checked SameBoy's CPU access by
     access and ours already agree for reads *and* writes (SameBoy's first
     access in an instruction sees `pending_cycles == 0`, so both models read
     `LD A,(a16)` on M-cycle 4 of 4); the wave window, the write redirect and
     the just-read flag all match `wave_form_just_read` too. So nothing is
     misplaced - the APU simply cannot express a non-multiple-of-4 phase.
   * **Per-dot DMG PPU** - STAT mode-2/0 read glitches, mid-scanline `SCX`, the
     per-fetch FIFO: all "which dot did this land on".
   * **Conflict-register T-offsets** - SameBoy splits writes to STAT/palettes/
     `WX`/`LCDC`/`SCX`/`SCY`/`NR10` across the M-cycle boundary by advancing
     1 T and leaving `pending_cycles` at 3/5/6. Expressible only once an access
     can land off the grid.
   `oam_bug`'s three remaining subtests are *not* on this list: 1 needs the
   LCD-enable latency modelled separately from the corruption window, and 7/8
   need the hardware's exact byte blend, neither of which stepping supplies. P1.
2. **GBA per-cycle accounting** - prefetch *overlap* timing, open-bus latch
   values, video-contention stalls (`GBAMemoryStallVRAM` needs live dot
   phase). P1.
3. **BIOS sound-driver SWIs (1Ah-1Dh)** mixing, plus `Sound*`/SIO/MultiBoot.
   P1.
4. **TAMA5's TAMA6 MCU + RTC** - the mapper loads and its RAM/ROM banking works,
   but the command ranges that need the Tamagotchi's 4-bit firmware (`$40-$6F`,
   buzzer included) and the TC8521AM RTC (`$80-$BF`) are stubs. Needs the chip
   firmware or a real ROM of the one game that uses it. P1.
5. **External ROM suites in CI** - Blargg and the jsmolka gba-tests now run in
   the `conformance` job (green subtests gate; known failures grouped and
   expected-red). Missing: the `sameboy` and `mgba-emu/suite` ROM sets, which
   have to be assembled and need an ARM toolchain the runner does not have.
   SameBoy is also the natural second opinion on the item-1 failures: its
   README claims it passes all of Blargg's ROMs while its wave path matches
   ours cycle for cycle, which is unexplained and is the cheapest lead on
   09/10/12. Its automation page (sameboy.github.io/automation) only covers
   commercial games, not the test ROMs - but it does list "Cartridge type fd
   is not yet supported" (TAMA5), which we do implement. P1.
6. **ImGui panels** - ROM grid, remap, real docking, breakpoints. P2.
7. **DMG + GBA body-skin photos** and the wider SVG icon set. P2.
8. Static menu-vert cache / sub-rect uploads (measured as not worth it; only
   revisit if the per-frame upload shows up in a profile). P3.
9. Windows/macOS CI (blockers listed at the end of P4; needs someone who can
    run those images). P4.

### P0 — Correctness bugs (cheap, high value)

0. [DONE] **GBA regression hunt (commercial games stopped booting).** The
   uncommitted DMA/scheduler work made every GBA boot fail: Golden Sun and
   Megaman Battle Chip Change ran into open bus (`--dump-gba` showed the CPU
   parked at `00000004`/all-black). `--trace-gba <rom> <frames>` (new triage
   tool: reports the first PC outside every legal GBA region with the
   preceding PCs) plus a build of the last commit in a scratch worktree found
   three defects, all in the DMA/IO timing path:
   1. **Deferred DMA completion was observable.** The completion (Enable
      clear, repeat reload, IRQ) ran as a scheduler event *after* the block's
      bus latency, so between the transfer and the event the channel still
      looked enabled. Golden Sun reprograms DMA3 immediately after a transfer
      (SAD/DAD/CNT back to back); the `was`-enabled test swallowed the second
      block, its 5 KiB IWRAM payload stayed zeroed, and the CPU slid through
      `0x00000000` at the payload's entry point. Hardware stalls the CPU for
      the whole block, so any register *access* now settles the pending
      completion first (`GbaDma::read`/`write`) - Pokemon Emerald polls CNT_H
      right after its init DMA and expects Enable to read back cleared.
   2. **The block's time was never charged.** With the completion deferred,
      the transfer cost zero emulated cycles: the PPU/timers advanced but the
      CPU kept executing through cycles the hardware spends stalled. The
      transfer now charges `pending_cycles_` to the scheduler clock itself and
      applies the completion immediately, so the CPU sees the stall and the
      completion IRQ still lands at the same emulated instant.
   3. **ROM sequential waitstates carried a spurious access cycle.** GBATEK
      lists ROM at the default WS0 (N=4/S=2) as "5/2": nonsequential pays
      N waits *plus* the access cycle, sequential pays S alone. Adding the
      base cycle to both (as EWRAM's fixed 3/6 does) made every instruction
      two cycles longer - ~25 cycles/frame - and desynchronised games that
      sync on VCOUNT. EWRAM 3/6 and SRAM stay GBATEK-exact.
   Verified by booting the affected ROMs headless (`--dump-gba` colour count
   and PC): Golden Sun 14→190 colours with PC in ROM code, Megaman 13→39,
   `pokeemerald`/Emerald reach their game loop instead of open bus. jsmolka
   8/8, `--selftest` 1159/1159, ctest, cpu_instrs 11/11, mem_timing 6/6.
   Two selftest expectations changed with the model (ROM S8/S16 = 2, S32 = 4,
   N32 = 7; the DMA "enable stays set until the completion event" check now
   asserts the hardware-visible cleared state) and two checks were added for
   the reprogram-after-transfer pattern. Also: the SIO notice is now only
   printed when a transfer is actually *started* (SIOCNT bit 7 with SI mode)
   - games probe the port on every boot, and a warning about a port we
   deliberately answer was noise, not a fault.
4. [DONE] **The CPU prefetch queue survived PC/state changes** (the Emerald half of
   the same hunt). `fetchArm`/`fetchThumb` only refilled the queue when
   `pf_addr_[0] != instr_addr_`, which is right for straight-line code but
   wrong across a **Thumb -> ARM switch**: the queue holds *halfwords* in
   Thumb (padded into a 32-bit slot), and `BX` to an ARM address that happens
   to equal the queued halfword address passes the address check, so the ARM
   decoder ran on `0x0000xxxx` - usually an `andeq`, i.e. an instruction that
   quietly does nothing. A self-check (comparing every cached word against a
   fresh bus read) proved the values were otherwise correct, which is what
   pointed at the address check rather than staleness. Fixed in
   `Arm7Tdmi::setPc`/`setState`/exception entry: any PC or state change drops
   the queue, exactly like it already dropped `fetch_seq_`. Nothing else was
   needed - in particular stores and DMA blocks do **not** invalidate it,
   because that is the jsmolka "nes" test 1 behaviour (a store cannot rewrite
   an already-fetched instruction) and the prefetch buffer is a latch.
   Pokemon Emerald and `pokeemerald` now reach their game loop (23 colours vs a
   black screen, PC in ROM code) and Golden Sun/Megaman stay green.
   [Verified as a *progression*, not just a non-blank frame: dumping Emerald
   at 200/500/900 frames gives three distinct screens - solid white boot
   (1 colour), a 49-colour title screen whose readable glyph rows come from
   the font tiles, and a 20-colour intro scene - i.e. the game walks its boot
   state machine instead of hanging. Frame dumps: `--dump-gba
   pokeemerald.gba <frames> <prefix>`, then read the `.ppm` (one PPM per run,
   plus a `.txt` with the last PC/DISPCNT/WIN state).]
   5 new selftest checks, including the BX Thumb->ARM case and a straight-line
   fetch-ahead case that pins the queue as still working.

   How the hunt went, for the next one: `--trace-gba` found the boot crash
   (first PC outside every legal region), a scratch build of the last commit
   gave a working reference, and then narrowing was cheapest in *data* space -
   frames the two builds agreed on (SWI call counts, then per-frame memory
   fingerprints), which jumped straight from "the game boots differently" to
   "the queue hands the decoder a zero-padded halfword". Two dead ends worth
   remembering: the per-cycle scheduler ticking and the 2-cycle IRQ delivery
   delay were both innocent (disabling each changed nothing).

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
   now names unsupported mappers (`MBC6/MBC7/TAMA5` + hex)
   and surfaces it in `GameBoy::load_rom` logs.

### P0 — BIOS sound SWIs (fixed, guarded)

* [DONE] **`SWI` was a no-op, so any game using the BIOS sound driver wrote
  nothing to the APU.** `0xCF` fell through to the undefined-opcode default.
  `SWI 1Ah`-`1Dh` are now implemented: `A == 0` reads the channel's first
  register into `A`, `A > 0` writes `A` into `base + A - 1` (so `A` is both the
  index and the value), and an index past the channel's five registers is ignored
  rather than allowed to scribble.
  Implementation note that cost an attempt: `0xCF` is `x=3, z=3, q=1, y=3` in the
  SM83 table, and it is **not** under the `switch (y)` that holds `JP`/`DI`/`EI`.
  The first version was placed there and `bios_swi` was never called - proven by
  instrumenting it, not assumed. It is now handled at the top of `execute()`,
  before the decode tree.
  10 new selftests, with an oracle that compares a SWI write against a direct
  register write of the same value. That indirection is deliberate: readback
  masks differ per register (`NR10` reads back with bit 7 set, `NR32` masks most
  of its byte, `NR30` reads `0x7F | nr30`), and an earlier version that asserted
  literal values failed for exactly that reason.
* [DONE - regression found and fixed] `SWI` must be gated on a boot ROM being
  enabled. Executing `0xCF` as a BIOS call unconditionally turned
  **`cpu_instrs` 07 from ok to failing**, because that test executes `0xCF` with
  **no BIOS present** - where it is genuinely an undefined opcode. The handler
  now runs only when `mmu.boot_rom_enabled` is set, which is the case in real
  play; with no boot ROM `0xCF` is inert again. Both directions are covered by
  selftests (writes land with a boot ROM, nothing happens without one).
  Worth recording as a process note: this was initially reported as verified
  green when it was not - the first pass at verification did not catch it, and
  only re-running `cpu_instrs` after an unrelated change exposed it. Treat the
  conformance list as the thing to re-check after *any* CPU change.
* [REVERTED] The rest of the BIOS calls (memory copy/fill, sum, sqrt, FF00,
  APU status, RAM bank) were implemented and then backed out. They worked -
  `0Ch` memcopy, `0Eh` memfill (HL = destination, DE's low byte = value), `10h`
  sum, `13h`/`14h` floor sqrt, `1Fh` APU status all verified - but adding them
  coincided with the `cpu_instrs` 07 failure above and could not be cleanly
  separated from it at the time. Only the sound block is modelled. Two things
  learned while doing it, kept here because they cost real time:
  - `0Eh`/`0Fh` take **HL as the destination** and DE's low byte as the fill
    value. It is the opposite of `0Ch`/`0Dh`, which is an easy way to get it
    backwards.
  - Asserting literal register values in these tests is a trap: readback masks
    differ per register (`NR10` returns `nr10 | 0x80`, `NR30` returns
    `0x7F | nr30`, `NR32` is not readable at all). Compare against a direct
    register write, or capture the masked value first, instead.
* Still open: the rest of the BIOS SWI set (VBlank/LSTAT, timers, joypad,
  memcopy/memfill, math, RAM bank) is still unimplemented and still a silent
  no-op, so games relying on them are broken in the same way - but it must not
  be done as one undifferentiated batch, given what the above cost.

### P0 — Latency / audio-video pacing (fixed, guarded)

* [DONE] **GB audio was batched in 100 ms chunks, which is what made audio and
  video feel slow and delayed.** `APU::generate_sample` only handed samples to
  the device once `sample_buffer_` reached `kSampleRate / 10 * 2` - 100 ms of
  audio - and `flush_samples()` has no other caller. So SDL received one large
  burst ten times a second and **underran in between** (crackle), and because
  video is deliberately paced off the audio queue (`AudioSync::pace` treats the
  device backlog as the master clock) the picture stuttered in lockstep with it.
  The GBA path already flushed one frame (`kSampleRate / 60 * 2` in
  `src/gba/audio.cpp`); the GB path had simply never been brought in line. It
  now flushes one video frame too, bounding GB audio latency at ~16.7 ms instead
  of ~100 ms.
  Ruled out while diagnosing, so they are not re-chased: emulation speed is fine
  (600 frames in 0.65 s, ~920 fps, ~15x real-time); the renderer adds no latency
  (`max_frames_in_flight = 2`); and silence is fed correctly, because the
  resampler emits zero-valued frames continuously rather than going quiet.
* [DONE] Regression guard, and it is a real one: with no device `flush_samples()`
  clears the buffer instead of skipping it, so the bound is observable headlessly
  via `APU::buffered_samples()`. The check asserts the buffer never exceeds one
  frame's worth over a simulated second, sampled per frame. Reintroducing the
  `/ 10` threshold makes it fail; restoring `/ 60` makes it pass - verified both
  ways, so it cannot silently rot.
* Still open: confirming the *perceived* latency is gone needs a real audio
  device. Headless verification can only show the batching is frame-sized.

### Reference resources available in-tree

For anyone picking up the timing work, this is what is already here and what is
missing:
* `docs/references/sameboy` - full SameBoy 1.0.3 source, **buildable**: SDL2 is
  present, and `make tester` produces a headless `sameboy_tester`. Patched
  locally (in a scratch copy, never in-tree) it can dump Blargg's console text
  out of the tile map at `$9800` and log the wave channel per access. That rig is
  what produced the `09`/`12` ground truth. Two traps: log from `Core/apu.c`, not
  from the tester's read callback (that re-enters `GB_read_memory` and
  **core-dumps**), and `GB_apu_read` receives `addr & 0xFF` with
  `GB_IO_WAV_START == 0x30`. SameBoy ships **only boot ROM assembly**, no test
  ROMs.
* `docs/references/mgba` - the GBA reference: waitstate tables and
  `GBAMemoryStall`/`GBAMemoryStallVRAM` for the prefetch-overlap item, plus a
  `test/` directory.
* `docs/references/gbatek` - register/behavior documentation. Note the vendored
  copy does **not** carry the DMG/CGB BIOS SWI table (only NDS/GBA entries
  match), which is why the SWI semantics had to come from elsewhere.
* Not available: melonDS is a **DS** emulator, so it is not a reference for GB,
  CGB or GBA APU/PPU/bus timing. Nothing in it can settle the wave-phase or
  prefetch questions.

### P1 — Emulation accuracy

GB/GBC:

* [DONE] Real `MBC3 RTC`: host wall-clock backed, latch `0->1`, halt +
  sticky day-carry, `<rom>.rtc` sidecar, 23 selftest checks.
  [DONE MBC1M + MBC30: MBC1M multicarts detected by the bank-$10 Nintendo
  header twin (SameBoy heuristic) with 4-bit wiring (bit 4 ignored, high
  bits to 4-5, mode-1 pages $10/$20/$30, no RAM banking); MBC30 detected
  by size (>2MB ROM or >32KB RAM) with the full 8-bit ROM bank and RAM
  banks 0-7 (mGBA). Same change also fixed two standard-MBC1 deviations
  the rewrite exposed against the same references: raw 5-bit register
  storage (the $10-on-256KB trick maps bank 0) and high-bit combining on
  1MB+ carts.   21 new selftest checks on synthetic header-valid ROMs.
  [DONE HuC1: Hudson mapper (Pan Docs, SameBoy-confirmed, mGBA-consistent):
  no RAM gate ($0000 picks the RAM vs IR view), full-byte ROM/RAM banks, no
  00->01 translation, $6000 ignored. IR reads report no-light ($C0, no local
  source single-player); the TX LED latches. 13 new selftest checks.
  DONE HuC3: Hudson MBC+RTC (Pan Docs, SameBoy huc3 model): 7-bit ROM bank
  with no translation, 4-bit RAM bank, mode-selected $A000 window (0 = RAM
  read-only, $A = read/write, $B mailbox executing synchronously on write,
  $C response echo with the $6,2 status-1 rule, $D always-ready semaphore,
  $E IR), minute/day counters host-clock backed like MBC3 (lazy advance on
  read, rebase on game time-set, `<rom>.rtc` sidecar in a HuC3 format), tone
  writes accepted silently, open bus ($FF) elsewhere. Deviations noted in
  code: low-nybble mode match (Pan Docs), response latch over D7-open-bus
  (SameBoy-compat, games use the low nybble), clean 0/0 start instead of
  SameBoy's no-data sentinel. 25 new selftest checks (banking, modes, IR,
  mailbox minutes/days/alarms/status, sidecar, rejection).
  [DONE MMM01: multicart controller (Pan Docs, SameBoy write-shape):
  unmapped menu (last 32 KiB, header at size-32KiB), menu-driven lock (Pan
  Docs 2000/6000/4000/0000 order), MBC1 emulation with the masked $00->$01
  rule (Pan Docs $10/$30->$11 example), RAM banking with mode-0 lock,
  multiplex swap, RAM/mask write locks keeping menu game-select bits (Pan
  Docs over SameBoy's forcing form, which testing proved drops banking),
  save/load of the full register file. 21 new selftest checks incl. a menu
  lock flow, mode-0/1 RAM, multiplex on 8MB, and state round-trip. Every
  documented mapper now loads: ROM-only, MBC1/MBC1M, MBC2, MBC3/MBC30, MBC5,
  HuC1, HuC3, MMM01 (MBC6, MBC7 and TAMA5 landed later - see their items).
  Every cartridge type Pan Docs lists is now implemented; nothing reports the
  "unsupported mapper" error any more.]
 * PPU (all remaining): `STAT` mode-2/0 glitches, the OAM-corruption bug,
   mid-scanline `SCX` and per-fetch FIFO/scroll timing. HDMA/OAM-DMA bus
   blocking is done (below); the rest need a per-dot renderer.
   [DONE the STAT *interrupt line* (not the mode-2/0 read glitches, which do
   need a per-dot renderer): Pan Docs "STAT Interrupts" makes the interrupt a
   low->high edge on the OR of the enabled sources, and an edge that happens
   while IE bit 1 is clear is *blocked* - the next request needs the line to
   fall and rise again. We raised IF on every source event regardless of IE,
   so a game that masks STAT and enables it mid-frame took a stale interrupt
   for a mode it had already passed. `PPU::update_stat_line` now computes the
   line (LYC=LY, mode 1, mode 2, mode 0/HBlank), fires only on a rising edge
   with IE set, and is re-evaluated from `write_stat`, every mode transition,
   every LY/LYC update and the LCD-off path (which drops the level, so an
   LCD cycle cannot fake an edge). Serialized as `stat_line_`, hence
   `kStateVersion` 2 -> 3. 8 new selftest checks: blocked edge stays silent,
   enabling IE mid-frame fires nothing, the next edge does fire, LYC=LY as a
   source, "enabling a source while its mode is active" being itself an edge,
   and the LCD-cycle case.
   Testing gotcha worth remembering: the selftest blocks drive the PPU through
   a new `GbMachine::runPpuCycles` (PPU only, no CPU) because the harness has
   no cartridge, so `cpu.step()` runs an all-$FF stream which decodes as `EI`
   and then *services* the very interrupt under test - IF reads back cleared
   and the check looks like a missing edge. With the CPU stopped the same
   sequence reports the interrupt exactly.]
   [DONE the joypad half of that investigation: `Joypad::write_p1` now raises
   the joypad interrupt on the documented high->low transition of P10-P13, so
   *switching the select lines* while a button is held interrupts too - which
   is what a game polling the pad by flipping the two select bits every
   scanline actually sees on hardware (Pan Docs "Joypad Input"). 3 new
   selftest checks (selecting an unpressed group is silent, selecting the held
   button's group interrupts, a release raises nothing), and every GB
   conformance ROM still passes.]
   [RESOLVED - IF/IE semantics, and the answer is *not* what Pan Docs' prose
   says. Two bugs fixed, one closed question:
   (1) `read_ie` now returns `0xE0 | ie` (the unused upper bits read back as 1).
   Validated by halt_bug, whose result table prints IE read back: the expected
   CRC only matches with the upper bits set (the column reads E1/F1).
   (2) The `IF = $10` question: Pan Docs (and mGBA's prose) say IF is "write 1
   to clear", i.e. an AND-NOT. Implementing that made `cpu_instrs` drop to
   `02:02` and `halt_bug` fail, so it was reverted - and then *settled* by
   reading the reference implementation we already vendored:
   `docs/references/sameboy/Core/memory.c` writes IF in the plain-assignment
   group (`gb->io_registers[GB_IO_IF] = value;`, next to SCX/SCY/BGP/OBP/SB),
   and SameBoy passes Blargg's halt_bug in CI. halt_bug confirms it from our
   side too: it writes `IF=$10` to *set* the joypad flag and reads it back
   before halting, so under AND-NOT the read-back column would be E1 where the
   expected CRC has F1 - and its measured delays (0x0411 vs 0x0C04) flip with
   that. Our assign model reproduces the expected CRC exactly.
   So: keep the assignment, and treat "write 1 to clear" as prose we could not
   reproduce on any conformance ROM. Worth remembering that this looked like a
   hardware bug for a whole session - the way to settle it was the vendored
   SameBoy source plus halt_bug's printed table, not more reasoning.]

   [DONE slice 1 since, segmented scanline replay for mid-line scroll:
   every BGHOFS/BGVOFS write logs its dot (scheduler clock minus the
   line-start tick stamped in endLine), and renderScanline replays the
   line in segments (4 dots per pixel) instead of rendering everything
   with end-of-line values. Static lines take the old single-render path
   bit-identically (plus an overflow fallback to live rendering past 512
   writes/line); BGxCNT, affine origins, palette, windows and the OAM bug
   still apply per-line. Also fixed alongside: save-state rearm now
   resumes from the restored dot instead of full periods (the old
   behaviour shifted every later line/VBlank/frame boundary after a
   mid-line load - newly visible because the line clock is tick-absolute).
   6 new selftest checks (mid-line split position both sides, persistence
   to the next line, overflow fallback). jsmolka suites still all pass.]
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
   retrigger corruption (needs PC-coupled cycle-accurate APU interleaving -
   i.e. the per-cycle CPU project; parked behind it).
* [DONE] **OAM DMA takes the CPU off the bus for 160 M-cycles.** The copy was
  instant *and free*: `write_dma` filled OAM without charging anything, so every
  sprite-using game (they all start one DMA a frame) ran with 160 M-cycles more
  CPU time per frame than hardware gives it. `MMU::write($FF46)` now calls
  `CPU::add_dma_stall(160)` (Pan Docs "OAM DMA Transfer": 160 reads x 4 T), and
  `CPU::step` burns those M-cycles through the cycle hook without executing -
  and *without* freezing DIV, unlike the STOP speed switch (the timer and PPU
  keep running). The copy itself stays instant because DMA has its own bus; the
  bus blocking that also implies (only HRAM readable mid-DMA) is unobservable
  while the CPU is stalled, so it is left out and noted here.
  `MMU::set_cpu` is the new wiring (GameBoy + the selftest's GbMachine), and
  `dma_stall_` is serialized (`kStateVersion` 5 -> 6). 6 new selftest checks:
  the stall length, the immediate OAM fill, a planted program whose final store
  cannot land before the window ends, and DIV still ticking. All six GB
  conformance ROMs pass and DMG Acid2 renders pixel-identical to before, so the
   timing change does not disturb the PPU-visible output.
* [DONE] **MBC7 (tilt + EEPROM) is implemented** - the last mapper on the list
  that only lacked hardware. Cartridge type `$22` now loads as
  `MBCType::MBC7` with a full `MBC7` class (`include/gb/cartridge.h`,
  `src/cartridge/cartridge.cpp`): a 7-bit ROM bank with **no** bank-0
  translation, the two independent enables (`$0000=$0A` and `$4000=$40`), and a
  256-byte `$A000-$A0FF` window that is not RAM at all - `$A020`/`$A030` are the
  X latch low/high byte, `$A040`/`$A050` the Y pair, `$A060` reads 0, and `$A080`
  is the EEPROM's CS/DI/DO/CLK register (bit 7/1/0/6); unmapped window addresses
  read `$FF`. The tilt latch matches SameBoy's `write_mbc7_ram`: writing `$55`
  to `$A000` arms it and parks both axes at `$8000`, writing `$AA` to `$A010`
  samples the accelerometer, and level tilt is `$81D0`. The bit-banged 93LC56
  is a transcription of the same function's `case 8`, which is where the
  subtleties are: the command register is **11 bits shifted in MSB first** (the
  first bit shifted in ends up in the *high* position, so it is not a plain
  LSB-first shifter), bits 0-6 are the word address, bits 6-9 the 4-bit
  *extended* opcode (EWDS/WRAL/ERAL/EWEN/WRITE/READ/ERASE - the address's top
  bit and the opcode's low bit are the same bit, so the two fields overlap), and
  bit 10 means "command complete". Every clock drives DO from
  `read_bits >> 15` and shifts that register left with a 1 coming in, so a
  READ's own completing clock preloads the word and the *next* edge already
  drives bit 15 out. WRITE/WRAL zero the target word when the command lands and
  then OR in 16 **MSB-first** data bits. `has_battery()` is true; the register
  file, both latches and the whole 256-byte EEPROM go into the save file (as one
  "bank"). `set_accelerometer(x, y)` is the input seam: a no-op virtual on the
  `MBC` base (like `has_rumble`/`has_rtc`, so no RTTI is needed) that `Cartridge`
  forwards, which is where a frontend's tilt/gamepad plumbing will drive it.
  28 new selftest checks: type detection, the battery flag, banking with and
  without the translation, both enables, every window address, the tilt latch
  (including a non-`$55`/`$AA` write leaving it alone), a full EEPROM
  `EWEN -> WRITE $BEEF -> EWDS -> READ` round trip, `ERAL` filling the device
  with `$FFFF`, and a save/load round trip covering the ROM bank, both tilted
  latches and the EEPROM word. The frontend now drives it: `GameBoy::
  apply_tilt` feeds the same COMMA/PERIOD keys that drive the Twisted gyro ADC
  into `Cartridge::set_accelerometer(tilt / 32, 0)` (SameBoy's -7..+7 units), so
  a tilt cart finally sees something other than level. (No selftest: the
  mapping lives in the frontend's key handler, and there is no display here to
  QA it against a real game - `clear_tilt()` on load/eject/focus-loss is the
  path that keeps a stale tilt out of a fresh boot.)

* [DONE] **The DMG OAM corruption bug is modelled** - the last piece of the
  Blargg `oam_bug` story that did not need a per-dot renderer. Writing
  $FE00-$FEFF while the PPU scans OAM is not merely dropped: it shuffles OAM
  (Blargg readme: "Occurs when 16-bit increment/decrement is made of value in
  range $FE00 to $FEFF, during around the first 20 cycles of a visible
  scanline"). `PPU::oam_bug_row()` is the scan's row pointer - no row with the
  LCD off, in the 10 VBlank lines or on the CGB (which has no bug), and
  `0xFF` past the scan - and `PPU::trigger_oam_bug()` applies SameBoy's
  `GB_trigger_oam_bug`: the 16-bit word at the row is blended with the two
  words below it (`((a ^ c) & (b ^ c)) ^ c`), then six bytes are copied up from
  the entry underneath. Three details cost real debugging time:
  (1) SameBoy indexes `base[-4]`/`base[-2]` as `uint16_t*`, so they are 8 and
  4 *bytes* back, not 4 and 2; (2) the window is the first **80 T-cycles of a
  visible line** with the row advancing every 2 T (each row held twice) and
  row 8 already live at dot 0 - fitted against `4-scanline_timing`, whose
  "first corruption / +18 clocks / not one clock later" cases pin all three
  edges; (3) SameBoy's last scan step (row $A0) blends one word past its own
  0xA0 array, which crashed our selftest intermittently until the blend was
  clamped. The triggers are the CPU's *internal* 16-bit bus writes, at the
  point in the instruction where SameBoy puts them and with the value on the
  bus **before** the change (`DEC DE` with DE=$FE00 must fire, so it cannot be
  the decremented value): `INC`/`DEC rp`, `PUSH`, `POP` (with SP after the
  first increment - Blargg's `LD SP,$FDFF : POP BC` case), `CALL`, `RST`,
  taken `JR` (PC after the jump), `LD SP,HL`, `LD A,(HL+/-)` and interrupt
  entry (`CPU::oam_bug_write` -> `MMU::trigger_oam_bug`); plus the blocked
  OAM write path in `PPU::write_oam`. `PPU::write_lcdc` also became a real
  function: enabling the LCD restarts LY/dot/mode *immediately* instead of on
  the next `step()`, so a CPU that touches OAM before the PPU has been stepped
  still hits the block. 13 new selftest checks: the row sequence
  (8,16..80 across mode 2, `0xFF` after 80 T), the blend and the copy inside
  the window, a dropped write past it, and immunity with the LCD off, in
  VBlank and on the CGB. `oam_bug` went 2/8 -> 5/8; nothing else moved (all
  six other GB conformance ROMs, the jsmolka suites and three commercial GBA
  games are byte-identical), and the three remaining subtests are documented
  above.
* [DONE] **MBC6 is implemented** - the last mapper on the list besides TAMA5,
  and the strangest: two *independently* switchable 8 KiB ROM/flash banks
  ($4000-$5FFF and $6000-$7FFF), two independently switchable 4 KiB SRAM banks
  ($A000-$AFFF and $B000-$BFFF), four memory-select registers at $0000-$0FFF
  and a Macronix MX29F008TC 1 MiB flash chip mapped into the same windows
  (Pan Docs "MBC6"; the game is Net de Get: Minigame @ 100, which downloads
  minigames into the flash over the Mobile Adapter). Every register is mirrored
  across its $400 slice and the SRAM is 8 banks of 4 KiB, so the effective bank
  count is twice what it looks like. The flash is a real command-sequenced
  part, not a blob: `$AA` to `$5555` + `$55` to `$2AAA` (which the 16 KiB ROM
  mirrors at `$4AAA`/`$6AAA`) unlock the command port; `$30` erases the sector
  the command byte's own address falls in, `$10` the chip, `$04`/`$E0`/`$77`
  the hidden 256 bytes, `$A0`/`$E0` program (AND-only, so programming without
  an erase behaves like Pan Docs warns), `$90` reads the JEDEC ID (`$C2 $81`),
  `$20`/`$40` set and clear the non-volatile sector-0 protection, and `$F0`
  exits anything - including program mode, which is why `$F0` can never be the
  byte a program writes. Erases and programs report a status byte (bit 7 done,
  bit 1 protection) and complete instantly here. Sector 0 and the hidden region
  sit behind *two* layers: the Flash Write Enable bit at $1000 (asserted after
  power up) and the Protect Sector 0 command, and a chip erase keeps a
  protected sector 0. `has_battery()` is true and the whole 1 MiB of flash goes
  into the battery save (4 SRAM + 128 flash banks of 8 KiB), because a save that
  dropped the flash would drop the downloaded minigames. 37 new selftest checks:
  bank A/B independence and the register mirrors, bank 0 not translated, the
  SRAM banks as independent views (including both pointing at one bank), the
  full command set, the AND-only program path, both protection layers, the
  hidden region, ID mode, and both save paths (state buffer and on-disk file,
  whose size is asserted so a regression that dropped the flash would fail).
  Not covered: Pan Docs mentions no EEPROM or tilt on MBC6, so the
  accelerometer-equipped variants some dumps claim are not modelled - the
  `set_accelerometer` seam exists but nothing reports through it.
* [PARTIAL] **TAMA5 (the Tamagotchi mapper) is implemented** - the last
  cartridge type Pan Docs lists, and the strangest protocol of them all: there
  is no register file in `$0000-$1FFF` at all. The whole mapper is two 4-bit
  registers on the cart bus, `$A000` (data) and `$A001` (which register the
  data register names), and nothing reads sensibly until the chip is unlocked
  by writing `$0A` to `$A001` (before that `$A000` alternates `$F0`/`$FF` and
  `$A001` reads locked). Implemented from Tauwasser's reverse-engineering notes
  (gbdev.gg8.se "TAMA5 (WIP)") - Pan Docs' Bandai TAMA5 page is a stub that
  only names the chip set, so those notes are the reference:
  - **ROM banking**: registers 0 and 1 hold address bits 14-17 and 18, i.e. a
    32 KiB block remapped over the *whole* `$0000-$7FFF` window (there is no
    fixed bank 0 on this mapper), 1 MiB total.
  - **The 32 bytes of RAM** (not a banked SRAM - the header's size is a lie):
    write `$4:L`, `$5:H`, `$6:Y`, `$7:X` and the write lands when `$7` completes
    the command; read the same address with bit 5 of `$6` set (the notes' "Y
    = `$2` OR the high bit of address" for write, `$2` clear for read), then
    read `$C:L` and `$D:H`. Bit 4 of the command is the address's high bit.
  - **Status**: the documented probe is "write `$A001`,`$A`; read `$A000`" ->
    `$A1`, i.e. the register echoes its index with the ready flag in bit 0;
    unused registers read open bus.
  - Battery-backed, with the register file and the 32 bytes serialized.
  19 new selftest checks: the locked/unlocked read patterns, writes being inert
  while locked, the 32 KiB banking through both registers, a full RAM
  write/read round trip (including the second 16-byte half), open bus on an
  unused register, and a save/load round trip.
  **Not modelled, and why:** the TAMA6 4-bit MCU (command range `$40-$6F`,
  which also owns the buzzer) and the TC8521AM RTC behind it (`$80-$BF`, whose
  four BCD pages and alarm/timer-enable bits are described in the same notes).
  Both need the chip's firmware, not just its register map - we would be
  guessing at a 4-bit MCU's behaviour with nothing to test against (the one
  game, Game de Hakken!! Tamagotchi, is not something we have a ROM of).
  RTC commands currently read back zeroes, which is at least deterministic.
  Also unverified: the notes only mention `$A000`/`$A001`, so the mirroring of
  the rest of the 8 KiB window is a guess (it returns open bus).
* [DONE] The serial port is **clocked by the divider** instead of completing
  inside the `SC` write. `SC=$81` used to log the byte, clear the start bit and
  raise the serial IRQ in the same instruction - about 1000 M-cycles early.
  Modelled like SameBoy (`serial_master_clock`/`serial_count` in
  `Core/timing.c GB_serial_master_edge`, called from
  `GB_set_internal_div_counter` on the selected bit's falling edge): the
  transfer clock toggles on every falling edge of divider bit 7 (bit 2 in CGB
  fast mode), one bit shifts per full period, SB shifts left with a 1 shifted
  in per bit (nothing is plugged in - the harness's log keeps the byte that went
  *out*), and the interrupt fires at the end. A falling edge of bit N lands
  every 2^(N+1) ticks, so 8 bits take 64*2*8 = 1024 M-cycles = **4096
  T-cycles**, which is exactly the 8 x 512 cycles Pan Docs quotes; fast mode is
  32 M-cycles. `SC`'s unused bits now read back as 1 (`value | 0x7C`, SameBoy's
  `value | ~0x83`). Serialized (`kStateVersion` 4 -> 5). 13 new selftest checks;
  all six GB conformance ROMs and the eight jsmolka suites still pass.
  The completion time depends on where in the edge period the write landed *and*
  on whether the port's idle clock was mid-phase, so the tests bound it
  (960..1090 M-cycles) instead of pinning an exact number.
* [DONE] TIMA overflow is a **three-phase reload**, not an instant one. We did
  `TIMA = TMA` + timer IRQ in the same tick; Pan Docs "Timer" (and SameBoy's
  `tima_reload_state`, `Core/timing.c advance_tima_state_machine` stepped once
  per M-cycle) make it: the register takes TMA at once but **reads 0** for an
  M-cycle, the timer interrupt lands an M-cycle later, a TIMA write in that
  window is dropped, and a TMA write inside it cancels the reload (landing in
  TIMA too). `Timer::tima_reload_` is the state, serialized (`kStateVersion`
  3 -> 4). 5 new selftest checks, driven M-cycle at a time through a new
  `GbMachine::runHwCycles` (timer + PPU, no CPU - see the STAT-line note for
  why the CPU must stay out of it), and all six GB conformance ROMs still pass.
  Register map worth writing down because I got it wrong twice while testing:
  DIV `$FF04`, TIMA `$FF05`, TMA `$FF06`, TAC `$FF07`.
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
* [DONE] The GB core runs the CPU in per-M-cycle slices (the GB half of the
  "per-cycle CPU" project the APU-retrigger and open-bus items were parked
  behind). `CPU::set_cycle_hook` installs a callback the CPU drives itself:
  one M-cycle per byte fetched (`fetch()` charges before the read, so the
  operand access lands on the M-cycle hardware uses), an extra M-cycle for
  the write-back half of a read-modify-write (`rmw_write8`, so `INC (HL)` /
  `RES`/`SET`/`RLC (HL)` read on the second-to-last cycle and write on the
  last), and whatever is left of the instruction afterwards, so the hook sees
  exactly the M-cycles `step()` reports. `GameBoy` installs it and
  `step_frame()` no longer bills anything itself; in CGB double speed the CPU
  drops every other M-cycle before the hook (the PPU/APU stay on the
  original clock, the timer keeps the CPU rate - `double_speed_phase_`,
  serialized, `kStateVersion` 1 -> 2). Verified by Blargg **mem_timing 3/3
  and mem_timing-2 3/3** (all six previously failed: `F0`/`FA`/`BIT b,(HL)`
  reads, `LD (HL),d8`/`LDH (a8),A`/`LD (a16),A` writes and every
  read-modify-write were landing a full M-cycle early, and the RAM-only
  write tests could not even tell read from write), with `cpu_instrs` 11/11,
  `instr_timing`, all jsmolka suites and the selftest unchanged. 10 new
  selftest checks (per-shape M-cycle billing, the double-speed half rate, and
  a `LDH A,(TIMA)` read landing on the instruction's last M-cycle).
* [DONE] The headless GB harness can read Blargg's *other* result channel.
  `oam_bug`, `mem_timing`, `dmg_sound` etc. report through cartridge RAM, not
  the serial port: `$A000` is the result code (`$80` while running),
  `$A001-$A003` carry the `DE B0 61` signature and `$A004` a zero-terminated
  log (their readme's "output to memory"). `--blargg` now polls that too and
  prints the partial log on timeout, so those ROMs report a real verdict and
  a failure message instead of always timing out. Plus `--dump-gb <rom>
  <frames> <prefix>` (headless 160x144 PPM + serial log, the DMG counterpart
  of `--dump-gba`) and `GB4ME_FORCE_DMG=1` (`Cartridge::set_hardware_mode`)
  to run a CGB-flagged test ROM on DMG hardware - Blargg's DMG-only ROMs are
  simply wrong under the CGB model.
* [DONE — `01` fixed] Blargg `dmg_sound`, part 1: an APU power cycle is not a
  cold boot. `APU::power_on_channels()` restored the *post-boot* register
  defaults ($BF/$F3/$77/$F3), but hardware leaves the registers **cleared** -
  `01-registers` #5 fills every register with $FF, cycles NR52 and expects
  every one to read back cleared. Power-on now just resets the channels and
  the frame-sequencer step and reinstates the saved length counters. Also
  fixed the noise channel's power-gating: on DMG it is the one channel that
  keeps running across a cycle, so writes to NR41-NR44 while powered off
  still drive it (`11-regs after power` #4) while leaving the register
  shadows alone (`01-registers` #6). `dmg_sound` subtest 1 passes now
  (was failing at #5) and 11 moved from #2 to #4. 3 new selftest checks.
  * [DONE the window width and the CGB rule, PARTIAL the phase] Wave RAM while
    the channel is playing: the *polarity* was backwards. GBATEK "Wave Pattern
    RAM" (and mGBA's `GBAAudioReadWave`, SameBoy's `wave_form_just_read`) say
    the RAM is inaccessible while the channel is on - reads come back as open
    bus and writes are dropped - except on the cycle the DAC fetches, where the
    access lands on the byte being played. We had it exactly inverted (open bus only *inside*
    the window, the played byte outside). `read_wave_ram`/`write_wave_ram`
    now follow the documented rule, and the three selftests that pinned the
    old polarity were rewritten to state the real behaviour (outside: $FF /
    dropped; inside the 4-cycle window: the played byte, and a write hits it).
    `09`/`10`/`12` still fail, but the *window* is now pinned and the remaining
    gap is narrower than it was. The window is **one cycle**, not four: mGBA
    blanks the last 4 cycles before a sample, but SameBoy's `wave_form_just_read`
    is set only when the APU update ended exactly on the fetch (and cleared if
    it ended anywhere else), and SameBoy is the emulator that passes these
    tests - so `APU::wave_bus_busy()` is now `ch3_.enabled &&
    wave_just_read_`, a flag set by the T-cycle loop when the step's *last*
    cycle was the fetch (serialized, hence `kStateVersion` 6 -> 7). The period
    itself was already right: GBATEK gives the wave sample rate as
    `2097152/(2048-n)` Hz, i.e. `(2048-n)*2` T-cycles, which is what the
    channel already used (SameBoy's `sample_length ^ 0x7FF` is the same number
    on its 2 MHz APU clock).
    The CGB has no conflict at all - GBATEK's rule is a DMG bus artefact - and
    we were applying it to both models, so `cgb_sound` 09/10/12 could never
    pass either. `APU::set_hardware_mode` now exists (wired from
    `MMU::set_hardware_mode`, the path the PPU already uses) and the DMG gate is
    skipped on the CGB: a read returns the addressed byte and a write lands on
    it whatever the DAC is doing. `cgb_sound` 09/10/12 still fail (same T-phase
    blocker below); `cgb_sound` 08/11 were the CGB power-cycle counters, which
    landed separately - see the power-cycle item.
    What is left is the **phase**, and the test is explicit about it: `09` sets
    `NR33 = -2`, so the wave period is `(2048-2046)*2` = **4 T-cycles** (one
    fetch per M-cycle), and then offsets the read with `delay_clocks 176` plus
    `$99` = 153 more clocks every iteration ("have its first sample read occur 2
    clocks earlier each loop iteration"). 153 mod 4 = 1, so the read walks one
    T-cycle per iteration through all four phases of the 4-T fetch period -
    which is exactly what the expected CRC encodes. Our CPU charges whole
    M-cycles (`fetch()` ticks before the read), so every access sits on the last
    T of its M-cycle and we only observe the phase where the fetch happens to
    land on that same T; `mem_timing` pins the *M-cycle* of each access, but
    nothing in the model says which T inside it, and hardware does. Hence the
    alternating `value, $FF` we print (ours `$821B56`, expected `$118A3620`).
    So the read model is right, and what these three subtests need is the T
    offset of each memory access - the same missing piece as `07`'s DIV-phase
    question, and now the single known blocker for `dmg_sound`. Note the APU
    needs no re-modelling: it already ticks per T-cycle internally, so finer
    *stepping* is not the issue - finer *placement* is.
  * [DONE] The last Blargg reporting channel: **on-screen verdicts**.
    `PPU::console_shows(word)` / `console_text()` read Blargg's console text
    straight out of the BG tile map - his font is loaded so that a tile index
    *is* the ASCII code - using the same scroll/wrap math as the renderer
    (whole-word match only, so "PassedExtra" cannot trigger a pass). That turns
    two ROMs that were previously invisible (`--blargg` timed out at 20000
    frames with no output at all) into real signal: `halt_bug.gb` **passes**
    (105 frames, DMG) and `interrupt_time.gb` **passes** (32 frames, **CGB** -
    the ROM is built `.define REQUIRE_CGB`, so a DMG run legitimately fails it;
    running it in DMG mode is how we found that out). Both are wired into the
    CI `blargg` job with the matching mode. 6 new selftest checks.
    Side effect: `dmg_sound`/`oam_bug` now stop at the frame the verdict
    appears (~2100/~830 instead of the whole frame budget) and print the console
    text next to the cart-RAM log.
  * Gotcha when reading VRAM from tooling: `PPU::read_vram()` returns $FF while
    the PPU is in mode 3 (correct hardware behaviour), and a `MMU::read` of the
    same address charges enough cycles to land in mode 0 - so a dump that goes
    through the PPU directly can look empty while the screen is full.
    `console_shows` reads the backing array instead, so it is not affected.
  * [DONE - sanity] DMG/CGB PPU sanity via Acid2: the SameBoy action ROMs
    `dmg-acid2.gb` and `cgb-acid2.gbc` render the canonical image headlessly
    (`--dump-gb`, then read the PPM): mirrored text rows, the sprite smiley
    with its priority ordering, the frame, and all four DMG shades are
    present, in both DMG and CGB mode. Not a pass/fail test (Acid2 has no
    in-ROM verdict, and SameBoy's expected hashes are over its own BMPs), but
    it is the strongest PPU signal available without a reference image -
    worth re-running after any PPU change.
  * SameBoy's `docs/references/sameboy/.github/actions/sanity_tests.sh` shows
    those six action ROMs are expected to work in their CI (it pins their
    frame hashes) - a useful target list; `cgb_sound.gb` fails the same five
    subtests as `dmg_sound`, `oam_bug-2.gb` fails like Blargg's `oam_bug`.
  * [DONE - both halves] A power cycle is DMG-*non-destructive*
    but CGB-*destructive* for the length counters. `cgb_sound`'s own header
    says it: "On CGB, length counters are reset when powered up. On DMG, they
    are unaffected, and not clocked." We implemented the DMG half only, so on
    the CGB the power-on restored the snapshot like the DMG does.
    `APU::power_on_channels` now zeroes the four counters when
    `is_cgb_mode(hardware_mode_)` and restores them otherwise - which is what
    took **`cgb_sound` 11 from failing at #4 to passing** ("Powering off should
    clear NR41": with the counter restored, the note expired early because the
    restore also brought back a *smaller* remaining length).
    `cgb_sound` 08 then pinned the *second* half of the split, which is the more
    interesting half: **the CGB does not restart the frame sequencer across the
    cycle** (only the DMG does). That is observable because a trigger on a zero
    length counter reloads 63 or 64 depending on whether the next DIV-APU step
    clocks length (GBATEK "Obscure behavior") - i.e. the sequencer's phase
    *is* the answer. With the DMG reset applied to both models, 08 printed `3F
    FF 3F 3F` (CRC `$D92C0407`); keeping the CGB sequencer running gives the
    expected `40 00 40 40` and CRC `$3CF589B4` (the value upstream's source
    carries in `check_crc_dmg_cgb`, which the vendored copy here did not).
    So `cgb_sound` is **8/12** (01-06, 08, 11) and the DMG half of both rules is
    unchanged - `dmg_sound` 08/11 still pass.
    Both halves are now pinned by a selftest (the earlier "no test added"
    note was premature - the first synthetic attempt asserted a wrong
    expectation, not that the behaviour was untestable). Two details make it
    work and are worth not re-deriving:
      * A length counter **cannot** be made 0 by writing NRx1: `64 - 0x3F`
        bottoms out at 1. So "trigger a zero counter" is reachable *only* via
        the CGB power-on reset - which is precisely why the two models can be
        told apart with identical register writes.
      * The APU is constructed in the **post-boot** state, where `NR41 = $FF`,
        i.e. a restored length of **1** - not 0. That is the DMG half's actual
        observable: the DMG note runs 1 clock because the counter came back
        intact and the zero-reload quirk never fires, while the CGB comes back
        0 and reloads 64. Writing the test against "the DMG reloads 63" (as the
        first attempt did) is what produced the confusing 1/70 results.
    One length clock is every *second* frame-sequencer step (the 256 Hz step
    fires on even steps), i.e. 2 x 8192 T = 4096 M-cycles - stepping 2048
    measures half a length clock and silently halves every count.
  * [DONE - `08` fixed] A power cycle is *non-destructive* for the length
    counters, and they stay frozen across it. Three changes in
    `src/apu/apu.cpp`: `clock_length` only clocks a channel whose **DAC is
    on** (a power cycle clears the DACs, so the restored counters idle until
    the game turns one back on - invisible in normal play, and consistent with
    02 #11 "Disabled channel should still clock length", where the channel is
    off but its DAC is on); `power_on_channels` restores the counters **and**
    their enable flags (so the next trigger sees no enable edge and therefore
    no "enabling clocks it once" quirk) and resets `frame_clock_` with
    `frame_step_`; and the powered-off NR44 write path no longer applies that
    quirk. 4 new selftest checks.
  * How the expected values were recovered (worth reusing on every remaining
    Blargg subtest): the CRC in `check_crc` is **not** over the printed ASCII.
    `print_hex` calls `update_crc` with the raw byte and prints the two hex
    digits with `print_char_nocrc`, so the checksum covers the *values*. For a
    4-value test that means the expected output is recoverable by brute force:
    `for a,b,c,d in 0..255: crc32(bytes([a,b,c,d]))` is 2^32 table lookups, ~5s
    in C. Doing that for 08 gave DMG `33 44 11 22` (print order ch4, ch3, ch1,
    ch2) - exactly the four lengths the ROM loaded - which is what proved the
    counters must not be clocked, and CGB `40 00 40 40` (counters reset at
    power-on; the wave channel reads 0, i.e. CGB behaviour we do not model).
    Do NOT try to invert it as ASCII text: no 8-hex-char string has those
    CRCs, which is what proves the raw-byte reading.
  * Register map warning: `NR21` really is `$FF16` and `NR41` really is
    `$FF20`; `$FF15`/`$FF1F` are the unused gaps (see Blargg's
    `dmg_sound/source/common/gb.inc`). "Fixing" the decode to the other
    plausible-looking map passes every round-trip test (01-registers only ever
    writes and reads the same address) and silently breaks 02/03/05 - it was
    tried and reverted.
  * [DONE - `11` fixed] A length register written **while powered off** has to
    land in the power-cycle snapshot too, not only in the channel state that
    `power_off_channels` is about to clear - otherwise the power-on resurrects
    the value from *before* the write. dmg_sound 11 #4 writes `NR41=$EE`
    (length 18) while off and expects the noise note to end 18 length clocks
    after the retrigger that follows the cycle; with the snapshot updated it
    does, and `dmg_sound` is at 8/12 (01-06, 08, 11 ok). 3 new selftest checks.
  * The CRC oracle is *validated*: `06-overflow on trigger` passes and its
    expected value is listed in the source (`0555 0666 071C 0787 07C1 07E0
    07F0`), and `crc32(bytes([0x05,0x55, 0x06,0x66, ...]))` == `$F604603B`
    exactly - so any Blargg CRC can be turned into an exact expected-output
    oracle, including for tests that print nothing readable.
  * `09` re-investigated with that oracle (superseded again by the window/phase
    entry above, which pinned the window to one cycle and the residual gap to
    the CPU's T offset): the 69 printed bytes are the raw values read from `$FF30`,
    and ~500 candidate read models (window at every phase offset/width, the
    addressed vs played vs next-played byte, contiguous bands and every
    parity pattern of the 2-clock phase sweep, all-`$FF`/all-pattern) match
    neither `$118A3620` nor `$270DA9A3`. So the gap is the **T-phase of the
    CPU read inside the 4-clock fetch period**, which our M-cycle CPU slices
    cannot express (the reads only ever land on `freq_timer` 2 or 4).
    **Superseded - the blocker is narrower than "per-T scheduling".** Reading
    SameBoy's actual CPU (`docs/references/sameboy/Core/sm83_cpu.c`) refutes the
    "we would need T-resolution scheduling" framing:
      * `cycle_read`/`cycle_write` advance `pending_cycles` *before* the access
        and then set it to 4, so the APU is still stepped in **4 T-cycle
        chunks** - the same chunking we already do via `CPU::tick()` ->
        `apu.step(mcycles)` (`mcycles * 4`).
      * SameBoy's DMG wave window is `wave_form_just_read` (Core/apu.c), set
        only when the fetch lands on the **final** cycle of the advance window
        (`wave_form_just_read = false` again if `cycles_left` remains) - which
        is exactly our `wave_just_read_`, and its write path redirects to
        `WAV_START + current_sample_index / 2`, exactly our `ch3_.pos / 2`.
        So the window, the redirect and the flag all already match the
        reference; `09`'s residual is *not* a missing wave-window behaviour.
      * What SameBoy has that we do not is **sub-M-cycle access placement for
        the IO-conflict registers**: it advances 1 T-cycle at a time in 9
        places and leaves `pending_cycles` at 3, 5 or 6 instead of 4
        (`GB_CONFLICT_STAT_*`, `PALETTE_*`, `WX_DMG`, `LCDC_*`, `NR10_*`,
        `SCX/SCY`). Those writes land 1 T off the M-cycle grid, which is how
        the real chip behaves on a register whose write is split across the
        M-cycle boundary.
    **Correction to my own first reading of this:** the obvious follow-on idea -
    port the conflict-register T-offset table (a "this write lands at T+1 within
    its M-cycle" rule for the registers in SameBoy's conflict maps) - provably
    does **not** unblock `09`/`10`/`12`. Those maps cover only IF, LYC, LCDC,
    SCY, STAT, BGP/OBP0/OBP1, WY, WX, SCX and NR10 (CGB double-speed); there is
    no APU register in any of them, and all three failing tests read `$FF30`
    and write NR33/NR34. The table would still be a real (small) accuracy win
    for PPU-affecting writes, but it is not this blocker.
    What the failures actually need is narrower and stranger: the wave *fetch*
    grid must be able to sit at an odd offset from the CPU's access grid.
    SameBoy gets that for free because its APU counts down `sample_countdown`
    in exact T-cycles from an absolute phase (`sample_countdown + 1` per fetch,
    `update_wave_sample(gb, cycles - cycles_left)`), so a read at absolute cycle
    N sees a fetch iff the fetch landed exactly on N - no M-cycle grid
    involved. We advance the APU in 4 T chunks from `CPU::tick()` and infer
    "fetch on the last cycle of the chunk", which is only equivalent while the
    two grids stay aligned - and `09`'s `add $99` per iteration deliberately
    walks the initial `(2048 - NR33) * 2` period by 2 T each pass, so every
    other iteration puts the fetch grid 2 T off ours and the read can never
    coincide. Closing that means giving the APU an absolute T-cycle phase and
    letting the CPU's access resolve against it (rather than a per-chunk
    boolean), which is the same shared-cycle-counter refactor the per-dot PPU
    wants - one project, two consumers. Until then `09`/`10`/`12` stay red by
    design, and no amount of APU constant-tuning reaches them.
  * `07` #5 characterised (still failing). Its `test_power` helper writes
    `NR14=$40` (length disabled), `NR11=$FF` (length 1), `NR12=8`, then
    `NR14=$C0`, and times the expiry; our trace shows the enable-edge quirk
    decrementing that counter 1 -> 0 and the trigger's "0 becomes maximum"
    turning it into a 64-clock note, ~16x the window the ROM accepts. Three
    fixes were tried and all regress something else: flipping the quirk's step
    parity breaks 02 #7 / 03 #2 / 05 #7, requiring `timer > 1` breaks 03 #5,
    and dropping the power-on divider reset changes nothing. The remaining
    explanation is a DIV-phase question (the quirk simply must not fire in that
    phase on hardware), which is the same T-cycle-accurate coupling that
    `09`/`10`/`12` need - not another APU constant. See the wave-window entry
    above: what that coupling concretely is (an absolute T-cycle phase shared
    between the APU and the CPU's accesses, per SameBoy's `sample_countdown`
    model) and what it is *not* (a conflict-register T-offset table - those
    registers are all PPU/timer ones and cannot affect an APU test).
  * **MEASURED AGAINST SAMEBOY - it passes 07 and 09, so this IS our bug.**
    Built SameBoy's headless `tester` from `docs/references/sameboy` (`make
    tester`, needs SDL2) and dumped its console text straight out of the tile
    map at $9800 (Blargg's shell loads a font where the BG tile index *is* the
    ASCII code, so the text is exact - no OCR needed):
    | ROM | SameBoy | us |
    |---|---|---|
    | `06` (control) | `Passed` | passes |
    | `07` | **`Passed`** | `Failed #5` |
    | `09` | **`Passed`** | `Failed` (CRC $821B56, wants $118A3620) |
    | `10`, `12` | screen still full of data at 45 s - verdict not visible | fail |
    So an accurate emulator *does* satisfy these, and the "no cycle-accurate
    model meets them" reading was wrong. (An earlier version of this entry
    inferred from bitmap ink volume that SameBoy failed them too; that
    inference was wrong - it is exactly why the tile-map dump was worth doing.
    Do not re-derive conclusions from screen-pixel statistics; read the text.)
    SameBoy's `09` output, which is the ground truth to match:
    `FF FF 00 FF 11 FF 11 FF 22 FF 22 FF 33 FF 33 ... EE FF EE FF FF FF FF
    FF 00 FF 00 FF 11` - i.e. the read alternates real-byte/open-bus as the
    2 T-per-iteration phase sweep walks the fetch grid across the CPU's, which
    is the behaviour this entry already describes. Ours diverges in the tail
    (`... FF FF FF FF FF 00 FF 00 FF 11 FF 11 FF 12`), so the phase is close but
    the grid is anchored differently - SameBoy's run is a clean subsequence of
    ours, so the shape is right and we are off in phase/alignment, not in the
    rule.
  * **Two hypotheses tested against that oracle, both wrong and now ruled out
    so they are not retried:**
    1. *"Our wave period is 2x too slow."* SameBoy reloads
       `sample_countdown = sample_length ^ 0x7FF` = `2047 - n`; ours is
       `(2048 - n) * 2`. For `09`'s `NR33 = -2` (n = 2046) that is 1 vs 4, so
       halving ours to `(2048 - n)` looked plausible. It is wrong: the CRC moved
       $821B56 -> $558756 and, decisively, the alternating `real FF real FF`
       pattern collapsed into near-solid reads (`22 33 33 44 44 55 55 66 66
       ...`), which is not what the hardware does. `(2048-n)*2` is correct and
       GBATEK-consistent (SameBoy's APU unit is 2 GB-T, so the two agree).
       Reverted.
    2. *"CPU operand accesses are an M-cycle early."* Refuted by reading
       SameBoy's `cycle_read`/`cycle_write`: its first access in an instruction
       sees `pending_cycles == 0` and its trailing M-cycle is deferred, so
       `LD A,(a16)` reads and `LD (a16),A` writes on M-cycle 4 of 4 in both
       models. There is no off-by-one-M-cycle to find.
  * **Hypothesis 3, also refuted (measured, then reverted):** SameBoy reloads the
    wave countdown on an **NR33 write** when the fetch is due within the next
    advance (`Core/apu.c`, `GB_IO_NR33`: `if (sample_countdown == 1)` -
    "just reloaded countdown"; its own comment says it is unverified by a test
    ROM). That looked like exactly the re-anchoring `09` needs, since the ROM
    writes NR33 mid-countdown every iteration. Implemented (SameBoy's unit is
    2 T, so its `countdown == 1` is our `freq_timer == 4`) and instrumented:
    **the condition never fires** - across `09`'s ~138 NR33 writes,
    `freq_timer == 4` never held at a write (`freq_timer` was 88 at write 100),
    and the CRC stayed exactly `$821B56`. So it is not the lever either. Reverted;
    do not re-add without a test that shows it mattering.
  * **EXACT DIFF against the oracle (the sharpest narrowing so far).** Parsed
    SameBoy's 69 printed values straight out of its tile map and ours out of the
    verdict reader, then compared element by element:
    * The alternation *pattern* is right - both hit every other access - but the
      **phase is inverted by one access**, and an earlier version of this entry
      wrongly called the structure identical. Traced directly (temporary trace,
      since removed), reading the returned byte alongside the window state:
      ```
      ours      rd#0 FF  rd#1 00  rd#2 FF  rd#3 11  rd#4 FF  rd#5 11  rd#6 FF  rd#7 22
      SameBoy   rd#0 FF  rd#1 FF  rd#2 00  rd#3 FF  rd#4 11  rd#5 FF  rd#6 11  rd#7 FF
      ```
      i.e. SameBoy's series is exactly `[FF] + ours`. We coincide with the fetch
      on accesses 1, 3, 5...; SameBoy on 0, 2, 4.... So SameBoy's first fetch
      lands one access *later* than ours - which is the "missing 2 T", now
      confirmed as a one-access phase inversion rather than a byte-count error.
    * The only difference is the wave **position**: ours runs exactly one byte
      ahead. SameBoy's even-index values are
      `00, 11, 11, 22, 22, 33, ... FF, FF, 00, 00, 11`; ours are
      `11, 11, 22, 22, ... FF, FF, 00, 00, 11, 11, 12`. At the first `$FF30`
      read SameBoy is at `current_sample_index` 0 and we are at `ch3_.pos` 2 -
      one byte of wave RAM further on - and our final value `$12` is not even a
      byte of the test's pattern.
    So this is **not** a phase/quantisation problem after all (that was the
    previous conclusion and it was too coarse): the fetch/open-bus coincidence
    is right, and one extra wave byte is consumed before the first read. That
    points at the trigger, not the timer. Concretely, compare our
    `APU::trigger_wave` with SameBoy's (`Core/apu.c` ~line 2004): it sets
    `current_sample_index = 0` and then, *only if the channel was already active
    and `sample_countdown == 0`*, latches `current_sample_byte` from
    `WAV_START` - it does **not** reload the countdown, and it does not consume a
    sample. Ours reloads `freq_timer` unconditionally and leaves the output
    buffer stale. Note also that an `NRx1` write reloads the counter in our
    model but SameBoy only reloads on a *fetch* - which is why h3 above never
    fired.
    Next step, and it is now a small one: log `ch3_.pos` and `ch3_.freq_timer`
    immediately before each `$FF30` read in `09` and find where the extra byte is
    consumed - candidate causes are (a) the unconditional `freq_timer` reload in
    `trigger_wave` letting a fetch land before the read, and (b) the stale output
    buffer making the first read report sample 1 instead of 0. Fix whichever the
    trace implicates; the alternation should survive.
  * **[SHIPPED] The trigger reload offset: `+ 4 T`.** SameBoy reloads the sample
    countdown on trigger as `(sample_length ^ 0x7FF) + 3` (`Core/apu.c` ~2014)
    and counts the wave channel in units of 2 T (confirmed from its square
    channels, where `(x ^ 0x7FF) * 2 + 1` only matches GBATEK's `(2048-n)*4` T at
    2 T per unit). So its trigger period is 4 T longer than our bare
    `(2048 - n) * 2`, and those 4 T decide whether the first DAC fetch lands just
    before or just after the ROM's `$FF30` access. Measured, against the oracle:
    | trigger reload | `09` | `12` |
    |---|---|---|
    | bare | 34/69 values match; one wave byte ahead | the written `$F7` never appears |
    | **`+ 4` (shipped)** | **50/69 match**, all odd (open-bus) indices correct | **`$F7` now appears** - the write lands |
    | `+ 6` | `09` *passes* | - (rejected, see below) |
    Three of our own wave-window selftests asserted the fetch landed at the bare
    period and failed at `+4`; they were asserting a *phase*, not a rule, so their
    step count moved from `(4096-1)/4` to `(4100-1)/4`. The window is still one
    cycle wide and still openable - that is now checked, not assumed.
  * **`+ 6` remains rejected.** It makes `09` pass, but it puts the fetch at
    2 (mod 4), i.e. off any M-cycle boundary, while `wave_just_read_` is defined
    as "the fetch happened on the *final* T of this 4 T chunk". A steady state
    can then never open the window, leaving DMG wave RAM unreadable - a real bug
    that would break games even though `09`'s CRC is happy. Not a stale assertion;
    a genuine conflict.
  * **Fourth hypothesis, refuted (and instructive): "our *per-fetch* reload is
    also 2 T too long."** SameBoy reloads to `sample_length ^ 0x7FF` and a fetch
    consumes `countdown + 1`, which reads as its countdown sitting 2 T lower than
    a bare period at the same instant, i.e. the fetch landing 2 T later within
    each period. Applying `- 2` to our post-fetch reload collapses `09` from
    50/69 to **4/69**: the value sequence degenerates to one new sample per read
    (`00 11 22 33 44 55 66`) instead of the correct doubling
    (`00 11 11 22 22 33`), because every access then coincides with a fetch.
    So the two forms are *not* interchangeable - ours fires when `freq_timer`
    reaches 0, SameBoy's after `countdown + 1` units - and the period length
    already agrees. Reverted; the per-fetch reload stays bare.
  * **Measured baseline for the remaining work** (temporary trace, since
    removed; recorded so it is not re-derived). At each `$FF30` access in `09`
    our side reads, per access: `pos`, `freq_timer`, `wave_just_read_`, and
    whether the window was open. It alternates perfectly, two accesses per
    wave-RAM byte:
    ```
    rd#0 pos=0 ft=2 just=0 busy=0     <- open bus
    rd#1 pos=1 ft=4 just=1 busy=1     <- real byte (ram[pos/2] = ram[0])
    rd#2 pos=1 ft=2 just=0 busy=0
    rd#3 pos=2 ft=4 just=1 busy=1
    ... pos advances 1 per two accesses
    ```
    So on a DMG the fetch lands on every *other* M-cycle, `freq_timer` reads 4
    when the window is open and 2 when it is not, and `pos` advances one nibble
    per open access. The APU has **no absolute T counter** - the phase is only
    observable as "was the fetch the last T of this 4 T chunk".
  * **The A/B trace against SameBoy is done, and it settles the mechanism.**
    SameBoy's tester now logs the same three fields from inside
    `GB_apu_read`'s wave branch (`Core/apu.c`). Two traps, both worth recording:
    (a) that branch compares against `GB_IO_WAV_START`, which is **`0x30`**, not
    `0xFF30` - `memory.c` calls `GB_apu_read(gb, addr & 0xFF)`, so the *masked*
    byte arrives and the comparison does match after all; and (b) hooking the
    tester's read callback instead **core-dumps the tester**, because the
    callback re-enters `GB_read_memory` which re-enters the callback. Log from
    `Core/apu.c`, not from the tester.
    With that, the two series are:
    ```
    SameBoy  rd#0 idx=0 cd=1 justread=0 | rd#1 idx=0 cd=0 justread=0
             rd#2 idx=1 cd=1 justread=1 | rd#3 idx=1 cd=0 justread=0
             rd#4 idx=2 cd=1 justread=1 | rd#5 ...
    ours     rd#0 pos=0 ft=2 busy=0     | rd#1 pos=1 ft=4 busy=1
             rd#2 pos=1 ft=2 busy=0     | rd#3 pos=2 ft=4 busy=1
    ```
    Two facts fall out, both previously guessed:
    1. SameBoy flags the fetch on **even** accesses, we on **odd** - the one
       access of phase inversion, measured on both sides rather than inferred.
    2. SameBoy's `sample_countdown` only ever takes the values **0 and 1**,
       while ours takes **2 and 4**. That is the quantisation difference
       suspected earlier and never confirmed: SameBoy's wave channel counts in
       fixed **2 T units**, so its fetch grid can only sit on even T, whereas ours
       counts every T. A 2 T-quantised grid and a 4 T-quantised CPU access grid
       coincide on every *other* access - which is exactly the alternation both
       models show, offset by one because of where each grid is anchored.
    So the fix is not a constant at all: ours must reproduce a **2 T-quantised**
    fetch grid.
  * **The 2 T-quantised model was implemented and measured; it works, and it is
    the faithful one - but it does not by itself reach 69/69.** Counting the wave
    channel in 2 T units (`--freq_timer` on `i & 1`, period `2048 - n`, reload on
    the fetch's last T of the advance) with SameBoy's literal trigger offset
    `(2047 - n) + 3`:
      * selftest **passes** (all the window checks, unmodified),
      * `09` reproduces SameBoy's series *structurally exactly*
        (`SameBoy = [FF] + ours`, the same relationship as before),
      * but the value is still **50/69** - the one-access anchor gap survives.
    Two things learned while doing it:
      - The flag must be set on the fetch landing on the **last T of the advance**
        (`i == 3`), matching SameBoy's "consumed the whole advance" and its
        clearing of the flag when `cycles_left` survives. `i == 2` is unreachable
        (the countdown can only expire on `i & 1`), which silently closes the
        window and yields all-`$FF`.
      - Dropping the trigger offset to `+ 2` units to shift the anchor closes the
        window instead of moving it: **5/69** and two selftest failures. So
        SameBoy's `+ 3` is the correct anchor and the residual offset is *not* in
        the trigger - it is that our CPU access resolves one 2 T unit away from
        SameBoy's `cycle_read`.
    Not shipped, deliberately: it is a larger behavioural change than the
    `+ 4 T` offset that is currently in the tree, it buys **no** additional
    passing subtest, and the remaining gap is in access placement. Keep `+ 4 T`
    until the access placement is fixed, then move to this model - it is the
    version that will actually close `09`/`10`/`12`, because it reproduces the
    reference's quantisation instead of compensating for it.
  * **The residual after `+4` is a consistent one-slot shift, in both `09` and
    `12`.** `09` differs from the oracle only in that SameBoy has one extra
    open-bus miss at the start (its first fetch lands later still); `12`'s tail
    reads `... EE FF 00 11 22 ...` where SameBoy reads `... EE FF 00 F7 22 ...`.
    Both say the same thing: we are one read *ahead* of SameBoy, consistently.
    Since SameBoy's own source implies `+4` and we still need 2 T more to match,
    the missing 2 T is in the *access* placement, not the timer - which is the
    absolute-phase window below.
  * **Superseded history, kept so it is not repeated:** the `+6` experiment below
    made `09` pass and was reverted; and an earlier version of this entry blamed
    the whole thing on a "2 T vs 4 T quantisation" difference, which the exact
    69-value diff disproved (the alternation was already correct).
  * **Original finding, for reference:** SameBoy reloads the
    sample countdown on trigger with an offset:
    `sample_countdown = (sample_length ^ 0x7FF) + 3` (`Core/apu.c` ~line 2014),
    and SameBoy counts the wave channel in units of 2 T (confirmed from its
    square channels: `(x ^ 0x7FF) * 2 + 1` matches GBATEK's `(2048-n)*4` T only
    at 2 T per unit). So the trigger period is 4 T longer than our bare
    `(2048 - n) * 2`. Measured, against the oracle:
    | trigger reload | `09` |
    |---|---|
    | `(2048-n)*2` (current) | CRC `$821B56`, one byte ahead |
    | `+ 4` (SameBoy's value) | CRC `$7EFFA9`; the value sequence then matches SameBoy **exactly** (`00, 11, 11, 22, 22, 33...`) - only SameBoy's index 0 is `$FF` where we hit |
    | `+ 6` | **`09` PASSES** |
    So the alternating read really is decided by this offset, and the correct
    value for `09` is `+6`, not the `+4` SameBoy's source implies - i.e. there is
    a further 2 T somewhere in the read or trigger placement.
    **Not shipped, deliberately:** `+6` breaks two of our own wave-window
    selftests ("read on the fetch cycle returns the played byte", "a write on the
    fetch cycle hits the played byte"). The cause is not a stale assertion: at
    `+6` the fetch phase becomes **2 mod 4**, so the fetch no longer lands on an
    M-cycle boundary, while `wave_just_read_` is defined as "the fetch happened
    on the *final* T of this 4 T chunk". In a steady state that window can then
    simply never open, which would leave the DMG wave RAM unreadable - a real
    emulation bug that would break games, even though `09`'s CRC is happy.
    This ties the whole entry together: the fix is **not** a constant, it is to
    make the window absolute instead of chunk-relative - give the wave channel a
    T-cycle phase and have a `$FF30` access resolve "did a fetch occur at exactly
    this T?" rather than inferring it from a per-chunk boolean. That is the same
    absolute-phase idea as before, but now with a concrete constant (`+6`), a
    concrete oracle (the 69 values), and a concrete failure mode to avoid. Do it
    that way, and re-check `10`/`12`/`07` at the same time.
  * **What is left, as a testable question:** SameBoy's fetch grid drifts across
    the access grid (consecutive `$FF30` reads alternate open-bus/real byte)
    while ours stays anchored - with the *same* chunked 4 T advance. So the
    difference is when the countdown is first loaded relative to the trigger:
    SameBoy reloads it inside the div path and lets it run down across chunk
    boundaries, anchoring the grid to an absolute phase rather than to the
    trigger access. Note what has now been *excluded*: the period formula (h1),
    operand-access placement (h2) and the NR33 reload (h3). The remaining
    untested difference is that SameBoy's fetch loop lives in the div path and
    consumes `countdown + 1` *units* per fetch, so its grid is quantised to 2 T
    while ours is quantised to 1 T; a 2 T-vs-4 T quantisation difference would
    produce exactly this "same shape, different anchor" symptom. Next step:
    trace `freq_timer` and `ch3_.pos` at each `$FF30` read and compare the
    residual offset against the oracle above - do that before touching the
    formula again, since three plausible fixes are already dead.
  * Follow-up, and it moves the goalposts in a useful direction: SameBoy's
    `cycle_read`/`cycle_write` were then read line by line against ours, since
    "both advance the APU in 4 T chunks" only holds if the *placement* agrees
    too. It does - SameBoy's first access in an instruction sees
    `pending_cycles == 0` and its trailing M-cycle is deferred into the next
    instruction, so `LD A,(a16)` reads on M-cycle 4 of 4 in both models and
    `LD (a16),A` writes on M-cycle 4 of 4 in both (we tick once per fetched byte
    and leave operand `read8`/`write8` on that same M-cycle, which lands in the
    identical place). So there is no hidden off-by-one-M-cycle to find here, and
    `09`/`10`/`12` are **not** caused by misplacement - only by the APU being
    unable to hold a phase that is not a multiple of 4 T - except that SameBoy
    *does* manage it (measured below), so the gap is in our bookkeeping rather
    than in the concept. Note the alternative
    ("hardware's trigger reloads the counter from NRx1") is ruled out by 02 #7:
    with `NRx1=4` and an expired counter, a trigger yields *maximum*, not 4.
  * STILL open in `dmg_sound` (`GB4ME_FORCE_DMG=1 ./bin/GB4ME --blargg
    dmg_sound.gb`, now 8/12): `07` len/sweep period sync #5, and `09`/`10`/`12`
    (wave fetch T-phase). Both are now known to be the *same* missing piece -
    the CPU charging M-cycles instead of T-cycles, so neither the DIV phase
    (`07`) nor a memory access's offset inside its M-cycle (`09`-`12`) can be
    expressed. No register-level bug is left in this suite.
  * `oam_bug` **6/8** in DMG mode - up from 2/8 with the corruption model
    (measured again this session; the 5/8 previously recorded here was stale -
    `02`-`07` all pass and only `01` and `08` fail).
    `1-lcd_sync` is now **measured, not guessed**. The ROM
    (`oam_bug/source/1-lcd_sync.s`) does: `LCDC=$81`, `delay 109`, read LY, want
    0; `LCDC=$81`, `delay 110`, read LY, want 1 - and `delay n` is n **clocks
    (M-cycles)**, not T-cycles (`delay_clocks n` is the T-cycle form and passes
    `n/4` down). So LY must roll over ~110 M-cycles after the LCDC write, which
    a 456-M-cycle scanline cannot do from dot 0 - the frame has to *resume
    mid-line*. We currently resume at dot 0, so `01` fails check 3 ("starts too
    early": LY is still 0 at 110 clocks).
    **Both obvious implementations are wrong, and that is the useful part.**
    (a) Repositioning the dot counter - `line_cycles_ = kDotsPerLine - 110` on
    enable - makes `01` checks 2 and 3 both pass, and simultaneously breaks
    `04-scanline_timing`, `05-timing_bug` and `06-timing_no_bug`, because those
    enable the LCD and then measure ordinary scanlines, now all short by 110.
    (b) Shortening **only the first line** - a per-line `line_target_dots_` set to
    110 on enable and restored to 456 afterwards, which is the shape the test
    implies - is worse: 2/8 (`02`->1, `04`->3, `05`->2, `06`->1). A 110-dot line
    cannot even contain mode 2 (80 dots) plus any of mode 3, so the mode/OAM/STAT
    logic *inside* the short line is wrong - and the OAM-corruption tests depend on
    exactly that. Both reverted.
    So the enable does not resume a normal line from either end; the real
    behaviour is in what the mode sequencer does across the enable. That is
    per-dot PPU territory, not a constant, and neither reference emulator models
    it cleanly either (mGBA below). Do not attempt this with a line-length or
    dot-counter tweak.
    mGBA is consistent with this being fiddly rather than a plain offset:
    `GBVideoWriteLCDC` (`src/gb/video.c`) does not start in mode 0 at all - it
    sets mode 2, schedules `_endMode2` at `GB_VIDEO_MODE_2_LENGTH - 5` with
    `// TODO: Why is this fudge factor needed? Might be related to T-cycles for
    load/store differing`, and deschedules the frame event outright.
    below (`2-causes`, `3-non_causes`, `4-scanline_timing`, `5-timing_bug`,
    `6-timing_no_bug` pass; `1-lcd_sync`, `7-timing_effect`,
    `8-instr_effect` fail). The three that fail are known and bounded:
    `1-lcd_sync` wants LY to increment 109-110 M-cycles after the LCDC write
    rather than 114, i.e. the LCD-enable edge has a *latency* before line 0
    starts (Blargg measures it at a few T-cycles). Two attempts, both
    reverted: shortening line 0 to compensate passes subtest 2 and fails 3
    *and* breaks `4-scanline_timing`/`5-timing_bug`, because those pin the
    corruption window to the LCD-enable + frame-boundary phase - so the
    latency, the line length and the window start are three separate
    quantities and only two of them can be had at once. `PPU::write_lcdc`
    already restarts LY/dot/mode at the enable instant (which is what made
    the OAM window correct from dot 0); what is missing is the few T-cycles
    between the write and the line actually starting. `7` and
    `8` check the *byte pattern* with a CRC: SameBoy's blend reproduces that
    a corruption happens and exactly when (both window-edge subtests pass),
    but not the resulting bytes. Pan Docs does not document the corruption at
    all and mGBA does not model it (it just drops OAM writes in modes 2/3), so
    there is no third reference to check against - a sweep of the plausible
    variants (source offsets, argument permutations, row phase, stride,
    window width) all missed Blargg's CRCs.

GBA:

* [DONE] Promote `GbaScheduler` from stub to a real event queue
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
  [DONE fourth producer - DMA completion: the transfer runs synchronously
  (data moves, bus latency accumulates per unit through `accessCycles`),
  then `finish()` (enable clear, repeat reload, IRQ) fires as a scheduler
  event after that latency (`scheduleCompletion`/`onCompletion` with a
  per-channel generation guard, `cancelPending` on stop/reprogram,
  `rearmEvents()` on load with deadline/pending/gen serialized). FIFO
  blocks complete the same way. Audio FIFO needs no producer of its own:
  consumption runs inside the timer-overflow events at exact ticks through
  the overflow hook (with DMA re-requests via `triggerFifo`), and FIFO
  state round-trips. Covered by the DMA completion/IRQ and FIFO selftests.
  What remains from this item: prefetch overlap and per-cycle CPU slices.]
* [PARTIAL] Prefetch buffer + full `N/S` `WAITCNT` accounting; correct open-bus
  BIOS/palette/OAM values per mGBA suite.
  [DONE the waitstates and the buffer: `GbaBus::romWaitN`/`romWaitS` decode the
  full GBATEK "System Control" N/S tables per WS0/1/2 slot (the nonsequential
  half pays N, the sequential one S, a 32-bit access splits into two halves),
  WAITCNT's SRAM wait is charged on the access, and bit 14 hands the 16-byte
  prefetch buffer to `fetchCycles`. `invalidatePrefetch()` drops the queue on
  every PC/state change (see P0 item 4). Still open: the *open-bus* latch
  values for BIOS/palette/OAM reads, the prefetch-overlap timing (how many
  buffered words may still be consumed before a nonsequential access costs
  again) and GBA video-contention stalls - all three want per-cycle CPU
  accounting.]
  [DONE - and it kept giving: the runner turned into a three-round accuracy
  driver, all diagnosed with `--gba-tests` + `--dump-gba` dumps. (1) test 66
  (shifts): ROR-by-register with amount&31==0 kept the old carry and
  returned garbage for 32/64 (ARM ARM: result is Rm, carry becomes bit 31);
  4 new checks. (2) test 227/229 (empty LDM/STM): the no-op was wrong -
  hardware transfers PC with a 0x40 writeback (mGBA LDM/STM_LOOP; Thumb
  store sees PC+2, ARM side writeback spans the 16-word block); 6 new
  checks. (3) test 230-232 (STM base in rlist): the base slot stores the
  ORIGINAL base only when it is the lowest register in the list, otherwise
  the writeback value; 5 new checks in both Thumb and ARM state. jsmolka
  `thumb.gba` now reports Passed (2 frames), `memory.gba` still clean.]
  [DONE the accounting slice: `accessCycles` returned wait-states without
  the base cycle (EWRAM 2/2/4, ROM N without +1, SRAM width×waits), so every
  GBA access undercounted. Now GBATEK/mGBA-table totals: EWRAM 3/3/6, ROM
  halves waits+1 with 32-bit split N+1+S+1 / S+1+S+1 (WS0 defaults 5/5/8 N,
  3/3/6 S), SRAM waits+1 per byte with 32-bit doubled. 21 new selftest
  checks pin the tables (both slots, custom WAITCNT, SRAM widths, fixed
  regions). Verified: `--selftest` green, `--dump-gba` Emerald 120f
  pixel-identical to baseline (only the sampled PC moves - ISR-vs-main at
  the frame edge, confirmed across adjacent frames), Emerald 600f + Golden
  Sun 120f boot clean.
  DONE since, prefetch buffer (mGBA-shaped, no per-cycle CPU needed):
  `GbaBus::fetchCycles` models the 16-byte hot block behind WAITCNT bit 14
  (a non-sequential fetch inside it costs S instead of N; data reads and DMA
  keep using `accessCycles` and never touch it), wired into both CPU fetch
  sites, serialized with tolerant reads so old states resume cold. 13 new
  selftest checks (off-parity, hot-block upgrade, refill, non-ROM exclusion,
  save/load preservation); Emerald 120f still pixel-identical with it on.
   Still open: full mGBA-style overlap (needs per-cycle CPU slices).
   [DONE slice 1 since, per-cycle clock: `GbaBus::accessCycles` advances
   the scheduler clock on every access when tick-clocking is on, so events
   fire mid-instruction at exact ticks (the clock still ends exactly where
   bulk timing put it - only the remainder past the bus-advanced point is
   applied after). Opt-in per driver: `GameBoyAdvance::step` enables it
   around each instruction, unit (GbaMachine) runs keep bulk timing, so no
   existing timing check moved. This fixed the documented first-event
   caveat for free (timer writes already re-arm at write time; DMA/IRQ
   arming rides the same ticks). 3 new selftest checks pin it with a
   bulk-vs-ticked straddle (write-armed Timer0 lands at write+16, not
   start+16). Still open: value prefetch + overlap (slice 2).]
  [DONE open-bus BIOS values: locked reads return mGBA's `biosPrefetch`
  reset word (`0xE3A02004`, never updating since no real BIOS executes
  under HLE), sliced per width/address by the 16/32-bit paths composing
  bytes exactly like mGBA's Load8/16/32. Palette/VRAM/OAM verified
  correct-as-is per GBATEK (CPU access never loses data, only stalls).
  8 new selftest checks. `--dump-gba` still pixel-identical.
   Still open: unmapped-region latch (needs CPU pipeline state to track the
   last bus value) and the video-contention +1 (needs display-period
   awareness in the bus); both need suite ROMs to pin down.]
   [Investigated since, both confirmed blocked: mGBA's open-bus values ARE
   its prefetch queue (`prefetch[1]`, Thumb composed) plus the DMA bus
   latch - i.e. the latch item is the per-cycle-CPU/prefetch-queue project
   wearing a different hat - and the contention item is mGBA's
   `GBAMemoryStallVRAM` LUT, which needs live dot phase (the per-dot PPU
   project). No open-bus suite ROM is runnable here (no ARM toolchain to
   build mgba-emu/suite), and blind timing constants would regress the
   A/B-verified timing. Both stay parked behind their prerequisites.]
* [DONE] GBA save hardware accuracy: Flash byte program is AND-only
  (`FlashSave::programByte`, GBATEK "Flash Memory" programming model - bits
  clear, only an erase restores 1s; mGBA assigns unconditionally), the erase
  command set is GBATEK's (`AA 55 10` chip, `AA 55 80 AA 55 30@sector`, shared
  `eraseChip`/`eraseSector`; the old `EraseConfirm` swallowed the repeated
   `55` and dropped the `30`, and `0x10` chip erase was not decoded at all -
   plus since: the full AA 55 80 AA 55 10 chip-erase sequence died silently
   in `EraseConfirm` (which only forwarded `0x30`), so jsmolka save test 10
   polled forever; `0x10@0x5555` there now erases too),
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
  [DONE mgba-emu/suite reporting WITHOUT SIO: reading the suite sources
  showed the blocker note was wrong - the suite reports over the mGBA MMIO
  debug channel (0x4FFF600 string + 0x4FFF700 flags with the 0x100 publish
  bit + 0x4FFF780 enable answering 0x1DEA to 0xC0DE), plus an SRAM text log
  and an on-screen menu. `GbaBus` now implements the channel (byte-exact
  16-bit-store behavior incl. single-commit on the high byte, 256-byte cap,
  enable/buffer/flags serialized, log kept as runner output) with 13 new
  selftest checks, and `--gba-suite <rom> [suite] [frames]` drives the
  suite menu (DOWNs + A via the keypad, like `--dump-gba --input`) and
  reports the first `END: passed/total` (exit 0/1, blargg-style output).
  Pass/fail/timeout all verified live against hand-assembled ROMs (the
  investigation also proved the emulator innocent twice over: missing entry
  branch, then LDR offsets, then a log-window bug in the runner itself).
  jsmolka/gba-tests instead draws verdict text into mode-4 VRAM ("All tests
  passed" at (56,76), "Failed test NNN" at (60,76), font decoded) - a
  framebuffer-template runner, queued next.]
* [PARTIAL] Wire `GB4S v2` savestates into UI; run `sameboy/mgba-test-suite +
  gba-tests` in `--selftest` CI. The state format, the UI (8 slots,
  thumbnails, rewind ring) and the whole jsmolka sweep are done, and the CI
  half has landed too: a `conformance` job clones `mgba-emu/gba-tests` next
  to `retrio/gb-test-roms` and gates on all eight verdict-producing jsmolka
  ROMs plus the five `oam_bug` subtests and eight `dmg_sound` subtests that
  pass. The known failures (`oam_bug` 1/7/8, `dmg_sound` 07/09/10/12) run
  inside `::group::` blocks that *expect* red, so a new failure stands out
  instead of being lost in a wall of expected ones - the point of enumerating
  them is that "the suite is red" stops being a usable signal. Still open: the
  `sameboy` and `mgba-emu/suite` ROM sets (they are assembled from source and
  need an ARM toolchain the runner does not have), and GBA prefetch *overlap*
  timing (see the prefetch item above).
  [DONE the GB/CGB state format and the device layer (the UI came later): The GBA core
  already had `GB4S` v2 (`gba/state.h`); the GB core had NO serialization at
  all, so this was a new subsystem: `include/gb/state.h` (a `gb::StateBuffer`
  mirroring the GBA writer - duplicated rather than shared, because the GBA
  side is frame-exact A/B verified and one gb->gba include in nine of its
  files is a worse trade than 50 lines), then per-device `save`/`load` over
  the ~314 private fields: cpu (incl. `ei_delay_`/`halt_bug_`, which change
  what the NEXT instruction does), timer, joypad, mmu (banked WRAM, HDMA
  mid-transfer registers), ppu (VRAM banks, OAM, palettes, both timing
  counters and the rendered frame), apu (all four channels, the frame
  sequencer, the register shadows, the pending sample buffer and the
  band-limited output stage's ring/DC state), cartridge (bank registers, RAM,
  and the MBC3 clock). `GameBoy::saveState`/`loadState` are the file shell.
  12 new selftest checks, and the test is the interesting part: it snapshots
  MID-SCANLINE, runs 8 more frames uninterrupted, reloads and runs the same
  8 frames, then requires every frame to be pixel-identical and every audio
  sample identical. Comparing state bytes would pass even with a forgotten
  field, since a field that is never written is never compared; comparing
  what a player sees and hears catches it. Two more checks earn their keep:
  "the load consumed exactly the bytes the save wrote" (a save/load pair that
  disagrees about one field size silently shifts every later field) and a
  cartridge-RAM round trip. That byte-count check earned its place
  immediately - the first run failed with the APU silent after a reload,
  because `MMU::load` never read `wram_bank_` back (1 byte short, everything
  after it garbage). Audio was the only thing that noticed; video matched.
   STILL OPEN at the time: no UI, no rewind buffer, and `GameBoy::saveState`
   itself exercised only through the device layer, not end-to-end, because it
   needs a loaded ROM and that needs a window.
   [DONE UI since: 8 slots per game beside the ROM (`<rom>.gb4s0` ..
   `<rom>.gb4s7`, PPM thumbnails at `<rom>.gb4sN.ppm`), `1`-`8` selects the
   slot while a ROM runs (the menu keeps `1`-`8` for recent-ROM quick-launch),
   `F5` saves and `F8` loads, both routed through whichever core is running
   (GB/CGB state vs GBA state - the frontend's old `saveState` refused GBA
   mode outright), with a one-line status pill on the renderer
   (`flash_status`, the same clock-only pattern as the volume OSD, so it
   stays headless-safe) confirming "Saved/Loaded/No state in slot N". The
   slot keys join the input-suppression list so a remapped digit never double
   as a pad press, and the audio pacer resets on every load (the file shell
   already did for GB; the slot path does it for GBA too). 6 new selftest
   checks cover the slot-path helper (beside-the-ROM naming, distinct files,
   out-of-range/empty rejection). DONE since: auto-thumbnail
   `<rom>.thumb.ppm` on eject (same P6 pixels as slot thumbs, no user image
   needed) + `S:n` save badge in the menu (`RomEntry.save_slots` via
   `refresh_save_counts`) + dim `T` marker when only a `.thumb.ppm` exists
   (play-then-eject visibly marks the cell; footer hints `/:FILTER`).
   Esc in the menu clears an active filter (it was consumed by the global
   shortcut before `handle_menu_key` ever saw it). 10 new selftest checks.
   [DONE full image thumbnails: `render_menu` calls `resolve_thumbs` before
   `update_gui_buffers`, which rebuilds `thumb_draws_` every frame from the
   LRU, and `record_command_buffer_menu` draws each tile as a textured quad
   (same fullscreen-quad + viewport trick as the body photo, neutral filter)
   before the text pass, restoring the full viewport after. `--selftest`
   737/737.]
   [DONE rewind buffer on top of the slots: in-memory `saveMemory`/`loadMemory`
   on both cores (same bytes as the file shell, no disk I/O) feeding a 120-entry
   ring snapshotted every 30 frames (~60s); hold `U` to step back one entry per
   frame at 60Hz pacing, cleared on load/eject. 6 new selftest checks.]
   [DONE end-to-end coverage, no window needed: `GameBoy::load_test_bytes`
   (public headless seam wiring ROM bytes into the live core with none of
   `load_rom()`'s window/audio/config/GUI side effects; `saveState`/
   `loadState` promoted to public) plus `test_step_frames` drive the REAL
   file shell: GB warm/save/run/save/reload/run/save is byte-identical
   across machines, and bad-magic/truncated/version/missing/no-ROM inputs
   fail cleanly. GBA goes through the public core API (`loadFromBytes` +
   `saveState`) with the same oracle, matching except the scheduler's
   trailing `seq_` tiebreaker (re-armed on load, behavior-neutral - all
   other bytes and frames match; exact cross-load ordering would need
   per-event seqs in the format). HOME is sandboxed in-process so
   constructor/destructor config writes stay hermetic. 27 new checks.]
  The GBA test-ROM runner is UNBLOCKED for mgba-emu/suite (see above);
  [DONE jsmolka/gba-tests too: those ROMs need no menu driving and report
  no log - they draw verdict text into mode-4 VRAM ("All tests passed" at
  (56,76), "Failed test NNN" at (60,76)). `gba/test_verdict` template-matches
  both strings pixel-exactly (fail-closed to Unknown) against the 21-glyph
  verdict alphabet vendored from the ROMs' own font table (the repo's
  lib/glyphs.asm has since drifted for several lowercase glyphs - caught
  because the first run timed out on a real verdict screen). `--gba-tests
  <rom> [frames]` (default 600) reports Passed / FAILED test N / TIMEOUT
   with blargg-style output and exits. Verified live: memory.gba Passed (2
   frames); thumb.gba FAILED test 66 (genuine Thumb gap, fixed since - ROR
   carry, empty LDM/STM, STM base-in-list; thumb.gba now Passed, see the
   prefetch item above).
   [DONE the full jsmolka sweep (repo ships prebuilt ROMs; no assembler
   needed): arm/thumb/memory/sram/flash64/flash128/none ALL Passed. The
   arm run was a five-round driver like thumb: (1) test 234, the ARM7 "bad
   CMP" quirk - TST/TEQ/CMP/CMN with Rd=PC still restore CPSR from SPSR
   (no PC write, no flush); without it a psr test left the CPU in FIQ mode
   and eval crashed drawing 'F' (same signature every time: FIQ cpsr,
   SP=FFFFFFA4, slide through open bus from an LDM-pop to FFFFFFFC -
   diagnosed via a temporary trace dump plus the Div digits eval leaves in
   IWRAM, all removed after). (2) tests 360/361 + 412/413: pre-indexed
   loads write back BEFORE the transfer, so the loaded value wins Rn==Rd
   (mGBA order; word and halfword paths). (3) test 508: unaligned block
   bases align the transfer addresses down while writeback keeps the low
   bits (mGBA `address | addressMisalign`; SRAM excluded). (4) test 512:
   LDM^ without PC is the user-bank transfer (the old condition was
   inverted). The save suites added two bus fixes: halfword/word stores
   must reach write16/write32 unaligned (SRAM decodes the low bits for its
   8-bit-bus byte select; plain regions still align inside the bus), and
   the flash machine dropped full-sequence chip erase (AA 55 80 AA 55 10
   died in EraseConfirm, which only knew 0x30). 36 new selftest checks;
   `--selftest` 1031/1031. Three principled expected-fails remain, all
   verified by reading the test sources: bios test 1 needs the real BIOS
   ROM's opcodes in the prefetch latch (0xE129F000 - unknowable to an HLE
   without copying Nintendo's bytes); nes test 1 needs a value-prefetch
   queue (self-modifying code in VRAM - a real architectural project, not
   a constant); unsafe test 1 assumes a 32K SRAM chip and FAILS ON HARDWARE
   too (author's README - our default is 64K).]
   [DONE slice 2 since, value prefetch: the CPU keeps current+next fetched
   values queued (`pf_addr_/val_`, refilled ahead BEFORE each execution),
   so stores cannot rewrite already-fetched instructions - nes test 1 now
   passes, and with it the whole nes.gba. Timing untouched (fetch costs
   still charge once per executed instruction; refills are value-only raw
   reads through the same bus calls). Data writes never invalidate (real
   hardware doesn't either); the selftest harness invalidates explicitly
   when it plants code (reset/load clear it; determinism holds since the
   queue rebuilds from serialized pc+memory). 2 new selftest checks (Thumb
   + ARM self-modifying miniatures). Still open: overlap timing (needs A/B
   ROMs) and the open-bus value latch on top.]
  The work also fixed a REAL renderer bug the blank screen exposed: affine
  BG2PA/PD reset to 0, collapsing every bitmap/affine layer to column 0
  whenever a game (like these tests) doesn't program them - reset now
  leaves identity (post-logo hardware state), with a permanent mode-4
  ink/paper test. 45 new selftest checks (alphabet coverage, raw spot
  checks, pass/fail/digit-boundary/unknown/corruption/tiny/null cases).
  SIO itself stays open for multiplayer/serial accuracy (and the
  sio-read/sio-timing suites, which need a link partner by definition).]

### P2 — GUI / UX

Custom Vulkan immediate GUI (`GuiVertex`, `gb/font.h 8x8`, single
`assets/gbc_body.png`) is charming but expensive to extend:

* [PARTIAL] Adopt `Dear ImGui` (docking) on top of existing Vulkan pass for
  settings / ROM grid / remap / debugger. Keep `quad` pipeline for
  game + photo background. `Nuklear` as single-header fallback.
  Still open: the ROM grid and remap panels, real docking, and breakpoints
  (they need pause/step integration and a display to QA).
  [DONE the overlay itself: `gb4me_imgui` vendored (see P4); `src/vulkan/
  vulkan_renderer_imgui.cpp` inits the SDL3+Vulkan backends against the
  app render pass (backend-owned descriptor pool, auto font upload),
  draws a display-only `GB4ME Debug` window (FPS/frame/ROM/core/volume/
  filter/tex) at the end of all 3 record paths, `F3` toggles
  (`GB4ME_DEBUG_OVERLAY=1` forces it on for screenshots). Best-effort:
  init failure no-ops, overlay never captures input. Screenshot-verified
  over the ROM menu + `--selftest` 467/467.
  DONE since, first interactive controls: `NoInputs` dropped (kept
  `NoFocusOnAppearing`, so the keyboard stays with the game unless the
  window takes focus). The window gained Vol -/+, Filter/Grid/Ghost cycle
  buttons, Turbo latch and Pause/Resume, pushed as `ImguiAction`s drained
  once per frame through the keyboard-shortcut paths (volume save, filter
  dispatch, turbo/pause pills). Mouse captured by the window skips the
  console GUI hit-testing; keyboard capture suppresses only pad input
  (F3/Esc shortcuts still run). 4 new selftest checks (empty queue +
  capture flags headless).
  DONE since, full settings window: `GB4ME Settings` (toggle button in the
  debug window, own close box) with palette / hardware-mode / console cycle
  buttons reusing the keyboard-shortcut dispatch paths plus a ROM-folder
  readout with a Browse button into the existing folder-dialog flow; stats
  extended (palette, hwmode, console, folder). `palette_name` promoted from
  its anonymous namespace to shared header scope for the overlay. 3 new
  selftest checks (stats defaults, no queued actions).
   Still open: ROM grid / remap (diagram covers it) / debugger panels.]
   [DONE since, debugger registers + trace viewer: the `GB4ME Debug`
   window gained a CPU section (GB: AF/BC/DE/HL/SP/PC; GBA: R0-R15 + CPSR)
   plus the newest-first trace-ring slice in hex, fed by extended
   `ImguiStats` filled in `push_imgui_stats` for whichever core runs.
   Headless-covered via a new `debug_stats_for_test` seam (GB post-boot
   values + live PC tracking, GBA mode/CPSR/trace-ring range). 10 new
   selftest checks. No breakpoints (those need
   pause/step integration) - a follow-up needing a display to QA.]
   [DONE since, trace disassembly: minimal ARMv4T disassembler
   (`disasmArm`/`disasmThumb` in `src/gba/cpu.cpp`, mirroring the execute
   dispatch) rendering the trace-ring lines as `addr  opcode  asm` text in
   the viewer. 36 new selftest checks against real encodings pulled from
   the jsmolka ROMs.]
* [PARTIAL] Assets: `DMG + GBA + GBC` skins (not just purple CGB), dark/light theme,
  TTF (`Inter/Noto` via `stb_truetype`; `stb_image.h` already vendored),
  SVG icons (`Lucide/Phosphor`) replacing `OPEN` text + `*` gear hack.
  Still open: the DMG and GBA body-skin *photos* (the `BodySkin` table and
  every measured-geometry path are done and hold one GBC entry, so adding a
  skin is a table row plus its asset) and the wider SVG icon set.
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
  `--selftest` bake checks + screenshot-verified.
  DONE since, console accent themes: the `console_color` setting (saved,
  loaded, per-game-overridden but never rendered) now themes the settings
  panel - `color_for_console[_dim]` replace the hardcoded purple `kAccent`
  pair, with Purple byte-identical to the legacy values so the default look
  is unchanged. New `Console` settings row cycles the 5 colors (no core
  push needed; the renderer reads the live setting). 7 new selftest checks
   (table distinctness/opacity, legacy values, names, row presence/value).
   DONE since, dark/light interface theme: `UiTheme` tables (panel, menu
   rows, list text - dark is the legacy values verbatim) threaded through
   the settings panel and ROM-menu drawing; `theme` setting (Dark/Light,
   global, live for panel/menu) with settings-panel row + debug-window
   cycle button, and the ImGui overlay follows via StyleColorsLight/Dark.
   Photo-sited chrome (gear/OPEN/shoulder pills, badges, cart art) and the
   transient OSD pills stay dark - they sit on photos and game content.
   32 new selftest checks (dark-legacy literals, light/dark luminance
   contrast direction incl. mute-between-text-and-fill, row presence/value,
   file round-trip). Visual QA still pending - no display in this
   environment.
    Still open: photo body skins, SVG icons (the gear `*`
   glyph needs visual QA no headless harness can give).]
   [DONE since, body-skin infrastructure: `BodySkin` table (id, asset,
   aspect, all measured geometry - bezel, dpad, corners, pad buttons,
   shoulder pills) with the GBC photo as the only bundled entry; layout,
   buttons and hit-testing resolve through `active_skin()` so a second
   photo is a table row. `body_skin` setting (Auto/explicit id, global like
   input, applies on next launch since the texture loads once at startup),
   settings-panel row (shows the Auto target honestly: "Auto (Game Boy
   Color)") + debug-window cycle button, renderer loads the resolved asset
   with GBC fallback. 12 new selftest checks (resolution incl. unknown
   fallback, row presence/value, file round-trip). Pending with the second
   photo: the asset itself, its measured geometry, live texture reload,
   and screenshot QA.]
   [DONE since, vector gear icon: the settings `*` text hack is now a
   primitive-drawn gear (8 teeth + ring + hub, same grey/white chrome
   language as the folder glyph, sized to the old `*`); the OPEN folder
   glyph was already vector. No new selftest (GUI buffers need a device;
   build + selftest green). Visual QA still pending - no display in this
   environment.]
* [DONE] Shaders: add `slot-mask/grille/NTSC/xBRZ/LCD-ghosting` alongside existing
  `quad.frag:0-3 Nearest/Bilinear/Smooth/Scale2x-EPX` + LCD grid.
  [DONE slot-mask, grille, xBRZ: `VideoFilter::SlotMask` (rounded subpixel
  slots, smoothstep border, depth = the LCD Grid slider), `VideoFilter::Grille`
  (Trinitron RGB triads, weights normalised by the max so nothing clips or
  goes black at the stripe seams) and `VideoFilter::XBRZ` (the four reference
  blend kernels over the 3x3 neighbourhood with its per-channel equality
  tests, then a bilinear across the four sub-blocks - the 4x box-downsample
  without 16 fetches). All are plain smooth-bilinear when the slider is 0.
  All three screenshot-verified on Wario Land.
  DONE since NTSC: `VideoFilter::NTSC` (mode 7, single pass, deliberately fake
  per the open question - 3-tap horizontal bandwidth limit, sharp luma +
  blurred chroma, alternating-column edge rainbow; linear-light approximation,
  LCD grid still applies on top). Enum value IS the shader mode (selftest pins
  0-7 + the NTSC name; filter cycle/settings pick it up via `Count`).
  DONE since LCD-ghosting: no GPU history buffer needed - the persistence
  lives on the CPU (`VulkanRenderer::ghost_prev_` + `ghostBlend`, IIR feedback
  for exponential trail decay, gamma-space blend documented as approximate).
  New `ghosting` display setting (0..3 Off/Light/Medium/Strong, depths
  0/0.30/0.50/0.70) with settings-file key, settings-panel row + `Ghosting`
  cycle action, per-game `<rom>.cfg` overlay support, and `setGhosting` pushed
  through all three display paths; history resets on ROM load/eject and seeds
  unblended on size change. 17 new selftest checks (blend math, depth/clamp,
  file round trip, overlay apply/restore).]
* [DONE] Features: recent-ROMs, `rom.png` boxart sidecar, search/filter,
  save slots `F1-F8 + thumbnails`, fast-forward (`Tab`), rewind buffer,
  turbo, cheats (`GameShark/ActionReplay`), volume OSD.
  [DONE save slots, in `1`-`8` + `F5`/`F8` form rather than literal `F1`-`F8`
  (`F3` already owns the debug overlay): 8 `<rom>.gb4sN` states + PPM
  thumbnails beside the ROM, both cores, status-pill confirmation - see the
  GB4S item above.
  [DONE `rom.png` boxart: `thumb_path_for` prefers `<rom>.png` over auto/slot
  shots (both cores resolve through the same LRU tile path); precedence covered
  by selftest.]
  [DONE rewind buffer: hold `U` in-game, 30f/120-entry ring, see the GB4S item.]
  [DONE turbo latch: `T` toggles fast-forward on/off (hold `Tab` still works),
  reserved/suppressed so it never drives the pad, `Turbo ON/OFF` status pill.
  Footer hints `T:TURBO`.]
  [DONE cheats: `<rom>.cheats` beside the ROM, `C` toggles the active core.
  GB/CGB (`gb/cheats.h`): `ADDR=VALUE` raw + `01YYXXXX` GameShark type 01,
  patched in `MMU::read` (IO `FF00-FF7F` excluded). GBA (`gba/cheats.h`):
  `AAAAAAAA=VV/VVVV/VVVVVVVV` raw 8/16/32-bit, width-matched, patched in
  `GbaBus::read8/16/32` (IO/BIOS/open/EEPROM/GPIO excluded). One file serves
  both cores (each parser keeps its own address width); encrypted AR/CB codes
  unsupported. 20 + 14 new selftest checks.]
  [DONE search/filter: `/` activates a case-insensitive substring filter over
  title+path (`GUIConsole::menu_filter_`, `filtered_` index map), printable
  appends, Backspace deletes, Esc clears; `R`/1-8 shortcuts suspend while
  filtering, Enter/click load via `filtered_to_rom`/`selected_rom`. Renderer
  shows `SELECT ROM (n)` + `FILTER:/...` hint and `NO MATCHES` empty state.
  12 new selftest checks.
  DONE since, layout-aware typing + shortcut ownership: filter text arrives
  as SDL text input (`GUIConsole::menu_filter_type`: `/` starts a search on
  any layout, Space never types since Space loads, idle text ignored), so
  AZERTY/IME spell correctly where scancode arithmetic (physical positions)
  could not. While a filter is active the menu owns every key: `handle_events`
  routes to `handle_menu_key` before the `O`/`S`/`T`/`U`/`C` shortcuts (Tab
  keeps fast-forward, F3 still toggles), and text input starts/stops with the
  search session (activation, Esc, ROM load). 11 new selftest checks.]
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
  [DONE turbo = true FF in game loop: the `delay=0` fast path used to be
  raised back to `floor_delay`, pinning FF to 60 FPS in-game (menu already
  skipped). Now FF skips both pace and floor, still feeding the resampler.
  DONE since: `T` latches turbo (`ff_toggle_`, focus-loss safe, `Turbo ON/OFF`
  pill) so grinding needs no held key; hold `Tab` unchanged.]
* [PARTIAL] Input: rumble passthrough (`MBC5`/`Pak13`), WarioWare gyro, profile OSD.
  Still open: the WarioWare gyro (it needs a real motion source - the Twisted
  ADC is protocol-complete but driven by two keys) and GPIO3 rumble.
  [DONE rumble: `MBC5::rumble_` records bit 3 of a 0x4000-0x5FFF write (the
  bit is stripped from the RAM bank, as before), `Cartridge::has_rumble()`/
  `rumble()` forward it, and the frontend drives the first rumble-capable
  pad with `SDL_RumbleGamepad` - stopping on shutdown and only ever from a
  cartridge that has a motor. 6 new selftest checks.
   DONE profile-switch OSD: profile activate/delete/load flash
   `Profile: <name>` via `renderer.flash_status` (same clock-only pill as
   volume/slots, headless-safe). Still open: WarioWare
   gyro (needs a motion source).]
   [DONE since, Twisted gyro ADC: GBATEK documents the whole protocol (an
   ADC bit-banged over GPIO0 start / GPIO1 clock / GPIO2 data: 4 dummy
   zeros then 12 data bits MSB-first), so this one was implementable
   without the game. `GpioRtc` gained the ADC engine (P0-rise latches the
   sample, P1-rise presents the next bit on input-direction P2, RZW carts
   get GPIO+gyro via detect), the sample centers at 0x800 with frontend
   tilt on fixed COMMA/PERIOD keys (outside the remappable pad map, held
   state cleared on focus loss/load/eject), serialized with the GPIO state.
   Rumble (GPIO3) stays unmodelled. 13 new selftest checks (RZW detect,
   full bit-bang conversion incl. re-latch, tilt mapping/clamps,
   mid-conversion save-state resume). No Twisted ROM to playtest with -
   protocol-conformant by construction.]
  [DONE fullscreen chrome: `L`/`R` shoulder pills and the `*` gear text
  lingered on top of the game in full-window mode - the fullscreen record
  path never rebuilt the GUI/text buffers, so the last console frame's
  overlay persisted. `render`/`render_gba` now take the console and rebuild
  OSD-only (`chrome = !gui.fullscreen_game()` guards shoulders, highlights,
  menu, open/gear, settings, modals); volume/status pills still show
  fullscreen. No new selftest (buffers need a device; verified by build +
  visual QA).]

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
* [PARTIAL] Still open: cache static menu verts + sub-rect texture updates instead of
  full staging upload. (Tracy hooks are in; the upload work is not started.)
  [DONE Tracy stubs: `include/gb/tracy.h` no-op macros + `GB4ME_TRACY` CMake
  option (uses `<tracy/Tracy.hpp>` when present, else no-ops), zones in
  `run`/`step_frame`/`step_gba_frame`. Sub-rect deferred: game frames are
  92-153KB full-upload per frame, diffing costs more than it saves.]

### P4 — Build / platform / maintainability

* [PARTIAL] `CMake`: `find_package(SDL3 CONFIG)`, `FetchContent` fallback, `vcpkg`
  manifest, `ccache`, `clang-tidy`, `ASan/UBSan` preset, CI
  (`linux/windows/macos`) running `--selftest`. Everything except the
  Windows/macOS runners is done; the blockers for those are listed at the
  bottom of this section, in the order they will bite. [DONE SDL3 discovery +
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
* [DONE] Packaging: shader + desktop + icon install
  [`CMakeLists.txt` installs the built `*.spv` to `share/GB4ME/shaders`
  plus `GB4ME.desktop` and the icon, matching the `/usr/share/GB4ME/shaders`
  lookup already in the pipeline candidates. Packaging/distribution beyond
  that is explicitly out of scope - do not add flatpak, AppStream, or
  similar without being asked.]
* [DONE] Replace custom `settings.cfg` parser with versioned schema; audit
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
