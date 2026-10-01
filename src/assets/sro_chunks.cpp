#include "assets/sro_chunks.hpp"

#include "assets/refpack.hpp"
#include "io/endian.hpp"

#include <cmath>
#include <cstring>

namespace sl_open::assets
{
namespace
{
float read_float(const std::uint8_t* bytes)
{
	const std::uint32_t bits = io::read_le32(bytes);
	float value{};
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}
}

bool sro_chunks_parse(sl_open::Blob stored, SroChunks& chunks)
{
	chunks = {};
	if (!unwrap_refpack(static_cast<sl_open::Blob&&>(stored), chunks.file))
	{
		return false;
	}

	std::size_t offset = 0;
	while (offset + 6 <= chunks.file.size)
	{
		const std::uint16_t tag = io::read_le16(chunks.file.data + offset);
		const std::uint16_t stride =
			io::read_le16(chunks.file.data + offset + 2);
		const std::uint16_t count =
			io::read_le16(chunks.file.data + offset + 4);
		const std::size_t bytes = static_cast<std::size_t>(stride) * count;
		if (!io::range_fits(chunks.file.size, offset + 6, bytes))
		{
			chunks = {};
			return false;
		}
		chunks.entries.push_back(
			{tag, stride, count, chunks.file.data + offset + 6});
		offset += 6 + bytes;
		if (tag == 0xffff)
		{
			if (stride == 0 && count == 0 && offset == chunks.file.size)
			{
				return true;
			}
			chunks = {};
			return false;
		}
	}
	chunks = {};
	return false;
}

const SroChunk* sro_chunk_request(
	const SroChunks& chunks,
	std::size_t& cursor,
	std::uint16_t tag)
{
	for (std::size_t index = cursor; index < chunks.entries.size(); ++index)
	{
		if (chunks.entries[index].tag == 0xffff)
		{
			return nullptr;
		}
		if (chunks.entries[index].tag == tag)
		{
			cursor = index + 1;
			return &chunks.entries[index];
		}
	}
	return nullptr;
}

bool sro_locator_decode(
	const SroChunk& chunk,
	std::uint32_t index,
	SroLocator& locator)
{
	if (chunk.stride < 0x48 || index >= chunk.count)
	{
		return false;
	}
	const std::uint8_t* record = chunk.data + index * chunk.stride;
	locator = {};
	locator.type = static_cast<std::int32_t>(io::read_le32(record));
	for (std::uint32_t axis = 0; axis < 3; ++axis)
	{
		locator.position[axis] = read_float(record + 4 + axis * 4);
	}
	for (std::uint32_t row = 0; row < 3; ++row)
	{
		for (std::uint32_t column = 0; column < 3; ++column)
		{
			locator.basis[column][row] =
				read_float(record + 0x10 + (row * 3 + column) * 4);
		}
	}
	for (std::uint32_t value = 0; value < 5; ++value)
	{
		locator.values[value] = static_cast<std::int32_t>(
			io::read_le32(record + 0x34 + value * 4));
	}
	return std::isfinite(locator.position.x)
		&& std::isfinite(locator.position.y)
		&& std::isfinite(locator.position.z)
		&& std::isfinite(locator.basis[0].x)
		&& std::isfinite(locator.basis[0].y)
		&& std::isfinite(locator.basis[0].z)
		&& std::isfinite(locator.basis[1].x)
		&& std::isfinite(locator.basis[1].y)
		&& std::isfinite(locator.basis[1].z)
		&& std::isfinite(locator.basis[2].x)
		&& std::isfinite(locator.basis[2].y)
		&& std::isfinite(locator.basis[2].z);
}
}
