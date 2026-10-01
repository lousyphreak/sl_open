#include "game/mission_session.hpp"

#include "ai/runtime.hpp"
#include "core/mission_log.hpp"
#include "frontend/gui_render.hpp"
#include "game/attachments.hpp"
#include "game/disruption_effects.hpp"
#include "game/exhaust_hazard.hpp"
#include "game/model_animation.hpp"
#include "hud/layout.hpp"
#include "hud/render.hpp"
#include "io/vfs.hpp"
#include "localization/language.hpp"
#include "mission/deathmatch_scenarios.hpp"
#include "mission/events.hpp"
#include "mission/director.hpp"
#include "mission/objectives.hpp"
#include "render/frontend_renderer.hpp"
#include "render/mission_renderer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>

namespace sl_open::game
{
namespace
{
constexpr float kErrorButtonWidth = 180.0f;
constexpr float kErrorButtonHeight = 32.0f;
constexpr float kMatchSpeedTargetLimit = 330000.0f;
constexpr std::uint8_t kCommsMenuPanel = 11;
constexpr std::uint8_t kTransientSessionSlots[] = {
	0, 1, 3, 9, 10, 14, 28, 34, 37,
};
static_assert(
	kMultiplayerPlayerCapacity
		== mission::kNetworkPlayerCapacity);
static_assert(
	kMultiplayerPlayerNameBytes
		== mission::kNetworkPlayerNameBytes);
static_assert(
	kMissionLoadoutSlots
		== mission::kNetworkPlayerLoadoutSlots);

void reset_mission_session(MissionSession& session)
{
	// Player_update_throttle owns DAT_0051cf7c. It is loader-zeroed when the
	// retail process starts and is not part of any mission reset, so retain it
	// while reconstructing the mission-owned state around it.
	const float manual_throttle = session.manual_throttle;
	// MissionSession contains the complete fixed-capacity game world.  Aggregate
	// assignment materializes a second MissionSession as a temporary, which is
	// larger than the process stack once the retail-sized runtime pools are
	// enabled.  Reconstruct the existing object in place instead.
	session.~MissionSession();
	::new (static_cast<void*>(&session)) MissionSession{};
	session.manual_throttle = manual_throttle;
}

bool import_campaign_session_state(MissionSession& session)
{
	if (session.request.mode == MissionMode::multiplayer
		&& !session.request.multiplayer.deathmatch_mode)
	{
		if (!mission::runtime_install_multiplayer_bootstrap(
				session.mission_runtime,
				session.request.multiplayer.bootstrap))
		{
			return false;
		}
	}
	else
	{
		for (std::size_t index = 0;
			index < std::size(
				kMultiplayerCampaignVariableSlots);
			++index)
		{
			const std::uint8_t slot =
				kMultiplayerCampaignVariableSlots[index];
			session.mission_runtime.session_state[slot] =
				std::bit_cast<std::uint32_t>(
					session.request
						.persistent_variables[index]);
			session.mission_runtime.session_state_kinds[slot] =
				static_cast<std::uint8_t>(
					mission::ValueKind::scalar);
		}
	}
	// FUN_004124d0, LANCER.EXE 0x004124d0, publishes the authored
	// player-object prefix in mission variable 15 before the executor starts.
	// Offline launch passes one at 0x004aa45a..0x004aa45c; multiplayer passes
	// either its compact co-op prefix or all eight deathmatch player slots.
	session.mission_runtime.session_state[15] =
		session.mission_runtime.player_prefix_count;
	session.mission_runtime.session_state_kinds[15] =
		static_cast<std::uint8_t>(mission::ValueKind::scalar);
	// Gameflow_reset_mission_globals, LANCER.EXE 0x00475620, runs for
	// every campaign mission after the retained VARS state is present.
	for (const std::uint8_t slot : kTransientSessionSlots)
	{
		session.mission_runtime.session_state[slot] = 0;
		session.mission_runtime.session_state_kinds[slot] =
			static_cast<std::uint8_t>(mission::ValueKind::scalar);
	}
	session.mission_runtime.mission_score_events =
		session.request.score_events;
	return true;
}

bool bind_multiplayer_launch(MissionSession& session)
{
	if (session.request.mode != MissionMode::multiplayer)
	{
		return true;
	}
	const MultiplayerLaunchSnapshot& launch =
		session.request.multiplayer;
	mission::Runtime& runtime = session.mission_runtime;
	if (launch.gameplay_player_prefix > runtime.object_count)
	{
		return false;
	}

	bool connected[mission::kNetworkPlayerCapacity]{};
	std::uint8_t one_way_latency[
		mission::kNetworkPlayerCapacity]{};
	std::int32_t teams[mission::kNetworkPlayerCapacity]{};
	for (std::size_t player = 0;
		player < kMultiplayerPlayerCapacity;
		++player)
	{
		connected[player] = launch.players[player].connected;
		one_way_latency[player] =
			launch.players[player].one_way_latency;
		teams[player] = launch.players[player].team;
	}
	const mission::NetworkRole role =
		launch.role == MultiplayerRole::host
			? mission::NetworkRole::host
			: mission::NetworkRole::client;
	mission::network_runtime_configure(
		runtime.network,
		role,
		launch.local_player,
		launch.player_count,
		connected,
		one_way_latency);
	mission::network_runtime_configure_teams(
		runtime.network,
		launch.deathmatch_mode,
		launch.team_mode,
		teams,
		launch.configured_team_count);
	for (std::uint8_t player = 0;
		player < launch.player_count;
		++player)
	{
		mission::network_set_player_metadata(
			runtime.network,
			player,
			launch.players[player].name,
			launch.players[player].latency);
	}
	runtime.network.respawn_targetable =
		launch.respawn_targetable;
	runtime.player_prefix_count =
		launch.gameplay_player_prefix;

	// FUN_00466c10 reads one 0x54-byte entry at
	// 0x00588400 + player * 0x54 before it builds a multiplayer player
	// object: selected ship followed by twenty hardpoint definitions.
	for (std::uint8_t player = 0;
		player < kMultiplayerPlayerCapacity;
		++player)
	{
		runtime.network.player_ship[player] =
			launch.players[player].selected_ship;
		std::copy(
			std::begin(launch.players[player].loadout),
			std::end(launch.players[player].loadout),
			std::begin(
				runtime.network.player_loadout[player]));
		runtime.network.player_loadout_valid[player] =
			launch.players[player].connected;
		if (launch.players[player].connected)
		{
			runtime.objects[player].type =
				static_cast<std::uint16_t>(
					launch.players[player].selected_ship);
		}
	}
	diagnostics::mission_log(
		"multiplayer launch bound role=%u local=%u players=%u "
		"prefix=%u mission=%u seed=%u deathmatch=%u",
		static_cast<unsigned>(role),
		static_cast<unsigned>(launch.local_player),
		static_cast<unsigned>(launch.player_count),
		static_cast<unsigned>(
			launch.gameplay_player_prefix),
		static_cast<unsigned>(
			launch.authoritative_mission),
		launch.authoritative_seed,
		launch.deathmatch_mode ? 1u : 0u);
	return true;
}

void capture_live_player_score(MissionSession& session)
{
	const WorldObject* player =
		world_resolve(session.world, session.world.player);
	if (player != nullptr)
	{
		session.live_player_score = player->score;
	}
}

void capture_terminal_result(
	MissionSession& session,
	SessionResultKind kind,
	std::uint8_t coordinator_result)
{
	if (session.result.campaign_state_captured)
	{
		return;
	}
	capture_live_player_score(session);
	session.result.kind = kind;
	session.result.coordinator_result = coordinator_result;
	const std::int32_t grade = std::bit_cast<std::int32_t>(
		session.mission_runtime.session_state[14]);
	session.result.grade =
		grade >= -1 && grade <= 4
			? static_cast<std::int16_t>(grade)
			: std::int16_t{-1};
	session.result.score_delta = std::bit_cast<std::int32_t>(
		std::bit_cast<std::uint32_t>(session.live_player_score)
		- std::bit_cast<std::uint32_t>(session.request.score));
	session.result.score_events =
		session.mission_runtime.mission_score_events;
	for (std::size_t index = 0;
		index < std::size(kMultiplayerCampaignVariableSlots);
		++index)
	{
		session.result.persistent_variables[index] =
			std::bit_cast<std::int32_t>(
				session.mission_runtime.session_state[
					kMultiplayerCampaignVariableSlots[index]]);
	}
	// FUN_00425240 reads DAT_0052a460 (session slot 28) to select
	// the successful- versus failed-objectives ejection report.
	session.result.objectives_completed_before_ejection =
		session.mission_runtime.session_state[28] != 0;
	session.result.campaign_state_captured = true;
}

void configure_campaign_player_loadout(
	MissionSession& session,
	WorldObject& player,
	ObjectHandle handle)
{
	if (!session.request.campaign_player_configuration
		|| !player.player
		|| !player.components_initialized
		|| session.configured_player == handle)
	{
		return;
	}
	player.special_player_loadout = false;
	const std::uint8_t loadout_column =
		std::min<std::uint8_t>(player.loadout_index, 4);
	for (std::uint8_t index = 0;
		index < player.attachment_count;
		++index)
	{
		std::int16_t definition =
			session.request.player_loadout[index];
		if (definition < -1 || definition > 9)
		{
			definition = -1;
		}
		player.attachments[index].default_loadout[loadout_column] =
			definition;
	}
	attachments_rebuild_selected_loadout(
		player,
		mission::network_is_deathmatch_mission(
			session.mission_runtime.mission_number),
		false);
	session.configured_player = handle;
	session.hud.ordnance_initialized = false;
	diagnostics::mission_log(
		"campaign player configured ship=%u hardpoints=%u",
		static_cast<unsigned>(player.type),
		static_cast<unsigned>(player.attachment_count));
}

struct ModelAnimationEventContext
{
	WeaponRuntime* weapons{};
	const assets::GunStatsTable* gun_stats{};
	const assets::ShipStatsTable* ship_stats{};
};

void fire_model_animation_type0(
	void* raw_context,
	World& world,
	WorldObject& object,
	std::uint16_t model_reference,
	std::uint32_t simulation_tick)
{
	ModelAnimationEventContext& context =
		*static_cast<ModelAnimationEventContext*>(raw_context);
	weapons_fire_model_animation_event(
		*context.weapons,
		world,
		object,
		model_reference,
		*context.gun_stats,
		*context.ship_stats,
		simulation_tick);
}

WorldObject* selected_target(World& world)
{
	return world_resolve(world, world.selected_target);
}

void disable_match_speed(
	MissionSession& session,
	WorldObject& player,
	bool restore_throttle,
	const char* reason)
{
	if (!player.match_speed_active)
	{
		session.match_speed_latched = false;
		return;
	}
	player.match_speed_active = false;
	session.match_speed_latched = false;
	if (restore_throttle)
	{
		session.flight_demand.throttle =
			session.match_speed_saved_throttle;
	}
	diagnostics::mission_log(
		"player match-speed active=0 reason=%s throttle=%.3f",
		reason,
		session.flight_demand.throttle);
}

void update_match_speed(MissionSession& session, WorldObject& player)
{
	if (!player.match_speed_active)
	{
		return;
	}
	WorldObject* target = selected_target(session.world);
	if (target == nullptr)
	{
		disable_match_speed(session, player, true, "no-target");
		return;
	}
	// Player_update_match_speed returns without changing the latch while
	// target object flag 0x100 is set.
	if ((target->runtime_flags & 0x00000100u) != 0)
	{
		return;
	}
	const float distance = glm::length(target->position - player.position);
	if (distance > kMatchSpeedTargetLimit
		|| (target->runtime_flags & kObjectFlagDestroyed) != 0)
	{
		disable_match_speed(session, player, true, "invalid-target");
		return;
	}

	if (!session.match_speed_latched)
	{
		session.match_speed_saved_throttle =
			session.flight_demand.throttle;
		session.match_speed_latched = true;
	}
	if (player.type >= assets::kShipStatsCount)
	{
		disable_match_speed(session, player, true, "invalid-player-type");
		return;
	}
	const float base_speed =
		world_effective_max_speed(
			player,
			session.ship_stats,
			session.world.camera_mode);
	if (base_speed <= 0.0f)
	{
		disable_match_speed(session, player, true, "zero-base-speed");
		return;
	}
	session.flight_demand.throttle =
		std::min(1.0f, target->speed / base_speed);
}

void toggle_match_speed(MissionSession& session, WorldObject& player)
{
	if (player.match_speed_active)
	{
		hud::runtime_enqueue_ui_sound(session.hud, 5);
		disable_match_speed(session, player, true, "control");
		return;
	}
	player.match_speed_active = true;
	session.match_speed_saved_throttle =
		session.flight_demand.throttle;
	session.match_speed_latched = true;
	const bool have_target = selected_target(session.world) != nullptr;
	hud::runtime_enqueue_ui_sound(session.hud, have_target ? 4 : 3);
	diagnostics::mission_log(
		"player match-speed active=1 target=%d",
		have_target
			? static_cast<int>(session.world.selected_target.index)
			: -1);
	update_match_speed(session, player);
}

PlayerControlAxes update_player_flight_controls(
	MissionSession& session,
	const Config& config,
	const input::GameplayInput& input,
	WorldObject* player)
{
	const bool match_speed_owns_throttle =
		player != nullptr && player->match_speed_active;
	const float matched_throttle = session.flight_demand.throttle;
	if (match_speed_owns_throttle)
	{
		// The retail manual-throttle global remains writable while match
		// speed owns the object's applied demand. Keep that saved demand
		// live so disabling match speed restores adjustments made while it
		// was active.
		session.flight_demand.throttle =
			session.match_speed_saved_throttle;
	}
	const bool interface_owns_rotation =
		input.powerball_interface
		|| input.shield_balance_interface;
	const PlayerControlAxes interface_axes =
		player_controls_update_flight_demand(
			config,
			input,
			session.player_control_state,
			session.flight_demand,
			session.manual_throttle,
			session.camera.mode,
			interface_owns_rotation);
	if (match_speed_owns_throttle)
	{
		session.match_speed_saved_throttle =
			session.flight_demand.throttle;
		session.flight_demand.throttle = matched_throttle;
	}
	if (input.shield_balance_interface)
	{
		session.flight_demand.roll = 0.0f;
		session.flight_demand.pitch = 0.0f;
		session.flight_demand.yaw = 0.0f;
	}
	session.flight_demand.afterburner =
		input.held[20] || session.afterburner_toggle;
	session.flight_demand.reverse = input.held[22];
	return interface_axes;
}

void launch_selected_ordnance(MissionSession& session)
{
	WorldObject* player =
		world_resolve(session.world, session.world.player);
	if (player == nullptr
		|| session.hud.selected_ordnance
			>= session.hud.ordnance_count)
	{
		return;
	}
	hud::OrdnanceEntry& selected =
		session.hud.ordnance[session.hud.selected_ordnance];
	// Player_launch_selected_ordnance (LANCER.EXE 0x00412820) accepts every
	// definition represented by the nine-entry player ordnance instrument.
	// Definition six (Solomon) deliberately bypasses the guided-lock switch
	// alongside definition zero, then reaches the ordinary mount launcher.
	const bool launchable =
		selected.type >= 0 && selected.type <= 8;
	const bool launch_disabled =
		(player->runtime_flags & 0x00210000u) != 0;
	if (!launchable || launch_disabled)
	{
		diagnostics::mission_log(
			"player missile blocked type=%d hud_count=%d disabled=%u",
			selected.type,
			selected.count,
			launch_disabled ? 1u : 0u);
		return;
	}

	// Player_launch_selected_ordnance 0x00412965..0x00412a0d routes
	// guided types 1..5, 7, and 8 through retail lock state three. An
	// unlocked request always plays standard FAT sample one and returns
	// while ammunition remains. At zero it additionally rate-limits
	// standard FAT sample zero to one request per 500 ticks.
	if (selected.type != 0
		&& selected.type != 6
		&& session.hud.missile_lock.phase
			!= hud::MissileLockPhase::locked)
	{
		hud::runtime_enqueue_sample(session.hud, 1);
		if (selected.count == 0
			&& session.clock.gameplay_tick
				> session.hud.ordnance_empty_sound_deadline)
		{
			hud::runtime_enqueue_sample(session.hud, 0);
			session.hud.ordnance_empty_sound_deadline =
				static_cast<std::uint32_t>(session.clock.gameplay_tick) + 500;
		}
		diagnostics::mission_log(
			"player missile blocked type=%d reason=lock phase=%u count=%d",
			selected.type,
			static_cast<unsigned>(session.hud.missile_lock.phase),
			selected.count);
		return;
	}

	// Outside deathmatch the first press consumes a live cloak and returns
	// without launching. The deathmatch owner deliberately bypasses this
	// branch, allowing the selected ordnance to fire without dropping cloak.
	if ((player->runtime_flags & 0x00000100u) != 0
		&& !mission::network_is_deathmatch_mission(
			session.mission_runtime.mission_number))
	{
		world_set_cloak_active(
			session.world,
			*player,
			false,
			static_cast<std::uint32_t>(session.clock.gameplay_tick));
		diagnostics::mission_log(
			"player missile blocked reason=cloak-disabled");
		return;
	}

	hud::runtime_open_panel(session.hud, 2);
	session.hud.panels[2].hold = true;
	if (selected.count < 1)
	{
		hud::runtime_enqueue_sample(session.hud, 0);
		diagnostics::mission_log(
			"player missile blocked type=%d reason=empty",
			selected.type);
		return;
	}

	std::uint8_t attachment_index = UINT8_MAX;
	for (std::uint8_t index = 0;
		index < player->attachment_count;
		++index)
	{
		const AttachmentSlot& attachment = player->attachments[index];
		if (attachment.definition_index == selected.type
			&& attachment.remaining_count >= 1)
		{
			attachment_index = index;
			break;
		}
	}
	if (attachment_index == UINT8_MAX)
	{
		diagnostics::mission_log(
			"player missile blocked type=%d reason=no-live-mount",
			selected.type);
		return;
	}

	ObjectHandle target;
	std::int16_t target_component = -1;
	if (session.hud.missile_lock.phase
		== hud::MissileLockPhase::locked
		&& world_resolve(
			session.world,
			session.world.selected_target) != nullptr)
	{
		target = session.world.selected_target;
		target_component = session.world.target_component;
	}
	const bool launched = missiles_launch_from_ship_mount(
		session.missiles,
		session.world,
		session.missile_stats,
		session.world.player,
		attachment_index,
		target,
		target_component,
		static_cast<std::uint32_t>(session.clock.gameplay_tick));
	// Player_launch_selected_ordnance (0x00412ae6) consumes the HUD entry
	// and aggregate after calling the void launcher. Retail therefore also
	// consumes them when the fixed 200-record missile pool is exhausted.
	--selected.count;
	--session.hud.aggregate_ordnance;
	diagnostics::mission_log(
		"hud ordnance fired type=%d launched=%u selected_count=%d aggregate=%d",
		selected.type,
		launched ? 1u : 0u,
		selected.count,
		session.hud.aggregate_ordnance);
}

void adjust_shield_balance(
	WorldObject& player,
	const assets::ShipStatsTable& stats,
	float pointing_y)
{
	if (player.type >= assets::kShipStatsCount)
	{
		return;
	}
	// Player_adjust_shield_balance (LANCER.EXE 0x00412d40) moves one
	// truncated quarter of the authored primary-bank maximum per update.
	// The two player-only accumulators are the globals 0x0051cf78/34
	// drawn by the flight schematic as shape families 178 and 183.
	const std::int32_t authored_maximum =
		stats.records[player.type].object.primary_bank_max;
	const float step = static_cast<float>(authored_maximum / 4);
	const float maximum = static_cast<float>(authored_maximum * 5);
	if (pointing_y > 0.5f && player.primary_shields[2] > 0.0f)
	{
		float& source_pool = player.auxiliary_shields[0];
		float& destination_pool = player.auxiliary_shields[1];
		source_pool -= step;
		if (source_pool < 0.0f)
		{
			player.primary_shields[2] += source_pool;
			const float transfer = -source_pool;
			if (player.primary_shields[2] <= 0.0f)
			{
				const float remainder =
					transfer + player.primary_shields[2];
				player.primary_shields[2] = 0.0f;
				player.primary_shields[3] += remainder;
			}
			else
			{
				player.primary_shields[3] += transfer;
			}
			source_pool = 0.0f;
		}
		else
		{
			player.primary_shields[3] += step;
		}
		if (player.primary_shields[3] > maximum)
		{
			destination_pool += player.primary_shields[3] - maximum;
			player.primary_shields[3] = maximum;
			destination_pool = std::min(destination_pool, maximum);
		}
		return;
	}
	if (pointing_y >= -0.5f || player.primary_shields[3] <= 0.0f)
	{
		return;
	}
	float& source_pool = player.auxiliary_shields[1];
	float& destination_pool = player.auxiliary_shields[0];
	source_pool -= step;
	if (source_pool < 0.0f)
	{
		player.primary_shields[3] += source_pool;
		const float transfer = -source_pool;
		if (player.primary_shields[3] <= 0.0f)
		{
			const float remainder =
				transfer + player.primary_shields[3];
			player.primary_shields[3] = 0.0f;
			player.primary_shields[2] += remainder;
		}
		else
		{
			player.primary_shields[2] += transfer;
		}
		source_pool = 0.0f;
	}
	else
	{
		player.primary_shields[2] += step;
	}
	if (player.primary_shields[2] > maximum)
	{
		destination_pool += player.primary_shields[2] - maximum;
		player.primary_shields[2] = maximum;
		destination_pool = std::min(destination_pool, maximum);
	}
}

void centered_text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* text,
	float center_x,
	float y,
	float scale,
	bgfx::TextureHandle palette)
{
	const float width = frontend::gui::text_width(
		renderer.shell.glyphs,
		renderer.shell.glyph_count,
		text,
		scale);
	render::frontend_text(
		commands,
		renderer,
		text,
		center_x - width * 0.5f,
		y,
		palette,
		0xffffffff,
		scale);
}

const char* multiplayer_player_name(
	const mission::NetworkRuntime& network,
	const LanguageTable& language,
	std::uint8_t player,
	char (&fallback)[48])
{
	if (player < mission::kNetworkPlayerCapacity
		&& network.player_name[player][0] != '\0')
	{
		return network.player_name[player];
	}
	std::snprintf(
		fallback,
		sizeof(fallback),
		"%s %u",
		language_text(language, 0xbf),
		static_cast<unsigned>(player + 1));
	return fallback;
}

void drain_multiplayer_hud_messages(
	MissionSession& session,
	const LanguageTable& language)
{
	mission::PlayerCommsState& comms =
		session.mission_runtime.player_comms;
	if (comms.multiplayer_landing_notice_pending)
	{
		comms.multiplayer_landing_notice_pending = false;
		hud::runtime_enqueue_message(
			session.hud,
			language_text(language, 0x558),
			session.clock.gameplay_tick);
	}
	mission::NetworkRuntime& network =
		session.mission_runtime.network;
	mission::NetworkChatMessage chat;
	while (mission::network_pop_chat(network, chat))
	{
		char fallback[48];
		char line[100];
		std::snprintf(
			line,
			sizeof(line),
			"%s: %s",
			multiplayer_player_name(
				network,
				language,
				chat.source_player,
				fallback),
			chat.text);
		hud::runtime_enqueue_message(
			session.hud,
			line,
			session.clock.gameplay_tick);
	}

	DeathHudNotification notification;
	while (death_effects_pop_hud_notification(
		session.world.death_effects,
		notification))
	{
		char victim_fallback[48];
		char attacker_fallback[48];
		const char* victim = multiplayer_player_name(
			network,
			language,
			notification.victim,
			victim_fallback);
		const char* attacker = multiplayer_player_name(
			network,
			language,
			notification.attacker,
			attacker_fallback);
		const char* bonus = notification.local_bonus
			? language_text(language, 0x2f4)
			: "";
		char line[100];
		switch (notification.kind)
		{
		case DeathHudNotificationKind::ordinary_kill:
			std::snprintf(
				line,
				sizeof(line),
				language_text(language, 0x546),
				victim,
				attacker,
				bonus);
			break;
		case DeathHudNotificationKind::bought_the_farm:
			std::snprintf(
				line,
				sizeof(line),
				language_text(language, 0x547),
				victim);
			break;
		case DeathHudNotificationKind::uber_kill:
			std::snprintf(
				line,
				sizeof(line),
				language_text(language, 0x301),
				attacker,
				victim,
				bonus);
			break;
		default:
			continue;
		}
		hud::runtime_enqueue_message(
			session.hud,
			line,
			session.clock.gameplay_tick);
	}
}

bool mission_session_online(const MissionSession& session)
{
	return session.mission_runtime.network.role
		!= mission::NetworkRole::offline;
}

frontend::GameplayPauseView mission_session_pause_view(
	const MissionSession& session)
{
	frontend::GameplayPauseView view;
	const mission::Runtime& runtime = session.mission_runtime;
	const mission::NetworkRuntime& network = runtime.network;
	for (std::uint8_t player = 0;
		player < mission::kNetworkPlayerCapacity;
		++player)
	{
		frontend::GameplayPausePlayer& output =
			view.players[player];
		output.name = network.player_name[player];
		output.kills = network.player_kills[player];
		output.deaths = network.player_deaths[player];
		output.latency = session.network_pause_latency[player];
		output.team = network.object_team[player];
		output.scenario_shape =
			mission::deathmatch_scenarios_scoreboard_shape(
				runtime, session.world, player);
		output.scenario_value =
			mission::deathmatch_scenarios_scoreboard_value(
				runtime, session.world, player);
		output.connected = network.connected[player];
		output.rejectable =
			session.network_pause_rejectable[player];
	}
	std::copy(
		std::begin(network.team_score),
		std::end(network.team_score),
		std::begin(view.team_score));
	std::copy(
		std::begin(network.team_deaths),
		std::end(network.team_deaths),
		std::begin(view.team_deaths));
	view.recent_message_count = std::min<std::uint8_t>(
		session.hud.message_count,
		static_cast<std::uint8_t>(
			frontend::kGameplayPauseMessageCapacity));
	for (std::uint8_t index = 0;
		index < view.recent_message_count;
		++index)
	{
		view.recent_messages[index] =
			session.hud.messages[index].text;
	}
	view.chat_text = session.hud.chat_message;
	view.local_player = network.local_player;
	view.pause_owner = session.network_pause_owner;
	view.pause_reason = static_cast<std::uint8_t>(
		session.network_pause_reason);
	view.configured_team_count =
		network.configured_team_count;
	view.deathmatch_mode = network.deathmatch_mode;
	view.team_mode = network.team_mode;
	view.chat_active = session.hud.chat_active;
	view.online_overlay = mission_session_online(session);
	return view;
}

void latch_pause_rejectable_players(MissionSession& session)
{
	std::fill(
		std::begin(session.network_pause_rejectable),
		std::end(session.network_pause_rejectable),
		false);
	if (!mission_session_online(session))
	{
		return;
	}
	const mission::NetworkRuntime& network =
		session.mission_runtime.network;
	for (std::uint8_t player = 0;
		player < mission::kNetworkPlayerCapacity;
		++player)
	{
		session.network_pause_rejectable[player] =
			network.connected[player]
			&& player != network.local_player
			&& session.network_pause_latency[player] > 100u;
	}
}

bool mission_session_chat_allowed(const MissionSession& session)
{
	return session.state == MissionSessionState::running
		|| (session.state == MissionSessionState::paused
			&& mission_session_online(session)
			&& session.pause.mode
				== frontend::GameplayPauseMode::online);
}

void enter_mission_pause(
	MissionSession& session,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now)
{
	frontend::gameplay_pause_reset(
		session.pause,
		drawable_width,
		drawable_height,
		now,
		mission_session_online(session));
	latch_pause_rejectable_players(session);
	std::fill(
		std::begin(session.pending_actions),
		std::end(session.pending_actions),
		false);
	session.state = MissionSessionState::paused;
}

void leave_mission_pause(
	MissionSession& session,
	std::uint64_t now)
{
	session.state = MissionSessionState::running;
	simulation_clock_reset(session.clock, now);
}

void apply_pending_network_pause(
	MissionSession& session,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now)
{
	if (!session.network_pause_transition_pending)
	{
		return;
	}
	if (session.pause.mode
		== frontend::GameplayPauseMode::network_termination)
	{
		// Coordinator nine owns the frozen terminal frame. Ordinary pause
		// packets may still arrive while its acknowledgment is visible, but
		// neither a late pause nor an unauthorised resume can replace it.
		session.network_pause_transition_pending = false;
		return;
	}
	const bool active = session.network_pause_transition_active;
	session.network_pause_transition_pending = false;
	if (active)
	{
		if (session.state == MissionSessionState::running)
		{
			enter_mission_pause(
				session, drawable_width, drawable_height, now);
		}
		return;
	}

	// Receive case 0x27 calls the shared resume owner even when the sender is
	// not the retained pause owner. Resetting the simulation clock here keeps
	// paused wall time out of the first resumed update.
	if (session.state == MissionSessionState::running
		|| session.state == MissionSessionState::paused)
	{
		leave_mission_pause(session, now);
	}
}
}

