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
for shader compilation.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The executable and compiled shaders land in `bin/`, so run it from the
repository root as `./bin/GB4ME`.

## Running

```bash
./bin/GB4ME                         # open the ROM menu
./bin/GB4ME game.gb                 # play a Game Boy ROM
./bin/GB4ME game.gbc                # play a Game Boy Color ROM
./bin/GB4ME game.gba                # play a Game Boy Advance ROM
./bin/GB4ME --list-roms <folder>    # list supported ROMs in a folder
./bin/GB4ME --selftest              # run the built-in hardware self-test
./bin/GB4ME --selftest game.gba     # include a GBA boot smoke test
./bin/GB4ME --help
```

Use `O` to open a file, `S` for settings, and `R` to rescan the configured
ROM folder. In the menu, use the arrow keys to select a ROM and `Enter` to
play it. Press `E` in a game to return to the menu. ROM entries show their
DMG, CGB, or GBA type.

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
fullscreen, and `Esc` returns to the menu (use the window close button to
quit). `-` and `=` adjust volume. Battery-backed saves are stored beside
the ROM as `<rom>.sav`.

## Cartridge support

GB/GBC: ROM-only, MBC1, MBC2, MBC3 (RTC stubbed), and MBC5 (rumble ignored).
The CGB flag at `0x0143` is detected and can be overridden in Settings.

GBA: standard ROM images with automatic detection for SRAM, Flash
(Panasonic, Sanyo, Macronix, and Atmel), and EEPROM (4 Kbit, 64 Kbit, and
512 Kbit). GPIO/RTC support is limited to the supported cartridge protocol;
GBA BIOS services are provided by the internal HLE BIOS.

## Verification status

- The built-in hardware self-test passes all 284 checks.
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
