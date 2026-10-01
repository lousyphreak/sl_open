#pragma once

#include "campaign/campaign.hpp"

#include <cstdint>

namespace sl_open::campaign
{
enum class MultiplayerMissionRoute : std::uint8_t
{
	debrief,
	relaunch_alternate,
	leave_session,
	campaign_complete,
};

struct MultiplayerMissionInput
{
	MissionCoordinatorResult coordinator{
		MissionCoordinatorResult::ordinary};
	MissionGrade grade{MissionGrade::none};
	std::int32_t score_delta{};
	std::uint16_t score_events{};
	std::int32_t persistent_variables[kBranchVariableCount]{};
	bool campaign_state_captured{};
};

struct MultiplayerMissionResolution
{
	AdvanceResult advance;
	MultiplayerMissionRoute route{
		MultiplayerMissionRoute::debrief};
	MissionCoordinatorResult coordinator{
		MissionCoordinatorResult::ordinary};
	MissionGrade grade{MissionGrade::none};
	bool retry_exhausted{};
	bool progression_applied{};
};

// The retail multiplayer owner captures slot 100 before the first shared
// loadout. Successful progression is applied and persisted before debrief.
// Replay alone restores slot 100; Continue replaces it with the progressed
// state before the next loadout, and Leave retains the already-applied state.
struct MultiplayerCampaignProgression
{
	CampaignState checkpoint;
	MultiplayerMissionResolution pending;
	bool checkpoint_valid{};
	bool alternate_mission_25{};
	bool debrief_pending{};
};

// Capture the replay image before the first loadout and after a leader
// Continue. Mission 25's alternate leg deliberately retains the first-leg
// image so Replay starts at mission 25A again.
void multiplayer_campaign_capture_checkpoint(
	MultiplayerCampaignProgression& progression,
	const CampaignState& campaign);

// Apply the exact outer multiplayer result branches after gameplay teardown.
// Presentation remains owned by App; the returned route says what follows it.
MultiplayerMissionResolution multiplayer_campaign_apply_result(
	MultiplayerCampaignProgression& progression,
	CampaignState& campaign,
	const MultiplayerMissionInput& input);

// Debrief decisions. Continue captures the already-progressed state, Replay
// restores the existing checkpoint without consuming it, and Leave merely
// discards the multiplayer coordinator state.
void multiplayer_campaign_commit(
	MultiplayerCampaignProgression& progression,
	const CampaignState& campaign);
bool multiplayer_campaign_replay(
	MultiplayerCampaignProgression& progression,
	CampaignState& campaign);
void multiplayer_campaign_abandon(
	MultiplayerCampaignProgression& progression);
}