bool mission_session_start(
	MissionSession& session,
	io::Vfs& vfs,
	const assets::GameStats& stats,
	const MissionLaunchRequest& request,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now)
{
	reset_mission_session(session);
	session.request = request;
	session.result.mission = request.mission;
	session.entered_at = now;
	session.error_pointer_x = drawable_width * 0.5f;
	session.error_pointer_y = drawable_height * 0.5f;
	if (!stats.ready
		|| !valid_launch_request(request)
		|| (request.mode == MissionMode::multiplayer
			&& mission::network_is_deathmatch_mission(
				request.mission)
				!= request.multiplayer.deathmatch_mode))
	{
		session.state = MissionSessionState::load_failed;
		session.result.kind = SessionResultKind::load_failed;
		session.load_error = mission::DteLoadError::not_found;
		std::snprintf(
			session.load_error_detail,
			sizeof(session.load_error_detail),
			"%s",
			mission::dte_load_error_text(session.load_error));
		return false;
	}

	char path[64];
	std::snprintf(
		path,
		sizeof(path),
		"missions/mission%u.dte",
		static_cast<unsigned>(
			request.mission == 25 && request.mission_25_alternate
				? 251
				: request.mission));
	if (!mission::dte_load(vfs, path, session.mission_file))
	{
		session.state = MissionSessionState::load_failed;
		session.result.kind = SessionResultKind::load_failed;
		session.load_error = session.mission_file.error;
		std::snprintf(
			session.load_error_detail,
			sizeof(session.load_error_detail),
			"%s",
			mission::dte_load_error_text(session.load_error));
		return false;
	}
	// The retail tables are process globals loaded before frontend use. Keep
	// one parsed source for both loadout presentation and each reset session.
	session.ship_stats = stats.ships;
	session.pilot_stats = stats.pilots;
	session.gun_stats = stats.guns;
	session.missile_stats = stats.missiles;
	world_reset(session.world, request.random_seed);
	session.world.light_maps_enabled = request.light_maps;
	if (!particle_system_ready(session.world.particles))
	{
		session.state = MissionSessionState::load_failed;
		session.result.kind = SessionResultKind::load_failed;
		std::snprintf(
			session.load_error_detail,
			sizeof(session.load_error_detail),
			"%s",
			"shared particle runtime could not be initialized");
		return false;
	}
	chaff_reset(session.chaff);
	missiles_reset(session.missiles);
	weapons_reset(session.weapons);
	session.weapons.feedback_enabled = request.force_feedback;
	if (!mission::runtime_initialize(
			session.mission_runtime, session.mission_file))
	{
		session.state = MissionSessionState::load_failed;
		session.result.kind = SessionResultKind::load_failed;
		std::snprintf(
			session.load_error_detail,
			sizeof(session.load_error_detail),
			"%s",
			"mission runtime tables are invalid");
		return false;
	}
	// Mission-system initialization clears the network globals. The
	// multiplayer owner binds its complete lobby/start snapshot only after
	// that reset and before any selected player or scripted object can be
	// instantiated (retail launch owner 0x004a9728..0x004a9ce6).
	if (!bind_multiplayer_launch(session))
	{
		session.state = MissionSessionState::load_failed;
		session.result.kind = SessionResultKind::load_failed;
		std::snprintf(
			session.load_error_detail,
			sizeof(session.load_error_detail),
			"%s",
			"multiplayer player prefix exceeds the mission object table");
		return false;
	}
	session.world.player_prefix_count =
		session.mission_runtime.player_prefix_count;
	session.world.network_active =
		session.mission_runtime.network.role
			!= mission::NetworkRole::offline;
	world_reserve_mission_slots(
		session.world,
		session.mission_runtime.object_count);
	session.mission_runtime.mission_number = request.mission;
	session.mission_runtime.mission_25_alternate =
		request.mission_25_alternate;
	session.mission_runtime.difficulty =
		std::min<std::uint8_t>(request.difficulty, 2);
	session.mission_runtime.player_pilot_family =
		request.player_pilot_family;
	mission::objectives_reset(session.mission_runtime);
	// SimPod dispatch keeps two independent retail flags. Mission 29 sets
	// DAT_00524fe4=2 and DAT_0057e044=1 at 0x0044f6a2..0x0044f6bb;
	// training missions 30-32 set only DAT_00524fe4=1 at
	// 0x0044f665..0x0044f68c.
	session.mission_runtime.simulator_mode =
		request.mode == MissionMode::instant_action;
	session.mission_runtime.object_factory_mode =
		request.mode == MissionMode::instant_action
			? 2
			: request.mode == MissionMode::training
				? 1
				: 0;
	if (!import_campaign_session_state(session))
	{
		session.state = MissionSessionState::load_failed;
		session.result.kind = SessionResultKind::load_failed;
		std::snprintf(
			session.load_error_detail,
			sizeof(session.load_error_detail),
			"%s",
			"multiplayer mission bootstrap does not match "
			"the authored mission");
		return false;
	}
	if (request.campaign_player_configuration
		&& request.selected_ship >= 0
		&& request.selected_ship <= 11
		&& session.mission_runtime.network.local_player
			< session.mission_runtime.object_count)
	{
		session.mission_runtime.objects[
			session.mission_runtime.network.local_player].type =
				static_cast<std::uint16_t>(request.selected_ship);
	}
	if (!mission::runtime_activate_curve_points(
			session.mission_runtime,
			session.world,
			session.ship_stats,
			session.mission_file))
	{
		session.state = MissionSessionState::load_failed;
		session.result.kind = SessionResultKind::load_failed;
		std::snprintf(
			session.load_error_detail,
			sizeof(session.load_error_detail),
			"mission curve spatial anchors could not be initialized");
		return false;
	}
	diagnostics::mission_log(
		"load mission=%u dte=%u seed=%u objects=%u groups=%u triggers=%u",
		static_cast<unsigned>(request.mission),
		static_cast<unsigned>(
			request.mission == 25 && request.mission_25_alternate
				? 251
				: request.mission),
		request.random_seed,
		static_cast<unsigned>(session.mission_runtime.object_count),
		static_cast<unsigned>(session.mission_runtime.group_count),
		static_cast<unsigned>(session.mission_runtime.trigger_count));
	if (!mission::executor_initialize(
			session.executor,
			session.mission_runtime,
			session.world,
			session.ship_stats,
			session.mission_file,
			0))
	{
		session.state = MissionSessionState::load_failed;
		session.result.kind = SessionResultKind::load_failed;
		std::snprintf(
			session.load_error_detail,
			sizeof(session.load_error_detail),
			"executor failed at 0x%x (opcode %u)",
			session.executor.error_pc,
			static_cast<unsigned>(session.executor.error_opcode));
		return false;
	}
	mission::deathmatch_scenarios_initialize(
		session.mission_runtime,
		session.world,
		session.ship_stats,
		static_cast<std::uint32_t>(session.clock.gameplay_tick));
	mission::environment_bind_controller_lights(
		session.mission_runtime.environment,
		session.world);
	if (WorldObject* player = world_resolve(session.world, session.world.player))
	{
		player->score = request.score;
	}
	session.live_player_score = request.score;
	camera_runtime_reset(
		session.camera,
		session.world,
		session.ship_stats,
		static_cast<std::uint32_t>(session.clock.gameplay_tick));
	session.camera.graphics_quality = request.graphics_detail;
	switch (request.default_view)
	{
	case 0:
		session.camera.view_state = 1;
		break;
	case 1:
		session.camera.view_state = 2;
		session.camera.follow_distance = 1500.0f;
		camera_runtime_request(
			session.camera,
			session.world,
			session.missiles,
			session.ship_stats,
			0,
			session.world.player.index,
			false,
			false,
			0,
			static_cast<std::uint32_t>(
				session.clock.gameplay_tick));
		break;
	case 2:
	default:
		session.camera.view_state = 0;
		break;
	}
	session.camera.multiplayer =
		session.mission_runtime.network.role
			!= mission::NetworkRole::offline;
	session.camera_mode =
		static_cast<std::uint8_t>(session.camera.mode);
	session.previous_frame_camera_mode = session.camera_mode;
	camera_runtime_publish_world_state(
		session.camera, session.world);

	session.state = MissionSessionState::running;
	session.result.kind = SessionResultKind::none;
	simulation_clock_reset(session.clock, now);
	hud::runtime_reset(
		session.hud,
		drawable_width,
		drawable_height,
		request.random_seed);
	frontend::gameplay_pause_reset(
		session.pause,
		drawable_width,
		drawable_height,
		now,
		mission_session_online(session));
	diagnostics::mission_log(
		"ready mission=%u active=%u",
		static_cast<unsigned>(request.mission),
		static_cast<unsigned>(
			session.mission_runtime.active_object_count));
	return true;
}

