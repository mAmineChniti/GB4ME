// GBA audio: PSG (nominal rates) + timer-driven Direct Sound FIFOs.
// Register units follow GBATEK; DSP structure mirrors the GB APU only where
// the GBA circuit is identical (duty/length/envelope/sweep/LFSR formulas),
// without GB DIV-phase silicon quirks.

#include "gba/audio.h"
#include "gba/dma.h"

#include <SDL3/SDL_hints.h>
#include <cstdio>

namespace gba
{

namespace
{

// Ticks per rate step at 16.78MHz.
constexpr u32 kLengthTicks = 65536;    // 256 Hz.
constexpr u32 kEnvelopeTicks = 262144; // 64 Hz.
constexpr u32 kSweepTicks = 131072;    // 128 Hz.
// Exact 44100 Hz from the 16.777216 MHz GBA clock: the ideal sample period
// is 16777216/44100 = 380.435718 ticks, so dither 380 + 0.435718 in
// fixed point. (The previous 380 + 35/100 landed on 44109.9 Hz.)
constexpr u32 kSampleTicks = 380;
constexpr u32 kSampleFracNum = 435718; // 0.435718 ticks, scaled by 1e6.
constexpr u32 kSampleFracDen = 1000000;

constexpr u8 kDutyTable[4][8] = {
	{0, 0, 0, 0, 0, 0, 0, 1},
	{1, 0, 0, 0, 0, 0, 0, 1},
	{1, 0, 0, 0, 0, 1, 1, 1},
	{0, 1, 1, 1, 1, 1, 1, 0},
};

i16 clampToS16(float v)
{
	if (v > 32767.0f)
		return 32767;
	if (v < -32767.0f)
		return -32767;
	return static_cast<i16>(v);
}

} // namespace

GbaAudio::GbaAudio()
{
	reset();
}

GbaAudio::~GbaAudio()
{
	close_audio();
}

void GbaAudio::reset()
{
	ch1_ = Pulse{};
	ch2_ = Pulse{};
	ch3_ = Wave{};
	ch4_ = Noise{};
	fifo_a_ = Fifo{};
	fifo_b_ = Fifo{};
	// Cold-boot state mirrors the GB APU post-boot values (same PSG block).
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
	soundcnt_h_ = 0;
	soundbias_ = 0x200;
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
	length_clock_ = envelope_clock_ = sweep_clock_ = sample_clock_ = 0;
	sample_frac_ = 0;
	hp_x_l_ = hp_y_l_ = hp_x_r_ = hp_y_r_ = 0;
	lp_l_ = lp_r_ = 0;
	sample_buffer_.clear();
	sample_buffer_.reserve(kSampleRate / 30 * 2);
}

bool GbaAudio::open_audio()
{
	if (stream_ != nullptr)
		return true;
	SDL_AudioSpec want{};
	want.format = SDL_AUDIO_S16;
	want.channels = 2;
	want.freq = kSampleRate;
	// Bounded device buffer (~46ms with per-frame pushes = ~63ms total
	// latency): large enough to ride out host scheduling hitches without
	// starving (starvation reads as harsh periodic ticks), small enough
	// that sound effects don't feel delayed. The SDL default can be
	// several hundred ms (reads as delayed audio).
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "2048");
	stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, nullptr, nullptr);
	if (stream_ == nullptr) {
#ifndef GB4ME_RELEASE
		std::printf("gba audio: no device (%s), running silent\n", SDL_GetError());
#endif
		return false;
	}
	SDL_ResumeAudioStreamDevice(stream_);
	return true;
}

void GbaAudio::close_audio()
{
	flushSamples();
	if (stream_ != nullptr) {
		SDL_DestroyAudioStream(stream_);
		stream_ = nullptr;
	}
}

void GbaAudio::take_samples(std::vector<i16> &out)
{
	out.insert(out.end(), sample_buffer_.begin(), sample_buffer_.end());
	sample_buffer_.clear();
}

