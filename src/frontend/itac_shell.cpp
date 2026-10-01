#include "frontend/itac_shell.hpp"

#include "campaign/campaign.hpp"
#include "frontend/gui.hpp"
#include "frontend/itac_internal.hpp"

#include <cstdio>

namespace sl_open::frontend
{
using namespace itac;

void itac_shell_reset(
	ItacShell& shell,
	bool late_campaign,
	bool post_mission,
	std::uint8_t scenario_id,
	const campaign::CampaignState& campaign)
{
	shell = {};
	// DAT_00523088 is one only for the automatic post-mission visit.
	// Retail skips the recognition/opening prelude in that mode, while
	// both modes still pass through itacinit before entering their first
	// page (LANCER.EXE 0x0043f461..0x0043f559).
	shell.phase =
		post_mission ? ItacPhase::initializing : ItacPhase::prelude;
	shell.active_tab = post_mission ? 0 : 1;
	shell.requested_tab = shell.active_tab;
	shell.scenario_id = scenario_id;
	shell.news_count = 0;
	for (const NewsRecord& record : kNews)
	{
		if (record.threshold < scenario_id)
		{
			++shell.news_count;
		}
	}
	if (shell.news_count == 0) shell.news_count = 1;
	shell.news_selection = shell.news_count - 1;
	shell.news_viewport =
		shell.news_count > 12 ? shell.news_count - 12 : 0;
	shell.movie_count = 0;
	for (const MovieRecord& record : kMovies)
	{
		if (record.threshold < scenario_id)
		{
			++shell.movie_count;
		}
	}
	if (shell.movie_count == 0) shell.movie_count = 1;
	constexpr std::uint8_t archive_prefix[] = {
		28, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 11, 11,
		12, 13, 14, 14, 15, 16, 17, 18, 18, 19, 20, 21, 22, 23, 24};
	const std::uint8_t progress =
		scenario_id < sizeof(archive_prefix) ? scenario_id : 29;
	shell.archive_count = archive_prefix[progress];
	shell.archive_selection =
		shell.archive_count > 0 ? shell.archive_count - 1 : 0;
	shell.archive_viewport =
		shell.archive_count > 13 ? shell.archive_count - 13 : 0;
	shell.archive_rank = campaign.rank;
	shell.archive_level = campaign.progression;
	shell.archive_overall_kills =
		static_cast<std::uint16_t>(campaign.score);
	constexpr std::uint8_t archive_missions[23] = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14,
		15, 16, 18, 19, 20, 21, 23, 24, 25, 26, 27};
	for (std::uint8_t index = 0; index < 23; ++index)
	{
		const std::uint8_t mission_id = archive_missions[index];
		const std::uint8_t mission = mission_id - 1;
		shell.archive_mission_ids[index] = mission_id;
		shell.archive_grades[index] =
			static_cast<std::int8_t>(campaign.mission_results[mission]);
		shell.archive_best_ranks[index] =
			campaign.mission_best_ranks[mission];
		shell.archive_retry_history[index] =
			campaign.retry_history[mission];
		shell.archive_mission_kills[index] =
			campaign.mission_score_events[mission];
	}
	std::snprintf(
		shell.kills_callsign,
		sizeof(shell.kills_callsign),
		"%s",
		campaign.callsign);
	std::uint32_t leaderboard_seed = campaign.leaderboard_seed;
	const auto leaderboard_rand = [&leaderboard_seed]()
	{
		leaderboard_seed =
			leaderboard_seed * 0x343fdu + 0x269ec3u;
		return static_cast<std::uint16_t>(
			(leaderboard_seed >> 16) & 0x7fffu);
	};
	constexpr std::size_t kill_record_count =
		sizeof(kKills) / sizeof(kKills[0]);
	std::uint16_t projected_kills[kill_record_count]{};
	for (std::uint8_t index = 0; index < kill_record_count; ++index)
	{
		std::uint16_t value = kKills[index].initial_kills;
		for (std::uint8_t mission = 1; mission < scenario_id; ++mission)
		{
			if (mission == 12 || mission == 13
				|| mission == 17 || mission == 22
				|| (kKills[index].name == 1755
					&& mission >= 19 && mission <= 23))
			{
				continue;
			}
			const float centered =
				static_cast<float>(leaderboard_rand())
					* (1.0f / 32767.0f)
				- 0.5f;
			const float projected =
				centered * kKills[index].kills_variance
				+ kKills[index].kills_mean;
			value = static_cast<std::uint16_t>(
				value + static_cast<std::int32_t>(projected));
		}
		projected_kills[index] = value;
	}
	for (std::uint8_t index = 0; index < kill_record_count; ++index)
	{
		if (!kill_record_visible(kKills[index].name, scenario_id)) continue;
		const std::uint8_t slot = shell.kills_count++;
		shell.kills_order[slot] = index;
		shell.kills_values[slot] = projected_kills[index];
	}
	const std::uint8_t player = shell.kills_count++;
	shell.kills_order[player] = 0xff;
	shell.kills_values[player] = shell.archive_overall_kills;
	for (std::uint8_t left = 0; left < shell.kills_count; ++left)
	{
		for (std::uint8_t right = left + 1;
			right < shell.kills_count;
			++right)
		{
			if (shell.kills_values[right] > shell.kills_values[left])
			{
				const std::uint16_t value = shell.kills_values[left];
				shell.kills_values[left] = shell.kills_values[right];
				shell.kills_values[right] = value;
				const std::uint8_t order = shell.kills_order[left];
				shell.kills_order[left] = shell.kills_order[right];
				shell.kills_order[right] = order;
			}
		}
	}
	shell.hovered_tab = -1;
	shell.pointer_x = 320.0f;
	shell.pointer_y = 240.0f;
	shell.late_campaign = late_campaign;
	// A normal visit starts on the News page, but it is still owned by the
	// hub's ITAC action and must return through that doorway.
	shell.opened_from_news = false;
}