bool mission_session_capture_multiplayer_bootstrap(
	const MissionSession& session,
	MultiplayerMissionBootstrap& bootstrap)
{
	bootstrap = {};
	if (session.request.mode != MissionMode::multiplayer
		|| session.request.multiplayer.deathmatch_mode
		|| !mission::runtime_capture_multiplayer_bootstrap(
			session.mission_runtime, bootstrap))
	{
		return false;
	}
	return valid_multiplayer_mission_bootstrap(
		bootstrap,
		session.request.multiplayer.authoritative_mission,
		session.request.multiplayer.player_count,
		false);
}

bool mission_session_service_object_activations(
	MissionSession& session)
{
	bool serviced = false;
	for (std::uint16_t mission_index = 0;
		mission_index < session.mission_runtime.object_count;
		++mission_index)
	{
		mission::ObjectRecord& record =
			session.mission_runtime.objects[mission_index];
		WorldObject* actor = world_resolve(session.world, record.live);
		if (actor != nullptr)
		{
			configure_campaign_player_loadout(
				session, *actor, record.live);
		}
		if (!record.activation_service_pending)
		{
			continue;
		}
		if (actor == nullptr)
		{
			record.activation_service_pending = false;
			continue;
		}
		if (!actor->components_initialized)
		{
			continue;
		}
		if (actor->ai.command_count == 0
			|| actor->ai.commands[0].id != 104)
		{
			record.activation_service_pending = false;
			continue;
		}
		ai::runtime_service_object_command(
			*actor,
			session.world,
			session.mission_runtime,
			session.mission_file,
			session.ship_stats,
			session.pilot_stats,
			session.gun_stats,
			session.missile_stats,
			session.weapons,
			session.missiles,
			session.chaff,
			session.flight_demand,
			session.hud.distortion_random_seed,
			static_cast<std::uint32_t>(session.clock.gameplay_tick));
		serviced = true;
		if (actor->ai.command_count == 0
			|| actor->ai.commands[0].id != 104
			|| !actor->ai.work.begin_pending)
		{
			record.activation_service_pending = false;
			diagnostics::mission_log(
				"launch-linked object serviced mission=%u slot=%u tick=%u",
				static_cast<unsigned>(mission_index),
				static_cast<unsigned>(record.live.index),
				static_cast<unsigned>(session.clock.gameplay_tick));
		}
	}
	return serviced;
}

void mission_session_stop(MissionSession& session)
{
	if (session.state != MissionSessionState::inactive)
	{
		diagnostics::mission_log(
			"stop mission=%u result=%u tick=%u active=%u",
			static_cast<unsigned>(session.request.mission),
			static_cast<unsigned>(session.result.kind),
			session.clock.gameplay_tick,
			static_cast<unsigned>(
				session.mission_runtime.active_object_count));
	}
	mission::network_runtime_shutdown(
		session.mission_runtime.network);
	// GameObjects_runtime_shutdown (LANCER.EXE 0x004666b0) destroys every
	// live allocation before shared object resources are released.
	for (std::uint16_t index = 0; index < kMaxGameObjects; ++index)
	{
		WorldObject& object = session.world.objects[index];
		if (object.active)
		{
			world_destroy(
				session.world,
				{index, object.generation});
		}
	}
	particle_system_shutdown(session.world.particles);
	transition_effects_shutdown(session.world.transition_effects);
	reset_mission_session(session);
}

bool mission_session_begin_network_termination_acknowledgement(
	MissionSession& session,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now)
{
	if (!mission_session_online(session)
		|| session.mission_runtime.gameplay_state != 9
		|| session.result.kind != SessionResultKind::none
		|| (session.state != MissionSessionState::running
			&& session.state != MissionSessionState::paused))
	{
		return false;
	}
	if (session.state == MissionSessionState::paused
		&& session.pause.mode
			== frontend::GameplayPauseMode::network_termination)
	{
		return true;
	}

	session.network_pause_transition_pending = false;
	frontend::gameplay_pause_reset_network_termination(
		session.pause,
		drawable_width,
		drawable_height,
		now);
	std::fill(
		std::begin(session.pending_actions),
		std::end(session.pending_actions),
		false);
	session.state = MissionSessionState::paused;
	simulation_clock_reset(session.clock, now);
	diagnostics::mission_log(
		"network abort awaiting acknowledgement tick=%u",
		session.clock.gameplay_tick);
	return true;
}

void mission_session_set_pause(
	MissionSession& session,
	bool active,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now,
	mission::NetworkPauseReason reason)
{
	if (session.pause.mode
		== frontend::GameplayPauseMode::network_termination)
	{
		return;
	}
	const std::uint8_t raw_reason =
		static_cast<std::uint8_t>(reason);
	if (raw_reason >= 16u)
	{
		return;
	}

	session.network_pause_transition_pending = false;
	mission::NetworkRuntime& network =
		session.mission_runtime.network;
	if (active)
	{
		if (session.state != MissionSessionState::running)
		{
			return;
		}
		if (mission_session_online(session))
		{
			session.network_pause_owner = network.local_player;
			session.network_pause_reason = reason;
		}
		enter_mission_pause(
			session, drawable_width, drawable_height, now);
		(void)mission::network_publish_pause_state(
			network, true, reason);
		return;
	}
	if (session.state != MissionSessionState::paused)
	{
		return;
	}
	leave_mission_pause(session, now);
	(void)mission::network_publish_pause_state(
		network, false, session.network_pause_reason);
}

void mission_session_toggle_pause(
	MissionSession& session,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now)
{
	if (session.pause.mode
		== frontend::GameplayPauseMode::network_termination)
	{
		return;
	}
	if (session.state == MissionSessionState::running
		|| session.state == MissionSessionState::paused)
	{
		mission_session_set_pause(
			session,
			session.state == MissionSessionState::running,
			drawable_width,
			drawable_height,
			now,
			mission::NetworkPauseReason::player_request);
	}
}

void mission_session_update_network_latency(
	MissionSession& session,
	std::uint8_t player,
	std::uint32_t smoothed_one_way_ticks,
	std::uint8_t raw_one_way_ticks,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now)
{
	if (!mission_session_online(session)
		|| player >= mission::kNetworkPlayerCapacity)
	{
		return;
	}
	mission::NetworkRuntime& network =
		session.mission_runtime.network;
	if (!network.connected[player])
	{
		return;
	}
	network.player_latency[player] =
		player == network.local_player
			? 0u
			: smoothed_one_way_ticks;
	network.one_way_latency[player] =
		player == network.local_player
			? 0u
			: raw_one_way_ticks;
	session.network_pause_latency[player] =
		network.player_latency[player];

	if (session.state == MissionSessionState::paused
		&& session.pause.mode
			== frontend::GameplayPauseMode::online
		&& player != network.local_player
		&& session.network_pause_latency[player] > 100u)
	{
		// Frontend_screen_online_pause latches eligibility for the lifetime
		// of this pause; recovery below the threshold does not hide REJECT.
		session.network_pause_rejectable[player] = true;
	}
	if (session.state == MissionSessionState::running
		&& session.result.kind == SessionResultKind::none
		&& player != network.local_player
		&& session.network_pause_latency[player] > 150u)
	{
		mission_session_set_pause(
			session,
			true,
			drawable_width,
			drawable_height,
			now,
			mission::NetworkPauseReason::connection_stall);
	}
}

void mission_session_set_pointer(
	MissionSession& session,
	const render::FrontendRenderer& renderer,
	float x,
	float y,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	bool inside)
{
	if (session.state == MissionSessionState::paused)
	{
		const frontend::GameplayPauseView view =
			mission_session_pause_view(session);
		frontend::gameplay_pause_set_pointer(
			session.pause,
			view,
			renderer,
			x,
			y,
			drawable_width,
			drawable_height,
			inside);
		return;
	}
	if (session.state != MissionSessionState::load_failed)
	{
		if (session.state == MissionSessionState::running)
		{
			hud::runtime_set_pointer(session.hud, x, y, inside);
		}
		return;
	}
	session.error_pointer_x = x;
	session.error_pointer_y = y;
	const hud::Layout layout =
		hud::make_layout(drawable_width, drawable_height);
	const float button_width = kErrorButtonWidth * layout.element_scale;
	const float button_height = kErrorButtonHeight * layout.element_scale;
	const float left = (layout.width - button_width) * 0.5f;
	const float top = layout.height * 0.67f;
	session.error_return_hovered = inside
		&& x > left && x < left + button_width
		&& y > top && y < top + button_height;
}

frontend::GameplayPauseAction mission_session_select(
	MissionSession& session)
{
	if (session.state == MissionSessionState::load_failed)
	{
		if (session.error_return_hovered)
		{
			mission_session_request_exit(session);
		}
		return frontend::GameplayPauseAction::none;
	}
	if (session.state != MissionSessionState::paused)
	{
		return frontend::GameplayPauseAction::none;
	}
	const frontend::GameplayPauseView view =
		mission_session_pause_view(session);
	const frontend::GameplayPauseSelection selection =
		frontend::gameplay_pause_select(session.pause, view);
	const frontend::GameplayPauseAction action =
		selection.action;
	if (action
		== frontend::GameplayPauseAction::acknowledge_termination)
	{
		// Frontend_screen_network_termination, LANCER.EXE
		// 0x00492140..0x00492164, resumes the paused network session and
		// replaces coordinator state nine with the ordinary exit-session
		// result four after the notice has been acknowledged.
		session.network_pause_transition_pending = false;
		session.state = MissionSessionState::running;
		session.clock.initialized = false;
		(void)mission::network_publish_pause_state(
			session.mission_runtime.network,
			false,
			session.network_pause_reason);
		capture_terminal_result(
			session, SessionResultKind::exit_to_origin, 4);
		diagnostics::mission_log(
			"network abort acknowledged tick=%u",
			session.clock.gameplay_tick);
	}
	else if (action == frontend::GameplayPauseAction::continue_mission)
	{
		session.network_pause_transition_pending = false;
		session.state = MissionSessionState::running;
		session.clock.initialized = false;
		(void)mission::network_publish_pause_state(
			session.mission_runtime.network,
			false,
			session.network_pause_reason);
	}
	else if (action == frontend::GameplayPauseAction::restart)
	{
		mission::deathmatch_scenarios_publish_restart(
			session.mission_runtime);
		session.result.kind = SessionResultKind::restart;
	}
	else if (action == frontend::GameplayPauseAction::leave_mission)
	{
		if (mission_session_online(session))
		{
			// Frontend_screen_online_pause resumes through the shared owner
			// and publishes pause_active=false before selecting coordinator
			// result four.
			session.network_pause_transition_pending = false;
			session.state = MissionSessionState::running;
			session.clock.initialized = false;
			(void)mission::network_publish_pause_state(
				session.mission_runtime.network,
				false,
				session.network_pause_reason);
		}
		mission_session_request_exit(session);
	}
	else if (action == frontend::GameplayPauseAction::reject_player)
	{
		if (mission::network_publish_player_departure(
				session.mission_runtime.network,
				selection.player))
		{
			// Frontend_screen_online_pause sends opcode 0x4c, flushes the
			// guaranteed buffer, then invokes the ordinary player-removal
			// owner locally (LANCER.EXE 0x00491fc0). The platform drains
			// this semantic queue after input dispatch; apply the same local
			// removal now while the transport still retains the target for
			// guaranteed relay.
			(void)mission::network_receive_player_departure(
				session.mission_runtime,
				session.world,
				session.ship_stats,
				selection.player);
		}
	}
	return action;
}

void mission_session_request_exit(MissionSession& session)
{
	session.result.kind = SessionResultKind::exit_to_origin;
	session.result.coordinator_result = 4;
}

void mission_session_fail_load(
	MissionSession& session,
	const char* detail)
{
	session.state = MissionSessionState::load_failed;
	session.result.kind = SessionResultKind::load_failed;
	std::snprintf(
		session.load_error_detail,
		sizeof(session.load_error_detail),
		"%s",
		detail == nullptr
			? "required mission asset could not be loaded"
			: detail);
	diagnostics::mission_log(
		"fault tick=%u detail=%s",
		session.clock.gameplay_tick,
		session.load_error_detail);
}

