#include "mission/dte.hpp"

#include "assets/refpack.hpp"
#include "io/endian.hpp"
#include "io/vfs.hpp"

#include <utility>

namespace sl_open::mission
{
namespace
{
constexpr std::uint32_t kUnusedOffset = 0x0000ffff;

constexpr std::uint16_t kSectionCapacities[kDteSectionCount] = {
	0xffff, 0xfffc, 0x0100, 0x0200, 0x0100, 0x0400, 0x8000,
	0x0380, 0x0100, 0x0100, 0xfffc, 0x0001, 0x0080, 0x0300,
	0x0400, 0x0200, 0x0100, 0x0100, 0x8000, 0x0100, 0x0a00,
	0x0000, 0xfffc, 0x0200, 0x0100, 0x0100, 0xfffc,
};

constexpr std::uint8_t kSectionStrides[kDteSectionCount] = {
	1, 2, 0x0c, 0x4c, 0x14, 0x30, 2, 8, 0x1c,
	0, 1, 4, 0x0c, 0x0c, 8, 0x10, 0x44, 0x1c,
	2, 0x0c, 0x0c, 0, 2, 2, 0, 0, 0x0c,
};

bool parse_directory(DteFile& file)
{
	if (file.image.size < kDteHeaderBytes)
	{
		file.error = DteLoadError::too_small;
		return false;
	}
	if (file.image.size > kMaxDteBytes)
	{
		file.error = DteLoadError::too_large;
		return false;
	}

	std::uint32_t offsets[kDteSectionCount + 1]{};
	for (std::size_t index = 0; index <= kDteSectionCount; ++index)
	{
		const std::uint8_t* slot = file.image.data + index * 8;
		const std::uint32_t count_flags = io::read_le32(slot);
		const std::uint32_t raw_offset = io::read_le32(slot + 4);
		const std::uint16_t count =
			static_cast<std::uint16_t>(count_flags);
		if (raw_offset == kUnusedOffset)
		{
			if (count != 0)
			{
				file.error = DteLoadError::invalid_directory;
				return false;
			}
			offsets[index] = static_cast<std::uint32_t>(file.image.size);
		}
		else
		{
			offsets[index] = raw_offset;
		}
		if (index == kDteSectionCount)
		{
			continue;
		}

		DteSection& section = file.sections[index];
		section.count_flags = count_flags;
		section.offset = offsets[index];
		section.count = count;
		if (section.count > kSectionCapacities[index])
		{
			file.error = DteLoadError::section_capacity_exceeded;
			return false;
		}
	}

	if (offsets[0] != kDteHeaderBytes)
	{
		file.error = DteLoadError::invalid_directory;
		return false;
	}
	for (std::size_t index = 0; index < kDteSectionCount; ++index)
	{
		if (offsets[index] > offsets[index + 1]
			|| offsets[index + 1] > file.image.size)
		{
			file.error = DteLoadError::invalid_directory;
			return false;
		}
		file.sections[index].bytes =
			offsets[index + 1] - offsets[index];
		const std::size_t stride = kSectionStrides[index];
		if (stride != 0
			&& static_cast<std::size_t>(file.sections[index].count) * stride
				> file.sections[index].bytes)
		{
			file.error = DteLoadError::invalid_directory;
			return false;
		}
	}

	for (std::size_t index = kDteSectionCount + 1;
		index < kDteHeaderSlots;
		++index)
	{
		const std::uint8_t* slot = file.image.data + index * 8;
		if (static_cast<std::uint16_t>(io::read_le32(slot)) != 0
			|| io::read_le32(slot + 4) != kUnusedOffset)
		{
			file.error = DteLoadError::invalid_directory;
			return false;
		}
	}
	return true;
}
}

bool dte_load(
	io::Vfs& vfs,
	const char* path,
	DteFile& file)
{
	file = {};
	sl_open::Blob stored;
	if (!io::vfs_read_all(vfs, path, stored))
	{
		file.error = DteLoadError::not_found;
		return false;
	}
	if (!assets::unwrap_refpack(
			static_cast<sl_open::Blob&&>(stored), file.image))
	{
		file.error = DteLoadError::decompression_failed;
		return false;
	}
	if (!parse_directory(file))
	{
		file.image.reset();
		return false;
	}
	return true;
}

const std::uint8_t* dte_section_data(
	const DteFile& file,
	std::size_t section)
{
	if (section >= kDteSectionCount || file.image.data == nullptr)
	{
		return nullptr;
	}
	return file.image.data + file.sections[section].offset;
}

const char* dte_load_error_text(DteLoadError error)
{
	switch (error)
	{
	case DteLoadError::none: return "none";
	case DteLoadError::not_found: return "not found";
	case DteLoadError::decompression_failed: return "RefPack decode failed";
	case DteLoadError::too_small: return "shorter than DTE header";
	case DteLoadError::too_large: return "larger than retail mission limit";
	case DteLoadError::invalid_directory: return "invalid DTE directory";
	case DteLoadError::section_capacity_exceeded:
		return "DTE section capacity exceeded";
	}
	return "unknown DTE error";
}
}