const char* itac_shell_movie(const ItacShell& shell)
{
	switch (shell.phase)
	{
	case ItacPhase::prelude:
		return shell.late_campaign
			? "inter/itac/itac open.bik"
			: "itac_eye_recog.bik";
	case ItacPhase::initializing:
		return "inter/itac/itacinit.bik";
	case ItacPhase::entering:
		return kEnterMovies[shell.active_tab];
	case ItacPhase::leaving:
		return kLeaveMovies[shell.active_tab];
	case ItacPhase::report:
		return kMovies[shell.movie_selection].movie;
	case ItacPhase::exiting:
		return kEnterMovies[kItacExitTab];
	default:
		return nullptr;
	}
}

bool itac_shell_movie_finished(ItacShell& shell)
{
	switch (shell.phase)
	{
	case ItacPhase::prelude:
		shell.phase = ItacPhase::initializing;
		return true;
	case ItacPhase::initializing:
		shell.phase = ItacPhase::entering;
		return true;
	case ItacPhase::entering:
		shell.phase = ItacPhase::active;
		return false;
	case ItacPhase::leaving:
		if (shell.requested_tab == kItacExitTab)
		{
			shell.phase = ItacPhase::exiting;
		}
		else
		{
			shell.active_tab = shell.requested_tab;
			shell.phase = ItacPhase::entering;
		}
		return true;
	case ItacPhase::report:
		shell.phase = ItacPhase::active;
		return false;
	case ItacPhase::exiting:
		shell.phase = ItacPhase::complete;
		return false;
	default:
		return false;
	}
}

