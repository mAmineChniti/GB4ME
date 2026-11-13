// APU: DMG sound hardware. Four channels share a 512Hz frame sequencer
// (length 256Hz, sweep 128Hz, envelope 64Hz); mixed stereo pushed to SDL.
// https://gbdev.io/pandocs/Audio.html
//
// Read masks follow Pan Docs (unmapped bits read 1, write-only regs read FF).

#include "gb/apu.h"
#include <SDL3/SDL_hints.h>

#include <cstdio>

namespace gb
{

namespace
{

// T-cycles per frame-sequencer step (512Hz at DMG clock).
constexpr u32 kFrameStepTcycles = 8192;
// T-cycles per *mix* sample at 8x the output rate, 16.16 fixed point:
// 4194304/352800 = 11.888118. Accumulating instead of dithering an
// integer 11/12 keeps the long-run rate exact and spreads the residual
// evenly instead of running a constant ~1-cycle late.
constexpr u32 kMixPeriodFx = 779045; // 11.888118 * 65536
constexpr u32 kOneTcycleFx = 1u << 16;
// DMG full-scale: four channels of 0-15 times an NR50 master of 1-8 is
// 480; 32767/480 = 68.3, so 68 lands the loudest mix just inside int16.
constexpr float kMixScale = 68.0f;
// Cycles before a wave RAM fetch in which the bus is owned by the DAC and
// CPU wave RAM accesses conflict (mGBA src/gb/io.c: 4 cycles).
// Width of the wave-RAM bus-conflict window, in T-cycles. The DAC fetches one
// byte on a 16-bit bus, so the fetch owns the bus for two cycles; Blargg's
// dmg_sound 09/10/12 sweep the read phase across the sample period and expect
// open bus everywhere except the fetch itself.
// Width of the conflict window in T-cycles, measured back from the fetch.
// mGBA blanks accesses in the last few cycles before the next sample.
// SameBoy models the DMG wave-RAM bus window as a *single* cycle
// (`wave_form_just_read`: set when the APU update ended exactly on a sample
// fetch, cleared if it ended anywhere else), not as a window around the fetch.
// A four-cycle window makes every access in a period succeed; hardware lets
// through one, which is what Blargg dmg_sound 09/10/12 measure with their CRC
// over the printed bytes.

constexpr u8 kWaveShifts[4] = {4, 0, 1, 2}; // NR32 volume code -> shift.
constexpr u8 kDutyTable[4][8] = {
	{0, 0, 0, 0, 0, 0, 0, 1}, // 12.5%
	{1, 0, 0, 0, 0, 0, 0, 1}, // 25%
	{1, 0, 0, 0, 0, 1, 1, 1}, // 50%
	{0, 1, 1, 1, 1, 1, 1, 0}, // 25% (negated)
};

} // namespace

APU::APU()
{
	reset();
}

APU::~APU()
{
	close_audio();
}

void APU::reset()
{
	ch1_ = Pulse{};
	ch2_ = Pulse{};
	ch3_ = Wave{};
	ch4_ = Noise{};
	// Post-boot register state (Pan Docs "Power Up Sequence"). The APU is
	// powered ON ($F0) with all channels silent. Tests and games observe
	// these values before touching sound.
	power_ = true;
	nr10_ = 0x00;
	nr11_ = 0xBF;
	nr12_ = 0xF3;
	nr14_ = 0x40;
	nr21_ = 0x00;
	nr22_ = 0x00;
	nr24_ = 0x40;
	nr30_ = 0x00;
	nr31_ = 0xFF;
	nr32_ = 0x00;
	nr34_ = 0x40;
	nr41_ = 0xFF;
	nr42_ = 0x00;
	nr43_ = 0x00;
	nr44_ = 0x40;
	nr50_ = 0x77;
	nr51_ = 0xF3;
	// Derive channel state touched by those register values.
	ch1_.duty = nr11_ >> 6;
	ch1_.length_timer = 64 - (nr11_ & 0x3F);
	ch1_.length_enabled = true;
	ch1_.dac = (nr12_ & 0xF8) != 0;
	ch1_.env_initial = nr12_ >> 4;
	ch1_.env_period = nr12_ & 7;
	ch2_.duty = nr21_ >> 6;
	ch2_.length_timer = 64 - (nr21_ & 0x3F);
	ch2_.length_enabled = true;
	ch2_.dac = (nr22_ & 0xF8) != 0;
	ch3_.length_timer = 256 - nr31_;
	ch3_.length_enabled = true;
	ch3_.dac = (nr30_ & 0x80) != 0;
	ch3_.volume_code = (nr32_ >> 5) & 3;
	ch4_.length_timer = 64 - (nr41_ & 0x3F);
	ch4_.length_enabled = true;
	ch4_.dac = (nr42_ & 0xF8) != 0;
	frame_step_ = 0;
	frame_clock_ = 0;
	mix_phase_ = 0;
	frames_emitted_ = 0;
	fade_left_.fill(0);
	fade_level_.fill(0);
	last_level_.fill(0);
	output_.reset();
	length_shadow_ = {0, 0, 0, 0};
	sample_buffer_.clear();
	sample_buffer_.reserve(kSampleRate / 10 * 2);
}

bool APU::open_audio()
{
	if (stream_ != nullptr)
		return true;
	SDL_AudioSpec want{};
	want.format = SDL_AUDIO_S16;
	want.channels = 2;
	want.freq = kSampleRate;
	// Bounded device buffer (~46ms); see GbaAudio::open_audio.
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "2048");
	stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, nullptr, nullptr);
	if (stream_ == nullptr) {
#ifndef GB4ME_RELEASE
		std::printf("audio: no device (%s), running silent\n", SDL_GetError());
#endif
		return false;
	}
	SDL_ResumeAudioStreamDevice(stream_);
	return true;
}

