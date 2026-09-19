#pragma once

#include "types.h"
#include <array>

namespace gb {

class MMU;  // Bus. PPU never touches memory directly; MMU routes via methods below.

// PPU (picture processing unit).
// https://gbdev.io/pandocs/Rendering.html
// https://gbdev.io/pandocs/Tile_Data.html
class PPU {
public:
    enum class Mode : u8 {
        HBlank   = 0,
        VBlank   = 1,
        OAMScan  = 2,
        Drawing  = 3
    };

    struct OAMEntry {
        u8 y = 0, x = 0, tile = 0, flags = 0;
    };

    struct alignas(16) FrameData {
        std::array<u32, SCREEN_WIDTH * SCREEN_HEIGHT> pixels{};
    };

    PPU();
    ~PPU() = default;

    void reset();
    void set_mmu(MMU* mmu) { mmu_ = mmu; }
    void set_hardware_mode(HardwareMode mode) { hardware_mode_ = mode; }
    HardwareMode hardware_mode() const { return hardware_mode_; }
    void step(u32 cycles);
    const FrameData& frame() const { return frame_buffer_; }
    bool frame_ready() const { return frame_ready_flag_; }
    void clear_frame_ready() { frame_ready_flag_ = false; }

    // ---- GBC-Ready Rule 7: rendering isolated behind scanline interface ----
    // DMG writes 4-shade monochrome. GBC will write RGB555 using palettes
    // and tile attributes through this same method (swappable renderer).
    // GBC: tile attributes in VRAM bank 1 will change this path.
    void render_scanline(u8 y);

    // VRAM/OAM access for the Bus ONLY (Rule 2: no direct array access).
    u8 read_vram(u16 addr) const;
    void write_vram(u16 addr, u8 value);
    // DMA/HDMA bypass VRAM/OAM blocking but respect VBK bank selection (per
    // Pan Docs CGB VRAM DMA: dest VRAM bank is FF4F). Used by MMU HDMA.
    void write_vram_dma(u16 addr, u8 value);
    u8 read_oam(u16 addr) const;
    void write_oam(u16 addr, u8 value);

    // DMG LCD registers (https://gbdev.io/pandocs/LCDC.html).
    void write_lcdc(u8 value) { lcdc_ = value; }
    void write_stat(u8 value) { stat_ = (stat_ & 0x07) | (value & 0xF8); }
    void write_scy(u8 value) { scy_ = value; }
    void write_scx(u8 value) { scx_ = value; }
    void write_ly(u8 value) { (void)value; /* LY is read-only */ }
    void write_lyc(u8 value) { lyc_ = value; }
    void write_dma(u8 value);
    void write_bgp(u8 value) { bgp_ = value; }
    void write_obp0(u8 value) { obp0_ = value; }
    void write_obp1(u8 value) { obp1_ = value; }
    void write_wy(u8 value) { wy_ = value; }
    void write_wx(u8 value) { wx_ = value; }

    u8 read_lcdc() const { return lcdc_; }
    u8 read_stat() const { return stat_; }
    u8 read_scy() const { return scy_; }
    u8 read_scx() const { return scx_; }
    u8 read_ly() const { return ly_; }
    u8 read_lyc() const { return lyc_; }
    u8 read_dma() const { return dma_; }
    u8 read_bgp() const { return bgp_; }
    u8 read_obp0() const { return obp0_; }
    u8 read_obp1() const { return obp1_; }
    u8 read_wy() const { return wy_; }
    u8 read_wx() const { return wx_; }