void GbaAudio::step(u32 ticks)
{
	for (u32 i = 0; i < ticks; i++) {
		if (++length_clock_ >= kLengthTicks) {
			length_clock_ = 0;
			clockLength();
		}
		if (++envelope_clock_ >= kEnvelopeTicks) {
			envelope_clock_ = 0;
			clockEnvelope();
		}
		if (++sweep_clock_ >= kSweepTicks) {
			sweep_clock_ = 0;
			clockSweep();
		}
		// GBA PSG runs at timingFactor 4 (mGBA src/gb/audio.c:65 sets 4 for
		// GB_AUDIO_GBA), so each duty step takes 16*(2048-freq) CPU cycles
		// for pulse and 8*(2048-freq) for wave. That reproduces GBATEK's
		// f = 131072/(2048-x) (pulse) and 65536/(2048-x) (wave); the old
		// *4 / *2 ran every channel an octave too high.
		if (ch1_.enabled && --ch1_.freq_timer == 0) {
			ch1_.freq_timer = (2048 - ch1_.freq) * 16;
			ch1_.duty_pos = (ch1_.duty_pos + 1) & 7;
		}
		if (ch2_.enabled && --ch2_.freq_timer == 0) {
			ch2_.freq_timer = (2048 - ch2_.freq) * 16;
			ch2_.duty_pos = (ch2_.duty_pos + 1) & 7;
		}
		if (ch3_.enabled && --ch3_.freq_timer == 0) {
			ch3_.freq_timer = (2048 - ch3_.freq) * 8;
			ch3_.pos = (ch3_.pos + 1) & (ch3_.two_banks ? 63 : 31);
		}
		// Noise: mGBA computes (ratio ? 2*ratio : 1) << frequency * 8 * 4,
		// i.e. {32,64,128,192,256,320,384,448} << shift. The clock-shift
		// field is a full 4 bits, so shifts 14-15 still produce the correct
		// very-low-frequency noise instead of holding a constant.
		if (ch4_.enabled && --ch4_.freq_timer == 0) {
			static constexpr u32 kDivisors[8] = {32, 64, 128, 192, 256, 320, 384, 448};
			ch4_.freq_timer = kDivisors[ch4_.divisor] << ch4_.clock_shift;
			const u16 xor_bit = ((ch4_.lfsr & 1) == ((ch4_.lfsr >> 1) & 1)) ? 1 : 0;
			ch4_.lfsr = (ch4_.lfsr >> 1) | (xor_bit << 14);
			if (ch4_.width_7bit)
				ch4_.lfsr = (ch4_.lfsr & ~0x40) | (xor_bit << 6);
		}
		u32 period = kSampleTicks;
		sample_frac_ += kSampleFracNum;
		if (sample_frac_ >= kSampleFracDen) {
			sample_frac_ -= kSampleFracDen;
			period++;
		}
		if (++sample_clock_ >= period) {
			sample_clock_ = 0;
			generateSample();
		}
	}
}

void GbaAudio::onTimerOverflow(unsigned timer)
{
	// A FIFO only advances when the master enable is on, it is routed to at
	// least one side, AND the overflow is from the timer SOUNDCNT_H selected
	// for it. mGBA src/gba/timer.c:25-32 gates the hook on enable+routing.
	// Note the timer match must be computed from the SELECT bit directly -
	// folding it into `a_uses_timer0` made the `timer == 1` branch fire when
	// the FIFO was disabled, which is exactly backwards.
	if (!power_)
		return;
	const bool a_routed = (soundcnt_h_ & 0x0300u) != 0; // bits 8/9.
	const bool b_routed = (soundcnt_h_ & 0x3000u) != 0; // bits 12/13.
	const unsigned a_timer = ((soundcnt_h_ >> 10) & 1) == 0 ? 0u : 1u;
	const unsigned b_timer = ((soundcnt_h_ >> 14) & 1) == 0 ? 0u : 1u;
	if (a_routed && timer == a_timer)
		consumeFifo(fifo_a_, 1);
	if (b_routed && timer == b_timer)
		consumeFifo(fifo_b_, 2);
}

void GbaAudio::consumeFifo(Fifo &fifo, unsigned dma_ch)
{
	if (fifo.count > 0) {
		fifo.dac = static_cast<i8>(fifo.data[fifo.read_pos]);
		fifo.read_pos = (fifo.read_pos + 1) & 31;
		fifo.count--;
	}
	// GBATEK playback procedure: refill when fewer than 16 bytes remain
	// (mGBA requests when free space exceeds 4 words, i.e. used < 16).
	if (fifo.count < 16 && dma_ != nullptr)
		dma_->triggerFifo(dma_ch);
}

void GbaAudio::clockLength()
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

void GbaAudio::clockEnvelope()
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