void APU::close_audio()
{
	flush_samples();
	if (stream_ != nullptr) {
		SDL_DestroyAudioStream(stream_);
		stream_ = nullptr;
	}
}

int APU::queued() const
{
	return stream_ != nullptr ? SDL_GetAudioStreamQueued(stream_) : 0;
}

void APU::take_samples(std::vector<i16> &out)
{
	out.insert(out.end(), sample_buffer_.begin(), sample_buffer_.end());
	sample_buffer_.clear();
}

void APU::step(u32 mcycles)
{
	// GBC: T-cycle rate scales with double_speed; M-cycle counts unchanged.
	const u32 tcycles = mcycles * 4;
	for (u32 i = 0; i < tcycles; i++) {
		const bool last_cycle = i + 1 == tcycles;
		// Frame sequencer.
		if (++frame_clock_ >= kFrameStepTcycles) {
			frame_clock_ = 0;
			clock_frame_sequencer();
		}
		// Channel frequency timers.
		if (ch1_.enabled && --ch1_.freq_timer == 0) {
			ch1_.freq_timer = (2048 - ch1_.freq) * 4;
			ch1_.duty_pos = (ch1_.duty_pos + 1) & 7;
		}
		if (ch2_.enabled && --ch2_.freq_timer == 0) {
			ch2_.freq_timer = (2048 - ch2_.freq) * 4;
			ch2_.duty_pos = (ch2_.duty_pos + 1) & 7;
		}
		const bool wave_fetched = ch3_.enabled && --ch3_.freq_timer == 0;
		// The wave RAM only answers on the cycle the DAC actually fetched, so
		// the flag survives exactly when the step ended on that fetch.
		wave_just_read_ = wave_fetched && last_cycle;
		if (wave_fetched) {
			ch3_.freq_timer = (2048 - ch3_.freq) * 2;
			ch3_.pos = (ch3_.pos + 1) & 31;
			const u8 byte = ch3_.ram[ch3_.pos / 2];
			ch3_.out_buffer = (ch3_.pos % 2 == 0) ? (byte >> 4) : (byte & 0x0F);
		}
		if (ch4_.enabled && ch4_.clock_shift < 14 && --ch4_.freq_timer == 0) {
			// LFSR: XNOR of bits 0,1 into bit 15 (bit 7 too in short mode),
			// then shift right; bit 0 selects the output. Shift 14/15 gets
			// no clocks at all.
			// https://gbdev.io/pandocs/Audio_details.html#noise-channel-ch4
			static constexpr u16 kDivisors[8] = {8, 16, 32, 48, 64, 80, 96, 112};
			ch4_.freq_timer = kDivisors[ch4_.divisor] << ch4_.clock_shift;
			const u16 xor_bit = ((ch4_.lfsr & 1) == ((ch4_.lfsr >> 1) & 1)) ? 1 : 0;
			ch4_.lfsr = (ch4_.lfsr >> 1) | (xor_bit << 14);
			if (ch4_.width_7bit)
				ch4_.lfsr = (ch4_.lfsr & ~0x40) | (xor_bit << 6);
		}
		// Mix clock: the PSG mixes 8x the output rate and OutputStage
		// decimates it band-limited.
		mix_phase_ += kOneTcycleFx;
		if (mix_phase_ >= kMixPeriodFx) {
			mix_phase_ -= kMixPeriodFx;
			generate_sample();
		}
	}
}

void APU::clock_frame_sequencer()
{
	// The DIV-APU phase and all counter effects run continuously, even while
	// powered off: the tests' sync routines power-cycle and wait for length
	// expiries observed via NR52, which can only fire if clocks keep firing.
	// Power gates channels, registers and writes -- never the sequencer.
	frame_step_ = (frame_step_ + 1) & 7;
	if (frame_step_ % 2 == 0)
		clock_length(); // 256Hz.
	if (frame_step_ == 2 || frame_step_ == 6)
		clock_sweep(); // 128Hz.
	if (frame_step_ == 7)
		clock_envelope(); // 64Hz.
}

void APU::disable_channel(unsigned index)
{
	// Only fade a channel that was actually contributing, so a
	// disabled-but-silent channel does not add a phantom step later.
	if (index < 4 && last_level_[index] != 0) {
		fade_left_[index] = kFadeMixSamples;
		fade_level_[index] = last_level_[index];
	}
}

void APU::clock_length()
{
	// Only a channel with its DAC on is clocked. That is invisible in normal
	// play (a channel with the DAC off is silent anyway) but it is what makes
	// a power cycle non-destructive: the counters survive with their DACs
	// cleared, so they sit frozen until the game turns a DAC back on and
	// retriggers. Blargg dmg_sound 08 measures exactly this - after 250ms
	// powered off it still reads back the four lengths it loaded before the
	// cycle (17/34/68/51). It is consistent with dmg_sound 02 #11, "Disabled
	// channel should still clock length", where the channel is off but its DAC
	// is still on.
	// A length expiry is a DAC power-down: fade, do not step (see
	// kFadeMixSamples). The channel's own level is sampled first so the
	// fade starts from whatever it was emitting.
	if (ch1_.dac && ch1_.length_enabled && ch1_.length_timer > 0) {
		if (--ch1_.length_timer == 0) {
			disable_channel(0);
			ch1_.enabled = false;
		}
	}
	if (ch2_.dac && ch2_.length_enabled && ch2_.length_timer > 0) {
		if (--ch2_.length_timer == 0) {
			disable_channel(1);
			ch2_.enabled = false;
		}
	}
	if (ch3_.dac && ch3_.length_enabled && ch3_.length_timer > 0) {
		if (--ch3_.length_timer == 0) {
			disable_channel(2);
			ch3_.enabled = false;
		}
	}
	if (ch4_.dac && ch4_.length_enabled && ch4_.length_timer > 0) {
		if (--ch4_.length_timer == 0) {
			disable_channel(3);
			ch4_.enabled = false;
		}
	}
}