void mission_session_update(
	MissionSession& session,
	const Config& config,
	const LanguageTable& language,
	input::GameplayInputPoller& input_poller,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now)
{
	apply_pending_network_pause(
		session, drawable_width, drawable_height, now);
	if (session.mission_runtime.gameplay_state == 9
		&& session.result.kind == SessionResultKind::none)
	{
		// Frontend_screen_online_pause selects FUN_0048e370 for coordinator
		// nine. Keep the final rendered world intact and freeze simulation
		// until its single OK record is acknowledged.
		if (mission_session_online(session))
		{
			(void)mission_session_begin_network_termination_acknowledgement(
				session,
				drawable_width,
				drawable_height,
				now);
			simulation_clock_reset(session.clock, now);
			return;
		}

		capture_terminal_result(
			session, SessionResultKind::mission_failed, 9);
		diagnostics::mission_log(
			"network abort accepted tick=%u",
			session.clock.gameplay_tick);
		return;
	}
	if (session.state != MissionSessionState::running)
	{
		simulation_clock_reset(session.clock, now);
		return;
	}
	capture_live_player_score(session);

	const std::uint32_t steps =
		simulation_clock_advance(session.clock, now);
	const std::uint32_t frame_tick =
		session.clock.gameplay_tick + steps;
	session.mission_runtime.frame_delta_ticks = steps;
	session.camera.current_time_tick = frame_tick;
	if (steps == 0)
	{
		// LANCER.EXE 0x004924b0 encloses gameplay dispatch in
		// a nonzero frame-delta-tick test. Preserve input edges for the next
		// admitted
		// update instead of advancing any state on a render-only iteration.
		return;
	}
	input::GameplayInput input;
	input::gameplay_input_capture_analog(input_poller, input);
	input::gameplay_input_capture_wait_for_key(input_poller, input);
	session.mission_runtime.player_comms.speech_hud_state =
		static_cast<std::uint8_t>(
			session.hud.panels[0].animation);
	input::gameplay_input_set_communications_state(
		input_poller,
		static_cast<std::uint8_t>(
			session.hud.panels[kCommsMenuPanel].animation));
	session.camera.multiplayer =
		session.mission_runtime.network.role
			!= mission::NetworkRole::offline;
	std::copy(
		std::begin(input.wait_for_key_accepted),
		std::end(input.wait_for_key_accepted),
		std::begin(session.mission_runtime.wait_for_key_accepted));

	// Player_update, LANCER.EXE 0x00414060, owns camera input before its
	// chat gate and before every other configurable gameplay action.
	if (session.camera.mode == 6 || session.camera.mode == 12)
	{
		input.camera_yaw_negative =
			input::gameplay_input_raw_keyboard_held(
				input_poller, SDL_SCANCODE_LEFT, 0);
		input.camera_yaw_positive =
			input::gameplay_input_raw_keyboard_held(
				input_poller, SDL_SCANCODE_RIGHT, 0);
		input.camera_zoom_in =
			input::gameplay_input_raw_keyboard_held(
				input_poller, SDL_SCANCODE_UP, 1);
		if (!input.camera_zoom_in)
		{
			input.camera_pitch_positive =
				input::gameplay_input_raw_keyboard_held(
					input_poller, SDL_SCANCODE_UP, 0);
		}
		input.camera_zoom_out =
			input::gameplay_input_raw_keyboard_held(
				input_poller, SDL_SCANCODE_DOWN, 1);
		if (!input.camera_zoom_out)
		{
			input.camera_pitch_negative =
				input::gameplay_input_raw_keyboard_held(
					input_poller, SDL_SCANCODE_DOWN, 0);
		}
	}
	for (std::uint8_t action = 0; action < 8; ++action)
	{
		input.pressed[action] =
			input::gameplay_input_action_pressed(
				input_poller, action);
	}
	const std::int16_t user_camera_request =
		camera_runtime_update_user_controls(
			session.camera, input, steps);
	const bool spectator_camera_input =
		session.camera.spectator_active
		&& input.pressed[0];
	if (spectator_camera_input)
	{
		const bool switched = camera_runtime_select_next_spectator(
			session.camera,
			session.world,
			session.missiles,
			session.ship_stats,
			steps,
			frame_tick);
		if (session.camera.spectator_complete)
		{
			// Spectator_select_next writes DAT_0052a414 when its exact
			// player-prefix scan finds no eligible peer.
			session.mission_runtime.player_death_transition_complete =
				true;
		}
		if (switched)
		{
			// Multiplayer_frame_update polls control zero at
			// 0x004776ed..0x0047771c and invokes Spectator_select_next
			// directly, bypassing the current death/follow camera lock.
			session.camera_mode =
				static_cast<std::uint8_t>(session.camera.mode);
			++session.camera_cut_serial;
			diagnostics::mission_log(
				"camera mode=%u target=%u "
				"source=spectator-input tick=%u",
				static_cast<unsigned>(session.camera_mode),
				static_cast<unsigned>(session.camera.target),
				session.clock.gameplay_tick);
		}
	}
	else if (user_camera_request >= 0
		&& camera_runtime_request(
			session.camera,
			session.world,
			session.missiles,
			session.ship_stats,
			user_camera_request,
			session.world.player.index,
			false,
			false,
			steps,
			frame_tick))
	{
		session.camera_mode =
			static_cast<std::uint8_t>(session.camera.mode);
		++session.camera_cut_serial;
		diagnostics::mission_log(
			"camera mode=%u target=%u source=input tick=%u",
			static_cast<unsigned>(session.camera_mode),
			static_cast<unsigned>(session.camera.target),
			session.clock.gameplay_tick);
	}
	camera_runtime_publish_world_state(
		session.camera, session.world);

	hud::runtime_prepare_targeting_input(
		session.hud,
		session.world);

	const auto update_comms_input_state = [&]()
	{
		// Panel zero is the speech/movie instrument used by the queued
		// comms owner. CommsMenu_dispatch closes instrument eleven at
		// LANCER.EXE 0x00455f6e..0x00455f81; its state gates number-key
		// gameplay bindings independently.
		session.mission_runtime.player_comms.speech_hud_state =
			static_cast<std::uint8_t>(
			session.hud.panels[0].animation);
		input::gameplay_input_set_communications_state(
			input_poller,
			static_cast<std::uint8_t>(
				session.hud.panels[kCommsMenuPanel].animation));
	};
	const auto begin_requested_comms_chat = [&]()
	{
		std::int16_t destination = -1;
		if (mission::player_comms_take_chat_request(
				session.mission_runtime.player_comms,
				destination))
		{
			hud::runtime_begin_chat(session.hud, destination);
		}
	};
	const auto scan_comms_options = [&]()
	{
		const std::int16_t raw_count =
			session.mission_runtime.player_comms.option_count;
		const std::uint8_t count = static_cast<std::uint8_t>(
			std::clamp<std::int16_t>(raw_count, 0, 8));
		for (std::uint8_t option = 0; option < count; ++option)
		{
			if (!input::gameplay_input_raw_keyboard_pressed(
					input_poller,
					static_cast<SDL_Scancode>(
						SDL_SCANCODE_1 + option),
					0))
			{
				continue;
			}
			input.comms_option_pressed[option] = true;
			mission::player_comms_select_option(
				session.mission_runtime,
				session.world,
				session.ship_stats,
				session.pilot_stats,
				static_cast<std::uint8_t>(option + 1),
				frame_tick);
			if (!session.mission_runtime.player_comms.menu_open)
			{
				hud::runtime_close_panel(
					session.hud, kCommsMenuPanel);
			}
			begin_requested_comms_chat();
			update_comms_input_state();
			break;
		}
	};
	mission::PlayerCommsState& player_comms =
		session.mission_runtime.player_comms;
	if (player_comms.open_panel_requested)
	{
		player_comms.open_panel_requested = false;
		hud::runtime_open_panel(
			session.hud, kCommsMenuPanel);
		session.hud.panels[kCommsMenuPanel].hold = true;
	}
	begin_requested_comms_chat();
	update_comms_input_state();
	if (session.hud.panels[kCommsMenuPanel].animation
		== hud::PanelAnimation::open)
	{
		// CommsMenu_update is called before the chat gate and before
		// targeting actions when a menu was already fully open.
		scan_comms_options();
	}

	const bool chat_blocks_discrete =
		session.hud.chat_active || session.chat_submit_consumed;
	const bool deathmatch =
		mission::network_is_deathmatch_mission(
			session.mission_runtime.mission_number);
	if (!chat_blocks_discrete)
	{
		constexpr std::uint8_t targeting_order[] = {
			17, 15, 16, 18, 8, 9, 12, 13, 10, 11, 14, 45};
		for (const std::uint8_t action : targeting_order)
		{
			input.pressed[action] =
				input::gameplay_input_action_pressed(
					input_poller, action);
		}
		if (!deathmatch)
		{
			input.pressed[46] =
				input::gameplay_input_action_pressed(
					input_poller, 46);
			input.pressed[47] =
				input::gameplay_input_action_pressed(
					input_poller, 47);
		}
		hud::runtime_apply_targeting_input(
			session.hud,
			session.world,
			session.camera,
			drawable_width,
			drawable_height,
			input);

		for (std::uint8_t shortcut = 0; shortcut < 4; ++shortcut)
		{
			const std::uint8_t action =
				static_cast<std::uint8_t>(67 + shortcut);
			input.pressed[action] =
				input::gameplay_input_action_pressed(
					input_poller, action);
			if (!input.pressed[action] || deathmatch)
			{
				continue;
			}
			if (shortcut < 3)
			{
				const bool instant_action_mission_29 =
					session.request.mode == MissionMode::instant_action
					&& session.request.mission == 29;
				if (instant_action_mission_29
					|| session.mission_runtime.object_factory_mode != 0
					|| !mission::player_comms_shortcut_target_valid(
						session.world))
				{
					continue;
				}
			}
			mission::player_comms_execute_shortcut(
				session.mission_runtime,
				session.world,
				session.pilot_stats,
				shortcut,
				frame_tick);
		}

		const auto pressed_player_action =
			[&](std::uint8_t action)
			{
				input.pressed[action] =
					input::gameplay_input_action_pressed(
						input_poller, action);
				hud::runtime_apply_player_action(
					session.hud,
					session.world,
					session.mission_runtime,
					action,
					input.pressed[action],
					frame_tick);
			};
		pressed_player_action(43);

		WorldObject* player_control =
			world_resolve(session.world, session.world.player);
		const bool player_control_active =
			player_control != nullptr
			&& player_control->ai.command_count != 0
			&& player_control->ai.commands[0].id == 100;
		if (player_control_active)
		{
			input.pressed[48] =
				input::gameplay_input_action_pressed(
					input_poller, 48);
			if (input.pressed[48])
			{
				hud::runtime_enqueue_ui_sound(session.hud, 0);
				const hud::PanelAnimation state =
					session.hud.panels[
						kCommsMenuPanel].animation;
				if (state == hud::PanelAnimation::closed)
				{
					hud::runtime_open_panel(
						session.hud, kCommsMenuPanel);
					session.hud.panels[
						kCommsMenuPanel].hold = true;
					mission::player_comms_open_menu(
						session.mission_runtime,
						session.world,
						session.ship_stats);
					update_comms_input_state();
					scan_comms_options();
				}
				else if (state == hud::PanelAnimation::open)
				{
					hud::runtime_close_panel(
						session.hud, kCommsMenuPanel);
					mission::player_comms_close_menu(
						session.mission_runtime);
					update_comms_input_state();
				}
			}
		}

		pressed_player_action(56);
		pressed_player_action(57);
		pressed_player_action(40);
		pressed_player_action(41);
		pressed_player_action(42);
		pressed_player_action(65);
		pressed_player_action(58);
		pressed_player_action(59);
		pressed_player_action(39);
		pressed_player_action(55);

		input.held[61] =
			input::gameplay_input_action_held(input_poller, 61);
		input.shield_balance_interface = input.held[61];
		hud::runtime_apply_player_action(
			session.hud,
			session.world,
			session.mission_runtime,
			61,
			input.held[61],
			frame_tick);
		pressed_player_action(60);

		if (session.hud.panels[0].animation
			== hud::PanelAnimation::closed)
		{
			for (std::uint8_t action = 51; action <= 54; ++action)
			{
				input.held[action] =
					input::gameplay_input_action_held(
						input_poller, action);
				hud::runtime_apply_player_action(
					session.hud,
					session.world,
					session.mission_runtime,
					action,
					input.held[action],
					frame_tick);
			}
		}
		input.held[49] =
			input::gameplay_input_action_held(input_poller, 49);
		input.powerball_interface = input.held[49];
		hud::runtime_apply_player_action(
			session.hud,
			session.world,
			session.mission_runtime,
			49,
			input.held[49],
			frame_tick);
		const hud::PanelAnimation powerball_before =
			session.hud.panels[7].animation;
		pressed_player_action(50);
		if (input.pressed[50]
			&& powerball_before != hud::PanelAnimation::open
			&& powerball_before != hud::PanelAnimation::closing)
		{
			input.powerball_interface = true;
		}
		pressed_player_action(19);
		if (!deathmatch)
		{
			pressed_player_action(66);
		}
	}

	if (!chat_blocks_discrete)
	{
		input.pressed[72] =
			input::gameplay_input_action_pressed(input_poller, 72);
		if (input.pressed[72]
			&& session.mission_runtime.network.role
				!= mission::NetworkRole::offline)
		{
			hud::runtime_begin_chat(session.hud, -1);
		}
	}
	const auto eject_player = [&](WorldObject* player)
	{
		const bool admissible =
			player != nullptr
			&& !deathmatch
			&& (session.mission_runtime.gameplay_state == 0
				|| session.mission_runtime.gameplay_state == 8)
			&& player->type != 45
			&& !player->eject_disabled
			&& (player->runtime_flags & 0x00040000u) == 0
			&& player->ai.command_count != 0
			&& (player->ai.commands[0].id == 100
				|| player->ai.commands[0].id == 118);
		if (admissible)
		{
			player->runtime_flags &= ~0x00000800u;
			if ((player->runtime_flags & 0x00000100u) != 0)
			{
				world_force_decloak(
					session.world,
					*player,
					frame_tick);
			}
			if (ai::command_push(
					session.world,
					*player,
					30,
					ai::TargetKind::none,
					UINT16_MAX))
			{
				camera_runtime_request(
					session.camera,
					session.world,
					session.missiles,
					session.ship_stats,
					7,
					session.world.player.index,
					true,
					true,
					steps,
					frame_tick);
				session.camera_mode =
					static_cast<std::uint8_t>(
						session.camera.mode);
				++session.camera_cut_serial;
				mission::network_publish_player_ejected(
					session.mission_runtime.network,
					session.world.player.index);
				diagnostics::mission_log(
					"player manual eject accepted actor=%u tick=%u",
					static_cast<unsigned>(player->mission_index),
					session.clock.gameplay_tick);
			}
			return;
		}
		diagnostics::mission_log(
			"player manual eject rejected result=%u command=%d "
			"disabled=%u deathmatch=%u tick=%u",
			static_cast<unsigned>(
				session.mission_runtime.gameplay_state),
			player != nullptr && player->ai.command_count != 0
				? static_cast<int>(player->ai.commands[0].id)
				: -1,
			player != nullptr && player->eject_disabled ? 1u : 0u,
			deathmatch ? 1u : 0u,
			session.clock.gameplay_tick);
	};
	const auto deploy_countermeasure = [&](WorldObject* player)
	{
		if (!(deathmatch
				|| session.mission_runtime.gameplay_state == 0))
		{
			return;
		}
		if (player == nullptr || player->chaff_count <= 0)
		{
			// PlayerControl queues standard FAT sample 15 before the
			// countermeasure owner. Chaff_deploy then contributes HUD event
			// three when the local inventory is empty.
			hud::runtime_enqueue_sample(session.hud, 15);
			hud::runtime_enqueue_ui_sound(session.hud, 3);
			diagnostics::mission_log(
				"player chaff empty tick=%u",
				session.clock.gameplay_tick);
			return;
		}
		const std::int16_t before = player->chaff_count;
		if (before == 6 || before == 4 || before == 2)
		{
			// The low-counter warning is likewise a direct standard FAT
			// sample, not HUD event 13 (which would add the HUD-bank offset).
			hud::runtime_enqueue_sample(session.hud, 13);
		}
		if (chaff_deploy(
				session.chaff,
				session.missiles,
				session.world,
				session.missile_stats,
				session.pilot_stats,
				session.world.player,
				frame_tick,
				session.hud.distortion_random_seed))
		{
			hud::runtime_enqueue_ui_sound(session.hud, 0);
		}
	};
	bool relative_mouse_consumed = false;
	auto service_player_control_callback = [&]()
		-> WorldObject*
	{
		WorldObject* controlled =
			world_resolve(session.world, session.world.player);
		if (controlled == nullptr
			|| ai::command_owns_flight(*controlled)
			|| (controlled->runtime_flags & kObjectFlagSimulationSuspended) != 0)
		{
			return controlled;
		}
		input::GameplayInput callback_input = input;
		std::fill(
			std::begin(callback_input.held),
			std::end(callback_input.held),
			false);
		std::fill(
			std::begin(callback_input.pressed),
			std::end(callback_input.pressed),
			false);
		if (relative_mouse_consumed)
		{
			// DirectInput relative axes are destructive reads. A second
			// PlayerControl invocation in the same rendered update sees the
			// retained ±800 displacement but no new relative mouse packet.
			callback_input.mouse_relative_x = 0.0f;
			callback_input.mouse_relative_y = 0.0f;
		}
		else
		{
			relative_mouse_consumed = true;
		}
		const auto held = [&](std::uint8_t action)
		{
			const bool accepted =
				input::gameplay_input_action_held(
					input_poller, action);
			callback_input.held[action] = accepted;
			input.held[action] = accepted;
			return accepted;
		};
		const auto pressed = [&](std::uint8_t action)
		{
			const bool accepted =
				input::gameplay_input_action_pressed(
					input_poller, action);
			callback_input.pressed[action] = accepted;
			input.pressed[action] =
				input.pressed[action] || accepted;
			return accepted;
		};
		const auto poll_throttle = [&]()
		{
			if (!held(25))
			{
				held(26);
			}
			pressed(27);
			pressed(28);
		};

		const bool callback_chat_active =
			session.hud.chat_active || session.chat_submit_consumed;
		const bool interface_owns_rotation =
			input.powerball_interface
			|| input.shield_balance_interface;
		if (interface_owns_rotation)
		{
			if (config.controller == 2)
			{
				if (!held(33))
				{
					held(34);
				}
				if (!held(31))
				{
					held(32);
				}
			}
		}
		else
		{
			switch (config.controller)
			{
			case 0:
				if (!held(37) && !callback_chat_active)
				{
					if (!held(29))
					{
						held(30);
					}
				}
				if (!callback_input.joystick_axis_available[2]
					&& !callback_input.joystick_axis_available[6]
					&& !callback_chat_active)
				{
					poll_throttle();
				}
				break;
			case kControllerMouse:
			case kControllerModernMouse:
				if (!callback_chat_active)
				{
					if (!held(29))
					{
						held(30);
					}
					poll_throttle();
				}
				break;
			case 2:
			default:
				if (!callback_chat_active)
				{
					if (!held(33))
					{
						held(34);
					}
					if (!held(31))
					{
						held(32);
					}
					if (!held(29))
					{
						held(30);
					}
					poll_throttle();
				}
				break;
			}
		}
		held(35);
		held(36);

		const PlayerControlAxes interface_axes =
			update_player_flight_controls(
				session, config, callback_input, controlled);
		mission::deathmatch_scenarios_apply_player_controls(
			session.mission_runtime,
			session.world.player.index,
			session.flight_demand);
		if (input.shield_balance_interface)
		{
			adjust_shield_balance(
				*controlled,
				session.ship_stats,
				interface_axes.vertical);
		}
		else if (input.powerball_interface)
		{
			hud::runtime_update_power(
				*controlled,
				controlled->power_cursor_x
					- static_cast<float>(steps)
						* interface_axes.horizontal,
				controlled->power_cursor_y
					- static_cast<float>(steps)
						* interface_axes.vertical);
		}

		if (!callback_chat_active)
		{
			const bool fire_release_gated =
				input::gameplay_input_fire_release_gated(input_poller);
			const bool fire = !fire_release_gated && held(38);
			if (fire)
			{
				hud::runtime_open_panel(session.hud, 1);
				if ((controlled->runtime_flags & 0x00000100u) != 0
					&& !deathmatch)
				{
					world_set_cloak_active(
						session.world,
						*controlled,
						false,
						static_cast<std::uint32_t>(
							session.clock.gameplay_tick));
				}
				else
				{
					weapons_apply_gun_cooldown(
						session.world,
						*controlled,
						static_cast<std::uint32_t>(
							session.clock.gameplay_tick),
						1);
				}
			}
			else if (!fire_release_gated
				&& controlled->nova_charge != 0.0f)
			{
				weapons_fire_charged_nova(
					session.weapons,
					session.world,
					session.mission_runtime,
					*controlled,
					session.ship_stats,
					static_cast<std::uint32_t>(
						session.clock.gameplay_tick));
			}

			const auto activate_powerup_or_launch = [&]()
			{
				if (!mission::deathmatch_scenarios_activate_powerup(
						session.mission_runtime,
						session.world,
						session.ship_stats,
						session.world.player.index,
						frame_tick))
				{
					launch_selected_ordnance(session);
				}
			};
			if (!fire_release_gated && pressed(44))
			{
				activate_powerup_or_launch();
			}
			if (pressed(24))
			{
				toggle_match_speed(session, *controlled);
			}
			if (pressed(21))
			{
				session.afterburner_toggle =
					!session.afterburner_toggle;
			}
			if (pressed(64)
				&& !deathmatch
				&& session.previous_frame_camera_mode != 13)
			{
				hud::runtime_apply_equipment_action(
					session.hud,
					session.world,
					session.mission_runtime,
					*controlled,
					64,
					frame_tick);
			}
			if (pressed(23)
				&& session.mission_runtime.gameplay_state == 0)
			{
				mission::events_commit_player_jump_or_warp_requests(
					session.mission_runtime,
					session.script_tick);
			}
			if (pressed(63))
			{
				eject_player(controlled);
			}
			if (pressed(62))
			{
				deploy_countermeasure(controlled);
			}
		}
		held(20);
		held(22);
		session.flight_demand.afterburner =
			callback_input.held[20] || session.afterburner_toggle;
		session.flight_demand.reverse = callback_input.held[22];
		const auto warn_propulsion_fuel = [&]()
		{
			const std::int32_t fuel_percent =
				controlled->afterburner_fuel / 100;
			const std::uint32_t simulation_tick =
				static_cast<std::uint32_t>(
					session.clock.gameplay_tick);
			if (fuel_percent >= 20
				|| session.propulsion_warning_deadline
					>= simulation_tick)
			{
				return;
			}
			hud::runtime_enqueue_sample(
				session.hud, fuel_percent == 0 ? 14 : 12);
			session.propulsion_warning_deadline =
				simulation_tick + 1000;
		};
		if (session.flight_demand.afterburner)
		{
			warn_propulsion_fuel();
		}
		if (session.flight_demand.reverse)
		{
			warn_propulsion_fuel();
		}
		if (!controlled->match_speed_active
			&& session.match_speed_latched)
		{
			session.match_speed_latched = false;
		}
		update_match_speed(session, *controlled);
		return controlled;
	};
	WorldObject* player =
		world_resolve(session.world, session.world.player);
	bool service_script_clock = false;
	for (std::uint32_t step = 0; step < steps; ++step)
	{
		const bool frame_dispatch = step + 1 == steps;
		// GameObjects_fixed_tick increments the retail 100 Hz counter before
		// invoking fixed callbacks; every deadline serviced below observes
		// the newly admitted tick.
		++session.clock.gameplay_tick;
		shields_service(
			session.world,
			static_cast<std::uint32_t>(session.clock.gameplay_tick),
			session.camera.position,
			session.camera.orientation[2],
			session.camera.mode);
		if (frame_dispatch)
		{
			transition_effects_service_fixed_gates(
				session.world,
				static_cast<std::uint32_t>(session.clock.gameplay_tick),
				steps);
		}
		if ((session.clock.gameplay_tick % kServiceTickInterval) == 0)
		{
			ModelAnimationEventContext animation_context{
				&session.weapons,
				&session.gun_stats,
				&session.ship_stats,
			};
			model_animation_service(
				session.world,
				static_cast<std::uint32_t>(session.clock.gameplay_tick),
				{
					&animation_context,
					fire_model_animation_type0,
					nullptr,
				});
			world_service_resources(
				session.world,
				session.ship_stats,
				session.mission_runtime);

			// GameObjects_service_phase 0x004774d0 invokes the local
			// PlayerControl command immediately before the ordinary object
			// integrator. Keep the gun service after this callback so its
			// one-tick firing request is consumed in the same service phase.
			// AI_update_all_objects invokes the same command again in the
			// rendered-frame owner; both invocations are intentional.
			player = service_player_control_callback();
			weapons_service_simple_guns(
				session.weapons,
				session.world,
				session.mission_runtime,
				session.gun_stats,
				session.ship_stats,
				static_cast<std::uint32_t>(session.clock.gameplay_tick));
			if (player != nullptr
				&& !ai::command_owns_flight(*player)
				&& (player->runtime_flags & kObjectFlagSimulationSuspended) == 0)
			{
				player->control_demand = session.flight_demand;
				player->ordinary_motion_enabled = true;
			}
			world_service_ordinary_motion(
				session.world,
				session.mission_runtime,
				session.mission_file,
				session.ship_stats,
				static_cast<std::uint32_t>(
					session.clock.gameplay_tick));
			world_service_collisions(
				session.world,
				session.mission_runtime,
				session.ship_stats,
				session.weapons.feedback_enabled,
				static_cast<std::uint32_t>(
					session.clock.gameplay_tick));
			// Projectile displacement and collision share the 25 Hz physics
			// owner. Rendering only interpolates the two snapshots published
			// here, so hit detection is independent of render submission rate.
			missiles_integrate(
				session.missiles,
				session.missile_stats);
			weapons_service_projectile_physics(
				session.weapons,
				session.world,
				session.mission_runtime,
				session.gun_stats,
				session.ship_stats,
				static_cast<std::uint32_t>(
					session.clock.gameplay_tick));
		}
		player = world_resolve(session.world, session.world.player);
		if (player != nullptr)
		{
			// MatchSpeed_command can clear the retail global from the
			// executor without passing through the input helper. Mirror
			// that transition into the session's saved-demand ownership so
			// a later scripted enable captures the then-current throttle.
			if (!player->match_speed_active
				&& session.match_speed_latched)
			{
				session.match_speed_latched = false;
			}
		}
		camera_runtime_publish_world_state(
			session.camera, session.world);
		session.mission_runtime.particle_camera_position =
			session.mission_runtime.director.mode == 13
				? session.mission_runtime.director.camera_position
				: session.camera.position;
		session.mission_runtime.particle_camera_forward =
			session.mission_runtime.director.mode == 13
				? session.mission_runtime.director
					.camera_orientation[2]
				: session.camera.orientation[2];
		if (frame_dispatch)
		{
			disruption_effects_service(
				session.world,
				session.mission_runtime,
				session.ship_stats,
				session.weapons.feedback_enabled,
				static_cast<std::uint32_t>(
					session.clock.gameplay_tick),
				steps);
			exhaust_hazard_service(
				session.world,
				session.mission_runtime,
				session.ship_stats,
				session.weapons.feedback_enabled,
				static_cast<std::uint32_t>(
					session.clock.gameplay_tick));
			disruption_effects_service_explosion_controllers(
				session.world,
				session.mission_runtime,
				session.ship_stats,
				static_cast<std::uint32_t>(
					session.clock.gameplay_tick));
			// FUN_004924b0 services the shared explosion owner immediately
			// after ExhaustHazard_update_player_damage and before Chaff_update
			// (LANCER.EXE 0x0049332a..0x00493334).
			explosion_billboards_service(
				session.world.death_effects,
				session.world,
				static_cast<std::uint32_t>(
					session.clock.gameplay_tick),
				steps);
		}
		if (frame_dispatch)
		{
			for (std::uint16_t event_index = 0;
				event_index < session.world.mission_event_count;
				++event_index)
			{
				const WorldMissionEvent& event =
					session.world.mission_events[event_index];
				if (event.type == WorldMissionEventType::shot_at)
				{
					mission::events_emit_shot_at(
						session.mission_runtime,
						session.world,
						session.ship_stats,
						event.source_mission_index,
						event.attacker_mission_index,
						event.selector);
				}
				else
				{
					mission::events_emit_direct(
						session.mission_runtime,
						event.type == WorldMissionEventType::cloaked
							? mission::EventType::cloaked
							: mission::EventType::decloaked,
						event.source_mission_index,
						nullptr,
						0);
				}
			}
			session.world.mission_event_count = 0;
			hud::runtime_consume_player_hit_triggers(
				session.hud,
				session.world,
				static_cast<std::uint32_t>(session.clock.gameplay_tick));
		}
		mission::network_fixed_tick(
			session.mission_runtime.network,
			static_cast<std::uint32_t>(session.clock.gameplay_tick));
		const bool fixed_script_clock =
			session.clock.gameplay_tick % kSimulationHz == 0;
		if (fixed_script_clock)
		{
			// GameObjects_fixed_tick 0x00477889 decrements the signed
			// mission-session countdown at +0x84 unconditionally on every
			// hundredth enabled fixed tick. The registered 1 Hz callback
			// advances the script clock and requests timer/proximity service.
			--session.mission_runtime.session_state[33];
			++session.script_tick;
			service_script_clock = true;
		}
		session.clock.service_phase = static_cast<std::uint8_t>(
			session.clock.gameplay_tick % kServiceTickInterval);
	}
	const bool scripts_stalled =
		mission::network_script_execution_stalled(
			session.mission_runtime.network);
	mission::deathmatch_scenarios_update(
		session.mission_runtime,
		session.world,
		session.ship_stats,
		static_cast<std::uint32_t>(
			session.clock.gameplay_tick));
	// MissionRuntime_tick resumes ordinary/yielded contexts exactly once per
	// admitted mission frame, independently of both fixed-clock catch-up and
	// the 1 Hz timer request.
	if (steps != 0 && !scripts_stalled)
	{
		mission::executor_tick(
			session.executor,
			session.mission_runtime,
			session.world,
			session.ship_stats,
			session.mission_file,
			session.script_tick,
			static_cast<std::uint32_t>(session.clock.gameplay_tick));
		if (session.executor.failed)
		{
			mission_session_fail_load(
				session,
				"mission executor entered invalid bytecode");
		}
		else
		{
			if (service_script_clock)
			{
				mission::executor_service_timers(
					session.executor,
					session.mission_file,
					session.script_tick);
				mission::events_service_proximity(
					session.mission_runtime, session.world);
			}
			if (!mission::events_flush(
					session.mission_runtime,
					session.executor,
					session.world,
					session.ship_stats,
					session.mission_file,
					session.script_tick,
					static_cast<std::uint32_t>(
						session.clock.gameplay_tick)))
			{
				mission_session_fail_load(
					session, "mission event delivery failed");
			}
		}
	}
	// Player_comms_frame_update is a mission-frame callback in the retail
	// owner, after fixed-clock catch-up and ordinary script dispatch. Running
	// it here also ensures the platform consumes one yielded script request
	// before that context can resume on a later frame.
	if (steps != 0)
	{
		mission::player_comms_service(
			session.mission_runtime,
			session.world,
			session.ship_stats,
			static_cast<std::uint32_t>(session.clock.gameplay_tick),
			session.script_tick,
			static_cast<std::uint32_t>(session.clock.gameplay_tick));
		// AI_update_all_objects services PlayerControl here, after
		// mission execution and comms but before ordinary object AI.
		// It prepares demand/actions for the next 25 Hz integration and
		// never advances the pose itself.
		service_player_control_callback();
		if (WorldObject* current_player =
				world_resolve(session.world, session.world.player))
		{
			// Chaff_update is one delta-driven mission-frame callback. Run
			// it after PlayerControl so a countermeasure deployed by that
			// callback participates in the same retail frame.
			chaff_step(
				session.chaff,
				session.missiles,
				session.world,
				frame_tick,
				steps,
				current_player->position,
				current_player->orientation,
				session.hud.distortion_random_seed);
		}
		// The retail frame owner calls mission execution and comms before
		// object AI. Command state and its ordinary flight callback both
		// advance once for the complete nonzero-delta rendered update.
		ai::runtime_tick(
			session.world,
			session.mission_runtime,
			session.mission_file,
			session.ship_stats,
			session.pilot_stats,
			session.gun_stats,
			session.missile_stats,
			session.weapons,
			session.missiles,
			session.chaff,
			session.flight_demand,
			session.hud.distortion_random_seed,
			static_cast<std::uint32_t>(session.clock.gameplay_tick));
		// Object_model_update_scene is the retail component-death boundary.
		// It follows mission dispatch, comms, and AI_update_all_objects, so
		// deaths produced earlier in the frame are not published or removed
		// until the victim has received that final AI service.
		disruption_effects_service_component_destruction_callbacks(
			session.world,
			session.mission_runtime,
			session.ship_stats,
			static_cast<std::uint32_t>(
				session.clock.gameplay_tick));
		world_service_component_destruction(
			session.world,
			session.mission_runtime,
			session.ship_stats,
			static_cast<std::uint32_t>(
				session.clock.gameplay_tick));
		// UberExplode_update and articulated gun mounts follow object AI
		// in LANCER.EXE 0x004924b0. Both are one delta-aware
		// rendered-frame callback.
		uber_explosion_service(
			session.world,
			session.mission_runtime,
			static_cast<std::uint32_t>(session.clock.gameplay_tick));
		weapons_service_articulated_guns(
			session.weapons,
			session.world,
			session.missiles,
			session.gun_stats,
			session.missile_stats,
			session.ship_stats,
			static_cast<std::uint32_t>(session.clock.gameplay_tick),
			steps,
			session.mission_runtime.network.role
				!= mission::NetworkRole::offline,
			session.mission_runtime.mission_number);
	}
	// Objects_service_render calls Object_model_update_scene after
	// ordinary AI and before Camera_update_frame in LANCER.EXE
	// 0x004924b0. Publish the same root transform once so models,
	// attachments, and every object-relative camera share one basis.
	world_publish_scene_poses(
		session.world, session.clock.service_phase);
	// Camera_update_frame owns mode 13 after Objects_service_render has
	// published every retained scene transform (LANCER.EXE
	// 0x004924b0 -> 0x0045fc90 -> 0x00450fa0). Director must therefore
	// sample the published scene nodes here, after mission scripts, AI,
	// object motion, and attachment composition. A StartDirectorCam command
	// has already received its separate immediate mode-installation update;
	// retail performs this ordinary rendered-frame update as well.
	mission::director_service(
		session.mission_runtime,
		session.world,
		session.mission_file,
		steps);
	session.mission_runtime.particle_camera_position =
		session.mission_runtime.director.mode == 13
			? session.mission_runtime.director.camera_position
			: session.camera.position;
	session.mission_runtime.particle_camera_forward =
		session.mission_runtime.director.mode == 13
			? session.mission_runtime.director.camera_orientation[2]
			: session.camera.orientation[2];
	missiles_publish_scene_poses(
		session.missiles,
		static_cast<float>(session.clock.service_phase)
			/ static_cast<float>(kServiceTickInterval));
	if (steps != 0)
	{
		// Missile_system_update (0x004960f0) is the immediately preceding
		// once-per-frame behavior/collision owner at 0x00492b9c.
		missiles_step(
			session.missiles,
			session.chaff,
			session.world,
			session.weapons,
			session.mission_runtime,
			session.missile_stats,
			session.ship_stats,
			static_cast<std::uint32_t>(
				session.clock.gameplay_tick),
			session.hud.distortion_random_seed,
			session.hud.missile_lock.phase
				== hud::MissileLockPhase::locked);
		// Retain the visual half of gun_projectiles_update_and_render here.
		// Collision has already consumed every admitted physics interval.
		weapons_step_projectiles(
			session.weapons,
			session.world,
			session.mission_runtime,
			static_cast<std::uint32_t>(
				session.clock.gameplay_tick),
			session.clock.service_phase);
		// Cloak_update runs in the rendered-object owner at LANCER.EXE
		// 0x00492d84, after AI and Object_model_update_scene. It consumes the
		// complete 100 Hz tick delta once, not each fixed catch-up step.
		world_service_cloaks(
			session.world,
			static_cast<std::uint32_t>(session.clock.gameplay_tick));
		particle_system_service(
			session.world,
			session.ship_stats,
			static_cast<std::uint32_t>(session.clock.gameplay_tick),
			steps,
			session.mission_runtime.particle_camera_position,
			session.mission_runtime.particle_camera_forward);
		// WGate retains its emitters outside Explosion_system's automatic
		// owner, but they still run after Particle_update_all. Running them
		// first made the generic emitter pre-advance new particles and the
		// common array traversal advance the same particles a second time.
		transition_effects_service_particles(
			session.world,
			static_cast<std::uint32_t>(session.clock.gameplay_tick),
			steps,
			session.mission_runtime.particle_camera_position,
			session.mission_runtime.particle_camera_forward);
	}
	mission::runtime_update_flyback(
		session.mission_runtime,
		session.world);
	if (session.mission_director_serial
		!= session.mission_runtime.director.serial)
	{
		session.mission_director_serial =
			session.mission_runtime.director.serial;
		if (session.mission_runtime.director.mode == 13)
		{
			if (camera_runtime_request(
					session.camera,
					session.world,
					session.missiles,
					session.ship_stats,
					13,
					session.world.player.index,
					true,
					true,
					steps,
					static_cast<std::uint32_t>(
						session.clock.gameplay_tick)))
			{
				session.camera_mode = 13;
				++session.camera_cut_serial;
				diagnostics::mission_log(
					"camera mode=13 source=director tick=%u",
					session.clock.gameplay_tick);
			}
		}
	}
	if (session.mission_camera_request_serial
		!= session.mission_runtime.camera_request_serial)
	{
		session.mission_camera_request_serial =
			session.mission_runtime.camera_request_serial;
		const bool superseded_by_director =
			session.mission_runtime.director.mode == 13
			&& session.mission_runtime.director.transition_order
				> session.mission_runtime.camera_request_order;
		if (superseded_by_director)
		{
			diagnostics::mission_log(
				"camera request superseded mode=%u source=director "
				"tick=%u",
				static_cast<unsigned>(
					session.mission_runtime.requested_camera_mode),
				session.clock.gameplay_tick);
		}
		else if (camera_runtime_request(
				session.camera,
				session.world,
				session.missiles,
				session.ship_stats,
				session.mission_runtime.requested_camera_mode,
				session.mission_runtime.requested_camera_target,
				session.mission_runtime.requested_camera_lock,
				session.mission_runtime
					.requested_camera_override_lock,
				steps,
				static_cast<std::uint32_t>(
					session.clock.gameplay_tick),
				session.mission_runtime
					.requested_camera_accept_departed))
		{
			if (session.mission_runtime.director.mode == 13)
			{
				// Any accepted non-Director camera installation leaves mode
				// 13 through Camera_switch_mode, which also clears the
				// queued head's recursive suspension scope.
				mission::director_camera_replaced(
					session.mission_runtime,
					session.world);
			}
			session.camera_mode =
				static_cast<std::uint8_t>(session.camera.mode);
			++session.camera_cut_serial;
			diagnostics::mission_log(
				"camera mode=%u target=%u source=mission tick=%u",
				static_cast<unsigned>(session.camera_mode),
				static_cast<unsigned>(session.camera.target),
				session.clock.gameplay_tick);
		}
	}
	const std::uint32_t camera_now =
		static_cast<std::uint32_t>(session.clock.gameplay_tick);
	const auto camera_elapsed_exceeds =
		[&session, camera_now](std::uint32_t delay)
		{
			return static_cast<std::int32_t>(camera_now)
				> static_cast<std::int32_t>(
					session.camera.installed_tick + delay);
		};
	bool terminal_camera_has_timeout = false;
	bool terminal_camera_expired = false;
	switch (session.camera.mode)
	{
	case 8:
	case 26:
	case 27:
		// Objects_service_render's compressed mode table maps all three
		// ordinary death cameras to the 600-tick owner.
		terminal_camera_has_timeout = true;
		terminal_camera_expired = camera_elapsed_exceeds(600);
		break;
	case 28:
		terminal_camera_has_timeout = true;
		terminal_camera_expired = camera_elapsed_exceeds(1200);
		break;
	case 29:
	{
		terminal_camera_has_timeout = true;
		// The hostile ejection camera does not start its 500-tick tail
		// until the local cockpit receives runtime bit 0x40. Before that,
		// retail rewrites the installed-camera tick on every frame.
		const std::uint16_t player_index =
			session.world.player.index;
		const bool player_destroyed =
			player_index < std::size(session.world.objects)
			&& (session.world.objects[player_index].runtime_flags
					& kObjectFlagDestroyed) != 0;
		if (player_destroyed)
		{
			terminal_camera_expired = camera_elapsed_exceeds(500);
		}
		else
		{
			session.camera.installed_tick = camera_now;
		}
		break;
	}
	default:
		break;
	}
	if (terminal_camera_expired
		&& !session.mission_runtime.network.deathmatch_mode)
	{
		// FUN_004924b0 0x004926be..0x004926ec owns this transition
		// independently of DAT_0052a414: offline sets the terminal latch;
		// multiplayer clears cinematic state, activates spectator mode,
		// and invokes Spectator_select_next directly.
		if (!session.camera.multiplayer)
		{
			session.mission_runtime.player_death_transition_complete =
				true;
		}
		else
		{
			session.world.cinematic_mode = 0;
			session.camera.spectator_active = true;
			const bool switched =
				camera_runtime_select_next_spectator(
					session.camera,
					session.world,
					session.missiles,
					session.ship_stats,
					steps,
					camera_now);
			if (session.camera.spectator_complete)
			{
				session.mission_runtime
					.player_death_transition_complete = true;
			}
			if (switched)
			{
				session.camera_mode =
					static_cast<std::uint8_t>(
						session.camera.mode);
				++session.camera_cut_serial;
				diagnostics::mission_log(
					"camera mode=%u target=%u "
					"source=spectator tick=%u",
					static_cast<unsigned>(
						session.camera_mode),
					static_cast<unsigned>(
						session.camera.target),
					session.clock.gameplay_tick);
			}
		}
	}
	if (session.mission_match_speed_request_serial
		!= session.mission_runtime.match_speed_request_serial)
	{
		session.mission_match_speed_request_serial =
			session.mission_runtime.match_speed_request_serial;
		if (WorldObject* player =
			world_resolve(session.world, session.world.player))
		{
			const bool enabled =
				session.mission_runtime.requested_match_speed;
			if (enabled)
			{
				// Set Match Speed calls Player_update_match_speed before
				// publishing the enabled latch.
				update_match_speed(session, *player);
				player->match_speed_active = true;
			}
			else
			{
				// The scripted clear writes the latch only; it does not
				// restore the pre-match throttle used by the manual toggle.
				player->match_speed_active = false;
				session.match_speed_latched = false;
			}
			diagnostics::mission_log(
				"player match-speed active=%u source=mission "
				"throttle=%.3f",
				enabled ? 1u : 0u,
				session.flight_demand.throttle);
		}
	}
	if (session.mission_player_motion_clear_serial
		!= session.mission_runtime.player_motion_clear_serial)
	{
		session.mission_player_motion_clear_serial =
			session.mission_runtime.player_motion_clear_serial;
		session.flight_demand = {};
		session.afterburner_toggle = false;
		diagnostics::mission_log(
			"player motion controls cleared source=mission tick=%u",
			session.clock.gameplay_tick);
	}
	if (session.weapons.camera_recoil != 0.0f)
	{
		session.camera.recoil = session.weapons.camera_recoil;
		session.weapons.camera_recoil = 0.0f;
	}
	const std::int16_t before = session.camera.mode;
	camera_runtime_service(
		session.camera,
		session.world,
		session.missiles,
		session.ship_stats,
		steps,
		static_cast<std::uint32_t>(session.clock.gameplay_tick));
	if (session.mission_runtime.director.mode == 13)
	{
		session.camera.previous_position = session.camera.position;
		session.camera.previous_orientation = session.camera.orientation;
		session.camera.position =
			session.mission_runtime.director.camera_position;
		session.camera.orientation =
			session.mission_runtime.director.camera_orientation;
	}
	session.camera_mode =
		static_cast<std::uint8_t>(session.camera.mode);
	camera_runtime_publish_world_state(
		session.camera, session.world);
	if (session.camera.mode != before)
	{
		++session.camera_cut_serial;
		diagnostics::mission_log(
			"camera mode=%u target=%u source=fallback tick=%u",
			static_cast<unsigned>(session.camera_mode),
			static_cast<unsigned>(session.camera.target),
			session.clock.gameplay_tick);
	}
	session.mission_runtime.active_camera_mode =
		session.mission_runtime.director.mode == 13
			? std::uint8_t{13}
			: static_cast<std::uint8_t>(session.camera.mode);
	mission::network_service_player_target_reference(
		session.mission_runtime.network,
		session.world);
	mission::network_service_object_states(
		session.mission_runtime,
		session.world,
		session.ship_stats,
		static_cast<std::uint32_t>(
			session.clock.gameplay_tick));
	for (std::uint8_t index = 0;
		index < std::size(session.mission_instrument_serials);
		++index)
	{
		const mission::InstrumentState& instrument =
			session.mission_runtime.instruments[index];
		if (session.mission_instrument_serials[index]
			== instrument.serial)
		{
			continue;
		}
		session.mission_instrument_serials[index] =
			instrument.serial;
		if (instrument.open)
		{
			hud::runtime_open_panel(session.hud, index);
		}
		else
		{
			hud::runtime_close_panel(session.hud, index);
		}
	}
	const bool non_deathmatch_multiplayer =
		session.camera.multiplayer
		&& !session.mission_runtime.network.deathmatch_mode;
	// FUN_004924b0's camera jump table sends mode seven past terminal
	// handling, gives 8/26/27/28/29 to their timeout owners, and sends
	// every other camera mode through the default multiplayer scan.
	const bool default_multiplayer_terminal_path =
		session.camera.mode != 7
		&& session.camera.mode != 8
		&& session.camera.mode != 26
		&& session.camera.mode != 27
		&& session.camera.mode != 28
		&& session.camera.mode != 29;
	bool local_command_defers_multiplayer_terminal = false;
	const std::uint16_t local_player_index =
		session.world.player.index;
	if (local_player_index < std::size(session.world.objects))
	{
		const WorldObject& local_player =
			session.world.objects[local_player_index];
		if (local_player.ai.command_count != 0)
		{
			// The retail default branch defers its player-prefix scan for
			// Friendly Fire, Jump Out, Land, Eject Player, and an Explode
			// command whose begin callback is still pending
			// (0x0049271b..0x00492770).
			const std::int16_t command =
				local_player.ai.commands[0].id;
			local_command_defers_multiplayer_terminal =
				command == 117
				|| command == 20
				|| command == 8
				|| command == 118
				|| (command == 11
					&& local_player.ai.work.begin_pending);
		}
	}
	const bool default_multiplayer_terminal_scan_admitted =
		default_multiplayer_terminal_path
		&& !local_command_defers_multiplayer_terminal;
	bool every_player_slot_dead = false;
	if (non_deathmatch_multiplayer)
	{
		every_player_slot_dead = true;
		const std::uint16_t player_count =
			std::min<std::uint16_t>(
				session.mission_runtime.player_prefix_count,
				static_cast<std::uint16_t>(
					std::size(session.world.objects)));
		for (std::uint16_t player_index = 0;
			player_index < player_count;
			++player_index)
		{
			// Retail scans every pointer in DAT_0058832c's contiguous
			// player prefix and tests exactly mask 0x10000840. It does
			// not substitute the current camera mode or allocation state.
			if ((session.world.objects[player_index].runtime_flags
					& 0x10000840u) == 0)
			{
				every_player_slot_dead = false;
				break;
			}
		}
		if (session.camera.spectator_complete
			|| (default_multiplayer_terminal_scan_admitted
				&& every_player_slot_dead))
		{
			// Spectator_select_next's exhausted branch and the default
			// player-prefix scan both publish DAT_0052a414.
			session.mission_runtime.player_death_transition_complete =
				true;
		}
	}
	const bool player_death_terminal_admitted =
		// A command-owned latch can become visible while an offline death
		// animation is still running. FUN_004924b0 keeps its timed camera
		// branch authoritative until the strict expiry comparison passes.
		(session.camera.multiplayer
			|| !terminal_camera_has_timeout
			|| terminal_camera_expired)
		// AI_ExplodeOrdinary_begin switches the death camera synchronously at
		// LANCER.EXE 0x004089c0..0x00408a3d before it publishes result state
		// one. The runtime/session bridge defers that switch until the next
		// admitted frame, so do not accept the destruction latch while its
		// camera request is still pending.
		&& session.mission_camera_request_serial
			== session.mission_runtime.camera_request_serial
		&& (!non_deathmatch_multiplayer
			|| session.camera.spectator_complete
			|| (default_multiplayer_terminal_scan_admitted
				&& every_player_slot_dead));
	if (session.mission_runtime.gameplay_state == 9)
	{
		// A state-nine packet published during this admitted frame is owned
		// by the next FUN_004924b0 entry. Do not let another terminal latch
		// reinterpret it before that front-of-frame abort path runs.
	}
	else if (session.mission_runtime.player_death_transition_complete
		&& player_death_terminal_admitted
		&& session.result.kind == SessionResultKind::none)
	{
		// FUN_004924b0 returns the DAT_0052a414 transition without
		// rewriting DAT_00588394. The mission-below-28 override in
		// the outer gameplay owner is gated by the independent
		// Terminate Mission counter and therefore belongs only to the
		// terminate_requested branch below. Rescue/capture/destroyed
		// ejection outcomes must retain their published 2/3/1 value.
		const std::uint8_t coordinator =
			session.mission_runtime.gameplay_state;
		const SessionResultKind kind =
			coordinator == 1
				? SessionResultKind::player_destroyed
				: coordinator == 3
						|| coordinator == 6
						|| coordinator == 7
					? SessionResultKind::mission_failed
					: SessionResultKind::mission_complete;
		capture_terminal_result(session, kind, coordinator);
		diagnostics::mission_log(
			"player death transition accepted result=%u tick=%u",
			static_cast<unsigned>(coordinator),
			session.clock.gameplay_tick);
	}
	else if (session.mission_runtime.landing_transition_complete
		&& session.result.kind == SessionResultKind::none)
	{
		// Both retail landing state machines hand off through the common
		// mission-transition latch. Result states six and seven are the two
		// Friendly Fire consequence paths; every ordinary landing is the
		// successful mission handoff.
		const std::uint8_t coordinator =
			session.mission_runtime.gameplay_state;
		const SessionResultKind kind =
			coordinator == 6 || coordinator == 7
			? SessionResultKind::mission_failed
			: SessionResultKind::mission_complete;
		capture_terminal_result(session, kind, coordinator);
		diagnostics::mission_log(
			"landing transition accepted result=%u tick=%u",
			static_cast<unsigned>(coordinator),
			session.clock.gameplay_tick);
	}
	else if (session.mission_runtime.terminate_requested
		&& session.result.kind == SessionResultKind::none)
	{
		const std::uint8_t coordinator =
			session.request.mission < 28
				? std::uint8_t{1}
				: session.mission_runtime.gameplay_state;
		capture_terminal_result(
			session,
			coordinator == 1
				? SessionResultKind::player_destroyed
				: coordinator == 3
						|| coordinator == 6
						|| coordinator == 7
					? SessionResultKind::mission_failed
					: SessionResultKind::mission_complete,
			coordinator);
		diagnostics::mission_log(
			"mission termination accepted requests=%u result=%u tick=%u",
			session.mission_runtime.terminate_request_count,
			static_cast<unsigned>(coordinator),
			session.clock.gameplay_tick);
	}
	if (session.mission_player_ordnance_rebuild_serial
		!= session.mission_runtime.player_ordnance_rebuild_serial)
	{
		session.mission_player_ordnance_rebuild_serial =
			session.mission_runtime.player_ordnance_rebuild_serial;
		// AI_Dock_mode1_update calls HUD_rebuild_ordnance_summary after
		// reconstructing the local player's hardpoint loadout.
		session.hud.ordnance_initialized = false;
	}
	while (session.mission_runtime.sound_2d_count != 0)
	{
		const std::uint8_t sample =
			session.mission_runtime.sound_2d_events[
				session.mission_runtime.sound_2d_read];
		session.mission_runtime.sound_2d_read =
			static_cast<std::uint8_t>(
				(session.mission_runtime.sound_2d_read + 1)
				% std::size(
					session.mission_runtime.sound_2d_events));
		--session.mission_runtime.sound_2d_count;
		hud::runtime_enqueue_sample(session.hud, sample);
	}
	drain_multiplayer_hud_messages(session, language);
	mission::deathmatch_scenarios_flush_messages(
		session.mission_runtime,
		session.hud,
		language,
		static_cast<std::uint32_t>(
			session.clock.gameplay_tick));
	hud::runtime_update(
		session.hud,
		session.world,
		session.mission_runtime,
		session.missile_stats,
		missiles_local_shot_has_target(
			session.missiles, session.world.player),
		steps,
		session.clock.gameplay_tick,
		drawable_width,
		drawable_height);
	if (input.held[38] != session.previous_actions[38])
	{
		diagnostics::mission_log(
			"player lasers active=%u energy=%.1f live_projectiles=%u",
			input.held[38] ? 1u : 0u,
			player != nullptr ? player->gun_energy : 0.0f,
			session.weapons.live_projectiles);
	}
	std::memcpy(
		session.previous_actions,
		input.held,
		sizeof(session.previous_actions));
	session.previous_frame_camera_mode =
		static_cast<std::uint8_t>(session.camera.mode);
	session.chat_submit_consumed = false;
}