void GbaAudio::clockSweep()
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
	if (next > 2047) {
		ch.enabled = false;
		return;
	}
	if (ch.sweep_slope != 0) {
		ch.shadow_freq = next;
		ch.freq = next;
		const u16 delta2 = next >> ch.sweep_slope;
		const u16 check = ch.sweep_down ? next - delta2 : next + delta2;
		if (check > 2047)
			ch.enabled = false;
	}
}

void GbaAudio::triggerPulse(Pulse &ch, bool with_sweep)
{
	ch.enabled = ch.dac;
	if (ch.length_timer == 0)
		ch.length_timer = 64;
	ch.freq_timer = (2048 - ch.freq) * 16;
	ch.volume = ch.env_initial;
	ch.env_timer = ch.env_period == 0 ? 8 : ch.env_period;
	if (with_sweep) {
		ch.shadow_freq = ch.freq;
		ch.sweep_timer = ch.sweep_pace == 0 ? 8 : ch.sweep_pace;
		ch.sweep_enabled = ch.sweep_pace != 0 || ch.sweep_slope != 0;
		if (ch.sweep_slope != 0) {
			const u16 delta = ch.shadow_freq >> ch.sweep_slope;
			const u16 next = ch.sweep_down ? ch.shadow_freq - delta : ch.shadow_freq + delta;
			if (next > 2047)
				ch.enabled = false;
		}
	}
}

void GbaAudio::triggerWave()
{
	ch3_.enabled = ch3_.dac;
	if (ch3_.length_timer == 0)
		ch3_.length_timer = 256;
	ch3_.freq_timer = (2048 - ch3_.freq) * 8;
	ch3_.pos = 0;
}

void GbaAudio::triggerNoise()
{
	ch4_.enabled = ch4_.dac;
	if (ch4_.length_timer == 0)
		ch4_.length_timer = 64;
	static constexpr u32 kDivisors[8] = {32, 64, 128, 192, 256, 320, 384, 448};
	ch4_.freq_timer = kDivisors[ch4_.divisor] << ch4_.clock_shift;
	ch4_.lfsr = 0;
	ch4_.volume = ch4_.env_initial;
	ch4_.env_timer = ch4_.env_period == 0 ? 8 : ch4_.env_period;
}

u8 GbaAudio::pulseOut(Pulse &ch)
{
	if (!ch.enabled || !ch.dac)
		return 0;
	return kDutyTable[ch.duty][ch.duty_pos] ? ch.volume : 0;
}

u8 GbaAudio::waveOut()
{
	if (!ch3_.enabled || !ch3_.dac)
		return 0;
	// Playing bank selected by NR30 bit 6; position spans both banks in
	// 64-digit mode. Nibble order matches GB (high nibble first).
	u8 bank = ch3_.bank;
	u8 pos = ch3_.pos;
	if (ch3_.two_banks && pos >= 32) {
		bank ^= 1;
		pos -= 32;
	}
	const u8 byte = ch3_.ram[bank * 16 + pos / 2];
	const u8 digit = (pos % 2 == 0) ? (byte >> 4) : (byte & 0x0F);
	if (ch3_.force_volume)
		return (digit * 3) >> 2; // 75%.
	static constexpr u8 kShifts[4] = {4, 0, 1, 2};
	return digit >> kShifts[ch3_.volume_code];
}