void APU::clock_envelope()
{
	auto clock_pulse = [](Pulse &ch) {
		if (ch.env_period == 0 || !ch.enabled)
			return;
		if (--ch.env_timer == 0) {
			ch.env_timer = ch.env_period;
			if (ch.env_up && ch.volume < 15)
				ch.volume++;
			if (!ch.env_up && ch.volume > 0)
				ch.volume--;
		}
	};
	clock_pulse(ch1_);
	clock_pulse(ch2_);
	if (ch4_.env_period != 0 && ch4_.enabled) {
		if (--ch4_.env_timer == 0) {
			ch4_.env_timer = ch4_.env_period;
			if (ch4_.env_up && ch4_.volume < 15)
				ch4_.volume++;
			if (!ch4_.env_up && ch4_.volume > 0)
				ch4_.volume--;
		}
	}
}

void APU::clock_sweep()
{
	Pulse &ch = ch1_;
	if (!ch.sweep_enabled || !ch.enabled)
		return;
	if (--ch.sweep_timer > 0)
		return;
	ch.sweep_timer = ch.sweep_pace == 0 ? 8 : ch.sweep_pace;
	if (ch.sweep_pace == 0)
		return;
	const u16 delta = ch.shadow_freq >> ch.sweep_slope;
	const u16 next = ch.sweep_down ? ch.shadow_freq - delta : ch.shadow_freq + delta;
	if (ch.sweep_down)
		ch.sweep_sub_done = true;
	// The overflow check applies to every calculation, even with slope 0
	// (only the write-back needs a nonzero slope). E.g. an UP sweep with
	// slope 0 doubles the shadow, disabling the channel above $3FF.
	// https://gbdev.io/pandocs/Audio_details.html#pulse-channel-with-sweep-ch1
	if (next > 2047) {
		ch.enabled = false;
		return;
	}
	if (ch.sweep_slope != 0) {
		ch.shadow_freq = next;
		ch.freq = next;
		// Overflow check runs the calculation a second time.
		const u16 delta2 = next >> ch.sweep_slope;
		const u16 check = ch.sweep_down ? next - delta2 : next + delta2;
		if (check > 2047)
			ch.enabled = false;
	}
}

void APU::trigger_pulse(Pulse &ch, bool with_sweep)
{
	ch.enabled = ch.dac;
	// Zero length reloads to maximum; when the DIV-APU next step does not
	// clock length it becomes maximum-1 instead.
	// https://gbdev.io/pandocs/Audio_details.html#obscure-behavior
	if (ch.length_timer == 0) {
		ch.length_timer = (ch.length_enabled && (frame_step_ % 2 == 0)) ? 63 : 64;
	}
	// Retriggering resets the duty timer but keeps its low 2 bits.
	ch.freq_timer = (((2048 - ch.freq) * 4) & ~3u) | (ch.freq_timer & 3);
	ch.volume = ch.env_initial;
	// Envelope timer reloads period (0 acts as 8), plus one if the next
	// DIV-APU step clocks the envelope.
	u8 env_base = ch.env_period == 0 ? 8 : ch.env_period;
	ch.env_timer = (frame_step_ == 6) ? env_base + 1 : env_base;
	if (with_sweep) {
		ch.shadow_freq = ch.freq;
		ch.sweep_timer = ch.sweep_pace == 0 ? 8 : ch.sweep_pace;
		ch.sweep_enabled = ch.sweep_pace != 0 || ch.sweep_slope != 0;
		ch.sweep_sub_done = false;
		if (ch.sweep_slope != 0) {
			// Overflow check on trigger.
			const u16 delta = ch.shadow_freq >> ch.sweep_slope;
			const u16 next = ch.sweep_down ? ch.shadow_freq - delta : ch.shadow_freq + delta;
			if (ch.sweep_down)
				ch.sweep_sub_done = true;
			if (next > 2047)
				ch.enabled = false;
		}
	}
}

void APU::trigger_wave()
{
	// DMG retrigger corruption: retriggering while the channel is playing
	// clobbers the first wave RAM bytes with the area being read. Only the
	// single-cycle-exact trigger phase is unmodeled (dmg_sound 09/10/12 need
	// T-cycle-exact coincidence, beyond M-cycle granularity).
	// https://gbdev.io/pandocs/Audio_details.html#obscure-behavior
	// GBC: no corruption (different wave RAM handling).
	if (ch3_.enabled) {
		const u8 byte = ch3_.pos / 2;
		if (byte < 4) {
			ch3_.ram[0] = ch3_.ram[byte];
		} else {
			const u8 base = byte & ~3u;
			for (u8 i = 0; i < 4; i++)
				ch3_.ram[i] = ch3_.ram[base + i];
		}
	}
	ch3_.enabled = ch3_.dac;
	if (ch3_.length_timer == 0) {
		ch3_.length_timer = (ch3_.length_enabled && (frame_step_ % 2 == 0)) ? 255 : 256;
	}
	// SameBoy reloads the sample countdown on trigger with a +3 offset
	// (Core/apu.c: `sample_countdown = (sample_length ^ 0x7FF) + 3`) and counts
	// the wave channel in units of 2 T, so its trigger period is 4 T longer than
	// a bare `(2048 - n) * 2`. Those 4 T decide whether the first DAC fetch lands
	// just before or just after the ROM's `$FF30` access - the difference between
	// `dmg_sound` 09 reading one wave byte ahead and matching the hardware.
	ch3_.freq_timer = (2048 - ch3_.freq) * 2 + 4;
	ch3_.pos = 0;
	// NOTE: the output buffer is intentionally NOT refreshed; the previously
	// latched nibble keeps playing until the first tick reads sample 1.
}

