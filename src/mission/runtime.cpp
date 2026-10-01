#include "mission/runtime.hpp"

#include "ai/runtime.hpp"
#include "assets/object_type_catalog.hpp"
#include "core/mission_log.hpp"
#include "io/endian.hpp"
#include "mission/executor.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>

namespace sl_open::mission
{
namespace
{
constexpr std::int16_t kRetailPilotCatalogIds[
	kRetailPilotCatalogCount] = {
	0x77, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d,
	0x5e, 0x60, 0x61, 0x62, 0x63, 0x64, 0x66, 0x67,
	0x68, 0x69, 0x8a, 0x6b, 0xb9, 0x6d, 0x6e, 0x8b,
	0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76,
	0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x90,
	0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
	0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2,
	0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
	0xab,
};
static_assert(
	std::size(kRetailPilotCatalogIds)
		== kRetailPilotCatalogCount);

glm::mat3 authored_orientation(const std::uint8_t* record)
{
	const float heading = glm::radians(static_cast<float>(
		static_cast<std::int16_t>(io::read_le16(record + 0x2e))));
	const float pitch = glm::radians(static_cast<float>(
		static_cast<std::int16_t>(io::read_le16(record + 0x3a))));
	const float roll = glm::radians(static_cast<float>(
		static_cast<std::int16_t>(io::read_le16(record + 0x4a))));
	glm::mat3 orientation{1.0f};
	orientation = math::postrotate(
		orientation, heading, {0.0f, 1.0f, 0.0f});
	orientation = math::postrotate(
		orientation, pitch, {1.0f, 0.0f, 0.0f});
	return math::postrotate(
		orientation, roll, {0.0f, 0.0f, 1.0f});
}

glm::vec3 authored_position(const std::uint8_t* record)
{
	glm::vec3 position;
	std::memcpy(&position, record + 0x1c, sizeof(position));
	return position;
}

bool is_spatial_anchor_type(std::uint16_t type)
{
	return type == 995
		|| type == 996
		|| type == 997
		|| type == 999;
}

bool group_destroy_releases_atmosphere(std::uint16_t type)
{
	// DestroyFlightGroup_command, LANCER.EXE 0x00457ff4.
	return (type > 0x5f && type < 0x6a)
		|| (type > 0xc9 && type < 0xd4);
}

std::uint8_t normalized_spawn_mode(std::uint8_t mode)
{
	// GameObject_create_runtime 0x00466c23 maps five to four, preserves
	// zero through four, and maps every other value to zero.
	if (mode == 5)
	{
		return 4;
	}
	return mode < 5 ? mode : 0;
}

bool initialize_default_ai(
	Runtime& runtime,
	game::World& world,
	game::WorldObject& actor,
	std::uint16_t mission_index)
{
	actor.player_slot = mission_index < runtime.player_prefix_count;
	const std::int16_t command =
		actor.player
			? 100
			: actor.player_slot
				? 101
				: 0;
	return ai::command_push(world,
		actor,
		command,
		ai::TargetKind::none,
		UINT16_MAX,
		-1);
}

void rebuild_group_allegiances(
	const Runtime& runtime,
	game::World& world)
{
	// MissionGroup_rebuild_allegiance_lists, LANCER.EXE 0x0045ac60,
	// clears the class 0/1/2 live-object lists and rebuilds them in
	// compiled group/member order after every CreateFlightGroup command.
	// The reimplementation derives those lists where they are consumed,
	// but must still publish the class on every currently live member.
	for (std::uint16_t group_index = 0;
		group_index < runtime.group_count;
		++group_index)
	{
		const GroupRecord& group = runtime.groups[group_index];
		if (group.object_class == UINT8_MAX)
		{
			continue;
		}
		for (std::uint16_t ordinal = 0;
			ordinal < group.member_count;
			++ordinal)
		{
			const std::uint16_t mission_index =
				runtime.group_members[group.first_member + ordinal];
			game::WorldObject* live = game::world_resolve(
				world, runtime.objects[mission_index].live);
			if (live == nullptr || live->type == 1001)
			{
				continue;
			}
			live->allegiance_class = group.object_class;
			live->hostile = group.object_class == 1;
			live->mission_group_class = group.object_class;
		}
	}
}
}

void runtime_publish_camera_request(
	Runtime& runtime,
	bool accept_departed_target)
{
	runtime.requested_camera_accept_departed = accept_departed_target;
	runtime.camera_request_order =
		++runtime.camera_transition_order;
	++runtime.camera_request_serial;
}

void runtime_publish_match_speed_request(
	Runtime& runtime,
	bool enabled)
{
	runtime.requested_match_speed = enabled;
	++runtime.match_speed_request_serial;
}

bool runtime_add_player_score(
	Runtime& runtime,
	game::World& world,
	std::uint16_t scorer_world_index,
	std::int32_t delta,
	bool publish)
{
	// Player_score_add, LANCER.EXE 0x004b14f0. The retail player table is
	// indexed directly by the scorer's live-object slot and performs
	// wrapping 32-bit addition.
	if (scorer_world_index >= kNetworkPlayerCapacity)
	{
		return false;
	}
	game::WorldObject& scorer = world.objects[scorer_world_index];
	const bool local_scorer =
		scorer_world_index == world.player.index;
	// Retail's separate global at 0x00562df4 is the score displayed and
	// carried forward for the local player. Remote adjustments mutate only
	// their replicated player-table rows.
	if (local_scorer)
	{
		scorer.score = std::bit_cast<std::int32_t>(
			std::bit_cast<std::uint32_t>(scorer.score)
			+ std::bit_cast<std::uint32_t>(delta));
	}
	runtime.network.player_kills[scorer_world_index] =
		std::bit_cast<std::int32_t>(
			std::bit_cast<std::uint32_t>(
				runtime.network.player_kills[
					scorer_world_index])
			+ std::bit_cast<std::uint32_t>(delta));

	if (runtime.network.role == NetworkRole::offline)
	{
		// The mission-history counter increments once per adjustment,
		// independently of the signed delta, and deliberately excludes
		// campaign mission 28 and later simulator missions.
		if (local_scorer && runtime.mission_number < 28)
		{
			++runtime.mission_score_events;
		}
	}
	else if (runtime.network.team_mode)
	{
		const std::int32_t team =
			runtime.network.object_team[scorer_world_index];
		if (team >= 0
			&& static_cast<std::size_t>(team)
				< std::size(runtime.network.team_score))
		{
			runtime.network.team_score[team] =
				std::bit_cast<std::int32_t>(
					std::bit_cast<std::uint32_t>(
						runtime.network.team_score[team])
					+ std::bit_cast<std::uint32_t>(delta));
		}
	}
	if (local_scorer && publish
		&& runtime.network.role != NetworkRole::offline)
	{
		network_publish_player_stats(
			runtime.network, world.player.index);
	}
	return true;
}

bool runtime_add_player_death(
	Runtime& runtime,
	const game::World& world,
	std::uint16_t player_world_index,
	std::int32_t delta,
	bool publish)
{
	// Deathmatch_add_player_death, LANCER.EXE 0x004b1580, gates on the
	// multiplayer-session global rather than the deathmatch-rules byte.
	// Co-op therefore retains the same replicated death total, while an
	// offline launch (including a locally inspected DM arena) does not.
	if (runtime.network.role == NetworkRole::offline
		|| player_world_index >= kNetworkPlayerCapacity)
	{
		return false;
	}
	runtime.network.player_deaths[player_world_index] =
		std::bit_cast<std::int32_t>(
			std::bit_cast<std::uint32_t>(
				runtime.network.player_deaths[
					player_world_index])
			+ std::bit_cast<std::uint32_t>(delta));
	if (runtime.network.team_mode)
	{
		const std::int32_t team =
			runtime.network.object_team[player_world_index];
		if (team >= 0
			&& static_cast<std::size_t>(team)
				< std::size(runtime.network.team_deaths))
		{
			runtime.network.team_deaths[team] =
				std::bit_cast<std::int32_t>(
					std::bit_cast<std::uint32_t>(
						runtime.network.team_deaths[team])
					+ std::bit_cast<std::uint32_t>(delta));
		}
	}
	if (player_world_index == world.player.index
		&& publish
		&& runtime.network.role != NetworkRole::offline)
	{
		network_publish_player_stats(
			runtime.network, world.player.index);
	}
	return true;
}

bool runtime_initialize(Runtime& runtime, const DteFile& file)
{
	runtime = {};
	environment_effects_reset(runtime.environment);
	network_runtime_reset(runtime.network);
	player_comms_reset(runtime.player_comms);
	std::fill(
		std::begin(runtime.trigger_log_tick),
		std::end(runtime.trigger_log_tick),
		UINT32_MAX);
	const DteSection& object_section = file.sections[3];
	const DteSection& group_section = file.sections[4];
	const DteSection& trigger_section = file.sections[5];
	const DteSection& span_section = file.sections[7];
	const DteSection& set_section = file.sections[12];
	const DteSection& link_section = file.sections[13];
	if (object_section.count > game::kMaxMissionObjects
		|| group_section.count > game::kMaxMissionGroups
		|| trigger_section.count > game::kMaxMissionTriggers
		|| span_section.count > game::kMaxMissionReferenceSpans
		|| set_section.count > game::kMaxMissionReferenceSets
		|| link_section.count > game::kMaxMissionReferenceLinks)
	{
		return false;
	}
	runtime.object_count = object_section.count;
	runtime.group_count = group_section.count;
	runtime.trigger_count = trigger_section.count;
	runtime.reference_span_count = span_section.count;
	runtime.reference_set_count = set_section.count;
	runtime.reference_link_count = link_section.count;
	const std::uint8_t* source_objects = dte_section_data(file, 3);
	for (std::uint16_t index = 0; index < runtime.object_count; ++index)
	{
		const std::uint8_t* source =
			source_objects + static_cast<std::size_t>(index) * 0x4c;
		ObjectRecord& object = runtime.objects[index];
		object.reference_span = io::read_le16(source);
		object.name_offset = io::read_le16(source + 4);
		object.group = source[0x14];
		object.pilot = source[0x15];
		// Mission-system initialization at LANCER.EXE 0x0045cd58 clears
		// this writable byte for every record; the source byte is not an
		// authored initial state.
		object.script_flags = 0;
		object.type = io::read_le16(source + 0x18);
		object.launch_source_type = io::read_le16(source + 0x28);
		object.attachment_descriptor = io::read_le16(source + 0x34);
		object.authored_position = authored_position(source);
		object.script_position = object.authored_position;
		object.authored_orientation = authored_orientation(source);
		object.launch_point = source[0x2b];
		object.spawn_mode = source[0x3d];
	}

	const std::uint8_t* source_groups = dte_section_data(file, 4);
	for (std::uint16_t group_index = 0;
		group_index < runtime.group_count;
		++group_index)
	{
		const std::uint8_t* source =
			source_groups + static_cast<std::size_t>(group_index) * 0x14;
		GroupRecord& group = runtime.groups[group_index];
		group.reference_span = io::read_le16(source);
		group.object_class = source[8];
		const std::uint8_t authored_member_count = source[9];
		if (group.object_class != UINT8_MAX
			&& group.object_class > 2)
		{
			runtime = {};
			return false;
		}
		group.first_member = runtime.member_count;
		for (std::uint16_t object_index = 0;
			object_index < runtime.object_count;
			++object_index)
		{
			if (runtime.objects[object_index].group != group_index)
			{
				continue;
			}
			if (runtime.member_count >= game::kMaxMissionObjects)
			{
				runtime = {};
				return false;
			}
			runtime.group_members[runtime.member_count++] = object_index;
			++group.member_count;
		}
		if (group.member_count != authored_member_count)
		{
			runtime = {};
			return false;
		}
	}

	// MissionDTE_build_subtype997_group_pairs groups route records by the
	// first subtype-997 object that introduces each group, retaining object
	// table order within a group.
	bool visited_route_groups[game::kMaxMissionGroups]{};
	for (std::uint16_t first = 0; first < runtime.object_count; ++first)
	{
		const ObjectRecord& route = runtime.objects[first];
		if (route.type != 997
			|| route.group == UINT8_MAX
			|| route.group >= runtime.group_count
			|| visited_route_groups[route.group])
		{
			continue;
		}
		visited_route_groups[route.group] = true;
		for (std::uint16_t object = 0;
			object < runtime.object_count;
			++object)
		{
			if (runtime.objects[object].type != 997
				|| runtime.objects[object].group != route.group)
			{
				continue;
			}
			runtime.subtype997_pairs[
				runtime.subtype997_pair_count++] = {
					route.group, object};
		}
	}

	const std::uint8_t* source_spans = dte_section_data(file, 7);
	for (std::uint16_t index = 0;
		index < runtime.reference_span_count;
		++index)
	{
		const std::uint8_t* source =
			source_spans + static_cast<std::size_t>(index) * 8;
		ReferenceSpan& span = runtime.reference_spans[index];
		span.owner_kind = source[0];
		span.trigger_count = source[1];
		span.first_trigger = io::read_le16(source + 2);
		if (span.owner_kind > 2
			|| (span.trigger_count != 0
				&& (span.first_trigger >= runtime.trigger_count
					|| span.trigger_count
						> runtime.trigger_count - span.first_trigger)))
		{
			runtime = {};
			return false;
		}
	}

	const std::uint8_t* source_triggers = dte_section_data(file, 5);
	for (std::uint16_t index = 0; index < runtime.trigger_count; ++index)
	{
		const std::uint8_t* source =
			source_triggers + static_cast<std::size_t>(index) * 0x30;
		TriggerRecord& trigger = runtime.triggers[index];
		trigger.type = source[0];
		trigger.repeat_mode = source[1];
		trigger.script_word = io::read_le16(source + 2);
		// ExecutorRuntime_initialize enables every compiled trigger; the
		// serialized byte is editor/runtime scratch, not initial gameplay
		// state.
		trigger.enabled = 1;
		trigger.selector = source[0x15];
		trigger.deferred = source[0x16];
		trigger.repeats_remaining = source[0x19];
		trigger.authored_repeats = source[0x1a];
		for (std::size_t condition = 0;
			condition < std::size(trigger.condition);
			++condition)
		{
			trigger.condition[condition] =
				io::read_le32(source + 0x1c + condition * 4);
		}
	}

	const std::uint8_t* source_sets = dte_section_data(file, 12);
	for (std::uint16_t index = 0;
		index < runtime.reference_set_count;
		++index)
	{
		const std::uint8_t* source =
			source_sets + static_cast<std::size_t>(index) * 0x0c;
		runtime.reference_sets[index].reference_span =
			io::read_le16(source);
		runtime.reference_sets[index].first_link =
			io::read_le16(source + 8);
		if (runtime.reference_sets[index].reference_span
				>= runtime.reference_span_count
			|| (runtime.reference_sets[index].first_link != UINT16_MAX
				&& runtime.reference_sets[index].first_link
					>= runtime.reference_link_count))
		{
			runtime = {};
			return false;
		}
	}

	const std::uint8_t* source_links = dte_section_data(file, 13);
	for (std::uint16_t index = 0;
		index < runtime.reference_link_count;
		++index)
	{
		const std::uint8_t* source =
			source_links + static_cast<std::size_t>(index) * 0x0c;
		ReferenceLink& link = runtime.reference_links[index];
		link.reference_span = io::read_le16(source);
		link.owner_set = io::read_le16(source + 4);
		link.selector = source[8];
		if (link.reference_span >= runtime.reference_span_count
			|| link.owner_set >= runtime.reference_set_count)
		{
			runtime = {};
			return false;
		}
	}
	runtime.ready = true;
	return true;
}

bool runtime_install_multiplayer_bootstrap(
	Runtime& runtime,
	const game::MultiplayerMissionBootstrap& bootstrap)
{
	if (!runtime.ready
		|| !game::valid_multiplayer_mission_bootstrap(
			bootstrap,
			bootstrap.mission,
			static_cast<std::uint8_t>(
				bootstrap.image.session_state[15]),
			false))
	{
		return false;
	}
	std::copy(
		std::begin(bootstrap.image.session_state),
		std::end(bootstrap.image.session_state),
		std::begin(runtime.session_state));
	std::fill(
		std::begin(runtime.session_state_kinds),
		std::end(runtime.session_state_kinds),
		static_cast<std::uint8_t>(ValueKind::scalar));

	std::fill(
		std::begin(runtime.multiplayer_pilot_availability),
		std::end(runtime.multiplayer_pilot_availability),
		std::uint8_t{2});
	runtime.multiplayer_pilot_availability[0] =
		bootstrap.image.pilo.availability;
	std::copy(
		std::begin(bootstrap.image.pilot_assignments),
		std::end(bootstrap.image.pilot_assignments),
		std::begin(runtime.multiplayer_pilot_assignments));

	// FUN_0049cd70 replaces the two mission-dependent authored pilots before
	// filling missing ALPH entries from the locally reset PILO catalog.
	if (bootstrap.mission <= 5)
	{
		runtime.multiplayer_pilot_assignments[4] = 0xac;
		runtime.multiplayer_pilot_assignments[5] = 7;
	}
	else if (bootstrap.mission <= 13)
	{
		runtime.multiplayer_pilot_assignments[4] = 0x78;
		runtime.multiplayer_pilot_assignments[5] = 6;
	}
	else if (bootstrap.mission <= 22)
	{
		runtime.multiplayer_pilot_assignments[4] = 0x78;
		runtime.multiplayer_pilot_assignments[5] = 0;
	}
	else
	{
		runtime.multiplayer_pilot_assignments[4] = 0x5f;
		runtime.multiplayer_pilot_assignments[5] = 1;
	}
	for (std::size_t assignment = 1;
		assignment < game::kMultiplayerPilotAssignmentCount;
		++assignment)
	{
		if (runtime.multiplayer_pilot_assignments[assignment] != -1)
		{
			continue;
		}
		std::size_t available = 0;
		while (available < kRetailPilotCatalogCount
			&& runtime.multiplayer_pilot_availability[available] != 2)
		{
			++available;
		}
		if (available == kRetailPilotCatalogCount)
		{
			return false;
		}
		runtime.multiplayer_pilot_assignments[assignment] =
			kRetailPilotCatalogIds[available];
		runtime.multiplayer_pilot_availability[available] = 1;
	}

	// The loader at 0x0045ac60 rebuilds each class table from its group
	// members; a later group of the same class replaces the prior table.
	// DAT_00515d88 is class zero, and 0x00493df1 binds entries 1..5.
	std::fill(
		std::begin(runtime.multiplayer_alpha_objects),
		std::end(runtime.multiplayer_alpha_objects),
		UINT16_MAX);
	for (std::uint16_t group_index = 0;
		group_index < runtime.group_count;
		++group_index)
	{
		const GroupRecord& group = runtime.groups[group_index];
		if (group.object_class != 0)
		{
			continue;
		}
		const std::size_t count = std::min<std::size_t>(
			group.member_count,
			game::kMultiplayerPilotAssignmentCount);
		for (std::size_t member = 0; member < count; ++member)
		{
			const std::size_t group_member =
				static_cast<std::size_t>(group.first_member)
				+ member;
			if (group_member >= runtime.member_count)
			{
				return false;
			}
			runtime.multiplayer_alpha_objects[member] =
				runtime.group_members[group_member];
		}
	}
	for (std::size_t assignment = 1;
		assignment < game::kMultiplayerPilotAssignmentCount;
		++assignment)
	{
		const std::uint16_t object =
			runtime.multiplayer_alpha_objects[assignment];
		const std::int16_t pilot =
			runtime.multiplayer_pilot_assignments[assignment];
		if (object >= runtime.object_count
			|| pilot < 0
			|| pilot > UINT8_MAX)
		{
			return false;
		}
		runtime.objects[object].pilot =
			static_cast<std::uint8_t>(pilot);
	}
	runtime.multiplayer_launch_generation =
		bootstrap.launch_generation;
	runtime.multiplayer_bootstrap_mission =
		bootstrap.mission;
	runtime.multiplayer_bootstrap_installed = true;
	return true;
}

bool runtime_capture_multiplayer_bootstrap(
	const Runtime& runtime,
	game::MultiplayerMissionBootstrap& bootstrap)
{
	bootstrap = {};
	if (!runtime.multiplayer_bootstrap_installed
		|| runtime.multiplayer_launch_generation == 0
		|| runtime.multiplayer_bootstrap_mission == 0)
	{
		return false;
	}
	bootstrap.present = true;
	bootstrap.launch_generation =
		runtime.multiplayer_launch_generation;
	bootstrap.mission = runtime.multiplayer_bootstrap_mission;
	std::copy(
		std::begin(runtime.session_state),
		std::end(runtime.session_state),
		std::begin(bootstrap.image.session_state));
	bootstrap.image.pilo.pilot_id =
		game::kMultiplayerFirstPilotId;
	bootstrap.image.pilo.availability =
		runtime.multiplayer_pilot_availability[0];
	bootstrap.image.pilo.reserved = 0;
	std::copy(
		std::begin(runtime.multiplayer_pilot_assignments),
		std::end(runtime.multiplayer_pilot_assignments),
		std::begin(bootstrap.image.pilot_assignments));
	return true;
}

void runtime_mark_multiplayer_pilot_destroyed(
	Runtime& runtime,
	std::uint16_t pilot)
{
	if (!runtime.multiplayer_bootstrap_installed
		|| pilot > game::kMultiplayerMaximumPilotId)
	{
		return;
	}
	bool assigned = false;
	for (std::size_t assignment = 1;
		assignment < game::kMultiplayerPilotAssignmentCount;
		++assignment)
	{
		if (runtime.multiplayer_pilot_assignments[assignment]
			== static_cast<std::int16_t>(pilot))
		{
			runtime.multiplayer_pilot_assignments[assignment] = -1;
			assigned = true;
			break;
		}
	}
	if (!assigned)
	{
		return;
	}
	for (std::size_t catalog = 0;
		catalog < kRetailPilotCatalogCount;
		++catalog)
	{
		if (kRetailPilotCatalogIds[catalog]
			== static_cast<std::int16_t>(pilot))
		{
			runtime.multiplayer_pilot_availability[catalog] = 0;
			break;
		}
	}
}

void runtime_reset_live(Runtime& runtime)
{
	for (std::uint16_t index = 0; index < runtime.object_count; ++index)
	{
		runtime.objects[index].live = {};
		runtime.objects[index].script_flags = 0;
		runtime.objects[index].live_component_mask = UINT32_MAX;
		runtime.objects[index].activation_service_pending = false;
	}
	runtime.active_object_count = 0;
	runtime.active_high_water = 0;
	runtime.subtype999_count = 0;
}

bool runtime_activate_object(
	Runtime& runtime,
	std::uint16_t mission_index,
	game::World& world,
	const assets::ShipStatsTable& stats)
{
	if (!runtime.ready || mission_index >= runtime.object_count)
	{
		return false;
	}
	ObjectRecord& source = runtime.objects[mission_index];
	game::WorldObject* existing =
		game::world_resolve(world, source.live);
	if (existing != nullptr && existing->type != 1001)
	{
		return true;
	}
	const bool recreating_departed = existing != nullptr;
	source.activation_service_pending = false;
	const bool spatial_anchor = is_spatial_anchor_type(source.type);
	const std::uint8_t group_class =
		source.group < runtime.group_count
			? runtime.groups[source.group].object_class
			: UINT8_MAX;
	std::uint16_t selected_type = spatial_anchor
		? std::uint16_t{1000}
		: assets::object_type_for_mission_creation(
			source.type,
			runtime.mission_number,
			group_class,
			runtime.mission_25_alternate);
	// GameObject_create_runtime 0x00466cad substitutes the Reliant for a
	// requested Yamato only in frontend owner mode one before mission 19.
	if (!spatial_anchor
		&& runtime.object_factory_mode == 1
		&& runtime.mission_number < 19
		&& selected_type == 0x0d)
	{
		selected_type = 0x0c;
	}
	const std::uint16_t runtime_type = spatial_anchor
		? selected_type
		: assets::object_type_runtime_alias(selected_type);
	// GameObject_create_runtime compares the explicit live slot with the
	// retained deathmatch local-player index, not literal slot zero. This
	// matters when reconstructing Command 34's local player on a client
	// whose assigned multiplayer slot is nonzero.
	const bool local_player =
		!spatial_anchor
		&& mission_index == runtime.network.local_player;
	if (runtime_type != source.type)
	{
		diagnostics::mission_log(
			"object type mission=%u authored=%u selected=%u runtime=%u group_class=%u",
			static_cast<unsigned>(mission_index),
			static_cast<unsigned>(source.type),
			static_cast<unsigned>(selected_type),
			static_cast<unsigned>(runtime_type),
			static_cast<unsigned>(group_class));
	}
	const game::ObjectHandle handle =
		recreating_departed
		? game::world_recreate(
			world,
			source.live,
			runtime_type,
			source.script_position,
			source.authored_orientation,
			stats,
			local_player,
			mission_index,
			source.group,
			spatial_anchor ? UINT8_MAX : source.pilot)
			: game::world_create_at(
				world,
				mission_index,
				runtime_type,
				source.script_position,
				source.authored_orientation,
			stats,
			local_player,
			mission_index,
			source.group,
			spatial_anchor ? UINT8_MAX : source.pilot);
	if (handle.index == UINT16_MAX)
	{
		// Recreation crosses the complete destructor boundary before the
		// replacement factory runs. A failed replacement therefore removes
		// the previously retained high-water object.
		source.live = {};
		if (recreating_departed
			&& runtime.active_object_count != 0)
		{
			--runtime.active_object_count;
		}
		diagnostics::mission_log(
			"object activation failed mission=%u stage=factory recreate=%u",
			static_cast<unsigned>(mission_index),
			recreating_departed ? 1u : 0u);
		return false;
	}
	source.live = handle;
	game::WorldObject* actor = game::world_resolve(world, handle);
	if (actor == nullptr)
	{
		source.live = {};
		if (recreating_departed
			&& runtime.active_object_count != 0)
		{
			--runtime.active_object_count;
		}
		diagnostics::mission_log(
			"object activation failed mission=%u stage=publish",
			static_cast<unsigned>(mission_index));
		return false;
	}
	if (mission_index == runtime.action_center_object)
	{
		world.action_center = handle;
	}
	actor->mission_group_class = group_class == UINT8_MAX
		? -1
		: static_cast<std::int16_t>(group_class);
	if (!spatial_anchor)
	{
		// MissionObject_instantiate_live_object forwards the normalized
		// spawn/loadout selector to GameObject_create_runtime; the retained
		// value is consumed by hardpoint definition rebuilds.
		actor->loadout_index = normalized_spawn_mode(source.spawn_mode);
		static_assert(
			sizeof(actor->multiplayer_player_loadout)
					/ sizeof(actor->multiplayer_player_loadout[0])
				== kNetworkPlayerLoadoutSlots);
		actor->multiplayer_player_loadout_valid =
			mission_index < kNetworkPlayerCapacity
			&& runtime.network.player_loadout_valid[mission_index];
		if (actor->multiplayer_player_loadout_valid)
		{
			std::copy(
				std::begin(
					runtime.network.player_loadout[mission_index]),
				std::end(
					runtime.network.player_loadout[mission_index]),
				std::begin(actor->multiplayer_player_loadout));
		}
		// Stable equivalent of the retail live-object index prefix used by
		// AI_command_forbidden_for_player_slot.
		actor->player_slot =
			mission_index < runtime.player_prefix_count;
		// GameObject_rebuild_ordnance_definitions uses the local-player
		// 5,3,1 override in the late mission range 30..35 (or frontend
		// game-flow state one). Instant Action mission 29 retains the
		// authored five-column loadout.
		actor->special_player_loadout =
			actor->player
			&& (runtime.object_factory_mode == 1
				|| (runtime.mission_number >= 30
					&& runtime.mission_number <= 35));
		if (!initialize_default_ai(
				runtime, world, *actor, mission_index))
		{
			game::world_destroy(world, handle);
			source.live = {};
			if (recreating_departed
				&& runtime.active_object_count != 0)
			{
				--runtime.active_object_count;
			}
			diagnostics::mission_log(
				"object activation failed mission=%u stage=default-ai",
				static_cast<unsigned>(mission_index));
			return false;
		}
	}
	if (!spatial_anchor && source.launch_point != UINT8_MAX)
	{
		bool queued = false;
		for (std::uint16_t carrier_index = 0;
			carrier_index < runtime.object_count;
			++carrier_index)
		{
			// MissionObject_instantiate_live_object stops at the first
			// section-3 record whose authored type matches. Its pointer-to-
			// slot conversion does not test whether that source is currently
			// live; command 104 owns resolution of the retained target.
			if (runtime.objects[carrier_index].type
				!= source.launch_source_type)
			{
				continue;
			}
			if (!ai::command_push(world,
					*actor,
					104,
					ai::TargetKind::object,
					carrier_index,
					source.launch_point))
			{
				game::world_destroy(world, handle);
				source.live = {};
				if (recreating_departed
					&& runtime.active_object_count != 0)
				{
					--runtime.active_object_count;
				}
				diagnostics::mission_log(
					"object activation failed mission=%u "
					"stage=launch-command carrier=%u",
					static_cast<unsigned>(mission_index),
					static_cast<unsigned>(carrier_index));
				return false;
			}
			queued = true;
			source.activation_service_pending = true;
			break;
		}
		if (!queued)
		{
			game::world_destroy(world, handle);
			source.live = {};
			if (recreating_departed
				&& runtime.active_object_count != 0)
			{
				--runtime.active_object_count;
			}
			diagnostics::mission_log(
				"object activation failed mission=%u "
				"stage=launch-source authored_type=%u",
				static_cast<unsigned>(mission_index),
				static_cast<unsigned>(
					source.launch_source_type));
			return false;
		}
	}
	// A fully initialized live object owns a fresh model/component tree.
	// Publish these only after every fallible creation stage succeeds so a
	// failed replacement cannot make an absent prior generation appear live.
	source.live_component_mask = UINT32_MAX;
	if (spatial_anchor && source.type == 999)
	{
		++runtime.subtype999_count;
	}
	if (!recreating_departed)
	{
		++runtime.active_object_count;
	}
	runtime.active_high_water =
		std::max(runtime.active_high_water, runtime.active_object_count);
	return true;
}

bool runtime_activate_curve_points(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file)
{
	// Mission initialization scans the object table and asks for the first
	// curve beginning at each object (LANCER.EXE 0x0045cd41..0x0045ce1d).
	// For every match it instantiates endpoint 0, control 0, control 1, and
	// endpoint 1 in that exact order when their retained live slots are not
	// active. Besides curve following, missions use these type-1000 spatial
	// anchors directly as static Director cameras.
	const std::uint8_t* curves = dte_section_data(file, 16);
	for (std::uint16_t object = 0;
		object < runtime.object_count;
		++object)
	{
		const std::uint8_t* curve = nullptr;
		for (std::uint16_t index = 0;
			index < file.sections[16].count;
			++index)
		{
			const std::uint8_t* candidate =
				curves + static_cast<std::size_t>(index) * 0x44;
			if (static_cast<std::uint16_t>(
					io::read_le32(candidate)) == object)
			{
				curve = candidate;
				break;
			}
		}
		if (curve == nullptr)
		{
			continue;
		}
		for (const std::size_t offset : {
			std::size_t{0}, std::size_t{0x20},
			std::size_t{0x24}, std::size_t{4}})
		{
			const std::uint16_t point = static_cast<std::uint16_t>(
				io::read_le32(curve + offset));
			if (point >= runtime.object_count)
			{
				return false;
			}
			const game::WorldObject* live = game::world_resolve(
				world, runtime.objects[point].live);
			if (live == nullptr
				&& !runtime_activate_object(
					runtime, point, world, stats))
			{
				return false;
			}
		}
	}
	return true;
}

bool runtime_activate_group(
	Runtime& runtime,
	std::uint16_t group_index,
	game::World& world,
	const assets::ShipStatsTable& stats)
{
	if (!runtime.ready || group_index >= runtime.group_count)
	{
		return false;
	}
	const GroupRecord& group = runtime.groups[group_index];
	std::uint16_t activated = 0;
	for (std::uint16_t ordinal = 0; ordinal < group.member_count; ++ordinal)
	{
		const std::uint16_t mission_index =
			runtime.group_members[group.first_member + ordinal];
		const game::WorldObject* before = game::world_resolve(
			world, runtime.objects[mission_index].live);
		const bool was_live =
			before != nullptr && before->type != 1001;
		if (!runtime_activate_object(
				runtime,
				mission_index,
				world,
				stats))
		{
			diagnostics::mission_log(
				"flight group create failed group=%u mission=%u "
				"created=%u",
				static_cast<unsigned>(group_index),
				static_cast<unsigned>(mission_index),
				static_cast<unsigned>(activated));
			return false;
		}
		if (!was_live)
		{
			++activated;
		}
	}
	if (activated != 0)
	{
		diagnostics::mission_log(
			"flight group created group=%u created=%u active=%u",
			static_cast<unsigned>(group_index),
			static_cast<unsigned>(activated),
			static_cast<unsigned>(runtime.active_object_count));
	}
	rebuild_group_allegiances(runtime, world);
	return true;
}

void runtime_deactivate_group(
	Runtime& runtime,
	std::uint16_t group_index,
	game::World& world)
{
	if (!runtime.ready || group_index >= runtime.group_count)
	{
		return;
	}
	const GroupRecord& group = runtime.groups[group_index];
	std::uint16_t departed = 0;
	for (std::uint16_t ordinal = 0; ordinal < group.member_count; ++ordinal)
	{
		ObjectRecord& object =
			runtime.objects[
				runtime.group_members[group.first_member + ordinal]];
		game::WorldObject* live =
			game::world_resolve(world, object.live);
		if (live == nullptr)
		{
			continue;
		}
		if (group_destroy_releases_atmosphere(live->type))
		{
			game::world_depart_planet_atmosphere(
				world, object.live);
		}
		if (game::world_mark_departed(world, object.live))
		{
			object.activation_service_pending = false;
			++departed;
		}
	}
	if (departed != 0)
	{
		diagnostics::mission_log(
			"flight group departed group=%u departed=%u active=%u",
			static_cast<unsigned>(group_index),
			static_cast<unsigned>(departed),
			static_cast<unsigned>(runtime.active_object_count));
	}
}

void runtime_update_flyback(
	Runtime& runtime,
	game::World& world)
{
	game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (player == nullptr)
	{
		return;
	}
	player->nav_point = UINT16_MAX;
	for (std::uint8_t index = 0;
		index < runtime.flyback_marker_count;
		++index)
	{
		const std::uint16_t marker = runtime.flyback_markers[index];
		if (marker == UINT16_MAX)
		{
			continue;
		}
		if (marker >= std::size(world.objects)
			|| !world.objects[marker].active)
		{
			runtime.flyback_markers[index] = UINT16_MAX;
			continue;
		}
		game::WorldObject* object = &world.objects[marker];
		if (glm::length(object->position - player->position)
			> runtime.flyback_radii[index])
		{
			player->nav_point = marker;
			return;
		}
	}
}

game::WorldObject* runtime_resolve_object(
	Runtime& runtime,
	std::uint16_t mission_index,
	game::World& world)
{
	return mission_index < runtime.object_count
		? game::world_resolve(world, runtime.objects[mission_index].live)
		: nullptr;
}

const game::WorldObject* runtime_resolve_object(
	const Runtime& runtime,
	std::uint16_t mission_index,
	const game::World& world)
{
	return mission_index < runtime.object_count
		? game::world_resolve(world, runtime.objects[mission_index].live)
		: nullptr;
}

namespace
{
int reference_object_owner(
	const Runtime& runtime,
	std::uint16_t reference_span)
{
	for (std::uint16_t index = 0; index < runtime.object_count; ++index)
	{
		if (runtime.objects[index].reference_span == reference_span)
		{
			return index;
		}
	}
	return -1;
}

int reference_group_owner(
	const Runtime& runtime,
	std::uint16_t reference_span)
{
	for (std::uint16_t index = 0; index < runtime.group_count; ++index)
	{
		if (runtime.groups[index].reference_span == reference_span)
		{
			return index;
		}
	}
	return -1;
}

int reference_set_owner(
	const Runtime& runtime,
	std::uint16_t reference_span)
{
	for (std::uint16_t index = 0;
		index < runtime.reference_set_count;
		++index)
	{
		if (runtime.reference_sets[index].reference_span == reference_span)
		{
			return index;
		}
	}
	return -1;
}

void expand_reference(
	const Runtime& runtime,
	ReferenceKind kind,
	std::uint16_t index,
	std::uint16_t* output,
	std::uint16_t capacity,
	std::uint16_t& count,
	std::uint16_t depth)
{
	if (count >= capacity)
	{
		return;
	}
	if (kind == ReferenceKind::object)
	{
		if (index < runtime.object_count)
		{
			output[count++] = index;
		}
		return;
	}
	if (kind == ReferenceKind::group)
	{
		if (index >= runtime.group_count)
		{
			return;
		}
		const GroupRecord& group = runtime.groups[index];
		for (std::uint16_t ordinal = 0;
			ordinal < group.member_count && count < capacity;
			++ordinal)
		{
			output[count++] =
				runtime.group_members[group.first_member + ordinal];
		}
		return;
	}
	if (index >= runtime.reference_set_count
		|| depth >= runtime.reference_set_count)
	{
		return;
	}
	const ReferenceSetRecord& set = runtime.reference_sets[index];
	if (set.first_link == UINT16_MAX)
	{
		return;
	}
	for (std::uint16_t link_index = set.first_link;
		link_index < runtime.reference_link_count && count < capacity;
		++link_index)
	{
		const ReferenceLink& link = runtime.reference_links[link_index];
		if (link.owner_set != index)
		{
			break;
		}
		const ReferenceSpan& span =
			runtime.reference_spans[link.reference_span];
		if (span.owner_kind == 0)
		{
			const int object =
				reference_object_owner(runtime, link.reference_span);
			if (object >= 0)
			{
				output[count++] = static_cast<std::uint16_t>(object);
			}
		}
		else if (span.owner_kind == 1)
		{
			const int group =
				reference_group_owner(runtime, link.reference_span);
			if (group >= 0)
			{
				expand_reference(
					runtime,
					ReferenceKind::group,
					static_cast<std::uint16_t>(group),
					output,
					capacity,
					count,
					depth);
			}
		}
		else if (span.owner_kind == 2)
		{
			const int nested =
				reference_set_owner(runtime, link.reference_span);
			if (nested >= 0)
			{
				expand_reference(
					runtime,
					ReferenceKind::set,
					static_cast<std::uint16_t>(nested),
					output,
					capacity,
					count,
					static_cast<std::uint16_t>(depth + 1));
			}
		}
	}
}

void expand_target_reference(
	const Runtime& runtime,
	ReferenceKind kind,
	std::uint16_t index,
	ExpandedTargetReference* output,
	std::uint16_t capacity,
	std::uint16_t& count,
	std::uint16_t depth)
{
	if (count >= capacity)
	{
		return;
	}
	if (kind == ReferenceKind::object)
	{
		if (index < runtime.object_count)
		{
			output[count++] = {index, -1};
		}
		return;
	}
	if (kind == ReferenceKind::group)
	{
		if (index >= runtime.group_count)
		{
			return;
		}
		const GroupRecord& group = runtime.groups[index];
		for (std::uint16_t ordinal = 0;
			ordinal < group.member_count && count < capacity;
			++ordinal)
		{
			output[count++] = {
				runtime.group_members[group.first_member + ordinal],
				-1,
			};
		}
		return;
	}
	if (index >= runtime.reference_set_count
		|| depth >= runtime.reference_set_count)
	{
		return;
	}
	const ReferenceSetRecord& set = runtime.reference_sets[index];
	if (set.first_link == UINT16_MAX)
	{
		return;
	}
	for (std::uint16_t link_index = set.first_link;
		link_index < runtime.reference_link_count && count < capacity;
		++link_index)
	{
		const ReferenceLink& link = runtime.reference_links[link_index];
		if (link.owner_set != index)
		{
			break;
		}
		const ReferenceSpan& span =
			runtime.reference_spans[link.reference_span];
		if (span.owner_kind == 0)
		{
			const int object =
				reference_object_owner(runtime, link.reference_span);
			if (object >= 0)
			{
					output[count++] = {
						static_cast<std::uint16_t>(object),
						static_cast<std::int16_t>(
							link.selector == UINT8_MAX
								? -1
								: link.selector),
					};
			}
		}
		else if (span.owner_kind == 1)
		{
			const int group =
				reference_group_owner(runtime, link.reference_span);
			if (group >= 0)
			{
				expand_target_reference(
					runtime,
					ReferenceKind::group,
					static_cast<std::uint16_t>(group),
					output,
					capacity,
					count,
					depth);
			}
		}
		else if (span.owner_kind == 2)
		{
			const int nested =
				reference_set_owner(runtime, link.reference_span);
			if (nested >= 0)
			{
				expand_target_reference(
					runtime,
					ReferenceKind::set,
					static_cast<std::uint16_t>(nested),
					output,
					capacity,
					count,
					static_cast<std::uint16_t>(depth + 1));
			}
		}
	}
}
}

std::uint16_t runtime_expand_reference(
	const Runtime& runtime,
	ReferenceKind kind,
	std::uint16_t index,
	std::uint16_t* output,
	std::uint16_t capacity)
{
	if (output == nullptr || capacity == 0)
	{
		return 0;
	}
	std::uint16_t count = 0;
	expand_reference(
		runtime, kind, index, output, capacity, count, 0);
	return count;
}

std::uint16_t runtime_expand_target_reference(
	const Runtime& runtime,
	ReferenceKind kind,
	std::uint16_t index,
	ExpandedTargetReference* output,
	std::uint16_t capacity)
{
	if (output == nullptr || capacity == 0)
	{
		return 0;
	}
	std::uint16_t count = 0;
	expand_target_reference(
		runtime, kind, index, output, capacity, count, 0);
	return count;
}
}