bool mission_session_receive_network_message(
	MissionSession& session,
	const mission::NetworkOutboundMessage& message)
{
	mission::Runtime& runtime = session.mission_runtime;
	mission::NetworkRuntime& network = runtime.network;
	if ((session.state != MissionSessionState::running
			&& session.state != MissionSessionState::paused)
		|| network.role == mission::NetworkRole::offline
		|| message.source_player >= network.player_count
		|| !network.connected[message.source_player]
		// DirectPlay's player/group sends are not delivered back through
		// the gameplay receiver owned by the sending player. Local effects
		// are applied by their publishing owner before the packet is queued.
		|| message.source_player == network.local_player)
	{
		return false;
	}
	switch (message.delivery)
	{
	case mission::NetworkDelivery::broadcast_conditional:
	case mission::NetworkDelivery::broadcast_guaranteed:
		if (message.destination_player != UINT8_MAX)
		{
			return false;
		}
		break;
	case mission::NetworkDelivery::directed_guaranteed:
	case mission::NetworkDelivery::directed_conditional:
		if (message.destination_player != network.local_player)
		{
			return false;
		}
		break;
	default:
		return false;
	}

	const std::uint32_t simulation_tick =
		static_cast<std::uint32_t>(session.clock.gameplay_tick);
	switch (message.kind)
	{
	case mission::NetworkOutboundKind::object_state:
		return mission::network_receive_object_state(
			runtime,
			session.world,
			session.ship_stats,
			message);
	case mission::NetworkOutboundKind::ai_sequence_sync:
		return mission::network_receive_ai_sequence_sync(
			network,
			session.world,
			message.object_index,
			message.sync_index,
			message.source_player);
	case mission::NetworkOutboundKind::ai_deferred_command:
		return mission::network_receive_ai_deferred_command(
			session.world, message, simulation_tick);
	case mission::NetworkOutboundKind::chat:
		if (std::memchr(
				message.chat_text,
				'\0',
				sizeof(message.chat_text)) == nullptr)
		{
			return false;
		}
		return mission::network_receive_chat(
			network,
			message.source_player,
			message.chat_text);
	case mission::NetworkOutboundKind::gameplay:
		break;
	default:
		return false;
	}

	switch (message.opcode)
	{
	case mission::NetworkGameplayOpcode::landing:
	{
		if (message.delivery
				!= mission::NetworkDelivery::broadcast_guaranteed)
		{
			return false;
		}
		const WorldObject* player = world_resolve(
			session.world, session.world.player);
		if (player == nullptr
			|| (player->runtime_flags & 0x10000840u) != 0)
		{
			// DPGMESSAGE_LANDING publishes DAT_0052a414 immediately when
			// the local player cannot enter the forced landing path.
			runtime.player_death_transition_complete = true;
			return true;
		}
		mission::player_comms_receive_network_landing(
			runtime,
			session.world,
			session.clock.gameplay_tick);
		return true;
	}
	case mission::NetworkGameplayOpcode::pause_state:
	{
		const std::uint8_t raw_reason =
			static_cast<std::uint8_t>(message.pause_reason);
		if (message.delivery
				!= mission::NetworkDelivery::broadcast_guaranteed
			|| raw_reason >= 16u)
		{
			return false;
		}

		// FUN_004b6f80, LANCER.EXE 0x004b8a1d..0x004b8aae. The packet has
		// no owner field and imposes no resume authorization: the validated
		// DirectPlay source slot is the pause owner, while any peer may resume.
		if (!message.pause_active)
		{
			session.network_pause_transition_pending = true;
			session.network_pause_transition_active = false;
			diagnostics::mission_log(
				"network rx opcode=0x27 pause=0 reason=%u player=%u",
				static_cast<unsigned>(raw_reason),
				static_cast<unsigned>(message.source_player));
			return true;
		}

		const bool already_paused =
			session.network_pause_transition_pending
				? session.network_pause_transition_active
				: session.state == MissionSessionState::paused;
		if (!already_paused)
		{
			session.network_pause_owner = message.source_player;
			session.network_pause_reason = message.pause_reason;
			session.network_pause_transition_pending = true;
			session.network_pause_transition_active = true;
		}
		else
		{
			session.network_pause_owner = std::min(
				session.network_pause_owner,
				message.source_player);
			if (static_cast<std::uint8_t>(
					session.network_pause_reason)
				== 0u)
			{
				session.network_pause_reason =
					message.pause_reason;
			}
		}
		diagnostics::mission_log(
			"network rx opcode=0x27 pause=1 reason=%u player=%u owner=%u",
			static_cast<unsigned>(raw_reason),
			static_cast<unsigned>(message.source_player),
			static_cast<unsigned>(session.network_pause_owner));
		return true;
	}
	case mission::NetworkGameplayOpcode::component_damage:
	case mission::NetworkGameplayOpcode::component_state:
	case mission::NetworkGameplayOpcode::bank_damage:
	case mission::NetworkGameplayOpcode::bank_state:
		return mission::network_receive_damage(
			runtime,
			session.world,
			session.ship_stats,
			message,
			session.weapons.feedback_enabled);
	case mission::NetworkGameplayOpcode::deathmatch_respawn:
		if (message.object_index
				>= mission::kNetworkPlayerCapacity
			|| message.spawn_object_index >= (1u << 9))
		{
			return false;
		}
		return mission::network_receive_deathmatch_respawn(
			runtime,
			session.world,
			session.ship_stats,
			static_cast<std::uint8_t>(message.object_index),
			message.spawn_object_index);
	case mission::NetworkGameplayOpcode::session_script_ready:
		if (network.role != mission::NetworkRole::host
			|| message.source_player != message.sync_index
			|| message.sync_index >= network.player_count
			|| !network.connected[message.sync_index])
		{
			return false;
		}
		mission::network_receive_session_script_ready(
			network, message.sync_index);
		return true;
	case mission::NetworkGameplayOpcode::session_script_start:
		if (network.role != mission::NetworkRole::client
			|| message.source_player
				!= network.authority_player)
		{
			return false;
		}
		mission::network_receive_session_script_start(
			network, message.one_way_latency);
		return true;
	case mission::NetworkGameplayOpcode::player_stats:
		return mission::network_receive_player_stats(
			runtime,
			session.world,
			message.source_player,
			message.player_kills,
			message.player_deaths);
	case mission::NetworkGameplayOpcode::player_attack_request:
	case mission::NetworkGameplayOpcode::player_backoff_request:
	case mission::NetworkGameplayOpcode::player_help_request:
		return mission::network_receive_player_comms_command(
			runtime,
			session.world,
			message);
	case mission::NetworkGameplayOpcode::cloak_state:
	{
		if (message.delivery
				!= mission::NetworkDelivery::broadcast_guaranteed
			|| message.source_player >= std::size(session.world.objects))
		{
			return false;
		}
		WorldObject& source =
			session.world.objects[message.source_player];
		if (!source.active)
		{
			return false;
		}
		(void)world_set_cloak_active(
			session.world,
			source,
			message.scenario_flag,
			simulation_tick);
		diagnostics::mission_log(
			"network rx opcode=0x44 cloak=%u player=%u",
			message.scenario_flag ? 1u : 0u,
			static_cast<unsigned>(message.source_player));
		return true;
	}
	case mission::NetworkGameplayOpcode::player_ejected:
		return mission::network_receive_player_ejected(
			network, session.world, message.object_index);
	case mission::NetworkGameplayOpcode::script_sync_ready:
		if (message.sync_index
			>= std::size(runtime.script_sync))
		{
			return false;
		}
		mission::network_receive_script_sync_ready(
			network,
			runtime.script_sync[message.sync_index],
			message.sync_index,
			message.source_player);
		return true;
	case mission::NetworkGameplayOpcode::script_sync_restart:
		if (message.sync_index
				>= std::size(runtime.script_sync)
			|| message.source_player
				!= network.authority_player)
		{
			return false;
		}
		mission::network_receive_script_sync_restart(
			network,
			runtime.script_sync[message.sync_index],
			message.sync_index,
			message.one_way_latency);
		return true;
	case mission::NetworkGameplayOpcode::friendly_fire:
		mission::network_receive_friendly_fire(
			network,
			session.world,
			message.source_player,
			message.all_players);
		return true;
	case mission::NetworkGameplayOpcode::player_target_reference:
		return mission::network_receive_player_target_reference(
			network,
			session.world,
			message);
	case mission::NetworkGameplayOpcode::player_departure:
		return mission::network_receive_player_departure(
			runtime,
			session.world,
			session.ship_stats,
			message.departure_player);
	case mission::NetworkGameplayOpcode::deathmatch_restart:
	case mission::NetworkGameplayOpcode::deathmatch_state_request:
	case mission::NetworkGameplayOpcode::deathmatch_state:
	case mission::NetworkGameplayOpcode::tag_bomb_assignment:
	case mission::NetworkGameplayOpcode::tag_bomb_detonate:
	case mission::NetworkGameplayOpcode::dark_reign_tower_state:
	case mission::NetworkGameplayOpcode::vampire_assignment:
	case mission::NetworkGameplayOpcode::shadow_assignment:
	case mission::NetworkGameplayOpcode::shadow_kill:
	case mission::NetworkGameplayOpcode::nuclear_reset_player:
	case mission::NetworkGameplayOpcode::dark_reign_drop:
	case mission::NetworkGameplayOpcode::nuclear_success:
	case mission::NetworkGameplayOpcode::deathmatch_pickup:
	case mission::NetworkGameplayOpcode::deathmatch_powerup_activate:
	case mission::NetworkGameplayOpcode::deathmatch_unhide_object:
	case mission::NetworkGameplayOpcode::deathmatch_proximity_mine:
	case mission::NetworkGameplayOpcode::deathmatch_reposition_object:
		return mission::deathmatch_scenarios_receive(
			runtime,
			session.world,
			session.ship_stats,
			message,
			simulation_tick);
	default:
		return false;
	}
}

