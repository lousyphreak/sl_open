#include "game/clock.hpp"

namespace sl_open::game
{
void simulation_clock_reset(
	SimulationClock& clock,
	std::uint64_t wall_time_ms)
{
	clock.wall_time_ms = wall_time_ms;
	clock.accumulator_ms = 0;
	clock.initialized = true;
}

std::uint32_t simulation_clock_advance(
	SimulationClock& clock,
	std::uint64_t wall_time_ms)
{
	if (!clock.initialized)
	{
		simulation_clock_reset(clock, wall_time_ms);
		return 0;
	}
	const std::uint64_t elapsed = wall_time_ms >= clock.wall_time_ms
		? wall_time_ms - clock.wall_time_ms
		: 0;
	clock.wall_time_ms = wall_time_ms;
	// Retail catches the 100 Hz owner up to the multimedia-timer snapshot;
	// it does not discard elapsed ticks after a render stall. Pauses and
	// synchronous movies reset the wall anchor at their explicit boundaries.
	const std::uint32_t admitted_ms =
		static_cast<std::uint32_t>(elapsed);
	clock.accumulator_ms += admitted_ms;
	const std::uint32_t steps =
		clock.accumulator_ms / kSimulationTickMilliseconds;
	clock.accumulator_ms -= steps * kSimulationTickMilliseconds;
	// Retail exposes only the 100 Hz tick count to gameplay. The sub-tick
	// wall-clock remainder is not visible to frame consumers.
	return steps;
}
}
