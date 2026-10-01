#pragma once

#include "core/blob.hpp"
#include "core/math.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sl_open::assets
{
struct SroChunk
{
	std::uint16_t tag{};
	std::uint16_t stride{};
	std::uint16_t count{};
	const std::uint8_t* data{};
};

struct SroChunks
{
	sl_open::Blob file;
	std::vector<SroChunk> entries;
};

struct SroLocator
{
	std::int32_t type{};
	glm::vec3 position{0.0f};
	glm::mat3 basis{1.0f};
	// Locator +0x34 is a subtype for general locators and loadout tier zero
	// for hardpoints. The following four dwords are parameters or tiers 1..4.
	std::int32_t values[5]{};
};

bool sro_chunks_parse(sl_open::Blob stored, SroChunks& chunks);
const SroChunk* sro_chunk_request(
	const SroChunks& chunks,
	std::size_t& cursor,
	std::uint16_t tag);
bool sro_locator_decode(
	const SroChunk& chunk,
	std::uint32_t index,
	SroLocator& locator);
}
