#pragma once

#include "core/blob.hpp"

#include <cstdint>
#include <vector>

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::hud
{
constexpr std::uint32_t kMovieMaximumPixels = 120 * 100;
constexpr std::uint32_t kMovieMaximumOperations = 1024;

struct MovieAsset
{
	char path[128]{};
	sl_open::Blob stream;
};

struct Movie
{
	const MovieAsset* asset{};
	const MovieAsset* static_asset{};
	std::vector<std::uint8_t> chunk;
	std::uint8_t frames[2][kMovieMaximumPixels]{};
	std::uint8_t palette[256 * 3]{};
	std::uint8_t rgba[kMovieMaximumPixels * 4]{};
	std::int32_t motion_offsets[kMovieMaximumOperations]{};
	std::uint8_t operations[kMovieMaximumOperations][16]{};
	std::uint64_t next_frame_units{};
	std::uint32_t offset{};
	std::uint32_t frame_count{};
	std::uint16_t width{};
	std::uint16_t height{};
	std::uint8_t current_frame{};
	std::uint8_t flags{};
	bool active{};
	bool ready{};
	bool new_frame{};
};

bool movie_asset_load(
	io::Vfs& vfs,
	const char* basename,
	MovieAsset& asset);
bool movie_asset_matches(const MovieAsset& asset, const char* basename);
bool movie_play(
	Movie& movie,
	const MovieAsset& asset,
	const MovieAsset* static_asset,
	std::uint8_t flags,
	std::uint64_t now);
void movie_update(Movie& movie, std::uint64_t now);
void movie_stop(Movie& movie);
}
