#pragma once

#include <cstddef>
#include <cstdint>

namespace sl_open::io
{
inline std::uint16_t read_le16(const std::uint8_t* bytes)
{
	return static_cast<std::uint16_t>(bytes[0])
		| static_cast<std::uint16_t>(bytes[1]) << 8;
}

inline std::uint32_t read_le32(const std::uint8_t* bytes)
{
	return static_cast<std::uint32_t>(bytes[0])
		| static_cast<std::uint32_t>(bytes[1]) << 8
		| static_cast<std::uint32_t>(bytes[2]) << 16
		| static_cast<std::uint32_t>(bytes[3]) << 24;
}

inline std::uint64_t read_le64(const std::uint8_t* bytes)
{
	return static_cast<std::uint64_t>(read_le32(bytes))
		| static_cast<std::uint64_t>(read_le32(bytes + 4)) << 32;
}

inline std::uint16_t read_be16(const std::uint8_t* bytes)
{
	return static_cast<std::uint16_t>(bytes[0]) << 8
		| static_cast<std::uint16_t>(bytes[1]);
}

inline std::uint32_t read_be24(const std::uint8_t* bytes)
{
	return static_cast<std::uint32_t>(bytes[0]) << 16
		| static_cast<std::uint32_t>(bytes[1]) << 8
		| static_cast<std::uint32_t>(bytes[2]);
}

inline std::uint32_t read_be32(const std::uint8_t* bytes)
{
	return static_cast<std::uint32_t>(bytes[0]) << 24
		| static_cast<std::uint32_t>(bytes[1]) << 16
		| static_cast<std::uint32_t>(bytes[2]) << 8
		| static_cast<std::uint32_t>(bytes[3]);
}

inline bool range_fits(std::size_t total, std::size_t offset, std::size_t length)
{
	return offset <= total && length <= total - offset;
}
}
