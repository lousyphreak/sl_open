#include "campaign/multiplayer_progression.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iterator>

namespace sl_open::campaign
{
namespace
{
bool retains_mission_variables(MissionCoordinatorResult result)
{
	return result != MissionCoordinatorResult::destroyed
		&& result != MissionCoordinatorResult::interrupted
		&& result != MissionCoordinatorResult::exit_session
		&& result != MissionCoordinatorResult::executed
		&& result != MissionCoordinatorResult::network_abort;
}

MissionGrade sanitize_grade(MissionGrade grade)
{
	const std::int16_t value = static_cast<std::int16_t>(grade);
	return value >= static_cast<std::int16_t>(MissionGrade::none)
			&& value <= static_cast<std::int16_t>(MissionGrade::perfect)
		? grade
		: MissionGrade::none;
}

void clear_pending(MultiplayerCampaignProgression& progression)
{
	progression.pending = {};
	progression.debrief_pending = false;
}

void retain_live_mission_score(
	CampaignState& campaign,
	const MultiplayerMissionInput& input)
{
	// FUN_004b14f0 updates the owning player's live campaign score and
	// mission event counter as kills are awarded. Those values therefore
	// remain visible in FUN_004296a0 even when the coordinator route does
	// not advance or persist the campaign.
	campaign.score = std::bit_cast<std::int32_t>(
		static_cast<std::uint32_t>(campaign.score)
		+ static_cast<std::uint32_t>(input.score_delta));
	if (campaign.mission >= 1 && campaign.mission <= kMissionCount)
	{
		campaign.mission_score_events[campaign.mission - 1] =
			input.score_events;
	}
}

bool restore_checkpoint(
	MultiplayerCampaignProgression& progression,
	CampaignState& campaign)
{
	if (!progression.checkpoint_valid)
	{
		return false;
	}
	const std::int16_t selected_ship = campaign.selected_ship;
	std::int16_t loadout[kLoadoutSlots];
	std::copy(
		std::begin(campaign.loadout),
		std::end(campaign.loadout),
		std::begin(loadout));
	campaign = progression.checkpoint;
	campaign.selected_ship = selected_ship;
	std::copy(
		std::begin(loadout),
		std::end(loadout),
		std::begin(campaign.loadout));
	progression.alternate_mission_25 = false;
	clear_pending(progression);
	return true;
}
}

void multiplayer_campaign_capture_checkpoint(
	MultiplayerCampaignProgression& progression,
	const CampaignState& campaign)
{
	progression.checkpoint = campaign;
	progression.checkpoint_valid = true;
	progression.alternate_mission_25 = false;
	clear_pending(progression);
}

MultiplayerMissionResolution multiplayer_campaign_apply_result(
	MultiplayerCampaignProgression& progression,
	CampaignState& campaign,
	const MultiplayerMissionInput& input)
{
	MultiplayerMissionResolution resolution;
	resolution.coordinator = input.coordinator;
	resolution.grade = sanitize_grade(input.grade);

	if (input.campaign_state_captured
		&& retains_mission_variables(resolution.coordinator))
	{
		std::copy(
			std::begin(input.persistent_variables),
			std::end(input.persistent_variables),
			std::begin(campaign.branch_variables));
	}

	if (resolution.coordinator == MissionCoordinatorResult::exit_session
		|| resolution.coordinator
			== MissionCoordinatorResult::network_abort)
	{
		resolution.route = MultiplayerMissionRoute::leave_session;
		progression.pending = resolution;
		progression.debrief_pending = false;
		return resolution;
	}

	if (resolution.coordinator == MissionCoordinatorResult::destroyed
		|| resolution.coordinator
			== MissionCoordinatorResult::interrupted
		|| resolution.coordinator
			== MissionCoordinatorResult::executed
		|| resolution.coordinator
			== MissionCoordinatorResult::cooperative_transfer)
	{
		retain_live_mission_score(campaign, input);
		resolution.route = MultiplayerMissionRoute::debrief;
		progression.pending = resolution;
		progression.debrief_pending = true;
		return resolution;
	}

	if (resolution.coordinator == MissionCoordinatorResult::retry)
	{
		resolution.retry_exhausted = !campaign_retry(campaign);
		if (resolution.retry_exhausted)
		{
			retain_live_mission_score(campaign, input);
			resolution.route = MultiplayerMissionRoute::debrief;
			progression.pending = resolution;
			progression.debrief_pending = true;
			return resolution;
		}
	}

	if (resolution.grade == MissionGrade::none)
	{
		// FUN_00475a90 changes every non-destroyed/non-captured
		// invalid-grade result into coordinator state five. That state owns
		// the transfer presentation and terminal RESTART semantics; retaining
		// ordinary, retry, or mission-side here would let clients disagree
		// about both the report and whether Continue is legal.
		retain_live_mission_score(campaign, input);
		resolution.coordinator = MissionCoordinatorResult::transfer;
		resolution.route = MultiplayerMissionRoute::debrief;
		progression.pending = resolution;
		progression.debrief_pending = true;
		return resolution;
	}

	if (campaign.mission == 25
		&& !progression.alternate_mission_25)
	{
		// Mission 251 continues mission 25's live score rather than applying
		// a completed-mission grade, promotion, award, or progression step.
		retain_live_mission_score(campaign, input);
		progression.alternate_mission_25 = true;
		resolution.route =
			MultiplayerMissionRoute::relaunch_alternate;
		progression.pending = resolution;
		progression.debrief_pending = false;
		return resolution;
	}

	resolution.advance = campaign_apply_mission_result(
		campaign,
		resolution.grade,
		input.score_delta,
		input.score_events,
		resolution.coordinator);
	resolution.progression_applied = true;
	progression.alternate_mission_25 = false;
	resolution.route =
		resolution.coordinator == MissionCoordinatorResult::transfer
			? MultiplayerMissionRoute::debrief
			: resolution.advance.campaign_complete
			? MultiplayerMissionRoute::campaign_complete
			: MultiplayerMissionRoute::debrief;
	progression.pending = resolution;
	progression.debrief_pending =
		resolution.route == MultiplayerMissionRoute::debrief;
	return resolution;
}

void multiplayer_campaign_commit(
	MultiplayerCampaignProgression& progression,
	const CampaignState& campaign)
{
	multiplayer_campaign_capture_checkpoint(
		progression, campaign);
}

bool multiplayer_campaign_replay(
	MultiplayerCampaignProgression& progression,
	CampaignState& campaign)
{
	return restore_checkpoint(progression, campaign);
}

void multiplayer_campaign_abandon(
	MultiplayerCampaignProgression& progression)
{
	progression.checkpoint_valid = false;
	progression.alternate_mission_25 = false;
	clear_pending(progression);
}
}
