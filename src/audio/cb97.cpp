#include "audio/cb97.hpp"

#include "io/endian.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace sl_open::audio
{
namespace
{
constexpr char kCodebookHex[] =
	"00000000b62c7fbf12867dbf7fdf7bbfeb387abf479278bfb3eb76bf0f4575bf"
	"7c9e73bfe5456ebf75ab67bf171161bfa7765abf38dc53bfd9414dbf69a746bf"
	"fa0c40bf9b7239bf2cd832bfbcae2bbfee791ebf0f4511bf411004bfe5b6edbe"
	"274dd3be8ae3b8beee799ebe521084be274d53beee791ebe6a4dd3bde44c53bd"
	"00000000e44c533d6a4dd33dee791e3e274d533e5210843eee799e3e8ae3b83e"
	"274dd33ee5b6ed3e4110043f0f45113fee791e3fbcae2b3f2cd8323f9b72393f"
	"fa0c403f69a7463fd9414d3f38dc533fa7765a3f1711613f75ab673fe5456e3f"
	"7c9e733f0f45753fb3eb763f4792783feb387a3f7fdf7b3f12867d3fb62c7f3f"
	;

constexpr char kSymbolLookupHex[] =
	"040605090406050d0406050a04060511040605090406050e0406050a04060515"
	"040605090406050d0406050a04060512040605090406050e0406050a04060519"
	"040605090406050d0406050a04060511040605090406050e0406050a04060516"
	"040605090406050d0406050a04060512040605090406050e0406050a04060500"
	"040605090406050d0406050a04060511040605090406050e0406050a04060515"
	"040605090406050d0406050a04060512040605090406050e0406050a0406051a"
	"040605090406050d0406050a04060511040605090406050e0406050a04060516"
	"040605090406050d0406050a04060512040605090406050e0406050a04060502"
	"040b070f040c0813040b0710040c0817040b070f040c0814040b0710040c081b"
	"040b070f040c0813040b0710040c0818040b070f040c0814040b0710040c0801"
	"040b070f040c0813040b0710040c0817040b070f040c0814040b0710040c081c"
	"040b070f040c0813040b0710040c0818040b070f040c0814040b0710040c0803"
	"040b070f040c0813040b0710040c0817040b070f040c0814040b0710040c081b"
	"040b070f040c0813040b0710040c0818040b070f040c0814040b0710040c0801"
	"040b070f040c0813040b0710040c0817040b070f040c0814040b0710040c081c"
	"040b070f040c0813040b0710040c0818040b070f040c0814040b0710040c0803";

static_assert(sizeof(kCodebookHex) == 64 * 8 + 1);
static_assert(sizeof(kSymbolLookupHex) == 512 * 2 + 1);

struct SymbolRecord
{
	std::uint8_t next_state;
	std::uint8_t consumed_bits;
	float value;
};

constexpr SymbolRecord kSymbols[] = {
	{1, 8, 0.0f}, {1, 7, 0.0f}, {0, 8, 0.0f}, {0, 7, 0.0f},
	{0, 2, 0.0f}, {0, 2, -1.0f}, {0, 2, 1.0f},
	{0, 3, -1.0f}, {0, 3, 1.0f},
	{1, 4, -2.0f}, {1, 4, 2.0f}, {1, 3, -2.0f}, {1, 3, 2.0f},
	{1, 5, -3.0f}, {1, 5, 3.0f}, {1, 4, -3.0f}, {1, 4, 3.0f},
	{1, 6, -4.0f}, {1, 6, 4.0f}, {1, 5, -4.0f}, {1, 5, 4.0f},
	{1, 7, -5.0f}, {1, 7, 5.0f}, {1, 6, -5.0f}, {1, 6, 5.0f},
	{1, 8, -6.0f}, {1, 8, 6.0f}, {1, 7, -6.0f}, {1, 7, 6.0f},
};

std::uint8_t hex_nibble(char value)
{
	return static_cast<std::uint8_t>(
		value <= '9' ? value - '0' : value - 'a' + 10);
}

std::uint8_t hex_byte(const char* text, std::size_t index)
{
	return static_cast<std::uint8_t>(
		hex_nibble(text[index * 2]) << 4
		| hex_nibble(text[index * 2 + 1]));
}

float codebook(std::uint32_t index)
{
	const std::size_t offset = static_cast<std::size_t>(index) * 4;
	const std::uint32_t bits =
		static_cast<std::uint32_t>(hex_byte(kCodebookHex, offset))
		| static_cast<std::uint32_t>(hex_byte(kCodebookHex, offset + 1)) << 8
		| static_cast<std::uint32_t>(hex_byte(kCodebookHex, offset + 2)) << 16
		| static_cast<std::uint32_t>(hex_byte(kCodebookHex, offset + 3)) << 24;
	return std::bit_cast<float>(bits);
}

float f32(double value)
{
	return static_cast<float>(value);
}

std::uint8_t source_byte(Cb97Decoder& decoder)
{
	const std::size_t stream_size = decoder.file_size - 12;
	const std::size_t position = decoder.byte_position++;
	if (position >= stream_size)
	{
		return 0;
	}

	std::uint8_t value = decoder.file[12 + position];
	if (position < decoder.file_size - 28)
	{
		constexpr std::uint8_t key[] = {0xab, 0x2d, 0x9a, 0xaa};
		value ^= key[position & 3];
	}
	return value;
}

void skip_bits(Cb97Decoder& decoder, std::uint32_t count)
{
	decoder.reservoir >>= count;
	decoder.reservoir_bits -= count;
	if (decoder.reservoir_bits < 8)
	{
		decoder.reservoir |=
			static_cast<std::uint32_t>(source_byte(decoder))
			<< decoder.reservoir_bits;
		decoder.reservoir_bits += 8;
	}
}

std::uint32_t read_bits(Cb97Decoder& decoder, std::uint32_t count)
{
	const std::uint32_t mask = (1u << count) - 1;
	const std::uint32_t value = decoder.reservoir & mask;
	skip_bits(decoder, count);
	return value;
}

void decode_excitation(
	Cb97Decoder& decoder,
	bool variable,
	std::uint32_t stride,
	float (&output)[108])
{
	std::memset(output, 0, sizeof(output));
	std::uint32_t position = 0;
	if (!variable)
	{
		while (position < 108)
		{
			const std::uint32_t code = decoder.reservoir & 3;
			if (code == 1)
			{
				output[position] = -2.0f;
				skip_bits(decoder, 2);
			}
			else if (code == 3)
			{
				output[position] = 2.0f;
				skip_bits(decoder, 2);
			}
			else
			{
				skip_bits(decoder, 1);
			}
			position += stride;
		}
		return;
	}

	std::uint32_t lookup_state = 0;
	while (position < 108)
	{
		const std::uint8_t symbol = hex_byte(
			kSymbolLookupHex,
			lookup_state * 256 + (decoder.reservoir & 0xff));
		const SymbolRecord& record = kSymbols[symbol];
		lookup_state = record.next_state;
		skip_bits(decoder, record.consumed_bits);
		if (symbol < 2)
		{
			std::uint32_t magnitude = 7;
			while (read_bits(decoder, 1) != 0)
			{
				++magnitude;
			}
			output[position] = read_bits(decoder, 1) != 0
				? static_cast<float>(magnitude)
				: -static_cast<float>(magnitude);
			position += stride;
		}
		else if (symbol < 4)
		{
			std::uint32_t run = read_bits(decoder, 6) + 7;
			run = std::min(run, (108 - position) / stride);
			position += run * stride;
		}
		else
		{
			output[position] = record.value;
			position += stride;
		}
	}
}

void complete_interleaved(
	float (&excitation)[108],
	std::uint32_t parity,
	bool filtered)
{
	const std::uint32_t other = 1 - parity;
	if (!filtered)
	{
		for (std::uint32_t index = other; index < 108; index += 2)
		{
			excitation[index] = 0.0f;
		}
		return;
	}

	float source[108];
	std::memcpy(source, excitation, sizeof(source));
	auto sample = [&source](int index) {
		return index >= 0 && index < 108 ? source[index] : 0.0f;
	};
	for (std::uint32_t index = other; index < 108; index += 2)
	{
		const int at = static_cast<int>(index);
		excitation[index] = f32(
			(static_cast<double>(sample(at - 5)) + sample(at + 5))
				* 0.018032679334282875
			- (static_cast<double>(sample(at - 3)) + sample(at + 3))
				* 0.1145915612578392
			+ (static_cast<double>(sample(at - 1)) + sample(at + 1))
				* 0.5973859429359436);
	}
}

void predictor_coefficients(const float* reflection, float (&output)[12])
{
	float temporary[11];
	float stages[12];
	std::memcpy(temporary, reflection, sizeof(temporary));
	float boundary = 1.0f;
	for (std::uint32_t stage = 0; stage < 12; ++stage)
	{
		double accumulator =
			-static_cast<double>(temporary[10]) * reflection[11];
		for (int index = 10; index >= 0; --index)
		{
			const float previous = index == 0 ? boundary : temporary[index - 1];
			accumulator -=
				static_cast<double>(reflection[index]) * previous;
			temporary[index] = f32(
				accumulator * reflection[index] + previous);
		}
		boundary = f32(accumulator);
		stages[stage] = boundary;
		for (std::uint32_t index = 0; index < stage; ++index)
		{
			accumulator -=
				static_cast<double>(stages[stage - 1 - index]) * output[index];
		}
		output[stage] = f32(accumulator);
	}
}

void synthesize(
	Cb97Decoder& decoder,
	float* samples,
	std::uint32_t offset,
	std::uint32_t blocks)
{
	float predictor[12];
	predictor_coefficients(decoder.coefficients, predictor);
	for (std::uint32_t block = 0; block < blocks; ++block)
	{
		const std::uint32_t base = offset + block * 12;
		for (std::uint32_t phase = 0; phase < 12; ++phase)
		{
			double accumulator = samples[base + phase];
			for (std::uint32_t coefficient = 0; coefficient < 12; ++coefficient)
			{
				accumulator += static_cast<double>(predictor[coefficient])
					* decoder.synthesis_history[
						(coefficient + 12 - phase) % 12];
			}
			const float value = f32(accumulator);
			decoder.synthesis_history[(11 + 12 - phase) % 12] = value;
			samples[base + phase] = value;
		}
	}
}

bool decode_frame(Cb97Decoder& decoder)
{
	float deltas[12];
	const std::uint32_t first_index = read_bits(decoder, 6);
	const bool variable =
		first_index < decoder.first_coefficient_threshold;
	deltas[0] = f32(
		(static_cast<double>(codebook(first_index)) - decoder.coefficients[0])
		* 0.25);
	for (std::uint32_t index = 1; index < 4; ++index)
	{
		deltas[index] = f32(
			(static_cast<double>(codebook(read_bits(decoder, 6)))
				- decoder.coefficients[index]) * 0.25);
	}
	for (std::uint32_t index = 4; index < 12; ++index)
	{
		deltas[index] = f32(
			(static_cast<double>(codebook(16 + read_bits(decoder, 5)))
				- decoder.coefficients[index]) * 0.25);
	}

	float work[324 + kCb97FrameSamples]{};
	std::memcpy(work, decoder.overlap, sizeof(decoder.overlap));
	std::uint32_t output_position = 0;
	int history_base = 216;
	for (std::uint32_t subframe = 0; subframe < 4; ++subframe)
	{
		const int history_position =
			history_base - static_cast<int>(read_bits(decoder, 8));
		if (history_position < 0 || history_position + 108 > 756)
		{
			return false;
		}
		const float gain = f32(
			read_bits(decoder, 4) * 0.06666667014360428);
		float amplitude = decoder.amplitudes[read_bits(decoder, 6)];
		float excitation[108];
		if (decoder.interleaved_mode == 0)
		{
			decode_excitation(decoder, variable, 1, excitation);
		}
		else
		{
			const std::uint32_t parity = read_bits(decoder, 1);
			const bool filtered = read_bits(decoder, 1) == 0;
			decode_excitation(decoder, variable, 2, excitation);
			if (parity != 0)
			{
				for (int index = 107; index > 0; --index)
				{
					excitation[index] = excitation[index - 1];
				}
				excitation[0] = 0.0f;
			}
			complete_interleaved(excitation, parity, filtered);
			if (filtered)
			{
				amplitude = f32(static_cast<double>(amplitude) * 0.5);
			}
		}

		for (std::uint32_t index = 0; index < 108; ++index)
		{
			work[324 + output_position + index] = f32(
				static_cast<double>(gain) * work[history_position + index]
				+ static_cast<double>(amplitude) * excitation[index]);
		}
		output_position += 108;
		history_base += 108;
	}

	std::memcpy(decoder.frame, work + 324, sizeof(decoder.frame));
	std::memcpy(decoder.overlap, work + 432, sizeof(decoder.overlap));
	constexpr std::uint32_t offsets[] = {0, 12, 24, 36};
	constexpr std::uint32_t blocks[] = {1, 1, 1, 33};
	for (std::uint32_t phase = 0; phase < 4; ++phase)
	{
		for (std::uint32_t index = 0; index < 12; ++index)
		{
			decoder.coefficients[index] = f32(
				static_cast<double>(decoder.coefficients[index]) + deltas[index]);
		}
		synthesize(decoder, decoder.frame, offsets[phase], blocks[phase]);
	}
	decoder.frame_position = 0;
	return true;
}

std::int16_t pcm_word(float sample)
{
	const std::uint32_t biased =
		std::bit_cast<std::uint32_t>(sample + 12582912.0f);
	std::uint32_t value = biased & 0x1ffff;
	if (value > 0x7fff && value < 0x18000)
	{
		value = value >= 0x10000 ? 0x8000 : 0x7fff;
	}
	return std::bit_cast<std::int16_t>(
		static_cast<std::uint16_t>(value));
}

bool reset_codec(Cb97Decoder& decoder)
{
	decoder.byte_position = 0;
	decoder.reservoir = source_byte(decoder);
	decoder.reservoir_bits = 8;
	decoder.interleaved_mode = static_cast<std::uint8_t>(read_bits(decoder, 1));
	decoder.first_coefficient_threshold = 32 - read_bits(decoder, 4);
	decoder.amplitudes[0] = f32((read_bits(decoder, 4) + 1) * 8.0);
	const double ratio =
		read_bits(decoder, 6) * 0.0010000000474974513
		+ 1.0399999618530273;
	for (std::uint32_t index = 1; index < 64; ++index)
	{
		decoder.amplitudes[index] =
			f32(ratio * decoder.amplitudes[index - 1]);
	}
	std::memset(decoder.coefficients, 0, sizeof(decoder.coefficients));
	std::memset(decoder.synthesis_history, 0, sizeof(decoder.synthesis_history));
	std::memset(decoder.overlap, 0, sizeof(decoder.overlap));
	decoder.sample_position = 0;
	decoder.frame_position = kCb97FrameSamples;
	return true;
}
}

bool cb97_init(
	Cb97Decoder& decoder,
	const std::uint8_t* file,
	std::size_t file_size)
{
	decoder = {};
	if (file == nullptr || file_size < 29 || file_size > UINT32_MAX
		|| sl_open::io::read_le32(file) != file_size - 4
		|| std::memcmp(file + 4, "CB", 2) != 0)
	{
		return false;
	}
	const std::uint32_t decoded_bytes = sl_open::io::read_le32(file + 8);
	if (decoded_bytes == 0 || (decoded_bytes & 1) != 0)
	{
		return false;
	}
	decoder.file = file;
	decoder.file_size = file_size;
	decoder.total_samples = decoded_bytes / 2;
	decoder.valid = reset_codec(decoder);
	return decoder.valid;
}

std::uint32_t cb97_read(
	Cb97Decoder& decoder,
	std::int16_t* output,
	std::uint32_t sample_count,
	bool loop)
{
	if (!decoder.valid || output == nullptr)
	{
		return 0;
	}

	std::uint32_t written = 0;
	while (written < sample_count)
	{
		if (decoder.sample_position == decoder.total_samples)
		{
			if (!loop)
			{
				break;
			}
			reset_codec(decoder);
		}
		if (decoder.frame_position == kCb97FrameSamples
			&& !decode_frame(decoder))
		{
			decoder.valid = false;
			break;
		}

		const std::uint32_t take = std::min({
			sample_count - written,
			kCb97FrameSamples - decoder.frame_position,
			decoder.total_samples - decoder.sample_position,
		});
		for (std::uint32_t index = 0; index < take; ++index)
		{
			output[written + index] =
				pcm_word(decoder.frame[decoder.frame_position + index]);
		}
		written += take;
		decoder.frame_position += take;
		decoder.sample_position += take;
	}
	return written;
}
}
