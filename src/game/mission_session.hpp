#pragma once

#include "assets/game_stats.hpp"
#include "frontend/gameplay_pause.hpp"
#include "game/camera_runtime.hpp"
#include "game/chaff.hpp"
#include "game/clock.hpp"
#include "game/missiles.hpp"
#include "game/player_controls.hpp"
#include "game/session.hpp"
#include "game/weapons.hpp"
#include "game/world.hpp"
#include "hud/runtime.hpp"
#include "input/gameplay_input.hpp"
#include "mission/dte.hpp"
#include "mission/executor.hpp"
#include "mission/runtime.hpp"

#include <cstdint>

namespace sl_open
{
struct Config;
struct LanguageTable;
}

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::render
{
struct FrontendCommands;
struct FrontendRenderer;
struct MissionRenderFrame;
struct MissionRenderer;
}

namespace sl_open::game
{
enum class MissionSessionState : std::uint8_t
{
	inactive,
	running,
	paused,
	load_failed,
};

struct MissionSession
{
	MissionLaunchRequest request;
	SessionResult result;
	mission::DteFile mission_file;
	mission::Runtime mission_runtime;
	mission::Executor executor;
	assets::GunStatsTable gun_stats;
	assets::PilotStatsTable pilot_stats;
	assets::MissileStatsTable missile_stats;
	assets::ShipStatsTable ship_stats;
	World world;
	ChaffRuntime chaff;
	MissileRuntime missiles;
	WeaponRuntime weapons;
	CameraRuntime camera;
	SimulationClock clock;
	std::uint32_t script_tick{};
	FlightDemand flight_demand;
	// DAT_0051cf7c is the process-retained digital target throttle. It is
	// distinct from the object demand written by an analog throttle axis.
	float manual_throttle{};
	PlayerControlState player_control_state;
	hud::Runtime hud;
	frontend::GameplayPause pause;
	MissionSessionState state{MissionSessionState::inactive};
	// Pause receive case 0x27 retains the lowest source slot for display and
	// promotes reason zero only when a later nonzero reason arrives. Network
	// delivery has no drawable/time context, so the resulting state transition
	// is consumed at the start of mission_session_update.
	std::uint8_t network_pause_owner{UINT8_MAX};
	mission::NetworkPauseReason network_pause_reason{
		mission::NetworkPauseReason::player_request};
	std::uint32_t network_pause_latency[
		mission::kNetworkPlayerCapacity]{};
	bool network_pause_rejectable[
		mission::kNetworkPlayerCapacity]{};
	bool network_pause_transition_pending{};
	bool network_pause_transition_active{};
	mission::DteLoadError load_error{mission::DteLoadError::none};
	float match_speed_saved_throttle{};
	bool match_speed_latched{};
	float error_pointer_x{};
	float error_pointer_y{};
	bool error_return_hovered{};
	bool previous_actions[kControlActionCount]{};
	bool pending_actions[kControlActionCount]{};
	bool afterburner_toggle{};
	std::uint32_t propulsion_warning_deadline{};
	bool chat_submit_consumed{};
	std::uint8_t camera_mode{};
	std::uint8_t previous_frame_camera_mode{};
	std::uint32_t camera_cut_serial{};
	std::uint32_t mission_camera_request_serial{};
	std::uint32_t mission_director_serial{};
	std::uint32_t mission_match_speed_request_serial{};
	std::uint32_t mission_player_motion_clear_serial{};
	std::uint32_t mission_player_ordnance_rebuild_serial{};
	std::uint32_t mission_instrument_serials[20]{};
	ObjectHandle configured_player;
	std::int32_t live_player_score{};
	std::uint64_t entered_at{};
	char load_error_detail[96]{};
};

bool mission_session_start(
	MissionSession& session,
	io::Vfs& vfs,
	const assets::GameStats& stats,
	const MissionLaunchRequest& request,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now);
// Captures the mutable retail opcode-9 image after gameplay so the transport
// can carry PILO/ALPH losses and all 64 session dwords into Continue while
// retaining the immutable launch image separately for Replay.
bool mission_session_capture_multiplayer_bootstrap(
	const MissionSession& session,
	MultiplayerMissionBootstrap& bootstrap);
bool mission_session_service_object_activations(
	MissionSession& session);
void mission_session_stop(MissionSession& session);
// Coordinator state nine owns a final frozen gameplay frame until the player
// acknowledges the retail online-termination notice.
bool mission_session_begin_network_termination_acknowledgement(
	MissionSession& session,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now);
void mission_session_set_pause(
	MissionSession& session,
	bool active,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now,
	mission::NetworkPauseReason reason =
		mission::NetworkPauseReason::player_request);
void mission_session_toggle_pause(
	MissionSession& session,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now);
// Supplies the transport's exact retail link-age values. The smoothed value
// is in one-way 100 Hz ticks; the raw eight-bit value feeds script/network
// scheduling. Online pause uses strict >100 reject and >150 auto-pause gates.
void mission_session_update_network_latency(
	MissionSession& session,
	std::uint8_t player,
	std::uint32_t smoothed_one_way_ticks,
	std::uint8_t raw_one_way_ticks,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now);
void mission_session_set_pointer(
	MissionSession& session,
	const render::FrontendRenderer& renderer,
	float x,
	float y,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	bool inside);
frontend::GameplayPauseAction mission_session_select(
	MissionSession& session);
void mission_session_request_exit(MissionSession& session);
void mission_session_fail_load(
	MissionSession& session,
	const char* detail);
void mission_session_update(
	MissionSession& session,
	const Config& config,
	const LanguageTable& language,
	input::GameplayInputPoller& input_poller,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now);
// Transport-neutral gameplay boundary. A transport adapter supplies one
// decoded semantic message at a time and drains publications in FIFO order;
// packet framing and socket ownership remain outside MissionSession.
bool mission_session_receive_network_message(
	MissionSession& session,
	const mission::NetworkOutboundMessage& message);
bool mission_session_pop_network_outbound(
	MissionSession& session,
	mission::NetworkOutboundMessage& message);
bool mission_session_chat_text(
	MissionSession& session,
	const render::FrontendRenderer& renderer,
	const char* text);
void mission_session_chat_backspace(MissionSession& session);
bool mission_session_chat_submit(MissionSession& session);
void mission_session_render_frame(
	MissionSession& session,
	render::MissionRenderFrame& frame);
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
	std::uint64_t now);
}
