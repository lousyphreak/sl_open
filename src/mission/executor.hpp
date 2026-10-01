#pragma once

#include "assets/ship_stats.hpp"
#include "game/runtime_limits.hpp"
#include "game/world.hpp"
#include "mission/dte.hpp"
#include "mission/runtime.hpp"

#include <cstdint>

namespace sl_open::mission
{
enum class ValueKind : std::uint8_t
{
	scalar,
	object,
	group,
	reference_set,
	string,
	curve,
	section19,
	null_value,
};

struct ExecutorValue
{
	std::uint32_t value{};
	ValueKind kind{ValueKind::scalar};
	std::uint8_t component{UINT8_MAX};
};

struct ExecutorContext
{
	ExecutorValue stack[game::kExecutorStackCells];
	std::uint32_t event_arguments[5]{};
	std::uint32_t pc{};
	std::uint32_t local_base{};
	std::uint32_t owner_frame{};
	std::uint32_t wait_until_tick{};
	std::uint32_t last_result{};
	ValueKind last_result_kind{ValueKind::scalar};
	std::uint8_t stack_count{};
	std::uint16_t function{};
	std::uint8_t function_table{};
	std::uint8_t dispatch_tag{UINT8_MAX};
	std::uint8_t call_depth{};
	std::uint8_t script_identity{UINT8_MAX};
	bool active{};
	bool suspended{};
};

struct ExecutorTimer
{
	std::int32_t function{-1};
	std::uint16_t interval{};
	std::uint16_t activations{};
	std::uint16_t countdown{};
	std::uint16_t identity{};
	std::uint32_t last_fire_tick{};
};
static_assert(sizeof(ExecutorTimer) == 0x10);
static_assert(
	sizeof(ExecutorTimer) * game::kMaxExecutorTimers == 0x100);

struct ExecutorWatch
{
	std::uint8_t context{UINT8_MAX};
	std::uint8_t stack_cell{UINT8_MAX};
	std::uint8_t component{UINT8_MAX};
};

enum class LvalueKind : std::uint8_t
{
	none,
	session,
	global,
	frame,
};

struct Executor
{
	ExecutorContext contexts[game::kMaxExecutorContexts];
	ExecutorTimer timers[game::kMaxExecutorTimers];
	ExecutorWatch watches[game::kExecutorWatchPairs];
	std::uint16_t active_contexts{};
	std::uint16_t active_timers{};
	std::uint16_t context_high_water{};
	std::uint16_t timer_high_water{};
	std::uint16_t watch_high_water{};
	std::uint16_t lvalue_index{};
	std::uint8_t lvalue_context{UINT8_MAX};
	LvalueKind lvalue_kind{LvalueKind::none};
	std::uint32_t command_count{};
	std::uint32_t error_pc{};
	std::uint8_t error_opcode{};
	bool context_overflow_logged{};
	bool timer_overflow_logged{};
	bool watch_overflow_logged{};
	bool failed{};
};

bool executor_initialize(
	Executor& executor,
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file,
	std::uint32_t tick);
bool executor_start_trigger(
	Executor& executor,
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file,
	std::uint16_t script_word,
	std::uint8_t script_identity,
	const std::uint32_t* event_arguments = nullptr,
	std::uint8_t argument_count = 0,
	bool deferred = true,
	std::uint32_t script_tick = 0,
	std::uint32_t simulation_tick = 0);
void executor_tick(
	Executor& executor,
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file,
	std::uint32_t script_tick,
	std::uint32_t simulation_tick);
void executor_service_timers(
	Executor& executor,
	const DteFile& file,
	std::uint32_t script_tick);
}
