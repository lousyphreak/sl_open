#include "mission/objectives.hpp"

#include "mission/runtime.hpp"

#include <iterator>

namespace sl_open::mission
{
void objectives_reset(Runtime& runtime)
{
	for (std::uint16_t objective = 0;
		objective < std::size(runtime.objectives);
		++objective)
	{
		const bool defined =
			objective_language_id(
				runtime.mission_number,
				runtime.mission_25_alternate,
				objective)
			!= UINT16_MAX;
		runtime.objectives[objective] =
			objective == 0 ? 2 : defined ? 1 : 0;
	}
	// HUD_init_objectives (LANCER.EXE 0x00499180) always selects slot zero,
	// including the intentionally undefined first slot of mission 22.
	runtime.current_objective = 0;
	runtime.no_current_objective = false;
}

void objectives_cycle(Runtime& runtime)
{
	// Player_process_discrete_input, LANCER.EXE
	// 0x00414aff..0x00414b65. Mission 25's alternate objective row is
	// selected through the same definition-table routing used at reset.
	const std::uint16_t row =
		objective_row(
			runtime.mission_number,
			runtime.mission_25_alternate);
	if (row >= std::size(kObjectiveLanguageIds))
	{
		runtime.no_current_objective = true;
		return;
	}

	// Retail's wrap-from-nine branch publishes slot zero directly. It
	// deliberately does not rescan or rewrite the retained no-current flag.
	if (runtime.current_objective == 9)
	{
		runtime.current_objective = 0;
		return;
	}

	std::uint16_t objective =
		runtime.current_objective < 9
			? static_cast<std::uint16_t>(
				runtime.current_objective + 1)
			: 0;
	// The executable scans at most one complete ten-slot row, skipping
	// state-zero entries and wrapping after slot nine. It retains the
	// starting slot and raises the separate no-current flag if all ten
	// states are zero.
	for (std::uint16_t skipped = 0;
		skipped < std::size(runtime.objectives)
			&& runtime.objectives[objective] == 0;
		++skipped)
	{
		objective =
			objective == 9
				? 0
				: static_cast<std::uint16_t>(objective + 1);
	}
	runtime.current_objective = objective;
	runtime.no_current_objective =
		runtime.objectives[objective] == 0;
}
}
