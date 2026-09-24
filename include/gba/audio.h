#pragma once

#include "gba/state.h"
#include <SDL3/SDL_audio.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace gba
{

class GbaBus;
class GbaDma;

// GBA audio (Phase 8): 4 PSG channels (GB-derived rates, no DIV-phase
// quirks) + 2 Direct Sound FIFOs, mixed to stereo SDL output at 44.1kHz.
// Fresh implementation (not the GB APU): wave channel has 2 banks,
// envelope/length/sweep run on nominal GBA rates, FIFO consumption is
// timer-driven with DMA refill.
// https://problemkaputt.de/gbatek.htm#gbasoundcontroller
class GbaAudio
{
  public:
	static constexpr u32 kSampleRate = 44100;

	GbaAudio();
	~GbaAudio();

	GbaAudio(const GbaAudio &) = delete;
	GbaAudio &operator=(const GbaAudio &) = delete;

	void reset();
	void setDma(GbaDma *dma)
	{
		dma_ = dma;
	}

	// Advance DSP + sample clock by ticks. FIFO consumption happens via
	// onTimerOverflow (timer 0/1 per SOUNDCNT_H select).
	void step(u32 ticks);
	void onTimerOverflow(unsigned timer);

	// SDL output (same best-effort pattern as the GB APU).
	bool open_audio();
	void close_audio();
	bool has_audio() const
	{
		return stream_ != nullptr;
	}
	void set_master_volume(float v)
	{
		master_volume_ = std::clamp(v, 0.0f, 1.0f);
	}
	float master_volume() const
	{
		return master_volume_;
	}
	void take_samples(std::vector<i16> &out);

	// Bus-facing sound block (0x04000060-0x040000A7).
	u8 read8(u32 addr) const;
	void write8(u32 addr, u8 value);
	u16 read16(u32 addr) const;
	void write16(u32 addr, u16 value);
	// Diagnostics/tests: FIFO fill level and held DAC byte.
	unsigned fifoACount() const
	{
		return fifo_a_.count;
	}
	i8 fifoADac() const
	{
		return fifo_a_.dac;
	}
	unsigned fifoBCount() const
	{
		return fifo_b_.count;
	}
	i8 fifoBDac() const
	{
		return fifo_b_.dac;
	}
	// Diagnostics/tests: current PSG frequency reload periods.
	u32 pulse1Period() const
	{
		return ch1_.freq_timer;
	}
	u32 wavePeriod() const
	{
		return ch3_.freq_timer;
	}

	// Save state serialization.
	void save(StateBuffer &out) const;
	void load(const StateBuffer &in);

  private:
	GbaDma *dma_ = nullptr;

	struct Pulse {
		bool enabled = false;
		bool dac = false;
		u8 duty = 0;
		u8 duty_pos = 0;
		u16 freq = 0;
		u16 freq_timer = 0;
		u8 length_timer = 0;
		bool length_enabled = false;
		u8 volume = 0;
		u8 env_initial = 0;
		bool env_up = false;
		u8 env_period = 0;
		u8 env_timer = 0;
		u8 sweep_pace = 0;
		bool sweep_down = false;
		u8 sweep_slope = 0;
		u8 sweep_timer = 0;
		bool sweep_enabled = false;
		u16 shadow_freq = 0;
	};

	struct Wave {
		bool enabled = false;
		bool dac = false;
		u8 bank = 0;            // Playing bank (reads/writes see the other).
		bool two_banks = false; // 64-digit mode (plays selected, then other).
		u16 length_timer = 0;
		bool length_enabled = false;
		u8 volume_code = 0;
		bool force_volume = false;
		u16 freq = 0;
		u16 freq_timer = 0;
		u8 pos = 0;               // 0-31 (or 0-63 in two-bank mode).
		std::array<u8, 32> ram{}; // Both 16-byte banks back to back.
	};

	struct Noise {
		bool enabled = false;
		bool dac = false;
		u8 length_timer = 0;
		bool length_enabled = false;
		u8 volume = 0;
		u8 env_initial = 0;
		bool env_up = false;
		u8 env_period = 0;
		u8 env_timer = 0;
		u16 lfsr = 0;
		// Noise periods reach 112 << 14 = 1835008, far past 16 bits, so this
		// must be 32-bit (mGBA uses int32_t for the same reason).
		u32 freq_timer = 0;
		u8 clock_shift = 0;
		u8 width_7bit = 0;
		u8 divisor = 0;
	};

	struct Fifo {
		std::array<u8, 32> data{};
		u8 read_pos = 0;
		u8 write_pos = 0;
		u8 count = 0;
		i8 dac = 0; // Last consumed byte (held on underrun).
	};

	Pulse ch1_, ch2_;
	Wave ch3_;
	Noise ch4_;
	Fifo fifo_a_, fifo_b_;

	u8 nr50_ = 0; // PSG master volume L/R.
	u8 nr51_ = 0; // PSG routing L/R.
	bool power_ = false;
	u16 soundcnt_h_ = 0; // DMA mixing/volume/timer-select/FIFO-reset.
	u16 soundbias_ = 0x200;

	// Rate dividers in ticks (nominal GBA PSG rates, GBATEK units).
	u32 length_clock_ = 0;   // 256 Hz: 65536 ticks.
	u32 envelope_clock_ = 0; // 64 Hz: 262144 ticks.
	u32 sweep_clock_ = 0;    // 128 Hz: 131072 ticks.
	u32 sample_clock_ = 0;   // 44100 Hz output.
	u32 sample_frac_ = 0;   // Fixed-point dither for the exact sample period.

	float hp_x_l_ = 0, hp_y_l_ = 0;
	float hp_x_r_ = 0, hp_y_r_ = 0;
	// Gentle 2-tap output low-pass (softens the raw DAC stair-step edge;
	// VBA-M defaults its PCM interpolation/low-pass on, mGBA band-limits
	// its resampler — unfiltered output sounds harsh/stingy by comparison).
	// Ephemeral post-mix state: intentionally not saved (1-sample transient).
	float lp_l_ = 0, lp_r_ = 0;

	SDL_AudioStream *stream_ = nullptr;
	std::vector<i16> sample_buffer_;
	float master_volume_ = 1.0f;

	// Register shadows for readable bits.
	u8 nr10_ = 0, nr11_ = 0, nr12_ = 0, nr14_ = 0;
	u8 nr21_ = 0, nr22_ = 0, nr24_ = 0;
	u8 nr30_ = 0, nr31_ = 0, nr32_ = 0, nr34_ = 0;
	u8 nr41_ = 0, nr42_ = 0, nr43_ = 0, nr44_ = 0;

	void clockLength();
	void clockEnvelope();
	void clockSweep();
	void triggerPulse(Pulse &ch, bool with_sweep);
	void triggerWave();
	void triggerNoise();
	u8 pulseOut(Pulse &ch);
	u8 waveOut();
	void consumeFifo(Fifo &fifo, unsigned dma_ch);
	void pushFifo(Fifo &fifo, u16 value);
	void generateSample();
	void emitStereo(i16 left, i16 right);
	void flushSamples();
	// Wave RAM byte access honoring bank visibility (reads/writes see the
	// non-playing bank).
	u8 readWave(u32 addr) const;
	void writeWave(u32 addr, u8 value);
};

} // namespace gba