bool mission_session_pop_network_outbound(
	MissionSession& session,
	mission::NetworkOutboundMessage& message)
{
	return mission::network_runtime_pop_outbound(
		session.mission_runtime.network, message);
}

bool mission_session_chat_text(
	MissionSession& session,
	const render::FrontendRenderer& renderer,
	const char* text)
{
	return mission_session_chat_allowed(session)
		&& hud::runtime_append_chat(session.hud, renderer, text);
}

void mission_session_chat_backspace(MissionSession& session)
{
	if (mission_session_chat_allowed(session))
	{
		hud::runtime_backspace_chat(session.hud);
	}
}

bool mission_session_chat_submit(MissionSession& session)
{
	std::int16_t destination = -1;
	char text[hud::kChatMessageBytes]{};
	if (!mission_session_chat_allowed(session)
		|| !hud::runtime_submit_chat(
			session.hud, destination, text))
	{
		return false;
	}
	// CommsMenu_text_input submits even its empty buffer and then clears
	// the complete 64-byte editor storage.
	(void)mission::network_publish_chat(
		session.mission_runtime.network,
		destination,
		text);
	mission::PlayerCommsState& comms =
		session.mission_runtime.player_comms;
	if (comms.menu_open
		&& (comms.current_command == 24
			|| comms.current_command == 25))
	{
		hud::runtime_close_panel(
			session.hud, kCommsMenuPanel);
		mission::player_comms_close_menu(
			session.mission_runtime);
	}
	session.chat_submit_consumed = true;
	return true;
}

