#pragma once

#include "assets/gameplay_model.hpp"
#include "render/texture.hpp"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <vector>

namespace sl_open::render
{
struct FrontendRenderer;

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

bool model_renderer_init_materials(FrontendRenderer& renderer);
bool model_renderer_upload(
	const assets::GameplayModel& source,
	MissionGpuModel& destination,
	const bgfx::VertexLayout& layout);
void model_renderer_shutdown(MissionGpuModel& model);
void model_renderer_submit_preview(
	const FrontendRenderer& renderer,
	const MissionGpuModel& model,
	bgfx::ViewId view,
	const glm::mat4& transform,
	bool gun_model,
	const glm::vec3& color,
	const glm::vec4& clip_plane,
	std::uint8_t lod_index = 0);
}
