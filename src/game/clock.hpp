#pragma once

#include "game/runtime_limits.hpp"

#include <cstdint>

namespace sl_open::game
{
struct SimulationClock
{
	std::uint64_t wall_time_ms{};
	std::uint32_t accumulator_ms{};
	// Retail's gameplay and frame-snapshot clocks are wrapping 32-bit
	// counters measured in 100 Hz ticks.
	std::uint32_t gameplay_tick{};
	std::uint8_t service_phase{};
	bool initialized{};
};

void simulation_clock_reset(
	SimulationClock& clock,
	std::uint64_t wall_time_ms);
std::uint32_t simulation_clock_advance(
	SimulationClock& clock,
	std::uint64_t wall_time_ms);
}
