#pragma once

#include <cstddef>
#include <cstdint>

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::assets
{
constexpr std::size_t kGunStatsCount = 15;

struct GunStats
{
	char name[65]{};
	std::int32_t lifetime_ticks{};
	float speed{};
	float shield_damage{};
	float hull_damage{};
	std::int32_t cooldown_ticks{};
	std::int32_t power_cost{};
};

struct GunStatsTable
{
	GunStats records[kGunStatsCount];
	bool ready{};
};

bool gun_stats_load(
	io::Vfs& vfs,
	const char* path,
	GunStatsTable& table);
}