void itac_shell_set_pointer(
	ItacShell& shell,
	float x,
	float y,
	bool inside,
	std::uint64_t now)
{
	shell.pointer_x = x;
	shell.pointer_y = y;
	const std::int8_t previous = shell.hovered_tab;
	shell.hovered_tab = -1;
	shell.news_hovered_row = -1;
	shell.movie_hovered_row = -1;
	shell.squad_hovered_row = -1;
	shell.pilot_hovered_row = -1;
	shell.capital_hovered_row = -1;
	shell.fighter_hovered_row = -1;
	shell.archive_hovered_row = -1;
	if (inside && shell.phase == ItacPhase::active)
	{
		for (std::uint8_t index = 0; index < kItacTabCount; ++index)
		{
			if (gui::hit_open(kTabRegions[index], x, y))
			{
				shell.hovered_tab = static_cast<std::int8_t>(index);
				break;
			}
		}
		if (shell.hovered_tab < 0 && shell.active_tab == 0)
		{
			for (std::uint8_t row = 0;
				row < shell.archive_visible_rows;
				++row)
			{
				const Region region{
					474,
					shell.archive_row_y[row],
					140,
					shell.archive_row_height[row]};
				if (gui::hit_open(region, x, y))
				{
					shell.archive_hovered_row =
						static_cast<std::int8_t>(row);
					break;
				}
			}
		}
		if (shell.hovered_tab < 0 && shell.active_tab == 1)
		{
			for (std::uint8_t row = 0;
				row < shell.news_visible_rows;
				++row)
			{
				const Region region{
					474,
					shell.news_row_y[row],
					140,
					shell.news_row_height[row]};
				if (gui::hit_open(region, x, y))
				{
					shell.news_hovered_row = static_cast<std::int8_t>(row);
					break;
				}
			}
		}
		if (shell.hovered_tab < 0 && shell.active_tab == 2)
		{
			for (std::uint8_t row = 0;
				row < shell.movie_visible_rows;
				++row)
			{
				const Region region{
					474,
					shell.movie_row_y[row],
					140,
					shell.movie_row_height[row]};
				if (gui::hit_open(region, x, y))
				{
					shell.movie_hovered_row = static_cast<std::int8_t>(row);
					break;
				}
			}
		}
		if (shell.hovered_tab < 0 && shell.active_tab == 5)
		{
			for (std::uint8_t row = 0;
				row < shell.squad_visible_rows;
				++row)
			{
				const Region region{
					472,
					shell.squad_row_y[row],
					147,
					shell.squad_row_height[row]};
				if (gui::hit_open(region, x, y))
				{
					shell.squad_hovered_row = static_cast<std::int8_t>(row);
					break;
				}
			}
		}
		if (shell.hovered_tab < 0 && shell.active_tab == 4)
		{
			for (std::uint8_t row = 0;
				row < shell.capital_visible_rows;
				++row)
			{
				const Region region{
					474,
					shell.capital_row_y[row],
					140,
					shell.capital_row_height[row]};
				if (gui::hit_open(region, x, y))
				{
					shell.capital_hovered_row =
						static_cast<std::int8_t>(row);
					break;
				}
			}
		}
		if (shell.hovered_tab < 0 && shell.active_tab == 3)
		{
			for (std::uint8_t row = 0;
				row < shell.fighter_visible_rows;
				++row)
			{
				const Region region{
					474,
					shell.fighter_row_y[row],
					140,
					shell.fighter_row_height[row]};
				if (gui::hit_open(region, x, y))
				{
					shell.fighter_hovered_row =
						static_cast<std::int8_t>(row);
					break;
				}
			}
		}
		if (shell.hovered_tab < 0 && shell.active_tab == 6)
		{
			for (std::uint8_t row = 0;
				row < shell.pilot_visible_rows;
				++row)
			{
				const Region region{
					474,
					shell.pilot_row_y[row],
					140,
					shell.pilot_row_height[row]};
				if (gui::hit_open(region, x, y))
				{
					shell.pilot_hovered_row = static_cast<std::int8_t>(row);
					break;
				}
			}
		}
	}
	if (shell.hovered_tab != previous)
	{
		shell.hover_started_at = now;
	}
}