    // ---- CGB registers ----
    // https://gbdev.io/pandocs/CGB_Registers.html
    u8 read_vbk() const { return 0xFE | vram_bank_; }
    void write_vbk(u8 value);
    // Palettes: BCPS/BGPI 0xFF68, BCPD/BGPD 0xFF69, OCPS/OBPI 0xFF6A, OCPD/OBPD 0xFF6B
    u8 read_bgpi() const { return bgpi_; }
    void write_bgpi(u8 value);
    u8 read_bgpd() const;
    void write_bgpd(u8 value);
    u8 read_obpi() const { return obpi_; }
    void write_obpi(u8 value);
    u8 read_obpd() const;
    void write_obpd(u8 value);
    u8 read_opri() const { return 0xFE | opri_; }
    void write_opri(u8 value) { opri_ = value & 0x01; }
    // CGB palette access for tests/debug
    u32 bg_palette_color(u8 palette, u8 color) const;
    u32 obj_palette_color(u8 palette, u8 color) const;
    // DMG 4-shade palette selection (settings.palette index). Applies to the
    // monochrome DMG path only; CGB games use their own RGB555 palettes.
    void set_dmg_palette(int idx);
    int dmg_palette() const { return dmg_palette_; }

private:
    MMU* mmu_ = nullptr;
    HardwareMode hardware_mode_ = HardwareMode::DMG;

    // ---- GBC-Ready Rule 3: VRAM modelled as banked from day one ----
    // DMG uses bank 0 only. GBC uses both (tile attributes live in bank 1).
    // Do NOT flatten to a single 8KB array.
    // GBC: reads/writes will branch on vram_bank_ (VBK register 0xFF4F).
    std::array<std::array<u8, 0x2000>, 2> vram_banks_{};
    u8 vram_bank_ = 0;  // DMG: always 0. GBC: switched via VBK.
    std::array<OAMEntry, 40> oam_{};

    // ---- CGB palettes ----
    // 8 BG palettes + 8 OBJ palettes, 4 colors each, RGB555.
    // Stored as raw 15-bit + cached RGBA8. Access via BCPS/BCPD, OCPS/OCPD.
    // https://gbdev.io/pandocs/Palettes.html
    u8 bgpi_ = 0; // BCPS/BGPI — bit 7 auto-inc, bits 0-5 index
    u8 obpi_ = 0;
    std::array<u8, 64> bgpd_ram_{};  // 64 bytes = 32 colors ×2
    std::array<u8, 64> obpd_ram_{};
    std::array<u32, 64> bg_palette_cache_{};
    std::array<u32, 64> obj_palette_cache_{};
    u8 opri_ = 0; // OPRI 0xFF6C bit0

    // Per-pixel BG info for CGB sprite priority
    std::array<u8, SCREEN_WIDTH * SCREEN_HEIGHT> cgb_bg_color_id_{};
    std::array<u8, SCREEN_WIDTH * SCREEN_HEIGHT> cgb_bg_has_priority_{};

    u8 lcdc_ = 0x91;
    u8 stat_ = 0x85;
    u8 scy_ = 0;
    u8 scx_ = 0;
    u8 ly_ = 0;
    u8 lyc_ = 0;
    u8 dma_ = 0;
    u8 bgp_ = 0xFC;
    u8 obp0_ = 0xFF;
    u8 obp1_ = 0xFF;
    // Active DMG shades (RGBA8), selected via set_dmg_palette().
    std::array<u32, 4> dmg_shades_ = {0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000};
    int dmg_palette_ = 0;
    u8 wy_ = 0;
    u8 wx_ = 0;

    Mode current_mode_ = Mode::OAMScan;
    u32 line_cycles_ = 0;
    u16 window_line_ = 0;  // Internal WY line counter (resets on VBlank/LCD-off).
    FrameData frame_buffer_{};
    bool frame_ready_flag_ = false;

    void render_background(u8 y);
    void render_window(u8 y);
    void render_sprites(u8 y);
    // CGB rendering variants
    void render_background_cgb(u8 y);
    void render_window_cgb(u8 y);
    void render_sprites_cgb(u8 y);
    static u32 rgb555_to_rgba(u16 c);
    u8 color_id_to_shade(u8 palette, u8 color_id) const;
    u32 shade_to_rgba(u8 shade) const;
    void request_vblank_interrupt();
    void request_stat_interrupt();
    void dma_transfer(u8 high);
};

} // namespace gb
