#pragma once

#include "campaign/campaign.hpp"
#include "game/runtime_limits.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace sl_open::game
{
constexpr std::size_t kMissionLoadoutSlots = 20;
constexpr std::size_t kMissionPersistentVariableCount = 25;
constexpr std::size_t kMultiplayerPlayerCapacity = 8;
constexpr std::size_t kMultiplayerPlayerNameBytes = 32;
constexpr std::size_t kMultiplayerSessionStateCount = 64;
constexpr std::size_t kMultiplayerPilotAssignmentCount = 6;
constexpr std::int16_t kMultiplayerFirstPilotId = 0x77;
constexpr std::int16_t kMultiplayerMaximumPilotId = 193;
constexpr std::uint8_t kMultiplayerCampaignVariableSlots[
	kMissionPersistentVariableCount] = {
	16, 17, 18, 19, 20, 21, 5, 22, 23, 6, 7, 8, 11,
	12, 13, 24, 25, 26, 27, 29, 30, 31, 32, 34, 36,
};

enum class MissionMode : std::uint8_t
{
	campaign,
	instant_action,
	training,
	multiplayer,
};

enum class MissionOrigin : std::uint8_t
{
	main_menu,
	campaign,
	campaign_sim_pod,
	multiplayer,
};

enum class MultiplayerRole : std::uint8_t
{
	offline,
	host,
	client,
};

enum class SessionResultKind : std::uint8_t
{
	none,
	restart,
	exit_to_origin,
	player_destroyed,
	mission_failed,
	mission_complete,
	load_failed,
	application_exit,
};

struct SimPodReturnState
{
	std::uint8_t page{};
	std::int8_t selected{-1};
	std::uint8_t hub_node{};
	bool late_campaign{};
};

struct MultiplayerPlayerLaunch
{
	std::int16_t loadout[kMissionLoadoutSlots]{};
	std::int16_t selected_ship{-1};
	std::int32_t team{-1};
	std::uint32_t latency{};
	char name[kMultiplayerPlayerNameBytes]{};
	std::uint8_t one_way_latency{};
	bool connected{};
};

// Each co-op participant retains a distinct live campaign/profile block.
// Shared START control selects the mission and difficulty; this image carries
// the owning process's personal progression into its MissionSession and its
// eventual per-player mission report. Mission 25's second leg reuses that
// personal image in-process.
struct MultiplayerCampaignLaunchState
{
	// Keep the complete semantic profile so score, rank, awards, retry state,
	// grades, loadout, and persistent variables never get projected from the
	// current transport authority onto another player.
	campaign::CampaignState state;
	bool present{};
	bool mission_25_alternate{};
};

// Opcode 9 sends only the first four-byte PILO catalog entry. The remaining
// 64 entries are reset locally by FUN_0049cd20 before this image is applied.
struct MultiplayerPilotCatalogEntry
{
	std::int16_t pilot_id{kMultiplayerFirstPilotId};
	std::uint8_t availability{2};
	std::uint8_t reserved{};
};
static_assert(sizeof(MultiplayerPilotCatalogEntry) == 4);

// Exact gameplay-state portion of retail opcode 9:
//   64 dwords (0x800 bits) + PILO (0x20 bits) + ALPH (0x60 bits).
// The transport encodes each integer explicitly; this semantic size assertion
// documents the retail payload rather than authorizing native-struct memcpy.
struct MultiplayerMissionBootstrapImage
{
	std::uint32_t session_state[kMultiplayerSessionStateCount]{};
	MultiplayerPilotCatalogEntry pilo;
	std::int16_t
		pilot_assignments[kMultiplayerPilotAssignmentCount]{
			-1, 0x55, 0x6c, 0x56, 0xac, 7};
};
constexpr std::size_t kMultiplayerMissionBootstrapImageBytes =
	kMultiplayerSessionStateCount * sizeof(std::uint32_t)
	+ sizeof(MultiplayerPilotCatalogEntry)
	+ kMultiplayerPilotAssignmentCount * sizeof(std::int16_t);
static_assert(kMultiplayerMissionBootstrapImageBytes == 272);

struct MultiplayerMissionBootstrap
{
	MultiplayerMissionBootstrapImage image;
	std::uint32_t launch_generation{};
	std::uint16_t mission{};
	bool present{};
};

struct MultiplayerLaunchSnapshot
{
	MultiplayerPlayerLaunch players[kMultiplayerPlayerCapacity];
	MultiplayerCampaignLaunchState campaign;
	// Shared mission state is immutable for this launch. Personal campaign
	// and profile fields remain in campaign and are never sourced from this
	// authority-authored image.
	MultiplayerMissionBootstrap bootstrap;
	std::uint32_t authoritative_seed{};
	std::uint16_t authoritative_mission{};
	std::uint8_t player_count{1};
	std::uint8_t local_player{};
	std::uint8_t gameplay_player_prefix{1};
	std::int8_t configured_team_count{-1};
	MultiplayerRole role{MultiplayerRole::offline};
	bool deathmatch_mode{};
	bool team_mode{};
	bool respawn_targetable{true};
	bool ai_turrets{};
};

struct MissionLaunchRequest
{
	std::uint16_t mission{};
	MissionMode mode{MissionMode::instant_action};
	MissionOrigin origin{MissionOrigin::main_menu};
	SimPodReturnState sim_pod;
	MultiplayerLaunchSnapshot multiplayer;
	std::uint32_t random_seed{};
	std::int32_t score{};
	std::int32_t persistent_variables[kMissionPersistentVariableCount]{};
	std::int16_t player_loadout[kMissionLoadoutSlots]{};
	std::int16_t selected_ship{-1};
	std::uint16_t score_events{};
	std::uint8_t graphics_detail{2};
	std::uint8_t default_view{};
	// LANCER.EXE 0x00562f14: 0 Easy, 1 Medium, 2 Hard.
	std::uint8_t difficulty{1};
	// LANCER.EXE DAT_00562f16. CommsVoice_format_player_line selects the
	// `mp` bank for zero and the `fp` bank for nonzero.
	std::uint8_t player_pilot_family{};
	bool light_maps{true};
	bool fixed_seed{};
	bool force_feedback{};
	bool mission_25_alternate{};
	bool campaign_player_configuration{};
};

struct SessionResult
{
	SessionResultKind kind{SessionResultKind::none};
	std::uint16_t mission{};
	std::int16_t grade{-1};
	std::int32_t score_delta{};
	std::int32_t persistent_variables[kMissionPersistentVariableCount]{};
	std::uint16_t score_events{};
	std::uint8_t coordinator_result{};
	bool campaign_state_captured{};
	// FUN_00425240 selects the ejection debrief from retained mission
	// session-state slot 28. Keep it in the terminal result so teardown and
	// per-player multiplayer publication cannot lose that branch.
	bool objectives_completed_before_ejection{};
};

constexpr bool valid_multiplayer_player(
	const MultiplayerPlayerLaunch& player,
	bool team_mode)
{
	bool name_terminated = false;
	for (const char character : player.name)
	{
		if (character == '\0')
		{
			name_terminated = true;
			break;
		}
	}
	if (!name_terminated
		|| player.team < -1
		|| player.team > 3
		|| (player.connected
			&& (player.selected_ship < 0
				|| player.selected_ship > 11
				|| (team_mode && player.team < 0))))
	{
		return false;
	}
	if (!player.connected)
	{
		return true;
	}
	for (const std::int16_t definition : player.loadout)
	{
		// FUN_00442cc0 maps the loadout UI's ninth missile selection to
		// definition ten; -1 remains the empty-hardpoint sentinel.
		if (definition < -1 || definition > 10)
		{
			return false;
		}
	}
	return true;
}

constexpr bool valid_multiplayer_mission_bootstrap(
	const MultiplayerMissionBootstrap& bootstrap,
	std::uint16_t mission,
	std::uint8_t player_count,
	bool deathmatch_mode)
{
	if (deathmatch_mode)
	{
		return !bootstrap.present;
	}
	if (!bootstrap.present
		|| bootstrap.launch_generation == 0
		|| bootstrap.mission != mission
		|| mission == 0
		|| mission > campaign::kMissionCount
		|| player_count == 0
		|| player_count > kMultiplayerPlayerCapacity
		|| bootstrap.image.session_state[15] != player_count
		|| bootstrap.image.pilo.pilot_id
			!= kMultiplayerFirstPilotId
		|| bootstrap.image.pilo.availability > 2
		|| bootstrap.image.pilo.reserved != 0
		|| bootstrap.image.pilot_assignments[0] != -1)
	{
		return false;
	}
	for (const std::int16_t pilot :
		bootstrap.image.pilot_assignments)
	{
		if (pilot < -1 || pilot > kMultiplayerMaximumPilotId)
		{
			return false;
		}
	}
	return true;
}

constexpr bool valid_multiplayer_launch_snapshot(
	const MultiplayerLaunchSnapshot& snapshot)
{
	if ((snapshot.role != MultiplayerRole::host
			&& snapshot.role != MultiplayerRole::client)
		|| snapshot.authoritative_mission == 0
			|| snapshot.player_count == 0
			|| snapshot.player_count > kMultiplayerPlayerCapacity
			|| snapshot.local_player >= snapshot.player_count
			|| snapshot.gameplay_player_prefix == 0
		|| snapshot.gameplay_player_prefix
			> kMultiplayerPlayerCapacity
		|| snapshot.local_player
			>= snapshot.gameplay_player_prefix
		|| snapshot.configured_team_count < -1
		|| snapshot.configured_team_count > 4
		|| (snapshot.team_mode && !snapshot.deathmatch_mode)
		|| (snapshot.ai_turrets && !snapshot.deathmatch_mode)
		|| !valid_multiplayer_mission_bootstrap(
			snapshot.bootstrap,
			snapshot.authoritative_mission,
			snapshot.player_count,
			snapshot.deathmatch_mode)
		|| (snapshot.campaign.mission_25_alternate
			&& (snapshot.deathmatch_mode
				|| snapshot.authoritative_mission != 25)))
	{
		return false;
	}
	// DirectPlay's migrate-host flag transfers authority independently of the
	// compact gameplay ordinal. A migrated authority may be nonzero in-flight,
	// and a later lobby join can legitimately make a client ordinal zero.
	if (snapshot.deathmatch_mode)
	{
		if (snapshot.campaign.present
			|| snapshot.campaign.mission_25_alternate)
		{
			return false;
		}
	}
	else if (!snapshot.campaign.present
		|| snapshot.campaign.state.mission
			!= snapshot.authoritative_mission
		|| snapshot.campaign.state.difficulty
			> campaign::Difficulty::hard)
	{
		return false;
	}
	// The multiplayer owner installs all eight authored player slots for a
	// deathmatch mission. Co-op installs the compact connected-player prefix
	// (LANCER.EXE 0x004a9728..0x004a97a9 and
	// 0x004a99cc..0x004a9ce6).
	if (snapshot.gameplay_player_prefix
		!= (snapshot.deathmatch_mode
			? kMultiplayerPlayerCapacity
			: snapshot.player_count))
	{
		return false;
	}
	for (std::size_t player = 0;
		player < kMultiplayerPlayerCapacity;
		++player)
	{
		const MultiplayerPlayerLaunch& launch =
			snapshot.players[player];
		if (!valid_multiplayer_player(
			launch, snapshot.team_mode))
		{
			return false;
		}
		// FUN_004b5900 compacts the 0xfc-byte lobby records after every
		// removal. The launch owners then assign gameplay slots in record
		// order, so a launch snapshot can contain neither a hole inside
		// player_count nor a connected suffix entry.
		if (launch.connected != (player < snapshot.player_count))
		{
			return false;
		}
	}
	return true;
}

constexpr bool valid_launch_request(const MissionLaunchRequest& request)
{
	if (request.mission == 0)
	{
		return false;
	}
	const bool multiplayer_mode =
		request.mode == MissionMode::multiplayer;
	const bool multiplayer_origin =
		request.origin == MissionOrigin::multiplayer;
	if (multiplayer_mode != multiplayer_origin)
	{
		return false;
	}
	if (!multiplayer_mode)
	{
		return request.mode == MissionMode::campaign
			|| request.mode == MissionMode::instant_action
			|| request.mode == MissionMode::training;
	}
	for (std::size_t index = 0;
		index < kMissionPersistentVariableCount;
		++index)
	{
		const std::int32_t authoritative_value =
			request.multiplayer.deathmatch_mode
				? 0
				: std::bit_cast<std::int32_t>(
					request.multiplayer.bootstrap.image
						.session_state[
							kMultiplayerCampaignVariableSlots[
								index]]);
		if (request.persistent_variables[index]
			!= authoritative_value)
		{
			return false;
		}
	}
	const campaign::CampaignState& campaign_state =
		request.multiplayer.campaign.state;
	const std::uint16_t campaign_score_events =
		request.multiplayer.deathmatch_mode
			|| request.mission == 0
			|| request.mission > campaign::kMissionCount
			? 0
			: campaign_state.mission_score_events[
				request.mission - 1];
	return !request.campaign_player_configuration
		&& request.mission
			== request.multiplayer.authoritative_mission
		&& request.random_seed
			== request.multiplayer.authoritative_seed
		&& request.score
			== (request.multiplayer.deathmatch_mode
				? 0
				: campaign_state.score)
		&& request.score_events
			== campaign_score_events
		&& request.difficulty
			== (request.multiplayer.deathmatch_mode
				? 0
				: static_cast<std::uint8_t>(
					campaign_state.difficulty))
		&& request.mission_25_alternate
			== request.multiplayer.campaign.mission_25_alternate
		&& valid_multiplayer_launch_snapshot(request.multiplayer);
}
}
