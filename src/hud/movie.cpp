#include "hud/movie.hpp"

#include "assets/refpack.hpp"
#include "core/mission_log.hpp"
#include "io/endian.hpp"
#include "io/vfs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace sl_open::hud
{
namespace
{
constexpr std::uint8_t kXorKey[4] = {0xa3, 0x27, 0xb7, 0xdd};
constexpr std::uint32_t kMovieRateHz = 15;

bool asset_path(const char* basename, char (&path)[128])
{
	if (basename == nullptr)
	{
		return false;
	}
	const int length = std::snprintf(
		path,
		sizeof(path),
		"%s%s",
		basename,
		std::strstr(basename, ".fm8") == nullptr ? ".fm8" : "");
	return length > 0 && static_cast<std::size_t>(length) < sizeof(path);
}

bool path_equal(const char* left, const char* right)
{
	while (*left != '\0' && *right != '\0')
	{
		const unsigned char a = static_cast<unsigned char>(
			*left == '\\' ? '/' : *left);
		const unsigned char b = static_cast<unsigned char>(
			*right == '\\' ? '/' : *right);
		if (std::tolower(a) != std::tolower(b))
		{
			return false;
		}
		++left;
		++right;
	}
	return *left == *right;
}

const char* path_basename(const char* path)
{
	const char* result = path;
	for (; *path != '\0'; ++path)
	{
		if (*path == '/' || *path == '\\')
		{
			result = path + 1;
		}
	}
	return result;
}

std::uint32_t read_bits(
	const std::uint8_t* source,
	std::size_t bytes,
	std::uint32_t bit,
	std::uint32_t count)
{
	if (count == 0 || count > 31
		|| static_cast<std::uint64_t>(bit) + count
			> static_cast<std::uint64_t>(bytes) * 8)
	{
		return UINT32_MAX;
	}
	std::uint64_t packed = 0;
	const std::uint32_t first_byte = bit >> 3;
	const std::uint32_t shift = bit & 7;
	const std::uint32_t needed = (shift + count + 7) >> 3;
	for (std::uint32_t index = 0; index < needed; ++index)
	{
		packed |= static_cast<std::uint64_t>(
			source[first_byte + index]) << (index * 8);
	}
	return static_cast<std::uint32_t>(
		(packed >> shift) & ((std::uint64_t{1} << count) - 1));
}

std::int32_t sign_extend(std::uint32_t value, std::uint32_t bits)
{
	const std::uint32_t sign = 1u << (bits - 1);
	return static_cast<std::int32_t>((value ^ sign) - sign);
}

void update_rgba(Movie& movie)
{
	const std::uint8_t* pixels = movie.frames[movie.current_frame];
	const std::uint32_t count =
		static_cast<std::uint32_t>(movie.width) * movie.height;
	for (std::uint32_t index = 0; index < count; ++index)
	{
		const std::uint8_t color = pixels[index];
		movie.rgba[index * 4] = movie.palette[color * 3];
		movie.rgba[index * 4 + 1] = movie.palette[color * 3 + 1];
		movie.rgba[index * 4 + 2] = movie.palette[color * 3 + 2];
		movie.rgba[index * 4 + 3] = 255;
	}
	movie.ready = true;
	movie.new_frame = true;
}

bool load_chunk(Movie& movie)
{
	if (movie.asset == nullptr
		|| movie.offset + 8 > movie.asset->stream.size)
	{
		return false;
	}
	const std::uint32_t size =
		io::read_le32(movie.asset->stream.data + movie.offset + 4);
	if (size < 8 || size > movie.asset->stream.size - movie.offset)
	{
		return false;
	}
	movie.chunk.assign(
		movie.asset->stream.data + movie.offset,
		movie.asset->stream.data + movie.offset + size);
	for (std::uint32_t index = 8; index < size - 8; ++index)
	{
		movie.chunk[index] ^= kXorKey[index & 3];
	}
	movie.offset += size;
	return true;
}

bool decode_key(Movie& movie)
{
	const std::uint8_t* data = movie.chunk.data();
	const std::size_t size = movie.chunk.size();
	if (size < 28)
	{
		return false;
	}
	const std::uint16_t height = io::read_le16(data + 8);
	const std::uint16_t width = io::read_le16(data + 10);
	const std::uint16_t palette_end = io::read_le16(data + 12);
	const std::uint16_t palette_start = io::read_le16(data + 14);
	if (width == 0 || height == 0 || (width & 3) != 0
		|| (height & 3) != 0
		|| static_cast<std::uint32_t>(width) * height
			> kMovieMaximumPixels
		|| palette_start > palette_end
		|| palette_end > 256)
	{
		return false;
	}
	const std::size_t refpack_offset =
		0x14 + static_cast<std::size_t>(palette_end) * 3;
	if (refpack_offset + 5 > size)
	{
		return false;
	}
	for (std::uint32_t entry = palette_start;
		entry < palette_end;
		++entry)
	{
		std::memcpy(
			movie.palette + entry * 3,
			data + 0x14 + entry * 3,
			3);
	}
	sl_open::Blob packed;
	sl_open::Blob pixels;
	if (!packed.allocate(size - refpack_offset))
	{
		return false;
	}
	std::memcpy(
		packed.data,
		data + refpack_offset,
		packed.size);
	if (!assets::refpack_decompress(packed, pixels)
		|| pixels.size != static_cast<std::size_t>(width) * height)
	{
		return false;
	}
	movie.width = width;
	movie.height = height;
	movie.current_frame = 0;
	std::memcpy(movie.frames[0], pixels.data, pixels.size);
	std::memset(movie.frames[1], 0, pixels.size);
	update_rgba(movie);
	return true;
}

bool decode_delta(Movie& movie)
{
	const std::uint8_t* data = movie.chunk.data();
	const std::size_t size = movie.chunk.size();
	if (!movie.ready || size < 28)
	{
		return false;
	}
	const std::uint32_t residual_bits = io::read_le16(data + 8);
	const std::uint32_t literal_count = io::read_le16(data + 10);
	const std::uint32_t motion_count = io::read_le16(data + 12);
	const std::uint32_t selector_count = io::read_le16(data + 14);
	const std::uint32_t operation_count =
		literal_count + selector_count;
	if (residual_bits == 0 || residual_bits > 31
		|| motion_count > kMovieMaximumOperations
		|| operation_count > kMovieMaximumOperations)
	{
		return false;
	}
	std::size_t cursor = 0x14;
	const std::size_t motion_bytes =
		((static_cast<std::size_t>(motion_count) * 20 + 31) / 32) * 4;
	if (motion_bytes > size - cursor)
	{
		return false;
	}
	for (std::uint32_t index = 0; index < motion_count; ++index)
	{
		const std::uint32_t dx =
			read_bits(data + cursor, motion_bytes, index * 20, 10);
		const std::uint32_t dy =
			read_bits(data + cursor, motion_bytes, index * 20 + 10, 10);
		if (dx == UINT32_MAX || dy == UINT32_MAX)
		{
			return false;
		}
		movie.motion_offsets[index] =
			sign_extend(dy, 10) * movie.width + sign_extend(dx, 10);
	}
	cursor += motion_bytes;
	if (static_cast<std::size_t>(literal_count) * 16 > size - cursor)
	{
		return false;
	}
	for (std::uint32_t index = 0; index < literal_count; ++index)
	{
		std::memcpy(movie.operations[index], data + cursor, 16);
		cursor += 16;
	}
	if (static_cast<std::size_t>(selector_count) * 8 > size - cursor)
	{
		return false;
	}
	for (std::uint32_t index = 0; index < selector_count; ++index)
	{
		const std::uint8_t* source = data + cursor;
		const std::uint32_t selectors = io::read_le32(source + 4);
		std::uint8_t* output =
			movie.operations[literal_count + index];
		for (std::uint32_t pixel = 0; pixel < 16; ++pixel)
		{
			output[pixel] = source[
				(selectors >> (2 * (15 - pixel))) & 3];
		}
		cursor += 8;
	}

	const std::uint32_t block_columns = movie.width / 4;
	const std::uint32_t block_count =
		static_cast<std::uint32_t>(movie.width) * movie.height / 16;
	const std::size_t residual_bytes = size - cursor;
	if (static_cast<std::uint64_t>(block_count) * residual_bits
		> residual_bytes * 8)
	{
		return false;
	}
	const std::uint8_t previous_index = movie.current_frame;
	const std::uint8_t current_index =
		static_cast<std::uint8_t>(1 - previous_index);
	const std::uint8_t* previous = movie.frames[previous_index];
	std::uint8_t* current = movie.frames[current_index];
	const std::int32_t frame_pixels =
		static_cast<std::int32_t>(movie.width) * movie.height;
	for (std::uint32_t block = 0; block < block_count; ++block)
	{
		const std::uint32_t code =
			read_bits(
				data + cursor,
				residual_bytes,
				block * residual_bits,
				residual_bits);
		if (code == UINT32_MAX)
		{
			return false;
		}
		const std::int32_t destination =
			static_cast<std::int32_t>(
				(block / block_columns) * 4 * movie.width
				+ (block % block_columns) * 4);
		if (code < motion_count)
		{
			const std::int32_t source =
				destination + movie.motion_offsets[code];
			if (source < 0
				|| source + 3 * movie.width + 4 > frame_pixels)
			{
				return false;
			}
			for (std::uint32_t row = 0; row < 4; ++row)
			{
				std::memcpy(
					current + destination + row * movie.width,
					previous + source + row * movie.width,
					4);
			}
		}
		else
		{
			const std::uint32_t operation = code - motion_count;
			if (operation >= operation_count)
			{
				return false;
			}
			for (std::uint32_t row = 0; row < 4; ++row)
			{
				std::memcpy(
					current + destination + row * movie.width,
					movie.operations[operation] + row * 4,
					4);
			}
		}
	}
	movie.current_frame = current_index;
	update_rgba(movie);
	return true;
}

bool decode_next(Movie& movie)
{
	if (!load_chunk(movie))
	{
		return false;
	}
	if (std::memcmp(movie.chunk.data(), "fYEK", 4) == 0)
	{
		return decode_key(movie);
	}
	if (std::memcmp(movie.chunk.data(), "fLED", 4) == 0)
	{
		return decode_delta(movie);
	}
	return std::memcmp(movie.chunk.data(), "fDNE", 4) == 0;
}

bool is_end(const Movie& movie)
{
	return movie.chunk.size() == 8
		&& std::memcmp(movie.chunk.data(), "fDNE", 4) == 0;
}
}

bool movie_asset_load(
	io::Vfs& vfs,
	const char* basename,
	MovieAsset& asset)
{
	asset = {};
	if (!asset_path(basename, asset.path)
		|| !io::vfs_read_all(vfs, asset.path, asset.stream))
	{
		asset = {};
		return false;
	}
	return true;
}

bool movie_asset_matches(const MovieAsset& asset, const char* basename)
{
	char path[128];
	return asset_path(basename, path)
		&& (path_equal(asset.path, path)
			|| path_equal(path_basename(asset.path), path_basename(path)));
}

bool movie_play(
	Movie& movie,
	const MovieAsset& asset,
	const MovieAsset* static_asset,
	std::uint8_t flags,
	std::uint64_t now)
{
	movie_stop(movie);
	movie.asset = &asset;
	movie.static_asset = static_asset;
	movie.flags = flags;
	movie.offset = 0;
	if (!decode_next(movie) || is_end(movie))
	{
		movie_stop(movie);
		return false;
	}
	movie.frame_count = 1;
	movie.next_frame_units = now * kMovieRateHz + 1000;
	movie.active = true;
	diagnostics::mission_log(
		"hud movie path=%s size=%ux%u flags=0x%x",
		asset.path,
		static_cast<unsigned>(movie.width),
		static_cast<unsigned>(movie.height),
		static_cast<unsigned>(flags));
	return true;
}

void movie_update(Movie& movie, std::uint64_t now)
{
	std::uint32_t callbacks = 0;
	while (movie.active
		&& now * kMovieRateHz >= movie.next_frame_units
		&& callbacks < 50)
	{
		movie.next_frame_units += 1000;
		++callbacks;
		if (!decode_next(movie))
		{
			diagnostics::mission_log(
				"hud movie decode failed frame=%u offset=0x%x",
				movie.frame_count,
				movie.offset);
			movie.active = false;
			return;
		}
		if (is_end(movie))
		{
			if ((movie.flags & 2) != 0)
			{
				const MovieAsset* static_asset = movie.static_asset;
				if (static_asset == nullptr
					|| !movie_play(
						movie, *static_asset, static_asset, 0x0d, now))
				{
					movie.active = false;
				}
				return;
			}
			if ((movie.flags & 1) != 0)
			{
				movie.offset = 0;
				if (!decode_next(movie) || is_end(movie))
				{
					movie.active = false;
					return;
				}
				movie.frame_count = 1;
				continue;
			}
			movie.active = false;
			return;
		}
		++movie.frame_count;
	}
}

void movie_stop(Movie& movie)
{
	movie.asset = nullptr;
	movie.static_asset = nullptr;
	movie.chunk.clear();
	movie.offset = 0;
	movie.frame_count = 0;
	movie.active = false;
	movie.ready = false;
	movie.new_frame = false;
}
}
