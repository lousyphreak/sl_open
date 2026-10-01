#pragma once

#include <cstddef>
#include <cstdint>

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::assets
{
constexpr std::size_t kMissileStatsCount = 11;

struct MissileStats
{
	char name[65]{};
	float speed{};
	float steering_factor{};
	std::int32_t lifetime_ticks{};
	float shield_damage{};
	float hull_damage{};
	float subsystem_damage{};
	std::int32_t lock_ticks{};
	std::int32_t chaff_diversion_percent{};
	float lock_range{};
	std::uint8_t behavior{};
};

struct MissileStatsTable
{
	MissileStats records[kMissileStatsCount];
	bool ready{};
};

bool missile_stats_load(
	io::Vfs& vfs,
	const char* path,
	MissileStatsTable& table);
}
