#pragma once

#include "game/runtime_limits.hpp"

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace sl_open::assets
{
struct ShipStatsTable;
}

namespace sl_open::game
{
struct ObjectHandle;
struct World;

constexpr std::size_t kParticleArrayCount = 10;
constexpr std::size_t kParticleStyleCount = 19;
constexpr std::size_t kParticleEmitterCount = 64;

enum class ParticleEmitterStyle : std::uint8_t
{
	// Explosion_system_init style globals in their compiled table order.
	ordinary_explosion = 0,
	expanding_explosion = 1,
	white_debris = 2,
	gray_explosion = 3,
	blue_explosion = 4,
	// Explosion-system style global 0x005538c0.
	large_destruction_smoke = 5,
	red_sparks = 6,
	red_sparks_dense = 7,
	red_sparks_fast = 8,
	red_sparks_large = 9,
	// PlayerEject_particle_style_initialize, LANCER.EXE 0x00415610.
	ejection_cockpit = 10,
	// WGate_initialize, LANCER.EXE 0x0041e280. Both styles use the shared
	// default particle array; the large style belongs to the six Boridin
	// warp-projector emitters.
	wgate = 11,
	wgate_large = 12,
	// Guns_initialize, LANCER.EXE 0x00478990. The ordinary Nova
	// projectile uses this style for its terminal thirty-particle burst.
	nova_expiry = 13,
	// mission_create_damage_particle_template, LANCER.EXE 0x004946b0.
	damage_light = 14,
	damage_medium = 15,
	damage_heavy = 16,
	// ShieldFX_init, LANCER.EXE 0x0049fd20. These styles use the global
	// partic4 array; ShieldFX does not own a separate particle array.
	shield_impact = 17,
	shield_gray = 18,
};

enum class ParticleTexture : std::uint8_t
{
	partic4,
	partic7,
};

enum class ParticleEmissionMode : std::uint8_t
{
	ordinary = 0,
	rare_debris = 1,
	debris_only = 2,
};

struct ParticleCurve
{
	float quadratic{};
	float linear{};
	float constant{};
};

struct ParticleStyle
{
	ParticleEmissionMode emission_mode{ParticleEmissionMode::ordinary};
	std::int32_t lifetime_base_ticks{};
	std::int32_t lifetime_random_ticks{};
	ParticleCurve emission;
	ParticleCurve size;
	ParticleCurve red;
	ParticleCurve green;
	ParticleCurve blue;
	std::uint8_t array_index{};
	float burst_distance_scale{1.0f};
	bool active{};
};

struct ParticleSlot
{
	std::int32_t birth_tick{};
	std::int32_t lifetime_ticks{};
	glm::vec3 velocity{0.0f};
	std::uint32_t style_index{};
};

struct ParticleVisual
{
	glm::vec3 position{0.0f};
	float size{};
	glm::vec4 rectangle{0.0f, 1.0f, 0.0f, 1.0f};
	glm::vec3 color{0.0f};
	float alpha{1.0f};
	bool active{};
};

struct ParticleArray
{
	std::vector<ParticleSlot> slots;
	std::vector<ParticleVisual> visuals;
	std::int32_t capacity{};
	std::int32_t high_water{};
	ParticleTexture texture{ParticleTexture::partic4};
	bool additive{true};
	bool active{};
};

struct ParticleEmitter
{
	std::int32_t duration_ticks{};
	std::int32_t start_tick{};
	glm::vec3 local_position{0.0f};
	glm::mat3 local_basis{1.0f};
	glm::vec3 world_position{0.0f};
	glm::mat3 world_basis{1.0f};
	glm::vec3 direction_center{0.0f};
	glm::vec3 direction_spread{0.0f};
	float speed_base{};
	float speed_random{};
	glm::vec3 inherited_velocity{0.0f};
	glm::vec4 rectangle{0.0f, 1.0f, 0.0f, 1.0f};
	std::uint16_t owner_index{UINT16_MAX};
	std::uint16_t owner_generation{};
	std::uint16_t model_reference{UINT16_MAX};
	std::uint32_t style_index{};
	bool model_owned{};
	bool active{};
};

struct ParticleFragment
{
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	glm::vec3 velocity{0.0f};
	glm::vec3 angular_step{0.0f};
	float scale{};
	std::int32_t start_tick{};
	std::int32_t duration_ticks{};
	std::uint16_t model_resource{};
	bool active{};
	bool render_active{};
};

struct ParticleRuntime
{
	std::array<ParticleArray, kParticleArrayCount> arrays;
	std::vector<ParticleStyle> styles;
	std::array<ParticleEmitter, kParticleEmitterCount> emitters;
	std::array<ParticleFragment, kMaxMeshDebris> fragments;
	std::uint16_t fragment_capacity{500};
	std::uint16_t fragment_cursor{};
	std::uint16_t fragment_live_count{};
	std::uint8_t default_array{};
	std::uint8_t damage_arrays[3]{};
	bool initialized{};
	bool array_saturation_reported{};
	bool emitter_saturation_reported{};
	bool initialization_failure_reported{};
};

ParticleCurve particle_curve_fit_start_mid_end(
	float start,
	float midpoint,
	float end);
float particle_curve_evaluate(const ParticleCurve& curve, float time);

// Particle_system_init/reset/shutdown, LANCER.EXE
// 0x0049bf60/0x0049bfb0/0x0049c010. The retained runtime owns every array,
// including arrays which retail expected adjacent subsystems to destroy.
bool particle_system_initialize(
	ParticleRuntime& runtime,
	std::uint8_t graphics_quality);
void particle_system_reset(ParticleRuntime& runtime);
void particle_system_shutdown(ParticleRuntime& runtime);
bool particle_system_ready(const ParticleRuntime& runtime);

// Explosion_system_init's graphics-quality selection. Values 0, 1, and 2
// select exact capacities 100, 300, and 500; other values retain the current
// capacity exactly as retail does.
void particle_system_configure_fragments(
	ParticleRuntime& runtime,
	std::uint8_t graphics_quality);

// Particle_array_create/destroy. Particle_array_create (0x0049c050) stores
// the image and BMO mode bytes on the array; styles only retain its address.
// The returned index preserves retail's ten-descriptor ownership boundary.
std::int16_t particle_array_create(
	ParticleRuntime& runtime,
	std::int32_t capacity,
	ParticleTexture texture,
	bool additive);
void particle_array_destroy(
	ParticleRuntime& runtime,
	std::uint8_t array_index);

// Particle_style_allocate/free. Style indices remain stable while active and
// are retained by live slots after their creating emitter is released.
std::int32_t particle_style_create(
	ParticleRuntime& runtime,
	std::uint8_t array_index);
void particle_style_destroy(
	ParticleRuntime& runtime,
	std::uint32_t style_index);

// Initializes an independent emitter with the exact allocator defaults.
// Automatic 64-owner insertion is intentionally separate; temporary retail
// callers initialize, burst, and discard this value synchronously.
bool particle_emitter_initialize(
	const ParticleRuntime& runtime,
	ParticleEmitter& emitter,
	std::uint32_t style_index,
	std::uint32_t duration_ticks,
	std::uint32_t simulation_tick);

// Reserves one entry in the exact 64-pointer destruction-emitter owner.
// Callers fill the public transform/emission fields on the returned value.
ParticleEmitter* particle_emitter_reserve_automatic(
	World& world,
	std::uint32_t style_index,
	std::uint32_t duration_ticks,
	std::uint32_t simulation_tick);
void particle_emitter_release_automatic(ParticleEmitter& emitter);

// Service an emitter whose storage belongs to another retained subsystem
// (WGate contexts own four each and the Boridin projector owns six). Retail
// routes these through Particle_emitter_update without inserting them in the
// separate 64-entry destruction-emitter owner.
bool particle_emitter_service_owned(
	World& world,
	ParticleEmitter& emitter,
	std::uint32_t simulation_tick,
	std::uint32_t elapsed_simulation_ticks,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward);

// Complete generic emitter service. Particle_emitter_update (0x0049c680)
// and Particle_spawn_into_slot (0x0049c1c0) store and compare the retail
// current 100 Hz simulation tick directly.
void particle_system_service(
	World& world,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick,
	std::uint32_t elapsed_simulation_ticks,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward);

// Explicit-burst equivalent of Particle_emitter_burst (0x0049c450).
bool particle_emitter_burst(
	World& world,
	ParticleEmitter& emitter,
	std::int32_t requested_count,
	std::uint32_t simulation_tick,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward);

// Particle_emitter_allocate + object_spawn_point_group_smoke_emitters,
// LANCER.EXE 0x0049c600/0x004715d0. Returns false when the exact shared
// 64-record destruction-emitter owner is full.
bool particle_emitter_create_model_owned(
	World& world,
	ObjectHandle owner,
	std::uint16_t model_reference,
	const glm::vec3& local_position,
	const glm::vec3& local_direction,
	const glm::vec3& direction_spread,
	float speed_base,
	float speed_random,
	std::uint32_t duration_ticks,
	ParticleEmitterStyle style,
	std::uint32_t simulation_tick);

// Temporary world-space emitter path used by retail's synchronous ejection
// and ordinary-destruction bursts. The particles remain owned by the shared
// array after the stack emitter is discarded.
bool particle_emitter_burst_world(
	World& world,
	const glm::vec3& position,
	const glm::mat3& basis,
	const glm::vec3& direction,
	const glm::vec3& direction_spread,
	float speed_base,
	float speed_random,
	std::int32_t requested_count,
	ParticleEmitterStyle style,
	std::uint32_t simulation_tick,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward,
	const glm::vec3& inherited_velocity = glm::vec3{0.0f});

// World-space burst with retail's exact simulation-tick start offset.
// Explosion constructors use this for authored staggered bursts.
bool particle_emitter_burst_world_delayed(
	World& world,
	const glm::vec3& position,
	const glm::mat3& basis,
	const glm::vec3& direction,
	const glm::vec3& direction_spread,
	float speed_base,
	float speed_random,
	std::int32_t requested_count,
	ParticleEmitterStyle style,
	std::uint32_t simulation_tick,
	std::uint32_t delay_ticks,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward);

// Explosion_fragment_spawn's random-direction form used by the terminal
// ship-destruction paths.
void particle_fragment_burst(
	World& world,
	const glm::vec3& position,
	float heavy_chance,
	float mesh_scale,
	float velocity_multiplier,
	std::uint16_t count,
	std::uint32_t simulation_tick);

// Explosion_fragment_spawn with a caller-supplied direction.
void particle_fragment_directional_burst(
	World& world,
	const glm::vec3& position,
	const glm::vec3& direction,
	float heavy_chance,
	float mesh_scale,
	float velocity_multiplier,
	std::uint16_t count,
	std::uint32_t simulation_tick);

// LANCER.EXE 0x004717d0 with Huge Explosion's compiled (0, .1, 1, 0)
// arguments and its camera-relative direction.
void particle_uber_debris_spawn(
	World& world,
	const glm::vec3& position,
	const glm::vec3& direction,
	std::uint32_t simulation_tick);
}
