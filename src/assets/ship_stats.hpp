#pragma once

#include <cstddef>
#include <cstdint>

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::assets
{
constexpr std::size_t kShipStatsCount = 256;

struct FlightStats
{
	float max_speed{};
	float roll_rate{};
	float pitch_rate{};
	float yaw_rate{};
	float linear_retention{};
	float roll_retention{};
	float pitch_retention{};
	float yaw_retention{};
	float speed_pitch_ratio{};
	bool simplified_steering{};
};

struct ObjectTypeStats
{
	std::int32_t primary_bank_max{};
	std::int32_t structural_bank_max{};
	std::int32_t afterburner_seconds{};
	float primary_recharge_time{};
	float gun_energy_max{};
	float gun_recharge_time{};
	std::int32_t ammunition{};
	std::int16_t name_language_id{};
	std::int16_t collision_class{};
	std::int16_t allegiance_class{};
	std::int16_t object_class{};
	bool targetable_capability{};
};

struct ShipStatsRecord
{
	char name[65]{};
	FlightStats flight;
	ObjectTypeStats object;
};

struct ShipStatsTable
{
	ShipStatsRecord records[kShipStatsCount];
	bool ready{};
};

bool ship_stats_load(
	io::Vfs& vfs,
	const char* path,
	ShipStatsTable& table);
}
