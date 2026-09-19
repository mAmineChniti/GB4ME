# Game Boy Color Emulator GUI + CGB Core — Agent Instructions

## Project Stack
- **Language**: C++23
- **Window/Input/Rendering**: SDL3 + Vulkan
- **UI Framework**: Custom SDL3 rendering (or FabKit — https://github.com/FabianPeters/fabkit)
- **Build System**: CMake 3.28+
- **Target**: A visually accurate Game Boy Color emulator GUI with **working CGB emulation core**.

## Project Goal
Build a GUI that replicates the physical Game Boy Color console, **AND implement full Game Boy Color (CGB) emulation mode** on top of the existing DMG core. The window should look like a Game Boy Color unit, with clickable buttons for input mapping and a settings panel.

**Two tracks run in parallel:**
1. **GUI track** — replicate the physical GBC console visually with input mapping and settings.
2. **Core track** — activate CGB mode: double-speed CPU, color palettes, banked VRAM, tile attributes.

---

## PART 1: CGB Core Implementation

### CGB Hardware Differences From DMG (Activate the Hooks)

Your DMG emulator already has the architecture hooks in place (banked VRAM, variable clock speed, `double_speed` flag, `HardwareMode` enum). **Now activate them.**

| Component | DMG | CGB |
|---|---|---|
| **CPU** | 4.194304 MHz | 4.194304 MHz / 8.388608 MHz (double-speed) |
| **VRAM** | 8 KB (1 bank) | 16 KB (2 banks) |
| **WRAM** | 8 KB (1 bank) | 32 KB (8 banks of 4KB) |
| **Palettes** | Fixed 4-shade | 8 BG palettes + 8 OBJ palettes, each 4 colors of RGB555 |
| **Tile attributes** | None | Per-tile priority, Y-flip, X-flip, bank, palette |
| **Registers** | DMG set | Adds KEY1, VBK, SVBK, RP, HDMA1-5, BCPS/BCPD, OCPS/OCPD, OPRI, FF72-FF75 |
| **Interrupts** | 5 sources | Adds CGB-specific triggers |
| **Boot** | DMG boot ROM | CGB boot ROM (colorizes DMG games) |

### CGB Implementation Order

#### 1. CGB Mode Detection
- Read cartridge header byte at `0x0143` (CGB flag).
- Values: `0x80` = CGB-enhanced (runs on DMG too), `0xC0` = CGB-only.
- Set `HardwareMode::CGB` or `HardwareMode::CGB_Compatible`.
- When CGB mode active, **do not run DMG boot ROM** — use CGB boot ROM or direct boot.

#### 2. CGB Boot ROM (Optional But Recommended for Colorization)
- CGB boot ROM colorizes DMG games by writing to palette registers before jumping to the game.
- Boot ROM is 2304 bytes (vs 256 for DMG).
- Reference: Pan Docs CGB section — https://gbdev.io/pandocs/CGB_Registers.html
- If you skip the boot ROM, DMG games will render with default CGB palettes (not colorized).

#### 3. CPU Double-Speed Mode
- Register `KEY1` at `0xFF4D`:
  - Bit 7: current speed (0 = normal, 1 = double)
  - Bit 0: request speed switch (write 1 to request)
- Switch occurs after `STOP` instruction executes.
- When `double_speed` is active, **CPU executes twice as many cycles per frame** (or the PPU/APU run at half speed relative to CPU — pick one model and be consistent).
- **Recommended model**: Keep PPU/APU timing fixed; CPU clock variable.
- Update `double_speed` flag from `KEY1` reads.

#### 4. VRAM Banking
- Register `VBK` at `0xFF4F`:
  - Bit 0: VRAM bank (0 or 1)
- All VRAM reads/writes go through `VBK` to select bank.
- PPU reads VRAM bank 0 for tile data, bank 1 for tile attributes in CGB mode.
- **Your existing banked VRAM array is already the right shape** — just wire up `VBK`.

#### 5. WRAM Banking
- Register `SVBK` at `0xFF70`:
  - Bits 0-2: WRAM bank (1-7). Bank 0 is always mapped to `0xC000-0xCFFF`.
- Address `0xD000-0xDFFF` maps to selected bank via `SVBK`.
- **Your existing banked WRAM array is already the right shape** — just wire up `SVBK`.

#### 6. CGB Palettes
Two register pairs:
- **BG Palette**: `BCPS` (`0xFF68`) index, `BCPD` (`0xFF69`) data
- **OBJ Palette**: `OCPS` (`0xFF6A`) index, `OCPD` (`0xFF6B`) data

Each palette is 8 entries of 4 colors each. Each color is 15-bit RGB555.

**Palette index register format**:
- Bits 0-5: color index (0-63)
- Bit 7: auto-increment flag

**When writing to index register**: Set auto-increment if desired, then write color data to data register.

**Rendering**: In CGB mode, the PPU looks up colors from these palettes instead of the fixed 4-shade DMG palette.

#### 7. Tile Attributes (CGB Only)
In CGB mode, VRAM bank 1 stores **tile attributes** for each tile map entry:
- Bit 0-2: BG palette number (0-7)
- Bit 3: Tile VRAM bank (0 or 1)
- Bit 4: X-flip
- Bit 5: Y-flip
- Bit 6: Priority (0 = above OBJ, 1 = behind OBJ when BG-to-OAM priority set)
- Bit 7: BG-to-OAM priority

**Read from VRAM bank 1** when rendering BG/window in CGB mode.

#### 8. PPU Rendering Update
Your existing `render_scanline(y)` interface needs a CGB branch:

```cpp
if (hardware_mode == HardwareMode::CGB) {
    // CGB rendering path
    // 1. Read tile map entry (BG map)
    // 2. Read tile attribute from VRAM bank 1 (using VBK = 1)
    // 3. Apply X/Y flip
    // 4. Select palette from attribute bits 0-2
    // 5. Look up color in BG palette (BCPS/BCPD or cached)
    // 6. Write RGB555 to framebuffer
} else {
    // DMG rendering path (existing)
    // Write 4-shade monochrome to framebuffer
}
Framebuffer format: In CGB mode, output RGB555 (15-bit color) instead of 2-bit shade. Adjust the Vulkan texture format accordingly.
9. HDMA / GDMA (Optional, But Many CGB Games Use It)

    Registers HDMA1-5 at 0xFF51-0xFF55:

        HDMA1/2: Source address high/low

        HDMA3/4: Destination address high/low

        HDMA5: Length/mode (bit 7 = HBlank vs General purpose)

    GDMA (General Purpose DMA): Bit 7 = 0. Transfers all data immediately.

    HDMA (HBlank DMA): Bit 7 = 1. Transfers 16 bytes per HBlank.

    Used for fast VRAM updates, common in CGB games for animations.

    Start with GDMA only — it's simpler and covers most early testing.

10. CGB-Specific Registers

Implement these at minimum:

    0xFF4D — KEY1 (speed switch)

    0xFF4F — VBK (VRAM bank)

    0xFF70 — SVBK (WRAM bank)

    0xFF68-0xFF6B — Palette registers (BG + OBJ)

    0xFF6C — OPRI (OBJ priority mode)

    0xFF72-0xFF73 — Boot ROM control

    0xFF74 — Unused

    0xFF75 — Unused (read as 0xFF)

    0xFF51-0xFF55 — HDMA1-5

CGB References

    Pan Docs — CGB Registers: https://gbdev.io/pandocs/CGB_Registers.html

    Pan Docs — Rendering (includes CGB): https://gbdev.io/pandocs/Rendering.html

    Pan Docs — Palettes: https://gbdev.io/pandocs/Palettes.html

    Pan Docs — VRAM Tile Data: https://gbdev.io/pandocs/Tile_Data.html

    Pan Docs — LCD Registers (VBK, SVBK): https://gbdev.io/pandocs/LCDC.html

    CGB Boot ROM Disassembly: https://gbdev.gg8.se/wiki/articles/Gameboy_Bootstrap_ROM

    SameBoy Source (best CGB accuracy reference): https://github.com/LIJI32/SameBoy

    Binjgb Source (simpler CGB reference): https://github.com/binji/binjgb

CGB Test ROMs

    Blargg's CGB test ROMs: https://github.com/retrio/gb-test-roms (in cgb_sound/ and cpu_instrs/)

    dmg-acid2 (DMG accuracy): https://github.com/mattcurrie/dmg-acid2

    cgb-acid2 (CGB accuracy — REQUIRED for CGB testing): https://github.com/mattcurrie/cgb-acid2

    SameBoy test ROMs: https://github.com/LIJI32/SameBoy/tree/master/TestRoms

PART 2: GUI Implementation
Physical Game Boy Color Layout Reference

Based on the physical console documentation (https://en.wikipedia.org/wiki/Game_Boy_Color):
Element	Location
Screen	Center, 44 x 39mm display
D-Pad	Left of screen
A and B Buttons	Right of screen (diagonal, A above-right of B)
Start / Select	Below screen, angled
Power LED	Left of screen (top)
Speaker	Bottom-right of unit
Power Switch	Right side
Volume Dial	Left side
Link Port	Top
IR Port	Top (next to link port)
Headphone Jack	Bottom
1. Main Console Window

    Aspect Ratio: Match physical GBC dimensions (75mm wide x 133mm tall).

    Body: Rounded rectangle with color options (Purple, Green, Blue, Yellow, Clear Purple).

    Screen Bezel: Dark border around the emulator display area.

    Buttons: All buttons are clickable and mappable.

2. Clickable Buttons (Input Mapping)

Every physical button is interactive:

    D-Pad (Up, Down, Left, Right)

    A Button

    B Button

    Start

    Select

Mapping Flow:

    User clicks a button on the GUI (e.g., the A button).

    A modal appears: "Press the key you want to map to A."

    User presses a keyboard key.

    The mapping is saved and displayed on/near the button.

Reference: melonDS uses a similar "click button, then press key" approach (https://melonds.kuribo64.net/faq.php).
3. Settings Panel

A settings button (gear icon on console body, or where Start/Select area is) opens a settings window.

Settings Window Content:
Setting	Description
ROM Folder	Path to default folder containing all ROMs
Input Mapping	Remap all buttons (D-Pad, A, B, Start, Select)
Volume	Slider or +/- controls
Video Scale	Window scaling options
Palette	GBC color palette selection (CGB mode)
Save States	Save/load state management
Controller	Gamepad mapping (if using a physical controller)
Hardware Mode	DMG / CGB / Auto-detect

Reference: Emulation General Wiki on GBC emulators (https://emulation.gametechwiki.com/index.php/Game_Boy_Color_emulators).
4. CGB-Specific GUI Elements

    Hardware Mode Indicator: Show "DMG" or "CGB" badge on the console body.

    Color Palette Picker: For CGB mode, allow selecting from built-in palettes.

    Speed Indicator: Show "1x" or "2x" when double-speed mode is active.

    Framebuffer Format: Switch Vulkan texture format between 2-bit (DMG) and RGB555 (CGB) based on mode.

Development Order Rules (GUI + Core Interleaved)

    Console Window Shell: Create the GBC body shape with correct proportions.

    Screen Area: Render a placeholder or the emulator output in the screen bezel.

    CGB Mode Detection: Read cartridge header, set HardwareMode.

    CGB VRAM/WRAM Banking: Wire up VBK and SVBK registers.

    CGB Palette Registers: Implement BCPS/BCPD, OCPS/OCPD.

    CGB PPU Rendering: Add CGB branch to render_scanline(y), output RGB555.

    Static Buttons: Draw D-Pad, A, B, Start, Select in correct positions.

    Button Click Detection: Detect clicks on each button region.

    Input Mapping Modal: Show modal on button click, capture keypress, save mapping.

    CGB Double-Speed Mode: Implement KEY1 register and STOP behavior.

    Settings Button: Add settings access point (gear icon or dedicated button).

    Settings Window: Build the settings panel with all options.

    ROM Folder Selection: File dialog for choosing ROM directory.

    Volume Controls: Slider tied to audio system.

    CGB HDMA/GDMA: Implement HDMA1-5 (start with GDMA).

    Persist Settings: Save/load configuration file.

C++23 + SDL3 Specific Guidance
UI Framework Options

    FabKit: WIP UI component library for SDL3 using C++23 modules — https://github.com/FabianPeters/fabkit

    SDL3pp: C++ port of SDL3 with RAII wrappers — https://github.com/Geod24/sdl3pp

    Custom: Draw the GBC shape using SDL3 primitives (rectangles, rounded corners) and handle click regions manually.

Button Hit Detection

Define a struct ButtonRegion { float x, y, w, h; std::string name; } for each physical button. On mouse click, check if the click falls within any button's bounding box.
Modal Input Capture

When a button is clicked for mapping, set a flag awaiting_input = true and store which button is being mapped. On the next SDL_EVENT_KEY_DOWN, capture the keycode, save the mapping, and clear the flag.
Framebuffer Format Switching
cpp

// DMG mode: 2-bit shade index per pixel (4 shades)
// CGB mode: 15-bit RGB555 per pixel
// Vulkan texture format switches between R8_UINT (DMG) and R5G5B5A1_UNORM_PACK16 (CGB)

Palette Caching

Cache decoded RGB555 colors from BCPS/BCPD and OCPS/OCPD writes. On each render_scanline(y) call, read from cache instead of decoding every pixel.
Code Style Requirements

    Separate GUI rendering from emulator core logic.

    Use a GUIConsole class that owns the console layout data and button regions.

    Keep settings in a Settings struct with save/load methods.

    Separate DMG and CGB rendering paths in the PPU — keep both readable.

    Add comments citing physical GBC layout dimensions and CGB register addresses.

Prohibited

    Do NOT hardcode window size without respecting aspect ratio.

    Do NOT implement settings persistence until the settings panel works.

    Do NOT block the emulator loop during input mapping modals.

    Do NOT mix DMG and CGB rendering logic in the same function — use separate branches.

    Do NOT skip cgb-acid2 testing.

Reference Images/Layout Data

    GBC dimensions: Width 75mm, Depth 27mm, Height 133mm

    Screen: 44 x 39mm

    A/B buttons: diagonal arrangement, A is upper-right, B is lower-left

    Volume dial: left side

    Power switch: right side

    IR port: top

External References

    melonDS FAQ (input mapping reference): https://melonds.kuribo64.net/faq.php

    FabKit UI library: https://github.com/FabianPeters/fabkit

    SDL3pp C++ wrapper: https://github.com/Geod24/sdl3pp

    Glaze JSON library (for settings persistence): https://github.com/stephenberry/glaze

    Pan Docs CGB Registers: https://gbdev.io/pandocs/CGB_Registers.html

    Pan Docs Rendering: https://gbdev.io/pandocs/Rendering.html

    Pan Docs Palettes: https://gbdev.io/pandocs/Palettes.html

    cgb-acid2 test ROM: https://github.com/mattcurrie/cgb-acid2

    SameBoy (CGB accuracy reference): https://github.com/LIJI32/SameBoy

    Binjgb (simpler CGB reference): https://github.com/binji/binjgb