void APU::trigger_noise()
{
	ch4_.enabled = ch4_.dac;
	if (ch4_.length_timer == 0) {
		ch4_.length_timer = (ch4_.length_enabled && (frame_step_ % 2 == 0)) ? 63 : 64;
	}
	static constexpr u16 kDivisors[8] = {8, 16, 32, 48, 64, 80, 96, 112};
	ch4_.freq_timer = kDivisors[ch4_.divisor] << ch4_.clock_shift;
	ch4_.lfsr = 0;
	ch4_.volume = ch4_.env_initial;
	u8 env_base = ch4_.env_period == 0 ? 8 : ch4_.env_period;
	ch4_.env_timer = (frame_step_ == 6) ? env_base + 1 : env_base;
}

// Enabling length mid-sequence clocks it once when the DIV-APU next step
// does not clock the length timer, i.e. when the current step is even
// (length clocks on even steps 0,2,4,6).
// https://gbdev.io/pandocs/Audio_details.html#obscure-behavior
void APU::set_length_enabled(bool &flag, bool &enabled, u8 &timer, u8 value)
{
	const bool was = flag;
	flag = (value & 0x40) != 0;
	if (!was && flag && (frame_step_ % 2 == 0) && timer > 0) {
		if (--timer == 0)
			enabled = false;
	}
}

u8 APU::pulse_output(Pulse &ch)
{
	if (!ch.enabled || !ch.dac)
		return 0;
	return kDutyTable[ch.duty][ch.duty_pos] ? ch.volume : 0;
}

u8 APU::wave_level()
{
	if (!ch3_.enabled || !ch3_.dac)
		return 0;
	return ch3_.out_buffer >> kWaveShifts[ch3_.volume_code];
}

u8 APU::noise_level()
{
	if (!ch4_.enabled || !ch4_.dac)
		return 0;
	return ((~ch4_.lfsr & 1) != 0) ? ch4_.volume : 0;
}

void APU::power_off_channels()
{
	// Powering off clears channels and registers but not wave RAM. Note
	// this is NOT the post-boot state ($F3 etc.): only a cold boot has
	// that, a power cycle clears to zero (GBATEK "SOUND2CNT_X": PSG
	// registers are reset while bit 7 is clear).
	// The four length counters are the exception - they keep their values
	// across the cycle on hardware (SameBoy Core/apu.c: the lengths are
	// saved before GB_apu_init and restored after). Games that power-cycle
	// the APU between notes depend on it: the next trigger then does not
	// reload the counter and the note ends when it originally should.
	length_shadow_ = {ch1_.length_timer, ch2_.length_timer, ch3_.length_timer, ch4_.length_timer};
	const auto wave = ch3_.ram;
	ch1_ = Pulse{};
	ch2_ = Pulse{};
	ch3_ = Wave{};
	ch4_ = Noise{};
	ch3_.ram = wave;
	nr10_ = nr11_ = nr12_ = nr14_ = 0;
	nr21_ = nr22_ = nr24_ = 0;
	nr30_ = nr31_ = nr32_ = nr34_ = 0;
	nr41_ = nr42_ = nr43_ = nr44_ = 0;
	nr50_ = nr51_ = 0;
	power_ = false;
}

void APU::power_on_channels()
{
	// A power cycle is not a cold boot: the registers stay CLEARED (Blargg
	// dmg_sound 01-registers #5 fills every register with $FF, powers the APU
	// off and on again, and expects every one to read back cleared, and 11
	// "regs after power" expects NR12/NR14 to read 0). Restoring the
	// post-boot defaults here ($BF/$F3/$77/$F3) is what made both fail.
	ch1_ = Pulse{};
	ch2_ = Pulse{};
	ch3_ = Wave{};
	ch4_ = Noise{};
	// Powering on resets the frame sequencer, divider included, so the first
	// step lands one full step period after the power-on (GBATEK "APU": the
	// sequencer restarts on power-up). Neutral against the current ROM set -
	// dmg_sound 08/11 pass either way - but it is what the hardware does, and
	// 07 "len sweep period sync" #5/#6 measure exactly this phase.
	// ...but only on the DMG. The CGB leaves the sequencer running across the
	// cycle, and that is observable: a trigger on a zero length counter reloads
	// 63 or 64 depending on whether the next step clocks length (GBATEK
	// "Obscure behavior"), so the sequencer's phase decides it. cgb_sound 08
	// prints `40 00 40 40` on the CGB and only gets that with the phase kept -
	// resetting it gives 63 and a CRC of $D92C0407 instead of $3CF589B4.
	if (!is_cgb_mode(hardware_mode_)) {
		frame_step_ = 0;
		frame_clock_ = 0;
	}
	// The four length counters survive the cycle on the DMG (see
	// power_off_channels) - but *not* on the CGB, where powering up resets
	// them (Blargg cgb_sound 08's own header: "On CGB, length counters are
	// reset when powered up. On DMG, they are unaffected, and not clocked").
	// The same split is why the wave channel's length reads 0 there.
	if (is_cgb_mode(hardware_mode_)) {
		ch1_.length_timer = 0;
		ch2_.length_timer = 0;
		ch3_.length_timer = 0;
		ch4_.length_timer = 0;
	} else {
		ch1_.length_timer = length_shadow_[0];
		ch2_.length_timer = length_shadow_[1];
		ch3_.length_timer = length_shadow_[2];
		ch4_.length_timer = length_shadow_[3];
	}
	// ...and so do their enable flags, so the next trigger sees no enable edge
	// (and therefore no "enabling clocks it once" quirk). What stops them from
	// counting down is the DAC: a power cycle clears it, and clock_length only
	// clocks a channel whose DAC is on (see there). Blargg dmg_sound 08 pins
	// this down: after a 250ms power-off it reads the four counters back ~34
	// length periods later and they are still exactly the loaded lengths
	// (51/68/17/34 for its NR41=$CD/NR31=$BC/NR11=$EF/NR21=$DE setup).
	ch1_.length_enabled = length_shadow_[0] != 0;
	ch2_.length_enabled = length_shadow_[1] != 0;
	ch3_.length_enabled = length_shadow_[2] != 0;
	ch4_.length_enabled = length_shadow_[3] != 0;
	power_ = true;
}

