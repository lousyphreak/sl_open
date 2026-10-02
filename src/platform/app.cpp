#define SDL_MAIN_USE_CALLBACKS 1

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <bgfx/bgfx.h>

#include "assets/image.hpp"
#include "assets/pilot_presentation.hpp"
#include "assets/gameplay_model.hpp"
#include "assets/vfx.hpp"
#include "audio/audio.hpp"
#include "audio/cb97_stream.hpp"
#include "audio/fat.hpp"
#include "audio/mp3.hpp"
#include "audio/wav.hpp"
#include "campaign/campaign.hpp"
#include "config/config.hpp"
#include "core/mission_log.hpp"
#include "frontend/campaign_frontend.hpp"
#include "frontend/campaign_induction.hpp"
#include "frontend/campaign_hub.hpp"
#include "frontend/cd_player.hpp"
#include "frontend/gui.hpp"
#include "frontend/itac_shell.hpp"
#include "frontend/loadout.hpp"
#include "frontend/main_menu.hpp"
#include "frontend/medal_display.hpp"
#include "frontend/mission_briefing.hpp"
#include "frontend/multiplayer_debrief.hpp"
#include "frontend/multiplayer_frontend.hpp"
#include "frontend/multiplayer_lobby.hpp"
#include "frontend/news_report.hpp"
#include "frontend/options.hpp"
#include "frontend/post_mission_choice.hpp"
#include "frontend/sim_pod.hpp"
#include "game/model_animation.hpp"
#include "input/controls.hpp"
#include "io/vfs.hpp"
#include "localization/language.hpp"
#include "media/bink_player.hpp"
#include "network/multiplayer_transport.hpp"
#include "platform/app_internal.hpp"
#include "platform/display.hpp"
#include "platform/frontend_flow.hpp"
#include "platform/frontend_loading.hpp"
#include "render/frontend_renderer.hpp"
#include "render/mission_renderer.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <utility>

void publish_mission_sound_owner_slot(
	void* userdata,
	const sl_open::audio::Sound3DSource& source,
	std::int16_t slot);

namespace
{
constexpr bgfx::ViewId kMainView = 0;
constexpr std::uint64_t kControlWheelQuietPeriodMs = 150;

using sl_open::platform::App;
using sl_open::platform::advance_startup_logos;
using sl_open::platform::begin_transition;
using sl_open::platform::enter_campaign;
using sl_open::platform::enter_campaign_hub;
using sl_open::platform::enter_multiplayer;
using sl_open::platform::enter_transition_destination;
using sl_open::platform::FrontendPhase;
using sl_open::platform::MissionFailureKind;
using sl_open::platform::play_fat_sample;
using sl_open::platform::play_frontend_click;
using sl_open::platform::play_walk_sound;
using sl_open::platform::prepare_campaign_hub;
using sl_open::platform::start_hub_actor;
using sl_open::platform::start_hub_ambience;
using sl_open::platform::StartupError;
using sl_open::platform::stop_briefing_room_chatter;
using sl_open::platform::stop_briefing_voice;
using sl_open::platform::stop_hub_ambience;
using sl_open::platform::TransitionAudio;
using sl_open::platform::TransitionDestination;

App g_app{};

struct MultiplayerFrontendSessions
{
	sl_open::frontend::MultiplayerSession
		sessions[sl_open::network::kMultiplayerDiscoveryCapacity]{};
	sl_open::frontend::MultiplayerSessionView view;
};

void copy_multiplayer_session_id(
	sl_open::frontend::MultiplayerSessionId& destination,
	const sl_open::network::MultiplayerSessionId& source)
{
	static_assert(sizeof(destination.bytes) == sizeof(source.bytes));
	std::memcpy(
		destination.bytes, source.bytes, sizeof(destination.bytes));
}

bool multiplayer_session_id_equal(
	const sl_open::frontend::MultiplayerSessionId& left,
	const sl_open::network::MultiplayerSessionId& right)
{
	static_assert(sizeof(left.bytes) == sizeof(right.bytes));
	return std::memcmp(
		left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

MultiplayerFrontendSessions multiplayer_frontend_sessions(
	const App& app)
{
	MultiplayerFrontendSessions result;
	const sl_open::network::MultiplayerDiscoveryView discovery =
		sl_open::network::multiplayer_transport_discovery_view(
			app.multiplayer_transport);
	const std::uint32_t count = std::min<std::uint32_t>(
		discovery.count, std::size(result.sessions));
	for (std::uint32_t index = 0; index < count; ++index)
	{
		const sl_open::network::MultiplayerDiscoverySession& source =
			discovery.sessions[index];
		sl_open::frontend::MultiplayerSession& destination =
			result.sessions[index];
		copy_multiplayer_session_id(
			destination.id, source.session_id);
		destination.name = source.session_name;
		destination.mission = source.mission;
		destination.player_count = source.player_count;
		destination.game_type =
			source.mode
					== sl_open::network::MultiplayerSessionMode::deathmatch
				? sl_open::frontend::MultiplayerGameType::deathmatch
				: sl_open::frontend::MultiplayerGameType::cooperative;
	}
	result.view.sessions = result.sessions;
	result.view.count = count;
	return result;
}

const sl_open::network::MultiplayerDiscoverySession*
find_multiplayer_discovery_session(
	const App& app,
	const sl_open::frontend::MultiplayerSessionId& id)
{
	const sl_open::network::MultiplayerDiscoveryView discovery =
		sl_open::network::multiplayer_transport_discovery_view(
			app.multiplayer_transport);
	for (std::uint32_t index = 0; index < discovery.count; ++index)
	{
		if (multiplayer_session_id_equal(
			id, discovery.sessions[index].session_id))
		{
			return &discovery.sessions[index];
		}
	}
	return nullptr;
}

template <std::size_t N>
bool append_text(char (&destination)[N], const char* text)
{
	if (text == nullptr)
	{
		return false;
	}
	std::size_t length = 0;
	while (length < N && destination[length] != '\0')
	{
		++length;
	}
	if (length == N)
	{
		destination[N - 1] = '\0';
		length = N - 1;
	}
	bool changed = false;
	for (const unsigned char* source =
			reinterpret_cast<const unsigned char*>(text);
		*source != 0 && length + 1 < N;
		++source)
	{
		if (*source >= 0x20 && *source < 0x7f)
		{
			destination[length++] = static_cast<char>(*source);
			changed = true;
		}
	}
	destination[length] = '\0';
	return changed;
}

template <std::size_t N>
bool backspace_text(char (&destination)[N])
{
	std::size_t length = 0;
	while (length < N && destination[length] != '\0')
	{
		++length;
	}
	if (length == 0 || length == N)
	{
		return false;
	}
	destination[length - 1] = '\0';
	return true;
}

template <std::size_t Lines, std::size_t Bytes>
void push_multiplayer_chat_line(
	char (&lines)[Lines][Bytes],
	std::uint8_t& line_count,
	const char* callsign,
	const char* text)
{
	const std::size_t retained = std::min<std::size_t>(
		line_count, Lines - 1);
	for (std::size_t index = retained; index != 0; --index)
	{
		std::memcpy(lines[index], lines[index - 1], Bytes);
	}
	std::snprintf(
		lines[0],
		Bytes,
		"%s: %s",
		callsign == nullptr ? "" : callsign,
		text == nullptr ? "" : text);
	line_count = static_cast<std::uint8_t>(
		std::min<std::size_t>(line_count + 1, Lines));
}

const sl_open::network::MultiplayerLobbyPlayer*
multiplayer_local_lobby_player(const App& app)
{
	const std::uint8_t slot =
		app.multiplayer_transport.local_lobby_slot;
	const sl_open::network::MultiplayerLobbySnapshot& snapshot =
		sl_open::network::multiplayer_transport_lobby(
			app.multiplayer_transport);
	return slot < sl_open::network::kMultiplayerTransportPlayerCapacity
		&& snapshot.players[slot].connected
		? &snapshot.players[slot]
		: nullptr;
}

sl_open::frontend::MultiplayerLobbyView multiplayer_lobby_view(
	const App& app)
{
	sl_open::frontend::MultiplayerLobbyView view;
	const sl_open::network::MultiplayerLobbySnapshot& snapshot =
		sl_open::network::multiplayer_transport_lobby(
			app.multiplayer_transport);
	view.kind = snapshot.rules.mode
			== sl_open::network::MultiplayerSessionMode::deathmatch
		? sl_open::frontend::MultiplayerLobbyKind::deathmatch
		: sl_open::frontend::MultiplayerLobbyKind::cooperative;
	view.role =
		sl_open::network::multiplayer_transport_is_host(
			app.multiplayer_transport)
		? sl_open::frontend::MultiplayerLobbyRole::host
		: sl_open::frontend::MultiplayerLobbyRole::client;
	for (std::uint8_t slot = 0;
		slot < sl_open::network::kMultiplayerTransportPlayerCapacity
			&& view.player_count
				< sl_open::frontend::kMultiplayerLobbyPlayerCapacity;
		++slot)
	{
		const sl_open::network::MultiplayerLobbyPlayer& source =
			snapshot.players[slot];
		if (!source.connected)
		{
			continue;
		}
		sl_open::frontend::MultiplayerLobbyPlayer& destination =
			view.players[view.player_count++];
		destination.identity =
			static_cast<std::uint64_t>(slot) + 1;
		destination.callsign = source.name;
		destination.team = source.team;
		destination.selected_ship = source.selected_ship;
		destination.connected = true;
		destination.local =
			slot == app.multiplayer_transport.local_lobby_slot;
		destination.ready = source.ready;
	}
	for (std::uint8_t index = 0;
		index < app.multiplayer_lobby_chat_count;
		++index)
	{
		view.chat_lines[index] =
			app.multiplayer_lobby_chat_lines[index];
	}
	view.chat_line_count = app.multiplayer_lobby_chat_count;
	const sl_open::network::MultiplayerLobbyPlayer* local =
		multiplayer_local_lobby_player(app);
	view.callsign = local == nullptr
		? app.multiplayer_callsign
		: local->name;
	view.host_callsign =
		snapshot.leader_slot
				< sl_open::network::kMultiplayerTransportPlayerCapacity
			&& snapshot.players[snapshot.leader_slot].connected
		? snapshot.players[snapshot.leader_slot].name
		: "";
	view.session_name =
		view.role == sl_open::frontend::MultiplayerLobbyRole::host
			? app.multiplayer_session_name
			: snapshot.session_name;
	view.chat_entry = app.multiplayer_lobby_chat_entry;
	view.cooperative_mission = snapshot.rules.mission;
	view.cooperative_saved_game =
		app.multiplayer_campaign_loaded_save;
	view.local_ready = local != nullptr && local->ready;
	view.ai_turrets = snapshot.rules.ai_turrets;
	view.target_players = snapshot.rules.respawn_targetable;
	view.teamplay = snapshot.rules.team_mode;
	if (snapshot.rules.mode
		== sl_open::network::MultiplayerSessionMode::deathmatch)
	{
		view.deathmatch_scenario =
			static_cast<sl_open::frontend::DeathmatchScenario>(
				snapshot.rules.mission);
		view.teamplay_supported =
			sl_open::frontend::deathmatch_scenario_supports_teamplay(
				view.deathmatch_scenario);
	}
	view.launch_allowed =
		sl_open::network::multiplayer_transport_udp_ready(
			app.multiplayer_transport);
	if (snapshot.rules.mode
		== sl_open::network::MultiplayerSessionMode::cooperative)
	{
		view.cooperative_campaign.identity =
			app.multiplayer_campaign_valid ? 1 : 0;
		view.cooperative_campaign.loadout =
			app.multiplayer_campaign.loadout;
		view.cooperative_campaign.callsign =
			app.multiplayer_campaign.callsign;
		view.cooperative_campaign.save_name =
			app.multiplayer_campaign_loaded_save
				? app.multiplayer_campaign.id
				: "";
		view.cooperative_campaign.mission =
			app.multiplayer_campaign.mission;
		view.cooperative_campaign.selected_ship =
			app.multiplayer_campaign.selected_ship;
		view.cooperative_campaign.loadout_count =
			sl_open::frontend::kMultiplayerLobbyLoadoutCapacity;
		view.cooperative_campaign.valid =
			app.multiplayer_campaign_valid;
		view.cooperative_campaign.loaded_save =
			app.multiplayer_campaign_loaded_save;
		if (view.role
			== sl_open::frontend::MultiplayerLobbyRole::host)
		{
			view.launch_allowed =
				view.launch_allowed
				&& app.multiplayer_campaign_valid;
		}
	}
	return view;
}

sl_open::frontend::MultiplayerDebriefView multiplayer_debrief_view(
	const App& app)
{
	sl_open::frontend::MultiplayerDebriefView view;
	const sl_open::network::MultiplayerLobbySnapshot& snapshot =
		sl_open::network::multiplayer_transport_lobby(
			app.multiplayer_transport);
	const sl_open::network::MultiplayerMissionResultSnapshot& result =
		sl_open::network::multiplayer_transport_mission_result(
			app.multiplayer_transport);
	view.report_title = app.multiplayer_report.title;
	view.report_body = app.multiplayer_report.body;
	view.report_recipient = app.multiplayer_report.recipient;
	view.chat_entry = app.multiplayer_debrief_chat_entry;
	view.chat_line_count = std::min<std::uint8_t>(
		app.multiplayer_debrief_chat_count,
		sl_open::frontend::kMultiplayerDebriefChatLineCapacity);
	for (std::uint8_t index = 0; index < view.chat_line_count; ++index)
	{
		view.chat_lines[index] =
			app.multiplayer_debrief_chat_lines[index];
	}
	sl_open::game::MultiplayerLaunchSnapshot launch;
	if (!sl_open::network::multiplayer_transport_launch_snapshot(
			app.multiplayer_transport, launch))
	{
		return view;
	}
	const std::uint8_t launch_count = std::min<std::uint8_t>(
		launch.player_count,
		sl_open::frontend::kMultiplayerDebriefPlayerCapacity);
	for (std::uint8_t gameplay_player = 0;
		gameplay_player < launch_count;
		++gameplay_player)
	{
		const std::uint8_t lobby_slot =
			sl_open::network::
				multiplayer_transport_lobby_slot_for_gameplay_player(
					app.multiplayer_transport, gameplay_player);
		if (lobby_slot
				>= sl_open::network::kMultiplayerTransportPlayerCapacity
			|| !snapshot.players[lobby_slot].connected)
		{
			continue;
		}
		const sl_open::network::MultiplayerLobbyPlayer& source =
			snapshot.players[lobby_slot];
		sl_open::frontend::MultiplayerDebriefPlayer& destination =
			view.players[view.player_count++];
		destination.callsign = source.name;
		destination.local =
			gameplay_player
				== sl_open::network::
					multiplayer_transport_local_gameplay_player(
						app.multiplayer_transport);
		destination.ready = source.post_mission_ready;
		destination.restarting = source.outcome_requires_restart;
		destination.leader =
			gameplay_player == result.debrief_leader_player;
	}
	view.local_is_leader =
		sl_open::network::multiplayer_transport_local_gameplay_player(
			app.multiplayer_transport)
			== result.debrief_leader_player;
	view.leader_valid = result.debrief_leader_valid;
	for (const std::uint16_t kills :
		app.multiplayer_campaign.mission_score_events)
	{
		view.overall_kills += kills;
	}
	const sl_open::network::MultiplayerPlayerMissionReport* const local_report =
		sl_open::network::multiplayer_transport_local_mission_report(
			app.multiplayer_transport);
	const std::uint16_t mission = local_report != nullptr
		? local_report->result.mission
		: app.multiplayer_campaign.mission;
	if (mission >= 1 && mission <= sl_open::campaign::kMissionCount)
	{
		view.mission_kills =
			app.multiplayer_campaign.mission_score_events[mission - 1];
	}
	view.rank = app.multiplayer_campaign.rank;
	view.level = app.multiplayer_campaign.progression;
	return view;
}

void apply_default_multiplayer_loadout(
	const App& app,
	std::int16_t selected_ship,
	std::int16_t (&loadout)[sl_open::game::kMissionLoadoutSlots])
{
	std::fill(std::begin(loadout), std::end(loadout), std::int16_t{-1});
	if (selected_ship < 0
		|| selected_ship
			>= static_cast<std::int16_t>(
				std::size(app.frontend_renderer.loadout_renderer.ship_models)))
	{
		return;
	}
	const sl_open::render::MissionGpuModel& ship =
		app.loadout_catalog.ships[selected_ship];
	const std::uint32_t hardpoint_count =
		std::min<std::uint32_t>(
			ship.hardpoints.size(), std::size(loadout));
	for (std::uint32_t index = 0;
		index < hardpoint_count;
		++index)
	{
		loadout[index] =
			static_cast<std::int16_t>(
				ship.hardpoints[index].default_loadout[0]);
	}
}

sl_open::network::MultiplayerLobbyPlayer make_multiplayer_local_player(
	const App& app,
	bool cooperative)
{
	sl_open::network::MultiplayerLobbyPlayer player;
	std::snprintf(
		player.name, sizeof(player.name), "%s",
		app.multiplayer_callsign);
	const sl_open::campaign::CampaignState* const personal_campaign =
		!cooperative
			? nullptr
			: app.multiplayer_campaign_valid
				? &app.multiplayer_campaign
				: app.multiplayer_local_profile_valid
					? &app.multiplayer_local_profile
					: nullptr;
	player.selected_ship = personal_campaign != nullptr
		? std::clamp<std::int16_t>(
			personal_campaign->selected_ship, 0, 11)
		: 0;
	player.team = -1;
	if (personal_campaign != nullptr)
	{
		std::copy(
			std::begin(personal_campaign->loadout),
			std::end(personal_campaign->loadout),
			std::begin(player.loadout));
	}
	else
	{
		apply_default_multiplayer_loadout(
			app, player.selected_ship, player.loadout);
	}
	player.connected = true;
	return player;
}

bool initialize_multiplayer_local_profile(App& app)
{
	sl_open::campaign::CampaignState profile;
	bool loaded = false;
	if (app.campaign_store.campaign_count != 0)
	{
		loaded = sl_open::campaign::campaign_profile_load(
			app.campaign_store,
			app.campaign_store.campaigns[0].id,
			profile);
	}
	if (!loaded)
	{
		sl_open::campaign::campaign_defaults(
			profile,
			app.multiplayer_callsign,
			sl_open::campaign::Difficulty::medium,
			sl_open::campaign::Pilot::female);
		if (!sl_open::campaign::campaign_create(
				app.campaign_store, profile))
		{
			SDL_Log(
				"Initial multiplayer profile could not be persisted");
		}
	}
	app.multiplayer_local_profile = profile;
	app.multiplayer_local_profile_valid =
		profile.id[0] != '\0';
	if (profile.callsign[0] != '\0')
	{
		std::snprintf(
			app.multiplayer_callsign,
			sizeof(app.multiplayer_callsign),
			"%.*s",
			static_cast<int>(
				sizeof(app.multiplayer_callsign) - 1),
			profile.callsign);
	}
	return app.multiplayer_local_profile_valid;
}

sl_open::campaign::CampaignState localize_multiplayer_campaign(
	const App& app,
	const sl_open::campaign::CampaignState& source)
{
	sl_open::campaign::CampaignState result = source;
	if (!app.multiplayer_local_profile_valid)
	{
		return result;
	}
	std::snprintf(
		result.id,
		sizeof(result.id),
		"%s",
		app.multiplayer_local_profile.id);
	std::snprintf(
		result.callsign,
		sizeof(result.callsign),
		"%s",
		app.multiplayer_callsign);
	return result;
}

sl_open::game::MultiplayerCampaignLaunchState
multiplayer_local_profile_launch_state(const App& app)
{
	sl_open::game::MultiplayerCampaignLaunchState launch;
	launch.state = app.multiplayer_local_profile;
	launch.present = app.multiplayer_local_profile_valid;
	return launch;
}

bool synchronize_multiplayer_personal_campaign(
	App& app,
	const sl_open::campaign::CampaignState& campaign,
	bool mission_25_alternate = false)
{
	sl_open::game::MultiplayerCampaignLaunchState launch;
	launch.state = campaign;
	launch.present = true;
	launch.mission_25_alternate = mission_25_alternate;
	return sl_open::network::multiplayer_transport_set_personal_campaign(
		app.multiplayer_transport, launch);
}

bool publish_multiplayer_coop_selection(App& app)
{
	if (!sl_open::network::multiplayer_transport_is_host(
			app.multiplayer_transport)
		|| !app.multiplayer_campaign_valid)
	{
		return false;
	}
	const sl_open::network::MultiplayerLobbySnapshot& lobby =
		sl_open::network::multiplayer_transport_lobby(
			app.multiplayer_transport);
	if (lobby.rules.mode
			!= sl_open::network::MultiplayerSessionMode::cooperative
		|| lobby.rules.mission == 0
		|| lobby.rules.mission
			> sl_open::campaign::kMissionCount)
	{
		return false;
	}
	// Retail opcode 8 (FUN_004ba2d0 / receive 0x004b7521) carries
	// DAT_00562dcc, the active campaign MISS/save identifier, followed by
	// localized string 0x414 and the selected one-byte mission number.
	return sl_open::network::multiplayer_transport_set_coop_mission_selection(
		app.multiplayer_transport,
		app.multiplayer_campaign.id,
		sl_open::language_text(app.language, 0x414),
		static_cast<std::uint8_t>(lobby.rules.mission));
}

void reset_multiplayer_chat(App& app)
{
	app.multiplayer_lobby_chat_count = 0;
	app.multiplayer_debrief_chat_count = 0;
	app.multiplayer_lobby_chat_entry[0] = '\0';
	app.multiplayer_debrief_chat_entry[0] = '\0';
	for (auto& line : app.multiplayer_lobby_chat_lines)
	{
		line[0] = '\0';
	}
	for (auto& line : app.multiplayer_debrief_chat_lines)
	{
		line[0] = '\0';
	}
}

void enter_multiplayer_lobby(App& app, std::uint64_t now)
{
	const sl_open::network::MultiplayerLobbySnapshot& snapshot =
		sl_open::network::multiplayer_transport_lobby(
			app.multiplayer_transport);
	const auto kind =
		snapshot.rules.mode
				== sl_open::network::MultiplayerSessionMode::deathmatch
			? sl_open::frontend::MultiplayerLobbyKind::deathmatch
			: sl_open::frontend::MultiplayerLobbyKind::cooperative;
	const auto role =
		sl_open::network::multiplayer_transport_is_host(
			app.multiplayer_transport)
			? sl_open::frontend::MultiplayerLobbyRole::host
			: sl_open::frontend::MultiplayerLobbyRole::client;
	sl_open::frontend::multiplayer_lobby_enter(
		app.multiplayer_lobby, kind, role, now);
	app.frontend_phase = FrontendPhase::multiplayer_lobby;
	app.transition_holds_frame = false;
	SDL_StopTextInput(app.window);
	if (app.audio.ready
		&& !app.menu_music.active
		&& !sl_open::audio::wav_stream_open(
			app.vfs,
			"music/New_Pensive.wav",
			app.audio.streams[0].source,
			app.audio.streams[0].buffers,
			true,
			app.menu_music))
	{
		SDL_Log("Multiplayer lobby music could not be opened");
	}
	sl_open::audio::apply_stream_gains(app.audio);
}

bool host_multiplayer_session(
	App& app,
	sl_open::network::MultiplayerSessionMode mode,
	std::uint64_t now)
{
	const bool cooperative =
		mode == sl_open::network::MultiplayerSessionMode::cooperative;
	sl_open::network::MultiplayerLobbyRules rules;
	rules.authoritative_seed = static_cast<std::uint32_t>(now);
	rules.mission = cooperative
		? 1
		: sl_open::frontend::deathmatch_scenario_mission(
			sl_open::frontend::DeathmatchScenario::asteroid_field);
	rules.mode = mode;
	rules.configured_team_count = -1;
	rules.team_mode = false;
	rules.respawn_targetable = true;
	app.multiplayer_campaign_valid = false;
	app.multiplayer_campaign_loaded_save = false;
	sl_open::campaign::multiplayer_campaign_abandon(
		app.multiplayer_progression);
	if (!sl_open::network::multiplayer_transport_set_personal_campaign(
			app.multiplayer_transport,
			multiplayer_local_profile_launch_state(app)))
	{
		return false;
	}
	const sl_open::network::MultiplayerLobbyPlayer local =
		make_multiplayer_local_player(app, cooperative);
	if (!sl_open::network::multiplayer_transport_host(
			app.multiplayer_transport,
			app.multiplayer_session_name,
			local,
			rules,
			sl_open::network::kMultiplayerDefaultSessionPort,
			now))
	{
		return false;
	}
	app.multiplayer_ai_turrets = false;
	reset_multiplayer_chat(app);
	enter_multiplayer_lobby(app, now);
	return true;
}

bool join_multiplayer_session(
	App& app,
	const sl_open::network::MultiplayerDiscoverySession& session,
	std::uint64_t now)
{
	sl_open::campaign::multiplayer_campaign_abandon(
		app.multiplayer_progression);
	app.multiplayer_campaign_valid = false;
	app.multiplayer_campaign_loaded_save = false;
	if (!sl_open::network::multiplayer_transport_set_personal_campaign(
			app.multiplayer_transport,
			multiplayer_local_profile_launch_state(app)))
	{
		return false;
	}
	const sl_open::network::MultiplayerLobbyPlayer local =
		make_multiplayer_local_player(
			app,
			session.mode
				== sl_open::network::MultiplayerSessionMode::cooperative);
	return sl_open::network::multiplayer_transport_join(
		app.multiplayer_transport, session, local, now);
}

void leave_multiplayer_for_browser(App& app, std::uint64_t now)
{
	(void)sl_open::network::multiplayer_transport_leave(
		app.multiplayer_transport);
	sl_open::network::multiplayer_transport_initialize(
		app.multiplayer_transport);
	sl_open::frontend::multiplayer_frontend_enter(
		app.multiplayer_frontend, now);
	app.frontend_phase = FrontendPhase::multiplayer_frontend;
	app.multiplayer_campaign_load_active = false;
	app.multiplayer_internal_relaunch = false;
	app.multiplayer_report_valid = false;
	app.multiplayer_local_outcome_reported = false;
	app.multiplayer_result_applied = false;
	app.multiplayer_pending_campaign_valid = false;
	app.multiplayer_profile_save_pending = false;
	app.multiplayer_pending_gameplay_valid = false;
	app.multiplayer_network_aborted = false;
	app.multiplayer_mission_presentation =
		sl_open::platform::MultiplayerMissionPresentation::none;
	sl_open::campaign::multiplayer_campaign_abandon(
		app.multiplayer_progression);
	SDL_StopTextInput(app.window);
	if (app.audio.ready
		&& !app.menu_music.active
		&& !sl_open::audio::wav_stream_open(
			app.vfs,
			"music/New_Pensive.wav",
			app.audio.streams[0].source,
			app.audio.streams[0].buffers,
			true,
			app.menu_music))
	{
		SDL_Log("Multiplayer frontend music could not be opened");
	}
	sl_open::audio::apply_stream_gains(app.audio);
}

void leave_multiplayer_for_main_menu(App& app, std::uint64_t now)
{
	(void)sl_open::network::multiplayer_transport_leave(
		app.multiplayer_transport);
	if (app.loadout_ambience_voice >= 0)
	{
		sl_open::audio::fat_stop(
			app.audio,
			static_cast<std::uint32_t>(
				app.loadout_ambience_voice));
		app.loadout_ambience_voice = -1;
	}
	if (app.campaign_cinematic_voice >= 0)
	{
		sl_open::audio::fat_stop(
			app.audio,
			static_cast<std::uint32_t>(
				app.campaign_cinematic_voice));
		app.campaign_cinematic_voice = -1;
	}
	sl_open::audio::fat_bank_close(
		app.vfs, app.campaign_cinematic_sounds);
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	app.campaign_sequence_waiting_audio = false;
	app.campaign_sequence_purpose =
		sl_open::platform::CampaignSequencePurpose::none;
	sl_open::campaign::multiplayer_campaign_abandon(
		app.multiplayer_progression);
	app.multiplayer_campaign_load_active = false;
	app.multiplayer_campaign_save_from_debrief = false;
	app.multiplayer_internal_relaunch = false;
	app.multiplayer_report_valid = false;
	app.multiplayer_local_outcome_reported = false;
	app.multiplayer_result_applied = false;
	app.multiplayer_pending_campaign_valid = false;
	app.multiplayer_profile_save_pending = false;
	app.multiplayer_pending_gameplay_valid = false;
	app.multiplayer_network_aborted = false;
	app.multiplayer_mission_presentation =
		sl_open::platform::MultiplayerMissionPresentation::none;
	SDL_StopTextInput(app.window);
	enter_main_menu(app, now);
}

bool handle_multiplayer_frontend_action(
	App& app,
	const sl_open::frontend::MultiplayerAction& action,
	std::uint64_t now)
{
	switch (action.type)
	{
	case sl_open::frontend::MultiplayerActionType::none:
	case sl_open::frontend::MultiplayerActionType::selection_changed:
		return false;
	case sl_open::frontend::MultiplayerActionType::choose_provider:
		sl_open::network::multiplayer_transport_end_discovery(
			app.multiplayer_transport);
		sl_open::frontend::multiplayer_frontend_open_browser(
			app.multiplayer_frontend, action.provider);
		if (action.provider
			== sl_open::frontend::MultiplayerProvider::local_network)
		{
			(void)sl_open::network::
				multiplayer_transport_begin_discovery(
					app.multiplayer_transport, now);
		}
		if (app.multiplayer_frontend.screen
			== sl_open::frontend::MultiplayerScreen::direct_ip)
		{
			SDL_StartTextInput(app.window);
		}
		else
		{
			SDL_StopTextInput(app.window);
		}
		return false;
	case sl_open::frontend::MultiplayerActionType::launch_gaming_zone:
		if (!SDL_OpenURL("http://www.zone.com/starlancer"))
		{
			SDL_Log(
				"Could not open the StarLancer Gaming Zone URL: %s",
				SDL_GetError());
		}
		return false;
	case sl_open::frontend::MultiplayerActionType::join_session:
		if (const sl_open::network::MultiplayerDiscoverySession* session =
				find_multiplayer_discovery_session(app, action.session);
			session != nullptr)
		{
			(void)join_multiplayer_session(app, *session, now);
		}
		return false;
	case sl_open::frontend::MultiplayerActionType::host_cooperative:
		(void)host_multiplayer_session(
			app,
			sl_open::network::MultiplayerSessionMode::cooperative,
			now);
		return false;
	case sl_open::frontend::MultiplayerActionType::host_deathmatch:
		(void)host_multiplayer_session(
			app,
			sl_open::network::MultiplayerSessionMode::deathmatch,
			now);
		return false;
	case sl_open::frontend::MultiplayerActionType::find_ip_games:
		std::snprintf(
			app.config.multiplayer_address,
			sizeof(app.config.multiplayer_address),
			"%s",
			action.ip_address);
		SDL_StopTextInput(app.window);
		(void)sl_open::network::
			multiplayer_transport_begin_ipv4_discovery(
				app.multiplayer_transport,
				action.ip_address,
				sl_open::network::kMultiplayerDiscoveryPort,
				now);
		return false;
	case sl_open::frontend::MultiplayerActionType::enter_ip_address:
	case sl_open::frontend::MultiplayerActionType::focus_ip_address:
		SDL_StartTextInput(app.window);
		return false;
	case sl_open::frontend::MultiplayerActionType::main_menu:
		sl_open::network::multiplayer_transport_end_discovery(
			app.multiplayer_transport);
		(void)sl_open::network::multiplayer_transport_leave(
			app.multiplayer_transport);
		begin_transition(
			app,
			sl_open::frontend::kMultiplayerBackMovie,
			TransitionDestination::main_menu,
			now);
		return false;
	case sl_open::frontend::MultiplayerActionType::quit:
		return true;
	}
	return false;
}

void update_multiplayer_local_player_name(App& app)
{
	const sl_open::network::MultiplayerLobbyPlayer* current =
		multiplayer_local_lobby_player(app);
	if (current == nullptr || app.multiplayer_callsign[0] == '\0')
	{
		return;
	}
	sl_open::network::MultiplayerLobbyPlayer player = *current;
	std::snprintf(
		player.name, sizeof(player.name), "%s",
		app.multiplayer_callsign);
	if (app.multiplayer_local_profile_valid)
	{
		(void)sl_open::campaign::campaign_callsign_set(
			app.multiplayer_local_profile,
			app.multiplayer_callsign);
	}
	if (app.multiplayer_campaign_valid)
	{
		(void)sl_open::campaign::campaign_callsign_set(
			app.multiplayer_campaign,
			app.multiplayer_callsign);
	}
	const sl_open::network::MultiplayerLobbySnapshot& lobby =
		sl_open::network::multiplayer_transport_lobby(
			app.multiplayer_transport);
	if (lobby.rules.mode
		== sl_open::network::MultiplayerSessionMode::cooperative)
	{
		const sl_open::campaign::CampaignState& personal =
			app.multiplayer_campaign_valid
				? app.multiplayer_campaign
				: app.multiplayer_local_profile;
		(void)synchronize_multiplayer_personal_campaign(
			app,
			personal,
			app.multiplayer_progression.alternate_mission_25);
	}
	(void)sl_open::network::multiplayer_transport_set_local_player(
		app.multiplayer_transport, player);
}

void apply_multiplayer_lobby_text_action(
	App& app,
	const sl_open::frontend::MultiplayerLobbyAction& action)
{
	if (action.type
		!= sl_open::frontend::MultiplayerLobbyActionType::edit_text
		&& action.type
			!= sl_open::frontend::MultiplayerLobbyActionType::backspace_text)
	{
		return;
	}
	const bool append =
		action.type
			== sl_open::frontend::MultiplayerLobbyActionType::edit_text;
	switch (action.text_field)
	{
	case sl_open::frontend::MultiplayerLobbyFocus::callsign:
		if (append)
		{
			(void)append_text(
				app.multiplayer_callsign, action.text);
		}
		else
		{
			(void)backspace_text(app.multiplayer_callsign);
		}
		update_multiplayer_local_player_name(app);
		break;
	case sl_open::frontend::MultiplayerLobbyFocus::session_name:
		if (!sl_open::network::multiplayer_transport_is_host(
				app.multiplayer_transport))
		{
			break;
		}
		{
			char proposed[sl_open::network::kMultiplayerSessionNameBytes];
			std::snprintf(
				proposed,
				sizeof(proposed),
				"%s",
				app.multiplayer_session_name);
			bool changed = false;
			if (append)
			{
				changed = append_text(proposed, action.text);
			}
			else
			{
				changed = backspace_text(proposed);
			}
			if (changed
				&& sl_open::network::multiplayer_transport_set_session_name(
					app.multiplayer_transport,
					proposed))
			{
				std::snprintf(
					app.multiplayer_session_name,
					sizeof(app.multiplayer_session_name),
					"%s",
					proposed);
			}
		}
		break;
	case sl_open::frontend::MultiplayerLobbyFocus::chat:
		if (append)
		{
			(void)append_text(
				app.multiplayer_lobby_chat_entry,
				action.text);
		}
		else
		{
			(void)backspace_text(
				app.multiplayer_lobby_chat_entry);
		}
		break;
	case sl_open::frontend::MultiplayerLobbyFocus::none:
		break;
	}
}

void enter_multiplayer_loadout(App& app, std::uint64_t now);
void enter_multiplayer_debrief(App& app, std::uint64_t now);
void apply_multiplayer_local_result(
	App& app,
	std::uint64_t now);
void maybe_publish_multiplayer_mission_result(App& app);
void finish_multiplayer_campaign_sequence(
	App& app,
	std::uint64_t now);
void finish_multiplayer_award_movie(
	App& app,
	std::uint64_t now);
void route_multiplayer_after_progression(
	App& app,
	std::uint64_t now);
void handle_multiplayer_post_mission_action(
	App& app,
	sl_open::network::MultiplayerPostMissionAction action,
	std::uint64_t now);
bool handle_multiplayer_debrief_action(
	App& app,
	const sl_open::frontend::MultiplayerDebriefAction& action,
	std::uint64_t now);
void launch_multiplayer_mission(
	App& app,
	const sl_open::game::MultiplayerLaunchSnapshot& snapshot);

void begin_instant_action(
	App& app,
	sl_open::game::MissionOrigin origin,
	std::uint64_t now,
	std::uint16_t mission,
	sl_open::game::MissionMode mode);

void enter_multiplayer_campaign_browser(
	App& app,
	sl_open::frontend::SaveLoadMode mode,
	bool from_debrief,
	std::uint64_t now)
{
	sl_open::frontend::campaign_frontend_enter_save_load(
		app.campaign_frontend,
		app.campaign_store,
		app.multiplayer_campaign,
		mode,
		from_debrief,
		now);
	app.multiplayer_campaign_load_active = true;
	app.multiplayer_campaign_save_from_debrief = from_debrief;
	app.frontend_phase = FrontendPhase::multiplayer_campaign_load;
	SDL_StartTextInput(app.window);
}

void leave_multiplayer_campaign_browser(
	App& app,
	std::uint64_t now)
{
	app.multiplayer_campaign_load_active = false;
	SDL_StopTextInput(app.window);
	if (app.multiplayer_campaign_save_from_debrief)
	{
		app.multiplayer_campaign_save_from_debrief = false;
		enter_multiplayer_debrief(app, now);
	}
	else
	{
		enter_multiplayer_lobby(app, now);
	}
}

bool apply_multiplayer_campaign_selection(
	App& app,
	sl_open::frontend::CampaignSelection selection,
	std::uint64_t now)
{
	switch (selection)
	{
	case sl_open::frontend::CampaignSelection::campaign_ready:
	{
		app.multiplayer_campaign_valid = true;
		app.multiplayer_campaign_loaded_save = true;
		app.multiplayer_progression = {};
		(void)sl_open::campaign::campaign_callsign_set(
			app.multiplayer_campaign,
			app.multiplayer_callsign);
		app.multiplayer_local_profile =
			app.multiplayer_campaign;
		app.multiplayer_local_profile_valid =
			app.multiplayer_campaign.id[0] != '\0';
		sl_open::network::MultiplayerLobbyRules rules =
			sl_open::network::multiplayer_transport_lobby(
				app.multiplayer_transport).rules;
		rules.mission = app.multiplayer_campaign.mission;
		if (!sl_open::network::multiplayer_transport_set_rules(
				app.multiplayer_transport, rules)
			|| !synchronize_multiplayer_personal_campaign(
				app, app.multiplayer_campaign)
			|| !publish_multiplayer_coop_selection(app))
		{
			return false;
		}
		sl_open::network::MultiplayerLobbyPlayer local =
			make_multiplayer_local_player(app, true);
		if (const sl_open::network::MultiplayerLobbyPlayer* current =
				multiplayer_local_lobby_player(app);
			current != nullptr)
		{
			local.ready = current->ready;
		}
		(void)sl_open::network::multiplayer_transport_set_local_player(
			app.multiplayer_transport, local);
		leave_multiplayer_campaign_browser(app, now);
		return false;
	}
	case sl_open::frontend::CampaignSelection::save_complete:
	case sl_open::frontend::CampaignSelection::main_menu:
	case sl_open::frontend::CampaignSelection::single_player:
		leave_multiplayer_campaign_browser(app, now);
		return false;
	case sl_open::frontend::CampaignSelection::quit:
		return true;
	case sl_open::frontend::CampaignSelection::none:
	case sl_open::frontend::CampaignSelection::save_load:
	case sl_open::frontend::CampaignSelection::new_campaign:
		return false;
	}
	return false;
}

sl_open::game::MultiplayerCampaignLaunchState
multiplayer_campaign_launch_state(const App& app)
{
	sl_open::game::MultiplayerCampaignLaunchState result;
	result.state = app.multiplayer_campaign;
	const sl_open::network::MultiplayerLobbySnapshot& lobby =
		sl_open::network::multiplayer_transport_lobby(
			app.multiplayer_transport);
	if (lobby.rules.mode
		== sl_open::network::MultiplayerSessionMode::cooperative)
	{
		result.state.mission = lobby.rules.mission;
	}
	result.present = app.multiplayer_campaign_valid;
	result.mission_25_alternate =
		app.multiplayer_progression.alternate_mission_25;
	return result;
}

bool handle_multiplayer_lobby_action(
	App& app,
	const sl_open::frontend::MultiplayerLobbyAction& action,
	std::uint64_t now)
{
	using Action = sl_open::frontend::MultiplayerLobbyActionType;
	switch (action.type)
	{
	case Action::none:
	case Action::select_player:
		return false;
	case Action::focus_callsign:
	case Action::focus_session_name:
	case Action::focus_chat:
		SDL_StartTextInput(app.window);
		return false;
	case Action::edit_text:
	case Action::backspace_text:
		apply_multiplayer_lobby_text_action(app, action);
		return false;
	case Action::send_chat:
		if (sl_open::network::multiplayer_transport_submit_lobby_chat(
				app.multiplayer_transport,
				action.text))
		{
			app.multiplayer_lobby_chat_entry[0] = '\0';
			SDL_StopTextInput(app.window);
		}
		return false;
	case Action::remove_player:
		if (action.player_identity > 0
			&& action.player_identity
				<= sl_open::network::kMultiplayerTransportPlayerCapacity)
		{
			const std::uint8_t slot =
				static_cast<std::uint8_t>(
					action.player_identity - 1);
			if (slot
				!= app.multiplayer_transport.local_lobby_slot)
			{
				(void)sl_open::network::
					multiplayer_transport_host_kick(
						app.multiplayer_transport, slot);
			}
		}
		return false;
	case Action::new_cooperative_game:
		if (!app.multiplayer_local_profile_valid)
		{
			(void)initialize_multiplayer_local_profile(app);
		}
		app.multiplayer_campaign =
			app.multiplayer_local_profile;
		app.multiplayer_campaign.mission = 1;
		(void)sl_open::campaign::campaign_callsign_set(
			app.multiplayer_campaign,
			app.multiplayer_callsign);
		app.multiplayer_campaign_valid = true;
		app.multiplayer_campaign_loaded_save = false;
		app.multiplayer_progression = {};
		{
			sl_open::network::MultiplayerLobbyRules rules =
				sl_open::network::multiplayer_transport_lobby(
					app.multiplayer_transport).rules;
			rules.mission = app.multiplayer_campaign.mission;
			if (!sl_open::network::multiplayer_transport_set_rules(
					app.multiplayer_transport, rules)
				|| !synchronize_multiplayer_personal_campaign(
					app, app.multiplayer_campaign)
				|| !publish_multiplayer_coop_selection(app))
			{
				return false;
			}
		}
		(void)sl_open::network::multiplayer_transport_set_local_player(
			app.multiplayer_transport,
			make_multiplayer_local_player(app, true));
		return false;
	case Action::load_cooperative_game:
		enter_multiplayer_campaign_browser(
			app,
			sl_open::frontend::SaveLoadMode::load,
			false,
			now);
		return false;
		case Action::start_cooperative_game:
			if (app.multiplayer_campaign_valid)
			{
				const sl_open::game::MultiplayerCampaignLaunchState launch =
					multiplayer_campaign_launch_state(app);
				if (synchronize_multiplayer_personal_campaign(
						app,
						launch.state,
						launch.mission_25_alternate)
					&& publish_multiplayer_coop_selection(app))
				{
					(void)sl_open::network::
						multiplayer_transport_begin_prelaunch(
							app.multiplayer_transport,
							launch);
				}
			}
		return false;
	case Action::toggle_ready:
		if (const sl_open::network::MultiplayerLobbyPlayer* current =
				multiplayer_local_lobby_player(app);
			current != nullptr)
		{
			sl_open::network::MultiplayerLobbyPlayer player = *current;
			player.ready = action.value;
			(void)sl_open::network::multiplayer_transport_set_local_player(
				app.multiplayer_transport, player);
		}
		return false;
	case Action::start_deathmatch:
	{
		sl_open::network::MultiplayerLobbyRules rules =
			sl_open::network::multiplayer_transport_lobby(
				app.multiplayer_transport).rules;
		rules.mission = action.mission;
		if (!sl_open::network::multiplayer_transport_set_rules(
				app.multiplayer_transport, rules))
		{
			return false;
		}
		if (const sl_open::network::MultiplayerLobbyPlayer* current =
				multiplayer_local_lobby_player(app);
			current != nullptr)
		{
			sl_open::network::MultiplayerLobbyPlayer player = *current;
			player.ready = true;
			if (!sl_open::network::multiplayer_transport_set_local_player(
					app.multiplayer_transport, player))
			{
				return false;
			}
		}
		(void)sl_open::network::multiplayer_transport_start_game(
			app.multiplayer_transport);
		return false;
	}
	case Action::select_deathmatch_scenario:
	case Action::toggle_ai_turrets:
	case Action::toggle_target_players:
	case Action::toggle_teamplay:
	{
		sl_open::network::MultiplayerLobbyRules rules =
			sl_open::network::multiplayer_transport_lobby(
				app.multiplayer_transport).rules;
		if (action.type == Action::select_deathmatch_scenario)
		{
			rules.mission = action.mission;
			if (!sl_open::frontend::deathmatch_scenario_supports_teamplay(
					action.scenario))
			{
				rules.team_mode = false;
			}
		}
		else if (action.type == Action::toggle_ai_turrets)
		{
			rules.ai_turrets = action.value;
		}
		else if (action.type == Action::toggle_target_players)
		{
			rules.respawn_targetable = action.value;
		}
		else
		{
			rules.team_mode = action.value;
		}
		(void)sl_open::network::multiplayer_transport_set_rules(
			app.multiplayer_transport, rules);
		return false;
	}
	case Action::select_team:
	case Action::select_ship:
		if (const sl_open::network::MultiplayerLobbyPlayer* current =
				multiplayer_local_lobby_player(app);
			current != nullptr)
		{
			sl_open::network::MultiplayerLobbyPlayer player = *current;
			if (action.type == Action::select_team)
			{
				player.team = action.team;
			}
			else
			{
				player.selected_ship = action.selected_ship;
				apply_default_multiplayer_loadout(
					app, player.selected_ship, player.loadout);
			}
			player.ready = false;
			(void)sl_open::network::multiplayer_transport_set_local_player(
				app.multiplayer_transport, player);
		}
		return false;
	case Action::back_to_browser:
		leave_multiplayer_for_browser(app, now);
		return false;
	case Action::main_menu:
		(void)sl_open::network::multiplayer_transport_leave(
			app.multiplayer_transport);
		begin_transition(
			app,
			sl_open::frontend::kMultiplayerBackMovie,
			TransitionDestination::main_menu,
			now);
		return false;
	case Action::quit:
		return true;
	}
	return false;
}

bool path_equal_case_insensitive(const char* left, const char* right)
{
	while (*left != '\0' && *right != '\0')
	{
		const unsigned char a = static_cast<unsigned char>(
			*left == '\\' ? '/' : *left);
		const unsigned char b = static_cast<unsigned char>(
			*right == '\\' ? '/' : *right);
		if (std::tolower(a) != std::tolower(b))
		{
			return false;
		}
		++left;
		++right;
	}
	return *left == *right;
}

bool path_starts_with_case_insensitive(const char* path, const char* prefix)
{
	while (*prefix != '\0')
	{
		if (*path == '\0'
			|| std::tolower(static_cast<unsigned char>(*path))
				!= std::tolower(static_cast<unsigned char>(*prefix)))
		{
			return false;
		}
		++path;
		++prefix;
	}
	return true;
}

bool path_ends_with_case_insensitive(const char* path, const char* suffix)
{
	const std::size_t path_length = std::strlen(path);
	const std::size_t suffix_length = std::strlen(suffix);
	return path_length >= suffix_length
		&& path_equal_case_insensitive(
			path + path_length - suffix_length, suffix);
}

const char* path_basename(const char* path)
{
	const char* result = path;
	for (; *path != '\0'; ++path)
	{
		if (*path == '/' || *path == '\\')
		{
			result = path + 1;
		}
	}
	return result;
}

const sl_open::audio::Cb97Asset* mission_speech_asset(
	const App& app,
	const char* path)
{
	for (const sl_open::audio::Cb97Asset& asset : app.mission_speech_assets)
	{
		if (sl_open::audio::cb97_asset_matches(asset, path))
		{
			return &asset;
		}
	}
	return nullptr;
}

const sl_open::audio::WavAsset* mission_music_asset(
	const App& app,
	const char* path)
{
	for (const sl_open::audio::WavAsset& asset : app.mission_music_assets)
	{
		if (sl_open::audio::wav_asset_matches(asset, path))
		{
			return &asset;
		}
	}
	return nullptr;
}

const sl_open::hud::MovieAsset* mission_movie_asset(
	const App& app,
	const char* path)
{
	for (const sl_open::hud::MovieAsset& asset : app.mission_movie_assets)
	{
		if (sl_open::hud::movie_asset_matches(asset, path))
		{
			return &asset;
		}
	}
	return nullptr;
}

bool load_mission_speech_asset(App& app, const char* path)
{
	if (mission_speech_asset(app, path) != nullptr)
	{
		return true;
	}
	sl_open::audio::Cb97Asset asset;
	if (!sl_open::audio::cb97_asset_load(app.vfs, path, asset))
	{
		return false;
	}
	app.mission_speech_assets.push_back(
		static_cast<sl_open::audio::Cb97Asset&&>(asset));
	return true;
}

bool load_mission_music_asset(App& app, const char* path)
{
	if (mission_music_asset(app, path) != nullptr)
	{
		return true;
	}
	sl_open::audio::WavAsset asset;
	if (!sl_open::audio::wav_asset_load(app.vfs, path, asset))
	{
		return false;
	}
	app.mission_music_assets.push_back(
		static_cast<sl_open::audio::WavAsset&&>(asset));
	return true;
}

bool load_mission_movie_asset(App& app, const char* path)
{
	if (mission_movie_asset(app, path) != nullptr)
	{
		return true;
	}
	sl_open::hud::MovieAsset asset;
	if (!sl_open::hud::movie_asset_load(app.vfs, path, asset))
	{
		return false;
	}
	app.mission_movie_assets.push_back(
		static_cast<sl_open::hud::MovieAsset&&>(asset));
	return true;
}

void load_mission_string_asset(App& app, const char* value)
{
	if (path_ends_with_case_insensitive(value, ".ut"))
	{
		load_mission_speech_asset(app, value);
		return;
	}
	if (path_ends_with_case_insensitive(value, ".fm8"))
	{
		load_mission_movie_asset(app, value);
		return;
	}
	if (path_ends_with_case_insensitive(value, ".wav"))
	{
		char path[128];
		const bool qualified = std::strchr(value, '/') != nullptr
			|| std::strchr(value, '\\') != nullptr;
		const int length = std::snprintf(
			path,
			sizeof(path),
			qualified ? "%s" : "music/%s",
			value);
		if (length > 0 && static_cast<std::size_t>(length) < sizeof(path))
		{
			load_mission_music_asset(app, path);
		}
		return;
	}

	// PlayCommsMovie data commonly omits the FM8 extension. Only an actual
	// pilot archive member is retained, so ordinary mission strings do not
	// become media assets.
	char path[128];
	const int length = std::snprintf(path, sizeof(path), "pilots/%s", value);
	if (length > 0 && static_cast<std::size_t>(length) < sizeof(path))
	{
		load_mission_movie_asset(app, path);
	}
}

void load_mission_script_assets(App& app)
{
	const sl_open::Blob& image = app.mission_session.mission_file.image;
	std::size_t position = 0;
	while (position < image.size)
	{
		while (position < image.size
			&& (image.data[position] < 32 || image.data[position] > 126))
		{
			++position;
		}
		const std::size_t begin = position;
		while (position < image.size
			&& image.data[position] >= 32 && image.data[position] <= 126)
		{
			++position;
		}
		if (position < image.size && image.data[position] == 0
			&& position - begin >= 3)
		{
			load_mission_string_asset(
				app,
				reinterpret_cast<const char*>(image.data + begin));
		}
		++position;
	}
}

void load_pilot_movies(App& app, std::uint16_t pilot, bool all_faces)
{
	const sl_open::assets::PilotPresentationDefinition* definition =
		sl_open::assets::pilot_presentation(pilot);
	if (definition == nullptr)
	{
		return;
	}
	const std::size_t count = all_faces ? 4 : 1;
	for (std::size_t face = 0; face < count; ++face)
	{
		if (definition->movies[face] == nullptr)
		{
			continue;
		}
		char path[128];
		const int length = std::snprintf(
			path, sizeof(path), "pilots/%s", definition->movies[face]);
		if (length > 0 && static_cast<std::size_t>(length) < sizeof(path))
		{
			load_mission_movie_asset(app, path);
		}
	}
}

void load_mission_voice_families(App& app)
{
	std::vector<const char*> prefixes = {
		"moo", "plck_", "plyrkl_", "enmejt_", "trpkl_",
		"npcdth_", "jmp_", "wrp_",
		app.mission_session.mission_runtime.player_pilot_family == 0
			? "mp" : "fp",
	};
	const sl_open::mission::Runtime& runtime =
		app.mission_session.mission_runtime;
	const bool training = runtime.object_factory_mode == 1
		|| (runtime.mission_number >= 30 && runtime.mission_number <= 35);
	if (training)
	{
		prefixes.push_back("trn");
		load_pilot_movies(app, 82, false);
	}
	else
	{
		load_pilot_movies(app, runtime.mission_number > 13 ? 2 : 4, false);
	}

	bool reliant = false;
	bool yamato = false;
	for (std::uint16_t index = 0; index < runtime.object_count; ++index)
	{
		const sl_open::mission::ObjectRecord& object = runtime.objects[index];
		load_pilot_movies(app, object.pilot, true);
		if (const char* prefix = sl_open::mission::player_comms_voice_prefix(
				object.pilot, false))
		{
			prefixes.push_back(prefix);
		}
		if (const char* prefix = sl_open::mission::player_comms_voice_prefix(
				object.pilot, true))
		{
			prefixes.push_back(prefix);
		}
		switch (object.pilot)
		{
		case 0x0a: prefixes.push_back("hs_res_"); break;
		case 0x0f: prefixes.push_back("ip_res_"); break;
		case 0x10: prefixes.push_back("np_res_"); break;
		case 0x32: prefixes.push_back("cm_res_"); break;
		case 0x83: prefixes.push_back("al_res_"); break;
		case 0x85: prefixes.push_back("rd_res_"); break;
		default: break;
		}
		reliant |= object.type == 0x0c;
		yamato |= object.type == 0x0d;
	}
	if (reliant)
	{
		prefixes.push_back("rel");
		load_pilot_movies(app, 60, false);
	}
	if (yamato)
	{
		prefixes.push_back("yam");
		load_pilot_movies(app, 84, false);
	}

	for (std::uint32_t archive_index = 0;
		archive_index < app.vfs.archive_count;
		++archive_index)
	{
		const sl_open::io::Archive& archive = app.vfs.archives[archive_index];
		if (!path_equal_case_insensitive(
				path_basename(archive.path), "msspeech.hog"))
		{
			continue;
		}
		for (std::uint32_t entry_index = 0;
			entry_index < archive.entry_count;
			++entry_index)
		{
			const char* name = archive.names
				+ archive.entries[entry_index].name_offset;
			for (const char* prefix : prefixes)
			{
				if (path_starts_with_case_insensitive(name, prefix))
				{
					load_mission_speech_asset(app, name);
					break;
				}
			}
		}
		break;
	}
}

void load_mission_media(App& app)
{
	app.mission_speech_assets.clear();
	app.mission_music_assets.clear();
	app.mission_movie_assets.clear();
	load_mission_script_assets(app);
	load_mission_voice_families(app);
	const sl_open::mission::PresentationRequest& presentation =
		app.mission_session.mission_runtime.presentation;
	load_pilot_movies(app, presentation.pilot, true);
	if (presentation.movie_path[0] != '\0')
	{
		load_mission_movie_asset(app, presentation.movie_path);
	}
	if (presentation.speech_path[0] != '\0')
	{
		load_mission_speech_asset(app, presentation.speech_path);
	}
	if (presentation.standalone_speech_path[0] != '\0')
	{
		load_mission_speech_asset(app, presentation.standalone_speech_path);
	}
	if (presentation.command_speech_path[0] != '\0')
	{
		load_mission_speech_asset(app, presentation.command_speech_path);
	}
	if (presentation.music_path[0] != '\0')
	{
		load_mission_music_asset(app, presentation.music_path);
	}
	load_mission_movie_asset(app, "pilots/static");

	std::size_t bytes = 0;
	for (const sl_open::audio::Cb97Asset& asset : app.mission_speech_assets)
	{
		bytes += asset.encoded.size;
	}
	for (const sl_open::audio::WavAsset& asset : app.mission_music_assets)
	{
		bytes += asset.file.size;
	}
	for (const sl_open::hud::MovieAsset& asset : app.mission_movie_assets)
	{
		bytes += asset.stream.size;
	}
	sl_open::diagnostics::mission_log(
		"media loaded speech=%u music=%u movies=%u bytes=%zu",
		static_cast<unsigned>(app.mission_speech_assets.size()),
		static_cast<unsigned>(app.mission_music_assets.size()),
		static_cast<unsigned>(app.mission_movie_assets.size()),
		bytes);
}

void start_live_mission(
	App& app,
	const sl_open::game::MissionLaunchRequest& request)
{
	app.pending_mission_request = request;
	app.mission_loading_presented = false;
	app.frontend_phase = FrontendPhase::mission_loading;
	app.transition_holds_frame = false;
	SDL_StopTextInput(app.window);
}

void finish_live_mission_load(App& app)
{
	const std::uint64_t now = SDL_GetTicks();
	const bool session_started = sl_open::game::mission_session_start(
		app.mission_session,
		app.vfs,
		app.game_stats,
		app.pending_mission_request,
		app.width,
		app.height,
		now);
	if (session_started)
	{
		load_mission_media(app);
		if (!sl_open::render::mission_renderer_init(
				app.vfs,
				app.frontend_renderer,
				app.mission_renderer,
				app.mission_session.mission_runtime,
				sl_open::game::world_resolve(
					app.mission_session.world,
					app.mission_session.world.player)->type,
				app.config.graphics_detail,
				app.mission_session.world.random_seed,
				static_cast<std::uint32_t>(
					app.mission_session.clock.gameplay_tick)))
		{
			sl_open::game::mission_session_fail_load(
				app.mission_session,
				"required mission render resources could not be loaded");
		}
		else
		{
			sl_open::render::mission_renderer_initialize_world_components(
				app.mission_renderer,
				app.mission_session.world,
				&app.mission_session.chaff);
			const bool activations_serviced =
				sl_open::game::mission_session_service_object_activations(
					app.mission_session);
			// Launch activation can construct the player's retained cinematic
			// hangar after the authored objects have received their model trees.
			// Finish those runtime-created model trees before the loading frame
			// is allowed to uncover the mission.
			if (activations_serviced)
			{
				sl_open::render::mission_renderer_initialize_world_components(
					app.mission_renderer,
					app.mission_session.world,
					&app.mission_session.chaff);
			}
			// Mission rendering/resource creation is synchronous. Retail
			// snapshots the gameplay clock after this load boundary, so
			// none of its wall time becomes combat catch-up.
			sl_open::game::simulation_clock_reset(
				app.mission_session.clock,
				SDL_GetTicks());
			app.mission_clock_warmup = true;
		}
	}
	if (!app.mission_clock_warmup)
	{
		app.frontend_phase = FrontendPhase::instant_action;
	}
	app.transition_holds_frame = false;
}

bool multiplayer_prelaunch_players_ready(const App& app)
{
	const sl_open::network::MultiplayerLobbySnapshot& lobby =
		sl_open::network::multiplayer_transport_lobby(
			app.multiplayer_transport);
	if (lobby.player_count == 0)
	{
		return false;
	}
	std::uint8_t connected = 0;
	for (const sl_open::network::MultiplayerLobbyPlayer& player :
		lobby.players)
	{
		if (!player.connected)
		{
			continue;
		}
		++connected;
		if (!player.ready)
		{
			return false;
		}
	}
	return connected == lobby.player_count;
}

void service_multiplayer_prelaunch_barrier(App& app)
{
	if (!sl_open::network::multiplayer_transport_is_host(
			app.multiplayer_transport))
	{
		return;
	}
	const sl_open::network::MultiplayerTransportState state =
		sl_open::network::multiplayer_transport_state(
			app.multiplayer_transport);
	if (state
			== sl_open::network::MultiplayerTransportState::hosting_lobby
		&& app.frontend_phase
			== FrontendPhase::multiplayer_launch_wait)
	{
		// A full reliable queue can reject the first post-debrief or
		// mission-25 prelaunch publication. Keep the retail launch barrier
		// live until authority can publish it instead of stranding every
		// participant on the held loadout frame.
		const sl_open::network::MultiplayerLobbySnapshot& lobby =
			sl_open::network::multiplayer_transport_lobby(
				app.multiplayer_transport);
		if (lobby.coop_selection.present
			|| publish_multiplayer_coop_selection(app))
		{
			(void)sl_open::network::multiplayer_transport_begin_prelaunch(
				app.multiplayer_transport,
				multiplayer_campaign_launch_state(app));
		}
		return;
	}
	if (state
			!= sl_open::network::MultiplayerTransportState::host_prelaunch
		|| !multiplayer_prelaunch_players_ready(app))
	{
		return;
	}
	(void)sl_open::network::multiplayer_transport_start_game(
		app.multiplayer_transport);
}

bool submit_multiplayer_prelaunch_loadout(App& app)
{
	const sl_open::network::MultiplayerLobbyPlayer* current =
		multiplayer_local_lobby_player(app);
	if (current == nullptr)
	{
		return false;
	}
	sl_open::network::MultiplayerLobbyPlayer player = *current;
	player.selected_ship = std::clamp<std::int16_t>(
		app.multiplayer_campaign.selected_ship, 0, 11);
	std::copy(
		std::begin(app.multiplayer_campaign.loadout),
		std::end(app.multiplayer_campaign.loadout),
		std::begin(player.loadout));
	if (!sl_open::network::multiplayer_transport_submit_prelaunch_loadout(
			app.multiplayer_transport, player))
	{
		return false;
	}
	app.frontend_phase = FrontendPhase::multiplayer_launch_wait;
	SDL_StopTextInput(app.window);
	service_multiplayer_prelaunch_barrier(app);
	return true;
}

void enter_multiplayer_loadout(App& app, std::uint64_t now)
{
	const sl_open::game::MultiplayerCampaignLaunchState& campaign =
		sl_open::network::multiplayer_transport_prelaunch_campaign(
			app.multiplayer_transport);
	if (!campaign.present)
	{
		return;
	}
	const bool initial_bootstrap =
		!app.multiplayer_progression.checkpoint_valid;
	if (initial_bootstrap)
	{
		app.multiplayer_campaign =
			localize_multiplayer_campaign(app, campaign.state);
	}
	app.multiplayer_campaign_valid = true;
	app.multiplayer_progression.alternate_mission_25 =
		campaign.mission_25_alternate;
	if (!campaign.mission_25_alternate && initial_bootstrap)
	{
		sl_open::campaign::multiplayer_campaign_capture_checkpoint(
			app.multiplayer_progression,
			app.multiplayer_campaign);
	}
	const sl_open::network::MultiplayerLobbyPlayer* local =
		multiplayer_local_lobby_player(app);
	if (local == nullptr)
	{
		return;
	}
	app.multiplayer_campaign.selected_ship =
		std::clamp<std::int16_t>(local->selected_ship, 0, 11);
	std::copy(
		std::begin(local->loadout),
		std::end(local->loadout),
		std::begin(app.multiplayer_campaign.loadout));

	// Mission 25's second leg retains the first leg's player configuration
	// and enters the launch barrier directly; retail does not show the
	// shared loadout a second time.
	if (campaign.mission_25_alternate)
	{
		app.multiplayer_internal_relaunch = true;
		(void)submit_multiplayer_prelaunch_loadout(app);
		return;
	}

	app.multiplayer_internal_relaunch = false;
	if (app.loadout_ambience_voice >= 0)
	{
		sl_open::audio::fat_stop(
			app.audio,
			static_cast<std::uint32_t>(
				app.loadout_ambience_voice));
		app.loadout_ambience_voice = -1;
	}
	sl_open::frontend::loadout_reset(
		app.loadout,
		app.multiplayer_campaign,
		app.loadout_catalog,
		now,
		app.config.graphics_detail);
	app.frontend_phase = FrontendPhase::multiplayer_loadout;
	if (app.loadout_sounds.ready)
	{
		app.loadout_ambience_voice = sl_open::audio::fat_play_auto(
			app.audio,
			app.loadout_sounds,
			0,
			40,
			0,
			64,
			-12);
		play_fat_sample(app, app.loadout_sounds, 3);
	}
	SDL_StopTextInput(app.window);
}

void begin_multiplayer_loadout_exit(App& app, std::uint64_t now)
{
	if (app.loadout.phase != sl_open::frontend::LoadoutPhase::active)
	{
		return;
	}
	sl_open::frontend::loadout_begin_exit(app.loadout, now);
	if (app.loadout.phase != sl_open::frontend::LoadoutPhase::exiting)
	{
		return;
	}
	if (app.loadout_ambience_voice >= 0)
	{
		sl_open::audio::fat_stop(
			app.audio,
			static_cast<std::uint32_t>(
				app.loadout_ambience_voice));
		app.loadout_ambience_voice = -1;
	}
	play_fat_sample(app, app.loadout_sounds, 2);
}

void launch_multiplayer_mission(
	App& app,
	const sl_open::game::MultiplayerLaunchSnapshot& snapshot)
{
	if (!sl_open::game::valid_multiplayer_launch_snapshot(snapshot))
	{
		return;
	}
	sl_open::game::MissionLaunchRequest request;
	request.mission = snapshot.authoritative_mission;
	request.mode = sl_open::game::MissionMode::multiplayer;
	request.origin = sl_open::game::MissionOrigin::multiplayer;
	request.multiplayer = snapshot;
	request.random_seed = snapshot.authoritative_seed;
	request.graphics_detail = app.config.graphics_detail;
	request.default_view = app.config.default_view;
	request.light_maps = app.config.light_maps;
	request.fixed_seed = true;
	request.force_feedback = app.config.force_feedback;
	request.player_pilot_family =
		static_cast<std::uint8_t>(app.campaign.pilot);
	if (!snapshot.deathmatch_mode)
	{
		app.multiplayer_campaign =
			localize_multiplayer_campaign(
				app, snapshot.campaign.state);
		app.multiplayer_campaign_valid = true;
		request.multiplayer.campaign.state =
			app.multiplayer_campaign;
		request.score = snapshot.campaign.state.score;
		request.difficulty = static_cast<std::uint8_t>(
			snapshot.campaign.state.difficulty);
		request.mission_25_alternate =
			snapshot.campaign.mission_25_alternate;
		for (std::size_t index = 0;
			index < sl_open::game::kMissionPersistentVariableCount;
			++index)
		{
			request.persistent_variables[index] =
				std::bit_cast<std::int32_t>(
					snapshot.bootstrap.image.session_state[
						sl_open::game::
							kMultiplayerCampaignVariableSlots[
								index]]);
		}
		if (request.mission >= 1
			&& request.mission <= sl_open::campaign::kMissionCount)
		{
			request.score_events =
				snapshot.campaign.state.mission_score_events[
					request.mission - 1];
		}
	}
	else
	{
		request.difficulty = 0;
	}
	app.multiplayer_local_outcome_reported = false;
	app.multiplayer_result_applied = false;
	app.multiplayer_pending_campaign_valid = false;
	app.multiplayer_profile_save_pending = false;
	app.multiplayer_completed_mission_25_alternate =
		snapshot.campaign.mission_25_alternate;
	app.multiplayer_pending_gameplay_valid = false;
	app.multiplayer_network_aborted = false;
	app.multiplayer_report_valid = false;
	app.multiplayer_mission_presentation =
		sl_open::platform::MultiplayerMissionPresentation::none;
	app.multiplayer_debrief_chat_count = 0;
	app.multiplayer_debrief_chat_entry[0] = '\0';
	sl_open::audio::wav_stream_close(app.vfs, app.menu_music);
	start_live_mission(app, request);
}

void drain_multiplayer_chat(App& app)
{
	const sl_open::network::MultiplayerLobbySnapshot& lobby =
		sl_open::network::multiplayer_transport_lobby(
			app.multiplayer_transport);
	sl_open::network::MultiplayerLobbyChat lobby_message;
	while (sl_open::network::multiplayer_transport_pop_lobby_chat(
		app.multiplayer_transport, lobby_message))
	{
		const char* callsign =
			lobby_message.source_player
					< sl_open::network::
						kMultiplayerTransportPlayerCapacity
				&& lobby.players[
					lobby_message.source_player].connected
				? lobby.players[lobby_message.source_player].name
				: "";
		push_multiplayer_chat_line(
			app.multiplayer_lobby_chat_lines,
			app.multiplayer_lobby_chat_count,
			callsign,
			lobby_message.text);
	}
	sl_open::network::MultiplayerPostMissionChat debrief_message;
	while (sl_open::network::multiplayer_transport_pop_post_mission_chat(
		app.multiplayer_transport, debrief_message))
	{
		const std::uint8_t lobby_slot =
			sl_open::network::
				multiplayer_transport_lobby_slot_for_gameplay_player(
					app.multiplayer_transport,
					debrief_message.source_player);
		const char* callsign =
			lobby_slot
					< sl_open::network::
						kMultiplayerTransportPlayerCapacity
				&& lobby.players[lobby_slot].connected
				? lobby.players[lobby_slot].name
				: "";
		push_multiplayer_chat_line(
			app.multiplayer_debrief_chat_lines,
			app.multiplayer_debrief_chat_count,
			callsign,
			debrief_message.text);
	}
}

void drain_multiplayer_gameplay_inbound(App& app)
{
	if (app.frontend_phase != FrontendPhase::instant_action
		|| app.mission_session.request.origin
			!= sl_open::game::MissionOrigin::multiplayer)
	{
		return;
	}
	sl_open::mission::NetworkOutboundMessage message;
	while (sl_open::network::multiplayer_transport_pop_gameplay(
		app.multiplayer_transport, message))
	{
		if (!sl_open::game::mission_session_receive_network_message(
				app.mission_session, message))
		{
			app.multiplayer_network_aborted = true;
			app.mission_session.mission_runtime.gameplay_state = 9;
			(void)sl_open::game::
				mission_session_begin_network_termination_acknowledgement(
					app.mission_session,
					app.width,
					app.height,
					SDL_GetTicks());
			break;
		}
	}
}

void service_multiplayer_transport(
	App& app,
	std::uint64_t now)
{
	sl_open::network::multiplayer_transport_poll(
		app.multiplayer_transport, now);
	if (app.frontend_phase == FrontendPhase::instant_action
		&& app.mission_session.request.origin
			== sl_open::game::MissionOrigin::multiplayer)
	{
		for (std::uint8_t player = 0;
			player < sl_open::network::
				kMultiplayerTransportPlayerCapacity;
			++player)
		{
			sl_open::network::MultiplayerGameplayPlayerLatency latency;
			if (!sl_open::network::
					multiplayer_transport_gameplay_player_latency(
						app.multiplayer_transport,
						player,
						latency))
			{
				continue;
			}
			sl_open::game::mission_session_update_network_latency(
				app.mission_session,
				player,
				latency.smoothed_one_way_ticks,
				latency.raw_one_way_ticks,
				app.width,
				app.height,
				now);
		}
	}
	drain_multiplayer_chat(app);
	drain_multiplayer_gameplay_inbound(app);

	sl_open::network::MultiplayerTransportEvent event;
	while (sl_open::network::multiplayer_transport_pop_event(
		app.multiplayer_transport, event))
	{
		switch (event.kind)
		{
		case sl_open::network::MultiplayerTransportEventKind::connected:
			enter_multiplayer_lobby(app, now);
			break;
		case sl_open::network::MultiplayerTransportEventKind::prelaunch:
			enter_multiplayer_loadout(app, now);
			break;
		case sl_open::network::MultiplayerTransportEventKind::launch:
		{
			sl_open::game::MultiplayerLaunchSnapshot snapshot;
			if (sl_open::network::multiplayer_transport_launch_snapshot(
					app.multiplayer_transport, snapshot))
			{
				launch_multiplayer_mission(app, snapshot);
			}
			break;
		}
		case sl_open::network::MultiplayerTransportEventKind::mission_result:
			apply_multiplayer_local_result(app, now);
			break;
		case sl_open::network::MultiplayerTransportEventKind::
				player_mission_outcome:
			maybe_publish_multiplayer_mission_result(app);
			break;
		case sl_open::network::MultiplayerTransportEventKind::peer_departed:
			maybe_publish_multiplayer_mission_result(app);
			break;
		case sl_open::network::MultiplayerTransportEventKind::post_mission_action:
			handle_multiplayer_post_mission_action(
				app, event.post_mission_action, now);
			break;
		case sl_open::network::MultiplayerTransportEventKind::authority_changed:
			if (app.frontend_phase == FrontendPhase::instant_action
				&& app.mission_session.request.origin
					== sl_open::game::MissionOrigin::multiplayer)
			{
				const bool local_authority =
					sl_open::network::multiplayer_transport_is_host(
						app.multiplayer_transport);
				const sl_open::mission::NetworkRole runtime_role =
					local_authority
						? sl_open::mission::NetworkRole::host
						: sl_open::mission::NetworkRole::client;
				if (!sl_open::mission::network_update_authority(
						app.mission_session.mission_runtime.network,
						runtime_role,
						event.player))
				{
					app.multiplayer_network_aborted = true;
					app.mission_session.mission_runtime.gameplay_state = 9;
					(void)sl_open::game::
						mission_session_begin_network_termination_acknowledgement(
							app.mission_session,
							app.width,
							app.height,
							now);
					break;
				}
				app.mission_session.request.multiplayer.role =
					local_authority
						? sl_open::game::MultiplayerRole::host
						: sl_open::game::MultiplayerRole::client;
			}
			break;
		case sl_open::network::MultiplayerTransportEventKind::error:
			if (sl_open::network::multiplayer_transport_state(
					app.multiplayer_transport)
					!= sl_open::network::MultiplayerTransportState::error
				&& sl_open::network::multiplayer_transport_state(
					app.multiplayer_transport)
					!= sl_open::network::
						MultiplayerTransportState::disconnected)
			{
				break;
			}
			[[fallthrough]];
		case sl_open::network::MultiplayerTransportEventKind::disconnected:
			if (app.frontend_phase == FrontendPhase::instant_action
				&& app.mission_session.request.origin
					== sl_open::game::MissionOrigin::multiplayer)
			{
				app.multiplayer_network_aborted = true;
				app.mission_session.mission_runtime.gameplay_state = 9;
				(void)sl_open::game::
					mission_session_begin_network_termination_acknowledgement(
						app.mission_session,
						app.width,
						app.height,
						now);
			}
			else
			{
				const bool active_cooperative_flow =
					app.frontend_phase
							== FrontendPhase::multiplayer_loadout
						|| app.frontend_phase
							== FrontendPhase::multiplayer_launch_wait
						|| app.frontend_phase
							== FrontendPhase::multiplayer_debrief
						|| (app.frontend_phase
								== FrontendPhase::
									multiplayer_campaign_load
							&& app.multiplayer_campaign_save_from_debrief)
						|| app.multiplayer_mission_presentation
							!= sl_open::platform::
								MultiplayerMissionPresentation::none;
				if (active_cooperative_flow)
				{
					leave_multiplayer_for_main_menu(app, now);
				}
				else
				{
					leave_multiplayer_for_browser(app, now);
				}
			}
			break;
		case sl_open::network::MultiplayerTransportEventKind::none:
		case sl_open::network::MultiplayerTransportEventKind::discovery_updated:
		case sl_open::network::MultiplayerTransportEventKind::lobby_updated:
		case sl_open::network::MultiplayerTransportEventKind::lobby_chat_available:
		case sl_open::network::MultiplayerTransportEventKind::peer_joined:
		case sl_open::network::MultiplayerTransportEventKind::gameplay_available:
		case sl_open::network::MultiplayerTransportEventKind::post_mission_updated:
		case sl_open::network::MultiplayerTransportEventKind::post_mission_chat_available:
			break;
		}
	}
	if (app.frontend_phase == FrontendPhase::multiplayer_debrief
		&& sl_open::network::multiplayer_transport_state(
				app.multiplayer_transport)
			== sl_open::network::MultiplayerTransportState::gameplay)
	{
		(void)sl_open::network::multiplayer_transport_enter_post_mission(
			app.multiplayer_transport);
	}
}

void flush_multiplayer_gameplay_outbound(App& app)
{
	if (app.frontend_phase != FrontendPhase::instant_action
		|| app.mission_session.request.origin
			!= sl_open::game::MissionOrigin::multiplayer)
	{
		return;
	}
	if (app.multiplayer_pending_gameplay_valid)
	{
		if (!sl_open::network::multiplayer_transport_gameplay_writable(
				app.multiplayer_transport,
				app.multiplayer_pending_gameplay)
			|| !sl_open::network::multiplayer_transport_submit_gameplay(
				app.multiplayer_transport,
				app.multiplayer_pending_gameplay))
		{
			return;
		}
		app.multiplayer_pending_gameplay_valid = false;
	}
	sl_open::mission::NetworkOutboundMessage message;
	while (sl_open::game::mission_session_pop_network_outbound(
		app.mission_session, message))
	{
		if (!sl_open::network::multiplayer_transport_gameplay_writable(
				app.multiplayer_transport, message)
			|| !sl_open::network::multiplayer_transport_submit_gameplay(
				app.multiplayer_transport, message))
		{
			app.multiplayer_pending_gameplay = message;
			app.multiplayer_pending_gameplay_valid = true;
			break;
		}
	}
}

void destroy_mission_sound_runtime(App& app)
{
	sl_open::audio::sound3d_destroy_all(
		app.audio,
		app.sound3d,
		publish_mission_sound_owner_slot,
		&app);
}

std::uint32_t parse_positive_u32(const char* text)
{
	if (text == nullptr || *text == '\0')
	{
		return 0;
	}

	std::uint64_t value = 0;
	for (const char* it = text; *it != '\0'; ++it)
	{
		if (*it < '0' || *it > '9')
		{
			return 0;
		}
		value = value * 10 + static_cast<unsigned>(*it - '0');
		if (value > UINT32_MAX)
		{
			return 0;
		}
	}
	return static_cast<std::uint32_t>(value);
}

void parse_arguments(App& app, int argc, char** argv)
{
	for (int index = 1; index < argc; ++index)
	{
		if (std::strcmp(argv[index], "--data") == 0 && index + 1 < argc)
		{
			app.requested_data_root = argv[++index];
		}
		else if (std::strncmp(argv[index], "--data=", 7) == 0)
		{
			app.requested_data_root = argv[index] + 7;
		}
		else if (std::strncmp(argv[index], "--frames=", 9) == 0)
		{
			app.frame_limit = parse_positive_u32(argv[index] + 9);
		}
		else if (std::strncmp(argv[index], "--mission=", 10) == 0)
		{
			const std::uint32_t mission =
				parse_positive_u32(argv[index] + 10);
			if (mission <= UINT16_MAX)
			{
				app.requested_mission =
					static_cast<std::uint16_t>(mission);
			}
		}
		else if (std::strcmp(argv[index], "--loose-first") == 0)
		{
			app.loose_first = true;
		}
	}
}

void apply_audio_config(App& app)
{
	app.audio.effects_volume = app.config.effects_volume;
	app.audio.music_volume = app.config.music_volume;
	app.audio.speech_volume = app.config.speech_volume;
	app.audio.master_volume = app.config.master_volume;

	sl_open::audio::SpatialMode mode = sl_open::audio::SpatialMode::Standard;
	switch (app.config.positional_audio)
	{
	case sl_open::PositionalAudio::off:
		mode = sl_open::audio::SpatialMode::Off;
		break;
	case sl_open::PositionalAudio::standard:
		break;
	case sl_open::PositionalAudio::hrtf:
		mode = sl_open::audio::SpatialMode::Hrtf;
		break;
	}
	if (app.audio.spatial_mode != mode
		&& !sl_open::audio::set_spatial_mode(app.audio, mode))
	{
		SDL_Log("Configured positional-audio mode is unavailable; using standard");
		sl_open::audio::set_spatial_mode(app.audio, sl_open::audio::SpatialMode::Standard);
	}
	sl_open::audio::apply_stream_gains(app.audio);
	if (app.mission_music.stream.active)
	{
		sl_open::audio::apply_music_gain(
			app.audio, app.mission_music.requested_volume);
	}
	sl_open::audio::sound3d_refresh_gains(app.audio, app.sound3d);
}

void open_campaign_induction_media(App& app, std::uint64_t now)
{
	const char* path =
		sl_open::frontend::campaign_induction_movie(app.campaign_induction);
	if (path == nullptr)
	{
		enter_campaign_hub(app, 10, now);
		return;
	}

	const sl_open::audio::Stream& movie_stream = app.audio.streams[2];
	if (!sl_open::media::bink_movie_open(
			app.vfs,
			path,
			app.audio.ready ? movie_stream.source : 0,
			movie_stream.buffers,
			now,
			app.transition_movie))
	{
		SDL_Log("Reliant induction movie could not be opened: %s", path);
		if (sl_open::frontend::campaign_induction_movie_finished(
				app.campaign_induction))
		{
			open_campaign_induction_media(app, now);
		}
		else
		{
			enter_campaign_hub(app, 10, now);
		}
		return;
	}

	app.induction_narration_started = false;
	if (app.campaign_induction.phase
		== sl_open::frontend::InductionPhase::narration)
	{
		sl_open::media::bink_movie_set_looping(app.transition_movie, true);
		const char* narration =
			sl_open::frontend::campaign_induction_narration(
				app.campaign_induction);
		const sl_open::audio::Stream& speech_stream = app.audio.streams[1];
		app.induction_narration_started =
			app.audio.ready
			&& sl_open::audio::cb97_stream_open(
				app.vfs,
				narration,
				speech_stream.source,
				speech_stream.buffers,
				false,
				app.campaign_speech);
		if (!app.induction_narration_started)
		{
			SDL_Log(
				"Reliant induction narration could not be opened: %s",
				narration);
		}
		sl_open::audio::apply_stream_gains(app.audio);
	}
}

void start_campaign_induction(App& app, std::uint64_t now)
{
	sl_open::audio::wav_stream_close(app.vfs, app.menu_music);
	sl_open::audio::cb97_stream_close(app.vfs, app.campaign_speech);
	sl_open::frontend::campaign_induction_reset(app.campaign_induction);
	app.frontend_phase = FrontendPhase::campaign_induction;
	app.transition_holds_frame = false;
	SDL_StopTextInput(app.window);
	open_campaign_induction_media(app, now);
}

void finish_induction_narration(App& app, std::uint64_t now)
{
	sl_open::audio::cb97_stream_close(app.vfs, app.campaign_speech);
	app.induction_narration_started = false;
	sl_open::frontend::campaign_induction_narration_finished(
		app.campaign_induction);
	open_campaign_induction_media(app, now);
}

void enter_hub_node(App& app, std::uint8_t node_index, std::uint64_t now)
{
	sl_open::frontend::campaign_hub_enter(app.campaign_hub, node_index);
	const sl_open::frontend::HubNode& node =
		sl_open::frontend::campaign_hub_node(app.campaign_hub);
	start_hub_actor(app, node.primary_movie);
	char path[sl_open::io::kMaxPath];
	std::snprintf(path, sizeof(path), "vr/%s", node.primary_movie);
	const sl_open::audio::Stream& stream = app.audio.streams[2];
	if (!sl_open::media::bink_movie_open(
		app.vfs,
		path,
		app.audio.ready ? stream.source : 0,
		stream.buffers,
		now,
		app.campaign_movie))
	{
		SDL_Log("Campaign hub movie could not be opened: %s", path);
	}
	app.frontend_phase = FrontendPhase::campaign_hub;
}

void enter_in_game_options(App& app, std::uint64_t now)
{
	app.options_return_phase = app.frontend_phase;
	sl_open::media::bink_movie_pause(app.campaign_movie, now);
	app.options.in_game = true;
	sl_open::frontend::options_enter_page(
		app.options,
		sl_open::frontend::OptionsPage::hub,
		app.config,
		now);
	app.frontend_phase = FrontendPhase::options;
	app.transition_holds_frame = false;
}

void return_to_instant_action_pause(App& app)
{
	app.options.in_game = false;
	app.options.modal = sl_open::frontend::OptionsModal::none;
	app.options.hovered = -1;
	app.frontend_phase = FrontendPhase::instant_action;
	app.transition_holds_frame = false;
}

void enter_instant_action_option(
	App& app,
	sl_open::frontend::OptionsPage page,
	std::uint64_t now)
{
	app.options_return_phase = FrontendPhase::instant_action;
	app.options.in_game = true;
	sl_open::frontend::options_enter_page(
		app.options, page, app.config, now);
	app.frontend_phase = FrontendPhase::options;
	app.transition_holds_frame = false;
}

void resume_in_game_options_owner(App& app, std::uint64_t now)
{
	app.options.in_game = false;
	app.options.modal = sl_open::frontend::OptionsModal::none;
	app.options.hovered = -1;
	app.frontend_phase = app.options_return_phase;
	app.transition_holds_frame = false;
	sl_open::media::bink_movie_resume(app.campaign_movie, now);
}

void stop_in_game_options_owner(App& app)
{
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	if (app.options_return_phase == FrontendPhase::campaign_hub)
	{
		sl_open::audio::mp3_stream_close(app.vfs, app.hub_actor_sound);
		stop_hub_ambience(app);
	}
	else if (app.options_return_phase == FrontendPhase::campaign_briefing)
	{
		stop_briefing_voice(app, app.briefing_wait_voice);
		stop_briefing_room_chatter(app);
		sl_open::audio::cb97_stream_close(app.vfs, app.campaign_speech);
		app.briefing_speech_started = false;
	}
}

void finish_in_game_save_load(
	App& app,
	bool persist_profile,
	std::uint64_t now)
{
	app.campaign_frontend.save_load_in_game = false;
	if (persist_profile)
	{
		sl_open::campaign::campaign_profile_save(
			app.campaign_store, app.campaign);
	}
	stop_in_game_options_owner(app);
	app.options.in_game = false;
	enter_campaign_hub(
		app, app.campaign.mission >= 19 ? 50 : 0, now);
}

void begin_in_game_options_transition(
	App& app,
	const char* path,
	TransitionDestination destination,
	std::uint64_t now)
{
	begin_transition(
		app,
		path,
		destination,
		now,
		false,
		false,
		TransitionAudio::in_game_options);
}

void leave_in_game_options_for_main_menu(App& app, std::uint64_t now)
{
	stop_in_game_options_owner(app);
	begin_in_game_options_transition(
		app,
		"interface/igo2mm.bik",
		TransitionDestination::main_menu,
		now);
	app.options.in_game = false;
}

void advance_mission_briefing_movie(App& app, std::uint64_t now);
void leave_mission_briefing(App& app, std::uint64_t now);
void enter_itac(App& app, bool post_mission, std::uint64_t now);

void open_mission_briefing_movie(App& app, std::uint64_t now)
{
	if (app.mission_briefing.phase
		== sl_open::frontend::MissionBriefingPhase::mission_video)
	{
		stop_briefing_room_chatter(app);
	}
	const char* path =
		sl_open::frontend::mission_briefing_movie(app.mission_briefing);
	if (path == nullptr)
	{
		if (app.mission_briefing.mission == 29
			&& app.mission_briefing.phase
				== sl_open::frontend::MissionBriefingPhase::mission_video
			&& !app.briefing_speech_started)
		{
			const sl_open::audio::Stream& stream = app.audio.streams[2];
			app.briefing_speech_started =
				sl_open::audio::cb97_stream_open(
					app.vfs,
					"enddebriefing.ut",
					app.audio.ready ? stream.source : 0,
					stream.buffers,
					false,
					app.campaign_speech,
					sl_open::audio::speech_gain(app.audio));
			if (!app.briefing_speech_started)
			{
				SDL_Log("End debriefing speech could not be opened");
				advance_mission_briefing_movie(app, now);
			}
		}
		return;
	}
	const sl_open::audio::Stream& stream = app.audio.streams[2];
	if (sl_open::media::bink_movie_open(
			app.vfs,
			path,
			app.audio.ready ? stream.source : 0,
			stream.buffers,
			now,
			app.campaign_movie))
	{
		sl_open::media::bink_movie_set_chroma_key(
			app.campaign_movie,
			app.mission_briefing.phase
				== sl_open::frontend::MissionBriefingPhase::mission_video);
	}
	else
	{
		SDL_Log("Mission briefing movie could not be opened: %s", path);
		advance_mission_briefing_movie(app, now);
	}
}

void enter_mission_briefing(App& app, std::uint64_t now)
{
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	sl_open::audio::mp3_stream_close(app.vfs, app.hub_actor_sound);
	stop_hub_ambience(app);
	stop_briefing_voice(app, app.briefing_wait_voice);
	stop_briefing_room_chatter(app);
	sl_open::audio::cb97_stream_close(app.vfs, app.campaign_speech);
	app.briefing_speech_started = false;
	sl_open::frontend::mission_briefing_reset(
		app.mission_briefing,
		static_cast<std::uint8_t>(app.campaign.mission),
		now);
	app.frontend_phase = FrontendPhase::campaign_briefing;
	app.briefing_wait_voice =
		play_fat_sample(app, app.briefing_wait_sounds, 0, 110);
}

void enter_mission_loadout(App& app, std::uint64_t now)
{
	if (app.loadout_ambience_voice >= 0)
	{
		sl_open::audio::fat_stop(
			app.audio,
			static_cast<std::uint32_t>(app.loadout_ambience_voice));
		app.loadout_ambience_voice = -1;
	}
	sl_open::frontend::loadout_reset(
		app.loadout, app.campaign, app.loadout_catalog, now, app.config.graphics_detail);
	app.frontend_phase = FrontendPhase::campaign_loadout;
	if (app.loadout_sounds.ready)
	{
		app.loadout_ambience_voice = sl_open::audio::fat_play_auto(
			app.audio,
			app.loadout_sounds,
			0,
			40,
			0,
			64,
			-12);
		play_fat_sample(app, app.loadout_sounds, 3);
	}
}

void begin_mission_loadout_exit(App& app, std::uint64_t now)
{
	if (app.loadout.phase != sl_open::frontend::LoadoutPhase::active)
	{
		return;
	}
	sl_open::frontend::loadout_begin_exit(app.loadout, now);
	if (app.loadout.phase != sl_open::frontend::LoadoutPhase::exiting)
	{
		return;
	}
	if (app.loadout_ambience_voice >= 0)
	{
		sl_open::audio::fat_stop(
			app.audio,
			static_cast<std::uint32_t>(app.loadout_ambience_voice));
		app.loadout_ambience_voice = -1;
	}
	play_fat_sample(app, app.loadout_sounds, 2);
}

void play_loadout_click_sounds(
	App& app,
	const sl_open::frontend::LoadoutClickResult& result)
{
	if ((result.sounds & sl_open::frontend::loadout_sound_button) != 0)
	{
		play_fat_sample(app, app.loadout_sounds, 1, 40);
	}
	if ((result.sounds & sl_open::frontend::loadout_sound_missile) != 0)
	{
		play_fat_sample(app, app.loadout_sounds, 4, 40);
	}
	if ((result.sounds & sl_open::frontend::loadout_sound_hardpoint) != 0)
	{
		play_fat_sample(app, app.loadout_sounds, 5, 40);
	}
	if ((result.sounds & sl_open::frontend::loadout_sound_ship) != 0)
	{
		play_fat_sample(app, app.loadout_sounds, 8, 40);
	}
	if ((result.sounds & sl_open::frontend::loadout_sound_info_flip) != 0)
	{
		play_fat_sample(app, app.loadout_sounds, 10);
	}
}

void advance_mission_briefing_movie(App& app, std::uint64_t now)
{
	if (app.briefing_speech_started)
	{
		sl_open::audio::cb97_stream_close(app.vfs, app.campaign_speech);
		app.briefing_speech_started = false;
	}
	if (sl_open::frontend::mission_briefing_movie_finished(
		app.mission_briefing, now))
	{
		if (app.mission_briefing.phase
			== sl_open::frontend::MissionBriefingPhase::complete)
		{
			leave_mission_briefing(app, now);
			return;
		}
		if (app.mission_briefing.phase
				== sl_open::frontend::MissionBriefingPhase::hologram
			&& app.mission_briefing.mission != 29)
		{
			enter_mission_loadout(app, now);
			return;
		}
		open_mission_briefing_movie(app, now);
	}
}

void leave_mission_loadout(App& app, std::uint64_t now)
{
	if (!sl_open::frontend::mission_briefing_begin_hologram_exit(
			app.mission_briefing, now))
	{
		return;
	}
	app.frontend_phase = FrontendPhase::campaign_briefing;
	open_mission_briefing_movie(app, now);
}

void launch_campaign_mission(App& app, std::uint64_t now)
{
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	// Gameflow's slot-100 checkpoint is rewritten immediately before every
	// campaign mission load (LANCER.EXE 0x004aa3fc), after briefing/loadout
	// selection and also before mission 251. Keep the in-memory replay image
	// at that same boundary.
	app.mission_checkpoint = app.campaign;
	app.mission_checkpoint_valid = true;
	if (!sl_open::campaign::campaign_save_slot(
			app.campaign_store,
			app.campaign,
			sl_open::campaign::kCheckpointSaveSlot,
			"restart"))
	{
		SDL_Log(
			"Campaign automatic checkpoint failed before mission %u",
			static_cast<unsigned>(app.campaign.mission));
	}
	begin_instant_action(
		app,
		sl_open::game::MissionOrigin::campaign,
		now,
		app.campaign.mission,
		sl_open::game::MissionMode::campaign);
}

void enter_mission_takeoff(App& app, std::uint64_t now)
{
	static constexpr const char* kEarlyMovies[] = {
		"r_h_ta.bik", "r_h_tb.bik", "r_h_tc.bik"};
	static constexpr const char* kLateMovies[] = {
		"y_h_ta.bik", "y_h_tb.bik", "y_h_tc.bik"};

	app.takeoff_movie_index =
		static_cast<std::uint8_t>((app.takeoff_movie_index + 1) % 3);
	const char* movie = app.campaign.mission <= 18
		? kEarlyMovies[app.takeoff_movie_index]
		: kLateMovies[app.takeoff_movie_index];
	const sl_open::audio::Stream& stream = app.audio.streams[2];
	if (!sl_open::media::bink_movie_open(
			app.vfs,
			movie,
			app.audio.ready ? stream.source : 0,
			stream.buffers,
			now,
			app.campaign_movie))
	{
		SDL_Log("Mission takeoff movie could not be opened: %s", movie);
		launch_campaign_mission(app, now);
		return;
	}
	app.frontend_phase = FrontendPhase::campaign_takeoff_movie;
}

void stop_campaign_sequence_audio(App& app)
{
	if (app.campaign_cinematic_voice >= 0)
	{
		sl_open::audio::fat_stop(
			app.audio,
			static_cast<std::uint32_t>(app.campaign_cinematic_voice));
		app.campaign_cinematic_voice = -1;
	}
	sl_open::audio::fat_bank_close(app.vfs, app.campaign_cinematic_sounds);
	app.campaign_sequence_waiting_audio = false;
}

void finish_campaign_sequence(App& app, std::uint64_t now);
void enter_post_mission_choice(App& app, std::uint64_t now);
void enter_post_mission_failure(
	App& app,
	MissionFailureKind failure,
	std::uint64_t now);

void open_campaign_sequence_movie(App& app, std::uint64_t now)
{
	if (app.campaign_sequence_index >= app.campaign_sequence.count)
	{
		finish_campaign_sequence(app, now);
		return;
	}
	if (app.campaign_sequence.audio_bank != nullptr
		&& app.campaign_sequence_index
			== app.campaign_sequence.audio_movie_index
		&& app.campaign_cinematic_voice < 0)
	{
		app.campaign_cinematic_voice = play_fat_sample(
			app,
			app.campaign_cinematic_sounds,
			app.campaign_sequence.audio_sample,
			64);
	}

	const char* movie =
		app.campaign_sequence.movies[app.campaign_sequence_index];
	const sl_open::audio::Stream& stream = app.audio.streams[2];
	if (!sl_open::media::bink_movie_open(
			app.vfs,
			movie,
			app.audio.ready ? stream.source : 0,
			stream.buffers,
			now,
			app.campaign_movie))
	{
		SDL_Log("Campaign sequence movie could not be opened: %s", movie);
		++app.campaign_sequence_index;
		open_campaign_sequence_movie(app, now);
		return;
	}
	app.frontend_phase = FrontendPhase::campaign_post_mission_movie;
}

void begin_campaign_sequence(
	App& app,
	const sl_open::campaign::CampaignMovieSequence& sequence,
	sl_open::platform::CampaignSequencePurpose purpose,
	std::uint64_t now)
{
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	stop_campaign_sequence_audio(app);
	app.campaign_sequence = sequence;
	app.campaign_sequence_purpose = purpose;
	app.campaign_sequence_index = 0;
	if (sequence.audio_bank != nullptr
		&& sl_open::audio::fat_bank_open(
			app.vfs, sequence.audio_bank, app.campaign_cinematic_sounds)
		&& !app.campaign_cinematic_sounds.ready)
	{
		app.campaign_sequence_waiting_audio = true;
		app.frontend_phase = FrontendPhase::campaign_post_mission_movie;
		return;
	}
	open_campaign_sequence_movie(app, now);
}

void start_post_mission(
	App& app,
	sl_open::campaign::MissionGrade grade,
	std::int32_t score_delta,
	std::uint16_t score_events,
	sl_open::campaign::MissionCoordinatorResult result,
	std::uint64_t now)
{
	app.pending_mission_grade = grade;
	if (app.campaign.mission == 25 && app.alternate_mission_25)
	{
		// Mission 25A's score has already been retained as the live starting
		// score for mission 251. The second session therefore contributes
		// only its score delta while reporting the absolute two-leg event
		// total.
		app.pending_score_delta = score_delta;
		app.pending_score_events = score_events;
	}
	else
	{
		app.pending_score_delta = score_delta;
		app.pending_score_events = score_events;
	}
	app.pending_mission_result = result;
	const bool bypass_landing =
		result == sl_open::campaign::MissionCoordinatorResult::executed
		|| result
			== sl_open::campaign::MissionCoordinatorResult::cooperative_transfer;
	begin_campaign_sequence(
		app,
		bypass_landing
			? sl_open::campaign::CampaignMovieSequence{}
			: sl_open::campaign::campaign_post_mission_sequence(
				app.campaign,
				app.campaign.mission,
				grade,
				app.alternate_mission_25),
		sl_open::platform::CampaignSequencePurpose::post_mission,
		now);
}

void start_final_campaign_sequence(App& app, std::uint64_t now)
{
	begin_campaign_sequence(
		app,
		sl_open::campaign::campaign_final_sequence(app.campaign),
		sl_open::platform::CampaignSequencePurpose::final_chapter,
		now);
}

void start_campaign_transfer(App& app, std::uint64_t now)
{
	sl_open::campaign::CampaignMovieSequence transfer;
	transfer.movies[0] = sl_open::campaign::campaign_transfer_movie(
		app.campaign, app.campaign.mission);
	transfer.count = 1;
	begin_campaign_sequence(
		app,
		transfer,
		sl_open::platform::CampaignSequencePurpose::transfer_exit,
		now);
}

void start_campaign_retry_exhausted(App& app, std::uint64_t now)
{
	sl_open::campaign::CampaignMovieSequence transfer;
	transfer.movies[0] =
		sl_open::campaign::campaign_ejection_exhausted_movie(
			app.campaign.mission);
	transfer.count = 1;
	begin_campaign_sequence(
		app,
		transfer,
		sl_open::platform::CampaignSequencePurpose::transfer_exit,
		now);
}

void apply_pending_live_mission_score(App& app)
{
	app.campaign.score = std::bit_cast<std::int32_t>(
		static_cast<std::uint32_t>(app.campaign.score)
		+ static_cast<std::uint32_t>(app.pending_score_delta));
	app.pending_score_delta = 0;
	if (app.campaign.mission < 1
		|| app.campaign.mission > sl_open::campaign::kMissionCount)
	{
		app.pending_score_events = 0;
		return;
	}
	const std::uint32_t mission_index = app.campaign.mission - 1;
	// The mission runtime reports the live absolute total. Mission 251 is
	// initialized from mission 25A's retained value and continues it.
	app.campaign.mission_score_events[mission_index] =
		app.pending_score_events;
	app.pending_score_events =
		app.campaign.mission_score_events[mission_index];
}

void finish_credits(App& app, std::uint64_t now)
{
	sl_open::audio::wav_stream_close(app.vfs, app.credits_music);
	if (app.multiplayer_mission_presentation
		== sl_open::platform::MultiplayerMissionPresentation::
			final_chapter)
	{
		app.multiplayer_campaign.mission = 1;
		app.multiplayer_mission_presentation =
			sl_open::platform::MultiplayerMissionPresentation::none;
		sl_open::campaign::multiplayer_campaign_abandon(
			app.multiplayer_progression);
		leave_multiplayer_for_main_menu(app, now);
		return;
	}
	app.campaign.mission = 1;
	app.alternate_mission_25 = false;
	app.mission_checkpoint_valid = false;
	enter_main_menu(app, now);
}

void enter_credits(App& app, std::uint64_t now)
{
	sl_open::frontend::credits_reset(app.credits, now);
	app.frontend_phase = FrontendPhase::credits;
	if (app.audio.ready
		&& !sl_open::audio::wav_stream_open(
			app.vfs,
			"music/new_sim07.wav",
			app.audio.streams[0].source,
			app.audio.streams[0].buffers,
			false,
			app.credits_music))
	{
		SDL_Log("Credits music could not be opened");
	}
	sl_open::audio::apply_stream_gains(app.audio);
}

void leave_mission_briefing(App& app, std::uint64_t now)
{
	stop_briefing_voice(app, app.briefing_wait_voice);
	stop_briefing_room_chatter(app);
	sl_open::audio::cb97_stream_close(app.vfs, app.campaign_speech);
	app.briefing_speech_started = false;
	if (app.campaign.mission == 29)
	{
		start_final_campaign_sequence(app, now);
		return;
	}
	enter_mission_takeoff(app, now);
}

void compose_automatic_campaign_save_name(
	const App& app,
	std::uint16_t mission,
	char (&name)[sl_open::campaign::kSaveNameBytes])
{
	// FUN_00475a90 uses this display-number table for the visible GAME00
	// description after single-player progression.
	static constexpr std::uint8_t kMissionDisplayNumbers[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
		10, 11, 12, 13, 12, 13, 14, 17, 15, 16,
		17, 18, 22, 19, 20, 21, 22, 23, 24, 0,
	};
	const std::uint8_t display_mission =
		mission < std::size(kMissionDisplayNumbers)
			? kMissionDisplayNumbers[mission]
			: 0;
	std::snprintf(
		name,
		sizeof(name),
		"%s%u",
		sl_open::language_text(app.language, 0x18a),
		static_cast<unsigned>(display_mission));
}

void route_after_mission(App& app, std::uint64_t now)
{
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	if (app.campaign_profile_save_pending)
	{
		app.campaign_profile_save_pending = false;
		// FUN_00475a90 writes visible GAME00 after progression/award
		// presentation and before profile.bin (0x00475bf6..0x00475cc8).
		// The table maps the active next mission to its retail display
		// number; Mission 28 returns before this entire save boundary.
		char automatic_name[sl_open::campaign::kSaveNameBytes];
		compose_automatic_campaign_save_name(
			app, app.campaign.mission, automatic_name);
		if (!sl_open::campaign::campaign_save_slot(
				app.campaign_store,
				app.campaign,
				0,
				automatic_name))
		{
			SDL_Log(
				"Campaign GAME00 save failed after mission %u",
				static_cast<unsigned>(
					app.pending_advance.completed_mission));
		}
		if (!sl_open::campaign::campaign_profile_save(
			app.campaign_store, app.campaign))
		{
			SDL_Log(
				"Campaign profile save failed after mission %u",
				static_cast<unsigned>(
					app.pending_advance.completed_mission));
		}
	}
	if (app.pending_mission_result
		== sl_open::campaign::MissionCoordinatorResult::transfer)
	{
		start_campaign_transfer(app, now);
		return;
	}
	if (app.pending_advance.campaign_complete)
	{
		app.itac_post_mission = false;
		enter_mission_briefing(app, now);
		return;
	}
	prepare_campaign_hub(app);
	app.itac_post_mission = true;
	enter_itac(app, true, now);
}

void apply_campaign_mission_result(
	App& app,
	sl_open::campaign::MissionGrade grade,
	std::int32_t score_delta,
	std::uint16_t score_events,
	std::uint64_t now)
{
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	app.pending_advance = sl_open::campaign::campaign_apply_mission_result(
		app.campaign,
		grade,
		score_delta,
		score_events,
		app.pending_mission_result);
	app.alternate_mission_25 = false;
	app.mission_checkpoint_valid = false;
	// FUN_00475a90 persists the profile only after any newly-earned medal
	// movie returns (0x00475b75..0x00475cc8). Mission 28 returns early at
	// 0x00475cd4 and deliberately leaves its mission-28 profile intact.
	app.campaign_profile_save_pending =
		!app.pending_advance.campaign_complete;
	// Mission 28 enters terminal state 29 immediately after campaign
	// postprocessing and bypasses the ordinary post-mission ITAC visit.
	if (app.pending_advance.campaign_complete)
	{
		route_after_mission(app, now);
		return;
	}
	const char* award_movie =
		sl_open::campaign::campaign_medal_movie(app.pending_advance.new_medal);
	if (award_movie == nullptr)
	{
		route_after_mission(app, now);
		return;
	}
	const sl_open::audio::Stream& stream = app.audio.streams[2];
	if (!sl_open::media::bink_movie_open(
		app.vfs,
		award_movie,
		app.audio.ready ? stream.source : 0,
		stream.buffers,
		now,
		app.campaign_movie))
	{
		SDL_Log("Campaign award movie could not be opened: %s", award_movie);
		route_after_mission(app, now);
		return;
	}
	app.frontend_phase = FrontendPhase::campaign_award_movie;
}

void finish_campaign_sequence(App& app, std::uint64_t now)
{
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	stop_campaign_sequence_audio(app);
	const sl_open::platform::CampaignSequencePurpose purpose =
		app.campaign_sequence_purpose;
	app.campaign_sequence_purpose =
		sl_open::platform::CampaignSequencePurpose::none;

	if (purpose == sl_open::platform::CampaignSequencePurpose::transfer_exit)
	{
		app.alternate_mission_25 = false;
		app.mission_checkpoint_valid = false;
		enter_main_menu(app, now);
		return;
	}
	if (purpose == sl_open::platform::CampaignSequencePurpose::final_chapter)
	{
		enter_credits(app, now);
		return;
	}
	if (purpose
		== sl_open::platform::CampaignSequencePurpose::
			multiplayer_post_mission)
	{
		finish_multiplayer_campaign_sequence(app, now);
		return;
	}
	if (purpose != sl_open::platform::CampaignSequencePurpose::post_mission)
	{
		return;
	}

	if (app.campaign_sequence.set_chapter2_thread3_seen)
	{
		app.campaign.branch_variables[
			sl_open::campaign::branch_0052a478] = 1;
	}
	if (app.pending_mission_result
		== sl_open::campaign::MissionCoordinatorResult::retry
		&& !sl_open::campaign::campaign_retry(app.campaign))
	{
		start_campaign_retry_exhausted(app, now);
		return;
	}
	if (app.pending_mission_result
		== sl_open::campaign::MissionCoordinatorResult::executed)
	{
		enter_post_mission_failure(
			app, MissionFailureKind::executed, now);
		return;
	}
	if (app.pending_mission_grade == sl_open::campaign::MissionGrade::none)
	{
		apply_pending_live_mission_score(app);
		start_campaign_transfer(app, now);
		return;
	}
	if (app.campaign.mission == 25 && !app.alternate_mission_25)
	{
		apply_pending_live_mission_score(app);
		app.alternate_mission_25 = true;
		enter_mission_takeoff(app, now);
		return;
	}
	apply_campaign_mission_result(
		app,
		app.pending_mission_grade,
		app.pending_score_delta,
		app.pending_score_events,
		now);
}

void enter_post_mission_choice(App& app, std::uint64_t now)
{
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	// The ordinary destroyed/captured/exit join clears DAT_00587cdc at
	// 0x004aa750 before opening FUN_0043eb80. Coordinator six reaches the
	// same dialog through 0x004aa52f and deliberately preserves it.
	if (app.pending_mission_result
		!= sl_open::campaign::MissionCoordinatorResult::executed)
	{
		app.alternate_mission_25 = false;
	}
	sl_open::frontend::post_mission_choice_reset(
		app.post_mission_choice, now);
	app.frontend_phase = FrontendPhase::campaign_restart_choice;
}

void enter_post_mission_failure(
	App& app,
	MissionFailureKind failure,
	std::uint64_t now)
{
	const char* movie = nullptr;
	switch (failure)
	{
	case MissionFailureKind::destroyed:
		movie = app.campaign.mission <= 18
			? "new_funeral.bik"
			: "new_funeral2.bik";
		break;
	case MissionFailureKind::interrupted:
		movie = "int.bik";
		break;
	case MissionFailureKind::executed:
		movie = app.campaign.mission < 19
			? "new_rel_exec.bik"
			: "new_y_exec.bik";
		break;
	}
	const sl_open::audio::Stream& stream = app.audio.streams[2];
	if (movie == nullptr
		|| !sl_open::media::bink_movie_open(
			app.vfs,
			movie,
			app.audio.ready ? stream.source : 0,
			stream.buffers,
			now,
			app.campaign_movie))
	{
		if (movie != nullptr)
		{
			SDL_Log("Post-mission failure movie could not be opened: %s", movie);
		}
		enter_post_mission_choice(app, now);
		return;
	}
	app.frontend_phase = FrontendPhase::campaign_failure_movie;
}

void handle_post_mission_choice(
	App& app,
	sl_open::frontend::PostMissionChoiceAction action,
	std::uint64_t now)
{
	auto restore_checkpoint = [&app](bool preserve_alternate)
	{
		if (!app.mission_checkpoint_valid)
		{
			return;
		}
		const bool alternate_mission_25 =
			app.alternate_mission_25;
		const std::int16_t selected_ship = app.campaign.selected_ship;
		std::int16_t loadout[sl_open::campaign::kLoadoutSlots];
		std::copy(
			std::begin(app.campaign.loadout),
			std::end(app.campaign.loadout),
			std::begin(loadout));
		app.campaign = app.mission_checkpoint;
		app.campaign.selected_ship = selected_ship;
		std::copy(
			std::begin(loadout),
			std::end(loadout),
			std::begin(app.campaign.loadout));
		app.alternate_mission_25 =
			preserve_alternate && alternate_mission_25;
	};
	switch (action)
	{
	case sl_open::frontend::PostMissionChoiceAction::replay_from_briefing:
		restore_checkpoint(false);
		enter_mission_briefing(app, now);
		break;
	case sl_open::frontend::PostMissionChoiceAction::replay_from_launch:
		// Coordinator six reaches this dialog without clearing the live
		// mission-251 selector (0x004aa52f..0x004aa557). Its direct-launch
		// choice therefore preserves that selector; the ordinary
		// destroyed/captured/exit routes clear it before the dialog.
		restore_checkpoint(
			app.pending_mission_result
				== sl_open::campaign::MissionCoordinatorResult::executed);
		// FUN_0043eb80 returns one for the launch replay button at
		// 0x0043ecbd..0x0043ecc9. Its caller jumps directly to
		// 0x004aa3d9, deliberately bypassing both the takeoff movie at
		// 0x004aa3d4 and the slot-100 rewrite at 0x004aa3fc.
		begin_instant_action(
			app,
			sl_open::game::MissionOrigin::campaign,
			now,
			app.campaign.mission,
			sl_open::game::MissionMode::campaign);
		break;
	case sl_open::frontend::PostMissionChoiceAction::main_menu:
		restore_checkpoint(false);
		app.mission_checkpoint_valid = false;
		begin_transition(
			app,
			"interface/sin2main.bik",
			TransitionDestination::main_menu,
			now);
		break;
	case sl_open::frontend::PostMissionChoiceAction::none:
		break;
	}
}

void open_fish_movie(App& app, const char* movie, std::uint64_t now)
{
	char path[sl_open::io::kMaxPath];
	std::snprintf(path, sizeof(path), "vr/%s", movie);
	const sl_open::audio::Stream& stream = app.audio.streams[2];
	if (!sl_open::media::bink_movie_open(
			app.vfs,
			path,
			app.audio.ready ? stream.source : 0,
			stream.buffers,
			now,
			app.campaign_movie))
	{
		SDL_Log("Fish-room movie could not be opened: %s", path);
	}
}

void enter_fish_cycle(App& app, std::uint64_t now)
{
	open_fish_movie(
		app,
		sl_open::frontend::campaign_hub_begin_fish_cycle(app.campaign_hub),
		now);
}

void advance_fish_cycle(App& app, std::uint64_t now)
{
	open_fish_movie(
		app,
		sl_open::frontend::campaign_hub_next_fish_movie(app.campaign_hub),
		now);
}

void open_sim_pod_movie(App& app, std::uint64_t now)
{
	const char* path = sl_open::frontend::sim_pod_movie(app.sim_pod);
	if (path == nullptr)
	{
		return;
	}
	const sl_open::audio::Stream& stream = app.audio.streams[2];
	if (!sl_open::media::bink_movie_open(
			app.vfs,
			path,
			app.audio.ready ? stream.source : 0,
			stream.buffers,
			now,
			app.campaign_movie))
	{
		SDL_Log("SimPod movie could not be opened: %s", path);
		sl_open::frontend::sim_pod_movie_finished(app.sim_pod);
	}
}

void begin_instant_action(
	App& app,
	sl_open::game::MissionOrigin origin,
	std::uint64_t now,
	std::uint16_t mission = 29,
	sl_open::game::MissionMode mode =
		sl_open::game::MissionMode::instant_action)
{
	sl_open::game::MissionLaunchRequest request;
	// Retail's second mission-25 leg keeps mission number 25 and selects
	// row 35 of the mission/HUD tables through a separate live flag. Accept
	// 251 as the direct-launch spelling used by campaign metadata, but do
	// not expose it to gameplay as a fictitious mission number.
	request.mission_25_alternate =
		mission == 251
		|| (mission == 25 && app.alternate_mission_25);
	request.mission = mission == 251 ? 25 : mission;
	request.mode = mode;
	request.origin = origin;
	request.random_seed = static_cast<std::uint32_t>(now);
	request.score = app.campaign.score;
	request.graphics_detail = app.config.graphics_detail;
	request.default_view = app.config.default_view;
	request.difficulty =
		origin == sl_open::game::MissionOrigin::campaign
				|| origin
					== sl_open::game::MissionOrigin::campaign_sim_pod
			? std::min<std::uint8_t>(
				static_cast<std::uint8_t>(
					app.campaign.difficulty),
				2)
			: 1;
	request.light_maps = app.config.light_maps;
	request.fixed_seed = false;
	request.force_feedback = app.config.force_feedback;
	request.player_pilot_family =
		static_cast<std::uint8_t>(app.campaign.pilot);
	// The campaign SimPod invokes gameplay without replacing the active
	// campaign globals. FUN_00475620 resets only the transient mission
	// values, so retained VARS and campaign difficulty remain visible to
	// missions 29-32 and are restored with the frontend snapshot on return.
	if (mode == sl_open::game::MissionMode::campaign
		|| origin == sl_open::game::MissionOrigin::campaign_sim_pod)
	{
		static_assert(
			sl_open::campaign::kBranchVariableCount
				== sl_open::game::kMissionPersistentVariableCount);
		std::copy(
			std::begin(app.campaign.branch_variables),
			std::end(app.campaign.branch_variables),
			std::begin(request.persistent_variables));
		if (request.mission >= 1
			&& request.mission <= sl_open::campaign::kMissionCount)
		{
			request.score_events =
				app.campaign.mission_score_events[
					request.mission - 1];
		}
	}
	if (origin == sl_open::game::MissionOrigin::campaign)
	{
		static_assert(
			sl_open::campaign::kLoadoutSlots
				== sl_open::game::kMissionLoadoutSlots);
		request.campaign_player_configuration = true;
		request.selected_ship = std::clamp<std::int16_t>(
			app.campaign.selected_ship, 0, 11);
		std::copy(
			std::begin(app.campaign.loadout),
			std::end(app.campaign.loadout),
			std::begin(request.player_loadout));
	}
	if (origin == sl_open::game::MissionOrigin::campaign_sim_pod)
	{
		request.sim_pod.page = app.sim_pod.page;
		request.sim_pod.selected = app.sim_pod.hovered;
		request.sim_pod.hub_node = app.campaign_hub.node;
		request.sim_pod.late_campaign = app.campaign_hub.late_campaign;
		sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
		sl_open::audio::mp3_stream_close(app.vfs, app.hub_actor_sound);
		stop_hub_ambience(app);
	}
	else
	{
		sl_open::audio::wav_stream_close(app.vfs, app.menu_music);
	}

	app.instant_action_snapshot_valid =
		origin != sl_open::game::MissionOrigin::campaign;
	if (app.instant_action_snapshot_valid)
	{
		app.instant_action_campaign_snapshot = app.campaign;
	}
	start_live_mission(app, request);
}

const char* mission_pilot_movie(std::uint16_t pilot)
{
	const sl_open::assets::PilotPresentationDefinition* definition =
		sl_open::assets::pilot_presentation(pilot);
	return definition != nullptr ? definition->movies[0] : nullptr;
#if 0
	// PilotPresentationDefinition normal_movie fields, in retail ordinal
	// order (LANCER.EXE 0x005048d8, 194 records of 0x18 bytes).
	static constexpr const char* movies[] = {
		"45TigersWL_Bandit", "45TigersWL_Diceman", "45Tigers_Moose",
		"45Tigers_Plt", "45Volntrs_Moose", "45Volntrs_Plt",
		"45VolntrWL_Bandit", "45VolntrWL_Viper", "51st_Plt",
		"BGUARDP", "BlacksunWL_Plt", "Blacksun_Plt", "Blades_Plt",
		"BlckEgleWL_Plt", "BlckEgle_Plt", "BlckGrdIP_Plt",
		"BlckGrdNP_Plt", "BLKACE", "Bremen_Brdge_Off", "BUCC",
		"Cat_Foster", "COB", "Couger_Plt", "C_comms_off1",
		"C_Fhtr_01", "C_Scientist", "Endevr_Brdge_Off",
		"Enq_RelBridge", "Enq_YamBridge", "Enriquez_Cockpit",
		"FOSTDED", "FrtBear_Comms", "FrtBxtr_Comms", "frtcrtr_comms",
		"frtshrmn_comms", "GAMMADEA", "Gamma_Ldr", "GoldWarr_Plt",
		"HellcatWL", "Hellcat_Plt", "Hornet_Plt", "JAckelWL_Plt",
		"Jackel_Plt", "Jaguar_Plt", "KestrlCmmnd_Comms",
		"Koenig_Brdge_off", "Kstrl_Brge_Off", "kurgen_pil",
		"Lonestar_Plt", "Mauler_Plt", "MCGANNNO", "Mitchel_Brdge_Off",
		"Nanny_Ldr", "PirateWL_Plt", "Pirate_Plt", "Prowler_Plt",
		"Pukov_Cap", "PumaWL_Plt", "Puma_Plt", "Raven_Plt",
		"Rel_Brdge_Off", "Ripper_Plt", "RoninWL_Plt", "Ronin_Plt",
		"RUSSBOMB", "SabreWL_plt_", "Sabre_Plt", "Saracen_Plt",
		"ScorpionWL_Plt", "Scorpion_Plt", "sladin_cap_normal",
		"Stahl_BrdShip", "Stahl_Marines", "Stiener_Ejected",
		"Stinger_Plt", "Stork_Plt", "Ullys_Capt", "ullys_capt_panic",
		"VampireWL_Stiener", "Vampire_Plt", "Varygag_Capt",
		"Victorious_Brdge_Off", "VirtFlt_Ins", "Washngtn_Brdge_Off",
		"Yam_Brdge_Off",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"Capt_Mukai", "Cat_foster_prwlr", "C_TorpWL_Plt",
		"Gen_Reese_Pod", "Kulov_Borodin", "Kulov_Pod",
		"Mammoth_Plt_01", "Mammoth_Plt_02", "Mammoth_Plt_03",
		"Mammoth_Plt_04", "SaracenWL_Plt", "Stahl_RippShip",
		"goldwarrwl_plt", "hornetwl_plt", "mammoth_plt_05",
		"mammoth_plt_06", "mammoth_plt_07",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt", "45Tigers_Plt",
		"45Tigers_Plt",
		"mammoth_plt_01", "mammoth_plt_03", "mammoth_plt_02",
		"mammoth_plt_05", "mammoth_plt_07", "mammoth_plt_06",
		"mammoth_plt_01", "mammoth_plt_02", "mammoth_plt_07",
		"mammoth_plt_05", "mammoth_plt_01", "45Tigers_Plt", "COB",
		"MarauderWL_Plt", "Marauders_Plt", "Marine_Leader",
		"Marine_Combat", "JaguarWL_Plt", "BuccnrsWL_Plt",
		"Pukov_Cap", "Mammoth_Plt_01",
	};
	static_assert(std::size(movies) == sl_open::assets::kPilotStatsCount);
	return pilot < std::size(movies) ? movies[pilot] : nullptr;
#endif
}

void resume_mission_foster_audio(App& app)
{
	constexpr std::uint32_t kFosterPausedStreams[] = {0, 1, 4};
	if (!app.audio.ready)
	{
		app.mission_foster_paused_voices = 0;
		app.mission_foster_paused_streams = 0;
		return;
	}
	for (std::uint32_t index = 0;
		index < sl_open::audio::kVoiceCount;
		++index)
	{
		if ((app.mission_foster_paused_voices
			& (std::uint64_t{1} << index)) != 0)
		{
			alSourcePlay(app.audio.voices[index].source);
		}
	}
	for (const std::uint32_t index : kFosterPausedStreams)
	{
		if ((app.mission_foster_paused_streams
			& (std::uint8_t{1} << index)) != 0)
		{
			alSourcePlay(app.audio.streams[index].source);
		}
	}
	app.mission_foster_paused_voices = 0;
	app.mission_foster_paused_streams = 0;
}

void finish_mission_foster(App& app)
{
	sl_open::media::bink_movie_stop(app.vfs, app.mission_foster_movie);
	resume_mission_foster_audio(app);
	app.mission_foster_active = false;
	// Retail resumes the mission clock after the synchronous movie owner
	// returns. Resetting the wall-clock anchor prevents a catch-up burst.
	app.mission_session.clock.initialized = false;
	sl_open::diagnostics::mission_log("foster movie finished");
}

void start_mission_foster(App& app, std::uint64_t now)
{
	constexpr std::uint32_t kFosterPausedStreams[] = {0, 1, 4};
	app.mission_session.mission_runtime.fosters_last_stand = false;
	if (app.mission_foster_active)
	{
		return;
	}

	app.mission_foster_paused_voices = 0;
	app.mission_foster_paused_streams = 0;
	if (app.audio.ready)
	{
		for (std::uint32_t index = 0;
			index < sl_open::audio::kVoiceCount;
			++index)
		{
			ALint state = AL_STOPPED;
			alGetSourcei(
				app.audio.voices[index].source,
				AL_SOURCE_STATE,
				&state);
			if (state == AL_PLAYING)
			{
				alSourcePause(app.audio.voices[index].source);
				app.mission_foster_paused_voices |=
					std::uint64_t{1} << index;
			}
		}
		for (const std::uint32_t index : kFosterPausedStreams)
		{
			ALint state = AL_STOPPED;
			alGetSourcei(
				app.audio.streams[index].source,
				AL_SOURCE_STATE,
				&state);
			if (state == AL_PLAYING)
			{
				alSourcePause(app.audio.streams[index].source);
				app.mission_foster_paused_streams |=
					std::uint8_t{1} << index;
			}
		}
	}

	const sl_open::audio::Stream& movie_stream = app.audio.streams[2];
	if (!sl_open::media::bink_movie_open(
			app.vfs,
			"foster.bik",
			app.audio.ready ? movie_stream.source : 0,
			movie_stream.buffers,
			now,
			app.mission_foster_movie))
	{
		SDL_Log("Foster's Last Stand movie could not be opened");
		resume_mission_foster_audio(app);
		app.mission_session.clock.initialized = false;
		return;
	}
	app.mission_foster_active = true;
	sl_open::diagnostics::mission_log("foster movie started");
}

void update_mission_presentation(App& app, std::uint64_t now)
{
	sl_open::mission::PresentationRequest& presentation =
		app.mission_session.mission_runtime.presentation;
	sl_open::hud::Movie& movie = app.mission_session.hud.movie;
	const std::uint32_t audio_tick = static_cast<std::uint32_t>(
		app.mission_session.clock.gameplay_tick);
	if (presentation.music_stop_pending)
	{
		presentation.music_stop_pending = false;
		presentation.music_pending = false;
		sl_open::audio::music_close(app.vfs, app.mission_music);
	}
	if (presentation.music_pending)
	{
		presentation.music_pending = false;
		const sl_open::audio::WavAsset* asset =
			mission_music_asset(app, presentation.music_path);
		if (asset == nullptr)
		{
			SDL_Log(
				"Mission music was not loaded: %s",
				presentation.music_path);
		}
		else
		{
			sl_open::audio::music_play(
				app.vfs,
				app.audio,
				app.mission_music,
				*asset,
				0,
				0x50,
				presentation.music_mode,
				audio_tick);
		}
	}
	if (app.mission_session.state
		== sl_open::game::MissionSessionState::paused)
	{
		sl_open::audio::music_pause(app.mission_music);
	}
	else
	{
		sl_open::audio::music_resume(app.mission_music);
	}
	sl_open::audio::music_update(
		app.vfs, app.audio, app.mission_music, audio_tick);
	if (presentation.command_speech_pending)
	{
		presentation.command_speech_pending = false;
		sl_open::audio::cb97_stream_close(
			app.vfs, app.mission_command_speech);
		presentation.command_speech_active =
			app.audio.ready
			&& mission_speech_asset(
				app, presentation.command_speech_path) != nullptr
			&& sl_open::audio::cb97_stream_open(
				*mission_speech_asset(
					app, presentation.command_speech_path),
				app.audio.streams[4].source,
				app.audio.streams[4].buffers,
				false,
				app.mission_command_speech,
				sl_open::audio::speech_gain(app.audio));
		if (!presentation.command_speech_active)
		{
			SDL_Log(
				"Mission command speech could not be opened: %s",
				presentation.command_speech_path);
		}
	}
	const auto start_comms = [&]()
	{
		presentation.comms_pending = false;
		// presentation_start publishes this before the deferred bridge runs,
		// matching hudmovie_play_resource's synchronous active flag.
		presentation.movie_pending = false;
		sl_open::audio::cb97_stream_close(app.vfs, app.mission_speech);
		presentation.speech_active = false;
		presentation.comms_active = false;
		sl_open::hud::movie_stop(movie);
		presentation.movie_active = false;
		presentation.comms_active =
			app.audio.ready
			&& mission_speech_asset(app, presentation.speech_path) != nullptr
			&& sl_open::audio::cb97_stream_open(
				*mission_speech_asset(app, presentation.speech_path),
				app.audio.streams[1].source,
				app.audio.streams[1].buffers,
				false,
				app.mission_speech,
				sl_open::audio::speech_gain(app.audio));
		const char* const movie_name =
			presentation.movie_path[0] != '\0'
				? presentation.movie_path
				: mission_pilot_movie(presentation.pilot);
		// CommsVoice_play_or_queue calls hudmovie_play_resource even when
		// HOG_bigread2 could not load the associated speech buffer.
		const sl_open::hud::MovieAsset* movie_asset = movie_name != nullptr
			? mission_movie_asset(app, movie_name)
			: nullptr;
		if (movie_name != nullptr
			&& !(presentation.movie_active =
				movie_asset != nullptr
				&& sl_open::hud::movie_play(
					movie,
					*movie_asset,
					mission_movie_asset(app, "static"),
					presentation.comms_category,
					now)))
		{
			SDL_Log(
				"Mission HUD movie could not be opened: %s",
				movie_name);
		}
		if (!presentation.comms_active)
		{
			SDL_Log(
				"Mission speech could not be opened: %s",
				presentation.speech_path);
		}
	};
	const auto start_standalone_speech = [&]()
	{
		presentation.speech_pending = false;
		sl_open::audio::cb97_stream_close(app.vfs, app.mission_speech);
		presentation.comms_active = false;
		presentation.speech_active = false;
		presentation.speech_active =
			app.audio.ready
			&& mission_speech_asset(
				app, presentation.standalone_speech_path) != nullptr
			&& sl_open::audio::cb97_stream_open(
				*mission_speech_asset(
					app, presentation.standalone_speech_path),
				app.audio.streams[1].source,
				app.audio.streams[1].buffers,
				false,
				app.mission_speech,
				sl_open::audio::speech_gain(app.audio));
		if (!presentation.speech_active)
		{
			SDL_Log(
				"Standalone mission speech could not be opened: %s",
				presentation.standalone_speech_path);
		}
	};
	if (presentation.comms_pending && presentation.speech_pending)
	{
		// Both commands can execute in one interpreter pass. Retail changes
		// CBOX slot zero synchronously, so preserve their request order even
		// though presentation work is deferred to the platform frame.
		if (presentation.comms_request_serial
			< presentation.speech_request_serial)
		{
			start_comms();
			start_standalone_speech();
		}
		else
		{
			start_standalone_speech();
			start_comms();
		}
	}
	else if (presentation.comms_pending)
	{
		start_comms();
	}
	else if (presentation.speech_pending)
	{
		start_standalone_speech();
	}
	if (presentation.movie_pending)
	{
		presentation.movie_pending = false;
		sl_open::hud::movie_stop(movie);
		const sl_open::hud::MovieAsset* asset =
			mission_movie_asset(app, presentation.movie_path);
		presentation.movie_active =
			presentation.movie_path[0] != '\0'
			&& asset != nullptr
			&& sl_open::hud::movie_play(
				movie,
				*asset,
				mission_movie_asset(app, "static"),
				5,
				now);
	}
	const bool mission_audio_paused =
		app.mission_session.state == sl_open::game::MissionSessionState::paused;
	if (mission_audio_paused)
	{
		sl_open::audio::cb97_stream_pause(app.mission_speech);
		sl_open::audio::cb97_stream_pause(app.mission_command_speech);
	}
	else
	{
		sl_open::audio::cb97_stream_resume(app.mission_speech);
		sl_open::audio::cb97_stream_resume(app.mission_command_speech);
	}
	if ((presentation.comms_active || presentation.speech_active)
		&& !sl_open::audio::cb97_stream_update(app.vfs, app.mission_speech))
	{
		presentation.comms_active = false;
		presentation.speech_active = false;
	}
	// HUD_render_panel_contents (LANCER.EXE 0x00486c7b) owns the lifetime
	// of a communications movie. Pilot movies may carry the loop flag, but
	// retail tears the movie down as soon as CBOX speech slot zero becomes
	// idle. WaitForMovie observes that teardown; leaving the looping FM8
	// active deadlocks Mission 29 immediately after its instructor line.
	if (presentation.movie_active
		&& !presentation.comms_active
		&& !presentation.speech_active)
	{
		sl_open::hud::movie_stop(movie);
		presentation.movie_active = false;
	}
	if (presentation.command_speech_active
		&& !sl_open::audio::cb97_stream_update(
			app.vfs, app.mission_command_speech))
	{
		presentation.command_speech_active = false;
	}
	if (presentation.movie_active)
	{
		sl_open::hud::movie_update(movie, now);
		if (!movie.active)
		{
			presentation.movie_active = false;
		}
	}
	if (movie.new_frame)
	{
		sl_open::render::frontend_movie_texture_update(
			app.frontend_renderer.shell.gameplay_hud_movie,
			movie.rgba);
		movie.new_frame = false;
	}
}

bool capture_multiplayer_mission_bootstrap(App& app)
{
	if (app.mission_session.request.origin
			!= sl_open::game::MissionOrigin::multiplayer
		|| app.mission_session.request.multiplayer.deathmatch_mode)
	{
		return false;
	}
	sl_open::game::MultiplayerMissionBootstrap bootstrap;
	return sl_open::game::mission_session_capture_multiplayer_bootstrap(
			app.mission_session, bootstrap)
		&& sl_open::network::multiplayer_transport_update_mission_bootstrap(
			app.multiplayer_transport, bootstrap);
}

void stop_mission_session(App& app)
{
	// Any participant may become the next authority. Preserve the last
	// admitted opcode-9 image before renderer/runtime teardown, including
	// the terminal frame that produced the local mission result.
	(void)capture_multiplayer_mission_bootstrap(app);
	SDL_StopTextInput(app.window);
	if (app.mission_foster_active)
	{
		finish_mission_foster(app);
	}
	if (app.mission_lock_tone_active)
	{
		sl_open::audio::fat_stop(app.audio, 1);
		app.mission_lock_tone_active = false;
	}
	if (app.mission_scanner_tone_active)
	{
		sl_open::audio::fat_stop(app.audio, 2);
		app.mission_scanner_tone_active = false;
	}
	if (app.mission_threat_tone_active)
	{
		sl_open::audio::fat_stop(app.audio, 3);
		app.mission_threat_tone_active = false;
	}
	app.mission_scanner_beep_serial = 0;
	sl_open::audio::cb97_stream_close(app.vfs, app.mission_speech);
	sl_open::audio::cb97_stream_close(
		app.vfs, app.mission_command_speech);
	sl_open::audio::music_close(app.vfs, app.mission_music);
	app.mission_speech_assets.clear();
	app.mission_music_assets.clear();
	app.mission_movie_assets.clear();
	sl_open::render::mission_renderer_shutdown(app.mission_renderer);
	destroy_mission_sound_runtime(app);
	sl_open::game::mission_session_stop(app.mission_session);
	app.options.in_game = false;
}

sl_open::network::MultiplayerPlayerMissionOutcome
multiplayer_player_mission_outcome(
	const sl_open::campaign::MultiplayerMissionResolution& resolution)
{
	using Coordinator = sl_open::campaign::MissionCoordinatorResult;
	using Outcome = sl_open::network::MultiplayerPlayerMissionOutcome;
	switch (resolution.coordinator)
	{
	case Coordinator::ordinary:
	case Coordinator::mission_side:
		return Outcome::survived;
	case Coordinator::destroyed:
		return Outcome::destroyed;
	case Coordinator::retry:
		return resolution.retry_exhausted
			? Outcome::ejected
			: Outcome::survived;
	case Coordinator::interrupted:
		return Outcome::captured;
	case Coordinator::exit_session:
		return Outcome::departed;
	case Coordinator::transfer:
		return resolution.route
				== sl_open::campaign::MultiplayerMissionRoute::
					relaunch_alternate
			? Outcome::survived
			: Outcome::kicked;
	case Coordinator::cooperative_transfer:
		return Outcome::kicked;
	case Coordinator::executed:
		return Outcome::executed;
	case Coordinator::network_abort:
		return Outcome::network_abort;
	}
	return Outcome::network_abort;
}

sl_open::campaign::MissionCoordinatorResult
multiplayer_coordinator_result(
	const sl_open::game::SessionResult& result,
	bool network_aborted)
{
	using Coordinator = sl_open::campaign::MissionCoordinatorResult;
	if (network_aborted)
	{
		return Coordinator::network_abort;
	}
	if (result.kind == sl_open::game::SessionResultKind::exit_to_origin)
	{
		return Coordinator::exit_session;
	}
	return result.coordinator_result
			<= static_cast<std::uint8_t>(Coordinator::network_abort)
		? static_cast<Coordinator>(result.coordinator_result)
		: Coordinator::network_abort;
}

sl_open::campaign::CampaignMovieSequence
multiplayer_terminal_sequence(
	const sl_open::campaign::CampaignState& campaign,
	const sl_open::campaign::MultiplayerMissionResolution& resolution,
	std::uint16_t completed_mission)
{
	using Coordinator = sl_open::campaign::MissionCoordinatorResult;
	sl_open::campaign::CampaignMovieSequence sequence;
	const char* movie = nullptr;
	switch (resolution.coordinator)
	{
	case Coordinator::destroyed:
		movie = completed_mission <= 18
			? "new_funeral.bik"
			: "new_funeral2.bik";
		break;
	case Coordinator::interrupted:
		movie = "int.bik";
		break;
	case Coordinator::executed:
		movie = completed_mission < 19
			? "new_rel_exec.bik"
			: "new_y_exec.bik";
		break;
	case Coordinator::transfer:
	case Coordinator::cooperative_transfer:
		// A valid coordinator-five result has already advanced the live
		// mission before retail selects this movie.
		movie = sl_open::campaign::campaign_transfer_movie(
			campaign, campaign.mission);
		break;
	case Coordinator::retry:
		if (resolution.retry_exhausted)
		{
			movie =
				sl_open::campaign::campaign_ejection_exhausted_movie(
					completed_mission);
		}
		break;
	case Coordinator::ordinary:
	case Coordinator::mission_side:
	case Coordinator::exit_session:
	case Coordinator::network_abort:
		break;
	}
	if (movie != nullptr)
	{
		sequence.movies[0] = movie;
		sequence.count = 1;
	}
	return sequence;
}

sl_open::campaign::MultiplayerMissionInput
multiplayer_mission_input(
	const sl_open::game::SessionResult& result,
	bool network_aborted)
{
	sl_open::campaign::MultiplayerMissionInput input;
	input.coordinator =
		multiplayer_coordinator_result(result, network_aborted);
	const std::int16_t grade = result.grade;
	input.grade =
		grade >= static_cast<std::int16_t>(
				sl_open::campaign::MissionGrade::none)
			&& grade <= static_cast<std::int16_t>(
				sl_open::campaign::MissionGrade::perfect)
		? static_cast<sl_open::campaign::MissionGrade>(grade)
		: sl_open::campaign::MissionGrade::none;
	input.score_delta = result.score_delta;
	input.score_events = result.score_events;
	std::copy(
		std::begin(result.persistent_variables),
		std::end(result.persistent_variables),
		std::begin(input.persistent_variables));
	input.campaign_state_captured = result.campaign_state_captured;
	return input;
}

void maybe_publish_multiplayer_mission_result(App& app)
{
	if (app.multiplayer_local_outcome_reported)
	{
		return;
	}
	if (app.mission_session.result.kind
			== sl_open::game::SessionResultKind::none
		|| app.mission_session.result.kind
			== sl_open::game::SessionResultKind::load_failed)
	{
		return;
	}
	(void)capture_multiplayer_mission_bootstrap(app);
	if (sl_open::network::multiplayer_transport_local_mission_report(
			app.multiplayer_transport) != nullptr)
	{
		app.multiplayer_local_outcome_reported = true;
		return;
	}
	sl_open::game::MultiplayerLaunchSnapshot launch;
	if (!sl_open::network::multiplayer_transport_launch_snapshot(
			app.multiplayer_transport, launch))
	{
		return;
	}
	if (launch.deathmatch_mode)
	{
		std::int32_t kills[
			sl_open::network::kMultiplayerTransportPlayerCapacity]{};
		std::int32_t deaths[
			sl_open::network::kMultiplayerTransportPlayerCapacity]{};
		std::copy(
			std::begin(
				app.mission_session.mission_runtime.network
					.player_kills),
			std::end(
				app.mission_session.mission_runtime.network
					.player_kills),
			std::begin(kills));
		std::copy(
			std::begin(
				app.mission_session.mission_runtime.network
					.player_deaths),
			std::end(
				app.mission_session.mission_runtime.network
					.player_deaths),
			std::begin(deaths));
		app.multiplayer_local_outcome_reported =
			sl_open::network::
				multiplayer_transport_publish_deathmatch_result(
					app.multiplayer_transport,
					kills,
					deaths,
					app.multiplayer_network_aborted);
		return;
	}

	sl_open::game::SessionResult authoritative =
		app.mission_session.result;
	const sl_open::campaign::MultiplayerMissionInput input =
		multiplayer_mission_input(
			authoritative, app.multiplayer_network_aborted);
	sl_open::campaign::CampaignState campaign =
		app.multiplayer_campaign;
	sl_open::campaign::MultiplayerCampaignProgression progression =
		app.multiplayer_progression;
	sl_open::campaign::MultiplayerMissionResolution resolution =
		sl_open::campaign::multiplayer_campaign_apply_result(
			progression, campaign, input);
	authoritative.coordinator_result =
		static_cast<std::uint8_t>(resolution.coordinator);
	authoritative.grade =
		static_cast<std::int16_t>(resolution.grade);

	sl_open::game::MultiplayerCampaignLaunchState campaign_launch;
	campaign_launch.state = campaign;
	campaign_launch.present = true;
	campaign_launch.mission_25_alternate =
		progression.alternate_mission_25;
	const sl_open::network::MultiplayerPlayerMissionOutcome outcome =
		multiplayer_player_mission_outcome(resolution);
	app.multiplayer_local_outcome_reported =
		sl_open::network::multiplayer_transport_publish_mission_result(
			app.multiplayer_transport,
			authoritative,
			campaign_launch,
			resolution.advance,
			outcome);
}

void apply_multiplayer_session_result(App& app, std::uint64_t now)
{
	const sl_open::game::SessionResult& result =
		app.mission_session.result;
	if (result.kind == sl_open::game::SessionResultKind::none
		|| result.kind == sl_open::game::SessionResultKind::load_failed)
	{
		return;
	}

	const sl_open::campaign::MissionCoordinatorResult coordinator =
		multiplayer_coordinator_result(
			result, app.multiplayer_network_aborted);
	const bool deathmatch =
		app.mission_session.request.multiplayer.deathmatch_mode;
	if (deathmatch)
	{
		if (coordinator
				== sl_open::campaign::MissionCoordinatorResult::network_abort
			|| coordinator
				== sl_open::campaign::MissionCoordinatorResult::exit_session)
		{
			stop_mission_session(app);
			leave_multiplayer_for_main_menu(app, now);
			return;
		}
		// Deathmatch has no cooperative debrief, but the authority must
		// first publish the final live score table. Clients retain the last
		// rendered world until that result arrives; the mission-result event
		// then takes every participant directly to frontend state zero.
		if (sl_open::network::multiplayer_transport_is_host(
				app.multiplayer_transport))
		{
			maybe_publish_multiplayer_mission_result(app);
		}
		return;
	}
	if (coordinator
			== sl_open::campaign::MissionCoordinatorResult::network_abort
		|| (coordinator
				== sl_open::campaign::MissionCoordinatorResult::exit_session
			&& !deathmatch))
	{
		stop_mission_session(app);
		sl_open::campaign::multiplayer_campaign_abandon(
			app.multiplayer_progression);
		leave_multiplayer_for_main_menu(app, now);
		return;
	}

	maybe_publish_multiplayer_mission_result(app);
}

sl_open::campaign::MultiplayerMissionResolution
multiplayer_resolution_from_report(
	const sl_open::network::MultiplayerPlayerMissionReport& report)
{
	using Coordinator = sl_open::campaign::MissionCoordinatorResult;
	using Route = sl_open::campaign::MultiplayerMissionRoute;
	sl_open::campaign::MultiplayerMissionResolution resolution;
	resolution.advance = report.advance;
	resolution.coordinator =
		report.result.coordinator_result
				<= static_cast<std::uint8_t>(
					Coordinator::network_abort)
			? static_cast<Coordinator>(
				report.result.coordinator_result)
			: Coordinator::network_abort;
	resolution.grade =
		report.result.grade
					>= static_cast<std::int16_t>(
						sl_open::campaign::MissionGrade::none)
				&& report.result.grade
					<= static_cast<std::int16_t>(
						sl_open::campaign::MissionGrade::perfect)
			? static_cast<sl_open::campaign::MissionGrade>(
				report.result.grade)
			: sl_open::campaign::MissionGrade::none;
	resolution.progression_applied =
		report.advance.completed_mission != 0;
	resolution.retry_exhausted =
		resolution.coordinator == Coordinator::retry
		&& !resolution.progression_applied
		&& report.campaign.present
		&& report.campaign.state.retry_count > 2;
	if (resolution.coordinator == Coordinator::exit_session
		|| resolution.coordinator == Coordinator::network_abort)
	{
		resolution.route = Route::leave_session;
	}
	else if (report.campaign.mission_25_alternate)
	{
		resolution.route = Route::relaunch_alternate;
	}
	else if (resolution.coordinator != Coordinator::transfer
		&& report.advance.campaign_complete)
	{
		resolution.route = Route::campaign_complete;
	}
	else
	{
		resolution.route = Route::debrief;
	}
	return resolution;
}

void compose_multiplayer_mission_report(
	App& app,
	const sl_open::network::MultiplayerPlayerMissionReport& report,
	const sl_open::game::MultiplayerLaunchSnapshot& launch)
{
	app.multiplayer_report = {};
	app.multiplayer_report_valid = false;
	if (!report.campaign.present)
	{
		return;
	}
	sl_open::frontend::MultiplayerDebriefReportInput input;
	input.recipient_callsign =
		launch.local_player < launch.player_count
			? launch.players[launch.local_player].name
			: app.multiplayer_callsign;
	input.mission =
		report.result.mission == 25
				&& app.multiplayer_completed_mission_25_alternate
			? 252
			: report.result.mission;
	if (report.result.mission >= 1
		&& report.result.mission
			<= sl_open::campaign::kMissionCount)
	{
		input.retry_history =
			report.campaign.state.retry_history[
				report.result.mission - 1];
	}
	input.grade =
		static_cast<sl_open::campaign::MissionGrade>(
			report.result.grade);
	input.outcome =
		static_cast<sl_open::campaign::MissionCoordinatorResult>(
			report.result.coordinator_result);
	input.objectives_completed_before_ejection =
		report.result
			.objectives_completed_before_ejection;
	input.new_medal = report.advance.new_medal;
	input.new_bar = report.advance.new_bar;
	input.new_rank = report.advance.new_rank;
	if (report.campaign.state.progression
		> app.multiplayer_campaign.progression)
	{
		input.new_progression = static_cast<std::int8_t>(
			report.campaign.state.progression);
	}
	app.multiplayer_report_valid =
		sl_open::frontend::multiplayer_debrief_compose_report(
			input,
			app.language,
			app.itac_language,
			app.multiplayer_report);
}

void adopt_multiplayer_pending_campaign(App& app)
{
	if (!app.multiplayer_pending_campaign_valid)
	{
		return;
	}
	app.multiplayer_campaign =
		app.multiplayer_pending_campaign;
	app.multiplayer_campaign_valid = true;
	app.multiplayer_progression.pending =
		app.multiplayer_pending_resolution;
	app.multiplayer_progression.alternate_mission_25 =
		app.multiplayer_pending_resolution.route
			== sl_open::campaign::MultiplayerMissionRoute::
				relaunch_alternate;
	app.multiplayer_progression.debrief_pending =
		app.multiplayer_pending_resolution.route
			== sl_open::campaign::MultiplayerMissionRoute::debrief;
	app.multiplayer_pending_campaign_valid = false;
}

void save_multiplayer_progress(App& app)
{
	if (!app.multiplayer_profile_save_pending)
	{
		return;
	}
	app.multiplayer_profile_save_pending = false;
	if (!sl_open::campaign::campaign_profile_save(
			app.campaign_store, app.multiplayer_campaign))
	{
		SDL_Log(
			"Multiplayer campaign profile save failed after mission %u",
			static_cast<unsigned>(
				app.multiplayer_pending_resolution.advance
					.completed_mission));
		return;
	}
	app.multiplayer_local_profile =
		app.multiplayer_campaign;
	app.multiplayer_local_profile_valid = true;
}

void enter_multiplayer_debrief(App& app, std::uint64_t now)
{
	const sl_open::network::MultiplayerPlayerMissionReport* const report =
		sl_open::network::multiplayer_transport_local_mission_report(
			app.multiplayer_transport);
	if (report != nullptr
		&& report->result.mission == 25
		&& !app.multiplayer_completed_mission_25_alternate)
	{
		// Multiplayer_game_loop exits before constructing the mission-25A
		// debrief whenever that leg reaches a terminal/restart outcome.
		sl_open::campaign::multiplayer_campaign_abandon(
			app.multiplayer_progression);
		leave_multiplayer_for_main_menu(app, now);
		return;
	}
	save_multiplayer_progress(app);
	const sl_open::network::MultiplayerTransportState state =
		sl_open::network::multiplayer_transport_state(
			app.multiplayer_transport);
	if (state != sl_open::network::MultiplayerTransportState::post_mission
		&& state != sl_open::network::MultiplayerTransportState::gameplay)
	{
		leave_multiplayer_for_main_menu(app, now);
		return;
	}
	if (state == sl_open::network::MultiplayerTransportState::gameplay)
	{
		// FUN_004296a0 is also the post-game network barrier. A local
		// presentation may finish before another connected player reports;
		// retain the real debrief UI and keep servicing that barrier instead
		// of treating an ordinary wait as a session failure.
		(void)sl_open::network::multiplayer_transport_enter_post_mission(
			app.multiplayer_transport);
	}
	sl_open::frontend::multiplayer_debrief_reset(
		app.multiplayer_debrief, now);
	app.frontend_phase = FrontendPhase::multiplayer_debrief;
	app.transition_holds_frame = false;
	SDL_StartTextInput(app.window);
}

void start_multiplayer_award_movie(App& app, std::uint64_t now)
{
	const char* movie = sl_open::campaign::campaign_medal_movie(
		app.multiplayer_pending_resolution.advance.new_medal);
	if (movie == nullptr)
	{
		finish_multiplayer_award_movie(app, now);
		return;
	}
	const sl_open::audio::Stream& stream = app.audio.streams[2];
	if (!sl_open::media::bink_movie_open(
			app.vfs,
			movie,
			app.audio.ready ? stream.source : 0,
			stream.buffers,
			now,
			app.campaign_movie))
	{
		SDL_Log(
			"Multiplayer award movie could not be opened: %s",
			movie);
		finish_multiplayer_award_movie(app, now);
		return;
	}
	app.multiplayer_mission_presentation =
		sl_open::platform::MultiplayerMissionPresentation::award;
	app.frontend_phase = FrontendPhase::campaign_award_movie;
}

void begin_next_multiplayer_prelaunch(
	App& app,
	std::uint64_t now)
{
	const sl_open::network::MultiplayerTransportState transport_state =
		sl_open::network::multiplayer_transport_state(
			app.multiplayer_transport);
	const bool direct_mission_25_alternate =
		transport_state
			== sl_open::network::MultiplayerTransportState::gameplay
		&& app.multiplayer_progression.alternate_mission_25;
	const sl_open::game::MultiplayerCampaignLaunchState next =
		direct_mission_25_alternate
			? multiplayer_campaign_launch_state(app)
			: sl_open::network::
				multiplayer_transport_prelaunch_campaign(
					app.multiplayer_transport);
	if (!next.present)
	{
		leave_multiplayer_for_main_menu(app, now);
		return;
	}
	app.multiplayer_campaign =
		localize_multiplayer_campaign(app, next.state);
	app.multiplayer_campaign_valid = true;
	app.multiplayer_progression.alternate_mission_25 =
		next.mission_25_alternate;
	app.multiplayer_result_applied = false;
	app.multiplayer_local_outcome_reported = false;
	app.multiplayer_report_valid = false;
	app.multiplayer_network_aborted = false;
	app.multiplayer_mission_presentation =
		sl_open::platform::MultiplayerMissionPresentation::none;
	app.frontend_phase = FrontendPhase::multiplayer_launch_wait;
	SDL_StopTextInput(app.window);
	if (sl_open::network::multiplayer_transport_is_host(
			app.multiplayer_transport))
	{
		const sl_open::network::MultiplayerLobbySnapshot& lobby =
			sl_open::network::multiplayer_transport_lobby(
				app.multiplayer_transport);
		const bool selection_ready =
			lobby.coop_selection.present
				|| publish_multiplayer_coop_selection(app);
		if (selection_ready)
		{
			(void)sl_open::network::
				multiplayer_transport_begin_prelaunch(
					app.multiplayer_transport, next);
		}
	}
}

void finish_multiplayer_award_movie(App& app, std::uint64_t now)
{
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	app.multiplayer_mission_presentation =
		sl_open::platform::MultiplayerMissionPresentation::none;
	save_multiplayer_progress(app);
	route_multiplayer_after_progression(app, now);
}

void route_multiplayer_after_progression(
	App& app,
	std::uint64_t now)
{
	const sl_open::network::MultiplayerPlayerMissionReport* const report =
		sl_open::network::multiplayer_transport_local_mission_report(
			app.multiplayer_transport);
	if (report == nullptr)
	{
		return;
	}
	const sl_open::campaign::CampaignMovieSequence sequence =
		multiplayer_terminal_sequence(
			app.multiplayer_campaign,
			app.multiplayer_pending_resolution,
			report->result.mission);
	if (sequence.count == 0)
	{
		enter_multiplayer_debrief(app, now);
		return;
	}
	app.multiplayer_mission_presentation =
		sl_open::platform::MultiplayerMissionPresentation::terminal;
	begin_campaign_sequence(
		app,
		sequence,
		sl_open::platform::CampaignSequencePurpose::
			multiplayer_post_mission,
		now);
}

void finish_multiplayer_campaign_sequence(
	App& app,
	std::uint64_t now)
{
	using Presentation =
		sl_open::platform::MultiplayerMissionPresentation;
	using Route = sl_open::campaign::MultiplayerMissionRoute;
	const Presentation presentation =
		app.multiplayer_mission_presentation;
	if (presentation == Presentation::final_chapter)
	{
		enter_credits(app, now);
		return;
	}
	if (presentation != Presentation::post_mission
		&& presentation != Presentation::terminal)
	{
		return;
	}
	if (presentation == Presentation::terminal)
	{
		app.multiplayer_mission_presentation = Presentation::none;
		enter_multiplayer_debrief(app, now);
		return;
	}

	adopt_multiplayer_pending_campaign(app);
	const Route route =
		app.multiplayer_pending_resolution.route;
	if (route == Route::relaunch_alternate)
	{
		begin_next_multiplayer_prelaunch(app, now);
		return;
	}
	if (route == Route::campaign_complete)
	{
		app.multiplayer_profile_save_pending = false;
		app.multiplayer_mission_presentation =
			Presentation::final_chapter;
		begin_campaign_sequence(
			app,
			sl_open::campaign::campaign_final_sequence(
				app.multiplayer_campaign),
			sl_open::platform::CampaignSequencePurpose::
				multiplayer_post_mission,
			now);
		return;
	}
	if (route == Route::leave_session)
	{
		sl_open::campaign::multiplayer_campaign_abandon(
			app.multiplayer_progression);
		leave_multiplayer_for_main_menu(app, now);
		return;
	}
	if (app.multiplayer_pending_resolution.advance.new_medal >= 0)
	{
		start_multiplayer_award_movie(app, now);
		return;
	}
	save_multiplayer_progress(app);
	route_multiplayer_after_progression(app, now);
}

void apply_multiplayer_local_result(
	App& app,
	std::uint64_t now)
{
	if (app.multiplayer_result_applied)
	{
		return;
	}
	const sl_open::network::MultiplayerMissionResultSnapshot& snapshot =
		sl_open::network::multiplayer_transport_mission_result(
			app.multiplayer_transport);
	if (snapshot.deathmatch_scores_valid)
	{
		app.multiplayer_result_applied = true;
		stop_mission_session(app);
		leave_multiplayer_for_main_menu(app, now);
		return;
	}
	const sl_open::network::MultiplayerPlayerMissionReport* const report =
		sl_open::network::multiplayer_transport_local_mission_report(
			app.multiplayer_transport);
	if (report == nullptr)
	{
		return;
	}
	app.multiplayer_result_applied = true;
	app.multiplayer_completed_mission_25_alternate =
		app.mission_session.request.mission_25_alternate;
	sl_open::game::MultiplayerLaunchSnapshot launch;
	if (!sl_open::network::multiplayer_transport_launch_snapshot(
			app.multiplayer_transport, launch))
	{
		app.multiplayer_result_applied = false;
		return;
	}
	if (!report->campaign.present)
	{
		stop_mission_session(app);
		leave_multiplayer_for_main_menu(app, now);
		return;
	}

	app.multiplayer_pending_campaign =
		localize_multiplayer_campaign(
			app, report->campaign.state);
	app.multiplayer_pending_campaign_valid = true;
	app.multiplayer_pending_resolution =
		multiplayer_resolution_from_report(*report);
	app.multiplayer_profile_save_pending =
		app.multiplayer_pending_resolution.progression_applied
		&& !app.multiplayer_pending_resolution
			.advance.campaign_complete;
	compose_multiplayer_mission_report(app, *report, launch);
	stop_mission_session(app);

	if (app.multiplayer_pending_resolution.route
		== sl_open::campaign::MultiplayerMissionRoute::leave_session)
	{
		adopt_multiplayer_pending_campaign(app);
		sl_open::campaign::multiplayer_campaign_abandon(
			app.multiplayer_progression);
		leave_multiplayer_for_main_menu(app, now);
		return;
	}
	// Multiplayer_game_loop gates FUN_004abde0 behind
	// DAT_00595c64 == 0 at 0x004a9fa2..0x004a9fc2. Cooperative sessions
	// therefore do not run the single-player landing/post-mission sequence;
	// they proceed to the award/terminal presentation and FUN_004296a0
	// debrief immediately.
	app.multiplayer_mission_presentation =
		sl_open::platform::MultiplayerMissionPresentation::
			post_mission;
	finish_multiplayer_campaign_sequence(app, now);
}

void handle_multiplayer_post_mission_action(
	App& app,
	sl_open::network::MultiplayerPostMissionAction action,
	std::uint64_t now)
{
	if (!app.multiplayer_progression.debrief_pending)
	{
		return;
	}
	switch (action)
	{
	case sl_open::network::MultiplayerPostMissionAction::replay:
		if (!sl_open::campaign::multiplayer_campaign_replay(
				app.multiplayer_progression,
				app.multiplayer_campaign))
		{
			leave_multiplayer_for_main_menu(app, now);
			return;
		}
		break;
	case sl_open::network::MultiplayerPostMissionAction::
			continue_campaign:
		sl_open::campaign::multiplayer_campaign_commit(
			app.multiplayer_progression,
			app.multiplayer_campaign);
		break;
	case sl_open::network::MultiplayerPostMissionAction::none:
		return;
	}
	begin_next_multiplayer_prelaunch(app, now);
}

bool handle_multiplayer_debrief_action(
	App& app,
	const sl_open::frontend::MultiplayerDebriefAction& action,
	std::uint64_t now)
{
	using Action =
		sl_open::frontend::MultiplayerDebriefActionType;
	switch (action.type)
	{
	case Action::none:
		return false;
	case Action::ready:
		(void)sl_open::network::
			multiplayer_transport_set_post_mission_ready(
				app.multiplayer_transport, true);
		return false;
	case Action::continue_mission:
		(void)sl_open::network::multiplayer_transport_post_mission_action(
			app.multiplayer_transport,
			sl_open::network::MultiplayerPostMissionAction::
				continue_campaign);
		return false;
	case Action::replay_mission:
		(void)sl_open::network::multiplayer_transport_post_mission_action(
			app.multiplayer_transport,
			sl_open::network::MultiplayerPostMissionAction::replay);
		return false;
	case Action::leave:
		sl_open::campaign::multiplayer_campaign_abandon(
			app.multiplayer_progression);
		leave_multiplayer_for_main_menu(app, now);
		return false;
	case Action::save_game:
		enter_multiplayer_campaign_browser(
			app,
			sl_open::frontend::SaveLoadMode::save,
			true,
			now);
		return false;
	case Action::edit_chat:
		(void)append_text(
			app.multiplayer_debrief_chat_entry,
			action.text);
		return false;
	case Action::backspace_chat:
		(void)backspace_text(
			app.multiplayer_debrief_chat_entry);
		return false;
	case Action::send_chat:
		if (sl_open::network::
				multiplayer_transport_submit_post_mission_chat(
					app.multiplayer_transport,
					action.text))
		{
			app.multiplayer_debrief_chat_entry[0] = '\0';
		}
		return false;
	}
	return false;
}

void finish_instant_action(App& app, std::uint64_t now)
{
	const sl_open::game::MissionLaunchRequest request =
		app.mission_session.request;
	if (app.instant_action_snapshot_valid)
	{
		if (std::memcmp(
				&app.campaign,
				&app.instant_action_campaign_snapshot,
				sizeof(app.campaign)) != 0)
		{
			SDL_Log(
				"Instant Action campaign-state mutation detected; restoring snapshot");
		}
		app.campaign = app.instant_action_campaign_snapshot;
	}
	app.instant_action_snapshot_valid = false;
	stop_mission_session(app);

	if (request.origin == sl_open::game::MissionOrigin::campaign_sim_pod)
	{
		app.sim_pod.phase = sl_open::frontend::SimPodPhase::active;
		app.sim_pod.page = request.sim_pod.page;
		app.sim_pod.hovered = request.sim_pod.selected;
		app.sim_pod.late_campaign = request.sim_pod.late_campaign;
		app.campaign_hub.node = request.sim_pod.hub_node;
		app.campaign_hub.late_campaign = request.sim_pod.late_campaign;
		app.frontend_phase = FrontendPhase::campaign_sim_pod;
		app.transition_holds_frame = false;
		start_hub_ambience(app);
	}
	else
	{
		enter_main_menu(app, now);
	}
}

void restart_instant_action(App& app)
{
	const sl_open::game::MissionLaunchRequest request =
		app.mission_session.request;
	stop_mission_session(app);
	start_live_mission(app, request);
}

void apply_campaign_session_result(App& app, std::uint64_t now)
{
	if (app.mission_session.result.kind
			== sl_open::game::SessionResultKind::none
		|| app.mission_session.result.kind
			== sl_open::game::SessionResultKind::load_failed)
	{
		return;
	}
	if (app.mission_session.result.kind
		== sl_open::game::SessionResultKind::restart)
	{
		// The in-game restart latch returns through 0x004aa45b, clears
		// DAT_00587cdc, reloads GAME100, and jumps straight back to DTE
		// loading. In particular, restarting mission 251 returns to the
		// first mission-25 leg rather than replaying the alternate request.
		if (app.mission_checkpoint_valid)
		{
			app.campaign = app.mission_checkpoint;
		}
		app.alternate_mission_25 = false;
		stop_mission_session(app);
		begin_instant_action(
			app,
			sl_open::game::MissionOrigin::campaign,
			now,
			app.campaign.mission,
			sl_open::game::MissionMode::campaign);
		return;
	}

	const sl_open::game::SessionResult result =
		app.mission_session.result;
	std::uint8_t coordinator = result.coordinator_result;
	if (result.kind == sl_open::game::SessionResultKind::exit_to_origin)
	{
		coordinator = static_cast<std::uint8_t>(
			sl_open::campaign::MissionCoordinatorResult::exit_session);
	}
	if (coordinator > static_cast<std::uint8_t>(
			sl_open::campaign::MissionCoordinatorResult::network_abort))
	{
		coordinator = static_cast<std::uint8_t>(
			sl_open::campaign::MissionCoordinatorResult::ordinary);
	}
	const auto campaign_result =
		static_cast<sl_open::campaign::MissionCoordinatorResult>(
			coordinator);
	app.pending_mission_result = campaign_result;
	if (result.campaign_state_captured
		&& campaign_result
			!= sl_open::campaign::MissionCoordinatorResult::destroyed
		&& campaign_result
			!= sl_open::campaign::MissionCoordinatorResult::interrupted
		&& campaign_result
			!= sl_open::campaign::MissionCoordinatorResult::exit_session
		&& campaign_result
			!= sl_open::campaign::MissionCoordinatorResult::executed)
	{
		static_assert(
			sl_open::campaign::kBranchVariableCount
				== sl_open::game::kMissionPersistentVariableCount);
		std::copy(
			std::begin(result.persistent_variables),
			std::end(result.persistent_variables),
			std::begin(app.campaign.branch_variables));
	}

	app.instant_action_snapshot_valid = false;
	stop_mission_session(app);
	switch (campaign_result)
	{
	case sl_open::campaign::MissionCoordinatorResult::destroyed:
		enter_post_mission_failure(
			app, MissionFailureKind::destroyed, now);
		return;
	case sl_open::campaign::MissionCoordinatorResult::interrupted:
		enter_post_mission_failure(
			app, MissionFailureKind::interrupted, now);
		return;
	case sl_open::campaign::MissionCoordinatorResult::exit_session:
		enter_post_mission_choice(app, now);
		return;
	default:
		break;
	}

	sl_open::campaign::MissionGrade grade =
		static_cast<sl_open::campaign::MissionGrade>(result.grade);
	start_post_mission(
		app,
		grade,
		result.score_delta,
		result.score_events,
		campaign_result,
		now);
}

void apply_instant_action_result(App& app, std::uint64_t now)
{
	if (app.mission_session.request.origin
		== sl_open::game::MissionOrigin::multiplayer)
	{
		apply_multiplayer_session_result(app, now);
		return;
	}
	if (app.mission_session.request.origin
		== sl_open::game::MissionOrigin::campaign)
	{
		apply_campaign_session_result(app, now);
		return;
	}
	switch (app.mission_session.result.kind)
	{
	case sl_open::game::SessionResultKind::restart:
		restart_instant_action(app);
		break;
	case sl_open::game::SessionResultKind::exit_to_origin:
	case sl_open::game::SessionResultKind::player_destroyed:
	case sl_open::game::SessionResultKind::mission_failed:
	case sl_open::game::SessionResultKind::mission_complete:
		finish_instant_action(app, now);
		break;
	default:
		break;
	}
}

void enter_sim_pod(App& app, std::uint64_t now)
{
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	sl_open::frontend::sim_pod_reset(
		app.sim_pod, app.campaign_hub.late_campaign);
	app.frontend_phase = FrontendPhase::campaign_sim_pod;
	open_sim_pod_movie(app, now);
}

void leave_sim_pod(App& app, std::uint64_t now)
{
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	play_walk_sound(app, 11);
	enter_hub_node(
		app,
		sl_open::frontend::campaign_hub_return_node(
			app.campaign_hub,
			sl_open::frontend::HubAction::sim_pod),
		now);
}

void handle_sim_pod_action(
	App& app,
	sl_open::frontend::SimPodResult result,
	std::uint64_t now)
{
	switch (result.action)
	{
	case sl_open::frontend::SimPodAction::changed_page:
		open_sim_pod_movie(app, now);
		break;
	case sl_open::frontend::SimPodAction::exit:
		if (app.sim_pod.phase == sl_open::frontend::SimPodPhase::complete)
		{
			leave_sim_pod(app, now);
		}
		else
		{
			open_sim_pod_movie(app, now);
			if (app.sim_pod.phase == sl_open::frontend::SimPodPhase::complete)
			{
				leave_sim_pod(app, now);
			}
		}
		break;
	case sl_open::frontend::SimPodAction::launch_mission:
		// SimPod_loop, LANCER.EXE 0x0044f665..0x0044f6bb: the three
		// training entries use object-owner mode one, while mission 29 uses
		// Instant Action owner mode two plus the simulator flag.
		if (result.mission >= 29 && result.mission <= 32)
		{
			begin_instant_action(
				app,
				sl_open::game::MissionOrigin::campaign_sim_pod,
				now,
				result.mission,
				result.mission == 29
					? sl_open::game::MissionMode::instant_action
					: sl_open::game::MissionMode::training);
		}
		break;
	default:
		break;
	}
}

void enter_cd_player(App& app)
{
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	sl_open::audio::wav_stream_close(app.vfs, app.cd_music);
	sl_open::frontend::cd_player_reset(
		app.cd_player, app.campaign_hub.late_campaign);
	app.frontend_phase = FrontendPhase::campaign_cd;
}

void leave_cd_player(App& app, std::uint64_t now)
{
	sl_open::audio::wav_stream_close(app.vfs, app.cd_music);
	enter_hub_node(
		app,
		sl_open::frontend::campaign_hub_return_node(
			app.campaign_hub,
			sl_open::frontend::HubAction::cd_player),
		now);
}

void play_cd_track(App& app)
{
	char path[sl_open::io::kMaxPath];
	if (sl_open::frontend::cd_player_track_path(
			app.cd_player, path, sizeof(path)) == nullptr)
	{
		return;
	}
	const sl_open::audio::Stream& stream = app.audio.streams[0];
	app.cd_player.started =
		app.audio.ready
		&& sl_open::audio::wav_stream_open(
			app.vfs,
			path,
			stream.source,
			stream.buffers,
			app.cd_player.repeat,
			app.cd_music);
	app.cd_player.paused = false;
	if (!app.cd_player.started)
	{
		SDL_Log("CD track could not be opened: %s", path);
	}
	if (app.audio.ready)
	{
		alSourcef(
			stream.source,
			AL_GAIN,
			static_cast<float>(app.cd_volume * app.audio.master_volume)
				/ (127.0f * 127.0f));
	}
}

void handle_cd_action(
	App& app,
	sl_open::frontend::CdAction action,
	std::uint64_t now)
{
	switch (action)
	{
	case sl_open::frontend::CdAction::play:
		play_cd_track(app);
		break;
	case sl_open::frontend::CdAction::pause:
		if (app.audio.ready)
		{
			if (app.cd_player.paused)
			{
				alSourcePause(app.audio.streams[0].source);
			}
			else
			{
				alSourcePlay(app.audio.streams[0].source);
			}
		}
		break;
	case sl_open::frontend::CdAction::stop:
		sl_open::audio::wav_stream_close(app.vfs, app.cd_music);
		break;
	case sl_open::frontend::CdAction::exit:
		leave_cd_player(app, now);
		break;
	case sl_open::frontend::CdAction::volume_up:
		if (app.cd_volume < 127)
		{
			++app.cd_volume;
		}
		break;
	case sl_open::frontend::CdAction::volume_down:
		if (app.cd_volume > 0)
		{
			--app.cd_volume;
		}
		break;
	case sl_open::frontend::CdAction::none:
		break;
	}
	if ((action == sl_open::frontend::CdAction::volume_up
			|| action == sl_open::frontend::CdAction::volume_down)
		&& app.audio.ready)
	{
		alSourcef(
			app.audio.streams[0].source,
			AL_GAIN,
			static_cast<float>(app.cd_volume * app.audio.master_volume)
				/ (127.0f * 127.0f));
	}
}

void leave_medal_display(App& app, std::uint64_t now)
{
	play_walk_sound(app, 6);
	enter_hub_node(
		app,
		sl_open::frontend::campaign_hub_return_node(
			app.campaign_hub,
			sl_open::frontend::HubAction::medals),
		now);
}

void open_medal_movie(App& app, std::uint64_t now)
{
	const char* path =
		sl_open::frontend::medal_display_movie(app.medal_display);
	if (path == nullptr)
	{
		if (app.medal_display.phase
			== sl_open::frontend::MedalPhase::complete)
		{
			leave_medal_display(app, now);
		}
		return;
	}
	const sl_open::audio::Stream& stream = app.audio.streams[2];
	if (!sl_open::media::bink_movie_open(
			app.vfs,
			path,
			app.audio.ready ? stream.source : 0,
			stream.buffers,
			now,
			app.campaign_movie))
	{
		SDL_Log("Medal Display movie could not be opened: %s", path);
		if (sl_open::frontend::medal_display_movie_finished(app.medal_display))
		{
			open_medal_movie(app, now);
		}
	}
}

void enter_medal_display(App& app, std::uint64_t now)
{
	sl_open::frontend::medal_display_reset(
		app.medal_display, app.campaign_hub.late_campaign);
	app.frontend_phase = FrontendPhase::campaign_medals;
	play_walk_sound(app, 5);
	open_medal_movie(app, now);
}

void close_medal_display(App& app, std::uint64_t now)
{
	if (sl_open::frontend::medal_display_begin_close(app.medal_display))
	{
		play_walk_sound(app, 4);
		open_medal_movie(app, now);
	}
}

void leave_news_report(App& app, std::uint64_t now)
{
	sl_open::audio::cb97_stream_close(app.vfs, app.campaign_speech);
	app.news_narration_started = false;
	start_hub_ambience(app);
	play_walk_sound(app, 3);
	enter_hub_node(
		app,
		sl_open::frontend::campaign_hub_return_node(
			app.campaign_hub,
			sl_open::frontend::HubAction::news),
		now);
}

void open_news_report_media(App& app, std::uint64_t now)
{
	sl_open::audio::cb97_stream_close(app.vfs, app.campaign_speech);
	const char* movie =
		sl_open::frontend::news_report_movie(app.news_report);
	const sl_open::audio::Stream& movie_stream = app.audio.streams[2];
	if (!sl_open::media::bink_movie_open(
			app.vfs,
			movie,
			app.audio.ready ? movie_stream.source : 0,
			movie_stream.buffers,
			now,
			app.campaign_movie))
	{
		SDL_Log("News report movie could not be opened: %s", movie);
	}
	else
	{
		sl_open::media::bink_movie_set_looping(
			app.campaign_movie,
			sl_open::frontend::news_report_movie_loops(app.news_report));
	}
	char speech_path[sl_open::io::kMaxPath];
	sl_open::frontend::news_report_speech(
		app.news_report, speech_path, sizeof(speech_path));
	const sl_open::audio::Stream& speech_stream = app.audio.streams[1];
	app.news_narration_started =
		app.audio.ready
		&& sl_open::audio::cb97_stream_open(
			app.vfs,
			speech_path,
			speech_stream.source,
			speech_stream.buffers,
			false,
			app.campaign_speech);
	if (!app.news_narration_started)
	{
		SDL_Log("News report narration could not be opened: %s", speech_path);
	}
	sl_open::audio::apply_stream_gains(app.audio);
}

void enter_news_report(App& app, std::uint64_t now)
{
	sl_open::frontend::news_report_reset(
		app.news_report,
		static_cast<std::uint8_t>(app.campaign.mission),
		app.campaign_hub.late_campaign);
	app.frontend_phase = FrontendPhase::campaign_news;
	open_news_report_media(app, now);
}

void advance_news_report(App& app, std::uint64_t now)
{
	sl_open::audio::cb97_stream_close(app.vfs, app.campaign_speech);
	app.news_narration_started = false;
	if (sl_open::frontend::news_report_speech_finished(app.news_report))
	{
		open_news_report_media(app, now);
	}
	else
	{
		leave_news_report(app, now);
	}
}

void leave_itac(App& app, std::uint64_t now)
{
	if (app.itac_post_mission)
	{
		app.itac_post_mission = false;
		// The post-mission ITAC return at 0x004aa6c8 resumes the campaign
		// owner at its interactive VR hub, not the next briefing. Briefing
		// remains a subsequent hub action, preserving the retail
		// mission-to-menu handoff.
		enter_hub_node(
			app,
			app.campaign_hub.late_campaign ? 50 : 0,
			now);
		return;
	}
	enter_hub_node(
		app,
		sl_open::frontend::campaign_hub_return_node(
			app.campaign_hub,
			app.itac_shell.opened_from_news
				? sl_open::frontend::HubAction::news
				: sl_open::frontend::HubAction::itac),
		now);
}

void open_itac_movie(App& app, std::uint64_t now)
{
	const char* path = sl_open::frontend::itac_shell_movie(app.itac_shell);
	if (path == nullptr)
	{
		if (app.itac_shell.phase == sl_open::frontend::ItacPhase::complete)
		{
			leave_itac(app, now);
		}
		return;
	}
	const sl_open::audio::Stream& stream = app.audio.streams[2];
	if (!sl_open::media::bink_movie_open(
			app.vfs,
			path,
			app.audio.ready ? stream.source : 0,
			stream.buffers,
			now,
			app.campaign_movie))
	{
		SDL_Log("ITAC movie could not be opened: %s", path);
		if (sl_open::frontend::itac_shell_movie_finished(app.itac_shell))
		{
			open_itac_movie(app, now);
		}
		else if (app.itac_shell.phase == sl_open::frontend::ItacPhase::complete)
		{
			leave_itac(app, now);
		}
	}
}

void enter_itac(App& app, bool post_mission, std::uint64_t now)
{
	sl_open::frontend::itac_shell_reset(
		app.itac_shell,
		app.campaign_hub.late_campaign,
		post_mission,
		static_cast<std::uint8_t>(app.campaign.mission),
		app.campaign);
	app.frontend_phase = FrontendPhase::campaign_itac;
	open_itac_movie(app, now);
}

void finish_itac_movie(App& app, std::uint64_t now)
{
	if (sl_open::frontend::itac_shell_movie_finished(app.itac_shell))
	{
		open_itac_movie(app, now);
		return;
	}
	sl_open::media::bink_movie_stop(app.vfs, app.campaign_movie);
	if (app.itac_shell.phase == sl_open::frontend::ItacPhase::complete)
	{
		leave_itac(app, now);
	}
}

bool transition_has_immediate_movie_successor(
	const App& app,
	std::uint64_t now)
{
	switch (app.frontend_phase)
	{
	case FrontendPhase::campaign_hub:
	{
		const sl_open::frontend::HubNode& node =
			sl_open::frontend::campaign_hub_node(app.campaign_hub);
		if (node.action == sl_open::frontend::HubAction::room)
		{
			return node.loop_movie != nullptr && node.loop_movie[0] != '\0';
		}
		return node.action == sl_open::frontend::HubAction::fish_room
			|| node.action == sl_open::frontend::HubAction::medals
			|| node.action == sl_open::frontend::HubAction::news
			|| node.action == sl_open::frontend::HubAction::itac
			|| (node.action == sl_open::frontend::HubAction::sim_pod
				&& app.campaign_hub.late_campaign);
	}
	case FrontendPhase::campaign_briefing:
	{
		sl_open::frontend::MissionBriefing next = app.mission_briefing;
		return sl_open::frontend::mission_briefing_movie_finished(next, now)
			&& sl_open::frontend::mission_briefing_movie(next) != nullptr;
	}
	case FrontendPhase::campaign_medals:
	{
		sl_open::frontend::MedalDisplay next = app.medal_display;
		return sl_open::frontend::medal_display_movie_finished(next)
			&& sl_open::frontend::medal_display_movie(next) != nullptr;
	}
	case FrontendPhase::campaign_itac:
	{
		sl_open::frontend::ItacShell next = app.itac_shell;
		return sl_open::frontend::itac_shell_movie_finished(next)
			&& sl_open::frontend::itac_shell_movie(next) != nullptr;
	}
	default:
		return false;
	}
}

bool interrupt_frontend_transition(App& app)
{
	sl_open::media::BinkMovie* movie = nullptr;
	switch (app.frontend_phase)
	{
	case FrontendPhase::transition:
		if (!app.transition_skips_directly)
		{
			movie = &app.transition_movie;
		}
		break;
	case FrontendPhase::campaign_hub:
		if (!app.campaign_hub.loop_active
			&& !app.campaign_hub.fish_active)
		{
			movie = &app.campaign_movie;
		}
		break;
	case FrontendPhase::campaign_briefing:
		if (sl_open::frontend::mission_briefing_movie(
				app.mission_briefing) != nullptr)
		{
			movie = &app.campaign_movie;
		}
		break;
	case FrontendPhase::campaign_sim_pod:
		if (sl_open::frontend::sim_pod_movie(app.sim_pod) != nullptr)
		{
			movie = &app.campaign_movie;
		}
		break;
	case FrontendPhase::campaign_medals:
		if (sl_open::frontend::medal_display_movie(
				app.medal_display) != nullptr)
		{
			movie = &app.campaign_movie;
		}
		break;
	case FrontendPhase::campaign_itac:
		if (sl_open::frontend::itac_shell_movie(app.itac_shell) != nullptr)
		{
			movie = &app.campaign_movie;
		}
		break;
	default:
		break;
	}
	if (movie == nullptr
		|| movie->state == sl_open::media::BinkMovieState::idle
		|| movie->state == sl_open::media::BinkMovieState::finished
		|| movie->state == sl_open::media::BinkMovieState::failed)
	{
		return false;
	}

	const bool advances_to_next_movie =
		transition_has_immediate_movie_successor(app, SDL_GetTicks());
	if (advances_to_next_movie)
	{
		sl_open::media::bink_movie_finish(*movie);
	}
	else
	{
		sl_open::media::bink_movie_skip_to_end(app.vfs, *movie);
	}
	SDL_Log(
		"Frontend transition interrupted in phase %u (%s)",
		static_cast<unsigned>(app.frontend_phase),
		advances_to_next_movie ? "advance sequence" : "seek final frame");
	return true;
}

void update_pointer(App& app, float window_x, float window_y)
{
	int window_width = 0;
	int window_height = 0;
	int pixel_width = 0;
	int pixel_height = 0;
	SDL_GetWindowSize(app.window, &window_width, &window_height);
	SDL_GetWindowSizeInPixels(app.window, &pixel_width, &pixel_height);
	if (window_width <= 0 || window_height <= 0)
	{
		return;
	}

	const float physical_x =
		window_x * static_cast<float>(pixel_width) / window_width;
	const float physical_y =
		window_y * static_cast<float>(pixel_height) / window_height;
	if (app.frontend_phase == FrontendPhase::instant_action)
	{
		const bool inside =
			physical_x >= 0.0f && physical_y >= 0.0f
			&& physical_x < static_cast<float>(app.width)
			&& physical_y < static_cast<float>(app.height);
		sl_open::game::mission_session_set_pointer(
			app.mission_session,
			app.frontend_renderer,
			std::clamp(
				physical_x, 0.0f, static_cast<float>(app.width - 1)),
			std::clamp(
				physical_y, 0.0f, static_cast<float>(app.height - 1)),
			app.width,
			app.height,
			inside);
		return;
	}
	float logical_x = 0.0f;
	float logical_y = 0.0f;
	const bool inside = sl_open::render::frontend_map_input(
		app.width,
		app.height,
		physical_x,
		physical_y,
		logical_x,
		logical_y);
	sl_open::frontend::main_menu_set_pointer(
		app.main_menu, logical_x, logical_y, inside);
	sl_open::frontend::options_set_pointer(
		app.options, logical_x, logical_y, inside);
	sl_open::frontend::campaign_frontend_set_pointer(
		app.campaign_frontend, logical_x, logical_y, inside);
	if (app.frontend_phase == FrontendPhase::multiplayer_frontend)
	{
		const MultiplayerFrontendSessions sessions =
			multiplayer_frontend_sessions(app);
		sl_open::frontend::multiplayer_frontend_set_pointer(
			app.multiplayer_frontend,
			sessions.view,
			logical_x,
			logical_y,
			inside,
			SDL_GetTicks());
	}
	if (app.frontend_phase == FrontendPhase::multiplayer_lobby)
	{
		const sl_open::frontend::MultiplayerLobbyView view =
			multiplayer_lobby_view(app);
		sl_open::frontend::multiplayer_lobby_set_pointer(
			app.multiplayer_lobby,
			view,
			logical_x,
			logical_y,
			inside);
	}
	if (app.frontend_phase == FrontendPhase::multiplayer_debrief)
	{
		sl_open::frontend::multiplayer_debrief_set_pointer(
			app.multiplayer_debrief,
			logical_x,
			logical_y,
			inside);
	}
	sl_open::frontend::campaign_hub_set_pointer(
		app.campaign_hub, logical_x, logical_y, inside);
	sl_open::frontend::mission_briefing_pointer(
		app.mission_briefing, logical_x, logical_y);
	if (sl_open::frontend::loadout_pointer(
			app.loadout, app.loadout_catalog, logical_x, logical_y, SDL_GetTicks())
			&& (app.frontend_phase == FrontendPhase::campaign_loadout
				|| app.frontend_phase
					== FrontendPhase::multiplayer_loadout))
	{
		play_fat_sample(app, app.loadout_sounds, 7, 40);
	}
	sl_open::frontend::post_mission_choice_pointer(
		app.post_mission_choice, logical_x, logical_y, inside);
	sl_open::frontend::sim_pod_set_pointer(
		app.sim_pod, logical_x, logical_y, inside);
	sl_open::frontend::cd_player_set_pointer(
		app.cd_player, logical_x, logical_y, inside);
	sl_open::frontend::medal_display_set_pointer(
		app.medal_display, logical_x, logical_y, inside);
	sl_open::frontend::itac_shell_set_pointer(
		app.itac_shell,
		logical_x,
		logical_y,
		inside,
		SDL_GetTicks());
}

void update_gameplay_mouse_mode(App& app)
{
	const bool requested =
		app.frontend_phase == FrontendPhase::instant_action
		&& app.mission_session.state
			== sl_open::game::MissionSessionState::running
		&& app.mission_session.result.kind
			== sl_open::game::SessionResultKind::none
		&& sl_open::controller_uses_mouse(app.config.controller);
	if (requested == app.relative_mouse_requested)
	{
		return;
	}
	app.relative_mouse_requested = requested;
	app.gameplay_devices.mouse_relative_x = 0.0f;
	app.gameplay_devices.mouse_relative_y = 0.0f;
	if (!SDL_SetWindowRelativeMouseMode(app.window, requested))
	{
		SDL_Log(
			"Could not %s relative mouse mode: %s",
			requested ? "enable" : "disable",
			SDL_GetError());
	}
}

std::uint8_t gamepad_dpad_hat(
	const sl_open::input::GameplayDeviceState& devices)
{
	std::uint8_t value = SDL_HAT_CENTERED;
	if (devices.joystick_buttons[SDL_GAMEPAD_BUTTON_DPAD_UP])
	{
		value = static_cast<std::uint8_t>(value | SDL_HAT_UP);
	}
	if (devices.joystick_buttons[SDL_GAMEPAD_BUTTON_DPAD_RIGHT])
	{
		value = static_cast<std::uint8_t>(value | SDL_HAT_RIGHT);
	}
	if (devices.joystick_buttons[SDL_GAMEPAD_BUTTON_DPAD_DOWN])
	{
		value = static_cast<std::uint8_t>(value | SDL_HAT_DOWN);
	}
	if (devices.joystick_buttons[SDL_GAMEPAD_BUTTON_DPAD_LEFT])
	{
		value = static_cast<std::uint8_t>(value | SDL_HAT_LEFT);
	}
	return value;
}

void set_joystick_hat_state(
	sl_open::input::GameplayDeviceState& devices,
	std::uint8_t hat,
	std::uint8_t value,
	bool pressed)
{
	if (hat >= std::size(devices.joystick_hats))
	{
		return;
	}
	if (pressed)
	{
		devices.joystick_hat_pressed[hat] = static_cast<std::uint8_t>(
			devices.joystick_hat_pressed[hat]
			| (value & ~devices.joystick_hats[hat]));
	}
	else
	{
		devices.joystick_hat_pressed[hat] = 0;
	}
	devices.joystick_hats[hat] = value;
	devices.joystick_hat_available[hat] = true;
}

void set_gamepad_trigger_state(
	sl_open::input::GameplayDeviceState& devices,
	std::uint8_t trigger,
	std::int16_t value,
	bool pressed)
{
	if (trigger >= std::size(devices.gamepad_triggers))
	{
		return;
	}
	constexpr std::int16_t press_threshold = 16384;
	constexpr std::int16_t release_threshold = 8192;
	const bool previous = devices.gamepad_triggers[trigger];
	const bool active = previous
		? value > release_threshold
		: value >= press_threshold;
	if (pressed && active && !previous)
	{
		devices.gamepad_trigger_pressed[trigger] = true;
	}
	else if (!active)
	{
		devices.gamepad_trigger_pressed[trigger] = false;
	}
	devices.gamepad_triggers[trigger] = active;
}

std::uint8_t hat_direction_index(std::uint8_t value)
{
	switch (value)
	{
	case SDL_HAT_UP: return 0;
	case SDL_HAT_RIGHT: return 1;
	case SDL_HAT_DOWN: return 2;
	case SDL_HAT_LEFT: return 3;
	default: return 4;
	}
}

void add_mouse_wheel_ticks(
	sl_open::input::GameplayDeviceState& devices,
	std::size_t direction,
	std::int32_t amount)
{
	if (direction >= std::size(devices.mouse_wheel) || amount <= 0)
	{
		return;
	}
	const std::uint32_t total =
		static_cast<std::uint32_t>(devices.mouse_wheel[direction])
		+ static_cast<std::uint32_t>(amount);
	devices.mouse_wheel[direction] = static_cast<std::uint16_t>(
		std::min<std::uint32_t>(total, UINT16_MAX));
}

bool mouse_control_bound(const sl_open::Config& config, std::int16_t control)
{
	for (const sl_open::ControlBinding& binding : config.bindings)
	{
		if (binding.mouse_control == control)
		{
			return true;
		}
	}
	return false;
}

void close_game_controller(App& app)
{
	if (app.gamepad != nullptr)
	{
		SDL_CloseGamepad(app.gamepad);
		app.gamepad = nullptr;
	}
	if (app.joystick != nullptr)
	{
		SDL_CloseJoystick(app.joystick);
		app.joystick = nullptr;
	}
	app.controller_instance = 0;
	std::fill(
		std::begin(app.gameplay_devices.joystick_buttons),
		std::end(app.gameplay_devices.joystick_buttons),
		false);
	std::fill(
		std::begin(app.gameplay_devices.joystick_pressed),
		std::end(app.gameplay_devices.joystick_pressed),
		false);
	std::fill(
		std::begin(app.gameplay_devices.joystick_hats),
		std::end(app.gameplay_devices.joystick_hats),
		SDL_HAT_CENTERED);
	std::fill(
		std::begin(app.gameplay_devices.joystick_hat_pressed),
		std::end(app.gameplay_devices.joystick_hat_pressed),
		0);
	std::fill(
		std::begin(app.gameplay_devices.joystick_hat_available),
		std::end(app.gameplay_devices.joystick_hat_available),
		false);
	std::fill(
		std::begin(app.gameplay_devices.gamepad_triggers),
		std::end(app.gameplay_devices.gamepad_triggers),
		false);
	std::fill(
		std::begin(app.gameplay_devices.gamepad_trigger_pressed),
		std::end(app.gameplay_devices.gamepad_trigger_pressed),
		false);
	std::fill(
		std::begin(app.gameplay_devices.joystick_axes),
		std::end(app.gameplay_devices.joystick_axes),
		0);
	std::fill(
		std::begin(app.gameplay_devices.joystick_axis_available),
		std::end(app.gameplay_devices.joystick_axis_available),
		false);
}

void refresh_game_controller_state(App& app)
{
	std::fill(
		std::begin(app.gameplay_devices.joystick_buttons),
		std::end(app.gameplay_devices.joystick_buttons),
		false);
	std::fill(
		std::begin(app.gameplay_devices.joystick_pressed),
		std::end(app.gameplay_devices.joystick_pressed),
		false);
	std::fill(
		std::begin(app.gameplay_devices.joystick_hats),
		std::end(app.gameplay_devices.joystick_hats),
		SDL_HAT_CENTERED);
	std::fill(
		std::begin(app.gameplay_devices.joystick_hat_pressed),
		std::end(app.gameplay_devices.joystick_hat_pressed),
		0);
	std::fill(
		std::begin(app.gameplay_devices.joystick_hat_available),
		std::end(app.gameplay_devices.joystick_hat_available),
		false);
	std::fill(
		std::begin(app.gameplay_devices.gamepad_triggers),
		std::end(app.gameplay_devices.gamepad_triggers),
		false);
	std::fill(
		std::begin(app.gameplay_devices.gamepad_trigger_pressed),
		std::end(app.gameplay_devices.gamepad_trigger_pressed),
		false);
	std::fill(
		std::begin(app.gameplay_devices.joystick_axes),
		std::end(app.gameplay_devices.joystick_axes),
		0);
	std::fill(
		std::begin(app.gameplay_devices.joystick_axis_available),
		std::end(app.gameplay_devices.joystick_axis_available),
		false);
	if (app.gamepad != nullptr)
	{
		SDL_UpdateGamepads();
		const SDL_GamepadAxis gamepad_axes[] = {
			SDL_GAMEPAD_AXIS_LEFTX,
			SDL_GAMEPAD_AXIS_LEFTY,
			SDL_GAMEPAD_AXIS_RIGHTX,
		};
		const std::uint8_t retail_axes[] = {0, 1, 5};
		for (std::size_t index = 0;
			index < std::size(gamepad_axes);
			++index)
		{
			if (!SDL_GamepadHasAxis(app.gamepad, gamepad_axes[index]))
			{
				continue;
			}
			app.gameplay_devices.joystick_axes[retail_axes[index]] =
				SDL_GetGamepadAxis(app.gamepad, gamepad_axes[index]);
			app.gameplay_devices
				.joystick_axis_available[retail_axes[index]] = true;
		}
		const std::size_t button_count = std::min(
			std::size(app.gameplay_devices.joystick_buttons),
			static_cast<std::size_t>(SDL_GAMEPAD_BUTTON_COUNT));
		for (std::size_t button = 0; button < button_count; ++button)
		{
			const SDL_GamepadButton gamepad_button =
				static_cast<SDL_GamepadButton>(button);
			if (SDL_GamepadHasButton(app.gamepad, gamepad_button))
			{
				app.gameplay_devices.joystick_buttons[button] =
					SDL_GetGamepadButton(app.gamepad, gamepad_button);
			}
		}
		const SDL_GamepadButton dpad_buttons[] = {
			SDL_GAMEPAD_BUTTON_DPAD_UP,
			SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
			SDL_GAMEPAD_BUTTON_DPAD_DOWN,
			SDL_GAMEPAD_BUTTON_DPAD_LEFT,
		};
		bool dpad_available = false;
		for (const SDL_GamepadButton button : dpad_buttons)
		{
			dpad_available = dpad_available
				|| SDL_GamepadHasButton(app.gamepad, button);
		}
		if (dpad_available)
		{
			set_joystick_hat_state(
				app.gameplay_devices,
				0,
				gamepad_dpad_hat(app.gameplay_devices),
				false);
		}
		const SDL_GamepadAxis trigger_axes[] = {
			SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
			SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
		};
		for (std::size_t trigger = 0;
			trigger < std::size(trigger_axes);
			++trigger)
		{
			if (SDL_GamepadHasAxis(app.gamepad, trigger_axes[trigger]))
			{
				set_gamepad_trigger_state(
					app.gameplay_devices,
					static_cast<std::uint8_t>(trigger),
					SDL_GetGamepadAxis(app.gamepad, trigger_axes[trigger]),
					false);
			}
		}
		return;
	}

	if (app.joystick != nullptr)
	{
		SDL_UpdateJoysticks();
		const int axis_count = std::min(
			SDL_GetNumJoystickAxes(app.joystick),
			static_cast<int>(
				sl_open::input::kGameplayJoystickAxisCount));
		for (int axis = 0; axis < axis_count; ++axis)
		{
			app.gameplay_devices.joystick_axes[axis] =
				SDL_GetJoystickAxis(app.joystick, axis);
			app.gameplay_devices.joystick_axis_available[axis] = true;
		}
		const int button_count = std::min(
			SDL_GetNumJoystickButtons(app.joystick),
			static_cast<int>(
				std::size(
					app.gameplay_devices.joystick_buttons)));
		for (int button = 0; button < button_count; ++button)
		{
			app.gameplay_devices.joystick_buttons[button] =
				SDL_GetJoystickButton(app.joystick, button);
		}
		const int hat_count = std::min(
			SDL_GetNumJoystickHats(app.joystick),
			static_cast<int>(
				std::size(app.gameplay_devices.joystick_hats)));
		for (int hat = 0; hat < hat_count; ++hat)
		{
			set_joystick_hat_state(
				app.gameplay_devices,
				static_cast<std::uint8_t>(hat),
				SDL_GetJoystickHat(app.joystick, hat),
				false);
		}
	}
}

void open_game_controller(App& app)
{
	if (app.gamepad != nullptr || app.joystick != nullptr)
	{
		return;
	}

	int count = 0;
	SDL_JoystickID* identifiers = SDL_GetGamepads(&count);
	for (int index = 0; index < count && app.gamepad == nullptr; ++index)
	{
		app.gamepad = SDL_OpenGamepad(identifiers[index]);
	}
	SDL_free(identifiers);
	if (app.gamepad != nullptr)
	{
		app.controller_instance = SDL_GetGamepadID(app.gamepad);
		refresh_game_controller_state(app);
		const char* name = SDL_GetGamepadName(app.gamepad);
		SDL_Log(
			"Gamepad opened: %s",
			name != nullptr ? name : "unnamed");
		return;
	}

	count = 0;
	identifiers = SDL_GetJoysticks(&count);
	for (int index = 0; index < count && app.joystick == nullptr; ++index)
	{
		if (!SDL_IsGamepad(identifiers[index]))
		{
			app.joystick = SDL_OpenJoystick(identifiers[index]);
		}
	}
	SDL_free(identifiers);
	if (app.joystick != nullptr)
	{
		app.controller_instance = SDL_GetJoystickID(app.joystick);
		refresh_game_controller_state(app);
		const char* name = SDL_GetJoystickName(app.joystick);
		SDL_Log(
			"Joystick opened: %s",
			name != nullptr ? name : "unnamed");
	}
}

void draw_boot_screen(const App& app)
{
	bgfx::setViewRect(kMainView, 0, 0, bgfx::BackbufferRatio::Equal);
	bgfx::touch(kMainView);
	bgfx::dbgTextClear(0, false);
	bgfx::dbgTextPrintf(3, 2, 0x6f, "sl_open");
	bgfx::dbgTextPrintf(3, 4, 0x0f, "Portable reimplementation - foundation build");
	bgfx::dbgTextPrintf(3, 6, 0x0b, "SDL callback mode / bgfx renderer");
	bgfx::dbgTextPrintf(3, 8, 0x07, "Renderer: %s", bgfx::getRendererName(bgfx::getRendererType()));

	if (app.data_root_looks_valid)
	{
		bgfx::dbgTextPrintf(3, 11, 0x2f, "Game data found");
		bgfx::dbgTextPrintf(3, 12, 0x08, "%s", app.data_root);
		bgfx::dbgTextPrintf(
			3,
			14,
			0x0b,
			"VFS: %u archives, %u + %u + %u entries",
			app.vfs.archive_count,
			app.vfs.archive_count > 0 ? app.vfs.archives[0].entry_count : 0,
			app.vfs.archive_count > 1 ? app.vfs.archives[1].entry_count : 0,
			app.vfs.archive_count > 2 ? app.vfs.archives[2].entry_count : 0);
		if (app.frontend_assets_ready)
		{
			bgfx::dbgTextPrintf(
				3,
				16,
				0x2f,
				"Retail frontend decoded: %ux%u TGA / %u SPR shapes / %u strings",
				app.upload_assets.splash.width,
				app.upload_assets.splash.height,
				app.upload_assets.frontend_sprites.shape_count,
				app.language.highest_id + 1);
		}
		else
		{
			bgfx::dbgTextPrintf(3, 16, 0x4f, "Retail frontend asset validation failed");
		}
	}
	else
	{
		bgfx::dbgTextPrintf(3, 11, 0x4f, "Original StarLancer game data was not found.");
		bgfx::dbgTextPrintf(3, 13, 0x0f, "Start with: sl_open --data /path/to/game");
	}

	bgfx::dbgTextPrintf(3, 19, 0x08, "Escape closes this window.");
	bgfx::frame();
}

}

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv)
{
	SDL_SetAppMetadata("sl_open", "0.1.0", "org.sl_open.sl_open");
	if (!SDL_Init(
			SDL_INIT_VIDEO | SDL_INIT_EVENTS
			| SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD))
	{
		SDL_Log("SDL_Init failed: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}
	*appstate = &g_app;
	sl_open::config_defaults(g_app.config);
	parse_arguments(g_app, argc, argv);
	std::snprintf(
		g_app.data_root,
		sizeof(g_app.data_root),
		"%s",
		g_app.requested_data_root != nullptr
			? g_app.requested_data_root
			: ".");
	SDL_EMFS_Config filesystem_config;
	SDL_EMFS_InitConfig(&filesystem_config);
	filesystem_config.asset_root = g_app.data_root;
	filesystem_config.organization = "sl_open";
	filesystem_config.application = "sl_open";
	g_app.filesystem = SDL_EMFS_Create(&filesystem_config);
	if (g_app.filesystem == nullptr || !SDL_EMFS_WaitReady(g_app.filesystem))
	{
		SDL_Log("SDL_emfs initialization failed: %s", SDL_GetError());
		return sl_open::platform::display_start_error(
			g_app, StartupError::data_not_found)
			? SDL_APP_CONTINUE
			: SDL_APP_FAILURE;
	}
	sl_open::config_load(g_app.filesystem, g_app.config);
	sl_open::network::multiplayer_transport_initialize(
		g_app.multiplayer_transport);
	if (!sl_open::campaign::campaign_store_init(
			g_app.campaign_store, g_app.filesystem))
	{
		SDL_Log("Campaign index could not be loaded; starting with an empty list");
	}

	g_app.data_root_looks_valid =
		sl_open::io::game_root_looks_valid(g_app.filesystem);
	if (!g_app.data_root_looks_valid)
	{
		SDL_Log("Original StarLancer game data was not found");
		return sl_open::platform::display_start_error(
			g_app, StartupError::data_not_found)
			? SDL_APP_CONTINUE
			: SDL_APP_FAILURE;
	}

	g_app.data_root_looks_valid =
		sl_open::io::vfs_init(
			g_app.vfs,
			g_app.filesystem,
			g_app.loose_first)
		&& sl_open::io::vfs_mount_archive(g_app.vfs, "resource.hog")
		&& sl_open::io::vfs_mount_archive(g_app.vfs, "CD1.HOG")
		&& sl_open::io::vfs_mount_archive(g_app.vfs, "CD2.HOG")
		&& sl_open::io::vfs_mount_archive(
			g_app.vfs, "ms_speech/msspeech.hog")
		&& sl_open::io::vfs_mount_archive(
			g_app.vfs, "pilots/pilots.hog");
	if (!g_app.data_root_looks_valid)
	{
		SDL_Log("Required StarLancer game archives could not be opened");
		return sl_open::platform::display_start_error(
			g_app, StartupError::archive_error)
			? SDL_APP_CONTINUE
			: SDL_APP_FAILURE;
	}

	SDL_Log(
		"VFS mounted %u archives (%u, %u, %u entries)",
		g_app.vfs.archive_count,
		g_app.vfs.archives[0].entry_count,
		g_app.vfs.archives[1].entry_count,
		g_app.vfs.archives[2].entry_count);
	if (!sl_open::audio::fat_bank_open(
			g_app.vfs, "stdsmp.fat", g_app.standard_sounds))
	{
		SDL_Log("Standard frontend sound bank could not be opened");
	}
	if (!sl_open::audio::fat_bank_open(
			g_app.vfs, "smp3d.fat", g_app.spatial_sounds))
	{
		SDL_Log("Spatial gameplay sound bank could not be opened");
	}
	if (!sl_open::audio::fat_bank_open_from_archive(
			g_app.vfs, "CD1.HOG", "vrsnd.fat", g_app.vr_sounds))
	{
		SDL_Log("VR ambience sound bank could not be opened");
	}
	if (!sl_open::audio::fat_bank_open_from_archive(
			g_app.vfs, "CD1.HOG", "wlksmp.fat", g_app.walk_sounds))
	{
		SDL_Log("VR transition sound bank could not be opened");
	}
	if (!sl_open::audio::fat_bank_open_from_archive(
			g_app.vfs, "CD1.HOG", "vrsfx.fat", g_app.briefing_sounds))
	{
		SDL_Log("Briefing transition sound bank could not be opened");
	}
	if (!sl_open::audio::fat_bank_open(
			g_app.vfs, "waitloop.fat", g_app.briefing_wait_sounds))
	{
		SDL_Log("Briefing clearance sound bank could not be opened");
	}
	if (!sl_open::audio::fat_bank_open(
			g_app.vfs, "ldsmp.fat", g_app.loadout_sounds))
	{
		SDL_Log("Loadout sound bank could not be opened");
	}
	g_app.frontend_assets_ready = sl_open::platform::frontend_load_assets(g_app);

	g_app.joystick_available = SDL_HasJoystick() || SDL_HasGamepad();
	open_game_controller(g_app);
	if (g_app.config.controller == 0 && !g_app.joystick_available)
	{
		g_app.config.controller = 2;
	}
	if (sl_open::audio::init(g_app.audio))
	{
		apply_audio_config(g_app);
		SDL_Log(
			"OpenAL initialized: %s (%u voices, %u streams, HRTF %s)",
			sl_open::audio::device_name(g_app.audio),
			g_app.audio.voice_count,
			g_app.audio.stream_count,
			g_app.audio.hrtf_supported ? "available" : "unavailable");
	}
	else
	{
		SDL_Log("OpenAL initialization failed; continuing without audio");
	}
	if (!sl_open::platform::display_create(g_app))
	{
		return SDL_APP_FAILURE;
	}
	if (g_app.frontend_assets_ready)
	{
		if (!sl_open::platform::frontend_upload_assets(g_app))
		{
			return SDL_APP_FAILURE;
		}
		const sl_open::render::FrontendTextureStats texture_stats =
			sl_open::render::frontend_texture_stats();
		SDL_Log(
			"Frontend GPU textures: created=%u live=%u high-water=%u",
			texture_stats.created,
			texture_stats.live,
			texture_stats.high_water);
		bgfx::setDebug(BGFX_DEBUG_NONE);
		SDL_HideCursor();
		const std::uint64_t now = SDL_GetTicks();
			sl_open::frontend::campaign_frontend_init(
				g_app.campaign_frontend,
				g_app.campaign_store,
				sl_open::language_text(g_app.language, 0xbf),
				now);
			std::snprintf(
				g_app.multiplayer_callsign,
				sizeof(g_app.multiplayer_callsign),
				"%s",
				sl_open::language_text(g_app.language, 0xbf));
			(void)initialize_multiplayer_local_profile(g_app);
			std::snprintf(
				g_app.multiplayer_session_name,
				sizeof(g_app.multiplayer_session_name),
				"%s%s",
				g_app.multiplayer_callsign,
				sl_open::language_text(g_app.language, 0x414));
			advance_startup_logos(g_app, now);
	}

	SDL_Log("sl_open initialized with %s", bgfx::getRendererName(bgfx::getRendererType()));
	return SDL_APP_CONTINUE;
}

SDL_AppResult handle_input_event(
	App& app,
	const sl_open::platform::InputEvent& event)
{
	if (event.type == SDL_EVENT_GAMEPAD_ADDED
		|| (event.type == SDL_EVENT_JOYSTICK_ADDED
			&& !SDL_IsGamepad(event.jdevice.which)))
	{
		open_game_controller(app);
		app.joystick_available = SDL_HasJoystick() || SDL_HasGamepad();
	}
	if ((event.type == SDL_EVENT_GAMEPAD_REMOVED
			&& event.gdevice.which == app.controller_instance)
		|| (event.type == SDL_EVENT_JOYSTICK_REMOVED
			&& event.jdevice.which == app.controller_instance))
	{
		close_game_controller(app);
		open_game_controller(app);
		app.joystick_available = SDL_HasJoystick() || SDL_HasGamepad();
	}
	if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
	{
		app.gameplay_devices = {};
		if (app.config.pause_in_background
			&& app.frontend_phase == FrontendPhase::instant_action
			&& !app.mission_foster_active
			&& app.mission_session.state
				== sl_open::game::MissionSessionState::running
			&& app.mission_session.result.kind
				== sl_open::game::SessionResultKind::none)
		{
			const bool bad_link_pause_pending =
				app.mission_session.network_pause_transition_pending
				&& app.mission_session
					.network_pause_transition_active
				&& app.mission_session.network_pause_reason
					== sl_open::mission::NetworkPauseReason::
						connection_stall;
			if (!bad_link_pause_pending)
			{
				sl_open::game::mission_session_set_pause(
					app.mission_session,
					true,
					app.width,
					app.height,
					SDL_GetTicks(),
					sl_open::mission::NetworkPauseReason::
						player_request);
			}
		}
	}
	else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)
	{
		refresh_game_controller_state(app);
	}
	if ((event.type == SDL_EVENT_KEY_DOWN
			|| event.type == SDL_EVENT_KEY_UP)
		&& event.key.scancode >= 0
		&& event.key.scancode < SDL_SCANCODE_COUNT)
	{
		if (event.type == SDL_EVENT_KEY_DOWN
			&& !event.key.repeat
			&& !app.gameplay_devices.keyboard[event.key.scancode])
		{
			app.gameplay_devices.keyboard_pressed[event.key.scancode] = true;
		}
		app.gameplay_devices.keyboard[event.key.scancode] =
			event.type == SDL_EVENT_KEY_DOWN;
		if (event.type == SDL_EVENT_KEY_UP)
		{
			app.gameplay_devices.keyboard_pressed[event.key.scancode] =
				false;
		}
	}
	if ((event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN
			|| event.type == SDL_EVENT_JOYSTICK_BUTTON_UP)
		&& app.joystick != nullptr
		&& event.jbutton.which == app.controller_instance
		&& event.jbutton.button
			< std::size(app.gameplay_devices.joystick_buttons))
	{
		if (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN
			&& !app.gameplay_devices.joystick_buttons[event.jbutton.button])
		{
			app.gameplay_devices.joystick_pressed[event.jbutton.button] = true;
		}
		app.gameplay_devices.joystick_buttons[event.jbutton.button] =
			event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN;
		if (event.type == SDL_EVENT_JOYSTICK_BUTTON_UP)
		{
			app.gameplay_devices.joystick_pressed[event.jbutton.button] =
				false;
		}
	}
	if ((event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
			|| event.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
		&& app.gamepad != nullptr
		&& event.gbutton.which == app.controller_instance
		&& event.gbutton.button
			< std::size(app.gameplay_devices.joystick_buttons))
	{
		if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
			&& !app.gameplay_devices.joystick_buttons[event.gbutton.button])
		{
			app.gameplay_devices.joystick_pressed[event.gbutton.button] = true;
		}
		app.gameplay_devices.joystick_buttons[event.gbutton.button] =
			event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
		if (event.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
		{
			app.gameplay_devices.joystick_pressed[event.gbutton.button] =
				false;
		}
		if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP
			|| event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT
			|| event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN
			|| event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT)
		{
			set_joystick_hat_state(
				app.gameplay_devices,
				0,
				gamepad_dpad_hat(app.gameplay_devices),
				true);
		}
	}
	if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
			|| event.type == SDL_EVENT_MOUSE_BUTTON_UP)
		&& event.button.button
			< std::size(app.gameplay_devices.mouse_buttons))
	{
		const std::uint8_t button = event.button.button;
		if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
			&& !app.gameplay_devices.mouse_buttons[button])
		{
			app.gameplay_devices.mouse_pressed[button] = true;
		}
		app.gameplay_devices.mouse_buttons[button] =
			event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
		if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
		{
			app.gameplay_devices.mouse_pressed[button] = false;
		}
	}
	if (event.type == SDL_EVENT_JOYSTICK_AXIS_MOTION
		&& app.joystick != nullptr
		&& event.jaxis.which == app.controller_instance
		&& event.jaxis.axis
			< sl_open::input::kGameplayJoystickAxisCount)
	{
		app.gameplay_devices.joystick_axes[event.jaxis.axis] =
			event.jaxis.value;
		app.gameplay_devices.joystick_axis_available[event.jaxis.axis] =
			true;
	}
	if (event.type == SDL_EVENT_JOYSTICK_HAT_MOTION
		&& app.joystick != nullptr
		&& event.jhat.which == app.controller_instance
		&& event.jhat.hat
			< std::size(app.gameplay_devices.joystick_hats))
	{
		set_joystick_hat_state(
			app.gameplay_devices,
			event.jhat.hat,
			event.jhat.value,
			true);
	}
	if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION
		&& app.gamepad != nullptr
		&& event.gaxis.which == app.controller_instance)
	{
		if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER
			|| event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
		{
			set_gamepad_trigger_state(
				app.gameplay_devices,
				event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 0 : 1,
				event.gaxis.value,
				true);
		}
		std::uint8_t retail_axis =
			static_cast<std::uint8_t>(
				sl_open::input::kGameplayJoystickAxisCount);
		switch (event.gaxis.axis)
		{
		case SDL_GAMEPAD_AXIS_LEFTX:
			retail_axis = 0;
			break;
		case SDL_GAMEPAD_AXIS_LEFTY:
			retail_axis = 1;
			break;
		case SDL_GAMEPAD_AXIS_RIGHTX:
			// Project SDL's standardized secondary horizontal rotation
			// control onto the DirectInput Rz slot consumed by retail
			// twist control.
			retail_axis = 5;
			break;
		default:
			break;
		}
		if (retail_axis < sl_open::input::kGameplayJoystickAxisCount)
		{
			app.gameplay_devices.joystick_axes[retail_axis] =
				event.gaxis.value;
			app.gameplay_devices.joystick_axis_available[retail_axis] =
				true;
		}
	}
	if (event.type == SDL_EVENT_MOUSE_WHEEL
		&& app.frontend_phase == FrontendPhase::instant_action
		&& app.mission_session.state
			== sl_open::game::MissionSessionState::running)
	{
		const std::int32_t vertical = event.wheel.integer_y != 0
			? event.wheel.integer_y
			: event.wheel.y >= 1.0f ? 1 : event.wheel.y <= -1.0f ? -1 : 0;
		const std::int32_t horizontal = event.wheel.integer_x != 0
			? event.wheel.integer_x
			: event.wheel.x >= 1.0f ? 1 : event.wheel.x <= -1.0f ? -1 : 0;
		const std::int16_t vertical_control = vertical > 0
			? sl_open::kControlMouseWheelUp
			: sl_open::kControlMouseWheelDown;
		if (vertical != 0
			&& mouse_control_bound(app.config, vertical_control))
		{
			add_mouse_wheel_ticks(
				app.gameplay_devices,
				vertical > 0 ? 0 : 1,
				std::abs(vertical));
		}
		const std::int16_t horizontal_control = horizontal < 0
			? sl_open::kControlMouseWheelLeft
			: sl_open::kControlMouseWheelRight;
		if (horizontal != 0
			&& mouse_control_bound(app.config, horizontal_control))
		{
			add_mouse_wheel_ticks(
				app.gameplay_devices,
				horizontal < 0 ? 2 : 3,
				std::abs(horizontal));
		}
	}
	if (event.type == SDL_EVENT_MOUSE_MOTION
		&& app.frontend_phase == FrontendPhase::instant_action
		&& app.mission_session.state
			== sl_open::game::MissionSessionState::running
		&& sl_open::controller_uses_mouse(app.config.controller))
	{
		app.gameplay_devices.mouse_relative_x +=
			event.motion.x_relative;
		app.gameplay_devices.mouse_relative_y +=
			event.motion.y_relative;
	}
	if (app.startup_error != StartupError::none)
	{
		const bool close_key =
			event.type == SDL_EVENT_KEY_DOWN
			&& !event.key.repeat
			&& (event.key.scancode == SDL_SCANCODE_ESCAPE
				|| event.key.scancode == SDL_SCANCODE_RETURN
				|| event.key.scancode == SDL_SCANCODE_SPACE);
		if (event.type == SDL_EVENT_QUIT
			|| close_key
			|| event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
		{
			return SDL_APP_SUCCESS;
		}
		return SDL_APP_CONTINUE;
	}
	if (app.frontend_phase == FrontendPhase::options
		&& app.options.page == sl_open::frontend::OptionsPage::controls
		&& app.options.capture_action >= 0)
	{
		if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
		{
			if (event.key.scancode == SDL_SCANCODE_ESCAPE)
			{
				app.options.capture_action = -1;
				return SDL_APP_CONTINUE;
			}
			switch (event.key.scancode)
			{
			case SDL_SCANCODE_LSHIFT:
			case SDL_SCANCODE_RSHIFT:
			case SDL_SCANCODE_LCTRL:
			case SDL_SCANCODE_RCTRL:
			case SDL_SCANCODE_LALT:
			case SDL_SCANCODE_RALT:
				return SDL_APP_CONTINUE;
			default:
				break;
			}
			std::uint8_t modifier = 0;
			if ((event.key.mod & SDL_KMOD_SHIFT) != 0) modifier = 1;
			else if ((event.key.mod & SDL_KMOD_CTRL) != 0) modifier = 2;
			else if ((event.key.mod & SDL_KMOD_ALT) != 0) modifier = 3;
			sl_open::frontend::options_capture_key(
				app.options,
				app.config,
				static_cast<std::uint16_t>(event.key.scancode),
				modifier);
			return SDL_APP_CONTINUE;
		}
		if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
		{
			sl_open::frontend::options_capture_joystick(
				app.options,
				app.config,
				static_cast<std::int16_t>(event.gbutton.button));
			return SDL_APP_CONTINUE;
		}
		if (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN)
		{
			sl_open::frontend::options_capture_joystick(
				app.options,
				app.config,
				static_cast<std::int16_t>(event.jbutton.button));
			return SDL_APP_CONTINUE;
		}
		if (event.type == SDL_EVENT_JOYSTICK_HAT_MOTION
			&& event.jhat.hat < sl_open::kControlJoystickHatCount)
		{
			const std::uint8_t direction =
				hat_direction_index(event.jhat.value);
			if (direction < 4)
			{
				sl_open::frontend::options_capture_joystick(
					app.options,
					app.config,
					sl_open::control_joystick_hat(
						event.jhat.hat, direction));
				return SDL_APP_CONTINUE;
			}
		}
		if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION
			&& event.gaxis.value >= 16384
			&& (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER
				|| event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
		{
			sl_open::frontend::options_capture_joystick(
				app.options,
				app.config,
				event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER
					? sl_open::kControlJoystickLeftTrigger
					: sl_open::kControlJoystickRightTrigger);
			return SDL_APP_CONTINUE;
		}
		if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
			&& event.button.button > 0
			&& event.button.button <= sl_open::kControlMouseButtonCount)
		{
			sl_open::frontend::options_capture_mouse(
				app.options, app.config, event.button.button);
			std::fill(
				std::begin(app.gameplay_devices.mouse_wheel),
				std::end(app.gameplay_devices.mouse_wheel),
				0);
			return SDL_APP_CONTINUE;
		}
		if (event.type == SDL_EVENT_MOUSE_WHEEL)
		{
			std::int16_t control = -1;
			if (event.wheel.y > 0.0f) control = sl_open::kControlMouseWheelUp;
			else if (event.wheel.y < 0.0f) control = sl_open::kControlMouseWheelDown;
			else if (event.wheel.x < 0.0f) control = sl_open::kControlMouseWheelLeft;
			else if (event.wheel.x > 0.0f) control = sl_open::kControlMouseWheelRight;
			if (control >= 0)
			{
				sl_open::frontend::options_capture_mouse(
					app.options, app.config, control);
				app.options.control_wheel_suppressed_until =
					SDL_GetTicks() + kControlWheelQuietPeriodMs;
				std::fill(
					std::begin(app.gameplay_devices.mouse_wheel),
					std::end(app.gameplay_devices.mouse_wheel),
					0);
				return SDL_APP_CONTINUE;
			}
		}
	}
	if (app.frontend_phase == FrontendPhase::instant_action
		&& (app.mission_session.state
				== sl_open::game::MissionSessionState::running
			|| (app.mission_session.state
					== sl_open::game::MissionSessionState::paused
				&& app.mission_session.pause.mode
					== sl_open::frontend::GameplayPauseMode::online))
		&& app.mission_session.hud.chat_active)
	{
		// CommsMenu_text_input owns text, Backspace, and the main Return
		// scancode before the ordinary 3D-loop instant-action keys see
		// them. Return sends even an empty editor buffer.
		if (event.type == SDL_EVENT_TEXT_INPUT)
		{
			(void)sl_open::game::mission_session_chat_text(
				app.mission_session,
				app.frontend_renderer,
				event.text.text);
			return SDL_APP_CONTINUE;
		}
		if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
		{
			if (event.key.scancode == SDL_SCANCODE_BACKSPACE)
			{
				sl_open::game::mission_session_chat_backspace(
					app.mission_session);
				return SDL_APP_CONTINUE;
			}
			if (event.key.scancode == SDL_SCANCODE_RETURN)
			{
				if (sl_open::game::mission_session_chat_submit(
						app.mission_session))
				{
					SDL_StopTextInput(app.window);
				}
				return SDL_APP_CONTINUE;
			}
		}
	}
	if (app.frontend_phase == FrontendPhase::instant_action
		&& app.mission_foster_active
		&& (event.type == SDL_EVENT_KEY_DOWN
			|| event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
			|| event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
			|| event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN))
	{
		sl_open::media::bink_movie_skip_to_end(
			app.vfs, app.mission_foster_movie);
		return SDL_APP_CONTINUE;
	}
	if (app.frontend_phase == FrontendPhase::instant_action
		&& event.type == SDL_EVENT_KEY_DOWN
		&& !event.key.repeat
		&& event.key.scancode == SDL_SCANCODE_ESCAPE)
	{
		if (app.mission_session.state
			== sl_open::game::MissionSessionState::load_failed)
		{
			sl_open::game::mission_session_request_exit(app.mission_session);
			apply_instant_action_result(app, SDL_GetTicks());
		}
		else if (app.mission_session.state
			== sl_open::game::MissionSessionState::paused)
		{
			sl_open::input::gameplay_input_gate_fire_until_key_released(
				app.gameplay_devices,
				SDL_SCANCODE_ESCAPE);
			// The pause frontend owns this Escape edge. Consume the shared
			// gameplay latch before resuming so Player_update cannot reopen
			// the pause screen with the same physical key press below.
			app.gameplay_devices.keyboard_pressed[
				SDL_SCANCODE_ESCAPE] = false;
			sl_open::game::mission_session_toggle_pause(
				app.mission_session,
				app.width,
				app.height,
				SDL_GetTicks());
		}
		// A running mission owns Escape through the same shared physical-key
		// latch as every other gameplay pressed query. The admitted-frame
		// owner below performs that query before action 73 and all simulation.
		return SDL_APP_CONTINUE;
	}
	const bool skip_key = event.type == SDL_EVENT_KEY_DOWN
		&& !event.key.repeat
		&& (event.key.scancode == SDL_SCANCODE_ESCAPE
			|| event.key.scancode == SDL_SCANCODE_RETURN
			|| event.key.scancode == SDL_SCANCODE_SPACE);
	if (app.frontend_phase == FrontendPhase::credits
		&& (event.type == SDL_EVENT_KEY_DOWN
			|| event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
			|| event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
			|| event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN))
	{
		finish_credits(app, SDL_GetTicks());
		return SDL_APP_CONTINUE;
	}
	if (app.frontend_phase == FrontendPhase::campaign_induction)
	{
		const bool narration_skip =
			event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
			|| (event.type == SDL_EVENT_KEY_DOWN
				&& !event.key.repeat
				&& (event.key.scancode == SDL_SCANCODE_RETURN
					|| event.key.scancode == SDL_SCANCODE_SPACE));
		if (event.type == SDL_EVENT_KEY_DOWN
			&& !event.key.repeat
			&& event.key.scancode == SDL_SCANCODE_ESCAPE)
		{
			sl_open::audio::cb97_stream_close(app.vfs, app.campaign_speech);
			app.transition_destination =
				TransitionDestination::campaign_hub;
			enter_transition_destination(app, SDL_GetTicks());
			return SDL_APP_CONTINUE;
		}
		if (narration_skip
			&& app.campaign_induction.phase
				== sl_open::frontend::InductionPhase::narration)
		{
			finish_induction_narration(app, SDL_GetTicks());
			return SDL_APP_CONTINUE;
		}
	}
	if ((app.frontend_phase == FrontendPhase::campaign_hub
			|| (app.frontend_phase == FrontendPhase::campaign_briefing
				&& app.mission_briefing.phase
					== sl_open::frontend::MissionBriefingPhase::hologram))
		&& event.type == SDL_EVENT_KEY_DOWN
		&& !event.key.repeat
		&& event.key.scancode == SDL_SCANCODE_ESCAPE)
	{
		enter_in_game_options(app, SDL_GetTicks());
		return SDL_APP_CONTINUE;
	}
	if ((skip_key || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
		&& app.frontend_phase == FrontendPhase::transition
		&& app.transition_skips_directly)
	{
		enter_transition_destination(app, SDL_GetTicks());
		return SDL_APP_CONTINUE;
	}
	if ((skip_key || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
		&& interrupt_frontend_transition(app))
	{
		return SDL_APP_CONTINUE;
	}
	if (app.frontend_phase == FrontendPhase::campaign_medals
		&& event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
	{
		close_medal_display(app, SDL_GetTicks());
		return SDL_APP_CONTINUE;
	}
	if (app.frontend_phase == FrontendPhase::campaign_briefing
		&& (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || skip_key))
	{
		const std::uint64_t now = SDL_GetTicks();
		if (app.mission_briefing.phase
			== sl_open::frontend::MissionBriefingPhase::hologram)
		{
			if (sl_open::frontend::mission_briefing_begin_hologram_exit(
				app.mission_briefing, now))
			{
				open_mission_briefing_movie(app, now);
			}
		}
		else if (app.mission_briefing.phase
			!= sl_open::frontend::MissionBriefingPhase::awaiting_clearance
			&& app.mission_briefing.phase
				!= sl_open::frontend::MissionBriefingPhase::exit_room)
		{
			advance_mission_briefing_movie(app, now);
		}
		return SDL_APP_CONTINUE;
	}
	if (app.frontend_phase == FrontendPhase::campaign_news
		&& (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || skip_key))
	{
		leave_news_report(app, SDL_GetTicks());
		return SDL_APP_CONTINUE;
	}
	if (app.frontend_phase == FrontendPhase::campaign_award_movie
		&& (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || skip_key))
	{
		if (app.multiplayer_mission_presentation
			== sl_open::platform::MultiplayerMissionPresentation::award)
		{
			finish_multiplayer_award_movie(
				app, SDL_GetTicks());
		}
		else
		{
			route_after_mission(app, SDL_GetTicks());
		}
		return SDL_APP_CONTINUE;
	}
	if (app.frontend_phase == FrontendPhase::campaign_post_mission_movie
		&& (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || skip_key))
	{
		if (app.campaign_sequence.audio_bank != nullptr
			&& std::strcmp(
				app.campaign_sequence.audio_bank,
				"newsloop.fat") != 0)
		{
			finish_campaign_sequence(app, SDL_GetTicks());
		}
		else if (!app.campaign_sequence_waiting_audio)
		{
			sl_open::media::bink_movie_finish(app.campaign_movie);
		}
		return SDL_APP_CONTINUE;
	}
	if (app.frontend_phase == FrontendPhase::campaign_takeoff_movie
		&& (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || skip_key))
	{
		launch_campaign_mission(app, SDL_GetTicks());
		return SDL_APP_CONTINUE;
	}
	if (app.frontend_phase == FrontendPhase::campaign_failure_movie
		&& (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || skip_key))
	{
		enter_post_mission_choice(app, SDL_GetTicks());
		return SDL_APP_CONTINUE;
	}
	if (app.frontend_phase == FrontendPhase::campaign_itac
		&& app.itac_shell.phase != sl_open::frontend::ItacPhase::active
		&& (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || skip_key))
	{
		finish_itac_movie(app, SDL_GetTicks());
		return SDL_APP_CONTINUE;
	}
	switch (event.type)
	{
	case SDL_EVENT_QUIT:
		return SDL_APP_SUCCESS;
		case SDL_EVENT_KEY_DOWN:
			if (app.frontend_phase
					== FrontendPhase::multiplayer_frontend
				&& app.multiplayer_frontend.screen
					== sl_open::frontend::MultiplayerScreen::direct_ip
				&& !event.key.repeat
				&& event.key.scancode
					== SDL_SCANCODE_BACKSPACE)
			{
				(void)sl_open::frontend::
					multiplayer_frontend_direct_ip_backspace(
						app.multiplayer_frontend);
				return SDL_APP_CONTINUE;
			}
			if (app.frontend_phase
					== FrontendPhase::multiplayer_debrief
				&& !event.key.repeat)
			{
				const sl_open::frontend::MultiplayerDebriefView view =
					multiplayer_debrief_view(app);
				if (event.key.scancode == SDL_SCANCODE_BACKSPACE)
				{
					(void)handle_multiplayer_debrief_action(
						app,
						sl_open::frontend::
							multiplayer_debrief_backspace(
								view),
						SDL_GetTicks());
					return SDL_APP_CONTINUE;
				}
				if (event.key.scancode == SDL_SCANCODE_RETURN
					|| event.key.scancode
						== SDL_SCANCODE_KP_ENTER)
				{
					(void)handle_multiplayer_debrief_action(
						app,
						sl_open::frontend::
							multiplayer_debrief_submit_chat(
								view),
						SDL_GetTicks());
					return SDL_APP_CONTINUE;
				}
				if (event.key.scancode == SDL_SCANCODE_ESCAPE)
				{
					sl_open::frontend::MultiplayerDebriefAction leave;
					leave.type =
						sl_open::frontend::
							MultiplayerDebriefActionType::leave;
					(void)handle_multiplayer_debrief_action(
						app, leave, SDL_GetTicks());
					return SDL_APP_CONTINUE;
				}
			}
			if (app.frontend_phase == FrontendPhase::multiplayer_lobby
				&& event.key.scancode == SDL_SCANCODE_BACKSPACE)
			{
				const sl_open::frontend::MultiplayerLobbyView view =
					multiplayer_lobby_view(app);
				(void)handle_multiplayer_lobby_action(
					app,
					sl_open::frontend::multiplayer_lobby_backspace(
						app.multiplayer_lobby, view),
					SDL_GetTicks());
				return SDL_APP_CONTINUE;
			}
			if (app.frontend_phase == FrontendPhase::multiplayer_lobby
				&& (event.key.scancode == SDL_SCANCODE_RETURN
					|| event.key.scancode == SDL_SCANCODE_KP_ENTER))
			{
				const sl_open::frontend::MultiplayerLobbyView view =
					multiplayer_lobby_view(app);
				(void)handle_multiplayer_lobby_action(
					app,
					sl_open::frontend::multiplayer_lobby_submit_chat(
						app.multiplayer_lobby, view),
					SDL_GetTicks());
				return SDL_APP_CONTINUE;
			}
			if (app.frontend_phase == FrontendPhase::campaign
				&& event.key.scancode == SDL_SCANCODE_BACKSPACE)
			{
				sl_open::frontend::campaign_frontend_backspace(app.campaign_frontend);
				return SDL_APP_CONTINUE;
			}
			if (app.frontend_phase
					== FrontendPhase::multiplayer_campaign_load
				&& event.key.scancode == SDL_SCANCODE_BACKSPACE)
			{
				sl_open::frontend::campaign_frontend_backspace(
					app.campaign_frontend);
				return SDL_APP_CONTINUE;
			}
			if (app.frontend_phase == FrontendPhase::campaign
				&& event.key.scancode == SDL_SCANCODE_RETURN
			&& app.campaign_frontend.screen
				== sl_open::frontend::CampaignScreen::save_load
			&& app.campaign_frontend.save_name_editing)
		{
			app.campaign_frontend.hovered = 16;
			const sl_open::frontend::CampaignSelection selection =
				sl_open::frontend::campaign_frontend_select(
					app.campaign_frontend,
					app.campaign_store,
					app.campaign);
			if (selection == sl_open::frontend::CampaignSelection::save_complete
				&& app.campaign_frontend.save_load_in_game)
			{
				finish_in_game_save_load(
					app, true, SDL_GetTicks());
				}
				return SDL_APP_CONTINUE;
			}
			if (app.frontend_phase
					== FrontendPhase::multiplayer_campaign_load
				&& event.key.scancode == SDL_SCANCODE_RETURN
				&& app.campaign_frontend.screen
					== sl_open::frontend::CampaignScreen::save_load
				&& app.campaign_frontend.save_name_editing)
			{
				app.campaign_frontend.hovered = 16;
				const sl_open::frontend::CampaignSelection selection =
					sl_open::frontend::campaign_frontend_select(
						app.campaign_frontend,
						app.campaign_store,
						app.multiplayer_campaign);
				if (apply_multiplayer_campaign_selection(
						app, selection, SDL_GetTicks()))
				{
					return SDL_APP_SUCCESS;
				}
				return SDL_APP_CONTINUE;
			}
			if (event.key.scancode == SDL_SCANCODE_ESCAPE)
			{
				if (app.frontend_phase
					== FrontendPhase::multiplayer_frontend)
				{
					sl_open::network::multiplayer_transport_end_discovery(
						app.multiplayer_transport);
					begin_transition(
						app,
						sl_open::frontend::kMultiplayerBackMovie,
						TransitionDestination::main_menu,
						SDL_GetTicks());
					return SDL_APP_CONTINUE;
				}
				if (app.frontend_phase
					== FrontendPhase::multiplayer_lobby)
				{
					leave_multiplayer_for_browser(app, SDL_GetTicks());
					return SDL_APP_CONTINUE;
				}
				if (app.frontend_phase
					== FrontendPhase::multiplayer_campaign_load)
				{
					const sl_open::frontend::CampaignSelection selection =
						sl_open::frontend::campaign_frontend_back(
							app.campaign_frontend);
					if (selection
							== sl_open::frontend::CampaignSelection::main_menu
						|| selection
							== sl_open::frontend::CampaignSelection::single_player)
					{
						leave_multiplayer_campaign_browser(
							app, SDL_GetTicks());
					}
					return SDL_APP_CONTINUE;
				}
				if (app.frontend_phase == FrontendPhase::campaign_cd)
			{
				leave_cd_player(app, SDL_GetTicks());
				return SDL_APP_CONTINUE;
			}
			if (app.frontend_phase == FrontendPhase::campaign_sim_pod)
			{
				if (sl_open::frontend::sim_pod_begin_exit(app.sim_pod))
				{
					if (app.sim_pod.phase
						== sl_open::frontend::SimPodPhase::complete)
					{
						leave_sim_pod(app, SDL_GetTicks());
					}
					else
					{
						open_sim_pod_movie(app, SDL_GetTicks());
					}
				}
				return SDL_APP_CONTINUE;
			}
			if (app.frontend_phase == FrontendPhase::campaign_medals)
			{
				close_medal_display(app, SDL_GetTicks());
				return SDL_APP_CONTINUE;
			}
			if (app.frontend_phase == FrontendPhase::campaign_itac)
			{
				if (sl_open::frontend::itac_shell_exit(app.itac_shell))
				{
					open_itac_movie(app, SDL_GetTicks());
				}
				return SDL_APP_CONTINUE;
			}
				if (app.frontend_phase == FrontendPhase::campaign_loadout)
				{
				const sl_open::frontend::LoadoutClickResult result =
					sl_open::frontend::loadout_launch(
					app.loadout, app.campaign, SDL_GetTicks());
				play_loadout_click_sounds(app, result);
				if (result.launch)
				{
					begin_mission_loadout_exit(app, SDL_GetTicks());
				}
				return SDL_APP_CONTINUE;
			}
			if (app.frontend_phase == FrontendPhase::campaign_hub
				&& app.campaign_hub.fish_active)
			{
				enter_hub_node(
					app,
					app.campaign_hub.late_campaign ? 50 : 0,
					SDL_GetTicks());
				return SDL_APP_CONTINUE;
			}
				if (app.frontend_phase == FrontendPhase::campaign)
				{
					const sl_open::frontend::CampaignSelection selection =
						sl_open::frontend::campaign_frontend_back(app.campaign_frontend);
				if (selection == sl_open::frontend::CampaignSelection::main_menu)
				{
					begin_transition(
						app,
						"interface/sin2main.bik",
						TransitionDestination::main_menu,
						SDL_GetTicks());
				}
				else if (selection
					== sl_open::frontend::CampaignSelection::single_player)
				{
					if (app.campaign_frontend.save_load_in_game)
					{
						sl_open::frontend::options_enter_page(
							app.options,
							sl_open::frontend::OptionsPage::hub,
							app.config,
							SDL_GetTicks());
						begin_in_game_options_transition(
							app,
							"interface/igofade2.bik",
							TransitionDestination::options,
							SDL_GetTicks());
					}
					else
					{
						begin_transition(
							app,
							"interface/sifad2mm.bik",
							TransitionDestination::campaign,
							SDL_GetTicks());
					}
					}
					return SDL_APP_CONTINUE;
				}
				if (app.frontend_phase
					== FrontendPhase::multiplayer_loadout)
				{
					const sl_open::frontend::LoadoutClickResult result =
						sl_open::frontend::loadout_launch(
							app.loadout, app.multiplayer_campaign, SDL_GetTicks());
					play_loadout_click_sounds(app, result);
					if (result.launch)
					{
						begin_multiplayer_loadout_exit(
							app, SDL_GetTicks());
					}
					return SDL_APP_CONTINUE;
				}
				if (app.frontend_phase == FrontendPhase::options)
				{
					if (app.options.modal != sl_open::frontend::OptionsModal::none)
				{
					app.options.modal = sl_open::frontend::OptionsModal::none;
					app.options.hovered = -1;
					return SDL_APP_CONTINUE;
				}
				if (app.options.in_game)
				{
					if (app.options_return_phase
						== FrontendPhase::instant_action)
					{
						return_to_instant_action_pause(app);
						return SDL_APP_CONTINUE;
					}
					if (app.options.page != sl_open::frontend::OptionsPage::hub)
					{
						sl_open::frontend::options_enter_page(
							app.options,
							sl_open::frontend::OptionsPage::hub,
							app.config,
							SDL_GetTicks());
						begin_in_game_options_transition(
							app,
							"interface/igofade2.bik",
							TransitionDestination::options,
							SDL_GetTicks());
					}
					else
					{
						resume_in_game_options_owner(
							app, SDL_GetTicks());
						}
						return SDL_APP_CONTINUE;
					}
					if (app.options.page != sl_open::frontend::OptionsPage::hub)
					{
					sl_open::frontend::options_enter_page(
						app.options,
						sl_open::frontend::OptionsPage::hub,
						app.config,
						SDL_GetTicks());
					begin_transition(
						app,
						"interface/optfade2.bik",
						TransitionDestination::options,
						SDL_GetTicks());
					return SDL_APP_CONTINUE;
				}
				begin_transition(
					app,
					"interface/opt2main.bik",
					TransitionDestination::main_menu,
					SDL_GetTicks());
				return SDL_APP_CONTINUE;
			}
			if (app.frontend_phase == FrontendPhase::main_menu)
			{
				app.main_menu.quit_confirmation =
					!app.main_menu.quit_confirmation;
				app.main_menu.hovered = -1;
			}
			return SDL_APP_CONTINUE;
		}
		break;
		case SDL_EVENT_TEXT_INPUT:
			if (app.frontend_phase
					== FrontendPhase::multiplayer_frontend
				&& app.multiplayer_frontend.screen
					== sl_open::frontend::MultiplayerScreen::direct_ip)
			{
				(void)sl_open::frontend::
					multiplayer_frontend_direct_ip_text(
						app.multiplayer_frontend,
						event.text.text);
			}
			else if (app.frontend_phase
				== FrontendPhase::multiplayer_debrief)
			{
				const sl_open::frontend::MultiplayerDebriefView view =
					multiplayer_debrief_view(app);
				(void)handle_multiplayer_debrief_action(
					app,
					sl_open::frontend::multiplayer_debrief_text(
						view, event.text.text),
					SDL_GetTicks());
			}
			else if (app.frontend_phase
				== FrontendPhase::multiplayer_lobby)
			{
				const sl_open::frontend::MultiplayerLobbyView view =
					multiplayer_lobby_view(app);
				(void)handle_multiplayer_lobby_action(
					app,
					sl_open::frontend::multiplayer_lobby_text(
						app.multiplayer_lobby,
						view,
						event.text.text),
					SDL_GetTicks());
			}
			else if (app.frontend_phase == FrontendPhase::campaign)
			{
				sl_open::frontend::campaign_frontend_text(
					app.campaign_frontend, event.text.text);
			}
			else if (app.frontend_phase
				== FrontendPhase::multiplayer_campaign_load)
			{
				sl_open::frontend::campaign_frontend_text(
					app.campaign_frontend, event.text.text);
			}
			break;
	case SDL_EVENT_MOUSE_MOTION:
		update_pointer(app, event.motion.x, event.motion.y);
		if (app.frontend_phase == FrontendPhase::options
			&& (event.motion.state & SDL_BUTTON_LMASK) != 0
			&& sl_open::frontend::options_drag(app.options, app.config))
		{
			if (app.options.page == sl_open::frontend::OptionsPage::audio)
			{
				apply_audio_config(app);
			}
		}
		break;
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (event.button.button == SDL_BUTTON_LEFT
			&& (app.frontend_phase == FrontendPhase::campaign_loadout
				|| app.frontend_phase == FrontendPhase::multiplayer_loadout))
		{
			update_pointer(app, event.button.x, event.button.y);
			const bool multiplayer = app.frontend_phase == FrontendPhase::multiplayer_loadout;
			const auto result = sl_open::frontend::loadout_release(app.loadout,
				multiplayer ? app.multiplayer_campaign : app.campaign, app.loadout_catalog,
				app.loadout.pointer_x, app.loadout.pointer_y, SDL_GetTicks());
			play_loadout_click_sounds(app, result);
			if (result.launch)
			{
				if (multiplayer)
				{
					begin_multiplayer_loadout_exit(app, SDL_GetTicks());
				}
				else
				{
					begin_mission_loadout_exit(app, SDL_GetTicks());
				}
			}
		}
		break;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
		update_pointer(app, event.button.x, event.button.y);
		if (event.button.button == SDL_BUTTON_LEFT)
		{
			if (app.frontend_phase == FrontendPhase::instant_action)
			{
				const sl_open::frontend::GameplayPauseAction action =
					sl_open::game::mission_session_select(app.mission_session);
				if (action
					== sl_open::frontend::GameplayPauseAction::continue_mission)
				{
					sl_open::input::
						gameplay_input_gate_fire_until_mouse_released(
							app.gameplay_devices,
							SDL_BUTTON_LEFT);
				}
				if (action != sl_open::frontend::GameplayPauseAction::none
					|| app.mission_session.result.kind
						!= sl_open::game::SessionResultKind::none)
				{
					play_frontend_click(app);
				}
				if (action == sl_open::frontend::GameplayPauseAction::audio)
				{
					enter_instant_action_option(
						app,
						sl_open::frontend::OptionsPage::audio,
						SDL_GetTicks());
				}
				else if (
					action == sl_open::frontend::GameplayPauseAction::controls)
				{
					enter_instant_action_option(
						app,
						sl_open::frontend::OptionsPage::controls,
						SDL_GetTicks());
				}
				else if (
					action == sl_open::frontend::GameplayPauseAction::video)
				{
					enter_instant_action_option(
						app,
						sl_open::frontend::OptionsPage::video,
						SDL_GetTicks());
				}
				apply_instant_action_result(app, SDL_GetTicks());
			}
				else if (app.frontend_phase == FrontendPhase::main_menu)
				{
				if (app.main_menu.hovered >= 0)
				{
					play_frontend_click(app);
				}
				if (app.main_menu.quit_confirmation)
				{
					if (app.main_menu.hovered == 0)
					{
						return SDL_APP_SUCCESS;
					}
					if (app.main_menu.hovered == 1)
					{
						app.main_menu.quit_confirmation = false;
						app.main_menu.hovered = -1;
					}
				}
				else if (app.main_menu.hovered == 3)
				{
					app.main_menu.quit_confirmation = true;
					app.main_menu.hovered = -1;
				}
				else if (app.main_menu.hovered == 2)
				{
					app.options.in_game = false;
					sl_open::frontend::options_enter_page(
						app.options,
						sl_open::frontend::OptionsPage::hub,
						app.config,
						SDL_GetTicks());
					begin_transition(
						app,
						"interface/main2opt.bik",
						TransitionDestination::options,
						SDL_GetTicks());
				}
					else if (app.main_menu.hovered == 0)
					{
					sl_open::frontend::campaign_frontend_enter_single_player(
						app.campaign_frontend,
						app.campaign_store,
						sl_open::language_text(app.language, 0xbf),
						SDL_GetTicks());
					begin_transition(
						app,
						"interface/main2sin.bik",
						TransitionDestination::campaign,
							SDL_GetTicks());
					}
					else if (app.main_menu.hovered == 1)
					{
						begin_transition(
							app,
							sl_open::frontend::kMultiplayerEnterMovie,
							TransitionDestination::multiplayer,
							SDL_GetTicks());
					}
					else if (app.main_menu.hovered == 4)
				{
					begin_instant_action(
						app,
						sl_open::game::MissionOrigin::main_menu,
						SDL_GetTicks());
				}
				}
				else if (app.frontend_phase
					== FrontendPhase::multiplayer_frontend)
				{
					const MultiplayerFrontendSessions sessions =
						multiplayer_frontend_sessions(app);
					const sl_open::frontend::MultiplayerAction action =
						sl_open::frontend::multiplayer_frontend_select(
							app.multiplayer_frontend,
							sessions.view);
					if (action.type
						!= sl_open::frontend::MultiplayerActionType::none)
					{
						play_frontend_click(app);
					}
					if (handle_multiplayer_frontend_action(
							app, action, SDL_GetTicks()))
					{
						return SDL_APP_SUCCESS;
					}
				}
				else if (app.frontend_phase
					== FrontendPhase::multiplayer_lobby)
				{
					const sl_open::frontend::MultiplayerLobbyView view =
						multiplayer_lobby_view(app);
					const sl_open::frontend::MultiplayerLobbyAction action =
						sl_open::frontend::multiplayer_lobby_select(
							app.multiplayer_lobby, view);
					if (action.type
						!= sl_open::frontend::MultiplayerLobbyActionType::none)
					{
						play_frontend_click(app);
					}
					if (handle_multiplayer_lobby_action(
							app, action, SDL_GetTicks()))
					{
						return SDL_APP_SUCCESS;
					}
				}
				else if (app.frontend_phase
					== FrontendPhase::multiplayer_debrief)
				{
					const sl_open::frontend::MultiplayerDebriefView view =
						multiplayer_debrief_view(app);
					const sl_open::frontend::MultiplayerDebriefAction action =
						sl_open::frontend::multiplayer_debrief_select(
							app.multiplayer_debrief, view);
					if (action.type
						!= sl_open::frontend::
							MultiplayerDebriefActionType::none)
					{
						play_frontend_click(app);
					}
					(void)handle_multiplayer_debrief_action(
						app, action, SDL_GetTicks());
				}
				else if (app.frontend_phase == FrontendPhase::options)
			{
				const sl_open::frontend::OptionsSelection selection =
					sl_open::frontend::options_select(
					app.options,
					app.config,
					app.display_modes,
					app.audio.hrtf_supported,
					app.joystick_available);
				if (selection != sl_open::frontend::OptionsSelection::none)
				{
					play_frontend_click(app);
				}
				switch (selection)
				{
				case sl_open::frontend::OptionsSelection::changed:
					if (app.options.page == sl_open::frontend::OptionsPage::audio)
					{
						apply_audio_config(app);
					}
					break;
				case sl_open::frontend::OptionsSelection::audio:
					sl_open::frontend::options_enter_page(
						app.options,
						sl_open::frontend::OptionsPage::audio,
						app.config,
						SDL_GetTicks());
					begin_transition(
						app,
						app.options.in_game
							? "interface/igofade.bik"
							: "interface/optfade.bik",
						TransitionDestination::options,
						SDL_GetTicks(),
						false,
						false,
						app.options.in_game
							? TransitionAudio::in_game_options
							: TransitionAudio::primary);
					break;
				case sl_open::frontend::OptionsSelection::video:
					sl_open::frontend::options_enter_page(
						app.options,
						sl_open::frontend::OptionsPage::video,
						app.config,
						SDL_GetTicks());
					begin_transition(
						app,
						app.options.in_game
							? "interface/igofade.bik"
							: "interface/optfade.bik",
						TransitionDestination::options,
						SDL_GetTicks(),
						false,
						false,
						app.options.in_game
							? TransitionAudio::in_game_options
							: TransitionAudio::primary);
					break;
				case sl_open::frontend::OptionsSelection::controls:
					sl_open::frontend::options_enter_page(
						app.options,
						sl_open::frontend::OptionsPage::controls,
						app.config,
						SDL_GetTicks());
					begin_transition(
						app,
						app.options.in_game
							? "interface/igofade.bik"
							: "interface/optfade.bik",
						TransitionDestination::options,
						SDL_GetTicks(),
						false,
						false,
						app.options.in_game
							? TransitionAudio::in_game_options
							: TransitionAudio::primary);
					break;
				case sl_open::frontend::OptionsSelection::options:
					if (app.options.in_game
						&& app.options_return_phase
							== FrontendPhase::instant_action)
					{
						return_to_instant_action_pause(app);
						break;
					}
					sl_open::frontend::options_enter_page(
						app.options,
						sl_open::frontend::OptionsPage::hub,
						app.config,
						SDL_GetTicks());
					if (!app.options.in_game)
					{
						begin_transition(
							app,
							"interface/optfade2.bik",
							TransitionDestination::options,
							SDL_GetTicks());
					}
					else
					{
						begin_in_game_options_transition(
							app,
							"interface/igofade2.bik",
							TransitionDestination::options,
							SDL_GetTicks());
					}
					break;
				case sl_open::frontend::OptionsSelection::apply_options:
					sl_open::platform::display_apply_config(app);
					sl_open::platform::display_resize(
						app,
						static_cast<int>(app.width),
						static_cast<int>(app.height));
					if (app.options.in_game
						&& app.options_return_phase
							== FrontendPhase::instant_action)
					{
						return_to_instant_action_pause(app);
						break;
					}
					sl_open::frontend::options_enter_page(
						app.options,
						sl_open::frontend::OptionsPage::hub,
						app.config,
						SDL_GetTicks());
					if (!app.options.in_game)
					{
						begin_transition(
							app,
							"interface/optfade2.bik",
							TransitionDestination::options,
							SDL_GetTicks());
					}
					else
					{
						begin_in_game_options_transition(
							app,
							"interface/igofade2.bik",
							TransitionDestination::options,
							SDL_GetTicks());
					}
					break;
				case sl_open::frontend::OptionsSelection::apply_main_menu:
					sl_open::platform::display_apply_config(app);
					sl_open::platform::display_resize(
						app,
						static_cast<int>(app.width),
						static_cast<int>(app.height));
					if (app.options.in_game
						&& app.options_return_phase
							== FrontendPhase::instant_action)
					{
						sl_open::game::mission_session_request_exit(
							app.mission_session);
						apply_instant_action_result(app, SDL_GetTicks());
						break;
					}
					if (app.options.in_game)
					{
						stop_in_game_options_owner(app);
						begin_in_game_options_transition(
							app,
							"interface/igof2mm.bik",
							TransitionDestination::main_menu,
							SDL_GetTicks());
						app.options.in_game = false;
					}
					else
					{
						begin_transition(
							app,
							"interface/opt2main.bik",
							TransitionDestination::main_menu,
							SDL_GetTicks());
					}
					break;
				case sl_open::frontend::OptionsSelection::main_menu:
					if (app.options.in_game)
					{
						if (app.options_return_phase
							== FrontendPhase::instant_action)
						{
							sl_open::game::mission_session_request_exit(
								app.mission_session);
							apply_instant_action_result(
								app, SDL_GetTicks());
							break;
						}
						if (app.options.page
							== sl_open::frontend::OptionsPage::hub)
						{
							leave_in_game_options_for_main_menu(
								app, SDL_GetTicks());
						}
						else
						{
							stop_in_game_options_owner(app);
							begin_in_game_options_transition(
								app,
								"interface/igof2mm.bik",
								TransitionDestination::main_menu,
								SDL_GetTicks());
							app.options.in_game = false;
						}
					}
					else
					{
						begin_transition(
							app,
							"interface/opt2main.bik",
							TransitionDestination::main_menu,
							SDL_GetTicks());
					}
					break;
				case sl_open::frontend::OptionsSelection::back:
					if (app.options_return_phase
						== FrontendPhase::instant_action)
					{
						return_to_instant_action_pause(app);
					}
					else
					{
						resume_in_game_options_owner(
							app, SDL_GetTicks());
					}
					break;
				case sl_open::frontend::OptionsSelection::load:
					sl_open::frontend::campaign_frontend_enter_save_load(
						app.campaign_frontend,
						app.campaign_store,
						app.campaign,
						sl_open::frontend::SaveLoadMode::load,
						true,
						SDL_GetTicks());
					begin_in_game_options_transition(
						app,
						"interface/igofade.bik",
						TransitionDestination::campaign,
						SDL_GetTicks());
					break;
				case sl_open::frontend::OptionsSelection::save:
					sl_open::frontend::campaign_frontend_enter_save_load(
						app.campaign_frontend,
						app.campaign_store,
						app.campaign,
						sl_open::frontend::SaveLoadMode::save,
						true,
						SDL_GetTicks());
					begin_in_game_options_transition(
						app,
						"interface/igofade.bik",
						TransitionDestination::campaign,
						SDL_GetTicks());
					break;
				case sl_open::frontend::OptionsSelection::quit:
					return SDL_APP_SUCCESS;
				case sl_open::frontend::OptionsSelection::none:
					break;
				default:
					break;
				}
			}
				else if (app.frontend_phase == FrontendPhase::campaign)
				{
				const sl_open::frontend::CampaignSelection selection =
					sl_open::frontend::campaign_frontend_select(
					app.campaign_frontend,
					app.campaign_store,
					app.campaign);
				if (selection != sl_open::frontend::CampaignSelection::none)
				{
					play_frontend_click(app);
				}
				switch (selection)
				{
				case sl_open::frontend::CampaignSelection::main_menu:
					if (app.campaign_frontend.save_load_in_game)
					{
						stop_in_game_options_owner(app);
						begin_in_game_options_transition(
							app,
							"interface/igof2mm.bik",
							TransitionDestination::main_menu,
							SDL_GetTicks());
						app.options.in_game = false;
					}
					else
					{
						begin_transition(
							app,
							"interface/sin2main.bik",
							TransitionDestination::main_menu,
							SDL_GetTicks());
					}
					break;
				case sl_open::frontend::CampaignSelection::quit:
					return SDL_APP_SUCCESS;
				case sl_open::frontend::CampaignSelection::single_player:
					if (app.campaign_frontend.save_load_in_game)
					{
						sl_open::frontend::options_enter_page(
							app.options,
							sl_open::frontend::OptionsPage::hub,
							app.config,
							SDL_GetTicks());
						begin_in_game_options_transition(
							app,
							"interface/igofade2.bik",
							TransitionDestination::options,
							SDL_GetTicks());
					}
					else
					{
						begin_transition(
							app,
							"interface/sifad2mm.bik",
							TransitionDestination::campaign,
							SDL_GetTicks());
					}
					break;
				case sl_open::frontend::CampaignSelection::save_load:
					begin_transition(
						app,
						"interface/sinfade.bik",
						TransitionDestination::campaign,
						SDL_GetTicks());
					break;
				case sl_open::frontend::CampaignSelection::new_campaign:
					start_campaign_induction(app, SDL_GetTicks());
					break;
				case sl_open::frontend::CampaignSelection::campaign_ready:
					SDL_StopTextInput(app.window);
					if (app.campaign_frontend.save_load_in_game)
					{
						finish_in_game_save_load(
							app, false, SDL_GetTicks());
					}
					else
					{
						app.transition_destination =
							TransitionDestination::campaign_hub;
						enter_transition_destination(app, SDL_GetTicks());
					}
					break;
				case sl_open::frontend::CampaignSelection::save_complete:
					SDL_StopTextInput(app.window);
					if (app.campaign_frontend.save_load_in_game)
					{
						finish_in_game_save_load(
							app, true, SDL_GetTicks());
					}
					break;
				case sl_open::frontend::CampaignSelection::none:
					break;
					}
				}
				else if (app.frontend_phase
					== FrontendPhase::multiplayer_campaign_load)
				{
					const sl_open::frontend::CampaignSelection selection =
						sl_open::frontend::campaign_frontend_select(
							app.campaign_frontend,
							app.campaign_store,
							app.multiplayer_campaign);
					if (selection
						!= sl_open::frontend::CampaignSelection::none)
					{
						play_frontend_click(app);
					}
					if (apply_multiplayer_campaign_selection(
							app, selection, SDL_GetTicks()))
					{
						return SDL_APP_SUCCESS;
					}
				}
				else if (app.frontend_phase == FrontendPhase::campaign_hub)
			{
				const std::uint8_t destination =
					sl_open::frontend::campaign_hub_select(app.campaign_hub);
				if (destination != sl_open::frontend::kNoHubNode)
				{
					sl_open::frontend::CampaignHub destination_hub =
						app.campaign_hub;
					sl_open::frontend::campaign_hub_enter(
						destination_hub, destination);
					const std::int16_t sound =
						sl_open::frontend::campaign_hub_node(
							destination_hub).selection_sound;
					if (sound >= 0)
					{
						play_fat_sample(
							app,
							app.vr_sounds,
							static_cast<std::uint32_t>(sound));
					}
					if (sl_open::frontend::campaign_hub_selection_enters_briefing(
							app.campaign_hub, destination))
					{
						enter_mission_briefing(app, SDL_GetTicks());
					}
					else
					{
						enter_hub_node(app, destination, SDL_GetTicks());
					}
				}
			}
				else if (app.frontend_phase == FrontendPhase::campaign_loadout)
				{
				const sl_open::frontend::LoadoutClickResult result =
					sl_open::frontend::loadout_press(
					app.loadout,
					app.campaign,
					app.loadout_catalog,
					app.loadout.pointer_x,
					app.loadout.pointer_y,
					SDL_GetTicks());
				play_loadout_click_sounds(app, result);
				if (result.launch)
				{
					begin_mission_loadout_exit(app, SDL_GetTicks());
				}
			}
			else if (app.frontend_phase
				== FrontendPhase::multiplayer_loadout)
			{
				const sl_open::frontend::LoadoutClickResult result =
					sl_open::frontend::loadout_press(
						app.loadout,
						app.multiplayer_campaign,
						app.loadout_catalog,
						app.loadout.pointer_x,
						app.loadout.pointer_y,
						SDL_GetTicks());
				play_loadout_click_sounds(app, result);
				if (result.launch)
				{
					begin_multiplayer_loadout_exit(
						app, SDL_GetTicks());
				}
			}
			else if (app.frontend_phase
				== FrontendPhase::campaign_restart_choice)
			{
				handle_post_mission_choice(
					app,
					sl_open::frontend::post_mission_choice_click(
						app.post_mission_choice),
					SDL_GetTicks());
			}
			else if (app.frontend_phase == FrontendPhase::campaign_sim_pod)
			{
				handle_sim_pod_action(
					app,
					sl_open::frontend::sim_pod_select(app.sim_pod),
					SDL_GetTicks());
			}
			else if (app.frontend_phase == FrontendPhase::campaign_cd)
			{
				handle_cd_action(
					app,
					sl_open::frontend::cd_player_select(app.cd_player),
					SDL_GetTicks());
			}
			else if (app.frontend_phase == FrontendPhase::campaign_itac
				&& sl_open::frontend::itac_shell_select(app.itac_shell))
			{
				open_itac_movie(app, SDL_GetTicks());
			}
		}
		break;
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		sl_open::platform::display_resize(
			app, event.window.data1, event.window.data2);
		break;
	case SDL_EVENT_WINDOW_RESIZED:
		if (app.config.display_mode == sl_open::DisplayMode::windowed
			&& event.window.data1 > 0 && event.window.data2 > 0)
		{
			app.config.display_width =
				static_cast<std::uint32_t>(event.window.data1);
			app.config.display_height =
				static_cast<std::uint32_t>(event.window.data2);
			sl_open::platform::display_add_resolution(
				app.display_modes,
				app.config.display_width,
				app.config.display_height);
		}
		break;
	case SDL_EVENT_MOUSE_WHEEL:
		if (app.frontend_phase == FrontendPhase::options
			&& app.options.page == sl_open::frontend::OptionsPage::controls)
		{
			const std::uint64_t now = SDL_GetTicks();
			if (now < app.options.control_wheel_suppressed_until)
			{
				// SDL can deliver several wheel events for one physical step.
				// Keep extending the quiet period until that burst has ended.
				app.options.control_wheel_suppressed_until =
					now + kControlWheelQuietPeriodMs;
			}
			else if (event.wheel.y != 0.0f)
			{
				sl_open::frontend::options_scroll_controls(
					app.options, event.wheel.y > 0.0f ? -1 : 1);
			}
		}
		break;
	case SDL_EVENT_JOYSTICK_ADDED:
	case SDL_EVENT_JOYSTICK_REMOVED:
	case SDL_EVENT_GAMEPAD_ADDED:
	case SDL_EVENT_GAMEPAD_REMOVED:
		app.joystick_available = SDL_HasJoystick() || SDL_HasGamepad();
		if (!app.joystick_available && app.config.controller == 0)
		{
			app.config.controller = 2;
		}
		break;
	default:
		break;
	}
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
	auto& app = *static_cast<App*>(appstate);
	if (sl_open::platform::input_queue_push(app.input_queue, *event))
	{
		return SDL_APP_CONTINUE;
	}
	if (event->type == SDL_EVENT_QUIT)
	{
		return SDL_APP_SUCCESS;
	}
	if (!app.input_queue.overflow_reported)
	{
		SDL_Log("Input queue capacity exceeded; dropping new events");
		app.input_queue.overflow_reported = true;
	}
	return SDL_APP_CONTINUE;
}

sl_open::audio::Sound3DListener mission_sound_listener(const App& app)
{
	return {
		app.mission_session.camera.position,
		app.mission_session.camera.orientation,
	};
}

bool resolve_mission_sound_source(
	void* userdata,
	const sl_open::audio::Sound3DSource& source,
	sl_open::audio::Sound3DTransform& transform)
{
	auto& app = *static_cast<App*>(userdata);
	const sl_open::game::MissionSession& session = app.mission_session;
	switch (source.binding)
	{
	case sl_open::audio::Sound3DBinding::explicit_transform:
		transform = source.transform;
		return true;
	case sl_open::audio::Sound3DBinding::projectile:
		if (source.index >= sl_open::game::kMaxGunProjectiles)
		{
			return false;
		}
		{
			const sl_open::game::GunProjectile& projectile =
				session.weapons.projectiles[source.index];
			if (!projectile.active
				|| projectile.sound_generation != source.serial)
			{
				return false;
			}
			transform.position = projectile.position;
			transform.direction = projectile.orientation[2];
			transform.velocity = projectile.velocity;
			return true;
		}
	case sl_open::audio::Sound3DBinding::missile:
		if (source.index >= sl_open::game::kMaxMissiles)
		{
			return false;
		}
		{
			const sl_open::game::Missile& missile =
				session.missiles.missiles[source.index];
			if (!missile.active
				|| missile.sound_generation != source.serial)
			{
				return false;
			}
			transform.position = missile.position;
			transform.direction = missile.orientation[2];
			transform.velocity = missile.velocity;
			return true;
		}
	case sl_open::audio::Sound3DBinding::object:
	case sl_open::audio::Sound3DBinding::model_frame:
	{
		const sl_open::game::ObjectHandle handle{
			source.index, source.generation};
		const sl_open::game::WorldObject* object =
			sl_open::game::world_resolve(session.world, handle);
		if (object == nullptr)
		{
			return false;
		}
		if (source.binding == sl_open::audio::Sound3DBinding::model_frame
			&& source.model_reference >= 0
			&& static_cast<std::size_t>(source.model_reference)
				< object->model_references.size())
		{
			glm::mat4 object_world{object->orientation};
			object_world[3] = glm::vec4(object->position, 1.0f);
			const glm::mat4 frame = object_world
				* sl_open::game::model_animation_render_transform(
					*object,
					static_cast<std::uint16_t>(
						source.model_reference),
					1.0f);
			transform.position = glm::vec3(frame[3]);
			transform.direction = glm::mat3(frame)[2];
		}
		else
		{
			transform.position = object->position
				+ object->orientation[2]
					* (object->player ? 200.0f : 0.0f);
			transform.direction = object->orientation[2];
		}
		transform.velocity = object->linear_velocity;
		return true;
	}
	}
	return false;
}

void publish_mission_sound_owner_slot(
	void* userdata,
	const sl_open::audio::Sound3DSource& source,
	std::int16_t slot)
{
	auto& app = *static_cast<App*>(userdata);
	const std::uint16_t owner_index =
		source.object_index != UINT16_MAX
			? source.object_index
			: source.binding == sl_open::audio::Sound3DBinding::object
				? source.index
				: UINT16_MAX;
	const std::uint16_t owner_generation =
		source.object_index != UINT16_MAX
			? source.object_generation
			: source.generation;
	sl_open::game::WorldObject* owner = sl_open::game::world_resolve(
		app.mission_session.world,
		{owner_index, owner_generation});
	if (owner == nullptr)
	{
		return;
	}
	if (slot >= 0 || owner->sound3d_slot >= 0)
	{
		owner->sound3d_slot = slot;
	}
}

sl_open::audio::Sound3DSource sound_source_from_world_event(
	const sl_open::game::WorldSoundEvent& event)
{
	sl_open::audio::Sound3DSource source;
	source.transform = {
		event.position, event.direction, event.velocity};
	source.index = event.object.index;
	source.generation = event.object.generation;
	source.object_index = event.object.index;
	source.object_generation = event.object.generation;
	source.model_reference = event.model_reference;
	switch (event.binding)
	{
	case sl_open::game::WorldSoundEvent::Binding::missile:
		source.binding = sl_open::audio::Sound3DBinding::missile;
		source.index = event.source_index;
		source.serial = event.source_serial;
		break;
	case sl_open::game::WorldSoundEvent::Binding::object:
		source.binding = sl_open::audio::Sound3DBinding::object;
		break;
	case sl_open::game::WorldSoundEvent::Binding::model_frame:
		source.binding = sl_open::audio::Sound3DBinding::model_frame;
		break;
	default:
		source.binding = sl_open::audio::Sound3DBinding::explicit_transform;
		source.object_index = UINT16_MAX;
		break;
	}
	return source;
}

void service_player_engine_and_flybys(
	App& app,
	const sl_open::audio::Sound3DListener& listener,
	std::uint32_t tick)
{
	sl_open::game::World& world = app.mission_session.world;
	sl_open::game::WorldObject* player =
		sl_open::game::world_resolve(world, world.player);
	if (player == nullptr)
	{
		return;
	}
	sl_open::audio::Sound3DSource player_source;
	player_source.binding = sl_open::audio::Sound3DBinding::object;
	player_source.index = world.player.index;
	player_source.generation = world.player.generation;
	player_source.object_index = world.player.index;
	player_source.object_generation = world.player.generation;
	player_source.player = true;
	resolve_mission_sound_source(
		&app, player_source, player_source.transform);
	sl_open::audio::sound3d_update_player_engine(
		app.audio,
		app.sound3d,
		app.spatial_sounds,
		player_source,
		listener,
		player->type,
		player->throttle,
		player->afterburner_active,
		player->reverse_thrust_active,
		app.mission_session.mission_runtime.game_mode,
		tick,
		publish_mission_sound_owner_slot,
		&app);

	const std::uint8_t mission_state =
		app.mission_session.mission_runtime.game_mode;
	for (std::uint16_t index = 0; index < sl_open::game::kMaxGameObjects; ++index)
	{
		sl_open::game::WorldObject& object = world.objects[index];
		if (!object.active || object.object_class != 1
			|| (object.runtime_flags & 0x00000441u) != 0
			|| object.throttle < .4f
			|| tick < object.last_flyby_sound_tick
				+ (object.player ? 200u : 500u)
			|| (object.player && (mission_state == 0
				|| mission_state == 1 || mission_state == 2
				|| mission_state == 3 || mission_state == 12
				|| mission_state == 15)))
		{
			continue;
		}
		const glm::vec3 separation =
			object.position - listener.position;
		if (glm::dot(separation, separation) > 100000000.0f
			|| glm::length(object.linear_velocity) < 100.0f)
		{
			continue;
		}
		const float cosine = glm::dot(
			glm::normalize(object.linear_velocity),
			listener.orientation[2]);
		const float maximum_cosine =
			object.allegiance_class == 1 ? .75f : 0.0f;
		if (cosine > maximum_cosine)
		{
			continue;
		}
		sl_open::audio::Sound3DSource source;
		source.binding = sl_open::audio::Sound3DBinding::object;
		source.index = index;
		source.generation = object.generation;
		source.object_index = index;
		source.object_generation = object.generation;
		source.player = object.player;
		resolve_mission_sound_source(&app, source, source.transform);
		if (sl_open::audio::sound3d_play(
				app.audio,
				app.sound3d,
				app.spatial_sounds,
				source,
				listener,
				object.allegiance_class == 1 ? 72 : 73,
				7,
				tick,
				1.0f,
				publish_mission_sound_owner_slot,
				&app) >= 0)
		{
			object.last_flyby_sound_tick = tick;
		}
	}
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
	auto& app = *static_cast<App*>(appstate);
	service_multiplayer_transport(app, SDL_GetTicks());
	sl_open::platform::InputEvent input_event;
	while (sl_open::platform::input_queue_pop(app.input_queue, input_event))
	{
		const SDL_AppResult result = handle_input_event(app, input_event);
		if (result != SDL_APP_CONTINUE)
		{
			return result;
		}
	}
	update_gameplay_mouse_mode(app);
	if (app.startup_error != StartupError::none)
	{
		sl_open::platform::display_draw_startup_error(app);
		++app.frame_count;
		if (app.frame_limit != 0 && app.frame_count >= app.frame_limit)
		{
			return SDL_APP_SUCCESS;
		}
		return SDL_APP_CONTINUE;
	}
	sl_open::render::frame_geometry_begin(app.frontend_renderer.frame_geometry);
	std::uint64_t now = SDL_GetTicks();
	if (app.requested_mission != 0)
	{
		const std::uint16_t requested_mission =
			app.requested_mission;
		app.requested_mission = 0;
		begin_instant_action(
			app,
			sl_open::game::MissionOrigin::main_menu,
			now,
			requested_mission,
			requested_mission <= sl_open::campaign::kMissionCount
					|| requested_mission == 251
				? sl_open::game::MissionMode::campaign
				: requested_mission >= 30
						&& requested_mission <= 35
					? sl_open::game::MissionMode::training
					: sl_open::game::MissionMode::instant_action);
		now = SDL_GetTicks();
	}
	if (app.frontend_phase == FrontendPhase::instant_action)
	{
		if (!app.mission_foster_active)
		{
			const std::uint64_t previous_gameplay_tick =
				app.mission_session.clock.gameplay_tick;
			sl_open::input::GameplayInputPoller gameplay_input;
			sl_open::input::gameplay_input_begin_frame(
				app.config,
				app.gameplay_devices,
				static_cast<std::uint8_t>(
					app.mission_session.hud.panels[0].animation),
				gameplay_input);
			if (app.mission_session.state
					== sl_open::game::MissionSessionState::running
				&& app.mission_session.result.kind
					== sl_open::game::SessionResultKind::none
				&& sl_open::input::gameplay_input_raw_keyboard_pressed(
					gameplay_input, SDL_SCANCODE_ESCAPE, 0))
			{
				// Player_update, LANCER.EXE 0x0049253a..0x00492578,
				// consumes raw unmodified Escape before the configurable
				// key-configuration action and immediately leaves gameplay
				// dispatch for this frame.
				sl_open::game::mission_session_toggle_pause(
					app.mission_session,
					app.width,
					app.height,
					now);
				update_gameplay_mouse_mode(app);
				return SDL_APP_CONTINUE;
			}
			// KEY CONFIG is a direct 3D-loop command, not merely a pause-menu
			// shortcut. LANCER.EXE 0x00492840 polls action 73 on its pressed
			// edge, suspends the single-player mission, and transfers straight
			// to the in-game key-configuration screen.
			const bool key_config_pressed =
				app.mission_session.state
					== sl_open::game::MissionSessionState::running
				&& app.mission_session.result.kind
					== sl_open::game::SessionResultKind::none
				&& sl_open::input::gameplay_input_action_pressed(
					gameplay_input, 73);
			if (key_config_pressed
				&& app.mission_session.mission_runtime.network.role
					== sl_open::mission::NetworkRole::offline)
			{
				sl_open::game::mission_session_toggle_pause(
					app.mission_session,
					app.width,
					app.height,
					now);
				enter_instant_action_option(
					app,
					sl_open::frontend::OptionsPage::controls,
					now);
			}
			if (app.frontend_phase != FrontendPhase::instant_action)
			{
				update_gameplay_mouse_mode(app);
				return SDL_APP_CONTINUE;
			}
			const bool chat_was_active =
				app.mission_session.hud.chat_active;
			const bool awaiting_multiplayer_result =
				app.mission_session.request.origin
						== sl_open::game::MissionOrigin::multiplayer
					&& app.mission_session.result.kind
						!= sl_open::game::SessionResultKind::none
					&& app.mission_session.result.kind
						!= sl_open::game::SessionResultKind::load_failed;
			if (!awaiting_multiplayer_result)
			{
				sl_open::game::mission_session_update(
					app.mission_session,
					app.config,
				app.language,
				gameplay_input,
				app.width,
					app.height,
					now);
				(void)capture_multiplayer_mission_bootstrap(app);
			}
			else
			{
				sl_open::game::simulation_clock_reset(
					app.mission_session.clock, now);
			}
				flush_multiplayer_gameplay_outbound(app);
				if (app.mission_session.state
					== sl_open::game::MissionSessionState::running
				&& app.mission_session.result.kind
					== sl_open::game::SessionResultKind::none)
			{
				const bool forced_scoreboard =
					app.mission_session.mission_runtime.network
						.deathmatch_mode
					&& (app.mission_session.world.camera_mode == 8
						|| app.mission_session.world.camera_mode == 26
						|| app.mission_session.world.camera_mode == 27);
				// Deathmatch_render_frame owns F10 after the player-control
				// callback. Its three death cameras bypass the query and force
				// the scoreboard directly.
				sl_open::hud::runtime_set_scoreboard_held(
					app.mission_session.hud,
					forced_scoreboard
						|| sl_open::input::gameplay_input_action_held(
							gameplay_input, 71));
				if (sl_open::input::gameplay_input_raw_keyboard_pressed(
						gameplay_input, SDL_SCANCODE_0, 0))
				{
					char screenshot_path[32];
					std::snprintf(
						screenshot_path,
						sizeof(screenshot_path),
						"screenshot%04u",
						app.screenshot_index);
					// bgfx's default screenshot callback appends ".tga";
					// the retail owner formats screenshot%04d.tga and
					// increments its counter after every capture request.
					bgfx::requestScreenShot(
						BGFX_INVALID_HANDLE, screenshot_path);
					++app.screenshot_index;
				}
			}
			if (!chat_was_active
				&& app.mission_session.hud.chat_active)
			{
				SDL_StartTextInput(app.window);
			}
			else if (chat_was_active
				&& !app.mission_session.hud.chat_active)
			{
				SDL_StopTextInput(app.window);
			}
			if (app.mission_session.result.kind
					!= sl_open::game::SessionResultKind::none
				&& app.mission_session.result.kind
					!= sl_open::game::SessionResultKind::load_failed)
			{
				apply_instant_action_result(app, now);
				if (app.frontend_phase != FrontendPhase::instant_action)
				{
					update_gameplay_mouse_mode(app);
					return SDL_APP_CONTINUE;
				}
			}
			if (!sl_open::controller_uses_mouse(app.config.controller)
				|| app.mission_session.state
					!= sl_open::game::MissionSessionState::running
				|| app.mission_session.clock.gameplay_tick
					!= previous_gameplay_tick)
			{
				app.gameplay_devices.mouse_relative_x = 0.0f;
				app.gameplay_devices.mouse_relative_y = 0.0f;
			}
		}
		if (!app.mission_foster_active
			&& app.mission_session.mission_runtime.fosters_last_stand)
		{
			start_mission_foster(app, now);
		}
		if (app.mission_foster_active)
		{
			const sl_open::media::BinkMovieState state =
				sl_open::media::bink_movie_update(
					app.vfs, now, app.mission_foster_movie);
			if (state == sl_open::media::BinkMovieState::finished
				|| state == sl_open::media::BinkMovieState::failed)
			{
				finish_mission_foster(app);
			}
		}
		if (!app.mission_foster_active)
		{
				sl_open::render::mission_renderer_initialize_world_components(
				app.mission_renderer,
				app.mission_session.world,
					&app.mission_session.chaff);
			if (sl_open::game::mission_session_service_object_activations(
					app.mission_session))
			{
				sl_open::render::mission_renderer_initialize_world_components(
					app.mission_renderer,
					app.mission_session.world,
					&app.mission_session.chaff);
			}
		const sl_open::audio::Sound3DListener listener =
			mission_sound_listener(app);
		const std::uint32_t audio_tick = static_cast<std::uint32_t>(
			app.mission_session.clock.gameplay_tick);
		if (app.mission_session.state
			== sl_open::game::MissionSessionState::paused)
		{
			sl_open::audio::sound3d_stop_all(app.audio, app.sound3d);
		}
		else
		{
			sl_open::audio::sound3d_resume_all(app.audio, app.sound3d);
			service_player_engine_and_flybys(
				app, listener, audio_tick);
		}
		sl_open::game::WeaponSoundEvent weapon_sound;
		while (sl_open::game::weapons_pop_sound(
			app.mission_session.weapons, weapon_sound))
		{
			sl_open::audio::Sound3DSource source;
			source.transform = {
				weapon_sound.position,
				weapon_sound.direction,
				weapon_sound.velocity};
			if (weapon_sound.projectile)
			{
				source.binding =
					sl_open::audio::Sound3DBinding::projectile;
				source.index = weapon_sound.source_index;
				source.serial = weapon_sound.source_generation;
			}
			sl_open::audio::sound3d_play(
				app.audio,
				app.sound3d,
				app.spatial_sounds,
				source,
				listener,
				weapon_sound.definition,
				weapon_sound.requested_class,
				audio_tick,
				1.0f,
				publish_mission_sound_owner_slot,
				&app);
		}
		sl_open::game::WorldSoundEvent world_sound;
		while (sl_open::game::world_pop_sound(
			app.mission_session.world, world_sound))
		{
			sl_open::audio::sound3d_play(
				app.audio,
				app.sound3d,
				app.spatial_sounds,
				sound_source_from_world_event(world_sound),
				listener,
				world_sound.definition,
				world_sound.requested_class,
				audio_tick,
				1.0f,
				publish_mission_sound_owner_slot,
				&app);
		}
		if (app.mission_session.state
			!= sl_open::game::MissionSessionState::paused)
		{
			sl_open::audio::sound3d_update(
				app.audio,
				app.sound3d,
				listener,
				audio_tick,
				resolve_mission_sound_source,
				publish_mission_sound_owner_slot,
				&app);
		}
		std::uint8_t hud_sound;
		std::uint8_t hud_sound_volume;
		while (sl_open::hud::runtime_pop_sound(
			app.mission_session.hud,
			hud_sound,
			hud_sound_volume))
		{
			play_fat_sample(
				app,
				app.standard_sounds,
				hud_sound,
				hud_sound_volume);
		}
		if (sl_open::hud::runtime_pop_hit_sound(
				app.mission_session.hud))
		{
			// mission_trigger_hit_distortion 0x00494890 buffers standard
			// FAT sample 12 at the listener/player position. Its spatial
			// accumulator saturates both channels before the retail flush.
			play_fat_sample(
				app,
				app.standard_sounds,
				12,
				127);
		}
		const bool lock_tone =
			app.mission_session.hud.missile_lock.tone_active;
		if (lock_tone != app.mission_lock_tone_active)
		{
			if (lock_tone)
			{
				app.mission_lock_tone_active =
					sl_open::audio::fat_play_in_slot(
						app.audio,
						app.standard_sounds,
						1,
						2,
						127,
						0,
						64,
						0) >= 0;
				if (app.mission_lock_tone_active)
				{
					app.audio.voices[1].protected_from_eviction = true;
				}
			}
			else
			{
				sl_open::audio::fat_stop(app.audio, 1);
				app.mission_lock_tone_active = false;
			}
		}
		const sl_open::hud::Runtime& hud = app.mission_session.hud;
		if (hud.scanner_beep_serial
			!= app.mission_scanner_beep_serial)
		{
			app.mission_scanner_beep_serial =
				hud.scanner_beep_serial;
			if (!hud.scanner_tone_continuous)
			{
				int pan = 64;
				const sl_open::game::WorldObject* player =
					sl_open::game::world_resolve(
						app.mission_session.world,
						app.mission_session.world.player);
				const sl_open::game::WorldObject* tracked =
					hud.tracked_contact
							< std::size(
								app.mission_session.world.objects)
						? &app.mission_session.world.objects[
							hud.tracked_contact]
						: nullptr;
				if (player != nullptr && tracked != nullptr
					&& tracked->active)
				{
					const glm::vec3 displacement =
						tracked->position - player->position;
					const float distance = glm::length(displacement);
					if (distance > 0.0f)
					{
						pan = std::clamp(
							static_cast<int>(
								64.0f
								+ 63.0f
									* glm::dot(
										displacement,
										player->orientation[0])
									/ distance),
							0,
							127);
					}
				}
				// mission_update_frame 0x00492abe..0x00492afd plays
				// standard FAT sample 7 at the projected scanner-contact
				// pan position. A minimum interval keeps the same sample
				// looping in the protected slot below.
				sl_open::audio::fat_play_auto(
					app.audio,
					app.standard_sounds,
					7,
					127,
					1,
					pan,
					0);
			}
		}
		if (hud.scanner_tone_continuous
			!= app.mission_scanner_tone_active)
		{
			if (hud.scanner_tone_continuous)
			{
				app.mission_scanner_tone_active =
					sl_open::audio::fat_play_in_slot(
						app.audio,
						app.standard_sounds,
						2,
						7,
						127,
						0,
						64,
						0) >= 0;
				if (app.mission_scanner_tone_active)
				{
					app.audio.voices[2].protected_from_eviction = true;
				}
			}
			else
			{
				sl_open::audio::fat_stop(app.audio, 2);
				app.mission_scanner_tone_active = false;
			}
		}
		if (hud.threat_warning_tone
			!= app.mission_threat_tone_active)
		{
			if (hud.threat_warning_tone)
			{
				// HUD_render_frame_callback 0x00484e90..0x00484ed0
				// retains standard FAT sample zero at volume 60 while
				// warning channel zero is active.
				app.mission_threat_tone_active =
					sl_open::audio::fat_play_in_slot(
						app.audio,
						app.standard_sounds,
						3,
						0,
						60,
						0,
						64,
						0) >= 0;
				if (app.mission_threat_tone_active)
				{
					app.audio.voices[3].protected_from_eviction = true;
				}
			}
			else
			{
				sl_open::audio::fat_stop(app.audio, 3);
				app.mission_threat_tone_active = false;
			}
		}
		update_mission_presentation(app, now);
		}
	}
	if (app.standard_sounds.loading)
	{
		sl_open::audio::fat_bank_update(app.vfs, app.standard_sounds);
	}
	if (app.spatial_sounds.loading)
	{
		sl_open::audio::fat_bank_update(app.vfs, app.spatial_sounds);
	}
	if (app.vr_sounds.loading)
	{
		sl_open::audio::fat_bank_update(app.vfs, app.vr_sounds);
	}
	if (app.walk_sounds.loading)
	{
		sl_open::audio::fat_bank_update(app.vfs, app.walk_sounds);
	}
	if (app.briefing_sounds.loading)
	{
		sl_open::audio::fat_bank_update(app.vfs, app.briefing_sounds);
	}
	if (app.briefing_wait_sounds.loading)
	{
		sl_open::audio::fat_bank_update(app.vfs, app.briefing_wait_sounds);
	}
	if (app.loadout_sounds.loading)
	{
		sl_open::audio::fat_bank_update(app.vfs, app.loadout_sounds);
	}
	if (app.campaign_cinematic_sounds.loading)
	{
		const bool loaded = sl_open::audio::fat_bank_update(
			app.vfs, app.campaign_cinematic_sounds);
		if (app.campaign_sequence_waiting_audio
			&& (!app.campaign_cinematic_sounds.loading || !loaded))
		{
			app.campaign_sequence_waiting_audio = false;
			open_campaign_sequence_movie(app, now);
		}
	}
	if (app.hub_actor_sound.active)
	{
		sl_open::audio::mp3_stream_update(app.vfs, app.hub_actor_sound);
	}
	if (app.frontend_phase == FrontendPhase::campaign_hub)
	{
		start_hub_ambience(app);
	}
	sl_open::audio::fat_update_voices(app.audio);
	if (app.frontend_phase == FrontendPhase::mission_loading
		&& app.mission_loading_presented)
	{
		// Retail presents its loading callback before mission parsing and keeps
		// that frame on screen while renderer/world resources are rebuilt.
		// Completing the synchronous load here also leaves gameplay update until
		// the following SDL iteration.
		finish_live_mission_load(app);
	}
	else if (app.frontend_phase == FrontendPhase::transition)
	{
		const sl_open::media::BinkMovieState state =
			sl_open::media::bink_movie_update(app.vfs, now, app.transition_movie);
		if (state == sl_open::media::BinkMovieState::finished
			|| state == sl_open::media::BinkMovieState::failed)
		{
			if (state == sl_open::media::BinkMovieState::failed)
			{
				SDL_Log("Frontend transition playback failed");
			}
			else
			{
				SDL_Log("Frontend transition completed");
			}
			enter_transition_destination(app, now);
		}
	}
	else if (app.frontend_phase == FrontendPhase::campaign_induction)
	{
		const sl_open::media::BinkMovieState state =
			sl_open::media::bink_movie_update(
				app.vfs, now, app.transition_movie);
		if (app.campaign_induction.phase
			== sl_open::frontend::InductionPhase::narration)
		{
			const bool narration_finished =
				app.induction_narration_started
				&& !sl_open::audio::cb97_stream_update(
					app.vfs, app.campaign_speech);
			if (narration_finished
				|| state == sl_open::media::BinkMovieState::failed)
			{
				finish_induction_narration(app, now);
			}
		}
		else if (state == sl_open::media::BinkMovieState::finished
			|| state == sl_open::media::BinkMovieState::failed)
		{
			if (sl_open::frontend::campaign_induction_movie_finished(
				app.campaign_induction))
			{
				open_campaign_induction_media(app, now);
			}
			else
			{
				enter_campaign_hub(app, 10, now);
			}
		}
	}
	else if (app.frontend_phase == FrontendPhase::campaign_hub)
	{
		sl_open::frontend::campaign_hub_update_cursor(app.campaign_hub);
		const sl_open::media::BinkMovieState state =
			app.campaign_hub.action_pending
				? sl_open::media::BinkMovieState::finished
				: sl_open::media::bink_movie_update(
					app.vfs, now, app.campaign_movie);
		if (state == sl_open::media::BinkMovieState::finished
				|| state == sl_open::media::BinkMovieState::failed)
		{
			const sl_open::frontend::HubNode& node =
				sl_open::frontend::campaign_hub_node(app.campaign_hub);
			if (node.action != sl_open::frontend::HubAction::room)
			{
				if (node.action == sl_open::frontend::HubAction::fish_room)
				{
					if (app.campaign_hub.fish_active)
					{
						advance_fish_cycle(app, now);
					}
					else
					{
						enter_fish_cycle(app, now);
					}
				}
				else if (node.action == sl_open::frontend::HubAction::cd_player)
				{
					enter_cd_player(app);
				}
				else if (node.action == sl_open::frontend::HubAction::medals)
				{
					play_walk_sound(app, 7);
					enter_medal_display(app, now);
				}
				else if (node.action == sl_open::frontend::HubAction::itac)
				{
					app.itac_post_mission = false;
					enter_itac(app, false, now);
				}
				else if (node.action == sl_open::frontend::HubAction::news)
				{
					stop_hub_ambience(app);
					play_walk_sound(app, 2);
					enter_news_report(app, now);
				}
				else if (node.action == sl_open::frontend::HubAction::sim_pod)
				{
					play_walk_sound(app, 9);
					enter_sim_pod(app, now);
				}
				else if (node.action == sl_open::frontend::HubAction::briefing)
				{
					enter_mission_briefing(app, now);
				}
				else if (!app.campaign_hub.action_pending)
				{
					app.campaign_hub.action_pending = true;
					SDL_Log(
						"Campaign hub action %u reached",
						static_cast<unsigned>(node.action));
				}
			}
			else if (!app.campaign_hub.loop_active
				&& node.loop_movie != nullptr
				&& node.loop_movie[0] != '\0')
			{
				app.campaign_hub.loop_active = true;
				char path[sl_open::io::kMaxPath];
				std::snprintf(path, sizeof(path), "vr/%s", node.loop_movie);
				const sl_open::audio::Stream& stream = app.audio.streams[2];
				sl_open::media::bink_movie_open(
					app.vfs,
					path,
					app.audio.ready ? stream.source : 0,
					stream.buffers,
					now,
					app.campaign_movie);
				sl_open::media::bink_movie_set_looping(app.campaign_movie, true);
			}
			else if (!app.campaign_hub.loop_active)
			{
				app.campaign_hub.loop_active = true;
			}
			else if (app.campaign_hub.loop_active
				&& node.loop_movie != nullptr
				&& node.loop_movie[0] != '\0')
			{
				sl_open::media::bink_movie_set_looping(app.campaign_movie, true);
			}
		}
	}
	else if (app.frontend_phase == FrontendPhase::campaign_briefing)
	{
		if (app.mission_briefing.phase
			== sl_open::frontend::MissionBriefingPhase::awaiting_clearance)
		{
			if (sl_open::frontend::mission_briefing_wait_finished(
				app.mission_briefing, now))
			{
				stop_briefing_voice(app, app.briefing_wait_voice);
				app.briefing_room_voices[0] =
					play_fat_sample(app, app.briefing_sounds, 1);
				app.briefing_room_voices[1] =
					play_fat_sample(app, app.briefing_sounds, 2);
				open_mission_briefing_movie(app, now);
			}
		}
		else if (app.mission_briefing.mission == 29
			&& app.mission_briefing.phase
				== sl_open::frontend::MissionBriefingPhase::mission_video)
		{
			if (app.briefing_speech_started
				&& !sl_open::audio::cb97_stream_update(
					app.vfs, app.campaign_speech))
			{
				app.briefing_speech_started = false;
				advance_mission_briefing_movie(app, now);
			}
		}
		else if (sl_open::frontend::mission_briefing_movie(
			app.mission_briefing) != nullptr)
		{
			const sl_open::media::BinkMovieState state =
				sl_open::media::bink_movie_update(
					app.vfs, now, app.campaign_movie);
			if (state == sl_open::media::BinkMovieState::finished
				|| state == sl_open::media::BinkMovieState::failed)
			{
				advance_mission_briefing_movie(app, now);
			}
		}
		sl_open::frontend::mission_briefing_update_animation(
			app.mission_briefing, now);
		if (sl_open::frontend::mission_briefing_trigger_exit_speech(
				app.mission_briefing))
		{
			char path[sl_open::io::kMaxPath];
			std::snprintf(
				path,
				sizeof(path),
				"enrbr_tag%02u",
				static_cast<unsigned>(app.mission_briefing.mission));
			const sl_open::audio::Stream& stream = app.audio.streams[2];
			app.briefing_speech_started =
				sl_open::audio::cb97_stream_open(
					app.vfs,
					path,
					app.audio.ready ? stream.source : 0,
					stream.buffers,
					false,
					app.campaign_speech,
					sl_open::audio::speech_gain(app.audio));
			if (!app.briefing_speech_started)
			{
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
					"Briefing exit speech could not be opened: %s",
					path);
			}
		}
		if (app.mission_briefing.phase
				== sl_open::frontend::MissionBriefingPhase::exit_room
			&& app.briefing_speech_started
			&& !sl_open::audio::cb97_stream_update(
				app.vfs, app.campaign_speech))
		{
			app.briefing_speech_started = false;
		}
		if (sl_open::frontend::mission_briefing_exit_finished(
			app.mission_briefing, now))
		{
			leave_mission_briefing(app, now);
		}
	}
	else if (app.frontend_phase == FrontendPhase::campaign_loadout
		|| app.frontend_phase == FrontendPhase::multiplayer_loadout)
	{
		const sl_open::frontend::LoadoutPhase previous_phase = app.loadout.phase;
		const sl_open::frontend::LoadoutPage previous_page = app.loadout.page;
		const bool previous_selection = app.loadout.ship_selection_active;
		if (sl_open::frontend::loadout_update(app.loadout, app.loadout_catalog, now))
		{
			if (app.frontend_phase == FrontendPhase::multiplayer_loadout)
			{
				(void)submit_multiplayer_prelaunch_loadout(app);
			}
			else
			{
				leave_mission_loadout(app, now);
			}
		}
		else if (previous_phase == sl_open::frontend::LoadoutPhase::entering
			&& app.loadout.phase == sl_open::frontend::LoadoutPhase::active)
		{
			play_fat_sample(app, app.loadout_sounds, 6);
		}
		if (sl_open::frontend::loadout_pointer(app.loadout, app.loadout_catalog,
			app.loadout.pointer_x, app.loadout.pointer_y, now))
		{
			play_fat_sample(app, app.loadout_sounds, 7, 40);
		}
		if (!previous_selection && app.loadout.ship_selection_active)
		{
			play_fat_sample(app, app.loadout_sounds, 8);
		}
		if (previous_page != sl_open::frontend::LoadoutPage::guns
			&& app.loadout.page == sl_open::frontend::LoadoutPage::guns)
		{
			play_fat_sample(app, app.loadout_sounds, 10);
		}
		if (previous_phase == sl_open::frontend::LoadoutPhase::active
			&& app.loadout.phase == sl_open::frontend::LoadoutPhase::exiting)
		{
			if (app.loadout_ambience_voice >= 0)
			{
				sl_open::audio::fat_stop(app.audio,
					static_cast<std::uint32_t>(app.loadout_ambience_voice));
				app.loadout_ambience_voice = -1;
			}
			play_fat_sample(app, app.loadout_sounds, 2);
		}
	}
	else if (app.frontend_phase
		== FrontendPhase::multiplayer_launch_wait)
	{
		service_multiplayer_prelaunch_barrier(app);
	}
	else if (app.frontend_phase == FrontendPhase::campaign_takeoff_movie)
	{
		const sl_open::media::BinkMovieState state =
			sl_open::media::bink_movie_update(app.vfs, now, app.campaign_movie);
		if (state == sl_open::media::BinkMovieState::finished
			|| state == sl_open::media::BinkMovieState::failed)
		{
			launch_campaign_mission(app, now);
		}
	}
	else if (app.frontend_phase == FrontendPhase::campaign_award_movie)
	{
		const sl_open::media::BinkMovieState state =
			sl_open::media::bink_movie_update(app.vfs, now, app.campaign_movie);
		if (state == sl_open::media::BinkMovieState::finished
			|| state == sl_open::media::BinkMovieState::failed)
		{
			if (app.multiplayer_mission_presentation
				== sl_open::platform::
					MultiplayerMissionPresentation::award)
			{
				finish_multiplayer_award_movie(app, now);
			}
			else
			{
				route_after_mission(app, now);
			}
		}
	}
	else if (app.frontend_phase == FrontendPhase::campaign_failure_movie)
	{
		const sl_open::media::BinkMovieState state =
			sl_open::media::bink_movie_update(app.vfs, now, app.campaign_movie);
		if (state == sl_open::media::BinkMovieState::finished
			|| state == sl_open::media::BinkMovieState::failed)
		{
			enter_post_mission_choice(app, now);
		}
	}
	else if (app.frontend_phase
		== FrontendPhase::campaign_post_mission_movie)
	{
		if (!app.campaign_sequence_waiting_audio)
		{
			const sl_open::media::BinkMovieState state =
				sl_open::media::bink_movie_update(
					app.vfs, now, app.campaign_movie);
			if (state == sl_open::media::BinkMovieState::finished
				|| state == sl_open::media::BinkMovieState::failed)
			{
				++app.campaign_sequence_index;
				open_campaign_sequence_movie(app, now);
			}
		}
	}
	else if (app.frontend_phase == FrontendPhase::campaign_sim_pod
		&& app.sim_pod.phase != sl_open::frontend::SimPodPhase::active
		&& app.sim_pod.phase != sl_open::frontend::SimPodPhase::complete)
	{
		const sl_open::media::BinkMovieState state =
			sl_open::media::bink_movie_update(
				app.vfs, now, app.campaign_movie);
		if (state == sl_open::media::BinkMovieState::finished
			|| state == sl_open::media::BinkMovieState::failed)
		{
			sl_open::frontend::sim_pod_movie_finished(app.sim_pod);
			if (app.sim_pod.phase == sl_open::frontend::SimPodPhase::complete)
			{
				leave_sim_pod(app, now);
			}
		}
	}
	else if (app.frontend_phase == FrontendPhase::campaign_news)
	{
		sl_open::media::bink_movie_update(
			app.vfs, now, app.campaign_movie);
		if (app.news_narration_started
			&& !sl_open::audio::cb97_stream_update(
				app.vfs, app.campaign_speech))
		{
			advance_news_report(app, now);
		}
	}
	else if (app.frontend_phase == FrontendPhase::campaign_cd
		&& app.cd_music.active
		&& !app.cd_player.paused
		&& !sl_open::audio::wav_stream_update(app.vfs, app.cd_music))
	{
		app.cd_player.started = false;
		if (app.cd_player.random)
		{
			const std::int8_t previous = app.cd_player.selected;
			for (std::uint8_t attempt = 0; attempt < 21; ++attempt)
			{
				app.cd_player.selected =
					static_cast<std::int8_t>(std::rand() % 12);
				if (app.cd_player.selected != previous)
				{
					break;
				}
			}
			app.cd_player.retail_navigation_path = true;
			play_cd_track(app);
		}
		else if (app.cd_player.selected < 11)
		{
			++app.cd_player.selected;
			app.cd_player.retail_navigation_path = true;
			play_cd_track(app);
		}
	}
	else if (app.frontend_phase == FrontendPhase::campaign_medals)
	{
		const sl_open::media::BinkMovieState state =
			sl_open::media::bink_movie_update(app.vfs, now, app.campaign_movie);
		if (state == sl_open::media::BinkMovieState::finished
			|| state == sl_open::media::BinkMovieState::failed)
		{
			if (sl_open::frontend::medal_display_movie_finished(
				app.medal_display))
			{
				open_medal_movie(app, now);
			}
			else if (app.medal_display.phase
				== sl_open::frontend::MedalPhase::complete)
			{
				leave_medal_display(app, now);
			}
		}
	}
	else if (app.frontend_phase == FrontendPhase::credits
		&& !sl_open::frontend::credits_update(app.credits, now))
	{
		finish_credits(app, now);
	}
	else if (app.frontend_phase == FrontendPhase::campaign_itac
		&& app.itac_shell.phase != sl_open::frontend::ItacPhase::active)
	{
		const sl_open::media::BinkMovieState state =
			sl_open::media::bink_movie_update(app.vfs, now, app.campaign_movie);
		if (state == sl_open::media::BinkMovieState::finished
			|| state == sl_open::media::BinkMovieState::failed)
		{
			finish_itac_movie(app, now);
		}
	}
	if (app.menu_music.active)
	{
		sl_open::audio::wav_stream_update(app.vfs, app.menu_music);
	}
	if (app.credits_music.active)
	{
		sl_open::audio::wav_stream_update(app.vfs, app.credits_music);
	}
	if (app.frontend_renderer.ready)
	{
		bool full_movie_viewport = false;
		const bool native_canvas =
			app.frontend_phase == FrontendPhase::instant_action;
		if (app.frontend_phase == FrontendPhase::transition
			|| app.frontend_phase == FrontendPhase::campaign_induction)
		{
			if (app.transition_movie.frame_visible
				|| !app.transition_holds_frame)
			{
				sl_open::render::frontend_commands_begin(
					app.frontend_commands);
				const bool standalone_movie =
					(app.frontend_phase == FrontendPhase::transition
						&& app.transition_destination
							== TransitionDestination::startup_logo)
					|| (app.frontend_phase
							== FrontendPhase::campaign_induction
						&& app.campaign_induction.phase
							== sl_open::frontend::InductionPhase::pre_roll
						&& app.campaign_induction.pre_roll_index == 0);
				full_movie_viewport =
					app.config.expand_widescreen_movies
					&& standalone_movie
					&& sl_open::media::bink_movie_is_widescreen(
						app.transition_movie);
				if (full_movie_viewport)
				{
					sl_open::media::bink_movie_build_fullscreen(
						app.transition_movie,
						app.frontend_commands,
						app.width,
						app.height);
				}
				else
				{
					sl_open::media::bink_movie_build(
						app.transition_movie, app.frontend_commands);
				}
			}
		}
		else
		{
			sl_open::render::frontend_commands_begin(app.frontend_commands);
			if (app.frontend_phase == FrontendPhase::mission_loading)
			{
				if (app.mission_clock_warmup)
				{
					sl_open::render::MissionRenderFrame mission_frame;
					sl_open::game::mission_session_render_frame(
						app.mission_session, mission_frame);
					sl_open::render::mission_renderer_submit(
						app.mission_renderer,
						app.frontend_renderer,
						mission_frame,
						app.width,
						app.height,
						static_cast<float>(app.config.brightness)
							/ 100.0f);
				}
				sl_open::render::frontend_rgba_quad(
					app.frontend_commands,
					app.frontend_renderer.shell.mission_loading_splash,
					0.0f,
					0.0f,
					640.0f,
					480.0f);
				const std::uint32_t string_id =
					app.pending_mission_request.mode
						== sl_open::game::MissionMode::training
						? 0x14b
						: app.pending_mission_request.mode
							== sl_open::game::MissionMode::instant_action
							? 0x289
							: 0xe2;
				const char* loading_text =
					sl_open::language_text(app.language, string_id);
				const float loading_width =
					sl_open::frontend::gui::text_width(
						app.frontend_renderer.shell.glyphs,
						app.frontend_renderer.shell.glyph_count,
						loading_text);
				sl_open::render::frontend_text(
					app.frontend_commands,
					app.frontend_renderer,
					loading_text,
					320.0f - loading_width * 0.5f,
					440.0f,
					app.frontend_renderer.shell.font_white_palette,
					0xffffffff);
			}
			else if (app.frontend_phase == FrontendPhase::instant_action)
			{
				if (app.mission_foster_active)
				{
					sl_open::media::bink_movie_build_native_fit(
						app.mission_foster_movie,
						app.frontend_commands,
						app.width,
						app.height);
				}
				else
				{
					sl_open::render::MissionRenderFrame mission_frame;
					sl_open::game::mission_session_render_frame(
						app.mission_session, mission_frame);
					sl_open::render::mission_renderer_submit(
						app.mission_renderer,
						app.frontend_renderer,
						mission_frame,
						app.width,
						app.height,
						static_cast<float>(app.config.brightness) / 100.0f);
					sl_open::game::mission_session_build(
						app.mission_session,
						app.config,
						app.language,
						app.frontend_renderer,
						app.mission_renderer,
						mission_frame,
						app.frontend_commands,
						app.width,
						app.height,
						now);
				}
			}
			else if (app.frontend_phase == FrontendPhase::main_menu)
			{
				sl_open::frontend::main_menu_build(
					app.main_menu,
					app.language,
					app.frontend_renderer,
					app.frontend_commands,
					now);
			}
			else if (app.frontend_phase
				== FrontendPhase::multiplayer_frontend)
			{
				const MultiplayerFrontendSessions sessions =
					multiplayer_frontend_sessions(app);
				sl_open::frontend::multiplayer_frontend_build(
					app.multiplayer_frontend,
					sessions.view,
					app.language,
					app.frontend_renderer,
					app.frontend_commands,
					now);
			}
			else if (app.frontend_phase
				== FrontendPhase::multiplayer_lobby)
			{
				const sl_open::frontend::MultiplayerLobbyView view =
					multiplayer_lobby_view(app);
				sl_open::frontend::multiplayer_lobby_build(
					app.multiplayer_lobby,
					view,
					app.language,
					app.frontend_renderer,
					app.frontend_commands,
					now);
			}
			else if (app.frontend_phase == FrontendPhase::options)
			{
				sl_open::frontend::options_build(
					app.options,
					app.language,
					app.frontend_renderer,
					app.config,
					app.audio.hrtf_supported,
					app.joystick_available,
					app.frontend_commands,
					now);
			}
			else if (app.frontend_phase == FrontendPhase::campaign
				|| app.frontend_phase
					== FrontendPhase::multiplayer_campaign_load)
			{
				sl_open::frontend::campaign_frontend_build(
					app.campaign_frontend,
					app.campaign_store,
					app.frontend_phase == FrontendPhase::campaign
						? app.campaign
						: app.multiplayer_campaign,
					app.language,
					app.frontend_renderer,
					app.frontend_commands,
				now);
		}
		else if (app.frontend_phase == FrontendPhase::campaign_hub)
		{
			sl_open::media::bink_movie_build(
				app.campaign_movie, app.frontend_commands);
			for (std::uint32_t slot = 0; slot < 2; ++slot)
			{
				if (!app.hub_actor_active[slot])
				{
					continue;
				}
				const std::uint32_t frame =
					app.campaign_movie.frame_index;
				const std::uint32_t start =
					app.hub_actor_start_frame[slot];
				if (frame < start)
				{
					continue;
				}
				const std::uint32_t shape = frame - start;
				const std::uint32_t count =
					app.hub_actor_frame_count[slot];
				if (shape >= count)
				{
					app.hub_actor_active[slot] = false;
					continue;
				}
				const sl_open::render::FrontendVrAmbientAssets& ambient =
					app.frontend_renderer.vr_ambient[slot];
				if (ambient.ready
					&& shape < ambient.shape_count
					&& bgfx::isValid(ambient.shapes[shape].handle))
				{
					sl_open::render::frontend_indexed_quad(
						app.frontend_commands,
						ambient.shapes[shape],
						ambient.palette,
						1.0f,
						1.0f);
				}
			}
			if (app.campaign_hub.loop_active)
			{
				const std::uint32_t edge_state =
					app.campaign_hub.cursor_edge_state;
				std::uint32_t action_cursor = UINT32_MAX;
				if (app.campaign_hub.hovered >= 0)
				{
					const sl_open::frontend::HubNode& node =
						sl_open::frontend::campaign_hub_node(app.campaign_hub);
					const std::uint8_t destination_index =
						node.destinations[app.campaign_hub.hovered];
					const sl_open::frontend::CampaignHub destination_hub{
						.node = destination_index,
						.late_campaign = app.campaign_hub.late_campaign};
					const sl_open::frontend::HubNode& destination =
						sl_open::frontend::campaign_hub_node(destination_hub);
					if (destination.action != sl_open::frontend::HubAction::room)
					{
						const std::uint32_t animation =
							static_cast<std::uint32_t>((now / 15) % 9);
						const std::uint32_t first =
							edge_state == 0
								? 31
								: (edge_state == 21 ? 40 : 22);
						action_cursor = first + animation;
					}
					const char* label = sl_open::language_text(
						app.language, destination.label_id);
					constexpr float kLabelScale = 0.7f;
					const float label_width =
						sl_open::frontend::gui::text_width(
							app.frontend_renderer.shell.glyphs,
							app.frontend_renderer.shell.glyph_count,
							label,
							kLabelScale);
					sl_open::render::frontend_text(
						app.frontend_commands,
						app.frontend_renderer,
						label,
						sl_open::frontend::gui::aligned_x(
							320.0f,
							label_width,
							sl_open::frontend::gui::TextAlign::center),
						440.0f,
						app.frontend_renderer.shell.font_white_palette,
						0xffffffff,
						kLabelScale);
				}
				sl_open::render::frontend_indexed_quad(
					app.frontend_commands,
					app.frontend_renderer.vr.cursor[edge_state],
					app.frontend_renderer.vr.cursor_palette,
					app.campaign_hub.pointer_x,
					app.campaign_hub.pointer_y);
				if (action_cursor != UINT32_MAX)
				{
					sl_open::render::frontend_indexed_quad(
						app.frontend_commands,
						app.frontend_renderer.vr.cursor[action_cursor],
						app.frontend_renderer.vr.cursor_palette,
						app.campaign_hub.pointer_x,
						app.campaign_hub.pointer_y);
				}
			}
		}
		else if (app.frontend_phase == FrontendPhase::campaign_briefing)
		{
			sl_open::frontend::mission_briefing_build(
				app.mission_briefing,
				app.language,
				app.frontend_renderer,
				app.campaign_movie,
				app.frontend_commands,
				now);
		}
		else if (app.frontend_phase == FrontendPhase::campaign_loadout
			|| app.frontend_phase
				== FrontendPhase::multiplayer_loadout
			|| app.frontend_phase
				== FrontendPhase::multiplayer_launch_wait)
		{
			sl_open::frontend::loadout_build(
				app.loadout,
				app.loadout_catalog,
				app.language,
				app.frontend_renderer,
				app.frontend_commands,
				now);
		}
		else if (app.frontend_phase == FrontendPhase::multiplayer_debrief)
		{
			const sl_open::frontend::MultiplayerDebriefView view =
				multiplayer_debrief_view(app);
			sl_open::frontend::multiplayer_debrief_build(
				app.multiplayer_debrief,
				view,
				app.language,
				app.itac_language,
				app.frontend_renderer,
				app.frontend_commands,
				now);
		}
		else if (app.frontend_phase == FrontendPhase::campaign_award_movie
			|| app.frontend_phase == FrontendPhase::campaign_takeoff_movie)
		{
			full_movie_viewport =
				app.config.expand_widescreen_movies
				&& sl_open::media::bink_movie_is_widescreen(app.campaign_movie);
			if (full_movie_viewport)
			{
				sl_open::media::bink_movie_build_fullscreen(
					app.campaign_movie,
					app.frontend_commands,
					app.width,
					app.height);
			}
			else
			{
				sl_open::media::bink_movie_build(
					app.campaign_movie, app.frontend_commands);
			}
		}
		else if (app.frontend_phase == FrontendPhase::campaign_failure_movie)
		{
			full_movie_viewport =
				app.config.expand_widescreen_movies
				&& sl_open::media::bink_movie_is_widescreen(app.campaign_movie);
			if (full_movie_viewport)
			{
				sl_open::media::bink_movie_build_fullscreen(
					app.campaign_movie,
					app.frontend_commands,
					app.width,
					app.height);
			}
			else
			{
				sl_open::media::bink_movie_build(
					app.campaign_movie, app.frontend_commands);
			}
		}
		else if (app.frontend_phase
			== FrontendPhase::campaign_post_mission_movie)
		{
			if (app.campaign_sequence_waiting_audio)
			{
				sl_open::render::frontend_commands_begin(app.frontend_commands);
			}
			else
			{
				full_movie_viewport =
					app.config.expand_widescreen_movies
					&& sl_open::media::bink_movie_is_widescreen(
						app.campaign_movie);
				if (full_movie_viewport)
				{
					sl_open::media::bink_movie_build_fullscreen(
						app.campaign_movie,
						app.frontend_commands,
						app.width,
						app.height);
				}
				else
				{
					sl_open::media::bink_movie_build(
						app.campaign_movie,
						app.frontend_commands);
				}
			}
		}
		else if (app.frontend_phase
			== FrontendPhase::campaign_restart_choice)
		{
			sl_open::frontend::post_mission_choice_build(
				app.post_mission_choice,
				app.language,
				app.frontend_renderer,
				app.frontend_commands,
				now);
		}
		else if (app.frontend_phase == FrontendPhase::campaign_sim_pod)
		{
			if (app.sim_pod.phase == sl_open::frontend::SimPodPhase::active)
			{
				sl_open::frontend::sim_pod_build(
					app.sim_pod,
					app.language,
					app.frontend_renderer,
					app.frontend_commands,
					now);
			}
			else
			{
				sl_open::media::bink_movie_build(
					app.campaign_movie, app.frontend_commands);
			}
		}
		else if (app.frontend_phase == FrontendPhase::campaign_news)
		{
			sl_open::media::bink_movie_build(
				app.campaign_movie, app.frontend_commands);
		}
		else if (app.frontend_phase == FrontendPhase::campaign_cd)
		{
			sl_open::frontend::cd_player_build(
				app.cd_player,
				app.language,
				app.frontend_renderer,
				app.frontend_commands,
				now);
		}
		else if (app.frontend_phase == FrontendPhase::campaign_medals)
		{
			sl_open::media::bink_movie_build(
				app.campaign_movie, app.frontend_commands);
			if (app.medal_display.phase
				!= sl_open::frontend::MedalPhase::zoom)
			{
				sl_open::frontend::medal_display_build(
					app.medal_display,
					app.campaign,
					app.language,
					app.frontend_renderer,
					app.frontend_commands,
					app.campaign_movie.frame_index,
					now);
			}
		}
		else if (app.frontend_phase == FrontendPhase::credits)
		{
			sl_open::frontend::credits_build(
				app.credits,
				app.language,
				app.frontend_renderer,
				app.frontend_commands);
		}
		else if (app.frontend_phase == FrontendPhase::campaign_itac)
		{
			if (app.itac_shell.phase == sl_open::frontend::ItacPhase::active)
			{
				sl_open::frontend::itac_shell_build(
					app.itac_shell,
					app.language,
					app.itac_language,
					app.frontend_renderer,
					app.frontend_commands,
					now);
			}
			else
			{
				const bool waiting_for_tab_movie =
					!app.campaign_movie.frame_visible
					&& (app.itac_shell.phase
							== sl_open::frontend::ItacPhase::leaving
						|| app.itac_shell.phase
							== sl_open::frontend::ItacPhase::entering);
				if (waiting_for_tab_movie
					&& app.itac_shell.phase
						== sl_open::frontend::ItacPhase::leaving)
				{
					sl_open::frontend::itac_shell_build(
						app.itac_shell,
						app.language,
						app.itac_language,
						app.frontend_renderer,
						app.frontend_commands,
						now);
				}
				else if (waiting_for_tab_movie)
				{
					sl_open::render::frontend_rgba_quad(
						app.frontend_commands,
						app.frontend_renderer.itac.backgrounds[
							app.itac_shell.active_tab],
						0.0f,
						0.0f,
						640.0f,
						480.0f);
				}
				else
				{
					sl_open::media::bink_movie_build(
						app.campaign_movie, app.frontend_commands);
				}
			}
			}
		}
		sl_open::render::frontend_submit(
			app.frontend_renderer,
			app.frontend_commands,
			app.width,
			app.height,
			static_cast<float>(app.config.brightness) / 100.0f,
			full_movie_viewport,
			native_canvas);
		if (app.frontend_phase == FrontendPhase::mission_loading)
		{
			if (app.mission_clock_warmup)
			{
				// The first mission submission realizes retained GPU resources
				// underneath the higher-numbered loading-screen view. Only the
				// following SDL iteration may expose gameplay.
				sl_open::game::simulation_clock_reset(
					app.mission_session.clock,
					SDL_GetTicks());
				app.mission_clock_warmup = false;
				app.frontend_phase = FrontendPhase::instant_action;
			}
			else
			{
				app.mission_loading_presented = true;
			}
		}
	}
	else
	{
		draw_boot_screen(app);
	}

	++app.frame_count;
	if (app.frame_limit != 0 && app.frame_count >= app.frame_limit)
	{
		return SDL_APP_SUCCESS;
	}
	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult)
{
	auto* app = static_cast<App*>(appstate);
	if (app != nullptr)
	{
		sl_open::network::multiplayer_transport_shutdown(
			app->multiplayer_transport);
		if (app->mission_lock_tone_active)
		{
			sl_open::audio::fat_stop(app->audio, 1);
			app->mission_lock_tone_active = false;
		}
		if (app->mission_scanner_tone_active)
		{
			sl_open::audio::fat_stop(app->audio, 2);
			app->mission_scanner_tone_active = false;
		}
		if (app->mission_threat_tone_active)
		{
			sl_open::audio::fat_stop(app->audio, 3);
			app->mission_threat_tone_active = false;
		}
		destroy_mission_sound_runtime(*app);
		sl_open::game::mission_session_stop(app->mission_session);
		sl_open::media::bink_movie_shutdown(app->vfs, app->transition_movie);
		sl_open::media::bink_movie_shutdown(app->vfs, app->campaign_movie);
		sl_open::media::bink_movie_shutdown(
			app->vfs, app->mission_foster_movie);
		sl_open::audio::cb97_stream_close(app->vfs, app->campaign_speech);
		sl_open::audio::cb97_stream_close(app->vfs, app->mission_speech);
		sl_open::audio::cb97_stream_close(
			app->vfs, app->mission_command_speech);
		sl_open::audio::music_close(app->vfs, app->mission_music);
		sl_open::audio::wav_stream_close(app->vfs, app->cd_music);
		sl_open::audio::wav_stream_close(app->vfs, app->credits_music);
	}
	if (app != nullptr && app->frontend_renderer.ready)
	{
		sl_open::render::mission_renderer_shutdown(app->mission_renderer);
		sl_open::render::frontend_renderer_shutdown(app->frontend_renderer);
		const sl_open::render::FrontendTextureStats texture_stats =
			sl_open::render::frontend_texture_stats();
		SDL_Log(
			"Frontend GPU texture shutdown: created=%u destroyed=%u live=%u "
			"high-water=%u",
			texture_stats.created,
			texture_stats.destroyed,
			texture_stats.live,
			texture_stats.high_water);
		SDL_ShowCursor();
	}
	if (app != nullptr)
	{
		sl_open::audio::wav_stream_close(app->vfs, app->menu_music);
		sl_open::audio::fat_bank_close(app->vfs, app->walk_sounds);
		sl_open::audio::fat_bank_close(app->vfs, app->vr_sounds);
		sl_open::audio::fat_bank_close(app->vfs, app->standard_sounds);
		sl_open::audio::fat_bank_close(app->vfs, app->spatial_sounds);
		sl_open::audio::fat_bank_close(app->vfs, app->briefing_sounds);
		sl_open::audio::fat_bank_close(app->vfs, app->briefing_wait_sounds);
		sl_open::audio::fat_bank_close(app->vfs, app->loadout_sounds);
		sl_open::audio::fat_bank_close(
			app->vfs, app->campaign_cinematic_sounds);
		sl_open::audio::mp3_stream_close(app->vfs, app->hub_actor_sound);
		sl_open::audio::shutdown(app->audio);
	}
	if (app != nullptr)
	{
		close_game_controller(*app);
		sl_open::platform::display_shutdown(*app);
		if (app->vfs.filesystem != nullptr
			&& !sl_open::config_save(app->filesystem, app->config))
		{
			SDL_Log("Configuration could not be saved");
		}
		sl_open::io::vfs_shutdown(app->vfs);
		if (app->filesystem != nullptr)
		{
			SDL_EMFS_Destroy(app->filesystem);
		}
		app->filesystem = nullptr;
	}
}
