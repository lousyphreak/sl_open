#include "assets/gun_stats.hpp"

#include "assets/refpack.hpp"
#include "core/blob.hpp"
#include "io/vfs.hpp"

#include <cmath>
#include <cstring>

namespace sl_open::assets
{
namespace
{
constexpr std::size_t kRecordBytes = 0x160;
constexpr std::size_t kNameBytes = 0x40;

float read_float(const std::uint8_t* source)
{
	float value;
	std::memcpy(&value, source, sizeof(value));
	return value;
}
}

bool gun_stats_load(
	io::Vfs& vfs,
	const char* path,
	GunStatsTable& table)
{
	table = {};
	sl_open::Blob stored;
	sl_open::Blob data;
	if (!io::vfs_read_all(vfs, path, stored)
		|| !unwrap_refpack(static_cast<sl_open::Blob&&>(stored), data)
		|| data.size != kGunStatsCount * kRecordBytes)
	{
		return false;
	}

	for (std::size_t index = 0; index < kGunStatsCount; ++index)
	{
		const std::uint8_t* source = data.data + index * kRecordBytes;
		const void* terminator = std::memchr(source, '\0', kNameBytes);
		if (terminator == nullptr)
		{
			table = {};
			return false;
		}
		GunStats& record = table.records[index];
		const std::size_t name_length =
			static_cast<const std::uint8_t*>(terminator) - source;
		std::memcpy(record.name, source, name_length);

		const float lifetime = read_float(source + 0x40);
		const float speed = read_float(source + 0x44);
		const float shield_damage = read_float(source + 0x48);
		const float hull_damage = read_float(source + 0x4c);
		const float shots_per_100_ticks = read_float(source + 0x50);
		const float power_cost = read_float(source + 0x54);
		if (!std::isfinite(lifetime)
			|| !std::isfinite(speed)
			|| !std::isfinite(shield_damage)
			|| !std::isfinite(hull_damage)
			|| !std::isfinite(shots_per_100_ticks)
			|| !std::isfinite(power_cost)
			|| shots_per_100_ticks == 0.0f)
		{
			table = {};
			return false;
		}
		for (std::size_t byte = 0x58; byte < kRecordBytes; ++byte)
		{
			if (source[byte] != 0)
			{
				table = {};
				return false;
			}
		}

		record.lifetime_ticks =
			static_cast<std::int32_t>(lifetime);
		record.speed = speed;
		record.shield_damage = shield_damage;
		record.hull_damage = hull_damage;
		record.cooldown_ticks = static_cast<std::int32_t>(
			100.0f / shots_per_100_ticks);
		record.power_cost =
			static_cast<std::int32_t>(power_cost);
	}
	table.ready = true;
	return true;
}
}
