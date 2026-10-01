#include "campaign/mission_metadata.hpp"

#include "mission/dte.hpp"

#include <cstdio>

namespace sl_open::campaign
{
bool campaign_mission_metadata_load(
	io::Vfs& vfs,
	std::uint16_t mission,
	bool alternate_mission_25,
	MissionMetadata& metadata)
{
	metadata = {};
	const unsigned file_mission =
		mission == 25 && alternate_mission_25 ? 251 : mission;
	std::snprintf(
		metadata.path,
		sizeof(metadata.path),
		"missions/mission%u.dte",
		file_mission);

	mission::DteFile file;
	if (!mission::dte_load(vfs, metadata.path, file))
	{
		return false;
	}
	metadata.loaded = true;
	metadata.valid = true;
	metadata.file_size = file.image.size;
	for (std::size_t index = 0; index < kMissionSectionCount; ++index)
	{
		metadata.sections[index].count = file.sections[index].count;
		metadata.sections[index].bytes = file.sections[index].bytes;
		metadata.total_records += file.sections[index].count;
		if (file.sections[index].count != 0)
		{
			++metadata.nonempty_sections;
		}
	}
	return true;
}
}
