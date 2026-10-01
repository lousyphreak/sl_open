#include "mission/player_comms.hpp"

#include "ai/runtime.hpp"
#include "assets/pilot_presentation.hpp"
#include "assets/pilot_stats.hpp"
#include "assets/ship_stats.hpp"
#include "core/mission_log.hpp"
#include "game/world.hpp"
#include "mission/events.hpp"
#include "mission/runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

#include <glm/geometric.hpp>

namespace sl_open::mission
{
namespace
{
bool return_to_base_requested(const Runtime& mission)
{
	// FUN_00453de0 reads DAT_0052a418 directly. That address is dword ten
	// in the mission executor's shared session-state table, so script
	// assignments must drive both the return prompt and landing command.
	return mission.session_state[10] != 0;
}

constexpr const char* kJumpWarnings[4][4] = {
	{"moo_w1001.ut", "moo_w1002.ut", "moo_w1003.ut", "moo_w1004.ut"},
	{"moo_w2001.ut", "moo_w2002.ut", "moo_w2003.ut", "moo_w2004.ut"},
	{"moo_w3001.ut", "moo_w3002.ut", "moo_w3003.ut", "moo_w3004.ut"},
	{"moo_w4001.ut", "moo_w4002.ut", "moo_w4003.ut", "moo_w4004.ut"},
};
constexpr const char* kReturnToBase[] = {
	"moolnd_001.ut", "moolnd_002.ut", "moolnd_003.ut"};
constexpr const char* kPlayerLock[] = {
	"plck_001.ut", "plck_002.ut", "plck_003.ut", "plck_004.ut",
	"plck_005.ut", "plck_006.ut", "plck_007.ut", "plck_008.ut"};
constexpr const char* kPlayerKill[] = {
	"plyrkl_001.ut", "plyrkl_002.ut", "plyrkl_003.ut",
	"plyrkl_004.ut", "plyrkl_005.ut", "plyrkl_006.ut",
	"plyrkl_007.ut", "plyrkl_008.ut", "plyrkl_009.ut"};
constexpr const char* kEnemyEjected[] = {
	"enmejt_001.ut", "enmejt_002.ut", "enmejt_003.ut"};
constexpr const char* kTransportKill[] = {
	"trpkl_001.ut", "trpkl_002.ut", "trpkl_003.ut",
	"trpkl_004.ut", "trpkl_005.ut", "trpkl_006.ut"};
constexpr const char* kNpcDeath[] = {
	"npcdth_001.ut", "npcdth_002.ut", "npcdth_003.ut", "npcdth_004.ut"};
constexpr const char* kJumpRequest[] = {
	"jmp_001.ut", "jmp_002.ut", "jmp_003.ut", "jmp_004.ut"};
constexpr const char* kWarpRequest[] = {
	"wrp_001.ut", "wrp_002.ut", "wrp_003.ut", "wrp_004.ut"};
constexpr const char* kRescue[] = {
	"res_001.ut", "res_002.ut", "res_003.ut"};
constexpr const char* kPilotDeath[] = {
	"dth_001.ut", "dth_002.ut", "dth_003.ut",
	"dth_004.ut", "dth_005.ut", "dth_006.ut"};
constexpr const char* kPlayerHitTaunt[] = {
	"tnt_001.ut", "tnt_002.ut", "tnt_003.ut", "tnt_004.ut",
	"tnt_005.ut", "tnt_006.ut", "tnt_007.ut", "tnt_008.ut",
	"tnt_009.ut", "tnt_010.ut", "tnt_011.ut", "tnt_012.ut",
	"tnt_013.ut"};
constexpr const char* kReliantLaunch[] = {
	"relbdg_001.ut", "relbdg_002.ut", "relbdg_003.ut",
	"relbdg_004.ut", "relbdg_005.ut", "relbdg_006.ut"};
constexpr const char* kYamatoLaunch[] = {
	"yambdg_001.ut", "yambdg_002.ut", "yambdg_003.ut",
	"yambdg_004.ut", "yambdg_005.ut"};
constexpr const char* kAttackRejectedShort[] = {
	"_amt_005.ut", "_amt_006.ut", "_amt_007.ut", "_amt_008.ut"};
constexpr const char* kAttackRejectedNormal[] = {
	"_amt_001.ut", "_amt_002.ut", "_amt_003.ut", "_amt_004.ut"};
constexpr const char* kAttackAcceptedShort[] = {
	"_amt_009.ut", "_amt_010.ut", "_amt_011.ut",
	"_amt_012.ut", "_amt_013.ut"};
constexpr const char* kAttackAcceptedNormal[] = {
	"_amt_005.ut", "_amt_006.ut", "_amt_007.ut", "_amt_008.ut"};
constexpr const char* kBackoffRejectedShort[] = {
	"_bkoff_001.ut", "_bkoff_002.ut", "_bkoff_003.ut",
	"_bkoff_004.ut", "_bkoff_005.ut", "_bkoff_006.ut"};
constexpr const char* kBackoffRejectedNormal[] = {
	"_bkoff_001.ut", "_bkoff_002.ut", "_bkoff_003.ut", "_bkoff_004.ut"};
constexpr const char* kBackoffAcceptedShort[] = {
	"_bkoff_007.ut", "_bkoff_008.ut", "_bkoff_009.ut",
	"_bkoff_010.ut", "_bkoff_011.ut", "_bkoff_012.ut",
	"_bkoff_013.ut", "_bkoff_014.ut", "_bkoff_015.ut"};
constexpr const char* kBackoffAcceptedNormal[] = {
	"_bkoff_005.ut", "_bkoff_006.ut", "_bkoff_007.ut",
	"_bkoff_008.ut", "_bkoff_009.ut"};
constexpr const char* kHelpRejectedShort[] = {
	"_hlpme_001.ut", "_hlpme_002.ut", "_hlpme_003.ut",
	"_hlpme_004.ut", "_hlpme_005.ut", "_hlpme_006.ut"};
constexpr const char* kHelpRejectedNormal[] = {
	"_hlpme_001.ut", "_hlpme_002.ut", "_hlpme_003.ut", "_hlpme_004.ut"};
constexpr const char* kHelpAcceptedShort[] = {
	"_hlpme_007.ut", "_hlpme_008.ut", "_hlpme_009.ut",
	"_hlpme_010.ut", "_hlpme_011.ut", "_hlpme_012.ut",
	"_hlpme_013.ut", "_hlpme_014.ut"};
constexpr const char* kHelpAcceptedNormal[] = {
	"_hlpme_005.ut", "_hlpme_006.ut", "_hlpme_007.ut", "_hlpme_008.ut"};
constexpr const char* kCriticizeShort[] = {
	"_cmon_001.ut", "_cmon_002.ut", "_cmon_003.ut", "_cmon_004.ut",
	"_cmon_005.ut", "_cmon_006.ut", "_cmon_007.ut", "_cmon_008.ut",
	"_cmon_009.ut", "_cmon_010.ut", "_cmon_011.ut", "_cmon_012.ut"};
constexpr const char* kCriticizeNormal[] = {
	"_cmon_001.ut", "_cmon_002.ut", "_cmon_003.ut", "_cmon_004.ut"};
constexpr const char* kPraiseShort[] = {
	"_iou_001.ut", "_iou_002.ut", "_iou_003.ut", "_iou_004.ut",
	"_iou_005.ut", "_iou_006.ut", "_iou_007.ut", "_iou_008.ut"};
constexpr const char* kPraiseNormal[] = {
	"_iou_001.ut", "_iou_002.ut", "_iou_003.ut", "_iou_004.ut"};
constexpr const char* kStatusShort[3][8] = {
	{"_status_001.ut", "_status_002.ut", "_status_003.ut", "_status_004.ut",
	 "_status_005.ut", "_status_006.ut", "_status_007.ut", nullptr},
	{"_status_008.ut", "_status_009.ut", "_status_010.ut", "_status_011.ut",
	 "_status_012.ut", "_status_013.ut", "_status_014.ut", "_status_015.ut"},
	{"_status_019.ut", "_status_020.ut", "_status_021.ut", "_status_022.ut",
	 "_status_023.ut", "_status_024.ut", nullptr, nullptr},
};
constexpr std::uint8_t kStatusShortCount[3] = {7, 8, 6};
constexpr const char* kStatusNormal[3][4] = {
	{"_status_001.ut", "_status_002.ut", "_status_003.ut", "_status_004.ut"},
	{"_status_005.ut", "_status_006.ut", "_status_007.ut", "_status_008.ut"},
	{"_status_009.ut", "_status_010.ut", "_status_011.ut", nullptr},
};
constexpr std::uint8_t kStatusNormalCount[3] = {4, 4, 3};
constexpr const char* kBackupAccepted[] = {
	"_reqbk_001.ut", "_reqbk_002.ut", "_reqbk_003.ut", "_reqbk_004.ut",
	"_reqbk_005.ut", "_reqbk_006.ut", "_reqbk_007.ut"};
constexpr const char* kBackupDenied[] = {
	"_reqbk_008.ut", "_reqbk_009.ut", "_reqbk_010.ut", "_reqbk_011.ut",
	"_reqbk_012.ut", "_reqbk_013.ut", "_reqbk_014.ut"};
constexpr const char* kLandingDenied[] = {
	"_lnd_den_01.ut", "_lnd_den_02.ut", "_lnd_den_03.ut", "_lnd_den_04.ut"};
constexpr const char* kLandingSuccess12[] = {
	"_lnd_010.ut", "_lnd_011.ut", "_lnd_012.ut", "_lnd_013.ut",
	"_lnd_014.ut", "_lnd_015.ut", "_lnd_016.ut"};
constexpr const char* kLandingSuccess34[] = {
	"_lnd_001.ut", "_lnd_002.ut", "_lnd_003.ut", "_lnd_004.ut",
	"_lnd_005.ut", "_lnd_006.ut", "_lnd_007.ut", "_lnd_008.ut",
	"_lnd_009.ut"};
constexpr const char* kLandingSuccessOther[] = {
	"_lnd_017.ut", "_lnd_018.ut", "_lnd_019.ut", "_lnd_020.ut",
	"_lnd_021.ut", "_lnd_022.ut", "_lnd_023.ut", "_lnd_024.ut"};
constexpr const char* kEnemyResponseGeneric[] = {
	"_res_001.ut", "_res_002.ut", "_res_003.ut", "_res_004.ut",
	"_res_005.ut", "_res_006.ut", "_res_007.ut", "_res_008.ut",
	"_res_009.ut", "_res_010.ut", "_res_011.ut", "_res_012.ut",
	"_res_013.ut", "_res_014.ut", "_res_015.ut", "_res_016.ut",
	"_res_017.ut", "_res_018.ut", "_res_019.ut", "_res_020.ut",
	"_res_021.ut", "_res_022.ut", "_res_023.ut", "_res_024.ut"};

std::uint16_t comms_rand15(game::World& world)
{
	world.random_seed = world.random_seed * 214013u + 2531011u;
	return static_cast<std::uint16_t>((world.random_seed >> 16) & 0x7fffu);
}

template<std::size_t N>
const char* random_line(game::World& world, const char* const (&lines)[N])
{
	return lines[comms_rand15(world) % N];
}

std::uint16_t world_index(const game::World& world, const game::WorldObject& object)
{
	return static_cast<std::uint16_t>(&object - std::begin(world.objects));
}

const char* allied_prefix(std::uint16_t type)
{
	switch (type)
	{
	case 0x00: case 0x06: case 0xac: return "ban";
	case 0x01: case 0x78: return "dic";
	case 0x03: case 0x05: case 0x55: case 0x71: case 0x91: return "fre";
	case 0x07: return "vip";
	case 0x1d: return "enq";
	case 0x56: case 0x66: case 0x6d: return "sil";
	case 0x57: case 0x8b: return "tak";
	case 0x58: case 0x67: case 0x8a: case 0x90:
	case 0x9f: case 0xa3: case 0xa6: return "jor";
	case 0x59: case 0x6e: case 0x8e: case 0xa9: return "vix";
	case 0x5a: case 0x68: case 0x92: case 0xa0: return "cut";
	case 0x5b: case 0x95: case 0x96: case 0x98: return "cla";
	case 0x5c: case 0x9d: case 0xa8: return "ski";
	case 0x5d: case 0x73: case 0x93: case 0x9c: case 0xa5: return "jui";
	case 0x5e: case 0x8f: case 0x99: case 0x9e: case 0xa2: return "fac";
	case 0x5f: return "haw";
	case 0x60: case 0x8d: return "arr";
	case 0x61: case 0x6b: case 0x6f: case 0x97: case 0xa4: return "ner";
	case 0x62: case 0x69: case 0x72: return "rhi";
	case 0x63: case 0x70: case 0x76: return "sta";
	case 0x64: case 0x75: case 0x8c: return "fla";
	case 0x6c: case 0x9a: case 0xa1: case 0xaa: case 0xb9: return "wor";
	case 0x74: case 0x77: case 0x94: case 0x9b: case 0xa7: return "ego";
	default: return nullptr;
	}
}

bool format_live_voice(
	const game::WorldObject& object,
	const char* suffix,
	char (&output)[50])
{
	const char* prefix = nullptr;
	if (object.allegiance_class == 0)
	{
		// CommsVoice_format_pilot_line reads GameObject+0x740. The
		// object type is the leading dword; +0x740 is the pilot identity.
		prefix = allied_prefix(object.pilot);
	}
	else if (object.allegiance_class == 1)
	{
		// PilotPresentationDefinition +0x06 is the observed enemy bank.
		const assets::PilotPresentationDefinition* definition =
			assets::pilot_presentation(object.pilot);
		if (definition != nullptr)
		{
			switch (definition->presentation_mode)
			{
			case 5: prefix = "rus"; break;
			case 6: prefix = "chn"; break;
			case 7: prefix = "arb"; break;
			default: break;
			}
		}
	}
	if (prefix == nullptr || suffix == nullptr)
	{
		return false;
	}
	return std::snprintf(output, sizeof(output), "%s%s", prefix, suffix)
		> 0;
}

void presentation_start(
	Runtime& mission,
	game::World& world,
	const QueuedComm& request)
{
	PresentationRequest& output = mission.presentation;
	output.voice = static_cast<std::uint16_t>(request.voice_id);
	output.comms_category =
		static_cast<std::uint8_t>(request.category);
	output.speaker_mission_index = UINT16_MAX;
	output.pilot = UINT16_MAX;
	if (request.speaker_id >= 0
		&& request.speaker_id < static_cast<std::int32_t>(
			std::size(world.objects)))
	{
		const game::WorldObject& speaker =
			world.objects[request.speaker_id];
		if (speaker.active)
		{
			output.speaker_mission_index = speaker.mission_index;
			output.pilot = speaker.pilot;
		}
	}
	else if (request.speaker_id >= 0xffff)
	{
		output.pilot = static_cast<std::uint16_t>(
			request.speaker_id - 0xffff);
	}
	std::snprintf(
		output.movie_path,
		sizeof(output.movie_path),
		"%s",
		request.fm8_path);
	// CommsVoice_play_or_queue mode zero invokes hudmovie_play_resource
	// synchronously after loading speech. Publish the deferred equivalent so
	// WaitForMovie in the same executor pass observes the movie as busy.
	output.movie_pending = output.movie_path[0] != '\0';
	const char* voice = request.voice_path;
	if (std::strchr(voice, '/') == nullptr
		&& std::strchr(voice, '\\') == nullptr)
	{
		std::snprintf(
			output.speech_path,
			sizeof(output.speech_path),
			"ms_speech/%s",
			voice);
	}
	else
	{
		std::snprintf(
			output.speech_path,
			sizeof(output.speech_path),
			"%s",
			voice);
	}
	output.comms_pending = true;
	output.comms_request_serial = ++output.slot_zero_request_serial;
	mission.player_comms.active_voice = request.voice_id;
	mission.player_comms.active_speaker = request.speaker_id;
	diagnostics::mission_log(
		"comms play mode=active speaker=%d voice=%d category=%d path=%s",
		request.speaker_id,
		static_cast<int>(request.voice_id),
		request.category,
		output.speech_path);
}

std::uint16_t compiled_chatter_pilot(const Runtime& mission)
{
	return mission.mission_number > 13 ? 2 : 4;
}

const game::WorldObject* resolve_mission_object(
	const game::World& world,
	std::uint16_t mission_index)
{
	for (const game::WorldObject& object : world.objects)
	{
		if (object.active && object.mission_index == mission_index)
		{
			return &object;
		}
	}
	return nullptr;
}

const game::WorldObject* resolve_command_target(
	const game::World& world,
	const ai::Command& command)
{
	if (command.target_kind == ai::TargetKind::world_object)
	{
		if (command.target >= std::size(world.objects))
		{
			return nullptr;
		}
		const game::WorldObject& target =
			world.objects[command.target];
		return target.active ? &target : nullptr;
	}
	return command.target_kind == ai::TargetKind::object
		? resolve_mission_object(world, command.target)
		: nullptr;
}

std::array<std::uint16_t, 6> alpha_slots(
	const Runtime& mission,
	const game::World& world)
{
	std::array<std::uint16_t, 6> output;
	output.fill(UINT16_MAX);
	for (std::uint16_t group_index = 0;
		group_index < mission.group_count;
		++group_index)
	{
		const GroupRecord& group = mission.groups[group_index];
		if (group.object_class != 0)
		{
			continue;
		}
		// MissionGroup_rebuild_allegiance_lists replaces the class table
		// each time it encounters another group of that class. The final
		// class-zero group therefore owns DAT_00515d88's six slots.
		output.fill(UINT16_MAX);
		const std::uint16_t count = std::min<std::uint16_t>(
			group.member_count,
			static_cast<std::uint16_t>(output.size()));
		for (std::uint16_t member = 0; member < count; ++member)
		{
			const std::uint16_t mission_index =
				mission.group_members[group.first_member + member];
			const game::WorldObject* live =
				resolve_mission_object(world, mission_index);
			if (live != nullptr)
			{
				output[member] = world_index(world, *live);
			}
		}
	}
	return output;
}

std::int16_t alpha_slot_for(
	const Runtime& mission,
	const game::World& world,
	std::uint16_t live_index)
{
	const auto slots = alpha_slots(mission, world);
	for (std::uint8_t index = 0; index < slots.size(); ++index)
	{
		if (slots[index] == live_index)
		{
			return index;
		}
	}
	return -1;
}

void append_option(
	PlayerCommsState& state,
	std::uint16_t language_id,
	std::int16_t command,
	std::int16_t target,
	std::uint16_t secondary_language_id = UINT16_MAX)
{
	if (state.option_count >= static_cast<std::int16_t>(
		std::size(state.options)))
	{
		return;
	}
	CommsMenuOption& option = state.options[state.option_count++];
	option = {};
	option.language_id = language_id;
	option.secondary_language_id = secondary_language_id;
	option.command = command;
	option.target = target;
}

void append_named_option(
	PlayerCommsState& state,
	const char* label,
	std::int16_t command,
	std::int16_t target)
{
	const std::int16_t previous_count = state.option_count;
	append_option(state, UINT16_MAX, command, target);
	if (state.option_count == previous_count)
	{
		return;
	}
	std::snprintf(
		state.options[state.option_count - 1].label,
		sizeof(state.options[state.option_count - 1].label),
		"%s",
		label == nullptr ? "" : label);
}

bool target_reference_valid_for_comms(
	const game::World& world,
	game::ObjectHandle target,
	std::int16_t component,
	std::uint32_t allowed_runtime_flags = 0)
{
	const game::WorldObject* object =
		game::world_resolve(world, target);
	if (object == nullptr
		|| (object->runtime_flags & 0x00000200u) == 0
		|| (object->runtime_flags
			& ~allowed_runtime_flags
			& 0x10000d40u) != 0)
	{
		return false;
	}
	return component < 0
		|| (component < object->component_count
			&& object->components[component].model_reference >= 0
			&& static_cast<std::size_t>(
				object->components[component].model_reference)
				< object->model_references.size()
			&& (object->components[component].runtime_flags
				& 0x0030u) == 0);
}

const ai::Command* player_control_command(
	const game::WorldObject& player)
{
	for (std::uint8_t index = 0;
		index < player.ai.command_count;
		++index)
	{
		if (player.ai.commands[index].id == 100)
		{
			return &player.ai.commands[index];
		}
	}
	return nullptr;
}

struct PlayerTargetReference
{
	const game::WorldObject* object{};
	std::uint16_t world_index{UINT16_MAX};
	std::int16_t component{-1};
};

bool player_control_target(
	const game::World& world,
	PlayerTargetReference& target)
{
	target = {};
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (player == nullptr)
	{
		return false;
	}
	const ai::Command* control = player_control_command(*player);
	if (control == nullptr)
	{
		return false;
	}
	target.object = resolve_command_target(world, *control);
	target.world_index = target.object == nullptr
		? UINT16_MAX
		: world_index(world, *target.object);
	target.component = control->target_component;
	return true;
}

bool player_control_target_valid(
	const game::World& world,
	const game::WorldObject*& selected)
{
	PlayerTargetReference target;
	if (!player_control_target(world, target) || target.object == nullptr)
	{
		selected = nullptr;
		return false;
	}
	selected = target.object;
	return target_reference_valid_for_comms(
		world,
		game::ObjectHandle{
			target.world_index, selected->generation},
		target.component);
}

void build_remote_command_prompt(PlayerCommsState& state)
{
	state.option_count = 0;
	state.title_language_id =
		state.remote_command_kind == 1
			? 353
			: state.remote_command_kind == 2
				? 354
				: 355;
	append_option(state, 372, 27, 0);
	append_option(state, 373, 28, 0);
}

bool selected_hostile_target(
	const game::World& world,
	const game::WorldObject*& selected,
	bool require_fighter_class)
{
	return player_control_target_valid(world, selected)
		&& selected->allegiance_class == 1
		&& (!require_fighter_class
			|| (selected->collision_class >= 1
				&& selected->collision_class <= 3));
}

void rebuild_menu(
	Runtime& mission,
	game::World& world,
	const assets::ShipStatsTable& stats)
{
	(void)stats;
	PlayerCommsState& state = mission.player_comms;
	do
	{
		state.option_count = 0;
		if (!state.menu_open)
		{
			return;
		}
		switch (state.current_command)
		{
		case -1:
		{
			state.title_language_id = 332;
			const game::WorldObject* selected = nullptr;
			if (selected_hostile_target(world, selected, true))
			{
				append_option(
					state, 333, 0,
					static_cast<std::int16_t>(
						world_index(world, *selected)));
			}
			for (const std::uint16_t live : alpha_slots(mission, world))
			{
				if (live == UINT16_MAX
					|| live == world.player.index)
				{
					continue;
				}
				const game::WorldObject& wing = world.objects[live];
				if ((wing.runtime_flags & game::kObjectFlagDestroyed) == 0)
				{
					append_option(state, 334, 1, 0);
					break;
				}
			}
			if (!mission.network.deathmatch_mode)
			{
				append_option(state, 335, 2, 0);
			}
			if (mission.network.role != NetworkRole::offline)
			{
				append_option(state, 365, 22, 0);
			}
			return;
		}
		case 0:
		case 4:
		{
			if (state.current_target < 0
				|| state.current_target >= static_cast<std::int16_t>(
					std::size(world.objects)))
			{
				break;
			}
			const game::WorldObject& target =
				world.objects[state.current_target];
			if ((target.runtime_flags & game::kObjectFlagDestroyed) != 0)
			{
				break;
			}
			const std::int16_t alpha =
				alpha_slot_for(
					mission, world,
					static_cast<std::uint16_t>(state.current_target));
			state.title_language_id =
				alpha >= 0
					? static_cast<std::uint16_t>(338 + alpha)
					: 351;
			if (target.allegiance_class == 1)
			{
				for (std::int16_t command = 6; command <= 10; ++command)
				{
					append_option(
						state,
						static_cast<std::uint16_t>(354 + command),
						command,
						state.current_target);
				}
				return;
			}
			const bool alpha_fighter =
				target.mission_group_class == 0;
			const game::WorldObject* selected = nullptr;
			if (alpha_fighter
				&& selected_hostile_target(world, selected, false))
			{
				append_option(state, 353, 11, state.current_target);
				append_option(state, 354, 12, state.current_target);
				append_option(state, 355, 13, state.current_target);
			}
			if (alpha_fighter)
			{
				append_option(state, 356, 14, state.current_target);
			}
			append_option(state, 357, 15, state.current_target);
			append_option(state, 358, 16, state.current_target);
			return;
		}
		case 1:
		{
			state.title_language_id = 337;
			std::uint8_t count = 0;
			const auto slots = alpha_slots(mission, world);
			for (std::uint8_t slot = 0; slot < slots.size(); ++slot)
			{
				const std::uint16_t live = slots[slot];
				if (live == UINT16_MAX || live == world.player.index)
				{
					continue;
				}
				if (mission.network.role != NetworkRole::offline
					&& slot < mission.network.player_count)
				{
					continue;
				}
				const game::WorldObject& object = world.objects[live];
				const assets::PilotPresentationDefinition* pilot =
					assets::pilot_presentation(object.pilot);
				if ((object.runtime_flags & game::kObjectFlagDestroyed) != 0
					|| pilot == nullptr)
				{
					continue;
				}
				append_option(
					state,
					static_cast<std::uint16_t>(
						pilot->portrait_or_movie_id),
					4,
					static_cast<std::int16_t>(live),
					static_cast<std::uint16_t>(338 + slot));
				++count;
			}
			if (count > 1)
			{
				append_option(state, 344, 5, -1);
			}
			return;
		}
		case 2:
			state.title_language_id = 345;
			if (mission.network.role == NetworkRole::offline
				|| state.multiplayer_landing_enabled)
			{
				append_option(state, 346, 18, 0);
			}
			append_option(state, 347, 19, 0);
			return;
		case 3:
			state.title_language_id = 348;
			append_option(state, 349, 20, 0);
			append_option(state, 350, 21, 0);
			return;
		case 5:
			state.title_language_id = 352;
			append_option(state, 353, 11, -1);
			append_option(state, 354, 12, -1);
			append_option(state, 355, 13, -1);
			return;
		case 22:
			state.title_language_id = 367;
			for (std::uint8_t player = 0;
				player < mission.network.player_count;
				++player)
			{
				if (player == mission.network.local_player
					|| player >= std::size(world.objects)
					|| (world.objects[player].runtime_flags
						& game::kObjectFlagDestroyed) != 0)
				{
					continue;
				}
				append_named_option(
					state,
					mission.network.player_name[player],
					23,
					static_cast<std::int16_t>(player));
			}
			append_option(state, 366, 23, -1);
			return;
		case 23:
		{
			state.title_language_id = 367;
			if (state.current_target != -1)
			{
				if (state.current_target < 0
					|| state.current_target
						>= mission.network.player_count
					|| state.current_target
						>= static_cast<std::int16_t>(
							std::size(world.objects)))
				{
					break;
				}
				const game::WorldObject& remote =
					world.objects[state.current_target];
				const game::WorldObject* selected = nullptr;
				if (remote.mission_group_class == 0
					&& player_control_target_valid(world, selected)
					&& selected->allegiance_class == 1)
				{
					append_option(
						state, 353, 11,
						state.current_target);
					append_option(
						state, 354, 12,
						state.current_target);
					append_option(
						state, 355, 13,
						state.current_target);
				}
				append_option(state, 357, 15, -2);
				append_option(state, 358, 16, -2);
				append_option(
					state, 374, 24,
					state.current_target);
				return;
			}
			append_option(state, 353, 11, -3);
			append_option(state, 354, 12, -3);
			append_option(state, 355, 13, -3);
			append_option(state, 374, 25, -1);
			return;
		}
		case 26:
			build_remote_command_prompt(state);
			return;
		default:
			return;
		}
		state.current_command = -1;
		state.current_target = -1;
	} while (true);
}

bool uses_short_ack_set(const game::WorldObject& object)
{
	switch (object.pilot)
	{
	case 0:
	case 1:
	case 6:
	case 7:
	case 0x1d:
	case 0x5f:
	case 0x78:
		return true;
	default:
		return false;
	}
}

PendingCommsResponse* allocate_response(PlayerCommsState& state)
{
	for (PendingCommsResponse& response : state.responses)
	{
		if (response.active == 0)
		{
			response = {};
			response.active = 1;
			response.speaker_id = -1;
			response.listener_id = -1;
			response.response_flag = 1;
			response.unknown_0e = -1;
			response.static_voice_id = -1;
			response.expires_at = -1;
			return &response;
		}
	}
	return nullptr;
}

bool schedule_live_response(
	Runtime& mission,
	game::World& world,
	std::uint16_t speaker,
	const char* suffix,
	std::uint32_t expires_at)
{
	if (speaker >= std::size(world.objects) || suffix == nullptr)
	{
		return false;
	}
	const game::WorldObject& object = world.objects[speaker];
	const assets::PilotPresentationDefinition* pilot =
		assets::pilot_presentation(object.pilot);
	PendingCommsResponse* response =
		allocate_response(mission.player_comms);
	if (!object.active || pilot == nullptr
		|| pilot->movies[0] == nullptr || response == nullptr)
	{
		if (response != nullptr)
		{
			response->active = 0;
		}
		return false;
	}
	char voice[50];
	if (!format_live_voice(object, suffix, voice))
	{
		response->active = 0;
		return false;
	}
	response->speaker_id = speaker;
	response->listener_id = world.player.index;
	response->expires_at = static_cast<std::int32_t>(expires_at);
	std::snprintf(
		response->fm8_path, sizeof(response->fm8_path),
		"pilots/%s.fm8", pilot->movies[0]);
	std::snprintf(
		response->voice_path, sizeof(response->voice_path), "%s", voice);
	return true;
}

bool schedule_base_response(
	Runtime& mission,
	const game::World& world,
	std::uint16_t carrier,
	const char* suffix,
	std::uint32_t expires_at)
{
	PendingCommsResponse* response =
		allocate_response(mission.player_comms);
	if (response == nullptr || suffix == nullptr)
	{
		return false;
	}
	const bool has_carrier = carrier < std::size(world.objects);
	const bool yamato_speaker = has_carrier
		&& world.objects[carrier].type == 0x0d;
	const bool yamato_voice = has_carrier
		&& (yamato_speaker
			|| (world.objects[carrier].runtime_flags & game::kObjectFlagDestroyed) != 0);
	response->speaker_id = yamato_speaker ? 0x10053 : 0x1003b;
	response->listener_id = world.player.index;
	response->static_voice_id = 68;
	response->expires_at = static_cast<std::int32_t>(expires_at);
	std::snprintf(
		response->fm8_path, sizeof(response->fm8_path),
		"%s", yamato_voice
			? "pilots/Yam_Brdge_Off.fm8"
			: "pilots/Rel_Brdge_Off.fm8");
	std::snprintf(
		response->voice_path, sizeof(response->voice_path),
		yamato_voice ? "yam%s" : "rel%s", suffix);
	return true;
}

void replace_player_command_voice(
	Runtime& mission,
	const char* suffix)
{
	// CommsVoice_play_player_line (LANCER.EXE 0x004566c0) suppresses the
	// local player's command speech in deathmatch without suppressing the
	// command or its network packet.
	if (suffix == nullptr || mission.network.deathmatch_mode)
	{
		return;
	}
	std::snprintf(
		mission.presentation.command_speech_path,
		sizeof(mission.presentation.command_speech_path),
		mission.player_pilot_family == 0
			? "ms_speech/mp%s"
			: "ms_speech/fp%s",
		suffix);
	char* extension = std::strrchr(
		mission.presentation.command_speech_path, '.');
	if (extension != nullptr
		&& (extension[1] == 'u' || extension[1] == 'U')
		&& (extension[2] == 't' || extension[2] == 'T')
		&& extension[3] == '\0')
	{
		*extension = '\0';
	}
	mission.presentation.command_speech_pending = true;
}

std::uint16_t find_carrier(
	const PlayerCommsState& state,
	const game::World& world)
{
	return state.carrier_world_index < std::size(world.objects)
		? state.carrier_world_index
		: UINT16_MAX;
}

void update_multiplayer_comms_state(
	Runtime& mission,
	game::World& world)
{
	PlayerCommsState& state = mission.player_comms;
	if (mission.network.role != NetworkRole::offline
		&& !state.multiplayer_landing_enabled)
	{
		for (std::uint8_t player = 0;
			player < mission.network.player_count;
			++player)
		{
			if ((world.objects[player].runtime_flags & 0x10000840u) != 0)
			{
				continue;
			}
			if (player == mission.network.local_player)
			{
				state.multiplayer_landing_enabled = true;
				if (player != 0 && !mission.network.deathmatch_mode)
				{
					// Multiplayer_frame_update, LANCER.EXE
					// 0x00492527..0x0049253e, posts language 0x558 only
					// when a non-host co-op player becomes the first live
					// player and gains landing authority.
					state.multiplayer_landing_notice_pending = true;
				}
			}
			break;
		}
	}

	if (state.carrier_world_index >= std::size(world.objects))
	{
		return;
	}
	const game::WorldObject& carrier =
		world.objects[state.carrier_world_index];
	if ((carrier.runtime_flags & game::kObjectFlagDestroyed) == 0)
	{
		return;
	}
	const std::uint16_t high_water = std::min<std::uint16_t>(
		world.object_high_water,
		static_cast<std::uint16_t>(std::size(world.objects)));
	for (std::uint16_t index = 0; index < high_water; ++index)
	{
		if (world.objects[index].type == 0x0d)
		{
			state.carrier_world_index = index;
			break;
		}
	}
}

std::uint16_t choose_uniform(
	game::World& world,
	const std::uint16_t* values,
	std::uint8_t count)
{
	return count == 0 ? UINT16_MAX : values[comms_rand15(world) % count];
}

std::uint16_t choose_distance_weighted(
	game::World& world,
	const game::WorldObject& player,
	const std::uint16_t* values,
	std::uint8_t count)
{
	if (count == 0)
	{
		return UINT16_MAX;
	}
	float cumulative[6]{};
	float total = 0.0f;
	for (std::uint8_t index = 0; index < count; ++index)
	{
		total += glm::length(
			world.objects[values[index]].position - player.position);
		cumulative[index] = total;
	}
	if (total <= 0.0f)
	{
		return choose_uniform(world, values, count);
	}
	const float draw =
		static_cast<float>(comms_rand15(world)) / 32768.0f * total;
	for (std::uint8_t index = 0; index + 1 < count; ++index)
	{
		if (draw < cumulative[index])
		{
			return values[index];
		}
	}
	return values[count - 1];
}

template<std::size_t N>
const char* response_line(
	game::World& world,
	bool short_set,
	const char* const (&short_lines)[N],
	const char* const* normal_lines,
	std::size_t normal_count)
{
	return short_set
		? short_lines[comms_rand15(world) % N]
		: normal_lines[comms_rand15(world) % normal_count];
}

std::uint8_t collect_recipients(
	const Runtime& mission,
	const game::World& world,
	std::int16_t requested,
	std::uint16_t (&output)[6])
{
	if (requested >= 0)
	{
		if (requested < static_cast<std::int16_t>(std::size(world.objects)))
		{
			output[0] = static_cast<std::uint16_t>(requested);
			return 1;
		}
		return 0;
	}
	// -1 is the Alpha-flight aggregate. Multiplayer command dispatch uses
	// distinct -2 (selected remote player) and -3 (all players) sentinels;
	// neither may fall through to the local wingman collector.
	if (requested != -1)
	{
		return 0;
	}
	std::uint8_t count = 0;
	for (const std::uint16_t live : alpha_slots(mission, world))
	{
		if (live != UINT16_MAX && live != world.player.index
			&& (world.objects[live].runtime_flags & game::kObjectFlagDestroyed) == 0
			&& count < std::size(output))
		{
			output[count++] = live;
		}
	}
	return count;
}

std::uint8_t classify_attack(
	const game::WorldObject& recipient,
	const game::World& world,
	const PlayerTargetReference& selected)
{
	// CommsCommand_classify_attack_recipient (LANCER.EXE 0x00454b80)
	// classifies the Executor DoNotDisturb flag as unavailable.
	if (recipient.do_not_disturb
		|| ai::command_has_positive_priority(recipient))
	{
		return 1;
	}
	if (recipient.ai.command_count != 0)
	{
		const ai::Command& active = recipient.ai.commands[0];
		if (selected.object != nullptr
			&& resolve_command_target(world, active)
				== selected.object
			&& active.target_component == selected.component)
		{
			return 2;
		}
	}
	return 0;
}

std::uint8_t classify_backoff(
	const game::WorldObject& recipient,
	const game::World& world,
	const PlayerTargetReference& selected)
{
	// CommsCommand_classify_backoff_recipient (LANCER.EXE 0x00454f40)
	// classifies the Executor DoNotDisturb flag as unavailable.
	if (recipient.do_not_disturb)
	{
		return 1;
	}
	if (recipient.ai.command_count == 0)
	{
		return 2;
	}
	const ai::Command& active = recipient.ai.commands[0];
	return active.id == 105
		&& selected.object != nullptr
		&& resolve_command_target(world, active)
			== selected.object
		? 0
		: 2;
}

bool command_targets_any(
	const game::WorldObject& object,
	const game::World& world,
	const std::uint16_t* live_targets,
	std::uint8_t count)
{
	if (object.ai.command_count == 0)
	{
		return false;
	}
	const ai::Command& active = object.ai.commands[0];
	if (active.id != 105)
	{
		return false;
	}
	const game::WorldObject* target =
		resolve_command_target(world, active);
	if (target == nullptr)
	{
		return false;
	}
	const std::uint16_t live_target = world_index(world, *target);
	for (std::uint8_t index = 0; index < count; ++index)
	{
		if (live_target == live_targets[index])
		{
			return true;
		}
	}
	return false;
}

std::uint8_t classify_help(
	const game::WorldObject& recipient,
	const game::World& world,
	const std::uint16_t* targets,
	std::uint8_t count)
{
	// CommsCommand_classify_help_recipient (LANCER.EXE 0x00455280)
	// classifies the Executor DoNotDisturb flag as unavailable.
	if (recipient.do_not_disturb
		|| ai::command_has_positive_priority(recipient))
	{
		return 1;
	}
	return command_targets_any(recipient, world, targets, count) ? 2 : 0;
}

std::uint16_t choose_recipient(
	game::World& world,
	const game::WorldObject& player,
	const std::uint16_t* candidates,
	const std::uint8_t* classifications,
	std::uint8_t count,
	bool distance_weighted)
{
	std::uint16_t eligible[6]{};
	std::uint8_t eligible_count = 0;
	for (std::uint8_t index = 0; index < count; ++index)
	{
		if (classifications[index] == 0)
		{
			eligible[eligible_count++] = candidates[index];
		}
	}
	return distance_weighted
		? choose_distance_weighted(
			world, player, eligible, eligible_count)
		: choose_uniform(world, eligible, eligible_count);
}

void publish_remote_player_command(
	Runtime& mission,
	std::int16_t requested,
	NetworkGameplayOpcode opcode,
	std::uint16_t target)
{
	if (requested != -2 && requested != -3)
	{
		return;
	}
	const std::int16_t destination =
		requested == -2
			? mission.player_comms.remote_selected_player
			: -1;
	if (requested == -2 && destination < 0)
	{
		return;
	}
	(void)network_publish_player_comms_command(
		mission.network,
		opcode,
		destination,
		target);
}

void execute_attack(
	Runtime& mission,
	game::World& world,
	const assets::PilotStatsTable& pilot_stats,
	std::int16_t requested,
	std::uint32_t gameplay_tick)
{
	replace_player_command_voice(mission, "hud_001.ut");
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	PlayerTargetReference selected;
	if (player == nullptr || !player_control_target(world, selected))
	{
		return;
	}
	if (requested == -2 || requested == -3)
	{
		publish_remote_player_command(
			mission,
			requested,
			NetworkGameplayOpcode::player_attack_request,
			selected.world_index);
		return;
	}
	std::uint16_t recipients[6]{};
	const std::uint8_t count =
		collect_recipients(mission, world, requested, recipients);
	std::uint8_t classes[6]{};
	for (std::uint8_t index = 0; index < count; ++index)
	{
		classes[index] = classify_attack(
			world.objects[recipients[index]], world, selected);
	}
	std::uint16_t chosen = requested >= 0
		? (count == 0 ? UINT16_MAX : recipients[0])
		: choose_recipient(
			world, *player, recipients, classes, count, true);
	if (chosen == UINT16_MAX)
	{
		return;
	}
	game::WorldObject& recipient = world.objects[chosen];
	const std::uint8_t classification =
		classify_attack(recipient, world, selected);
	const bool short_set = uses_short_ack_set(recipient);
	const char* suffix = nullptr;
	if (classification == 1)
	{
		suffix = response_line(
			world, short_set,
			kAttackRejectedShort,
			kAttackRejectedNormal,
			std::size(kAttackRejectedNormal));
	}
	else
	{
		if (classification == 0)
		{
			const std::int16_t behavior =
				recipient.pilot < assets::kPilotStatsCount
					&& pilot_stats.ready
					? pilot_stats.records[recipient.pilot].behavior_20
					: -1;
			if (behavior >= 0 && behavior <= 2)
			{
				(void)comms_rand15(world);
			}
			ai::command_push(world,
				recipient, 105, ai::TargetKind::world_object,
				selected.world_index,
				selected.component);
		}
		suffix = response_line(
			world, short_set,
			kAttackAcceptedShort,
			kAttackAcceptedNormal,
			std::size(kAttackAcceptedNormal));
	}
	schedule_live_response(
		mission, world, chosen, suffix, gameplay_tick + 300);
	diagnostics::mission_log(
		"comms command=attack recipient=%u class=%u target=%u component=%d",
		static_cast<unsigned>(recipient.mission_index),
		static_cast<unsigned>(classification),
		static_cast<unsigned>(selected.world_index),
		static_cast<int>(selected.component));
}

void execute_backoff(
	Runtime& mission,
	game::World& world,
	const assets::PilotStatsTable& pilot_stats,
	std::int16_t requested,
	std::uint32_t gameplay_tick)
{
	replace_player_command_voice(mission, "hud_002.ut");
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	PlayerTargetReference selected;
	if (player == nullptr || !player_control_target(world, selected))
	{
		return;
	}
	if (requested == -2 || requested == -3)
	{
		publish_remote_player_command(
			mission,
			requested,
			NetworkGameplayOpcode::player_backoff_request,
			selected.world_index);
		return;
	}
	std::uint16_t recipients[6]{};
	const std::uint8_t count =
		collect_recipients(mission, world, requested, recipients);
	std::uint8_t classes[6]{};
	for (std::uint8_t index = 0; index < count; ++index)
	{
		classes[index] = classify_backoff(
			world.objects[recipients[index]], world, selected);
	}
	const std::uint16_t chosen = requested >= 0
		? (count == 0 ? UINT16_MAX : recipients[0])
		: choose_recipient(
			world, *player, recipients, classes, count, false);
	if (chosen == UINT16_MAX)
	{
		return;
	}
	game::WorldObject& recipient = world.objects[chosen];
	const std::uint8_t classification =
		classify_backoff(recipient, world, selected);
	const bool short_set = uses_short_ack_set(recipient);
	const char* suffix = nullptr;
	if (classification == 1)
	{
		suffix = response_line(
			world, short_set,
			kBackoffRejectedShort,
			kBackoffRejectedNormal,
			std::size(kBackoffRejectedNormal));
	}
	else
	{
		if (classification == 0)
		{
			const std::int16_t behavior =
				recipient.pilot < assets::kPilotStatsCount
					&& pilot_stats.ready
					? pilot_stats.records[recipient.pilot].behavior_20
					: -1;
			if (behavior >= 0 && behavior <= 2)
			{
				(void)comms_rand15(world);
			}
			ai::command_pop(world, recipient);
		}
		// CommsCommand_send_backoff writes the same retained exclusion pair
		// consumed by AI_FindNewTarget_candidate_visitor. Authored targets use
		// their mission-object index; runtime-only targets retain the live slot.
		recipient.find_target_exclusion_index =
			selected.object->mission_index != UINT16_MAX
				? selected.object->mission_index
				: selected.world_index;
		recipient.find_target_exclusion_deadline = gameplay_tick + 3000;
		suffix = response_line(
			world, short_set,
			kBackoffAcceptedShort,
			kBackoffAcceptedNormal,
			std::size(kBackoffAcceptedNormal));
	}
	schedule_live_response(
		mission, world, chosen, suffix, gameplay_tick + 300);
	diagnostics::mission_log(
		"comms command=backoff recipient=%u class=%u",
		static_cast<unsigned>(recipient.mission_index),
		static_cast<unsigned>(classification));
}

std::uint8_t collect_help_targets(
	const game::World& world,
	const game::WorldObject& player,
	std::uint16_t (&targets)[100])
{
	std::uint8_t count = 0;
	const std::uint16_t high_water = std::min<std::uint16_t>(
		world.object_high_water,
		static_cast<std::uint16_t>(std::size(world.objects)));
	for (std::uint16_t index = 0; index < high_water; ++index)
	{
		const game::WorldObject& object = world.objects[index];
		if (object.allegiance_class != 1
			|| !target_reference_valid_for_comms(
				world,
				game::ObjectHandle{index, object.generation},
				-1)
			|| object.ai.command_count == 0)
		{
			continue;
		}
		const ai::Command& active = object.ai.commands[0];
		if (active.id == 105
			&& resolve_command_target(world, active) == &player
			&& count < std::size(targets))
		{
			targets[count++] = world_index(world, object);
		}
	}
	if (count == 0)
	{
		PlayerTargetReference selected;
		if (!player_control_target(world, selected)
			|| selected.object == nullptr)
		{
			return 0;
		}
		if (target_reference_valid_for_comms(
				world,
				game::ObjectHandle{
					selected.world_index,
					selected.object->generation},
				selected.component)
			&& selected.object->allegiance_class == 1)
		{
			targets[count++] = selected.world_index;
		}
	}
	return count;
}

void execute_help(
	Runtime& mission,
	game::World& world,
	const assets::PilotStatsTable& pilot_stats,
	std::int16_t requested,
	std::uint32_t gameplay_tick)
{
	replace_player_command_voice(mission, "hud_003.ut");
	if (requested == -2 || requested == -3)
	{
		publish_remote_player_command(
			mission,
			requested,
			NetworkGameplayOpcode::player_help_request,
			UINT16_MAX);
		return;
	}
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (player == nullptr)
	{
		return;
	}
	std::uint16_t targets[100]{};
	const std::uint8_t target_count =
		collect_help_targets(world, *player, targets);
	if (target_count == 0)
	{
		return;
	}
	std::uint16_t recipients[6]{};
	const std::uint8_t count =
		collect_recipients(mission, world, requested, recipients);
	std::uint8_t classes[6]{};
	for (std::uint8_t index = 0; index < count; ++index)
	{
		classes[index] = classify_help(
			world.objects[recipients[index]],
			world,
			targets,
			target_count);
	}
	const std::uint16_t chosen = requested >= 0
		? (count == 0 ? UINT16_MAX : recipients[0])
		: choose_recipient(
			world, *player, recipients, classes, count, true);
	if (chosen == UINT16_MAX)
	{
		return;
	}
	game::WorldObject& recipient = world.objects[chosen];
	const std::uint8_t classification =
		classify_help(recipient, world, targets, target_count);
	const bool short_set = uses_short_ack_set(recipient);
	const char* suffix = nullptr;
	if (classification == 1)
	{
		suffix = response_line(
			world, short_set,
			kHelpRejectedShort,
			kHelpRejectedNormal,
			std::size(kHelpRejectedNormal));
	}
	else
	{
		if (classification == 0)
		{
			const std::int16_t behavior =
				recipient.pilot < assets::kPilotStatsCount
					&& pilot_stats.ready
					? pilot_stats.records[recipient.pilot].behavior_20
					: -1;
			if (behavior >= 0 && behavior <= 2)
			{
				(void)comms_rand15(world);
			}
			ai::command_push(world,
				recipient, 105, ai::TargetKind::world_object,
				targets[comms_rand15(world) % target_count], -1);
		}
		suffix = response_line(
			world, short_set,
			kHelpAcceptedShort,
			kHelpAcceptedNormal,
			std::size(kHelpAcceptedNormal));
	}
	schedule_live_response(
		mission, world, chosen, suffix, gameplay_tick + 300);
	diagnostics::mission_log(
		"comms command=help recipient=%u class=%u threats=%u",
		static_cast<unsigned>(recipient.mission_index),
		static_cast<unsigned>(classification),
		static_cast<unsigned>(target_count));
}

void execute_status(
	Runtime& mission,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const assets::PilotStatsTable& pilots,
	std::int16_t requested,
	std::uint32_t gameplay_tick)
{
	replace_player_command_voice(mission, "hud_004.ut");
	if (requested < 0
		|| requested >= static_cast<std::int16_t>(std::size(world.objects)))
	{
		return;
	}
	const game::WorldObject& recipient = world.objects[requested];
	if (recipient.pilot >= assets::kPilotStatsCount
		|| !pilots.ready
		|| assets::pilot_presentation(recipient.pilot) == nullptr
		|| pilots.records[recipient.pilot].behavior_1e <= 0
		|| recipient.type >= assets::kShipStatsCount)
	{
		return;
	}
	const float maximum = static_cast<float>(
		stats.records[recipient.type].object.structural_bank_max * 6);
	if (maximum <= 0.0f)
	{
		return;
	}
	const float total = recipient.secondary_shields[3]
		+ recipient.secondary_shields[2]
		+ recipient.secondary_shields[1]
		+ recipient.secondary_shields[0];
	const std::uint8_t group = static_cast<std::uint8_t>(
		std::clamp(
			static_cast<int>(total / maximum) - 1,
			0, 2));
	const bool short_set = uses_short_ack_set(recipient);
	const char* suffix = short_set
		? kStatusShort[group][
			comms_rand15(world) % kStatusShortCount[group]]
		: kStatusNormal[group][
			comms_rand15(world) % kStatusNormalCount[group]];
	schedule_live_response(
		mission, world,
		static_cast<std::uint16_t>(requested),
		suffix,
		gameplay_tick + 250);
}

void execute_opinion(
	Runtime& mission,
	game::World& world,
	std::int16_t requested,
	bool praise,
	std::uint32_t gameplay_tick)
{
	replace_player_command_voice(
		mission, praise ? "hud_006.ut" : "hud_005.ut");
	if (requested < 0
		|| requested >= static_cast<std::int16_t>(std::size(world.objects)))
	{
		return;
	}
	const game::WorldObject& recipient = world.objects[requested];
	const bool short_set = uses_short_ack_set(recipient);
	const char* suffix = nullptr;
	if (praise)
	{
		suffix = response_line(
			world, short_set,
			kPraiseShort, kPraiseNormal, std::size(kPraiseNormal));
	}
	else
	{
		suffix = response_line(
			world, short_set,
			kCriticizeShort,
			kCriticizeNormal,
			std::size(kCriticizeNormal));
	}
	schedule_live_response(
		mission, world,
		static_cast<std::uint16_t>(requested),
		suffix,
		gameplay_tick + 300);
}

bool schedule_enemy_taunt_response(
	Runtime& mission,
	game::World& world,
	std::uint16_t target,
	std::uint32_t gameplay_tick)
{
	if (target >= std::size(world.objects))
	{
		return false;
	}
	const game::WorldObject& enemy = world.objects[target];
	char suffix[32];
	const char* prefix = nullptr;
	std::uint8_t count = 0;
	// CommsCommand_send_taunt switches GameObject+0x740, the same pilot
	// identity consumed by the live-prefix formatter, not the leading
	// ship-type dword.
	switch (enemy.pilot)
	{
	case 0x0a: prefix = "hs_res_"; count = 7; break;
	case 0x0f: prefix = "ip_res_"; count = 14; break;
	case 0x10: prefix = "np_res_"; count = 10; break;
	case 0x32: prefix = "cm_res_"; count = 10; break;
	case 0x83: prefix = "al_res_"; count = 11; break;
	case 0x85: prefix = "rd_res_"; count = 6; break;
	default:
		return schedule_live_response(
			mission, world, target,
			random_line(world, kEnemyResponseGeneric),
			gameplay_tick + 300);
	}
	std::snprintf(
		suffix, sizeof(suffix), "%s%03u.ut",
		prefix,
		static_cast<unsigned>(comms_rand15(world) % count + 1));
	const assets::PilotPresentationDefinition* pilot =
		assets::pilot_presentation(enemy.pilot);
	PendingCommsResponse* response =
		allocate_response(mission.player_comms);
	if (pilot == nullptr || pilot->movies[0] == nullptr
		|| response == nullptr)
	{
		if (response != nullptr) response->active = 0;
		return false;
	}
	response->speaker_id = target;
	response->listener_id = world.player.index;
	response->expires_at = static_cast<std::int32_t>(gameplay_tick + 300);
	std::snprintf(
		response->fm8_path, sizeof(response->fm8_path),
		"pilots/%s.fm8", pilot->movies[0]);
	std::snprintf(
		response->voice_path, sizeof(response->voice_path), "%s", suffix);
	return true;
}

void execute_taunt(
	Runtime& mission,
	game::World& world,
	const assets::PilotStatsTable& pilot_stats,
	std::int16_t requested,
	std::int16_t command,
	std::uint32_t gameplay_tick)
{
	char player_line[16];
	std::snprintf(
		player_line, sizeof(player_line), "hud_%03d.ut", command + 1);
	replace_player_command_voice(mission, player_line);
	if (requested < 0
		|| requested >= static_cast<std::int16_t>(std::size(world.objects)))
	{
		return;
	}
	game::WorldObject& target = world.objects[requested];
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	// CommsCommand_send_taunt (LANCER.EXE 0x00454870) rejects the
	// Executor DoNotDisturb flag before enqueueing Fight or a response.
	if (target.do_not_disturb
		|| target.collision_class != 1 || player == nullptr
		|| ai::command_has_positive_priority(target)
		|| (target.ai.command_count != 0
			&& resolve_command_target(world, target.ai.commands[0]) == player))
	{
		return;
	}
	const std::int16_t behavior =
		target.pilot < assets::kPilotStatsCount && pilot_stats.ready
			? pilot_stats.records[target.pilot].behavior_20
			: -1;
	if (behavior == 0 || behavior == 1)
	{
		(void)comms_rand15(world);
	}
	ai::command_push(world,
		target, 105, ai::TargetKind::world_object,
		world.player.index, -1);
	schedule_enemy_taunt_response(
		mission, world,
		static_cast<std::uint16_t>(requested),
		gameplay_tick);
	diagnostics::mission_log(
		"comms command=taunt variant=%d target=%u",
		static_cast<int>(command - 5),
		static_cast<unsigned>(target.mission_index));
}

void execute_backup(
	Runtime& mission,
	game::World& world,
	std::uint32_t gameplay_tick)
{
	update_multiplayer_comms_state(mission, world);
	replace_player_command_voice(mission, "hud_013.ut");
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (player == nullptr)
	{
		return;
	}
	// FUN_004558d0 gates the request with DAT_0052a3fc, mission
	// session-state dword three. The PlayerWantsBackup trigger consumes the
	// accepted request; its presence is not what makes backup available.
	const bool available =
		mission.session_state[3] != 0
		&& !mission.player_comms.backup_requested;
	if (available)
	{
		mission.player_comms.backup_requested = true;
		events_emit_direct(
			mission,
			EventType::player_wants_backup,
			player->mission_index,
			nullptr,
			0);
	}
	const std::uint16_t carrier = find_carrier(mission.player_comms, world);
	schedule_base_response(
		mission, world, carrier,
		available
			? random_line(world, kBackupAccepted)
			: random_line(world, kBackupDenied),
		gameplay_tick + 300);
	diagnostics::mission_log(
		"comms command=backup available=%u carrier=%u",
		available ? 1u : 0u,
		carrier < std::size(world.objects)
			? static_cast<unsigned>(world.objects[carrier].mission_index)
			: static_cast<unsigned>(UINT16_MAX));
}

void execute_landing(
	Runtime& mission,
	game::World& world,
	std::uint32_t gameplay_tick,
	bool network_forced = false)
{
	update_multiplayer_comms_state(mission, world);
	PlayerCommsState& state = mission.player_comms;
	if (!network_forced && gameplay_tick < state.next_landing_request)
	{
		return;
	}
	if (!network_forced)
	{
		state.next_landing_request = gameplay_tick + 500;
	}
	game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (player == nullptr)
	{
		return;
	}
	const std::uint16_t carrier = find_carrier(state, world);
	if (network_forced)
	{
		const std::int32_t grade = std::bit_cast<std::int32_t>(
			mission.session_state[14]);
		const char* response = grade == 1 || grade == 2
			? random_line(world, kLandingSuccess12)
			: grade == 3 || grade == 4
				? random_line(world, kLandingSuccess34)
				: random_line(world, kLandingSuccessOther);
		schedule_base_response(
			mission, world, carrier, response, gameplay_tick + 300);
		if (carrier < std::size(world.objects))
		{
			ai::command_push(world,
				*player, 8, ai::TargetKind::world_object,
				carrier, -1);
		}
		return;
	}
	const bool training = mission.object_factory_mode == 1
		|| (mission.mission_number >= 30 && mission.mission_number <= 35);
	if (training)
	{
		if (!return_to_base_requested(mission))
		{
			return;
		}
		replace_player_command_voice(mission, "hud_012.ut");
		PendingCommsResponse* response = allocate_response(state);
		if (response != nullptr)
		{
			response->speaker_id = 0x10051;
			response->listener_id = world.player.index;
			response->static_voice_id = 256;
			response->expires_at =
				static_cast<std::int32_t>(gameplay_tick + 300);
			std::snprintf(
				response->fm8_path, sizeof(response->fm8_path),
				"pilots/VirtFlt_Ins.fm8");
			std::snprintf(
				response->voice_path, sizeof(response->voice_path),
				"trnglnd_001.ut");
		}
		if (carrier < std::size(world.objects))
		{
			ai::command_push(world,
				*player, 8, ai::TargetKind::world_object,
				carrier, -1);
		}
		return;
	}
	if (player->ai.command_count != 0
		&& player->ai.commands[0].id == 8)
	{
		return;
	}
	if (mission.network.role != NetworkRole::offline
		&& !state.multiplayer_landing_enabled)
	{
		return;
	}
	replace_player_command_voice(mission, "hud_012.ut");
	if (!return_to_base_requested(mission))
	{
		schedule_base_response(
			mission, world, carrier,
			random_line(world, kLandingDenied),
			gameplay_tick + 300);
		return;
	}
	if (mission.network.role != NetworkRole::offline)
	{
		(void)network_publish_landing(mission.network);
	}
	const std::int32_t grade = std::bit_cast<std::int32_t>(
		mission.session_state[14]);
	const char* response = nullptr;
	switch (grade)
	{
	case 1:
	case 2:
		response = random_line(world, kLandingSuccess12);
		break;
	case 3:
	case 4:
		response = random_line(world, kLandingSuccess34);
		break;
	default:
		response = random_line(world, kLandingSuccessOther);
		break;
	}
	schedule_base_response(
		mission, world, carrier, response, gameplay_tick + 300);
	if (carrier < std::size(world.objects))
	{
		ai::command_push(world,
			*player, 8, ai::TargetKind::world_object,
			carrier, -1);
	}
	diagnostics::mission_log(
		"comms command=landing accepted carrier=%u grade=%d",
		carrier < std::size(world.objects)
			? static_cast<unsigned>(world.objects[carrier].mission_index)
			: static_cast<unsigned>(UINT16_MAX),
		grade);
}

ai::Command* player_control_command(game::WorldObject& player)
{
	for (std::uint8_t index = 0;
		index < player.ai.command_count;
		++index)
	{
		if (player.ai.commands[index].id == 100)
		{
			return &player.ai.commands[index];
		}
	}
	return nullptr;
}

void write_player_target(
	game::World& world,
	std::uint16_t target,
	std::int16_t component)
{
	game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (player == nullptr)
	{
		return;
	}
	const game::ObjectHandle handle =
		target < std::size(world.objects)
			? game::ObjectHandle{
				target,
				world.objects[target].generation}
			: game::ObjectHandle{target, 0};
	world.selected_target = handle;
	world.target_component = component;
	world.target_panel_refresh_requested = true;
	player->selected_target_index = target;
	player->selected_target_component = component;
	if (ai::Command* command = player_control_command(*player))
	{
		command->target_kind = ai::TargetKind::world_object;
		command->target = target;
		command->target_component = component;
	}
}

void cycle_next_hostile_target(game::World& world)
{
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	const std::uint16_t count =
		std::min<std::uint16_t>(
			world.object_high_water,
			static_cast<std::uint16_t>(
				std::size(world.objects)));
	if (player == nullptr || count == 0)
	{
		write_player_target(world, UINT16_MAX, -1);
		return;
	}
	std::int32_t current =
		world.selected_target.index < count
			? static_cast<std::int32_t>(
				world.selected_target.index)
			: -1;
	for (std::uint16_t attempt = 0; attempt < count; ++attempt)
	{
		current = (current + 1) % count;
		const game::WorldObject& candidate =
			world.objects[current];
		if (!target_reference_valid_for_comms(
				world,
				game::ObjectHandle{
					static_cast<std::uint16_t>(current),
					candidate.generation},
				-1,
				0x800u)
			|| current == world.player.index
			|| candidate.allegiance_class != 1
			|| glm::distance(
				candidate.scene_position,
				player->scene_position) > 660000.0f)
		{
			continue;
		}
		write_player_target(
			world,
			static_cast<std::uint16_t>(current),
			-1);
		return;
	}
	write_player_target(world, UINT16_MAX, -1);
}

void service_remote_player_command(
	PlayerCommsState& state,
	game::World& world)
{
	if (state.remote_response == 0
		|| state.remote_command_kind == 0)
	{
		return;
	}
	if (state.remote_response == 1)
	{
		switch (state.remote_command_kind)
		{
		case 1:
		case 3:
			write_player_target(
				world,
				state.remote_command_target,
				-1);
			break;
		case 2:
			cycle_next_hostile_target(world);
			break;
		default:
			break;
		}
	}
	state.remote_response = 0;
	state.remote_command_kind = 0;
	state.remote_command_source = UINT8_MAX;
	state.remote_command_target = 0;
}
}

const char* player_comms_voice_prefix(
	std::uint16_t pilot,
	bool hostile)
{
	if (!hostile)
	{
		return allied_prefix(pilot);
	}
	const assets::PilotPresentationDefinition* definition =
		assets::pilot_presentation(pilot);
	if (definition == nullptr)
	{
		return nullptr;
	}
	switch (definition->presentation_mode)
	{
	case 5: return "rus";
	case 6: return "chn";
	case 7: return "arb";
	default: return nullptr;
	}
}

void player_comms_reset(PlayerCommsState& state)
{
	state = {};
	state.current_command = -1;
	state.current_target = -1;
	state.remote_selected_player = -1;
	state.chat_destination = -1;
	state.carrier_world_index = UINT16_MAX;
	state.active_speaker = -1;
	state.saved_ejected_pilot = -1;
	state.remote_command_source = UINT8_MAX;
	state.all_channels_open = true;
	for (QueuedComm& line : state.queue)
	{
		line.speaker_id = -1;
		line.expires_at = -1;
	}
	for (PendingCommsResponse& response : state.responses)
	{
		response.speaker_id = -1;
		response.listener_id = -1;
		response.unknown_0e = -1;
		response.static_voice_id = -1;
		response.expires_at = -1;
	}
}

bool player_comms_busy(
	const PlayerCommsState& state,
	const PresentationRequest& presentation)
{
	return presentation.comms_pending || presentation.comms_active
		|| presentation.speech_pending || presentation.speech_active
		|| state.queue_count > 0;
}

bool player_comms_play_or_queue(
	Runtime& mission,
	game::World& world,
	const char* fm8_path,
	const char* voice_path,
	CommsPlaybackMode mode,
	std::int16_t voice_id,
	std::int32_t category,
	std::int32_t speaker_id,
	std::int32_t lifetime,
	std::uint32_t simulation_tick)
{
	if (fm8_path == nullptr || voice_path == nullptr)
	{
		return false;
	}
	// CommsVoice_play_or_queue (LANCER.EXE 0x004562d0) does not consult
	// DisableGenericComms. The latch is tested by autonomous chatter
	// producers before they call this shared playback primitive; authored
	// CommsFromShip/CommsFromPilot commands deliberately bypass it. Mission
	// 29 depends on that distinction: its startup disables generic chatter,
	// then immediately plays the virtual-flight instructor communication.
	PlayerCommsState& state = mission.player_comms;
	if (mode == CommsPlaybackMode::only_if_idle
		&& player_comms_busy(state, mission.presentation))
	{
		return false;
	}
	QueuedComm request;
	request.category = category;
	request.voice_id = voice_id;
	std::snprintf(request.fm8_path, sizeof(request.fm8_path), "%s", fm8_path);
	std::snprintf(request.voice_path, sizeof(request.voice_path), "%s", voice_path);
	request.speaker_id = speaker_id;
	request.expires_at = lifetime < 1
		? -1
		: static_cast<std::int32_t>(simulation_tick + lifetime);
	if (mode == CommsPlaybackMode::immediate)
	{
		mission.presentation.comms_active = false;
		presentation_start(mission, world, request);
		return true;
	}
	if (state.queue_count >= 5)
	{
		return false;
	}
	state.queue[state.queue_write] = request;
	state.queue_write = static_cast<std::int16_t>((state.queue_write + 1) % 5);
	++state.queue_count;
	return true;
}

bool player_comms_play_live_pilot(
	Runtime& mission,
	game::World& world,
	std::uint16_t speaker_world_index,
	std::uint32_t face_variant,
	const char* voice_path,
	CommsPlaybackMode mode,
	std::int32_t category,
	std::int32_t lifetime,
	std::uint32_t simulation_tick)
{
	if (speaker_world_index >= std::size(world.objects))
	{
		return false;
	}
	const game::WorldObject& speaker = world.objects[speaker_world_index];
	const assets::PilotPresentationDefinition* definition =
		assets::pilot_presentation(speaker.pilot);
	if (!speaker.active || speaker.type == 0x3e9
		|| (speaker.runtime_flags & game::kObjectFlagDestroyed) != 0
		|| definition == nullptr || face_variant >= 4
		|| definition->movies[face_variant] == nullptr)
	{
		return false;
	}
	char fm8[50];
	std::snprintf(
		fm8,
		sizeof(fm8),
		"pilots/%s.fm8",
		definition->movies[face_variant]);
	return player_comms_play_or_queue(
		mission,
		world,
		fm8,
		voice_path,
		mode,
		definition->portrait_or_movie_id,
		category,
		speaker_world_index,
		lifetime,
		simulation_tick);
}

bool player_comms_play_compiled_pilot(
	Runtime& mission,
	game::World& world,
	std::uint16_t pilot,
	std::uint32_t face_variant,
	const char* voice_path,
	CommsPlaybackMode mode,
	std::int32_t category,
	std::int32_t lifetime,
	std::uint32_t simulation_tick)
{
	const assets::PilotPresentationDefinition* definition =
		assets::pilot_presentation(pilot);
	if (definition == nullptr || face_variant >= 4
		|| definition->movies[face_variant] == nullptr)
	{
		return false;
	}
	char fm8[50];
	std::snprintf(
		fm8,
		sizeof(fm8),
		"pilots/%s.fm8",
		definition->movies[face_variant]);
	return player_comms_play_or_queue(
		mission,
		world,
		fm8,
		voice_path,
		mode,
		definition->portrait_or_movie_id,
		category,
		static_cast<std::int32_t>(pilot) + 0xffff,
		lifetime,
		simulation_tick);
}

void player_comms_service(
	Runtime& mission,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t gameplay_tick,
	std::uint32_t script_tick,
	std::uint32_t simulation_tick)
{
	PlayerCommsState& state = mission.player_comms;
	update_multiplayer_comms_state(mission, world);
	// MultiplayerCommand_service (LANCER.EXE 0x00477670) consumes the
	// accept/reject latch before the ordinary communications frame work.
	service_remote_player_command(state, world);
	if (state.menu_open)
	{
		rebuild_menu(mission, world, stats);
	}
	for (PendingCommsResponse& response : state.responses)
	{
		if (response.active == 0
			|| response.expires_at >= static_cast<std::int32_t>(gameplay_tick))
		{
			continue;
		}
		response.active = 0;
		if (response.response_flag != 1
			|| response.speaker_id == -1
			|| response.speaker_id == 0x3e9)
		{
			continue;
		}
		std::int16_t voice = response.static_voice_id;
		if (response.speaker_id < 0xffff)
		{
			if (response.speaker_id < 0
				|| response.speaker_id >= static_cast<std::int32_t>(
					std::size(world.objects))
				|| !world.objects[response.speaker_id].active
				|| (world.objects[response.speaker_id].runtime_flags
					& game::kObjectFlagDestroyed) != 0)
			{
				continue;
			}
			const assets::PilotPresentationDefinition* definition =
				assets::pilot_presentation(
					world.objects[response.speaker_id].pilot);
			if (definition == nullptr)
			{
				continue;
			}
			voice = definition->portrait_or_movie_id;
		}
		player_comms_play_or_queue(
			mission,
			world,
			response.fm8_path,
			response.voice_path,
			CommsPlaybackMode::queue,
			voice,
			5,
			response.speaker_id,
			-1,
			simulation_tick);
	}

	if (state.queue_count > 0
		&& !mission.presentation.comms_pending
		&& !mission.presentation.comms_active
		&& !mission.presentation.speech_pending
		&& !mission.presentation.speech_active
		&& (state.speech_hud_state == 0
			|| state.speech_hud_state == 2))
	{
		const QueuedComm request = state.queue[state.queue_read];
		--state.queue_count;
		state.queue_read = static_cast<std::int16_t>((state.queue_read + 1) % 5);
		if (request.expires_at < 1
			|| simulation_tick <= static_cast<std::uint32_t>(
				request.expires_at))
		{
			if (request.speaker_id != 0x3e9)
			{
				presentation_start(mission, world, request);
			}
		}
	}

	const bool training = mission.object_factory_mode == 1
		|| (mission.mission_number >= 30 && mission.mission_number <= 35);
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	const bool player_landing = player != nullptr
		&& player->ai.command_count != 0
		&& player->ai.commands[0].id == 8;
	if (!player_landing)
	{
		if (!return_to_base_requested(mission)
			|| mission.gameplay_state != 0)
		{
			state.return_prompt_latched = false;
		}
		else
		{
			if (!state.return_prompt_latched)
			{
				state.return_prompt_latched = true;
				state.next_return_prompt = simulation_tick + 4500;
				if (training)
				{
					player_comms_play_compiled_pilot(
						mission, world, 82, 0, "trnprm_001.ut",
						CommsPlaybackMode::queue, 5, -1, simulation_tick);
				}
			}
			if (!training && simulation_tick > state.next_return_prompt)
			{
				if (mission.generic_comms_disabled == 0)
				{
					player_comms_play_compiled_pilot(
						mission, world, compiled_chatter_pilot(mission), 0,
						random_line(world, kReturnToBase),
						CommsPlaybackMode::queue, 5, -1, simulation_tick);
				}
				state.next_return_prompt = simulation_tick + 4500;
			}
		}
	}

	const bool jump_requested = mission.session_state[0] != 0;
	const bool warp_requested = mission.session_state[1] != 0;
	if ((!jump_requested && !warp_requested)
		|| mission.gameplay_state != 0)
	{
		state.jump_warning_latched = false;
	}
	else if (!state.jump_warning_latched)
	{
		state.jump_warning_latched = true;
		state.jump_warning_stage = 0;
		state.next_jump_warning = gameplay_tick + 2000;
		const char* line = training
			? "trnjmp_001.ut"
			: random_line(
				world,
				jump_requested ? kJumpRequest : kWarpRequest);
		player_comms_play_compiled_pilot(
			mission, world,
			training ? 82 : compiled_chatter_pilot(mission), 0,
			line, CommsPlaybackMode::queue, 5, -1, simulation_tick);
	}
	else if (!training && gameplay_tick >= state.next_jump_warning)
	{
		if (state.jump_warning_stage == 4)
		{
			state.jump_warning_latched = false;
			events_commit_player_jump_or_warp_requests(
				mission, script_tick);
		}
		else
		{
			player_comms_play_compiled_pilot(
				mission, world, compiled_chatter_pilot(mission), 0,
				random_line(world, kJumpWarnings[state.jump_warning_stage]),
				CommsPlaybackMode::queue, 5, -1, simulation_tick);
			++state.jump_warning_stage;
			state.next_jump_warning = gameplay_tick
				+ (state.jump_warning_stage == 4 ? 300 : 2000);
		}
	}

	if (player != nullptr && player->active
		&& (player->runtime_flags & 0x10000840u) == 0
		&& player->incoming_missile
		&& simulation_tick > state.next_lock_warning)
	{
		if (mission.generic_comms_disabled == 0)
		{
			player_comms_play_compiled_pilot(
				mission, world, compiled_chatter_pilot(mission), 0,
				random_line(world, kPlayerLock),
				CommsPlaybackMode::only_if_idle, 5, 500, simulation_tick);
		}
		state.next_lock_warning = simulation_tick + 1000;
	}

	if (state.saved_ejected_pilot >= 0
		&& simulation_tick > state.saved_ejection_rescue_at)
	{
		const std::int32_t saved = state.saved_ejected_pilot;
		const auto slots = alpha_slots(mission, world);
		const std::uint16_t responder = slots[5];
		if (saved < static_cast<std::int32_t>(std::size(world.objects))
			&& world.objects[saved].active
			&& (world.objects[saved].runtime_flags & game::kObjectFlagDestroyed) == 0
			&& mission.generic_comms_disabled == 0
			&& responder < std::size(world.objects)
			&& world.objects[responder].active)
		{
			char voice[50];
			if (format_live_voice(
					world.objects[responder],
					random_line(world, kRescue),
					voice))
			{
				// 0x00456b55 loads Alpha slot six, formats its voice,
				// then passes that object's +0x740 pilot identity to
				// the compiled-pilot playback adapter.
				player_comms_play_compiled_pilot(
					mission, world,
					world.objects[responder].pilot, 0, voice,
					CommsPlaybackMode::only_if_idle, 5, -1,
					simulation_tick);
			}
		}
		state.saved_ejected_pilot = -1;
	}
}

void player_comms_open_menu(
	Runtime& mission,
	game::World& world,
	const assets::ShipStatsTable& stats)
{
	update_multiplayer_comms_state(mission, world);
	PlayerCommsState& state = mission.player_comms;
	state.menu_open = true;
	state.current_command = -1;
	state.current_target = -1;
	if (!mission.instruments[11].open)
	{
		mission.instruments[11].open = true;
		++mission.instruments[11].serial;
	}
	rebuild_menu(mission, world, stats);
}

bool player_comms_shortcut_target_valid(const game::World& world)
{
	const game::WorldObject* target = nullptr;
	return player_control_target_valid(world, target)
		&& target->allegiance_class == 1;
}

void player_comms_execute_shortcut(
	Runtime& mission,
	game::World& world,
	const assets::PilotStatsTable& pilot_stats,
	std::uint8_t shortcut,
	std::uint32_t gameplay_tick)
{
	// Player_update calls these four handlers directly at
	// 0x00414590..0x0041467e. It does not pass through CommsMenu_dispatch,
	// so shortcuts must not alter or close instrument eleven.
	switch (shortcut)
	{
	case 0:
		execute_attack(
			mission, world, pilot_stats, -1, gameplay_tick);
		break;
	case 1:
		execute_backoff(
			mission, world, pilot_stats, -1, gameplay_tick);
		break;
	case 2:
		execute_help(
			mission, world, pilot_stats, -1, gameplay_tick);
		break;
	case 3:
		execute_landing(mission, world, gameplay_tick);
		break;
	default:
		break;
	}
}

void player_comms_close_menu(Runtime& mission)
{
	mission.player_comms.menu_open = false;
	mission.player_comms.option_count = 0;
	mission.player_comms.open_panel_requested = false;
	if (mission.instruments[11].open)
	{
		mission.instruments[11].open = false;
		++mission.instruments[11].serial;
	}
}

void player_comms_select_option(
	Runtime& mission,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const assets::PilotStatsTable& pilot_stats,
	std::uint8_t one_based_option,
	std::uint32_t gameplay_tick)
{
	PlayerCommsState& state = mission.player_comms;
	if (!state.menu_open || one_based_option == 0
		|| one_based_option > state.option_count)
	{
		return;
	}
	const CommsMenuOption selected = state.options[one_based_option - 1];
	state.current_command = selected.command;
	state.current_target = selected.target;
	if (selected.command == 0 || selected.command == 1
		|| selected.command == 2 || selected.command == 3
		|| selected.command == 4 || selected.command == 5
		|| selected.command == 22 || selected.command == 23)
	{
		if (selected.command == 23)
		{
			// Command 23 saves DAT_00529596 in DAT_00529fbc before
			// constructing the remote-player command menu.
			state.remote_selected_player = selected.target;
		}
		rebuild_menu(mission, world, stats);
		return;
	}
	switch (selected.command)
	{
	case 6: case 7: case 8: case 9: case 10:
		execute_taunt(
			mission, world, pilot_stats, selected.target,
			selected.command, gameplay_tick);
		break;
	case 11:
		execute_attack(
			mission, world, pilot_stats,
			selected.target, gameplay_tick);
		break;
	case 12:
		execute_backoff(
			mission, world, pilot_stats,
			selected.target, gameplay_tick);
		break;
	case 13:
		execute_help(
			mission, world, pilot_stats,
			selected.target, gameplay_tick);
		break;
	case 14:
		execute_status(
			mission, world, stats, pilot_stats,
			selected.target, gameplay_tick);
		break;
	case 15:
		execute_opinion(
			mission, world, selected.target, false, gameplay_tick);
		break;
	case 16:
		execute_opinion(
			mission, world, selected.target, true, gameplay_tick);
		break;
	case 17:
		break;
	case 18:
		execute_landing(
			mission, world, gameplay_tick);
		break;
	case 19:
		execute_backup(mission, world, gameplay_tick);
		break;
	case 20:
	case 21:
		state.all_channels_open = selected.command == 20;
		break;
	case 24:
	case 25:
		state.title_language_id = 368;
		state.chat_destination =
			selected.command == 24
				? selected.target
				: -1;
		state.chat_requested = true;
		break;
	case 27:
	case 28:
		state.remote_response =
			selected.command == 27 ? 1 : 2;
		break;
	default:
		break;
	}
	if (selected.command == 24 || selected.command == 25)
	{
		// DAT_00529fb8 makes CommsMenu_dispatch skip its instrument-eleven
		// close path at 0x00455f43..0x00455f86 while text entry is active.
		state.option_count = 0;
		return;
	}
	player_comms_close_menu(mission);
}

bool player_comms_receive_network_command(
	Runtime& mission,
	game::World& world,
	std::uint8_t source_player,
	std::uint8_t command,
	std::uint16_t target_object)
{
	(void)world;
	if (mission.network.role == NetworkRole::offline
		|| source_player >= mission.network.player_count
		|| source_player == mission.network.local_player
		|| !mission.network.connected[source_player]
		|| command < 1 || command > 3)
	{
		return false;
	}
	replace_player_command_voice(
		mission,
		command == 1
			? "hud_001.ut"
			: command == 2
				? "hud_002.ut"
				: "hud_003.ut");
	PlayerCommsState& state = mission.player_comms;
	state.remote_command_source = source_player;
	state.remote_command_kind = command;
	state.remote_command_target = target_object;
	state.remote_response = 0;
	state.current_command = 26;
	state.current_target = 0;
	state.menu_open = true;
	state.open_panel_requested = true;
	if (!mission.instruments[11].open)
	{
		mission.instruments[11].open = true;
		++mission.instruments[11].serial;
	}
	build_remote_command_prompt(state);
	diagnostics::mission_log(
		"network rx player-comms command=%u source=%u target=%d",
		static_cast<unsigned>(command),
		static_cast<unsigned>(source_player),
		target_object == UINT16_MAX
			? -1
			: static_cast<int>(target_object));
	return true;
}

void player_comms_receive_network_landing(
	Runtime& mission,
	game::World& world,
	std::uint32_t gameplay_tick)
{
	execute_landing(mission, world, gameplay_tick, true);
}

bool player_comms_take_chat_request(
	PlayerCommsState& state,
	std::int16_t& destination)
{
	if (!state.chat_requested)
	{
		return false;
	}
	destination = state.chat_destination;
	state.chat_requested = false;
	state.chat_destination = -1;
	return true;
}

void player_comms_on_player_destroyed_target(
	Runtime& mission,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t target_world_index,
	std::uint32_t gameplay_tick,
	std::uint32_t simulation_tick)
{
	if (target_world_index >= std::size(world.objects)
		|| mission.generic_comms_disabled != 0)
	{
		return;
	}
	game::WorldObject& target = world.objects[target_world_index];
	const std::uint16_t static_pilot = compiled_chatter_pilot(mission);
	if ((target.runtime_flags & 0x800u) != 0)
	{
		player_comms_play_compiled_pilot(
			mission, world, static_pilot, 0,
			random_line(world, kEnemyEjected),
			CommsPlaybackMode::only_if_idle, 5, -1, simulation_tick);
		return;
	}
	if (gameplay_tick <= mission.player_comms.next_player_kill)
	{
		return;
	}
	mission.player_comms.next_player_kill = gameplay_tick + 600;
	if (player_comms_busy(mission.player_comms, mission.presentation))
	{
		return;
	}
	// The retail tests the type descriptor word at +0x28. In the retained
	// descriptor layout this is collision_class (object_class is +0x2c).
	const std::int16_t collision_class = target.type < assets::kShipStatsCount
		? stats.records[target.type].object.collision_class
		: target.collision_class;
	if (collision_class == 1)
	{
		char voice[50];
		if (format_live_voice(target, random_line(world, kPilotDeath), voice))
		{
			player_comms_play_live_pilot(
				mission, world, target_world_index, 3, voice,
				CommsPlaybackMode::queue, 6, 200, simulation_tick);
		}
		player_comms_play_compiled_pilot(
			mission, world, static_pilot, 0,
			random_line(world, kPlayerKill),
			CommsPlaybackMode::queue, 5, 500, simulation_tick);
	}
	else if (collision_class == 5)
	{
		player_comms_play_compiled_pilot(
			mission, world, static_pilot, 0,
			random_line(world, kTransportKill),
			CommsPlaybackMode::only_if_idle, 5, 500, simulation_tick);
	}
}

void player_comms_on_pilot_death(
	Runtime& mission,
	game::World& world,
	std::uint16_t pilot_world_index,
	std::uint32_t simulation_tick)
{
	if (pilot_world_index >= std::size(world.objects)
		|| pilot_world_index == world.player.index
		|| mission.generic_comms_disabled != 0
		|| player_comms_busy(mission.player_comms, mission.presentation))
	{
		return;
	}
	game::WorldObject& pilot = world.objects[pilot_world_index];
	char voice[50];
	if (format_live_voice(pilot, "dth_001.ut", voice))
	{
		player_comms_play_live_pilot(
			mission, world, pilot_world_index, 3, voice,
			CommsPlaybackMode::only_if_idle, 6, -1, simulation_tick);
	}
	player_comms_play_compiled_pilot(
		mission, world, compiled_chatter_pilot(mission), 0,
		random_line(world, kNpcDeath), CommsPlaybackMode::queue,
		5, -1, simulation_tick);
}

void player_comms_on_pilot_ejected(
	Runtime& mission,
	game::World& world,
	std::uint16_t pilot_world_index,
	std::uint32_t simulation_tick)
{
	if (pilot_world_index >= std::size(world.objects)
		|| pilot_world_index == world.player.index)
	{
		return;
	}
	mission.player_comms.saved_ejected_pilot = pilot_world_index;
	mission.player_comms.saved_ejection_rescue_at = simulation_tick + 1000;
	if (mission.generic_comms_disabled != 0)
	{
		return;
	}
	char voice[50];
	if (format_live_voice(
			world.objects[pilot_world_index], "ejt_001.ut", voice))
	{
		player_comms_play_live_pilot(
			mission, world, pilot_world_index, 0, voice,
			CommsPlaybackMode::only_if_idle, 5, -1, simulation_tick);
	}
}

void player_comms_on_player_damaged(
	Runtime& mission,
	game::World& world,
	std::uint16_t attacker_world_index,
	std::uint32_t simulation_tick)
{
	if (attacker_world_index >= std::size(world.objects))
	{
		return;
	}
	game::WorldObject& attacker = world.objects[attacker_world_index];
	if (!attacker.active || attacker.allegiance_class != 1
		|| (attacker.runtime_flags & 2u) != 0
		|| simulation_tick <= mission.player_comms.next_damage_taunt)
	{
		return;
	}
	mission.player_comms.next_damage_taunt = simulation_tick + 2000;
	if (mission.generic_comms_disabled != 0
		|| mission.taunts_disabled != 0)
	{
		return;
	}
	char voice[50];
	if (format_live_voice(
		attacker, random_line(world, kPlayerHitTaunt), voice))
	{
		player_comms_play_live_pilot(
			mission, world, attacker_world_index, 0, voice,
			CommsPlaybackMode::only_if_idle, 5, -1, simulation_tick);
	}
}

void player_comms_on_player_launch(
	Runtime& mission,
	game::World& world,
	std::uint16_t carrier_world_index,
	std::uint32_t simulation_tick)
{
	player_comms_retain_carrier(mission, carrier_world_index);
	if (mission.generic_comms_disabled != 0)
	{
		return;
	}
	const bool training = mission.object_factory_mode == 1
		|| (mission.mission_number >= 30 && mission.mission_number <= 35);
	if (training)
	{
		player_comms_play_compiled_pilot(
			mission, world, 82, 0, "trnlch_001.ut",
			CommsPlaybackMode::only_if_idle, 5, -1, simulation_tick);
		return;
	}
	if (carrier_world_index >= std::size(world.objects)
		|| !world.objects[carrier_world_index].active)
	{
		return;
	}
	const std::uint16_t type = world.objects[carrier_world_index].type;
	if (type == 0x0c)
	{
		player_comms_play_compiled_pilot(
			mission, world, 60, 0, random_line(world, kReliantLaunch),
			CommsPlaybackMode::only_if_idle, 5, -1, simulation_tick);
	}
	else if (type == 0x0d)
	{
		player_comms_play_compiled_pilot(
			mission, world, 84, 0, random_line(world, kYamatoLaunch),
			CommsPlaybackMode::only_if_idle, 5, -1, simulation_tick);
	}
}

void player_comms_retain_carrier(
	Runtime& mission,
	std::uint16_t carrier_world_index)
{
	// Both retail Launch begin paths write DAT_0057e05c before the command
	// is released. Base comms therefore has its carrier while the player is
	// still attached in the launch bay.
	mission.player_comms.carrier_world_index = carrier_world_index;
}
}
