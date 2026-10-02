#include "render/mission_renderer.hpp"

#include "assets/gameplay_model.hpp"
#include "assets/image.hpp"
#include "assets/missile_stats.hpp"
#include "assets/object_type_catalog.hpp"
#include "assets/player_ship.hpp"
#include "assets/texture_cache.hpp"
#include "assets/vfx.hpp"
#include "core/mission_log.hpp"
#include "game/attachments.hpp"
#include "game/exhaust_hazard.hpp"
#include "game/model_animation.hpp"
#include "game/retained_components.hpp"
#include "game/world.hpp"
#include "io/vfs.hpp"
#include "mission/environment_effects.hpp"
#include "mission/network_runtime.hpp"
#include "mission/runtime.hpp"
#include "render/retail_clip.hpp"
#include "render/lighting_response.hpp"
#include "frontend/loadout_layout.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <span>

namespace sl_open::render
{
void initialize_attachment_cloak_models(
	game::AttachmentSlot& attachment,
	const MissionGpuModel& model,
	std::uint16_t object_type);
bool attachment_cloak_models_match(
	const game::AttachmentSlot& attachment,
	const MissionGpuModel& model);

namespace
{
constexpr bgfx::ViewId kMissionEnvironmentView = 1;
constexpr bgfx::ViewId kMissionSunView = 2;
constexpr bgfx::ViewId kMissionStarView = 3;
constexpr bgfx::ViewId kMissionFarAsteroidView = 4;
constexpr bgfx::ViewId kMissionAtmosphereView = 5;
constexpr bgfx::ViewId kMissionNebulaGridView = 6;
constexpr bgfx::ViewId kMissionView = 7;
constexpr bgfx::ViewId kMissionTransparentView = 8;
constexpr bgfx::ViewId kMissionCockpitView = 9;
constexpr float kMissionNearPlane = 100.0f;
constexpr std::uint32_t kRuntimeModelForceHighestDetail = 0x0008u;
constexpr std::uint32_t kRuntimeModelHidden = 0x0020u;

const char* cockpit_model_path(std::uint16_t player_type)
{
	// mission_runtime_init (LANCER.EXE 0x00493737..0x004938b7)
	// selects one camera-attached frame SRO from the effective player type.
	// Tiger multiplayer aliases retain the matching base cockpit.
	if (player_type >= 0xf4u && player_type <= 0xffu)
	{
		player_type = static_cast<std::uint16_t>(player_type - 0xf4u);
	}
	if (player_type == 0x2du)
	{
		return "kamg_frm.shp";
	}
	constexpr const char* kCockpitModels[assets::kPlayerShipCount] = {
		"preg_frm.shp",
		"nagg_frm.shp",
		"gre2_frm.shp",
		"cru3_frm.shp",
		"coyg_frm.shp",
		"mirg_frm.shp",
		"temg_frm.shp",
		"pat2_frm.shp",
		"wolv_frm.shp",
		"rea2_frm.shp",
		"shr2_frm.shp",
		"phe2_frm.shp",
	};
	return player_type < std::size(kCockpitModels)
		? kCockpitModels[player_type]
		: nullptr;
}

const game::CapitalExplosionController* active_explosion_portal(
	const MissionRenderFrame& frame,
	const game::WorldObject& object)
{
	if (frame.world == nullptr)
	{
		return nullptr;
	}
	std::uint16_t object_index = UINT16_MAX;
	for (std::uint16_t index = 0; index < game::kMaxGameObjects; ++index)
	{
		if (&frame.world->objects[index] == &object)
		{
			object_index = index;
			break;
		}
	}
	if (object_index == UINT16_MAX)
	{
		return nullptr;
	}
	for (const game::CapitalExplosionController& controller
		: frame.world->disruption_effects.explosion_controllers)
	{
		if (!controller.active)
		{
			continue;
		}
		const bool owner = controller.owner_index == object_index
			&& controller.owner_generation == object.generation;
		const bool breakaway = controller.breakaway_index == object_index
			&& controller.breakaway_generation == object.generation;
		if (owner || breakaway)
		{
			return &controller;
		}
	}
	return nullptr;
}

glm::mat4 explosion_portal_root(
	const MissionRenderFrame& frame,
	const game::WorldObject& object,
	std::uint16_t reference,
	const glm::mat4& ordinary_root,
	float scale,
	bool* visible = nullptr)
{
	if (visible != nullptr)
	{
		*visible = true;
	}
	if (reference >= object.model_references.size()
		|| object.model_references[reference].explosion_portal_group == 0)
	{
		return ordinary_root;
	}
	const game::CapitalExplosionController* controller =
		active_explosion_portal(frame, object);
	if (controller == nullptr)
	{
		return ordinary_root;
	}
	if (!controller->portal_submitted)
	{
		if (visible != nullptr)
		{
			*visible = false;
		}
		return ordinary_root;
	}
	// RuntimeTree_bind_render_parent_recursive only changes the Surrender
	// render parent.  The two controller objects are opposing clipping
	// portals; +0x3c is their moving plane position.  Treating that position
	// as the model root translated the complete normal and DEST hulls to each
	// scheduled explosion point, exposing the owner's deliberate +/-10 shake.
	glm::vec3 retained_root = controller->original_owner_position;
	if (frame.world != nullptr
		&& controller->breakaway_index < game::kMaxGameObjects
		&& &frame.world->objects[controller->breakaway_index] == &object
		&& controller->owner_index < game::kMaxGameObjects)
	{
		const game::WorldObject& owner =
			frame.world->objects[controller->owner_index];
		retained_root += controller->portal_orientation
			* (object.center_of_mass - owner.center_of_mass);
	}
	return sl_open::math::model_transform(
		controller->portal_orientation,
		scale,
		retained_root);
}

bool explosion_portal_local_clip_plane(
	const MissionRenderFrame& frame,
	const game::WorldObject& object,
	std::uint16_t reference,
	const glm::mat4& model_transform,
	glm::vec4& local_plane)
{
	if (reference >= object.model_references.size())
	{
		return false;
	}
	const std::uint8_t group =
		object.model_references[reference].explosion_portal_group;
	if (group == 0)
	{
		return false;
	}
	const game::CapitalExplosionController* controller =
		active_explosion_portal(frame, object);
	if (controller == nullptr
		|| !controller->portal_submitted
		|| !controller->portal_clipping)
	{
		return false;
	}

	// Generic_destruction_sequence_start gives portal one local vector
	// (0,0,-1) and portal two (0,0,1). Surrender rejects the half-space in
	// the direction of that vector, whereas the shader below retains signed
	// distances >= 0, so use the negated authored portal normals here.
	const glm::vec3 normal = controller->portal_orientation
		* glm::vec3{0.0f, 0.0f, group == 1 ? 1.0f : -1.0f};
	const glm::vec4 world_plane{
		normal, -glm::dot(normal, controller->portal_position)};
	local_plane = glm::transpose(model_transform) * world_plane;
	return true;
}

enum class RetailBlendSelector : std::uint8_t
{
	opaque = 0,
	additive = 1,
	premultiplied_alpha = 2,
	source_alpha = 3,
	source_alpha_additive = 4,
};

constexpr std::uint64_t retail_blend_state(
	RetailBlendSelector selector)
{
	// srd3d_sync_material_state (srd3d.dll 0x10001b20) indexes the
	// source table at 0x10057e0c and destination table at 0x1002977c
	// with material byte +6. Selector zero disables blending.
	switch (selector)
	{
	case RetailBlendSelector::opaque:
		return 0;
	case RetailBlendSelector::additive:
		return BGFX_STATE_BLEND_ADD;
	case RetailBlendSelector::premultiplied_alpha:
		return BGFX_STATE_BLEND_FUNC(
			BGFX_STATE_BLEND_ONE,
			BGFX_STATE_BLEND_INV_SRC_ALPHA);
	case RetailBlendSelector::source_alpha:
		return BGFX_STATE_BLEND_ALPHA;
	case RetailBlendSelector::source_alpha_additive:
		return BGFX_STATE_BLEND_FUNC(
			BGFX_STATE_BLEND_SRC_ALPHA,
			BGFX_STATE_BLEND_ONE);
	}
	return 0;
}

void submit_retail_transparent(
	const MissionRenderer& renderer,
	bgfx::ProgramHandle program,
	bgfx::ViewId view = kMissionTransparentView)
{
	// The SRO, BMO, and generic-mesh callbacks prepend every blended face
	// to the bucket-one list (srd3d.dll 0x10002400, 0x10006ad0,
	// 0x10006bf0, and 0x10006c80). srd3d_render_list (0x10003390)
	// drains that list only after opaque callbacks have completed. A unique,
	// increasing bgfx depth key plus DepthDescending reproduces the exact
	// reverse encounter order without confusing it with camera-space depth:
	// retail computes and stores that value but never sorts this list by it.
	// The foreground cockpit shares one DepthAscending view with its opaque
	// sections; keys descending from UINT32_MAX keep those blended sections
	// after depth-zero opaque work while retaining the same reverse order.
	const std::uint32_t order =
		renderer.transparent_submission_order++;
	bgfx::submit(
		view,
		program,
		view == kMissionCockpitView ? UINT32_MAX - order : order);
}

constexpr const char* kDecoyModelPath = "ships/decoy.shp";

std::uint64_t transition_blend_state(game::TransitionBlend blend)
{
	switch (blend)
	{
	case game::TransitionBlend::additive:
		return retail_blend_state(RetailBlendSelector::additive);
	case game::TransitionBlend::source_alpha_additive:
		return retail_blend_state(
			RetailBlendSelector::source_alpha_additive);
	}
	return retail_blend_state(RetailBlendSelector::additive);
}

struct StarVertex
{
	float x;
	float y;
	float z;
	std::uint32_t color;
	float u;
	float v;
};
static_assert(sizeof(StarVertex) == sizeof(float) * 6);

struct ModelRenderVertex
{
	float x;
	float y;
	float z;
	std::uint32_t color;
	float u;
	float v;
	float environment_u;
	float environment_v;
};
static_assert(sizeof(ModelRenderVertex) == sizeof(float) * 8);
static_assert(offsetof(ModelRenderVertex, color) == 12);
static_assert(offsetof(ModelRenderVertex, u) == 16);
static_assert(offsetof(ModelRenderVertex, environment_u) == 24);

struct LitModelVertex
{
	float x;
	float y;
	float z;
	float u;
	float v;
	float normal_x;
	float normal_y;
	float normal_z;
	float secondary_normal_x;
	float secondary_normal_y;
	float secondary_normal_z;
	float static_red;
	float static_green;
	float static_blue;
	float secondary_position_x;
	float secondary_position_y;
	float secondary_position_z;
};
static_assert(sizeof(LitModelVertex) == sizeof(float) * 17);

ModelRenderVertex model_render_vertex(
	const assets::GameplayVertex& source)
{
	return {
		source.x,
		source.y,
		source.z,
		source.rgba,
		source.u,
		source.v,
		0.0f,
		0.0f,
	};
}

glm::vec4 unpack_model_diffuse(std::uint32_t color)
{
	return {
		static_cast<float>(color & 0xffu) * (1.0f / 255.0f),
		static_cast<float>((color >> 8) & 0xffu) * (1.0f / 255.0f),
		static_cast<float>((color >> 16) & 0xffu) * (1.0f / 255.0f),
		static_cast<float>((color >> 24) & 0xffu) * (1.0f / 255.0f),
	};
}

std::uint32_t pack_model_diffuse(const glm::vec4& color)
{
	const auto channel = [](float value)
	{
		return static_cast<std::uint32_t>(
			std::clamp(std::lrint(value * 255.0f), 0l, 255l));
	};
	return channel(color.r)
		| channel(color.g) << 8
		| channel(color.b) << 16
		| channel(color.a) << 24;
}

retail_clip::Vertex model_clip_vertex(
	const ModelRenderVertex& source,
	const glm::mat4& object_to_camera)
{
	const glm::vec3 local{source.x, source.y, source.z};
	return {
		local,
		glm::vec3(object_to_camera * glm::vec4(local, 1.0f)),
		unpack_model_diffuse(source.color),
		{source.u, source.v},
		{source.environment_u, source.environment_v},
	};
}

ModelRenderVertex model_render_vertex(
	const retail_clip::Vertex& source)
{
	return {
		source.local_position.x,
		source.local_position.y,
		source.local_position.z,
		pack_model_diffuse(source.diffuse),
		source.primary_uv.x,
		source.primary_uv.y,
		source.environment_uv.x,
		source.environment_uv.y,
	};
}

struct RetailFrustumClassification
{
	std::uint8_t any_outside{};
	std::uint8_t all_outside{retail_clip::kFrustumPlanes};
};

RetailFrustumClassification classify_model_bounds(
	const glm::vec3& minimum,
	const glm::vec3& maximum,
	const glm::mat4& object_to_camera,
	const retail_clip::Frustum& frustum)
{
	RetailFrustumClassification classification;
	for (std::uint32_t corner = 0; corner < 8; ++corner)
	{
		const glm::vec3 local{
			(corner & 1u) != 0 ? maximum.x : minimum.x,
			(corner & 2u) != 0 ? maximum.y : minimum.y,
			(corner & 4u) != 0 ? maximum.z : minimum.z,
		};
		const std::uint8_t flags = retail_clip::classify(
			glm::vec3(object_to_camera * glm::vec4(local, 1.0f)),
			frustum);
		classification.any_outside |= flags;
		classification.all_outside &= flags;
	}
	return classification;
}

struct ClippedSectionCounts
{
	std::uint32_t vertices{};
	std::uint32_t indices{};
};

ClippedSectionCounts clipped_section_counts(
	const std::vector<std::uint16_t>& source_indices,
	std::size_t source_vertex_count,
	const MissionGpuSection& section,
	const ModelRenderVertex* source_vertices,
	const glm::mat4& object_to_camera,
	const retail_clip::Frustum& frustum,
	const glm::vec4* local_clip_plane)
{
	ClippedSectionCounts counts;
	const std::uint32_t vertices_per_face = section.lines ? 2u : 3u;
	const std::uint64_t section_end =
		static_cast<std::uint64_t>(section.first_index)
			+ section.index_count;
	if (source_vertices == nullptr
		|| section_end > source_indices.size())
	{
		return counts;
	}
	std::array<retail_clip::Vertex, 3> face;
	for (std::uint32_t index = section.first_index;
		index + vertices_per_face <= section_end;
		index += vertices_per_face)
	{
		bool valid = true;
		for (std::uint32_t corner = 0;
			corner < vertices_per_face;
			++corner)
		{
			const std::uint16_t source_index =
				source_indices[index + corner];
			if (source_index >= source_vertex_count)
			{
				valid = false;
				break;
			}
			face[corner] = model_clip_vertex(
				source_vertices[source_index],
				object_to_camera);
		}
		if (!valid)
		{
			continue;
		}
		retail_clip::Polygon polygon;
		if (!polygon.clip(
				std::span<const retail_clip::Vertex>{
					face.data(), vertices_per_face},
				frustum)
			|| (local_clip_plane != nullptr
				&& !polygon.clip_local_half_space(
					*local_clip_plane)))
		{
			continue;
		}
		const std::uint32_t polygon_vertices = polygon.size();
		const std::uint32_t polygon_indices = section.lines
			? 2u
			: (polygon_vertices - 2u) * 3u;
		if (counts.vertices
				> UINT32_MAX - polygon_vertices
			|| counts.indices > UINT32_MAX - polygon_indices)
		{
			return {};
		}
		counts.vertices += polygon_vertices;
		counts.indices += polygon_indices;
	}
	return counts;
}

bool write_clipped_section(
	const std::vector<std::uint16_t>& source_indices,
	std::size_t source_vertex_count,
	const MissionGpuSection& section,
	const ModelRenderVertex* source_vertices,
	const glm::mat4& object_to_camera,
	const retail_clip::Frustum& frustum,
	const glm::vec4* local_clip_plane,
	ModelRenderVertex* output_vertices,
	std::uint32_t* output_indices,
	const ClippedSectionCounts& expected)
{
	if (source_vertices == nullptr || output_vertices == nullptr)
	{
		return false;
	}
	const std::uint32_t vertices_per_face = section.lines ? 2u : 3u;
	const std::uint64_t section_end =
		static_cast<std::uint64_t>(section.first_index)
			+ section.index_count;
	if (section_end > source_indices.size())
	{
		return false;
	}
	std::uint32_t vertex_cursor = 0;
	std::uint32_t index_cursor = 0;
	std::array<retail_clip::Vertex, 3> face;
	for (std::uint32_t index = section.first_index;
		index + vertices_per_face <= section_end;
		index += vertices_per_face)
	{
		bool valid = true;
		for (std::uint32_t corner = 0;
			corner < vertices_per_face;
			++corner)
		{
			const std::uint16_t source_index =
				source_indices[index + corner];
			if (source_index >= source_vertex_count)
			{
				valid = false;
				break;
			}
			face[corner] = model_clip_vertex(
				source_vertices[source_index],
				object_to_camera);
		}
		if (!valid)
		{
			continue;
		}
		retail_clip::Polygon polygon;
		if (!polygon.clip(
				std::span<const retail_clip::Vertex>{
					face.data(), vertices_per_face},
				frustum)
			|| (local_clip_plane != nullptr
				&& !polygon.clip_local_half_space(
					*local_clip_plane)))
		{
			continue;
		}
		const std::uint32_t polygon_vertices = polygon.size();
		if (vertex_cursor > expected.vertices
			|| polygon_vertices
				> expected.vertices - vertex_cursor)
		{
			return false;
		}
		const std::uint32_t base_vertex = vertex_cursor;
		for (std::uint32_t corner = 0;
			corner < polygon_vertices;
			++corner)
		{
			output_vertices[vertex_cursor++] =
				model_render_vertex(polygon.vertex(corner));
		}
		if (output_indices == nullptr)
		{
			continue;
		}
		if (section.lines)
		{
			if (index_cursor > expected.indices
				|| 2u > expected.indices - index_cursor)
			{
				return false;
			}
			output_indices[index_cursor++] = base_vertex;
			output_indices[index_cursor++] = base_vertex + 1u;
		}
		else
		{
			for (std::uint32_t corner = 1;
				corner + 1 < polygon_vertices;
				++corner)
			{
				if (index_cursor > expected.indices
					|| 3u > expected.indices - index_cursor)
				{
					return false;
				}
				output_indices[index_cursor++] = base_vertex;
				output_indices[index_cursor++] =
					base_vertex + corner;
				output_indices[index_cursor++] =
					base_vertex + corner + 1u;
			}
		}
	}
	return vertex_cursor == expected.vertices
		&& (output_indices == nullptr
			|| index_cursor == expected.indices);
}

void set_camera_relative_transform(
	const glm::mat4& world_transform,
	const glm::vec3& camera_position)
{
	const glm::mat4 render_transform =
		math::camera_relative_transform(world_transform, camera_position);
	bgfx::setTransform(glm::value_ptr(render_transform));
}

struct EnvironmentVertex
{
	float x;
	float y;
	float z;
	float r;
	float g;
	float b;
	float a;
	float u;
	float v;
};
static_assert(sizeof(EnvironmentVertex) == sizeof(float) * 9);

std::uint16_t environment_rand15(std::uint32_t& seed)
{
	seed = seed * 0x343fdu + 0x269ec3u;
	return static_cast<std::uint16_t>((seed >> 16) & 0x7fffu);
}

float environment_random_unit(std::uint32_t& seed)
{
	return static_cast<float>(environment_rand15(seed))
		* (1.0f / 32767.0f);
}

bool initialize_model_modifier_textures(FrontendRenderer& renderer)
{
	constexpr float kShapes[4] = {1.0f, 2.0f, 5.0f, 10.0f};
	for (std::uint32_t texture_index = 0;
		texture_index < std::size(renderer.model_modifier_textures);
		++texture_index)
	{
		assets::TextureImage image;
		image.width = 64;
		image.height = 64;
		if (!image.pixels.allocate(image.width * image.height * 4u))
		{
			return false;
		}
		const float shape =
			kShapes[texture_index & 3u] + 0.009999999776482582f;
		const float radial_scale =
			std::pow(shape + 1.0f, 1.0f / shape);
		for (std::int32_t y = 0; y < 64; ++y)
		{
			const float vertical =
				static_cast<float>(y * 2 - 64) / 64.0f;
			for (std::int32_t x = 0; x < 64; ++x)
			{
				const float horizontal =
					static_cast<float>(x * 2 - 64) / 64.0f;
				const float radius_squared =
					horizontal * horizontal
					+ vertical * vertical;
				float radial = 0.4000000059604645f;
				if (radius_squared <= 1.0f)
				{
					radial =
						std::pow(
							(1.0f - std::sqrt(radius_squared))
								* radial_scale,
							shape)
						+ 0.4000000059604645f;
				}
				std::int32_t intensity = static_cast<std::int32_t>(
					std::lrint(
						(radial / 1.4000000059604645f)
							* 200.0f));
				if (texture_index > 3)
				{
					intensity = intensity * 7 / 10;
				}
				// srd3d_create_modifier_map
				// (srd3d.dll 0x10001620) stores
				// clamp((intensity - 200) / 3) in RGB and
				// clamp(intensity * 3 / 2) in alpha. Integer division
				// truncates toward zero in both expressions.
				const std::uint8_t color =
					static_cast<std::uint8_t>(
						std::clamp(
							(intensity - 200) / 3,
							0,
							255));
				const std::uint8_t alpha =
					static_cast<std::uint8_t>(
						std::clamp(
							intensity * 3 / 2,
							0,
							255));
				std::uint8_t* pixel =
					image.pixels.data
					+ (static_cast<std::size_t>(y) * 64u
						+ static_cast<std::size_t>(x)) * 4u;
				pixel[0] = color;
				pixel[1] = color;
				pixel[2] = color;
				pixel[3] = alpha;
			}
		}
		if (!mission_texture_upload(
				renderer.model_modifier_textures[texture_index],
				image))
		{
			return false;
		}
	}
	return true;
}

void destroy_environment_mesh(MissionEnvironmentMesh& mesh)
{
	if (bgfx::isValid(mesh.vertices))
	{
		bgfx::destroy(mesh.vertices);
	}
	if (bgfx::isValid(mesh.indices))
	{
		bgfx::destroy(mesh.indices);
	}
	mesh = {};
}

bool upload_environment_mesh(
	MissionEnvironmentMesh& mesh,
	const bgfx::VertexLayout& layout,
	const std::vector<EnvironmentVertex>& vertices,
	const std::vector<std::uint16_t>& indices)
{
	destroy_environment_mesh(mesh);
	if (vertices.empty() || indices.empty())
	{
		return false;
	}
	mesh.vertices = bgfx::createVertexBuffer(
		bgfx::copy(
			vertices.data(),
			static_cast<std::uint32_t>(
				vertices.size() * sizeof(EnvironmentVertex))),
		layout);
	mesh.indices = bgfx::createIndexBuffer(
		bgfx::copy(
			indices.data(),
			static_cast<std::uint32_t>(
				indices.size() * sizeof(std::uint16_t))));
	mesh.index_count = static_cast<std::uint32_t>(indices.size());
	return bgfx::isValid(mesh.vertices) && bgfx::isValid(mesh.indices);
}

bool build_accelerated_nebula_dome(
	const assets::TextureImage& source,
	MissionEnvironmentRenderer& environment)
{
	if (source.width != 256 || source.height != 256)
	{
		return false;
	}
	constexpr std::uint32_t kColumns = 15;
	constexpr std::uint32_t kRows = 8;
	std::vector<EnvironmentVertex> vertices;
	vertices.reserve(kColumns * kRows);
	for (std::uint32_t row = 0; row < kRows; ++row)
	{
		const float y = static_cast<float>(row) / (kRows - 1);
		for (std::uint32_t column = 0; column < kColumns; ++column)
		{
			const float x =
				static_cast<float>(column) / (kColumns - 1);
			const std::uint32_t sample_x =
				static_cast<std::uint32_t>(
					std::trunc(x * 255.0f));
			const std::uint32_t sample_y =
				static_cast<std::uint32_t>(
					std::trunc(y * 255.0f));
			const std::uint8_t* pixel =
				source.pixels.data
				+ (sample_y * 256u + sample_x) * 4u;
			glm::vec3 direction{
				std::sin(x * glm::two_pi<float>()),
				(y - 0.5f) * 5.0f,
				std::cos(x * glm::two_pi<float>()),
			};
			direction = glm::normalize(direction) * 5000.0f;
			vertices.push_back({
				direction.x,
				direction.y,
				direction.z,
				static_cast<float>(pixel[0]) * (1.0f / 256.0f),
				static_cast<float>(pixel[1]) * (1.0f / 256.0f),
				static_cast<float>(pixel[2]) * (1.0f / 256.0f),
				1.0f,
				0.0f,
				0.0f,
			});
		}
	}
	std::vector<std::uint16_t> indices;
	indices.reserve((kColumns - 1) * (kRows - 1) * 6);
	for (std::uint16_t row = 0; row < kRows - 1; ++row)
	{
		for (std::uint16_t column = 0;
			column < kColumns - 1;
			++column)
		{
			const std::uint16_t upper =
				static_cast<std::uint16_t>(row * kColumns + column);
			const std::uint16_t lower =
				static_cast<std::uint16_t>(upper + kColumns);
			indices.push_back(upper);
			indices.push_back(lower);
			indices.push_back(static_cast<std::uint16_t>(upper + 1));
			indices.push_back(lower);
			indices.push_back(static_cast<std::uint16_t>(upper + 1));
			indices.push_back(static_cast<std::uint16_t>(lower + 1));
		}
	}
	return upload_environment_mesh(
		environment.nebula_dome,
		environment.layout,
		vertices,
		indices);
}

bool build_nebula_grid(
	MissionEnvironmentMesh& output,
	const bgfx::VertexLayout& layout,
	float angle_minimum,
	float angle_maximum)
{
	constexpr std::uint16_t kColumns = 10;
	constexpr std::uint16_t kRows = 10;
	std::vector<EnvironmentVertex> vertices;
	vertices.reserve((kColumns + 1) * (kRows + 1));
	for (std::uint16_t row = 0; row <= kRows; ++row)
	{
		const float vertical =
			angle_minimum
			+ (angle_maximum - angle_minimum)
				* static_cast<float>(row) / kRows;
		for (std::uint16_t column = 0;
			column <= kColumns;
			++column)
		{
			const float horizontal =
				angle_minimum
				+ (angle_maximum - angle_minimum)
					* static_cast<float>(column) / kColumns;
			glm::mat3 rotation{1.0f};
			rotation = math::postrotate(
				rotation, vertical, {0.0f, 1.0f, 0.0f});
			rotation = math::postrotate(
				rotation, horizontal, {1.0f, 0.0f, 0.0f});
			const glm::vec3 position =
				rotation * glm::vec3{0.0f, 0.0f, 5000.0f};
			vertices.push_back({
				position.x,
				position.y,
				position.z,
				1.0f,
				1.0f,
				1.0f,
				1.0f,
				static_cast<float>(column) / kColumns,
				static_cast<float>(row) / kRows,
			});
		}
	}
	std::vector<std::uint16_t> indices;
	indices.reserve(kColumns * kRows * 6);
	for (std::uint16_t row = 0; row < kRows; ++row)
	{
		for (std::uint16_t column = 0; column < kColumns; ++column)
		{
			const std::uint16_t upper =
				static_cast<std::uint16_t>(
					row * (kColumns + 1) + column);
			const std::uint16_t lower =
				static_cast<std::uint16_t>(upper + kColumns + 1);
			indices.push_back(upper);
			indices.push_back(static_cast<std::uint16_t>(lower + 1));
			indices.push_back(static_cast<std::uint16_t>(upper + 1));
			indices.push_back(upper);
			indices.push_back(lower);
			indices.push_back(static_cast<std::uint16_t>(lower + 1));
		}
	}
	return upload_environment_mesh(output, layout, vertices, indices);
}

bool build_planet_atmosphere_mesh(
	MissionEnvironmentMesh& mesh,
	const bgfx::VertexLayout& layout,
	float inner_radius,
	float outer_radius)
{
	// Effect_cylinder_wall_mesh_data_create (LANCER.EXE 0x0044f200)
	// creates twenty wrapped segments. Planet construction collapses its
	// height to zero and scales one ring to exactly 0.85, producing the
	// camera-facing annulus owned by each atmosphere record.
	constexpr std::uint16_t kSegments = 20;
	std::vector<EnvironmentVertex> vertices;
	std::vector<std::uint16_t> indices;
	vertices.reserve(kSegments * 4);
	indices.reserve(kSegments * 6);
	for (std::uint16_t segment = 0; segment < kSegments; ++segment)
	{
		const float a0 =
			glm::two_pi<float>() * static_cast<float>(segment)
			/ static_cast<float>(kSegments);
		const float a1 =
			glm::two_pi<float>() * static_cast<float>(segment + 1)
			/ static_cast<float>(kSegments);
		const glm::vec2 d0{std::sin(a0), std::cos(a0)};
		const glm::vec2 d1{std::sin(a1), std::cos(a1)};
		const std::uint16_t base =
			static_cast<std::uint16_t>(vertices.size());
		vertices.push_back({
			d0.x * inner_radius, d0.y * inner_radius, 0.0f,
			1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f});
		vertices.push_back({
			d1.x * inner_radius, d1.y * inner_radius, 0.0f,
			1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
		vertices.push_back({
			d0.x * outer_radius, d0.y * outer_radius, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
		vertices.push_back({
			d1.x * outer_radius, d1.y * outer_radius, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f});
		indices.push_back(base);
		indices.push_back(static_cast<std::uint16_t>(base + 1));
		indices.push_back(static_cast<std::uint16_t>(base + 2));
		indices.push_back(static_cast<std::uint16_t>(base + 1));
		indices.push_back(static_cast<std::uint16_t>(base + 3));
		indices.push_back(static_cast<std::uint16_t>(base + 2));
	}
	return upload_environment_mesh(
		mesh,
		layout,
		vertices,
		indices);
}

bool build_engine_flare_mesh(
	MissionEnvironmentMesh& mesh,
	const bgfx::VertexLayout& layout)
{
	// EngineFlare_build_mesh (LANCER.EXE 0x00469400): one square at the
	// nozzle followed by three longitudinal fins separated by pi/3.
	std::vector<EnvironmentVertex> vertices;
	std::vector<std::uint16_t> indices;
	vertices.reserve(16);
	indices.reserve(24);
	const auto append_quad = [&](
		const glm::vec3& p0,
		const glm::vec3& p1,
		const glm::vec3& p2,
		const glm::vec3& p3)
	{
		const std::uint16_t base =
			static_cast<std::uint16_t>(vertices.size());
		constexpr glm::vec2 uv[4] = {
			{0.9900000095367432f, 0.9900000095367432f},
			{0.9900000095367432f, 0.03999999910593033f},
			{0.03999999910593033f, 0.03999999910593033f},
			{0.03999999910593033f, 0.9900000095367432f},
		};
		const glm::vec3 points[4] = {p0, p1, p2, p3};
		for (std::uint32_t index = 0; index < 4; ++index)
		{
			vertices.push_back({
				points[index].x,
				points[index].y,
				points[index].z,
				0.9900000095367432f,
				0.9900000095367432f,
				0.9900000095367432f,
				0.9900000095367432f,
				uv[index].x,
				uv[index].y,
			});
		}
		indices.push_back(base);
		indices.push_back(static_cast<std::uint16_t>(base + 1));
		indices.push_back(static_cast<std::uint16_t>(base + 2));
		indices.push_back(base);
		indices.push_back(static_cast<std::uint16_t>(base + 2));
		indices.push_back(static_cast<std::uint16_t>(base + 3));
	};
	append_quad(
		{-1.0f, -1.0f, 0.0f},
		{1.0f, -1.0f, 0.0f},
		{1.0f, 1.0f, 0.0f},
		{-1.0f, 1.0f, 0.0f});
	for (std::uint32_t fin = 0; fin < 3; ++fin)
	{
		const float angle =
			static_cast<float>(fin) * glm::pi<float>() / 3.0f;
		const float sine = std::sin(angle);
		const float cosine = std::cos(angle);
		append_quad(
			{-sine, -cosine, 0.0f},
			{-sine, -cosine, 1.0f},
			{sine, cosine, 1.0f},
			{sine, cosine, 0.0f});
	}
	return upload_environment_mesh(mesh, layout, vertices, indices);
}

constexpr float kPlanetBombardRectanglePixels[32] = {
	214.0f, 36.0f, 236.0f, 60.0f,
	214.0f, 0.0f, 248.0f, 34.0f,
	92.0f, 196.0f, 138.0f, 242.0f,
	170.0f, 112.0f, 234.0f, 178.0f,
	92.0f, 116.0f, 168.0f, 194.0f,
	0.0f, 116.0f, 90.0f, 212.0f,
	112.0f, 0.0f, 212.0f, 110.0f,
	0.0f, 0.0f, 110.0f, 114.0f,
};
float g_planet_bombard_rectangles[32]{};
float g_planet_bombard_scale[16]{};
constexpr std::int16_t kPlanetBombardFrameMap[16] = {
	0, 1, 2, 3, 4, 5, 6, 7,
	7, 7, 7, 7, 7, 7, 7, 7,
};

void destroy_environment_renderer(MissionEnvironmentRenderer& environment)
{
	destroy_environment_mesh(environment.nebula_dome);
	destroy_environment_mesh(environment.nebula_grid_broad);
	destroy_environment_mesh(environment.nebula_grid_narrow);
	destroy_environment_mesh(environment.planet_atmosphere);
	destroy_environment_mesh(environment.planet_atmosphere_compact);
	for (FrontendTexture& texture : environment.nebula_materials)
	{
		frontend_texture_shutdown(texture);
	}
	frontend_texture_shutdown(environment.planet_atmosphere_texture);
	frontend_texture_shutdown(environment.far_asteroid_texture);
	frontend_texture_shutdown(environment.planet_bombard_texture);
	for (FrontendTexture& texture : environment.sun_layers)
	{
		frontend_texture_shutdown(texture);
	}
	for (FrontendTexture& texture : environment.sun_flares)
	{
		frontend_texture_shutdown(texture);
	}
	environment.initialized = false;
	environment.atmosphere_tick_valid = false;
	environment.previous_sun_edge_distance = 0.0f;
	environment.sun_layer_three_brightness = 0.0f;
	// Environment_effects_shutdown destroys the ten Boom Mesh objects but
	// does not clear their retained active bytes. Do not value-initialize the
	// bombard records here; the next initialization replaces their resource
	// state and schedule while preserving that byte.
}

bool initialize_planet_bombard(
	MissionEnvironmentRenderer& environment,
	std::uint32_t& random_seed,
	std::uint32_t tick)
{
	for (std::size_t index = 0;
		index < std::size(g_planet_bombard_rectangles);
		++index)
	{
		g_planet_bombard_rectangles[index] =
			(kPlanetBombardRectanglePixels[index] + 0.5f)
			* (1.0f / 256.0f);
	}
	const float inverse_base_width =
		1.0f
		/ (g_planet_bombard_rectangles[2]
			- g_planet_bombard_rectangles[0]);
	const float inverse_base_height =
		1.0f
		/ (g_planet_bombard_rectangles[3]
			- g_planet_bombard_rectangles[1]);
	for (std::uint32_t rectangle = 0; rectangle < 8; ++rectangle)
	{
		const float* source =
			g_planet_bombard_rectangles + rectangle * 4;
		g_planet_bombard_scale[rectangle * 2] =
			(source[2] - source[0]) * inverse_base_width;
		g_planet_bombard_scale[rectangle * 2 + 1] =
			(source[3] - source[1]) * inverse_base_height;
	}

	for (MissionPlanetBombardSlot& slot : environment.bombard)
	{
		const bool retained_active = slot.active;
		if (environment_rand15(random_seed) % 10u == 0)
		{
			slot.square_size = 25000.0f;
		}
		else
		{
			slot.square_size =
				3000.0f + environment_random_unit(random_seed) * 15000.0f;
		}
		slot.local_position = {0.0f, 0.0f, 0.0f};
		slot.local_orientation = glm::mat3{1.0f};
		const float x0 = g_planet_bombard_rectangles[0];
		const float y0 = g_planet_bombard_rectangles[1];
		const float x1 = g_planet_bombard_rectangles[2];
		const float y1 = g_planet_bombard_rectangles[3];
		slot.uv[0] = {x0, y1};
		slot.uv[1] = {x1, y1};
		slot.uv[2] = {x0, y0};
		slot.uv[3] = {x1, y1};
		slot.uv[4] = {x1, y0};
		slot.uv[5] = {x0, y0};
		slot.scale = 1.0f;
		slot.brightness = 0.0f;
		slot.scheduled_or_start_tick =
			static_cast<std::int32_t>(tick);
		slot.active = retained_active;
	}
	return true;
}

void initialize_far_asteroids(
	MissionEnvironmentRenderer& environment,
	std::uint8_t graphics_quality,
	std::uint32_t& random_seed,
	std::uint32_t prior_count)
{
	switch (graphics_quality)
	{
	case 0: environment.far_asteroid_count = 200; break;
	case 1: environment.far_asteroid_count = 500; break;
	case 2: environment.far_asteroid_count = 800; break;
	default:
		environment.far_asteroid_count = std::min(
			prior_count, kMissionFarAsteroidCapacity);
		break;
	}
	constexpr float kThresholds[] = {
		0.2f, 0.4f, 0.6f, 0.8f, 1.0f,
	};
	for (std::uint32_t index = 0;
		index < environment.far_asteroid_count;
		++index)
	{
		MissionFarAsteroid& asteroid =
			environment.far_asteroids[index];
		const float uv_trial = environment_random_unit(random_seed);
		for (std::uint8_t band = 0; band < std::size(kThresholds); ++band)
		{
			if (uv_trial < kThresholds[band])
			{
				asteroid.uv_band = band;
				break;
			}
		}
		asteroid.roll_rate =
			static_cast<float>(
				static_cast<std::int32_t>(index % 7u) - 3)
			* 0.0005f;
		asteroid.brightness =
			(environment_random_unit(random_seed) + 1.0f) * 0.5f;

		const float source_radius =
			2000.0f + (1.0f - asteroid.brightness) * 1000.0f;
		const float x_range =
			environment_random_unit(random_seed) >= 0.65f
				? 0.7f
				: 0.1f;
		const float x_angle =
			(environment_random_unit(random_seed) - 0.5f) * x_range;
		const float y_base =
			environment_random_unit(random_seed) >= 0.5f
				? glm::pi<float>()
				: 0.0f;
		const float y_angle =
			environment_random_unit(random_seed)
				* (0.7f * glm::pi<float>())
			+ y_base;
		glm::mat3 rotation{1.0f};
		rotation = math::postrotate(
			rotation, x_angle, {1.0f, 0.0f, 0.0f});
		rotation = math::postrotate(
			rotation, y_angle, {0.0f, 1.0f, 0.0f});
		asteroid.camera_offset =
			glm::normalize(
				rotation
				* glm::vec3{source_radius, 0.0f, 0.0f})
			* 2500.0f;

		if (environment_random_unit(random_seed) >= 0.06f)
		{
			asteroid.scale =
				5.0f + environment_random_unit(random_seed) * 25.0f;
		}
		else
		{
			asteroid.scale =
				30.0f + environment_random_unit(random_seed) * 70.0f;
		}
	}
}

bool initialize_environment_renderer(
	io::Vfs& vfs,
	const assets::TextureCache& texture_cache,
	MissionEnvironmentRenderer& environment,
	std::uint8_t graphics_quality,
	std::uint32_t& random_seed,
	std::uint32_t tick)
{
	const std::uint32_t prior_far_count =
		environment.far_asteroid_count;
	destroy_environment_renderer(environment);
	environment.layout
		.begin()
		.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
		.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
		.end();

	assets::TextureImage star_reference;
	if (!assets::load_tga(
			vfs, "nebula/starref12.tga", star_reference)
		|| !build_accelerated_nebula_dome(
			star_reference, environment)
		|| !build_nebula_grid(
			environment.nebula_grid_broad,
			environment.layout,
			-glm::quarter_pi<float>(),
			glm::quarter_pi<float>())
		|| !build_nebula_grid(
			environment.nebula_grid_narrow,
			environment.layout,
			-glm::pi<float>() / 5.0f,
			glm::pi<float>() / 5.0f)
		|| !build_planet_atmosphere_mesh(
			environment.planet_atmosphere,
			environment.layout,
			0.8500000238418579f,
			1.0049999952316284f)
		|| !build_planet_atmosphere_mesh(
			environment.planet_atmosphere_compact,
			environment.layout,
			0.8500000238418579f,
			0.9499999880790710f))
	{
		destroy_environment_renderer(environment);
		return false;
	}

	constexpr const char* kNebulaMaterials[7] = {
		"neb01", "neb02", "neb03", "neb04", "neb05", "neb06", "neb07",
	};
	for (std::uint32_t index = 0; index < 7; ++index)
	{
		assets::TextureImage image;
		if (!assets::texture_cache_decode(
				texture_cache, kNebulaMaterials[index], image)
			|| !mission_texture_upload(
				environment.nebula_materials[index], image))
		{
			destroy_environment_renderer(environment);
			return false;
		}
	}
	assets::TextureImage far_asteroid;
	assets::TextureImage planet_bombard;
	assets::TextureImage planet_atmosphere;
	if (!assets::texture_cache_decode(
			texture_cache, "atmos", planet_atmosphere)
		|| !mission_texture_upload(
			environment.planet_atmosphere_texture,
			planet_atmosphere)
		|| !assets::texture_cache_decode(
			texture_cache, "farast2", far_asteroid)
		|| !mission_texture_upload(
			environment.far_asteroid_texture, far_asteroid)
		|| !assets::texture_cache_decode(
			texture_cache, "pbang", planet_bombard)
		|| !mission_texture_upload(
			environment.planet_bombard_texture, planet_bombard))
	{
		destroy_environment_renderer(environment);
		return false;
	}
	constexpr const char* kSunLayers[3] = {
		"sunlayer1", "sunlayer2", "sunlayer3",
	};
	constexpr const char* kSunFlares[4] = {
		"sunflare1", "sunflare2", "sunflare3", "sunflare4",
	};
	for (std::uint32_t index = 0; index < std::size(kSunLayers); ++index)
	{
		assets::TextureImage image;
		if (!assets::texture_cache_decode(
				texture_cache, kSunLayers[index], image)
			|| !mission_texture_upload(
				environment.sun_layers[index], image))
		{
			destroy_environment_renderer(environment);
			return false;
		}
	}
	for (std::uint32_t index = 0; index < std::size(kSunFlares); ++index)
	{
		assets::TextureImage image;
		if (!assets::texture_cache_decode(
				texture_cache, kSunFlares[index], image)
			|| !mission_texture_upload(
				environment.sun_flares[index], image))
		{
			destroy_environment_renderer(environment);
			return false;
		}
	}
	initialize_far_asteroids(
		environment,
		graphics_quality,
		random_seed,
		prior_far_count);
	environment.last_bombard_callback_tick = tick;
	environment.last_atmosphere_tick = tick;
	environment.atmosphere_tick_valid = true;
	environment.previous_sun_edge_distance = 0.0f;
	environment.sun_layer_three_brightness = 0.0f;
	environment.initialized = true;
	diagnostics::mission_log(
		"environment initialized far_asteroids=%u quality=%u bombard_slots=%u",
		environment.far_asteroid_count,
		static_cast<unsigned>(graphics_quality),
		static_cast<unsigned>(kMissionPlanetBombardSlots));
	return true;
}

bool upload_starfield(
	io::Vfs& vfs,
	MissionRenderer& renderer,
	std::uint32_t& random_seed)
{
	assets::TextureImage atlas;
	if (!assets::load_tga(vfs, "space.tga", atlas)
		|| atlas.width != 360 || atlas.height != 360)
	{
		return false;
	}
	renderer.star_count = 0;
	constexpr float kDegrees = glm::pi<float>() / 180.0f;
	for (std::uint32_t tile_y = 0; tile_y < 10; ++tile_y)
	{
		const float polar = (9.0f + tile_y * 18.0f) * kDegrees;
		for (std::uint32_t tile_x = 0; tile_x < 10; ++tile_x)
		{
			MissionStarTile& tile =
				renderer.star_tiles[tile_y * 10 + tile_x];
			tile.first = static_cast<std::uint16_t>(
				renderer.star_count);
			const float heading = (9.0f + tile_x * 18.0f) * kDegrees;
			const glm::vec3 forward{
				std::cos(heading) * std::sin(polar),
				std::cos(polar),
				std::sin(heading) * std::sin(polar),
			};
			const glm::vec3 right = glm::normalize(
				glm::cross(glm::vec3{0.0f, 1.0f, 0.0f}, forward));
			const glm::vec3 up = glm::normalize(glm::cross(forward, right));
			tile.orientation =
				glm::mat3{right, up, glm::normalize(forward)};
			for (std::uint32_t row = 0; row < 36; ++row)
			{
				for (std::uint32_t column = 0; column < 36; ++column)
				{
					const std::uint32_t image_x = tile_x * 36 + column;
					const std::uint32_t image_y = tile_y * 36 + row;
					const std::uint8_t* pixel =
						atlas.pixels.data
						+ (static_cast<std::size_t>(image_y)
								* atlas.width
							+ image_x)
							* 4;
					if ((pixel[0] | pixel[1] | pixel[2]) == 0)
					{
						continue;
					}
					const glm::vec3 source{
						std::sin(
							(static_cast<float>(row) - 18.0f)
								* 0.5f * kDegrees),
						std::sin(
							(static_cast<float>(column) - 18.0f)
								* 0.5f * kDegrees),
						1.0f,
					};
					// Space.tga intentionally retains the retail BGR-to-RGB
					// channel swap documented by the original scene builder.
					const std::uint32_t color =
						0xff000000u
						| static_cast<std::uint32_t>(pixel[0]) << 16
						| static_cast<std::uint32_t>(pixel[1]) << 8
						| pixel[2];
					if (renderer.star_count >= kMissionStarCount)
					{
						return false;
					}
					renderer.stars[renderer.star_count++] = {
						source,
						color,
					};
				}
			}
			tile.count = static_cast<std::uint16_t>(
				renderer.star_count - tile.first);
		}
	}
	for (MissionStar& star : renderer.random_stars)
	{
		star.source = {
			environment_random_unit(random_seed) * 8191.0f,
			environment_random_unit(random_seed) * 8191.0f,
			environment_random_unit(random_seed) * 8191.0f,
		};
		star.color = 0xff808080u;
	}
	renderer.star_sample_valid = false;
	return renderer.star_count == kMissionStarCount;
}

bool initialize_engine_flares(
	const assets::TextureCache& texture_cache,
	MissionRenderer& renderer)
{
	for (std::uint32_t index = 0;
		index < std::size(renderer.engine_flares);
		++index)
	{
		MissionEngineFlare& flare = renderer.engine_flares[index];
		if (!build_engine_flare_mesh(
				flare.mesh, renderer.environment.layout))
		{
			return false;
		}
		char material_a[16];
		char material_b[16];
		std::snprintf(
			material_a, sizeof(material_a), "matflarea%u", index + 1);
		std::snprintf(
			material_b, sizeof(material_b), "matflareb%u", index + 1);
		assets::TextureImage image_a;
		assets::TextureImage image_b;
		if (!assets::texture_cache_decode(
				texture_cache, material_a, image_a)
			|| !assets::texture_cache_decode(
				texture_cache, material_b, image_b)
			|| !mission_texture_upload(flare.material_a, image_a)
			|| !mission_texture_upload(flare.material_b, image_b))
		{
			return false;
		}
	}
	return true;
}

bool clip_star_endpoint(
	glm::vec2& endpoint,
	const glm::vec2& other,
	float boundary,
	std::uint32_t axis)
{
	const float denominator = other[axis] - endpoint[axis];
	if (denominator == 0.0f)
	{
		return false;
	}
	const float interpolation =
		(boundary - endpoint[axis]) / denominator;
	endpoint[1 - axis] +=
		(other[1 - axis] - endpoint[1 - axis]) * interpolation;
	endpoint[axis] = boundary;
	return true;
}

bool clip_star_segment(
	glm::vec2& current,
	glm::vec2& previous,
	const glm::vec2& minimum,
	const glm::vec2& maximum)
{
	for (std::uint32_t axis = 0; axis < 2; ++axis)
	{
		if (current[axis] < minimum[axis]
			&& previous[axis] < minimum[axis])
		{
			return false;
		}
		if (current[axis] < minimum[axis])
		{
			clip_star_endpoint(
				current, previous, minimum[axis], axis);
		}
		else if (previous[axis] < minimum[axis])
		{
			clip_star_endpoint(
				previous, current, minimum[axis], axis);
		}
		if (current[axis] > maximum[axis]
			&& previous[axis] > maximum[axis])
		{
			return false;
		}
		if (current[axis] > maximum[axis])
		{
			clip_star_endpoint(
				current, previous, maximum[axis], axis);
		}
		else if (previous[axis] > maximum[axis])
		{
			clip_star_endpoint(
				previous, current, maximum[axis], axis);
		}
	}
	return true;
}

float wrap_random_star_component(float value)
{
	// Star_preprocess mode one (LANCER.EXE 0x004c5380) rounds to an integer,
	// applies the authored 0x1fff mask, then recenters by integer half-range.
	const std::int32_t rounded = static_cast<std::int32_t>(
		std::lrint(value));
	return static_cast<float>(
		static_cast<std::int32_t>(
			static_cast<std::uint32_t>(rounded) & 0x1fffu)
		- 0x0fff);
}

bool clip_random_star_near(
	glm::vec3& current,
	glm::vec3& previous)
{
	if (current.z < kMissionNearPlane
		&& previous.z < kMissionNearPlane)
	{
		return false;
	}
	const auto clip = [](glm::vec3& endpoint, const glm::vec3& other)
	{
		const float interpolation =
			(kMissionNearPlane - endpoint.z)
			/ (other.z - endpoint.z);
		endpoint += (other - endpoint) * interpolation;
		endpoint.z = kMissionNearPlane;
	};
	if (current.z < kMissionNearPlane)
	{
		clip(current, previous);
	}
	else if (previous.z < kMissionNearPlane)
	{
		clip(previous, current);
	}
	return true;
}

std::uint32_t scale_star_color(
	std::uint32_t color,
	float intensity,
	bool half)
{
	const float scale = intensity * (half ? 127.5f : 255.0f);
	const std::uint8_t red = static_cast<std::uint8_t>(
		std::clamp(std::lrint(scale * (color & 0xff) / 255.0f),
			0l, 255l));
	const std::uint8_t green = static_cast<std::uint8_t>(
		std::clamp(std::lrint(scale * ((color >> 8) & 0xff) / 255.0f),
			0l, 255l));
	const std::uint8_t blue = static_cast<std::uint8_t>(
		std::clamp(std::lrint(scale * ((color >> 16) & 0xff) / 255.0f),
			0l, 255l));
	// bgfx consumes the normalized Uint8 attribute in ABGR byte order.
	return 0xff000000u
		| static_cast<std::uint32_t>(blue) << 16
		| static_cast<std::uint32_t>(green) << 8
		| red;
}

struct VisibleStar
{
	glm::vec2 current{0.0f};
	glm::vec2 previous{0.0f};
	std::uint32_t color{};
	std::uint32_t half_color{};
	bool point{};
};

void destroy_lod(MissionGpuLod& lod)
{
	if (bgfx::isValid(lod.lit_vertices))
	{
		bgfx::destroy(lod.lit_vertices);
	}
	if (bgfx::isValid(lod.indices))
	{
		bgfx::destroy(lod.indices);
	}
	if (bgfx::isValid(lod.cloak_indices))
	{
		bgfx::destroy(lod.cloak_indices);
	}
	lod = {};
}

void destroy_model(MissionGpuModel& model)
{
	for (MissionGpuNode& node : model.nodes)
	{
		for (MissionGpuLod& lod : node.lods)
		{
			destroy_lod(lod);
		}
	}
	for (FrontendTexture& texture : model.textures)
	{
		frontend_texture_shutdown(texture);
	}
	for (FrontendTexture& texture : model.alternate_textures)
	{
		frontend_texture_shutdown(texture);
	}
	model = {};
}

bool build_node_transform(
	const assets::GameplayModel& source,
	std::uint32_t index,
	std::uint8_t* state,
	glm::mat4* transforms)
{
	if (state[index] == 2)
	{
		return true;
	}
	if (state[index] == 1)
	{
		return false;
	}
	state[index] = 1;
	const assets::GameplayNode& node = source.nodes[index];
	glm::vec3 local_position = node.position - source.center_of_mass;
	if (node.parent >= 0)
	{
		const std::uint32_t parent =
			static_cast<std::uint32_t>(node.parent);
		if (!build_node_transform(source, parent, state, transforms))
		{
			return false;
		}
		// object_recompute_model_transform_after_sequence
		// (LANCER.EXE 0x0049a140) uses the serialized parent's
		// exported_position as the static anchor. At zero animation the
		// parent chain telescopes to exported_position - center_of_mass;
		// rest_translation/local_basis are joint-pivot data and cancel.
		local_position = node.position - source.nodes[parent].position;
	}
	transforms[index] =
		(node.parent >= 0 ? transforms[node.parent] : glm::mat4{1.0f})
		* glm::translate(glm::mat4{1.0f}, local_position);
	state[index] = 2;
	return true;
}

void accumulate_mesh_extent(
	const MissionGpuNode& node,
	const glm::mat4& transform,
	float& radius,
	bool& have_bounds,
	glm::vec3& bounds_min,
	glm::vec3& bounds_max)
{
	if (node.lods.empty())
	{
		return;
	}
	for (const assets::GameplayVertex& vertex
		: node.lods[0].source_vertices)
	{
		const glm::vec3 point = glm::vec3(
			transform * glm::vec4{
				vertex.x, vertex.y, vertex.z, 1.0f});
		radius = std::max(radius, glm::length(point));
		if (!have_bounds)
		{
			bounds_min = point;
			bounds_max = point;
			have_bounds = true;
		}
		else
		{
			bounds_min = glm::min(bounds_min, point);
			bounds_max = glm::max(bounds_max, point);
		}
	}
}

bool upload_model(
	const assets::GameplayModel& source,
	MissionGpuModel& output,
	const bgfx::VertexLayout& lit_model_layout)
{
	destroy_model(output);
	output.radius = 0.0f;
	output.root_flags = source.root_flags;
	output.center_of_mass = source.center_of_mass;
	output.inertia_tensor = source.inertia_tensor;
	output.camera_offset = source.camera_offset;
	output.total_mass = source.total_mass;
	bool have_bounds = false;
	output.textures.resize(source.materials.size());
	output.alternate_textures.resize(source.materials.size());
	output.alternate_texture_loaded.assign(
		source.materials.size(), false);
	for (std::uint32_t index = 0; index < source.materials.size(); ++index)
	{
		if (!mission_texture_upload(
				output.textures[index],
				source.materials[index].image))
		{
			diagnostics::mission_log(
				"model upload failed stage=texture material=%u",
				index);
			destroy_model(output);
			return false;
		}
		if (source.materials[index].has_alternate)
		{
			if (!mission_texture_upload(
					output.alternate_textures[index],
					source.materials[index].alternate_image))
			{
				diagnostics::mission_log(
					"model upload failed stage=alternate-texture material=%u",
					index);
				destroy_model(output);
				return false;
			}
			output.alternate_texture_loaded[index] = true;
		}
	}

	std::vector<std::uint8_t> state(source.nodes.size());
	std::vector<glm::mat4> transforms(source.nodes.size(), glm::mat4{1.0f});
	std::vector<std::uint32_t> subsystem_part_groups;
	for (const assets::GameplayNode& node : source.nodes)
	{
		if ((node.flags & 0x0002u) != 0
			&& std::find(
				subsystem_part_groups.begin(),
				subsystem_part_groups.end(),
				node.part_group_id) == subsystem_part_groups.end())
		{
			subsystem_part_groups.push_back(node.part_group_id);
		}
	}
	for (std::uint32_t index = 0; index < source.nodes.size(); ++index)
	{
		if (!build_node_transform(source, index, state.data(), transforms.data()))
		{
			diagnostics::mission_log(
				"model upload failed stage=node-transform node=%u",
				index);
			destroy_model(output);
			return false;
		}
	}

	if (source.nodes.size() > UINT16_MAX)
	{
		diagnostics::mission_log(
			"model upload failed stage=node-capacity nodes=%u",
			static_cast<unsigned>(source.nodes.size()));
		destroy_model(output);
		return false;
	}
	output.nodes.resize(source.nodes.size());
	output.node_indices_in_preorder.clear();
	output.node_indices_in_preorder.reserve(source.nodes.size());
	std::vector<std::vector<std::uint16_t>> children(source.nodes.size());
	std::vector<std::uint16_t> roots;
	for (std::uint32_t node_index = 0;
		node_index < source.nodes.size();
		++node_index)
	{
		// GameObject_instantiate_sro_model (LANCER.EXE 0x004760c0)
		// appends one live model for each serialized tag-1 record in source
		// order. GameObject_link_model_parents (0x00476130) links those
		// already-created records afterwards; it does not reorder them into
		// a tree walk. Script component indices and locator traversal use
		// this live array, so retain the serialized index exactly.
		output.nodes[node_index].runtime_model_index =
			static_cast<std::uint16_t>(node_index);
		const std::int32_t parent = source.nodes[node_index].parent;
		if (parent < 0)
		{
			roots.push_back(static_cast<std::uint16_t>(node_index));
		}
		else
		{
			children[static_cast<std::uint32_t>(parent)].push_back(
				static_cast<std::uint16_t>(node_index));
		}
	}
	std::function<void(std::uint16_t)> append_preorder =
		[&](std::uint16_t node_index)
		{
			output.node_indices_in_preorder.push_back(node_index);
			for (const std::uint16_t child : children[node_index])
			{
				append_preorder(child);
			}
		};
	for (const std::uint16_t root : roots)
	{
		append_preorder(root);
	}
	if (output.node_indices_in_preorder.size() != source.nodes.size())
	{
		diagnostics::mission_log(
			"model upload failed stage=node-preorder assigned=%u nodes=%u",
			static_cast<unsigned>(
				output.node_indices_in_preorder.size()),
			static_cast<unsigned>(source.nodes.size()));
		destroy_model(output);
		return false;
	}
	output.hardpoints.reserve(source.hardpoints.size());
	output.gameplay_locators = source.locators;
	for (const assets::GameplayHardpoint& source_hardpoint
		: source.hardpoints)
	{
		if (source_hardpoint.source_node >= transforms.size())
		{
			diagnostics::mission_log(
				"model upload failed stage=hardpoint-node node=%u nodes=%u",
				source_hardpoint.source_node,
				static_cast<unsigned>(transforms.size()));
			destroy_model(output);
			return false;
		}
		const glm::mat4 locator_transform =
			glm::translate(
				glm::mat4{1.0f},
				source_hardpoint.position)
			* glm::mat4(source_hardpoint.basis);
		const glm::mat4 object_transform =
			transforms[source_hardpoint.source_node]
				* locator_transform;
		assets::GameplayHardpoint hardpoint = source_hardpoint;
		hardpoint.position = glm::vec3(object_transform[3]);
		hardpoint.basis = glm::mat3(object_transform);
		output.hardpoints.push_back(hardpoint);
	}
	output.locators.reserve(source.locators.size());
	for (std::uint32_t locator_index = 0;
		locator_index < source.locators.size();
		++locator_index)
	{
		const assets::GameplayLocator& source_locator =
			source.locators[locator_index];
		if (source_locator.source_node >= transforms.size())
		{
			diagnostics::mission_log(
				"model upload failed stage=locator-node node=%u nodes=%u",
				source_locator.source_node,
				static_cast<unsigned>(transforms.size()));
			destroy_model(output);
			return false;
		}
		const glm::mat4 locator_transform =
			glm::translate(glm::mat4{1.0f}, source_locator.position)
			* glm::mat4(source_locator.basis);
		MissionGpuLocator locator;
		locator.object_transform =
			transforms[source_locator.source_node]
				* locator_transform;
		std::copy(
			std::begin(source_locator.parameters),
			std::end(source_locator.parameters),
			std::begin(locator.parameters));
		locator.dimensions = source_locator.dimensions;
		locator.on_time = source_locator.on_time;
		locator.off_time = source_locator.off_time;
		locator.phase = source_locator.phase;
		locator.exporter_id = source_locator.exporter_id;
		locator.light_radius = source_locator.light_radius;
		locator.light_intensity = source_locator.light_intensity;
		locator.source_node = source_locator.source_node;
		locator.type = source_locator.type;
		locator.subtype = source_locator.subtype;
		output.locators.push_back(locator);
		output.nodes[source_locator.source_node]
			.locator_indices.push_back(locator_index);
	}
	output.gun_clearance_directions = source.gun_clearance_directions;
	output.gun_clearance_masks = source.gun_clearance_masks;
	for (std::uint32_t node_index = 0;
		node_index < source.nodes.size();
		++node_index)
	{
		const assets::GameplayNode& source_node =
			source.nodes[node_index];
		MissionGpuNode& node = output.nodes[node_index];
		glm::vec3 origin_offset{0.0f};
		std::memcpy(node.name, source_node.name, sizeof(node.name));
		node.model_type = source_node.model_type;
		node.part_group_id = source_node.part_group_id;
		node.flags = source_node.flags;
		node.gun_mount_kind = source_node.gun_mount_kind;
		node.gun_part_slot = source_node.gun_part_slot;
		node.maximum_hit_points = source_node.maximum_hit_points;
		node.damage_group_selector = source_node.damage_group_selector;
		std::copy(
			std::begin(source_node.suppress_anim_rotation),
			std::end(source_node.suppress_anim_rotation),
			std::begin(node.suppress_anim_rotation));
		node.rest_translation = source_node.rest_translation;
		node.exported_position = source_node.position;
		node.source_local_basis = source_node.basis;
		node.joint_min_degrees = source_node.joint_min_degrees;
		node.joint_max_degrees = source_node.joint_max_degrees;
		node.parent_reference =
			source_node.parent < 0
				? -1
				: static_cast<std::int16_t>(source_node.parent);
		node.collision = source_node.collision;
		node.portals = source_node.portals;
		node.object_transform = transforms[node_index];
		node.sequences = source_node.sequences;
		node.point_groups = source_node.point_groups;
		const bool subsystem_highlightable =
			std::find(
				subsystem_part_groups.begin(),
				subsystem_part_groups.end(),
				source_node.part_group_id) != subsystem_part_groups.end();
		node.subsystem_highlightable = subsystem_highlightable;
		node.lods.resize(source_node.lods.size());
		for (std::uint32_t lod_index = 0;
			lod_index < source_node.lods.size();
			++lod_index)
		{
			const assets::GameplayLod& source_lod =
				source_node.lods[lod_index];
			MissionGpuLod& lod = node.lods[lod_index];
			lod.threshold = source_lod.threshold;
			lod.radius = source_lod.radius;
			if (lod_index == 0)
			{
				origin_offset = source_lod.origin_offset;
				if (node_index == 0)
				{
					output.atmosphere_radius = source_lod.original_radius;
				}
			}
			lod.bounds_min = source_lod.bounds_min;
			lod.bounds_max = source_lod.bounds_max;
			lod.index_count = source_lod.index_count;
			lod.cloak_index_count = source_lod.cloak_index_count;
			for (std::uint32_t section_index = 0;
				section_index < source_lod.sections.size();
				++section_index)
			{
				const assets::GameplaySection& section =
					source_lod.sections[section_index];
				lod.sections.push_back({
					section.first_index,
					section.index_count,
					section.texture,
					section.mode,
					section.modifier,
					section.light_channel,
					section.lines,
					section.suppressed,
					section.double_sided,
				});
			}
			for (const assets::GameplaySection& section
				: source_lod.cloak_sections)
			{
				lod.cloak_sections.push_back({
					section.first_index,
					section.index_count,
					section.texture,
					section.mode,
					section.modifier,
					section.light_channel,
					section.lines,
					section.suppressed,
					section.double_sided,
				});
			}
			if (source_lod.index_count == 0)
			{
				continue;
			}
			if (source_lod.vertices.size > UINT32_MAX
				|| source_lod.indices.size > UINT32_MAX
				|| source_lod.cloak_vertices.size > UINT32_MAX
				|| source_lod.cloak_indices.size > UINT32_MAX)
			{
				diagnostics::mission_log(
					"model upload failed stage=lod-capacity node=%u lod=%u",
					node_index,
					lod_index);
				destroy_model(output);
				return false;
			}
			const std::size_t source_vertex_count =
				source_lod.vertices.size
				/ sizeof(assets::GameplayVertex);
			if (source_lod.vertices.size
					!= source_vertex_count
						* sizeof(assets::GameplayVertex)
				|| source_vertex_count != source_lod.vertex_count
				|| source_lod.normals.size() != source_vertex_count
				|| source_lod.secondary_normals.size()
					!= source_vertex_count
				|| source_lod.static_lighting_rgb.size()
					!= source_vertex_count
				|| source_lod.indices.size
					!= static_cast<std::size_t>(
						source_lod.index_count)
						* sizeof(std::uint32_t))
			{
				diagnostics::mission_log(
					"model upload failed stage=lod-streams node=%u "
					"lod=%u vertices=%zu/%u normals=%zu/%zu/%zu "
					"indices=%zu/%u",
					node_index,
					lod_index,
					source_vertex_count,
					source_lod.vertex_count,
					source_lod.normals.size(),
					source_lod.secondary_normals.size(),
					source_lod.static_lighting_rgb.size(),
					source_lod.indices.size / sizeof(std::uint32_t),
					source_lod.index_count);
				destroy_model(output);
				return false;
			}
			const std::size_t cloak_vertex_count =
				source_lod.cloak_vertices.size
					/ sizeof(assets::GameplayVertex);
			const std::size_t cloak_index_count =
				source_lod.cloak_indices.size
					/ sizeof(std::uint32_t);
			if (source_lod.cloak_vertices.size
					!= cloak_vertex_count
						* sizeof(assets::GameplayVertex)
				|| cloak_vertex_count
					!= source_lod.cloak_vertex_count
				|| source_lod.cloak_indices.size
					!= cloak_index_count * sizeof(std::uint32_t)
				|| cloak_index_count != source_lod.cloak_index_count)
			{
				diagnostics::mission_log(
					"model upload failed stage=cloak-streams node=%u "
					"lod=%u vertices=%zu/%u indices=%zu/%u",
					node_index,
					lod_index,
					cloak_vertex_count,
					source_lod.cloak_vertex_count,
					cloak_index_count,
					source_lod.cloak_index_count);
				destroy_model(output);
				return false;
			}
			lod.source_vertices.resize(source_vertex_count);
			std::memcpy(
				lod.source_vertices.data(),
				source_lod.vertices.data,
				source_lod.vertices.size);
			lod.normals = source_lod.normals;
			lod.secondary_normals = source_lod.secondary_normals;
			lod.secondary_positions = source_lod.secondary_positions;
			lod.static_lighting_rgb =
				source_lod.static_lighting_rgb;
			std::vector<LitModelVertex> lit_vertices(source_vertex_count);
			for (std::size_t vertex = 0;
				vertex < source_vertex_count;
				++vertex)
			{
				const assets::GameplayVertex& position =
					lod.source_vertices[vertex];
				const glm::vec3& normal = lod.normals[vertex];
				const glm::vec3& secondary =
					lod.secondary_normals[vertex];
				const glm::vec3& lighting =
					lod.static_lighting_rgb[vertex];
				const glm::vec3& secondary_position =
					lod.secondary_positions[vertex];
				lit_vertices[vertex] = {
					position.x,
					position.y,
					position.z,
					position.u,
					position.v,
					normal.x,
					normal.y,
					normal.z,
					secondary.x,
					secondary.y,
					secondary.z,
					lighting.r,
					lighting.g,
					lighting.b,
					secondary_position.x,
					secondary_position.y,
					secondary_position.z,
				};
			}
			lod.lit_vertices = bgfx::createVertexBuffer(
				bgfx::copy(
					lit_vertices.data(),
					static_cast<std::uint32_t>(
						lit_vertices.size()
							* sizeof(LitModelVertex))),
				lit_model_layout);
			if (!bgfx::isValid(lod.lit_vertices))
			{
				diagnostics::mission_log(
					"model upload failed stage=lit-buffer node=%u lod=%u",
					node_index,
					lod_index);
				destroy_model(output);
				return false;
			}
			lod.source_faces = source_lod.faces;
			lod.source_face_corners = source_lod.face_corners;
			lod.source_sections = source_lod.sections;
			const std::size_t source_index_count =
				source_lod.indices.size / sizeof(std::uint32_t);
			lod.source_indices.resize(source_index_count);
			const auto* source_indices =
				reinterpret_cast<const std::uint32_t*>(
					source_lod.indices.data);
			for (std::size_t source_index = 0;
				source_index < source_index_count;
				++source_index)
			{
				if (source_indices[source_index] > UINT16_MAX)
				{
					diagnostics::mission_log(
						"model upload failed stage=shield-index node=%u "
						"lod=%u index=%u value=%u",
						node_index,
						lod_index,
						static_cast<unsigned>(source_index),
						source_indices[source_index]);
					destroy_model(output);
					return false;
				}
				lod.source_indices[source_index] =
					static_cast<std::uint16_t>(
						source_indices[source_index]);
			}
			lod.indices = bgfx::createIndexBuffer(
				bgfx::copy(
					source_lod.indices.data,
					static_cast<std::uint32_t>(
						source_lod.indices.size)),
				BGFX_BUFFER_INDEX32);
			if (!bgfx::isValid(lod.indices))
			{
				diagnostics::mission_log(
					"model upload failed node=%u lod=%u "
					"index=%u",
					node_index,
					lod_index,
					bgfx::isValid(lod.indices) ? 1u : 0u);
				destroy_model(output);
				return false;
			}
			lod.cloak_source_vertices.resize(cloak_vertex_count);
			if (cloak_vertex_count != 0)
			{
				std::memcpy(
					lod.cloak_source_vertices.data(),
					source_lod.cloak_vertices.data,
					source_lod.cloak_vertices.size);
			}
			lod.cloak_source_indices.resize(cloak_index_count);
			const auto* cloak_source_indices =
				reinterpret_cast<const std::uint32_t*>(
					source_lod.cloak_indices.data);
			for (std::size_t source_index = 0;
				source_index < cloak_index_count;
				++source_index)
			{
				if (cloak_source_indices[source_index] > UINT16_MAX)
				{
					diagnostics::mission_log(
						"model upload failed stage=cloak-index node=%u "
						"lod=%u index=%u value=%u",
						node_index,
						lod_index,
						static_cast<unsigned>(source_index),
						cloak_source_indices[source_index]);
					destroy_model(output);
					return false;
				}
				lod.cloak_source_indices[source_index] =
					static_cast<std::uint16_t>(
						cloak_source_indices[source_index]);
			}
			if (source_lod.cloak_index_count != 0)
			{
				lod.cloak_indices = bgfx::createIndexBuffer(
					bgfx::copy(
						source_lod.cloak_indices.data,
						static_cast<std::uint32_t>(
							source_lod.cloak_indices.size)),
					BGFX_BUFFER_INDEX32);
				if (!bgfx::isValid(lod.cloak_indices))
				{
					diagnostics::mission_log(
						"model upload failed stage=cloak-buffer node=%u "
						"lod=%u",
						node_index,
						lod_index);
					destroy_model(output);
					return false;
				}
			}
		}
		if (!node.lods.empty()
			&& !node.lods[0].source_vertices.empty())
		{
			// SR_mesh_calculate_bounds (LANCER.EXE 0x004c3f10) publishes
			// the live LOD-0 mesh bounds and radius. The tag-1 fields are
			// not the BMO bounds; Rus_Sabre's body fields are all zero.
			node.bounds_min = node.lods[0].bounds_min;
			node.bounds_max = node.lods[0].bounds_max;
			node.radius = node.lods[0].radius;
		}
		// object_accumulate_model_inertia_and_bounds
		// (LANCER.EXE 0x00476768..0x0047681e) transforms every live
		// LOD-0 mesh point and updates GameObject+0x59c/+0x5a0..0x5b4.
		accumulate_mesh_extent(
			node,
			node.object_transform * glm::translate(
				glm::mat4{1.0f}, origin_offset),
			output.radius,
			have_bounds,
			output.bounds_min,
			output.bounds_max);
	}
	return true;
}

std::int32_t embedded_attachment_definition(
	const MissionGpuLocator& locator)
{
	if ((locator.type != 1 && locator.type != 5)
		|| locator.subtype < 0 || locator.subtype >= 20)
	{
		return -1;
	}
	return locator.type * 20 + locator.subtype;
}

void recenter_model_with_embedded_attachments(
	MissionGpuModel& model,
	const MissionGpuModel
		(&attachments)[game::kAttachmentDefinitionCount][2])
{
	const glm::vec3 old_center = model.center_of_mass;
	glm::vec3 first_mass_moment = old_center * model.total_mass;
	float total_mass = model.total_mass;
	const glm::mat4 restore_object_origin =
		glm::translate(glm::mat4{1.0f}, old_center);
	for (const MissionGpuLocator& locator : model.locators)
	{
		const std::int32_t definition =
			embedded_attachment_definition(locator);
		if (definition < 0)
		{
			continue;
		}
		if (locator.source_node >= model.nodes.size()
			|| (model.nodes[locator.source_node].flags & 0x0004u) != 0)
		{
			// object_accumulate_model_mass does not descend through a
			// hidden parent RenderObject, so neither does its embedded
			// tag-1/tag-5 model tree.
			continue;
		}
		const MissionGpuModel& attached =
			attachments[definition][0];
		if (attached.total_mass <= 0.0f)
		{
			continue;
		}
		// object_instantiate_model_locator (LANCER.EXE 0x00499a10)
		// places the nested embedded root at
		// locator.position + locator.basis * nested.center_of_mass.
		// Restoring the outer model's object origin converts the retained
		// center-relative locator transform back into that exact frame.
		const glm::vec3 attached_center =
			glm::vec3(
				restore_object_origin
				* locator.object_transform
				* glm::vec4(attached.center_of_mass, 1.0f));
		first_mass_moment += attached.total_mass * attached_center;
		total_mass += attached.total_mass;
	}
	if (total_mass <= 0.0f)
	{
		return;
	}
	const glm::vec3 new_center = first_mass_moment / total_mass;
	const auto parallel_axis = [](float mass, const glm::vec3& offset)
	{
		const float squared = glm::dot(offset, offset);
		return mass * (
			glm::mat3{squared}
			- glm::outerProduct(offset, offset));
	};
	glm::mat3 aggregate_inertia =
		model.inertia_tensor
		+ parallel_axis(model.total_mass, old_center - new_center);
	for (const MissionGpuLocator& locator : model.locators)
	{
		const std::int32_t definition =
			embedded_attachment_definition(locator);
		if (definition < 0)
		{
			continue;
		}
		const MissionGpuModel& attached =
			attachments[definition][0];
		if (attached.total_mass <= 0.0f)
		{
			continue;
		}
		const glm::mat4 attachment_transform =
			restore_object_origin * locator.object_transform;
		const glm::vec3 attached_center =
			glm::vec3(
				attachment_transform
				* glm::vec4(attached.center_of_mass, 1.0f));
		const glm::mat3 orientation{attachment_transform};
		aggregate_inertia +=
			orientation
				* attached.inertia_tensor
				* glm::transpose(orientation)
			+ parallel_axis(
				attached.total_mass,
				attached_center - new_center);
	}
	const glm::mat4 correction =
		glm::translate(glm::mat4{1.0f}, old_center - new_center);
	for (MissionGpuNode& node : model.nodes)
	{
		node.object_transform = correction * node.object_transform;
	}
	for (MissionGpuLocator& locator : model.locators)
	{
		locator.object_transform = correction * locator.object_transform;
	}
	for (assets::GameplayHardpoint& hardpoint : model.hardpoints)
	{
		hardpoint.position += old_center - new_center;
	}
	model.center_of_mass = new_center;
	model.inertia_tensor = aggregate_inertia;
	model.total_mass = total_mass;
	bool have_bounds = false;
	model.radius = 0.0f;
	for (const MissionGpuNode& node : model.nodes)
	{
		accumulate_mesh_extent(
			node,
			node.object_transform,
			model.radius,
			have_bounds,
			model.bounds_min,
			model.bounds_max);
	}
	for (const MissionGpuLocator& locator : model.locators)
	{
		const std::int32_t definition =
			embedded_attachment_definition(locator);
		if (definition < 0)
		{
			continue;
		}
		const MissionGpuModel& attached =
			attachments[definition][0];
		const glm::mat4 attached_root =
			locator.object_transform
				* glm::translate(
					glm::mat4{1.0f}, attached.center_of_mass);
		for (const MissionGpuNode& node : attached.nodes)
		{
			const glm::mat4 transform =
				attached_root * node.object_transform;
			accumulate_mesh_extent(
				node,
				transform,
				model.radius,
				have_bounds,
				model.bounds_min,
				model.bounds_max);
		}
	}
}

using MissionSceneLights = std::vector<MissionSceneLight>;

void set_explosion_source_mesh(
	const game::ObjectModelReference& reference,
	const MissionGpuLod& lod)
{
	reference.explosion_vertices = &lod.source_vertices;
	reference.explosion_indices = &lod.source_indices;
	reference.explosion_faces = &lod.source_faces;
	reference.explosion_face_corners = &lod.source_face_corners;
	reference.explosion_sections = &lod.source_sections;
	reference.explosion_normals = &lod.normals;
	reference.explosion_secondary_normals = &lod.secondary_normals;
	reference.explosion_static_lighting = &lod.static_lighting_rgb;
}

const MissionGpuLod* choose_lod(
	const MissionGpuNode& node,
	float view_depth,
	float detail_scale,
	bool force_highest_detail = false,
	float* lod_blend = nullptr)
{
	if (lod_blend != nullptr)
	{
		*lod_blend = 0.0f;
	}
	if (node.lods.empty())
	{
		return nullptr;
	}
	if (force_highest_detail)
	{
		return node.lods[0].index_count == 0 ? nullptr : &node.lods[0];
	}
	const float lod_distance = view_depth / detail_scale;
	for (std::size_t index = 0; index < node.lods.size(); ++index)
	{
		const MissionGpuLod& candidate = node.lods[index];
		if (lod_distance < candidate.threshold)
		{
			if (lod_blend != nullptr
				&& index + 1 < node.lods.size()
				&& (node.flags & 0x0030u) != 0)
			{
				const float lower =
					index == 0
						? 0.0f
						: node.lods[index - 1].threshold;
				const float interval = candidate.threshold - lower;
				if (interval != 0.0f)
				{
					const float fraction =
						(lod_distance - lower) / interval;
					if (fraction >= 0.75f)
					{
						*lod_blend =
							(fraction - 0.75f) * 4.0f;
					}
				}
			}
			return candidate.index_count == 0 ? nullptr : &candidate;
		}
	}
	return nullptr;
}

std::uint32_t pack_lighting_rgb(const glm::vec3& color)
{
	const std::uint8_t red = static_cast<std::uint8_t>(
		std::clamp(std::lrint(color.r * 255.0f), 0l, 255l));
	const std::uint8_t green = static_cast<std::uint8_t>(
		std::clamp(std::lrint(color.g * 255.0f), 0l, 255l));
	const std::uint8_t blue = static_cast<std::uint8_t>(
		std::clamp(std::lrint(color.b * 255.0f), 0l, 255l));
	return 0xff000000u
		| static_cast<std::uint32_t>(blue) << 16
		| static_cast<std::uint32_t>(green) << 8
		| red;
}

void apply_mesh_lighting(
	const std::vector<assets::GameplayVertex>& source_vertices,
	const std::vector<glm::vec3>& normals,
	const std::vector<glm::vec3>& secondary_normals,
	const std::vector<glm::vec3>& static_lighting_rgb,
	float mesh_radius,
	const glm::mat4& transform,
	const glm::mat3& camera_orientation,
	float mesh_scale,
	float normal_blend,
	std::uint32_t exclusion_mask,
	bool static_lighting_enabled,
	const MissionSceneLights& lights,
	ModelRenderVertex* output,
	bool sine_response = false,
	std::span<const glm::vec3> secondary_positions = {},
	float position_blend = 0.0f)
{
	const std::size_t vertex_count = source_vertices.size();

	// SR_mesh_generate_environment_uv_channel
	// (LANCER.EXE 0x004c7360) normalizes the first two axes of the
	// object-to-view transform and dots each against the mesh normal. It
	// then maps both signed results through value * 0.5 + 0.5. During an
	// authored LOD transition it performs the same operation on both normal
	// streams and linearly combines their contributions; the blended normal
	// is deliberately not renormalized.
	// StarLancer's basis is left-handed: camera column 2 is forward. The
	// executable composes camera-transpose with the object matrix and uses
	// rows 0 and 1 directly. No backend clip-space Z conversion belongs in
	// these texture coordinates.
	const glm::mat3 object_to_view =
		glm::transpose(camera_orientation) * glm::mat3(transform);
	glm::vec3 environment_u_axis{
		object_to_view[0][0],
		object_to_view[1][0],
		object_to_view[2][0],
	};
	glm::vec3 environment_v_axis{
		object_to_view[0][1],
		object_to_view[1][1],
		object_to_view[2][1],
	};
	const float u_axis_length = glm::length(environment_u_axis);
	const float v_axis_length = glm::length(environment_v_axis);
	if (u_axis_length > 0.0f)
	{
		environment_u_axis /= u_axis_length;
	}
	if (v_axis_length > 0.0f)
	{
		environment_v_axis /= v_axis_length;
	}

	glm::vec3 base{0.0f};
	for (const MissionSceneLight& light : lights)
	{
		if (light.subtype == 2
			&& (exclusion_mask & light.mask) == 0)
		{
			base += light.intensity * light.rgb;
		}
	}
	const glm::mat3 world_orientation =
		glm::mat3(transform) / mesh_scale;
	const glm::mat3 inverse_orientation =
		glm::transpose(world_orientation);
	const glm::vec3 mesh_position = glm::vec3(transform[3]);
	bool affected = static_lighting_enabled;

	// Retail updates a persistent mesh color array. The transient vertex
	// buffer is already the frame-local equivalent, so accumulate one vertex
	// at a time instead of allocating a second color array on every node.
	for (const MissionSceneLight& light : lights)
	{
		if ((exclusion_mask & light.mask) != 0)
		{
			continue;
		}
		if (light.subtype == 1)
		{
			affected = true;
		}
		else if (light.subtype == 0)
		{
			const glm::vec3 light_local =
				inverse_orientation
				* (light.position - mesh_position);
			const float effective_radius =
				light.intensity * light.radius;
			const float broad_radius =
				effective_radius + mesh_radius;
			if (glm::dot(light_local, light_local)
				>= broad_radius * broad_radius)
			{
				continue;
			}
			affected = true;
		}
	}

	for (std::size_t vertex = 0; vertex < vertex_count; ++vertex)
	{
		output[vertex] =
			model_render_vertex(source_vertices[vertex]);
		if (position_blend != 0.0f)
		{
			const glm::vec3 position = glm::mix(
				glm::vec3{output[vertex].x, output[vertex].y, output[vertex].z},
				secondary_positions[vertex], position_blend);
			output[vertex].x = position.x;
			output[vertex].y = position.y;
			output[vertex].z = position.z;
		}
		const glm::vec3 environment_normal =
			normals[vertex] * (1.0f - normal_blend)
				+ secondary_normals[vertex] * normal_blend;
		output[vertex].environment_u =
			glm::dot(environment_u_axis, environment_normal) * 0.5f
				+ 0.5f;
		output[vertex].environment_v =
			glm::dot(environment_v_axis, environment_normal) * 0.5f
				+ 0.5f;
		glm::vec3 color = base;
		if (static_lighting_enabled)
		{
			color += static_lighting_rgb[vertex];
		}
		for (const MissionSceneLight& light : lights)
		{
			if ((exclusion_mask & light.mask) != 0)
			{
				continue;
			}
			if (light.subtype == 1)
			{
				const glm::vec3 direction =
					glm::normalize(
						inverse_orientation * light.direction);
				float normal_dot;
				if (sine_response)
				{
					normal_dot = lighting_response_table()[static_cast<std::size_t>(
						std::lrint(std::clamp(glm::dot(
							direction * light.intensity, normals[vertex]),
							0.0f, 1.0f) * 4095.0f))];
				}
				else
				{
					normal_dot = glm::dot(
						direction * ((1.0f - normal_blend) * light.intensity),
						normals[vertex]) + glm::dot(
							direction * (normal_blend * light.intensity),
							secondary_normals[vertex]);
				}
				if (normal_dot > 0.0f)
				{
					color += normal_dot * light.rgb;
				}
			}
			else if (light.subtype == 0)
			{
				const glm::vec3 light_local =
					inverse_orientation
						* (light.position - mesh_position);
				const float effective_radius =
					light.intensity * light.radius;
				const float broad_radius =
					effective_radius + mesh_radius;
				if (glm::dot(light_local, light_local)
					>= broad_radius * broad_radius)
				{
					continue;
				}
				const assets::GameplayVertex& source =
					source_vertices[vertex];
				const glm::vec3 to_light =
					light_local
						- mesh_scale
							* glm::vec3{
								source.x, source.y, source.z};
				const float distance_squared =
					glm::dot(to_light, to_light);
				const float radius_squared =
					effective_radius * effective_radius;
				if (!(distance_squared < radius_squared))
				{
					continue;
				}
				float normal_dot =
					glm::dot(to_light, normals[vertex]);
				if (normal_blend > 0.0f)
				{
					normal_dot =
						normal_dot * (1.0f - normal_blend)
						+ glm::dot(
							to_light,
							secondary_normals[vertex])
							* normal_blend;
				}
				if (!(normal_dot > 0.0f))
				{
					continue;
				}
				const float distance = std::sqrt(distance_squared);
				const float attenuation =
					normal_dot
						* (1.0f / distance
							+ distance / radius_squared
							- 2.0f / effective_radius);
				color += attenuation * light.intensity * light.rgb;
			}
		}

		// SR_mesh_apply_lighting clamps after an additive base or any
		// dynamic routine reports that it affected the mesh. There is no
		// lower clamp.
		if (affected)
		{
			color = glm::min(color, glm::vec3{1.0f});
		}
		output[vertex].color = pack_lighting_rgb(color);
	}
}

struct GpuMeshLighting
{
	static constexpr std::uint32_t kLightCapacity = 32;

	float environment_u[4]{};
	float environment_v[4]{};
	float base[4]{};
	float params[4]{};
	float morph[4]{};
	float positions_and_radii[kLightCapacity][4]{};
	float directions_and_types[kLightCapacity][4]{};
	float colors_and_intensities[kLightCapacity][4]{};
	std::uint16_t light_count = 0;
};

bool prepare_gpu_mesh_lighting(
	float mesh_radius,
	const glm::mat4& transform,
	const glm::mat3& camera_orientation,
	float mesh_scale,
	float normal_blend,
	std::uint32_t exclusion_mask,
	bool static_lighting_enabled,
	std::span<const MissionSceneLight> lights,
	GpuMeshLighting& output,
	float position_blend = 0.0f)
{
	std::uint32_t light_count = 0;
	glm::vec3 base{0.0f};
	bool affected = static_lighting_enabled;

	const glm::mat3 world_orientation =
		glm::mat3(transform) / mesh_scale;
	const glm::mat3 inverse_orientation =
		glm::transpose(world_orientation);
	const glm::vec3 mesh_position = glm::vec3(transform[3]);
	for (const MissionSceneLight& light : lights)
	{
		if ((exclusion_mask & light.mask) != 0)
		{
			continue;
		}
		if (light.subtype == 2)
		{
			base += light.intensity * light.rgb;
			continue;
		}

		glm::vec3 position{0.0f};
		glm::vec3 direction{0.0f};
		float radius = 0.0f;
		if (light.subtype == 1)
		{
			direction = glm::normalize(
				inverse_orientation * light.direction);
			affected = true;
		}
		else if (light.subtype == 0)
		{
			position = inverse_orientation
				* (light.position - mesh_position);
			radius = light.intensity * light.radius;
			const float broad_radius = radius + mesh_radius;
			if (glm::dot(position, position)
				>= broad_radius * broad_radius)
			{
				continue;
			}
			affected = true;
		}
		else
		{
			continue;
		}
		if (light_count == GpuMeshLighting::kLightCapacity)
		{
			return false;
		}
		output.positions_and_radii[light_count][0] = position.x;
		output.positions_and_radii[light_count][1] = position.y;
		output.positions_and_radii[light_count][2] = position.z;
		output.positions_and_radii[light_count][3] = radius;
		output.directions_and_types[light_count][0] = direction.x;
		output.directions_and_types[light_count][1] = direction.y;
		output.directions_and_types[light_count][2] = direction.z;
		output.directions_and_types[light_count][3] =
			light.subtype == 1 ? 1.0f : 0.0f;
		output.colors_and_intensities[light_count][0] = light.rgb.r;
		output.colors_and_intensities[light_count][1] = light.rgb.g;
		output.colors_and_intensities[light_count][2] = light.rgb.b;
		output.colors_and_intensities[light_count][3] = light.intensity;
		++light_count;
	}

	const glm::mat3 object_to_view =
		glm::transpose(camera_orientation) * glm::mat3(transform);
	glm::vec3 environment_u_axis{
		object_to_view[0][0],
		object_to_view[1][0],
		object_to_view[2][0],
	};
	glm::vec3 environment_v_axis{
		object_to_view[0][1],
		object_to_view[1][1],
		object_to_view[2][1],
	};
	const float u_axis_length = glm::length(environment_u_axis);
	const float v_axis_length = glm::length(environment_v_axis);
	if (u_axis_length > 0.0f)
	{
		environment_u_axis /= u_axis_length;
	}
	if (v_axis_length > 0.0f)
	{
		environment_v_axis /= v_axis_length;
	}
	output.environment_u[0] = environment_u_axis.x;
	output.environment_u[1] = environment_u_axis.y;
	output.environment_u[2] = environment_u_axis.z;
	output.environment_u[3] = 0.0f;
	output.environment_v[0] = environment_v_axis.x;
	output.environment_v[1] = environment_v_axis.y;
	output.environment_v[2] = environment_v_axis.z;
	output.environment_v[3] = 0.0f;
	output.base[0] = base.r;
	output.base[1] = base.g;
	output.base[2] = base.b;
	output.base[3] = static_lighting_enabled ? 1.0f : 0.0f;
	output.params[0] = mesh_scale;
	output.params[1] = normal_blend;
	output.params[2] = static_cast<float>(light_count);
	output.params[3] = affected ? 1.0f : 0.0f;
	output.morph[0] = position_blend;
	output.light_count = static_cast<std::uint16_t>(light_count);
	return true;
}

void bind_gpu_mesh_lighting(
	const FrontendRenderer& frontend,
	const GpuMeshLighting& lighting)
{
	bgfx::setUniform(
		frontend.lighting_environment_u_uniform,
		lighting.environment_u);
	bgfx::setUniform(
		frontend.lighting_environment_v_uniform,
		lighting.environment_v);
	bgfx::setUniform(frontend.lighting_base_uniform, lighting.base);
	bgfx::setUniform(frontend.lighting_params_uniform, lighting.params);
	bgfx::setUniform(frontend.model_morph_uniform, lighting.morph);
	if (lighting.light_count != 0)
	{
		bgfx::setUniform(
			frontend.lighting_position_radius_uniform,
			lighting.positions_and_radii,
			lighting.light_count);
		bgfx::setUniform(
			frontend.lighting_direction_type_uniform,
			lighting.directions_and_types,
			lighting.light_count);
		bgfx::setUniform(
			frontend.lighting_color_intensity_uniform,
			lighting.colors_and_intensities,
			lighting.light_count);
	}
}

glm::vec3 dynamic_locator_light_color(std::int16_t subtype)
{
	switch (subtype)
	{
	case 0: return {0.0f, 0.0f, 1.0f};
	case 1: return {0.0f, 1.0f, 0.0f};
	case 2: return {1.0f, 1.0f, 0.0f};
	case 3: return {1.0f, 0.0f, 0.0f};
	case 4: return {0.0f, 1.0f, 1.0f};
	case 5: return {1.0f, 1.0f, 1.0f};
	default: return glm::vec3{0.0f};
	}
}

float dynamic_locator_light_edge(
	const MissionGpuLocator& locator,
	std::uint32_t gameplay_tick,
	const game::WorldObject* owner)
{
	const std::uint32_t period =
		static_cast<std::uint32_t>(locator.on_time)
		+ static_cast<std::uint32_t>(locator.off_time);
	if (period == 0)
	{
		return 1.0f;
	}
	// Object_model_tree_render (LANCER.EXE 0x0049a8c0) promotes the
	// unsigned period before remainder. Preserve that wrap for an authored
	// phase larger than the early mission clock, then reinterpret the
	// remainder as signed for the two compiled comparisons below.
	const std::uint32_t numerator =
		(static_cast<std::uint32_t>(
				static_cast<std::int32_t>(
					static_cast<std::int16_t>(
						owner != nullptr
							? owner->random_phase : 0)))
			+ gameplay_tick)
			* 10u
		- static_cast<std::uint32_t>(locator.phase);
	const std::uint32_t unsigned_cycle = numerator % period;
	const std::int32_t cycle =
		static_cast<std::int32_t>(unsigned_cycle);
	float edge = 1.0f;
	if (locator.on_time < cycle)
	{
		edge = static_cast<float>(
			locator.on_time - cycle + 200) * 0.005f;
	}
	if (static_cast<std::int32_t>(period) < cycle)
	{
		edge = static_cast<float>(
			cycle - static_cast<std::int32_t>(period) + 200)
			* 0.005f;
	}
	return edge;
}

bool dynamic_locator_light_active(
	const MissionGpuLocator& locator,
	std::uint32_t gameplay_tick,
	const game::WorldObject* owner)
{
	if (locator.type != 4
		|| !(locator.light_intensity > 0.0f))
	{
		return false;
	}
	// Object_model_tree_render initializes the edge to one and only applies
	// the cycle calculation when on_time + off_time is nonzero. A zero-period
	// type-four locator is therefore an always-on point light; launch-hangar
	// models use these alongside their animated doors.
	// Object_model_tree_render (LANCER.EXE 0x0049a8c0) uses the
	// retained 200-unit edge calculation as an inclusion test for the
	// type-five child light; it does not scale the light intensity.
	return dynamic_locator_light_edge(
		locator, gameplay_tick, owner) > 0.0f;
}

glm::vec3 dynamic_locator_flare_color(std::int16_t subtype)
{
	switch (subtype)
	{
	case 0: return {0.2f, 0.5f, 1.0f};
	case 1: return {0.5f, 1.0f, 0.5f};
	case 2: return {1.0f, 1.0f, 1.0f};
	case 3: return {1.0f, 0.5f, 0.2f};
	case 4: return {0.5f, 1.0f, 1.0f};
	case 5: return {1.0f, 1.0f, 1.0f};
	default: return glm::vec3{0.0f};
	}
}

glm::mat3 cloak_wobble(
	const game::WorldObject& object,
	std::uint32_t simulation_tick);
MissionRenderInstance with_planet_spin(
	const MissionRenderInstance& instance,
	const game::World* world);

void collect_model_point_lights(
	const MissionRenderer& renderer,
	const MissionGpuModel& model,
	const glm::mat4& model_root,
	const game::WorldObject* object,
	bool animate_runtime,
	std::uint16_t runtime_reference_base,
	const MissionRenderFrame& frame,
	std::uint32_t depth,
	MissionSceneLights& lights)
{
	if (depth >= 32)
	{
		return;
	}
	for (std::size_t locator_index = 0;
		locator_index < model.locators.size();
		++locator_index)
	{
		const MissionGpuLocator& locator = model.locators[locator_index];
		if (locator.source_node >= model.nodes.size())
		{
			continue;
		}
		const MissionGpuNode& source_node =
			model.nodes[locator.source_node];
		const std::uint16_t runtime_reference =
			static_cast<std::uint16_t>(
				runtime_reference_base
					+ source_node.runtime_model_index);
		const game::ObjectModelReference* runtime_model = nullptr;
		if (animate_runtime
			&& object != nullptr
			&& runtime_reference
				< object->model_references.size())
		{
			runtime_model =
				&object->model_references[
					runtime_reference];
			if (runtime_model->removed
				|| (runtime_model->runtime_flags
					& kRuntimeModelHidden) != 0)
			{
				continue;
			}
		}
		else if ((source_node.flags & 0x0004u) != 0)
		{
			continue;
		}
		bool portal_visible = true;
		const glm::mat4 source_model_root =
			animate_runtime && object != nullptr
				? explosion_portal_root(
					frame,
					*object,
					runtime_reference,
					model_root,
					glm::length(glm::vec3{model_root[0]}),
					&portal_visible)
				: model_root;
		if (!portal_visible)
		{
			continue;
		}

		glm::mat4 locator_transform = locator.object_transform;
		if (runtime_model != nullptr
			&& locator_index < model.gameplay_locators.size())
		{
			const assets::GameplayLocator& gameplay_locator =
				model.gameplay_locators[locator_index];
			locator_transform = runtime_model->scene_transform
				* glm::translate(
					glm::mat4{1.0f}, gameplay_locator.position)
				* glm::mat4(gameplay_locator.basis);
		}
		const glm::mat4 locator_world =
			source_model_root * locator_transform;
		if (dynamic_locator_light_active(
				locator, frame.simulation_tick, object))
		{
			lights.push_back({
				glm::vec3(locator_world[3]),
				{0.0f, 0.0f, 1.0f},
				dynamic_locator_light_color(locator.subtype),
				locator.light_intensity,
				locator.light_radius,
				0u,
				0u,
			});
		}

		if ((locator.type != 1 && locator.type != 5)
			|| locator.subtype < 0 || locator.subtype >= 20)
		{
			continue;
		}
		const std::uint32_t definition =
			static_cast<std::uint32_t>(locator.type) * 20u
			+ static_cast<std::uint32_t>(locator.subtype);
		if (definition >= game::kAttachmentDefinitionCount
			|| !renderer.attachment_model_loaded[definition][0])
		{
			continue;
		}
		const MissionGpuModel& attached =
			renderer.attachment_models[definition][0];
		const game::EmbeddedModelTree* runtime_tree = nullptr;
		if (animate_runtime && object != nullptr)
		{
			for (const game::EmbeddedModelTree& candidate
				: object->embedded_model_trees)
			{
				if (candidate.parent_scope_base
						== runtime_reference_base
					&& candidate.source_locator == locator_index
					&& candidate.attachment_definition == definition)
				{
					runtime_tree = &candidate;
					break;
				}
			}
		}
		collect_model_point_lights(
			renderer,
			attached,
			runtime_tree != nullptr
				? source_model_root
				: locator_world
					* glm::translate(
						glm::mat4{1.0f}, attached.center_of_mass),
			object,
			runtime_tree != nullptr,
			runtime_tree != nullptr
				? runtime_tree->runtime_reference_base : 0,
			frame,
			depth + 1,
			lights);
	}
}

const MissionSceneLights& build_scene_lights(
	MissionRenderer& renderer,
	const MissionRenderFrame& frame)
{
	const glm::vec3 sun_direction =
		frame.environment != nullptr
			? frame.environment->sun_direction
			: glm::normalize(glm::vec3{1.0f, -0.5f, 0.2f});
	const glm::vec3 ambient_direction =
		frame.environment != nullptr
			? frame.environment->ambient_direction
			: glm::normalize(glm::vec3{-1.0f, 0.5f, 0.0f});
	const glm::vec3 nebula_rgb =
		frame.environment != nullptr
			? frame.environment->nebula_light_rgb
			: glm::vec3{0.24f, 0.50f, 1.0f};
	MissionSceneLights& lights = renderer.scene_lights;
	lights.clear();
	const std::size_t required_light_capacity =
		6 + frame.instance_count * 4
			+ game::kMaxAnimatedExplosions
			+ game::kMaxDestructionLights + 1;
	if (lights.capacity() < required_light_capacity)
	{
		lights.reserve(required_light_capacity);
	}
	lights.push_back({
		{}, sun_direction, {1.0f, 1.0f, 0.8f},
		1.0f, 0.0f, 1u, 1u});
	lights.push_back({
		{}, ambient_direction, nebula_rgb,
		1.0f, 0.0f, 2u, 1u});
	lights.push_back({
		{}, {}, {0.04f, 0.04f, 0.04f},
		1.0f, 0.0f, 4u, 2u});
	lights.push_back({
		{}, sun_direction, {1.0f, 1.0f, 0.8f},
		1.0f, 0.0f, 8u, 1u});
	lights.push_back({
		{}, ambient_direction, nebula_rgb,
		0.7f, 0.0f, 16u, 1u});
	lights.push_back({
		{}, {}, {0.09f, 0.09f, 0.09f},
		1.0f, 0.0f, 32u, 2u});

	if (frame.world != nullptr)
	{
		const game::UberExplosionEffect& uber =
			frame.world->death_effects.uber_explosion;
		if (uber.active)
		{
			// Uber_explosion_create, LANCER.EXE 0x004730bc..0x00473109:
			// this point light remains unchanged until singleton teardown.
			lights.push_back({
				uber.position,
				{},
				{0.7f, 0.5f, 1.0f},
				2.0f,
				uber.size * 20.0f,
				0u,
				0u,
			});
		}
		for (const game::ExplosionBillboardEffect& explosion
			: frame.world->death_effects.explosion_billboards)
		{
			if (!explosion.active || !explosion.create_light)
			{
				continue;
			}
			const std::int32_t age = static_cast<std::int32_t>(
				frame.simulation_tick - explosion.start_tick)
				- explosion.delay_ticks;
			if (age < 0 || age >= explosion.duration_ticks)
			{
				continue;
			}
			// Explosion_billboard_spawn/update (LANCER.EXE 0x0046bd00,
			// 0x0046e712) creates this light with radius sqrt(size) * 50
			// and linearly reduces its initial intensity of ten.
			lights.push_back({
				explosion.position,
				{},
				{1.0f, 0.5f, 0.1f},
				10.0f
					- static_cast<float>(age)
						* 10.0f
						/ static_cast<float>(explosion.duration_ticks),
				std::sqrt(explosion.size) * 50.0f,
				0u,
				0u,
			});
		}
		for (const game::DestructionLightEffect& destruction
			: frame.world->death_effects.destruction_lights)
		{
			if (!destruction.active
				|| destruction.owner_index >= game::kMaxGameObjects)
			{
				continue;
			}
			const game::WorldObject& owner =
				frame.world->objects[destruction.owner_index];
			if (!owner.active
				|| owner.generation != destruction.owner_generation)
			{
				continue;
			}
			const glm::mat4 ordinary_root = math::model_transform(
				owner.scene_orientation, 1.0f, owner.scene_position);
			glm::mat4 transform = ordinary_root;
			if (destruction.model_reference
				< owner.model_references.size())
			{
				transform = explosion_portal_root(
					frame,
					owner,
					destruction.model_reference,
					ordinary_root,
					1.0f)
					* owner.model_references[
						destruction.model_reference].scene_transform;
			}
			lights.push_back({
				glm::vec3(
					transform
						* glm::vec4(destruction.local_position, 1.0f)),
				{},
				{1.0f, 0.0f, 0.0f},
				destruction.intensity,
				20000.0f,
				0u,
				0u,
			});
		}
	}

	for (std::uint32_t index = 0; index < frame.instance_count; ++index)
	{
		const MissionRenderInstance instance =
			with_planet_spin(frame.instances[index], frame.world);
		if (instance.direct_mesh)
		{
			continue;
		}
		const MissionGpuModel& model =
			renderer.models[static_cast<std::size_t>(instance.model)];
		const game::WorldObject* object =
			instance.source_object;
		const glm::mat3 wobble =
			object != nullptr
				&& (object->runtime_flags & 0x00000100u) != 0
				? cloak_wobble(*object, frame.simulation_tick)
				: glm::mat3{1.0f};
		const glm::mat4 root = sl_open::math::model_transform(
			instance.orientation * wobble,
			instance.scale,
			instance.position);
		collect_model_point_lights(
			renderer,
			model,
			root,
			object,
			true,
			0,
			frame,
			0,
			lights);

		if (object == nullptr)
		{
			continue;
		}
		for (std::uint8_t attachment_index = 0;
			attachment_index < object->attachment_count;
			++attachment_index)
		{
			const game::AttachmentSlot& attachment =
				object->attachments[attachment_index];
			if (!attachment.live_model
				|| attachment.definition_index < 0
				|| static_cast<std::size_t>(
					attachment.definition_index)
					>= game::kAttachmentDefinitionCount)
			{
				continue;
			}
			const std::uint32_t variant_index =
				attachment.alternate_model ? 1u : 0u;
			if (!renderer.attachment_model_loaded[
					attachment.definition_index][variant_index])
			{
				continue;
			}
			glm::mat4 hardpoint_transform =
				glm::translate(
					glm::mat4{1.0f},
					attachment.local_position)
				* glm::mat4(attachment.local_orientation);
			if (attachment.model_reference >= 0
				&& static_cast<std::size_t>(
					attachment.model_reference)
					< object->model_references.size())
			{
				hardpoint_transform =
					object->model_references[
						static_cast<std::uint16_t>(
							attachment.model_reference)].scene_transform
					* attachment.hardpoint_from_model;
			}
			hardpoint_transform = hardpoint_transform
				* glm::mat4(
					sl_open::math::rotation_from_euler(
						{0.0f, -glm::pi<float>(), 0.0f}));
			const glm::mat4 hardpoint_root =
				attachment.model_reference >= 0
					? explosion_portal_root(
						frame,
						*object,
						static_cast<std::uint16_t>(
							attachment.model_reference),
						root,
						instance.scale)
					: root;
			const MissionGpuModel& attached =
				renderer.attachment_models[
					attachment.definition_index][variant_index];
			collect_model_point_lights(
				renderer,
				attached,
				hardpoint_root * hardpoint_transform
					* glm::translate(
						glm::mat4{1.0f},
						attached.center_of_mass),
				object,
				false,
				0,
				frame,
				0,
				lights);
		}
	}
	for (std::uint32_t index = 0;
		index < frame.gun_projectile_count;
		++index)
	{
		const MissionGunProjectile& projectile =
			frame.gun_projectiles[index];
		if (projectile.type_index != 11
			|| !renderer.model_loaded[0xb1])
		{
			continue;
		}
		collect_model_point_lights(
			renderer,
			renderer.models[0xb1],
			sl_open::math::model_transform(
				projectile.orientation,
				1.0f,
				projectile.position),
			nullptr,
			false,
			0,
			frame,
			0,
			lights);
	}
	return lights;
}

bool blended_mode(std::uint8_t mode)
{
	// SRO_build_lod_mesh (LANCER.EXE 0x004a3040) maps the low material
	// nibble to hardware material byte +6 as follows:
	//   0/1/3/6/7 -> 0, 2/4/8/10 -> 1, 5 -> 3.
	// srd3d_sync_material_state (srd3d.dll 0x10001f3d) maps nonzero
	// values to enabled blending and the bucket-one depth setup disables
	// Z writes for them.
	return mode == 2 || mode == 4 || mode == 5
		|| mode == 8 || mode == 10;
}

bool material_uses_vertex_diffuse(std::uint8_t mode)
{
	// SRO_build_lod_mesh (LANCER.EXE 0x004a3040) writes material byte +4
	// for modes 0, 1, 2, 6, 7, 8, and 10. The srd3d one- and two-texture
	// submission paths replace the packed vertex diffuse value with white
	// whenever that byte is clear. In particular, modes 3, 4, and 5 are
	// textured but fully unlit; feeding them scene lighting is the source of
	// the characteristic blue tint in nebula missions.
	switch (mode)
	{
	case 0:
	case 1:
	case 2:
	case 6:
	case 7:
	case 8:
	case 10:
		return true;
	default:
		return false;
	}
}

std::uint64_t material_blend_state(std::uint8_t mode)
{
	// The retail D3DBLEND lookup tables initialized at
	// srd3d.dll 0x100058d2 are ONE/ZERO for byte 0, ONE/ONE for byte 1,
	// and SRCALPHA/INVSRCALPHA for byte 3.
	return retail_blend_state(
		mode == 5
			? RetailBlendSelector::source_alpha
			: RetailBlendSelector::additive);
}

bool cloak_node_eligible(
	std::uint16_t object_type,
	const char* node_name)
{
	if (object_type != 0x95u)
	{
		return true;
	}
	constexpr const char* excluded[] = {
		"Kaf bot vent",
		"Kaf comms twr",
		"Kaf ext vent",
		"Kaf frnt vent",
		"Kaf land plat",
		"Kaf shield gen",
		"Kaf top vent",
	};
	for (const char* name : excluded)
	{
		if (std::strcmp(node_name, name) == 0)
		{
			return false;
		}
	}
	return true;
}

glm::vec3 cloak_palette(float phase)
{
	const std::int32_t index = static_cast<std::int32_t>(
		std::nearbyint(phase * 1023.0f));
	const float p = static_cast<float>(index) / 1024.0f;
	const glm::vec3 red{1.0f, 0.0f, 0.0f};
	const glm::vec3 green{
		0.72265625f, 0.79296875f, 0.3203125f};
	const glm::vec3 blue{0.0f, 0.0f, 0.3125f};
	const auto ease = [](const glm::vec3& a,
		const glm::vec3& b,
		float t)
	{
		const float weight =
			0.5f * std::cos(glm::pi<float>() * t
				+ glm::pi<float>()) + 0.5f;
		return a + (b - a) * weight;
	};
	return p < 0.3f
		? ease(red, green, p / 0.3f)
		: ease(green, blue, (p - 0.3f) / 0.7f);
}

glm::mat3 cloak_wobble(
	const game::WorldObject& object,
	std::uint32_t simulation_tick)
{
	if (object.type == 0x95u
		|| object.cloak_phase == game::CloakPhase::none
		|| object.cloak_phase == game::CloakPhase::cloaked)
	{
		return glm::mat3{1.0f};
	}
	const float t = std::min(
		1.0f,
		// Cloak_wobble, LANCER.EXE 0x004639fa..0x00463a68, uses the
		// same 1/250 transition fraction as Cloak_update.
		static_cast<float>(
			simulation_tick - object.cloak_transition_tick)
			* 0.004f);
	const float envelope = std::sin(glm::pi<float>() * t);
	const float a = std::sin(20.0f * t) * envelope * 0.15f;
	const float b = std::sin(26.0f * t + 2.8f) * envelope * 0.15f;
	const float c = std::sin(14.0f * t + 0.9f) * envelope * 0.15f;
	glm::mat3 shear_a{1.0f};
	shear_a[0][1] = a;
	glm::mat3 shear_b{1.0f};
	shear_b[1][0] = b;
	glm::mat3 shear_c{1.0f};
	shear_c[2][0] = c;
	return shear_a * shear_b * shear_c;
}

glm::mat3 basis_from_forward(const glm::vec3& direction)
{
	const glm::vec3 forward = glm::normalize(direction);
	const glm::vec3 reference =
		std::abs(forward.y) < 0.95f
			? glm::vec3{0.0f, 1.0f, 0.0f}
			: glm::vec3{1.0f, 0.0f, 0.0f};
	const glm::vec3 right =
		glm::normalize(glm::cross(reference, forward));
	const glm::vec3 up = glm::normalize(glm::cross(forward, right));
	return {right, up, forward};
}

void submit_projectile_ribbon(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const glm::vec3& camera_position,
	const glm::vec3& position,
	const glm::mat3& orientation,
	float half_width,
	float length,
	std::uint32_t color,
	float roll = 0.0f,
	const glm::vec3& local_offset = glm::vec3{0.0f},
	float texture_scroll = 0.0f,
	const FrontendTexture* texture_override = nullptr,
	const std::uint32_t* corner_colors = nullptr,
	const glm::vec2* authored_uv = nullptr,
	bool crossed = true)
{
	const FrontendTexture& texture =
		texture_override == nullptr
			? renderer.laser_cannon_texture
			: *texture_override;
	const std::uint32_t vertex_count = crossed ? 12u : 6u;
	if (!bgfx::isValid(texture.handle)
		|| get_available_frame_vertices(frontend.frame_geometry,
			vertex_count, frontend.layout) < vertex_count)
	{
		return;
	}
	const glm::vec3 points[8] = {
		{0.0f, half_width, 0.0f},
		{0.0f, -half_width, 0.0f},
		{0.0f, -half_width, length},
		{0.0f, half_width, length},
		{half_width, 0.0f, 0.0f},
		{-half_width, 0.0f, 0.0f},
		{-half_width, 0.0f, length},
		{half_width, 0.0f, length},
	};
	constexpr std::uint8_t triangles[12] = {
		0, 1, 2, 0, 2, 3,
		4, 5, 6, 4, 6, 7,
	};
	constexpr glm::vec2 default_uv[4] = {
		{0.0f, 0.0f}, {1.0f, 0.0f},
		{1.0f, 1.0f}, {0.0f, 1.0f},
	};
	FrameVertexBuffer buffer;
	alloc_frame_vertex_buffer(frontend.frame_geometry,
		&buffer, vertex_count, frontend.layout);
	auto* vertices =
		reinterpret_cast<assets::GameplayVertex*>(buffer.data);
	for (std::uint32_t vertex = 0; vertex < vertex_count; ++vertex)
	{
		const std::uint8_t source = triangles[vertex];
		vertices[vertex] = {
			points[source].x,
			points[source].y,
			points[source].z,
			corner_colors == nullptr
				? color
				: corner_colors[source & 3u],
			(authored_uv == nullptr
				? default_uv[source & 3u]
				: authored_uv[source]).x,
			(authored_uv == nullptr
				? default_uv[source & 3u]
				: authored_uv[source]).y + texture_scroll,
		};
	}
	const glm::mat3 rolled = roll == 0.0f
		? orientation
		: sl_open::math::postrotate(
			orientation, roll, {0.0f, 0.0f, 1.0f});
	const glm::mat4 transform = sl_open::math::model_transform(
		rolled,
		1.0f,
		position - camera_position + orientation * local_offset);
	const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
	bgfx::setUniform(frontend.uv_rect_uniform, uv);
	bgfx::setUniform(frontend.tint_uniform, tint);
	bgfx::setTransform(glm::value_ptr(transform));
	set_frame_vertex_buffer(0, &buffer);
	bgfx::setTexture(
		0,
		frontend.texture_sampler,
		texture.handle);
	bgfx::setState(
		BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_DEPTH_TEST_GREATER
			| retail_blend_state(RetailBlendSelector::additive)
			| BGFX_STATE_MSAA);
	submit_retail_transparent(
		renderer, frontend.mission_rgba_program);
}

std::uint32_t packed_effect_color(
	float intensity,
	float alpha)
{
	const std::uint32_t component = static_cast<std::uint32_t>(
		std::clamp(std::lrint(intensity * 255.0f), 0l, 255l));
	const std::uint32_t opacity = static_cast<std::uint32_t>(
		std::clamp(std::lrint(alpha * 255.0f), 0l, 255l));
	return opacity << 24
		| component << 16
		| component << 8
		| component;
}

void projectile_uvs(
	std::uint8_t type,
	std::int16_t affiliation,
	std::uint32_t simulation_tick,
	std::uint32_t child,
	glm::vec2 (&uv)[8])
{
	// gun_projectile_create_type_visuals (0x0047d9a0) reads these exact
	// signed integer tables from 0x00500fb0/0x00500fec, scales them by
	// 1/256, and writes the A,A+B,A+B,A U pattern with the
	// high,high,low,low V pattern to both crossed planes.
	constexpr std::int16_t base[15] = {
		0, -1, 32, 64, 96, 224, 160, -1,
		192, 64, 64, 64, 32, 32, 32,
	};
	constexpr std::int16_t span[15] = {
		32, 32, 32, 32, 32, 32, 32, 32,
		32, 32, 32, 32, 32, 32, 0,
	};
	type = std::min<std::uint8_t>(type, 14);
	const float edge =
		static_cast<float>(base[type]) * (1.0f / 256.0f);
	const float center =
		static_cast<float>(base[type] + span[type])
			* (1.0f / 256.0f);
	// The second faction set selects the lower half of the authored gun
	// texture. The hardware factory uses these exact texel-center values.
	const bool alternate =
		affiliation != 0 || type == 12;
	const float high_v =
		alternate ? 255.0f / 256.0f : 127.0f / 256.0f;
	const float low_v = alternate ? 0.5f : 0.0f;
	const glm::vec2 ordinary[4] = {
		{edge, high_v},
		{center, high_v},
		{center, low_v},
		{edge, low_v},
	};
	std::copy(std::begin(ordinary), std::end(ordinary), uv);
	std::copy(std::begin(ordinary), std::end(ordinary), uv + 4);
	if (type == 12)
	{
		// TurretLaser_Mesh's second perpendicular plane has its own exact
		// atlas lane (0x0047e65f..0x0047e6fb).
		const float turret_high = affiliation == 0 ? 0.5f : 1.0f;
		const float turret_low =
			affiliation == 0 ? 0.375f : 0.875f;
		uv[4] = {0.875f, turret_high};
		uv[5] = {1.0f, turret_high};
		uv[6] = {1.0f, turret_low};
		uv[7] = {0.875f, turret_low};
	}
	else if (type == 8)
	{
		// Gattlingplasma rewrites the four child UV rectangles every frame
		// (0x0047baed..0x0047bc02). Child zero uses the 30x30 cell in
		// the middle band; the other three use 30x62 cells.
		const float frame_u =
			static_cast<float>((simulation_tick % 3u) << 5u)
				/ 256.0f;
		const float v_scale = child == 0 ? 30.0f : 62.0f;
		const float v_bias = child == 0 ? 0.25f : 0.0f;
		constexpr glm::vec2 source[4] = {
			{1.0f, 255.0f / 256.0f},
			{1.0f, 0.0f},
			{0.0f, 0.0f},
			{0.0f, 255.0f / 256.0f},
		};
		for (std::uint32_t vertex = 0; vertex < 4; ++vertex)
		{
			uv[vertex] = {
				(source[vertex].x * 30.0f + 0.5f) / 256.0f
					+ frame_u,
				(source[vertex].y * v_scale + 0.5f) / 256.0f
					+ v_bias,
			};
			uv[vertex + 4] = uv[vertex];
		}
	}
}

template<typename SubmitChildren>
void submit_instance_pass(
	const MissionRenderer& renderer,
	const MissionGpuModel& model,
	const MissionRenderInstance& instance,
	const FrontendRenderer& frontend,
	const FrontendTexture& cloak_texture,
	const MissionSceneLights& scene_lights,
	const MissionRenderFrame& frame,
	std::uint32_t simulation_tick,
	SubmitChildren&& submit_children);
void submit_model_with_locator_children(
	const MissionRenderer& renderer,
	const MissionGpuModel& model,
	const MissionRenderInstance& instance,
	const FrontendRenderer& frontend,
	const MissionSceneLights& scene_lights,
	const MissionRenderFrame& frame,
	bool submit_locator_light_flares,
	bool animate_runtime,
	std::uint32_t depth);

void submit_billboard(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const FrontendTexture& texture,
	const glm::vec3& camera_position,
	const glm::vec3& position,
	float size,
	std::uint32_t color,
	float u0,
	float v0,
	float u1,
	float v1,
	const glm::mat3& camera_orientation,
	float height_scale = 1.0f,
	RetailBlendSelector blend = RetailBlendSelector::additive,
	float depth_bias = 0.0f);
void electric_ray_midpoint_displace(
	game::World& world,
	glm::vec3 (&points)[17],
	std::uint32_t left,
	std::uint32_t right,
	float displacement,
	std::int32_t depth);
void submit_electric_ray_segment(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const FrontendTexture& texture,
	const glm::vec3& camera_position,
	const glm::vec3& position,
	const glm::mat3& orientation,
	float radius,
	float length,
	std::uint32_t color);

void submit_huge_gun_polyhedron(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const glm::vec3& camera_position,
	const glm::vec3& position,
	const glm::mat3& orientation,
	float extent,
	float fade)
{
	// AlliedHugeGun/CoalitionHugeGun prototype constructors
	// (0x004801b0/0x00480420) emit twelve vertices: three mutually
	// perpendicular four-vertex planes with all coordinates at +/-extent.
	if (!bgfx::isValid(renderer.huge_gun_texture.handle)
		|| get_available_frame_vertices(frontend.frame_geometry,
			18, frontend.layout) < 18)
	{
		return;
	}
	const glm::vec3 points[12] = {
		{0.0f, extent, -extent},
		{0.0f, -extent, -extent},
		{0.0f, -extent, extent},
		{0.0f, extent, extent},
		{extent, 0.0f, -extent},
		{-extent, 0.0f, -extent},
		{-extent, 0.0f, extent},
		{extent, 0.0f, extent},
		{-extent, -extent, 0.0f},
		{extent, -extent, 0.0f},
		{extent, extent, 0.0f},
		{-extent, extent, 0.0f},
	};
	constexpr std::uint8_t triangles[18] = {
		0, 1, 2, 0, 2, 3,
		4, 5, 6, 4, 6, 7,
		8, 9, 10, 8, 10, 11,
	};
	constexpr glm::vec2 coordinates[4] = {
		{0.0f, 0.0f}, {1.0f, 0.0f},
		{1.0f, 1.0f}, {0.0f, 1.0f},
	};
	FrameVertexBuffer buffer;
	alloc_frame_vertex_buffer(frontend.frame_geometry,
		&buffer, 18, frontend.layout);
	auto* vertices =
		reinterpret_cast<assets::GameplayVertex*>(buffer.data);
	const std::uint32_t color =
		packed_effect_color(fade, fade);
	for (std::uint32_t index = 0; index < 18; ++index)
	{
		const std::uint8_t source = triangles[index];
		vertices[index] = {
			points[source].x,
			points[source].y,
			points[source].z,
			color,
			coordinates[source & 3u].x,
			coordinates[source & 3u].y,
		};
	}
	const glm::mat4 transform = sl_open::math::model_transform(
		orientation, 1.0f, position - camera_position);
	const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
	bgfx::setUniform(frontend.uv_rect_uniform, uv);
	bgfx::setUniform(frontend.tint_uniform, tint);
	bgfx::setTransform(glm::value_ptr(transform));
	set_frame_vertex_buffer(0, &buffer);
	bgfx::setTexture(
		0,
		frontend.texture_sampler,
		renderer.huge_gun_texture.handle);
	bgfx::setState(
		BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_DEPTH_TEST_GREATER
			| retail_blend_state(RetailBlendSelector::additive)
			| BGFX_STATE_MSAA);
	submit_retail_transparent(
		renderer, frontend.mission_rgba_program);
}

void submit_oriented_effect_quad(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const FrontendTexture& texture,
	const glm::vec3& camera_position,
	const glm::vec3& position,
	const glm::mat3& orientation,
	float half_width,
	float half_height,
	std::uint32_t color,
	float u0 = 0.0f,
	float v0 = 0.0f,
	float u1 = 1.0f,
	float v1 = 1.0f)
{
	if (!bgfx::isValid(texture.handle)
		|| get_available_frame_vertices(frontend.frame_geometry,
			6, frontend.layout) < 6)
	{
		return;
	}
	const glm::vec3 points[4] = {
		{-half_width, -half_height, 0.0f},
		{half_width, -half_height, 0.0f},
		{half_width, half_height, 0.0f},
		{-half_width, half_height, 0.0f},
	};
	constexpr std::uint8_t order[6] = {0, 1, 2, 0, 2, 3};
	const glm::vec2 uv[4] = {
		{u0, v0}, {u1, v0},
		{u1, v1}, {u0, v1},
	};
	FrameVertexBuffer buffer;
	alloc_frame_vertex_buffer(frontend.frame_geometry,
		&buffer, 6, frontend.layout);
	auto* vertices =
		reinterpret_cast<assets::GameplayVertex*>(buffer.data);
	for (std::uint32_t index = 0; index < 6; ++index)
	{
		const std::uint8_t source = order[index];
		vertices[index] = {
			points[source].x,
			points[source].y,
			points[source].z,
			color,
			uv[source].x,
			uv[source].y,
		};
	}
	const glm::mat4 transform = sl_open::math::model_transform(
		orientation, 1.0f, position - camera_position);
	const float full_uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
	bgfx::setUniform(frontend.uv_rect_uniform, full_uv);
	bgfx::setUniform(frontend.tint_uniform, tint);
	bgfx::setTransform(glm::value_ptr(transform));
	set_frame_vertex_buffer(0, &buffer);
	bgfx::setTexture(
		0, frontend.texture_sampler, texture.handle);
	bgfx::setState(
		BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_DEPTH_TEST_GREATER
			| retail_blend_state(RetailBlendSelector::additive)
			| BGFX_STATE_MSAA);
	submit_retail_transparent(
		renderer, frontend.mission_rgba_program);
}

void submit_muzzle_flashes(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	for (std::uint32_t index = 0;
		index < frame.muzzle_flash_count;
		++index)
	{
		const MissionMuzzleFlash& flash =
			frame.muzzle_flashes[index];
		const float duration = static_cast<float>(
			flash.expiration_tick - flash.start_tick);
		const float fade = duration > 0.0f
			? std::clamp(
				static_cast<float>(
					flash.expiration_tick
						- frame.simulation_tick) / duration,
				0.0f,
				1.0f)
			: 0.0f;
		const std::uint32_t color =
			packed_effect_color(fade, fade);
		// The shared muzzle prototype at 0x004786e0 copies the exact
		// (30,30,1200) source vector, doubles X/Y, halves Z, and emits
		// one front plane plus three radial ribbons 120 degrees apart.
		const FrontendTexture& front_texture =
			flash.type == 8
				? renderer.sfx_alpha_texture
				: renderer.muzzle_flare_a_texture;
		const FrontendTexture& ribbon_texture =
			flash.type == 8
				? renderer.sfx_alpha_texture
				: renderer.muzzle_flare_b_texture;
		submit_oriented_effect_quad(
			renderer,
			frontend,
			front_texture,
			frame.camera_position,
			flash.position,
			flash.orientation,
			60.0f,
			60.0f,
			color);
		for (std::uint32_t plane = 0; plane < 3; ++plane)
		{
			submit_projectile_ribbon(
				renderer,
				frontend,
				frame.camera_position,
				flash.position,
				flash.orientation,
				60.0f,
				600.0f,
				color,
				static_cast<float>(plane)
					* glm::two_pi<float>() / 3.0f,
				glm::vec3{0.0f},
				0.0f,
				&ribbon_texture,
				nullptr,
				nullptr,
				false);
		}
	}
}

void submit_ion_cannon_effects(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	if (frame.world == nullptr)
	{
		return;
	}
	const auto model_frame = [&frame](
		const game::WorldObject& object,
		std::uint16_t reference)
	{
		const glm::mat4 root = math::model_transform(
			object.scene_orientation, 1.0f, object.scene_position);
		return reference < object.model_references.size()
			? explosion_portal_root(
				frame, object, reference, root, 1.0f)
				* object.model_references[reference].scene_transform
			: root;
	};
	const auto locator_frame = [&](
		const game::WorldObject& object,
		std::uint16_t reference,
		std::int16_t type,
		std::uint16_t ordinal)
	{
		const assets::GameplayLocator* locator =
			game::model_animation_find_locator(
				object, reference, type, ordinal);
		return locator == nullptr
			? model_frame(object, reference)
			: explosion_portal_root(
				frame,
				object,
				reference,
				math::model_transform(
					object.scene_orientation,
					1.0f,
					object.scene_position),
				1.0f)
				* object.model_references[reference].scene_transform
				* glm::translate(glm::mat4{1.0f}, locator->position)
				* glm::mat4(locator->basis);
	};
	const auto radial_mesh = [&](
		const glm::mat4& transform,
		std::uint32_t planes,
		float radius,
		float half_length,
		const FrontendTexture& texture,
		float uv_scroll = 0.0f,
		bool centered = true)
	{
		for (std::uint32_t plane = 0; plane < planes; ++plane)
		{
			submit_projectile_ribbon(
				renderer,
				frontend,
				frame.camera_position,
				glm::vec3(transform[3]),
				glm::mat3(transform),
				radius,
				half_length * 2.0f,
				0xffffffffu,
				static_cast<float>(plane)
					* glm::pi<float>()
					/ static_cast<float>(planes),
				centered
					? glm::vec3{0.0f, 0.0f, -half_length}
					: glm::vec3{0.0f},
				uv_scroll,
				&texture,
				nullptr,
				nullptr,
				false);
		}
	};
	const auto ray = [&](
		const glm::vec3& start,
		const glm::vec3& end,
		float displacement,
		float radius,
		const glm::vec3& color,
		float alpha)
	{
		glm::vec3 points[17]{};
		points[0] = start;
		points[16] = end;
		electric_ray_midpoint_displace(
			*frame.world, points, 0, 16, displacement, 4);
		const auto channel = [](float value)
		{
			return static_cast<std::uint32_t>(
				std::clamp(std::lrint(value * 255.0f), 0l, 255l));
		};
		const std::uint32_t packed =
			channel(alpha) << 24
			| channel(color.b) << 16
			| channel(color.g) << 8
			| channel(color.r);
		for (std::uint32_t segment = 0; segment < 16; ++segment)
		{
			const glm::vec3 delta =
				points[segment + 1] - points[segment];
			const float length = glm::length(delta);
			if (length == 0.0f)
			{
				continue;
			}
			submit_electric_ray_segment(
				renderer,
				frontend,
				renderer.ion_beam_texture,
				frame.camera_position,
				points[segment],
				basis_from_forward(delta),
				radius,
				length,
				packed);
		}
	};
	for (const game::WorldObject& actor : frame.world->objects)
	{
		if (!actor.active || actor.ai.command_count == 0
			|| actor.ai.commands[0].id != 110
			|| static_cast<std::uint16_t>(
				&actor - std::begin(frame.world->objects))
				!= frame.world->ion_cannon_effect_owner)
		{
			continue;
		}
		const ai::IonCannonWork& ion = actor.ai.work.ion_cannon;
		if (!ion.initialized
			|| ion.target_object >= game::kMaxGameObjects)
		{
			continue;
		}
		const game::WorldObject& target =
			frame.world->objects[ion.target_object];
		if (!target.active)
		{
			continue;
		}
		if (ion.focus_active && ion.target_field_fraction > 0.0f)
		{
			const glm::mat4 focus =
				locator_frame(actor, ion.emitter_model, 15, 0);
			float length = glm::distance(
				glm::vec3(focus[3]), target.scene_position);
			if (ion.target_field_fraction < 0.3f)
			{
				length *= ion.target_field_fraction
					* 3.3333332538604736f;
			}
			radial_mesh(
				focus,
				3,
				ion.focus_radius,
				length * 0.5f,
				renderer.nova_cannon_texture,
				0.0f,
				false);
		}

		const std::uint32_t duration =
			ion.stage == 3u ? 300u
				: ion.stage == 4u ? 150u
				: ion.stage == 5u ? 200u
				: ion.stage == 6u ? 100u
				: 1u;
		const float fraction = std::clamp(
			static_cast<float>(
				frame.simulation_tick - ion.stage_start_tick)
				/ static_cast<float>(duration),
			0.0f,
			1.0f);
		if (actor.type != 0xa5u && ion.stage >= 4u
			&& ion.stage <= 6u)
		{
			for (std::uint32_t index = 0;
				index < ion.plasma_count;
				++index)
			{
				const assets::GameplayLocator* locator =
					game::model_animation_find_locator(
						actor,
						ion.plasma_model,
						12,
						static_cast<std::uint16_t>(index));
				const glm::mat4 emitter = locator_frame(
					actor,
					ion.plasma_model,
					12,
					static_cast<std::uint16_t>(index));
				glm::vec3 local_end =
					locator == nullptr
						? glm::vec3{0.0f, 0.0f, 3000.0f}
						: locator->dimensions;
				if (local_end == glm::vec3{0.0f})
				{
					local_end = {0.0f, 0.0f, 3000.0f};
				}
				ray(
					glm::vec3(emitter[3]),
					glm::vec3(
						emitter * glm::vec4(local_end, 1.0f)),
					0.1f,
					2000.0f,
					{0.0f, 0.0f,
						ion.stage == 6u
							? 1.0f - fraction : 1.0f},
					0.5f);
			}
		}
		if (actor.type != 0xa5u && ion.stage >= 3u
			&& ion.stage <= 6u)
		{
			const std::uint32_t visual_count =
				actor.type == 0x48u ? 3u : 5u;
			const std::uint32_t first =
				ion.stage == 3u
					? std::min<std::uint32_t>(
						static_cast<std::uint32_t>(
							fraction / 0.2f),
						visual_count - 1u)
					: 0u;
			const std::uint32_t last =
				ion.stage == 3u ? first + 1u : visual_count;
			for (std::uint32_t index = first;
				index < last;
				++index)
			{
				const glm::vec3 position{
					locator_frame(
						actor,
						ion.lower_model,
						13,
						static_cast<std::uint16_t>(index))[3]};
				submit_billboard(
					renderer,
					frontend,
					renderer.ion_cannon_texture,
					frame.camera_position,
					position,
					4000.0f,
					0xffffffffu,
					0.0f, 0.0f, 1.0f, 1.0f,
					frame.camera_orientation);
			}
		}
		if (ion.beam_active && actor.type != 0xa5u
			&& ion.stage >= 5u && ion.stage <= 6u)
		{
			glm::mat4 beam =
				locator_frame(actor, ion.beam_model, 11, 0);
			const assets::GameplayLocator* beam_locator =
				game::model_animation_find_locator(
					actor, ion.beam_model, 11, 0);
			if (beam_locator != nullptr)
			{
				beam[3] = beam
					* glm::vec4(
						beam_locator->dimensions * 0.5f,
						1.0f);
			}
			beam = beam * glm::rotate(
				glm::mat4{1.0f},
				glm::half_pi<float>(),
				glm::vec3{1.0f, 0.0f, 0.0f});
			radial_mesh(
				beam,
				4,
				ion.beam_radius,
				ion.beam_half_length,
				renderer.ion_cannon_texture,
				static_cast<float>(frame.simulation_tick)
					* 0.006000000052154064f);
			for (std::uint32_t index = 0; index < 5; ++index)
			{
				const float phase =
					std::fmod(
						static_cast<float>(index) * 0.2f
							+ fraction * 2.0f,
						1.0f);
				const glm::mat3 orientation =
					glm::mat3(beam)
					* glm::mat3(glm::rotate(
						glm::mat4{1.0f},
						phase * glm::two_pi<float>(),
						glm::vec3{0.0f, 0.0f, 1.0f}));
				submit_oriented_effect_quad(
					renderer,
					frontend,
					renderer.ion_field_texture,
					frame.camera_position,
					glm::vec3(beam[3]),
					orientation,
					1500.0f,
					1500.0f,
					0xffffffffu);
			}
		}
		if (ion.impact_active)
		{
			const glm::vec3 impact_start{
				locator_frame(
					actor, ion.impact_origin_model, 14, 0)[3]};
			const glm::vec3 impact_colors[3] = {
				actor.type == 0xa5u
					? glm::vec3{0.5f, 0.0f, 1.0f}
					: glm::vec3{1.0f},
				{0.3f, 0.0f, 1.0f},
				actor.type == 0xa5u
					? glm::vec3{0.7f, 0.0f, 1.0f}
					: glm::vec3{0.3f, 0.0f, 1.0f},
			};
			for (const glm::vec3& color : impact_colors)
			{
				ray(
					impact_start,
					target.scene_position,
					0.2f,
					2000.0f,
					color,
					0.5f);
			}
		}
	}
}

void submit_gun_projectiles(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame,
	const MissionSceneLights& scene_lights)
{
	for (std::uint32_t index = 0;
		index < frame.gun_projectile_count;
		++index)
	{
		const MissionGunProjectile& projectile =
			frame.gun_projectiles[index];
		const float lifetime = static_cast<float>(
			projectile.expiration_tick - projectile.spawn_tick);
		const float fade = lifetime > 0.0f
			? std::clamp(
				static_cast<float>(
					projectile.expiration_tick
						- frame.simulation_tick) / lifetime,
				0.0f,
				1.0f)
			: 0.0f;
		const std::uint32_t color =
			packed_effect_color(fade, fade);
		const float age = static_cast<float>(
			frame.simulation_tick - projectile.spawn_tick);
		const auto projectile_lod = [&](float final_threshold)
		{
			// SR_lod_select (0x004c5fb0) divides camera-space Z by the
			// adaptive global detail multiplier, then selects the first
			// mesh whose authored threshold is greater than that value.
			const float view_depth = glm::dot(
				projectile.position - frame.camera_position,
				frame.camera_orientation[2]);
			const float distance =
				view_depth / renderer.lod_detail_scale;
			if (distance < 15000.0f)
			{
				return 1;
			}
			return distance < final_threshold ? 0 : -1;
		};
		glm::mat3 animated_orientation = projectile.orientation;
		switch (projectile.type_index)
		{
		case 1:
			animated_orientation = sl_open::math::postrotate(
				animated_orientation,
				age * 0.11999999731779099f,
				{1.0f, 0.0f, 0.0f});
			animated_orientation = sl_open::math::postrotate(
				animated_orientation,
				age * 0.019999999552965164f,
				{0.0f, 1.0f, 0.0f});
			animated_orientation = sl_open::math::postrotate(
				animated_orientation,
				age * 0.10000000149011612f,
				{0.0f, 0.0f, 1.0f});
			break;
		case 4:
		case 5:
			animated_orientation = sl_open::math::postrotate(
				animated_orientation,
				age * 0.4000000059604645f,
				{0.0f, 0.0f, 1.0f});
			break;
		case 7:
			animated_orientation = sl_open::math::postrotate(
				animated_orientation,
				age * 0.20000000298023224f,
				{0.0f, 0.0f, 1.0f});
			break;
		case 13:
		case 14:
			animated_orientation = sl_open::math::postrotate(
				animated_orientation,
				age * 0.800000011920929f,
				{1.0f, 0.0f, 0.0f});
			animated_orientation = sl_open::math::postrotate(
				animated_orientation,
				age * 0.30000001192092896f,
				{0.0f, 1.0f, 0.0f});
			animated_orientation = sl_open::math::postrotate(
				animated_orientation,
				age * 0.10000000149011612f,
				{0.0f, 0.0f, 1.0f});
			break;
		default:
			break;
		}
		const auto ribbon = [&](
			const glm::vec3& position,
			const glm::mat3& orientation,
			float half_width,
			float length,
			float roll = 0.0f,
			const glm::vec3& local_offset = glm::vec3{0.0f},
			float texture_scroll = 0.0f,
			const FrontendTexture* texture = nullptr,
			std::uint32_t child = 0u,
			bool crossed = true)
		{
			glm::vec2 uv[8];
			projectile_uvs(
				projectile.type_index,
				projectile.shooter_affiliation,
				frame.simulation_tick,
				child,
				uv);
			submit_projectile_ribbon(
				renderer,
				frontend,
				frame.camera_position,
				position,
				orientation,
				half_width,
				length,
				color,
				roll,
				local_offset,
				texture_scroll,
				texture,
				nullptr,
				uv,
				crossed);
		};
		switch (projectile.type_index)
		{
		case 0:
		{
			// LaserCannon_Mesh switches from two perpendicular planes to
			// its authored one-plane LOD at 15,000, then culls at 100,000.
			const int lod = projectile_lod(100000.0f);
			if (lod >= 0)
			{
				ribbon(
					projectile.position,
					projectile.orientation,
					30.0f, 1200.0f,
					0.0f, {}, 0.0f, nullptr, 0u, lod != 0);
			}
			break;
		}
		case 1:
		{
			// PulseCannon BMO1/BMO2 are camera-facing BMO primitives,
			// not longitudinal meshes. Their +0x20/+0x24 fields are
			// half extents 60 and 30 (SR_bmopipe at 0x004ce4d0).
			const FrontendTexture& pulse =
				renderer.pulse_cannon_texture[
					projectile.shooter_affiliation == 0 ? 0 : 1];
			submit_billboard(
				renderer,
				frontend,
				pulse,
				frame.camera_position,
				projectile.position,
				120.0f,
				color,
				0.0f, 0.0f, 1.0f, 1.0f,
				frame.camera_orientation);
			submit_billboard(
				renderer,
				frontend,
				pulse,
				frame.camera_position,
				projectile.position
					+ animated_orientation
						* glm::vec3{55.0f, 0.0f, 0.0f},
				60.0f,
				color,
				0.0f, 0.0f, 1.0f, 1.0f,
				frame.camera_orientation);
			break;
		}
		case 2:
			for (std::uint32_t child = 0; child < 3; ++child)
			{
				// The four Messon prototypes are
				// (5,5,40+400*n); the factory instantiates the first
				// three, translates local X by 20 and local Z by a
				// retained random [0,300], then applies a random roll.
				ribbon(
					projectile.position,
					animated_orientation,
					5.0f,
					40.0f + 400.0f * child,
					projectile.visual_random[child * 2 + 1]
						* glm::two_pi<float>(),
					{20.0f, 0.0f,
						projectile.visual_random[child * 2]
							* 300.0f});
			}
			break;
		case 3:
		{
			const int lod = projectile_lod(100000.0f);
			if (lod >= 0)
			{
				ribbon(
					projectile.position,
					projectile.orientation,
					50.0f, 1400.0f,
					0.0f, {}, 0.0f, nullptr, 0u, lod != 0);
			}
			break;
		}
		case 4:
			for (std::uint32_t child = 0; child < 3; ++child)
			{
				const float angle =
					child * glm::two_pi<float>() / 3.0f;
				ribbon(
					projectile.position,
					animated_orientation,
					30.0f, 1200.0f,
					angle,
					{std::cos(angle) * 30.0f,
						std::sin(angle) * 30.0f, 0.0f});
			}
			break;
		case 5:
		{
			// TachyonCannon_mesh1 is generated by the three-plane helper
			// at 0x004adf90 with radius 60, Z=-800..800 and this fixed
			// atlas rectangle. It does not receive the ordinary per-gun
			// UV override used by flags 0x200000.
			glm::vec2 tachyon_uv[8] = {
				{1.0f, 0.24f},
				{1.0f, 0.0f},
				{0.875f, 0.0f},
				{0.875f, 0.24f},
				{}, {}, {}, {},
			};
			std::copy(
				std::begin(tachyon_uv),
				std::begin(tachyon_uv) + 4,
				tachyon_uv + 4);
			for (std::uint32_t plane = 0; plane < 3; ++plane)
			{
				submit_projectile_ribbon(
					renderer,
					frontend,
					frame.camera_position,
					projectile.position,
					animated_orientation,
					60.0f,
					1600.0f,
					color,
					static_cast<float>(plane)
						* glm::two_pi<float>() / 3.0f,
					{0.0f, 0.0f, -800.0f},
					0.0f,
					nullptr,
					nullptr,
					tachyon_uv,
					false);
			}
			// TachyonCannon_mesh2 is the authored local-XY quad from
			// 0x0047f612..0x0047f6bb, translated +100 on local Z.
			submit_oriented_effect_quad(
				renderer,
				frontend,
				renderer.laser_cannon_texture,
				frame.camera_position,
				projectile.position
					+ animated_orientation
						* glm::vec3{0.0f, 0.0f, 100.0f},
				animated_orientation,
				100.0f,
				100.0f,
				color,
				0.875f,
				0.251953125f,
				1.0f,
				0.375f);
			break;
		}
		case 6:
		{
			const int lod = projectile_lod(10000000.0f);
			if (lod >= 0)
			{
				ribbon(
					projectile.position,
					projectile.orientation,
					80.0f, 1500.0f,
					projectile.visual_random[0],
					{}, 0.0f, nullptr, 0u, lod != 0);
			}
			break;
		}
		case 7:
		{
			const FrontendTexture& collapser =
				renderer.collapser_cannon_texture[
					projectile.shooter_affiliation == 0 ? 0 : 1];
			for (const float x : {-30.0f, 30.0f})
			{
				submit_billboard(
					renderer,
					frontend,
					collapser,
					frame.camera_position,
					projectile.position
						+ animated_orientation
							* glm::vec3{x, 0.0f, 0.0f},
					96.0f,
					color,
					0.0f, 0.0f, 1.0f, 1.0f,
					frame.camera_orientation);
			}
			break;
		}
		case 8:
			for (std::uint32_t child = 0; child < 4; ++child)
			{
				const std::uint32_t sample = child * 3;
				const float angle =
					projectile.visual_random[sample + 2]
						* glm::two_pi<float>();
				ribbon(
					projectile.position,
					projectile.orientation,
					40.0f,
					200.0f + 150.0f * child,
					angle,
					{10.0f
							+ projectile.visual_random[sample + 1]
								* 20.0f,
						0.0f,
						projectile.visual_random[sample]
							* 200.0f},
					0.0f,
					nullptr,
					child);
			}
			break;
		case 9:
		{
			// Vulcanbattery's factory writes these four exact offsets.
			constexpr glm::vec3 offsets[4] = {
				{-50.0f, -12.0f, 0.0f},
				{50.0f, -12.0f, 0.0f},
				{-50.0f, 12.0f, 0.0f},
				{50.0f, 12.0f, 0.0f},
			};
			for (const glm::vec3& offset : offsets)
			{
				ribbon(
					projectile.position,
					projectile.orientation,
					40.0f, 300.0f,
					age * (offset.y < 0.0f
						? 0.10000000149011612f
						: -0.10000000149011612f),
					offset);
			}
			break;
		}
		case 10:
			ribbon(
				projectile.position,
				animated_orientation,
				180.0f, 10000.0f,
				glm::quarter_pi<float>());
			break;
		case 11:
		{
			// gun_flak_resource_init (0x00479140) binds object type
			// 0xb1, shell.shp, as the projectile visual.
			const MissionGpuModel& shell = renderer.models[0xb1];
			MissionRenderInstance instance;
			instance.position = projectile.position;
			instance.orientation = projectile.orientation;
			instance.direct_mesh = true;
			submit_model_with_locator_children(
				renderer,
				shell,
				instance,
				frontend,
				scene_lights,
				frame,
				true,
				false,
				0);
			break;
		}
		case 12:
			ribbon(
				projectile.position,
				projectile.orientation,
				32.0f, 1200.0f);
			break;
		case 13:
		case 14:
		{
			const bool allied = projectile.type_index == 13;
			const float extent = allied ? 700.0f : 1700.0f;
			submit_huge_gun_polyhedron(
				renderer,
				frontend,
				frame.camera_position,
				projectile.position,
				animated_orientation,
				extent,
				fade);
			const glm::vec3 flare_color = allied
				? glm::vec3{0.2f, 0.3f, 0.3f}
				: glm::vec3{0.6f, 0.4f, 0.1f};
			const std::uint32_t flare =
				static_cast<std::uint32_t>(
					std::lrint(flare_color.b * fade * 255.0f))
					<< 16
				| static_cast<std::uint32_t>(
					std::lrint(flare_color.g * fade * 255.0f))
					<< 8
				| static_cast<std::uint32_t>(
					std::lrint(flare_color.r * fade * 255.0f))
				| static_cast<std::uint32_t>(
					std::lrint(fade * 255.0f)) << 24;
			submit_billboard(
				renderer,
				frontend,
				renderer.huge_gun_texture,
				frame.camera_position,
				projectile.position,
				allied ? 5000.0f : 7500.0f,
				flare,
				0.0f, 0.0f, 1.0f, 1.0f,
				frame.camera_orientation);
			break;
		}
		default:
			break;
		}
	}
}

void submit_nova_beams(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	for (std::uint32_t beam_index = 0;
		beam_index < frame.nova_beam_count;
		++beam_index)
	{
		const MissionNovaBeam& beam =
			frame.nova_beams[beam_index];
		const std::uint32_t remaining =
			beam.expiration_tick > frame.simulation_tick
				? beam.expiration_tick - frame.simulation_tick
				: 0;
		const float progress = 1.0f - std::min(
			1.0f,
			static_cast<float>(remaining)
				* 0.012500000186264515f);
		const float segment_scale =
			progress <= 0.5f
				? std::sqrt(std::max(0.0f, progress * 2.0f))
				: 1.0f
					- std::pow(
						(progress - 0.5f) * 2.0f,
						2.0f);

		// nova_cannon_effects_update builds 31 points through
		// 0x0047b100. Its exact local curve is:
		// angle=(i*.03+progress-.3)*12*pi, radius=100,
		// z=max(450,(i*.03+progress-.3)*15000+600).
		glm::vec3 points[31];
		for (std::uint32_t index = 0; index < 31; ++index)
		{
			const float argument =
				static_cast<float>(index)
					* 0.029999999329447746f
				+ progress
				- 0.30000001192092896f;
			const float angle =
				argument * 37.69911193847656f;
			const glm::vec3 local{
				std::sin(angle) * 100.0f,
				std::cos(angle) * 100.0f,
				std::max(
					450.0f,
					argument * 15000.0f + 600.0f),
			};
			points[index] =
				beam.start + beam.orientation * local;
		}
		const std::uint32_t color =
			beam.fully_charged
				? 0xffffffffu
				: 0xff80c0ffu;
		for (std::uint32_t segment = 0;
			segment < 30;
			++segment)
		{
			const glm::vec3 delta =
				points[segment + 1] - points[segment];
			const float length = glm::length(delta);
			if (!(length > 0.0f))
			{
				continue;
			}
			submit_projectile_ribbon(
				renderer,
				frontend,
				frame.camera_position,
				points[segment],
				basis_from_forward(delta),
				150.0f * segment_scale,
				length * segment_scale,
				color,
				0.0f,
				glm::vec3{0.0f},
				0.0f,
				&renderer.nova_cannon_texture);
			if (beam.fully_charged)
			{
				// nova_cannon_effects_update submits every segment a
				// second time when slot +0x7c is set.
				submit_projectile_ribbon(
					renderer,
					frontend,
					frame.camera_position,
					points[segment],
					basis_from_forward(delta),
					150.0f * segment_scale,
					length * segment_scale,
					color,
					0.0f,
					glm::vec3{0.0f},
					0.0f,
					&renderer.nova_cannon_texture);
			}
		}

		// nova_cannon_fire_charged_beam creates four longitudinal planes
		// from eight radial points at pi/4 increments. The source shape is
		// 40,000 units long, centered at local Z=20,680. During the final
		// sixteen ticks 0x00480690 scales it linearly to zero.
		const float remaining_fraction =
			std::min(
				1.0f,
				static_cast<float>(remaining)
					* 0.012500000186264515f);
		const float core_scale =
			remaining_fraction <= 0.20000000298023224f
				? remaining_fraction * 5.0f
				: 1.0f;
		const float core_radius =
			beam.charge * beam.charge * 150.0f * core_scale;
		const glm::vec3 core_center =
			beam.start + beam.orientation[2] * 20680.0f;
		const glm::vec3 core_start =
			core_center
				- beam.orientation[2]
					* (20000.0f * core_scale);
		for (std::uint32_t plane = 0; plane < 4; ++plane)
		{
			const std::uint32_t core_colors[4] = {
				packed_effect_color(core_scale, core_scale),
				packed_effect_color(core_scale, core_scale),
				0,
				0,
			};
			submit_projectile_ribbon(
				renderer,
				frontend,
				frame.camera_position,
				core_start,
				beam.orientation,
				core_radius,
				40000.0f * core_scale,
				color,
				static_cast<float>(plane)
					* glm::quarter_pi<float>(),
				glm::vec3{0.0f},
				-static_cast<float>(frame.simulation_tick)
					* static_cast<float>(plane)
					* 0.009999999776482582f,
				&renderer.ion_cannon_texture,
				core_colors);
		}
	}
}

std::uint32_t shield_vertex_color(const glm::vec3& color)
{
	const std::uint8_t red = static_cast<std::uint8_t>(
		std::clamp(std::lrint(color.r * 255.0f), 0l, 255l));
	const std::uint8_t green = static_cast<std::uint8_t>(
		std::clamp(std::lrint(color.g * 255.0f), 0l, 255l));
	const std::uint8_t blue = static_cast<std::uint8_t>(
		std::clamp(std::lrint(color.b * 255.0f), 0l, 255l));
	return 0xff000000u
		| static_cast<std::uint32_t>(blue) << 16
		| static_cast<std::uint32_t>(green) << 8
		| red;
}

bool submit_spherical_shields(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	if (frame.world == nullptr)
	{
		return true;
	}
	// Shield_init (LANCER.EXE 0x0049eea9..0x0049eec5) selects one of
	// the three exact cutoff rows at 0x0050887c using Gdetail.
	constexpr float cutoffs[3][game::kShieldLodCount] = {
		{1250.0f, 2500.0f, 5000.0f,
			10000.0f, 20000.0f, 40000.0f},
		{2500.0f, 5000.0f, 10000.0f,
			20000.0f, 40000.0f, 80000.0f},
		{10000.0f, 20000.0f, 40000.0f,
			80000.0f, 160000.0f, 320000.0f},
	};
	const std::uint32_t quality = renderer.graphics_quality;
	for (std::uint16_t object_index = 0;
		object_index < game::kMaxGameObjects;
		++object_index)
	{
		game::WorldObject& object =
			frame.world->objects[object_index];
		game::ShieldSphereState& state =
			frame.world->shields.sphere[object_index];
		if (!object.active
			|| object.type == 1001
			|| !state.active
			|| state.last_hit_tick == 0
			|| frame.simulation_tick - state.last_hit_tick > 100)
		{
			continue;
		}
		const float distance =
			glm::distance(
				object.scene_position, frame.camera_position);
		std::uint32_t lod = 0;
		while (lod < game::kShieldLodCount
			&& !(distance < cutoffs[quality][lod]))
		{
			++lod;
		}
		if (lod == game::kShieldLodCount)
		{
			// Shield_update_all returns here, skipping later objects and
			// the complete CapShield pass.
			return false;
		}
		state.lod = static_cast<std::uint8_t>(lod);
		if (object.player
			&& (object.runtime_flags & game::kObjectFlagRenderSuppressed) != 0)
		{
			continue;
		}
		const game::ShieldSphereGeometry& geometry =
			game::shield_sphere_geometry(lod);
		// HShield_render_callback (0x0049f450) runs only after the BMO is
		// submitted. Its first callback uses zero elapsed time, and the
		// animator visits only the active geometry's point count.
		const std::uint32_t elapsed = state.last_render_tick == 0
			? 0
			: frame.simulation_tick - state.last_render_tick;
		state.last_render_tick = frame.simulation_tick;
		// Shield_sphere_instance_animate (0x0049e7d0) freezes the hit
		// histories during flicker, including the update that expires it.
		const bool flicker = state.flicker_until_tick >= 0;
		bool forcefield_texture = flicker;
		bool bright_flicker = false;
		if (flicker)
		{
			if (static_cast<std::uint32_t>(state.flicker_until_tick)
				< frame.simulation_tick)
			{
				state.flicker_until_tick = -1;
				forcefield_texture = false;
			}
			bright_flicker = (game::world_rand15(*frame.world) & 3u) == 0;
		}
		else
		{
			for (auto& buffer : state.hit)
			{
				for (std::uint32_t vertex = 0;
					vertex < geometry.point_count;
					++vertex)
				{
					buffer[vertex] = std::max(
						0.0f,
						buffer[vertex]
							- static_cast<float>(elapsed) * 0.025f);
				}
			}
		}
		for (std::uint32_t vertex = 0;
			vertex < geometry.point_count;
			++vertex)
		{
			glm::vec2& coordinate = state.uv[vertex];
			glm::vec2 relative =
				coordinate
					- glm::vec2{
						state.uv_center_x,
						state.uv_center_y};
			const float angle =
				static_cast<float>(elapsed) * 0.00001f
					/ glm::dot(relative, relative);
			const float cosine = std::cos(angle);
			const float sine = std::sin(angle);
			relative = {
				cosine * relative.x - sine * relative.y,
				sine * relative.x + cosine * relative.y,
			};
			// Retail stores the rotated relative UV without adding the center.
			coordinate = relative;
		}
		const float center_angle =
			static_cast<float>(elapsed) * 0.0001f;
		const float center_cosine = std::cos(center_angle);
		const float center_sine = std::sin(center_angle);
		const float old_center_x = state.uv_center_x;
		state.uv_center_x =
			center_cosine * old_center_x
				- center_sine * state.uv_center_y;
		state.uv_center_y =
			center_sine * old_center_x
				+ center_cosine * state.uv_center_y;

		if (get_available_frame_vertices(frontend.frame_geometry,
				geometry.point_count,
				frontend.layout) < geometry.point_count
			|| get_available_frame_indices(frontend.frame_geometry,
				geometry.index_count)
				< geometry.index_count)
		{
			continue;
		}
		FrameVertexBuffer vertices;
		FrameIndexBuffer triangles;
		alloc_frame_vertex_buffer(frontend.frame_geometry,
			&vertices,
			geometry.point_count,
			frontend.layout);
		alloc_frame_index_buffer(frontend.frame_geometry,
			&triangles,
			geometry.index_count);
		auto* output =
			reinterpret_cast<assets::GameplayVertex*>(
				vertices.data);
		std::memcpy(
			triangles.data,
			geometry.indices.data(),
			geometry.index_count * sizeof(std::uint16_t));
		for (std::uint32_t vertex = 0;
			vertex < geometry.point_count;
			++vertex)
		{
			glm::vec3 color{0.0f};
			if (flicker)
			{
				// The dark branch consumes the same per-vertex random values.
				const float value =
					static_cast<float>(game::world_rand15(*frame.world))
						* (1.0f / 32767.0f);
				if (bright_flicker)
				{
					color = {value, value, value};
				}
			}
			else
			{
				for (const auto& buffer : state.hit)
				{
					color += game::shield_color_lookup(
						buffer[vertex],
						false);
				}
				color = glm::min(color, glm::vec3{1.0f});
			}
			const glm::vec2 coordinate = state.uv[vertex];
			output[vertex] = {
				geometry.points[vertex].x,
				geometry.points[vertex].y,
				geometry.points[vertex].z,
				shield_vertex_color(color),
				coordinate.x,
				coordinate.y,
			};
		}
		const glm::mat4 transform = sl_open::math::model_transform(
			object.scene_orientation,
			object.radius * 1.1f,
			object.scene_position);
		const float full_uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
		const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
		bgfx::setUniform(frontend.uv_rect_uniform, full_uv);
		bgfx::setUniform(frontend.tint_uniform, tint);
		set_camera_relative_transform(transform, frame.camera_position);
		set_frame_vertex_buffer(0, &vertices);
		set_frame_index_buffer(&triangles);
		bgfx::setTexture(
			0,
			frontend.texture_sampler,
			(forcefield_texture
				? renderer.forcefield_texture
				: renderer.shield_texture).handle);
		bgfx::setState(
			BGFX_STATE_WRITE_RGB
				| BGFX_STATE_WRITE_A
				| BGFX_STATE_DEPTH_TEST_GREATER
				| retail_blend_state(RetailBlendSelector::additive)
				| BGFX_STATE_MSAA);
		submit_retail_transparent(
			renderer, frontend.mission_rgba_program);
	}
	return true;
}

void submit_cap_shields(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	if (frame.world == nullptr)
	{
		return;
	}
	for (game::CapShieldSlot& slot : frame.world->shields.cap)
	{
		if (slot.expiry_tick == 0
			|| frame.simulation_tick >= slot.expiry_tick
			|| slot.object_index >= game::kMaxGameObjects)
		{
			continue;
		}
		game::WorldObject& object =
			frame.world->objects[slot.object_index];
		if (!object.active
			|| object.generation != slot.object_generation
			|| slot.model_index < 0
			|| static_cast<std::size_t>(slot.model_index)
				>= object.model_references.size())
		{
			slot = {};
			continue;
		}
		const game::ObjectModelReference& reference =
			object.model_references[slot.model_index];
		const MissionGpuModel& model = reference.explosion_source_attachment
			? renderer.attachment_models[reference.explosion_source_model][0]
			: renderer.models[reference.explosion_source_model];
		const MissionGpuNode& node = model.nodes[reference.source_node];
		const glm::mat4 node_transform =
			sl_open::math::model_transform(
				object.scene_orientation,
				1.0f,
				object.scene_position)
			* reference.scene_transform;
		// CapShield_slot_create (0x0049f79e) clones LOD-set +0x2c,
		// the first mesh, independently of the source's selected LOD.
		const MissionGpuLod* lod = &node.lods[0];
		if (lod->source_vertices.empty()
			|| lod->source_indices.empty())
		{
			continue;
		}
		slot.forcefield = reference.forcefield;
		if (get_available_frame_vertices(frontend.frame_geometry,
				static_cast<std::uint32_t>(
					lod->source_vertices.size()),
				frontend.layout)
				< lod->source_vertices.size())
		{
			continue;
		}
		FrameVertexBuffer vertices;
		alloc_frame_vertex_buffer(frontend.frame_geometry,
			&vertices,
			static_cast<std::uint32_t>(
				lod->source_vertices.size()),
			frontend.layout);
		auto* output =
			reinterpret_cast<assets::GameplayVertex*>(
				vertices.data);
		for (std::uint32_t vertex = 0;
			vertex < lod->source_vertices.size();
			++vertex)
		{
			const assets::GameplayVertex& source =
				lod->source_vertices[vertex];
			const glm::vec3 local_position{
				source.x,
				source.y,
				source.z,
			};
			glm::vec3 color{0.0f};
			for (const game::CapShieldHit& hit : slot.hit)
			{
				if (!hit.active)
				{
					continue;
				}
				float value = slot.forcefield
					? 1.0f
					: glm::distance(
						local_position,
						hit.local_center) <= hit.radius
						? std::clamp(
							2.0f
								* glm::distance(
									local_position,
									hit.local_center)
								/ hit.radius,
							0.0f,
							2.0f)
						: 0.0f;
				value = std::max(
					0.0f,
					value
						- static_cast<float>(
							frame.simulation_tick - hit.tick)
							* 0.025f);
				color += game::shield_color_lookup(
					value,
					slot.forcefield) * 3.0f;
			}
			color = glm::min(color, glm::vec3{1.0f});
			float u = source.u;
			float v = source.v;
			if (slot.forcefield)
			{
				color.b = std::min(
					1.0f, color.g + color.b);
				color.g = 0.0f;
			}
			else
			{
				glm::vec2 relative{u - 0.5f, v - 0.5f};
				// Rotation about a fixed center preserves radius, so integrating
				// from creation reproduces the retained UV animation directly.
				const float angle =
					static_cast<float>(
						frame.simulation_tick
							- slot.birth_tick)
						* 0.001f
						/ glm::dot(relative, relative);
				const float cosine = std::cos(angle);
				const float sine = std::sin(angle);
				u = cosine * relative.x
					- sine * relative.y + 0.5f;
				v = sine * relative.x
					+ cosine * relative.y + 0.5f;
			}
			output[vertex] = {
				source.x,
				source.y,
				source.z,
				shield_vertex_color(color),
				u,
				v,
			};
		}
		if (slot.forcefield)
		{
			// Retail randomizes the face-corner UV stream. Expanded GPU fan
			// triangles share those values rather than drawing new randoms.
			for (const assets::GameplayFace& face : lod->source_faces)
			{
				for (std::uint32_t corner = 0; corner < face.corner_count; ++corner)
				{
					assets::GameplayVertex& vertex = output[
						lod->source_face_corners[face.first_corner + corner]];
					vertex.u = static_cast<float>(game::world_rand15(*frame.world))
						* (1.0f / 32767.0f);
					vertex.v = static_cast<float>(game::world_rand15(*frame.world))
						* (1.0f / 32767.0f);
				}
				for (std::uint32_t triangle = 1;
					triangle + 2 < face.corner_count;
					++triangle)
				{
					const std::uint32_t first = face.first_index + triangle * 3;
					output[first].u = output[face.first_index].u;
					output[first].v = output[face.first_index].v;
					output[first + 1].u = output[first - 1].u;
					output[first + 1].v = output[first - 1].v;
				}
			}
		}
		// CapShield_create (LANCER.EXE 0x0049f790) clones the first
		// SRO mesh with mesh-pass flag 0x800, clears face-policy bit zero
		// on every face, and rewrites every material to selector one.
		// Submit every retained face without culling: 0x800 globally
		// bypasses the retail facing test regardless of face bit one.
		for (const MissionGpuSection& section : lod->sections)
		{
			if (section.index_count == 0)
			{
				continue;
			}
			const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
			const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
			bgfx::setUniform(frontend.uv_rect_uniform, uv);
			bgfx::setUniform(frontend.tint_uniform, tint);
			set_camera_relative_transform(
				node_transform, frame.camera_position);
			set_frame_vertex_buffer(0, &vertices);
			bgfx::setIndexBuffer(
				lod->indices,
				section.first_index,
				section.index_count);
			bgfx::setTexture(
				0,
				frontend.texture_sampler,
				(slot.forcefield
					? renderer.forcefield_texture
					: renderer.shield_texture).handle);
			std::uint64_t state =
				BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_DEPTH_TEST_GREATER
					| retail_blend_state(
						RetailBlendSelector::additive)
					| BGFX_STATE_MSAA;
			if (section.lines)
			{
				state |= BGFX_STATE_PT_LINES;
			}
			bgfx::setState(state);
			submit_retail_transparent(
				renderer, frontend.mission_rgba_program);
		}
	}
}

void submit_sparks(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	if (frame.world == nullptr)
	{
		return;
	}
	constexpr glm::vec3 dimensions[5] = {
		{90.0f, 90.0f, 500.0f},
		{30.0f, 30.0f, 140.0f},
		{30.0f, 30.0f, 90.0f},
		{20.0f, 20.0f, 90.0f},
		{90.0f, 90.0f, 500.0f},
	};
	constexpr glm::vec3 start_color[5] = {
		{1.0f, 1.0f, 1.0f},
		{1.0f, 1.0f, 1.0f},
		{1.0f, 1.0f, 1.0f},
		{0.4f, 0.5f, 1.0f},
		{1.0f, 0.9f, 0.7f},
	};
	constexpr glm::vec3 end_color[5] = {
		{0.0f, 0.0f, 0.1f},
		{0.0f, 0.0f, 0.0f},
		{0.0f, 0.0f, 0.0f},
		{0.0f, 0.0f, 0.0f},
		{0.1f, 0.0f, 0.0f},
	};
	constexpr std::uint32_t lifetime[5] = {
		300, 100, 100, 100, 300,
	};
	constexpr std::uint8_t triangles[6] = {
		0, 1, 2, 0, 2, 3,
	};
	for (const game::Spark& spark : frame.world->shields.sparks)
	{
		if (!spark.active || spark.type >= 5)
		{
			continue;
		}
		const std::uint32_t elapsed =
			frame.simulation_tick - spark.birth_tick;
		if (elapsed > lifetime[spark.type])
		{
			continue;
		}
		const FrontendTexture& texture =
			spark.type == 0
				? renderer.spark_huge_texture
				: renderer.laser_cannon_texture;
		const std::uint32_t quad_count =
			spark.type == 0 ? 3u : 2u;
		const std::uint32_t vertex_count = quad_count * 6u;
		if (!bgfx::isValid(texture.handle)
			|| get_available_frame_vertices(frontend.frame_geometry,
				vertex_count, frontend.layout) < vertex_count)
		{
			continue;
		}
		const glm::vec3 d = dimensions[spark.type];
		glm::vec3 points[12];
		if (spark.type == 0)
		{
			const glm::vec3 source[12] = {
				{0.0f, d.y, -d.z},
				{0.0f, -d.y, -d.z},
				{0.0f, -d.y, d.z},
				{0.0f, d.y, d.z},
				{d.x, 0.0f, -d.z},
				{-d.x, 0.0f, -d.z},
				{-d.x, 0.0f, d.z},
				{d.x, 0.0f, d.z},
				{-d.x, -d.y, 0.0f},
				{d.x, -d.y, 0.0f},
				{d.x, d.y, 0.0f},
				{-d.x, d.y, 0.0f},
			};
			std::copy(std::begin(source), std::end(source), points);
		}
		else
		{
			const glm::vec3 source[8] = {
				{0.0f, d.y, 0.0f},
				{0.0f, -d.y, 0.0f},
				{0.0f, -d.y, d.z},
				{0.0f, d.y, d.z},
				{d.x, 0.0f, 0.0f},
				{-d.x, 0.0f, 0.0f},
				{-d.x, 0.0f, d.z},
				{d.x, 0.0f, d.z},
			};
			std::copy(std::begin(source), std::end(source), points);
		}
		const float t =
			static_cast<float>(elapsed)
			/ static_cast<float>(lifetime[spark.type]);
		const std::uint32_t packed_color =
			shield_vertex_color(glm::mix(
				start_color[spark.type],
				end_color[spark.type],
				t));
		const float v0 = spark.type == 3 ? 0.0f : 0.5f;
		const float v1 = spark.type == 3 ? 0.5f : 1.0f;
		constexpr float u0 = 0.125f;
		constexpr float u1 = 0.25f;
		const glm::vec2 uv[4] = {
			{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1},
		};
	FrameVertexBuffer buffer;
		alloc_frame_vertex_buffer(frontend.frame_geometry,
			&buffer, vertex_count, frontend.layout);
		auto* output =
			reinterpret_cast<assets::GameplayVertex*>(
				buffer.data);
		for (std::uint32_t quad = 0; quad < quad_count; ++quad)
		{
			for (std::uint32_t corner = 0; corner < 6; ++corner)
			{
				const std::uint8_t source = triangles[corner];
				const glm::vec3 point =
					points[quad * 4u + source];
				output[quad * 6u + corner] = {
					point.x,
					point.y,
					point.z,
					packed_color,
					uv[source].x,
					uv[source].y,
				};
			}
		}
		const glm::mat4 transform = sl_open::math::model_transform(
			basis_from_forward(spark.unit_drift),
			1.0f,
			spark.position);
		const float full_uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
		const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
		bgfx::setUniform(frontend.uv_rect_uniform, full_uv);
		bgfx::setUniform(frontend.tint_uniform, tint);
		set_camera_relative_transform(transform, frame.camera_position);
		set_frame_vertex_buffer(0, &buffer);
		bgfx::setTexture(
			0,
			frontend.texture_sampler,
			texture.handle);
		bgfx::setState(
			BGFX_STATE_WRITE_RGB
				| BGFX_STATE_WRITE_A
				| BGFX_STATE_DEPTH_TEST_GREATER
				| retail_blend_state(RetailBlendSelector::additive)
				| BGFX_STATE_MSAA);
		submit_retail_transparent(
			renderer, frontend.mission_rgba_program);
	}
}

std::uint32_t electric_ray_color(
	const glm::vec3& color,
	float alpha)
{
	const auto channel = [](float value)
	{
		return static_cast<std::uint32_t>(
			std::clamp(std::lrint(value * 255.0f), 0l, 255l));
	};
	return channel(alpha) << 24
		| channel(color.b) << 16
		| channel(color.g) << 8
		| channel(color.r);
}

void electric_ray_midpoint_displace(
	game::World& world,
	glm::vec3 (&points)[17],
	std::uint32_t left,
	std::uint32_t right,
	float displacement,
	std::int32_t depth)
{
	const std::uint32_t middle = (left + right) >> 1u;
	const glm::vec3 base = (points[left] + points[right]) * 0.5f;
	// ERAYFX_midpoint_displace (0x0046aa70) draws Z, then Y, then X.
	const float z = (static_cast<float>(game::world_rand15(world)) / 32767.0f
		- 0.5f) * 6.2831853071795864769f;
	const float y = (static_cast<float>(game::world_rand15(world)) / 32767.0f
		- 0.5f) * 6.2831853071795864769f;
	const float x = (static_cast<float>(game::world_rand15(world)) / 32767.0f
		- 0.5f) * 6.2831853071795864769f;
	glm::vec3 direction{x, y, z};
	const float direction_length = glm::length(direction);
	if (direction_length > 0.0f)
	{
		direction /= direction_length;
	}
	const float magnitude =
		static_cast<float>(game::world_rand15(world)) / 32767.0f
		* glm::distance(points[left], points[right])
		* displacement;
	points[middle] = base + direction * magnitude;
	--depth;
	if (depth != 0)
	{
		electric_ray_midpoint_displace(
			world, points, left, middle, displacement, depth);
		electric_ray_midpoint_displace(
			world, points, middle, right, displacement, depth);
	}
}

void submit_electric_ray_segment(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const FrontendTexture& texture,
	const glm::vec3& camera_position,
	const glm::vec3& position,
	const glm::mat3& orientation,
	float radius,
	float length,
	std::uint32_t color)
{
	if (!bgfx::isValid(texture.handle)
		|| get_available_frame_vertices(frontend.frame_geometry,
			18, frontend.layout) < 18)
	{
		return;
	}
	const glm::vec3 points[12] = {
		{-radius, -radius, 0.0f},
		{radius, -radius, 0.0f},
		{radius, radius, 0.0f},
		{-radius, radius, 0.0f},
		{0.0f, -radius, 0.0f},
		{0.0f, radius, 0.0f},
		{0.0f, radius, length},
		{0.0f, -radius, length},
		{-radius, 0.0f, 0.0f},
		{radius, 0.0f, 0.0f},
		{radius, 0.0f, length},
		{-radius, 0.0f, length},
	};
	constexpr std::uint8_t order[18] = {
		0, 1, 2, 0, 2, 3,
		4, 5, 6, 4, 6, 7,
		8, 9, 10, 8, 10, 11,
	};
	constexpr glm::vec2 uv[4] = {
		{0.99f, 0.99f},
		{0.99f, 0.04f},
		{0.04f, 0.04f},
		{0.04f, 0.99f},
	};
	FrameVertexBuffer buffer;
	alloc_frame_vertex_buffer(frontend.frame_geometry,
		&buffer, 18, frontend.layout);
	auto* vertices =
		reinterpret_cast<assets::GameplayVertex*>(buffer.data);
	for (std::uint32_t index = 0; index < 18; ++index)
	{
		const std::uint8_t source = order[index];
		vertices[index] = {
			points[source].x,
			points[source].y,
			points[source].z,
			color,
			uv[index % 6u == 0u || index % 6u == 3u
				? 0u
				: index % 6u == 1u
					? 1u
					: index % 6u == 2u || index % 6u == 4u
						? 2u
						: 3u].x,
			uv[index % 6u == 0u || index % 6u == 3u
				? 0u
				: index % 6u == 1u
					? 1u
					: index % 6u == 2u || index % 6u == 4u
						? 2u
						: 3u].y,
		};
	}
	const glm::mat4 transform =
		sl_open::math::model_transform(
			orientation, 1.0f, position - camera_position);
	const float uv_rect[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
	bgfx::setUniform(frontend.uv_rect_uniform, uv_rect);
	bgfx::setUniform(frontend.tint_uniform, tint);
	bgfx::setTransform(glm::value_ptr(transform));
	set_frame_vertex_buffer(0, &buffer);
	bgfx::setTexture(
		0, frontend.texture_sampler, texture.handle);
	bgfx::setState(
		BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_DEPTH_TEST_GREATER
			// ElectricRay_create (LANCER.EXE 0x0046a850) assigns retail
			// blend selector four, SRCALPHA/ONE.
			| retail_blend_state(
				RetailBlendSelector::source_alpha_additive)
			| BGFX_STATE_MSAA);
	submit_retail_transparent(
		renderer, frontend.mission_rgba_program);
}

void submit_electric_rays(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	if (frame.world == nullptr)
	{
		return;
	}
	for (const game::ElectricRayEffect& effect
		: frame.world->disruption_effects.electric_rays)
	{
		if (!effect.active
			|| effect.intensity == 0.0f
			|| effect.owner_index >= game::kMaxGameObjects)
		{
			continue;
		}
		const game::WorldObject& owner =
			frame.world->objects[effect.owner_index];
		if (!owner.active
			|| owner.generation != effect.owner_generation)
		{
			continue;
		}
		const glm::mat4 ordinary_root =
			math::model_transform(
				owner.scene_orientation,
				1.0f,
				owner.scene_position);
		glm::mat4 parent_transform = ordinary_root;
		if (effect.parent_model_reference
			< owner.model_references.size())
		{
			parent_transform = explosion_portal_root(
				frame,
				owner,
				effect.parent_model_reference,
				ordinary_root,
				1.0f)
				* owner.model_references[
					effect.parent_model_reference].scene_transform;
		}
		const float parent_scale =
			glm::length(glm::vec3{parent_transform[0]});
		const glm::mat3 parent_orientation{
			glm::normalize(glm::vec3{parent_transform[0]}),
			glm::normalize(glm::vec3{parent_transform[1]}),
			glm::normalize(glm::vec3{parent_transform[2]}),
		};
		for (std::int32_t group = 0;
			group < effect.group_count;
			++group)
		{
			glm::vec3 points[17]{};
			points[0] = effect.start;
			points[16] = effect.end;
			electric_ray_midpoint_displace(
				*frame.world,
				points,
				0,
				16,
				effect.displacement,
				4);
			const std::uint32_t color = electric_ray_color(
				effect.groups[group].color,
				effect.groups[group].alpha * effect.intensity);
			for (std::uint32_t segment = 0;
				segment < 16;
				++segment)
			{
				const glm::vec3 local_delta =
					points[segment + 1] - points[segment];
				const glm::vec3 world_delta{
					parent_transform
						* glm::vec4(local_delta, 0.0f)};
				const float length = glm::length(world_delta);
				if (length <= 0.0f)
				{
					continue;
				}
				submit_electric_ray_segment(
					renderer,
					frontend,
					renderer.nova_cannon_texture,
					frame.camera_position,
					glm::vec3{
						parent_transform
							* glm::vec4(points[segment], 1.0f)},
					basis_from_forward(world_delta),
					effect.radius * parent_scale,
					length,
					color);
			}
			// Electric_ray_update submits mesh object 16 even though it
			// never receives the per-frame segment transform.
			submit_electric_ray_segment(
				renderer,
				frontend,
				renderer.nova_cannon_texture,
				frame.camera_position,
				glm::vec3{parent_transform[3]},
				parent_orientation,
				effect.radius * parent_scale,
				400.0f * parent_scale,
				color);
		}
	}
}

void submit_shockwaves(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	if (frame.world == nullptr)
	{
		return;
	}
	constexpr std::uint8_t mesh_for_type[9] = {
		0, 1, 2, 3, 4, 4, 3, 3, 3,
	};
	for (const game::Shockwave& wave
		: frame.world->disruption_effects.shockwaves)
	{
		if (!wave.active
			|| wave.type >= std::size(mesh_for_type)
			|| wave.type == 7
			|| wave.lifetime_ticks <= 0)
		{
			continue;
		}
		const float age =
			static_cast<float>(
				frame.simulation_tick - wave.start_tick)
			/ static_cast<float>(wave.lifetime_ticks);
		if (age < 0.0f || age >= 1.0f)
		{
			continue;
		}
		const FrontendTexture& texture =
			renderer.shockwave_textures[mesh_for_type[wave.type]];
		if (!bgfx::isValid(texture.handle)
			|| get_available_frame_vertices(frontend.frame_geometry,
				48, frontend.layout) < 48)
		{
			continue;
		}
		glm::vec3 source[16];
		for (std::uint32_t sector = 0; sector < 8; ++sector)
		{
			const float angle =
				static_cast<float>(sector)
				* 0.78539816339744830962f;
			const float x = -std::sin(angle);
			const float y = std::cos(angle);
			source[sector * 2] = {x, y, 0.0f};
			source[sector * 2 + 1] = {
				x * 0.1f, y * 0.1f, 0.0f};
		}
		FrameVertexBuffer buffer;
		alloc_frame_vertex_buffer(frontend.frame_geometry,
			&buffer, 48, frontend.layout);
		auto* vertices =
			reinterpret_cast<assets::GameplayVertex*>(buffer.data);
		const std::uint32_t color =
			packed_effect_color(1.0f - age, 1.0f - age);
		std::uint32_t output = 0;
		for (std::uint32_t sector = 0; sector < 8; ++sector)
		{
			const std::uint8_t outer =
				static_cast<std::uint8_t>(sector * 2);
			const std::uint8_t inner =
				static_cast<std::uint8_t>(outer + 1);
			const std::uint8_t next_outer =
				static_cast<std::uint8_t>((outer + 2) % 16);
			const std::uint8_t next_inner =
				static_cast<std::uint8_t>((inner + 2) % 16);
			const std::uint8_t indices[6] = {
				outer, inner, next_outer,
				next_outer, next_inner, inner,
			};
			for (std::uint8_t index : indices)
			{
				const glm::vec3 point = source[index];
				vertices[output++] = {
					point.x,
					point.y,
					point.z,
					color,
					std::max(std::abs(point.x), 1.0f / 64.0f),
					std::max(std::abs(point.y), 1.0f / 64.0f),
				};
			}
		}
		const glm::mat4 transform = sl_open::math::model_transform(
			wave.orientation,
			age * wave.final_radius,
			wave.position);
		const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
		const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
		bgfx::setUniform(frontend.uv_rect_uniform, uv);
		bgfx::setUniform(frontend.tint_uniform, tint);
		set_camera_relative_transform(transform, frame.camera_position);
		set_frame_vertex_buffer(0, &buffer);
		bgfx::setTexture(
			0, frontend.texture_sampler, texture.handle);
		bgfx::setState(
			BGFX_STATE_WRITE_RGB
				| BGFX_STATE_WRITE_A
				| BGFX_STATE_DEPTH_TEST_GREATER
				// Shockwave_create (LANCER.EXE 0x004a0b30) assigns
				// retail selector one, ONE/ONE.
				| retail_blend_state(RetailBlendSelector::additive)
				| BGFX_STATE_MSAA);
		submit_retail_transparent(
			renderer, frontend.mission_rgba_program);
	}
}

void submit_dock_ring(
	MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	// DockRing_resources_init/update_and_render
	// (LANCER.EXE 0x00468920/0x00468d00) owns a procedural 100-vertex,
	// 48-line/12-triangle guidance mesh. SetEscortPoint writes the local
	// player's target slot at +0x724; the renderer copies that object's
	// full transform, advances a 300-tick red guidance wave, and submits
	// only in the four in-cockpit flight camera modes.
	if (!frame.dock_ring_active || frame.camera_mode > 3)
	{
		renderer.dock_ring_last_tick = frame.simulation_tick;
		renderer.dock_ring_tick_valid = true;
		return;
	}
	if (renderer.dock_ring_tick_valid)
	{
		renderer.dock_ring_phase =
			(renderer.dock_ring_phase
				+ (frame.simulation_tick
					- renderer.dock_ring_last_tick))
			% 300u;
	}
	renderer.dock_ring_last_tick = frame.simulation_tick;
	renderer.dock_ring_tick_valid = true;

	glm::vec3 points[100]{};
	float intensities[100];
	std::fill(std::begin(intensities), std::end(intensities), 1.0f);
	std::uint32_t point = 0;
	for (std::uint32_t layer = 0; layer < 4; ++layer)
	{
		const float z = static_cast<float>(layer) * 640.0f - 960.0f;
		for (std::uint32_t quadrant = 0; quadrant < 4; ++quadrant)
		{
			const float x_sign = (quadrant & 1u) == 0 ? 1.0f : -1.0f;
			const float y_sign = (quadrant & 2u) == 0 ? 1.0f : -1.0f;
			points[point++] = {x_sign * 160.0f, y_sign * 640.0f, z};
			points[point++] = {x_sign * 320.0f, y_sign * 640.0f, z};
			points[point++] = {x_sign * 640.0f, y_sign * 320.0f, z};
			points[point++] = {x_sign * 640.0f, y_sign * 160.0f, z};
		}
	}
	for (std::uint32_t triangle = 0; triangle < 12; ++triangle)
	{
		const float sector =
			static_cast<float>(triangle & 3u)
			* glm::half_pi<float>();
		const float sector_cos = std::cos(sector);
		const float sector_sin = std::sin(sector);
		const float z_center =
			static_cast<float>(triangle / 4u) * 640.0f - 640.0f;
		for (std::uint32_t vertex = 0; vertex < 3; ++vertex)
		{
			const float angle =
				static_cast<float>(vertex)
				* glm::two_pi<float>() / 3.0f;
			const glm::vec3 unrotated{
				std::cos(angle) * 100.0f,
				640.0f,
				std::sin(angle) * 100.0f + z_center,
			};
			points[point++] = {
				unrotated.x * sector_cos
					- unrotated.y * sector_sin,
				unrotated.x * sector_sin
					+ unrotated.y * sector_cos,
				unrotated.z,
			};
		}
	}

	for (std::uint32_t band = 0; band < 3; ++band)
	{
		const std::int32_t offset = 300 - static_cast<std::int32_t>(
			band * 100u);
		const std::int32_t wrapped =
			(static_cast<std::int32_t>(renderer.dock_ring_phase)
				+ offset)
			% 300;
		const float intensity = std::max(
			0.25f,
			1.0f
				- std::abs(static_cast<float>(wrapped - 150))
					* 0.01f);
		std::fill_n(intensities + 16u * (band + 1u), 16u, intensity);
	}
	for (std::uint32_t band = 0; band < 3; ++band)
	{
		const std::int32_t wrapped =
			(static_cast<std::int32_t>(renderer.dock_ring_phase)
				- static_cast<std::int32_t>(band * 100u)
				+ 350)
			% 300;
		const float intensity = std::max(
			0.25f,
			1.0f
				- std::abs(static_cast<float>(wrapped - 150))
					* 0.01f);
		std::fill_n(intensities + 64u + 12u * band, 12u, intensity);
	}

	glm::vec3 placement = frame.dock_ring_target_position;
	glm::vec3 camera_to_target = placement - frame.camera_position;
	const float distance = glm::length(camera_to_target);
	if (distance > 50000.0f)
	{
		placement =
			frame.camera_position
			+ camera_to_target * (50000.0f / distance);
	}
	const glm::mat4 transform = sl_open::math::model_transform(
		frame.dock_ring_target_orientation, 1.0f, placement);
	const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
	constexpr std::uint32_t kLineVertexCount = 48u * 2u;
	if (get_available_frame_vertices(frontend.frame_geometry,
			kLineVertexCount, frontend.layout) >= kLineVertexCount)
	{
		FrameVertexBuffer buffer;
		alloc_frame_vertex_buffer(frontend.frame_geometry,
			&buffer, kLineVertexCount, frontend.layout);
		assets::GameplayVertex* vertices =
			reinterpret_cast<assets::GameplayVertex*>(buffer.data);
		std::uint32_t output = 0;
		for (std::uint32_t group = 0; group < 16; ++group)
		{
			for (std::uint32_t segment = 0; segment < 3; ++segment)
			{
				for (std::uint32_t endpoint = 0; endpoint < 2; ++endpoint)
				{
					const std::uint32_t source =
						group * 4u + segment + endpoint;
					const std::uint32_t red =
						static_cast<std::uint32_t>(std::nearbyint(
							intensities[source] * 255.0f));
					vertices[output++] = {
						points[source].x,
						points[source].y,
						points[source].z,
						0xff000000u | red,
						0.5f,
						0.5f,
					};
				}
			}
		}
		bgfx::setUniform(frontend.uv_rect_uniform, uv);
		bgfx::setUniform(frontend.tint_uniform, tint);
		set_camera_relative_transform(transform, frame.camera_position);
		set_frame_vertex_buffer(0, &buffer);
		bgfx::setTexture(
			0, frontend.texture_sampler, frontend.white.handle);
		bgfx::setState(
			BGFX_STATE_WRITE_RGB
				| BGFX_STATE_WRITE_A
				| BGFX_STATE_DEPTH_TEST_GREATER
				| retail_blend_state(
					RetailBlendSelector::source_alpha_additive)
				| BGFX_STATE_PT_LINES
				| BGFX_STATE_MSAA);
		submit_retail_transparent(
			renderer, frontend.mission_rgba_program);
	}

	constexpr std::uint32_t kTriangleVertexCount = 12u * 3u;
	if (get_available_frame_vertices(frontend.frame_geometry,
			kTriangleVertexCount, frontend.layout) >= kTriangleVertexCount)
	{
		FrameVertexBuffer buffer;
		alloc_frame_vertex_buffer(frontend.frame_geometry,
			&buffer, kTriangleVertexCount, frontend.layout);
		assets::GameplayVertex* vertices =
			reinterpret_cast<assets::GameplayVertex*>(buffer.data);
		for (std::uint32_t vertex = 0;
			vertex < kTriangleVertexCount;
			++vertex)
		{
			const std::uint32_t source = 64u + vertex;
			const std::uint32_t red =
				static_cast<std::uint32_t>(std::nearbyint(
					intensities[source] * 255.0f));
			vertices[vertex] = {
				points[source].x,
				points[source].y,
				points[source].z,
				0xff000000u | red,
				0.5f,
				0.5f,
			};
		}
		bgfx::setUniform(frontend.uv_rect_uniform, uv);
		bgfx::setUniform(frontend.tint_uniform, tint);
		set_camera_relative_transform(transform, frame.camera_position);
		set_frame_vertex_buffer(0, &buffer);
		bgfx::setTexture(
			0, frontend.texture_sampler, frontend.white.handle);
		bgfx::setState(
			BGFX_STATE_WRITE_RGB
				| BGFX_STATE_WRITE_A
				| BGFX_STATE_DEPTH_TEST_GREATER
				| retail_blend_state(
					RetailBlendSelector::source_alpha_additive)
				| BGFX_STATE_MSAA);
		submit_retail_transparent(
			renderer, frontend.mission_rgba_program);
	}
}

void submit_missiles(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame,
	const MissionSceneLights& scene_lights)
{
	for (std::uint32_t index = 0;
		index < frame.missile_count;
		++index)
	{
		const MissionMissile& missile = frame.missiles[index];
		if (missile.type >= assets::kMissileStatsCount)
		{
			continue;
		}
		// Missile_launch_from_ship_mount (LANCER.EXE 0x00496290)
		// preserves the mounted SRO's complete world pose. Definitions
		// with a separate projectile resource clone that second catalog
		// model; the others detach and retain the mounted model itself.
		// Keep using the SRO material path after launch so its authored
		// blend, depth-write, lighting, and cull state remain unchanged.
		const std::size_t variant_index = missile.model_variant;
		if (!renderer.attachment_model_loaded[
				missile.type][variant_index])
		{
			continue;
		}
		MissionRenderInstance fired_model{};
		fired_model.position = missile.position;
		fired_model.orientation = sl_open::math::postrotate(
			missile.orientation,
			-glm::pi<float>(),
			{0.0f, 1.0f, 0.0f});
		fired_model.scale = 1.0f;
		submit_model_with_locator_children(
			renderer,
			renderer.attachment_models[missile.type][variant_index],
			fired_model,
			frontend,
			scene_lights,
			frame,
			true,
			false,
			0);
	}
}

void submit_missile_trail_ribbon(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame,
	const game::MissileTrailRing* rings,
	std::uint16_t ring_count,
	const glm::vec3& ribbon_color)
{
	if (ring_count < 2
		|| !bgfx::isValid(renderer.missile_trail_texture.handle))
	{
		return;
	}
	const std::uint32_t segment_count = ring_count - 1u;
	const std::uint32_t vertex_count = segment_count * 18u;
	if (get_available_frame_vertices(frontend.frame_geometry,
			vertex_count, frontend.layout) < vertex_count)
	{
		return;
	}
	FrameVertexBuffer buffer;
	alloc_frame_vertex_buffer(frontend.frame_geometry, &buffer, vertex_count, frontend.layout);
	auto* vertices = reinterpret_cast<assets::GameplayVertex*>(buffer.data);
	const auto packed_color = [&ribbon_color](
		const game::MissileTrailRing& ring)
	{
		const auto channel = [](float value)
		{
			return static_cast<std::uint32_t>(std::clamp(
				std::lrint(value * 255.0f), 0l, 255l));
		};
		return channel(std::max(ring.alpha, 0.0f)) << 24
			| channel(ribbon_color.b * ring.intensity) << 16
			| channel(ribbon_color.g * ring.intensity) << 8
			| channel(ribbon_color.r * ring.intensity);
	};
	constexpr std::uint8_t order[6] = {0, 1, 2, 0, 2, 3};
	std::uint32_t output = 0;
	for (std::uint32_t segment = 0; segment < segment_count; ++segment)
	{
		const game::MissileTrailRing& previous = rings[segment];
		const game::MissileTrailRing& current = rings[segment + 1u];
		const std::uint32_t previous_color = packed_color(previous);
		const std::uint32_t current_color = packed_color(current);
		const glm::vec3 quads[3][4] = {
			{previous.points[0], previous.points[2],
				current.points[2], current.points[0]},
			{previous.points[1], previous.points[3],
				current.points[3], current.points[1]},
			{current.points[0], current.points[1],
				current.points[2], current.points[3]},
		};
		const glm::vec2 texture_uv[3][4] = {
			{{0.0f, 0.0f}, {0.0f, 0.5f}, {1.0f, 0.5f}, {1.0f, 0.0f}},
			{{0.0f, 0.0f}, {0.0f, 0.5f}, {1.0f, 0.5f}, {1.0f, 0.0f}},
			{{0.0f, 0.5f}, {1.0f, 0.5f}, {1.0f, 1.0f}, {0.0f, 1.0f}},
		};
		for (std::uint8_t polygon = 0; polygon < 3; ++polygon)
		{
			for (std::uint8_t vertex = 0; vertex < 6; ++vertex)
			{
				const std::uint8_t corner = order[vertex];
				const glm::vec3 point = quads[polygon][corner]
					- frame.camera_position;
				vertices[output++] = {
					point.x, point.y, point.z,
					polygon == 2 || corner >= 2
						? current_color : previous_color,
					texture_uv[polygon][corner].x,
					texture_uv[polygon][corner].y,
				};
			}
		}
	}
	const float full_uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
	bgfx::setUniform(frontend.uv_rect_uniform, full_uv);
	bgfx::setUniform(frontend.tint_uniform, tint);
	bgfx::setTransform(glm::value_ptr(glm::mat4{1.0f}));
	set_frame_vertex_buffer(0, &buffer);
	bgfx::setTexture(
		0, frontend.texture_sampler, renderer.missile_trail_texture.handle);
	bgfx::setState(
		BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_DEPTH_TEST_GREATER
			| retail_blend_state(RetailBlendSelector::additive)
			| BGFX_STATE_MSAA);
	submit_retail_transparent(renderer, frontend.mission_rgba_program);
}

void submit_missile_burst_trail(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame,
	const game::MissileTrail& trail)
{
	constexpr std::uint32_t kVertices = 5u * 9u * 6u;
	if (!trail.attached
		|| get_available_frame_vertices(frontend.frame_geometry,
			kVertices, frontend.layout) < kVertices)
	{
		return;
	}
	FrameVertexBuffer buffer;
	alloc_frame_vertex_buffer(frontend.frame_geometry, &buffer, kVertices, frontend.layout);
	auto* vertices = reinterpret_cast<assets::GameplayVertex*>(buffer.data);
	glm::vec3 points[6][9];
	for (std::uint8_t ring = 0; ring < 6; ++ring)
	{
		const float radius = ring == 0
			? 0.0f
			: std::sin(static_cast<float>(ring) * 0.2f
				* glm::half_pi<float>()) * 180.0f + 30.0f;
		const float z = ring == 5
			? -630.0f
			: -static_cast<float>(ring) * 90.0f;
		for (std::uint8_t radial = 0; radial < 9; ++radial)
		{
			const float angle = static_cast<float>(radial)
				* glm::two_pi<float>() / 9.0f;
			points[ring][radial] = {
				std::sin(angle) * radius,
				std::cos(angle) * radius,
				z - static_cast<float>(ring) * 0.1f,
			};
		}
	}
	std::uint32_t output = 0;
	constexpr std::uint8_t triangle[6] = {0, 1, 2, 0, 2, 3};
	for (std::uint8_t ring = 0; ring < 5; ++ring)
	{
		for (std::uint8_t radial = 0; radial < 9; ++radial)
		{
			const std::uint8_t next = static_cast<std::uint8_t>((radial + 1) % 9);
			const glm::vec3 quad[4] = {
				points[ring][radial], points[ring + 1u][radial],
				points[ring + 1u][next], points[ring][next],
			};
			for (std::uint8_t vertex = 0; vertex < 6; ++vertex)
			{
				const glm::vec3 local = quad[triangle[vertex]];
				const glm::vec3 point = trail.burst_position
					+ trail.burst_orientation * local - frame.camera_position;
				vertices[output++] = {
					point.x, point.y, point.z, 0xffffffffu,
					local.z * 0.0006666666595265269f,
					std::atan2(local.x, local.y)
						* 0.07957746833562851f + trail.burst_v_offset,
				};
			}
		}
	}
	const float full_uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
	bgfx::setUniform(frontend.uv_rect_uniform, full_uv);
	bgfx::setUniform(frontend.tint_uniform, tint);
	bgfx::setTransform(glm::value_ptr(glm::mat4{1.0f}));
	set_frame_vertex_buffer(0, &buffer);
	bgfx::setTexture(
		0, frontend.texture_sampler, renderer.missile_trail_texture.handle);
	bgfx::setState(
		BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_DEPTH_TEST_GREATER
			| retail_blend_state(RetailBlendSelector::additive)
			| BGFX_STATE_MSAA);
	submit_retail_transparent(renderer, frontend.mission_rgba_program);
}

void submit_missile_trails(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	for (std::uint32_t trail_index = 0;
		trail_index < frame.missile_trail_count;
		++trail_index)
	{
		const game::MissileTrail* source =
			frame.missile_trails[trail_index].trail;
		if (source == nullptr)
		{
			continue;
		}
		const game::MissileTrail& trail = *source;
		if ((trail.flags & 4u) != 0)
		{
			for (std::uint8_t side = 0; side < trail.side_count; ++side)
			{
				const game::MissileTrailRibbon& ribbon =
					trail.side_ribbons[side];
				submit_missile_trail_ribbon(
					renderer,
					frontend,
					frame,
					ribbon.rings.data(),
					ribbon.ring_count,
					trail.side_color);
			}
		}
		if ((trail.flags & 1u) != 0)
		{
			submit_missile_trail_ribbon(
				renderer,
				frontend,
				frame,
				trail.rings.data(),
				trail.ring_count,
				trail.color);
		}
		if ((trail.flags & 2u) != 0)
		{
			submit_missile_burst_trail(renderer, frontend, frame, trail);
		}
		if ((trail.flags & 8u) != 0 && trail.attached
			&& trail.glow_size > 0.0f)
		{
			submit_billboard(
				renderer,
				frontend,
				renderer.missile_trail_texture,
				frame.camera_position,
				trail.glow_position,
				trail.glow_size * 2.0f,
				packed_effect_color(1.0f, 1.0f),
				0.0f,
				0.0f,
				1.0f,
				1.0f,
				frame.camera_orientation,
				1.0f,
				RetailBlendSelector::additive,
				-trail.glow_size);
		}
	}
}

void submit_launch_external_trails(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	if (frame.launch_trail_ring_count < 2)
	{
		return;
	}
	std::array<
		const MissionLaunchTrailRing*,
		kMaxMissionLaunchTrailRings> rings;
	for (std::uint32_t index = 0;
		index < frame.launch_trail_ring_count;
		++index)
	{
		rings[index] = &frame.launch_trail_rings[index];
	}
	std::sort(
		rings.begin(),
		rings.begin() + frame.launch_trail_ring_count,
		[](const MissionLaunchTrailRing* first,
			const MissionLaunchTrailRing* second)
		{
			return first->owner < second->owner
				|| (first->owner == second->owner
					&& (first->generation < second->generation
						|| (first->generation
								== second->generation
								&& first->sequence
									< second->sequence)));
		});
	std::uint32_t segment_count = 0;
	for (std::size_t index = 1;
		index < frame.launch_trail_ring_count;
		++index)
	{
		if (rings[index - 1]->owner == rings[index]->owner
			&& rings[index - 1]->generation
				== rings[index]->generation)
		{
			++segment_count;
		}
	}
	const std::uint32_t vertex_count = segment_count * 18u;
	if (vertex_count == 0
		|| get_available_frame_vertices(frontend.frame_geometry,
			vertex_count, frontend.layout) < vertex_count)
	{
		return;
	}
	FrameVertexBuffer buffer;
	alloc_frame_vertex_buffer(frontend.frame_geometry,
		&buffer, vertex_count, frontend.layout);
	assets::GameplayVertex* vertices =
		reinterpret_cast<assets::GameplayVertex*>(buffer.data);
	std::uint32_t output = 0;
	const std::uint32_t now_tick = frame.simulation_tick;
	for (std::size_t index = 1;
		index < frame.launch_trail_ring_count;
		++index)
	{
		const MissionLaunchTrailRing& previous = *rings[index - 1];
		const MissionLaunchTrailRing& current = *rings[index];
		if (previous.owner != current.owner
			|| previous.generation != current.generation)
		{
			continue;
		}
		const auto ring_color =
			[now_tick](const MissionLaunchTrailRing& ring)
			{
				const float phase = std::clamp(
					static_cast<float>(now_tick - ring.birth_tick)
						/ 70.0f,
					0.0f,
					1.0f);
				const std::uint8_t channel =
					static_cast<std::uint8_t>(
						std::lrint((1.0f - phase) * 255.0f));
				return 0xff000000u
					| static_cast<std::uint32_t>(channel) << 16
					| static_cast<std::uint32_t>(channel) << 8
					| channel;
			};
		const std::uint32_t previous_color = ring_color(previous);
		const std::uint32_t current_color = ring_color(current);
		// Launch_strategy3_update (LANCER.EXE 0x0041a498..0x0041a4a0)
		// creates the ordinary missile-trail type 9 for this actor. Preserve
		// Missile_trail_mesh_create's two crossed sides and current-ring cap;
		// mtrail2's upper half is the longitudinal streak and its lower half
		// is the cap image (LANCER.EXE 0x004970a0).
		const glm::vec3 quads[3][4] = {
			{previous.points[0], previous.points[2],
				current.points[2], current.points[0]},
			{previous.points[1], previous.points[3],
				current.points[3], current.points[1]},
			{current.points[0], current.points[1],
				current.points[2], current.points[3]},
		};
		constexpr glm::vec2 texture_uv[3][4] = {
			{{0.0f, 0.0f}, {0.0f, 0.5f}, {1.0f, 0.5f}, {1.0f, 0.0f}},
			{{0.0f, 0.0f}, {0.0f, 0.5f}, {1.0f, 0.5f}, {1.0f, 0.0f}},
			{{0.0f, 0.5f}, {1.0f, 0.5f}, {1.0f, 1.0f}, {0.0f, 1.0f}},
		};
		constexpr std::uint8_t order[6] = {0, 1, 2, 0, 2, 3};
		for (std::uint8_t polygon = 0; polygon < 3; ++polygon)
		{
			for (std::uint8_t vertex = 0; vertex < 6; ++vertex)
			{
				const std::uint8_t corner = order[vertex];
				const glm::vec3 point =
					quads[polygon][corner] - frame.camera_position;
				vertices[output++] = {
					point.x,
					point.y,
					point.z,
					polygon == 2 || corner >= 2
						? current_color
						: previous_color,
					texture_uv[polygon][corner].x,
					texture_uv[polygon][corner].y,
				};
			}
		}
	}
	const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
	bgfx::setTransform(glm::value_ptr(glm::mat4{1.0f}));
	bgfx::setUniform(frontend.uv_rect_uniform, uv);
	bgfx::setUniform(frontend.tint_uniform, tint);
	set_frame_vertex_buffer(0, &buffer);
	bgfx::setTexture(
		0,
		frontend.texture_sampler,
		renderer.missile_trail_texture.handle);
	bgfx::setState(
		BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_DEPTH_TEST_GREATER
			| retail_blend_state(RetailBlendSelector::additive)
			| BGFX_STATE_MSAA);
	submit_retail_transparent(
		renderer, frontend.mission_rgba_program);
}

template<typename SubmitChildren>
void submit_instance_pass(
	const MissionRenderer& renderer,
	const MissionGpuModel& model,
	const MissionRenderInstance& instance,
	const FrontendRenderer& frontend,
	const FrontendTexture& cloak_texture,
	const MissionSceneLights& scene_lights,
	const MissionRenderFrame& frame,
	std::uint32_t simulation_tick,
	SubmitChildren&& submit_children);
void submit_model_with_locator_children(
	const MissionRenderer& renderer,
	const MissionGpuModel& model,
	const MissionRenderInstance& instance,
	const FrontendRenderer& frontend,
	const MissionSceneLights& scene_lights,
	const MissionRenderFrame& frame,
	bool submit_locator_light_flares,
	bool animate_runtime,
	std::uint32_t depth);
void submit_mounted_ordnance_children(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderInstance& instance,
	const glm::mat4& owner_root,
	const MissionSceneLights& scene_lights,
	const MissionRenderFrame& frame,
	std::uint16_t owner_runtime_model_index)
{
	if (instance.source_object == nullptr)
	{
		return;
	}
	const game::WorldObject& object = *instance.source_object;
	// GameObject_build_attachment_models (0x0045e1a0) appends live
	// hardpoint models to each owning node after its tag-9 locator children.
	// Reverse scene traversal consequently visits hardpoints last-to-first,
	// before the node's static locator children and its own mesh.
	for (std::uint8_t index = object.attachment_count;
		index-- > 0;)
	{
		game::AttachmentSlot& attachment =
			const_cast<game::AttachmentSlot&>(
				object.attachments[index]);
		if (attachment.model_reference
				!= static_cast<std::int16_t>(
					owner_runtime_model_index)
			|| !attachment.live_model
			|| attachment.definition_index < 0
			|| static_cast<std::size_t>(attachment.definition_index)
				>= game::kAttachmentDefinitionCount)
		{
			continue;
		}
		const std::uint32_t variant_index =
			attachment.alternate_model ? 1u : 0u;
		if (!renderer.attachment_model_loaded[
				attachment.definition_index][variant_index])
		{
			continue;
		}
		const MissionGpuModel& model =
			renderer.attachment_models[
				attachment.definition_index][variant_index];
		if (!attachment_cloak_models_match(attachment, model))
		{
			initialize_attachment_cloak_models(
				attachment, model, object.type);
			if (frame.world != nullptr
				&& attachment.cloak_install_pending)
			{
				game::world_initialize_cloak_attachment(
					*frame.world, attachment);
			}
		}
		glm::mat4 hardpoint_transform =
			glm::translate(
				glm::mat4{1.0f},
				attachment.local_position)
			* glm::mat4(attachment.local_orientation);
		if (attachment.model_reference >= 0
			&& static_cast<std::size_t>(
				attachment.model_reference)
				< object.model_references.size())
		{
			hardpoint_transform =
				object.model_references[
					static_cast<std::uint16_t>(
						attachment.model_reference)].scene_transform
				* attachment.hardpoint_from_model;
		}
		hardpoint_transform = hardpoint_transform
			* glm::mat4(
				sl_open::math::rotation_from_euler(
					{0.0f, -glm::pi<float>(), 0.0f}));
		const glm::mat4 attachment_root =
			owner_root
			* hardpoint_transform
			* glm::translate(
				glm::mat4{1.0f}, model.center_of_mass);
		MissionRenderInstance mounted;
		mounted.position = glm::vec3(attachment_root[3]);
		mounted.orientation =
			glm::mat3(attachment_root)
				/ instance.scale;
		mounted.scale = instance.scale;
		mounted.effect_owner = &object;
		mounted.effect_cloak_models = &attachment.cloak_models;
		mounted.effect_root_wobble_applied = true;
		submit_model_with_locator_children(
			renderer,
			model,
			mounted,
			frontend,
			scene_lights,
			frame,
			true,
			false,
			0);
	}
}

bool ripper_exhaust_node_visible(
	const game::WorldObject& object,
	const MissionGpuNode& source_node)
{
	if (object.type != 0x1f)
	{
		return true;
	}
	const bool reverse_callback =
		object.flight_callback_mode
				== game::FlightCallbackMode::standard_reverse
			|| object.flight_callback_mode
				== game::FlightCallbackMode::provider_reverse;
	if (reverse_callback)
	{
		return std::strcmp(
				source_node.name, "Ripper_Back_pincer_2") == 0
			|| std::strcmp(
				source_node.name, "Ripper_Back_pincer_03") == 0
			|| std::strcmp(
				source_node.name, "Ripper_Back_pincer_04") == 0
			|| std::strcmp(
				source_node.name, "Ripper_Back_pincer_05") == 0;
	}
	return std::strcmp(source_node.name, "Ripper_l_thrust") == 0
		|| std::strcmp(source_node.name, "Ripper_r_thrust") == 0;
}

void submit_engine_flares(
	MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	if (frame.random_seed == nullptr)
	{
		return;
	}
	const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {
		0.9900000095367432f,
		0.9900000095367432f,
		0.9900000095367432f,
		0.9900000095367432f,
	};
	auto submit_model =
		[&](auto&& self,
			const MissionGpuModel& model,
			const MissionRenderInstance& instance,
			const game::WorldObject& object,
			bool animate_runtime) -> void
	{
		const glm::mat3 root_wobble =
			animate_runtime
				&& (object.runtime_flags & 0x00000100u) != 0
				? cloak_wobble(object, frame.simulation_tick)
				: glm::mat3{1.0f};
		const glm::mat4 instance_transform = sl_open::math::model_transform(
			instance.orientation * root_wobble,
			instance.scale,
			instance.position);
		for (std::uint32_t locator_index = 0;
			locator_index < model.locators.size();
			++locator_index)
		{
			const MissionGpuLocator& locator =
				model.locators[locator_index];
			if (locator.source_node >= model.nodes.size())
			{
				continue;
			}
			const MissionGpuNode& source_node =
				model.nodes[locator.source_node];
			const std::uint16_t runtime_index =
				static_cast<std::uint16_t>(
					instance.runtime_reference_base
						+ source_node.runtime_model_index);
			const glm::mat4 source_model_root = animate_runtime
				? explosion_portal_root(
					frame,
					object,
					runtime_index,
					instance_transform,
					instance.scale)
				: instance_transform;
			if (animate_runtime
				&& runtime_index < object.model_references.size()
				&& object.model_references[runtime_index].removed)
			{
				continue;
			}
			glm::mat4 locator_transform = locator.object_transform;
			if (animate_runtime
				&& locator_index < model.gameplay_locators.size()
				&& runtime_index < object.model_references.size())
			{
				const assets::GameplayLocator& gameplay_locator =
					model.gameplay_locators[locator_index];
				locator_transform = object.model_references[
					runtime_index].scene_transform
					* glm::translate(
						glm::mat4{1.0f}, gameplay_locator.position)
					* glm::mat4(gameplay_locator.basis);
			}
			const std::int32_t attachment_definition =
				embedded_attachment_definition(locator);
			if (attachment_definition >= 0
				&& renderer.attachment_model_loaded[
					attachment_definition][0])
			{
				const MissionGpuModel& attached =
					renderer.attachment_models[
						attachment_definition][0];
				const game::EmbeddedModelTree* runtime_tree = nullptr;
				if (animate_runtime)
				{
					for (const game::EmbeddedModelTree& candidate
						: object.embedded_model_trees)
					{
						if (candidate.parent_scope_base
								== instance.runtime_reference_base
							&& candidate.source_locator == locator_index
							&& candidate.attachment_definition
								== attachment_definition)
						{
							runtime_tree = &candidate;
							break;
						}
					}
				}
				if (runtime_tree != nullptr)
				{
					MissionRenderInstance child = instance;
					child.runtime_reference_base =
						runtime_tree->runtime_reference_base;
					child.effect_root_wobble_applied = true;
					self(self, attached, child, object, true);
					continue;
				}
				const glm::mat4 child_root =
					source_model_root
					* locator_transform
					* glm::translate(
						glm::mat4{1.0f}, attached.center_of_mass);
				MissionRenderInstance child;
				child.position = glm::vec3(child_root[3]);
				child.orientation =
					glm::mat3(child_root) / instance.scale;
				child.scale = instance.scale;
				child.effect_owner = &object;
				child.effect_root_wobble_applied = true;
				self(self, attached, child, object, false);
				continue;
			}
			if (locator.type != 2)
			{
				continue;
			}
			float intensity = locator.subtype == 7
				? 1.0f
				: object.exhaust_scalar
					* object.engine_component_scale;
			if (locator.subtype != 7)
			{
				if (locator.dimensions.z * locator.dimensions.x > 0.0f)
				{
					intensity = -intensity;
				}
				if (intensity <= 0.0f && object.type != 0x1f)
				{
					continue;
				}
				intensity *=
					0.800000011920929f
					+ static_cast<float>(
						environment_rand15(*frame.random_seed))
						* (0.20000000298023224f / 32767.0f);
			}
			if (!ripper_exhaust_node_visible(object, source_node))
			{
				continue;
			}
			const bool ripper_reverse_callback =
				object.type == 0x1f
				&& (object.flight_callback_mode
						== game::FlightCallbackMode::standard_reverse
					|| object.flight_callback_mode
						== game::FlightCallbackMode::provider_reverse);
			const glm::vec3 scale{
				locator.dimensions.x,
				locator.dimensions.y,
				intensity * locator.dimensions.z
					* (ripper_reverse_callback ? -1.0f : 1.0f),
			};
			if (scale.x == 0.0f || scale.y == 0.0f
				|| scale.z == 0.0f)
			{
				continue;
			}
			const glm::mat4 transform =
				source_model_root
					* locator_transform
				* glm::scale(glm::mat4{1.0f}, scale);
			const std::int32_t authored =
				std::clamp<std::int32_t>(locator.subtype, 1, 7);
			const MissionEngineFlare& flare =
				renderer.engine_flares[authored - 1];
			bgfx::setUniform(frontend.uv_rect_uniform, uv);
			bgfx::setUniform(frontend.tint_uniform, tint);
			set_camera_relative_transform(
				transform, frame.camera_position);
			bgfx::setVertexBuffer(0, flare.mesh.vertices);
			bgfx::setIndexBuffer(
				flare.mesh.indices, 0, flare.mesh.index_count);
			bgfx::setTexture(
				0, frontend.texture_sampler, flare.material_a.handle);
			bgfx::setState(
				BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_DEPTH_TEST_GREATER
					// EngineFlare_create (LANCER.EXE 0x00469400)
					// assigns selector one to the shared material.
					| retail_blend_state(
						RetailBlendSelector::additive)
					| BGFX_STATE_MSAA);
			submit_retail_transparent(
				renderer, frontend.mission_rgba_program);
			bgfx::setUniform(frontend.uv_rect_uniform, uv);
			bgfx::setUniform(frontend.tint_uniform, tint);
			set_camera_relative_transform(
				transform, frame.camera_position);
			bgfx::setVertexBuffer(0, flare.mesh.vertices);
			bgfx::setIndexBuffer(
				flare.mesh.indices, 0, flare.mesh.index_count);
			bgfx::setTexture(
				0, frontend.texture_sampler, flare.material_b.handle);
			bgfx::setState(
				BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_DEPTH_TEST_GREATER
					| retail_blend_state(
						RetailBlendSelector::additive)
					| BGFX_STATE_MSAA);
			submit_retail_transparent(
				renderer, frontend.mission_rgba_program);
		}
	};
	for (std::uint32_t instance_index = 0;
		instance_index < frame.instance_count;
		++instance_index)
	{
		const MissionRenderInstance& instance =
			frame.instances[instance_index];
		const game::WorldObject* object = instance.source_object;
		if (object == nullptr)
		{
			continue;
		}
		const MissionGpuModel& model =
			renderer.models[static_cast<std::size_t>(instance.model)];
		submit_model(submit_model, model, instance, *object, true);
		const glm::mat3 root_wobble =
			(object->runtime_flags & 0x00000100u) != 0
				? cloak_wobble(*object, frame.simulation_tick)
				: glm::mat3{1.0f};
		const glm::mat4 instance_transform =
			sl_open::math::model_transform(
				instance.orientation * root_wobble,
				instance.scale,
				instance.position);
		for (std::uint8_t attachment_index = 0;
			attachment_index < object->attachment_count;
			++attachment_index)
		{
			const game::AttachmentSlot& attachment =
				object->attachments[attachment_index];
			if (!attachment.live_model
				|| attachment.definition_index < 0
				|| static_cast<std::size_t>(
					attachment.definition_index)
					>= game::kAttachmentDefinitionCount)
			{
				continue;
			}
			const std::uint32_t variant_index =
				attachment.alternate_model ? 1u : 0u;
			if (!renderer.attachment_model_loaded[
					attachment.definition_index][variant_index])
			{
				continue;
			}
			const MissionGpuModel& mounted_model =
				renderer.attachment_models[
					attachment.definition_index][variant_index];
			glm::mat4 hardpoint_transform =
				glm::translate(
					glm::mat4{1.0f}, attachment.local_position)
				* glm::mat4(attachment.local_orientation);
			if (attachment.model_reference >= 0
				&& static_cast<std::size_t>(
					attachment.model_reference)
					< object->model_references.size())
			{
				hardpoint_transform =
					object->model_references[
						static_cast<std::uint16_t>(
							attachment.model_reference)].scene_transform
					* attachment.hardpoint_from_model;
			}
			hardpoint_transform = hardpoint_transform
				* glm::mat4(sl_open::math::rotation_from_euler(
					{0.0f, -glm::pi<float>(), 0.0f}));
			const glm::mat4 hardpoint_root =
				attachment.model_reference >= 0
					? explosion_portal_root(
						frame,
						*object,
						static_cast<std::uint16_t>(
							attachment.model_reference),
						instance_transform,
						instance.scale)
					: instance_transform;
			const glm::mat4 mounted_root =
				hardpoint_root
					* hardpoint_transform
				* glm::translate(
					glm::mat4{1.0f}, mounted_model.center_of_mass);
			MissionRenderInstance mounted;
			mounted.position = glm::vec3(mounted_root[3]);
			mounted.orientation =
				glm::mat3(mounted_root) / instance.scale;
			mounted.scale = instance.scale;
			mounted.effect_owner = object;
			mounted.effect_root_wobble_applied = true;
			submit_model(
				submit_model,
				mounted_model,
				mounted,
				*object,
				false);
		}
	}
}

void submit_billboard(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const FrontendTexture& texture,
	const glm::vec3& camera_position,
	const glm::vec3& position,
	float size,
	std::uint32_t color,
	float u0,
	float v0,
	float u1,
	float v1,
	const glm::mat3& camera_orientation,
	float height_scale,
	RetailBlendSelector blend,
	float depth_bias)
{
	if (!bgfx::isValid(texture.handle)
		|| get_available_frame_vertices(frontend.frame_geometry, 6, frontend.layout) < 6)
	{
		return;
	}
	const float half = size * 0.5f;
	const glm::vec3 right = camera_orientation[0] * half;
	const glm::vec3 up =
		camera_orientation[1] * (half * height_scale);
	const glm::vec3 relative_position = position - camera_position;
	glm::vec3 points[4] = {
		relative_position - right + up,
		relative_position + right + up,
		relative_position - right - up,
		relative_position + right - up,
	};
	if (depth_bias != 0.0f)
	{
		// SR_bmopipe (0x004ce4d0) projects the rectangle at its original
		// depth, then biases only the depth used for occlusion. Move along
		// each camera ray so the GPU keeps those same screen coordinates.
		const float depth = glm::dot(relative_position, camera_orientation[2]);
		if (depth < kMissionNearPlane)
		{
			return;
		}
		const float depth_scale =
			std::max(depth + depth_bias, kMissionNearPlane) / depth;
		for (glm::vec3& point : points)
		{
			point *= depth_scale;
		}
	}
	const glm::vec2 uv[4] = {
		{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1},
	};
	constexpr std::uint8_t corners[6] = {0, 1, 2, 2, 1, 3};
	FrameVertexBuffer buffer;
	alloc_frame_vertex_buffer(frontend.frame_geometry, &buffer, 6, frontend.layout);
	assets::GameplayVertex* vertices =
		reinterpret_cast<assets::GameplayVertex*>(buffer.data);
	for (std::uint32_t index = 0; index < 6; ++index)
	{
		const std::uint8_t corner = corners[index];
		vertices[index] = {
			points[corner].x,
			points[corner].y,
			points[corner].z,
			color,
			uv[corner].x,
			uv[corner].y,
		};
	}
	const glm::mat4 identity{1.0f};
	const float full_uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
	bgfx::setUniform(frontend.uv_rect_uniform, full_uv);
	bgfx::setUniform(frontend.tint_uniform, tint);
	bgfx::setTransform(glm::value_ptr(identity));
	set_frame_vertex_buffer(0, &buffer);
	bgfx::setTexture(0, frontend.texture_sampler, texture.handle);
	bgfx::setState(
		BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_DEPTH_TEST_GREATER
			| retail_blend_state(blend)
			| BGFX_STATE_MSAA);
	submit_retail_transparent(
		renderer, frontend.mission_rgba_program);
}

void submit_particle_effects(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	for (std::uint32_t index = 0; index < frame.particle_count; ++index)
	{
		const MissionParticle& particle = frame.particles[index];
		submit_billboard(
			renderer,
			frontend,
			particle.missile_trail_texture
				? renderer.missile_trail_texture
				: renderer.particle_texture,
			frame.camera_position,
			particle.position,
			particle.size,
			particle.color,
			particle.u0,
			particle.v0,
			particle.u1,
			particle.v1,
				particle.oriented
					? particle.orientation
					: frame.camera_orientation,
				particle.height_scale);
	}
	if (frame.world == nullptr)
	{
		return;
	}
	for (const game::ExplosionBillboardEffect& explosion
		: frame.world->death_effects.explosion_billboards)
	{
		if (!explosion.active)
		{
			continue;
		}
		const std::int32_t age = static_cast<std::int32_t>(
			frame.simulation_tick - explosion.start_tick)
			- explosion.delay_ticks;
		if (age < 0 || age >= explosion.duration_ticks)
		{
			continue;
		}
		const bool separate_frames =
			explosion.type == game::ExplosionBillboardType::separate_frames;
		const std::uint8_t texture_frame = static_cast<std::uint8_t>(
			age * 16 / explosion.duration_ticks);
		const std::uint8_t atlas_frame = static_cast<std::uint8_t>(
			age * 9 / explosion.duration_ticks);
		const FrontendTexture& texture = explosion.alternate_atlas
			? renderer.flak_explosion_texture
			: (separate_frames
				? renderer.destruction_explosion_textures[
					std::min<std::uint8_t>(texture_frame, 15u)]
				: renderer.explosion_sheet_texture);
		float u0 = 0.0f;
		float v0 = 0.0f;
		float u1 = 1.0f;
		float v1 = 1.0f;
		if (!separate_frames || explosion.alternate_atlas)
		{
			const std::uint8_t column = atlas_frame % 3u;
			const std::uint8_t row = atlas_frame / 3u;
			const float cell_stride = separate_frames
				? 0.33f
				: 0.3203125f;
			const float cell_size = separate_frames
				? 0.33f
				: 0.31640625f;
			u0 = static_cast<float>(column) * cell_stride;
			v0 = static_cast<float>(row) * cell_stride;
			u1 = u0 + cell_size;
			v1 = v0 + cell_size;
		}
		if (!separate_frames)
		{
			if ((explosion.frame_flags & 1u) != 0) std::swap(u0, u1);
			if ((explosion.frame_flags & 2u) != 0) std::swap(v0, v1);
		}
		const float animation_scale = explosion.animate_scale
			? static_cast<float>(age)
				/ static_cast<float>(explosion.duration_ticks)
			: 1.0f;
		submit_billboard(
			renderer,
			frontend,
			texture,
			frame.camera_position,
			explosion.position,
			explosion.size * 2.0f * animation_scale,
			0xffffffffu,
			u0,
			v0,
			u1,
			v1,
			frame.camera_orientation,
			1.0f,
			explosion.alternate_atlas
				? RetailBlendSelector::additive
				: RetailBlendSelector::premultiplied_alpha,
			-explosion.size);
	}
	for (const game::PowercoreEffect& powercore
		: frame.world->death_effects.powercores)
	{
		if (!powercore.active
			|| powercore.owner_index >= game::kMaxGameObjects)
		{
			continue;
		}
		const game::WorldObject& owner =
			frame.world->objects[powercore.owner_index];
		if (!owner.active
			|| owner.generation != powercore.owner_generation
			|| powercore.model_reference >= owner.model_references.size())
		{
			continue;
		}
		const glm::mat4 ordinary_root = math::model_transform(
			owner.scene_orientation, 1.0f, owner.scene_position);
		const glm::mat4 transform = explosion_portal_root(
			frame,
			owner,
			powercore.model_reference,
			ordinary_root,
			1.0f)
			* owner.model_references[
				powercore.model_reference].scene_transform;
		submit_billboard(
			renderer,
			frontend,
			renderer.powercore_texture,
			frame.camera_position,
			glm::vec3(transform[3]),
			powercore.size * 2.0f,
			0xffffffffu,
			0.0f,
			0.0f,
			1.0f,
			1.0f,
			frame.camera_orientation,
			1.0f,
			RetailBlendSelector::additive);
	}
}

std::uint32_t shared_particle_color(
	const game::ParticleVisual& particle)
{
	const auto channel = [](float value)
	{
		return static_cast<std::uint8_t>(std::lrint(
			std::clamp(value, 0.0f, 1.0f) * 255.0f));
	};
	return static_cast<std::uint32_t>(channel(particle.alpha)) << 24
		| static_cast<std::uint32_t>(channel(particle.color.b)) << 16
		| static_cast<std::uint32_t>(channel(particle.color.g)) << 8
		| channel(particle.color.r);
}

void submit_shared_particles(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	if (frame.world == nullptr)
	{
		return;
	}
	for (const game::ParticleArray& array
		: frame.world->particles.arrays)
	{
		if (!array.active)
		{
			continue;
		}
		const std::int32_t high_water = std::min<std::int32_t>(
			array.high_water,
			static_cast<std::int32_t>(array.visuals.size()));
		for (std::int32_t index = 0; index < high_water; ++index)
		{
			const game::ParticleVisual& particle = array.visuals[index];
			if (!particle.active)
			{
				continue;
			}
			submit_billboard(
				renderer,
				frontend,
				array.texture == game::ParticleTexture::partic7
					? renderer.damage_particle_texture
					: renderer.particle_texture,
				frame.camera_position,
				particle.position,
				// Particle_update_all writes the evaluated curve to the BMO
				// record at +0x20/+0x24 (0x0049c996..0x0049c999). The D3D
				// BMO pipeline uses those values as half-extents, constructing
				// the quad at center +/- width and center +/- height
				// (srd3d.dll 0x10011fdf..0x1001200d). submit_billboard takes
				// the complete side length.
				particle.size * 2.0f,
				shared_particle_color(particle),
				particle.rectangle.x,
				particle.rectangle.z,
				particle.rectangle.y,
				particle.rectangle.w,
				frame.camera_orientation,
				1.0f,
				array.additive
					? RetailBlendSelector::additive
					: RetailBlendSelector::premultiplied_alpha);
		}
	}
}

const FrontendTexture& transition_texture(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	game::TransitionTexture texture)
{
	switch (texture)
	{
	case game::TransitionTexture::solid:
		return frontend.white;
	case game::TransitionTexture::jump_trail:
		return renderer.jump_trail_texture;
	case game::TransitionTexture::jump_flare:
		return renderer.jump_flare_texture;
	case game::TransitionTexture::jump_burst:
		return renderer.shield_texture;
	case game::TransitionTexture::jump_light:
		return renderer.jump_light_texture;
	case game::TransitionTexture::jump_light_bright:
		return renderer.jump_light_bright_texture;
	case game::TransitionTexture::warp_primary:
		return renderer.warp_primary_texture;
	case game::TransitionTexture::warp_secondary:
		return renderer.warp_secondary_texture;
	case game::TransitionTexture::particle_flare:
		return renderer.missile_flare_texture;
	}
	return renderer.warp_primary_texture;
}

void submit_transition_mesh(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const game::TransitionMesh& mesh,
	const MissionRenderFrame& frame,
	const glm::mat4* parent_transform = nullptr)
{
	if (!mesh.active || mesh.vertices.empty() || mesh.indices.empty()
		|| mesh.vertices.size() > UINT16_MAX
		|| get_available_frame_vertices(frontend.frame_geometry,
			static_cast<std::uint32_t>(mesh.vertices.size()),
			frontend.layout) < mesh.vertices.size()
		|| get_available_frame_indices(frontend.frame_geometry,
			static_cast<std::uint32_t>(mesh.indices.size()))
			< mesh.indices.size())
	{
		return;
	}
	const FrontendTexture& texture =
		transition_texture(renderer, frontend, mesh.texture);
	if (!bgfx::isValid(texture.handle))
	{
		return;
	}
	FrameVertexBuffer vertices;
	FrameIndexBuffer indices;
	alloc_frame_vertex_buffer(frontend.frame_geometry,
		&vertices,
		static_cast<std::uint32_t>(mesh.vertices.size()),
		frontend.layout);
	alloc_frame_index_buffer(frontend.frame_geometry,
		&indices,
		static_cast<std::uint32_t>(mesh.indices.size()));
	assets::GameplayVertex* output =
		reinterpret_cast<assets::GameplayVertex*>(vertices.data);
	for (std::size_t index = 0; index < mesh.vertices.size(); ++index)
	{
		const game::TransitionVertex& input = mesh.vertices[index];
		output[index] = {
			input.position.x,
			input.position.y,
			input.position.z,
			input.color,
			input.uv.x,
			input.uv.y,
		};
	}
	std::memcpy(
		indices.data,
		mesh.indices.data(),
		mesh.indices.size() * sizeof(std::uint16_t));
	glm::mat4 transform =
		math::model_transform(
			mesh.orientation, mesh.scale, mesh.position);
	if (parent_transform != nullptr)
	{
		transform = *parent_transform * transform;
	}
	const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
	bgfx::setUniform(frontend.uv_rect_uniform, uv);
	bgfx::setUniform(frontend.tint_uniform, tint);
	set_camera_relative_transform(transform, frame.camera_position);
	set_frame_vertex_buffer(0, &vertices);
	set_frame_index_buffer(&indices);
	bgfx::setTexture(0, frontend.texture_sampler, texture.handle);
	std::uint64_t state =
		BGFX_STATE_WRITE_RGB
		| BGFX_STATE_WRITE_A
		| BGFX_STATE_DEPTH_TEST_GREATER
		| BGFX_STATE_MSAA;
	// The retail mesh-pass bit 0x800 bypasses its CPU facing test. Preserve
	// that authored/procedural policy through GPU state: every other
	// triangle mesh gets normal backface culling after it reaches drawing.
	if (!mesh.double_sided)
	{
		state |= BGFX_STATE_CULL_CW;
	}
	state |= transition_blend_state(mesh.blend);
	bgfx::setState(state);
	submit_retail_transparent(
		renderer, frontend.mission_rgba_program);
}

void submit_transition_billboard(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const game::TransitionBillboard& billboard,
	const MissionRenderFrame& frame,
	const glm::mat4* parent_transform = nullptr)
{
	if (!billboard.active || billboard.half_extent.x <= 0.0f)
	{
		return;
	}
	const glm::vec3 position = parent_transform == nullptr
		? billboard.position
		: glm::vec3(
			*parent_transform * glm::vec4(billboard.position, 1.0f));
	submit_billboard(
		renderer,
		frontend,
		transition_texture(
			renderer, frontend, billboard.texture),
		frame.camera_position,
		position,
		billboard.half_extent.x * 2.0f,
		billboard.color,
		0.0f,
		0.0f,
		1.0f,
		1.0f,
		frame.camera_orientation,
		billboard.half_extent.y / billboard.half_extent.x);
}

void submit_transition_effects(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	if (frame.world == nullptr)
	{
		return;
	}
	const game::TransitionEffectsRuntime& effects =
		frame.world->transition_effects;
	for (const game::JumpVisualContext& context
		: effects.jump.contexts)
	{
		if (!context.active)
		{
			continue;
		}
		if (context.owner_index >= game::kMaxGameObjects)
		{
			continue;
		}
		const game::WorldObject& owner =
			frame.world->objects[context.owner_index];
		if (!owner.active || owner.generation != context.owner_generation)
		{
			continue;
		}
		const glm::mat4 owner_transform = math::model_transform(
			owner.scene_orientation, 1.0f, owner.scene_position);
		for (const game::TransitionMesh& trail : context.trails)
		{
			submit_transition_mesh(
				renderer, frontend, trail, frame, &owner_transform);
		}
		// Retail retains and updates the Jump Out burst at context +0x64,
		// but none of AI_JumpOut_update's scene-link tails submit it. Drawing
		// it here produced the large static exhaust-plume mesh on departure.
		submit_transition_mesh(
			renderer,
			frontend,
			context.jump_in_burst,
			frame,
			&owner_transform);
		for (const game::TransitionBillboard& light : context.lights)
		{
			submit_transition_billboard(
				renderer,
				frontend,
				light,
				frame,
				&owner_transform);
		}
		submit_transition_mesh(
			renderer,
			frontend,
			context.flare,
			frame);
	}
	for (const game::WGateContext& context
		: effects.wgate.contexts)
	{
		if (!context.active)
		{
			continue;
		}
		submit_transition_mesh(
			renderer, frontend, context.portal, frame);
		for (const game::TransitionMesh& beam : context.beams)
		{
			submit_transition_mesh(renderer, frontend, beam, frame);
		}
		for (const game::TransitionBillboard& emitter : context.emitters)
		{
			submit_transition_billboard(
				renderer,
				frontend,
				emitter,
				frame);
		}
		for (const game::TransitionMesh& quad : context.fixed_quads)
		{
			submit_transition_mesh(renderer, frontend, quad, frame);
		}
	}
	if (effects.wgate.projector_submission_tick
		== frame.simulation_tick)
	{
		for (const game::WGateProjector& projector
			: effects.wgate.projectors)
		{
			if (!projector.active)
			{
				continue;
			}
			submit_transition_mesh(
				renderer, frontend, projector.beam, frame);
		}
		submit_transition_mesh(
			renderer, frontend, effects.wgate.projector_portal, frame);
	}
	submit_transition_mesh(
		renderer, frontend, effects.wgate.wormhole, frame);
	const game::UberExplosionEffect& uber =
		frame.world->death_effects.uber_explosion;
	if (uber.active)
	{
		submit_transition_mesh(
			renderer, frontend, uber.hemispheres[0], frame);
		submit_transition_mesh(
			renderer, frontend, uber.hemispheres[1], frame);
		submit_transition_mesh(
			renderer, frontend, uber.inner_shell, frame);
	}
	for (const game::TractorEffect& effect
		: frame.world->death_effects.tractor)
	{
		if (!effect.active || !effect.submitted)
		{
			continue;
		}
		for (const game::TransitionMesh& beam : effect.beams)
		{
			submit_transition_mesh(renderer, frontend, beam, frame);
		}
		submit_transition_mesh(
			renderer, frontend, effect.shield, frame);
		submit_transition_billboard(
			renderer,
			frontend,
			effect.light,
			frame);
	}
	for (const game::RipperGrabEffect& effect
		: frame.world->death_effects.ripper_grab)
	{
		if (!effect.active || !effect.submitted)
		{
			continue;
		}
		for (const game::TransitionMesh& beam : effect.beams)
		{
			submit_transition_mesh(renderer, frontend, beam, frame);
		}
	}
	for (const game::RespawnEffect& effect
		: frame.world->death_effects.respawn)
	{
		if (!effect.active || !effect.submitted)
		{
			continue;
		}
		submit_transition_mesh(
			renderer, frontend, effect.portal, frame);
		submit_transition_mesh(
			renderer, frontend, effect.projection, frame);
	}
}

bool environment_sphere_visible(
	const glm::vec3& position,
	float radius,
	const MissionRenderFrame& frame,
	std::uint32_t drawable_width)
{
	const glm::vec3 view_position =
		glm::transpose(frame.camera_orientation)
		* (position - frame.camera_position);
	const float horizontal_pixel_scale =
		(static_cast<float>(drawable_width) - 0.1f)
		/ (2.0f * frame.horizontal_tangent);
	return math::model_bounding_sphere_visible(
		view_position,
		radius,
		frame.horizontal_tangent,
		frame.vertical_tangent,
		horizontal_pixel_scale,
		kMissionNearPlane);
}

void submit_environment_mesh(
	const MissionEnvironmentMesh& mesh,
	const FrontendRenderer& frontend,
	bgfx::TextureHandle texture,
	const glm::mat4& transform,
	float brightness,
	std::uint64_t state,
	bgfx::ViewId view)
{
	if (!bgfx::isValid(mesh.vertices)
		|| !bgfx::isValid(mesh.indices)
		|| mesh.index_count == 0)
	{
		return;
	}
	bgfx::setTransform(glm::value_ptr(transform));
	bgfx::setVertexBuffer(0, mesh.vertices);
	bgfx::setIndexBuffer(mesh.indices, 0, mesh.index_count);
	bgfx::setTexture(0, frontend.texture_sampler, texture);
	const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {brightness, brightness, brightness, 1.0f};
	bgfx::setUniform(frontend.uv_rect_uniform, uv);
	bgfx::setUniform(frontend.tint_uniform, tint);
	bgfx::setState(state);
	bgfx::submit(view, frontend.mission_rgba_program);
}

void submit_nebula(
	const MissionEnvironmentRenderer& environment,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame,
	float brightness)
{
	if (!environment.initialized)
	{
		return;
	}
	std::int32_t material = 0;
	glm::mat3 dome_orientation{1.0f};
	glm::mat3 grid_orientation = math::postrotate(
		glm::mat3{1.0f},
		-glm::half_pi<float>(),
		{0.0f, 1.0f, 0.0f});
	if (frame.environment != nullptr)
	{
		material = frame.environment->applied_nebula_material;
		dome_orientation = frame.environment->nebula_dome_orientation;
		grid_orientation = frame.environment->nebula_grid_orientation;
	}

	// Nebula_render (LANCER.EXE 0x00498e10) submits the opaque accelerated
	// dome before the textured additive grid. Keeping that composition order
	// is essential: reversing it erases the grid everywhere the dome covers.
	const glm::mat4 dome_transform{dome_orientation};
	submit_environment_mesh(
		environment.nebula_dome,
		frontend,
		frontend.white.handle,
		dome_transform,
		brightness,
		BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A,
		kMissionEnvironmentView);

	if (material >= 0 && material < 7)
	{
		const MissionEnvironmentMesh& grid = material == 5
			? environment.nebula_grid_narrow
			: environment.nebula_grid_broad;
		const glm::mat4 grid_transform{grid_orientation};
		// Nebula_accelerated_grid_create (LANCER.EXE 0x00498ea0)
		// writes material blend selector one. The retail driver tables
		// initialized at srd3d.dll 0x100058f1 map that selector to
		// D3DBLEND_ONE/D3DBLEND_ONE. These textures are RGB565 and carry no
		// source alpha, so substituting alpha blending produces an opaque,
		// visibly clipped patch.
		submit_environment_mesh(
			grid,
			frontend,
			environment.nebula_materials[material].handle,
			grid_transform,
			brightness,
			BGFX_STATE_WRITE_RGB
				| BGFX_STATE_WRITE_A
				| retail_blend_state(
					RetailBlendSelector::additive),
			kMissionNebulaGridView);
	}
}

void submit_far_asteroids(
	const MissionEnvironmentRenderer& environment,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame)
{
	if (!environment.initialized
		|| frame.environment == nullptr
		|| (frame.environment->current_mask & 1u) == 0)
	{
		return;
	}
	std::uint32_t visible_count = 0;
	for (std::uint32_t index = 0;
		index < environment.far_asteroid_count;
		++index)
	{
		if (glm::dot(
				frame.camera_orientation[2],
				environment.far_asteroids[index].camera_offset)
			> 1625.0f
			&& environment.far_asteroids[index].uv_band < 5)
		{
			++visible_count;
		}
	}
	if (visible_count == 0)
	{
		return;
	}
	const std::uint32_t vertex_count = visible_count * 3;
	if (get_available_frame_vertices(frontend.frame_geometry,
			vertex_count, environment.layout) < vertex_count)
	{
		return;
	}
	FrameVertexBuffer buffer;
	alloc_frame_vertex_buffer(frontend.frame_geometry,
		&buffer, vertex_count, environment.layout);
	EnvironmentVertex* output =
		reinterpret_cast<EnvironmentVertex*>(buffer.data);
	constexpr float kUv[5][6] = {
		{0.25f, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f},
		{0.75f, 0.0f, 0.5f, 0.5f, 1.0f, 0.5f},
		{0.25f, 0.5f, 0.0f, 1.0f, 0.5f, 1.0f},
		{0.75f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f},
		{0.25f, 0.0f, 0.5f, 0.5f, 0.75f, 0.0f},
	};
	constexpr glm::vec2 kLocal[3] = {
		{0.0f, 0.0f}, {-0.5f, 1.0f}, {0.5f, 1.0f},
	};
	std::uint32_t output_index = 0;
	for (std::uint32_t index = environment.far_asteroid_count;
		index-- > 0;)
	{
		const MissionFarAsteroid& asteroid =
			environment.far_asteroids[index];
		if (asteroid.uv_band >= 5
			|| glm::dot(
				frame.camera_orientation[2],
				asteroid.camera_offset) <= 1625.0f)
		{
			continue;
		}
		const glm::vec3 center = asteroid.camera_offset;
		const glm::vec3 facing = glm::normalize(-center);
		glm::vec3 right = glm::cross(
			frame.camera_orientation[1], facing);
		if (glm::dot(right, right) < 0.000001f)
		{
			right = frame.camera_orientation[0];
		}
		else
		{
			right = glm::normalize(right);
		}
		glm::vec3 up = glm::normalize(glm::cross(facing, right));
		const float roll =
			static_cast<float>(frame.simulation_tick)
				* asteroid.roll_rate;
		const float sine = std::sin(roll);
		const float cosine = std::cos(roll);
		const glm::vec3 rolled_right = right * cosine + up * sine;
		const glm::vec3 rolled_up = up * cosine - right * sine;
		for (std::uint32_t vertex = 0; vertex < 3; ++vertex)
		{
			const glm::vec3 position =
				center
				+ rolled_right
					* (kLocal[vertex].x * asteroid.scale)
				+ rolled_up
					* (kLocal[vertex].y * asteroid.scale);
			output[output_index++] = {
				position.x,
				position.y,
				position.z,
				asteroid.brightness,
				asteroid.brightness,
				asteroid.brightness,
				1.0f,
				kUv[asteroid.uv_band][vertex * 2],
				kUv[asteroid.uv_band][vertex * 2 + 1],
			};
		}
	}
	const glm::mat4 identity{1.0f};
	const float full_uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
	bgfx::setUniform(frontend.uv_rect_uniform, full_uv);
	bgfx::setUniform(frontend.tint_uniform, tint);
	bgfx::setTransform(glm::value_ptr(identity));
	set_frame_vertex_buffer(0, &buffer);
	bgfx::setTexture(
		0,
		frontend.texture_sampler,
		environment.far_asteroid_texture.handle);
	bgfx::setState(
		BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			// FarAsteroids_create (LANCER.EXE 0x0046a250) assigns retail
			// selector two, ONE/INVSRCALPHA.
			| retail_blend_state(
				RetailBlendSelector::premultiplied_alpha)
			| BGFX_STATE_MSAA);
	bgfx::submit(
		kMissionFarAsteroidView, frontend.mission_rgba_program);
}

struct MissionSunFrame
{
	glm::vec3 camera_position{0.0f};
	float flare{};
	float previous_edge_distance{};
};

MissionSunFrame update_sun_frame(
	MissionEnvironmentRenderer& environment,
	const MissionRenderFrame& frame,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height)
{
	const glm::vec3 sun_direction =
		frame.environment != nullptr
			? frame.environment->sun_direction
			: glm::normalize(glm::vec3{1.0f, -0.5f, 0.2f});
	MissionSunFrame output;
	output.camera_position =
		glm::transpose(frame.camera_orientation)
		* (sun_direction * 1000.0f);
	output.previous_edge_distance =
		environment.previous_sun_edge_distance;
	if (output.camera_position.z == 0.0f)
	{
		// Space_render (LANCER.EXE 0x004a5d8b/0x004a62b3) cannot project
		// an exactly edge-on sun and clears the retained edge distance.
		environment.previous_sun_edge_distance = 0.0f;
		return output;
	}
	const float nx = output.camera_position.x / output.camera_position.z;
	const float ny = output.camera_position.y / output.camera_position.z;
	const float radial = 1.0f - std::min(
		std::sqrt(nx * nx + ny * ny), 1.0f);
	output.flare =
		(output.previous_edge_distance * 0.05f + 0.5f)
		* radial;
	const float half_height = frame.vertical_tangent;
	const float half_width = frame.horizontal_tangent;
	const glm::vec2 screen = sl_open::math::camera_plane_to_framebuffer(
		{nx, ny},
		{half_width, half_height},
		{
			static_cast<float>(drawable_width),
			static_cast<float>(drawable_height),
		});
	const float screen_x = screen.x;
	const float screen_y = screen.y;
	environment.previous_sun_edge_distance = std::max(
		0.0f,
		std::min({
			10.0f,
			screen_x,
			screen_y,
			static_cast<float>(drawable_width) - screen_x,
			static_cast<float>(drawable_height) - screen_y,
		}));
	return output;
}

void submit_sun_billboard(
	const FrontendRenderer& frontend,
	const FrontendTexture& texture,
	const MissionRenderFrame& frame,
	const glm::vec3& camera_position,
	float width_scale,
	float brightness)
{
	if (!bgfx::isValid(texture.handle)
		|| camera_position.z <= 0.0f
		|| get_available_frame_vertices(frontend.frame_geometry, 6, frontend.layout) < 6)
	{
		return;
	}
	const float width = static_cast<float>(texture.width)
		* camera_position.z * (1.0f / 768.0f) * width_scale;
	const float height = static_cast<float>(texture.height)
		* camera_position.z * (1.0f / 768.0f) * width_scale;
	const glm::vec3 right = frame.camera_orientation[0] * (width * 0.5f);
	const glm::vec3 up = frame.camera_orientation[1] * (height * 0.5f);
	const glm::vec3 center =
		frame.camera_orientation * camera_position;
	const glm::vec3 points[4] = {
		center - right + up,
		center + right + up,
		center - right - up,
		center + right - up,
	};
	const glm::vec2 uv[4] = {
		{0.0f, 0.0f}, {1.0f, 0.0f},
		{0.0f, 1.0f}, {1.0f, 1.0f},
	};
	constexpr std::uint8_t kCorners[6] = {0, 1, 2, 2, 1, 3};
	FrameVertexBuffer buffer;
	alloc_frame_vertex_buffer(frontend.frame_geometry, &buffer, 6, frontend.layout);
	assets::GameplayVertex* vertices =
		reinterpret_cast<assets::GameplayVertex*>(buffer.data);
	const std::uint32_t color = pack_model_diffuse({
		brightness, brightness, brightness, 1.0f});
	for (std::uint32_t index = 0; index < 6; ++index)
	{
		const std::uint8_t corner = kCorners[index];
		vertices[index] = {
			points[corner].x,
			points[corner].y,
			points[corner].z,
			color,
			uv[corner].x,
			uv[corner].y,
		};
	}
	const glm::mat4 identity{1.0f};
	const float full_uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	bgfx::setUniform(frontend.uv_rect_uniform, full_uv);
	bgfx::setTransform(glm::value_ptr(identity));
	set_frame_vertex_buffer(0, &buffer);
	bgfx::setTexture(0, frontend.texture_sampler, texture.handle);
	const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
	bgfx::setUniform(frontend.tint_uniform, tint);
	bgfx::setState(
		BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| retail_blend_state(RetailBlendSelector::additive)
			| BGFX_STATE_MSAA);
	bgfx::submit(kMissionSunView, frontend.mission_rgba_program);
}

void submit_sun(
	MissionEnvironmentRenderer& environment,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame,
	const MissionSunFrame& sun)
{
	// Space_render (LANCER.EXE 0x004a5cd0) submits these BMO objects to
	// the prepend-only background list. The order below is the resulting
	// retail traversal order, not the source call order.
	submit_sun_billboard(
		frontend,
		environment.sun_layers[0],
		frame,
		sun.camera_position,
		0.5f,
		1.0f);

	if (sun.flare > 0.0f
		&& (frame.camera_mode != 0
			|| (frame.camera_view_state == 2
				&& sun.previous_edge_distance > 0.0f)))
	{
		struct LensFlare
		{
			std::uint8_t texture;
			float position_scale;
		};
		constexpr LensFlare kTraversal[] = {
			{3, -0.5f},
			{2, -0.6000000238418579f},
			{1, -0.20000000298023224f},
			{2, 0.20000000298023224f},
			{0, 0.3333333432674408f},
			{1, 0.5f},
		};
		for (const LensFlare& flare : kTraversal)
		{
			const glm::vec3 position{
				sun.camera_position.x * flare.position_scale,
				sun.camera_position.y * flare.position_scale,
				sun.camera_position.z,
			};
			submit_sun_billboard(
				frontend,
				environment.sun_flares[flare.texture],
				frame,
				position,
				1.0f,
				sun.flare);
		}
	}

	if (sun.flare > 0.0f)
	{
		const float layer_two = std::min(sun.flare * 2.0f, 1.0f);
		environment.sun_layer_three_brightness =
			sun.flare < 0.800000011920929f
				? sun.flare * 0.15000000596046448f + 0.1f
				: sun.flare * 0.30000001192092896f;
		submit_sun_billboard(
			frontend,
			environment.sun_layers[1],
			frame,
			sun.camera_position,
			2.0f,
			layer_two);
	}
	if (sun.previous_edge_distance > 0.5f)
	{
		submit_sun_billboard(
			frontend,
			environment.sun_layers[2],
			frame,
			sun.camera_position,
			2.0f,
			environment.sun_layer_three_brightness);
	}
}

void update_planet_atmosphere_spin(
	MissionEnvironmentRenderer& environment,
	game::World& world,
	std::uint32_t tick)
{
	const std::uint32_t delta = environment.atmosphere_tick_valid
		? tick - environment.last_atmosphere_tick
		: 0;
	environment.last_atmosphere_tick = tick;
	environment.atmosphere_tick_valid = true;
	constexpr float kSpinPerTick = 0.000699999975040555f;
	for (std::uint8_t index = 0;
		index < world.atmosphere_count;
		++index)
	{
		game::PlanetAtmosphere& atmosphere = world.atmospheres[index];
		atmosphere.spin_radians = std::fmod(
			atmosphere.spin_radians
				+ static_cast<float>(delta) * kSpinPerTick,
			glm::two_pi<float>());
	}
}

const game::PlanetAtmosphere* planet_atmosphere_for_object(
	const game::World& world,
	const game::WorldObject* object)
{
	if (object == nullptr)
	{
		return nullptr;
	}
	const std::uint16_t object_index = static_cast<std::uint16_t>(
		object - std::begin(world.objects));
	for (std::uint8_t index = 0; index < world.atmosphere_count; ++index)
	{
		const game::PlanetAtmosphere& atmosphere =
			world.atmospheres[index];
		if (atmosphere.owner.index == object_index
			&& atmosphere.owner.generation == object->generation)
		{
			return &atmosphere;
		}
	}
	return nullptr;
}

MissionRenderInstance with_planet_spin(
	const MissionRenderInstance& source,
	const game::World* world)
{
	MissionRenderInstance output = source;
	if (world == nullptr)
	{
		return output;
	}
	const game::PlanetAtmosphere* atmosphere =
		planet_atmosphere_for_object(*world, source.source_object);
	if (atmosphere != nullptr)
	{
		output.orientation = math::postrotate(
			output.orientation,
			atmosphere->spin_radians,
			{0.0f, 1.0f, 0.0f});
	}
	return output;
}

void submit_planet_atmospheres(
	const MissionRenderer& renderer,
	MissionEnvironmentRenderer& environment,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame,
	float flare,
	float scene_brightness)
{
	if (!environment.initialized || frame.world == nullptr)
	{
		return;
	}
	update_planet_atmosphere_spin(
		environment, *frame.world, frame.simulation_tick);
	if (flare <= 0.0f)
	{
		return;
	}
	// Atmosphere objects enter the object list in ascending order. The mesh
	// callback encounters them in reverse, then the blended-face queue
	// reverses that encounter order a second time.
	for (std::uint8_t index = 0;
		index < frame.world->atmosphere_count;
		++index)
	{
		const game::PlanetAtmosphere& atmosphere =
			frame.world->atmospheres[index];
		const game::WorldObject* owner =
			game::world_resolve(*frame.world, atmosphere.owner);
		if (owner == nullptr || !owner->visible
			|| (owner->runtime_flags & game::kObjectFlagDisabled) != 0
			|| owner->radius <= 0.0f)
		{
			continue;
		}
		const glm::vec3 facing_delta =
			frame.camera_position - owner->scene_position;
		if (glm::dot(facing_delta, facing_delta) <= 0.0f)
		{
			continue;
		}
		const glm::vec3 forward = glm::normalize(facing_delta);
		glm::vec3 right = glm::cross(
			glm::vec3{0.0f, 1.0f, 0.0f}, forward);
		if (glm::dot(right, right) < 0.000001f)
		{
			right = frame.camera_orientation[0];
		}
		else
		{
			right = glm::normalize(right);
		}
		const glm::vec3 up = glm::normalize(glm::cross(forward, right));
		const float radius = renderer.models[owner->type].atmosphere_radius;
		glm::mat4 transform{1.0f};
		transform[0] = glm::vec4(right * radius, 0.0f);
		transform[1] = glm::vec4(up * radius, 0.0f);
		transform[2] = glm::vec4(forward * radius, 0.0f);
		transform[3] = glm::vec4(owner->scene_position, 1.0f);
		const MissionEnvironmentMesh* mesh =
			&environment.planet_atmosphere;
		glm::vec3 color{0.2f, 0.2f, 0.15f};
		if (owner->type == 0x5f || owner->type == 0xc9)
		{
			mesh = &environment.planet_atmosphere_compact;
			color = {0.1f, 0.15f, 0.2f};
		}
		else if (owner->type == 0x61 || owner->type == 0xcb)
		{
			mesh = &environment.planet_atmosphere_compact;
			color = {0.15f, 0.2f, 0.2f};
		}
		const float tint[] = {
			color.r * scene_brightness,
			color.g * scene_brightness,
			color.b * scene_brightness,
			flare,
		};
		const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
		bgfx::setUniform(frontend.uv_rect_uniform, uv);
		bgfx::setUniform(frontend.tint_uniform, tint);
		set_camera_relative_transform(transform, frame.camera_position);
		bgfx::setVertexBuffer(0, mesh->vertices);
		bgfx::setIndexBuffer(
			mesh->indices,
			0,
			mesh->index_count);
		bgfx::setTexture(
			0,
			frontend.texture_sampler,
			environment.planet_atmosphere_texture.handle);
		bgfx::setState(
			BGFX_STATE_WRITE_RGB
				| BGFX_STATE_WRITE_A
				// Planet atmosphere construction at LANCER.EXE
				// 0x0046795d assigns retail selector four,
				// SRCALPHA/ONE.
				| retail_blend_state(
					RetailBlendSelector::source_alpha_additive)
				| BGFX_STATE_MSAA);
		bgfx::submit(
			kMissionAtmosphereView, frontend.mission_rgba_program);
	}
}

void activate_planet_bombard(
	MissionPlanetBombardSlot& slot,
	const MissionPlanetBombardOwner& owner,
	std::uint32_t& random_seed)
{
	slot.active = true;
	glm::vec3 euler;
	switch (environment_rand15(random_seed) % 3u)
	{
	case 0: euler = {-0.5f, 1.0f, 0.0f}; break;
	case 1: euler = {0.2f, 4.0f, 0.0f}; break;
	default: euler = {0.4f, 7.0f, 0.0f}; break;
	}
	euler.x += environment_random_unit(random_seed)
		* (0.3f * glm::pi<float>());
	euler.y += environment_random_unit(random_seed)
		* (0.3f * glm::pi<float>());
	euler.z += environment_random_unit(random_seed)
		* (0.3f * glm::pi<float>());
	slot.local_orientation = math::rotation_from_euler(euler);
	slot.local_position =
		slot.local_orientation
		* glm::vec3{0.0f, 0.0f, owner.radius};
}

void planet_bombard_callback(
	MissionEnvironmentRenderer& environment,
	const MissionPlanetBombardOwner& owner,
	std::uint32_t now,
	std::uint32_t& random_seed)
{
	for (MissionPlanetBombardSlot& slot : environment.bombard)
	{
		const std::int32_t elapsed =
			static_cast<std::int32_t>(now)
			- slot.scheduled_or_start_tick;
		if (!slot.active || elapsed < 35)
		{
			if (!slot.active
				&& slot.scheduled_or_start_tick
					< static_cast<std::int32_t>(now))
			{
				activate_planet_bombard(slot, owner, random_seed);
			}
			if (!slot.active)
			{
				slot.brightness = 0.0f;
				for (glm::vec4& color : slot.vertex_color)
				{
					color = {0.0f, 0.0f, 0.0f, 1.0f};
				}
				continue;
			}
			slot.uv[0] = {0.0f, 1.0f};
			slot.uv[1] = {1.0f, 1.0f};
			slot.uv[2] = {0.0f, 0.0f};
			slot.uv[3] = {1.0f, 1.0f};
			slot.uv[4] = {1.0f, 0.0f};
			slot.uv[5] = {0.0f, 0.0f};
			const std::int32_t active_elapsed =
				static_cast<std::int32_t>(now)
				- slot.scheduled_or_start_tick;
			const std::int32_t frame = static_cast<std::int32_t>(
				std::lrint(
					static_cast<float>(active_elapsed)
					* (16.0f / 35.0f)));
			slot.scale = frame < 8 && frame >= 0
				? g_planet_bombard_scale[
					kPlanetBombardFrameMap[frame]]
				: 2.75f;
			const float age =
				static_cast<float>(active_elapsed) / 35.0f;
			slot.brightness = age < 0.5f
				? 0.1f
				: (1.0f - 2.0f * (age - 0.5f)) * 0.3f;
			for (glm::vec4& color : slot.vertex_color)
			{
				color = {
					slot.brightness,
					slot.brightness,
					slot.brightness,
					1.0f,
				};
			}
		}
		else
		{
			slot.active = false;
			slot.scheduled_or_start_tick =
				static_cast<std::int32_t>(
					std::lrint(
						static_cast<float>(now)
						+ environment_random_unit(random_seed)
							* 1000.0f));
			// Retail leaves the prior vertex brightness in the dormant mesh
			// until the next inactive callback writes zero.
		}
	}
	environment.last_bombard_callback_tick = now;
}

void service_planet_bombard(
	MissionEnvironmentRenderer& environment,
	const MissionRenderFrame& frame,
	std::uint32_t drawable_width)
{
	if (!environment.initialized
		|| frame.environment == nullptr
		|| frame.random_seed == nullptr
		|| (frame.environment->current_mask & 4u) == 0
		|| frame.bombard_owner_count == 0)
	{
		return;
	}
	// Object construction overwrites the global parent with every eligible
	// type-99/type-205 object; the last-created surviving owner is therefore
	// used for activations performed by every eligible callback.
	const MissionPlanetBombardOwner* parent =
		&frame.bombard_owners[0];
	for (std::uint32_t index = 1;
		index < frame.bombard_owner_count;
		++index)
	{
		if (frame.bombard_owners[index].creation_serial
			> parent->creation_serial)
		{
			parent = &frame.bombard_owners[index];
		}
	}
	for (std::uint32_t index = 0;
		index < frame.bombard_owner_count;
		++index)
	{
		const MissionPlanetBombardOwner& callback_owner =
			frame.bombard_owners[index];
		if (!callback_owner.visible
			|| !environment_sphere_visible(
				callback_owner.position,
				callback_owner.radius,
				frame,
				drawable_width))
		{
			continue;
		}
		planet_bombard_callback(
			environment,
			*parent,
			frame.simulation_tick,
			*frame.random_seed);
	}
	// No Boom Mesh is submitted here. Exhaustive retail xrefs show resource
	// construction, callback mutation, parenting, and destruction only; the
	// missing scene-list link is a retained original integration defect.
}

const game::RespawnEffect* respawn_model_override(
	const MissionRenderFrame& frame,
	const game::WorldObject* source_object,
	const game::ObjectModelReference* runtime_model)
{
	if (frame.world == nullptr
		|| source_object == nullptr
		|| runtime_model == nullptr
		|| runtime_model->respawn_render_override_slot < 0)
	{
		return nullptr;
	}
	const std::size_t slot = static_cast<std::size_t>(
		runtime_model->respawn_render_override_slot);
	if (slot >= frame.world->death_effects.respawn.size())
	{
		return nullptr;
	}
	const game::RespawnEffect& effect =
		frame.world->death_effects.respawn[slot];
	const game::WorldObject* owner = game::world_resolve(
		*frame.world,
		{effect.owner_index, effect.owner_generation});
	return effect.active && owner == source_object
		? &effect
		: nullptr;
}

template<typename SubmitChildren>
void submit_instance_pass(
	const MissionRenderer& renderer,
	const MissionGpuModel& model,
	const MissionRenderInstance& instance,
	const FrontendRenderer& frontend,
	const FrontendTexture& cloak_texture,
	const MissionSceneLights& scene_lights,
	const MissionRenderFrame& frame,
	std::uint32_t simulation_tick,
	SubmitChildren&& submit_children)
{
	const game::WorldObject* source_object =
		instance.effect_owner != nullptr
			? instance.effect_owner
			: instance.source_object;
	const bool cloak_active =
		source_object != nullptr
		&& (source_object->runtime_flags & 0x00000100u) != 0;
	const glm::mat3 wobble = cloak_active
		&& !instance.effect_root_wobble_applied
		? cloak_wobble(*source_object, simulation_tick)
		: glm::mat3{1.0f};
	const glm::mat4 instance_transform =
		sl_open::math::model_transform(
			instance.orientation * wobble,
			instance.scale,
			instance.position);
	const bgfx::ViewId opaque_view = instance.foreground_overlay
		? kMissionCockpitView
		: kMissionView;
	const bgfx::ViewId transparent_view = instance.foreground_overlay
		? kMissionCockpitView
		: kMissionTransparentView;
	// Objects_render_model_tree_and_process_destroyed_parts (0x0049a8c0)
	// visits the authored model tree in ascending/preorder and prepends each
	// drawable mesh. Scene traversal therefore visits the separately retained
	// preorder runtime index in reverse; serialized node order is not used.
	for (std::uint32_t preorder_index =
			static_cast<std::uint32_t>(
				model.node_indices_in_preorder.size());
		preorder_index-- > 0;)
	{
		const std::uint32_t node_index =
			model.node_indices_in_preorder[preorder_index];
		const MissionGpuNode& node = model.nodes[node_index];
		// Explosion_fragment_spawn/Rock_chunk_create (0x004717d0/0x00472a00)
		// pass the first SRO LOD set straight to SR_mesh_create. They never
		// instantiate the model tree, exported translations, or locators.
		if (instance.direct_mesh && node_index != 0)
		{
			continue;
		}
		if (node.runtime_model_index >= instance.submitted_node_count)
		{
			continue;
		}
		const std::uint16_t runtime_reference =
			static_cast<std::uint16_t>(
				instance.runtime_reference_base
				+ node.runtime_model_index);
		const game::ObjectModelReference* runtime_model = nullptr;
		if (instance.source_object != nullptr
			&& runtime_reference
				< instance.source_object->model_references.size())
		{
			runtime_model =
				&instance.source_object->model_references[
					runtime_reference];
		}
		const game::CloakMeshRuntime* cloak_runtime =
			runtime_model != nullptr
				? &runtime_model->cloak
				: instance.effect_cloak_models != nullptr
					&& node.runtime_model_index
						< instance.effect_cloak_models->size()
					? &(*instance.effect_cloak_models)[
						node.runtime_model_index]
					: nullptr;
		// Objects_render_model_tree_and_process_destroyed_parts
		// (0x0049a8c0) skips removed nodes and runtime suppression bit
		// 0x20. A destroyed group removes its live nodes but clears 0x20
		// on its authored DEST alternate, revealing that exact mesh.
		if (runtime_model != nullptr
			&& (runtime_model->removed
				|| (runtime_model->runtime_flags
					& kRuntimeModelHidden) != 0))
		{
			continue;
		}
		if (!instance.direct_mesh && runtime_model == nullptr
			&& (node.flags & 0x0004u) != 0)
		{
			continue;
		}
		bool portal_visible = true;
		const glm::mat4 node_root =
			instance.source_object != nullptr
				? explosion_portal_root(
					frame,
					*instance.source_object,
					runtime_reference,
					instance_transform,
					instance.scale,
					&portal_visible)
				: instance_transform;
		if (!portal_visible)
		{
			continue;
		}
		// Retail instantiates every tag-9 locator object as a child of the
		// source model node. The model service walks the node before those
		// children, while scene insertion prepends every drawable. Emit the
		// children first here to reproduce the resulting reverse preorder.
		if (!instance.direct_mesh)
		{
			submit_children(node_index);
		}
		const bool cloak_node =
			cloak_active
			&& cloak_runtime != nullptr
			&& cloak_runtime->installed;
		const glm::mat4 transform =
			node_root
				* (instance.direct_mesh
					? glm::mat4{1.0f}
					: runtime_model != nullptr
					? runtime_model->scene_transform
					: instance.node_transform_override != nullptr
						? instance.node_transform_override[
							node.runtime_model_index]
						: node.object_transform);
		const game::RespawnEffect* respawn_override =
			respawn_model_override(
				frame, instance.source_object, runtime_model);
		glm::vec4 local_respawn_clip_plane{0.0f};
		glm::vec4 local_explosion_clip_plane{0.0f};
		const glm::vec4* local_clip_plane = nullptr;
		if (respawn_override != nullptr)
		{
			// Respawn_Material's 0x100 modifier clips every retained node
			// against one actor-space Z plane. Convert that plane into this
			// node's local coordinates so animated and embedded meshes share
			// the same reveal edge, as FUN_004adee0/0x004ccf30 do.
			const glm::mat4 actor_root =
				sl_open::math::model_transform(
					instance.source_object->scene_orientation,
					instance.scale,
					instance.source_object->scene_position);
			const glm::mat4 node_to_actor =
				glm::inverse(actor_root) * transform;
			local_respawn_clip_plane = {
				node_to_actor[0][2],
				node_to_actor[1][2],
				node_to_actor[2][2],
				node_to_actor[3][2]
					- respawn_override->model_clip_z,
			};
			local_clip_plane = &local_respawn_clip_plane;
		}
		const bool explosion_clipped = runtime_model != nullptr
			&& explosion_portal_local_clip_plane(
				frame,
				*instance.source_object,
				runtime_reference,
				transform,
				local_explosion_clip_plane);
		const glm::vec4 disabled_model_clip{0.0f, 0.0f, 0.0f, 1.0f};
		const glm::vec4& model_clip_plane = explosion_clipped
			? local_explosion_clip_plane
			: disabled_model_clip;
		float lod_blend = 0.0f;
		// SR_mesh_select_lod_and_classify_aabb (0x004c5fb0) uses the node
		// origin's camera-space Z for ships as well as other model trees.
		// A single-LOD set still has a cutoff; source SRO flag 8 is unused.
		const float view_depth =
			(glm::transpose(frame.camera_orientation)
				* (glm::vec3(transform[3]) - frame.camera_position)).z;
		const MissionGpuLod* lod = choose_lod(
			node,
			view_depth,
			renderer.lod_detail_scale,
			runtime_model != nullptr
				&& (runtime_model->runtime_flags
					& kRuntimeModelForceHighestDetail) != 0,
			instance.direct_mesh ? nullptr : &lod_blend);
		if (lod == nullptr
			|| !bgfx::isValid(lod->indices))
		{
			continue;
		}
		const float normal_blend = (node.flags & 0x0010u) != 0 ? lod_blend : 0.0f;
		const float position_blend = (node.flags & 0x0020u) != 0 ? lod_blend : 0.0f;
		const std::size_t lod_index = static_cast<std::size_t>(
			lod - node.lods.data());
		if (runtime_model != nullptr)
		{
			set_explosion_source_mesh(*runtime_model, *lod);
		}
		if (cloak_runtime != nullptr
			&& lod_index <= UINT16_MAX)
		{
			cloak_runtime->selected_lod =
				static_cast<std::uint16_t>(lod_index);
		}
		const glm::mat4 object_to_camera =
			math::camera_rotation_view(frame.camera_orientation)
				* math::camera_relative_transform(
					transform, frame.camera_position);
		const retail_clip::Frustum frustum{
			frame.horizontal_tangent,
			frame.vertical_tangent,
			kMissionNearPlane,
		};
		// Submit every enabled node in the model tree. Large capital-ship
		// children can have authored bounds that do not follow their fully
		// composed runtime transform, so rejecting individual nodes against
		// the camera frustum makes otherwise-visible ship sections disappear.
		// Modern hardware clips the complete capital mesh against its retained
		// destruction portal in the model shaders. CPU polygon reconstruction
		// remains only for the gameplay respawn material.
		const bool clip_polygons = local_clip_plane != nullptr;
		const bool static_lighting_enabled =
			!instance.direct_mesh && (node.flags & 0x0040u) != 0
			&& (runtime_model == nullptr
				|| (runtime_model->render_flags
					& 0x00040000u) != 0);
		const bool planet = runtime_model != nullptr
			&& (runtime_model->render_flags & 0x00100000u) != 0;
		const bool highlighted =
			node.part_group_id == instance.selected_part_group_id
				&& runtime_model != nullptr
				&& runtime_model->owner_scope
					== instance.selected_part_owner_scope
				&& node.subsystem_highlightable
				&& (node.flags & 0x0004u) == 0;
		const std::uint32_t exclusion_mask =
			instance.direct_mesh ? 0u : runtime_model != nullptr
				? runtime_model->light_exclusion_mask
				: ((model.root_flags
					& assets::kGameplayModelRootCompound) == 0
						? 3u
						: 0x18u);
		GpuMeshLighting gpu_lighting_constants;
		const bool gpu_lighting =
			!clip_polygons
			&& !cloak_node
			&& !highlighted
			&& bgfx::isValid(lod->lit_vertices)
			&& prepare_gpu_mesh_lighting(
				lod->radius,
				transform,
				frame.camera_orientation,
				instance.scale,
				normal_blend,
				exclusion_mask,
				static_lighting_enabled,
				scene_lights,
				gpu_lighting_constants,
				position_blend);
		FrameVertexBuffer lighting_buffer;
		bool have_lighting_buffer = false;
		if (!gpu_lighting
			&& !lod->source_vertices.empty()
			&& get_available_frame_vertices(frontend.frame_geometry,
				static_cast<std::uint32_t>(
					lod->source_vertices.size()),
				frontend.model_layout)
				>= lod->source_vertices.size())
		{
			alloc_frame_vertex_buffer(frontend.frame_geometry,
				&lighting_buffer,
				static_cast<std::uint32_t>(
					lod->source_vertices.size()),
				frontend.model_layout);
			apply_mesh_lighting(
				lod->source_vertices,
				lod->normals,
				lod->secondary_normals,
				lod->static_lighting_rgb,
				lod->radius,
				transform,
				frame.camera_orientation,
				instance.scale,
				normal_blend,
				exclusion_mask,
				static_lighting_enabled,
				scene_lights,
				reinterpret_cast<ModelRenderVertex*>(
					lighting_buffer.data),
				planet,
				lod->secondary_positions,
				position_blend);
			if (highlighted)
			{
				auto* vertices =
					reinterpret_cast<ModelRenderVertex*>(
						lighting_buffer.data);
				for (std::size_t vertex = 0;
					vertex < lod->source_vertices.size();
					++vertex)
				{
					vertices[vertex].color = 0xff0000ffu;
				}
			}
			have_lighting_buffer = true;
		}
		// Retail builds this transformed diffuse/environment stream in its
		// bounded mesh pool for every submitted node. Pool exhaustion rejects
		// the node; it never falls back to stale lighting or placeholder UVs.
		if (!lod->source_vertices.empty()
			&& !gpu_lighting
			&& !have_lighting_buffer)
		{
			continue;
		}
		FrameVertexBuffer cloak_normal_buffer;
		bool have_cloak_buffers = false;
		if (cloak_node
			&& have_lighting_buffer
			&& !lod->source_vertices.empty())
		{
			const std::uint32_t vertex_count =
				static_cast<std::uint32_t>(
					lod->source_vertices.size());
			// Cloak's primary pass only changes the alpha of the normal
			// diffuse stream (LANCER.EXE 0x004632f0). The separately built
			// cloak mesh and its runtime clone are submitted below.
			cloak_normal_buffer = lighting_buffer;
			auto* normal_vertices =
				reinterpret_cast<ModelRenderVertex*>(
					cloak_normal_buffer.data);
			for (std::uint32_t vertex = 0;
				vertex < vertex_count;
				++vertex)
			{
				const ModelRenderVertex source =
					have_lighting_buffer
						? reinterpret_cast<
							const ModelRenderVertex*>(
								lighting_buffer.data)[vertex]
						: model_render_vertex(
							lod->source_vertices[vertex]);
				normal_vertices[vertex] = source;
				const float hit = cloak_runtime != nullptr
					&& vertex < cloak_runtime->hit_alpha.size()
					? cloak_runtime->hit_alpha[vertex]
					: 0.0f;
				// SR_mesh_render adds mesh +0xc0 to vertex float4 W and
				// clamps the sum. Hit glow is not an alpha replacement.
				const float normal_alpha = std::min(
					1.0f,
					source_object->cloak_normal_alpha + hit);
				normal_vertices[vertex].color =
					(source.color & 0x00ffffffu)
					| static_cast<std::uint32_t>(
						std::clamp(
							std::lrint(normal_alpha * 255.0f),
							0l, 255l))
						<< 24;
			}
			have_cloak_buffers = true;
		}
		if (cloak_node && !have_cloak_buffers)
		{
			continue;
		}
		for (const MissionGpuSection& section : lod->sections)
		{
			if (section.index_count == 0 || section.suppressed)
			{
				continue;
			}
			FrameVertexBuffer clipped_primary_buffer;
			FrameIndexBuffer clipped_index_buffer;
			bool have_clipped_buffers = false;
			if (clip_polygons)
			{
				const auto* primary_vertices =
					reinterpret_cast<const ModelRenderVertex*>(
						have_cloak_buffers
							? cloak_normal_buffer.data
							: lighting_buffer.data);
				const ClippedSectionCounts counts =
					clipped_section_counts(
						lod->source_indices,
						lod->source_vertices.size(),
						section,
						primary_vertices,
						object_to_camera,
						frustum,
						local_clip_plane);
				if (counts.vertices == 0
					|| counts.indices == 0
					|| get_available_frame_vertices(frontend.frame_geometry,
						counts.vertices,
						frontend.model_layout)
						< counts.vertices
					|| get_available_frame_indices(frontend.frame_geometry,
						counts.indices,
						true) < counts.indices)
				{
					continue;
				}
				alloc_frame_vertex_buffer(frontend.frame_geometry,
					&clipped_primary_buffer,
					counts.vertices,
					frontend.model_layout);
				alloc_frame_index_buffer(frontend.frame_geometry,
					&clipped_index_buffer,
					counts.indices,
					true);
				have_clipped_buffers = write_clipped_section(
					lod->source_indices,
					lod->source_vertices.size(),
					section,
						primary_vertices,
						object_to_camera,
						frustum,
						local_clip_plane,
						reinterpret_cast<ModelRenderVertex*>(
						clipped_primary_buffer.data),
					reinterpret_cast<std::uint32_t*>(
						clipped_index_buffer.data),
					counts);
				if (!have_clipped_buffers)
				{
					continue;
				}
			}
			set_camera_relative_transform(
				transform, frame.camera_position);
			if (have_clipped_buffers)
			{
				set_frame_vertex_buffer(
					0, &clipped_primary_buffer);
			}
			else if (have_cloak_buffers)
			{
				set_frame_vertex_buffer(
					0, &cloak_normal_buffer);
			}
			else if (have_lighting_buffer)
			{
				set_frame_vertex_buffer(
					0, &lighting_buffer);
			}
			else if (gpu_lighting)
			{
				bgfx::setVertexBuffer(0, lod->lit_vertices);
			}
			else
			{
				continue;
			}
			if (have_clipped_buffers)
			{
				set_frame_index_buffer(&clipped_index_buffer);
			}
			else
			{
				bgfx::setIndexBuffer(
					lod->indices,
					section.first_index,
					section.index_count);
			}
			const bgfx::TextureHandle texture =
				section.texture == UINT32_MAX
				? frontend.white.handle
				: model.textures[section.texture].handle;
			bgfx::setTexture(
				0, frontend.texture_sampler, texture);
			bool retained_channel_enabled =
				runtime_model == nullptr;
			if (runtime_model != nullptr
				&& section.light_channel
					< runtime_model->light_channels.size())
			{
				retained_channel_enabled =
					runtime_model->light_channels[
						section.light_channel] != 0;
			}
			// Cloak_create_primary_node (LANCER.EXE 0x00462b80) installs
			// callback 0x00463b90 on the existing mesh. The callback changes
			// its vertex diffuse alpha through 0x004632f0; it does not replace
			// the authored material or either texture stage. This matters for
			// the Basilisk, whose complete model uses mode seven.
			const bool mode_six_channel =
				section.mode == 6
				&& (node.flags & 0x0080u) != 0
				&& retained_channel_enabled
				&& section.texture != UINT32_MAX
				&& section.texture
					< model.alternate_texture_loaded.size()
				&& model.alternate_texture_loaded[section.texture];
			const bool mode_seven_channel =
				section.mode == 7
				&& section.modifier
					< std::size(frontend.model_modifier_textures)
				&& ((node.flags & 0x0080u) == 0
					|| retained_channel_enabled);
			bgfx::ProgramHandle material_program =
				gpu_lighting
					? planet ? frontend.planet_rgba_program
						: frontend.lit_model_rgba_program
					: frontend.model_rgba_program;
			if (mode_six_channel)
			{
				bgfx::setTexture(
					1,
					frontend.material_texture_sampler,
					model.alternate_textures[
						section.texture].handle);
				material_program = gpu_lighting
					? frontend.lit_model_mode_six_program
					: frontend.model_mode_six_program;
			}
			else if (mode_seven_channel)
			{
				bgfx::setTexture(
					1,
					frontend.material_texture_sampler,
					frontend.model_modifier_textures[
						section.modifier].handle);
				material_program = gpu_lighting
					? planet ? frontend.planet_mode_seven_program
						: frontend.lit_model_mode_seven_program
					: frontend.model_mode_seven_program;
			}
			else if (section.mode == 8 || section.mode == 10)
			{
				// SRO_build_lod_mesh gives modes eight and ten the same
				// textured, vertex-diffuse, selector-one material state.
				// Both consume the generated environment UV channel.
				material_program = gpu_lighting
					? frontend.lit_model_mode_eight_program
					: frontend.model_mode_eight_program;
			}
			const float normal_tint[] = {
				1.0f, 1.0f, 1.0f, 1.0f};
			const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
			bgfx::setUniform(frontend.uv_rect_uniform, uv);
			bgfx::setUniform(
				frontend.tint_uniform, normal_tint);
			const float diffuse_enabled[] = {
				highlighted || cloak_node
						|| material_uses_vertex_diffuse(section.mode)
					? 1.0f
					: 0.0f,
				0.0f,
				0.0f,
				0.0f,
			};
			bgfx::setUniform(
				frontend.material_diffuse_uniform,
				diffuse_enabled);
			if (gpu_lighting)
			{
				bind_gpu_mesh_lighting(
					frontend, gpu_lighting_constants);
				if (planet)
				{
					bgfx::setTexture(2, frontend.lighting_response_sampler,
						frontend.lighting_response_texture);
				}
			}
			std::uint64_t state =
				BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_DEPTH_TEST_GREATER
					| BGFX_STATE_MSAA;
			// Approved implementation divergence: retail performs its
			// one-sided face rejection in
			// SR_mesh_select_elements_and_mark_vertices
			// (LANCER.EXE 0x004c6280).
			// Preserve the authored per-face double-sided policy, but express
			// the facing test through GPU render state instead of manually
			// rejecting polygons. The left-handed model/view basis followed
			// by framebuffer-Y projection makes authored front faces
			// counter-clockwise at bgfx's state boundary, so cull CW.
			//
			// Do not confuse the 0x800 vertex-format channel which
			// SRO_build_lod_mesh derives from node flag 0x40 with mesh
			// instance flag 0x800 at +0x14. Only the latter bypasses
			// SR_mesh_select_elements_and_mark_vertices' facing test, and
			// ordinary SRO instances do not inherit it from node flag 0x40.
			if (!section.lines && !section.double_sided)
			{
				state |= BGFX_STATE_CULL_CW;
			}
			if (cloak_node)
			{
				state |= retail_blend_state(
					RetailBlendSelector::source_alpha);
			}
			else if (blended_mode(section.mode))
			{
				state |= material_blend_state(section.mode);
			}
			if (section.lines)
			{
				state |= BGFX_STATE_PT_LINES;
			}
			// srd3d bucket one enables Z writes for selector zero and
			// disables them for every blended selector. Primitive topology
			// does not alter that state, so opaque line materials write Z.
			if (!cloak_node && !blended_mode(section.mode))
			{
				state |= BGFX_STATE_WRITE_Z;
			}
			bgfx::setState(state);
			bgfx::setUniform(
				frontend.model_clip_plane_uniform,
				glm::value_ptr(model_clip_plane));
			if (cloak_node || blended_mode(section.mode))
			{
				submit_retail_transparent(
					renderer, material_program, transparent_view);
			}
			else
			{
				bgfx::submit(opaque_view, material_program);
			}
		}

		// SRO_build_cloak_lod_mesh (LANCER.EXE 0x004a3cb0) creates one
		// consolidated cloak64 mesh from the authored vertices and faces.
		// Cloak_create_node (0x00462eb0) clones that mesh with flags
		// 0x280000; it never reuses the ordinary material sections above.
		if (cloak_node
			&& source_object->type != 0x95u
			// cloak_secondary_render_callback (LANCER.EXE
			// 0x00463b41..0x00463b5c) suppresses this mesh only when
			// cloak phase is exactly zero. Phase one is the retained,
			// fully cloaked presentation used before cinematic decloaks.
			&& source_object->cloak_phase_value != 0.0f
			&& !lod->cloak_source_vertices.empty()
			&& bgfx::isValid(lod->cloak_indices)
			&& get_available_frame_vertices(frontend.frame_geometry,
				static_cast<std::uint32_t>(
					lod->cloak_source_vertices.size()),
				frontend.model_layout)
				>= lod->cloak_source_vertices.size())
		{
			const std::uint32_t vertex_count =
				static_cast<std::uint32_t>(
					lod->cloak_source_vertices.size());
			FrameVertexBuffer cloak_secondary_buffer;
			alloc_frame_vertex_buffer(frontend.frame_geometry,
				&cloak_secondary_buffer,
				vertex_count,
				frontend.model_layout);
			auto* secondary_vertices =
				reinterpret_cast<ModelRenderVertex*>(
					cloak_secondary_buffer.data);
			const glm::vec3 palette =
				cloak_palette(source_object->cloak_phase_value);
			const std::uint8_t palette_red =
				static_cast<std::uint8_t>(
					std::clamp(
						std::lrint(palette.r * 255.0f),
						0l, 255l));
			const std::uint8_t palette_green =
				static_cast<std::uint8_t>(
					std::clamp(
						std::lrint(palette.g * 255.0f),
						0l, 255l));
			const std::uint8_t palette_blue =
				static_cast<std::uint8_t>(
					std::clamp(
						std::lrint(palette.b * 255.0f),
						0l, 255l));
			for (std::uint32_t vertex = 0;
				vertex < vertex_count;
				++vertex)
			{
				secondary_vertices[vertex] = model_render_vertex(
					lod->cloak_source_vertices[vertex]);
				const glm::vec2 uv = cloak_runtime != nullptr
					&& vertex < cloak_runtime->secondary_uv.size()
					? cloak_runtime->secondary_uv[vertex]
					: glm::vec2{0.0f};
				secondary_vertices[vertex].u = uv.x;
				secondary_vertices[vertex].v = uv.y;
				// The dedicated mesh has base alpha zero, and palette update
				// writes only dynamic RGB. Premultiplied-alpha selector two
				// therefore retains the executable's additive destination term.
				secondary_vertices[vertex].color =
					static_cast<std::uint32_t>(
							palette_blue) << 16
						| static_cast<std::uint32_t>(
							palette_green) << 8
						| palette_red;
			}

			for (const MissionGpuSection& section
				: lod->cloak_sections)
			{
				if (section.index_count == 0 || section.suppressed)
				{
					continue;
				}
				FrameVertexBuffer clipped_vertex_buffer;
				FrameIndexBuffer clipped_index_buffer;
				bool have_clipped_buffers = false;
				if (clip_polygons)
				{
					const ClippedSectionCounts counts =
						clipped_section_counts(
							lod->cloak_source_indices,
							lod->cloak_source_vertices.size(),
							section,
							secondary_vertices,
							object_to_camera,
							frustum,
							local_clip_plane);
					if (counts.vertices == 0
						|| counts.indices == 0
						|| get_available_frame_vertices(frontend.frame_geometry,
							counts.vertices,
							frontend.model_layout)
							< counts.vertices
						|| get_available_frame_indices(frontend.frame_geometry,
							counts.indices,
							true) < counts.indices)
					{
						continue;
					}
					alloc_frame_vertex_buffer(frontend.frame_geometry,
						&clipped_vertex_buffer,
						counts.vertices,
						frontend.model_layout);
					alloc_frame_index_buffer(frontend.frame_geometry,
						&clipped_index_buffer,
						counts.indices,
						true);
					have_clipped_buffers = write_clipped_section(
						lod->cloak_source_indices,
						lod->cloak_source_vertices.size(),
						section,
						secondary_vertices,
						object_to_camera,
						frustum,
						local_clip_plane,
						reinterpret_cast<ModelRenderVertex*>(
							clipped_vertex_buffer.data),
						reinterpret_cast<std::uint32_t*>(
							clipped_index_buffer.data),
						counts);
					if (!have_clipped_buffers)
					{
						continue;
					}
				}
				set_camera_relative_transform(
					transform, frame.camera_position);
				if (have_clipped_buffers)
				{
					set_frame_vertex_buffer(
						0, &clipped_vertex_buffer);
					set_frame_index_buffer(&clipped_index_buffer);
				}
				else
				{
					set_frame_vertex_buffer(
						0, &cloak_secondary_buffer);
					bgfx::setIndexBuffer(
						lod->cloak_indices,
						section.first_index,
						section.index_count);
				}
				bgfx::setTexture(
					0,
					frontend.texture_sampler,
					cloak_texture.handle);
				const float cloak_tint[] =
					{1.0f, 1.0f, 1.0f, 1.0f};
				const float uv[] =
					{0.0f, 0.0f, 1.0f, 1.0f};
				bgfx::setUniform(
					frontend.uv_rect_uniform, uv);
				bgfx::setUniform(
					frontend.tint_uniform, cloak_tint);
				const float cloak_diffuse_enabled[] =
					{1.0f, 0.0f, 0.0f, 0.0f};
				bgfx::setUniform(
					frontend.material_diffuse_uniform,
					cloak_diffuse_enabled);
				std::uint64_t cloak_state =
					BGFX_STATE_WRITE_RGB
						| BGFX_STATE_WRITE_A
						| BGFX_STATE_DEPTH_TEST_GREATER
						// The dedicated cloak material sets selector two
						// at LANCER.EXE 0x004a3e03.
						| retail_blend_state(
							RetailBlendSelector::
								premultiplied_alpha)
						| BGFX_STATE_MSAA;
				if (!section.double_sided)
				{
					cloak_state |= BGFX_STATE_CULL_CW;
				}
				bgfx::setState(cloak_state);
				bgfx::setUniform(
					frontend.model_clip_plane_uniform,
					glm::value_ptr(model_clip_plane));
				submit_retail_transparent(
					renderer,
					frontend.model_rgba_program,
					transparent_view);
			}
		}
	}
}

void submit_exploding_meshes(
	MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionSceneLights& scene_lights,
	const MissionRenderFrame& frame)
{
	if (frame.world == nullptr)
	{
		return;
	}
	for (std::size_t effect_index = 0;
		effect_index < game::kMaxExplodingMeshes; ++effect_index)
	{
		const game::ExplodingMeshEffect& effect =
			frame.world->death_effects.exploding_meshes[effect_index];
		ExplodingMeshGpuIndices& gpu =
			renderer.exploding_mesh_indices[effect_index];
		if (!effect.active && bgfx::isValid(gpu.handle))
		{
			bgfx::destroy(gpu.handle);
			gpu = {};
		}
		if (!effect.active || !effect.render_active)
		{
			continue;
		}
		const game::ExplodingMeshGeometry& geometry = effect.geometry;
		if (geometry.vertices.empty()
			|| geometry.indices.empty()
			|| geometry.normals.size() != geometry.vertices.size()
			|| geometry.secondary_normals.size() != geometry.vertices.size()
			|| geometry.static_lighting_rgb.size()
				!= geometry.vertices.size())
		{
			continue;
		}
		const MissionGpuModel* model = nullptr;
		if (geometry.source_attachment)
		{
			if (geometry.source_model >= game::kAttachmentDefinitionCount
				|| !renderer.attachment_model_loaded[
					geometry.source_model][0])
			{
				continue;
			}
			model = &renderer.attachment_models[
				geometry.source_model][0];
		}
		else
		{
			if (geometry.source_model
					>= static_cast<std::size_t>(MissionModel::count)
				|| !renderer.model_loaded[geometry.source_model])
			{
				continue;
			}
			model = &renderer.models[geometry.source_model];
		}
		const glm::mat4 transform = math::model_transform(
			effect.orientation, 1.0f, effect.position);
		const glm::mat4 object_to_camera =
			math::camera_rotation_view(frame.camera_orientation)
				* math::camera_relative_transform(
					transform, frame.camera_position);
		const retail_clip::Frustum frustum{
			frame.horizontal_tangent,
			frame.vertical_tangent,
			kMissionNearPlane,
		};
		const RetailFrustumClassification bounds_classification =
			classify_model_bounds(
				geometry.bounds_min,
				geometry.bounds_max,
				object_to_camera,
				frustum);
		if (bounds_classification.all_outside != 0)
		{
			continue;
		}
		const std::uint32_t vertex_count = static_cast<std::uint32_t>(
			geometry.vertices.size());
		if (get_available_frame_vertices(frontend.frame_geometry,
				vertex_count, frontend.model_layout) < vertex_count)
		{
			continue;
		}
		if (gpu.geometry_serial != effect.geometry_serial
			|| !bgfx::isValid(gpu.handle))
		{
			if (bgfx::isValid(gpu.handle))
			{
				bgfx::destroy(gpu.handle);
			}
			gpu.handle = bgfx::createIndexBuffer(
				bgfx::copy(
					geometry.indices.data(),
					static_cast<std::uint32_t>(
						geometry.indices.size() * sizeof(std::uint16_t))));
			gpu.geometry_serial = effect.geometry_serial;
			if (!bgfx::isValid(gpu.handle))
			{
				continue;
			}
		}
		FrameVertexBuffer vertex_buffer;
		alloc_frame_vertex_buffer(frontend.frame_geometry,
			&vertex_buffer, vertex_count, frontend.model_layout);
		apply_mesh_lighting(
			geometry.vertices,
			geometry.normals,
			geometry.secondary_normals,
			geometry.static_lighting_rgb,
			geometry.radius,
			transform,
			frame.camera_orientation,
			1.0f,
			0.0f,
			geometry.light_exclusion_mask,
			geometry.static_lighting_enabled,
			scene_lights,
			reinterpret_cast<ModelRenderVertex*>(vertex_buffer.data));
		for (const assets::GameplaySection& source_section
			: geometry.sections)
		{
			if (source_section.index_count == 0
				|| source_section.suppressed
				|| static_cast<std::uint64_t>(source_section.first_index)
					+ source_section.index_count > geometry.indices.size())
			{
				continue;
			}
			const MissionGpuSection section{
				source_section.first_index,
				source_section.index_count,
				source_section.texture,
				source_section.mode,
				source_section.modifier,
				source_section.light_channel,
				source_section.lines,
				source_section.suppressed,
				source_section.double_sided,
			};
			FrameVertexBuffer clipped_vertex_buffer;
			FrameIndexBuffer clipped_index_buffer;
			bool clipped = false;
			if (bounds_classification.any_outside != 0)
			{
				const ClippedSectionCounts counts = clipped_section_counts(
					geometry.indices,
					geometry.vertices.size(),
					section,
					reinterpret_cast<const ModelRenderVertex*>(
						vertex_buffer.data),
					object_to_camera,
					frustum,
					nullptr);
				if (counts.vertices == 0 || counts.indices == 0
					|| get_available_frame_vertices(frontend.frame_geometry,
						counts.vertices, frontend.model_layout)
						< counts.vertices
					|| get_available_frame_indices(frontend.frame_geometry,
						counts.indices, true) < counts.indices)
				{
					continue;
				}
				alloc_frame_vertex_buffer(frontend.frame_geometry,
					&clipped_vertex_buffer,
					counts.vertices,
					frontend.model_layout);
				alloc_frame_index_buffer(frontend.frame_geometry,
					&clipped_index_buffer, counts.indices, true);
				clipped = write_clipped_section(
					geometry.indices,
					geometry.vertices.size(),
					section,
					reinterpret_cast<const ModelRenderVertex*>(
						vertex_buffer.data),
					object_to_camera,
					frustum,
					nullptr,
					reinterpret_cast<ModelRenderVertex*>(
						clipped_vertex_buffer.data),
					reinterpret_cast<std::uint32_t*>(
						clipped_index_buffer.data),
					counts);
				if (!clipped)
				{
					continue;
				}
			}
			set_camera_relative_transform(transform, frame.camera_position);
			if (clipped)
			{
				set_frame_vertex_buffer(0, &clipped_vertex_buffer);
				set_frame_index_buffer(&clipped_index_buffer);
			}
			else
			{
				set_frame_vertex_buffer(0, &vertex_buffer);
				bgfx::setIndexBuffer(
					gpu.handle,
					section.first_index,
					section.index_count);
			}
			const bgfx::TextureHandle texture =
				section.texture == UINT32_MAX
					|| section.texture >= model->textures.size()
				? frontend.white.handle
				: model->textures[section.texture].handle;
			bgfx::setTexture(0, frontend.texture_sampler, texture);
			const bool retained_channel_enabled =
				section.light_channel < geometry.light_channels.size()
				&& geometry.light_channels[section.light_channel] != 0;
			const bool mode_six_channel =
				section.mode == 6
				&& (geometry.node_flags & 0x0080u) != 0
				&& retained_channel_enabled
				&& section.texture != UINT32_MAX
				&& section.texture < model->alternate_texture_loaded.size()
				&& model->alternate_texture_loaded[section.texture];
			const bool mode_seven_channel =
				section.mode == 7
				&& section.modifier
					< std::size(frontend.model_modifier_textures)
				&& ((geometry.node_flags & 0x0080u) == 0
					|| retained_channel_enabled);
			bgfx::ProgramHandle material_program =
				frontend.model_rgba_program;
			if (mode_six_channel)
			{
				bgfx::setTexture(
					1,
					frontend.material_texture_sampler,
					model->alternate_textures[section.texture].handle);
				material_program = frontend.model_mode_six_program;
			}
			else if (mode_seven_channel)
			{
				bgfx::setTexture(
					1,
					frontend.material_texture_sampler,
					frontend.model_modifier_textures[
						section.modifier].handle);
				material_program = frontend.model_mode_seven_program;
			}
			else if (section.mode == 8 || section.mode == 10)
			{
				material_program = frontend.model_mode_eight_program;
			}
			const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
			const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
			const float diffuse_enabled[] = {
				material_uses_vertex_diffuse(section.mode) ? 1.0f : 0.0f,
				0.0f,
				0.0f,
				0.0f,
			};
			bgfx::setUniform(frontend.uv_rect_uniform, uv);
			bgfx::setUniform(frontend.tint_uniform, tint);
			bgfx::setUniform(
				frontend.material_diffuse_uniform, diffuse_enabled);
			std::uint64_t state =
				BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_DEPTH_TEST_GREATER
					| BGFX_STATE_MSAA;
			if (!section.lines && !section.double_sided)
			{
				state |= BGFX_STATE_CULL_CW;
			}
			if (blended_mode(section.mode))
			{
				state |= material_blend_state(section.mode);
			}
			else
			{
				state |= BGFX_STATE_WRITE_Z;
			}
			if (section.lines)
			{
				state |= BGFX_STATE_PT_LINES;
			}
			bgfx::setState(state);
			const float disabled_model_clip[] =
				{0.0f, 0.0f, 0.0f, 1.0f};
			bgfx::setUniform(
				frontend.model_clip_plane_uniform,
				disabled_model_clip);
			if (blended_mode(section.mode))
			{
				submit_retail_transparent(renderer, material_program);
			}
			else
			{
				bgfx::submit(kMissionView, material_program);
			}
		}
	}
}

void submit_model_with_locator_children(
	const MissionRenderer& renderer,
	const MissionGpuModel& model,
	const MissionRenderInstance& instance,
	const FrontendRenderer& frontend,
	const MissionSceneLights& scene_lights,
	const MissionRenderFrame& frame,
	bool submit_locator_light_flares,
	bool animate_runtime,
	std::uint32_t depth)
{
	if (depth >= 32)
	{
		return;
	}

	const game::WorldObject* object =
		animate_runtime ? instance.source_object : nullptr;
	const game::WorldObject* effect_owner =
		instance.effect_owner != nullptr
			? instance.effect_owner
			: instance.source_object;
	const glm::mat3 wobble =
		effect_owner != nullptr
			&& !instance.effect_root_wobble_applied
			&& (effect_owner->runtime_flags & 0x00000100u) != 0
			? cloak_wobble(*effect_owner, frame.simulation_tick)
			: glm::mat3{1.0f};
	const glm::mat4 model_root = sl_open::math::model_transform(
		instance.orientation * wobble,
		instance.scale,
		instance.position);
	const auto submit_children = [&](std::uint32_t node_index)
	{
		const MissionGpuNode& source_node = model.nodes[node_index];
		const std::uint16_t source_runtime_reference =
			static_cast<std::uint16_t>(
				instance.runtime_reference_base
					+ source_node.runtime_model_index);
		if (animate_runtime && object != nullptr)
		{
			const glm::mat4 source_model_root = explosion_portal_root(
				frame,
				*object,
				source_runtime_reference,
				model_root,
				instance.scale);
			submit_mounted_ordnance_children(
				renderer,
				frontend,
				instance,
				source_model_root,
				scene_lights,
				frame,
				source_runtime_reference);
		}
		// object_instantiate_model_locator (0x00499a10) allocates child
		// slots in authored locator order. Reverse scene traversal therefore
		// visits the last locator child first.
		for (auto locator_it = source_node.locator_indices.rbegin();
			locator_it != source_node.locator_indices.rend();
			++locator_it)
		{
			const std::size_t locator_index = *locator_it;
			const MissionGpuLocator& locator =
				model.locators[locator_index];
			const game::ObjectModelReference* runtime_model = nullptr;
			if (object != nullptr
				&& source_runtime_reference
					< object->model_references.size())
			{
				runtime_model =
					&object->model_references[
						source_runtime_reference];
			}
			glm::mat4 locator_transform = locator.object_transform;
			if (runtime_model != nullptr
				&& locator_index < model.gameplay_locators.size())
			{
				const assets::GameplayLocator& gameplay_locator =
					model.gameplay_locators[locator_index];
				locator_transform = runtime_model->scene_transform
					* glm::translate(
						glm::mat4{1.0f}, gameplay_locator.position)
					* glm::mat4(gameplay_locator.basis);
			}
			const glm::mat4 source_model_root =
				object != nullptr
					? explosion_portal_root(
						frame,
						*object,
						source_runtime_reference,
						model_root,
						instance.scale)
					: model_root;
			const glm::mat4 locator_world =
				source_model_root * locator_transform;
			if (submit_locator_light_flares
				&& locator.type == 4
				&& locator.dimensions.x > 0.0f)
			{
				const float cycle_edge = dynamic_locator_light_edge(
					locator,
					frame.simulation_tick,
					effect_owner);
				if (cycle_edge > 0.0f)
				{
					const glm::vec3 position{
						locator_world[3]};
					const float distance = glm::distance(
						position, frame.camera_position);
					float outer_intensity = cycle_edge;
					if (distance > 15000.0f)
					{
						outer_intensity *= 0.1f;
					}
					else if (distance > 1000.0f)
					{
						const float t =
							(distance - 1000.0f) / 14000.0f;
						outer_intensity *=
							1.0f + (0.1f - 1.0f) * t;
					}
					outer_intensity = std::clamp(
						outer_intensity, 0.0f, 1.0f);
					const float distance_size =
						distance < 6000.0f
							? distance / 6000.0f
							: 1.0f;
					const glm::vec3 primary =
						dynamic_locator_light_color(locator.subtype)
						* (outer_intensity * 0.5f);
					submit_billboard(
						renderer,
						frontend,
						renderer.model_light_flare_texture,
						frame.camera_position,
						position,
						locator.dimensions.y * 14.0f
							* distance_size * instance.scale,
						pack_lighting_rgb(primary),
						0.0f,
						0.0f,
						1.0f,
						1.0f,
						frame.camera_orientation,
						1.0f,
						RetailBlendSelector::additive,
						locator.dimensions.x * -2.25f);
					// The second 0x64-byte Light BMO record switches to
					// `newlight`, retains the authored secondary color,
					// and is zeroed at the ends of the cycle.
					if (cycle_edge >= 0.9f)
					{
						submit_billboard(
							renderer,
							frontend,
							renderer.model_light_core_texture,
							frame.camera_position,
							position,
							locator.dimensions.y * 0.6f
								* instance.scale,
							pack_lighting_rgb(
								dynamic_locator_flare_color(
									locator.subtype)),
							0.0f,
							0.0f,
							1.0f,
							1.0f,
							frame.camera_orientation,
							1.0f,
							RetailBlendSelector::additive,
							locator.dimensions.x * -2.25f);
					}
				}
			}
			if ((locator.type != 1 && locator.type != 5)
				|| locator.subtype < 0 || locator.subtype >= 20)
			{
				continue;
			}
			const std::uint32_t definition =
				static_cast<std::uint32_t>(locator.type) * 20u
				+ static_cast<std::uint32_t>(locator.subtype);
			if (definition >= game::kAttachmentDefinitionCount
				|| !renderer.attachment_model_loaded[definition][0])
			{
				continue;
			}
			const MissionGpuModel& attached =
				renderer.attachment_models[definition][0];
			const game::EmbeddedModelTree* runtime_tree = nullptr;
			if (animate_runtime && object != nullptr)
			{
				for (const game::EmbeddedModelTree& candidate
					: object->embedded_model_trees)
				{
					if (candidate.parent_scope_base
							== instance.runtime_reference_base
						&& candidate.source_locator == locator_index
						&& candidate.attachment_definition == definition)
					{
						runtime_tree = &candidate;
						break;
					}
				}
			}
			if (runtime_tree != nullptr)
			{
				MissionRenderInstance child = instance;
				child.runtime_reference_base =
					runtime_tree->runtime_reference_base;
				child.effect_root_wobble_applied = true;
				submit_model_with_locator_children(
					renderer,
					attached,
					child,
					frontend,
					scene_lights,
					frame,
					submit_locator_light_flares,
					true,
					depth + 1);
				continue;
			}
			const glm::mat4 child_root =
				locator_world
				* glm::translate(
					glm::mat4{1.0f}, attached.center_of_mass);
			MissionRenderInstance child;
			child.position = glm::vec3(child_root[3]);
			child.orientation =
				glm::mat3(child_root) / instance.scale;
			child.scale = instance.scale;
			child.effect_owner = effect_owner;
			child.effect_root_wobble_applied = true;
			submit_model_with_locator_children(
				renderer,
				attached,
				child,
				frontend,
				scene_lights,
				frame,
				submit_locator_light_flares,
				false,
				depth + 1);
		}
	};
	submit_instance_pass(
		renderer,
		model,
		instance,
		frontend,
		renderer.cloak_texture,
		scene_lights,
		frame,
		frame.simulation_tick,
		submit_children);
}

bool cockpit_visible(
	const MissionRenderer& renderer,
	const MissionRenderFrame& frame)
{
	// The retail scene path at 0x00493348 submits the camera-attached
	// cockpit only for mode-zero cockpit view, in live-node order 1 then 0.
	// In particular, it does not submit Phoenix's third authored "base" node.
	return renderer.cockpit_model_loaded
		&& frame.camera_mode == 0
		&& frame.camera_view_state == 1
		&& renderer.cockpit_model.nodes.size() >= 2;
}

void submit_cockpit(
	const MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionSceneLights& scene_lights,
	const MissionRenderFrame& frame)
{
	if (!cockpit_visible(renderer, frame))
	{
		return;
	}

	const MissionGpuModel& model = renderer.cockpit_model;
	std::array<glm::mat4, 2> node_transforms = {
		model.nodes[0].object_transform,
		model.nodes[1].object_transform,
	};
	const MissionGpuNode& component = model.nodes[1];
	const glm::vec3 component_position =
		component.exported_position - model.center_of_mass
		+ component.rest_translation
		- frame.cockpit_component_orientation
			* component.rest_translation
		+ glm::vec3{0.0f, 0.0f, frame.cockpit_recoil_offset};
	glm::mat4& component_transform = node_transforms[1];
	// Camera_update_frame (0x00461615..0x004616d5) overwrites live node one
	// with the generated Euler matrix, restores its authored mass-centered
	// position, rotates around the serialized pivot, and applies recoil on Z.
	component_transform =
		glm::mat4{frame.cockpit_component_orientation};
	component_transform[3] = glm::vec4{component_position, 1.0f};

	MissionRenderInstance instance;
	// Camera_update_frame writes the root as a child of the camera: its local
	// translation is the speed offset minus the selected cockpit SRO's tag-0
	// camera offset, while its animated orientation does not rotate that
	// translation.
	instance.position =
		frame.camera_position
			+ frame.camera_orientation
				* (frame.cockpit_local_position - model.camera_offset);
	instance.orientation =
		frame.camera_orientation * frame.cockpit_orientation;
	instance.node_transform_override = node_transforms.data();
	instance.submitted_node_count = 2;
	instance.foreground_overlay = true;
	submit_instance_pass(
		renderer,
		model,
		instance,
		frontend,
		renderer.cloak_texture,
		scene_lights,
		frame,
		frame.simulation_tick,
		[](std::uint32_t) {});
}
}

bool model_renderer_init_materials(FrontendRenderer& renderer)
{
	return initialize_model_modifier_textures(renderer);
}

bool model_renderer_upload(
	const assets::GameplayModel& source,
	MissionGpuModel& destination,
	const bgfx::VertexLayout& layout)
{
	return upload_model(source, destination, layout);
}

void model_renderer_shutdown(MissionGpuModel& model)
{
	destroy_model(model);
}

void model_renderer_submit_preview(
	const FrontendRenderer& renderer,
	const MissionGpuModel& model,
	bgfx::ViewId view,
	const glm::mat4& transform,
	bool gun_model,
	const glm::vec3& color,
	const glm::vec4& clip_plane,
	std::uint8_t lod_index)
{
	const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {color.r, color.g, color.b, 1.0f};
	// Loadout_enter registers these lights at 0x00442720. The display
	// masks exclude the purple and directional lights; wire nodes also
	// exclude the white point light. The green point light is never inserted.
	constexpr MissionSceneLight lights[] = {
		{{15.0f, -15.0f, -10.0f}, {}, {1.0f, 1.0f, 1.0f}, 2.0f, 100000.0f, 2u, 0u},
		{{}, {}, {0.0f, 1.0f, 0.0f}, 0.2f, 0.0f, 0u, 2u},
	};
	const glm::mat3 camera_orientation = glm::transpose(glm::mat3(frontend::loadout_view()));
	// Gun SHPs contain authored line faces, including their suppressed
	// edges; the ordinary model importer preserves their topology.
	for (auto node_index = model.node_indices_in_preorder.rbegin();
		node_index != model.node_indices_in_preorder.rend(); ++node_index)
	{
		const MissionGpuNode& node = model.nodes[*node_index];
		if (node.lods.empty())
		{
			continue;
		}
		const MissionGpuLod& lod = node.lods[std::min<std::size_t>(lod_index, node.lods.size() - 1)];
		const glm::mat4 node_transform = transform * node.object_transform;
		const glm::vec4 node_clip =
			glm::transpose(node.object_transform) * clip_plane;
		const bool wire_node = gun_model && !lod.source_faces.empty()
			&& lod.source_faces.front().corner_count == 2;
		GpuMeshLighting lighting;
		prepare_gpu_mesh_lighting(lod.radius, node_transform, camera_orientation,
			glm::length(glm::vec3(node_transform[0])), 0.0f,
			wire_node ? 0xfffbu : 0xfffdu, (node.flags & 0x0040u) != 0, lights, lighting);
		// SROModelTree_set_color_recursive stores alpha first, then RGB:
		// the gun display base is (0, .03, 0), not its red texture color.
		lighting.base[1] += gun_model ? 0.03f : 0.0f;
		for (const MissionGpuSection& section : lod.sections)
		{
			if (section.suppressed || section.index_count == 0)
			{
				continue;
			}
			bgfx::setTransform(glm::value_ptr(node_transform));
			bgfx::setVertexBuffer(0, lod.lit_vertices);
			bgfx::setIndexBuffer(
				lod.indices, section.first_index, section.index_count);
			bgfx::setUniform(renderer.uv_rect_uniform, uv);
			bgfx::setUniform(renderer.tint_uniform, tint);
			const float diffuse[] = {material_uses_vertex_diffuse(section.mode) ? 1.0f : 0.0f,
				0.0f, 0.0f, 0.0f};
			bgfx::setUniform(renderer.material_diffuse_uniform, diffuse);
			bind_gpu_mesh_lighting(renderer, lighting);
			bgfx::setUniform(renderer.model_clip_plane_uniform, glm::value_ptr(node_clip));
			bgfx::setTexture(
				0, renderer.texture_sampler,
				section.texture == UINT32_MAX ? renderer.white.handle : model.textures[section.texture].handle);
			bgfx::ProgramHandle program = renderer.lit_model_rgba_program;
			if (section.mode == 6 && (node.flags & 0x0080u) != 0)
			{
				bgfx::setTexture(1, renderer.material_texture_sampler,
					model.alternate_textures[section.texture].handle);
				program = renderer.lit_model_mode_six_program;
			}
			else if (section.mode == 7)
			{
				bgfx::setTexture(1, renderer.material_texture_sampler,
					renderer.model_modifier_textures[section.modifier].handle);
				program = renderer.lit_model_mode_seven_program;
			}
			else if (section.mode == 8 || section.mode == 10)
			{
				program = renderer.lit_model_mode_eight_program;
			}
			std::uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
				| BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA;
			if (section.lines)
			{
				state |= BGFX_STATE_PT_LINES;
			}
			else if (!section.double_sided)
			{
				state |= BGFX_STATE_CULL_CW;
			}
			if (blended_mode(section.mode))
			{
				state |= material_blend_state(section.mode);
			}
			else
			{
				state |= BGFX_STATE_WRITE_Z;
			}
			bgfx::setState(state);
			bgfx::submit(view, program);
		}
	}
}

bool mission_model_for_type(
	std::uint16_t type,
	MissionModel& model)
{
	const std::uint16_t effective =
		assets::object_type_runtime_alias(type);
	if (!assets::object_type_has_model(effective))
	{
		return false;
	}
	model = static_cast<MissionModel>(effective);
	return true;
}

bool gun_component_model_type(std::uint32_t type)
{
	return type == 3 || type == 9 || type == 10 || type == 18;
}

std::uint16_t gun_frame_shape_for_type(std::uint16_t type)
{
	// Game-session setup, LANCER.EXE 0x0049379e..0x004938bc, selects
	// one authored gun-frame family for each of the twelve flyable ships.
	// Multiplayer aliases 0xf4..0xff use the same table as types 0..11.
	constexpr std::uint16_t shapes[12] = {
		0x116, 0x10e, 0x108, 0x107,
		0x106, 0x10b, 0x11b, 0x10f,
		0x11e, 0x117, 0x11a, 0x112,
	};
	if (type < std::size(shapes))
	{
		return shapes[type];
	}
	if (type >= 0xf4 && type <= 0xff)
	{
		return shapes[type - 0xf4];
	}
	// Mission 25A replaces every class-zero craft with type 0x2d and
	// explicitly selects the type-eleven/Phoenix gun frame.
	return type == 0x2d ? shapes[11] : UINT16_MAX;
}

game::GunMount* append_gun_mount(
	game::WorldObject& object,
	const glm::mat4& transform,
	std::int8_t mount_kind,
	std::uint8_t bullet_type,
	std::uint32_t part_group)
{
	if (object.gun_mount_count >= std::size(object.gun_mounts))
	{
		return nullptr;
	}
	game::GunMount& mount =
		object.gun_mounts[object.gun_mount_count++];
	// Retail allocates the complete 0x60-byte mount array from the
	// zero-filling SR allocator before the second enumeration pass. Reset
	// the semantic record as well, including kind-specific state which the
	// constructors deliberately leave at zero.
	mount = {};
	mount.local_position = glm::vec3(transform[3]);
	mount.local_orientation = glm::mat3(transform);
	mount.part_group_id = part_group;
	mount.mount_kind = mount_kind;
	// object_enumerate_gun_mounts_recursive repairs a zero type-three
	// emitter selector to the built-in one-based projectile type one.
	mount.bullet_type = bullet_type == 0 ? 1 : bullet_type;
	mount.pair_group_id =
		static_cast<std::uint32_t>(mount.bullet_type - 1);
	return &mount;
}

void enumerate_gun_mounts(
	game::WorldObject& object,
	const MissionGpuModel& model,
	const glm::mat4& parent_transform,
	std::uint16_t runtime_reference_base)
{
	std::uint32_t articulated_groups[20]{};
	std::uint8_t articulated_count = 0;
	for (const MissionGpuNode& node : model.nodes)
	{
		if ((node.flags & 0x0004u) != 0
			|| !gun_component_model_type(node.model_type)
			|| node.gun_mount_kind < 1
			|| node.gun_mount_kind > 3)
		{
			continue;
		}
		bool present = false;
		for (std::uint8_t index = 0;
			index < articulated_count;
			++index)
		{
			present =
				present
				|| articulated_groups[index] == node.part_group_id;
		}
		if (present || articulated_count >= std::size(articulated_groups))
		{
			continue;
		}
		articulated_groups[articulated_count++] = node.part_group_id;

		const MissionGpuNode* gun_node = &node;
		for (const MissionGpuNode& candidate : model.nodes)
		{
			if ((candidate.flags & 0x0004u) == 0
				&& candidate.part_group_id == node.part_group_id
				&& candidate.model_type == 3)
			{
				gun_node = &candidate;
			}
		}
		const MissionGpuLocator* emitter = nullptr;
		for (const MissionGpuLocator& locator : model.locators)
		{
			if (locator.type == 3
				&& locator.source_node < model.nodes.size()
				&& (model.nodes[locator.source_node].flags & 0x0004u)
					== 0
				&& model.nodes[locator.source_node].part_group_id
					== node.part_group_id)
			{
				emitter = &locator;
			}
		}
		game::GunMount* mount = append_gun_mount(
			object,
			parent_transform
				* (emitter != nullptr
					? emitter->object_transform
					: gun_node->object_transform),
			static_cast<std::int8_t>(node.gun_mount_kind),
			emitter != nullptr
				? static_cast<std::uint8_t>(emitter->exporter_id)
				: 1,
			node.part_group_id);
		if (mount == nullptr)
		{
			continue;
		}
		for (const MissionGpuNode& candidate : model.nodes)
		{
			// gun_mount_register_model_links_and_clearance_mask and both
			// adjacent articulated constructors skip runtime flag 0x20.
			// Source flag 0x0004 is the destroyed replacement that receives
			// that runtime bit; allowing it here overwrites the live slot.
			if ((candidate.flags & 0x0004u) == 0
				&& candidate.part_group_id == node.part_group_id
				&& candidate.gun_part_slot
					< mount->part_references.size())
			{
				mount->part_references[candidate.gun_part_slot] =
					static_cast<std::int16_t>(
						runtime_reference_base
						+ candidate.runtime_model_index);
			}
		}
		const std::uint16_t emitter_source =
			emitter != nullptr
				? emitter->source_node
				: static_cast<std::uint16_t>(
					gun_node - model.nodes.data());
		if (emitter_source < model.nodes.size())
		{
			const MissionGpuNode& emitter_node =
				model.nodes[emitter_source];
			mount->emitter_reference =
				static_cast<std::int16_t>(
					runtime_reference_base
					+ emitter_node.runtime_model_index);
			const glm::mat4 emitter_transform =
				emitter != nullptr
					? emitter->object_transform
					: gun_node->object_transform;
			mount->emitter_from_part =
				glm::inverse(emitter_node.object_transform)
				* emitter_transform;
		}
	}

	for (const MissionGpuLocator& locator : model.locators)
	{
		if (locator.type != 3
			|| locator.source_node >= model.nodes.size()
			|| (model.nodes[locator.source_node].flags & 0x0004u) != 0)
		{
			continue;
		}
		const std::uint32_t part_group =
			model.nodes[locator.source_node].part_group_id;
		bool articulated = false;
		for (std::uint8_t index = 0;
			index < articulated_count;
			++index)
		{
			articulated =
				articulated
				|| articulated_groups[index] == part_group;
		}
		if (articulated)
		{
			continue;
		}
		game::GunMount* mount = append_gun_mount(
			object,
			parent_transform * locator.object_transform,
			0,
			static_cast<std::uint8_t>(locator.exporter_id),
			part_group);
		if (mount != nullptr)
		{
			const MissionGpuNode& emitter_node =
				model.nodes[locator.source_node];
			mount->emitter_reference =
				static_cast<std::int16_t>(
					runtime_reference_base
					+ emitter_node.runtime_model_index);
			mount->emitter_from_part =
				glm::inverse(emitter_node.object_transform)
				* locator.object_transform;
		}
	}
}

void build_gun_pair_table(game::WorldObject& object)
{
	struct Candidate
	{
		std::uint8_t mount{};
		std::int8_t nearest{-1};
		float nearest_distance{
			std::numeric_limits<float>::max() * 0.5f};
		bool live{true};
	};
	Candidate candidates[20];
	std::uint8_t candidate_count = 0;
	for (std::uint8_t mount = 0;
		mount < object.gun_mount_count;
		++mount)
	{
		if (object.gun_mounts[mount].mount_kind == 0
			|| object.gun_mounts[mount].mount_kind == 2)
		{
			candidates[candidate_count++].mount = mount;
		}
	}
	object.gun_pair_count = 0;
	for (;;)
	{
		// ObjectType_build_gun_pair_table (LANCER.EXE 0x004667f0)
		// recomputes every live candidate's nearest live partner after each
		// emitted pair. Retaining a now-retired nearest neighbor incorrectly
		// turns later three-or-more mount groups into singletons.
		for (std::uint8_t left = 0; left < candidate_count; ++left)
		{
			Candidate& candidate = candidates[left];
			if (!candidate.live)
			{
				continue;
			}
			candidate.nearest = -1;
			candidate.nearest_distance =
				std::numeric_limits<float>::max() * 0.5f;
			const game::GunMount& source =
				object.gun_mounts[candidate.mount];
			const glm::vec3 mirrored{
				-source.local_position.x,
				source.local_position.y,
				source.local_position.z,
			};
			for (std::uint8_t right = 0;
				right < candidate_count;
				++right)
			{
				if (left == right || !candidates[right].live)
				{
					continue;
				}
				const game::GunMount& target =
					object.gun_mounts[candidates[right].mount];
				if (source.pair_group_id != target.pair_group_id)
				{
					continue;
				}
				const float distance =
					glm::distance(mirrored, target.local_position);
				if (distance < candidate.nearest_distance)
				{
					candidate.nearest =
						static_cast<std::int8_t>(right);
					candidate.nearest_distance = distance;
				}
			}
		}
		std::int8_t selected = -1;
		float selected_distance = std::numeric_limits<float>::max();
		for (std::uint8_t index = 0; index < candidate_count; ++index)
		{
			if (candidates[index].live
				&& (selected < 0
					|| candidates[index].nearest_distance
						< selected_distance))
			{
				selected = static_cast<std::int8_t>(index);
				selected_distance = candidates[index].nearest_distance;
			}
		}
		if (selected < 0
			|| object.gun_pair_count >= std::size(object.gun_pairs))
		{
			break;
		}
		Candidate& left = candidates[selected];
		const std::int8_t nearest = left.nearest;
		game::GunPair& pair =
			object.gun_pairs[object.gun_pair_count++];
		if (nearest < 0 || !candidates[nearest].live)
		{
			pair.first = static_cast<std::int8_t>(left.mount);
			left.live = false;
			continue;
		}
		Candidate& right = candidates[nearest];
		const game::GunMount& left_mount = object.gun_mounts[left.mount];
		const game::GunMount& right_mount = object.gun_mounts[right.mount];
		if (right_mount.local_position.x > left_mount.local_position.x)
		{
			pair.first = static_cast<std::int8_t>(right.mount);
			pair.second = static_cast<std::int8_t>(left.mount);
		}
		else
		{
			pair.first = static_cast<std::int8_t>(left.mount);
			pair.second = static_cast<std::int8_t>(right.mount);
		}
		left.live = false;
		right.live = false;
	}
	object.gun_group_count = object.gun_pair_count;
	for (std::uint8_t pair_index = 0;
		pair_index < object.gun_pair_count;
		++pair_index)
	{
		const game::GunPair& pair = object.gun_pairs[pair_index];
		if (pair.first >= 0)
		{
			object.gun_mounts[pair.first].alternating_side = 0;
		}
		if (pair.second >= 0)
		{
			object.gun_mounts[pair.second].alternating_side = 1;
		}
	}
	object.active_weapon_selection_bits =
		object.gun_pair_count == 1 ? 0x0020u : 0x0030u;
	object.selected_gun_group = 0;
	object.alternating_gun_side = 0;
	object.gun_synchronized =
		(object.active_weapon_selection_bits & 0x0010u) != 0;
	object.gun_sync_frame = static_cast<std::uint8_t>(
		(object.active_weapon_selection_bits >> 5) & 1u);
}

void build_exhaust_hazard_volumes(
	const MissionRenderer& renderer,
	const MissionGpuModel& root_model,
	game::WorldObject& object)
{
	object.exhaust_hazard_volumes.clear();
	const auto collect_nested =
		[&](auto&& self,
			const MissionGpuModel& model,
			std::uint16_t driver_model_reference,
			const glm::mat4& model_from_driver) -> void
	{
		for (const MissionGpuLocator& locator : model.locators)
		{
			if (locator.source_node >= model.nodes.size())
			{
				continue;
			}
			const glm::mat4 locator_from_driver =
				model_from_driver * locator.object_transform;
			if (locator.type == 2)
			{
				game::ExhaustHazardVolume volume;
				// SRO_add_type_two_locator (0x00499540) creates a child at
				// the locator pose using the canonical Engine_Mesh helper.
				volume.locator_from_driver = locator_from_driver;
				volume.dimensions = locator.dimensions;
				volume.driver_model_reference =
					driver_model_reference;
				object.exhaust_hazard_volumes.push_back(volume);
				continue;
			}
			const std::int32_t attachment_definition =
				embedded_attachment_definition(locator);
			if (attachment_definition < 0
				|| !renderer.attachment_model_loaded[
					attachment_definition][0])
			{
				continue;
			}
			const MissionGpuModel& attached =
				renderer.attachment_models[
					attachment_definition][0];
			self(
				self,
				attached,
				driver_model_reference,
				locator_from_driver
					* glm::translate(
						glm::mat4{1.0f},
						attached.center_of_mass));
		}
	};

	for (const MissionGpuLocator& locator : root_model.locators)
	{
		if (locator.source_node >= root_model.nodes.size())
		{
			continue;
		}
		const MissionGpuNode& source_node =
			root_model.nodes[locator.source_node];
		const std::uint16_t driver_model_reference =
			source_node.runtime_model_index;
		const glm::mat4 locator_from_driver =
			glm::inverse(source_node.object_transform)
				* locator.object_transform;
		if (locator.type == 2)
		{
			game::ExhaustHazardVolume volume;
			volume.locator_from_driver = locator_from_driver;
			volume.dimensions = locator.dimensions;
			volume.driver_model_reference =
				driver_model_reference;
			object.exhaust_hazard_volumes.push_back(volume);
			continue;
		}
		const std::int32_t attachment_definition =
			embedded_attachment_definition(locator);
		if (attachment_definition < 0
			|| !renderer.attachment_model_loaded[
				attachment_definition][0])
		{
			continue;
		}
		const MissionGpuModel& attached =
			renderer.attachment_models[
				attachment_definition][0];
		collect_nested(
			collect_nested,
			attached,
			driver_model_reference,
			locator_from_driver
				* glm::translate(
					glm::mat4{1.0f},
					attached.center_of_mass));
	}
}

void initialize_cloak_mesh_runtime(
	game::CloakMeshRuntime& runtime,
	const MissionGpuNode& node,
	std::uint16_t object_type)
{
	runtime = {};
	runtime.eligible = cloak_node_eligible(object_type, node.name);
	runtime.local_transform = node.object_transform;
	runtime.normal_lods.reserve(node.lods.size());
	runtime.secondary_lods.reserve(node.lods.size());
	runtime.lod_bounds_min.reserve(node.lods.size());
	runtime.lod_bounds_max.reserve(node.lods.size());
	std::size_t maximum_normal_vertices = 0;
	std::size_t maximum_secondary_vertices = 0;
	for (const MissionGpuLod& lod : node.lods)
	{
		runtime.normal_lods.push_back(&lod.source_vertices);
		runtime.secondary_lods.push_back(&lod.cloak_source_vertices);
		runtime.lod_bounds_min.push_back(lod.bounds_min);
		runtime.lod_bounds_max.push_back(lod.bounds_max);
		maximum_normal_vertices = std::max(
			maximum_normal_vertices, lod.source_vertices.size());
		maximum_secondary_vertices = std::max(
			maximum_secondary_vertices,
			lod.cloak_source_vertices.size());
	}
	runtime.hit_alpha.assign(maximum_normal_vertices, 0.0f);
	runtime.secondary_uv.assign(
		maximum_secondary_vertices, glm::vec2{0.0f});
}

void initialize_attachment_cloak_models(
	game::AttachmentSlot& attachment,
	const MissionGpuModel& model,
	std::uint16_t object_type)
{
	attachment.cloak_models.clear();
	attachment.cloak_model_order.clear();
	attachment.cloak_models.resize(model.nodes.size());
	attachment.cloak_model_order.reserve(model.nodes.size());
	for (const MissionGpuNode& node : model.nodes)
	{
		if (node.runtime_model_index >= attachment.cloak_models.size())
		{
			continue;
		}
		initialize_cloak_mesh_runtime(
			attachment.cloak_models[node.runtime_model_index],
			node,
			object_type);
		attachment.cloak_models[
			node.runtime_model_index].local_transform =
			glm::translate(glm::mat4{1.0f}, model.center_of_mass)
				* node.object_transform;
		attachment.cloak_model_order.push_back(
			node.runtime_model_index);
	}
}

bool attachment_cloak_models_match(
	const game::AttachmentSlot& attachment,
	const MissionGpuModel& model)
{
	if (attachment.cloak_models.size() != model.nodes.size())
	{
		return false;
	}
	for (const MissionGpuNode& node : model.nodes)
	{
		if (node.runtime_model_index >= attachment.cloak_models.size())
		{
			return false;
		}
		const game::CloakMeshRuntime& cloak =
			attachment.cloak_models[node.runtime_model_index];
		if (cloak.normal_lods.size() != node.lods.size())
		{
			return false;
		}
		for (std::size_t lod = 0; lod < node.lods.size(); ++lod)
		{
			if (cloak.normal_lods[lod]
					!= &node.lods[lod].source_vertices
				|| cloak.secondary_lods[lod]
					!= &node.lods[lod].cloak_source_vertices)
			{
				return false;
			}
		}
	}
	return true;
}

void initialize_embedded_model_reference(
	game::ObjectModelReference& reference,
	const MissionGpuNode& node,
	const MissionGpuModel& model,
	std::uint16_t source_model,
	bool source_attachment,
	std::uint16_t source_node,
	std::uint16_t owner_scope,
	std::uint16_t object_type,
	std::int16_t parent_reference,
	const glm::mat4& parent_pose_prefix,
	const glm::mat4& initial_transform)
{
	initialize_cloak_mesh_runtime(reference.cloak, node, object_type);
	reference.collision = &node.collision;
	reference.portals = &node.portals;
	if (!node.lods.empty())
	{
		set_explosion_source_mesh(reference, node.lods[0]);
	}
	reference.explosion_source_model = source_model;
	reference.explosion_source_attachment = source_attachment;
	if (!node.lods.empty() && node.lods[0].source_vertices.size() >= 4)
	{
		for (std::uint8_t vertex = 0; vertex < 4; ++vertex)
		{
			const assets::GameplayVertex& source =
				node.lods[0].source_vertices[vertex];
			reference.mesh_quad_vertices[vertex] = {
				source.x, source.y, source.z};
		}
		reference.mesh_quad_vertex_count = 4;
	}
	reference.local_transform = initial_transform;
	reference.scene_transform = initial_transform;
	reference.base_transform = initial_transform;
	reference.parent_pose_prefix = parent_pose_prefix;
	reference.source_local_basis = node.source_local_basis;
	reference.object_space_joint_basis =
		glm::mat3(initial_transform) * node.source_local_basis;
	reference.exported_position = node.exported_position;
	reference.parent_anchor = model.center_of_mass;
	if (node.parent_reference >= 0)
	{
		reference.parent_anchor =
			model.nodes[static_cast<std::size_t>(
				node.parent_reference)].exported_position;
	}
	reference.rest_translation = node.rest_translation;
	reference.joint_min_degrees = node.joint_min_degrees;
	reference.joint_max_degrees = node.joint_max_degrees;
	reference.sequences = &node.sequences;
	reference.point_groups = &node.point_groups;
	reference.locators = &model.gameplay_locators;
	reference.radius = node.radius;
	reference.bounds_min = node.bounds_min;
	reference.bounds_max = node.bounds_max;
	reference.health = static_cast<float>(node.maximum_hit_points);
	reference.maximum_health =
		static_cast<float>(node.maximum_hit_points);
	reference.source_flags = node.flags;
	reference.model_type = node.model_type;
	reference.part_group_id = node.part_group_id;
	reference.damage_group_selector = node.damage_group_selector;
	reference.runtime_flags =
		(node.flags & 0x0004u) != 0 ? 0x0020u : 0u;
	reference.render_flags =
		(node.flags & 0x0040u) != 0 ? 0x00040000u : 0u;
	reference.light_exclusion_mask =
		(model.root_flags & assets::kGameplayModelRootCompound) == 0
			? 3u
			: 0x18u;
	std::size_t light_channel_count = 0;
	for (const MissionGpuLod& lod : node.lods)
	{
		for (const MissionGpuSection& section : lod.sections)
		{
			light_channel_count = std::max(
				light_channel_count,
				static_cast<std::size_t>(section.light_channel) + 1);
		}
	}
	reference.light_channels.assign(light_channel_count, 0);
	if ((node.flags & 0x0080u) != 0)
	{
		for (const MissionGpuLod& lod : node.lods)
		{
			for (const MissionGpuSection& section : lod.sections)
			{
				if (section.light_channel < reference.light_channels.size()
					&& (section.mode == 6 || section.mode == 7))
				{
					reference.light_channels[section.light_channel] = 1;
				}
			}
		}
	}
	std::copy(
		std::begin(node.suppress_anim_rotation),
		std::end(node.suppress_anim_rotation),
		std::begin(reference.suppress_anim_rotation));
	reference.source_node = source_node;
	reference.owner_scope = owner_scope;
	reference.parent_reference = parent_reference;
	std::memcpy(reference.name, node.name, sizeof(reference.name));
	char lowercase_name[65];
	std::strncpy(lowercase_name, node.name, sizeof(lowercase_name));
	lowercase_name[sizeof(lowercase_name) - 1] = '\0';
	for (char& character : lowercase_name)
	{
		character = static_cast<char>(
			std::tolower(static_cast<unsigned char>(character)));
	}
	reference.forcefield =
		std::strstr(lowercase_name, "forcefield") != nullptr;
}

void append_embedded_model_trees(
	const MissionRenderer& renderer,
	game::WorldObject& object,
	const MissionGpuModel& parent_model,
	std::uint16_t parent_scope_base)
{
	for (std::uint16_t locator_index = 0;
		locator_index < parent_model.locators.size();
		++locator_index)
	{
		const MissionGpuLocator& locator =
			parent_model.locators[locator_index];
		const std::int32_t definition =
			embedded_attachment_definition(locator);
		if (definition < 0
			|| locator.source_node >= parent_model.nodes.size()
			|| !renderer.attachment_model_loaded[definition][0])
		{
			continue;
		}
		const MissionGpuModel& attached =
			renderer.attachment_models[definition][0];
		if (attached.nodes.empty()
			|| object.model_references.size() + attached.nodes.size()
				> static_cast<std::size_t>(INT16_MAX))
		{
			continue;
		}
		const MissionGpuNode& parent_node =
			parent_model.nodes[locator.source_node];
		const std::uint16_t parent_reference =
			static_cast<std::uint16_t>(
				parent_scope_base + parent_node.runtime_model_index);
		const std::uint16_t runtime_base =
			static_cast<std::uint16_t>(
				object.model_references.size());
		game::EmbeddedModelTree tree;
		tree.runtime_reference_base = runtime_base;
		tree.runtime_reference_count =
			static_cast<std::uint16_t>(attached.nodes.size());
		tree.parent_runtime_reference = parent_reference;
		tree.parent_scope_base = parent_scope_base;
		tree.source_locator = locator_index;
		tree.attachment_definition =
			static_cast<std::uint16_t>(definition);
		object.embedded_model_trees.push_back(tree);
		object.model_references.resize(
			object.model_references.size() + attached.nodes.size());

		const glm::mat4 locator_from_parent =
			glm::inverse(parent_node.object_transform)
			* locator.object_transform
			* glm::translate(
				glm::mat4{1.0f}, attached.center_of_mass);
		const glm::mat4 parent_initial =
			object.model_references[parent_reference].local_transform;
		for (std::uint16_t node_index = 0;
			node_index < attached.nodes.size();
			++node_index)
		{
			const MissionGpuNode& node = attached.nodes[node_index];
			const bool root = node.parent_reference < 0;
			const std::int16_t runtime_parent =
				root
					? static_cast<std::int16_t>(parent_reference)
					: static_cast<std::int16_t>(
						runtime_base + node.parent_reference);
			const glm::mat4 initial_transform =
				parent_initial * locator_from_parent
				* node.object_transform;
			initialize_embedded_model_reference(
				object.model_references[
					runtime_base + node.runtime_model_index],
				node,
				attached,
				static_cast<std::uint16_t>(definition),
				true,
				node_index,
				static_cast<std::uint16_t>(
					object.embedded_model_trees.size()),
				object.type,
				runtime_parent,
				root ? locator_from_parent : glm::mat4{1.0f},
				initial_transform);
		}
		enumerate_gun_mounts(
			object,
			attached,
			locator.object_transform
				* glm::translate(
					glm::mat4{1.0f}, attached.center_of_mass),
			runtime_base);
		append_embedded_model_trees(
			renderer, object, attached, runtime_base);
	}
}

const game::EmbeddedModelTree* find_embedded_model_tree(
	const game::WorldObject& object,
	std::uint16_t parent_scope_base,
	std::uint16_t source_locator)
{
	for (const game::EmbeddedModelTree& tree
		: object.embedded_model_trees)
	{
		if (tree.parent_scope_base == parent_scope_base
			&& tree.source_locator == source_locator)
		{
			return &tree;
		}
	}
	return nullptr;
}

void assign_network_model_ids(
	const MissionRenderer& renderer,
	game::WorldObject& object,
	const MissionGpuModel& model,
	std::uint16_t scope_base,
	std::uint32_t* next_id)
{
	// GameObject_instantiate_sro_model (0x004760c0) inserts every tag-1
	// model wrapper directly into the owning non-tag-1 object root, in
	// serialized source order. GameObject_link_model_parents (0x00476130)
	// later reparents only the underlying render objects; it deliberately
	// leaves this wrapper-child array unchanged.
	for (std::uint16_t source_node = 0;
		source_node < model.nodes.size();
		++source_node)
	{
		const MissionGpuNode& node = model.nodes[source_node];
		const std::size_t runtime_reference =
			static_cast<std::size_t>(scope_base)
				+ node.runtime_model_index;
		if (runtime_reference >= object.model_references.size())
		{
			continue;
		}

		// FUN_00466ba0 walks every instantiated SR scene node in recursive
		// preorder, but only tag-1 model wrappers consume and publish an ID
		// at +0xfc.
		object.cloak_model_order.push_back(
			static_cast<std::uint16_t>(runtime_reference));
		if (next_id != nullptr)
		{
			object.model_references[
				runtime_reference].network_model_id = (*next_id)++;
		}

		// SRO_create_model (0x00499430) creates all authored locator
		// children while constructing this tag-1 wrapper. Type-one and
		// type-five locators insert a non-tag-1 child-object root; that
		// root's source-ordered tag-1 wrappers are consequently visited
		// here, before the next primary wrapper at the owning object root.
		for (std::uint16_t locator_index = 0;
			locator_index < model.locators.size();
			++locator_index)
		{
			const MissionGpuLocator& locator =
				model.locators[locator_index];
			if (locator.source_node != source_node)
			{
				continue;
			}
			const game::EmbeddedModelTree* embedded =
				find_embedded_model_tree(
					object, scope_base, locator_index);
			if (embedded == nullptr
				|| embedded->attachment_definition
					>= game::kAttachmentDefinitionCount
				|| !renderer.attachment_model_loaded[
					embedded->attachment_definition][0])
			{
				continue;
			}
			assign_network_model_ids(
				renderer,
				object,
				renderer.attachment_models[
					embedded->attachment_definition][0],
				embedded->runtime_reference_base,
				next_id);
		}
	}
}

void register_model_components(
	const MissionRenderer& renderer,
	game::WorldObject& object,
	const MissionGpuModel& model,
	std::uint16_t scope_base,
	bool register_current_scope)
{
	std::int16_t local_component_index = -1;
	for (std::uint16_t source_node = 0;
		source_node < model.nodes.size();
		++source_node)
	{
		const MissionGpuNode& node = model.nodes[source_node];
		const std::uint16_t runtime_reference =
			static_cast<std::uint16_t>(
				scope_base + node.runtime_model_index);
		if (node.model_type == 5)
		{
			++object.engine_component_count;
		}
		if (node.model_type == 6)
		{
			++object.shield_generator_component_count;
			object.runtime_flags |= 0x00004000u;
		}
		if (register_current_scope && (node.flags & 0x0002u) != 0)
		{
			++local_component_index;
			if (runtime_reference < object.model_references.size()
				&& object.component_count < game::kMaxObjectComponents)
			{
				game::ObjectModelReference& reference =
					object.model_references[runtime_reference];
				const std::uint8_t component_index =
					object.component_count++;
				game::ObjectComponent& component =
					object.components[component_index];
				component.local_position =
					glm::vec3(reference.local_transform[3]);
				component.local_center = glm::vec3(
					reference.local_transform
						* glm::vec4(
							(node.bounds_min + node.bounds_max) * 0.5f,
							1.0f));
				for (std::uint32_t corner = 0; corner < 8; ++corner)
				{
					const glm::vec3 local{
						(corner & 1u) != 0
							? node.bounds_max.x : node.bounds_min.x,
						(corner & 2u) != 0
							? node.bounds_max.y : node.bounds_min.y,
						(corner & 4u) != 0
							? node.bounds_max.z : node.bounds_min.z,
					};
					const glm::vec3 transformed = glm::vec3(
						reference.local_transform
							* glm::vec4(local, 1.0f));
					component.radius = std::max(
						component.radius,
						glm::distance(
							component.local_center, transformed));
				}
				component.health =
					static_cast<float>(node.maximum_hit_points);
				component.maximum_health = component.health;
				component.model_type = node.model_type;
				component.part_group_id = node.part_group_id;
				component.damage_group_selector =
					node.damage_group_selector;
				component.model_node = source_node;
				component.model_reference =
					static_cast<std::int16_t>(runtime_reference);
				const std::uint64_t component_bit =
					std::uint64_t{1} << component_index;
				if ((object.component_protection_override_mask
					& component_bit) != 0)
				{
					component.protection_state =
						object.component_protection_override[
							component_index];
				}
				component.runtime_flags = 0x0100u;
				if ((node.flags & 0x1000u) != 0)
				{
					component.runtime_flags |= 0x2000u;
				}
				if ((node.flags & 0x0004u) != 0)
				{
					component.runtime_flags |= 0x0020u;
				}
				if ((object.component_targetable_override_mask
					& component_bit) != 0)
				{
					if ((object.component_targetable_value_mask
						& component_bit) != 0)
					{
						component.runtime_flags |= 0x2000u;
					}
					else
					{
						component.runtime_flags &= ~0x2000u;
					}
				}
				reference.component_index =
					static_cast<std::int16_t>(component_index);
				reference.runtime_flags |= component.runtime_flags;
			}

			if (local_component_index < 0
				|| static_cast<std::size_t>(local_component_index)
					>= model.gun_clearance_masks.size())
			{
				continue;
			}
			for (std::uint8_t mount_index = 0;
				mount_index < object.gun_mount_count;
				++mount_index)
			{
				game::GunMount& mount =
					object.gun_mounts[mount_index];
				if (mount.mount_kind == 1
					&& mount.part_references[0]
						== static_cast<std::int16_t>(
							runtime_reference))
				{
					mount.clearance_mask =
						model.gun_clearance_masks[
							local_component_index];
					mount.has_clearance_mask = true;
				}
			}
		}

		// GameObject_register_model_components (LANCER.EXE 0x00468760)
		// recursively walks the live scene hierarchy, but creation only calls
		// it for a compound object (object flag 0x0002). Keep visiting embedded
		// trees on ordinary objects for their engine/shield-generator counts,
		// without incorrectly publishing their guns and ordnance as targetable
		// components. For compound objects, the recursive order supplies the
		// exact interleaved numbering consumed by mission component selectors
		// and SetTargetable overrides.
		for (std::uint16_t locator_index = 0;
			locator_index < model.locators.size();
			++locator_index)
		{
			const MissionGpuLocator& locator =
				model.locators[locator_index];
			if (locator.source_node != source_node)
			{
				continue;
			}
			const game::EmbeddedModelTree* embedded =
				find_embedded_model_tree(
					object, scope_base, locator_index);
			if (embedded == nullptr
				|| embedded->attachment_definition
					>= game::kAttachmentDefinitionCount
				|| !renderer.attachment_model_loaded[
					embedded->attachment_definition][0])
			{
				continue;
			}
			register_model_components(
				renderer,
				object,
				renderer.attachment_models[
					embedded->attachment_definition][0],
				embedded->runtime_reference_base,
				register_current_scope);
		}
	}
}

void mission_renderer_initialize_world_components(
	const MissionRenderer& renderer,
	game::World& world,
	game::ChaffRuntime* chaff)
{
	if (!renderer.ready)
	{
		return;
	}
	game::particle_system_configure_fragments(
		world.particles, renderer.graphics_quality);
	for (std::uint16_t type = 0; type < 256; ++type)
	{
		MissionModel model_index;
		if (!mission_model_for_type(type, model_index))
		{
			continue;
		}
		world.model_radius_by_type[type] =
			renderer.models[static_cast<std::size_t>(model_index)].radius;
		world.model_cloak_supported_by_type[type] =
			(renderer.models[static_cast<std::size_t>(model_index)]
				.root_flags & assets::kGameplayModelRootCloak) != 0;
	}
	for (std::size_t definition = 0;
		definition < game::kAttachmentDefinitionCount;
		++definition)
	{
		for (std::size_t variant = 0; variant < 2; ++variant)
		{
			const bool loaded =
				renderer.attachment_model_loaded[definition][variant];
			world.attachment_model_loaded[definition][variant] = loaded;
			world.attachment_model_radius[definition][variant] = loaded
				? renderer.attachment_models[definition][variant].radius
				: 0.0f;
			world.attachment_model_bounds_min[definition][variant] = loaded
				? renderer.attachment_models[definition][variant].bounds_min
				: glm::vec3{0.0f};
			world.attachment_model_bounds_max[definition][variant] = loaded
				? renderer.attachment_models[definition][variant].bounds_max
				: glm::vec3{0.0f};
		}
	}
	const std::uint8_t transition_radial =
		renderer.graphics_quality == 0 ? 9
			: renderer.graphics_quality == 1 ? 12 : 16;
	const std::uint8_t transition_axial =
		renderer.graphics_quality == 0 ? 6
			: renderer.graphics_quality == 1 ? 8 : 12;
	if (!world.transition_effects.jump.initialized
		|| world.transition_effects.wgate.radial_segments
			!= transition_radial
		|| world.transition_effects.wgate.axial_segments
			!= transition_axial)
	{
		game::transition_effects_initialize(
			world.transition_effects, renderer.graphics_quality);
	}
	if (chaff != nullptr && !chaff->model_bounds_ready)
	{
		const MissionGpuModel& decoy = renderer.models[
			static_cast<std::size_t>(MissionModel::decoy)];
		game::chaff_set_model_bounds(
			*chaff, decoy.bounds_min, decoy.bounds_max);
	}
	std::uint32_t initialized_models = 0;
	std::uint32_t initialized_compound_models = 0;
	std::uint32_t initialized_components = 0;
	for (game::WorldObject& object : world.objects)
	{
		if (!object.active || object.components_initialized)
		{
			continue;
		}
		MissionModel model_index;
		if (!mission_model_for_type(object.type, model_index))
		{
			object.components_initialized = true;
			continue;
		}
		const MissionGpuModel& model =
			renderer.models[static_cast<std::size_t>(model_index)];
		// The instantiated root publishes its aggregate transformed bounds
		// and radius at GameObject+0x5a0..+0x5b4/+0x59c. Proximity,
		// targeting, collision, and projectile candidate collection all
		// consume this model-derived spatial extent.
		object.bounds_min = model.bounds_min;
		object.bounds_max = model.bounds_max;
		object.center_of_mass = model.center_of_mass;
		object.camera_offset = model.camera_offset;
		// GameObject_finalize_model_tree publishes the aggregate model-tree
		// mass at GameObject+0x520. Ordinary rigid-sphere collision response
		// consumes this exact value for both inverse mass and impact damage.
		object.physics_mass = model.total_mass;
		const float inertia_determinant =
			glm::determinant(model.inertia_tensor);
		object.inverse_inertia =
			std::isfinite(inertia_determinant)
				&& inertia_determinant != 0.0f
				? glm::inverse(model.inertia_tensor)
				: glm::mat3{0.0f};
		object.radius = model.radius;
		// GameObject_finalize_model_tree (LANCER.EXE 0x004769f0) subtracts
		// the aggregate mass center from the live model tree. Creation then
		// calls GameObject_set_position again at 0x004672e3, republishing the
		// authored mission position over every object/root position cache.
		// The object root therefore remains the mass-centered gameplay point;
		// adding center_of_mass here displaced the complete rendered model and
		// every Director target by a capital-ship-sized vector.
		// GameObject_create_runtime (LANCER.EXE 0x004673dc) maps the authored
		// compound-root flag to the compound and kinematic object flags. The
		// former selects compound model-reference, collision, targeting, and
		// missile-reacquisition paths; the latter leaves motion ownership with
		// the flight/script controller instead of collision impulse response.
		if ((model.root_flags
			& assets::kGameplayModelRootCompound) != 0)
		{
			object.runtime_flags |=
				game::kObjectFlagCompound | game::kObjectFlagKinematic;
		}
		object.cloak_supported =
			(model.root_flags & assets::kGameplayModelRootCloak) != 0;
		object.gun_mount_count = 0;
		object.gun_pair_count = 0;
		object.gun_group_count = 0;
		object.model_references.clear();
		object.embedded_model_trees.clear();
		object.cloak_model_order.clear();
		// GameObject+0x120/+0x128 is the source-ordered live tag-1 model
		// array. The separately allocated GameObject+0x518 traversal cache
		// must not dictate these externally visible model indices.
		object.model_references.resize(model.nodes.size());
		object.primary_model_reference_count =
			static_cast<std::uint16_t>(model.nodes.size());
		for (std::uint32_t node_index = 0;
			node_index < model.nodes.size();
			++node_index)
		{
			const MissionGpuNode& node = model.nodes[node_index];
			// Runtime model indices retain serialized source order; parent
			// links are installed only after all instances exist.
			if (!object.model_references.empty())
			{
				game::ObjectModelReference& reference =
					object.model_references[
						node.runtime_model_index];
				initialize_cloak_mesh_runtime(
					reference.cloak, node, object.type);
				reference.collision = &node.collision;
				reference.portals = &node.portals;
				if (!node.lods.empty())
				{
					set_explosion_source_mesh(reference, node.lods[0]);
				}
				reference.explosion_source_model =
					static_cast<std::uint16_t>(model_index);
				reference.explosion_source_attachment = false;
				reference.mesh_quad_vertex_count = 0;
				if (!node.lods.empty()
					&& node.lods[0].source_vertices.size() >= 4)
				{
					for (std::uint8_t vertex = 0; vertex < 4; ++vertex)
					{
						const assets::GameplayVertex& source =
							node.lods[0].source_vertices[vertex];
						reference.mesh_quad_vertices[vertex] = {
							source.x, source.y, source.z};
					}
					reference.mesh_quad_vertex_count = 4;
				}
				reference.local_transform = node.object_transform;
				reference.scene_transform = node.object_transform;
				reference.base_transform = node.object_transform;
				glm::mat4 parent_object_transform{1.0f};
				if (node.parent_reference >= 0)
				{
					for (const MissionGpuNode& parent : model.nodes)
					{
						if (parent.runtime_model_index
							== node.parent_reference)
						{
							parent_object_transform =
								parent.object_transform;
							break;
						}
					}
				}
				reference.rest_local_transform =
					node.parent_reference >= 0
						? glm::inverse(
							parent_object_transform)
							* node.object_transform
						: node.object_transform;
				reference.source_local_basis =
					node.source_local_basis;
				reference.object_space_joint_basis =
					node.source_local_basis;
				reference.exported_position =
					node.exported_position;
				reference.parent_anchor =
					model.center_of_mass;
				if (node.parent_reference >= 0)
				{
					for (const MissionGpuNode& parent : model.nodes)
					{
						if (parent.runtime_model_index
							== node.parent_reference)
						{
							reference.parent_anchor =
								parent.exported_position;
							break;
						}
					}
				}
				reference.rest_translation =
					node.rest_translation;
				reference.joint_min_degrees =
					node.joint_min_degrees;
				reference.joint_max_degrees =
					node.joint_max_degrees;
				reference.sequences = &node.sequences;
				reference.point_groups = &node.point_groups;
				reference.locators =
					&model.gameplay_locators;
				reference.radius = node.radius;
				reference.bounds_min = node.bounds_min;
				reference.bounds_max = node.bounds_max;
				reference.health =
					static_cast<float>(node.maximum_hit_points);
				reference.maximum_health =
					static_cast<float>(node.maximum_hit_points);
				reference.source_flags = node.flags;
				reference.model_type = node.model_type;
				reference.part_group_id = node.part_group_id;
				reference.damage_group_selector =
					node.damage_group_selector;
				reference.runtime_flags =
					(node.flags & 0x0004u) != 0
						? 0x0020u
						: 0u;
				reference.render_flags =
					(node.flags & 0x0040u) != 0
						? 0x00040000u
						: 0u;
				reference.light_exclusion_mask =
					(model.root_flags
						& assets::kGameplayModelRootCompound) == 0
						? 3u : 0x18u;
				if (assets::object_type_is_planet(object.type))
				{
					// GameObject_create_runtime, 0x00467be9..0x00467bf7:
					// flags 0x101100 select the sine response, no baked colors,
					// and the rotation-only path which retains the first LOD.
					reference.render_flags = 0x00100000u;
					reference.light_exclusion_mask = 0x37u;
					reference.runtime_flags |= kRuntimeModelForceHighestDetail;
				}
				std::size_t light_channel_count = 0;
				for (const MissionGpuLod& lod : node.lods)
				{
					for (const MissionGpuSection& section : lod.sections)
					{
						light_channel_count = std::max(
							light_channel_count,
							static_cast<std::size_t>(
								section.light_channel) + 1);
					}
				}
				reference.light_channels.assign(
					light_channel_count, 0);
				if ((node.flags & 0x0080u) != 0)
				{
					for (const MissionGpuLod& lod : node.lods)
					{
						for (const MissionGpuSection& section
							: lod.sections)
						{
							if (section.light_channel
									< reference.light_channels.size()
								&& (section.mode == 6
									|| section.mode == 7))
							{
								reference.light_channels[
									section.light_channel] = 1;
							}
						}
					}
				}
				std::copy(
					std::begin(node.suppress_anim_rotation),
					std::end(node.suppress_anim_rotation),
					std::begin(
						reference.suppress_anim_rotation));
				reference.source_node =
					static_cast<std::uint16_t>(node_index);
				std::memcpy(
					reference.name,
					node.name,
					sizeof(reference.name));
				reference.owner_scope = 0;
				reference.parent_reference =
					node.parent_reference;
				char lowercase_name[65];
				std::strncpy(
					lowercase_name,
					node.name,
					sizeof(lowercase_name));
				lowercase_name[
					sizeof(lowercase_name) - 1] = '\0';
				for (char& character : lowercase_name)
				{
					character = static_cast<char>(
						std::tolower(
							static_cast<unsigned char>(
								character)));
				}
				reference.forcefield =
					std::strstr(
						lowercase_name,
						"forcefield") != nullptr;
			}
		}
		enumerate_gun_mounts(
			object, model, glm::mat4{1.0f}, 0);
		append_embedded_model_trees(renderer, object, model, 0);
		// Cloak creation walks the recursive scene hierarchy for every
		// object. Only compound objects additionally publish its network ID.
		std::uint32_t next_network_model_id = 0;
		assign_network_model_ids(
			renderer,
			object,
			model,
			0,
			(object.runtime_flags & game::kObjectFlagCompound) != 0
				? &next_network_model_id : nullptr);
		build_gun_pair_table(object);
		object.attack_run_direction_count =
			static_cast<std::uint8_t>(
				std::min<std::size_t>(
					model.gun_clearance_directions.size(),
					std::size(object.attack_run_directions)));
		for (std::uint8_t index = 0;
			index < object.attack_run_direction_count;
			++index)
		{
			object.attack_run_directions[index] =
				model.gun_clearance_directions[index];
		}
		game::attachments_initialize_hardpoints(
			object,
			model.hardpoints,
			renderer.deathmatch_mission,
			false);
		for (std::uint8_t slot = 0;
			slot < object.attachment_count;
			++slot)
		{
			game::AttachmentSlot& attachment =
				object.attachments[slot];
			const assets::GameplayHardpoint& hardpoint =
				model.hardpoints[slot];
			attachment.local_position = hardpoint.position;
			attachment.local_orientation = hardpoint.basis;
			if (hardpoint.source_node < model.nodes.size())
			{
				const MissionGpuNode& source_node =
					model.nodes[hardpoint.source_node];
				attachment.model_reference =
					static_cast<std::int16_t>(
						source_node.runtime_model_index);
				const glm::mat4 hardpoint_transform =
					glm::translate(
						glm::mat4{1.0f},
						hardpoint.position)
					* glm::mat4(hardpoint.basis);
				attachment.hardpoint_from_model =
					glm::inverse(source_node.object_transform)
					* hardpoint_transform;
			}
			if (attachment.live_model
				&& attachment.definition_index >= 0
				&& static_cast<std::size_t>(attachment.definition_index)
					< game::kAttachmentDefinitionCount)
			{
				const std::uint32_t variant =
					attachment.alternate_model ? 1u : 0u;
				if (renderer.attachment_model_loaded[
						attachment.definition_index][variant])
				{
					initialize_attachment_cloak_models(
						attachment,
						renderer.attachment_models[
							attachment.definition_index][variant],
						object.type);
				}
			}
		}
		game::world_initialize_cloak_meshes(world, object);
		if (object.player)
		{
			char loadout[256]{};
			std::size_t used = 0;
			for (const assets::GameplayHardpoint& hardpoint
				: model.hardpoints)
			{
				const int written = std::snprintf(
					loadout + used,
					sizeof(loadout) - used,
					"%s[%d,%d,%d,%d,%d]",
					used == 0 ? "" : " ",
					hardpoint.default_loadout[0],
					hardpoint.default_loadout[1],
					hardpoint.default_loadout[2],
					hardpoint.default_loadout[3],
					hardpoint.default_loadout[4]);
				if (written <= 0
					|| static_cast<std::size_t>(written)
						>= sizeof(loadout) - used)
				{
					break;
				}
				used += static_cast<std::size_t>(written);
			}
			diagnostics::mission_log(
				"player hardpoints=%u loadout=%s",
				static_cast<unsigned>(model.hardpoints.size()),
				loadout);
			object.gun_frame_shape =
				gun_frame_shape_for_type(object.type);
			diagnostics::mission_log(
				"hud guns mounts=%u first_mount_kind=%d "
				"first_bullet_type=%u groups=%u frame=%u",
				static_cast<unsigned>(object.gun_mount_count),
				object.gun_mount_count == 0
					? -1
					: static_cast<int>(
						object.gun_mounts[0].mount_kind),
				object.gun_mount_count == 0
					? 0u
					: static_cast<unsigned>(
						object.gun_mounts[0].bullet_type),
				static_cast<unsigned>(object.gun_group_count),
				static_cast<unsigned>(object.gun_frame_shape));
		}
		const bool compound =
			(object.runtime_flags & game::kObjectFlagCompound) != 0;
		if (compound)
		{
			++initialized_compound_models;
		}
		register_model_components(
			renderer,
			object,
			model,
			0,
			compound);
		game::model_animation_initialize_object(object);
		if (object.explosion_model_variant != UINT8_MAX)
		{
			const auto set_named_hidden = [&](const char* name, bool hidden)
			{
				for (game::ObjectModelReference& reference
					: object.model_references)
				{
					if (std::strcmp(reference.name, name) != 0)
					{
						continue;
					}
					if (hidden)
					{
						reference.runtime_flags |= 0x0020u;
					}
					else
					{
						reference.runtime_flags &= ~0x0020u;
					}
					break;
				}
			};
			if (object.type == 151)
			{
				for (std::uint8_t door = 1; door <= 3; ++door)
				{
					char name[] = "Stalag Door 1";
					name[12] = static_cast<char>('0' + door);
					set_named_hidden(
						name, door != object.explosion_model_variant);
				}
			}
			else if (object.type == 116)
			{
				for (std::uint8_t plate = 1; plate <= 3; ++plate)
				{
					char name[] = "Core plate 1";
					name[11] = static_cast<char>('0' + plate);
					set_named_hidden(
						name, plate != object.explosion_model_variant);
				}
			}
		}
		if (object.effect_scale != 1.0f)
		{
			// Asteroid Explode mode 3 writes the fragment scale into the
			// first retained model's +0x48 runtime scale after construction.
			game::retained_set_root_model_scale(
				object, glm::vec3{object.effect_scale});
		}
		build_exhaust_hazard_volumes(renderer, model, object);
		++initialized_models;
		initialized_components += object.component_count;
		object.components_initialized = true;
		game::exhaust_hazard_register_object(world, object);
	}
	if (initialized_models != 0)
	{
		diagnostics::mission_log(
			"models initialized=%u compound=%u component_refs=%u",
			initialized_models,
			initialized_compound_models,
			initialized_components);
	}
}

bool mission_renderer_init(
	io::Vfs& vfs,
	const FrontendRenderer& frontend,
	MissionRenderer& renderer,
	const mission::Runtime& mission_runtime,
	std::uint16_t player_type,
	std::uint8_t graphics_quality,
	std::uint32_t& random_seed,
	std::uint32_t gameplay_tick)
{
	mission_renderer_shutdown(renderer);
	renderer.graphics_quality = graphics_quality;
	renderer.deathmatch_mission =
		mission::network_is_deathmatch_mission(
			mission_runtime.mission_number);
	// SR_objects_init sets the global multiplier to 1.0. The first call to
	// Objects_service_render then applies the ordinary adaptive update and
	// quality clamp using the profiler total (zero on a fresh profiler).
	renderer.lod_detail_scale = 1.0f;
	renderer.lod_previous_submit_nanoseconds = 0;
	renderer.lod_submit_time_valid = false;
	if (!frontend.ready)
	{
		diagnostics::mission_log(
			"render load failed stage=frontend detail=renderer-not-ready");
		return false;
	}
	assets::TextureCache texture_cache;
	if (!assets::texture_cache_load(
			vfs, "tcachehw.dat", "palette.ccb", texture_cache))
	{
		diagnostics::mission_log(
			"render load failed stage=texture-cache");
		return false;
	}
	if (!initialize_environment_renderer(
			vfs,
			texture_cache,
			renderer.environment,
			graphics_quality,
			random_seed,
			gameplay_tick))
	{
		diagnostics::mission_log(
			"render load failed stage=environment");
		mission_renderer_shutdown(renderer);
		return false;
	}
	if (!frame_geometry_register_layout(
			frontend.frame_geometry, renderer.environment.layout))
	{
		diagnostics::mission_log(
			"render load failed stage=frame-geometry");
		mission_renderer_shutdown(renderer);
		return false;
	}
	if (!initialize_engine_flares(texture_cache, renderer))
	{
		diagnostics::mission_log(
			"render load failed stage=engine-flares");
		mission_renderer_shutdown(renderer);
		return false;
	}
	renderer.dock_ring_phase = 0;
	renderer.dock_ring_last_tick = gameplay_tick;
	renderer.dock_ring_tick_valid = true;
	assets::TextureImage laser_cannon;
	assets::TextureImage pulse_cannon;
	assets::TextureImage pulse_cannon_alternate;
	assets::TextureImage collapser_cannon;
	assets::TextureImage collapser_cannon_alternate;
	assets::TextureImage ion_cannon;
	assets::TextureImage ion_beam;
	assets::TextureImage ion_field;
	assets::TextureImage nova_cannon;
	assets::TextureImage huge_gun;
	assets::TextureImage spark_huge;
	assets::TextureImage muzzle_flare_a;
	assets::TextureImage muzzle_flare_b;
	if (!assets::texture_cache_decode(
			texture_cache, "lasers", laser_cannon)
		|| !mission_texture_upload(
			renderer.laser_cannon_texture, laser_cannon)
		|| !assets::texture_cache_decode(
			texture_cache, "1pulse", pulse_cannon)
		|| !mission_texture_upload(
			renderer.pulse_cannon_texture[0], pulse_cannon)
		|| !assets::texture_cache_decode(
			texture_cache, "1pulse-e", pulse_cannon_alternate)
		|| !mission_texture_upload(
			renderer.pulse_cannon_texture[1],
			pulse_cannon_alternate)
		|| !assets::texture_cache_decode(
			texture_cache, "7colgun", collapser_cannon)
		|| !mission_texture_upload(
			renderer.collapser_cannon_texture[0],
			collapser_cannon)
		|| !assets::texture_cache_decode(
			texture_cache, "7colgun-e",
			collapser_cannon_alternate)
		|| !mission_texture_upload(
			renderer.collapser_cannon_texture[1],
			collapser_cannon_alternate)
		|| !assets::texture_cache_decode(
			texture_cache, "ionc", ion_cannon)
		|| !mission_texture_upload(
			renderer.ion_cannon_texture, ion_cannon)
		|| !assets::texture_cache_decode(
			texture_cache, "laser3", ion_beam)
		|| !mission_texture_upload(
			renderer.ion_beam_texture, ion_beam)
		|| !assets::texture_cache_decode(
			texture_cache, "warpin3", ion_field)
		|| !mission_texture_upload(
			renderer.ion_field_texture, ion_field)
		|| !assets::texture_cache_decode(
			texture_cache, "laser2", nova_cannon)
		|| !mission_texture_upload(
			renderer.nova_cannon_texture, nova_cannon)
		|| !assets::texture_cache_decode(
			texture_cache, "sunlayer3", huge_gun)
		|| !mission_texture_upload(
			renderer.huge_gun_texture, huge_gun)
		|| !assets::texture_cache_decode(
			texture_cache, "alhuge", spark_huge)
		|| !mission_texture_upload(
			renderer.spark_huge_texture, spark_huge)
		|| !assets::texture_cache_decode(
			texture_cache, "matflarea3", muzzle_flare_a)
		|| !mission_texture_upload(
			renderer.muzzle_flare_a_texture, muzzle_flare_a)
		|| !assets::texture_cache_decode(
			texture_cache, "matflareb3", muzzle_flare_b)
		|| !mission_texture_upload(
			renderer.muzzle_flare_b_texture, muzzle_flare_b))
	{
		diagnostics::mission_log(
			"render load failed stage=gun-projectile textures");
		mission_renderer_shutdown(renderer);
		return false;
	}
	assets::TextureImage particle;
	assets::TextureImage damage_particle;
	assets::TextureImage powercore;
	assets::TextureImage missile_trail;
	assets::TextureImage missile_flare;
	if (!assets::texture_cache_decode(
			texture_cache, "partic4", particle)
		|| !mission_texture_upload(
			renderer.particle_texture, particle)
		|| !assets::texture_cache_decode(
			texture_cache, "partic7", damage_particle)
		|| !mission_texture_upload(
			renderer.damage_particle_texture, damage_particle)
		|| !assets::texture_cache_decode(
			texture_cache, "partic5", powercore)
		|| !mission_texture_upload(
			renderer.powercore_texture, powercore)
		|| !assets::texture_cache_decode(
			texture_cache, "mtrail2", missile_trail)
		|| !mission_texture_upload(
			renderer.missile_trail_texture, missile_trail)
		|| !assets::texture_cache_decode(
			texture_cache, "partic6", missile_flare)
		|| !mission_texture_upload(
			renderer.missile_flare_texture, missile_flare))
	{
		diagnostics::mission_log(
			"render load failed stage=particle textures");
		mission_renderer_shutdown(renderer);
		return false;
	}
	assets::TextureImage explosion_sheet;
	assets::TextureImage flak_explosion;
	if (!assets::texture_cache_decode(
			texture_cache, "explosion sheet", explosion_sheet)
		|| !mission_texture_upload(
			renderer.explosion_sheet_texture, explosion_sheet)
		|| !assets::texture_cache_decode(
			texture_cache, "flak04", flak_explosion)
		|| !mission_texture_upload(
			renderer.flak_explosion_texture, flak_explosion))
	{
		diagnostics::mission_log(
			"render load failed stage=explosion texture=explosion-sheet");
		mission_renderer_shutdown(renderer);
		return false;
	}
	for (std::uint32_t frame = 0;
		frame < std::size(renderer.destruction_explosion_textures);
		++frame)
	{
		char name[16]{};
		std::snprintf(
			name,
			sizeof(name),
			"bang_000%02u",
			static_cast<unsigned>(frame));
		assets::TextureImage image;
		if (!assets::texture_cache_decode(texture_cache, name, image)
			|| !mission_texture_upload(
				renderer.destruction_explosion_textures[frame],
				image))
		{
			diagnostics::mission_log(
				"render load failed stage=explosion texture=%s",
				name);
			mission_renderer_shutdown(renderer);
			return false;
		}
	}
	assets::TextureImage cloak;
	assets::TextureImage model_light_flare;
	assets::TextureImage model_light_core;
	if (!assets::texture_cache_decode(
			texture_cache, "cloak64", cloak)
		|| !mission_texture_upload(
			renderer.cloak_texture, cloak)
		|| !assets::texture_cache_decode(
			texture_cache, "newflare", model_light_flare)
		|| !mission_texture_upload(
			renderer.model_light_flare_texture,
			model_light_flare)
		|| !assets::texture_cache_decode(
			texture_cache, "newlight", model_light_core)
		|| !mission_texture_upload(
			renderer.model_light_core_texture,
			model_light_core))
	{
		diagnostics::mission_log(
			"render load failed stage=object-effect textures");
		mission_renderer_shutdown(renderer);
		return false;
	}
	assets::TextureImage shield;
	assets::TextureImage forcefield;
	assets::TextureImage sfx_alpha;
	assets::TextureImage shockwave[5];
	if (!assets::texture_cache_decode(
			texture_cache, "shield128", shield)
		|| !mission_texture_upload(
			renderer.shield_texture, shield)
		|| !assets::texture_cache_decode(
			texture_cache, "ffield", forcefield)
		|| !mission_texture_upload(
			renderer.forcefield_texture, forcefield)
		|| !assets::texture_cache_decode(
			texture_cache, "sfxalpha1", sfx_alpha)
		|| !mission_texture_upload(
			renderer.sfx_alpha_texture, sfx_alpha)
		|| !assets::texture_cache_decode(
			texture_cache, "rng_02", shockwave[0])
		|| !mission_texture_upload(
			renderer.shockwave_textures[0], shockwave[0])
		|| !assets::texture_cache_decode(
			texture_cache, "rng_03", shockwave[1])
		|| !mission_texture_upload(
			renderer.shockwave_textures[1], shockwave[1])
		|| !assets::texture_cache_decode(
			texture_cache, "rng_04", shockwave[2])
		|| !mission_texture_upload(
			renderer.shockwave_textures[2], shockwave[2])
		|| !assets::texture_cache_decode(
			texture_cache, "rng_01", shockwave[3])
		|| !mission_texture_upload(
			renderer.shockwave_textures[3], shockwave[3])
		|| !assets::texture_cache_decode(
			texture_cache, "rng_06", shockwave[4])
		|| !mission_texture_upload(
			renderer.shockwave_textures[4], shockwave[4]))
	{
		diagnostics::mission_log(
			"render load failed stage=shield-textures");
		mission_renderer_shutdown(renderer);
		return false;
	}
	assets::TextureImage jump_trail;
	assets::TextureImage jump_flare;
	assets::TextureImage jump_light;
	assets::TextureImage jump_light_bright;
	assets::TextureImage warp_primary;
	assets::TextureImage warp_secondary;
	if (!assets::texture_cache_decode(
			texture_cache, "trail3", jump_trail)
		|| !mission_texture_upload(
			renderer.jump_trail_texture, jump_trail)
		|| !assets::texture_cache_decode(
			texture_cache, "jflare", jump_flare)
		|| !mission_texture_upload(
			renderer.jump_flare_texture, jump_flare)
		|| !assets::texture_cache_decode(
			texture_cache, "flare-b", jump_light)
		|| !mission_texture_upload(
			renderer.jump_light_texture, jump_light)
		|| !assets::texture_cache_decode(
			texture_cache, "flare-lb", jump_light_bright)
		|| !mission_texture_upload(
			renderer.jump_light_bright_texture, jump_light_bright)
		|| !assets::texture_cache_decode(
			texture_cache, "warp128", warp_primary)
		|| !mission_texture_upload(
			renderer.warp_primary_texture, warp_primary)
		|| !assets::texture_cache_decode(
			texture_cache, "laser2", warp_secondary)
		|| !mission_texture_upload(
			renderer.warp_secondary_texture, warp_secondary))
	{
		diagnostics::mission_log(
			"render load failed stage=transition-textures");
		mission_renderer_shutdown(renderer);
		return false;
	}
	const char* cockpit_path = cockpit_model_path(player_type);
	assets::GameplayModel cockpit_model;
	if (cockpit_path == nullptr
		|| !assets::load_gameplay_model(
			vfs, cockpit_path, texture_cache, cockpit_model)
		|| !upload_model(
			cockpit_model,
			renderer.cockpit_model,
			frontend.lit_model_layout))
	{
		diagnostics::mission_log(
			"render load failed stage=cockpit-model type=%u path=%s",
			static_cast<unsigned>(player_type),
			cockpit_path != nullptr ? cockpit_path : "none");
		mission_renderer_shutdown(renderer);
		return false;
	}
	renderer.cockpit_model_loaded = true;
	bool required_models[assets::kObjectTypeResourceCount]{};
	// Explosion_init (LANCER.EXE 0x0046b240) creates the ten shared
	// deb_1..deb_10 render objects independently of mission-authored
	// objects. Particle fragments retain those catalog resource indices.
	for (std::uint16_t type = 0x4eu; type <= 0x57u; ++type)
	{
		required_models[type] = true;
	}
	// The same initializer also retains the three reachable xdeb heavy
	// resources and all five rock chunks. Neither family is guaranteed to
	// appear as a mission-authored object, but both are selected directly by
	// Explosion_fragment_spawn/Rock_chunk_spawn.
	for (std::uint16_t type = 0x58u; type <= 0x5au; ++type)
	{
		required_models[type] = true;
	}
	for (std::uint16_t type = 0xb2u; type <= 0xb6u; ++type)
	{
		required_models[type] = true;
	}
	// Generic_destruction_sequence_start creates these compiled breakaway
	// types on demand. Preload them for the portable renderer just as the
	// retail object/resource layer makes them available to world_create.
	constexpr std::uint16_t explosion_breakaway_types[] = {
		93, 75, 117, 183, 130, 197, 143, 150, 170, 173,
		186, 187, 189, 191, 217, 195, 196, 169, 198, 107,
	};
	for (const std::uint16_t type : explosion_breakaway_types)
	{
		required_models[type] = true;
	}
	// Player launch and landing strategies create these close-up hangar
	// objects at runtime. They are not mission-authored records, so their
	// complete model trees must be retained explicitly before world component
	// initialization sees the dynamically created scene object.
	required_models[0xd4u] = true; // yam_tube.shp
	required_models[0xd5u] = true; // yamhanger.shp
	required_models[0xd6u] = true; // reliant_hang.shp
	if (mission::network_is_deathmatch_mission(
			mission_runtime.mission_number))
	{
		// DMPowerup_create_pool allocates twenty type-0x6f mines before the
		// renderer starts. They are not authored mission records, but their
		// shared model must still be resident when a pool slot is activated.
		required_models[0x6f] = true;
	}
	for (std::uint16_t mission_index = 0;
		mission_index < mission_runtime.object_count;
		++mission_index)
	{
		const mission::ObjectRecord& object =
			mission_runtime.objects[mission_index];
		if (object.type == 995 || object.type == 996
			|| object.type == 997 || object.type == 999)
		{
			continue;
		}
		const std::uint8_t group_class =
			object.group < mission_runtime.group_count
				? mission_runtime.groups[object.group].object_class
				: UINT8_MAX;
		std::uint16_t type = assets::object_type_for_mission_creation(
			object.type,
			mission_runtime.mission_number,
			group_class,
			mission_runtime.mission_25_alternate);
		if (mission_runtime.object_factory_mode == 1
			&& mission_runtime.mission_number < 19
			&& type == 0x0d)
		{
			type = 0x0c;
		}
		type = assets::object_type_runtime_alias(type);
		if (type < std::size(required_models))
		{
			required_models[type] = true;
		}
	}
	std::uint32_t loaded_object_models = 0;
	for (std::uint32_t index = 0;
		index < assets::kObjectTypeResourceCount;
		++index)
	{
		if (!required_models[index])
		{
			continue;
		}
		const assets::ObjectTypeResourceDefinition& resource =
			assets::object_type_resource(
				static_cast<std::uint16_t>(index));
		if (resource.model_path == nullptr)
		{
			continue;
		}
		assets::GameplayModel model;
		sl_open::Blob stored_model;
		if (!io::vfs_read_all(vfs, resource.model_path, stored_model))
		{
			diagnostics::mission_log(
				"render load failed stage=model-read index=%u path=%s",
				index,
				resource.model_path);
			mission_renderer_shutdown(renderer);
			return false;
		}
		if (!assets::parse_gameplay_model(
				static_cast<sl_open::Blob&&>(stored_model),
				texture_cache,
				model,
				renderer.deathmatch_mission
					&& index < assets::kPlayerShipCount,
				assets::object_type_is_planet(static_cast<std::uint16_t>(index))))
		{
			diagnostics::mission_log(
				"render load failed stage=model-parse index=%u path=%s",
				index,
				resource.model_path);
			mission_renderer_shutdown(renderer);
			return false;
		}
		// Explosion_system_init acquires model[0] +0x10c, the count-prefixed
		// LOD set. Its 1.5/2.5 multipliers extend thresholds only.
		if (index >= 78 && index <= 91)
		{
			const float threshold_scale = index < 88 ? 1.5f : 2.5f;
			for (assets::GameplayLod& lod : model.nodes[0].lods)
			{
				lod.threshold *= threshold_scale;
			}
		}
		if (!upload_model(
				model,
				renderer.models[index],
				frontend.lit_model_layout))
		{
			diagnostics::mission_log(
				"render load failed stage=model-upload index=%u path=%s",
				index,
				resource.model_path);
			mission_renderer_shutdown(renderer);
			return false;
		}
		renderer.model_loaded[index] = true;
		++loaded_object_models;
	}
	{
		constexpr std::size_t kDecoyIndex =
			static_cast<std::size_t>(MissionModel::decoy);
		assets::GameplayModel model;
		if (!assets::load_gameplay_model(
				vfs, kDecoyModelPath, texture_cache, model)
			|| !upload_model(
				model,
				renderer.models[kDecoyIndex],
				frontend.lit_model_layout))
		{
			diagnostics::mission_log(
				"render load failed stage=model index=%u path=%s",
				static_cast<unsigned>(kDecoyIndex),
				kDecoyModelPath);
			mission_renderer_shutdown(renderer);
			return false;
		}
		renderer.model_loaded[kDecoyIndex] = true;
	}
	for (std::uint32_t index = 0;
		index < game::kAttachmentDefinitionCount;
		++index)
	{
		for (std::uint32_t variant_index = 0;
			variant_index < 2;
			++variant_index)
		{
			const char* path = game::attachment_definition_model(
				static_cast<std::int16_t>(index),
				variant_index != 0
					? game::AttachmentModelVariant::alternate
					: game::AttachmentModelVariant::primary);
			if (path == nullptr)
			{
				continue;
			}
			assets::GameplayModel model;
			if (!assets::load_gameplay_model(
				vfs, path, texture_cache, model)
				|| !upload_model(
					model,
					renderer.attachment_models[
						index][variant_index],
					frontend.lit_model_layout))
			{
				diagnostics::mission_log(
					"render load failed stage=attachment-model "
					"index=%u variant=%u path=%s",
					index,
					variant_index,
					path);
				mission_renderer_shutdown(renderer);
				return false;
			}
			renderer.attachment_model_loaded[
				index][variant_index] = true;
		}
	}
	for (std::size_t index = 0;
		index < std::size(renderer.models);
		++index)
	{
		if (!renderer.model_loaded[index])
		{
			continue;
		}
		if (assets::object_type_is_planet(static_cast<std::uint16_t>(index)))
		{
			// Planet mesh recentering follows the retail object-bound
			// calculation; retain the original aggregate bounds from upload.
			continue;
		}
		recenter_model_with_embedded_attachments(
			renderer.models[index],
			renderer.attachment_models);
	}
	{
		assets::SpriteList sprites[
			static_cast<std::size_t>(MissionModel::count)];
		FrontendSpriteAtlasSource sources[
			static_cast<std::size_t>(MissionModel::count)
				* kMissionHudSchematicShapeCount];
		std::uint32_t source_count = 0;
		for (std::uint32_t index = 0;
			index < assets::kObjectTypeResourceCount;
			++index)
		{
			if (!required_models[index])
			{
				continue;
			}
			const char* path =
				assets::object_type_resource(
					static_cast<std::uint16_t>(index))
					.schematic_path;
			if (path == nullptr)
			{
				continue;
			}
			if (!assets::load_sprite_list(
					vfs, path, sprites[index], false)
				|| (sprites[index].shape_count != 1
					&& sprites[index].shape_count
						!= kMissionHudSchematicShapeCount))
			{
				diagnostics::mission_log(
					"render load failed stage=hud-schematic index=%u path=%s",
					index,
					path);
				mission_renderer_shutdown(renderer);
				return false;
			}
			MissionHudSchematic& schematic =
				renderer.hud_schematics[index];
			for (std::uint32_t shape = 0;
				shape < sprites[index].shape_count;
				++shape)
			{
				sources[source_count++] = {
					&sprites[index],
					shape,
					&schematic.shapes[shape],
				};
			}
			// The retail archive contains both five-record fighter SCEMs and
			// one-record capital/object SCEMs. The base silhouette is record
			// zero in both forms; rejecting the one-record form prevents
			// mission 29 from entering flight at all.
			schematic.ready = true;
		}
		if (!frontend_sprite_atlas_upload(
				renderer.hud_schematic_atlas,
				sources,
				source_count))
		{
			diagnostics::mission_log(
				"render load failed stage=hud-schematic-atlas shapes=%u",
				source_count);
			mission_renderer_shutdown(renderer);
			return false;
		}
	}
	if (!upload_starfield(vfs, renderer, random_seed))
	{
		diagnostics::mission_log(
			"render load failed stage=starfield path=space.tga");
		mission_renderer_shutdown(renderer);
		return false;
	}
	// Space_init (LANCER.EXE 0x004a4e70) consumes the shared CRT random
	// stream for far asteroids and both star layers before the later
	// environment-effect initializers create their Boom Mesh records.
	initialize_planet_bombard(
		renderer.environment, random_seed, gameplay_tick);
	assets::TextureImage missile_lock_ring;
	if (!assets::load_tga(vfs, "TARRING.TGA", missile_lock_ring)
		|| missile_lock_ring.width != 256
		|| missile_lock_ring.height != 256)
	{
		diagnostics::mission_log(
			"render load failed stage=missile-lock path=TARRING.TGA");
		mission_renderer_shutdown(renderer);
		return false;
	}
	// TARRING.TGA is a 32-bit grayscale mask whose authored alpha byte is
	// zero for every texel. Surrender's ring material takes opacity from the
	// texture intensity and then applies the per-mesh alpha written by
	// HUD_update_missile_lock_ring (LANCER.EXE 0x00491be1..0x00491c3a).
	// The generic RGBA frontend shader instead multiplies texture alpha, so
	// publish the authored grayscale channel solely as the modern mask alpha
	// and normalize RGB. Leaving grayscale in RGB as well would apply edge
	// coverage twice when the frontend alpha blend runs.
	for (std::size_t pixel = 0;
		pixel < static_cast<std::size_t>(missile_lock_ring.width)
			* missile_lock_ring.height;
		++pixel)
	{
		std::uint8_t* texel =
			missile_lock_ring.pixels.data + pixel * 4;
		const std::uint8_t mask = texel[0];
		texel[0] = 255;
		texel[1] = 255;
		texel[2] = 255;
		texel[3] = mask;
	}
	if (!mission_texture_upload(
			renderer.missile_lock_ring, missile_lock_ring))
	{
		diagnostics::mission_log(
			"render load failed stage=missile-lock path=TARRING.TGA");
		mission_renderer_shutdown(renderer);
		return false;
	}
	renderer.ready = true;
	diagnostics::mission_log(
		"render object catalog loaded=%u capacity=%u",
		loaded_object_models,
		static_cast<unsigned>(assets::kObjectTypeResourceCount));
	return true;
}

void mission_renderer_shutdown(MissionRenderer& renderer)
{
	for (ExplodingMeshGpuIndices& gpu : renderer.exploding_mesh_indices)
	{
		if (bgfx::isValid(gpu.handle))
		{
			bgfx::destroy(gpu.handle);
		}
		gpu = {};
	}
	destroy_environment_renderer(renderer.environment);
	destroy_model(renderer.cockpit_model);
	renderer.cockpit_model_loaded = false;
	for (MissionEngineFlare& flare : renderer.engine_flares)
	{
		destroy_environment_mesh(flare.mesh);
		frontend_texture_shutdown(flare.material_a);
		frontend_texture_shutdown(flare.material_b);
	}
	for (std::size_t index = 0;
		index < std::size(renderer.models);
		++index)
	{
		destroy_model(renderer.models[index]);
		renderer.model_loaded[index] = false;
	}
	for (std::uint32_t definition = 0;
		definition < game::kAttachmentDefinitionCount;
		++definition)
	{
		for (std::uint32_t variant_index = 0;
			variant_index < 2;
			++variant_index)
		{
			destroy_model(
				renderer.attachment_models[
					definition][variant_index]);
			renderer.attachment_model_loaded[
				definition][variant_index] = false;
		}
	}
	for (FrontendTexture& page : renderer.hud_schematic_atlas.pages)
	{
		frontend_texture_shutdown(page);
	}
	renderer.hud_schematic_atlas.page_count = 0;
	for (MissionHudSchematic& schematic : renderer.hud_schematics)
	{
		schematic = {};
	}
	frontend_texture_shutdown(renderer.missile_lock_ring);
	frontend_texture_shutdown(renderer.laser_cannon_texture);
	for (FrontendTexture& texture : renderer.pulse_cannon_texture)
	{
		frontend_texture_shutdown(texture);
	}
	for (FrontendTexture& texture : renderer.collapser_cannon_texture)
	{
		frontend_texture_shutdown(texture);
	}
	frontend_texture_shutdown(renderer.ion_cannon_texture);
	frontend_texture_shutdown(renderer.ion_beam_texture);
	frontend_texture_shutdown(renderer.ion_field_texture);
	frontend_texture_shutdown(renderer.nova_cannon_texture);
	frontend_texture_shutdown(renderer.huge_gun_texture);
	frontend_texture_shutdown(renderer.spark_huge_texture);
	frontend_texture_shutdown(renderer.muzzle_flare_a_texture);
	frontend_texture_shutdown(renderer.muzzle_flare_b_texture);
	frontend_texture_shutdown(renderer.particle_texture);
	frontend_texture_shutdown(renderer.damage_particle_texture);
	frontend_texture_shutdown(renderer.powercore_texture);
	frontend_texture_shutdown(renderer.missile_trail_texture);
	frontend_texture_shutdown(renderer.missile_flare_texture);
	frontend_texture_shutdown(renderer.explosion_sheet_texture);
	frontend_texture_shutdown(renderer.flak_explosion_texture);
	for (FrontendTexture& texture
		: renderer.destruction_explosion_textures)
	{
		frontend_texture_shutdown(texture);
	}
	frontend_texture_shutdown(renderer.cloak_texture);
	frontend_texture_shutdown(renderer.model_light_flare_texture);
	frontend_texture_shutdown(renderer.model_light_core_texture);
	frontend_texture_shutdown(renderer.shield_texture);
	frontend_texture_shutdown(renderer.forcefield_texture);
	frontend_texture_shutdown(renderer.sfx_alpha_texture);
	for (FrontendTexture& texture : renderer.shockwave_textures)
	{
		frontend_texture_shutdown(texture);
	}
	frontend_texture_shutdown(renderer.jump_trail_texture);
	frontend_texture_shutdown(renderer.jump_flare_texture);
	frontend_texture_shutdown(renderer.jump_light_texture);
	frontend_texture_shutdown(renderer.jump_light_bright_texture);
	frontend_texture_shutdown(renderer.warp_primary_texture);
	frontend_texture_shutdown(renderer.warp_secondary_texture);
	renderer.star_count = 0;
	renderer.dock_ring_phase = 0;
	renderer.dock_ring_last_tick = 0;
	renderer.dock_ring_tick_valid = false;
	renderer.lod_previous_submit_nanoseconds = 0;
	renderer.lod_submit_time_valid = false;
	renderer.deathmatch_mission = false;
	renderer.ready = false;
}

void update_adaptive_lod_detail(MissionRenderer& renderer)
{
	const auto now = std::chrono::steady_clock::now().time_since_epoch();
	const std::uint64_t nanoseconds =
		static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
	const float previous_frame_seconds = renderer.lod_submit_time_valid
		? static_cast<float>(
			nanoseconds - renderer.lod_previous_submit_nanoseconds)
			* 0.000000001f
		: 0.0f;
	renderer.lod_previous_submit_nanoseconds = nanoseconds;
	renderer.lod_submit_time_valid = true;

	// Objects_service_render (0x004924b0) uses the previous frame's summed
	// QueryPerformanceCounter profiler time. These constants are the exact
	// retail .rdata values at 0x004dc404/408/474/560/584/8a8.
	const bool high = renderer.graphics_quality >= 2;
	const float lower_target_fps = high ? 40.0f : 30.0f;
	const float upper_target_fps = high ? 60.0f : 40.0f;
	const float minimum = renderer.graphics_quality == 0
		? 0.75f
		: renderer.graphics_quality == 1 ? 1.0f : 1.5f;
	const float maximum = renderer.graphics_quality == 0
		? 1.5f
		: renderer.graphics_quality == 1 ? 2.0f : 3.0f;
	if (previous_frame_seconds < 1.0f / upper_target_fps)
	{
		renderer.lod_detail_scale += 0.05000000074505806f;
	}
	if (1.0f / lower_target_fps < previous_frame_seconds)
	{
		renderer.lod_detail_scale -= 0.5f;
	}
	renderer.lod_detail_scale = std::clamp(
		renderer.lod_detail_scale, minimum, maximum);
}

void mission_renderer_submit(
	MissionRenderer& renderer,
	const FrontendRenderer& frontend,
	const MissionRenderFrame& frame,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	float brightness)
{
	if (!renderer.ready || drawable_width == 0 || drawable_height == 0)
	{
		return;
	}
	update_adaptive_lod_detail(renderer);
	const float projection_aspect =
		frame.horizontal_tangent
		/ frame.vertical_tangent;
	const MissionSceneLights& scene_lights =
		build_scene_lights(renderer, frame);
	const glm::mat4 view =
		sl_open::math::camera_rotation_view(frame.camera_orientation);
	const glm::mat4 environment_projection =
		sl_open::math::perspective_lh_reverse_infinite_framebuffer_y_down(
			2.0f * std::atan(frame.vertical_tangent),
			projection_aspect,
			kMissionNearPlane,
			bgfx::getCaps()->homogeneousDepth);
	bgfx::setViewRect(
		kMissionEnvironmentView,
		0,
		0,
		static_cast<std::uint16_t>(drawable_width),
		static_cast<std::uint16_t>(drawable_height));
	bgfx::setViewClear(
		kMissionEnvironmentView,
		BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
		0x000000ff,
		0.0f);
	bgfx::setViewMode(
		kMissionEnvironmentView, bgfx::ViewMode::Sequential);
	bgfx::setViewTransform(
		kMissionEnvironmentView,
		glm::value_ptr(view),
		glm::value_ptr(environment_projection));
	bgfx::touch(kMissionEnvironmentView);
	submit_nebula(
		renderer.environment,
		frontend,
		frame,
		std::min(brightness, 1.0f));

	// srd3d_render_list (srd3d.dll 0x10003390) walks retained objects in
	// reverse submission order. The dome, sun BMOs, and stars render during
	// that walk; blended mesh faces are prepended to a second list and reverse
	// once more. The resulting retail phases are dome, sun, stars, far
	// asteroids, atmospheres, then the additive nebula grid.
	for (const bgfx::ViewId background_view : {
			kMissionSunView,
			kMissionFarAsteroidView,
			kMissionAtmosphereView,
			kMissionNebulaGridView,
		})
	{
		bgfx::setViewRect(
			background_view,
			0,
			0,
			static_cast<std::uint16_t>(drawable_width),
			static_cast<std::uint16_t>(drawable_height));
		bgfx::setViewClear(
			background_view,
			BGFX_CLEAR_NONE,
			0,
			1.0f);
		bgfx::setViewMode(
			background_view, bgfx::ViewMode::Sequential);
		bgfx::setViewTransform(
			background_view,
			glm::value_ptr(view),
			glm::value_ptr(environment_projection));
		bgfx::touch(background_view);
	}

	bgfx::setViewRect(
		kMissionStarView,
		0,
		0,
		static_cast<std::uint16_t>(drawable_width),
		static_cast<std::uint16_t>(drawable_height));
	bgfx::setViewClear(
		kMissionStarView,
		BGFX_CLEAR_NONE,
		0,
		1.0f);
	bgfx::setViewMode(kMissionStarView, bgfx::ViewMode::Sequential);
	const glm::mat4 star_view{1.0f};
	const glm::mat4 star_projection = sl_open::math::orthographic_lh(
		0.0f,
		static_cast<float>(drawable_width),
		static_cast<float>(drawable_height),
		0.0f,
		0.0f,
		100.0f,
		bgfx::getCaps()->homogeneousDepth);
	bgfx::setViewTransform(
		kMissionStarView,
		glm::value_ptr(star_view),
		glm::value_ptr(star_projection));
	bgfx::touch(kMissionStarView);
	bgfx::setViewRect(
		kMissionView,
		0,
		0,
		static_cast<std::uint16_t>(drawable_width),
		static_cast<std::uint16_t>(drawable_height));
	bgfx::setViewClear(
		kMissionView,
		BGFX_CLEAR_NONE,
		0,
		1.0f);
	bgfx::setViewMode(kMissionView, bgfx::ViewMode::Sequential);
	const glm::mat4 projection =
		sl_open::math::perspective_lh_reverse_infinite_framebuffer_y_down(
		2.0f * std::atan(frame.vertical_tangent),
		projection_aspect,
		kMissionNearPlane,
		bgfx::getCaps()->homogeneousDepth);
	bgfx::setViewTransform(
		kMissionView,
		glm::value_ptr(view),
		glm::value_ptr(projection));
	bgfx::touch(kMissionView);
	renderer.transparent_submission_order = 0;
	bgfx::setViewRect(
		kMissionTransparentView,
		0,
		0,
		static_cast<std::uint16_t>(drawable_width),
		static_cast<std::uint16_t>(drawable_height));
	bgfx::setViewClear(
		kMissionTransparentView,
		BGFX_CLEAR_NONE,
		0,
		1.0f);
	bgfx::setViewMode(
		kMissionTransparentView,
		bgfx::ViewMode::DepthDescending);
	bgfx::setViewTransform(
		kMissionTransparentView,
		glm::value_ptr(view),
		glm::value_ptr(projection));
	bgfx::touch(kMissionTransparentView);
	if (cockpit_visible(renderer, frame))
	{
		// The cockpit is a foreground layer. A dedicated reverse-depth clear
		// prevents nearby world meshes from piercing it while retaining normal
		// depth testing and self-occlusion within the two cockpit nodes.
		bgfx::setViewRect(
			kMissionCockpitView,
			0,
			0,
			static_cast<std::uint16_t>(drawable_width),
			static_cast<std::uint16_t>(drawable_height));
		bgfx::setViewClear(
			kMissionCockpitView,
			BGFX_CLEAR_DEPTH,
			0,
			0.0f);
		bgfx::setViewMode(
			kMissionCockpitView, bgfx::ViewMode::DepthAscending);
		bgfx::setViewTransform(
			kMissionCockpitView,
			glm::value_ptr(view),
			glm::value_ptr(projection));
		bgfx::touch(kMissionCockpitView);
	}
	const MissionSunFrame sun = update_sun_frame(
		renderer.environment,
		frame,
		drawable_width,
		drawable_height);

	VisibleStar visible[kMissionStarCount + kMissionRandomStarCount];
	std::uint32_t visible_count = 0;
	const bool first_sample =
		!renderer.star_sample_valid
		|| renderer.previous_camera_cut_serial
			!= frame.camera_cut_serial;
	const glm::mat3 previous_camera = first_sample
		? frame.camera_orientation
		: renderer.previous_star_camera;
	const glm::vec3 previous_position = first_sample
		? frame.camera_position
		: renderer.previous_star_position;
	const float half_height = frame.vertical_tangent;
	const float half_width = frame.horizontal_tangent;
	const glm::vec2 clip_minimum{-half_width, -half_height};
	const glm::vec2 clip_maximum{half_width, half_height};

	// Space_init creates a second, 200-point mode-one star object after the
	// one hundred atlas tiles. Its points repeat in an 8192-unit cube around
	// the moving camera, so translation produces the close-star streak layer
	// while the tiled sphere below supplies the distant sky.
	for (const MissionStar& star : renderer.random_stars)
	{
		glm::vec3 current_wrapped;
		glm::vec3 previous_wrapped;
		bool continuous = true;
		for (std::uint32_t axis = 0; axis < 3; ++axis)
		{
			current_wrapped[axis] = wrap_random_star_component(
				star.source[axis] - frame.camera_position[axis]);
			previous_wrapped[axis] = wrap_random_star_component(
				star.source[axis] - previous_position[axis]);
			if (std::abs(
					current_wrapped[axis] - previous_wrapped[axis])
				> 4095.0f)
			{
				continuous = false;
				break;
			}
		}
		if (!continuous)
		{
			continue;
		}
		glm::vec3 current =
			glm::transpose(frame.camera_orientation) * current_wrapped;
		glm::vec3 previous =
			glm::transpose(previous_camera) * previous_wrapped;
		const float distance_squared = glm::dot(current, current);
		if (!clip_random_star_near(current, previous))
		{
			continue;
		}
		glm::vec2 endpoint_current{
			current.x / current.z,
			current.y / current.z,
		};
		glm::vec2 endpoint_previous{
			previous.x / previous.z,
			previous.y / previous.z,
		};
		const glm::vec2 difference =
			endpoint_previous - endpoint_current;
		const float manhattan =
			std::abs(difference.x) + std::abs(difference.y);
		const float length_factor = std::min(manhattan, 0.1f);
		if (manhattan > 0.1f)
		{
			endpoint_previous =
				endpoint_current + difference * (0.1f / manhattan);
		}
		if (!clip_star_segment(
				endpoint_current,
				endpoint_previous,
				clip_minimum,
				clip_maximum))
		{
			continue;
		}
		const float intensity = std::clamp(
			(0.25f - distance_squared / (8191.0f * 8191.0f))
				* 16.0f
				/ (length_factor * 100.0f + 1.0f),
			0.0f,
			1.0f);
		if (intensity <= 0.0f)
		{
			continue;
		}
		VisibleStar& output = visible[visible_count++];
		output.current = sl_open::math::camera_plane_to_framebuffer(
			endpoint_current,
			{half_width, half_height},
			{drawable_width, drawable_height});
		output.previous = sl_open::math::camera_plane_to_framebuffer(
			endpoint_previous,
			{half_width, half_height},
			{drawable_width, drawable_height});
		output.color = scale_star_color(star.color, intensity, false);
		output.half_color = scale_star_color(star.color, intensity, true);
		const glm::vec2 pixel_delta = output.previous - output.current;
		output.point = glm::dot(pixel_delta, pixel_delta) <= 1.0f;
	}

	// Retail submits tiles 0..99 to a prepend-only scene list, so draw
	// traversal visits them 99..0.
	for (std::uint32_t tile_index = kMissionStarTileCount;
		tile_index-- > 0;)
	{
		const MissionStarTile& tile =
			renderer.star_tiles[tile_index];
		const glm::mat3 current_relative =
			glm::transpose(frame.camera_orientation)
			* tile.orientation;
		const glm::mat3 previous_relative =
			glm::transpose(previous_camera)
			* tile.orientation;
		const float facing =
			previous_relative[2][2] < 0.0f ? -1.0f : 1.0f;
		if (previous_relative[2][2] * facing
			< 0.6000000238418579f)
		{
			continue;
		}
		for (std::uint32_t index = 0;
			index < tile.count;
			++index)
		{
			const MissionStar& star =
				renderer.stars[tile.first + index];
			const glm::vec3 current =
				current_relative * star.source;
			const glm::vec3 previous =
				previous_relative * star.source;
			const double current_facing =
				static_cast<double>(facing * current.z);
			const double previous_facing =
				static_cast<double>(facing * previous.z);
			if (current_facing < 0.6
				|| previous_facing < 0.6
				|| (current_facing < 0.7999999046325683
					&& previous_facing < 0.7999999046325683))
			{
				continue;
			}
			glm::vec2 endpoint_current{
				current.x / current.z,
				current.y / current.z,
			};
			glm::vec2 endpoint_previous{
				previous.x / previous.z,
				previous.y / previous.z,
			};
			const glm::vec2 difference =
				endpoint_previous - endpoint_current;
			const float manhattan =
				std::abs(difference.x) + std::abs(difference.y);
			const float length_factor = manhattan > 0.1f
				? 0.1f
				: manhattan;
			if (manhattan > 0.1f)
			{
				endpoint_previous =
					endpoint_current
					+ difference * (0.1f / manhattan);
			}
			if (!clip_star_segment(
					endpoint_current,
					endpoint_previous,
					clip_minimum,
					clip_maximum))
			{
				continue;
			}
			const float intensity = std::clamp(
				1.0f / (length_factor * 100.0f + 1.0f),
				0.0f,
				1.0f);
			VisibleStar& output = visible[visible_count++];
			output.current = sl_open::math::camera_plane_to_framebuffer(
				endpoint_current,
				{half_width, half_height},
				{drawable_width, drawable_height});
			output.previous = sl_open::math::camera_plane_to_framebuffer(
				endpoint_previous,
				{half_width, half_height},
				{drawable_width, drawable_height});
			output.color =
				scale_star_color(star.color, intensity, false);
			output.half_color =
				scale_star_color(star.color, intensity, true);
			const glm::vec2 pixel_delta =
				output.previous - output.current;
			output.point =
				glm::dot(pixel_delta, pixel_delta) <= 1.0f;
		}
	}
	renderer.previous_star_camera = frame.camera_orientation;
	renderer.previous_star_position = frame.camera_position;
	renderer.previous_camera_cut_serial = frame.camera_cut_serial;
	renderer.star_sample_valid = true;

	// SRD3D walks the preprocessed star array once and issues each point or
	// line in that order. Preserve mixed point/line overlap order by batching
	// only adjacent primitives of the same kind.
	for (std::uint32_t first = 0; first < visible_count;)
	{
		const bool point = visible[first].point;
		std::uint32_t end = first + 1;
		while (end < visible_count && visible[end].point == point)
		{
			++end;
		}
		const std::uint32_t primitive_count = end - first;
		const std::uint32_t vertex_count =
			primitive_count * (point ? 1u : 2u);
		if (get_available_frame_vertices(frontend.frame_geometry,
				vertex_count, frontend.layout) < vertex_count)
		{
			break;
		}
		FrameVertexBuffer buffer;
		alloc_frame_vertex_buffer(frontend.frame_geometry,
			&buffer, vertex_count, frontend.layout);
		StarVertex* vertices =
			reinterpret_cast<StarVertex*>(buffer.data);
		std::uint32_t output_index = 0;
		for (std::uint32_t index = first; index < end; ++index)
		{
			vertices[output_index++] = {
				visible[index].current.x,
				visible[index].current.y,
				1.0f,
				visible[index].color,
				0.5f,
				0.5f,
			};
			if (!point)
			{
				vertices[output_index++] = {
					visible[index].previous.x,
					visible[index].previous.y,
					1.0f,
					visible[index].half_color,
					0.5f,
					0.5f,
				};
			}
		}
		bgfx::setTransform(glm::value_ptr(star_view));
		const float uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
		const float tint[] = {1.0f, 1.0f, 1.0f, 1.0f};
		bgfx::setUniform(frontend.uv_rect_uniform, uv);
		bgfx::setUniform(frontend.tint_uniform, tint);
		set_frame_vertex_buffer(0, &buffer);
		bgfx::setTexture(
			0, frontend.texture_sampler, frontend.white.handle);
		bgfx::setState(
			BGFX_STATE_WRITE_RGB
				| BGFX_STATE_WRITE_A
				| retail_blend_state(
					RetailBlendSelector::additive)
				| (point
					? BGFX_STATE_PT_POINTS
					: BGFX_STATE_PT_LINES));
		bgfx::submit(kMissionStarView, frontend.rgba_program);
		first = end;
	}
	submit_sun(renderer.environment, frontend, frame, sun);
	service_planet_bombard(
		renderer.environment,
		frame,
		drawable_width);
	submit_far_asteroids(renderer.environment, frontend, frame);
	submit_planet_atmospheres(
		renderer,
		renderer.environment,
		frontend,
		frame,
		sun.flare,
		brightness);
	for (std::uint32_t index = 0;
		index < frame.instance_count;
		++index)
	{
		const MissionRenderInstance instance =
			with_planet_spin(frame.instances[index], frame.world);
		const MissionGpuModel& model =
			renderer.models[static_cast<std::size_t>(instance.model)];
		submit_model_with_locator_children(
			renderer,
			model,
			instance,
			frontend,
			scene_lights,
			frame,
			true,
			true,
				0);
	}
	submit_exploding_meshes(renderer, frontend, scene_lights, frame);
	submit_engine_flares(renderer, frontend, frame);
	submit_missiles(renderer, frontend, frame, scene_lights);
	submit_missile_trails(renderer, frontend, frame);
	submit_launch_external_trails(renderer, frontend, frame);
	submit_muzzle_flashes(renderer, frontend, frame);
	submit_gun_projectiles(
		renderer, frontend, frame, scene_lights);
	submit_nova_beams(renderer, frontend, frame);
	submit_shockwaves(renderer, frontend, frame);
	submit_electric_rays(renderer, frontend, frame);
	// Particle_system_render (0x0049c8e0) services/submits the companion
	// spark batch before walking the ten shared particle arrays.
	submit_sparks(renderer, frontend, frame);
	submit_shared_particles(renderer, frontend, frame);
	submit_transition_effects(renderer, frontend, frame);
	submit_ion_cannon_effects(renderer, frontend, frame);
	submit_particle_effects(renderer, frontend, frame);
	if (submit_spherical_shields(renderer, frontend, frame))
	{
		submit_cap_shields(renderer, frontend, frame);
	}
	submit_cockpit(renderer, frontend, scene_lights, frame);
	submit_dock_ring(renderer, frontend, frame);
}
}