void GbaAudio::generateSample()
{
	// While the master enable (NR52 bit 7) is clear, no PSG or Direct Sound
	// output reaches the mixer (GBATEK 4000084h; mGBA skips PSG processing
	// when disabled). Without this, registers written during power-down
	// would still produce sound once sampled.
	if (!power_) {
		fifo_a_.dac = 0;
		fifo_b_.dac = 0;
		lp_l_ = 0.0f;
		lp_r_ = 0.0f;
		emitStereo(0, 0);
		return;
	}
	const u8 o1 = pulseOut(ch1_);
	const u8 o2 = pulseOut(ch2_);
	const u8 o3 = waveOut();
	u8 o4 = 0;
	if (ch4_.enabled && ch4_.dac) {
		o4 = ((~ch4_.lfsr & 1) != 0) ? ch4_.volume : 0;
	}
	const u8 vol_l = (nr50_ >> 4) & 7;
	const u8 vol_r = nr50_ & 7;
	u32 psg_l = 0, psg_r = 0;
	const u8 outs[4] = {o1, o2, o3, o4};
	for (int i = 0; i < 4; i++) {
		if (nr51_ & (0x10 << i))
			psg_l += outs[i];
		if (nr51_ & (0x01 << i))
			psg_r += outs[i];
	}
	// PSG master scale from SOUNDCNT_H bits 0-1 (25/50/100%; 3 = 100%).
	const float psg_scale = ((soundcnt_h_ & 3u) == 0)   ? 0.25f
							: ((soundcnt_h_ & 3u) == 1) ? 0.5f
														: 1.0f;
	float left = static_cast<float>(psg_l * (vol_l + 1)) * psg_scale;
	float right = static_cast<float>(psg_r * (vol_r + 1)) * psg_scale;
	// Direct Sound A/B: signed DAC bytes, 50/100% volume, L/R routing.
	// GBATEK + mGBA GBARegisterSOUNDCNT_HI: bit 8 = A Right, bit 9 = A Left,
	// bit 12 = B Right, bit 13 = B Left (the R/L pair is NOT L-then-R).
	const float va = ((soundcnt_h_ >> 2) & 1) != 0 ? 1.0f : 0.5f;
	const float vb = ((soundcnt_h_ >> 3) & 1) != 0 ? 1.0f : 0.5f;
	if ((soundcnt_h_ & 0x0100u) != 0)
		right += fifo_a_.dac * va;
	if ((soundcnt_h_ & 0x0200u) != 0)
		left += fifo_a_.dac * va;
	if ((soundcnt_h_ & 0x1000u) != 0)
		right += fifo_b_.dac * vb;
	if ((soundcnt_h_ & 0x2000u) != 0)
		left += fifo_b_.dac * vb;
	// Scale to s16 + DC-blocking high-pass (same generic DSP as GB APU).
	const float raw_l = left * 68.0f;
	const float raw_r = right * 68.0f;
	const float out_l_raw = raw_l - hp_x_l_ + 0.996f * hp_y_l_;
	const float out_r_raw = raw_r - hp_x_r_ + 0.996f * hp_y_r_;
	hp_x_l_ = raw_l;
	hp_y_l_ = out_l_raw;
	hp_x_r_ = raw_r;
	hp_y_r_ = out_r_raw;
	const float out_l = out_l_raw * master_volume_;
	const float out_r = out_r_raw * master_volume_;
	// 2-tap average with the previous output sample (-3dB near 11kHz,
	// -6dB at Nyquist): takes the harsh digital edge off DAC steps
	// without dulling the mix.
	const float soft_l = (out_l + lp_l_) * 0.5f;
	const float soft_r = (out_r + lp_r_) * 0.5f;
	lp_l_ = out_l;
	lp_r_ = out_r;
	emitStereo(clampToS16(soft_l), clampToS16(soft_r));
}

void GbaAudio::emitStereo(i16 left, i16 right)
{
	sample_buffer_.push_back(left);
	sample_buffer_.push_back(right);
	// Push to SDL about once per frame (~16.7ms): larger batches add
	// constant output latency that reads as delayed sound effects.
	if (sample_buffer_.size() >= kSampleRate / 60 * 2)
		flushSamples();
}

void GbaAudio::flushSamples()
{
	if (stream_ == nullptr || sample_buffer_.empty()) {
		if (stream_ == nullptr)
			sample_buffer_.clear();
		return;
	}
	if (SDL_GetAudioStreamQueued(stream_) > static_cast<int>(kSampleRate)) {
		SDL_ClearAudioStream(stream_);
	}
	SDL_PutAudioStreamData(stream_, sample_buffer_.data(),
						   static_cast<int>(sample_buffer_.size() * sizeof(i16)));
	sample_buffer_.clear();
}

u8 GbaAudio::readWave(u32 addr) const
{
	// Reads/writes address the NON-playing bank (GBATEK).
	const u8 bank = ch3_.bank ^ 1;
	return ch3_.ram[bank * 16 + (addr & 0x0Fu)];
}

void GbaAudio::writeWave(u32 addr, u8 value)
{
	const u8 bank = ch3_.bank ^ 1;
	ch3_.ram[bank * 16 + (addr & 0x0Fu)] = value;
}

