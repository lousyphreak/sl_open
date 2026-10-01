#include "assets/image.hpp"

#include "assets/refpack.hpp"
#include "io/endian.hpp"
#include "io/vfs.hpp"

#include <cstring>

namespace sl_open::assets
{
namespace
{
bool write_tga_pixel(
	const std::uint8_t* source,
	std::uint8_t source_bytes,
	const std::uint8_t* palette,
	std::uint32_t palette_length,
	std::uint8_t* destination)
{
	if (source_bytes == 1)
	{
		const std::uint32_t index = source[0];
		if (palette == nullptr || index >= palette_length)
		{
			destination[0] = index;
			destination[1] = index;
			destination[2] = index;
			destination[3] = 255;
			return palette == nullptr;
		}
		const std::uint8_t* color = palette + index * 3;
		destination[0] = color[2];
		destination[1] = color[1];
		destination[2] = color[0];
		destination[3] = 255;
		return true;
	}
	if (source_bytes == 2)
	{
		const std::uint16_t packed = sl_open::io::read_le16(source);
		destination[0] = static_cast<std::uint8_t>(((packed >> 10) & 31) * 255 / 31);
		destination[1] = static_cast<std::uint8_t>(((packed >> 5) & 31) * 255 / 31);
		destination[2] = static_cast<std::uint8_t>((packed & 31) * 255 / 31);
		destination[3] = (packed & 0x8000) != 0 ? 255 : 0;
		return true;
	}
	if (source_bytes == 3 || source_bytes == 4)
	{
		destination[0] = source[2];
		destination[1] = source[1];
		destination[2] = source[0];
		destination[3] = source_bytes == 4 ? source[3] : 255;
		return true;
	}
	return false;
}
}

bool parse_tga(sl_open::Blob stored, TextureImage& image)
{
	image = {};
	sl_open::Blob file;
	if (!unwrap_refpack(static_cast<sl_open::Blob&&>(stored), file)
		|| file.size < 18)
	{
		return false;
	}

	const std::uint8_t id_length = file.data[0];
	const std::uint8_t color_map_type = file.data[1];
	const std::uint8_t image_type = file.data[2];
	const std::uint32_t color_map_length = sl_open::io::read_le16(file.data + 5);
	const std::uint8_t color_map_depth = file.data[7];
	const std::uint32_t width = sl_open::io::read_le16(file.data + 12);
	const std::uint32_t height = sl_open::io::read_le16(file.data + 14);
	const std::uint8_t pixel_depth = file.data[16];
	const std::uint8_t descriptor = file.data[17];
	const std::uint8_t source_bytes = pixel_depth / 8;
	const std::size_t palette_bytes = static_cast<std::size_t>(color_map_length)
		* (color_map_depth / 8);
	const std::size_t image_offset = 18 + id_length + palette_bytes;
	const bool compressed = image_type == 9 || image_type == 10 || image_type == 11;
	const bool supported_type = image_type == 1 || image_type == 2 || image_type == 3
		|| compressed;

	if (!supported_type || width == 0 || height == 0
		|| source_bytes == 0 || source_bytes > 4
		|| !sl_open::io::range_fits(file.size, 18 + id_length, palette_bytes)
		|| image_offset > file.size
		|| static_cast<std::uint64_t>(width) * height > SIZE_MAX / 4
		|| !image.pixels.allocate(static_cast<std::size_t>(width) * height * 4))
	{
		return false;
	}

	const std::uint8_t* palette = nullptr;
	if (color_map_type != 0)
	{
		if (color_map_depth != 24)
		{
			image = {};
			return false;
		}
		palette = file.data + 18 + id_length;
	}

	std::size_t source = image_offset;
	std::uint32_t file_x = 0;
	std::uint32_t file_y = 0;
	auto emit = [&](const std::uint8_t* pixel) {
		const std::uint32_t x = (descriptor & 0x10) != 0
			? width - 1 - file_x
			: file_x;
		const std::uint32_t y = (descriptor & 0x20) != 0
			? file_y
			: height - 1 - file_y;
		if (!write_tga_pixel(
			pixel,
			source_bytes,
			palette,
			color_map_length,
			image.pixels.data + (static_cast<std::size_t>(y) * width + x) * 4))
		{
			return false;
		}
		++file_x;
		if (file_x == width)
		{
			file_x = 0;
			++file_y;
		}
		return true;
	};

	const std::uint64_t total_pixels = static_cast<std::uint64_t>(width) * height;
	std::uint64_t written = 0;
	while (written < total_pixels)
	{
		std::uint32_t count = 1;
		bool repeated = false;
		if (compressed)
		{
			if (source >= file.size)
			{
				image = {};
				return false;
			}
			const std::uint8_t packet = file.data[source++];
			count = (packet & 0x7f) + 1;
			repeated = (packet & 0x80) != 0;
			if (count > width - file_x || count > total_pixels - written)
			{
				image = {};
				return false;
			}
		}

		if (repeated)
		{
			if (!sl_open::io::range_fits(file.size, source, source_bytes))
			{
				image = {};
				return false;
			}
			const std::uint8_t* pixel = file.data + source;
			source += source_bytes;
			for (std::uint32_t index = 0; index < count; ++index)
			{
				if (!emit(pixel))
				{
					image = {};
					return false;
				}
			}
		}
		else
		{
			const std::size_t bytes = static_cast<std::size_t>(count) * source_bytes;
			if (!sl_open::io::range_fits(file.size, source, bytes))
			{
				image = {};
				return false;
			}
			for (std::uint32_t index = 0; index < count; ++index)
			{
				if (!emit(file.data + source))
				{
					image = {};
					return false;
				}
				source += source_bytes;
			}
		}
		written += count;
	}

	image.width = width;
	image.height = height;
	return true;
}

bool load_tga(sl_open::io::Vfs& vfs, const char* path, TextureImage& image)
{
	sl_open::Blob stored;
	return sl_open::io::vfs_read_all(vfs, path, stored)
		&& parse_tga(static_cast<sl_open::Blob&&>(stored), image);
}

bool parse_tga_palette(
	sl_open::Blob stored,
	std::uint8_t (&rgba)[256 * 4])
{
	sl_open::Blob file;
	if (!unwrap_refpack(static_cast<sl_open::Blob&&>(stored), file)
		|| file.size < 18)
	{
		return false;
	}
	const std::uint8_t id_length = file.data[0];
	const std::uint32_t color_map_length = sl_open::io::read_le16(file.data + 5);
	const std::uint8_t color_map_depth = file.data[7];
	const std::size_t palette_offset = 18 + id_length;
	if (file.data[1] == 0
		|| color_map_length != 256
		|| color_map_depth != 24
		|| !sl_open::io::range_fits(file.size, palette_offset, 256 * 3))
	{
		return false;
	}
	for (std::uint32_t index = 0; index < 256; ++index)
	{
		const std::uint8_t* color = file.data + palette_offset + index * 3;
		rgba[index * 4] = color[2];
		rgba[index * 4 + 1] = color[1];
		rgba[index * 4 + 2] = color[0];
		rgba[index * 4 + 3] = 255;
	}
	return true;
}

bool load_tga_palette(
	sl_open::io::Vfs& vfs,
	const char* path,
	std::uint8_t (&rgba)[256 * 4])
{
	sl_open::Blob stored;
	return sl_open::io::vfs_read_all(vfs, path, stored)
		&& parse_tga_palette(static_cast<sl_open::Blob&&>(stored), rgba);
}
}
