#include "assets/pilot_stats.hpp"

#include "assets/refpack.hpp"
#include "core/blob.hpp"
#include "io/endian.hpp"
#include "io/vfs.hpp"

namespace sl_open::assets
{
namespace
{
constexpr std::size_t kDiskRecordBytes = 0x160;

void install_defaults(PilotRuntimeStats& output)
{
	output = {};
	output.control_gain = 0.8f;
	output.control_bias = 0.2f;
	output.control_threshold = 50;
	output.aim_error_scalar = 3.0f;
	output.timing_0_min = 30;
	output.timing_0_max = 50;
	output.timing_1_min = 400;
	output.timing_1_max = 800;
	output.timing_2_min = 200;
	output.timing_2_max = 400;
	output.behavior_1c = 1;
	output.behavior_1e = 1;
	output.behavior_20 = 1;
	output.behavior_22 = 1;
}

void apply_timing_profile(
	PilotRuntimeStats& output,
	std::int32_t profile)
{
	switch (profile)
	{
	case 0:
		output.timing_0_min = 10;
		output.timing_0_max = 40;
		output.timing_1_min = 800;
		output.timing_1_max = 1600;
		output.timing_2_min = 400;
		output.timing_2_max = 800;
		break;
	case 1:
		output.timing_0_min = 30;
		output.timing_0_max = 50;
		output.timing_1_min = 400;
		output.timing_1_max = 800;
		output.timing_2_min = 300;
		output.timing_2_max = 600;
		break;
	case 2:
		output.timing_0_min = 100;
		output.timing_0_max = 100;
		output.timing_1_min = 200;
		output.timing_1_max = 400;
		output.timing_2_min = 200;
		output.timing_2_max = 400;
		break;
	default:
		break;
	}
}

void apply_aim_profile(
	PilotRuntimeStats& output,
	std::int32_t profile)
{
	switch (profile)
	{
	case 0: output.aim_error_scalar = 5.0f; break;
	case 1: output.aim_error_scalar = 3.0f; break;
	case 2: output.aim_error_scalar = 1.5f; break;
	default: break;
	}
}

void apply_control_profile(
	PilotRuntimeStats& output,
	std::int32_t profile)
{
	switch (profile)
	{
	case 0:
		output.control_gain = 0.6f;
		output.control_bias = 0.4f;
		output.control_threshold = 100;
		break;
	case 1:
		output.control_gain = 0.8f;
		output.control_bias = 0.2f;
		output.control_threshold = 50;
		break;
	case 2:
		output.control_gain = 1.0f;
		output.control_bias = 0.0f;
		output.control_threshold = 25;
		output.timing_2_min = 50;
		output.timing_2_max = 100;
		break;
	default:
		break;
	}
}

std::int32_t read_i32(const std::uint8_t* source)
{
	return static_cast<std::int32_t>(io::read_le32(source));
}
}

bool pilot_stats_load(
	io::Vfs& vfs,
	const char* path,
	PilotStatsTable& table)
{
	table = {};
	for (PilotRuntimeStats& record : table.records)
	{
		install_defaults(record);
	}

	sl_open::Blob stored;
	sl_open::Blob data;
	if (!io::vfs_read_all(vfs, path, stored)
		|| !unwrap_refpack(static_cast<sl_open::Blob&&>(stored), data))
	{
		return false;
	}
	const std::size_t complete_records = data.size / kDiskRecordBytes;
	if (complete_records > kPilotStatsCount)
	{
		table = {};
		return false;
	}
	table.disk_record_count =
		static_cast<std::uint16_t>(complete_records);
	for (std::size_t index = 0; index < complete_records; ++index)
	{
		const std::uint8_t* source =
			data.data + index * kDiskRecordBytes;
		PilotRuntimeStats& output = table.records[index];
		apply_timing_profile(output, read_i32(source + 0x40));
		apply_aim_profile(output, read_i32(source + 0x44));
		apply_control_profile(output, read_i32(source + 0x48));
		output.behavior_22 =
			static_cast<std::int16_t>(read_i32(source + 0x4c));
		output.behavior_1e =
			static_cast<std::int16_t>(read_i32(source + 0x50));
		output.behavior_1c =
			static_cast<std::int16_t>(read_i32(source + 0x54));
		output.behavior_20 =
			static_cast<std::int16_t>(read_i32(source + 0x58));
	}
	table.ready = true;
	return true;
}
}
