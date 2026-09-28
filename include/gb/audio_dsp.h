#pragma once

// Band-limited PSG output stage, shared by the GB and GBA audio cores.
//
// The APU is a stepped DAC: sampling it once per output sample folds its
// square-wave harmonics and the noise LFSR back into the audible band
// (aliasing buzz that a naive "2-tap average" only smears over). Instead
// the PSG mixes at kMixDiv times the output rate and the result is
// decimated through a windowed-sinc low-pass (65 taps, Blackman window,
// cutoff just under the output Nyquist), which is the same job SameBoy and
// mGBA do with their "high pass"/blip_buf_sample filters.
//
// Signal order (matters, and is the hardware order):
//   oversampled mix -> FIR decimate -> DC blocker (the DMG/GBA output
//   capacitor) -> master volume -> 16-bit clamp.

#include "types.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace gb::dsp
{

// Output streams run at 44.1kHz in both cores; the PSG mixes 8x faster.
inline constexpr u32 kOutputRate = 44100;
inline constexpr unsigned kMixDiv = 8;
inline constexpr u32 kMixRate = kOutputRate * kMixDiv; // 352800Hz
inline constexpr unsigned kTaps = 65;                  // Odd: exact centre tap.

// Cutoff a touch below the output Nyquist (22.05kHz) so the transition band
// lands where the DMG/GBA output has no energy anyway.
inline constexpr double kCutoffHz = 0.45 * static_cast<double>(kOutputRate);

class OutputStage
{
  public:
	OutputStage()
	{
		// Ideal low-pass sinc(2*fc/fs * n), Blackman-windowed, unit DC gain.
		const double fc = 2.0 * kCutoffHz / static_cast<double>(kMixRate);
		const double half = static_cast<double>(kTaps - 1) / 2.0;
		double sum = 0.0;
		for (unsigned k = 0; k < kTaps; k++) {
			const double n = static_cast<double>(k) - half;
			const double x = fc * n;
			const double sinc = (std::fabs(x) < 1e-9) ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
			const double w =
				0.42 + 0.5 * std::cos(M_PI * n / half) + 0.08 * std::cos(2.0 * M_PI * n / half);
			kernel_[k] = static_cast<float>(sinc * w);
			sum += kernel_[k];
		}
		for (float &h : kernel_)
			h = static_cast<float>(h / sum);
	}

	void reset()
	{
		ring_l_.fill(0.0f);
		ring_r_.fill(0.0f);
		pos_ = 0;
		ratio_acc_ = 0;
		dc_l_ = 0.0f;
		dc_prev_l_ = 0.0f;
		dc_r_ = 0.0f;
		dc_prev_r_ = 0.0f;
	}

	void set_volume(float v)
	{
		volume_ = std::clamp(v, 0.0f, 1.0f);
	}
	// Drift correction: resample at `ratio` output frames per kMixDiv mix
	// samples (1.0 = exact). The audio-clock pacer nudges this by up to ±1%
	// to correct the difference between the emulated sample rate and the
	// device's, instead of dropping or repeating whole frames.
	void set_resample_ratio(float ratio)
	{
		ratio_ = std::clamp(ratio, 0.99f, 1.01f);
		ratio_fixed_ = static_cast<u32>(ratio_ * 65536.0f);
	}
	float resample_ratio() const
	{
		return ratio_;
	}
	float volume() const
	{
		return volume_;
	}

	// Feed one mix-rate stereo sample. Returns true (and writes the output
	// pair) on every kMixDiv-th call, i.e. once per kOutputRate.
	bool push(float left, float right, i16 *out_l, i16 *out_r)
	{
		ring_l_[pos_] = left;
		ring_r_[pos_] = right;
		pos_ = (pos_ + 1) & kMask;
		// Fractional decimation: ratio_fixed_/65536 output frames per mix
		// sample, so ±1% costs nothing and never repeats or drops a frame.
		ratio_acc_ += ratio_fixed_;
		if (ratio_acc_ < kMixDiv * 65536u)
			return false;
		ratio_acc_ -= kMixDiv * 65536u;

		// Decimate: sum the windowed kernel over the last kTaps mix samples
		// (the centre tap is the sample just pushed).
		const unsigned newest = (pos_ + kRing - 1) & kMask;
		float acc_l = 0.0f, acc_r = 0.0f;
		for (unsigned k = 0; k < kTaps; k++) {
			const unsigned idx = (newest + kRing - (kTaps - 1) + k) & kMask;
			acc_l += kernel_[k] * ring_l_[idx];
			acc_r += kernel_[k] * ring_r_[idx];
		}
		// One-pole DC blocker: the output capacitor otherwise leaves a
		// wandering bias and silence would not sit at digital zero.
		const float y_l = acc_l - dc_prev_l_ + kDcPole * dc_l_;
		const float y_r = acc_r - dc_prev_r_ + kDcPole * dc_r_;
		dc_prev_l_ = acc_l;
		dc_l_ = y_l;
		dc_prev_r_ = acc_r;
		dc_r_ = y_r;
		*out_l = clampToS16(y_l * volume_);
		*out_r = clampToS16(y_r * volume_);
		return true;
	}

	static i16 clampToS16(float v)
	{
		if (v > 32767.0f)
			return 32767;
		if (v < -32767.0f)
			return -32767;
		return static_cast<i16>(v);
	}

	// ~22Hz corner: below anything the PSG produces, above any DC.
	static constexpr float kDcPole = 0.996f;

  private:
	static constexpr unsigned kRing = 256; // Power of two, > kTaps + kMixDiv.
	static constexpr unsigned kMask = kRing - 1;

	std::array<float, kTaps> kernel_{};
	std::array<float, kRing> ring_l_{};
	std::array<float, kRing> ring_r_{};
	unsigned pos_ = 0;
	u32 ratio_acc_ = 0;
	u32 ratio_fixed_ = 65536; // 1.0
	float ratio_ = 1.0f;
	float dc_l_ = 0.0f, dc_prev_l_ = 0.0f;
	float dc_r_ = 0.0f, dc_prev_r_ = 0.0f;
	float volume_ = 1.0f;
};

} // namespace gb::dsp
