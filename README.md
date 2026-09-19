# GB4ME

A Nintendo Game Boy (DMG) emulator in C++23, with SDL3 + Vulkan. Built
GBC-ready from day one (banked VRAM/WRAM, polymorphic MBCs, variable clock);
see `AGENTS.md` for the architecture rules and development order.

## Building

Requires CMake 3.28+, SDL3, Vulkan SDK, and `glslc` (for shaders).

```bash
cmake -S . -B bin -DCMAKE_BUILD_TYPE=Release
cmake --build bin -j
```

The binary lands at `bin/GB4ME`.

## Running

```bash
./bin/GB4ME                      # ROM menu (no arg needed)
./bin/GB4ME "game.gb"            # play in a window
./bin/GB4ME "game.gb" --debug    # headless test run (no window)
./bin/GB4ME --list-roms <folder> # print playable ROMs in a folder
```

Menu: `Up/Down` select, `Enter` play, `O` open file, `S` settings,
`R` rescan, `E` (in game) back to menu. The folder button (top-left)
opens a file picker; the gear (top-right) opens settings. ROMs in the
settings ROM folder appear on the Game Boy screen with title + CGB/DMG
badge; click a selected entry (or press `Enter`) to play.

Debug options: `[max_mcycles]`, `--ppm <file> --frames <N>` (screenshot),
`--press <F>:<Key>` / `--release <F>:<Key>` (scripted input),
`--wav <file> --seconds <N>` (audio capture).
Exit codes: 0 = Passed, 1 = Failed, 2 = timeout/error.
Keys for `--press`: `Right Left Up Down A B Select Start`.

## Controls (windowed)

Arrows = D-pad, `Z` = B, `X` = A, `Enter` = Start, `Right Shift` = Select,
`P` = pause, `Esc` = quit. Battery saves are stored next to the ROM as
`<rom>.sav`.

## Cartridge support

ROM-only, MBC1, MBC2, MBC3 (RTC stubbed), MBC5 (rumble ignored). CGB flag
at `0x0143` is detected and stored per Rule 8 (DMG mode ignores it).

## Verification status

- Blargg `cpu_instrs` (01–11): all pass — `./bin/GB4ME rom.gb --debug`
- `instr_timing`: passes
- `dmg-acid2`: pixel-perfect vs reference (0/23040 differ)
- `dmg_sound`: 01–06 pass (registers, length, trigger, sweep, overflow);
  07–12 partially pass — remaining failures need T-cycle-exact wave-RAM
  conflict and power-phase behavior, beyond M-cycle granularity
- Commercial: Wario Land SML3 boots and plays with sound (MBC1 + battery)

## Assets

- `assets/gbc_body.png`: atomic-purple Game Boy Color front photo, cropped
  to the 75:133 shell aspect for the textured console background. Source:
  Evan-Amos, via Wikimedia Commons (`File:Game-Boy-Color-Purple.png`),
  released into the public domain (CC0). Game screen and button hit
  regions are calibrated to this image; see `GUIConsole::update_layout`.
- `assets/*.gb(c)`: test ROMs and commercial games for local testing.
