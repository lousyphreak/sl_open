#pragma once

#include "media/bink_dsp.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define av_cold
#define av_restrict
#define FFMIN(a, b) std::min((a), (b))
#define FFMAX(a, b) std::max((a), (b))
#define FFALIGN(value, alignment) (((value) + (alignment) - 1) & ~((alignment) - 1))
#define FFSWAP(type, a, b) do { type swap_tmp = (a); (a) = (b); (b) = swap_tmp; } while (0)
#define AVERROR(value) (-(value))
#define AVERROR_INVALIDDATA (-1)
#define AV_LOG_ERROR 0
#define AV_LOG_WARNING 1
#define LOCAL_ALIGNED_16(type, name, array) alignas(16) type name array
#define LOCAL_ALIGNED_32(type, name, array) alignas(32) type name array
#define emms_c() do {} while (0)

struct AVCodecContext
{
	void* priv_data{};
	const std::uint8_t* extradata{};
	int extradata_size{};
	int width{};
	int height{};
	int flags{};
	int pix_fmt{};
	int color_range{};
	std::uint32_t codec_tag{};
};

struct AVFrame
{
	std::uint8_t* data[4]{};
	int linesize[4]{};
};

struct AVPacket
{
	std::uint8_t* data{};
	int size{};
};

struct BlockDSPContext
{
	void (*clear_block)(std::int16_t*){};
	void (*fill_block_tab[2])(std::uint8_t*, std::uint8_t, int, int){};
};

using op_pixels_func =
	void (*)(std::uint8_t*, const std::uint8_t*, int, int);

struct HpelDSPContext
{
	op_pixels_func put_pixels_tab[2][1]{};
};

struct GetBitContext
{
	const std::uint8_t* data{};
	int bit_size{};
	int bit_position{};
	bool failed{};
};

inline void init_get_bits(
	GetBitContext* bits,
	const std::uint8_t* data,
	int bit_count)
{
	*bits = {data, bit_count, 0, false};
}

inline int get_bits_left(const GetBitContext* bits)
{
	return bits->bit_size - bits->bit_position;
}

inline int get_bits_count(const GetBitContext* bits)
{
	return bits->bit_position;
}

inline unsigned get_bits(GetBitContext* bits, int count)
{
	if (count < 0 || count > 32 || get_bits_left(bits) < count)
	{
		bits->failed = true;
		bits->bit_position = bits->bit_size;
		return 0;
	}
	unsigned value = 0;
	for (int index = 0; index < count; ++index)
	{
		const int position = bits->bit_position++;
		value |= ((bits->data[position >> 3] >> (position & 7)) & 1u) << index;
	}
	return value;
}

inline unsigned get_bits1(GetBitContext* bits)
{
	return get_bits(bits, 1);
}

inline void skip_bits_long(GetBitContext* bits, int count)
{
	(void)get_bits(bits, count);
}

inline int av_log2(unsigned value)
{
	int result = 0;
	while (value >>= 1)
	{
		++result;
	}
	return result;
}

inline std::uint32_t AV_RL32(const std::uint8_t* bytes)
{
	return static_cast<std::uint32_t>(bytes[0])
		| static_cast<std::uint32_t>(bytes[1]) << 8
		| static_cast<std::uint32_t>(bytes[2]) << 16
		| static_cast<std::uint32_t>(bytes[3]) << 24;
}

inline void av_log(void*, int, const char* format, ...)
{
	std::fputs("Bink decode: ", stderr);
	va_list arguments;
	va_start(arguments, format);
	std::vfprintf(stderr, format, arguments);
	va_end(arguments);
}

inline void* av_calloc(std::size_t count, std::size_t size)
{
	return std::calloc(count, size);
}

inline void av_freep(void* pointer)
{
	void** value = static_cast<void**>(pointer);
	std::free(*value);
	*value = nullptr;
}

inline void clear_block(std::int16_t* block)
{
	std::memset(block, 0, 64 * sizeof(*block));
}

inline void fill_block(
	std::uint8_t* destination,
	std::uint8_t value,
	int stride,
	int height)
{
	for (int y = 0; y < height; ++y)
	{
		std::memset(destination + y * stride, value, 8);
	}
}

inline void fill_block16(
	std::uint8_t* destination,
	std::uint8_t value,
	int stride,
	int height)
{
	for (int y = 0; y < height; ++y)
	{
		std::memset(destination + y * stride, value, 16);
	}
}

inline void copy_pixels(
	std::uint8_t* destination,
	const std::uint8_t* source,
	int stride,
	int height)
{
	for (int y = 0; y < height; ++y)
	{
		std::memmove(destination + y * stride, source + y * stride, 8);
	}
}

inline void ff_blockdsp_init(BlockDSPContext* context, AVCodecContext*)
{
	context->clear_block = clear_block;
	context->fill_block_tab[0] = fill_block16;
	context->fill_block_tab[1] = fill_block;
}

inline void ff_hpeldsp_init(HpelDSPContext* context, int)
{
	context->put_pixels_tab[1][0] = copy_pixels;
}
