# Game Boy Color Emulator GUI + CGB Core — Agent Instructions

## Project Stack
- **Language**: C++23
- **Window/Input/Rendering**: SDL3 + Vulkan
- **UI Framework**: Custom SDL3 rendering
- **Build System**: CMake 3.28+
- **Target**: Game Boy Color emulator GUI with working CGB core + GBA emulation

## Project Goal
GB4ME is a Game Boy Color emulator that already has a substantially complete GBA implementation. The GBA core uses an internal HLE BIOS (no external dump required) and implements the ARM7TDMI CPU, GBA memory bus, PPU, DMA, timers, IRQ controller, audio, keypad, cartridge save hardware, and save states.

**Current focus**: All green (444/444 bare, 446/446 GB·GBC, 447/447 GBA smoke,
Sep 23 2026). Full-audit fixes: affine BG map indexing, PSG 4x frequency
error, OBJ vertical wrap, SRAM 8-bit bus, EI ordering, renderer/bus perf.

---

## Phase 1: Project Discovery — COMPLETE

### Repository Structure
```
.
├── CMakeLists.txt
├── include/
│   ├── gb.h
│   ├── gb/          # GB/GBC headers: gameboy.h, cpu.h, mmu.h, ppu.h, apu.h, cartridge.h, joypad.h, input.h, timer.h, settings.h, types.h, vulkan_renderer.h, font.h, gui_console.h
│   └── gba/         # GBA headers: core.h, cpu.h, bus.h, bios.h, irq.h, timers.h, dma.h, ppu.h, audio.h, cartridge.h, save_hardware.h, state.h, scheduler.h, keypad.h
├── src/
│   ├── main.cpp
│   ├── selftest.cpp / selftest.h   # Self-test framework (205 checks, exit 0=pass, 1=fail)
│   ├── stb_image.h
│   ├── core/         # GB core: cpu.cpp, joypad.cpp, timer.cpp, mmu.cpp
│   ├── gba/          # GBA implementation (17 source files)
│   ├── ppu/          # GB PPU: ppu.cpp
│   ├── ui/           # GUI: gameboy.cpp, gui_console.cpp, input.cpp
│   ├── vulkan/       # Vulkan renderer: vulkan_renderer.cpp
│   └── cartridge/    # ROM loading: cartridge.cpp
├── build/            # Build output + test binaries
├── assets/
├── shaders/
└── AGENTS.md
```

### Build & Test
```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j4   # binary lands in bin/
./bin/GB4ME --selftest               # 444 hardware checks, headless
./bin/GB4ME [rom.gb|.gbc|.gba]       # GUI mode
```
Layout rule (Sep 23 2026, user order): `build/` holds ONLY build artifacts
(CMake cache, objects); the compiled binary + shaders go to `bin/` via
`CMAKE_RUNTIME_OUTPUT_DIRECTORY`. Never copy the binary anywhere by hand.

### Current Self-Test Status
- **444 checks total, 0 failures** (SELFTEST PASSED); **446/446** with GB/GBC boot
  smoke (Wario Land .gb, Zelda Oracle of Ages .gbc); **357/357** with GBA boot
  smoke (Golden Sun, Megaman BC Challenge, Pokemon Emerald, pokeemerald) — Sep 24 2026
- GB/GBC tests: all passing; GBA tests: all passing
- ctest: NO TESTS (tests/ directory absent from tree; "No tests were found")

