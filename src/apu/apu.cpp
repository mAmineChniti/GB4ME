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
// T-cycles per audio sample (44100Hz at DMG clock).
constexpr u32 kSampleTcycles = 95;     // 4194304/44100 = 95.11; dithered below.
constexpr u32 kSampleTcyclesFrac = 11; // 0.11 * 100 extra cycle every ~9 samples.

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
	sample_clock_ = 0;
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
		if (ch3_.enabled && --ch3_.freq_timer == 0) {
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
		// Sample clock (dithered 95/96 to average 95.11).
		static u32 frac = 0;
		u32 period = kSampleTcycles;
		frac += kSampleTcyclesFrac;
		if (frac >= 100) {
			frac -= 100;
			period++;
		}
		if (++sample_clock_ >= period) {
			sample_clock_ = 0;
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

void APU::clock_length()
{
	if (ch1_.length_enabled && ch1_.length_timer > 0) {
		if (--ch1_.length_timer == 0)
			ch1_.enabled = false;
	}
	if (ch2_.length_enabled && ch2_.length_timer > 0) {
		if (--ch2_.length_timer == 0)
			ch2_.enabled = false;
	}
	if (ch3_.length_enabled && ch3_.length_timer > 0) {
		if (--ch3_.length_timer == 0)
			ch3_.enabled = false;
	}
	if (ch4_.length_enabled && ch4_.length_timer > 0) {
		if (--ch4_.length_timer == 0)
			ch4_.enabled = false;
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
	ch3_.freq_timer = (2048 - ch3_.freq) * 2;
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

void APU::generate_sample()
{
	u8 o1 = pulse_output(ch1_);
	u8 o2 = pulse_output(ch2_);
	u8 o3 = 0;
	if (ch3_.enabled && ch3_.dac) {
		static constexpr u8 kShifts[4] = {4, 0, 1, 2};
		o3 = ch3_.out_buffer >> kShifts[ch3_.volume_code];
	}
	u8 o4 = 0;
	if (ch4_.enabled && ch4_.dac) {
		o4 = ((~ch4_.lfsr & 1) != 0) ? ch4_.volume : 0;
	}
	// Stereo mix per NR51 routing, master volume per NR50. Silence (no
	// channels) must come out as digital zero, so scale from 0 and strip
	// residual DC with the high-pass filters.
	const u8 vol_l = (nr50_ >> 4) & 7;
	const u8 vol_r = nr50_ & 7;
	u32 left = 0, right = 0;
	const u8 outs[4] = {o1, o2, o3, o4};
	for (int i = 0; i < 4; i++) {
		if (nr51_ & (0x10 << i))
			left += outs[i];
		if (nr51_ & (0x01 << i))
			right += outs[i];
	}
	// Max sum 60 * max master 8 = 480 -> *68 = full int16 range.
	const float raw_l = static_cast<float>(left * (vol_l + 1) * 68);
	const float raw_r = static_cast<float>(right * (vol_r + 1) * 68);
	const float out_l_raw = raw_l - hp_x_l_ + 0.996f * hp_y_l_;
	const float out_r_raw = raw_r - hp_x_r_ + 0.996f * hp_y_r_;
	hp_x_l_ = raw_l;
	hp_y_l_ = out_l_raw;
	hp_x_r_ = raw_r;
	hp_y_r_ = out_r_raw;
	const float out_l = out_l_raw * master_volume_;
	const float out_r = out_r_raw * master_volume_;
	const auto clamp16 = [](float v) {
		if (v > 32767.0f)
			return static_cast<i16>(32767);
		if (v < -32767.0f)
			return static_cast<i16>(-32767);
		return static_cast<i16>(v);
	};
	sample_buffer_.push_back(clamp16(out_l));
	sample_buffer_.push_back(clamp16(out_r));
	if (sample_buffer_.size() >= kSampleRate / 10 * 2)
		flush_samples();
}

void APU::flush_samples()
{
	if (stream_ == nullptr || sample_buffer_.empty()) {
		if (stream_ == nullptr)
			sample_buffer_.clear();
		return;
	}
	// Drop backlog beyond ~250ms to avoid runaway latency after hitches.
	if (SDL_GetAudioStreamQueued(stream_) > static_cast<int>(kSampleRate)) {
		SDL_ClearAudioStream(stream_);
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
	// Length timers stay writable while powered off (Pan Docs "NR52").
	if (!power_ && addr != 0xFF26) {
		if (addr == 0xFF11)
			ch1_.length_timer = 64 - (value & 0x3F);
		if (addr == 0xFF16)
			ch2_.length_timer = 64 - (value & 0x3F);
		if (addr == 0xFF1B)
			ch3_.length_timer = 256 - value;
		if (addr == 0xFF20)
			ch4_.length_timer = 64 - (value & 0x3F);
		return;
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
		if (!power_) {
			// Powering off clears channels and registers but not wave
			// RAM. Note this is NOT the post-boot state ($F3 etc.):
			// only a cold boot has that; a power cycle clears to zero.
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
		} else if (!was) {
			frame_step_ = 0;
			ch3_.out_buffer = 0;
		}
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
	// Normal access. The exact DMG access-conflict windows (same T-cycle as
	// the sample read, redirected bytes) need sub-M-cycle CPU timing that an
	// M-cycle core cannot resolve; dmg_sound 09/12 document the gap. Real
	// games always stop the channel (or DAC) before touching wave RAM, which
	// takes the conflict-free path and is unaffected.
	// https://gbdev.io/pandocs/Audio_Registers.html#ff30ff3f--wave-pattern-ram
	// GBC: different conflict behavior; revisit with GBC support.
	ch3_.ram[addr - 0xFF30] = value;
}

u8 APU::read_wave_ram(u16 addr) const
{
	if (addr < 0xFF30 || addr > 0xFF3F)
		return 0xFF;
	return ch3_.ram[addr - 0xFF30];
}

} // namespace gb