bool itac_shell_select(ItacShell& shell)
{
	if (shell.phase != ItacPhase::active)
	{
		return false;
	}
	if (shell.hovered_tab < 0 && shell.active_tab == 0)
	{
		if (shell.archive_count == 0)
		{
			return false;
		}
		if (shell.archive_hovered_row >= 0)
		{
			shell.archive_selection = static_cast<std::uint8_t>(
				shell.archive_viewport + shell.archive_hovered_row);
			shell.archive_body_scroll = 0;
		}
		else if (gui::hit_open(
			{514, 367, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.archive_viewport + 13 < shell.archive_count)
		{
			++shell.archive_viewport;
		}
		else if (gui::hit_open(
			{541, 367, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.archive_viewport > 0)
		{
			--shell.archive_viewport;
		}
		else if (gui::hit_open(
			{325, 215, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.archive_body_scroll > 0)
		{
			--shell.archive_body_scroll;
		}
		else if (gui::hit_open(
			{352, 215, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.archive_body_scroll < shell.archive_body_scroll_max)
		{
			++shell.archive_body_scroll;
		}
		return false;
	}
	if (shell.hovered_tab < 0 && shell.active_tab == 1)
	{
		if (shell.news_hovered_row >= 0)
		{
			shell.news_selection = static_cast<std::uint8_t>(
				shell.news_viewport + shell.news_hovered_row);
			shell.news_body_scroll = 0;
		}
		else if (gui::hit_open({515, 368, 25, 25}, shell.pointer_x, shell.pointer_y)
			&& shell.news_viewport + 1 < shell.news_count)
		{
			++shell.news_viewport;
		}
		else if (gui::hit_open({540, 368, 25, 25}, shell.pointer_x, shell.pointer_y)
			&& shell.news_viewport > 0)
		{
			--shell.news_viewport;
		}
		else if (gui::hit_open({45, 327, 25, 25}, shell.pointer_x, shell.pointer_y)
			&& shell.news_body_scroll > 0)
		{
			--shell.news_body_scroll;
		}
		else if (gui::hit_open(
			{70, 327, 25, 25}, shell.pointer_x, shell.pointer_y)
			&& shell.news_body_scroll < shell.news_body_scroll_max)
		{
			++shell.news_body_scroll;
		}
		return false;
	}
	if (shell.hovered_tab < 0 && shell.active_tab == 2)
	{
		if (shell.movie_hovered_row >= 0)
		{
			shell.movie_selection =
				static_cast<std::uint8_t>(shell.movie_hovered_row);
			shell.movie_body_scroll = 0;
		}
		else if (gui::hit_open(
			{135, 277, 25, 25}, shell.pointer_x, shell.pointer_y)
			&& shell.movie_body_scroll > 0)
		{
			--shell.movie_body_scroll;
		}
		else if (gui::hit_open(
			{160, 277, 25, 25}, shell.pointer_x, shell.pointer_y)
			&& shell.movie_body_scroll < shell.movie_body_scroll_max)
		{
			++shell.movie_body_scroll;
		}
		else if (gui::hit_open(
			{360, 277, 40, 40}, shell.pointer_x, shell.pointer_y))
		{
			shell.phase = ItacPhase::report;
			return true;
		}
		return false;
	}
	if (shell.hovered_tab < 0 && shell.active_tab == 5)
	{
		std::int8_t faction = -1;
		if (gui::hit_open({550, 61, 60, 60}, shell.pointer_x, shell.pointer_y))
		{
			faction = 0;
		}
		else if (gui::hit_open(
			{480, 61, 60, 60}, shell.pointer_x, shell.pointer_y))
		{
			faction = 1;
		}
		if (faction >= 0
			&& shell.squad_faction != static_cast<std::uint8_t>(faction))
		{
			shell.squad_faction = static_cast<std::uint8_t>(faction);
			shell.squad_selection = 0;
			shell.squad_viewport = 0;
			shell.squad_body_scroll = 0;
		}
		else if (shell.squad_hovered_row >= 0)
		{
			shell.squad_selection = static_cast<std::uint8_t>(
				shell.squad_viewport + shell.squad_hovered_row);
			shell.squad_body_scroll = 0;
		}
		else if (gui::hit_open(
			{515, 367, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.squad_viewport + 13 < squad_count(shell))
		{
			++shell.squad_viewport;
		}
		else if (gui::hit_open(
			{542, 367, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.squad_viewport > 0)
		{
			--shell.squad_viewport;
		}
		else if (gui::hit_open(
			{325, 215, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.squad_body_scroll > 0)
		{
			--shell.squad_body_scroll;
		}
		else if (gui::hit_open(
			{352, 215, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.squad_body_scroll < shell.squad_body_scroll_max)
		{
			++shell.squad_body_scroll;
		}
		return false;
	}
	if (shell.hovered_tab < 0 && shell.active_tab == 4)
	{
		std::int8_t faction = -1;
		if (gui::hit_open({551, 59, 64, 41}, shell.pointer_x, shell.pointer_y))
		{
			faction = 0;
		}
		else if (gui::hit_open(
			{478, 59, 64, 61}, shell.pointer_x, shell.pointer_y))
		{
			faction = 1;
		}
		if (faction >= 0
			&& shell.capital_faction != static_cast<std::uint8_t>(faction))
		{
			shell.capital_faction = static_cast<std::uint8_t>(faction);
			shell.capital_selection = 0;
			shell.capital_viewport = 0;
			shell.capital_body_scroll = 0;
		}
		else if (shell.capital_hovered_row >= 0)
		{
			shell.capital_selection = static_cast<std::uint8_t>(
				shell.capital_viewport + shell.capital_hovered_row);
			shell.capital_body_scroll = 0;
		}
		else if (gui::hit_open(
			{514, 367, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.capital_viewport + 13 < capital_count(shell))
		{
			++shell.capital_viewport;
		}
		else if (gui::hit_open(
			{541, 367, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.capital_viewport > 0)
		{
			--shell.capital_viewport;
		}
		else if (gui::hit_open(
			{325, 215, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.capital_body_scroll > 0)
		{
			--shell.capital_body_scroll;
		}
		else if (gui::hit_open(
			{352, 215, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.capital_body_scroll < shell.capital_body_scroll_max)
		{
			++shell.capital_body_scroll;
		}
		return false;
	}
	if (shell.hovered_tab < 0 && shell.active_tab == 3)
	{
		std::int8_t faction = -1;
		if (gui::hit_open({551, 59, 64, 41}, shell.pointer_x, shell.pointer_y))
		{
			faction = 0;
		}
		else if (gui::hit_open(
			{478, 59, 64, 61}, shell.pointer_x, shell.pointer_y))
		{
			faction = 1;
		}
		if (faction >= 0
			&& shell.fighter_faction != static_cast<std::uint8_t>(faction))
		{
			shell.fighter_faction = static_cast<std::uint8_t>(faction);
			shell.fighter_selection = 0;
		}
		else if (shell.fighter_hovered_row >= 0)
		{
			shell.fighter_selection =
				static_cast<std::uint8_t>(shell.fighter_hovered_row);
		}
		return false;
	}
	if (shell.hovered_tab < 0 && shell.active_tab == 6)
	{
		std::int8_t faction = -1;
		if (gui::hit_open({551, 59, 64, 41}, shell.pointer_x, shell.pointer_y))
		{
			faction = 0;
		}
		else if (gui::hit_open(
			{478, 59, 64, 61}, shell.pointer_x, shell.pointer_y))
		{
			faction = 1;
		}
		if (faction >= 0
			&& shell.pilot_faction != static_cast<std::uint8_t>(faction))
		{
			shell.pilot_faction = static_cast<std::uint8_t>(faction);
			shell.pilot_selection = 0;
			shell.pilot_viewport = 0;
			shell.pilot_body_scroll = 0;
		}
		else if (shell.pilot_hovered_row >= 0)
		{
			shell.pilot_selection = static_cast<std::uint8_t>(
				shell.pilot_viewport + shell.pilot_hovered_row);
			shell.pilot_body_scroll = 0;
		}
		else if (shell.pilot_faction == 0
			&& gui::hit_open(
				{514, 367, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.pilot_viewport + 13 < pilot_count(shell))
		{
			++shell.pilot_viewport;
		}
		else if (shell.pilot_faction == 0
			&& gui::hit_open(
				{541, 367, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.pilot_viewport > 0)
		{
			--shell.pilot_viewport;
		}
		else if (gui::hit_open(
			{44, 342, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.pilot_body_scroll > 0)
		{
			--shell.pilot_body_scroll;
		}
		else if (gui::hit_open(
			{71, 342, 27, 27}, shell.pointer_x, shell.pointer_y)
			&& shell.pilot_body_scroll < shell.pilot_body_scroll_max)
		{
			++shell.pilot_body_scroll;
		}
		return false;
	}
	if (shell.hovered_tab < 0 && shell.active_tab == 7)
	{
		if (gui::hit_open({515, 368, 25, 25}, shell.pointer_x, shell.pointer_y)
			&& shell.kills_viewport + 5 < shell.kills_count)
		{
			++shell.kills_viewport;
		}
		else if (gui::hit_open(
			{540, 368, 25, 25}, shell.pointer_x, shell.pointer_y)
			&& shell.kills_viewport > 0)
		{
			--shell.kills_viewport;
		}
		return false;
	}
	if (shell.hovered_tab < 0)
	{
		return false;
	}
	shell.requested_tab =
		static_cast<std::uint8_t>(shell.hovered_tab);
	if (shell.requested_tab == shell.active_tab)
	{
		return false;
	}
	shell.phase = ItacPhase::leaving;
	shell.hovered_tab = -1;
	return true;
}

bool itac_shell_exit(ItacShell& shell)
{
	if (shell.phase != ItacPhase::active)
	{
		return false;
	}
	shell.requested_tab = kItacExitTab;
	shell.phase = ItacPhase::leaving;
	shell.hovered_tab = -1;
	return true;
}

}