void APU::generate_sample()
{
	const u8 o1 = pulse_output(ch1_);
	const u8 o2 = pulse_output(ch2_);
	const u8 o3 = wave_level();
	const u8 o4 = noise_level();
	// Remember what each channel last emitted, for the power-down fade.
	const u8 levels[4] = {o1, o2, o3, o4};
	for (unsigned i = 0; i < 4; i++) {
		if (levels[i] != 0)
			last_level_[i] = levels[i];
	}
	// Stereo mix per NR51 routing, master volume per NR50. Silence (no
	// channels) must come out as digital zero; the DC blocker downstream
	// keeps it there.
	const u8 vol_l = (nr50_ >> 4) & 7;
	const u8 vol_r = nr50_ & 7;
	u32 left = 0, right = 0;
	int mix[4] = {o1, o2, o3, o4};
	// Fold in the pending power-down fades (a disabled channel still rings
	// down through its output capacitor for ~0.5ms).
	for (unsigned i = 0; i < 4; i++) {
		if (fade_left_[i] == 0)
			continue;
		// Linear ramp from the channel's level down to 0 over
		// kFadeMixSamples; the channel itself is already silent, so this
		// is the output capacitor discharging.
		mix[i] +=
			static_cast<int>(static_cast<u32>(fade_level_[i]) * fade_left_[i] / kFadeMixSamples);
		if (--fade_left_[i] == 0)
			fade_level_[i] = 0;
	}
	for (int i = 0; i < 4; i++) {
		if (nr51_ & (0x10 << i))
			left += static_cast<u32>(mix[i] < 0 ? 0 : mix[i]);
		if (nr51_ & (0x01 << i))
			right += static_cast<u32>(mix[i] < 0 ? 0 : mix[i]);
	}
	i16 out_l = 0, out_r = 0;
	if (output_.push(static_cast<float>(left * (vol_l + 1)) * kMixScale,
					 static_cast<float>(right * (vol_r + 1)) * kMixScale, &out_l, &out_r)) {
		sample_buffer_.push_back(out_l);
		sample_buffer_.push_back(out_r);
		++frames_emitted_;
		// Hand the device audio about one video frame at a time. This used to
		// batch 100 ms (kSampleRate/10), which is not a throughput problem -
		// it is a latency one: the device got one big chunk ten times a
		// second, so it underran in between (audible crackle) and, because
		// video is paced off the audio queue, the picture stuttered with it.
		// The GBA path already used a frame here (src/gba/audio.cpp); this
		// brings the GB path in line with it.
		if (sample_buffer_.size() >= kSampleRate / 60 * 2)
			flush_samples();
	}
}

void APU::flush_samples()
{
	if (stream_ == nullptr || sample_buffer_.empty()) {
		if (stream_ == nullptr)
			sample_buffer_.clear();
		return;
	}
	// Drop backlog beyond ~250ms to avoid runaway latency after hitches.
	// Counted: a drop is exactly the moment a game would have crackled.
	if (SDL_GetAudioStreamQueued(stream_) > static_cast<int>(kSampleRate)) {
		SDL_ClearAudioStream(stream_);
		++dropouts_;
	}
	SDL_PutAudioStreamData(stream_, sample_buffer_.data(),
						   static_cast<int>(sample_buffer_.size() * sizeof(i16)));
	sample_buffer_.clear();
}

u8 APU::read_reg(u16 addr) const
{
	// Reads behave normally while powered off (lengths/duty/shadows stay
	// readable); only NR52 loses its power bit and channel flags go quiet.
	if (!power_ && addr >= 0xFF30)
		return read_wave_ram(addr);
	switch (addr) {
	case 0xFF10:
		return 0x80 | nr10_;
	case 0xFF11:
		return 0x3F | nr11_;
	case 0xFF12:
		return nr12_;
	case 0xFF13:
		return 0xFF;
	case 0xFF14:
		return 0xBF | nr14_;
	case 0xFF16:
		return 0x3F | nr21_;
	case 0xFF17:
		return nr22_;
	case 0xFF18:
		return 0xFF;
	case 0xFF19:
		return 0xBF | nr24_;
	case 0xFF1A:
		return 0x7F | nr30_;
	case 0xFF1B:
		return 0xFF;
	case 0xFF1C:
		return 0x9F | nr32_;
	case 0xFF1D:
		return 0xFF;
	case 0xFF1E:
		return 0xBF | nr34_;
	case 0xFF20:
		return 0xFF;
	case 0xFF21:
		return nr42_;
	case 0xFF22:
		return nr43_;
	case 0xFF23:
		return 0xBF | nr44_;
	case 0xFF24:
		return nr50_;
	case 0xFF25:
		return nr51_;
	case 0xFF26: {
		u8 v = 0x70;
		if (power_)
			v |= 0x80;
		if (ch1_.enabled)
			v |= 0x01;
		if (ch2_.enabled)
			v |= 0x02;
		if (ch3_.enabled)
			v |= 0x04;
		if (ch4_.enabled)
			v |= 0x08;
		return v;
	}
	default:
		return 0xFF;
	}
}

