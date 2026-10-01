/*
 * Bink Audio decoder
 * Copyright (c) 2007-2011 Peter Ross (pross@xvid.org)
 * Copyright (c) 2009 Daniel Verkamp (daniel@drv.nu)
 *
 * This file is derived from FFmpeg and is licensed under the GNU Lesser
 * General Public License version 2.1 or later.
 */

#include "media/bink_audio.hpp"

#include "io/endian.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sl_open::media
{
namespace
{
constexpr std::uint16_t kCriticalFrequencies[25] = {
	100, 200, 300, 400, 510, 630, 770, 920,
	1080, 1270, 1480, 1720, 2000, 2320, 2700, 3150,
	3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 24500,
};

constexpr std::uint8_t kRunLengths[16] = {
	2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64,
};

constexpr float kPi = 3.14159265358979323846f;

struct Bits
{
	const std::uint8_t* data{};
	std::uint32_t size{};
	std::uint32_t position{};
	bool failed{};
};

std::uint32_t read_bits(Bits& bits, std::uint32_t count)
{
	if (count > 32 || count > bits.size - std::min(bits.size, bits.position))
	{
		bits.failed = true;
		bits.position = bits.size;
		return 0;
	}
	std::uint32_t value = 0;
	for (std::uint32_t index = 0; index < count; ++index)
	{
		const std::uint32_t position = bits.position++;
		value |= ((bits.data[position >> 3] >> (position & 7)) & 1u) << index;
	}
	return value;
}

float read_float(Bits& bits)
{
	const int power = static_cast<int>(read_bits(bits, 5));
	float value = std::ldexp(
		static_cast<float>(read_bits(bits, 23)), power - 23);
	if (read_bits(bits, 1) != 0)
	{
		value = -value;
	}
	return value;
}

void complex_fft(float* data, std::uint32_t count)
{
	for (std::uint32_t index = 1, reversed = 0; index < count; ++index)
	{
		std::uint32_t bit = count >> 1;
		for (; (reversed & bit) != 0; bit >>= 1)
		{
			reversed ^= bit;
		}
		reversed ^= bit;
		if (index < reversed)
		{
			std::swap(data[index * 2], data[reversed * 2]);
			std::swap(data[index * 2 + 1], data[reversed * 2 + 1]);
		}
	}

	for (std::uint32_t length = 2; length <= count; length <<= 1)
	{
		const float angle = -2.0f * kPi / static_cast<float>(length);
		const float step_real = std::cos(angle);
		const float step_imaginary = std::sin(angle);
		for (std::uint32_t start = 0; start < count; start += length)
		{
			float twiddle_real = 1.0f;
			float twiddle_imaginary = 0.0f;
			for (std::uint32_t offset = 0; offset < length / 2; ++offset)
			{
				const std::uint32_t even = (start + offset) * 2;
				const std::uint32_t odd = (start + offset + length / 2) * 2;
				const float odd_real =
					data[odd] * twiddle_real
					- data[odd + 1] * twiddle_imaginary;
				const float odd_imaginary =
					data[odd] * twiddle_imaginary
					+ data[odd + 1] * twiddle_real;
				const float even_real = data[even];
				const float even_imaginary = data[even + 1];
				data[even] = even_real + odd_real;
				data[even + 1] = even_imaginary + odd_imaginary;
				data[odd] = even_real - odd_real;
				data[odd + 1] = even_imaginary - odd_imaginary;

				const float next_real =
					twiddle_real * step_real
					- twiddle_imaginary * step_imaginary;
				twiddle_imaginary =
					twiddle_real * step_imaginary
					+ twiddle_imaginary * step_real;
				twiddle_real = next_real;
			}
		}
	}
}

void inverse_rdft(float* data, std::uint32_t count)
{
	const float first = data[0];
	data[0] = (first + data[1]) * 0.5f;
	data[1] = (first - data[1]) * 0.5f;

	for (std::uint32_t index = 1; index < count / 4; ++index)
	{
		const std::uint32_t first_index = index * 2;
		const std::uint32_t second_index = count - first_index;
		const float even_real =
			0.5f * (data[first_index] + data[second_index]);
		const float odd_imaginary =
			-0.5f * (data[second_index] - data[first_index]);
		const float even_imaginary =
			0.5f * (data[first_index + 1] - data[second_index + 1]);
		const float odd_real =
			-0.5f * (data[first_index + 1] + data[second_index + 1]);
		const float angle =
			2.0f * kPi * static_cast<float>(index)
			/ static_cast<float>(count);
		const float cosine = std::cos(angle);
		const float sine = std::sin(angle);
		const float sum_real = odd_real * cosine + odd_imaginary * sine;
		const float sum_imaginary =
			odd_imaginary * cosine - odd_real * sine;
		data[first_index] = even_real + sum_real;
		data[first_index + 1] = even_imaginary + sum_imaginary;
		data[second_index] = even_real - sum_real;
		data[second_index + 1] = sum_imaginary - even_imaginary;
	}
	complex_fft(data, count / 2);
}

std::int16_t to_pcm16(float value)
{
	const float scaled = std::clamp(value * 32768.0f, -32768.0f, 32767.0f);
	return static_cast<std::int16_t>(std::lrint(scaled));
}
}

bool bink_audio_init(
	BinkAudioDecoder& decoder,
	std::uint32_t sample_rate,
	std::uint8_t channels)
{
	decoder = {};
	if ((sample_rate != 22050 && sample_rate != 44100)
		|| (channels != 1 && channels != 2))
	{
		return false;
	}

	std::uint32_t frame_bits = sample_rate < 44100 ? 10 : 11;
	if (channels == 2)
	{
		++frame_bits;
	}
	decoder.sample_rate = sample_rate;
	decoder.channels = channels;
	decoder.frame_length = 1u << frame_bits;
	decoder.overlap_length = decoder.frame_length / 16;
	decoder.block_samples =
		decoder.frame_length - decoder.overlap_length;
	const std::uint32_t transformed_rate = sample_rate * channels;
	const std::uint32_t half_rate = (transformed_rate + 1) / 2;
	const float root =
		2.0f / (std::sqrt(static_cast<float>(decoder.frame_length)) * 32768.0f);
	for (std::uint32_t index = 0; index < 96; ++index)
	{
		decoder.quant_table[index] =
			std::exp(static_cast<float>(index) * 0.15289164787221953823f)
			* root;
	}

	decoder.band_count = 1;
	while (decoder.band_count < 25
		&& half_rate > kCriticalFrequencies[decoder.band_count - 1])
	{
		++decoder.band_count;
	}
	decoder.bands[0] = 2;
	for (std::uint32_t index = 1; index < decoder.band_count; ++index)
	{
		decoder.bands[index] =
			(kCriticalFrequencies[index - 1] * decoder.frame_length / half_rate)
			& ~1u;
	}
	decoder.bands[decoder.band_count] = decoder.frame_length;
	decoder.first_block = true;
	return true;
}

bool bink_audio_packet_begin(
	BinkAudioDecoder& decoder,
	const std::uint8_t* packet,
	std::size_t packet_size)
{
	if (decoder.frame_length == 0 || packet == nullptr || packet_size < 4
		|| packet_size > UINT32_MAX / 8)
	{
		return false;
	}
	decoder.packet = packet;
	decoder.packet_size = packet_size;
	decoder.bit_position = 32;
	decoder.expected_bytes = io::read_le32(packet);
	if (decoder.expected_bytes == 0
		|| decoder.expected_bytes > 192 * 1024
		|| decoder.expected_bytes % (decoder.channels * 2) != 0)
	{
		decoder.packet_active = false;
		return false;
	}
	decoder.packet_active = true;
	return true;
}

bool bink_audio_decode_block(
	BinkAudioDecoder& decoder,
	std::int16_t* output,
	std::size_t frame_capacity,
	std::uint32_t& decoded_frames,
	bool& packet_finished)
{
	decoded_frames = 0;
	packet_finished = false;
	const std::uint32_t frames = decoder.block_samples / decoder.channels;
	if (!decoder.packet_active || output == nullptr || frame_capacity < frames)
	{
		return false;
	}

	Bits bits{
		decoder.packet,
		static_cast<std::uint32_t>(decoder.packet_size * 8),
		decoder.bit_position,
		false,
	};
	float* coefficients = decoder.coefficients;
	const float root =
		2.0f / (std::sqrt(static_cast<float>(decoder.frame_length)) * 32768.0f);
	coefficients[0] = read_float(bits) * root;
	coefficients[1] = read_float(bits) * root;

	float quant[25];
	for (std::uint32_t index = 0; index < decoder.band_count; ++index)
	{
		quant[index] =
			decoder.quant_table[std::min<std::uint32_t>(read_bits(bits, 8), 95)];
	}

	std::uint32_t band = 0;
	float scale = quant[0];
	std::uint32_t index = 2;
	while (index < decoder.frame_length)
	{
		const std::uint32_t end = std::min(
			decoder.frame_length,
			index + (read_bits(bits, 1) != 0
				? static_cast<std::uint32_t>(
					kRunLengths[read_bits(bits, 4)]) * 8
				: 8u));
		const std::uint32_t width = read_bits(bits, 4);
		if (width == 0)
		{
			std::memset(
				coefficients + index,
				0,
				(end - index) * sizeof(*coefficients));
			index = end;
			while (decoder.bands[band] < index)
			{
				scale = quant[band++];
			}
			continue;
		}
		while (index < end)
		{
			if (decoder.bands[band] == index)
			{
				scale = quant[band++];
			}
			const std::uint32_t coefficient = read_bits(bits, width);
			if (coefficient == 0)
			{
				coefficients[index] = 0.0f;
			}
			else
			{
				coefficients[index] =
					(read_bits(bits, 1) != 0 ? -scale : scale)
					* static_cast<float>(coefficient);
			}
			++index;
		}
	}
	if (bits.failed)
	{
		decoder.packet_active = false;
		return false;
	}

	inverse_rdft(coefficients, decoder.frame_length);
	if (!decoder.first_block)
	{
		const std::uint32_t count = decoder.overlap_length;
		for (std::uint32_t overlap = 0; overlap < decoder.overlap_length; ++overlap)
		{
			coefficients[overlap] =
				(decoder.previous[overlap] * static_cast<float>(count - overlap)
					+ coefficients[overlap] * static_cast<float>(overlap))
				/ static_cast<float>(count);
		}
	}
	std::memcpy(
		decoder.previous,
		coefficients + decoder.frame_length - decoder.overlap_length,
		decoder.overlap_length * sizeof(*decoder.previous));
	decoder.first_block = false;

	for (std::uint32_t sample = 0; sample < decoder.block_samples; ++sample)
	{
		output[sample] = to_pcm16(coefficients[sample]);
	}
	decoded_frames = frames;

	bits.position = (bits.position + 31) & ~31u;
	if (bits.position >= bits.size)
	{
		decoder.packet_active = false;
		packet_finished = true;
	}
	else
	{
		decoder.bit_position = bits.position;
	}
	return true;
}
}
