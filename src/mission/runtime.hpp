#pragma once

#include "config/config.hpp"
#include "game/runtime_limits.hpp"
#include "game/session.hpp"
#include "game/world.hpp"
#include "mission/deathmatch_scenarios.hpp"
#include "mission/dte.hpp"
#include "mission/environment_effects.hpp"
#include "mission/network_runtime.hpp"
#include "mission/player_comms.hpp"

#include <cstddef>
#include <cstdint>

namespace sl_open::mission
{
constexpr std::size_t kRetailPilotCatalogCount = 65;

enum class ReferenceKind : std::uint8_t
{
	object,
	group,
	set,
};

struct ObjectRecord
{
	game::ObjectHandle live;
	// Retail's writable 0x4c-byte mission-object record keeps the current
	// script position at +0x08 and the immutable authored origin at +0x1c.
	glm::vec3 script_position{0.0f};
	glm::vec3 authored_position{0.0f};
	glm::mat3 authored_orientation{1.0f};
	std::uint16_t reference_span{UINT16_MAX};
	std::uint16_t type{};
	std::uint16_t name_offset{UINT16_MAX};
	std::uint16_t launch_source_type{UINT16_MAX};
	// AI_ObjectAttach_begin reads the descriptor index directly from the
	// writable retail mission-object record at +0x34.
	std::uint16_t attachment_descriptor{UINT16_MAX};
	std::uint8_t group{UINT8_MAX};
	std::uint8_t pilot{UINT8_MAX};
	std::uint8_t launch_point{UINT8_MAX};
	std::uint8_t script_flags{};
	std::uint8_t spawn_mode{};
	std::uint32_t live_component_mask{UINT32_MAX};
	bool activation_service_pending{};
};

struct GroupRecord
{
	std::uint16_t reference_span{UINT16_MAX};
	std::uint16_t first_member{};
	std::uint16_t member_count{};
	std::uint8_t object_class{};
};

struct ReferenceSpan
{
	std::uint16_t first_trigger{UINT16_MAX};
	std::uint8_t owner_kind{UINT8_MAX};
	std::uint8_t trigger_count{};
};

struct TriggerRecord
{
	std::uint32_t condition[5]{};
	std::uint16_t script_word{UINT16_MAX};
	std::uint8_t type{UINT8_MAX};
	std::uint8_t repeat_mode{};
	std::uint8_t enabled{};
	std::uint8_t selector{UINT8_MAX};
	std::uint8_t deferred{};
	std::uint8_t repeats_remaining{};
	std::uint8_t authored_repeats{};
};

struct ReferenceSetRecord
{
	std::uint16_t reference_span{UINT16_MAX};
	std::uint16_t first_link{UINT16_MAX};
};

struct ReferenceLink
{
	std::uint16_t reference_span{UINT16_MAX};
	std::uint16_t owner_set{UINT16_MAX};
	std::uint8_t selector{UINT8_MAX};
};

struct Subtype997Pair
{
	std::uint16_t group{UINT16_MAX};
	std::uint16_t object{UINT16_MAX};
};

struct ExpandedTargetReference
{
	std::uint16_t object{UINT16_MAX};
	std::int16_t model{-1};
};

struct QueuedEvent
{
	std::uint32_t arguments[game::kMissionEventArguments]{};
	std::uint16_t source{UINT16_MAX};
	std::uint8_t type{UINT8_MAX};
	std::uint8_t argument_count{};
	std::uint8_t selector{UINT8_MAX};
	bool propagate_scopes{};
};

struct PresentationRequest
{
	std::uint16_t pilot{UINT16_MAX};
	std::uint16_t voice{UINT16_MAX};
	std::uint16_t speaker_mission_index{UINT16_MAX};
	char speech_path[64]{};
	// HOG_bigread2's PlaySpeech path work buffer is 128 bytes.
	char standalone_speech_path[128]{};
	char command_speech_path[64]{};
	char movie_path[64]{};
	char music_path[128]{};
	char subtitle[128]{};
	char debug_message[160]{};
	bool comms_pending{};
	bool comms_active{};
	bool speech_pending{};
	bool speech_active{};
	bool command_speech_pending{};
	bool command_speech_active{};
	bool movie_pending{};
	bool movie_active{};
	bool music_pending{};
	bool music_stop_pending{};
	std::uint32_t slot_zero_request_serial{};
	std::uint32_t comms_request_serial{};
	std::uint32_t speech_request_serial{};
	std::uint32_t debug_expiry{};
	// HUD initialization publishes resource 0x90 as the no-subtitle
	// sentinel. DisplaySubTitle replaces it with a language resource ID.
	std::uint32_t subtitle_id{0x90};
	std::uint32_t music_mode{};
	std::uint8_t comms_category{};
};

inline bool presentation_slot_zero_busy(
	const PresentationRequest& presentation)
{
	// CBOX slot zero is shared by immediate communications speech and
	// PlaySpeech. Pending requests stand for the synchronous retail start
	// until the platform bridge consumes them; queued communications records
	// have not reached the slot and are deliberately absent here.
	return presentation.comms_pending
		|| presentation.comms_active
		|| presentation.speech_pending
		|| presentation.speech_active;
}

inline bool presentation_movie_busy(
	const PresentationRequest& presentation)
{
	// A pending platform request represents hudmovie_play_resource's
	// synchronous active state until its deferred asset load is consumed.
	return presentation.movie_pending || presentation.movie_active;
}

struct HudIconCommandState
{
	std::int32_t mode{};
	std::uint32_t serial{};
};

struct MissionReferenceValue
{
	ReferenceKind kind{ReferenceKind::object};
	std::uint16_t index{UINT16_MAX};
	std::int16_t selector{-1};
};

struct DirectorShot
{
	MissionReferenceValue position;
	MissionReferenceValue target;
	MissionReferenceValue moving_origin;
	MissionReferenceValue disable;
	std::uint16_t curve{UINT16_MAX};
	std::uint32_t duration{};
	std::uint32_t elapsed{};
	bool first_is_curve{};
	bool valid{};
};

struct DirectorState
{
	DirectorShot active;
	// Retail owns ten contiguous requests. The active record is queue slot
	// zero and these nine records are slots one through nine.
	DirectorShot stack[9];
	glm::vec3 camera_position{0.0f};
	glm::mat3 camera_orientation{1.0f};
	glm::vec3 moving_start{0.0f};
	float chain_length{};
	float next_marker_time{};
	std::uint32_t requested_ticks{};
	std::uint32_t segment_ticks{};
	std::uint8_t stack_count{};
	std::uint8_t mode{};
	std::uint16_t current_curve{UINT16_MAX};
	std::uint16_t static_object{UINT16_MAX};
	std::uint16_t marker_object{UINT16_MAX};
	bool waiting{};
	bool stop_requested{};
	bool disable_applied{};
	bool shot_started{};
	bool queue_overflow_logged{};
	std::uint32_t transition_order{};
	std::uint32_t serial{};
};

struct InstrumentState
{
	std::uint32_t serial{};
	bool open{};
};

struct ScriptSyncState
{
	std::uint32_t state{};
	std::uint32_t ready_mask{};
	std::uint32_t deadline{};
};

// LANCER.EXE 0x005dc600..0x005dcc00 contains 128 twelve-byte records;
// gameplay opcodes 0x47 and 0x48 serialize the index in seven bits.
constexpr std::size_t kScriptSyncCount = 128;

struct LaunchParticle
{
	// Generic particle slots store the current simulation tick at +0 and a
	// lifetime in the same 100 Hz units (Particle_spawn_into_slot,
	// 0x0049c1c0).
	glm::vec3 position{0.0f};
	glm::vec3 velocity{0.0f};
	std::uint32_t birth_tick{};
	std::uint16_t lifetime_ticks{};
	bool active{};
};

struct LaunchTrailRing
{
	glm::vec3 points[4]{};
	std::uint32_t birth_tick{};
	std::uint32_t sequence{};
	std::uint16_t owner{UINT16_MAX};
	std::uint16_t generation{};
	bool active{};
};

struct Runtime
{
	// Snapshot of elapsed simulation ticks for the current admitted frame.
	// Delta-aware frame callbacks consume this independently of the absolute
	// current simulation tick passed through their public APIs.
	std::uint32_t frame_delta_ticks{};
	ObjectRecord objects[game::kMaxMissionObjects];
	GroupRecord groups[game::kMaxMissionGroups];
	std::uint16_t group_members[game::kMaxMissionObjects];
	ReferenceSpan reference_spans[game::kMaxMissionReferenceSpans];
	TriggerRecord triggers[game::kMaxMissionTriggers];
	ReferenceSetRecord reference_sets[game::kMaxMissionReferenceSets];
	ReferenceLink reference_links[game::kMaxMissionReferenceLinks];
	Subtype997Pair subtype997_pairs[game::kMaxMissionObjects];
	QueuedEvent events[game::kMaxMissionEvents];
	std::uint32_t event_captures[
		game::kMaxMissionReferenceSpans][2][5]{};
	std::uint8_t sound_2d_events[16]{};
	std::uint32_t session_state[
		game::kMultiplayerSessionStateCount]{};
	std::uint32_t globals[game::kMaxMissionGlobals]{};
	// Retail stores raw pointers in VM cells. The reimplementation uses
	// stable mission indices, so assignments must retain the equivalent
	// object/group/reference-set identity beside externally visible dwords.
	std::uint8_t session_state_kinds[
		game::kMultiplayerSessionStateCount]{};
	// FUN_0049cd20/70 mission continuity. Retail retains all 65 availability
	// bytes in-process but opcode 9 and the PILO save chunk carry only entry
	// zero; ALPH carries all six assignments.
	std::uint8_t multiplayer_pilot_availability[
		kRetailPilotCatalogCount]{};
	std::int16_t multiplayer_pilot_assignments[
		game::kMultiplayerPilotAssignmentCount]{};
	std::uint16_t multiplayer_alpha_objects[
		game::kMultiplayerPilotAssignmentCount]{};
	std::uint32_t multiplayer_launch_generation{};
	std::uint16_t multiplayer_bootstrap_mission{};
	bool multiplayer_bootstrap_installed{};
	std::uint8_t global_kinds[game::kMaxMissionGlobals]{};
	std::uint32_t trigger_log_tick[35]{};
	std::int16_t objectives[10]{};
	HudIconCommandState hud_icons[20];
	InstrumentState instruments[20];
	ScriptSyncState script_sync[kScriptSyncCount];
	LaunchParticle launch_particles[game::kMaxLaunchParticles];
	LaunchTrailRing launch_trail_rings[200];
	glm::vec3 particle_camera_position{0.0f};
	glm::vec3 particle_camera_forward{0.0f, 0.0f, 1.0f};
	DirectorState director;
	EnvironmentState environment;
	NetworkRuntime network;
	DeathmatchScenarioState deathmatch;
	PlayerCommsState player_comms;
	std::uint16_t flyback_markers[10]{
		UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX,
		UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX,
	};
	float flyback_radii[10]{};
	PresentationRequest presentation;
	std::uint16_t scanner_target{UINT16_MAX};
	std::uint32_t scanner_serial{};
	std::uint8_t flyback_marker_count{};
	std::uint16_t object_count{};
	std::uint16_t mission_number{};
	std::uint16_t group_count{};
	std::uint16_t member_count{};
	std::uint16_t reference_span_count{};
	std::uint16_t trigger_count{};
	std::uint16_t reference_set_count{};
	std::uint16_t reference_link_count{};
	std::uint16_t subtype997_pair_count{};
	std::uint16_t subtype999_count{};
	std::uint16_t active_object_count{};
	std::uint16_t active_high_water{};
	std::uint16_t player_prefix_count{1};
	// Frontend gameplay-owner mode at LANCER.EXE 0x00524fe4. Mode one is
	// the simulator mission owner; mode two is Instant Action mission 29.
	std::uint8_t object_factory_mode{};
	// LANCER.EXE 0x00562f14: 0 Easy, 1 Medium, 2 Hard. Instant Action
	// installs Medium; campaign launch retains the selected save value.
	std::uint8_t difficulty{1};
	// DAT_00562f16, consumed by CommsVoice_format_player_line.
	std::uint8_t player_pilot_family{};
	std::uint16_t current_objective{UINT16_MAX};
	// HUD objective cycling retains the selected 0..9 slot separately
	// from the retail no-current-objective presentation flag.
	bool no_current_objective{};
	std::uint16_t primary_target{UINT16_MAX};
	std::int16_t primary_target_component{-1};
	// The action-centre owner retains a mission-object index rather than a
	// transient live pointer, so it survives object recreation.
	std::uint16_t action_center_object{};
	std::uint32_t waiting_control{UINT32_MAX};
	bool wait_for_key_accepted[kControlActionCount]{};
	std::uint16_t requested_camera_target{UINT16_MAX};
	// Mission executor initialization writes 0x0000ffff to the last-player-
	// jump tick before any player jump has been committed.
	std::uint32_t last_player_jump_tick{UINT16_MAX};
	std::uint32_t player_ordnance_rebuild_serial{};
	std::uint32_t camera_request_serial{};
	// Camera switches are immediate in retail. The reimplementation bridges
	// them to the session after the fixed mission step, so these monotonically
	// ordered stamps preserve multiple same-step request ordering.
	std::uint32_t camera_transition_order{};
	std::uint32_t camera_request_order{};
	std::uint32_t match_speed_request_serial{};
	std::uint32_t player_motion_clear_serial{};
	std::uint32_t terminate_request_count{};
	std::uint32_t launch_trail_cursor{};
	std::uint32_t launch_title_next_tick{};
	// Offline FUN_004b14f0 increments this 16-bit campaign counter for a
	// local-player score adjustment only in missions 0..27. Campaign
	// mission 25B keeps the value produced by mission 25A instead of
	// starting from zero.
	std::uint16_t mission_score_events{};
	std::uint16_t launch_title_reveal{};
	std::uint8_t requested_camera_mode{};
	std::uint8_t active_camera_mode{};
	std::uint8_t game_mode{};
	// Campaign/gameplay coordinator dword 0x00588394. Zero is ordinary
	// in-mission state; values 1..9 select the retail teardown route.
	std::uint8_t gameplay_state{};
	bool requested_camera_lock{};
	bool requested_camera_override_lock{};
	// Death cameras are requested while their target is live in retail, but
	// the session bridge may receive them after Explode has installed the
	// departed placeholder in that same fixed step.
	bool requested_camera_accept_departed{};
	bool requested_match_speed{};
	bool launch_title_active{};
	// Separate AI/presentation latch at 0x00587cd4. Ejection writes four
	// here before publishing its actual coordinator result above.
	std::uint8_t mission_result_code{};
	std::uint16_t taunts_disabled{};
	std::uint16_t generic_comms_disabled{};
	// Mission initialization at LANCER.EXE 0x493d49 publishes 100/0/0.
	std::uint32_t rescue_probability{100};
	std::uint32_t capture_probability{};
	std::uint32_t destroyed_probability{};
	std::uint16_t event_count{};
	std::uint16_t event_high_water{};
	std::uint8_t sound_2d_read{};
	std::uint8_t sound_2d_count{};
	bool event_overflow_logged{};
	bool terminate_requested{};
	bool landing_transition_complete{};
	bool player_death_transition_complete{};
	bool mission_25_alternate{};
	bool simulator_mode{};
	bool fosters_last_stand{};
	// Executor opcode 79 publishes the compiled event descriptor's
	// section-25 player-prefix policy for subsequent group/set expansion.
	bool reference_skip_player_prefix{};
	bool ready{};
};

bool runtime_initialize(Runtime& runtime, const DteFile& file);
bool runtime_install_multiplayer_bootstrap(
	Runtime& runtime,
	const game::MultiplayerMissionBootstrap& bootstrap);
bool runtime_capture_multiplayer_bootstrap(
	const Runtime& runtime,
	game::MultiplayerMissionBootstrap& bootstrap);
void runtime_mark_multiplayer_pilot_destroyed(
	Runtime& runtime,
	std::uint16_t pilot);
void runtime_publish_camera_request(
	Runtime& runtime,
	bool accept_departed_target = false);
void runtime_publish_match_speed_request(
	Runtime& runtime,
	bool enabled);
bool runtime_add_player_score(
	Runtime& runtime,
	game::World& world,
	std::uint16_t scorer_world_index,
	std::int32_t delta,
	bool publish = false);
bool runtime_add_player_death(
	Runtime& runtime,
	const game::World& world,
	std::uint16_t player_world_index,
	std::int32_t delta,
	bool publish = false);
void runtime_reset_live(Runtime& runtime);
bool runtime_activate_object(
	Runtime& runtime,
	std::uint16_t mission_index,
	game::World& world,
	const assets::ShipStatsTable& stats);
bool runtime_activate_curve_points(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file);
bool runtime_activate_group(
	Runtime& runtime,
	std::uint16_t group_index,
	game::World& world,
	const assets::ShipStatsTable& stats);
void runtime_deactivate_group(
	Runtime& runtime,
	std::uint16_t group_index,
	game::World& world);
void runtime_update_flyback(
	Runtime& runtime,
	game::World& world);
game::WorldObject* runtime_resolve_object(
	Runtime& runtime,
	std::uint16_t mission_index,
	game::World& world);
const game::WorldObject* runtime_resolve_object(
	const Runtime& runtime,
	std::uint16_t mission_index,
	const game::World& world);
std::uint16_t runtime_expand_reference(
	const Runtime& runtime,
	ReferenceKind kind,
	std::uint16_t index,
	std::uint16_t* output,
	std::uint16_t capacity);
std::uint16_t runtime_expand_target_reference(
	const Runtime& runtime,
	ReferenceKind kind,
	std::uint16_t index,
	ExpandedTargetReference* output,
	std::uint16_t capacity);
}