u8 GbaAudio::read8(u32 addr) const
{
	// Read masks follow the mGBA suite's io-read table (hardware-measured):
	// write-only bits read back as 0 on GBA (not 1 as on DMG), gaps read 0.
	switch (addr) {
	case 0x04000060u:
		return nr10_ & 0x7Fu;
	case 0x04000061u:
		return 0x00; // NR10 high byte unused.
	case 0x04000062u:
		return nr11_ & 0xC0u;
	case 0x04000063u:
		return nr12_;
	case 0x04000064u:
		return 0x00;
	case 0x04000065u:
		return nr14_ & 0x40u;
	case 0x04000066u:
	case 0x04000067u:
		return 0x00; // Gap.
	case 0x04000068u:
		return nr21_ & 0xC0u;
	case 0x04000069u:
		return nr22_;
	case 0x0400006Au:
	case 0x0400006Bu:
		return 0x00; // Gap.
	case 0x0400006Cu:
		return 0x00;
	case 0x0400006Du:
		return nr24_ & 0x40u;
	case 0x0400006Eu:
	case 0x0400006Fu:
		return 0x00; // Gap.
	case 0x04000070u:
		return nr30_ & 0xE0u;
	case 0x04000071u:
		return 0x00; // Gap.
	case 0x04000072u:
		return 0x00;
	case 0x04000073u:
		return nr32_ & 0xE0u;
	case 0x04000074u:
		return 0x00;
	case 0x04000075u:
		return nr34_ & 0x40u;
	case 0x04000076u:
	case 0x04000077u:
		return 0x00; // Gap.
	case 0x04000078u:
		return 0x00;
	case 0x04000079u:
		return nr42_;
	case 0x0400007Au:
	case 0x0400007Bu:
		return 0x00; // Gap.
	case 0x0400007Cu:
		return nr43_;
	case 0x0400007Du:
		return nr44_ & 0x40u;
	case 0x0400007Eu:
	case 0x0400007Fu:
		return 0x00; // Gap.
	case 0x04000080u:
		return nr50_ & 0x77u; // No Vin on GBA: bits 3,7 read 0.
	case 0x04000081u:
		return nr51_;
	case 0x04000082u:
		return static_cast<u8>(soundcnt_h_ & 0x0Fu);
	case 0x04000083u:
		return static_cast<u8>(((soundcnt_h_ >> 8) & 0x77u));
	case 0x04000084u: {
		u8 v = 0x00;
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
	case 0x04000085u:
		return 0x00; // Gap.
	case 0x04000086u:
	case 0x04000087u:
		return 0x00; // Gap.
	case 0x04000088u:
		return static_cast<u8>(soundbias_ & 0xFFu);
	case 0x04000089u:
		return static_cast<u8>(soundbias_ >> 8);
	default:
		break;
	}
	if (addr >= 0x04000090u && addr < 0x040000A0u) {
		return readWave(addr - 0x04000090u);
	}
	if (addr >= 0x040000A0u && addr < 0x040000A8u)
		return 0; // FIFO W-only.
	return 0xFF;
}

u16 GbaAudio::read16(u32 addr) const
{
	addr &= ~1u;
	return static_cast<u16>(read8(addr) | (read8(addr + 1) << 8));
}

void GbaAudio::write8(u32 addr, u8 value)
{
	// Byte writes update the 16-bit shadow then apply (split byte writes
	// to trigger regs may double-trigger; games use 16/32-bit — noted).
	if (addr >= 0x04000090u && addr < 0x040000A0u) {
		writeWave(addr - 0x04000090u, value);
		return;
	}
	if (addr >= 0x040000A0u && addr < 0x040000A8u)
		return; // Use write16/32.
	const u32 base = addr & ~1u;
	u16 cur = static_cast<u16>(read8(base) | (read8(base + 1) << 8));
	if ((addr & 1u) == 0)
		cur = (cur & 0xFF00u) | value;
	else
		cur = (cur & 0x00FFu) | (static_cast<u16>(value) << 8);
	write16(base, cur);
}

void GbaAudio::write16(u32 addr, u16 value)
{
	addr &= ~1u;
	if (addr >= 0x04000090u && addr < 0x040000A0u) {
		writeWave(addr - 0x04000090u, static_cast<u8>(value & 0xFFu));
		writeWave(addr - 0x04000090u + 1, static_cast<u8>(value >> 8));
		return;
	}
	if (addr == 0x040000A0u || addr == 0x040000A2u) {
		pushFifo(fifo_a_, value);
		return;
	}
	if (addr == 0x040000A4u || addr == 0x040000A6u) {
		pushFifo(fifo_b_, value);
		return;
	}
	switch (addr) {
	case 0x04000060u:
		nr10_ = value & 0x7F;
		ch1_.sweep_pace = (value >> 4) & 7;
		ch1_.sweep_down = (value & 0x08) != 0;
		ch1_.sweep_slope = value & 7;
		break;
	case 0x04000062u:
		nr11_ = value & 0xFF;
		nr12_ = value >> 8;
		ch1_.duty = value >> 14;
		ch1_.length_timer = 64 - (value & 0x3F);
		ch1_.dac = (nr12_ & 0xF8) != 0;
		ch1_.env_initial = nr12_ >> 4;
		ch1_.env_up = (nr12_ & 0x08) != 0;
		ch1_.env_period = nr12_ & 7;
		if (!ch1_.dac)
			ch1_.enabled = false;
		break;
	case 0x04000064u: {
		ch1_.freq = (ch1_.freq & 0x700) | (value & 0x7FF);
		const u8 ctrl = (value >> 8) & 0xFF;
		nr14_ = ctrl & 0x40;
		ch1_.length_enabled = (ctrl & 0x40) != 0;
		if (value & 0x8000)
			triggerPulse(ch1_, true);
		break;
	}
	case 0x04000068u:
		nr21_ = value & 0xFF;
		nr22_ = value >> 8;
		ch2_.duty = value >> 14;
		ch2_.length_timer = 64 - (value & 0x3F);
		ch2_.dac = (nr22_ & 0xF8) != 0;
		ch2_.env_initial = nr22_ >> 4;
		ch2_.env_up = (nr22_ & 0x08) != 0;
		ch2_.env_period = nr22_ & 7;
		if (!ch2_.dac)
			ch2_.enabled = false;
		break;
	case 0x0400006Cu: {
		ch2_.freq = (ch2_.freq & 0x700) | (value & 0x7FF);
		const u8 ctrl = (value >> 8) & 0xFF;
		nr24_ = ctrl & 0x40;
		ch2_.length_enabled = (ctrl & 0x40) != 0;
		if (value & 0x8000)
			triggerPulse(ch2_, false);
		break;
	}
	case 0x04000070u:
		nr30_ = value & 0xE0;
		ch3_.dac = (value & 0x80) != 0;
		ch3_.bank = (value & 0x40) != 0 ? 1 : 0;
		ch3_.two_banks = (value & 0x20) != 0;
		if (!ch3_.dac)
			ch3_.enabled = false;
		break;
	case 0x04000072u:
		nr31_ = value & 0xFF;
		nr32_ = (value >> 8) & 0xFF;
		ch3_.length_timer = 256 - nr31_;
		ch3_.volume_code = (nr32_ >> 5) & 3;
		ch3_.force_volume = (nr32_ & 0x80) != 0;
		break;
	case 0x04000074u: {
		ch3_.freq = (ch3_.freq & 0x700) | (value & 0x7FF);
		const u8 ctrl = (value >> 8) & 0xFF;
		nr34_ = ctrl & 0x40;
		ch3_.length_enabled = (ctrl & 0x40) != 0;
		if (value & 0x8000)
			triggerWave();
		break;
	}
	case 0x04000078u:
		nr41_ = value & 0xFF;
		nr42_ = value >> 8;
		ch4_.length_timer = 64 - (value & 0x3F);
		ch4_.dac = (nr42_ & 0xF8) != 0;
		ch4_.env_initial = nr42_ >> 4;
		ch4_.env_up = (nr42_ & 0x08) != 0;
		ch4_.env_period = nr42_ & 7;
		if (!ch4_.dac)
			ch4_.enabled = false;
		break;
	case 0x0400007Cu: {
		nr43_ = value & 0xFF;
		ch4_.clock_shift = value >> 12;
		ch4_.width_7bit = (value & 0x08) != 0;
		ch4_.divisor = value & 7;
		const u8 ctrl = (value >> 8) & 0xFF;
		nr44_ = ctrl & 0x40;
		ch4_.length_enabled = (ctrl & 0x40) != 0;
		if (value & 0x8000)
			triggerNoise();
		break;
	}
	case 0x04000080u:
		nr50_ = value & 0xFF;
		nr51_ = value >> 8;
		break;
	case 0x04000082u: {
		soundcnt_h_ = value;
		// FIFO reset bits 11/15: "1=Reset". They act whenever the written
		// value has the bit set, not only on a 0->1 edge (mGBA audio.c), so
		// rewriting SOUNDCNT_H with the bit already high still clears.
		if ((value & 0x0800u) != 0) {
			fifo_a_ = Fifo{};
		}
		if ((value & 0x8000u) != 0) {
			fifo_b_ = Fifo{};
		}
		break;
	}
	case 0x04000084u: {
		const bool was = power_;
		power_ = (value & 0x80) != 0;
		if (!power_) {
			ch1_ = Pulse{};
			ch2_ = Pulse{};
			ch4_ = Noise{};
			const auto wave = ch3_.ram;
			ch3_ = Wave{};
			ch3_.ram = wave;
			nr10_ = nr11_ = nr12_ = nr14_ = 0;
			nr21_ = nr22_ = nr24_ = 0;
			nr30_ = nr31_ = nr32_ = nr34_ = 0;
			nr41_ = nr42_ = nr43_ = nr44_ = 0;
			nr50_ = nr51_ = 0;
			// SOUNDCNT_H stays readable and writable while the master
			// enable is off (GBATEK 4000082h); only the channel/MIX
			// registers are cleared. mGBA's disable path preserves it too.
			(void)was;
		}
		break;
	}
	case 0x04000088u:
		soundbias_ = value;
		break;
	default:
		break;
	}
}

void GbaAudio::pushFifo(Fifo &fifo, u16 value)
{
	// 32-byte ring; overflowing writes overwrite the oldest unread bytes
	// (matches mGBA's wraparound ring semantics).
	for (int i = 0; i < 2; i++) {
		const u8 byte = (i == 0) ? (value & 0xFF) : (value >> 8);
		if (fifo.count == 32) {
			fifo.read_pos = (fifo.read_pos + 1) & 31;
			fifo.count--;
		}
		fifo.data[fifo.write_pos] = byte;
		fifo.write_pos = (fifo.write_pos + 1) & 31;
		fifo.count++;
	}
}

void GbaAudio::save(StateBuffer &out) const
{
	out.write(ch1_);
	out.write(ch2_);
	out.write(ch3_);
	out.write(ch4_);
	out.write(fifo_a_);
	out.write(fifo_b_);
	out.write(nr50_);
	out.write(nr51_);
	out.write(power_);
	out.write(soundcnt_h_);
	out.write(soundbias_);
	out.write(length_clock_);
	out.write(envelope_clock_);
	out.write(sweep_clock_);
	out.write(sample_clock_);
	out.write(sample_frac_);
	out.write(hp_x_l_);
	out.write(hp_y_l_);
	out.write(hp_x_r_);
	out.write(hp_y_r_);
	out.write(master_volume_);
	out.write(nr10_);
	out.write(nr11_);
	out.write(nr12_);
	out.write(nr14_);
	out.write(nr21_);
	out.write(nr22_);
	out.write(nr24_);
	out.write(nr30_);
	out.write(nr31_);
	out.write(nr32_);
	out.write(nr34_);
	out.write(nr41_);
	out.write(nr42_);
	out.write(nr43_);
	out.write(nr44_);
}

void GbaAudio::load(const StateBuffer &in)
{
	in.read(ch1_);
	in.read(ch2_);
	in.read(ch3_);
	in.read(ch4_);
	in.read(fifo_a_);
	in.read(fifo_b_);
	in.read(nr50_);
	in.read(nr51_);
	in.read(power_);
	in.read(soundcnt_h_);
	in.read(soundbias_);
	in.read(length_clock_);
	in.read(envelope_clock_);
	in.read(sweep_clock_);
	in.read(sample_clock_);
	in.read(sample_frac_);
	in.read(hp_x_l_);
	in.read(hp_y_l_);
	in.read(hp_x_r_);
	in.read(hp_y_r_);
	in.read(master_volume_);
	in.read(nr10_);
	in.read(nr11_);
	in.read(nr12_);
	in.read(nr14_);
	in.read(nr21_);
	in.read(nr22_);
	in.read(nr24_);
	in.read(nr30_);
	in.read(nr31_);
	in.read(nr32_);
	in.read(nr34_);
	in.read(nr41_);
	in.read(nr42_);
	in.read(nr43_);
	in.read(nr44_);
}

} // namespace gba
