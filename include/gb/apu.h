#pragma once

#include "types.h"
#include <SDL3/SDL_audio.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace gb
{

class MMU; // Bus. APU register access goes through MMU::read/write.

// APU: 2 pulse channels + wave + noise, 512Hz frame sequencer, stereo SDL
// output at 44.1kHz.
// https://gbdev.io/pandocs/Audio.html
// GBC: sample timing derives from the CPU clock (double_speed scales the
// T-cycle rate; M-cycle accounting here is unchanged).
class APU
{
  public:
	static constexpr u32 kSampleRate = 44100;

	APU();
	~APU();

	APU(const APU &) = delete;
	APU &operator=(const APU &) = delete;

	void reset();
	void set_mmu(MMU *mmu)
	{
		mmu_ = mmu;
	}
	void step(u32 mcycles);

	// SDL audio output. Non-fatal: if no device, the core still runs silent.
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
	// Hand buffered samples to the caller (used by --debug WAV capture).
	void take_samples(std::vector<i16> &out);

	u8 read_reg(u16 addr) const;
	void write_reg(u16 addr, u8 value);

	void write_wave_ram(u16 addr, u8 value);
	u8 read_wave_ram(u16 addr) const;

  private:
	MMU *mmu_ = nullptr;

	struct Pulse {
		bool enabled = false;
		bool dac = false;
		u8 duty = 0;         // Waveform shape 0-3.
		u8 duty_pos = 0;     // 0-7 step in the duty pattern.
		u16 freq = 0;        // 11-bit frequency.
		u16 freq_timer = 0;  // Counts down in T-cycles.
		u8 length_timer = 0; // 0-64.
		bool length_enabled = false;
		u8 volume = 0; // Current envelope volume 0-15.
		u8 env_initial = 0;
		bool env_up = false;
		u8 env_period = 0;
		u8 env_timer = 0;
		// Sweep (channel 1 only).
		u8 sweep_pace = 0;
		bool sweep_down = false;
		u8 sweep_slope = 0;
		u8 sweep_timer = 0;
		bool sweep_enabled = false;
		bool sweep_sub_done = false; // A subtraction calc ran since trigger.
		u16 shadow_freq = 0;
	};

	struct Wave {
		bool enabled = false;
		bool dac = false;
		u16 length_timer = 0; // 0-256 (needs 9 bits for full length).
		bool length_enabled = false;
		u8 volume_code = 0; // 0=mute 1=100% 2=50% 3=25%.
		u16 freq = 0;
		u16 freq_timer = 0;
		u8 pos = 0;        // 0-31 sample position.
		u8 out_buffer = 0; // Last nibble read; this is what plays.
		std::array<u8, 16> ram{};
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
		u16 freq_timer = 0;
		u8 clock_shift = 0;
		u8 width_7bit = 0;
		u8 divisor = 0;
	};

	Pulse ch1_, ch2_;
	Wave ch3_;
	Noise ch4_;

	u8 nr50_ = 0; // Master volume L/R.
	u8 nr51_ = 0; // Channel routing L/R.
	bool power_ = false;

	u8 frame_step_ = 0;    // 0-7 frame sequencer position.
	u32 frame_clock_ = 0;  // T-cycles into the current 8192-cycle step.
	u32 sample_clock_ = 0; // T-cycles into the current audio sample.

	// One-pole DC-blocking high-pass state per side (output capacitor
	// emulation). Without this, channel on/off steps leave a wandering DC
	// bias and silence would sit at full-scale negative instead of zero.
	float hp_x_l_ = 0, hp_y_l_ = 0;
	float hp_x_r_ = 0, hp_y_r_ = 0;

	SDL_AudioStream *stream_ = nullptr;
	std::vector<i16> sample_buffer_;
	float master_volume_ = 1.0f;

	// Register shadows for readable bits.
	u8 nr10_ = 0, nr11_ = 0, nr12_ = 0, nr14_ = 0;
	u8 nr21_ = 0, nr22_ = 0, nr24_ = 0;
	u8 nr30_ = 0, nr31_ = 0, nr32_ = 0, nr34_ = 0;
	u8 nr41_ = 0, nr42_ = 0, nr43_ = 0, nr44_ = 0;

	void clock_frame_sequencer();
	void clock_length();
	void clock_envelope();
	void clock_sweep();
	void trigger_pulse(Pulse &ch, bool with_sweep);
	void trigger_wave();
	void trigger_noise();
	void set_length_enabled(bool &flag, bool &enabled, u8 &timer, u8 value);
	u8 pulse_output(Pulse &ch);
	void generate_sample();
	void flush_samples();
};

} // namespace gb
