#include "assets/texture_cache.hpp"

#include "assets/image.hpp"
#include "assets/refpack.hpp"
#include "io/endian.hpp"
#include "io/vfs.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>

namespace sl_open::assets
{
namespace
{
constexpr std::uint32_t kVersion = 0x66;
constexpr std::size_t kHeaderBytes = 0x0c;
constexpr std::size_t kRecordBytes = 0xf0;
constexpr std::size_t kRecordArrayBytes =
	kTextureCacheCapacity * kRecordBytes;
constexpr std::size_t kPixelDataOffset =
	kHeaderBytes + kRecordArrayBytes;
constexpr std::size_t kColorCubePaletteBytes = 0x300;
constexpr std::size_t kColorCubeLookupBytes = 0xc00;
constexpr std::size_t kColorCubeMetadataOffset =
	kColorCubePaletteBytes + kColorCubeLookupBytes;
constexpr std::size_t kColorCubeHeaderBytes =
	kColorCubeMetadataOffset + 0x20;

bool ascii_equal(const char* left, const char* right)
{
	for (;; ++left, ++right)
	{
		const unsigned char a = static_cast<unsigned char>(*left);
		const unsigned char b = static_cast<unsigned char>(*right);
		if (std::tolower(a) != std::tolower(b))
		{
			return false;
		}
		if (a == 0)
		{
			return true;
		}
	}
}

std::uint32_t channel_value(
	std::uint32_t packed,
	std::uint32_t mask)
{
	if (mask == 0)
	{
		return 0;
	}
	const unsigned shift = std::countr_zero(mask);
	const std::uint32_t maximum = mask >> shift;
	const std::uint32_t value = (packed & mask) >> shift;
	return maximum == 0 ? 0 : value * 255 / maximum;
}

bool calculate_pixel_bytes(
	std::uint32_t width,
	std::uint32_t height,
	std::uint32_t levels,
	bool mipmapped,
	std::uint32_t bytes_per_pixel,
	std::uint32_t& result)
{
	std::uint64_t pixels = 0;
	const std::uint32_t count = mipmapped ? levels : 1;
	if (count == 0)
	{
		return false;
	}
	for (std::uint32_t level = 0; level < count; ++level)
	{
		pixels += static_cast<std::uint64_t>(width) * height;
		width = std::max(width / 2, 1u);
		height = std::max(height / 2, 1u);
	}
	const std::uint64_t bytes = pixels * bytes_per_pixel;
	if (bytes > UINT32_MAX)
	{
		return false;
	}
	result = static_cast<std::uint32_t>(bytes);
	return true;
}
}

bool texture_cache_load(
	io::Vfs& vfs,
	const char* cache_path,
	const char* palette_path,
	TextureCache& cache)
{
	cache = {};
	sl_open::Blob stored_palette;
	sl_open::Blob color_cube;
	if (!io::vfs_read_all(vfs, cache_path, cache.file)
		|| !io::vfs_read_all(vfs, palette_path, stored_palette)
		|| !unwrap_refpack(
			static_cast<sl_open::Blob&&>(stored_palette), color_cube)
		|| cache.file.size < kPixelDataOffset
		|| color_cube.size < kColorCubeHeaderBytes
		|| io::read_le32(cache.file.data) != kVersion)
	{
		cache = {};
		return false;
	}
	const std::uint32_t count = io::read_le32(cache.file.data + 4);
	const std::uint32_t pixel_end = io::read_le32(cache.file.data + 8);
	if (count > kTextureCacheCapacity
		|| pixel_end < kPixelDataOffset
		|| pixel_end > cache.file.size)
	{
		cache = {};
		return false;
	}
	const std::uint32_t color_cube_data_bytes =
		io::read_le32(
			color_cube.data + kColorCubeMetadataOffset + 0x1c);
	if (color_cube_data_bytes
			> color_cube.size - kColorCubeHeaderBytes
		|| !std::all_of(
			color_cube.data,
			color_cube.data + kColorCubePaletteBytes,
			[](std::uint8_t component) {
				return component <= 0x3fu;
			}))
	{
		cache = {};
		return false;
	}
	for (std::uint32_t index = 0; index < 256; ++index)
	{
		// SR_CCB_load (LANCER.EXE 0x004cb9d0) first unwraps the RefPack
		// member, then copies its leading 0x300 bytes as the active palette.
		// Those are six-bit DAC values. The hardware cache path expands each
		// component by shifting it left two bits; it does not replicate the
		// high bits into the low two positions.
		cache.palette[index * 4] =
			static_cast<std::uint8_t>(
				color_cube.data[index * 3] << 2);
		cache.palette[index * 4 + 1] =
			static_cast<std::uint8_t>(
				color_cube.data[index * 3 + 1] << 2);
		cache.palette[index * 4 + 2] =
			static_cast<std::uint8_t>(
				color_cube.data[index * 3 + 2] << 2);
		cache.palette[index * 4 + 3] = 255;
	}

	std::uint32_t expected_pixel_offset =
		static_cast<std::uint32_t>(kPixelDataOffset);
	for (std::uint32_t index = 0; index < count; ++index)
	{
		const std::uint8_t* source =
			cache.file.data + kHeaderBytes + index * kRecordBytes;
		TextureCacheRecord& record = cache.records[index];
		const void* terminator = std::memchr(source + 0x48, 0, 32);
		if (terminator == nullptr
			|| io::read_le32(source + 0x68) != index)
		{
			cache = {};
			return false;
		}
		const std::size_t name_bytes =
			static_cast<const std::uint8_t*>(terminator)
			- (source + 0x48);
		std::memcpy(record.name, source + 0x48, name_bytes);
		record.width = io::read_le32(source + 0x40);
		record.height = io::read_le32(source + 0x44);
		record.flags = io::read_le32(source + 0x70);
		record.mip_levels = io::read_le32(source + 0x74);
		record.bytes_per_pixel = io::read_le32(source);
		for (std::uint32_t channel = 0; channel < 5; ++channel)
		{
			record.channel_masks[channel] =
				io::read_le32(source + 4 + channel * 12);
		}
		record.pixel_offset = io::read_le32(source + 0xe0);
		if (record.width == 0 || record.height == 0
			|| (record.bytes_per_pixel != 1
				&& record.bytes_per_pixel != 2
				&& record.bytes_per_pixel != 4)
			|| (record.flags & 0x08) != 0
			|| !calculate_pixel_bytes(
				record.width,
				record.height,
				record.mip_levels,
				(record.flags & 0x02) != 0,
				record.bytes_per_pixel,
				record.pixel_bytes)
			|| record.pixel_offset != expected_pixel_offset
			|| !io::range_fits(
				pixel_end, record.pixel_offset, record.pixel_bytes))
		{
			cache = {};
			return false;
		}
		expected_pixel_offset += record.pixel_bytes;
	}
	if (expected_pixel_offset != pixel_end)
	{
		cache = {};
		return false;
	}
	cache.record_count = count;
	return true;
}

const TextureCacheRecord* texture_cache_find(
	const TextureCache& cache,
	const char* basename)
{
	if (basename == nullptr)
	{
		return nullptr;
	}
	for (std::uint32_t index = 0; index < cache.record_count; ++index)
	{
		if (ascii_equal(cache.records[index].name, basename))
		{
			return &cache.records[index];
		}
	}
	return nullptr;
}

bool texture_cache_decode(
	const TextureCache& cache,
	const char* basename,
	TextureImage& image)
{
	image = {};
	const TextureCacheRecord* record =
		texture_cache_find(cache, basename);
	std::uint64_t rgba_pixels = 0;
	std::uint32_t level_width =
		record == nullptr ? 0 : record->width;
	std::uint32_t level_height =
		record == nullptr ? 0 : record->height;
	const std::uint32_t level_count =
		record == nullptr
			? 0
			: ((record->flags & 0x02) != 0
				? record->mip_levels
				: 1);
	const bool native_rgb565 =
		record != nullptr
		&& record->bytes_per_pixel == 2
		&& record->channel_masks[0] == 0
		&& record->channel_masks[1] == 0xf800u
		&& record->channel_masks[2] == 0x07e0u
		&& record->channel_masks[3] == 0x001fu
		&& record->channel_masks[4] == 0;
	for (std::uint32_t level = 0;
		level < level_count;
		++level)
	{
		rgba_pixels +=
			static_cast<std::uint64_t>(level_width) * level_height;
		level_width = std::max(level_width / 2, 1u);
		level_height = std::max(level_height / 2, 1u);
	}
	if (record == nullptr
		|| rgba_pixels
			> SIZE_MAX / (native_rgb565 ? 2u : 4u)
		|| !image.pixels.allocate(
			static_cast<std::size_t>(rgba_pixels)
				* (native_rgb565 ? 2u : 4u)))
	{
		return false;
	}

	const std::uint8_t* source = cache.file.data + record->pixel_offset;
	if (native_rgb565)
	{
		std::memcpy(
			image.pixels.data,
			source,
			image.pixels.size);
		image.width = record->width;
		image.height = record->height;
		image.mip_levels = level_count;
		image.pixel_format = TexturePixelFormat::rgb565;
		return true;
	}
	std::uint8_t* output = image.pixels.data;
	level_width = record->width;
	level_height = record->height;
	for (std::uint32_t level = 0;
		level < level_count;
		++level)
	{
		const std::size_t count =
			static_cast<std::size_t>(level_width) * level_height;
		for (std::size_t index = 0; index < count; ++index)
		{
			std::uint32_t packed = 0;
			std::memcpy(
				&packed,
				source + index * record->bytes_per_pixel,
				record->bytes_per_pixel);
			if (record->channel_masks[0] != 0)
			{
				const std::uint32_t palette_index =
					channel_value(packed, record->channel_masks[0]);
				std::memcpy(
					output, cache.palette + palette_index * 4, 4);
				if (record->channel_masks[4] != 0)
				{
					output[3] = static_cast<std::uint8_t>(
						channel_value(
							packed, record->channel_masks[4]));
				}
			}
			else
			{
				output[0] = static_cast<std::uint8_t>(
					channel_value(packed, record->channel_masks[1]));
				output[1] = static_cast<std::uint8_t>(
					channel_value(packed, record->channel_masks[2]));
				output[2] = static_cast<std::uint8_t>(
					channel_value(packed, record->channel_masks[3]));
				output[3] = record->channel_masks[4] == 0
					? 255
					: static_cast<std::uint8_t>(
						channel_value(
							packed, record->channel_masks[4]));
			}
			output += 4;
		}
		source += count * record->bytes_per_pixel;
		level_width = std::max(level_width / 2, 1u);
		level_height = std::max(level_height / 2, 1u);
	}
	image.width = record->width;
	image.height = record->height;
	image.mip_levels = level_count;
	return true;
}
}