void mission_session_render_frame(
	MissionSession& session,
	render::MissionRenderFrame& frame)
{
	frame = {};
	if (session.state != MissionSessionState::running
		&& session.state != MissionSessionState::paused)
	{
		return;
	}
	const WorldObject* player =
		world_resolve(session.world, session.world.player);
	if (player == nullptr)
	{
		return;
	}
	frame.environment = &session.mission_runtime.environment;
	frame.world = &session.world;
	frame.random_seed = &session.world.random_seed;
	frame.simulation_tick =
		static_cast<std::uint32_t>(session.clock.gameplay_tick);
	frame.camera_position = session.camera.position;
	frame.camera_orientation = session.camera.orientation;
	frame.cockpit_local_position = session.camera.cockpit_local_position;
	frame.cockpit_orientation = session.camera.cockpit_orientation;
	frame.cockpit_component_orientation =
		session.camera.cockpit_component_orientation;
	frame.cockpit_recoil_offset = session.camera.cockpit_recoil_offset;
	// SR_view_set_projection_bounds publishes independent horizontal and
	// vertical slopes. Retail does not derive either one from framebuffer
	// aspect (default divisors are .6 and .8 respectively).
	frame.horizontal_tangent = session.camera.horizontal_tangent;
	frame.vertical_tangent = session.camera.vertical_tangent;
	frame.camera_cut_serial = session.camera_cut_serial;
	frame.camera_mode = session.camera_mode;
	frame.camera_view_state = session.camera.view_state;
	if (player->escort_point != UINT16_MAX
		&& player->escort_point < std::size(session.world.objects))
	{
		const WorldObject& target =
			session.world.objects[player->escort_point];
		frame.dock_ring_target_position = target.scene_position;
		frame.dock_ring_target_orientation = target.scene_orientation;
		frame.dock_ring_active = true;
	}
	// Camera mode metadata marks only modes 0..3 as owning/suppressing the
	// selected render object. Every other camera sees the local ship.
	if (player->visible
		&& (player->runtime_flags & kObjectFlagRenderSuppressed) == 0
		&& (player->runtime_flags & kObjectFlagDisabled) == 0
		&& frame.instance_count < std::size(frame.instances))
	{
		render::MissionModel player_model;
		if (render::mission_model_for_type(
			player->type, player_model))
		{
			frame.instances[frame.instance_count++] = {
				player_model,
				player->scene_position,
				player->scene_orientation,
				1.0f,
				UINT32_MAX,
				player,
				player,
			};
		}
	}
	// Objects_service_render (0x004924b0) services non-player slots in
	// ascending order and the player last. Every resulting scene object is
	// prepended, so renderer traversal sees the player first and remaining
	// world slots in descending order. Collect bombard owners independently
	// so their retained ascending callback order is unchanged.
	for (std::uint16_t world_index = 0;
		world_index < std::size(session.world.objects);
		++world_index)
	{
		const WorldObject& object =
			session.world.objects[world_index];
		if (object.active
			&& (object.type == 99 || object.type == 205)
			&& frame.bombard_owner_count
				< std::size(frame.bombard_owners))
		{
			frame.bombard_owners[frame.bombard_owner_count++] = {
				object.scene_position,
				object.scene_orientation,
				object.radius,
				world_index,
				object.creation_serial,
				object.visible
					&& (object.runtime_flags & kObjectFlagDisabled) == 0,
			};
		}
	}
	for (std::uint16_t world_index =
			static_cast<std::uint16_t>(
				std::size(session.world.objects));
		world_index-- > 0;)
	{
		const WorldObject& object =
			session.world.objects[world_index];
		if (!object.active || !object.visible || object.player)
		{
			continue;
		}
		if ((object.runtime_flags & (kObjectFlagRenderSuppressed | kObjectFlagDisabled)) != 0)
		{
			continue;
		}
		render::MissionModel model;
		if (!render::mission_model_for_type(object.type, model))
		{
			continue;
		}
		std::uint32_t selected_part_group_id = UINT32_MAX;
		std::uint16_t selected_part_owner_scope = UINT16_MAX;
		if (session.world.selected_target.index == world_index
			&& session.world.selected_target.generation
				== object.generation
			&& session.world.target_component >= 0
			&& session.world.target_component < object.component_count)
		{
			const ObjectComponent& component =
				object.components[session.world.target_component];
			selected_part_group_id = component.part_group_id;
			if (component.model_reference >= 0
				&& static_cast<std::size_t>(component.model_reference)
					< object.model_references.size())
			{
				selected_part_owner_scope = object.model_references[
					component.model_reference].owner_scope;
			}
		}
		render::MissionRenderInstance& instance =
			frame.instances[frame.instance_count++];
		instance = {
			model,
			object.scene_position,
			object.scene_orientation,
			1.0f,
			selected_part_group_id,
			&object,
			&object,
		};
		instance.selected_part_owner_scope =
			selected_part_owner_scope;
	}
	for (const ParticleFragment& fragment
		: session.world.particles.fragments)
	{
		if (!fragment.active
			|| !fragment.render_active
			|| frame.instance_count >= std::size(frame.instances))
		{
			continue;
		}
		frame.instances[frame.instance_count++] = {
			static_cast<render::MissionModel>(
				fragment.model_resource),
			fragment.position,
			fragment.orientation,
			fragment.scale,
			UINT32_MAX,
			nullptr,
			nullptr,
		};
	}
	for (const RockChunkEffect& chunk
		: session.world.death_effects.rock_chunks)
	{
		if (!chunk.active
			|| !chunk.render_active
			|| frame.instance_count >= std::size(frame.instances))
		{
			continue;
		}
		frame.instances[frame.instance_count++] = {
			static_cast<render::MissionModel>(chunk.model_resource),
			chunk.position,
			chunk.orientation,
			chunk.scale,
			UINT32_MAX,
			nullptr,
			nullptr,
		};
	}
	for (const GunProjectile& projectile
		: session.weapons.projectiles)
	{
		if (projectile.type_index < 0
			|| frame.gun_projectile_count
				>= std::size(frame.gun_projectiles))
		{
			continue;
		}
		frame.gun_projectiles[frame.gun_projectile_count++] = {
			projectile.scene_position,
			projectile.orientation,
			projectile.spawn_tick,
			static_cast<std::uint32_t>(
				std::max(projectile.expiration_tick, 0)),
			{},
			projectile.shooter_affiliation,
			static_cast<std::uint8_t>(projectile.type_index),
		};
		std::copy(
			std::begin(projectile.visual_random),
			std::end(projectile.visual_random),
			std::begin(
				frame.gun_projectiles[
					frame.gun_projectile_count - 1].visual_random));
	}
	for (const NovaBeam& beam : session.weapons.nova_beams)
	{
		if (!beam.active
			|| session.clock.gameplay_tick >= beam.expiration_tick
			|| frame.nova_beam_count
				>= std::size(frame.nova_beams))
		{
			continue;
		}
		const WorldObject* owner =
			beam.owner_index < kMaxGameObjects
				? &session.world.objects[beam.owner_index]
				: nullptr;
		const glm::vec3 beam_start =
			owner != nullptr && owner->active
				? owner->scene_position
				: beam.start;
		const glm::mat3 beam_orientation =
			owner != nullptr && owner->active
				? owner->scene_orientation
				: beam.orientation;
		frame.nova_beams[frame.nova_beam_count++] = {
			beam_start,
			beam_start + beam_orientation[2] * 40000.0f,
			beam_orientation,
			beam.expiration_tick,
			beam.charge,
			beam.fully_charged,
		};
	}
	for (const MuzzleFlash& flash : session.weapons.muzzle_flashes)
	{
		if (!flash.active
			|| session.clock.gameplay_tick >= flash.expiration_tick
			|| frame.muzzle_flash_count
				>= std::size(frame.muzzle_flashes))
		{
			continue;
		}
		frame.muzzle_flashes[frame.muzzle_flash_count++] = {
			flash.position,
			flash.orientation,
			flash.start_tick,
			flash.expiration_tick,
			flash.shooter_affiliation,
			flash.type,
		};
	}
	for (const Missile& missile : session.missiles.missiles)
	{
		if (!missile.active || missile.type < 0
			|| missile.type
				>= static_cast<std::int32_t>(
					assets::kMissileStatsCount)
			|| frame.missile_count >= std::size(frame.missiles))
		{
			continue;
		}
		frame.missiles[frame.missile_count++] = {
			missile.scene_position,
			missile.scene_orientation,
			static_cast<std::uint8_t>(missile.type),
			missile.model_variant,
		};
	}
	for (const MissileTrail& trail : session.missiles.trails)
	{
		if (!trail.active || trail.ring_count < 2
			|| frame.missile_trail_count
				>= std::size(frame.missile_trails))
		{
			continue;
		}
		frame.missile_trails[frame.missile_trail_count++].trail = &trail;
	}
	for (const ChaffRecord& chaff : session.chaff.records)
	{
		if (!chaff.active
			|| frame.instance_count >= std::size(frame.instances))
		{
			continue;
		}
		frame.instances[frame.instance_count++] = {
			render::MissionModel::decoy,
			chaff.position,
			chaff.orientation,
			1.0f,
			UINT32_MAX,
			nullptr,
			nullptr,
		};
	}
	for (std::uint16_t index = 0;
		index < session.chaff.particle_high_water
			&& frame.particle_count < std::size(frame.particles);
		++index)
	{
		const ChaffParticle& particle = session.chaff.particles[index];
		if (!particle.active)
		{
			continue;
		}
		const std::uint32_t age =
			static_cast<std::uint32_t>(session.clock.gameplay_tick)
			- particle.birth_tick;
		if (age >= particle.lifetime_ticks)
		{
			continue;
		}
		const float t = static_cast<float>(age)
			/ static_cast<float>(particle.lifetime_ticks);
		const float size = 50.0f + 50.0f * t;
		const float channel = std::clamp(
			-0.1f * t * t - 0.15f * t + 0.25f,
			0.0f,
			1.0f);
		const std::uint8_t component = static_cast<std::uint8_t>(
			std::lrint(channel * 255.0f));
		const std::uint32_t color =
			0xff000000u
			| static_cast<std::uint32_t>(component) << 16
			| static_cast<std::uint32_t>(component) << 8
			| component;
		frame.particles[frame.particle_count++] = {
			particle.position,
			size,
			color,
		};
	}
	const std::uint32_t now_tick =
		static_cast<std::uint32_t>(session.clock.gameplay_tick);
	for (std::uint16_t owner = 0;
		owner < std::size(session.world.objects);
		++owner)
	{
		game::WorldObject& object = session.world.objects[owner];
			if (!object.active
				|| !object.external_trail_active)
		{
			continue;
		}
		mission::LaunchTrailRing& ring =
			session.mission_runtime.launch_trail_rings[
				session.mission_runtime.launch_trail_cursor
					% std::size(
						session.mission_runtime.launch_trail_rings)];
			++session.mission_runtime.launch_trail_cursor;
		const glm::vec3 local[4] = {
			{object.bounds_max.x,
				object.bounds_max.y,
				object.bounds_min.z},
			{object.bounds_min.x,
				object.bounds_max.y,
				object.bounds_min.z},
			{object.bounds_min.x,
				object.bounds_min.y,
				object.bounds_min.z},
			{object.bounds_max.x,
				object.bounds_min.y,
				object.bounds_min.z},
		};
		for (std::uint8_t point = 0; point < 4; ++point)
		{
			glm::vec3 jittered = local[point];
			jittered.x *=
				0.5f
				+ static_cast<float>(
					game::world_rand15(session.world))
					/ 32767.0f * 1.5f;
			jittered.y *=
				0.5f
				+ static_cast<float>(
					game::world_rand15(session.world))
					/ 32767.0f * 1.5f;
			ring.points[point] =
				object.scene_position
					+ object.scene_orientation * jittered;
		}
			ring.birth_tick = now_tick;
			ring.sequence =
				session.mission_runtime.launch_trail_cursor;
			ring.owner = owner;
			ring.generation = object.generation;
			ring.active = true;
	}
	for (mission::LaunchParticle& particle
		: session.mission_runtime.launch_particles)
	{
		if (!particle.active)
		{
			continue;
		}
		const std::uint32_t age_ticks =
			now_tick - particle.birth_tick;
		if (age_ticks >= particle.lifetime_ticks)
		{
			particle.active = false;
			continue;
		}
		if (frame.particle_count >= std::size(frame.particles))
		{
			continue;
		}
		const float phase =
			static_cast<float>(age_ticks)
				/ static_cast<float>(particle.lifetime_ticks);
		const float size =
			-40.0f * phase * phase + 120.0f * phase + 20.0f;
		const float intensity = 1.0f - phase * phase;
		const std::uint8_t channel = static_cast<std::uint8_t>(
			std::lrint(std::clamp(intensity, 0.0f, 1.0f) * 255.0f));
		const std::uint32_t color =
			0xff000000u
			| static_cast<std::uint32_t>(channel) << 16
			| static_cast<std::uint32_t>(channel) << 8
			| channel;
		frame.particles[frame.particle_count++] = {
			particle.position
				+ particle.velocity
					* static_cast<float>(age_ticks),
			size,
			color,
		};
	}
	for (mission::LaunchTrailRing& ring
		: session.mission_runtime.launch_trail_rings)
	{
		if (!ring.active)
		{
			continue;
		}
		if (now_tick - ring.birth_tick >= 70)
		{
			ring.active = false;
			continue;
		}
		if (frame.launch_trail_ring_count
			>= std::size(frame.launch_trail_rings))
		{
			continue;
		}
		render::MissionLaunchTrailRing& output =
			frame.launch_trail_rings[
				frame.launch_trail_ring_count++];
		std::copy(
			std::begin(ring.points),
			std::end(ring.points),
			std::begin(output.points));
			output.birth_tick = ring.birth_tick;
			output.sequence = ring.sequence;
			output.owner = ring.owner;
		output.generation = ring.generation;
	}
	for (std::uint32_t index = 0;
		index < frame.launch_trail_ring_count
			&& frame.particle_count < std::size(frame.particles);
		++index)
	{
		const render::MissionLaunchTrailRing& ring =
			frame.launch_trail_rings[index];
		bool latest = true;
		for (std::uint32_t candidate = 0;
			candidate < frame.launch_trail_ring_count;
			++candidate)
		{
			if (frame.launch_trail_rings[candidate].owner == ring.owner
				&& frame.launch_trail_rings[candidate].generation
					== ring.generation
					&& frame.launch_trail_rings[candidate].sequence
						> ring.sequence)
			{
				latest = false;
				break;
			}
		}
		if (!latest)
		{
			continue;
		}
			const game::WorldObject* trail_owner =
				game::world_resolve(
					session.world,
					{ring.owner, ring.generation});
			if (trail_owner == nullptr)
			{
				continue;
			}
			const float random_extent =
			1.0f
			+ static_cast<float>(
				game::world_rand15(session.world))
				/ 32767.0f
				* (1.0f / 6.0f);
			const float rear_width =
				trail_owner->bounds_max.x * 2.0f;
			const float glow_width =
				rear_width * 2.0f * random_extent;
			frame.particles[frame.particle_count++] = {
				trail_owner->scene_position
					+ trail_owner->scene_orientation
						* glm::vec3{
							0.0f,
							0.0f,
							trail_owner->bounds_min.z - 50.0f},
				glow_width,
				0xffffffffu,
				rear_width / glow_width,
				true,
				trail_owner->scene_orientation,
				true,
			};
	}
}