### Active Todo List (in order)
1. ~~Fix BIOS Sqrt fixed-point return value (SWI 08h)~~ — DONE
2. ~~Fix BIOS ArcTan polynomial and ArcTan2 edge cases~~ — DONE (also fixed test values: 0x4000 = 1.0 in 1.14 format, not 0x10000)
3. ~~CpuSet/CpuFastSet, BgAffineSet/ObjAffineSet, Div r2, MSR flags~~ — DONE (already passing, no code change needed)
4. ~~Fix IRQ delivery timing — VBlank IRQ not firing~~ — DONE (test IRQ handler LDR offset #24 -> #32)
5. ~~Fix IntrWait halting behavior — SWI 04h not halting~~ — DONE (test SWI encoding 0xEF000004 -> 0xEF040000 + IE/DISPSTAT setup; core polls wait after final halt slice)
6. ~~Fix VBlank DMA trigger timing and IF raise on DMA3 completion~~ — DONE (test 32-bit encodings (count<<16)|control -> (control<<16)|count)
7. ~~Fix timer counter reload and CNT_L write visibility~~ — DONE (code prescaler() uses control bits; test expects reload 0xFF00 and frozen counter 0x0000 per GBATEK)
8. ~~Rebuild and run selftest to verify all fixes~~ — DONE (superseded: 444/444 selftest today; tests/ dir removed)

### Key Implementation Files (Input)
- `include/gb/input.h` — `InputManager`, `PadButton`, `BindKind`, `Binding`,
  `BindingTable`, `PadDevice`; `pad_button_name()` / `pad_label()`
- `src/ui/input.cpp` — hotplug, raw state, per-frame aggregate, `autoMap()`,
  `applyTo()` with the explicit Joypad/PadButton mapping table
- `include/gb/gui_console.h` — `InputProfile` + `Settings::profiles` /
  `active_profile` / `addProfile` / `renameProfile` / `deleteProfile` /
  `activateProfile` / `syncActiveProfile`; `currentBinding` / `setBinding` for
  the one-slot-per-action model; `RowStyle`; the text-entry prompt
- `src/ui/gui_console.cpp` — profile serialization, `settings_rows()` /
  `settings_hit_test()` for both tabs, `rebuild_settings_cells()` (panel
  anchored below the LCD glass, height depends on the tab)

### Key Implementation Files (GBA)
- `src/gba/core.cpp` — GameBoyAdvance: owns CPU + HLE BIOS, wires bus/scheduler/cartridge/peripherals
- `src/gba/cpu.cpp` — Arm7Tdmi: register banking, CPSR/SPSR, exceptions, shifter, step loop
- `src/gba/cpu_arm.cpp` — ARM (32-bit) decoder + executor
- `src/gba/cpu_thumb.cpp` — THUMB (16-bit) decoder + executor
- `src/gba/bus.cpp` — GbaBus: region decode, RAM backing, ROM mirror, save routing, WAITCNT, EEPROM
- `src/gba/bios.cpp` — HleBios core: reset, startup, SWI dispatcher, IntrWait wait logic
- `src/gba/bios_system.cpp` — SWI 00h-05h, 0Dh, 19h+ (SoftReset, Halt, Stop, IntrWait, VBlankIntrWait, GetBiosChecksum, sound, stubs)
- `src/gba/bios_math.cpp` — SWI 06h-0Ah, 1Fh (Div, DivArm, Sqrt, ArcTan, ArcTan2, MidiKey2Freq)
- `src/gba/bios_memory.cpp` — SWI 0Bh-0Ch (CpuSet, CpuFastSet)
- `src/gba/bios_decomp.cpp` — SWI 10h-18h (BitUnPack, LZ77, Huffman, RL, Diff filters)
- `src/gba/bios_affine.cpp` — SWI 0Eh-0Fh (BgAffineSet, ObjAffineSet)
- `src/gba/irq.cpp` — GbaIrq: IE/IF/IME, level-triggered IRQ delivery
- `src/gba/timers.cpp` — GbaTimers: TM0-3, prescalers, cascade, overflow IRQ
- `src/gba/dma.cpp` — GbaDma: DMA0-3, immediate/blanking/FIFO, repeat, IRQ
- `src/gba/ppu.cpp` — GbaPpu core: video memory, LCD registers, scanline timing, IRQ/DMA events
- `src/gba/ppu_render.cpp` — Scanline renderer: text/affine/bitmap BGs, sprites, windows, blending, mosaic
- `src/gba/audio.cpp` — GbaAudio: 4 PSG channels + 2 Direct Sound FIFOs
- `src/gba/cartridge.cpp` — GbaCartridge: ROM loading, save-type detection, header parsing
- `src/gba/save_hardware.cpp` — SaveHardware: SRAM, Flash (Panasonic/Sanyo/Macronix/Atmel), EEPROM
- `src/gba/scheduler.cpp` — GbaScheduler: cycle-based event queue
- `src/gba/keypad.cpp` — GbaKeypad: KEYINPUT, KEYCNT, IRQ

### Key Header Files (GBA)
- `include/gba/core.h` — GameBoyAdvance class (owns all subsystems)
- `include/gba/cpu.h` — Arm7Tdmi, CpuMode, CpuState, CpuException, CPSR flags
- `include/gba/bus.h` — GbaBus, region enum, memory map constants, WAITCNT
- `include/gba/bios.h` — HleBios, SWI numbers (0x00-0x2A), Result enum
- `include/gba/irq.h` — GbaIrq, IRQ bit assignments (VBlank, HBlank, VCount, timers, DMA, keypad, etc.)
- `include/gba/timers.h` — GbaTimers, prescaler/cascade/IRQ bits
- `include/gba/dma.h` — GbaDma, channel structure, timing enum
- `include/gba/ppu.h` — GbaPpu, display modes, frame data, LCD registers
- `include/gba/audio.h` — GbaAudio, PSG + FIFO structures
- `include/gba/cartridge.h` — GbaCartridge, header, save detection
- `include/gba/save_hardware.h` — SaveHardware interface, SRAM/Flash/EEPROM implementations
- `include/gba/state.h` — StateBuffer, kStateMagic, kStateVersion
- `include/gba/scheduler.h` — GbaScheduler, event queue
- `include/gba/keypad.h` — GbaKeypad, KEYINPUT/KEYCNT addresses

### Reusable Abstractions
- `gb::types.h` — shared u8/u16/u32/u64/i8/i16/i32/i64/f32/f64, SCREEN_WIDTH/HEIGHT, HardwareMode enum, Vec2/3/4, Mat4
- `gba::state.h` — StateBuffer serialization (used by all GBA subsystems + save hardware)
- `gb::HardwareMode` — DMG/CGB_Compatible/CGB_Only enum (GBA mode is separate via gba::GameBoyAdvance)

### GB/GBC Architecture Summary
- CPU: SM83 (LR35902) in `gb::CPU` with MMU delegation
- MMU: `gb::MMU` bus with banked WRAM/VRAM, CGB register stubs, HDMA
- PPU: `gb::PPU` with DMG + CGB rendering paths, palettes, tile attributes
- APU: `gb::APU` with 4 channels, frame sequencer
- Cartridge: `gb::Cartridge` with MBC1/2/3/5, save RAM, CGB flag detection
- GameBoy: `gb::GameBoy` owns all subsystems, VulkanRenderer for display, GUI console

### How GBA Integrates
- `gb::GameBoy` has a `gba::GameBoyAdvance gba_` member and `gba_mode_` flag
- `step_gba_frame()` runs the GBA core to the next frame
- GBA uses its own timing model (cycle-based scheduler) separate from GB's fixed M-cycle-per-frame
- Frontend routes .gba files to GBA core, .gb/.gbc to GB core
- VulkanRenderer has `render_gba()` for 240x160 RGBA8 frames vs `render()` for 160x144 GB frames

---

## Phase 2: Reference Research — IN PROGRESS

### Primary References
- **GBATEK**: http://problemkaputt.de/gbatek.htm — primary GBA hardware/register documentation
- **mGBA GBATEK repo**: https://github.com/mgba-emu/gbatek — maintained markdown version
- **mGBA**: https://github.com/mgba-emu/mgba — primary implementation reference (cloned to /tmp/mgba)
- **mGBA test suite**: https://github.com/mgba-emu/suite
- **VBA-M**: https://github.com/visualboyadvance-m/visualboyadvance-m — independent reference
- **SameBoy**: https://github.com/LIJI32/SameBoy — GB/GBC accuracy reference (not GBA)
- **Pan Docs**: https://gbdev.io/pandocs/ — GB/GBC documentation

### Local Reference Copies (in-repo, git-ignored — re-fetch with the commands below)
- `docs/references/gbatek` — gbatek markdown repo (`git clone --depth 1 https://github.com/mgba-emu/gbatek`)
- `docs/references/mgba` — mGBA source, sparse checkout of `src/arm src/gba include/mgba`
  (`git clone --depth 1 --filter=blob:none --sparse https://github.com/mgba-emu/mgba`,
  then `git sparse-checkout set --cone src/arm src/gba include/mgba`)
- `docs/references/sameboy` — SameBoy source (GB/GBC accuracy + graphics).
  `git clone --depth 1 https://github.com/LIJI32/SameBoy docs/references/sameboy`
  (cloned Sep 24 2026 at 213a12c). **Graphics reference only** — it has NO
  GBA PPU: its `GB_MODEL_AGB_*` models still output the 160x144 GB LCD, so only
  its *display-technique* work transfers to GBA, not its video hardware.

### Reference Usage Rules
- Do NOT blindly copy code from mGBA/VBA-M/SameBoy
- Use references to determine hardware behavior, timing, register semantics, edge cases
- Prefer behavior supported by hardware documentation, dedicated tests, multiple implementations
- Record ambiguities and resolve deliberately

---

## Phase 3+: Implementation Status

### GBA Subsystems Implemented
- **CPU**: ARM7TDMI with ARM + THUMB decoders, register banking, CPSR/SPSR, all exception types, barrel shifter, unaligned memory access semantics, cycle accounting (S=N=I=1, WAITCNT-ready shape)
- **Memory/Bus**: Full GBA address map, BIOS region with HLE protection, EWRAM/IWRAM, palette/VRAM/OAM, ROM mirrors, SRAM/Flash/EEPROM save routing, 0x0D EEPROM serial, WAITCNT, access cycle calculation (N/S waitstates, region-specific timings), open-bus for unmapped
- **BIOS/HLE**: No external dump required. Internal HLE BIOS with SWI dispatcher, exception vectors, IRQ trampoline, post-BIOS startup state (System mode, ARM state, I+F clear, SP_usr, zeroed IWRAM system area, POSTFLG=1, IE/IF/IME cleared). SWIs implemented: Div, DivArm, Sqrt, ArcTan, ArcTan2, GetBiosChecksum, CpuSet, CpuFastSet, BgAffineSet, ObjAffineSet, BitUnPack, LZ77UnCompWram, RlUnCompWram, Diff8bitUnFilterWram, plus system stubs (SoftReset, Halt, Stop, IntrWait, VBlankIntrWait, sound driver stubs, MultiBoot, HardReset, CustomHalt)
- **Interrupts**: IE/IF/IME, all 14 IRQ sources, level-triggered delivery, IE gating, CPSR.I masking, IntrWait-family wait logic, VBlank/HBlank/VCount IRQ timing
- **Timers**: TM0-TM3, prescalers (1/64/256/1024), reload values, overflow IRQ, cascade (TM1-3 count up on previous overflow), enable/disable, timer 0/1 overflow hooks for audio FIFO
- **DMA**: DMA0-DMA3, immediate/VBlank/HBlank/Special timing, source/destination addressing with MSBs ignored, increment/decrement/fixed/reload destination, repeat, IRQ on completion, FIFO A/B for sound DMA (DMA1/2, Special, repeat), EEPROM serial support (DMA3, one halfword per bit), pending cycles for CPU stall
- **PPU/Video**: Display modes 0-5, text backgrounds (mode 0), affine backgrounds (mode 1-2), bitmap modes (mode 3/4/5), tile maps, character/tile data, palettes (BG + OBJ, 16-bit entries, RGB555), sprites/OBJ (128 sprites, attributes, affine sprites with rotation matrices, X/Y flip, mosaic, priority, semi-transparent), windows (WIN0/WIN1/OBJWIN with region enables), blending (alpha, addition, subtraction), brightness, mosaic (BG + OBJ, horizontal/vertical), priority (BG < 4, OBJ on tie, lower OAM index on OBJ tie, backdrop last), backdrop color, scanline timing (1232 dots/line, 1008 draw + 224 blank, 228 lines, 280896/frame), HBlank/VBlank events, VCOUNT, DISPSTAT flags/IRQ enables/LYC, LCDC-related interrupts, VRAM/OAM access rules (8-bit VRAM replicates except OBJ VRAM, 8-bit OAM ignored, 16/32-bit native), green swap effect
- **Audio**: 4 PSG channels (pulse 1/2, wave, noise) with GB-derived rates, envelope/length/sweep timers at nominal GBA rates (256Hz length, 64Hz envelope, 128Hz sweep), wave channel with 2 banks (64-digit mode), Direct Sound FIFO A/B (32-byte FIFO, timer-driven consumption via DMA1/DMA2, 4x32-bit transfers per request), SOUNDCNT_H mixing/volume/timer-select/FIFO-reset, SOUNDBIAS, mixing to stereo SDL output at 44.1kHz, high-pass DC blocking
- **Input**: KEYINPUT (active-low, 10 buttons: A/B/Select/Start/Right/Left/Up/Down/R/L), KEYCNT interrupt control, key state tracking, keypad IRQ with mGBA GBATestKeypadIRQ-compatible evaluation (including firing on empty AND mask)
- **Cartridge**: ROM loading with header detection (title, game code, maker code, fixed byte 0x96, version, checksum), save-type detection (SRAM, Flash Panasonic/Sanyo/Macronix/Atmel, EEPROM 4Kbit/64Kbit/512Kbit), save hardware polymorphism (SaveHardware interface), EEPROM serial engine (DMA3 path, bitstream protocol with address/data/stop bits, self-synchronizing on transfer count), save/load battery-backed files
- **BIOS/HLE**: See above — no BIOS dump required, internal HLE with SWI dispatcher, vectors, IRQ trampoline, post-BIOS startup state, required SWIs
- **Save States**: StateBuffer serialization for all subsystems (bus, cartridge, CPU, PPU, DMA, timers, IRQ, audio, keypad, BIOS, scheduler), magic + version header, save/load to file

### Input (keyboard + controller) + settings UI
- **`include/gb/input.h` + `src/ui/input.cpp`** — `InputManager` owns ALL gameplay
  input for both cores. Ten logical buttons (`PadButton`: A,B,Select,Start,
  Right,Left,Up,Down,R,L) in the same order as `gba::GbaKeypad::Key`.
- **Unified, device-agnostic path**: SDL keyboard events and SDL gamepad
  events feed one `InputManager`. The logical state is RECOMPUTED every frame
  from raw device state (keyboard OR controller OR secondary stick), not
  latched on events — so two devices cannot clear each other and a lost
  key-up cannot leave a button stuck. `applyTo()` pushes the result into
  both `gb::Joypad` and `gba::GbaKeypad` once per frame.
- **`Joypad::Key` and `PadButton` have DIFFERENT enum orders**
  (`Joypad::Key` = Right,Left,Up,Down,A,B,Select,Start; `PadButton` =
  A,B,Select,Start,Right,Left,Up,Down,R,L). `applyTo()` maps them through
  an explicit table — a straight cast silently sends A to Right. The
  selftest catches this (4 checks fail if the table is replaced by a cast).
  `GbaKeypad::Key` DOES share `PadButton`'s order, so that one is a direct cast.
- **Bindings**: `Binding` is a packed `(kind<<16)|code` pair, where kind is
  None/Key/Button/AxisNeg/AxisPos. Each button has a keyboard binding plus TWO
  controller bindings (`pad_map` + `pad_map_alt`), so auto-map can bind both the
  d-pad and the left stick to a direction. Serialised as `pad0..pad9` /
  `alt0..pad9` in the settings file and round-tripped by `pack`/`unpack`.
- **THE UI SHOWS ONE BINDING PER ACTION**, the way RPCS2/PCSX2/Dolphin present
  controls: a list of actions, each with a single slot, and pressing the row
  arms a capture that accepts *either* a key or a controller button. The device
  is NOT a separate list. `Settings::setBinding()` enforces one source per
  button (assigning a key clears that button's pad bindings and vice versa);
  `currentBinding()` resolves key > pad > secondary for display. The previous
  split "KEYBOARD" / "CONTROLLER" halves and the separate Profiles tab were
  removed on user correction — they were the unusual layout.
- **THE INPUTS TAB IS A GBA DIAGRAM, not a list of rows** (user request). The
  page is: a `Profile` combo box with `Load` / `Save`, a `Device` combo box, an
  optional `Auto-map to controller` button, and below that an outline/silhouette
  drawing of a GBA. The drawing's D-pad, A/B circles, START/SELECT pills and
  L/R shoulder tabs are each a hit region, so you click the button you want to
  rebind exactly as you click the GBC body photo — no list of labels at all.
  Layout lives in `GUIConsole::GbaPad` and `hit_gba_pad()`; the D-pad cross
  resolves to the dominant axis so diagonal clicks still pick a direction.
  * **The outline is laid out in REAL MILLIMETRES**, not eyeballed fractions:
    the AGB-001 is **144.5 x 82 mm** with a **61.2 x 40.8 mm** screen
    (Nintendo tech data / GBATEK), so the body is 1.762:1 and the screen 3:2.
    Control positions follow GBATEK's layout sketch: D-pad at (20,50) mm on the
    far left, A/B right of the screen with B lower-left, START/SELECT below
    the screen (SELECT left), L/R as tabs on the top edge, plus the cartridge
    slot, power switch and the six diagonal speaker holes. Selftests assert the
    two aspect ratios and the relative control positions, so it cannot drift.
  * **The D-pad centre bug** (found by the user: clicking it did nothing): the
    hit region used `ccx = (h.x + h.w) * 0.5f`, which is the RIGHT EDGE, not
    the centre — the correct midpoint is `h.x + h.w * 0.5f`. The whole region
    was therefore shifted off the cross and every arm click missed. Pinned by
    4 hit tests that fail when reverted. The armed highlight also lights only the
    pressed ARM (top/bottom half of the vertical bar, left/right half of the
    horizontal one) instead of the whole cross, so the direction you just bound
    is unambiguous.
- **Device combo box** (`GUIConsole::Dropdown`): entry 0 is always `Keyboard`,
  then one entry per connected controller (by name). With keyboard only it is
  **disabled and locked to Keyboard**, so it is obvious why it will not open
  rather than silently ignoring clicks. The selection is what the armed button
  captures (`captureWantsKey()`). A controller that disappears falls back to
  Keyboard instead of leaving a dangling selection.
- **Profile combo box**: lists every profile then three trailing commands
  (`+ New profile...`, `Rename this...`, `Delete this`).
  * **The menu-boundary bug** (found by the user: the commands did nothing):
    `profileMenuSize()` added `kProfileMenuSpecials` to `options.size()` while
    `options` ALREADY contained those three entries, so the command boundary
    was `profileCount() + 3` instead of `profileCount()`. Clicking
    "+ New profile..." therefore fell into the profile branch and called
    `activateProfile(1)`, which failed silently. `profileCommandIndex()` is
    now the single source of the boundary and is pinned by tests.
  Selecting a profile
  activates it immediately; `Load` re-applies it (discarding live edits) and
  `Save` commits the live mapping into it. A drop-down that would leave the panel
  **flips above** its box instead of clipping.
- **Inputs page is NOT drawn by the generic row loop**: `settings_rows()`
  returns nothing for it, and the renderer draws the widgets and the diagram from
  their own rects, so the diagram can use circles and outlines that a
  label/value row cannot express.
- **Frontend key suppression**: a key the frontend owns (F/F11 fullscreen,
  S save, O open, E export, P screenshot, +/- volume, Esc) or any key pressed
  while a modal is up (no ROM loaded, settings panel, remap pending) is marked
  `setSuppressed()` so it can never also drive the game. Suppression is cleared
  on EVERY key-up so it stays symmetric even when the modal state changes
  between down and up. (`Joypad::key_down()` was added as a direct,
  unmultiplexed state accessor so this is testable without driving P1.)
- **One-click defaults, context-sensitive on the selected device.** A single
  button above the diagram:
  * **Keyboard selected → "Reset to defaults"** — restores the factory keyboard
    layout via `Settings::applyDefaultKeyMap()`. It also CLEARS the controller
    bindings, because a button has exactly one source: a keyboard layout is a
    replacement, not an addition.
  * **Controller selected → "Auto-map to controller"** — conventional layout:
    face buttons to A/B, shoulders to L/R, START/BACK to Start/Select, d-pad to
    the directions, and the left stick mirrored as the secondary source.
  * The button is ALWAYS visible (both devices have a default) and shows
    "Applied" in green after a successful run. `autoMapLabel()` picks the wording
    so the label always matches what the click will do.
  * `Settings::defaultKeyMap()` is a single source of truth shared by the
    constructor and the reset, so the factory layout cannot drift. Note R is
    bound to D and never S, because S is a frontend shortcut.
- Axis activation threshold sits well inside stick travel so a resting stick
  never triggers a direction. Controller removal (and window focus loss) calls
  `releaseAll()` so held buttons cannot stick.

### Input profiles
- **Named mappings the user can switch between**, so setting up a controller
  once does not have to be repeated. Lived on their own `SettingsTab::Profiles`
  until the user asked for the RPCS2/PCSX2 layout; the profile picker and its
  three commands now sit at the TOP and BOTTOM of the single **Inputs** tab.
- **`InputProfile` = name + `key_map` + `pad_map` + `pad_map_alt`** — the exact
  same three tables, so a profile is just a saved state of the live mapping.
- **`profiles[0]` is always "Default"** and cannot be deleted or renamed, so the
  original mapping always remains reachable. Cap is
  `Settings::kMaxProfiles` (16, including Default).
- **The active profile is mirrored into the live `key_map`/`pad_map*` fields**,
  which stay the single source of truth for gameplay and for the Inputs tab.
  Activating copies profile -> live; nothing else in the codebase needed to
  change.
- **Edits commit to the active profile**: `syncActiveProfile()` runs on
  `activateProfile()` (switching away) and on `save()`. So remapping while a
  profile is selected updates THAT profile instead of being silently discarded,
  and it survives a restart. "Default" is an ordinary editable profile in this
  respect — only its name and existence are protected.
- **Naming uses a real text prompt** (`SDL_StartTextInput` /
  `SDL_EVENT_TEXT_INPUT`, so it respects the keyboard layout and IME), filtered
  to printable ASCII because the overlay font has no other glyphs. Backspace
  edits, Enter accepts, Esc cancels. While it is open the keyboard is
  suppressed from gameplay.
- **Persistence**: `profN_name` / `profN_keyM` / `profN_padM` / `profN_altM`
  (N 1-based, so `prof1` is Default) plus `active_profile`. The bare
  `key0..9`/`pad0..9`/`alt0..9` lines are still written with the ACTIVE
  profile's values, so an older build that does not understand profiles still
  reads the live mapping. A pre-profiles settings file loads unchanged and
  becomes Default.
- **Two subtle traps that the selftests pin** (both were real bugs):
  (1) the save loop must run `p = 1 .. profileCount()` and index
  `profiles[p-1]`, or the last profile is dropped and Default is never written;
  (2) `load()` must adopt `prof1` as Default verbatim and load the active
  profile with `loadProfile()` (no pre-sync) — going through
  `activateProfile()` there writes the just-parsed live tables into Default
  and clobbers it. `deleteProfile()` has the same hazard, which is why the
  non-syncing `loadProfile()` exists as a separate step.

### Settings panel visual design
- **Two tabs only**: `General` (6 rows) and `Inputs` (16 rows).
- **Palette**: one accent (`0xFF6B3FA0`, the console purple) on a neutral ramp
  (`kPanelInner 0xFF16161C`, `kRowBg 0xFF23232D`, `kPillBg 0xFF32323F`) plus
  a semantic green (`0xFF4FB477`) used only for the volume fill and a
  successful auto-map. Text: `kText`/`kTextDim`/`kTextMute`. All `constexpr`
  and local to the draw block so the page reads as one design.
- **Rows** carry a `SettingsRow::RowStyle` (`Value` / `Button` / `Action` /
  `Header`), a `selected` flag (armed remap) and an `unbound` flag. The value
  is drawn as an inset "pill" (`+2px` inset) rather than a second column of
  text, so it reads as a control. Action rows get a darker plate; selected rows
  take the accent and their pill goes solid accent.
- **Tabs** use an active underline over a dim accent plate instead of two
  independent buttons. Tab labels shrink to fit, since "PROFILES" used to
  overlap INPUTS at the minimum window size.
- **Panel is anchored below the LCD glass** (see the upscaling section) so the
  running game stays visible while graphics options are judged.

### Upscaling (image quality, display-only)
- **This is FILTERING, not window scaling.** The frame always fills the same
  area; only the resampling changes. Two rows on the settings **General** tab,
  each click-to-cycle, **both defaulting to the pre-existing behaviour so the
  feature is strictly opt-in.**
- **Filter** (`VideoFilter`): `Nearest` | `Bilinear` | `Smooth Bilinear` |
  `Scale2x`. `Nearest` is the old behaviour. Implemented in `shaders/quad.frag`.
  * An earlier integer-Scale row (Fit / Original / 2x..6x) was REMOVED: it
    changed the drawn size, which is window scaling, not upscaling quality.
- **LCD Grid**: `Off` | `Light` | `Medium` | `Strong`, mapping to a border
  depth of 0 / 0.12 / 0.22 / 0.34. Darkens a 1/6-wide border on all four sides
  of each source pixel, matching SameBoy's six-cell MonoLCD construction
  (`Shaders/MonoLCD.fsh`, whose `SCANLINE_DEPTH` is 0.25).
- **Settings panel sits BELOW the LCD glass**, not centred on the body. The
  point of these rows is to watch the effect on the running game, so the
  screen must stay uncovered. `rebuild_settings_cells()` anchors the panel to
  the band under `bezel_rect_` and caps its height to that band; verified over
  3 tabs x 8 window sizes that it always clears the glass, stays inside the
  body, and still fits every row.
- **Why the filtering is hand-written in GLSL rather than just setting the
  sampler to `VK_FILTER_LINEAR`**: (a) Scale2x/EPX and the grid need neighbour
  texels and pattern matching, which a sampler cannot do; (b) the texture is
  `VK_FORMAT_B8G8R8A8_SRGB` and the swapchain is sRGB too, so every fetch is
  already linearised and the write is re-encoded — interpolating between
  linearised fetches is gamma-correct for free. SameBoy has to do it by hand
  (`Shaders/MasterShader.fsh` does `pow(c, 2.2)` on every sample and
  `pow(..., 1/2.2)` on output) because its texture is a plain `GL_RGBA8`.
  Keeping the sampler `NEAREST` and fetching at exact texel centres means
  `texel()` returns one precise texel and the grid can still reach neighbours
  by integer offset.
- **State path**: `Settings::video_filter` / `lcd_grid` ->
  `GameBoy::apply_video_settings()` -> `VulkanRenderer::setVideoFilter/
  setLcdGrid` -> a 16-byte `VideoPush` **push constant** on the game pipeline
  layout (the GUI pipeline has its own layout and is unaffected). Pushed in
  both places that draw the game quad (`record_command_buffer` and
  `record_command_buffer_with_gui`); `record_command_buffer_menu` draws no game
  quad, and the two `graphics_pipeline` binds inside the body-photo block are
  the bezel photo, not the frame.
- **Two bugs found while reviewing this, both fixed and pinned**: the bezel
  photo is drawn with the SAME pipeline + fragment shader as the game, so the
  LCD grid would have been drawn over the entire shell photo (a neutral
  Nearest/grid-0 push now precedes every photo draw, which also stops the menu
  path reading uninitialised push constants); and the viewport was being copied
  into the scissor with truncating casts, which clipped the right/bottom edge of
  a fractional viewport — now floor/ceil so the scissor always covers it.
- Aspect letterboxing is plain geometry in `gb/types.h::fit_aspect_rect` (NOT an
  integer/whole-multiple scale) so the selftest verifies the source aspect ratio
  is preserved and the result always fits, without needing a GPU.

### Release builds (no terminal output)
- `-DGB4ME_RELEASE=ON` strips every developer facility. The resulting binary
  emits **zero bytes** to stdout and stderr in every case, including with all
  `GB4ME_*_TRACE` env vars set:
  ```bash
  cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DGB4ME_RELEASE=ON         -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$PWD/build-release/bin
  cmake --build build-release -j$(nproc)
  ```
- **What is removed**: `src/selftest.cpp` is dropped from the source list (not
  left to the linker, so no test code or format strings survive); `--selftest`,
  `--list-roms`, `--dump-gba` and the whole headless dump tool are compiled
  out; `usage()` is gone, so only a bare ROM path is accepted and unknown
  flags are silently ignored; all trace/printf sites in `gameboy.cpp`
  (`GB4ME_ITRACE`), `gba/{irq,dma,cpu,audio}.cpp`, `apu/apu.cpp` and the
  `GB4ME_GUI_COUNT` probe are `#ifdef`-ed out, and the matching
  `std::getenv` calls never run, so the env var names are not in the binary.
- **SDL's own logging** goes to stderr, so `GameBoy::initialize()` installs an
  explicit no-op `SDL_SetLogOutputFunction` sink before anything initialises.
  (This SDL3 has no `SDL_LOG_PRIORITY_NONE`, and a null callback means "install
  your own", not "disable", so an explicit empty sink is required.)
- **`-DGB4ME_KEEP_SELFTEST=ON`** keeps the harness in an otherwise-release
  build so the suite can be run against release flags. Used to prove the
  stripping changed no behaviour: 444/446/447 pass identically to the dev build.
- `CMAKE_RUNTIME_OUTPUT_DIRECTORY` is now overridable, so a release build no
  longer clobbers the dev binary in `bin/` (which was the stale-binary trap).
  `bin/GB4ME` remains the development binary.

### Cartridge/Save Types Supported
- SRAM (64KB, battery-backed)
- Flash Panasonic (MN63F805MNP, 64KB)
- Flash Sanyo (LE26FV10N1TS, 64KB/128KB)
- Flash Macronix (MX29L002/004, 64KB/128KB)
- Flash Atmel (AT29LV512/010, 64KB/128KB)
- EEPROM 4Kbit (512 bytes)
- EEPROM 64Kbit (8KB)
- EEPROM 512Kbit (64KB, unlicensed/unused, protocol-identical to 64Kbit)

### BIOS/HLE Behavior Implemented
- No external BIOS dump required or requested
- HLE BIOS installs exception vectors + IRQ trampoline into BIOS backing
- BIOS protection: reads return open-bus (0xFF) when CPU outside BIOS region; writes ignored; setBiosActive(true) allows reads for vector inspection
- Post-BIOS startup state: System mode, ARM state, CPSR = 0x1F (I+F clear), SP_usr = 0x03007F00, R0-R12 = 0, banked SPs at BIOS areas, IWRAM system area [0x3007E00..0x3007FFF] zeroed, IE/IF/IME cleared, POSTFLG = 1
- CPU enters ROM at header branch target (ARM B at 0x08000000, sign-extended offset)
- SWI dispatcher routes to system/math/memory/decomp/affine subdispatchers
- IntrWait/Halt/VBlankIntrWait: halt CPU, wake on waited IRQ via pollWait
- HardReset: sets reboot_requested_ flag, core resets + applyStartup again (skips logo intro)
- GetBiosChecksum: returns 0xBAAE187F (GBATEK value)
- Sound driver stubs: SoundBias, SoundDriverInit/Mode/Main/VSync/VSyncOff/VSyncOn/ChannelClear, MusicPlayerOpen/Start/Stop/Continue/FadeOut, GetJumpList — return without side effects (actual sound via GbaAudio)

### Known Remaining Compatibility Gaps
- Selftest gaps: none (444/444 bare; 446/446 with GB/GBC boot smoke; 447/447
  with GBA boot smoke — Sep 24 2026).
- Real-ROM status (headless probes, Sep 23 2026 — SUPERSEDES the Sep-20 "hang"
  notes; those were THUMB.2-bit-swap-era observations): all three commercial
  ROMs BOOT, RENDER, and RESPOND TO INPUT on the current tree:
  * Golden Sun: first lit frame f=39, intro story screens render (teal
    palette + white text, 29 distinct colours verified in framebuffer dump);
    progresses through DISPCNT 0x0140->0x1440->0x1540; task counter advances.
  * Megaman Battle Chip Challenge: lit at f=1, full-screen scenes (nb~38k),
    DISPCNT 0x1380->0x3300->0x3400->0x3600; key presses advance pc across
    regions ending in a scene change (nb=38400 at 0x080051CC).
  * Pokemon Emerald: lit from f=0, intro/outdoor graphics (grass greens +
    dialogue-text rows in dump); input advances pc across regions with
    DISPCNT changes (0xB441->0x1741->0x3140); the 0x08001084 wait loop
    RETURNS (polled byte goes nonzero) — not a hang.
  * pokeemerald.gba (decomp build) also passes boot smoke.
  Interactive play-through not verified headless (no display in this env).
- Audio verdict (Sep 23 2026, headless mix capture): Golden Sun plays from
  boot (61k/295k nonzero, RMS 633); Emerald is silent for the first ~200
  frames then PLAYS (1.17M/1.48M nonzero over 1000 frames, RMS 634, peak
  9151) — Sappy streams zeros until the song starts, so early silence is
  CORRECT (matches hardware: logos silent, music from title). FIFO DMA
  verified flowing (12.5k blocks/channel/900f) with mGBA-parity refill.
  Waveform audit (raw capture): clean music + clean digital silence, NO
  periodic ticks and NO clipping in emulated output (GS: music 0-0.5s,
  silent logos 1-4s, music 4s+; Emerald: silent 0-1s, jingle/title 1s+).
  The reported "stingy tick" is a realtime playback artifact (SDL device
  starvation), NOT the mix: buffers retuned to per-frame pushes + 2048
  device frames (~63ms total — survives ~30ms host hitches; the 1024
  experiment starved on any hitch). Plus a mild 2-tap output low-pass
  (VBA-M defaults its PCM low-pass on, mGBA band-limits; raw DAC steps
  sound harsh/stingy without it).
- Graphics verdict (Sep 23 2026, framebuffer dumps): 9+ scenes across all 3
  games structured and correct (GS teal story text, Emerald grass overworld
  + dialogue rows + bright title, Megaman full-screen scenes). No core
  glitch found in any sampled scene.
- IF THE USER REPORTS GLITCHES: first suspect a STALE BINARY. There are
  THREE copies: /usr/bin/GB4ME (root-owned, Sep-20, predates ALL fixes),
  ~/.local/bin/GB4ME (Sep-23, missing post-Sep-23 fixes), bin/GB4ME
  (current, produced by every build). ALWAYS run ./bin/GB4ME. NEVER copy
  the binary anywhere (user order).
- mGBA suite (cloned to /tmp/opencode/suite — NOT in repo): test SOURCES only,
  no prebuilt ROMs; building requires devkitARM (absent, no sudo/apt) and the
  suite reports via mGBA-debug registers (0x04FFFxxx, unimplemented — test
  harness, not hardware). Differential evidence instead: ArcTan/Sqrt
  polynomials match mGBA source exactly; suite's ArcTan(0x4000)->0x2000 vector
  is a selftest check; DMA/timer/KEYCNT semantics cross-checked against
  docs/references/mgba sources during implementation.
- ctest unit tests are GONE (tests/ wiped with the rest of the tree; build
  reports "No tests were found"). The 252-check selftest is the automated
  suite until tests/ is restored.

### Intentionally Unsupported Hardware
- Prefetch (not modeled yet — Phase 5+)
- Grocko/DCT (prohibited opcode traps handled)
- Coprocessor instructions (trap as Undefined)
- 26-bit overflow (not on 32-bit GBA)
- FIQ (unwired on retail GBA)
- RTC in MBC3 (stubbed for GB side, not applicable to GBA)

---

## Quick Reference: Key Addresses and Constants

### GBA Memory Map (GBATEK)
- 0x00000000-0x00003FFF: BIOS (16KB, HLE-protected)
- 0x02000000-0x0203FFFF: EWRAM (256KB)
- 0x03000000-0x03007FFF: IWRAM (32KB)
- 0x04000000-0x040003FF: I/O (LCD, timers, DMA, IRQ, keypad, audio, WAITCNT)
- 0x05000000-0x050003FF: Palette RAM (1KB)
- 0x06000000-0x06017FFF: VRAM (96KB)
- 0x07000000-0x070003FF: OAM (1KB)
- 0x08000000-0x0BFFFFFF: ROM (mirrors every 32MB, wait-state slots)
- 0x0E000000-0x0EFFFFFF: SRAM/Flash/EEPROM (16MB window)
- 0x0D000000-0x0DFFFFFF: EEPROM serial (when active, otherwise ROM mirror)

### GBA Key I/O Addresses
- 0x04000000: DISPCNT
- 0x04000004: DISPSTAT
- 0x04000006: VCOUNT (read-only)
- 0x04000008-0x0400001E: BG counters/scrolls (BG0-BG3)
- 0x04000020-0x0400003E: Affine parameters (BG2/BG3)
- 0x04000040-0x0400004E: Window dimensions/ins/outs
- 0x0400004C: MOSAIC
- 0x04000050-0x04000054: BLDCNT/BLDALPHA/BLDY
- 0x04000060-0x040000A7: Sound controller (PSG + FIFO + SOUNDCNT + SOUNDBIAS)
- 0x040000B0-0x040000DF: DMA (DMA0-DMA3)
- 0x04000100-0x0400010F: Timers (TM0-TM3)
- 0x04000130-0x04000133: KEYINPUT/KEYCNT
- 0x04000200: IE
- 0x04000202: IF
- 0x04000204: WAITCNT
- 0x04000208-0x04000209: IME
- 0x03007FFC: IRQ handler address (game IRQ vector)
- 0x03007FF8: IntrWait check flags
- 0x03007FFA: Boot flag (SoftReset return)
- 0x03007FE0: SP_svc
- 0x03007FA0: SP_irq
- 0x03007F00: SP_usr

### GBA BIOS SWI Numbers
- 0x00: SoftReset
- 0x01: RegisterRamReset
- 0x02: Halt
- 0x03: Stop
- 0x04: IntrWait
- 0x05: VBlankIntrWait
- 0x06: Div
- 0x07: DivArm
- 0x08: Sqrt
- 0x09: ArcTan
- 0x0A: ArcTan2
- 0x0B: CpuSet
- 0x0C: CpuFastSet
- 0x0D: GetBiosChecksum (returns 0xBAAE187F)
- 0x0E: BgAffineSet
- 0x0F: ObjAffineSet
- 0x10: BitUnPack
- 0x11: LZ77UnCompWram
- 0x12: LZ77UnCompVram
- 0x13: HuffUnComp
- 0x14: RlUnCompWram
- 0x15: RlUnCompVram
- 0x16: Diff8bitUnFilterWram
- 0x17: Diff8bitUnFilterVram
- 0x18: Diff16bitUnFilter
- 0x19: SoundBias
- 0x1A-0x1D: SoundDriverInit/Mode/Main/VSync
- 0x1E: SoundChannelClear
- 0x1F: MidiKey2Freq
- 0x20-0x24: MusicPlayerOpen/Start/Stop/Continue/FadeOut
- 0x25: MultiBoot
- 0x26: HardReset
- 0x27: CustomHalt
- 0x28: SoundDriverVSyncOff
- 0x29: SoundDriverVSyncOn
- 0x2A: SoundDriverGetJumpList

### GBA Timing
- CPU: ARM7TDMI, ARM state (32-bit), THUMB state (16-bit)
- PPU: 1232 dots/line (1008 draw + 224 HBlank), 228 lines, 280896 dots/frame
- Frame: 240x160 pixels, RGB555 (15-bit color)
- Audio: 44.1kHz stereo output
- VBlank: lines 160-227 (68 lines)
- HBlank: dots 1008-1231 (224 dots)

---

## Build/Run Instructions for GBA

### Build
```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j4
```
Binary + shaders land in `bin/` (cmake `CMAKE_RUNTIME_OUTPUT_DIRECTORY` —
no manual copying, ever). Stale-binary trap (Sep 2026): `/usr/bin/GB4ME`
is a root-owned Sep-20 install that predates all fixes, and
`~/.local/bin/GB4ME` is a Sep-23 copy missing later fixes — the user was
unknowingly running those. ALWAYS run `./bin/GB4ME` from the repo root.
Verify with `strings bin/GB4ME | grep 'gb;gbc'` (must show `gb;gbc;gba`).

### Run GBA ROM
```bash
./bin/GB4ME path/to/rom.gba
```

### Run Self-Test
```bash
./bin/GB4ME --selftest          # headless hardware checks
./bin/GB4ME --selftest rom.gba  # with boot smoke test
```

### List ROMs
```bash
./bin/GB4ME --list-roms /path/to/roms
```

### Unit Tests
None — the `tests/` directory is not part of the tree, so `ctest` reports
"No tests were found". `./bin/GB4ME --selftest` is the automated suite.

### Debug Environment Variables
- `GB4ME_IRQ_TRACE` — log every IRQ entry with handler address and BIOS-active state
- `GB4ME_GBA_DMA_TRACE` — log DMA transfers

---

## Confirmation

- **No copyrighted GBA BIOS dump added or required.** The emulator uses an internal HLE BIOS implementation. Games boot without a gba_bios.bin file. The usage message explicitly states "built-in HLE BIOS — no firmware files are needed or requested."
- **Existing GB/GBC functionality preserved.** GB/GBC self-tests all pass. GBA support is additive (gba::GameBoyAdvance is a separate core, integrated via gb::GameBoy::gba_ member and gba_mode_ flag).
- **BIOS/HLE is the only GBA startup path.** applyStartup() installs vectors and enters the ROM at its header branch target. There is no direct-to-ROM jump and no external BIOS loading.

---

## Files Added/Modified (Recent)
- `src/gba/bios_math.cpp` — Fixed Sqrt to return integer (not 16.16), fixed ArcTan polynomial to match mGBA exactly, fixed ArcTan2 edge cases
- `src/selftest.cpp` — Fixed test expectations: Sqrt(0x10000000) expects 0x4000 (integer), ArcTan(1.0) uses 0x4000 input (1.14 format), ArcTan2(1,1) uses 0x4000 inputs, ArcTan2(0,1) expects 0x4000 (90 degrees); fixed IRQ handler LDR offset, IntrWait SWI encoding + IE/DISPSTAT setup, DMA 32-bit encodings, timer reload/frozen-counter expectations per GBATEK
- `include/gba/timers.h` + `src/gba/timers.cpp` — Fixed prescaler() to use control register bits (was ignoring argument and using timer index)
- `src/gba/core.cpp` — Poll IntrWait after final halt slice so VBlank raised on last cycle wakes CPU immediately
- Frontend fixes (Sep 2026): Open-dialog filter `gb;gbc` -> `gb;gbc;gba` (GBA files were
  unselectable); new `gba_screen_rect_` (3:2 fit into LCD glass) used by the GBA render
  path + `hit_screen` overload so GBA frames are no longer stretched into the GB-shaped
  rect; gear/OPEN icons floored at 18px + `SDL_SetWindowMinimumSize(300x420)`; menu
  header/footer text now scales with window (`ui_scale` shrink-to-fit: width + footer/
  title reserves, bottom-anchored footer); overlay NDC now uses window (not swapchain)
  dims so HiDPI/fractional-scale no longer shrinks/misplaces the whole overlay;
  `menu_cell_h` capped at list height (tiny windows spilled cells over the footer).
  Layout proven by standalone prover (`/tmp/opencode/layout_dump.cpp`): gear/footer/
  cells/hit-test invariants hold for 6 window sizes x 3 list lengths x 3 scroll spots.
  Overlay robustness (Sep 2026): GUI indices are 32-bit and vertex/index buffers grow
  on demand — long ROM lists could overflow the old fixed 16k/32k + u16 wrap and
  silently drop tail draws (footer tail, gear/open). Overlay NDC already uses window
  dims (HiDPI fix above).   Window title now carries the build date (`GB4ME (Sep 22
  2026)`) so stale installs are instantly recognizable — Ramsey: `strings $(which
  GB4ME) | grep 'gb;gbc'` must show `gb;gbc;gba` and the title bar must show the date.
- Upscaling (Sep 24 2026, user request): `include/gb/types.h` gains
  `VideoFilter` (Nearest/Bilinear/SmoothBilinear/Scale2x) with a name helper
  and `fit_aspect_rect`; `shaders/quad.frag` implements all four filters plus
  the SameBoy-style 1/6-pixel LCD grid on a NEAREST sampler; `vulkan_renderer`
  gains a 16-byte `VideoPush` push constant, `setVideoFilter`/`setLcdGrid` and
  `push_neutral_video_constants()`; the General tab gains Filter + LCD Grid
  rows, and the settings panel is re-anchored below the LCD glass so the game
  stays visible while the effect is judged. SameBoy cloned to
  `docs/references/sameboy` as the reference. Display-only: no emulation
  behaviour changed. An integer-Scale row (Fit/Original/2x..6x) was added and
  then REMOVED on user correction: it resized the frame (window scaling), which
  is not what "upscaling" means here.
- `include/gb/font.h`: redrew `N` with a full diagonal band — the old N differed from
  `M` by only 2 pixels (one row) and misread as M at small sizes/compositor scaling.
- Core fixes (Sep 2026): THUMB.2 ADD/SUB bit-swap in `cpu_thumb.cpp` (bit10=imm flag,
  bit9=ADD/SUB per GBATEK; ADD-imm3 ran as SUB-reg — proof: 0x02003090-0x08000367 =
  observed 0xFA002D29); Halt now wakes on (IE&IF)!=0 per GBATEK in `core.cpp` +
  `cpu.cpp` (plain Halt previously slept forever under `core.step`). 5 new THUMB.2
  selftest checks added (all four bit[10:9] forms + SUB carry).
- Test fix, NOT core (Sep 23 2026): "OBJ mosaic uses screen-Y snap" failed with
  0xFF000000 — the TEST was wrong twice, the renderer was right (region dump
  proved screen-Y snap + clamping correct: rows y=20-23 -> colour 1, y=24-27
  -> colour 5). (1) Threshold `> 0x60` unreachable: max palette entry 8 maps
  to (66,66,66); colour 5 = (41,41,41). Lowered to `> 0x20` (separates colour
  5=41 from colour 1=8 and black=0, preserving intent). (2) Sampled x=10 lands
  on the ZERO high byte of the 0x55 row pattern (transparent by construction);
  moved to x=8 (opaque byte 0). `src/selftest.cpp` only. Lesson: when a PPU
  test fails, dump the sprite REGION first — pattern-match the tile bytes
  before touching the renderer.
- Ref audit (Sep 23 2026, GBATEK + mGBA source + suite expectation tables —
  NO probing, pure code-vs-ref comparison). Fixes, all in-repo:
  * DMA CNT_H write mask (mGBA: 0xF7E0 ch0-2, 0xFFE0 ch3) + CNT_L 14-bit mask
    on ch0-2 (`src/gba/dma.cpp` latch/finish/write).
  * Sound FIFO DMA: dropped the require-repeat gate + always finish() after a
    FIFO block (mGBA services FIFO requests whenever armed+enabled+Special;
    non-repeat clears Enable after one block) + refill threshold <=16 -> <16
    (mGBA: request when free > 4 words). `dma.cpp` + `audio.cpp`.
  * EEPROM: ROM-size size heuristic replaced by engine self-correction —
    `EepromSave` transparently upgrades 4Kbit->8KB on first out-of-range
    access (0xFF fill = erased state), preserving low data. A static
    ROM-size guess would misclassify 64Kbit titles (Boktai per mGBA
    overrides). 4 new selftest checks (serial write/read round trip).
  Verified-correct against refs, NO changes needed: shifter edge cases
  (LSL/LSR/ASR/ROR/RRX/reg>=32), ADC/SBC/RSC+V (checked vs suite carry
  vectors), MUL/MULL (N/Z, RdHi/Lo), LDM/STM (writeback/PC/SPSR/user-bank;
  empty-list = R15-only like mGBA), SWI/IRQ/Undefined entry (LR/CPSR per
  GBATEK), all 5 decompression SWIs (match mGBA bios.c exactly — note:
  GBATEK's Huff tree-size prose is loose, mGBA's `(b<<1)+1` is HW-proven),
  timers (prescale/cascade/mask), IRQ controller, PPU affine stepping +
  wrap, OBJ mosaic snap, compositor (priority/windows/alpha/brightness),
  HBlank-every-line DMA+IRQ (mGBA parity), RTC/GPIO protocol, Flash IDs.
  Deliberately NOT emulated (no game impact): open-bus last-fetch values
  (we return 0xFF), DMA0-from-SRAM quirk, empty-LDM 16-word variant,
  EEPROM settle delay, SIO/link, FIQ.
- USER ORDER (Sep 23 2026): NEVER copy the binary to ~/.local/bin or anywhere
  else. Run and test from build/. The old deployment note below is kept for
  archaeology only — do NOT act on it.
- Repo cleanup (Sep 23 2026): removed probe-run *.sav artifacts, aider
  cache/history, op.sh; wiped + reconfigured build/ from scratch. Fresh
  bin/GB4ME passes 444/444.
- Symptom-driven fixes (Sep 23 2026, user-reported: Emerald title "dripping",
  delayed audio, GS characters appearing/disappearing):
  * HBlank DMA fired on VBlank lines too — WRONG (mGBA `_startHblank` gates
    `GBADMARunHblank` on vcount<160; an old selftest even asserted the wrong
    behavior). Repeat raster channels overran 68 blocks/frame, desyncing
    HBlank effects (title wobble). Gated to visible lines in `ppu.cpp`;
    selftest now asserts transfers-on-visible + idle-through-VBlank.
    Proven on-title: 1852 title pixels change with the gate.
  * Audio latency ~76ms (30ms push batch + 46ms device buffer) read as
    "delayed" SFX. Cut to ~40ms (per-frame push + 1024-frame device hint)
    in `audio.cpp`.
  * GS sprite flicker: renderer state proven deterministic (all px structs
    value-initialized); sprite budget charges audited vs GBATEK text
    (1210/954 + n/10+2n — ours under-charges slightly = shows MORE, never
    drops early). No core flicker mechanism found; likely same raster
    desync class as the title (GS uses HBlank effects) — fixed by the gate
    above. If flicker persists in bin/GB4ME, need the exact scene.
- Emerald title investigation (Sep 23 2026, `--dump-gba` frame + OAM dumps):
  the title is a LIVE animated sequence, not stuck (f=1700 partial logo 386px
  -> f=1900 635px -> flashes -> red coils -> blue scene -> black fade ->
  new scenes to f=4000; OAM clean: 17 active + 111 parked tile-0, logo
  sprites spawn correctly when the script spawns them). Sprite pixel path
  (nibble, palette, 1D/2D, 256-color stride, affine math) verified vs VBA-M
  gbaGfx.h. LZ77UnCompVram call log (debug build, f<1200): all 10 uploads
  land by f=207 (Nintendo/GF logos + title set), nothing after — uploads
  COMPLETE, renderer draws what it's given.
  Root-caused 2026-09-23 with pokeemerald (game's own code, cloned to
  /tmp/opencode/pokeemerald): the POKEMON logo is a BG layer (retail: text
  map 2048B at 0x06003800/screen 7 + 32KB GFX at char 0 — NOTE retail
  differs from decomp master, which says affine/0x06004800; the game is
  self-consistent so this is fine). Map is COMPLETE (1024 entries), GFX
  present (~256 tiles), BG2 enabled, text renderer verified — yet only
  sparse logo-colored specks show full-width. Remaining variable is
  BG2 scroll/slide dynamics (write-only regs, unobservable headlessly);
  the slide (Phase2 BG2Y -32->0 + HBlank wave) is the prime suspect for
  showing an empty map region. If the settled attract title still lacks
  letters in bin/GB4ME, the bug is scroll/slide pacing, NOT rendering,
  NOT DMA, NOT decompression (all three proven correct).
- PSG read-mask fix (Sep 23 2026, mGBA suite io-read table — REAL bug in
  previously-unaudited code): our PSG register readbacks were DMG-style
  (unreadable bits forced 1: `0x80|nr10_`, `0x3F|nr11_`, `0xFF` triggers/
  gaps, NR42 read from wrong addr 0x7A, SOUNDCNT_H unmasked, NR52 base
  0x70). GBA hardware reads write-only bits as 0 (suite-proven vectors:
  NR10 0x007F, NR11/12 0xFFC0, NR13/14 0x4000, ... SOUNDCNT_H 0x770F).
  Any driver doing read-modify-write on sound regs got corrupted values
  (wrong lengths/envelopes/triggers = glitchy audio). Fixed all of
  `audio.cpp` read8 to suite masks + gaps read 0 + LCD masks (BG0/1CNT
  bit 13, WININ/WINOUT reserved bits per suite 0xDFFF/0x3F3F in `ppu.cpp`).
  12 new selftest checks, one per mask. DMA SAD/DAD + CNT_L + FIFO + BLDY
  readbacks deliberately left (open-bus/live-counter semantics games
  cannot depend on; no game impact).
- Full-audit fixes (Sep 23 2026, GBATEK + mGBA + VBA-M + suite + pokeemerald):
  * Alpha blending blended with the first 2nd-target at ANY depth — WRONG
    (GBATEK: only the immediately-next pixel qualifies; a non-2nd-target
    pixel between disables blending). Now blends top with stack[1] only.
  * Window X1>X2 rendered empty — WRONG (GBATEK: X1>X2 acts as X2=240,
    Y1>Y2 acts as Y2=160; suite "Window offscreen reset" confirms clamp).
    Our own comment even documented the wrong behavior; fixed.
  * step_gba_frame tick cap 400000 (~1.4 frames) could truncate giant-DMA
    frames (280896 + ~131K stall), dropping PPU lines + timer/audio time.
    Raised to 1200000 (~4 frames).
  * Flash bank switching (B0 command) unimplemented — 128KB saves
    (Emerald!) wrote upper-bank data to bank 0 = corrupt saves. mGBA-parity
    bank_base_ + physAddr in all 4 vendors + state save/load. 2 new checks.
  * Audio: 46ms device buffer restored (23ms starved on host hitches =
    ticks) + mild 2-tap output low-pass (VBA-M/mGBA both filter; raw DAC
    steps sound stingy). Waveform captures prove clean mixes in both games.
  * Direct Sound L/R enable bits SWAPPED in the mixer — GBATEK +
    mGBA `GBARegisterSOUNDCNT_HI`: bit 8 = A **Right**, bit 9 = A **Left**,
    bit 12 = B **Right**, bit 13 = B **Left** (we had Left first, so any
    game using stereo Direct Sound was panned backwards). Fixed.
  * BG2X/BG2Y/BG3X/BG3Y: software writes were copied latch→internal
    unconditionally. GBATEK "Internal Reference Point Registers": the
    automatic latch→internal copy happens at VBlank, and a software write
    only hits the internal register **outside** VBlank. Writes during
    VBlank now stage for the next frame only.
  * ARM single-data-transfer writeback (LDR/STR/LDRH/STRH/LDRSB/LDRSH):
    pre-indexed+writeback suppressed the base update when Rn == Rd. ARM ARM
    A3.2.1 only suppresses writeback for the post-indexed and
    unregister/scaled-offset forms; the pre-indexed+writeback form ALWAYS
    writes the computed address. Fixed in both the word and halfword groups.
  Verified-correct this pass (no change): CpuSet/CpuFastSet, BgAffineSet/
  ObjAffineSet (theta/matrix vs GBATEK), ARM MUL/SWP/halfword groups,
  THUMB remainder, VRAM/OAM/PAL mirrors + 8-bit rules (vs mGBA memory.c),
  palette/OAM, RegisterRamReset (kClearBase 0x3007E00), audio PSG trigger/
  length/envelope/sweep/wave/noise details (duty table 12.5/25/50/75%,
  wave 0/25/50/75/100% + 75% = digit*3/4, two-bank 64-digit position,
  noise LFSR XOR-feedback + 7-bit mode + {8,16,32,48,64,80,96,112}<<s),
  SOUNDCNT_H volume ratios (PSG 0.25/0.5/1/1, DS 50/100%), timer select
  bits 10/14, FIFO hold-last-sample + refill at <16 bytes, suite video.c
  window-offscreen clamp (matches), forced blank (DISPCNT bit 7, white),
  save detection (all 3 ROMs' markers correct: GS FLASH_V123, MM SRAM_V113,
  EM FLASH1M_V103).
- Full-project audit (Sep 23 2026, CodeRabbit CLI 0.8.0 on uncommitted +
  3 parallel deep-dive agents over CPU/bus, PPU/audio, GB core/frontend).
  Every finding was re-verified against the code before fixing. Fixed:
  * GBA bus 8-bit SRAM protocol (GBATEK "Accessing SRAM Area by
    16bit/32bit"): a wide read now fetches ONE byte and replicates it
    (x0101 / x01010101); a wide write updates only the addressed byte with
    the LSB of `value ROR (addr*8)`. Previously a 32-bit Flash write could
    corrupt two save locations. Added `GbaBus::sramOffset()` so the 32MB
    SRAM mirror and the 32K-chip repeat map to the same physical byte
    (0x0E010000 / 0x0F000000 aliases used to read 0xFF).
  * GBA 8-bit I/O writes now merge with the neighbouring byte and dispatch
    as a 16-bit write (`GbaBus::dispatchIo16` + `io_merge_active_` guard).
    Before this a `STRB` to DISPCNT/DMA*/TM*/IE/IF/KEYCNT only touched the
    `io_` shadow and never reached the owning device. Every handled 16-bit
    write now updates the shadow so the merge sees the true neighbour.
  * ARM7 mis-aligned LDRH/LDRSH (GBATEK "Mis-aligned LDRH,LDRSH"):
    `LDRH [odd]` = aligned load ROR 8; `LDRSH [odd]` = `LDRSB [odd]`.
  * GBA audio: the mixer is now gated on NR52 bit 7 (no sound while the
    master enable is off); disabling power keeps SOUNDCNT_H (it stays
    R/W while off, mGBA parity) and no longer wipes the FIFOs; the FIFO
    reset bits 11/15 act whenever written set, not only on a 0->1 edge.
  * GBA noise `freq_timer` widened u16 -> u32: valid periods reach
    `112 << 14 = 1835008`, so `8 << 13` used to wrap to 0.
  * GBA sample clock is now exactly 44100 Hz: the dither was
    `380 + 35/100` (44109.9 Hz); it is now `380 + 0.435718` in fixed point.
    The fractional phase was a function-local `static` (shared across
    instances, absent from save states) and is now a member serialised in
    save/load.
  * GBA mode 4 (and 5) bitmap layers now honour BG2X/BG2Y and BG2PA..PD
    (GBATEK "Bitmap Mode Distortions") and BG2 mosaic in both axes.
  * GBA mode 4 palette index 0 is now transparent (GBATEK), so sprites and
    lower-priority layers show through instead of being hidden.
  * GBA affine BG sampling now uses the FULL matrix (PB/PD row terms were
    missing) and applies vertical mosaic; the Y snap previously only
    existed on text BGs.
  * GBA text/affine BG map+character fetches past 0x10000 are now
    transparent instead of mirroring into OBJ VRAM (mGBA parity).
  * GBA transformed (affine) OBJ now applies mosaic; only the non-affine
    path had it.
  * `GameBoy::flush_active_save()` — a GBA->GBA ROM switch previously
    replaced the cartridge WITHOUT flushing the outgoing game's save, and
    a GBA->GB switch never flushed it at all. GBA images are now validated
    with a throwaway `gba::Cartridge` before anything is torn down.
  * `GameBoy::set_volume` now reaches the GBA audio stream (the slider and
    mouse wheel were GB-only); window focus loss releases held keys on both
    the GB joypad and the GBA keypad (missed key-ups used to stick).
  * GBA remap prompt now reads `pending_gba_key()` first, so GBA rows show
    the key name instead of "button".
  * GB MBC: added `raw_read`/`raw_write`/`raw_bank_count` to the `MBC`
    interface so `save_ram`/`load_ram` use the backing store directly
    (the old bank-register walk reset a running game's bank/mode and
    produced an all-0xFF save whenever RAM was disabled). MBC2 has no RAM
    bank register, so its count is 1 and its width is 512 nibbles.
  * GB cartridge type `$09` (ROM+RAM+BATTERY) was built as `NoMBC`, which
    reported `has_battery() == false` — those games' SRAM was never saved.
  * GB serial: the output buffer is now a bounded 64 KiB ring (an emulated
    ROM could grow it until OOM) and a completed transfer raises the serial
    interrupt (bit 8), which was never requested.
  * GB `write_tac` now evaluates the selected-DIV-bit falling edge, so
    disabling the timer while the bit is high (or switching from a high
    selected bit to a low one) clocks TIMA exactly once.
  * GB `EI` now sets IME AFTER the interrupt-service check of the following
    step, so a pending interrupt is not vectored before the instruction
    following EI. The old selftest only asserted IME eventually became true
    and passed an immediate-enable implementation; it now checks the
    ordering (3 new checks).
  * GB DMG: LCDC bit 0 now gates the WINDOW as well as the background
    (Pan Docs), and OBJ-to-BG priority now compares the BG palette INDEX
    (recorded into `cgb_bg_color_id_`) instead of the rendered RGB, which
    misfired whenever a legal BGP mapped two indices to the same shade.
  * Build hardening: `glslc` is now REQUIRED (a configure that skipped the
    SPIR-V step produced a binary that could not create a window), the
    shader list must be non-empty, and both GB and GBA cartridge loads cap
    the file at 64 MiB before allocating (a huge or sparse file threw
    bad_alloc and terminated the process during folder auto-scan).
  * `load_rom` sets `gba_mode_` only after the GB cartridge load succeeds, so
    a failure can no longer leave save routing disagreeing with the running
    core.
  Not fixed (deliberate, documented): GB OAM-DMA/GDMA charge zero DMA time
  and the serial transfer still completes instantly; both are timing-fidelity
  gaps with no test or observed-game impact, and the DMG HALT-bug quirk
  remains unmodelled.
- Affine BG map indexing — THE Emerald title-logo "scattered pixels" bug
  (user-reported Sep 23 2026, finally root-caused). `ppu_render.cpp` indexed
  the affine map with a **pixel** stride: `map_base + ty*size + tx`, where
  `size` is the pixel dimension (128/256/512/1024). Affine map entries are
  ONE BYTE each covering an 8x8 block (GBATEK "Rotation/Scaling BG Screen
  (1 byte per entry)"), so the correct index is
  `(ty>>3) * (size>>3) + (tx>>3)`. For a size-1 (256x256) map the bad form
  walked 0x4800..0xE6FF (40KB, ~20x past the 2KB map) into the Rayquaza
  BG's tiles, which is exactly the dithered speckle banding reported.
  Fixed; the Pokémon/EMERALD VERSION logo now renders correctly.
  How it was found (do NOT repeat the wasted paths): earlier notes claimed
  the logo was a *text* BG at screen 7 — that was frame 1900, the intro
  scene. At the actual title (f~3800) DISPCNT=0x1741 is MODE 1 and
  BG2CNT=0x4981 is AFFINE, screen 9 (matching the decomp's
  `BG_SCREEN_ADDR(9)`), overflow bit set. Isolating layers with temporary
  renderer env gates proved BG2 was the source.
- PPU/renderer performance (user: "graphics after the title are so slow it
  hangs"). Profiling showed the core ran at only ~104 fps emulated for
  Emerald, leaving almost no headroom over 60 fps once Vulkan+audio are
  added, so any host hitch dropped frames and the game appeared to crawl.
  Rendering was 24% of the time, the CPU/bus 76%. Fixes:
  * `GbaBus::read16`/`read32` now fast-path IWRAM/EWRAM/ROM instead of
    decomposing into `read8` calls. A 32-bit LDR previously ran `decode()`
    8 times (accessCycles + read32 + 2x read16 + 4x read8). GPIO/RTC
    addresses are explicitly excluded from the Rom fast path - they live in
    ROM space and must fall through to the device handler (caught by the
    RTC selftest).
  * The renderer now takes `GbaPpu::vramData()` once per scanline and
    indexes the backing array directly, removing ~500K out-of-line
    `readVram` calls per frame.
  Result: Emerald 104 -> 126 fps, Golden Sun 170 -> 195, Megaman -> 141.
  The temporary layer-disable env gates, chrono render timer and dump cap
  mismatch (dump used 400000, GUI uses 1200000) were all removed again.
- OBJ line budget cost (GBATEK "OBJ Character Processing"): normal OBJ is
  charged `n*1` cycles (n = horizontal pixel size) and rotation/scaling
  `10 + n*2`. The old code charged `n-2` and `8+2n`, under-charging every
  sprite by 2 cycles and therefore fitting more OBJs per line than hardware.
  The 1210/954 budget split on DISPCNT bit 5 was already correct.
- Golden Sun "characters appear and disappear": NOT reproduced. The only
  per-frame change found in the accessible (no-input) GS scenes is the
  Golden Sun logo shimmer at f704-f720, which is the game's own palette
  cycle, not an artifact. Headless runs stop at the name-entry screen, so
  battle sprites were never reached; a confirmed cause is still open.
  Rejected as the cause: OBJ-vs-OBJ overlap resolution (priority then OAM
  index, `ppu_render.cpp` ~line 494, is correct) and a per-frame OAM
  snapshot (tried and reverted - Emerald sets DISPCNT bit 5
  H-Blank Interval Free in some scenes, so a whole-frame OAM snapshot would
  wrongly delay legal mid-frame OAM updates).
- Headless input injection added so gameplay past the title/name screens is
  observable: `GB4ME --dump-gba <rom> <frames> <prefix> --input
  "frame:key,..." --hold N --snap N`. Keys A,B,S,E,U,D,L,R,H,J. `--snap N`
  writes `<prefix>_<frame>.ppm` every N frames, which makes a frame-to-frame
  flicker scan possible from ONE long run (re-running from frame 0 for every
  sample is far too slow, and comparing dumps from regenerated scripts is
  invalid because the runs diverge). Used this to drive Golden Sun through
  the title, overworld, interior and into a real battle.
- **OBJ vertical wrap was applied to non-transformed sprites (real bug).**
  `ppu_render.cpp` computed `ry = (y - obj_y) & 0xFF` for EVERY sprite.
  mGBA wraps Y only for rotation/scaling sprites (`if (inY < 0) inY += 256;`
  in `software-obj.c:222-225`); non-transformed sprites are simply culled
  when outside `[0,height)`. Games park unused OAM slots at Y=160..255, and
  the unconditional modulo redrew those sprites wrapped onto the top of the
  screen, where they also consumed the per-line OBJ budget and could push a
  legitimate character out - matching "characters appear and disappear".
  Now: non-transformed sprites with `obj_y >= kHeight` are culled, and Y is
  only wrapped for transformed ones. 1 new selftest, and it was verified to
  FAIL with the fix reverted and PASS with it applied.
- Audit #4 (Sep 23 2026, second self-review + first-ever GB-core and HLE-BIOS
  passes). Fixed:
  * **8-bit I/O merge injected 0xFF (regression from audit #3's own fix).**
    `deviceByte16()` returns 0xFFFF for registers no unit owns, so a STRB of
    0x34 to DISPCNT became 0xFF34 - enabling every BG and setting reserved
    bits. It now covers PPU + keypad registers and falls back to the io_
    shadow otherwise, with an explicit `owned` out-param. 1 new selftest,
    verified to FAIL without the fix (`0xFF34` vs `0x0434`).
  * `IntrWait` with a zero mask hung FOREVER: `wait_mask_` was set to 0 and
    `wait_any_` was left false, so `(fired & mask)` could never be non-zero.
    GBATEK defines a zero mask as "wait for any enabled IRQ". Fixed +
    2 new selftests, verified to fail without it.
  * GB MBC1: on carts <= 512 KiB the secondary bank register's bits are not
    wired and the 5-bit register must be masked to the real bank count
    (Pan Docs "MBC1"). A 256 KiB cart programmed with bank register $10
    selected bank 1 instead of bank 0.
  * GB MBC5: bit 3 is the rumble motor, not a RAM bank bit (mGBA
    `src/gb/mbc.c:246-254`); keeping it made a rumble cart addressing bank 0
    read bank 8.
  * GB PPU: writing LYC now recomputes the STAT coincidence flag and can
    raise the STAT interrupt (the flag is derived, not latched), and STAT
    bit 7 reads back as 1 (mGBA masks 0x78, so bit 7 is never stored).
  * GBA saves: added a per-title forced save-type table from mGBA's
    `overrides.c`, checked BEFORE marker scanning. Several carts ship a
    marker that contradicts the real chip - e.g. BDKJ (DigiCommunication
    Nyo) carries `SRAM_F_V103` but uses EEPROM, and Iridion II / Stuart
    Little 2 / Top Gun have no battery at all. Marker detection remains the
    fallback.
  Known-open after this pass (documented, not fixed): GB STOP does not halt
  `step()`; the DMG HALT bug is unmodelled; GB mode 3/0 lengths are fixed
  rather than SCX/OBJ-conditional; the window has no latched WY "Y
  condition"; palette reads/writes are not blocked in mode 3; the HLE BIOS
  Stop/Sleep stubs behave like Halt and `CustomHalt` ignores r2=0x80.
  All are fidelity gaps rather than crashes or visibly-wrong output.
- Audit #3 (Sep 23 2026): self-review of this session's own diff + a fresh
  renderer-vs-mGBA pass. The self-review caught FIVE real bugs that I had
  introduced while "optimising"/"fixing" earlier, all now fixed:
  * **Out-of-bounds VRAM read (the worst one).** The renderer fast path
    indexed `vram_[off & 0x1FFFF]` directly, but `vram_` is only 0x18000
    bytes and `GbaPpu::mirrorVram` folds 0x18000-0x1FFFF back to
    0x10000-0x17FFF. Any OBJ tile index reaching 0x1FFFF read past the
    array (UB). Added a `vread()` lambda that reproduces mirrorVram exactly
    and used it for every direct access.
  * `write32` split into two `write16` calls - and `write16` had just been
    changed to write ONE SRAM byte, so the second call wrote a stray 0 at
    addr+2, corrupting a neighbouring save byte and sending Flash a phantom
    command byte. Now the 8-bit-bus rule is applied directly in write32.
  * `read32`'s new ROM fast path ran BEFORE the EEPROM check, so a 32-bit
    read from 0x0D returned raw cartridge bytes instead of serial data.
    EEPROM is handled first now.
  * FIFO gating (added earlier the same session) folded "enabled+routed"
    into the `uses_timer0` predicate, so a DISABLED FIFO still fired on
    timer 1 - exactly backwards. Rewritten to compare the timer against the
    select bit directly.
  * PSG trigger still reloaded the OLD periods (`*4` / `*2`) after the
    step-loop periods were corrected to `*16` / `*8`, so the first duty edge
    after a trigger landed early. Both trigger paths corrected.
  * Bitmap-mode mosaic was gated on the MOSAIC register's bit 10 (the OBJ
    enable) instead of BG2CNT bit 6, and transformed-OBJ mosaic used
    sprite-local phase where mGBA uses screen-space (`outX%H`, `y-y%V`).
  * The 8-bit I/O merge read the stale `io_` shadow for the neighbour byte;
    it now reads the live register via `deviceByte16`, so a byte write to IF
    no longer clobbers hardware-raised bits.
  Renderer-vs-mGBA pass: text screen-block selection, window clamping and
  priority, blend arithmetic (alpha/brighten/darken), semi-transparent OBJ
  first-target, sprite budget/offscreen charging, and backdrop priority all
  MATCH. Only the two mosaic items above needed changing.
  LESSON: a PPU test is only real if you
  confirm it fails without the fix. Three separate mistakes made it pass
  vacuously - an 8x8 sprite at Y=200 (wraps to 56, never drawn anyway), the
  wrong tile stride, and `0x7C00` being BLUE not red. Two more traps: an
  all-zero OAM entry is a LIVE 8x8 sprite at (0,0), so all 128 slots must be
  disabled; and the OBJ disable flag is attr0 bit 9 (0x200), NOT bit 15.
  The same discipline was then applied to the three new audio/bus checks:
  each was confirmed to FAIL with its fix reverted and PASS with it
  applied. Two of them were themselves wrong at first (SRAM expectations
  ignored that write32 aligns down, and NR13/NR14 are a packed halfword at
  0x64 in this implementation).
- Reference audit #2 (Sep 23 2026, mGBA source in `docs/references/mgba` +
  GBATEK). The local mGBA checkout turned out to be much broader than
  documented (419 .c / 449 .h, including `src/gba/renderers/`, `src/arm/`,
  `src/gb/` and `src/gba/overrides.c`), so a real line-by-line differential
  audit was possible. Confirmed + fixed:
  * **PSG frequencies were 4x too fast on ALL four channels** — the big
    audio find. mGBA sets `timingFactor = 4` for GBA (`src/gb/audio.c:65`),
    giving pulse `4*(2048-freq)*4` and wave `2*(2048-freq)*4` cycles per
    step. We used `*4` and `*2`, i.e. one octave high, so every game played
    sharp and the noise channel was 4x hissy. Only the GBATEK
    `f = 131072/(2048-x)` formula with the x4 factor matches reality.
  * Noise divisor table was the GB table; GBA is
    `{32,64,128,192,256,320,384,448} << shift` (mGBA: `(ratio?2*ratio:1)
    << frequency * 8 * 4`). The clock-shift field is a full 4 bits, so the
    old `clock_shift < 14` guard wrongly held a constant for shifts 14-15
    instead of producing the correct very-low-frequency noise.
  * Direct Sound FIFO kept consuming on every timer overflow even when the
    master enable was off or the FIFO was routed nowhere, silently burning
    samples (mGBA `src/gba/timer.c:25-32` gates on both). Now gated; the
    two existing FIFO tests had to be taught the prerequisites and a third
    check was added for the unrouted case.
  * Power-off did not clear NR30/NR31/NR32/NR34, so software read stale wave
    registers while powered down (GBATEK: 4000060h..4000081h reset to 0).
  * `MSR CPSR_sx,r0` wrote the architecturally reserved S and X fields;
    GBATEK marks them do-not-change and mGBA writes only flags+control for
    CPSR (`src/arm/isa-arm.c:704-718`, and also excludes S/X from the SPSR
    mask at :729-735). Now reserved for CPSR, honoured for SPSR.
  * `STRH r15,[...]` and `STM {...,r15}` stored the internal PC+8 instead of
    the architectural PC+12 (GBATEK "Using R15 (PC)"; mGBA applies the
    store-only `+4`). Tail-call/prologue sequences resumed 4 bytes early.
  Deliberately NOT changed after review:
  * SOUNDBIAS resolution bits 14-15 (sample rate 32.768-262.144 kHz) are
    still ignored; the final mixer still does not apply mGBA's
    bias-add / clamp-to-0x400 / bias-subtract saturation
    (`src/gba/audio.c:351-359`). Both are real differences, but adopting them
    means rescaling the whole mix, and a prior capture had verified the
    current mix is clean and clipping-free - so this is deferred rather than
    riskier blind. Flagged here so it is not lost.
  * PSG frame-sequencer PHASE (mGBA runs an 8-state sequencer; we run three
    independent counters with correct rates but different phase), the
    immediate length decrement on a rising length-enable edge, mGBA's
    shifting wave-RAM register model, and the one-sample FIFO ordering at
    coincident timer/sample events. All minor, all noted, none fixed.
  * ARM/THUMB empty register lists (we treat as a no-op / one PC transfer
    with +-4 writeback; GBATEK documents a strange `Rn+-0x40` behaviour).
    Only reachable via malformed encodings an assembler rejects.
  * ARMv5 `BKPT` encoding falls through as `TEQ` in our decoder instead of
    trapping Undefined. ARMv5-only, never emitted by GBA toolchains.
  Verified-correct this pass (no change): shifter semantics for every
  immediate/register amount incl. >=32, ADC/SBC/RSB/RSC carry ordering,
  TEQ/TST/CMP/CMN flag-only behaviour, MUL/MLA/MULL/UMULL/SMULL/SMLAL/UMLAL
  flag+register placement, LDR-to-PC, all halfword/byte addressing, LDRD/STRD
  Undefined traps, ARM7 mis-aligned LDRH/LDRSH, exception LR/CPSR for
  SWI/IRQ/Undefined, THUMB ALU/shift/literal-load/PUSH/POP/BL, PSG envelope
  + length + sweep clock RATES (64/256/128 Hz), wave bank visibility and
  0/25/50/75/100% digit mappings, noise 15-bit/7-bit feedback algorithm,
  DS 50/100% scaling and L/R routing, save-type marker detection.