void APU::write_reg(u16 addr, u8 value)
{
	if (addr >= 0xFF30) {
		write_wave_ram(addr, value);
		return;
	}
	// Power gates most writes while off (Pan Docs "NR52"), with two
	// documented exceptions Blargg's dmg_sound pins down:
	//  * the four length registers keep loading their counters (dmg_sound 02
	//    "len ctr"), and
	//  * the noise channel's own registers NR41-NR44 are NOT ignored - it is
	//    the one channel that keeps running across a power cycle on DMG, so a
	//    write there still triggers it and NR52 bit 3 still reports it
	//    (dmg_sound 11 "regs after power" #4).
	// The counters live in `length_shadow_` across a power cycle, so a length
	// written while off has to land there too - otherwise the power-on restore
	// would resurrect the value from *before* the write (dmg_sound 11 writes
	// NR41 while powered off and expects it to survive the cycle).
	if (!power_ && addr != 0xFF26) {
		switch (addr) {
		case 0xFF11:
			ch1_.length_timer = length_shadow_[0] = 64 - (value & 0x3F);
			return;
		case 0xFF16:
			ch2_.length_timer = length_shadow_[1] = 64 - (value & 0x3F);
			return;
		case 0xFF1B:
			ch3_.length_timer = length_shadow_[2] = 256 - value;
			return;
		case 0xFF20:
			ch4_.length_timer = length_shadow_[3] = 64 - (value & 0x3F);
			return;
		// NR42-NR44 drive the channel but leave the register shadows alone:
		// 11 "regs after power" #4 needs the trigger to work, while
		// 01-registers #6 needs the registers to still read cleared after the
		// same writes.
		case 0xFF21:
			ch4_.dac = (value & 0xF8) != 0;
			ch4_.env_initial = value >> 4;
			ch4_.env_up = (value & 0x08) != 0;
			ch4_.env_period = value & 7;
			if (!ch4_.dac)
				ch4_.enabled = false;
			return;
		case 0xFF22:
			ch4_.clock_shift = value >> 4;
			ch4_.width_7bit = (value & 0x08) != 0;
			ch4_.divisor = value & 7;
			return;
		case 0xFF23:
			// No frame-sequencer quirk here: the sequencer's coupling to the
			// enable edge is what dmg_sound 08 measures, and while the APU is
			// off nothing may clock (its counters come back untouched - the
			// test reads 51 for a NR41=$CD load).
			ch4_.length_enabled = (value & 0x40) != 0;
			if (value & 0x80)
				trigger_noise();
			return;
		default:
			return;
		}
	}
	switch (addr) {
	case 0xFF10:
		nr10_ = value & 0x7F;
		// Clearing negate mode after a subtraction calculation ran
		// since the last trigger disables the channel immediately.
		// https://gbdev.io/pandocs/Audio_details.html#obscure-behavior
		if (ch1_.sweep_down && (value & 0x08) == 0 && ch1_.sweep_sub_done) {
			ch1_.enabled = false;
		}
		ch1_.sweep_pace = (value >> 4) & 7;
		ch1_.sweep_down = (value & 0x08) != 0;
		ch1_.sweep_slope = value & 7;
		break;
	case 0xFF11:
		nr11_ = value;
		ch1_.duty = value >> 6;
		ch1_.length_timer = 64 - (value & 0x3F);
		break;
	case 0xFF12:
		nr12_ = value;
		ch1_.dac = (value & 0xF8) != 0;
		ch1_.env_initial = value >> 4;
		ch1_.env_up = (value & 0x08) != 0;
		ch1_.env_period = value & 7;
		if (!ch1_.dac)
			ch1_.enabled = false;
		break;
	case 0xFF13:
		ch1_.freq = (ch1_.freq & 0x700) | value;
		break;
	case 0xFF14: {
		nr14_ = value & 0x40;
		ch1_.freq = (ch1_.freq & 0xFF) | ((value & 7) << 8);
		set_length_enabled(ch1_.length_enabled, ch1_.enabled, ch1_.length_timer, value);
		if (value & 0x80)
			trigger_pulse(ch1_, true);
		break;
	}
	case 0xFF16:
		nr21_ = value;
		ch2_.duty = value >> 6;
		ch2_.length_timer = 64 - (value & 0x3F);
		break;
	case 0xFF17:
		nr22_ = value;
		ch2_.dac = (value & 0xF8) != 0;
		ch2_.env_initial = value >> 4;
		ch2_.env_up = (value & 0x08) != 0;
		ch2_.env_period = value & 7;
		if (!ch2_.dac)
			ch2_.enabled = false;
		break;
	case 0xFF18:
		ch2_.freq = (ch2_.freq & 0x700) | value;
		break;
	case 0xFF19: {
		nr24_ = value & 0x40;
		ch2_.freq = (ch2_.freq & 0xFF) | ((value & 7) << 8);
		set_length_enabled(ch2_.length_enabled, ch2_.enabled, ch2_.length_timer, value);
		if (value & 0x80)
			trigger_pulse(ch2_, false);
		break;
	}
	case 0xFF1A:
		nr30_ = value & 0x80;
		ch3_.dac = (value & 0x80) != 0;
		if (!ch3_.dac)
			ch3_.enabled = false;
		break;
	case 0xFF1B:
		nr31_ = value;
		ch3_.length_timer = 256 - value;
		break;
	case 0xFF1C:
		nr32_ = value & 0x60;
		ch3_.volume_code = (value >> 5) & 3;
		break;
	case 0xFF1D:
		ch3_.freq = (ch3_.freq & 0x700) | value;
		break;
	case 0xFF1E: {
		nr34_ = value & 0x40;
		ch3_.freq = (ch3_.freq & 0xFF) | ((value & 7) << 8);
		bool len_was = ch3_.length_enabled;
		ch3_.length_enabled = (value & 0x40) != 0;
		if (!len_was && ch3_.length_enabled && (frame_step_ % 2 == 0) && ch3_.length_timer > 0) {
			if (--ch3_.length_timer == 0)
				ch3_.enabled = false;
		}
		if (value & 0x80)
			trigger_wave();
		break;
	}
	case 0xFF20:
		nr41_ = value;
		ch4_.length_timer = 64 - (value & 0x3F);
		break;
	case 0xFF21:
		nr42_ = value;
		ch4_.dac = (value & 0xF8) != 0;
		ch4_.env_initial = value >> 4;
		ch4_.env_up = (value & 0x08) != 0;
		ch4_.env_period = value & 7;
		if (!ch4_.dac)
			ch4_.enabled = false;
		break;
	case 0xFF22:
		nr43_ = value;
		ch4_.clock_shift = value >> 4;
		ch4_.width_7bit = (value & 0x08) != 0;
		ch4_.divisor = value & 7;
		break;
	case 0xFF23: {
		nr44_ = value & 0x40;
		set_length_enabled(ch4_.length_enabled, ch4_.enabled, ch4_.length_timer, value);
		if (value & 0x80)
			trigger_noise();
		break;
	}
	case 0xFF24:
		nr50_ = value;
		break;
	case 0xFF25:
		nr51_ = value;
		break;
	case 0xFF26: {
		const bool was = power_;
		power_ = (value & 0x80) != 0;
		if (!power_)
			power_off_channels();
		else if (!was)
			power_on_channels();
		break;
	}
	default:
		break;
	}
}

