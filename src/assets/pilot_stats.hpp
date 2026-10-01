#pragma once

#include <cstddef>
#include <cstdint>

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::assets
{
constexpr std::size_t kPilotStatsCount = 194;

struct PilotRuntimeStats
{
	float control_gain{};
	float control_bias{};
	std::int16_t control_threshold{};
	std::uint16_t pad_0a{};
	float aim_error_scalar{};
	std::int16_t timing_0_min{};
	std::int16_t timing_0_max{};
	std::int16_t timing_1_min{};
	std::int16_t timing_1_max{};
	std::int16_t timing_2_min{};
	std::int16_t timing_2_max{};
	std::int16_t behavior_1c{};
	std::int16_t behavior_1e{};
	std::int16_t behavior_20{};
	std::int16_t behavior_22{};
};
static_assert(sizeof(PilotRuntimeStats) == 0x24);

struct PilotStatsTable
{
	PilotRuntimeStats records[kPilotStatsCount];
	std::uint16_t disk_record_count{};
	bool ready{};
};

bool pilot_stats_load(
	io::Vfs& vfs,
	const char* path,
	PilotStatsTable& table);
}
