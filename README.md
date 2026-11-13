# GB4ME

GB4ME is a Game Boy (DMG), Game Boy Color (CGB), and Game Boy Advance (GBA)
emulator written in C++23 with SDL3 and Vulkan. The GBA core uses a built-in
HLE BIOS, so no BIOS or firmware file is required.

Supported ROM formats:

- `.gb` — original Game Boy (DMG)
- `.gbc` — Game Boy Color
- `.gba` — Game Boy Advance

## Building

Requires CMake 3.28+, a C++23 compiler, SDL3, the Vulkan SDK, and `glslc`
for shader compilation. SDL3 is found through its CMake package when
available (`SDL3::SDL3`), then through `pkg-config`, and finally - only when
neither finds it - built from a pinned tag with `FetchContent`; pass
`-DGB4ME_SDL3_PKGCONFIG=ON` or `-DGB4ME_SDL3_FETCH=ON` to force either path. `vcpkg.json` declares the
dependencies for a `vcpkg install` based setup. `settings.cfg` carries a
`schema=` line, so an older or newer file still loads (unknown keys are
ignored and reported in developer builds). A `<rom>.cfg` beside a ROM is
picked up as a per-game overlay: the display keys it sets (`palette`,
`hardware_mode`, `video_filter`, `lcd_grid`, `video_scale`, `volume`,
`console_color`) apply to that game only, without touching the global
config - opening the settings panel drops the overlay again so your edits
are global.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The executable and compiled shaders land in `bin/`, so run it from the
repository root as `./bin/GB4ME`.

Frame pacing is driven by the audio buffer rather than a fixed 1/60 sleep,
with a ±1% resample correcting for the difference between the emulated and
device clocks; the `F3` overlay shows the emulated clock and audio health.

CMake presets cover the verification builds:

```bash
cmake --preset dev && cmake --build --preset dev   # release, -Werror
cmake --preset dev-asan && ctest --preset selftest-asan   # ASan + UBSan
cmake --preset dev-strict                            # release + selftest kept
```

## Running

```bash
./bin/GB4ME                         # open the ROM menu
./bin/GB4ME game.gb                 # play a Game Boy ROM
./bin/GB4ME game.gbc                # play a Game Boy Color ROM
./bin/GB4ME game.gba                # play a Game Boy Advance ROM
./bin/GB4ME --list-roms <folder>    # list supported ROMs in a folder
./bin/GB4ME --selftest              # run the built-in hardware self-test
./bin/GB4ME --selftest game.gba     # include a GBA boot smoke test
./bin/GB4ME --blargg cpu.gb [frames] # headless Blargg test ROM (serial verdict)
./bin/GB4ME --gba-suite suite.gba [suite] [frames] # headless mgba-emu/suite ROM
./bin/GB4ME --gba-tests tests.gba [frames] # headless jsmolka/gba-tests ROM
./bin/GB4ME --help
```

Use `O` to open a file, `S` for settings, and `R` to rescan the configured
ROM folder. In the menu, use the arrow keys to select a ROM and `Enter` to
play it. Press `/` to search (typing follows your keyboard layout, shortcuts
stay quiet while searching, `Esc` clears). Press `E` in a game to return to
the menu. ROM entries show their
DMG, CGB, or GBA type. Recently played ROMs pin to the top with a `*` and
launch with `1`-`8`.

The optional GBA diagnostic runner can be used without opening a window:

```bash
./bin/GB4ME --dump-gba game.gba 600 dump \
  --input "120:A,240:S" --hold 6 --snap 60
```

It writes a final `.ppm`/`.txt` dump and, with `--snap`, additional numbered
frame images. See `./bin/GB4ME --help` for its key names and options.

## Controls

Arrows = D-pad, `X` = A, `Z` = B, `Enter` = Start, and `Right Shift` =
Select. In GBA games, `A` and `D` are the default L and R shoulder buttons;
all controls can be remapped in Settings. `P` pauses, `F` or `F11` toggles
fullscreen, `F3` toggles the debug overlay (clickable Vol/Filter/Grid/Ghost/
Turbo/Pause buttons plus a Settings window with palette, hardware mode,
console color and ROM folder controls; clicks on it never reach the game),
hold `Tab` for fast-forward (`T`
latches turbo on/off), hold `U` to rewind ~60s (a snapshot every 30 frames),
and `Esc` returns to the menu (use the window close button to
quit). `-` and `=` adjust volume, with an on-screen readout. While a game
runs, `1`-`8` selects savestate slot 1-8, `F5` saves and `F8` loads, `C` toggles cheats, with a
confirmation pill on screen. Battery-backed
saves are stored beside the ROM as `<rom>.sav`; cartridge clocks (MBC3 and the
GBA GPIO RTC) keep their state in `<rom>.rtc`.

`GB4S` savestates cover all three systems (GB, GBC and GBA) and carry the
whole machine, not just the CPU: the timer, the PPU's mid-frame dot position
and rendered frame, all four APU channels with their pending samples, the
joypad, the cartridge bank registers and RAM, and the MBC3 clock. A reload
mid-scanline resumes with the same picture and the same audio, verified by
comparing frames and samples against an uninterrupted run. Eight slots per
game sit beside the ROM as `<rom>.gb4s0` … `<rom>.gb4s7` with a PPM
thumbnail each (`<rom>.gb4sN.ppm`), in both GB/CGB and GBA mode.

