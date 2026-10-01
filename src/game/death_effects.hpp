#pragma once

#include "assets/gameplay_model.hpp"
#include "game/transition_effects.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sl_open::assets
{
struct ShipStatsTable;
}

namespace sl_open::mission
{
struct Runtime;
}

namespace sl_open::game
{
struct ObjectHandle;
struct World;
struct WorldObject;
struct ObjectModelReference;
enum class ParticleEmitterStyle : std::uint8_t;

constexpr std::size_t kTractorEffectCount = 5;
constexpr std::size_t kRipperGrabEffectCount = 150;
constexpr std::size_t kRespawnEffectCount = 8;
constexpr std::size_t kUberExplosionCandidateCount = 80;
constexpr std::size_t kDeathHudNotificationCount = 4;

enum class ExplosionBillboardType : std::uint8_t
{
	separate_frames = 0,
	sheet = 1,
};

struct ExplosionBillboardEffect
{
	glm::vec3 position{0.0f};
	glm::vec3 velocity_per_tick{0.0f};
	std::uint32_t start_tick{};
	std::int32_t duration_ticks{};
	std::int32_t delay_ticks{};
	std::uint32_t frame_flags{};
	float size{};
	ExplosionBillboardType type{ExplosionBillboardType::separate_frames};
	bool create_light{};
	bool animate_scale{};
	bool alternate_atlas{};
	bool active{};
};

struct RockChunkEffect
{
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	glm::vec3 velocity_per_tick{0.0f};
	glm::vec3 angular_velocity_per_tick{0.0f};
	std::uint32_t expiration_tick{};
	float scale{1.0f};
	std::uint16_t model_resource{};
	bool active{};
	bool render_active{};
};

struct ExplodingMeshGeometry
{
	std::vector<assets::GameplayVertex> vertices;
	std::vector<std::uint16_t> indices;
	std::vector<assets::GameplayFace> faces;
	std::vector<std::uint32_t> face_corners;
	std::vector<assets::GameplaySection> sections;
	std::vector<glm::vec3> normals;
	std::vector<glm::vec3> secondary_normals;
	std::vector<glm::vec3> static_lighting_rgb;
	glm::vec3 center{0.0f};
	glm::vec3 bounds_min{0.0f};
	glm::vec3 bounds_max{0.0f};
	float radius{};
	std::uint16_t source_model{};
	std::uint32_t node_flags{};
	std::uint32_t light_exclusion_mask{};
	std::vector<std::uint8_t> light_channels;
	bool source_attachment{};
	bool static_lighting_enabled{};
};

struct ExplodingMeshEffect
{
	ExplodingMeshGeometry geometry;
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	glm::vec3 velocity_per_tick{0.0f};
	glm::mat3 angular_step{1.0f};
	std::uint32_t expiration_tick{};
	std::uint32_t geometry_serial{};
	std::uint16_t emitter_index{UINT16_MAX};
	std::uint8_t stage{};
	bool active{};
	bool render_active{};
};

struct DestructionLightEffect
{
	glm::vec3 local_position{0.0f};
	float remaining{1.0f};
	std::uint16_t owner_index{UINT16_MAX};
	std::uint16_t owner_generation{};
	std::uint16_t model_reference{UINT16_MAX};
	float intensity{};
	bool active{};
};

struct PowercoreEffect
{
	std::uint16_t owner_index{UINT16_MAX};
	std::uint16_t owner_generation{};
	std::uint16_t model_reference{UINT16_MAX};
	std::uint16_t emitter_index{UINT16_MAX};
	float size{2500.0f};
	bool active{};
};

enum class DeathHudNotificationKind : std::uint8_t
{
	ordinary_kill,
	bought_the_farm,
	uber_kill,
};

struct DeathHudNotification
{
	DeathHudNotificationKind kind{};
	std::uint8_t victim{};
	std::uint8_t attacker{};
	bool local_bonus{};
};

struct TractorEffect
{
	std::array<TransitionMesh, 2> beams;
	TransitionMesh shield;
	TransitionBillboard light;
	std::uint16_t owner_index{UINT16_MAX};
	std::uint16_t owner_generation{};
	std::uint16_t target_index{UINT16_MAX};
	std::uint16_t target_generation{};
	float beam_stagger{};
	bool active{};
	bool submitted{};
};

struct RespawnEffect
{
	TransitionMesh portal;
	TransitionMesh projection;
	// Respawn_Material flag 0x100 supplies one actor-space reveal plane to
	// every render node at SR render-object +0xac.
	float model_clip_z{};
	std::uint16_t owner_index{UINT16_MAX};
	std::uint16_t owner_generation{};
	bool active{};
	bool submitted{};
};

struct RipperGrabEffect
{
	std::array<TransitionMesh, 4> beams;
	std::uint16_t owner_index{UINT16_MAX};
	std::uint16_t owner_generation{};
	std::uint16_t target_index{UINT16_MAX};
	std::uint16_t target_generation{};
	bool active{};
	bool submitted{};
};

struct UberExplosionCandidate
{
	std::uint16_t object_index{UINT16_MAX};
	std::uint16_t generation{};
	bool reached{};
};

struct UberExplosionEffect
{
	std::array<TransitionMesh, 2> hemispheres;
	TransitionMesh inner_shell;
	std::array<UberExplosionCandidate, kUberExplosionCandidateCount>
		candidates;
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	std::uint32_t start_tick{};
	std::uint16_t owner_index{UINT16_MAX};
	std::uint16_t owner_generation{};
	float size{};
	float inner_radius{};
	std::uint16_t duration_ticks{};
	std::uint8_t candidate_count{};
	bool active{};
	bool multiplayer{};
};

struct DeathEffectsRuntime
{
	std::array<TractorEffect, kTractorEffectCount> tractor;
	std::array<RipperGrabEffect, kRipperGrabEffectCount> ripper_grab;
	std::array<RespawnEffect, kRespawnEffectCount> respawn;
	std::array<ExplosionBillboardEffect, kMaxAnimatedExplosions>
		explosion_billboards;
	std::array<RockChunkEffect, kMaxRocks> rock_chunks;
	std::array<ExplodingMeshEffect, kMaxExplodingMeshes> exploding_meshes;
	std::uint32_t exploding_mesh_serial{};
	std::array<DestructionLightEffect, kMaxDestructionLights>
		destruction_lights;
	std::array<PowercoreEffect, kMaxGameObjects> powercores;
	UberExplosionEffect uber_explosion;
	std::array<DeathHudNotification, kDeathHudNotificationCount>
		hud_notifications;
	glm::vec3 camera_mode27_anchor{0.0f};
	std::uint16_t camera_mode27_anchor_owner{UINT16_MAX};
	std::uint16_t camera_mode27_anchor_generation{};
	std::uint8_t ripper_grab_cursor{};
	std::uint8_t respawn_cursor{};
	std::uint8_t hud_notification_count{};
	std::uint16_t rock_chunk_cursor{};
	bool camera_mode27_anchor_valid{};
	bool tractor_saturation_reported{};
};

void death_effects_reset(DeathEffectsRuntime& runtime);
void death_effects_enqueue_hud_notification(
	DeathEffectsRuntime& runtime,
	const DeathHudNotification& notification);
bool death_effects_pop_hud_notification(
	DeathEffectsRuntime& runtime,
	DeathHudNotification& notification);
void death_effects_release_owner(
	DeathEffectsRuntime& runtime,
	ObjectHandle owner);
bool explosion_billboard_create(
	DeathEffectsRuntime& runtime,
	World& world,
	const glm::vec3& position,
	const glm::vec3& velocity_per_tick,
	ExplosionBillboardType type,
	float size,
	std::int32_t duration_ticks,
	bool create_light,
	std::int32_t delay_ticks,
	bool animate_scale,
	bool alternate_atlas,
	std::uint32_t simulation_tick);
void destruction_light_create(
	DeathEffectsRuntime& runtime,
	ObjectHandle owner,
	std::uint16_t model_reference,
	const glm::vec3& local_position);
void powercore_effect_create(
	World& world,
	ObjectHandle owner,
	std::uint16_t model_reference,
	ParticleEmitterStyle emitter_style,
	std::uint32_t simulation_tick);
void powercore_effect_release(World& world, ObjectHandle owner);
void rock_chunk_spawn_world(
	World& world,
	const glm::vec3& position,
	const glm::vec3& direction,
	bool giant,
	std::uint32_t simulation_tick);
void explosion_mesh_breakup_world(
	World& world,
	const WorldObject& actor,
	bool blue_trail,
	std::uint32_t simulation_tick);
void explosion_component_breakup_world(
	World& world,
	const WorldObject& actor,
	const ObjectModelReference& component,
	float group_radius,
	std::uint32_t simulation_tick);
void explosion_billboards_service(
	DeathEffectsRuntime& runtime,
	World& world,
	std::uint32_t simulation_tick,
	std::uint32_t elapsed_ticks);
void uber_explosion_start(
	World& world,
	ObjectHandle owner,
	const glm::vec3& position,
	const glm::mat3& orientation,
	float size,
	std::uint32_t duration_ticks,
	bool multiplayer,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick);
void uber_explosion_service(
	World& world,
	mission::Runtime& mission,
	std::uint32_t simulation_tick);

std::int16_t tractor_effect_allocate(
	DeathEffectsRuntime& runtime,
	ObjectHandle owner,
	ObjectHandle target);
void tractor_effect_release(
	DeathEffectsRuntime& runtime,
	std::int16_t slot);
void tractor_effect_set_submitted(
	DeathEffectsRuntime& runtime,
	std::int16_t slot,
	bool submitted);
void tractor_effect_update(
	DeathEffectsRuntime& runtime,
	std::int16_t slot,
	const glm::vec3 origins[2],
	const WorldObject& target,
	float beam_a_alpha,
	float beam_b_alpha,
	float shield_intensity,
	float shield_phase,
	float light_range);

std::int16_t ripper_grab_effect_allocate(
	DeathEffectsRuntime& runtime,
	ObjectHandle owner,
	ObjectHandle target);
void ripper_grab_effect_release(
	DeathEffectsRuntime& runtime,
	std::int16_t slot);
void ripper_grab_effect_update(
	DeathEffectsRuntime& runtime,
	std::int16_t slot,
	const glm::vec3 origins[4],
	const glm::vec3 targets[4],
	float alpha);

std::int16_t respawn_effect_allocate(
	DeathEffectsRuntime& runtime,
	ObjectHandle owner,
	WorldObject& actor);
bool respawn_effect_active(
	const DeathEffectsRuntime& runtime,
	std::int16_t slot);
void respawn_effect_release(
	DeathEffectsRuntime& runtime,
	std::int16_t slot);
void respawn_effect_clear_model_override(WorldObject& actor);
void respawn_effect_update(
	DeathEffectsRuntime& runtime,
	std::int16_t slot,
	const WorldObject& actor,
	float remaining_fraction);
}