void APU::write_wave_ram(u16 addr, u8 value)
{
	if (addr < 0xFF30 || addr > 0xFF3F)
		return;
	const u8 index = static_cast<u8>(addr - 0xFF30);
	// DMG bus conflict: while the channel is playing, the wave RAM shares
	// the bus the DAC fetches on. A CPU access inside the fetch window lands
	// on the byte currently being played instead of the addressed one;
	// outside the window the RAM is unreachable and the access is dropped
	// (mGBA src/gb/io.c redirects to wavedata8[window >> 1] and returns $FF
	// when the RAM is not "readable"; SameBoy Core/apu.c does the same via
	// wave_form_just_read). Stopping the channel (or the DAC) first takes
	// the conflict-free path every real game uses.
	// https://gbdev.io/pandocs/Audio_Registers.html#ff30ff3f--wave-pattern-ram
	// CGB: no conflict at all - the addressed byte is written whatever the DAC
	// is doing (GBATEK "Wave Pattern RAM": the conflict is a DMG artefact).
	if (!ch3_.enabled || is_cgb_mode(hardware_mode_))
		ch3_.ram[index] = value;
	else if (wave_bus_busy())
		ch3_.ram[ch3_.pos / 2] = value; // Conflicts with the fetch.
}

u8 APU::read_wave_ram(u16 addr) const
{
	if (addr < 0xFF30 || addr > 0xFF3F)
		return 0xFF;
	const u8 index = static_cast<u8>(addr - 0xFF30);
	if (!ch3_.enabled)
		return ch3_.ram[index];
	// GBATEK "Wave Pattern RAM": on the DMG, while the channel is on the RAM
	// shares the bus the DAC fetches on - on the fetch cycle the access lands
	// on the byte being played, on every other cycle the RAM is unreachable
	// (open bus on read, dropped on write; see write_wave_ram). On the CGB the
	// conflict does not exist and the address is simply read.
	if (wave_bus_busy())
		return ch3_.ram[ch3_.pos / 2];
	if (is_cgb_mode(hardware_mode_))
		return ch3_.ram[index];
	return 0xFF;
}

bool APU::wave_bus_busy() const
{
	// The conflict is a DMG bus artefact: on the CGB the wave RAM is CPU-
	// readable at any time while the channel plays.
	if (is_cgb_mode(hardware_mode_))
		return false;
	// On the DMG the bus belongs to the DAC on the cycle it fetched, and only
	// then: the access lands on the byte being played. Any other cycle the RAM
	// is unreachable (open bus on read, dropped on write).
	return ch3_.enabled && wave_just_read_;
}

namespace dsp
{

void OutputStage::save(StateBuffer &buf) const
{
	buf.writeArray(ring_l_);
	buf.writeArray(ring_r_);
	buf.write(pos_);
	buf.write(ratio_acc_);
	buf.write(ratio_fixed_);
	buf.write(ratio_);
	buf.write(dc_l_);
	buf.write(dc_prev_l_);
	buf.write(dc_r_);
	buf.write(dc_prev_r_);
	buf.write(volume_);
}

void OutputStage::load(StateBuffer &buf)
{
	buf.readArray(ring_l_);
	buf.readArray(ring_r_);
	buf.read(pos_);
	buf.read(ratio_acc_);
	buf.read(ratio_fixed_);
	buf.read(ratio_);
	buf.read(dc_l_);
	buf.read(dc_prev_l_);
	buf.read(dc_r_);
	buf.read(dc_prev_r_);
	buf.read(volume_);
}

} // namespace dsp

