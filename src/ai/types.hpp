#pragma once

#include "game/runtime_limits.hpp"

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cstdint>

namespace sl_open::ai
{
enum class TargetKind : std::uint8_t
{
	object,
	group,
	set,
	world_object,
	none,
};

struct Command
{
	std::uint32_t state[4]{};
	std::int16_t id{-1};
	std::int16_t selector{};
	std::uint16_t target{UINT16_MAX};
	std::int16_t target_component{-1};
	std::int16_t sequence{};
	TargetKind target_kind{TargetKind::none};
};

struct DeferredCommand
{
	// Stable, naturally aligned equivalent of retail's packed 0x24-byte
	// AIDeferredCommand. The live Command retains the exact four-field
	// identity and 16-byte payload; activation_tick is retail +0x20.
	Command command;
	std::uint32_t activation_tick{};
	std::uint8_t publication_seed{};
};

struct ControlDemand
{
	float throttle{};
	float roll{};
	float pitch{};
	float yaw{};
	bool afterburner{};
};

struct CurveWork
{
	glm::vec3 moving_anchor{0.0f};
	std::uint32_t segment_start_tick{};
	float chain_length{};
	float next_event_parameter{-1.0f};
	std::uint16_t first_curve{UINT16_MAX};
	std::uint16_t current_curve{UINT16_MAX};
	std::uint16_t segment_duration{};
	std::uint16_t moving_object{UINT16_MAX};
	bool moving_anchor_active{};
};

struct PatrolWork
{
	glm::vec3 route_offset{0.0f};
	glm::vec3 arrival_target{0.0f};
	glm::vec3 previous_target{0.0f};
	glm::vec3 active_target{0.0f};
	glm::vec3 member_target{0.0f};
	glm::vec3 coordinator_lookahead{0.0f};
	float initial_distance{};
	float speed_parameter{0.15f};
	float route_extent{};
	std::uint16_t route_pair{UINT16_MAX};
	std::uint16_t route_object{UINT16_MAX};
	std::uint16_t peer_object{UINT16_MAX};
	std::int16_t formation_id{-1};
	std::uint8_t mode{};
	bool group_compact{};
	bool ready{};
	bool transition_pending{};
	bool leg_complete{};
};

struct FormationRegroupWork
{
	glm::vec3 rear_target{0.0f};
	glm::vec3 forward_target{0.0f};
	glm::vec3 curve_start{0.0f};
	glm::vec3 curve_tangent{0.0f};
	std::uint32_t curve_start_tick{};
	std::uint16_t coordinator{UINT16_MAX};
	std::uint8_t stage{};
	std::uint8_t barrier_updates{};
	bool ready{};
	bool direct_approach{};
};

struct DockWork
{
	glm::vec3 interpolation_start{0.0f};
	glm::vec3 locator_local_position{0.0f};
	std::uint16_t target_object{UINT16_MAX};
	std::uint16_t target_locator{UINT16_MAX};
	std::uint16_t actor_model_reference{UINT16_MAX};
	std::uint16_t actor_locator_ordinal{UINT16_MAX};
	std::uint16_t target_model_reference{UINT16_MAX};
	std::uint16_t target_model_locator_ordinal{UINT16_MAX};
	std::uint16_t first_door_model{UINT16_MAX};
	std::uint16_t second_door_model{UINT16_MAX};
	std::uint16_t paired_object{UINT16_MAX};
	std::uint16_t paired_generation{};
	std::uint32_t deadline{};
	std::uint32_t interpolation_deadline_tick{};
	std::uint8_t mode{};
	std::uint8_t stage{};
	bool mirrored{};
};

struct FriendlyFireWork
{
	std::uint32_t deadline{};
	std::uint8_t stage{};
};

struct LandWork
{
	glm::vec3 bay_axis{0.0f};
	glm::vec3 deck_position{0.0f};
	glm::vec3 attachment_local_position{0.0f};
	glm::mat3 deck_orientation{1.0f};
	glm::mat3 attachment_local_orientation{1.0f};
	// Command 8 retains a live GameObject-table index. Type 0x0c keeps the
	// command target; type 0x0d replaces it with reserved landing scene 399.
	std::uint16_t target_world_index{UINT16_MAX};
	std::uint16_t transition_model{UINT16_MAX};
	std::uint32_t deadline{};
	std::uint32_t attachment_deadline{};
	std::uint8_t mode{};
	std::uint8_t stage{};
	bool prepared{};
};

struct LaunchWork
{
	// AI_Launch's retained 0x90-byte work record
	// (LANCER.EXE 0x00418eb0..0x0041bb8f).
	glm::vec3 relative_position{0.0f};
	glm::mat3 relative_orientation{1.0f};
	glm::vec3 cinematic_anchor_position{0.0f};
	glm::mat3 cinematic_anchor_orientation{1.0f};
	float flight_scalar{};
	std::uint32_t emitter_end_tick[6]{};
	std::uint32_t emitter_next_tick[6]{};
	// Work +4 is an absolute current-simulation-tick deadline. Launch state
	// constants and the neighboring particle-emitter records use 100 Hz
	// ticks.
	std::uint32_t deadline_tick{};
	std::uint16_t carrier_object{UINT16_MAX};
	std::uint16_t launch_model{UINT16_MAX};
	std::uint16_t animation_a{UINT16_MAX};
	std::uint16_t animation_b{UINT16_MAX};
	std::uint16_t cinematic_object{UINT16_MAX};
	std::uint16_t cinematic_generation{};
	std::int16_t launch_point{-1};
	std::int16_t requested_ordinal{};
	std::uint8_t strategy{};
	std::uint8_t stage{};
	std::uint8_t cinematic_branch{};
	bool emitter_burst_active[6]{};
	bool launch_active{};
	bool alternate_flight{};
	bool cinematic_active{};
	bool cinematic_placement_pending{};
	bool cinematic_attachment{};
};

struct JumpWork
{
	glm::vec3 destination{0.0f};
	glm::vec3 saved_position{0.0f};
	glm::vec3 hidden_position{0.0f};
	glm::vec3 corridor_endpoint{0.0f};
	glm::mat3 destination_orientation{1.0f};
	glm::mat3 saved_orientation{1.0f};
	std::uint32_t previous_tick{};
	std::uint32_t transition_start_tick{};
	std::uint32_t deadline{};
	float phase{};
	std::int16_t visual_context{-1};
	std::uint8_t light_count{};
	std::uint8_t state{};
	std::uint8_t saved_flight_callback{};
	bool synchronized{};
	bool local_transition{};
};

struct WarpWork
{
	glm::vec3 destination{0.0f};
	glm::vec3 saved_position{0.0f};
	glm::mat3 destination_orientation{1.0f};
	glm::mat3 saved_orientation{1.0f};
	std::uint32_t previous_tick{};
	float phase{};
	std::int16_t context{-1};
	std::uint8_t state{};
	bool local_transition{};
	bool actor_revealed{};
};

struct FixedGateWork
{
	glm::vec3 start_position{0.0f};
	glm::vec3 end_position{0.0f};
	glm::vec3 gate_position{0.0f};
	glm::mat3 gate_orientation{1.0f};
	std::uint32_t previous_tick{};
	float phase{};
	std::int16_t context{-1};
	std::uint16_t gate_object{UINT16_MAX};
	std::uint16_t explosion_count{};
	std::uint8_t state{};
	bool owns_context_lock{};
	bool local_transition{};
};

struct WarpProjectorWork
{
	std::uint32_t start_tick{};
	std::uint32_t previous_tick{};
	bool initialized{};
};

struct EjectionWork
{
	std::uint16_t separated_object{UINT16_MAX};
	std::uint16_t separated_generation{};
	std::uint8_t saved_protection_state{};
	std::uint8_t outcome{};
	bool split_complete{};
	bool destruction_notified{};
};

struct ScoopUpWork
{
	glm::vec3 target_start_position{0.0f};
	glm::vec3 approach_position{0.0f};
	std::uint32_t state_start_tick{};
	std::uint32_t previous_tick{};
	std::uint16_t target_object{UINT16_MAX};
	std::uint16_t target_generation{};
	std::int16_t effect_slot{-1};
	float beam_stagger{};
	std::uint8_t state{};
	bool reserved{};
	bool visual_active{};
};

struct ExplodeWork
{
	glm::vec3 emission_direction{0.0f, 0.0f, 1.0f};
	std::uint32_t next_emission_tick{};
	std::uint8_t mode{};
	std::uint8_t variant{};
	bool destruction_notified{};
};

struct RipperGrabWork
{
	glm::vec3 approach_position{0.0f};
	glm::vec3 beam_start{0.0f};
	glm::vec3 beam_end{0.0f};
	glm::vec3 target_rotation_start{0.0f};
	glm::vec3 target_rotation_end{0.0f};
	std::uint32_t stage_start_tick{};
	std::uint16_t target_object{UINT16_MAX};
	std::uint16_t target_generation{};
	std::int16_t effect_slot{-1};
	std::uint8_t saved_target_capture_state{};
	std::uint8_t saved_flight_callback{};
	std::uint8_t stage{};
	bool close_approach{};
};

struct RipperAttachWork
{
	glm::vec3 aim_position{0.0f};
	glm::vec3 approach_position{0.0f};
	glm::vec3 component_position{0.0f};
	glm::vec3 component_rotation{0.0f};
	glm::vec3 cargo_start_position{0.0f};
	glm::vec3 cargo_start_rotation{0.0f};
	std::uint32_t stage_start_tick{};
	std::uint16_t carried_object{UINT16_MAX};
	std::uint16_t carried_generation{};
	std::int16_t effect_slot{-1};
	std::uint8_t stage{};
};

struct RespawnWork
{
	std::uint32_t deadline{};
	std::int16_t effect_slot{-1};
};

struct IonCannonWork
{
	// AI_DarkReignShoot's shared work record and retained visual state,
	// LANCER.EXE 0x0040d020..0x0040e96f. Retail stores the first values in
	// the current AI work block and the visual handles in its one-entry
	// IonCannonEffects pool.
	std::uint32_t stage_start_tick{};
	std::uint32_t previous_tick{};
	std::uint16_t target_object{UINT16_MAX};
	std::uint16_t lower_model{UINT16_MAX};
	std::uint16_t upper_model{UINT16_MAX};
	std::uint16_t emitter_model{UINT16_MAX};
	std::uint16_t beam_model{UINT16_MAX};
	std::uint16_t plasma_model{UINT16_MAX};
	std::uint16_t impact_origin_model{UINT16_MAX};
	std::uint8_t stage{};
	std::int8_t last_plasma{-1};
	std::uint8_t plasma_count{};
	float alignment{};
	float target_field_fraction{};
	float focus_radius{};
	float beam_radius{};
	float beam_half_length{};
	bool initialized{};
	bool focus_active{};
	bool beam_active{};
	bool impact_active{};
};

struct Work
{
	glm::vec3 vector{0.0f};
	glm::vec3 vector2{0.0f};
	glm::mat3 orientation{1.0f};
	std::uint32_t entered_tick{};
	std::uint32_t previous_tick{};
	std::uint32_t deadline{};
	float phase{};
	std::uint16_t target{UINT16_MAX};
	std::uint8_t stage{};
	std::uint8_t strategy{};
	bool begin_pending{true};

