#pragma once

#include "assets/image.hpp"
#include "core/blob.hpp"
#include "core/math.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::assets
{
struct TextureCache;

// Authored flags on the tag-0 SRO root record. The compound bit selects the
// compound GameObject/model-tree path; the cloak bit advertises the retained
// cloak mesh path on resource generations which carry this field.
inline constexpr std::uint32_t kGameplayModelRootCompound = 0x00000001u;
inline constexpr std::uint32_t kGameplayModelRootCloak = 0x00000002u;

struct GameplayVertex
{
	float x;
	float y;
	float z;
	std::uint32_t rgba;
	float u;
	float v;
};
static_assert(sizeof(GameplayVertex) == sizeof(float) * 6);

struct GameplaySection
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

struct GameplayFace
{
	std::uint32_t first_index{};
	std::uint32_t index_count{};
	std::uint32_t first_corner{};
	std::uint16_t corner_count{};
	std::uint16_t section{};
};

struct GameplayLod
{
	sl_open::Blob vertices;
	sl_open::Blob indices;
	sl_open::Blob cloak_vertices;
	sl_open::Blob cloak_indices;
	std::vector<glm::vec3> normals;
	std::vector<glm::vec3> secondary_normals;
	std::vector<glm::vec3> secondary_positions;
	std::vector<glm::vec3> static_lighting_rgb;
	std::vector<GameplaySection> sections;
	std::vector<GameplaySection> cloak_sections;
	// Explosion_mesh_split consumes the pre-triangulation face corners.
	// Keep those alongside the draw indices so merged SRO fans retain the
	// same classification point and recentering weight as retail.
	std::vector<GameplayFace> faces;
	std::vector<std::uint32_t> face_corners;
	float threshold{};
	float radius{};
	// Planet construction recenters mesh points after publishing object bounds.
	glm::vec3 origin_offset{0.0f};
	float original_radius{};
	glm::vec3 bounds_min{0.0f};
	glm::vec3 bounds_max{0.0f};
	std::uint32_t vertex_count{};
	std::uint32_t index_count{};
	std::uint32_t cloak_vertex_count{};
	std::uint32_t cloak_index_count{};
};

struct GameplayCollisionTriangle
{
	glm::vec3 points[3]{};
	glm::vec3 normals[3]{};
};

struct GameplayCollisionPolygon
{
	std::uint32_t first_triangle{};
	std::uint32_t triangle_count{};
	glm::vec3 plane_normal{0.0f};
};

struct GameplayCollisionNode
{
	glm::mat3 orientation{1.0f};
	glm::vec3 half_extents{0.0f};
	glm::vec3 center{0.0f};
	std::uint32_t child_a{UINT32_MAX};
	std::uint32_t child_b{UINT32_MAX};
	std::vector<std::uint32_t> polygons;
};

struct GameplayCollisionTree
{
	std::vector<GameplayCollisionNode> nodes;
	std::vector<GameplayCollisionPolygon> polygons;
	std::vector<GameplayCollisionTriangle> triangles;
};

struct GameplayPortal
{
	glm::vec3 vertices[4]{};
	glm::vec3 plane_normal{0.0f};
	float plane_constant{};
	std::uint8_t vertex_count{};
};

struct GameplaySequenceKey
{
	std::int32_t time{};
	glm::vec3 euler{0.0f};
	glm::vec3 translation{0.0f};
};

struct GameplaySequenceEvent
{
	std::int32_t time{};
	std::int32_t type{};
	std::int32_t parameter{};
};

struct GameplaySequence
{
	char name[19]{};
	std::vector<GameplaySequenceKey> keys;
	std::vector<GameplaySequenceEvent> events;
	std::int32_t duration{};
	std::int16_t default_mode{};
};

struct GameplayPoint
{
	glm::vec3 position{0.0f};
	glm::vec3 direction{0.0f, 0.0f, 1.0f};
	std::uint32_t polygon{};
};

struct GameplayPointGroup
{
	std::int32_t type{};
	std::vector<GameplayPoint> points;
};

struct GameplayNode
{
	char name[65]{};
	std::int32_t parent{-1};
	std::uint32_t model_type{};
	std::uint32_t part_group_id{};
	std::uint32_t flags{};
	std::uint16_t gun_mount_kind{};
	std::uint32_t gun_part_slot{};
	std::int32_t maximum_hit_points{};
	std::uint32_t damage_group_selector{};
	glm::vec3 position{0.0f};
	glm::vec3 rest_translation{0.0f};
	glm::mat3 basis{1.0f};
	std::uint32_t suppress_anim_rotation[3]{};
	std::vector<GameplayPointGroup> point_groups;
	glm::vec3 joint_min_degrees{0.0f};
	glm::vec3 joint_max_degrees{0.0f};
	glm::vec3 bounds_min{0.0f};
	glm::vec3 bounds_max{0.0f};
	std::vector<GameplayLod> lods;
	std::vector<GameplayPortal> portals;
	std::vector<GameplaySequence> sequences;
	GameplayCollisionTree collision;
};

struct GameplayMaterial
{
	char basename[65]{};
	TextureImage image;
	TextureImage alternate_image;
	bool has_alternate{};
};

constexpr std::uint32_t kMaximumGameplayHardpoints = 20;
// GameObject_create_runtime normalizes the selector to 0..4, and
// GameObject_rebuild_ordnance_definitions indexes the five consecutive
// dwords at locator +0x34 through +0x44 with that value.
constexpr std::uint32_t kGameplayLoadoutTiers = 5;

struct GameplayHardpoint
{
	glm::vec3 position{0.0f};
	glm::mat3 basis{1.0f};
	std::int32_t default_loadout[kGameplayLoadoutTiers]{
		-1, -1, -1, -1, -1};
	std::uint16_t source_node{};
};

struct GameplayLocator
{
	glm::vec3 position{0.0f};
	glm::mat3 basis{1.0f};
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

struct GameplayModel
{
	std::vector<GameplayNode> nodes;
	std::vector<GameplayMaterial> materials;
	std::vector<GameplayHardpoint> hardpoints;
	std::vector<GameplayLocator> locators;
	std::vector<glm::vec3> gun_clearance_directions;
	std::vector<std::array<std::uint8_t, 64>> gun_clearance_masks;
	glm::vec3 center_of_mass{0.0f};
	// Aggregate body-space inertia about center_of_mass. Compound
	// collisions consume the authored volume integrals rather than a
	// bounding-sphere approximation.
	glm::mat3 inertia_tensor{0.0f};
	// Tag-0 +0x08. GameObject_instantiate_sro_model copies this authored
	// vector to GameObject+0x628; all cockpit/side camera modes consume it.
	glm::vec3 camera_offset{0.0f};
	std::uint32_t root_flags{};
	// SRO_load also builds the dedicated cloak mesh for the twelve retail
	// multiplayer fighter resources, independently of the authored cloak flag.
	bool cloak_mesh_available{};
	float total_mass{};
	float radius{};
};

bool parse_gameplay_model(
	sl_open::Blob stored,
	const TextureCache& texture_cache,
	GameplayModel& model,
	bool force_cloak_mesh = false,
	bool planet = false);
bool load_gameplay_model(
	io::Vfs& vfs,
	const char* path,
	const TextureCache& texture_cache,
	GameplayModel& model);
}
