#include "assets/ship_stats.hpp"

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

// LANCER.EXE 0x004f9e70, 256 compiled 0x28-byte FlightStats records.
// The signed word at +0x24 selects AI_steer_toward_point's simplified
// local-axis solver at 0x00401690 instead of the normal solver at 0x00401710.
constexpr bool kSimplifiedSteering[kShipStatsCount] = {
	0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,
	1,1,1,1,1,1,1,0,0,0,0,0,0,0,1,0,
	1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,0,0,1,0,0,1,0,0,0,0,0,0,1,
	1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,
	1,1,1,0,0,0,0,1,1,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,0,0,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

// LANCER.EXE 0x004fc670, 256 records of 0x30 bytes. shipstats.bin
// overwrites only the first seven fields; these three signed words are the
// preserved compiled tail at offsets +0x26, +0x28, +0x2a, and +0x2c.
constexpr std::int16_t kNameLanguageId[kShipStatsCount] = {
	8, 3, 17, 16, 19, 15, 142, 141, 18, 21, 2, 1, 644, 4, 0, 156,
	0, 159, 1049, 1050, 155, 0, 143, 7, 14, 20, 1053, 0, 1054, 163, 149, 13,
	157, 148, 0, 0, 0, 158, 0, 1055, 5, 1062, 1061, 11, 1056, 1057, 1058, 0,
	1059, 1060, 1063, 0, 1064, 1066, 1071, 12, 1076, 0, 162, 1077, 10, 1078, 6,
	1079, 1074, 1080, 1081, 1082, 1083, 1084, 151, 1085, 1086, 9, 0, 0, 0, 1087, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 150, 0, 1089, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 152, 1091, 160,
	161, 1092, 0, 0, 0, 0, 0, 0, 1067, 0, 0, 0, 0, 0, 0, 0,
	1093, 1094, 0, 1095, 1096, 0, 0, 0, 0, 0, 0, 0, 0, 1120, 1097, 0,
	1098, 643, 1101, 1102, 1103, 0, 0, 0, 0, 1104, 1075, 1105, 1065, 1090, 1052, 1106,
	1051, 1108, 1110, 0, 0, 1111, 0, 0, 1112, 0, 0, 0, 0, 0, 0, 1113,
	1107, 0, 0, 0, 0, 0, 0, 0, 0, 1114, 0, 0, 0, 0, 0, 0,
	1115, 1116, 1070, 0, 0, 0, 0, 1117, 1118, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 1119, 0, 0, 1048, 1068, 1069, 1072, 1073, 1088,
	1099, 1100, 1109, 1121, 1122, 1123, 1124, 1125, 1126, 1127, 1128, 1129, 1130, 1131, 1132, 1133,
	0, 0, 0, 0, 8, 3, 17, 16, 19, 15, 142, 141, 18, 21, 2, 1,
};
constexpr std::int16_t kCollisionClass[kShipStatsCount] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 1, 1, 3, 1, 1,
	1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3,
	2, 1, 3, 3, 3, 3, 3, 3, 3, 3, 5, 4, 4, 3, 6, 6,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 5, 4, 2, 8,
	8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 2, 2, 4, 4, 7,
	4, 4, 2, 2, 6, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 4, 2,
	3, 4, 4, 4, 4, 4, 2, 6, 6, 3, 3, 2, 3, 3, 3, 3,
	3, 3, 3, 2, 2, 4, 2, 2, 4, 2, 2, 4, 4, 2, 6, 4,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 2, 2, 4, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 8, 8, 8, 8, 8, 8, 8,
	8, 8, 8, 8, 4, 4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 3,
	3, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
constexpr std::int16_t kAllegianceClass[kShipStatsCount] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 0, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1,
	2, 0, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 2,
	1, 1, 2, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 2,
	1, 0, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0,
	0, 1, 1, 2, 2, 1, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 0, 2, 2, 2,
	1, 1, 1, 2, 2, 2, 2, 0, 0, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 0, 1, 2, 0, 1, 1, 1, 1, 0,
	1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
constexpr std::int16_t kObjectClass[kShipStatsCount] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1,
	0, 1, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0,
	1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1,
	1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1,
	1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 1,
	1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
// LANCER.EXE 0x004fc670, compiled type descriptor word +0x24. The retail
// whole-object targetability setter admits a request only when this word is
// nonzero.
constexpr bool kTargetableCapability[kShipStatsCount] = {
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,
	0,1,1,1,1,0,1,1,1,1,1,0,1,1,1,1,
	1,1,0,0,0,1,0,1,1,1,1,1,1,1,1,0,
	1,1,1,0,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,0,0,0,1,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,
	1,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
	1,1,0,1,1,0,0,0,0,0,0,0,0,1,1,0,
	1,1,1,1,1,1,0,0,0,1,1,1,1,0,1,1,
	1,1,1,0,0,1,0,0,1,0,0,0,0,0,0,1,
	1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,
	1,1,1,0,0,0,0,1,1,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,0,0,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,
};

float read_float(const std::uint8_t* bytes)
{
	float value;
	std::memcpy(&value, bytes, sizeof(value));
	return value;
}
}

