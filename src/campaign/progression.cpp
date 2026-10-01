#include "campaign/campaign.hpp"

#include <bit>
#include <cstdint>
#include <iterator>

namespace sl_open::campaign
{
namespace
{
std::uint8_t rank_for_score(std::int32_t score)
{
	constexpr std::int32_t thresholds[] =
		{0, 35, 72, 115, 150, 200, 255, 275, 300};
	std::uint8_t result = 0;
	for (std::uint8_t index = 1; index < std::size(thresholds); ++index)
	{
		if (score < thresholds[index])
		{
			break;
		}
		result = index;
	}
	return result;
}

std::int8_t bar_for_mission(std::uint16_t mission)
{
	switch (mission)
	{
	case 7: return 0;
	case 11: return 1;
	case 19: return 2;
	case 21: return 3;
	case 25: return 4;
	default: return -1;
	}
}

std::int8_t medal_for_mission(std::uint16_t mission)
{
	switch (mission)
	{
	case 6: return 0;
	case 11: return 1;
	case 16: return 2;
	case 21: return 3;
	case 23: return 4;
	case 27: return 5;
	default: return -1;
	}
}

std::uint16_t next_mission(std::uint16_t mission)
{
	switch (mission)
	{
	case 11:
	case 12: return 14;
	case 16: return 18;
	case 21: return 23;
	case 28: return 29;
	default: return static_cast<std::uint16_t>(mission + 1);
	}
}
}

AdvanceResult campaign_apply_mission_result(
	CampaignState& state,
	MissionGrade grade,
	std::int32_t score_delta,
	std::uint16_t score_events,
	MissionCoordinatorResult coordinator_result)
{
	AdvanceResult result;
	result.completed_mission = state.mission;
	result.previous_rank = static_cast<std::int8_t>(state.rank);
	if (state.mission < 1 || state.mission > kMissionCount
		|| grade == MissionGrade::none)
	{
		result.next_mission = state.mission;
		return result;
	}

	// FUN_00475a90 first copies the completed transient grade
	// (DAT_0052a428) into retained VARS slot DAT_0052a45c. This precedes
	// mission-28's early return and every rank/award/progression update.
	state.branch_variables[branch_0052a45c] =
		static_cast<std::int32_t>(grade);
	const std::uint32_t mission_index = state.mission - 1;
	state.score = std::bit_cast<std::int32_t>(
		static_cast<std::uint32_t>(state.score) +
		static_cast<std::uint32_t>(score_delta));
	const std::uint8_t score_rank = rank_for_score(state.score);
	if (score_rank > state.rank)
	{
		state.rank = score_rank;
		state.mission_best_ranks[mission_index] = score_rank;
		result.new_rank = static_cast<std::int8_t>(score_rank);
	}
	if (state.mission != 28)
	{
		state.mission_results[mission_index] = grade;
	}
	state.mission_score_events[mission_index] = score_events;

	const std::int8_t bar = bar_for_mission(state.mission);
	if (bar >= 0 && !state.bars[bar])
	{
		state.bars[bar] = true;
		result.new_bar = bar;
	}
	const std::int8_t medal = medal_for_mission(state.mission);
	if (medal >= 0
		&& coordinator_result != MissionCoordinatorResult::retry
		&& grade == MissionGrade::perfect)
	{
		state.medals[medal] = true;
		result.new_medal = medal;
	}
	if (state.mission == 11)
	{
		state.progression = 1;
	}
	if (state.mission == 19)
	{
		state.progression = 2;
	}
	if (state.mission == 21)
	{
		state.progression = 3;
	}

	state.mission = next_mission(state.mission);
	result.next_mission = state.mission;
	result.campaign_complete = state.mission == 29;
	return result;
}

bool campaign_retry(CampaignState& state)
{
	if (state.mission < 1 || state.mission > kMissionCount)
	{
		return false;
	}
	++state.retry_count;
	if (state.retry_count <= 2)
	{
		state.retry_history[state.mission - 1] = state.retry_count;
		return true;
	}
	return false;
}

const char* campaign_medal_movie(std::int8_t medal_index)
{
	constexpr const char* movies[kAwardCount] = {
		"new_silver.bik",
		"new_black eagle.bik",
		"new_valour.bik",
		"new_legion.bik",
		"new_navy_cross.bik",
		"new_medal_of_honour.bik",
	};
	return medal_index >= 0 && medal_index < static_cast<std::int8_t>(kAwardCount)
		? movies[medal_index]
		: nullptr;
}
}