void mission_session_build(
	MissionSession& session,
	const Config& config,
	const LanguageTable& language,
	render::FrontendRenderer& renderer,
	const render::MissionRenderer& mission_renderer,
	const render::MissionRenderFrame& mission_frame,
	render::FrontendCommands& commands,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now)
{
	if (session.state == MissionSessionState::paused)
	{
		const frontend::GameplayPauseView view =
			mission_session_pause_view(session);
		frontend::gameplay_pause_build(
			session.pause,
			view,
			language,
			renderer,
			commands,
			drawable_width,
			drawable_height,
			now);
		return;
	}

	render::frontend_commands_begin(commands);
	const hud::Layout layout =
		hud::make_layout(drawable_width, drawable_height);
	const float scale = layout.element_scale;
	if (session.state == MissionSessionState::load_failed)
	{
		render::frontend_rgba_quad(
			commands,
			renderer.white,
			0.0f,
			0.0f,
			layout.width,
			layout.height,
			0x020609ff);
		centered_text(
			commands,
			renderer,
			"INSTANT ACTION COULD NOT START",
			layout.width * 0.5f,
			layout.height * 0.28f,
			scale,
			renderer.shell.font_white_palette);
		centered_text(
			commands,
			renderer,
			session.load_error_detail,
			layout.width * 0.5f,
			layout.height * 0.42f,
			scale,
			renderer.shell.font_gold_palette);
		const float button_width = kErrorButtonWidth * scale;
		const float button_height = kErrorButtonHeight * scale;
		const float left = (layout.width - button_width) * 0.5f;
		const float top = layout.height * 0.67f;
		render::frontend_rgba_quad(
			commands,
			renderer.white,
			left,
			top,
			button_width,
			button_height,
			session.error_return_hovered ? 0x326f82ff : 0x17343fff);
		centered_text(
			commands,
			renderer,
			"RETURN",
			layout.width * 0.5f,
			top + 6.0f * scale,
			scale,
			session.error_return_hovered
				? renderer.shell.font_white_palette
				: renderer.shell.font_gold_palette);
		const std::uint64_t elapsed =
			now > session.entered_at ? now - session.entered_at : 0;
		const std::size_t cursor_frame = static_cast<std::size_t>(
			(elapsed / 40) % std::size(renderer.shell.cursor));
		const render::FrontendTexture& cursor =
			renderer.shell.cursor[cursor_frame];
		render::frontend_indexed_scaled_quad(
			commands,
			cursor,
			renderer.shell.cursor_palette,
			session.error_pointer_x,
			session.error_pointer_y,
			cursor.width * scale,
			cursor.height * scale);
		return;
	}

	if (session.camera_mode == 0
		&& (session.hud.whiteout_exhaust > 0.0f
			|| session.hud.whiteout_red > 0.0f))
	{
		// The retail camera-attached whiteout BMO uses material bytes
		// mode_2=0, mode_4=1, mode_6=1. Surrender's D3D selector-one
		// tables resolve that to untextured diffuse ONE+ONE blending.
		// ExhaustHazard's retained percentage wins over ordinary red hit
		// distortion and drives all three color channels equally.
		const float exhaust = std::clamp(
			session.hud.whiteout_exhaust, 0.0f, 1.0f);
		const std::uint32_t red = static_cast<std::uint32_t>(
			std::nearbyint(
				(exhaust > 0.0f
					? exhaust
					: std::clamp(
						session.hud.whiteout_red, 0.0f, 1.0f))
					* 255.0f));
		const std::uint32_t exhaust_channel =
			static_cast<std::uint32_t>(
				std::nearbyint(exhaust * 255.0f));
		render::frontend_rgba_additive_quad(
			commands,
			renderer.white,
			0.0f,
			0.0f,
			static_cast<float>(drawable_width),
			static_cast<float>(drawable_height),
			(red << 24)
				| (exhaust_channel << 16)
				| (exhaust_channel << 8)
				| 0x000000ffu);
	}
		const char* const launch_title =
			session.mission_runtime.mission_number >= 1
				&& session.mission_runtime.mission_number < 29
			? language_text(
				language,
				static_cast<std::uint16_t>(
					0x3d1u
					+ session.mission_runtime.mission_number))
			: nullptr;
		if (session.mission_runtime.launch_title_active
			&& launch_title != nullptr
			&& session.mission_runtime.launch_title_reveal
				< std::strlen(launch_title)
			&& session.clock.gameplay_tick
				> session.mission_runtime.launch_title_next_tick)
		{
			// HUD_render_frame_callback 0x0048464b..0x00484695 advances
			// the typewriter only while the reveal index remains below the
			// localized title length. Once complete, the counter is stable.
			session.mission_runtime.launch_title_next_tick =
				static_cast<std::uint32_t>(session.clock.gameplay_tick) + 8u;
			++session.mission_runtime.launch_title_reveal;
		}
		hud::render(
		session.hud,
		session.world,
		session.mission_runtime,
		session.ship_stats,
		session.gun_stats,
		config,
		language,
		mission_renderer,
		mission_frame,
		renderer,
		commands,
		drawable_width,
		drawable_height,
		session.clock.gameplay_tick,
		session.request.mission);
}
}