void APU::save(StateBuffer &buf) const
{
	// A shared writer for the two pulse channels: they are the same struct
	// and every field of it is audible state (duty position and the
	// frequency/envelope/sweep timers decide the next sample).
	const auto pulse = [&buf](const Pulse &c) {
		buf.write(c.enabled);
		buf.write(c.dac);
		buf.write(c.duty);
		buf.write(c.duty_pos);
		buf.write(c.freq);
		buf.write(c.freq_timer);
		buf.write(c.length_timer);
		buf.write(c.length_enabled);
		buf.write(c.volume);
		buf.write(c.env_initial);
		buf.write(c.env_up);
		buf.write(c.env_period);
		buf.write(c.env_timer);
		buf.write(c.sweep_pace);
		buf.write(c.sweep_down);
		buf.write(c.sweep_slope);
		buf.write(c.sweep_timer);
		buf.write(c.sweep_enabled);
		buf.write(c.sweep_sub_done);
		buf.write(c.shadow_freq);
	};
	pulse(ch1_);
	pulse(ch2_);
	buf.write(ch3_.enabled);
	buf.write(ch3_.dac);
	buf.write(ch3_.length_timer);
	buf.write(ch3_.length_enabled);
	buf.write(ch3_.volume_code);
	buf.write(ch3_.freq);
	buf.write(ch3_.freq_timer);
	buf.write(ch3_.pos);
	buf.write(ch3_.out_buffer);
	buf.writeArray(ch3_.ram);
	buf.write(ch4_.enabled);
	buf.write(ch4_.dac);
	buf.write(ch4_.length_timer);
	buf.write(ch4_.length_enabled);
	buf.write(ch4_.volume);
	buf.write(ch4_.env_initial);
	buf.write(ch4_.env_up);
	buf.write(ch4_.env_period);
	buf.write(ch4_.env_timer);
	buf.write(ch4_.lfsr);
	buf.write(ch4_.freq_timer);
	buf.write(ch4_.clock_shift);
	buf.write(ch4_.width_7bit);
	buf.write(ch4_.divisor);
	buf.writeArray(fade_left_);
	buf.writeArray(fade_level_);
	buf.writeArray(last_level_);
	buf.write(nr50_);
	buf.write(nr51_);
	buf.write(power_);
	buf.write(frame_step_);
	buf.write(frame_clock_);
	buf.write(static_cast<u8>(wave_just_read_ ? 1 : 0));
	buf.write(mix_phase_);
	buf.writeArray(length_shadow_);
	buf.write(nr10_);
	buf.write(nr11_);
	buf.write(nr12_);
	buf.write(nr14_);
	buf.write(nr21_);
	buf.write(nr22_);
	buf.write(nr24_);
	buf.write(nr30_);
	buf.write(nr31_);
	buf.write(nr32_);
	buf.write(nr34_);
	buf.write(nr41_);
	buf.write(nr42_);
	buf.write(nr43_);
	buf.write(nr44_);
	buf.writeVector(sample_buffer_);
	buf.write(frames_emitted_);
	buf.write(dropouts_);
	output_.save(buf);
	// stream_ is the live SDL handle and is never restored from a file.
}

void APU::load(StateBuffer &buf)
{
	const auto pulse = [&buf](Pulse &c) {
		buf.read(c.enabled);
		buf.read(c.dac);
		buf.read(c.duty);
		buf.read(c.duty_pos);
		buf.read(c.freq);
		buf.read(c.freq_timer);
		buf.read(c.length_timer);
		buf.read(c.length_enabled);
		buf.read(c.volume);
		buf.read(c.env_initial);
		buf.read(c.env_up);
		buf.read(c.env_period);
		buf.read(c.env_timer);
		buf.read(c.sweep_pace);
		buf.read(c.sweep_down);
		buf.read(c.sweep_slope);
		buf.read(c.sweep_timer);
		buf.read(c.sweep_enabled);
		buf.read(c.sweep_sub_done);
		buf.read(c.shadow_freq);
	};
	pulse(ch1_);
	pulse(ch2_);
	buf.read(ch3_.enabled);
	buf.read(ch3_.dac);
	buf.read(ch3_.length_timer);
	buf.read(ch3_.length_enabled);
	buf.read(ch3_.volume_code);
	buf.read(ch3_.freq);
	buf.read(ch3_.freq_timer);
	buf.read(ch3_.pos);
	buf.read(ch3_.out_buffer);
	buf.readArray(ch3_.ram);
	buf.read(ch4_.enabled);
	buf.read(ch4_.dac);
	buf.read(ch4_.length_timer);
	buf.read(ch4_.length_enabled);
	buf.read(ch4_.volume);
	buf.read(ch4_.env_initial);
	buf.read(ch4_.env_up);
	buf.read(ch4_.env_period);
	buf.read(ch4_.env_timer);
	buf.read(ch4_.lfsr);
	buf.read(ch4_.freq_timer);
	buf.read(ch4_.clock_shift);
	buf.read(ch4_.width_7bit);
	buf.read(ch4_.divisor);
	buf.readArray(fade_left_);
	buf.readArray(fade_level_);
	buf.readArray(last_level_);
	buf.read(nr50_);
	buf.read(nr51_);
	buf.read(power_);
	buf.read(frame_step_);
	buf.read(frame_clock_);
	u8 wave_flag = 0;
	buf.read(wave_flag);
	wave_just_read_ = wave_flag != 0;
	buf.read(mix_phase_);
	buf.readArray(length_shadow_);
	buf.read(nr10_);
	buf.read(nr11_);
	buf.read(nr12_);
	buf.read(nr14_);
	buf.read(nr21_);
	buf.read(nr22_);
	buf.read(nr24_);
	buf.read(nr30_);
	buf.read(nr31_);
	buf.read(nr32_);
	buf.read(nr34_);
	buf.read(nr41_);
	buf.read(nr42_);
	buf.read(nr43_);
	buf.read(nr44_);
	buf.readVector(sample_buffer_);
	buf.read(frames_emitted_);
	buf.read(dropouts_);
	output_.load(buf);
}

} // namespace gb
