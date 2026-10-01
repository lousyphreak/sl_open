#pragma once

#include "core/blob.hpp"

#include <cstdint>

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::assets
{
struct TextureImage;

constexpr std::uint32_t kTextureCacheCapacity = 1000;

struct TextureCacheRecord
{
	char name[33]{};
	std::uint32_t width{};
	std::uint32_t height{};
	std::uint32_t flags{};
	std::uint32_t mip_levels{};
	std::uint32_t bytes_per_pixel{};
	std::uint32_t channel_masks[5]{};
	std::uint32_t pixel_offset{};
	std::uint32_t pixel_bytes{};
};

struct TextureCache
{
	sl_open::Blob file;
	TextureCacheRecord records[kTextureCacheCapacity]{};
	std::uint8_t palette[256 * 4]{};
	std::uint32_t record_count{};
};

bool texture_cache_load(
	io::Vfs& vfs,
	const char* cache_path,
	const char* palette_path,
	TextureCache& cache);
const TextureCacheRecord* texture_cache_find(
	const TextureCache& cache,
	const char* basename);
bool texture_cache_decode(
	const TextureCache& cache,
	const char* basename,
	TextureImage& image);
}
