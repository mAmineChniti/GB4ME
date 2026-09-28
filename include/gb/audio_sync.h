#pragma once

// Audio-clock video pacing (the fix for the known "crackle at 1/60 with a
// 46ms buffer" problem).
//
// The old loop slept a fixed 1/60s per frame and let the SDL audio stream
// follow. Any emulation that runs slightly long, or a sleep that overshoots
// by a millisecond, drains the device buffer and the next put underruns -
// an audible click, and the drift never recovers because both sides are
// nominally locked to different clocks.
//
// This pacer inverts that: the *audio* buffer level is the master clock and
// the video follows it.
//   * Frames are emulated as fast as the CPU allows (no fixed sleep), and
//     the loop then waits only while the device still holds more audio than
//     the target level. When the buffer is low, the frame is not delayed at
//     all, so the device never starves.
//   * The buffer level is measured as `queued - produced_since_last_call`,
//     which strips this frame's own contribution (otherwise the measurement
//     is always one frame stale and the loop oscillates).
//   * A long-run ratio of "how much the level moved" vs "what we produced"
//     exposes the difference between the emulated clock and the device's;
//     that is fed back as a ±1% resample ratio on the PSG output stage, so
//     the error is absorbed smoothly instead of by dropping frames.
//
// SDL-free by design: the caller passes the queue level in, which keeps the
// controller unit-testable (see the selftest's "Audio clock pacing" section).

#include "types.h"
#include <algorithm>

namespace gb
{

class AudioSync
{
  public:
	// `rate` is the output sample rate (44100), `device_buffer` the device's
	// buffer size in frames if known (0 = unknown -> a 46ms assumption).
	void reset(u32 rate, u32 device_buffer)
	{
		rate_ = rate != 0 ? rate : 44100;
		// Keep the device roughly half full: enough runway for a scheduling
		// hiccup, little enough that input->audio latency stays low.
		target_ = device_buffer != 0 ? device_buffer / 2 : rate_ * 46 / 2000;
		target_ = std::max<u32>(target_, rate_ / 100); // never below 10ms
		// Below this the device is about to underrun: no delay allowed.
		low_ = target_ / 2;
		produced_acc_ = 0;
		produced_window_ = 0;
		level_start_ = 0;
		window_frames_ = 0;
		ratio_ = 1.0f;
		dropped_ = 0;
		starved_ = 0;
	}

	// One emulated video frame. `queued` is what SDL reports as still
	// buffered (i.e. not yet played) and `produced` the frames this frame
	// generated. Returns the seconds the caller should sleep before the next
	// frame (0 = run flat out).
	double pace(u32 queued, u32 produced)
	{
		// The level the device is really playing from: its own backlog minus
		// what we just handed it.
		const i64 level = static_cast<i64>(queued) - static_cast<i64>(produced);
		// Drift window: what matters is not the level itself but how it
		// MOVES. A rising level means we hand the device more audio than it
		// plays, so the emulated rate has to come down; a falling level means
		// the opposite. One second of frames is a long enough window that
		// ordinary jitter averages out.
		if (window_frames_ == 0)
			level_start_ = level;
		produced_window_ += produced;
		if (++window_frames_ >= 60) {
			const f64 made = static_cast<f64>(produced_window_);
			if (made > 0.0) {
				const f64 drift = (static_cast<f64>(level) -
								   static_cast<f64>(level_start_)) /
								  made; // signed, per produced frame
				// Clamp the per-update step so a stall cannot slam the ratio.
				const f64 adj = std::clamp(-drift * 0.5, -0.002, 0.002);
				ratio_ = std::clamp(static_cast<f32>(ratio_ + adj), 0.99f, 1.01f);
			}
			produced_window_ = 0;
			window_frames_ = 0;
		}
		produced_acc_ += produced;

		// Only wait while there is genuinely too much audio buffered. Waiting
		// is how video slows down to match audio, which is what keeps A/V in
		// sync without a separate sync model.
		if (static_cast<u32>(std::max<i64>(level, 0)) > target_) {
			// Convert the excess into seconds of playback, but never sleep
			// for a whole frame: the next frame's own audio is the finer
			// unit, and a long sleep here is what starves the device.
			const f64 excess = (static_cast<f64>(level) - static_cast<f64>(target_)) /
							  static_cast<f64>(rate_);
			const f64 frame = 1.0 / 60.0;
			return std::min(excess, frame);
		}
		if (level < static_cast<i64>(low_))
			++starved_;
		return 0.0;
	}

	// Current drift correction to hand to the PSG output stage.
	float resample_ratio() const
	{
		return ratio_;
	}
	// Frames the device was ready to play but the loop had none to give.
	u32 starved_frames() const
	{
		return starved_;
	}
	// Batches the audio backend had to throw away to catch up.
	u32 dropped_batches() const
	{
		return dropped_;
	}
	void note_drop()
	{
		++dropped_;
	}
	// Target level in frames, exposed for the overlay/tests.
	u32 target_frames() const
	{
		return target_;
	}
	u32 produced_total() const
	{
		return produced_acc_;
	}

  private:
	u32 rate_ = 44100;
	u32 target_ = 1024;
	u32 low_ = 512;
	u32 produced_acc_ = 0;
	u32 produced_window_ = 0;
	i64 level_start_ = 0;
	u32 window_frames_ = 0;
	float ratio_ = 1.0f;
	u32 dropped_ = 0;
	u32 starved_ = 0;
};

} // namespace gb
