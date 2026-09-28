#pragma once

#include "audio_dsp.h"
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
// The mix runs 8x oversampled and is band-limited on the way out
// (dsp::OutputStage) instead of being sampled straight at 44.1kHz.
class APU
{
  public:
	static constexpr u32 kSampleRate = dsp::kOutputRate;

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
		output_.set_volume(v);
	}
	float master_volume() const
	{
		return output_.volume();
	}
	// Hand buffered samples to the caller (used by --debug WAV capture).
	void take_samples(std::vector<i16> &out);
	// Output frames produced since reset, whether or not a device took them
	// (flush_samples drops the buffer when running silent).
	u64 frames_emitted() const
	{
		return frames_emitted_;
	}
	// Audio batches dropped because the device fell behind (SDL's queued
	// backlog overran). A non-zero value is the audible-crackle signal the
	// debug overlay reports.
	u32 dropouts() const
	{
		return dropouts_;
	}
	// Frames the device has not played yet (0 with no device), and the
	// drift-correction ratio the audio-clock pacer asks for.
	int queued() const;
	void set_resample_ratio(float ratio)
	{
		output_.set_resample_ratio(ratio);
	}

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

	// DAC power-down "pop": a disabled channel's output capacitor does not
	// vanish, it discharges over a few microseconds, so the mix gets a
	// short linear fade instead of a step (the classic click at the end of
	// every note). Indexed 0..3 for ch1..ch4, counted in mix samples.
	static constexpr u8 kFadeMixSamples = 176; // 176/352800 = 0.5ms
	std::array<u8, 4> fade_left_{};
	std::array<u8, 4> fade_level_{};
	// Last non-zero level each channel emitted. The output capacitor holds
	// that level, so a power-down fade starts from it (sampling the live
	// output would often catch a square wave's low phase and skip the
	// click that is being smoothed).
	std::array<u8, 4> last_level_{};
	// Power channel `i` (0..3) down with a fade instead of a hard cut.
	void disable_channel(unsigned index);

	u8 nr50_ = 0; // Master volume L/R.
	u8 nr51_ = 0; // Channel routing L/R.
	bool power_ = false;

	u8 frame_step_ = 0;   // 0-7 frame sequencer position.
	u32 frame_clock_ = 0; // T-cycles into the current 8192-cycle step.
	u32 mix_phase_ = 0;   // 16.16 T-cycles since the last mix sample.

	// Band-limited output: 8x oversampled mix -> FIR decimate -> DC
	// blocker -> volume -> s16. Its kernel/ring own the rate constants.
	dsp::OutputStage output_{};

	// Length counters survive an APU power cycle on DMG hardware (the
	// registers are cleared, the counters are not - SameBoy Core/apu.c
	// restores them after GB_apu_init). Saved when powering off; ch3
	// counts to 256, hence 16 bits.
	std::array<u16, 4> length_shadow_{};

	SDL_AudioStream *stream_ = nullptr;
	std::vector<i16> sample_buffer_;
	u64 frames_emitted_ = 0;
	u32 dropouts_ = 0;

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
	u8 wave_level();
	u8 noise_level();
	void generate_sample();
	// True while the wave DAC owns the wave RAM bus (imminent fetch).
	bool wave_bus_busy() const;
	void flush_samples();
	void power_off_channels();
	void power_on_channels();
};

} // namespace gb
