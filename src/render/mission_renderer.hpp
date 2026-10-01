#pragma once

#include "assets/gameplay_model.hpp"
#include "core/math.hpp"
#include "game/chaff.hpp"
#include "game/attachments.hpp"
#include "game/missiles.hpp"
#include "game/runtime_limits.hpp"
#include "render/frontend_renderer.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <vector>

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::game
{
struct World;
struct WorldObject;
}

namespace sl_open::mission
{
struct EnvironmentState;
struct Runtime;
}

namespace sl_open::render
{
enum class MissionModel : std::uint16_t
{
	coyote = 0x04,
	reliant = 0x0c,
	haidar = 0x27,
	karak = 0x28,
	kurgan = 0x3c,
	kossac = 0x32,
	azan = 0x2a,
	gurevich = 0x3e,
	salin = 0x29,
	sabre = 0x2b,
	scarab = 0x49,
	saracen = 0x2e,
	berijev = 0x3f,
	lagg = 0x2c,
	badanov = 0x37,
	basilisk = 0x31,
	zakov = 0xb0,
	ramases = 0x34,
	kiev = 0xc2,
	czar = 0xb9,
	gate = 0x6d,
	asteroid1 = 0x79,
	asteroid2 = 0x7a,
	asteroid3 = 0x7b,
	asteroid5 = 0x7d,
	asteroid7 = 0x7f,
	titan_high = 0x63,
	titan_low = 0xcd,
	decoy = 0x100,
	count = 0x101,
};

struct MissionGpuSection
{
	std::uint32_t first_index{};
	std::uint32_t index_count{};
	std::uint32_t texture{};
	std::uint8_t mode{};
	std::uint8_t modifier{};
	std::uint16_t light_channel{};
	bool lines{};
	bool suppressed{};
	bool double_sided{};
};

struct MissionGpuLod
{
	bgfx::VertexBufferHandle lit_vertices{bgfx::kInvalidHandle};
	bgfx::IndexBufferHandle indices{bgfx::kInvalidHandle};
	bgfx::IndexBufferHandle cloak_indices{bgfx::kInvalidHandle};
	std::vector<assets::GameplayVertex> source_vertices;
	std::vector<assets::GameplayVertex> cloak_source_vertices;
	std::vector<glm::vec3> normals;
	std::vector<glm::vec3> secondary_normals;
	std::vector<glm::vec3> static_lighting_rgb;
	std::vector<std::uint16_t> source_indices;
	std::vector<std::uint16_t> cloak_source_indices;
	std::vector<assets::GameplayFace> source_faces;
	std::vector<std::uint32_t> source_face_corners;
	std::vector<assets::GameplaySection> source_sections;
	std::vector<MissionGpuSection> sections;
	std::vector<MissionGpuSection> cloak_sections;
	float threshold{};
	float radius{};
	glm::vec3 bounds_min{0.0f};
	glm::vec3 bounds_max{0.0f};
	std::uint32_t index_count{};
	std::uint32_t cloak_index_count{};
};

struct MissionGpuNode
{
	char name[65]{};
	glm::mat4 object_transform{1.0f};
	std::vector<MissionGpuLod> lods;
	std::vector<std::uint32_t> locator_indices;
	std::vector<assets::GameplaySequence> sequences;
	std::vector<assets::GameplayPointGroup> point_groups;
	std::vector<assets::GameplayPortal> portals;
	assets::GameplayCollisionTree collision;
	std::uint32_t model_type{};
	std::uint32_t part_group_id{};
	std::uint32_t flags{};
	std::uint16_t gun_mount_kind{};
	std::uint32_t gun_part_slot{};
	std::int32_t maximum_hit_points{};
	std::uint32_t damage_group_selector{};
	bool subsystem_highlightable{};
	std::uint32_t suppress_anim_rotation[3]{};
	glm::vec3 exported_position{0.0f};
	glm::vec3 rest_translation{0.0f};
	glm::mat3 source_local_basis{1.0f};
	glm::vec3 joint_min_degrees{0.0f};
	glm::vec3 joint_max_degrees{0.0f};
	glm::vec3 bounds_min{0.0f};
	glm::vec3 bounds_max{0.0f};
	float radius{};
	std::uint16_t runtime_model_index{};
	std::int16_t parent_reference{-1};
};

struct MissionGpuLocator
{
	glm::mat4 object_transform{1.0f};
	std::int32_t parameters[4]{};
	glm::vec3 dimensions{0.0f};
	std::int32_t on_time{};
	std::int32_t off_time{};
	std::int32_t phase{};
	std::uint32_t exporter_id{};
	float light_radius{};
	float light_intensity{};
	std::uint16_t source_node{};
	std::int16_t type{};
	std::int16_t subtype{};
};

struct MissionGpuModel
{
	std::vector<MissionGpuNode> nodes;
	// Serialized model indices remain source-ordered. Rendering separately
	// walks the linked hierarchy in preorder before scene insertion reverses
	// that order.
	std::vector<std::uint16_t> node_indices_in_preorder;
	std::vector<FrontendTexture> textures;
	std::vector<FrontendTexture> alternate_textures;
	std::vector<bool> alternate_texture_loaded;
	std::vector<assets::GameplayHardpoint> hardpoints;
	std::vector<assets::GameplayLocator> gameplay_locators;
	std::vector<MissionGpuLocator> locators;
	std::vector<glm::vec3> gun_clearance_directions;
	std::vector<std::array<std::uint8_t, 64>> gun_clearance_masks;
	glm::vec3 bounds_min{0.0f};
	glm::vec3 bounds_max{0.0f};
	glm::vec3 center_of_mass{0.0f};
	glm::mat3 inertia_tensor{0.0f};
	glm::vec3 camera_offset{0.0f};
	std::uint32_t root_flags{};
	float total_mass{};
	float radius{};
};

constexpr std::uint32_t kMissionHudSchematicShapeCount = 5;

struct MissionHudSchematic
{
	FrontendTexture shapes[kMissionHudSchematicShapeCount];
	bool ready{};
};

struct MissionStar
{
	glm::vec3 source{0.0f};
	std::uint32_t color{};
};

constexpr std::uint32_t kMissionStarCount = 1050;
constexpr std::uint32_t kMissionStarTileCount = 100;
constexpr std::uint32_t kMissionRandomStarCount = 200;

struct MissionStarTile
{
	glm::mat3 orientation{1.0f};
	std::uint16_t first{};
	std::uint16_t count{};
};

struct MissionEnvironmentMesh
{
	bgfx::VertexBufferHandle vertices{bgfx::kInvalidHandle};
	bgfx::IndexBufferHandle indices{bgfx::kInvalidHandle};
	std::uint32_t index_count{};
};

struct MissionEngineFlare
{
	MissionEnvironmentMesh mesh;
	FrontendTexture material_a;
	FrontendTexture material_b;
};

struct MissionFarAsteroid
{
	glm::vec3 camera_offset{0.0f};
	float scale{};
	float roll_rate{};
	float brightness{};
	std::uint8_t uv_band{UINT8_MAX};
};

struct MissionPlanetBombardSlot
{
	glm::vec3 local_position{0.0f};
	glm::mat3 local_orientation{1.0f};
	glm::vec2 uv[6]{};
	glm::vec4 vertex_color[4]{};
	float square_size{};
	float scale{};
	float brightness{};
	std::int32_t scheduled_or_start_tick{};
	bool active{};
};

constexpr std::uint32_t kMissionFarAsteroidCapacity = 800;
constexpr std::uint32_t kMissionPlanetBombardSlots = 10;

struct MissionEnvironmentRenderer
{
	bgfx::VertexLayout layout;
	MissionEnvironmentMesh nebula_dome;
	MissionEnvironmentMesh nebula_grid_broad;
	MissionEnvironmentMesh nebula_grid_narrow;
	MissionEnvironmentMesh planet_atmosphere;
	MissionEnvironmentMesh planet_atmosphere_compact;
	FrontendTexture nebula_materials[7];
	FrontendTexture planet_atmosphere_texture;
	FrontendTexture far_asteroid_texture;
	FrontendTexture planet_bombard_texture;
	FrontendTexture sun_layers[3];
	FrontendTexture sun_flares[4];
	MissionFarAsteroid far_asteroids[kMissionFarAsteroidCapacity];
	MissionPlanetBombardSlot bombard[kMissionPlanetBombardSlots];
	std::uint32_t far_asteroid_count{};
	std::uint32_t last_bombard_callback_tick{};
	std::uint32_t last_atmosphere_tick{};
	float previous_sun_edge_distance{};
	float sun_layer_three_brightness{};
	bool atmosphere_tick_valid{};
	bool initialized{};
};

struct MissionSceneLight
{
	glm::vec3 position{0.0f};
	glm::vec3 direction{0.0f, 0.0f, 1.0f};
	glm::vec3 rgb{0.0f};
	float intensity{};
	float radius{};
	std::uint32_t mask{};
	std::uint8_t subtype{};
};

struct ExplodingMeshGpuIndices
{
	bgfx::IndexBufferHandle handle{bgfx::kInvalidHandle};
	std::uint32_t geometry_serial{};
};

struct MissionRenderer
{
	MissionGpuModel models[static_cast<std::size_t>(MissionModel::count)];
	bool model_loaded[static_cast<std::size_t>(MissionModel::count)]{};
	MissionGpuModel cockpit_model;
	MissionGpuModel
		attachment_models[game::kAttachmentDefinitionCount][2];
	bool attachment_model_loaded[
		game::kAttachmentDefinitionCount][2]{};
	MissionHudSchematic
		hud_schematics[static_cast<std::size_t>(MissionModel::count)];
	FrontendSpriteAtlas hud_schematic_atlas;
	FrontendTexture missile_lock_ring;
	FrontendTexture laser_cannon_texture;
	FrontendTexture pulse_cannon_texture[2];
	FrontendTexture collapser_cannon_texture[2];
	FrontendTexture ion_cannon_texture;
	FrontendTexture ion_beam_texture;
	FrontendTexture ion_field_texture;
	FrontendTexture nova_cannon_texture;
	FrontendTexture huge_gun_texture;
	FrontendTexture spark_huge_texture;
	FrontendTexture muzzle_flare_a_texture;
	FrontendTexture muzzle_flare_b_texture;
	FrontendTexture particle_texture;
	FrontendTexture damage_particle_texture;
	FrontendTexture powercore_texture;
	FrontendTexture missile_trail_texture;
	FrontendTexture missile_flare_texture;
	FrontendTexture explosion_sheet_texture;
	FrontendTexture flak_explosion_texture;
	FrontendTexture destruction_explosion_textures[16];
	FrontendTexture cloak_texture;
	FrontendTexture model_light_flare_texture;
	FrontendTexture model_light_core_texture;
	FrontendTexture model_modifier_textures[8];
	FrontendTexture shield_texture;
	FrontendTexture forcefield_texture;
	FrontendTexture sfx_alpha_texture;
	FrontendTexture shockwave_textures[5];
	FrontendTexture jump_trail_texture;
	FrontendTexture jump_flare_texture;
	FrontendTexture jump_light_texture;
	FrontendTexture jump_light_bright_texture;
	FrontendTexture warp_primary_texture;
	FrontendTexture warp_secondary_texture;
	MissionEngineFlare engine_flares[7];
	MissionEnvironmentRenderer environment;
	ExplodingMeshGpuIndices exploding_mesh_indices[game::kMaxExplodingMeshes];
	std::vector<MissionSceneLight> scene_lights;
	MissionStar stars[kMissionStarCount];
	MissionStar random_stars[kMissionRandomStarCount];
	MissionStarTile star_tiles[kMissionStarTileCount];
	glm::mat3 previous_star_camera{1.0f};
	glm::vec3 previous_star_position{0.0f};
	std::uint32_t star_count{};
	std::uint32_t previous_camera_cut_serial{};
	std::uint32_t dock_ring_phase{};
	std::uint32_t dock_ring_last_tick{};
	std::uint64_t lod_previous_submit_nanoseconds{};
	mutable std::uint32_t transparent_submission_order{};
	float lod_detail_scale{1.0f};
	std::uint8_t graphics_quality{2};
	bool star_sample_valid{};
	bool dock_ring_tick_valid{};
	bool lod_submit_time_valid{};
	bool deathmatch_mission{};
	bool cockpit_model_loaded{};
	bool ready{};
};

struct MissionRenderInstance
{
	MissionModel model{};
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	float scale{1.0f};
	std::uint32_t selected_part_group_id{UINT32_MAX};
	const game::WorldObject* source_object{};
	const game::WorldObject* effect_owner{};
	std::uint16_t selected_part_owner_scope{UINT16_MAX};
	const std::vector<game::CloakMeshRuntime>* effect_cloak_models{};
	std::uint16_t runtime_reference_base{};
	bool effect_root_wobble_applied{};
	const glm::mat4* node_transform_override{};
	std::uint16_t submitted_node_count{UINT16_MAX};
	bool foreground_overlay{};
};

constexpr std::uint32_t kMaxMissionRenderInstances =
	game::kMaxGameObjects + game::kMaxMeshDebris + game::kMaxRocks
		+ game::kMaxChaff;

struct MissionGunProjectile
{
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	std::uint32_t spawn_tick{};
	std::uint32_t expiration_tick{};
	float visual_random[12]{};
	std::int16_t shooter_affiliation{};
	std::uint8_t type_index{};
};

struct MissionNovaBeam
{
	glm::vec3 start{0.0f};
	glm::vec3 end{0.0f};
	glm::mat3 orientation{1.0f};
	std::uint32_t expiration_tick{};
	float charge{};
	bool fully_charged{};
};

struct MissionMuzzleFlash
{
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	std::uint32_t start_tick{};
	std::uint32_t expiration_tick{};
	std::int16_t shooter_affiliation{};
	std::uint8_t type{};
};

struct MissionMissile
{
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	std::uint8_t type{};
	std::uint8_t model_variant{};
};

struct MissionMissileTrail
{
	// MissionRenderFrame is consumed synchronously before the missile runtime
	// can advance again. Retain the stable fixed-pool record instead of
	// duplicating all five 70-ring buffers into this stack-resident frame.
	const game::MissileTrail* trail{};
};

struct MissionParticle
{
	glm::vec3 position{0.0f};
	float size{};
	std::uint32_t color{};
	float height_scale{1.0f};
	bool missile_trail_texture{};
	glm::mat3 orientation{1.0f};
	bool oriented{};
	float u0{};
	float v0{};
	float u1{1.0f};
	float v1{1.0f};
};

struct MissionLaunchTrailRing
{
	glm::vec3 points[4]{};
	std::uint32_t birth_tick{};
	std::uint32_t sequence{};
	std::uint16_t owner{UINT16_MAX};
	std::uint16_t generation{};
};

constexpr std::uint32_t kMaxMissionLaunchTrailRings = 200;

struct MissionPlanetBombardOwner
{
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	float radius{};
	std::uint16_t world_index{UINT16_MAX};
	std::uint32_t creation_serial{};
	bool visible{};
};

struct MissionRenderFrame
{
	glm::vec3 camera_position{0.0f};
	glm::mat3 camera_orientation{1.0f};
	glm::vec3 cockpit_local_position{0.0f};
	glm::mat3 cockpit_orientation{1.0f};
	glm::mat3 cockpit_component_orientation{1.0f};
	float cockpit_recoil_offset{};
	MissionRenderInstance instances[kMaxMissionRenderInstances];
	MissionGunProjectile gun_projectiles[game::kMaxGunProjectiles];
	MissionNovaBeam nova_beams[8];
	MissionMuzzleFlash muzzle_flashes[64];
	MissionMissile missiles[game::kMaxMissiles];
	MissionMissileTrail missile_trails[game::kMaxMissileTrails];
	MissionParticle particles[
		game::kMaxChaffParticles
			+ game::kMaxLaunchParticles
			+ 200];
	MissionLaunchTrailRing launch_trail_rings[kMaxMissionLaunchTrailRings];
	MissionPlanetBombardOwner
		bombard_owners[game::kMaxGameObjects];
	mission::EnvironmentState* environment{};
	game::World* world{};
	std::uint32_t* random_seed{};
	std::uint32_t instance_count{};
	std::uint32_t gun_projectile_count{};
	std::uint32_t nova_beam_count{};
	std::uint32_t muzzle_flash_count{};
	std::uint32_t missile_count{};
	std::uint32_t missile_trail_count{};
	std::uint32_t particle_count{};
	std::uint32_t launch_trail_ring_count{};
	std::uint32_t bombard_owner_count{};
	std::uint32_t camera_cut_serial{};
	std::uint32_t simulation_tick{};
	float horizontal_tangent{0.5f / 0.6000000238418579f};
	float vertical_tangent{0.625f};
	glm::vec3 dock_ring_target_position{0.0f};
	glm::mat3 dock_ring_target_orientation{1.0f};
	std::uint8_t camera_mode{};
	std::uint8_t camera_view_state{};
	bool dock_ring_active{};
};

bool mission_model_for_type(
	std::uint16_t type,
	MissionModel& model);
void mission_renderer_initialize_world_components(
	const MissionRenderer& renderer,
	game::World& world,
	game::ChaffRuntime* chaff);
bool mission_renderer_init(
	io::Vfs& vfs,
	const FrontendRenderer& frontend,
	MissionRenderer& renderer,
	const mission::Runtime& mission_runtime,
	std::uint16_t player_type,
	std::uint8_t graphics_quality,
	std::uint32_t& random_seed,
	std::uint32_t gameplay_tick);
void mission_renderer_shutdown(MissionRenderer& renderer);
void mission_renderer_submit(
	MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	float brightness);
}
