#pragma once

#include "game/particle_emitters.hpp"
#include "game/runtime_limits.hpp"

#include <glm/mat3x3.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace sl_open::game
{
struct ObjectHandle;
struct World;
struct WorldObject;

constexpr std::size_t kJumpVisualContextCount = 64;
constexpr std::size_t kJumpTrailCount = 5;
constexpr std::size_t kJumpLightCount = 20;
constexpr std::size_t kWGateContextCount = 32;
constexpr std::size_t kWGateMaximumRadialSegments = 16;
constexpr std::size_t kWGateMaximumAxialSegments = 12;
constexpr std::size_t kWGateMaximumRows =
	kWGateMaximumAxialSegments + 1;

enum class TransitionTexture : std::uint8_t
{
	solid,
	jump_trail,
	jump_flare,
	jump_burst,
	jump_light,
	jump_light_bright,
	warp_primary,
	warp_secondary,
	particle_flare,
};

enum class TransitionBlend : std::uint8_t
{
	additive,
	source_alpha_additive,
};

struct TransitionVertex
{
	glm::vec3 position{0.0f};
	std::uint32_t color{0xffffffffu};
	glm::vec2 uv{0.0f};
};

struct TransitionMesh
{
	std::vector<TransitionVertex> vertices;
	// Retail deformations rewrite runtime positions from retained mesh data;
	// keeping the undeformed positions prevents phase updates accumulating.
	std::vector<glm::vec3> base_positions;
	std::vector<std::uint16_t> indices;
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	// SR object +0x48. Fixed-gate open/close animate the retained portal
	// object's uniform scale rather than changing vertex alpha.
	float scale{1.0f};
	TransitionTexture texture{TransitionTexture::warp_primary};
	TransitionBlend blend{TransitionBlend::additive};
	bool active{};
	bool double_sided{};
};

struct TransitionBillboard
{
	glm::vec3 position{0.0f};
	glm::vec2 half_extent{1.0f};
	std::uint32_t color{0xffffffffu};
	TransitionTexture texture{TransitionTexture::jump_light};
	bool active{};
};

struct JumpVisualContext
{
	std::array<TransitionMesh, kJumpTrailCount> trails;
	std::array<TransitionBillboard, kJumpLightCount> lights;
	TransitionMesh jump_out_burst;
	TransitionMesh jump_in_burst;
	TransitionMesh flare;
	glm::mat3 jump_in_basis{1.0f};
	std::uint16_t owner_index{UINT16_MAX};
	std::uint16_t owner_generation{};
	std::uint8_t trail_count{};
	std::uint8_t light_count{};
	bool active{};
};

struct JumpEffectsRuntime
{
	std::array<JumpVisualContext, kJumpVisualContextCount> contexts;
	float overlay_scalar{};
	std::uint32_t overlay_next_tick{};
	std::uint16_t transition_owner_index{UINT16_MAX};
	std::uint16_t transition_owner_generation{};
	std::uint8_t overlay_countdown{};
	bool overlay_active{};
	bool environment_transition_active{};
	bool initialized{};
	bool pool_saturation_reported{};
};

enum class WGateMode : std::uint8_t
{
	ship = 0,
	prototype = 1,
	advanced = 2,
	boridin = 3,
};

enum class WGateShipStage : std::uint8_t
{
	warp_out_open,
	warp_out_reverse,
	warp_out_depart,
	warp_in_open,
	warp_in_delay,
};

struct WGateContext
{
	TransitionMesh portal;
	std::array<TransitionMesh, 4> beams;
	std::array<TransitionBillboard, 4> emitters;
	std::array<ParticleEmitter, 4> particle_emitters;
	std::array<TransitionMesh, 2> fixed_quads;
	std::array<float, kWGateMaximumRows> row_radii{};
	std::array<float, kWGateMaximumRows> row_positions{};
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	std::uint32_t created_tick{};
	std::uint32_t previous_tick{};
	std::uint32_t uv_tick{};
	float radius{};
	float mesh_radius{};
	float animation_radius{};
	float axial_offset{};
	float phase{};
	std::uint16_t owner_index{UINT16_MAX};
	std::uint16_t owner_generation{};
	std::uint8_t radial_segments{};
	std::uint8_t axial_segments{};
	WGateMode mode{WGateMode::ship};
	bool has_special_parameters{};
	bool fixed_gate_lock{};
	bool active{};
};

struct WGateProjector
{
	TransitionMesh beam;
	ParticleEmitter particle_emitter;
	glm::vec3 position{0.0f};
	bool active{};
};

struct WGateEffectsRuntime
{
	std::array<WGateContext, kWGateContextCount> contexts;
	std::array<WGateProjector, 6> projectors;
	std::array<std::uint16_t, kMaxGameObjects>
		fixed_deformation_objects{};
	TransitionMesh projector_portal;
	TransitionMesh wormhole;
	glm::vec3 camera_target_position{0.0f};
	glm::mat3 camera_target_orientation{1.0f};
	std::uint32_t live_mask{};
	std::uint32_t transition_start_tick{};
	std::uint32_t projector_submission_tick{UINT32_MAX};
	std::uint32_t camera_expiry_tick{};
	// LANCER.EXE 0x004e3f68. FixedGateJumpIn assigns successive NPCs the
	// exact -2..2 entry lanes and resets the sequence around the player.
	std::int32_t fixed_jump_angle_index{-2};
	std::int16_t camera_context_index{-1};
	std::uint16_t camera_target_index{UINT16_MAX};
	std::uint16_t projector_owner_index{UINT16_MAX};
	std::uint16_t projector_owner_generation{};
	std::uint16_t wormhole_owner_index{UINT16_MAX};
	std::uint16_t wormhole_owner_generation{};
	std::uint16_t warp_transition_owner_index{UINT16_MAX};
	std::uint16_t warp_transition_owner_generation{};
	std::uint16_t fixed_transition_owner_index{UINT16_MAX};
	std::uint16_t fixed_transition_owner_generation{};
	std::uint16_t fixed_departure_owner_index{UINT16_MAX};
	std::uint16_t fixed_departure_owner_generation{};
	std::uint8_t fixed_deformation_count{};
	std::uint8_t radial_segments{16};
	std::uint8_t axial_segments{12};
	bool warp_transition_active{};
	bool fixed_gate_transition_active{};
	bool fixed_departure_lock{};
	bool krasny_split_ready{};
	// Fixed-gate construction arms the shared Krasny gate effect; an unarmed
	// post-threshold JumpIn explicitly clears it.
	bool krasny_gate_effect_active{true};
	bool wormhole_active{};
	bool fixed_tunnel_camera_active{};
	bool initialized{};
	bool context_saturation_reported{};
};

struct TransitionEffectsRuntime
{
	JumpEffectsRuntime jump;
	WGateEffectsRuntime wgate;
};

bool transition_effects_initialize(
	TransitionEffectsRuntime& runtime,
	std::uint8_t graphics_quality);
void transition_effects_reset(TransitionEffectsRuntime& runtime);
void transition_effects_shutdown(TransitionEffectsRuntime& runtime);
void transition_effects_release_owner(
	TransitionEffectsRuntime& runtime,
	ObjectHandle owner);
bool transition_explosion_create(
	World& world,
	const glm::vec3& position,
	float size,
	std::uint32_t duration_ticks,
	std::uint32_t tick);
bool transition_explosion_create_delayed(
	World& world,
	const glm::vec3& position,
	float size,
	std::uint32_t duration_ticks,
	std::uint32_t tick,
	std::uint32_t delay_ticks);

std::int16_t jump_visual_allocate(
	JumpEffectsRuntime& runtime,
	ObjectHandle owner);
JumpVisualContext* jump_visual_get(
	JumpEffectsRuntime& runtime,
	std::int16_t index);
void jump_visual_free(
	JumpEffectsRuntime& runtime,
	std::int16_t index);
bool jump_visual_build_in(
	World& world,
	std::int16_t context,
	WorldObject& actor,
	bool large);
bool jump_visual_build_out(
	World& world,
	std::int16_t context,
	WorldObject& actor,
	bool large,
	const glm::vec3& corridor_endpoint);
void jump_visual_set_trail_phase(
	JumpVisualContext& context,
	float phase);
void jump_visual_set_burst_phase(
	TransitionMesh& burst,
	float ring_phase,
	float core_phase);
void jump_visual_set_out_burst_phase(
	TransitionMesh& burst,
	float phase);

std::int16_t wgate_context_allocate(
	World& world,
	WGateEffectsRuntime& runtime,
	WGateMode mode,
	ObjectHandle owner,
	const glm::vec3& position,
	const glm::mat3& orientation,
	std::uint32_t tick);
std::int16_t wgate_context_find_owner(
	const WGateEffectsRuntime& runtime,
	std::uint16_t owner_index);
WGateContext* wgate_context_get(
	WGateEffectsRuntime& runtime,
	std::int16_t index);
void wgate_context_free(
	WGateEffectsRuntime& runtime,
	std::int16_t index);
void wgate_context_set_alpha(WGateContext& context, float alpha);
void wgate_context_set_ship_phase(
	WGateContext& context,
	WGateShipStage stage,
	float phase);
void wgate_context_scroll_uv(
	WGateContext& context,
	float u_delta,
	float v_delta);
void wgate_context_register_deformation(
	WGateEffectsRuntime& runtime,
	std::uint16_t object_index);
void wgate_context_unregister_deformation(
	WGateEffectsRuntime& runtime,
	std::uint16_t object_index);
void wgate_context_set_collapse_wave(
	WGateContext& context,
	float phase);
void wgate_context_set_collapse_front(
	WGateContext& context,
	float phase);
void transition_effects_service_fixed_gates(
	World& world,
	std::uint32_t tick,
	std::uint32_t elapsed_ticks);
bool wgate_wormhole_create(
	WGateEffectsRuntime& runtime,
	const glm::vec3& position,
	const glm::mat3& orientation);
void wgate_wormhole_animate(
	WGateEffectsRuntime& runtime,
	std::uint32_t tick,
	std::uint32_t elapsed_ticks);
bool wgate_projectors_create(
	World& world,
	WGateEffectsRuntime& runtime,
	ObjectHandle owner,
	std::uint32_t tick);
void wgate_projectors_update(
	World& world,
	WGateEffectsRuntime& runtime,
	const std::array<glm::vec3, 6>& anchor_positions,
	const glm::vec3& projector_position,
	const glm::mat3& projector_orientation,
	std::uint32_t tick,
	std::uint32_t elapsed_ticks,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward);
void transition_effects_service_particles(
	World& world,
	std::uint32_t tick,
	std::uint32_t elapsed_ticks,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward);
}