GameShark/raw cheats live beside the ROM as `<rom>.cheats` (one per line,
`#` comments). GB/CGB: `C000=FF` raw or `01YYXXXX` GameShark type 01 only.
GBA: `02000000=FF` / `=1234` / `=12345678` raw 8/16/32-bit (address 5-8 hex
digits; encrypted AR/CB codes unsupported). They patch bus reads (never IO
registers) and toggle with `C`.

## Video filters

The Settings panel (or the Filter key) cycles the upscale used for the
emulated screen: `Nearest`, `Bilinear`, `Smooth Bilinear`, `Scale2x` (EPX), `xBRZ`,
`Slot Mask` (rounded subpixel slots) and `Grille` (Trinitron-style RGB
triads). The `LCD Grid` slider darkens the pixel borders for every filter and
sets the mask/triad depth for the last two (0 = off, so those two behave like
`Smooth Bilinear`). The `Ghosting` setting (also Off/Light/Medium/Strong)
blends each frame with the previous one for the slow-response GB panel look.
Rumble-capable controllers are driven from the MBC5
rumble bit when a cartridge has a motor.

## Cartridge support

GB/GBC: ROM-only, MBC1, MBC2, MBC3 (real-time clock, persisted to
`<rom>.rtc`), and MBC5 (rumble ignored).
The CGB flag at `0x0143` is detected and can be overridden in Settings.

GBA: standard ROM images with automatic detection for SRAM, Flash
(Panasonic, Sanyo, Macronix, and Atmel), and EEPROM (4 Kbit, 64 Kbit, and
512 Kbit). Flash follows the GBATEK command set with AND-only byte
programming, EEPROM models the ~6.5 ms write settle time (status reads report
"busy" while it runs), and the GPIO/RTC honours clocks a game sets, keeping
the offset in `<rom>.rtc`. GBA BIOS services are provided by the internal HLE
BIOS.

## Developer environment hooks

All of these are compiled out of a `GB4ME_RELEASE=ON` build
(`cmake .. -DGB4ME_RELEASE=ON`, which also drops the selftest and dumps).
Verified with `GB4ME_KEEP_SELFTEST=ON` that they change no core behaviour.

| Variable | Effect |
| --- | --- |
| `GB4ME_ITRACE` | Input event log (frontend) |
| `GB4ME_INPUT_TRACE` | SDL event log (frontend) |
| `GB4ME_IRQ_TRACE` | GBA IRQ entry log (handler address, BIOS context) |
| `GB4ME_IO_TRACE` | GBA CPU MMIO access log (incl. GPIO/RTC window) |
| `GB4ME_GBA_DMA_TRACE` | GBA DMA channel log |
| `GB4ME_GUI_COUNT` | Headless GUI budget probe (overlay size, no display) |
| `GB4ME_FASTFORWARD` | Skip frame pacing from boot (same as holding `Tab`) |
| `GB4ME_VOLUME_OSD` | Pin the volume OSD on screen (screenshot hook) |
| `GB4ME_DEBUG_OVERLAY` | Force the `F3` ImGui overlay on (screenshot hook) |

Frame pacing is driven by the audio buffer rather than a fixed 1/60 sleep,
with a ±1% resample correcting for the difference between the emulated and
device clocks; the `F3` overlay shows the emulated clock and audio health.

CMake presets cover the verification builds: `dev` (release, `-Werror`),
`dev-asan` (ASan + UBSan) and `dev-strict` (release + selftest kept).
`ctest --preset selftest` / `ctest --preset selftest-asan` run the suite.

## Verification status

- The built-in hardware self-test passes all 1,355 checks.
- Blargg `cpu_instrs` passes 11/11 (headless `--blargg <rom>`, also in CI).
- GB/GBC boot smoke tests pass 286/286 checks.
- GBA boot smoke tests pass 287/287 checks.
- Commercial smoke tests cover Wario Land, Zelda: Oracle of Ages, Golden
  Sun, Megaman Battle Chip Challenge, and Pokemon Emerald; headless input
  tests advance past their title/name-entry screens.

## Assets

- `assets/gbc_body.png`: atomic-purple Game Boy Color front photo, cropped
  to the 75:133 shell aspect for the textured console background. Source:
  Evan-Amos, via Wikimedia Commons (`File:Game-Boy-Color-Purple.png`),
  released into the public domain (CC0). Game screen and button hit regions
  are calibrated to this image; see `GUIConsole::update_layout`.
- `assets/*.gb`, `assets/*.gbc`, and `assets/*.gba`: optional local test
  ROMs.
- UI typeface: Cousine (monospace, Apache-2.0) staged to `bin/fonts/` at
  build time from the pinned Dear ImGui source; system DejaVu/Noto fonts
  are used as fallback, then the built-in 8x8 bitmap.
