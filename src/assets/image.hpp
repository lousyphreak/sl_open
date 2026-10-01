#pragma once

#include "core/blob.hpp"

#include <cstdint>

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::assets
{
enum class TexturePixelFormat : std::uint8_t
{
	rgba8,
	rgb565,
};

struct TextureImage
{
	sl_open::Blob pixels;
	std::uint32_t width{};
	std::uint32_t height{};
	// Texture-cache images retain every authored level in largest-to-smallest
	// order. Standalone TGA and generated images have only their base level.
	std::uint32_t mip_levels{1};
	TexturePixelFormat pixel_format{TexturePixelFormat::rgba8};
};

bool parse_tga(sl_open::Blob stored, TextureImage& image);
bool load_tga(sl_open::io::Vfs& vfs, const char* path, TextureImage& image);
bool parse_tga_palette(
	sl_open::Blob stored,
	std::uint8_t (&rgba)[256 * 4]);
bool load_tga_palette(
	sl_open::io::Vfs& vfs,
	const char* path,
	std::uint8_t (&rgba)[256 * 4]);
}
