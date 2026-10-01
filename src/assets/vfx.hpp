#pragma once

#include "core/blob.hpp"

#include <cstdint>

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::assets
{
struct IndexedPixel
{
	std::uint8_t index;
	std::uint8_t opacity;
};

struct IndexedImage
{
	sl_open::Blob pixels;
	std::uint32_t width{};
	std::uint32_t height{};
	std::int32_t min_x{};
	std::int32_t min_y{};
};

struct SpriteList
{
	sl_open::Blob data;
	std::uint32_t shape_count{};
};

struct Font
{
	sl_open::Blob data;
	std::uint32_t directory_count{};
	std::uint32_t height{};
};

struct GlyphView
{
	const std::uint8_t* pixels{};
	std::uint32_t width{};
	std::uint32_t height{};
};

bool parse_sprite_list(
	sl_open::Blob source,
	SpriteList& sprite,
	bool require_embedded_palette = true);
bool load_sprite_list(
	sl_open::io::Vfs& vfs,
	const char* path,
	SpriteList& sprite,
	bool require_embedded_palette = true);
bool decode_sprite_palette(
	const SpriteList& sprite,
	std::uint32_t index,
	std::uint8_t (&rgba)[256 * 4]);
bool decode_sprite_shape(
	const SpriteList& sprite,
	std::uint32_t index,
	IndexedImage& image);
bool parse_font(sl_open::Blob source, Font& font);
bool load_font(sl_open::io::Vfs& vfs, const char* path, Font& font);
bool font_glyph(const Font& font, std::uint32_t index, GlyphView& glyph);
}