	// Command 105 embeds the 0x90-byte AIDefend work record recovered at
	// LANCER.EXE 0x00404f80..0x00406a8f. These fields retain the retail
	// offsets' semantics without imposing the original pointer layout.
	glm::vec3 fight_aim_point{0.0f};       // retail +0x00
	glm::vec3 fight_target_motion{0.0f};   // retail +0x0c
	glm::vec3 maneuver_vector{0.0f};       // retail +0x40
	ControlDemand fight_demand{};
	std::uint32_t maneuver_expiration{};
	std::uint32_t target_refresh_deadline{};
	std::uint32_t weapon_deadline{};
	std::uint32_t cloak_deadline{};
	std::uint32_t instruction_deadline{};
	std::uint32_t mirror_mask{};
	std::uint16_t maneuver_ship{UINT16_MAX};
	std::uint16_t maneuver_index{};
	std::uint32_t pending_maneuver_duration{};
	std::uint32_t primary_weapon_delay{};
	std::uint16_t pending_maneuver_ship{UINT16_MAX};
	std::uint16_t secondary_weapon_target{UINT16_MAX};
	std::uint8_t pending_maneuver_index{};
	std::uint8_t secondary_weapon_mount{UINT8_MAX};
	std::int16_t secondary_weapon_component{-1};
	std::uint8_t script_line{UINT8_MAX};
	bool instruction_active{};
	bool weapon_solution{};
	bool requested_cloak{};
	bool attack_permission{};
	bool afterburner_latch{};
	bool runaway_aligned{};
	bool maneuver_initialized{};
	bool maneuver_refresh_pending{};
	bool maneuver_preselection_latched{};
	bool crash_avoidance_active{};

	CurveWork curve;
	PatrolWork patrol;
	FormationRegroupWork formation_regroup;
	DockWork dock;
	FriendlyFireWork friendly_fire;
	LandWork land;
	LaunchWork launch;
	JumpWork jump;
	WarpWork warp;
	FixedGateWork fixed_gate;
	WarpProjectorWork warp_projector;
	EjectionWork ejection;
	ScoopUpWork scoop_up;
	ExplodeWork explode;
	RipperGrabWork ripper_grab;
	RipperAttachWork ripper_attach;
	RespawnWork respawn;
	IonCannonWork ion_cannon;
};

struct ObjectState
{
	Command commands[game::kMaxAiCommandsPerObject];
	DeferredCommand deferred_commands[game::kMaxAiCommandsPerObject];
	Work work;
	float friendly_fire_damage{};
	std::uint32_t friendly_fire_warning_deadline{};
	std::uint8_t command_count{};
	std::uint8_t deferred_command_count{};
	std::uint8_t friendly_fire_status{};
	std::uint8_t friendly_fire_warning_level{};
};
}
