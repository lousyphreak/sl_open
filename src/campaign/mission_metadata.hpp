#pragma once

#include "io/vfs.hpp"
#include "mission/dte.hpp"

#include <cstddef>
#include <cstdint>

namespace sl_open::campaign
{
constexpr std::size_t kMissionSectionCount = mission::kDteSectionCount;

struct MissionSectionMetadata
{
	std::uint16_t count{};
	std::uint32_t bytes{};
};

struct MissionMetadata
{
	char path[64]{};
	std::uint64_t file_size{};
	MissionSectionMetadata sections[kMissionSectionCount]{};
	std::uint32_t total_records{};
	std::uint8_t nonempty_sections{};
	bool loaded{};
	bool valid{};
};

bool campaign_mission_metadata_load(
	io::Vfs& vfs,
	std::uint16_t mission,
	bool alternate_mission_25,
	MissionMetadata& metadata);
}
