#include "assets/missile_stats.hpp"

#include "assets/refpack.hpp"
#include "core/blob.hpp"
#include "io/vfs.hpp"

#include <cmath>
#include <cstring>
#include <iterator>

namespace sl_open::assets
{
namespace
{
constexpr std::size_t kRecordBytes = 0x160;
constexpr std::size_t kNameBytes = 0x40;

// LANCER.EXE 0x005037a0 + type*0x28, gameplay-definition field +0x18.
// missilestats.bin overwrites the numeric fields but leaves this compiled
// behavior selector intact.
constexpr std::uint8_t kBehavior[kMissileStatsCount] = {
	2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11,
};

float read_float(const std::uint8_t* source)
{
	float value;
	std::memcpy(&value, source, sizeof(value));
	return value;
}
}

bool missile_stats_load(
	io::Vfs& vfs,
	const char* path,
	MissileStatsTable& table)
{
	table = {};
	sl_open::Blob stored;
	sl_open::Blob data;
	if (!io::vfs_read_all(vfs, path, stored)
		|| !unwrap_refpack(static_cast<sl_open::Blob&&>(stored), data)
		|| data.size < kMissileStatsCount * kRecordBytes
		|| data.size % kRecordBytes != 0)
	{
		return false;
	}

	for (std::size_t index = 0; index < kMissileStatsCount; ++index)
	{
		const std::uint8_t* source = data.data + index * kRecordBytes;
		const void* terminator = std::memchr(source, '\0', kNameBytes);
		if (terminator == nullptr)
		{
			table = {};
			return false;
		}
		MissileStats& record = table.records[index];
		const std::size_t name_length =
			static_cast<const std::uint8_t*>(terminator) - source;
		std::memcpy(record.name, source, name_length);

		float values[9];
		for (std::size_t field = 0; field < std::size(values); ++field)
		{
			values[field] = read_float(source + 0x40 + field * 4);
			if (!std::isfinite(values[field]))
			{
				table = {};
				return false;
			}
		}
		for (std::size_t byte = 0x64; byte < kRecordBytes; ++byte)
		{
			if (source[byte] != 0)
			{
				table = {};
				return false;
			}
		}

		record.speed = values[0];
		record.steering_factor = values[1];
		record.lifetime_ticks =
			static_cast<std::int32_t>(values[2] * 100.0f);
		record.shield_damage = values[3];
		record.hull_damage = values[4];
		record.lock_ticks = static_cast<std::int32_t>(values[5]);
		record.chaff_diversion_percent =
			static_cast<std::int32_t>(values[6]);
		record.lock_range = values[7];
		record.subsystem_damage = values[8];
		record.behavior = kBehavior[index];
	}
	table.ready = true;
	return true;
}
}
