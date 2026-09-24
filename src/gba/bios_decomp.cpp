// HLE BIOS decompression: LZ77 / Huffman / RLE / BitUnPack / DiffFilter.
// Stream formats follow GBATEK ("BIOS Decompression Functions"); the walk
// logic matches mGBA bios.c observable behavior (headers, VRAM halfword
// assembly, RL padding, BitUnPack info block), re-implemented here.
// Transfers run through the CPU bus accessors so WAITCNT timing accrues.

#include "gba/bios.h"
#include "gba/bus.h"
#include "gba/cpu.h"

namespace gba
{

namespace
{

// VRAM-destination (width 2) write path shared by LZ77/RL: assembles
// halfwords across odd destination addresses exactly like the reference.
void storeDecompByte(Arm7Tdmi &cpu, u32 dest, u8 byte, u16 &halfword)
{
	if ((dest & 1) != 0) {
		halfword |= static_cast<u16>(byte << 8);
		cpu.storeHalf(dest ^ 1, halfword);
	} else {
		halfword = byte;
		cpu.storeByte(dest, byte);
	}
}

} // namespace

HleBios::Outcome HleBios::decomp(u8 num, Arm7Tdmi &cpu, GbaBus &bus)
{
	(void) bus;
	switch (num) {
	case kLz77UnCompWram:
	case kLz77UnCompVram: {
		const int width = (num == kLz77UnCompWram) ? 1 : 2;
		u32 source = cpu.reg(0);
		u32 dest = cpu.reg(1);
		// Header word: 0x10 signature + 24-bit uncompressed length.
		u32 remaining = (cpu.loadWord(source & ~3u) & 0xFFFFFF00u) >> 8;
		source += 4;
		u8 blockheader = 0;
		int blocks_remaining = 0;
		u16 halfword = 0;
		while (remaining > 0) {
			if (blocks_remaining != 0) {
				if ((blockheader & 0x80) != 0) {
					// Compressed: 12-bit disp + 4-bit length (+3).
					const u32 block =
						(static_cast<u32>(cpu.loadByte(source)) << 8) | cpu.loadByte(source + 1);
					source += 2;
					u32 disp = dest - (block & 0x0FFFu) - 1;
					u32 bytes = (block >> 12) + 3;
					while (bytes-- != 0) {
						if (remaining == 0)
							break; // Overrun guard.
						--remaining;
						if (width == 2) {
							u16 v = static_cast<u16>(cpu.loadHalf(disp & ~1u, false) >>
													 ((disp & 1) * 8));
							if ((dest & 1) != 0) {
								halfword |= static_cast<u16>(v << 8);
								cpu.storeHalf(dest ^ 1, halfword);
							} else {
								halfword = static_cast<u16>(v & 0xFFu);
							}
						} else {
							cpu.storeByte(dest, cpu.loadByte(disp));
						}
						++disp;
						++dest;
					}
				} else {
					// Literal byte.
					const u8 byte = cpu.loadByte(source);
					++source;
					if (width == 2) {
						storeDecompByte(cpu, dest, byte, halfword);
					} else {
						cpu.storeByte(dest, byte);
					}
					++dest;
					--remaining;
				}
				blockheader <<= 1;
				--blocks_remaining;
			} else {
				blockheader = cpu.loadByte(source);
				++source;
				blocks_remaining = 8;
			}
		}
		cpu.setReg(0, source);
		cpu.setReg(1, dest);
		cpu.setReg(3, 0);
		return {Result::Return, 16};
	}
	case kHuffUnComp: {
		u32 source = cpu.reg(0) & ~3u;
		u32 dest = cpu.reg(1);
		const u32 header = cpu.loadWord(source);
		u32 remaining = header >> 8;
		unsigned bits = header & 0xFu;
		if (bits == 0)
			bits = 8; // Invalid size: reference falls back.
		if (bits == 1 || 32 % bits != 0)
			return {Result::Return, 8}; // Unaligned: no-op.
		const u32 tree_size = (static_cast<u32>(cpu.loadByte(source + 4)) << 1) + 1;
		const u32 tree_base = source + 5;
		source += 5 + tree_size;
		u32 node_ptr = tree_base;
		u8 node = cpu.loadByte(node_ptr);
		u32 block = 0;
		int bits_seen = 0;
		while (remaining > 0) {
			u32 bitstream = cpu.loadWord(source);
			source += 4;
			for (int left = 32; left > 0 && remaining > 0; --left, bitstream <<= 1) {
				const u32 next = (node_ptr & ~1u) + ((node & 0x3F) * 2) + 2;
				u8 leaf;
				if ((bitstream & 0x80000000u) != 0) { // Right.
					if ((node & 0x40) != 0) {
						leaf = cpu.loadByte(next + 1);
					} else {
						node_ptr = next + 1;
						node = cpu.loadByte(node_ptr);
						continue;
					}
				} else { // Left.
					if ((node & 0x80) != 0) {
						leaf = cpu.loadByte(next);
					} else {
						node_ptr = next;
						node = cpu.loadByte(node_ptr);
						continue;
					}
				}
				block |= (static_cast<u32>(leaf) & ((1u << bits) - 1)) << bits_seen;
				bits_seen += bits;
				node_ptr = tree_base;
				node = cpu.loadByte(node_ptr);
				if (bits_seen == 32) {
					bits_seen = 0;
					cpu.storeWord(dest, block);
					dest += 4;
					remaining -= 4;
					block = 0;
				}
			}
		}
		cpu.setReg(0, source);
		cpu.setReg(1, dest);
		return {Result::Return, 16};
	}
	case kRlUnCompWram:
	case kRlUnCompVram: {
		const int width = (num == kRlUnCompWram) ? 1 : 2;
		u32 source = cpu.reg(0);
		u32 dest = cpu.reg(1);
		u32 remaining = (cpu.loadWord(source & ~3u) & 0xFFFFFF00u) >> 8;
		u32 padding = (4 - remaining) & 0x3u;
		source += 4;
		u16 halfword = 0;
		while (remaining > 0) {
			u32 header = cpu.loadByte(source);
			++source;
			if ((header & 0x80) != 0) {
				header = (header & 0x7F) + 3;
				const u8 block = cpu.loadByte(source);
				++source;
				while (header-- != 0 && remaining != 0) {
					--remaining;
					if (width == 2)
						storeDecompByte(cpu, dest, block, halfword);
					else
						cpu.storeByte(dest, block);
					++dest;
				}
			} else {
				++header;
				while (header-- != 0 && remaining != 0) {
					--remaining;
					const u8 byte = cpu.loadByte(source);
					++source;
					if (width == 2)
						storeDecompByte(cpu, dest, byte, halfword);
					else
						cpu.storeByte(dest, byte);
					++dest;
				}
			}
		}
		if (width == 2) {
			if ((dest & 1) != 0) {
				--padding;
				++dest;
			}
			for (; padding > 0; padding -= 2, dest += 2)
				cpu.storeHalf(dest, 0);
		} else {
			while (padding-- != 0) {
				cpu.storeByte(dest, 0);
				++dest;
			}
		}
		cpu.setReg(0, source);
		cpu.setReg(1, dest);
		return {Result::Return, 16};
	}
	case kBitUnPack: {
		const u32 source0 = cpu.reg(0);
		u32 dest = cpu.reg(1);
		const u32 info = cpu.reg(2);
		const unsigned source_len = bus.read16(info);
		const unsigned source_width = bus.read8(info + 2);
		const unsigned dest_width = bus.read8(info + 3);
		const bool src_ok =
			source_width == 1 || source_width == 2 || source_width == 4 || source_width == 8;
		const bool dst_ok = dest_width == 1 || dest_width == 2 || dest_width == 4 ||
							dest_width == 8 || dest_width == 16 || dest_width == 32;
		if (!src_ok || !dst_ok)
			return {Result::Return, 8}; // No-op.
		const u32 bias = bus.read32(info + 4);
		u32 source = source0;
		u32 left = source_len;
		u8 in = 0;
		u32 out = 0;
		int bits_remaining = 0;
		int bits_eaten = 0;
		while (left > 0 || bits_remaining != 0) {
			if (bits_remaining == 0) {
				in = cpu.loadByte(source);
				bits_remaining = 8;
				++source;
				--left;
			}
			unsigned scaled = in & ((1u << source_width) - 1);
			in >>= source_width;
			if (scaled != 0 || (bias & 0x80000000u) != 0) {
				scaled += bias & 0x7FFFFFFFu;
			}
			bits_remaining -= source_width;
			out |= scaled << bits_eaten;
			bits_eaten += dest_width;
			if (bits_eaten == 32) {
				cpu.storeWord(dest, out);
				bits_eaten = 0;
				out = 0;
				dest += 4;
			}
		}
		cpu.setReg(0, source);
		cpu.setReg(1, dest);
		return {Result::Return, 16};
	}
	case kDiff8bitUnFilterWram:
	case kDiff8bitUnFilterVram:
	case kDiff16bitUnFilter: {
		const int in_width = (num == kDiff16bitUnFilter) ? 2 : 1;
		const int out_width = (num == kDiff8bitUnFilterWram) ? 1 : 2;
		u32 source = cpu.reg(0) & ~3u;
		u32 dest = cpu.reg(1);
		u32 remaining = cpu.loadWord(source) >> 8;
		u16 halfword = 0;
		u16 old = 0;
		source += 4;
		while (remaining > 0) {
			u16 cur = (in_width == 1) ? cpu.loadByte(source)
									  : static_cast<u16>(cpu.loadHalf(source, false));
			cur += old;
			if (out_width > in_width) {
				halfword >>= 8;
				halfword |= (cur << 8);
				if ((source & 1) != 0) {
					cpu.storeHalf(dest, halfword);
					dest += 2;
					remaining -= 2;
				}
			} else if (out_width == 1) {
				cpu.storeByte(dest, static_cast<u8>(cur & 0xFFu));
				dest += 1;
				remaining -= 1;
			} else {
				cpu.storeHalf(dest, cur);
				dest += 2;
				remaining -= 2;
			}
			old = cur;
			source += in_width;
		}
		cpu.setReg(0, source);
		cpu.setReg(1, dest);
		return {Result::Return, 16};
	}
	default:
		return {Result::Return, 4};
	}
}

} // namespace gba
