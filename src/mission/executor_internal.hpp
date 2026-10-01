#pragma once

#include "mission/executor.hpp"

namespace sl_open::mission
{
enum class CommandFlow : std::uint8_t
{
	continue_execution,
	yield,
	rewind_two_and_yield,
	rewind_four_and_yield,
};

CommandFlow executor_execute_command(
	std::uint8_t command,
	const ExecutorValue* arguments,
	ExecutorContext& context,
	Executor& executor,
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file,
	std::uint32_t script_tick,
	std::uint32_t simulation_tick);
}
