#include "assets/vfx.hpp"

#include "assets/refpack.hpp"
#include "io/endian.hpp"
#include "io/vfs.hpp"

#include <climits>
#include <cstring>

namespace sl_open::assets
{
namespace
{
std::int32_t read_i32(const std::uint8_t* bytes)
{
	return static_cast<std::int32_t>(sl_open::io::read_le32(bytes));
}

std::uint32_t sprite_offset(const SpriteList& sprite, std::uint32_t index)
{
	return sl_open::io::read_le32(sprite.data.data + 8 + index * 8);
}

std::size_t sprite_record_end(const SpriteList& sprite, std::uint32_t index)
{
	const std::uint32_t start = sprite_offset(sprite, index);
	std::uint32_t end = static_cast<std::uint32_t>(sprite.data.size);
	for (std::uint32_t other = 0; other < sprite.shape_count; ++other)
	{
		const std::uint32_t offset = sprite_offset(sprite, other);
		if (offset > start && offset < end)
		{
			end = offset;
		}
	}
	return end;
}

bool sprite_record_is_palette(
	const SpriteList& sprite,
	std::uint32_t index)
{
	const std::size_t offset = sprite_offset(sprite, index);
	const std::size_t end = sprite_record_end(sprite, index);
	if (end != offset + 768
		|| !sl_open::io::range_fits(sprite.data.size, offset, 768))
	{
		return false;
	}
	for (std::size_t byte = offset; byte < end; ++byte)
	{
		if (sprite.data.data[byte] > 63)
		{
			return false;
		}
	}
	return true;
}
}

bool parse_sprite_list(
	sl_open::Blob source,
	SpriteList& sprite,
	bool require_embedded_palette)
{
	sprite = {};
	if (!unwrap_refpack(static_cast<sl_open::Blob&&>(source), sprite.data)
		|| sprite.data.size < 16
		|| std::memcmp(sprite.data.data, "1.40", 4) != 0)
	{
		return false;
	}

	const std::uint32_t count = sl_open::io::read_le32(sprite.data.data + 4);
	// Retail SCEM resources for capital ships and large objects can contain
	// only their single base silhouette record (for example RELISCEM.SPR).
	if (count < 1 || count > 4096
		|| !sl_open::io::range_fits(sprite.data.size, 8, static_cast<std::size_t>(count) * 8))
	{
		sprite = {};
		return false;
	}

	for (std::uint32_t index = 0; index < count; ++index)
	{
		const std::uint32_t offset = sl_open::io::read_le32(
			sprite.data.data + 8 + index * 8);
		const std::uint32_t palette_offset = sl_open::io::read_le32(
			sprite.data.data + 12 + index * 8);
		if (offset >= sprite.data.size
			|| (palette_offset != 0
				&& !sl_open::io::range_fits(sprite.data.size, palette_offset, 768)))
		{
			sprite = {};
			return false;
		}
	}

	sprite.shape_count = count;
	std::uint8_t palette[256 * 4];
	bool palette_found = false;
	for (std::uint32_t index = 0; index < count; ++index)
	{
		if (decode_sprite_palette(sprite, index, palette))
		{
			palette_found = true;
			break;
		}
	}
	if (require_embedded_palette && !palette_found)
	{
		sprite = {};
		return false;
	}
	return true;
}

bool load_sprite_list(
	sl_open::io::Vfs& vfs,
	const char* path,
	SpriteList& sprite,
	bool require_embedded_palette)
{
	sl_open::Blob source;
	return sl_open::io::vfs_read_all(vfs, path, source)
		&& parse_sprite_list(
			static_cast<sl_open::Blob&&>(source),
			sprite,
			require_embedded_palette);
}

bool decode_sprite_palette(
	const SpriteList& sprite,
	std::uint32_t index,
	std::uint8_t (&rgba)[256 * 4])
{
	if (index >= sprite.shape_count)
	{
		return false;
	}

	const std::size_t palette_offset = sprite_offset(sprite, index);
	if (!sprite_record_is_palette(sprite, index))
	{
		return false;
	}

	for (std::uint32_t index = 0; index < 256; ++index)
	{
		rgba[index * 4 + 0] =
			static_cast<std::uint8_t>(sprite.data.data[palette_offset + index * 3] << 2);
		rgba[index * 4 + 1] =
			static_cast<std::uint8_t>(sprite.data.data[palette_offset + index * 3 + 1] << 2);
		rgba[index * 4 + 2] =
			static_cast<std::uint8_t>(sprite.data.data[palette_offset + index * 3 + 2] << 2);
		rgba[index * 4 + 3] = 255;
	}
	return true;
}

bool decode_sprite_shape(
	const SpriteList& sprite,
	std::uint32_t index,
	IndexedImage& image)
{
	image = {};
	if (index >= sprite.shape_count
		|| sprite_record_is_palette(sprite, index))
	{
		return false;
	}

	const std::size_t offset = sprite_offset(sprite, index);
	const std::size_t end = sprite_record_end(sprite, index);
	if (!sl_open::io::range_fits(sprite.data.size, offset, 24) || end < offset + 24)
	{
		return false;
	}
	if (end == offset + 24)
	{
		return true;
	}

	const std::uint8_t* record = sprite.data.data + offset;
	const std::int32_t min_x = read_i32(record + 8);
	const std::int32_t min_y = read_i32(record + 12);
	const std::int32_t max_x = read_i32(record + 16);
	const std::int32_t max_y = read_i32(record + 20);
	if (max_x < min_x || max_y < min_y)
	{
		return false;
	}

	const std::uint64_t width64 =
		static_cast<std::uint64_t>(static_cast<std::int64_t>(max_x) - min_x + 1);
	const std::uint64_t height64 =
		static_cast<std::uint64_t>(static_cast<std::int64_t>(max_y) - min_y + 1);
	if (width64 > UINT32_MAX || height64 > UINT32_MAX
		|| width64 * height64 > SIZE_MAX / sizeof(IndexedPixel)
		|| !image.pixels.allocate(
			static_cast<std::size_t>(width64 * height64) * sizeof(IndexedPixel)))
	{
		return false;
	}
	std::memset(image.pixels.data, 0, image.pixels.size);

	const std::uint32_t width = static_cast<std::uint32_t>(width64);
	const std::uint32_t height = static_cast<std::uint32_t>(height64);
	std::size_t command = offset + 24;
	auto* pixels = reinterpret_cast<IndexedPixel*>(image.pixels.data);
	for (std::uint32_t y = 0; y < height; ++y)
	{
		std::uint32_t x = 0;
		for (;;)
		{
			if (command >= end)
			{
				image = {};
				return false;
			}
			const std::uint8_t token = sprite.data.data[command++];
			if (token == 0)
			{
				break;
			}
			if (token == 1)
			{
				if (command >= end)
				{
					image = {};
					return false;
				}
				x += sprite.data.data[command++];
				if (x > width)
				{
					image = {};
					return false;
				}
				continue;
			}

			const std::uint32_t count = token >> 1;
			if (count > width - x)
			{
				image = {};
				return false;
			}
			if ((token & 1) == 0)
			{
				if (command >= end)
				{
					image = {};
					return false;
				}
				const std::uint8_t color = sprite.data.data[command++];
				for (std::uint32_t pixel = 0; pixel < count; ++pixel)
				{
					pixels[static_cast<std::size_t>(y) * width + x++] = {color, 255};
				}
			}
			else
			{
				if (!sl_open::io::range_fits(end, command, count))
				{
					image = {};
					return false;
				}
				for (std::uint32_t pixel = 0; pixel < count; ++pixel)
				{
					pixels[static_cast<std::size_t>(y) * width + x++] = {
						sprite.data.data[command++],
						255};
				}
			}
		}
	}

	if (command != end)
	{
		image = {};
		return false;
	}

	image.width = width;
	image.height = height;
	image.min_x = min_x;
	image.min_y = min_y;
	return true;
}

bool parse_font(sl_open::Blob source, Font& font)
{
	font = {};
	if (!unwrap_refpack(static_cast<sl_open::Blob&&>(source), font.data)
		|| font.data.size < 16)
	{
		return false;
	}

	const std::uint32_t tag = sl_open::io::read_le32(font.data.data);
	const std::uint32_t count = sl_open::io::read_le32(font.data.data + 4);
	const std::uint32_t height = sl_open::io::read_le32(font.data.data + 8);
	if ((tag != 0x00002e31 && tag != 0x00002e32)
		|| count == 0 || count > 4096 || height == 0 || height > 1024
		|| !sl_open::io::range_fits(font.data.size, 16, static_cast<std::size_t>(count) * 4))
	{
		font = {};
		return false;
	}

	font.directory_count = count;
	font.height = height;
	for (std::uint32_t index = 0; index < count; ++index)
	{
		GlyphView glyph;
		if (!font_glyph(font, index, glyph))
		{
			font = {};
			return false;
		}
	}
	return true;
}

bool load_font(sl_open::io::Vfs& vfs, const char* path, Font& font)
{
	sl_open::Blob source;
	return sl_open::io::vfs_read_all(vfs, path, source)
		&& parse_font(static_cast<sl_open::Blob&&>(source), font);
}

bool font_glyph(const Font& font, std::uint32_t index, GlyphView& glyph)
{
	glyph = {};
	if (index >= font.directory_count)
	{
		return false;
	}
	const std::uint32_t offset = sl_open::io::read_le32(font.data.data + 16 + index * 4);
	if (offset == 0)
	{
		return true;
	}
	if (!sl_open::io::range_fits(font.data.size, offset, 4))
	{
		return false;
	}

	const std::uint32_t width = sl_open::io::read_le32(font.data.data + offset);
	if (width > UINT32_MAX / font.height
		|| !sl_open::io::range_fits(
			font.data.size,
			offset + 4,
			static_cast<std::size_t>(width) * font.height))
	{
		return false;
	}
	glyph.pixels = font.data.data + offset + 4;
	glyph.width = width;
	glyph.height = font.height;
	return true;
}
}