bool ship_stats_load(
	io::Vfs& vfs,
	const char* path,
	ShipStatsTable& table)
{
	table = {};
	sl_open::Blob stored;
	sl_open::Blob data;
	if (!io::vfs_read_all(vfs, path, stored)
		|| !unwrap_refpack(static_cast<sl_open::Blob&&>(stored), data)
		|| data.size != kShipStatsCount * kRecordBytes)
	{
		return false;
	}
	for (std::size_t index = 0; index < kShipStatsCount; ++index)
	{
		const std::uint8_t* source = data.data + index * kRecordBytes;
		const void* terminator = std::memchr(source, '\0', kNameBytes);
		if (terminator == nullptr)
		{
			table = {};
			return false;
		}
		const std::size_t name_length =
			static_cast<const std::uint8_t*>(terminator) - source;
		ShipStatsRecord& record = table.records[index];
		std::memcpy(record.name, source, name_length);
		record.name[name_length] = '\0';

		float values[15];
		for (std::size_t field = 0; field < 15; ++field)
		{
			values[field] = read_float(source + 0x40 + field * 4);
			if (!std::isfinite(values[field]))
			{
				table = {};
				return false;
			}
		}
		for (std::size_t byte = 0x7c; byte < kRecordBytes; ++byte)
		{
			if (source[byte] != 0)
			{
				table = {};
				return false;
			}
		}

		record.flight.max_speed = values[0];
		record.flight.roll_rate = values[6];
		record.flight.pitch_rate = values[4];
		record.flight.yaw_rate = values[2];
		record.flight.linear_retention = values[1];
		record.flight.roll_retention = values[7];
		record.flight.pitch_retention = values[5];
		record.flight.yaw_retention = values[3];
		record.flight.speed_pitch_ratio =
			values[0] / values[4];
		record.flight.simplified_steering =
			kSimplifiedSteering[index];
		record.object.primary_bank_max =
			static_cast<std::int32_t>(values[8]);
		record.object.structural_bank_max =
			static_cast<std::int32_t>(values[9]);
		record.object.afterburner_seconds =
			static_cast<std::int32_t>(values[10]);
		record.object.primary_recharge_time =
			values[11] == 0.0f ? 10.0f : values[11];
		record.object.gun_energy_max = values[12];
		record.object.gun_recharge_time = values[13];
		record.object.ammunition =
			static_cast<std::int32_t>(values[14]);
		record.object.name_language_id = kNameLanguageId[index];
		record.object.collision_class = kCollisionClass[index];
		record.object.allegiance_class = kAllegianceClass[index];
		record.object.object_class = kObjectClass[index];
		record.object.targetable_capability =
			kTargetableCapability[index];
	}
	table.ready = true;
	return true;
}
}
