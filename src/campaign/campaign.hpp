#pragma once

#include "io/vfs.hpp"

#include <cstddef>
#include <cstdint>

namespace sl_open::campaign
{
constexpr std::size_t kCampaignIdCharacters = 32;
constexpr std::size_t kCallsignBytes = 50;
constexpr std::size_t kSaveNameBytes = 64;
constexpr std::uint32_t kMissionCount = 28;
constexpr std::uint32_t kLoadoutSlots = 20;
constexpr std::uint32_t kAwardCount = 6;
constexpr std::uint32_t kMaxCampaigns = 10;
// Retail's visible GAME00..GAME99 range is followed by the private
// GAME100 "restart" checkpoint. It is never enumerated by the save browser.
constexpr std::uint32_t kMaxSaveSlots = 100;
constexpr std::uint16_t kCheckpointSaveSlot = 100;
constexpr std::size_t kBranchVariableCount = 25;

// Indices follow the retail VARS save-chunk order.
enum BranchVariable : std::size_t
{
	branch_0052a434 = 1,
	branch_0052a438 = 2,
	branch_0052a408 = 9,
	branch_0052a40c = 10,
	branch_0052a410 = 11,
	branch_0052a424 = 14,
	branch_0052a45c = 18,
	branch_0052a464 = 19,
	branch_0052a470 = 22,
	branch_0052a478 = 23,
	branch_0052a480 = 24,
};

enum class Difficulty : std::uint8_t
{
	easy,
	medium,
	hard,
};

enum class Pilot : std::uint8_t
{
	female,
	male,
};

// Mission grades retain the campaign-facing values used by award rules.
enum class MissionGrade : std::int16_t
{
	none = -1,
	failed = 0,
	completed = 1,
	good = 2,
	excellent = 3,
	perfect = 4,
};

enum class MissionCoordinatorResult : std::uint8_t
{
	ordinary = 0,
	destroyed = 1,
	retry = 2,
	interrupted = 3,
	exit_session = 4,
	transfer = 5,
	executed = 6,
	cooperative_transfer = 7,
	mission_side = 8,
	network_abort = 9,
};

struct CampaignState
{
	char id[kCampaignIdCharacters + 1]{};
	char callsign[kCallsignBytes]{};
	Difficulty difficulty{Difficulty::medium};
	Pilot pilot{Pilot::female};
	std::uint16_t mission{1};
	std::int32_t score{};
	std::uint8_t rank{};
	std::uint8_t progression{};
	bool medals[kAwardCount]{};
	bool bars[kAwardCount]{};
	MissionGrade mission_results[kMissionCount]{};
	std::uint16_t mission_score_events[kMissionCount]{};
	std::uint16_t retry_history[kMissionCount]{};
	std::uint16_t retry_count{};
	std::int16_t selected_ship{};
	std::int16_t loadout[kLoadoutSlots]{};
	std::uint8_t mission_best_ranks[kMissionCount]{};
	std::int32_t branch_variables[kBranchVariableCount]{};
	std::uint32_t leaderboard_seed{};
};

struct CampaignSummary
{
	char id[kCampaignIdCharacters + 1]{};
	char callsign[kCallsignBytes]{};
	std::uint16_t mission{};
	std::uint8_t rank{};
};

struct CampaignStore
{
	SDL_EMFS_Context* filesystem{};
	char directory[io::kMaxPath]{};
	CampaignSummary campaigns[kMaxCampaigns]{};
	std::uint32_t campaign_count{};
};

struct SaveSlot
{
	std::uint16_t slot{};
	std::uint16_t mission{};
	char name[kSaveNameBytes]{};
	char callsign[kCallsignBytes]{};
};

struct SaveList
{
	SaveSlot slots[kMaxSaveSlots]{};
	std::uint32_t count{};
};

struct AdvanceResult
{
	std::uint16_t completed_mission{};
	std::uint16_t next_mission{};
	std::int8_t new_medal{-1};
	std::int8_t new_bar{-1};
	std::int8_t previous_rank{-1};
	std::int8_t new_rank{-1};
	bool campaign_complete{};
};

void campaign_defaults(
	CampaignState& state,
	const char* callsign,
	Difficulty difficulty,
	Pilot pilot);
bool campaign_callsign_set(CampaignState& state, const char* callsign);

bool campaign_store_init(CampaignStore& store, SDL_EMFS_Context* filesystem);
bool campaign_store_reload(CampaignStore& store);
bool campaign_create(CampaignStore& store, CampaignState& state);
bool campaign_profile_save(CampaignStore& store, const CampaignState& state);
bool campaign_profile_load(
	const CampaignStore& store,
	const char* campaign_id,
	CampaignState& state);

bool campaign_save_slot(
	const CampaignStore& store,
	const CampaignState& state,
	std::uint16_t slot,
	const char* display_name);
bool campaign_load_slot(
	const CampaignStore& store,
	const char* campaign_id,
	std::uint16_t slot,
	CampaignState& state,
	char* display_name,
	std::size_t display_name_capacity);
bool campaign_list_saves(
	const CampaignStore& store,
	const char* campaign_id,
	SaveList& saves);

AdvanceResult campaign_apply_mission_result(
	CampaignState& state,
	MissionGrade grade,
	std::int32_t score_delta,
	std::uint16_t score_events,
	MissionCoordinatorResult coordinator_result =
		MissionCoordinatorResult::ordinary);
bool campaign_retry(CampaignState& state);
const char* campaign_medal_movie(std::int8_t medal_index);
}
