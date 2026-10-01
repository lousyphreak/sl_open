#include "game/death_effects.hpp"

#include "ai/runtime.hpp"
#include "assets/ship_stats.hpp"
#include "core/math.hpp"
#include "core/mission_log.hpp"
#include "game/disruption_effects.hpp"
#include "game/model_animation.hpp"
#include "game/particle_emitters.hpp"
#include "game/world.hpp"
#include "mission/events.hpp"
#include "mission/runtime.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace sl_open::game
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kRandScale = 1.0f / 32767.0f;

std::uint8_t alpha_byte(float alpha)
{
	return static_cast<std::uint8_t>(
		std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
}

std::uint32_t green_color(float alpha)
{
	return
		(static_cast<std::uint32_t>(alpha_byte(alpha)) << 24)
		| 0x0000ff00u;
}

std::uint32_t pack_effect_color(
	float red,
	float green,
	float blue,
	float alpha)
{
	// bgfx exposes the little-endian normalized Uint8 attribute bytes to
	// the shader as red, green, blue, alpha, so the integer is ABGR.
	return
		(static_cast<std::uint32_t>(alpha_byte(alpha)) << 24)
		| (static_cast<std::uint32_t>(alpha_byte(blue)) << 16)
		| (static_cast<std::uint32_t>(alpha_byte(green)) << 8)
		| static_cast<std::uint32_t>(alpha_byte(red));
}

void build_uber_sphere(TransitionMesh& mesh, bool close_south_pole)
{
	mesh = {};
	mesh.texture = TransitionTexture::solid;
	mesh.blend = TransitionBlend::additive;
	mesh.double_sided = true;
	mesh.active = true;
	mesh.vertices.reserve(128);
	mesh.vertices.push_back({
		{0.0f, 0.0f, 0.0f},
		pack_effect_color(0.0f, 0.0f, 0.0f, 0.3f),
		{0.5f, 0.5f},
	});
	for (std::uint16_t latitude = 1; latitude < 8; ++latitude)
	{
		const float radial = std::sin(
			static_cast<float>(latitude) * kPi / 16.0f);
		for (std::uint16_t longitude = 0; longitude < 18; ++longitude)
		{
			const float angle =
				static_cast<float>(longitude) * kPi / 9.0f;
			mesh.vertices.push_back({
				{},
				pack_effect_color(0.0f, 0.0f, 0.0f, 0.3f),
				{
					std::cos(angle) * radial * 0.5f + 0.5f,
					std::sin(angle) * radial * 0.5f + 0.5f,
				},
			});
		}
	}
	mesh.vertices.push_back({
		{close_south_pole ? 0.0f : 1.0f,
		 0.0f,
		 close_south_pole ? -1.0f : 0.0f},
		pack_effect_color(0.0f, 0.0f, 0.0f, 0.3f),
		{close_south_pole ? 0.5f : 1.0f,
		 close_south_pole ? 1.0f : 0.5f},
	});
	for (std::uint16_t longitude = 0; longitude < 18; ++longitude)
	{
		const std::uint16_t next =
			static_cast<std::uint16_t>((longitude + 1) % 18);
		mesh.indices.insert(
			mesh.indices.end(),
			{0,
			 static_cast<std::uint16_t>(1 + longitude),
			 static_cast<std::uint16_t>(1 + next)});
	}
	for (std::uint16_t latitude = 0; latitude < 6; ++latitude)
	{
		const std::uint16_t first =
			static_cast<std::uint16_t>(1 + latitude * 18);
		const std::uint16_t second =
			static_cast<std::uint16_t>(first + 18);
		for (std::uint16_t longitude = 0; longitude < 18; ++longitude)
		{
			const std::uint16_t next =
				static_cast<std::uint16_t>((longitude + 1) % 18);
			mesh.indices.insert(
				mesh.indices.end(),
				{
					static_cast<std::uint16_t>(first + longitude),
					static_cast<std::uint16_t>(second + longitude),
					static_cast<std::uint16_t>(second + next),
					static_cast<std::uint16_t>(first + longitude),
					static_cast<std::uint16_t>(second + next),
					static_cast<std::uint16_t>(first + next),
				});
		}
	}
	if (close_south_pole)
	{
		const std::uint16_t south = 127;
		const std::uint16_t last_ring = 109;
		for (std::uint16_t longitude = 0; longitude < 18; ++longitude)
		{
			const std::uint16_t next =
				static_cast<std::uint16_t>((longitude + 1) % 18);
			mesh.indices.insert(
				mesh.indices.end(),
				{static_cast<std::uint16_t>(last_ring + longitude),
				 south,
				 static_cast<std::uint16_t>(last_ring + next)});
		}
	}
	mesh.base_positions.resize(mesh.vertices.size());
}

void set_uber_cap(
	TransitionMesh& mesh,
	float completion,
	float alpha)
{
	const float cap = std::clamp(completion, 0.0f, 1.0f);
	std::size_t vertex = 0;
	for (std::uint16_t latitude = 0; latitude <= 8; ++latitude)
	{
		const float theta =
			static_cast<float>(latitude) * cap * kPi / 16.0f;
		const float radial = std::sin(theta);
		const float z = std::cos(theta) - 1.0f;
		const std::uint16_t count =
			latitude == 0 || latitude == 8 ? 1 : 18;
		for (std::uint16_t longitude = 0;
			longitude < count;
			++longitude)
		{
			const float angle =
				static_cast<float>(longitude) * kPi / 9.0f;
			mesh.vertices[vertex].position = {
				std::cos(angle) * radial,
				std::sin(angle) * radial,
				z,
			};
			mesh.vertices[vertex].color =
				vertex + 18 < mesh.vertices.size()
				? pack_effect_color(
					alpha / 0.3f,
					alpha / 0.3f,
					alpha / 0.3f,
					alpha)
				: 0;
			mesh.base_positions[vertex] =
				mesh.vertices[vertex].position;
			++vertex;
		}
	}
}

void set_uber_inner_sphere(
	TransitionMesh& mesh,
	World& world)
{
	std::size_t vertex = 0;
	for (std::uint16_t latitude = 0; latitude <= 8; ++latitude)
	{
		const float theta =
			static_cast<float>(latitude) * kPi / 8.0f;
		const float radial = std::sin(theta);
		const float z = std::cos(theta);
		const std::uint16_t count =
			latitude == 0 || latitude == 8 ? 1 : 18;
		for (std::uint16_t longitude = 0;
			longitude < count;
			++longitude)
		{
			const float angle =
				static_cast<float>(longitude) * kPi / 9.0f;
			mesh.vertices[vertex].position = {
				std::cos(angle) * radial,
				std::sin(angle) * radial,
				z,
			};
			float intensity =
				static_cast<float>(world_rand15(world))
					* kRandScale;
			intensity =
				intensity * intensity * intensity
				* intensity * intensity;
			mesh.vertices[vertex].color =
				pack_effect_color(
					intensity,
					intensity * 0.3f,
					0.0f,
					1.0f);
			mesh.vertices[vertex].uv = {
				0.3515625f, 0.140625f};
			mesh.base_positions[vertex] =
				mesh.vertices[vertex].position;
			++vertex;
		}
	}
}

void build_tractor_beam(TransitionMesh& beam)
{
	beam = {};
	beam.texture = TransitionTexture::warp_secondary;
	// TractorBeam_create (LANCER.EXE 0x0041cbc0) uses retail selector
	// four, SRCALPHA/ONE, so the animated beam alpha reaches blending.
	beam.blend = TransitionBlend::source_alpha_additive;
	beam.double_sided = true;
	beam.vertices.resize(16);
	const glm::vec3 square[4] = {
		{-50.0f, -50.0f, 0.0f},
		{-50.0f, 50.0f, 0.0f},
		{50.0f, 50.0f, 0.0f},
		{50.0f, -50.0f, 0.0f},
	};
	for (std::uint16_t index = 0; index < 4; ++index)
	{
		beam.vertices[index].position = square[index];
		beam.vertices[index].uv = {
			(index == 2 || index == 3) ? 1.0f : 0.0f,
			(index == 1 || index == 2) ? 1.0f : 0.0f,
		};
	}
	for (std::uint16_t ribbon = 0; ribbon < 3; ++ribbon)
	{
		const float angle = static_cast<float>(ribbon) * 2.0f * kPi / 3.0f;
		const float x = std::sin(angle) * 50.0f;
		const float y = std::cos(angle) * 50.0f;
		const std::uint16_t base = static_cast<std::uint16_t>(4 + ribbon * 4);
		beam.vertices[base + 0].position = {-x, -y, 0.0f};
		beam.vertices[base + 1].position = {-x, -y, 400.0f};
		beam.vertices[base + 2].position = {x, y, 400.0f};
		beam.vertices[base + 3].position = {x, y, 0.0f};
		for (std::uint16_t corner = 0; corner < 4; ++corner)
		{
			beam.vertices[base + corner].uv = {
				(corner == 2 || corner == 3) ? 1.0f : 0.0f,
				(corner == 1 || corner == 2) ? 1.0f : 0.0f,
			};
		}
	}
	for (std::uint16_t group = 0; group < 4; ++group)
	{
		const std::uint16_t base =
			static_cast<std::uint16_t>(group * 4);
		beam.indices.insert(
			beam.indices.end(),
			{base, static_cast<std::uint16_t>(base + 1),
			 static_cast<std::uint16_t>(base + 2),
			 base, static_cast<std::uint16_t>(base + 2),
			 static_cast<std::uint16_t>(base + 3)});
	}
	beam.base_positions.reserve(beam.vertices.size());
	for (const TransitionVertex& vertex : beam.vertices)
	{
		beam.base_positions.push_back(vertex.position);
	}
}

void build_shield(TransitionMesh& shield)
{
	shield = {};
	shield.texture = TransitionTexture::warp_secondary;
	shield.blend = TransitionBlend::additive;
	shield.double_sided = true;
	constexpr std::uint16_t longitude_count = 12;
	constexpr std::uint16_t latitude_count = 6;
	for (std::uint16_t latitude = 0; latitude <= latitude_count; ++latitude)
	{
		const float v =
			static_cast<float>(latitude) / latitude_count;
		const float pitch = (v - 0.5f) * kPi;
		for (std::uint16_t longitude = 0;
			longitude <= longitude_count;
			++longitude)
		{
			const float u =
				static_cast<float>(longitude) / longitude_count;
			const float yaw = u * 2.0f * kPi;
			shield.vertices.push_back({
				{
					std::cos(pitch) * std::sin(yaw),
					std::sin(pitch),
					std::cos(pitch) * std::cos(yaw),
				},
				0xff00ff00u,
				{u, v},
			});
		}
	}
	for (std::uint16_t latitude = 0; latitude < latitude_count; ++latitude)
	{
		for (std::uint16_t longitude = 0;
			longitude < longitude_count;
			++longitude)
		{
			const std::uint16_t row = longitude_count + 1;
			const std::uint16_t a =
				static_cast<std::uint16_t>(latitude * row + longitude);
			const std::uint16_t b = static_cast<std::uint16_t>(a + row);
			shield.indices.insert(
				shield.indices.end(),
				{a, static_cast<std::uint16_t>(a + 1), b,
				 static_cast<std::uint16_t>(a + 1),
				 static_cast<std::uint16_t>(b + 1), b});
		}
	}
}

void set_beam_alpha(TransitionMesh& beam, float alpha)
{
	for (std::size_t index = 0; index < beam.vertices.size(); ++index)
	{
		const std::size_t corner = index & 3u;
		beam.vertices[index].color =
			green_color(corner == 0 || corner == 3 ? 0.0f : alpha);
	}
}

void set_beam_length(
	TransitionMesh& beam,
	const glm::vec3& origin,
	const glm::vec3& target)
{
	const glm::vec3 delta = target - origin;
	const float distance = glm::length(delta);
	beam.position = origin;
	if (distance > 0.0001f)
	{
		const glm::vec3 forward = delta / distance;
		const glm::vec3 helper =
			std::abs(forward.y) < 0.99f
				? glm::vec3{0.0f, 1.0f, 0.0f}
				: glm::vec3{1.0f, 0.0f, 0.0f};
		const glm::vec3 right = glm::normalize(glm::cross(helper, forward));
		const glm::vec3 up = glm::cross(forward, right);
		beam.orientation = glm::mat3{right, up, forward};
	}
	for (std::size_t index = 0; index < beam.vertices.size(); ++index)
	{
		if ((index & 3u) == 1 || (index & 3u) == 2)
		{
			beam.vertices[index].position.z = distance;
		}
	}
}

void build_respawn_quad(TransitionMesh& portal, float radius)
{
	portal = {};
	portal.texture = TransitionTexture::warp_primary;
	portal.blend = TransitionBlend::additive;
	portal.double_sided = true;
	portal.vertices = {
		{{-radius, -radius, 0.0f}, 0xffff004du, {0.0f, 0.0f}},
		{{radius, -radius, 0.0f}, 0xffff004du, {1.0f, 0.0f}},
		{{radius, radius, 0.0f}, 0xffff004du, {1.0f, 1.0f}},
		{{-radius, radius, 0.0f}, 0xffff004du, {0.0f, 1.0f}},
	};
	portal.indices = {0, 1, 2, 0, 2, 3};
}

void build_respawn_projection(TransitionMesh& projection)
{
	projection = {};
	projection.texture = TransitionTexture::warp_secondary;
	projection.blend = TransitionBlend::additive;
	projection.double_sided = true;
	constexpr float kHalfWidth = 50.0f;
	constexpr float kInitialLength = 400.0f;
	projection.vertices = {
		{{-kHalfWidth, -kHalfWidth, 0.0f}, 0xff0000ffu, {0.0f, 0.0f}},
		{{kHalfWidth, -kHalfWidth, 0.0f}, 0xff0000ffu, {1.0f, 0.0f}},
		{{kHalfWidth, kHalfWidth, 0.0f}, 0xff0000ffu, {1.0f, 1.0f}},
		{{-kHalfWidth, kHalfWidth, 0.0f}, 0xff0000ffu, {0.0f, 1.0f}},
	};
	projection.indices = {0, 1, 2, 0, 2, 3};
	for (std::uint16_t side = 0; side < 3; ++side)
	{
		// WProject_Mesh_create, LANCER.EXE 0x0041d2b0, authors three
		// longitudinal quads at successive twelve-degree offsets.
		const float angle = static_cast<float>(side) * kPi / 15.0f;
		const float x = std::sin(angle) * kHalfWidth;
		const float y = std::cos(angle) * kHalfWidth;
		const std::uint16_t base = static_cast<std::uint16_t>(
			projection.vertices.size());
		projection.vertices.insert(
			projection.vertices.end(),
			{
				{{-x, -y, 0.0f}, 0xff0000ffu, {0.0f, 0.0f}},
				{{-x, -y, kInitialLength},
					0xff0000ffu, {0.0f, 1.0f}},
				{{x, y, kInitialLength},
					0xff0000ffu, {1.0f, 1.0f}},
				{{x, y, 0.0f}, 0xff0000ffu, {1.0f, 0.0f}},
			});
		projection.indices.insert(
			projection.indices.end(),
			{base, static_cast<std::uint16_t>(base + 1),
			 static_cast<std::uint16_t>(base + 2),
			 base, static_cast<std::uint16_t>(base + 2),
			 static_cast<std::uint16_t>(base + 3)});
	}
}

glm::mat3 respawn_projector_orientation(const glm::vec3& direction)
{
	// SR_mat3_look_at_points, LANCER.EXE 0x004c1940.
	glm::vec3 local = direction;
	glm::mat3 orientation{1.0f};
	orientation = math::postrotate(
		orientation,
		std::atan2(local.x, local.z),
		glm::vec3{0.0f, 1.0f, 0.0f});
	local = glm::transpose(orientation) * local;
	return math::postrotate(
		orientation,
		-std::atan2(local.y, local.z),
		glm::vec3{1.0f, 0.0f, 0.0f});
}

float explosion_random(World& world)
{
	return static_cast<float>(world_rand15(world)) * kRandScale;
}

glm::vec3 explosion_world_center(
	const WorldObject& actor,
	const ExplodingMeshGeometry& geometry)
{
	return actor.position + actor.orientation * geometry.center;
}

bool build_explosion_source_geometry(
	const WorldObject& actor,
	std::uint16_t model_reference,
	ExplodingMeshGeometry& output)
{
	if (model_reference >= actor.model_references.size())
	{
		return false;
	}
	const ObjectModelReference& reference =
		actor.model_references[model_reference];
	if (reference.explosion_vertices == nullptr
		|| reference.explosion_indices == nullptr
		|| reference.explosion_faces == nullptr
		|| reference.explosion_face_corners == nullptr
		|| reference.explosion_sections == nullptr
		|| reference.explosion_normals == nullptr
		|| reference.explosion_secondary_normals == nullptr
		|| reference.explosion_static_lighting == nullptr
		|| reference.explosion_vertices->size()
			!= reference.explosion_normals->size()
		|| reference.explosion_vertices->size()
			!= reference.explosion_secondary_normals->size()
		|| reference.explosion_vertices->size()
			!= reference.explosion_static_lighting->size())
	{
		return false;
	}

	output = {};
	output.vertices = *reference.explosion_vertices;
	output.indices = *reference.explosion_indices;
	output.faces = *reference.explosion_faces;
	output.face_corners = *reference.explosion_face_corners;
	output.sections = *reference.explosion_sections;
	output.normals = *reference.explosion_normals;
	output.secondary_normals =
		*reference.explosion_secondary_normals;
	output.static_lighting_rgb =
		*reference.explosion_static_lighting;
	output.source_model = reference.explosion_source_model;
	output.source_attachment = reference.explosion_source_attachment;
	output.node_flags = reference.source_flags;
	output.light_exclusion_mask = reference.light_exclusion_mask;
	output.light_channels = reference.light_channels;
	output.static_lighting_enabled =
		(reference.source_flags & 0x0040u) != 0
		&& (reference.render_flags & 0x00040000u) != 0;

	const glm::mat4 model_transform = model_animation_render_transform(
		actor, model_reference, 1.0f);
	const glm::mat3 normal_transform = glm::transpose(
		glm::inverse(glm::mat3(model_transform)));
	for (std::size_t vertex = 0; vertex < output.vertices.size(); ++vertex)
	{
		assets::GameplayVertex& position = output.vertices[vertex];
		const glm::vec3 transformed = glm::vec3(
			model_transform
				* glm::vec4(position.x, position.y, position.z, 1.0f));
		position.x = transformed.x;
		position.y = transformed.y;
		position.z = transformed.z;
		output.normals[vertex] = glm::normalize(
			normal_transform * output.normals[vertex]);
		output.secondary_normals[vertex] = glm::normalize(
			normal_transform * output.secondary_normals[vertex]);
	}
	return !output.faces.empty();
}

void append_explosion_vertex(
	const ExplodingMeshGeometry& source,
	std::uint16_t source_vertex,
	ExplodingMeshGeometry& output)
{
	output.vertices.push_back(source.vertices[source_vertex]);
	output.normals.push_back(source.normals[source_vertex]);
	output.secondary_normals.push_back(
		source.secondary_normals[source_vertex]);
	output.static_lighting_rgb.push_back(
		source.static_lighting_rgb[source_vertex]);
	output.indices.push_back(static_cast<std::uint16_t>(
		output.vertices.size() - 1));
}

std::vector<ExplodingMeshGeometry> split_explosion_geometry(
	World& world,
	const ExplodingMeshGeometry& source,
	std::uint8_t plane_count)
{
	// Explosion_mesh_split, LANCER.EXE 0x0046bf20. Planes pass through the
	// owning object's origin. A face is classified from the sum of its
	// original (pre-triangulation) corners, then all of its attribute
	// streams are copied into one of 2^N independently recentered meshes.
	glm::vec3 planes[3]{};
	for (std::uint8_t plane = 0; plane < plane_count; ++plane)
	{
		planes[plane].z = explosion_random(world) - 0.5f;
		planes[plane].y = explosion_random(world) - 0.5f;
		planes[plane].x = explosion_random(world) - 0.5f;
	}
	const std::size_t group_count = std::size_t{1} << plane_count;
	std::vector<std::uint8_t> masks(source.faces.size());
	for (std::size_t face_index = 0;
		face_index < source.faces.size();
		++face_index)
	{
		const assets::GameplayFace& face = source.faces[face_index];
		glm::vec3 sum{0.0f};
		for (std::uint16_t corner = 0; corner < face.corner_count; ++corner)
		{
			const std::uint32_t corner_offset = face.first_corner + corner;
			if (corner_offset >= source.face_corners.size())
			{
				continue;
			}
			const std::uint32_t vertex =
				source.face_corners[corner_offset];
			if (vertex >= source.vertices.size())
			{
				continue;
			}
			const assets::GameplayVertex& position = source.vertices[vertex];
			sum += glm::vec3{position.x, position.y, position.z}
				+ source.center;
		}
		for (std::uint8_t plane = 0; plane < plane_count; ++plane)
		{
			if (glm::dot(sum, planes[plane]) > 0.0f)
			{
				masks[face_index] |=
					static_cast<std::uint8_t>(1u << plane);
			}
		}
	}

	std::vector<ExplodingMeshGeometry> groups(group_count);
	for (std::size_t group_index = 0;
		group_index < group_count;
		++group_index)
	{
		ExplodingMeshGeometry& output = groups[group_index];
		output.source_model = source.source_model;
		output.source_attachment = source.source_attachment;
		output.node_flags = source.node_flags;
		output.light_exclusion_mask = source.light_exclusion_mask;
		output.light_channels = source.light_channels;
		output.static_lighting_enabled =
			source.static_lighting_enabled;
		glm::vec3 center_sum{0.0f};
		std::uint32_t center_count = 0;
		std::uint16_t source_section = UINT16_MAX;
		for (std::size_t face_index = 0;
			face_index < source.faces.size();
			++face_index)
		{
			if (masks[face_index] != group_index)
			{
				continue;
			}
			const assets::GameplayFace& face = source.faces[face_index];
			if (face.section >= source.sections.size())
			{
				continue;
			}
			if (source_section != face.section)
			{
				source_section = face.section;
				output.sections.push_back(source.sections[source_section]);
				output.sections.back().first_index =
					static_cast<std::uint32_t>(output.indices.size());
				output.sections.back().index_count = 0;
			}
			const std::uint32_t output_face_first =
				static_cast<std::uint32_t>(output.indices.size());
			std::vector<std::pair<std::uint16_t, std::uint16_t>> remap;
			const std::uint32_t face_end = face.first_index + face.index_count;
			for (std::uint32_t index = face.first_index;
				index < face_end && index < source.indices.size();
				++index)
			{
				const std::uint16_t source_vertex = source.indices[index];
				if (source_vertex >= source.vertices.size())
				{
					continue;
				}
				const std::uint16_t output_vertex =
					static_cast<std::uint16_t>(output.vertices.size());
				append_explosion_vertex(source, source_vertex, output);
				remap.emplace_back(source_vertex, output_vertex);
			}
			const std::uint32_t output_first_corner =
				static_cast<std::uint32_t>(output.face_corners.size());
			for (std::uint16_t corner = 0;
				corner < face.corner_count;
				++corner)
			{
				const std::uint32_t corner_offset = face.first_corner + corner;
				if (corner_offset >= source.face_corners.size())
				{
					continue;
				}
				const std::uint32_t source_corner =
					source.face_corners[corner_offset];
				for (const auto& [from, to] : remap)
				{
					if (from != source_corner)
					{
						continue;
					}
					output.face_corners.push_back(to);
					const assets::GameplayVertex& position =
						source.vertices[from];
					center_sum += glm::vec3{
						position.x, position.y, position.z}
						+ source.center;
					++center_count;
					break;
				}
			}
			const std::uint32_t output_face_count =
				static_cast<std::uint32_t>(output.indices.size())
					- output_face_first;
			output.faces.push_back({
				output_face_first,
				output_face_count,
				output_first_corner,
				static_cast<std::uint16_t>(
					output.face_corners.size() - output_first_corner),
				static_cast<std::uint16_t>(output.sections.size() - 1),
			});
			output.sections.back().index_count += output_face_count;
		}
		if (center_count == 0)
		{
			output = {};
			continue;
		}
		output.center = center_sum / static_cast<float>(center_count);
		for (std::size_t vertex = 0; vertex < output.vertices.size(); ++vertex)
		{
			assets::GameplayVertex& position = output.vertices[vertex];
			glm::vec3 local{position.x, position.y, position.z};
			local += source.center - output.center;
			position.x = local.x;
			position.y = local.y;
			position.z = local.z;
			if (vertex == 0)
			{
				output.bounds_min = local;
				output.bounds_max = local;
			}
			else
			{
				output.bounds_min = glm::min(output.bounds_min, local);
				output.bounds_max = glm::max(output.bounds_max, local);
			}
			output.radius = std::max(output.radius, glm::length(local));
		}
	}
	return groups;
}

ExplodingMeshEffect* allocate_exploding_mesh(
	World& world,
	ExplodingMeshGeometry&& geometry,
	const WorldObject& actor,
	const glm::vec3& velocity_per_tick,
	const glm::vec3& angular_euler,
	std::uint32_t expiration_tick,
	bool trail,
	ParticleEmitterStyle trail_style,
	std::uint32_t simulation_tick)
{
	for (ExplodingMeshEffect& effect
		: world.death_effects.exploding_meshes)
	{
		if (effect.active)
		{
			continue;
		}
		effect = {};
		effect.geometry_serial =
			++world.death_effects.exploding_mesh_serial;
		effect.geometry = static_cast<ExplodingMeshGeometry&&>(geometry);
		effect.position = explosion_world_center(actor, effect.geometry);
		effect.orientation = actor.orientation;
		effect.velocity_per_tick = velocity_per_tick;
		effect.angular_step = math::rotation_from_euler(angular_euler);
		effect.expiration_tick = expiration_tick;
		effect.active = true;
		if (trail)
		{
			ParticleEmitter* emitter = particle_emitter_reserve_automatic(
				world,
				static_cast<std::uint8_t>(trail_style),
				1000,
				simulation_tick);
			if (emitter != nullptr)
			{
				effect.emitter_index = static_cast<std::uint16_t>(
					emitter - world.particles.emitters.data());
				emitter->local_position = effect.position;
				emitter->local_basis = effect.orientation;
				emitter->direction_center = {0.0f, 0.0f, 1.0f};
				emitter->direction_spread = {0.25f, 0.25f, 0.0f};
				emitter->speed_base = 5.0f;
				emitter->speed_random = 2.0f;
				emitter->model_owned = false;
			}
		}
		return &effect;
	}
	return nullptr;
}

glm::vec3 radial_piece_velocity(
	const WorldObject& actor,
	const ExplodingMeshGeometry& geometry,
	float radial_speed,
	float total_scale)
{
	const glm::vec3 radial = glm::normalize(
		actor.orientation * geometry.center);
	return (radial * radial_speed + actor.linear_velocity) * total_scale;
}
}

void death_effects_reset(DeathEffectsRuntime& runtime)
{
	const std::uint32_t exploding_mesh_serial =
		runtime.exploding_mesh_serial;
	runtime = {};
	runtime.exploding_mesh_serial = exploding_mesh_serial;
}

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
	std::uint32_t simulation_tick)
{
	// Explosion_billboard_spawn (LANCER.EXE 0x0046bd00) scans the fixed
	// 30-record pool from its beginning. The low random bits are retained
	// even for the separate-frame path, where the updater intentionally
	// leaves the full-image UV rectangle unchanged.
	for (ExplosionBillboardEffect& explosion
		: runtime.explosion_billboards)
	{
		if (explosion.active)
		{
			continue;
		}
		explosion = {
			position,
			velocity_per_tick,
			simulation_tick,
			duration_ticks,
			delay_ticks,
			static_cast<std::uint32_t>(world_rand15(world) & 3u)
				| (type == ExplosionBillboardType::separate_frames
					? 4u
					: 0u),
			size,
			type,
			create_light,
			animate_scale,
			alternate_atlas,
			true,
		};
		return true;
	}
	return false;
}

void destruction_light_create(
	DeathEffectsRuntime& runtime,
	ObjectHandle owner,
	std::uint16_t model_reference,
	const glm::vec3& local_position)
{
	for (DestructionLightEffect& light : runtime.destruction_lights)
	{
		if (light.active)
		{
			continue;
		}
		light = {};
		light.active = true;
		light.remaining = 1.0f;
		light.owner_index = owner.index;
		light.owner_generation = owner.generation;
		light.model_reference = model_reference;
		light.local_position = local_position;
		return;
	}
}

void powercore_effect_create(
	World& world,
	ObjectHandle owner,
	std::uint16_t model_reference,
	ParticleEmitterStyle emitter_style,
	std::uint32_t simulation_tick)
{
	if (owner.index >= world.death_effects.powercores.size()
		|| world_resolve(world, owner) == nullptr)
	{
		return;
	}
	powercore_effect_release(world, owner);
	PowercoreEffect& effect = world.death_effects.powercores[owner.index];
	effect = {};
	effect.active = true;
	effect.owner_index = owner.index;
	effect.owner_generation = owner.generation;
	effect.model_reference = model_reference;
	effect.size = 2500.0f;

	ParticleEmitter* emitter = particle_emitter_reserve_automatic(
		world,
		static_cast<std::uint8_t>(emitter_style),
		9999999u,
		simulation_tick);
	if (emitter == nullptr)
	{
		return;
	}
	effect.emitter_index = static_cast<std::uint16_t>(
		emitter - world.particles.emitters.data());
	emitter->local_position = {0.0f, 0.0f, 0.0f};
	emitter->local_basis = glm::mat3{1.0f};
	emitter->direction_center = {0.0f, 0.0f, 0.0f};
	emitter->direction_spread = {
		glm::two_pi<float>(),
		glm::quarter_pi<float>(),
		glm::two_pi<float>()};
	emitter->speed_base = 20.0f;
	emitter->speed_random = 10.0f;
	emitter->owner_index = owner.index;
	emitter->owner_generation = owner.generation;
	emitter->model_reference = model_reference;
	emitter->model_owned = true;
}

void powercore_effect_release(World& world, ObjectHandle owner)
{
	if (owner.index >= world.death_effects.powercores.size())
	{
		return;
	}
	PowercoreEffect& effect = world.death_effects.powercores[owner.index];
	if (!effect.active || effect.owner_generation != owner.generation)
	{
		return;
	}
	if (effect.emitter_index < world.particles.emitters.size())
	{
		ParticleEmitter& emitter =
			world.particles.emitters[effect.emitter_index];
		// Powercore_release, LANCER.EXE 0x0046e1c4..0x0046e1d9,
		// makes start+duration zero. The automatic emitter service owns
		// the following removal; already emitted particles remain alive.
		emitter.duration_ticks = -emitter.start_tick;
	}
	effect = {};
}

void rock_chunk_spawn_world(
	World& world,
	const glm::vec3& position,
	const glm::vec3& direction,
	bool giant,
	std::uint32_t simulation_tick)
{
	// Rock_chunk_spawn/Rock_chunk_create_in_current_slot, LANCER.EXE
	// 0x00472780/0x00472a00. The 300 records form an overwrite ring. The
	// resource and lifetime draws belong to the create helper and therefore
	// precede every transform draw made by the owning spawn path.
	DeathEffectsRuntime& runtime = world.death_effects;
	RockChunkEffect& chunk =
		runtime.rock_chunks[runtime.rock_chunk_cursor];
	chunk = {};
	chunk.active = true;
	chunk.model_resource = static_cast<std::uint16_t>(
		178u + world_rand15(world) % 5u);
	chunk.expiration_tick = simulation_tick + 20000u
		+ static_cast<std::uint32_t>(
			static_cast<float>(world_rand15(world))
				* kRandScale * 20000.0f);

	// Retail MSVC evaluates vector source arguments right to left.
	chunk.angular_velocity_per_tick.z =
		(static_cast<float>(world_rand15(world)) * kRandScale - 0.5f)
			* 0.1f;
	chunk.angular_velocity_per_tick.y =
		(static_cast<float>(world_rand15(world)) * kRandScale - 0.5f)
			* 0.1f;
	chunk.angular_velocity_per_tick.x =
		(static_cast<float>(world_rand15(world)) * kRandScale - 0.5f)
			* 0.1f;
	chunk.position = position;

	const float orientation_z =
		static_cast<float>(world_rand15(world)) * kRandScale * 2.0f * kPi;
	const float orientation_y =
		static_cast<float>(world_rand15(world)) * kRandScale * 2.0f * kPi;
	const float orientation_x =
		static_cast<float>(world_rand15(world)) * kRandScale * 2.0f * kPi;
	chunk.orientation = math::rotation_from_euler({
		orientation_x, orientation_y, orientation_z});

	glm::vec3 velocity = direction
		* ((1.0f
			+ static_cast<float>(world_rand15(world)) * kRandScale)
			* 6.0f);
	const float velocity_rotation_z =
		(static_cast<float>(world_rand15(world)) * kRandScale - 0.5f)
			* 0.2f;
	const float velocity_rotation_y =
		(static_cast<float>(world_rand15(world)) * kRandScale - 0.5f)
			* 0.2f;
	const float velocity_rotation_x =
		(static_cast<float>(world_rand15(world)) * kRandScale - 0.5f)
			* 0.2f;
	chunk.velocity_per_tick = math::rotation_from_euler({
		velocity_rotation_x,
		velocity_rotation_y,
		velocity_rotation_z}) * velocity;
	if (giant)
	{
		chunk.scale = 10.0f
			+ static_cast<float>(world_rand15(world))
				* kRandScale * 5.0f;
		chunk.velocity_per_tick *= 4.0f;
	}
	else
	{
		chunk.scale = 1.0f;
	}

	(void)explosion_billboard_create(
		runtime,
		world,
		chunk.position,
		glm::vec3{0.0f},
		ExplosionBillboardType::separate_frames,
		200.0f,
		40,
		true,
		0,
		false,
		true,
		simulation_tick);
	++runtime.rock_chunk_cursor;
	if (runtime.rock_chunk_cursor == runtime.rock_chunks.size())
	{
		runtime.rock_chunk_cursor = 0;
	}
}

void explosion_mesh_breakup_world(
	World& world,
	const WorldObject& actor,
	bool blue_trail,
	std::uint32_t simulation_tick)
{
	// GameObject_create_exploding_meshes, LANCER.EXE 0x0046c550. The
	// recursive scene walk splits every tag-one render model into four.
	// Outer ordinals zero/three enter the moving pool directly; ordinals
	// one/two are split once/twice again before insertion.
	const auto explode_reference = [&](auto&& self, std::uint16_t reference_index)
		-> void
	{
		if (reference_index >= actor.model_references.size())
		{
			return;
		}
		const ObjectModelReference& reference =
			actor.model_references[reference_index];
		if (!reference.removed && reference.model_type == 1)
		{
			ExplodingMeshGeometry source;
			if (build_explosion_source_geometry(
					actor, reference_index, source))
			{
				std::vector<ExplodingMeshGeometry> outer =
					split_explosion_geometry(world, source, 2);
				for (std::uint8_t ordinal = 0;
					ordinal < outer.size();
					++ordinal)
				{
					ExplodingMeshGeometry& group = outer[ordinal];
					if (group.faces.empty())
					{
						continue;
					}
					if (ordinal % 3 == 0)
					{
						const float radial_speed =
							14.0f + explosion_random(world) * 10.0f;
						glm::vec3 angular;
						angular.z = (explosion_random(world) - 0.5f) * 0.04f;
						angular.y = (explosion_random(world) - 0.5f) * 0.04f;
						angular.x = (explosion_random(world) - 0.5f) * 0.04f;
						const std::uint32_t expiration = simulation_tick
							+ 200u + world_rand15(world) % 300u;
						(void)allocate_exploding_mesh(
							world,
							static_cast<ExplodingMeshGeometry&&>(group),
							actor,
							radial_piece_velocity(
								actor, group, radial_speed, 0.25f),
							angular,
							expiration,
							true,
							blue_trail
								? ParticleEmitterStyle::blue_explosion
								: ParticleEmitterStyle::ordinary_explosion,
							simulation_tick);
						continue;
					}
					const std::uint8_t nested_depth = ordinal % 3;
					std::vector<ExplodingMeshGeometry> nested =
						split_explosion_geometry(
							world, group, nested_depth);
					for (ExplodingMeshGeometry& piece : nested)
					{
						if (piece.faces.empty())
						{
							continue;
						}
						const float angular_scale =
							static_cast<float>(ordinal)
							* (blue_trail ? 0.01f : 0.05f);
						glm::vec3 angular;
						angular.z =
							(explosion_random(world) - 0.5f) * angular_scale;
						angular.y =
							(explosion_random(world) - 0.5f) * angular_scale;
						angular.x =
							(explosion_random(world) - 0.5f) * angular_scale;
						const std::uint32_t expiration = simulation_tick
							+ (blue_trail ? 300u : 0u)
							+ world_rand15(world) % 300u;
						const glm::vec3 velocity = radial_piece_velocity(
							actor,
							piece,
							static_cast<float>(ordinal) * 20.0f,
							0.25f);
						(void)allocate_exploding_mesh(
							world,
							static_cast<ExplodingMeshGeometry&&>(piece),
							actor,
							velocity,
							angular,
							expiration,
							false,
							ParticleEmitterStyle::ordinary_explosion,
							simulation_tick);
					}
				}
			}
		}
		for (std::uint16_t child = 0;
			child < actor.model_references.size();
			++child)
		{
			if (actor.model_references[child].parent_reference
				== static_cast<std::int16_t>(reference_index))
			{
				self(self, child);
			}
		}
	};
	for (std::uint16_t reference = 0;
		reference < actor.model_references.size();
		++reference)
	{
		if (actor.model_references[reference].parent_reference < 0)
		{
			explode_reference(explode_reference, reference);
		}
	}
}

void explosion_component_breakup_world(
	World& world,
	const WorldObject& actor,
	const ObjectModelReference& component,
	float group_radius,
	std::uint32_t simulation_tick)
{
	if (actor.model_references.empty()
		|| &component < actor.model_references.data()
		|| &component >= actor.model_references.data()
			+ actor.model_references.size())
	{
		return;
	}
	const std::uint16_t root = static_cast<std::uint16_t>(
		&component - actor.model_references.data());
	const auto explode_reference = [&](auto&& self, std::uint16_t reference_index)
		-> void
	{
		const ObjectModelReference& reference =
			actor.model_references[reference_index];
		if (!reference.removed && reference.model_type == 1)
		{
			ExplodingMeshGeometry source;
			if (build_explosion_source_geometry(
					actor, reference_index, source))
			{
				const glm::mat4 model_transform =
					model_animation_render_transform(
						actor, reference_index, 1.0f);
				const glm::vec3 node_position = actor.position
					+ actor.orientation * glm::vec3(model_transform[3]);
				(void)explosion_billboard_create(
					world.death_effects,
					world,
					node_position,
					glm::vec3{0.0f},
					ExplosionBillboardType::separate_frames,
					reference.radius,
					150,
					true,
					0,
					false,
					false,
					simulation_tick);
				for (std::uint8_t fragment = 0; fragment < 20; ++fragment)
				{
					glm::vec3 local;
					local.z = (explosion_random(world) - 0.5f)
						* reference.radius * 0.3f;
					local.y = (explosion_random(world) - 0.5f)
						* reference.radius * 0.3f;
					local.x = (explosion_random(world) - 0.5f)
						* reference.radius * 0.3f;
					const glm::vec3 world_offset =
						actor.orientation * glm::mat3(model_transform) * local;
					if (fragment % 3 == 0)
					{
						particle_fragment_directional_burst(
							world,
							node_position + world_offset,
							glm::normalize(world_offset),
							0.3f,
							0.1f,
							0.0f,
							1,
							simulation_tick);
					}
				}
				std::vector<ExplodingMeshGeometry> outer =
					split_explosion_geometry(world, source, 2);
				for (std::uint8_t ordinal = 0;
					ordinal < outer.size();
					++ordinal)
				{
					if (outer[ordinal].faces.empty())
					{
						continue;
					}
					const std::uint8_t depth =
						static_cast<std::uint8_t>(ordinal % 3 + 1);
					std::vector<ExplodingMeshGeometry> nested =
						split_explosion_geometry(
							world, outer[ordinal], depth);
					for (ExplodingMeshGeometry& piece : nested)
					{
						if (piece.faces.empty())
						{
							continue;
						}
						const float angular_scale =
							static_cast<float>(depth) * 0.005f;
						glm::vec3 angular;
						angular.z =
							(explosion_random(world) - 0.5f) * angular_scale;
						angular.y =
							(explosion_random(world) - 0.5f) * angular_scale;
						angular.x =
							(explosion_random(world) - 0.5f) * angular_scale;
						const std::uint32_t expiration = simulation_tick
							+ 100u + world_rand15(world) % 20u;
						const glm::vec3 velocity = radial_piece_velocity(
							actor,
							piece,
							static_cast<float>(depth)
								* group_radius * (1.0f / 60.0f),
							0.01f);
						ExplodingMeshEffect* effect = allocate_exploding_mesh(
							world,
							static_cast<ExplodingMeshGeometry&&>(piece),
							actor,
							velocity,
							angular,
							expiration,
							false,
							ParticleEmitterStyle::ordinary_explosion,
							simulation_tick);
						if (effect == nullptr)
						{
							continue;
						}
						const std::uint32_t delay = expiration - simulation_tick;
						(void)explosion_billboard_create(
							world.death_effects,
							world,
							effect->position
								+ effect->velocity_per_tick
									* static_cast<float>(delay),
							glm::vec3{0.0f},
							ExplosionBillboardType::separate_frames,
							effect->geometry.radius,
							150,
							true,
							static_cast<std::int32_t>(delay),
							false,
							false,
							simulation_tick);
					}
				}
			}
		}
		for (std::uint16_t child = 0;
			child < actor.model_references.size();
			++child)
		{
			if (actor.model_references[child].parent_reference
				== static_cast<std::int16_t>(reference_index))
			{
				self(self, child);
			}
		}
	};
	explode_reference(explode_reference, root);
}

void explosion_billboards_service(
	DeathEffectsRuntime& runtime,
	World& world,
	std::uint32_t simulation_tick,
	std::uint32_t elapsed_ticks)
{
	for (ExplodingMeshEffect& effect : runtime.exploding_meshes)
	{
		effect.render_active = false;
		if (!effect.active)
		{
			continue;
		}
		if (effect.stage == 0 && effect.expiration_tick < simulation_tick)
		{
			(void)explosion_billboard_create(
				runtime,
				world,
				effect.position,
				glm::vec3{0.0f},
				ExplosionBillboardType::sheet,
				effect.geometry.radius * 1.2f,
				60,
				false,
				0,
				false,
				false,
				simulation_tick);
			if (effect.emitter_index < world.particles.emitters.size())
			{
				particle_emitter_release_automatic(
					world.particles.emitters[effect.emitter_index]);
				effect.emitter_index = UINT16_MAX;
			}
			effect.stage = 1;
			effect.expiration_tick = simulation_tick + 12u;
		}
		else if (effect.stage != 0
			&& effect.expiration_tick < simulation_tick)
		{
			effect = {};
			continue;
		}
		if (effect.emitter_index < world.particles.emitters.size())
		{
			ParticleEmitter& emitter =
				world.particles.emitters[effect.emitter_index];
			emitter.local_position = effect.position;
			emitter.local_basis = effect.orientation;
		}
		effect.position += effect.velocity_per_tick
			* static_cast<float>(elapsed_ticks);
		for (std::uint32_t tick = 0; tick < elapsed_ticks; ++tick)
		{
			effect.orientation *= effect.angular_step;
		}
		effect.render_active = true;
	}
	for (ExplosionBillboardEffect& explosion
		: runtime.explosion_billboards)
	{
		if (!explosion.active)
		{
			continue;
		}
		const std::int32_t age = static_cast<std::int32_t>(
			simulation_tick - explosion.start_tick)
			- explosion.delay_ticks;
		if (age < 0)
		{
			continue;
		}
		if (age >= explosion.duration_ticks)
		{
			explosion.active = false;
			continue;
		}
		explosion.position += explosion.velocity_per_tick
			* static_cast<float>(elapsed_ticks);
	}
	for (RockChunkEffect& chunk : runtime.rock_chunks)
	{
		chunk.render_active = false;
		if (!chunk.active)
		{
			continue;
		}
		if (simulation_tick > chunk.expiration_tick)
		{
			chunk.active = false;
			continue;
		}
		const float elapsed = static_cast<float>(elapsed_ticks);
		chunk.position += chunk.velocity_per_tick * elapsed;
		chunk.orientation = chunk.orientation
			* math::rotation_from_euler(
				chunk.angular_velocity_per_tick * elapsed);
		chunk.render_active = true;
	}
	for (DestructionLightEffect& light : runtime.destruction_lights)
	{
		if (!light.active)
		{
			continue;
		}
		light.remaining -= static_cast<float>(elapsed_ticks) * 0.0001f;
		if (light.remaining <= 0.0f)
		{
			light = {};
			continue;
		}
		light.intensity = 1.0f - explosion_random(world) * 0.5f;
	}
}

void death_effects_enqueue_hud_notification(
	DeathEffectsRuntime& runtime,
	const DeathHudNotification& notification)
{
	// FUN_0048ceb0 retains four timed messages and evicts the oldest on
	// the fifth. These semantic events are drained by the session in the
	// same gameplay update, so preserving the last four gives the same
	// final HUD queue for a multi-kill burst.
	if (runtime.hud_notification_count
		== kDeathHudNotificationCount)
	{
		for (std::size_t index = 1;
			index < runtime.hud_notification_count;
			++index)
		{
			runtime.hud_notifications[index - 1] =
				runtime.hud_notifications[index];
		}
		--runtime.hud_notification_count;
	}
	runtime.hud_notifications[
		runtime.hud_notification_count++] = notification;
}

bool death_effects_pop_hud_notification(
	DeathEffectsRuntime& runtime,
	DeathHudNotification& notification)
{
	if (runtime.hud_notification_count == 0)
	{
		return false;
	}
	notification = runtime.hud_notifications[0];
	for (std::size_t index = 1;
		index < runtime.hud_notification_count;
		++index)
	{
		runtime.hud_notifications[index - 1] =
			runtime.hud_notifications[index];
	}
	--runtime.hud_notification_count;
	runtime.hud_notifications[
		runtime.hud_notification_count] = {};
	return true;
}

void uber_explosion_start(
	World& world,
	ObjectHandle owner,
	const glm::vec3& position,
	const glm::mat3& orientation,
	float size,
	std::uint32_t duration_ticks,
	bool multiplayer,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick)
{
	UberExplosionEffect& effect =
		world.death_effects.uber_explosion;
	effect = {};
	effect.active = true;
	effect.position = position;
	effect.orientation = orientation;
	effect.size = size;
	effect.start_tick = simulation_tick;
	effect.duration_ticks = static_cast<std::uint16_t>(
		std::max<std::uint32_t>(1, duration_ticks));
	effect.owner_index = owner.index;
	effect.owner_generation = owner.generation;
	effect.multiplayer = multiplayer;
	// Uber_explosion_create, 0x00472bd2..0x00472d73, clones ring3 for
	// each open hemisphere. The compiled ring contains 128 vertices and
	// 234 triangles: its final rim vertex and final 18 colors are retained,
	// but there is deliberately no closing fan. The colored center uses the
	// separately generated 18-by-8 shield sphere and does close its pole.
	build_uber_sphere(effect.hemispheres[0], false);
	build_uber_sphere(effect.hemispheres[1], false);
	build_uber_sphere(effect.inner_shell, true);
	effect.inner_shell.texture = TransitionTexture::particle_flare;
	// Uber_explosion_create clones the selector-one shield sphere material
	// for both hemispheres (LANCER.EXE 0x00472ab0): ONE/ONE, not ordinary
	// source-alpha transparency.
	effect.hemispheres[0].blend = TransitionBlend::additive;
	effect.hemispheres[1].blend = TransitionBlend::additive;
	effect.hemispheres[0].position = position;
	effect.hemispheres[0].orientation = orientation;
	effect.hemispheres[1].position = position;
	effect.hemispheres[1].orientation =
		orientation
			* math::rotation_from_euler(
				{0.0f, kPi, 0.0f});
	effect.inner_shell.position = position;
	effect.inner_shell.orientation = orientation;
	effect.inner_shell.active = false;
	set_uber_cap(effect.hemispheres[0], 0.0f, 0.0f);
	set_uber_cap(effect.hemispheres[1], 0.0f, 0.0f);
	// The constructor clears the final 19 colors. The updater's literal
	// boundary differs by one and clears only the final 18 thereafter.
	for (TransitionMesh& hemisphere : effect.hemispheres)
	{
		for (std::size_t vertex = hemisphere.vertices.size() - 19;
			vertex < hemisphere.vertices.size();
			++vertex)
		{
			hemisphere.vertices[vertex].color = 0;
		}
	}

	const std::uint16_t excluded_index =
		multiplayer ? owner.index : world.player.index;
	const std::uint16_t candidate_limit = multiplayer
		? 8
		: kMaxGameObjects;
	for (std::uint16_t index = 0;
		index < candidate_limit
			&& effect.candidate_count
				< effect.candidates.size();
		++index)
	{
		const WorldObject& candidate = world.objects[index];
		if (!candidate.active
			|| index == excluded_index
			|| (candidate.runtime_flags & kObjectFlagDisabled) != 0
			|| candidate.type >= 256u
			// ObjectTypeStats+0x2a is the compiled allegiance word.
			|| ship_stats.records[
				candidate.type].object.allegiance_class == 2
			|| candidate.ai.command_count == 0
			|| candidate.type == 0x48u
			|| candidate.type == 0x6du
			|| candidate.type == 0x6eu
			|| candidate.type == 0xa8u
			|| glm::distance(candidate.position, position)
				> size * 5.0f)
		{
			continue;
		}
		effect.candidates[effect.candidate_count++] = {
			index,
			candidate.generation,
			false,
		};
	}

	(void)shockwave_create(
		world,
		position,
		orientation,
		{},
		3,
		size * 16.0f,
		static_cast<float>(
			static_cast<std::int32_t>(
				static_cast<float>(duration_ticks) * 0.25f)),
		0,
		owner.index,
		simulation_tick);
	(void)shockwave_create(
		world,
		position,
		orientation,
		{},
		3,
		size * 6.0f,
		static_cast<float>(
			static_cast<std::int32_t>(
				static_cast<float>(duration_ticks) * 0.5f)),
		0,
		owner.index,
		simulation_tick);
	world_queue_sound_object(world, owner, 66, 4);
	// Uber_explosion_create, LANCER.EXE 0x00472ab0, writes the shared
	// whiteout percentage consumed by mission_update_whiteout_overlay.
	world.player_exhaust_exposure_percent = 100;
	diagnostics::mission_log(
		"uber explosion start owner=%u size=%.0f duration=%u tick=%u",
		static_cast<unsigned>(owner.index),
		size,
		duration_ticks,
		simulation_tick);
}

void uber_explosion_service(
	World& world,
	mission::Runtime& mission,
	std::uint32_t simulation_tick)
{
	UberExplosionEffect& effect =
		world.death_effects.uber_explosion;
	if (!effect.active)
	{
		return;
	}
	if (simulation_tick
		> effect.start_tick + effect.duration_ticks)
	{
		for (std::uint8_t ordinal = 0;
			ordinal < effect.candidate_count;
			++ordinal)
		{
			const UberExplosionCandidate& retained =
				effect.candidates[ordinal];
			if (!retained.reached)
			{
				continue;
			}
			WorldObject* candidate = world_resolve(
				world,
				{retained.object_index, retained.generation});
			if (candidate == nullptr
				|| (candidate->ai.command_count != 0
					&& candidate->ai.commands[0].id == 11))
			{
				continue;
			}
			candidate->angular_x = 0.0f;
			candidate->angular_y = 0.0f;
			candidate->angular_z = 0.0f;
			if (effect.multiplayer
				&& retained.object_index < 8)
			{
				candidate->last_attacker_index =
					static_cast<std::uint16_t>(-2);
				// UberExplode_update, LANCER.EXE
				// 0x004732d9..0x00473377. Only network player
				// victims are eligible, and only the local Uber owner
				// receives the score adjustment.
				const bool local_owner =
					effect.owner_index
						== world.player.index;
				death_effects_enqueue_hud_notification(
					world.death_effects,
					{
						DeathHudNotificationKind::uber_kill,
						static_cast<std::uint8_t>(
							retained.object_index),
						static_cast<std::uint8_t>(
							effect.owner_index),
						local_owner,
					});
				if (local_owner)
				{
					(void)mission::runtime_add_player_score(
						mission,
						world,
						world.player.index,
						1,
						true);
				}
			}
			// UberExplode_update, LANCER.EXE
			// 0x00473383..0x00473394, invokes the complete death selector
			// with cause zero and forced player explosion. This preserves
			// the active command's end callback and every terminal-death
			// side effect owned by AI_schedule_death_command.
			(void)ai::schedule_death_command(
				*candidate, world, mission, 0, true);
		}
		world_queue_sound_explicit(
			world,
			effect.position,
			effect.orientation[2],
			{},
			65,
			2);
		diagnostics::mission_log(
			"uber explosion complete owner=%u tick=%u",
			static_cast<unsigned>(effect.owner_index),
			simulation_tick);
		const WorldObject* owner = world_resolve(
			world,
			{effect.owner_index, effect.owner_generation});
		if (owner != nullptr)
		{
			// UberExplode_update 0x004733a3..0x004733af publishes
			// ExplosionShip with the exploding ship as both the event
			// source and its sole object argument.
			mission::events_emit_explosion_ship(
				mission,
				owner->mission_index,
				owner->mission_index);
		}
		effect = {};
		return;
	}
	const float phase = static_cast<float>(
		simulation_tick - effect.start_tick)
		/ static_cast<float>(effect.duration_ticks);
	if (phase < 0.5f)
	{
		const float scale =
			phase <= 0.05f ? phase * 20.0f
				: phase <= 0.3f ? 1.0f
				: 1.0f - (phase - 0.3f) * 5.0f;
		const float cap = std::min(1.0f, phase / 0.3f);
		for (TransitionMesh& hemisphere : effect.hemispheres)
		{
			hemisphere.active = true;
			// Uber_explosion_update 0x004734d4..0x00473536 keeps the
			// object radius fixed; only the object/vertex brightness fades.
			hemisphere.scale = effect.size * 1.3f;
			set_uber_cap(
				hemisphere, cap, scale * 0.3f);
		}
	}
	else
	{
		effect.hemispheres[0].active = false;
		effect.hemispheres[1].active = false;
	}
	if (phase > 0.3f)
	{
		const float expansion =
			std::clamp((phase - 0.3f) / 0.7f, 0.0f, 1.0f);
		// UberExplode_update discards two draws immediately before
		// regenerating the inner sphere's per-vertex colors.
		(void)world_rand15(world);
		(void)world_rand15(world);
		effect.inner_radius =
			std::max(0.001f, effect.size * 5.0f * expansion);
		effect.inner_shell.active = true;
		effect.inner_shell.scale = effect.inner_radius;
		set_uber_inner_sphere(effect.inner_shell, world);
		// UberExplode_update 0x00473640..0x00473656 replaces the common
		// camera disturbance with twice the live expansion phase.
		world.player_camera_disturbance = expansion * 2.0f;

		for (std::uint8_t ordinal = 0;
			ordinal < effect.candidate_count;
			++ordinal)
		{
			UberExplosionCandidate& retained =
				effect.candidates[ordinal];
			if (retained.reached)
			{
				continue;
			}
			WorldObject* candidate = world_resolve(
				world,
				{retained.object_index, retained.generation});
			if (candidate == nullptr
				|| glm::distance(
					candidate->position, effect.position)
					> effect.inner_radius)
			{
				continue;
			}
			if (candidate->ai.command_count != 0
				&& candidate->ai.commands[0].id == 11)
			{
				continue;
			}
			// Build the same randomized object-relative explosion frame as
			// 0x004736e8..0x00473845. Argument evaluation consumes Z, Y, X.
			const float frame_rotation_z =
				static_cast<float>(world_rand15(world))
					* kRandScale * 2.0f * kPi;
			const float frame_rotation_y =
				static_cast<float>(world_rand15(world))
					* kRandScale * 2.0f * kPi;
			const float frame_rotation_x =
				static_cast<float>(world_rand15(world))
					* kRandScale * 2.0f * kPi;
			const glm::mat3 prior_orientation = candidate->orientation;
			const glm::mat3 explosion_basis =
				prior_orientation
					* math::rotation_from_euler({
						frame_rotation_x,
						frame_rotation_y,
						frame_rotation_z});
			const glm::vec3 explosion_center =
				candidate->position
					+ explosion_basis[2]
						* (candidate->radius * 0.6f);
			glm::vec3 outward = candidate->position - effect.position;
			const float outward_length = glm::length(outward);
			if (outward_length > 0.0f)
			{
				outward /= outward_length;
			}
			else
			{
				outward = glm::vec3{0.0f};
			}
			const glm::vec3 impulse =
				outward * (candidate->physics_mass * 2000.0f);
			candidate->accumulated_linear_impulse += impulse;
			candidate->accumulated_angular_impulse += glm::cross(
				impulse, explosion_center - candidate->previous_position);
			++candidate->accumulated_impulse_count;

			// Uber_explosion_update writes a fresh, small absolute Euler
			// orientation to GameObject+0x56c before emitting the five hits.
			// Argument evaluation consumes Z, Y, X.
			const float object_rotation_z =
				(explosion_random(world) - 0.5f) * 0.3f;
			const float object_rotation_y =
				(explosion_random(world) - 0.5f) * 0.3f;
			const float object_rotation_x =
				(explosion_random(world) - 0.5f) * 0.3f;
			candidate->orientation = math::rotation_from_euler({
				object_rotation_x,
				object_rotation_y,
				object_rotation_z});
			for (std::uint32_t delay = 0;
				delay < 150;
				delay += 30)
			{
				const float offset_z =
					(static_cast<float>(world_rand15(world))
							* kRandScale
						- 0.5f)
					* candidate->radius;
				const float offset_y =
					(static_cast<float>(world_rand15(world))
							* kRandScale
						- 0.5f)
					* candidate->radius;
				const float offset_x =
					(static_cast<float>(world_rand15(world))
							* kRandScale
						- 0.5f)
					* candidate->radius;
				(void)transition_explosion_create_delayed(
					world,
					candidate->position
						+ prior_orientation
							* glm::vec3{
								offset_x,
								offset_y,
								offset_z},
					candidate->radius * 0.5f,
					150,
					simulation_tick,
					delay);
			}
			ai::command_clear(world, *candidate);
			(void)ai::command_push(world,
				*candidate,
				0,
				ai::TargetKind::none,
				UINT16_MAX);
			// The expanding shell clears the victim and installs Do
			// Nothing at 0x0047390b..0x00473921. Explode is installed
			// only by the completion pass above.
			retained.reached = true;
		}
	}
	else
	{
		effect.inner_shell.active = false;
	}
	if (phase > 0.5f)
	{
		// UberExplode_update, LANCER.EXE 0x00473aca..0x00473b42,
		// constructs these fragments in a zero-roll frame from the active
		// scene camera to the explosion. Their travel direction is the
		// normalized vector from the explosion back toward that camera.
		const glm::vec3 camera_position =
			mission.particle_camera_position;
		const glm::mat3 camera_explosion_frame =
			respawn_projector_orientation(
				effect.position - camera_position);
		glm::vec3 camera_direction =
			camera_position - effect.position;
		const float camera_distance = glm::length(camera_direction);
		if (camera_distance > 0.0f)
		{
			camera_direction /= camera_distance;
		}
		std::uint16_t emitted = 0;
		while (emitted < static_cast<std::uint16_t>(
			12 - static_cast<std::int32_t>(
				static_cast<float>(world_rand15(world))
					* kRandScale * -13.0f)))
		{
			// MSVC evaluates the Y argument before X.
			const float y =
				(static_cast<float>(world_rand15(world))
						* kRandScale
					- 0.5f)
				* 4000.0f;
			const float x =
				(static_cast<float>(world_rand15(world))
						* kRandScale
					- 0.5f)
				* 4000.0f;
			particle_uber_debris_spawn(
				world,
				camera_position
					+ camera_explosion_frame
						* glm::vec3{x, y, 10000.0f},
				camera_direction,
				simulation_tick);
			++emitted;
		}
	}
	if (phase > 0.95f)
	{
		world.player_exhaust_exposure_percent =
			static_cast<std::int32_t>(
				std::lrint((phase - 0.95f) * 2000.0f));
	}
}

void death_effects_release_owner(
	DeathEffectsRuntime& runtime,
	ObjectHandle owner)
{
	for (TractorEffect& effect : runtime.tractor)
	{
		if (effect.active
			&& ((effect.owner_index == owner.index
					&& effect.owner_generation == owner.generation)
				|| (effect.target_index == owner.index
					&& effect.target_generation == owner.generation)))
		{
			effect = {};
		}
	}
	for (RipperGrabEffect& effect : runtime.ripper_grab)
	{
		if (effect.active
			&& ((effect.owner_index == owner.index
					&& effect.owner_generation == owner.generation)
				|| (effect.target_index == owner.index
					&& effect.target_generation == owner.generation)))
		{
			effect = {};
		}
	}
	for (RespawnEffect& effect : runtime.respawn)
	{
		if (effect.active
			&& effect.owner_index == owner.index
			&& effect.owner_generation == owner.generation)
		{
			effect = {};
		}
	}
	for (DestructionLightEffect& light : runtime.destruction_lights)
	{
		if (light.active
			&& light.owner_index == owner.index
			&& light.owner_generation == owner.generation)
		{
			light = {};
		}
	}
	if (owner.index < runtime.powercores.size())
	{
		PowercoreEffect& effect = runtime.powercores[owner.index];
		if (effect.active
			&& effect.owner_generation == owner.generation)
		{
			effect = {};
		}
	}
	if (runtime.camera_mode27_anchor_owner == owner.index
		&& runtime.camera_mode27_anchor_generation == owner.generation)
	{
		runtime.camera_mode27_anchor_valid = false;
		runtime.camera_mode27_anchor_owner = UINT16_MAX;
		runtime.camera_mode27_anchor_generation = 0;
	}
}

std::int16_t tractor_effect_allocate(
	DeathEffectsRuntime& runtime,
	ObjectHandle owner,
	ObjectHandle target)
{
	for (std::size_t index = 0; index < runtime.tractor.size(); ++index)
	{
		TractorEffect& effect = runtime.tractor[index];
		if (effect.active)
		{
			continue;
		}
		effect = {};
		effect.active = true;
		effect.owner_index = owner.index;
		effect.owner_generation = owner.generation;
		effect.target_index = target.index;
		effect.target_generation = target.generation;
		build_tractor_beam(effect.beams[0]);
		build_tractor_beam(effect.beams[1]);
		build_shield(effect.shield);
		effect.light.texture = TransitionTexture::warp_secondary;
		return static_cast<std::int16_t>(index);
	}
	if (!runtime.tractor_saturation_reported)
	{
		diagnostics::mission_log(
			"tractor effect pool exhausted capacity=%u",
			static_cast<unsigned>(runtime.tractor.size()));
		runtime.tractor_saturation_reported = true;
	}
	return -1;
}

void tractor_effect_release(
	DeathEffectsRuntime& runtime,
	std::int16_t slot)
{
	if (slot >= 0
		&& static_cast<std::size_t>(slot) < runtime.tractor.size())
	{
		runtime.tractor[slot] = {};
	}
}

void tractor_effect_set_submitted(
	DeathEffectsRuntime& runtime,
	std::int16_t slot,
	bool submitted)
{
	if (slot < 0
		|| static_cast<std::size_t>(slot) >= runtime.tractor.size())
	{
		return;
	}
	TractorEffect& effect = runtime.tractor[slot];
	effect.submitted = submitted;
	for (TransitionMesh& beam : effect.beams)
	{
		beam.active = submitted;
	}
	effect.shield.active = submitted;
	effect.light.active = submitted;
}

void tractor_effect_update(
	DeathEffectsRuntime& runtime,
	std::int16_t slot,
	const glm::vec3 origins[2],
	const WorldObject& target,
	float beam_a_alpha,
	float beam_b_alpha,
	float shield_intensity,
	float shield_phase,
	float light_range)
{
	if (slot < 0
		|| static_cast<std::size_t>(slot) >= runtime.tractor.size())
	{
		return;
	}
	TractorEffect& effect = runtime.tractor[slot];
	if (!effect.active)
	{
		return;
	}
	set_beam_length(effect.beams[0], origins[0], target.position);
	set_beam_length(effect.beams[1], origins[1], target.position);
	set_beam_alpha(effect.beams[0], beam_a_alpha);
	set_beam_alpha(effect.beams[1], beam_b_alpha);
	effect.shield.position = target.position;
	effect.shield.orientation =
		target.orientation * glm::mat3{target.radius * 1.5f};
	for (std::size_t index = 0;
		index < effect.shield.vertices.size();
		++index)
	{
		const float green =
			(std::sin(0.3f * static_cast<float>(index)
					+ 3.0f * shield_phase)
				+ 1.0f)
			* 0.05f * std::clamp(shield_intensity, 0.0f, 1.0f);
		effect.shield.vertices[index].color =
			(static_cast<std::uint32_t>(alpha_byte(green)) << 24)
			| 0x0000ff00u;
	}
	effect.light.position = target.position;
	effect.light.half_extent = {
		std::max(0.0f, light_range) * 0.05f,
		std::max(0.0f, light_range) * 0.05f,
	};
	effect.light.color = green_color(
		std::clamp(light_range / 10000.0f, 0.0f, 1.0f));
	tractor_effect_set_submitted(runtime, slot, true);
}

std::int16_t ripper_grab_effect_allocate(
	DeathEffectsRuntime& runtime,
	ObjectHandle owner,
	ObjectHandle target)
{
	const std::uint8_t slot = runtime.ripper_grab_cursor;
	runtime.ripper_grab_cursor = static_cast<std::uint8_t>(
		(runtime.ripper_grab_cursor + 1)
			% runtime.ripper_grab.size());
	RipperGrabEffect& effect = runtime.ripper_grab[slot];
	effect = {};
	effect.active = true;
	effect.owner_index = owner.index;
	effect.owner_generation = owner.generation;
	effect.target_index = target.index;
	effect.target_generation = target.generation;
	for (TransitionMesh& beam : effect.beams)
	{
		build_tractor_beam(beam);
	}
	return static_cast<std::int16_t>(slot);
}

void ripper_grab_effect_release(
	DeathEffectsRuntime& runtime,
	std::int16_t slot)
{
	if (slot >= 0
		&& static_cast<std::size_t>(slot)
			< runtime.ripper_grab.size())
	{
		runtime.ripper_grab[slot] = {};
	}
}

void ripper_grab_effect_update(
	DeathEffectsRuntime& runtime,
	std::int16_t slot,
	const glm::vec3 origins[4],
	const glm::vec3 targets[4],
	float alpha)
{
	if (slot < 0
		|| static_cast<std::size_t>(slot)
			>= runtime.ripper_grab.size())
	{
		return;
	}
	RipperGrabEffect& effect = runtime.ripper_grab[slot];
	if (!effect.active)
	{
		return;
	}
	for (std::size_t index = 0; index < effect.beams.size(); ++index)
	{
		set_beam_length(
			effect.beams[index], origins[index], targets[index]);
		set_beam_alpha(effect.beams[index], alpha);
		effect.beams[index].active = true;
	}
	effect.submitted = true;
}

std::int16_t respawn_effect_allocate(
	DeathEffectsRuntime& runtime,
	ObjectHandle owner,
	WorldObject& actor)
{
	const std::uint8_t slot = runtime.respawn_cursor;
	runtime.respawn_cursor = static_cast<std::uint8_t>(
		(runtime.respawn_cursor + 1) % runtime.respawn.size());
	RespawnEffect& effect = runtime.respawn[slot];
	effect = {};
	effect.active = true;
	effect.owner_index = owner.index;
	effect.owner_generation = owner.generation;
	effect.model_clip_z = actor.bounds_min.z;
	// Respawn_Mesh_create, LANCER.EXE 0x0044f000, halves both dimensions
	// supplied from GameObject+0x59c before authoring the quad corners.
	build_respawn_quad(effect.portal, actor.radius * 0.5f);
	build_respawn_projection(effect.projection);
	for (ObjectModelReference& model : actor.model_references)
	{
		model.respawn_render_override_slot =
			static_cast<std::int16_t>(slot);
	}
	return slot;
}

bool respawn_effect_active(
	const DeathEffectsRuntime& runtime,
	std::int16_t slot)
{
	return slot >= 0
		&& static_cast<std::size_t>(slot) < runtime.respawn.size()
		&& runtime.respawn[slot].active;
}

void respawn_effect_release(
	DeathEffectsRuntime& runtime,
	std::int16_t slot)
{
	if (slot >= 0
		&& static_cast<std::size_t>(slot) < runtime.respawn.size())
	{
		runtime.respawn[slot] = {};
	}
}

void respawn_effect_clear_model_override(WorldObject& actor)
{
	// ModelTree_clear_render_override, LANCER.EXE 0x004adf40. The retail
	// tree is recursive; model_references is its stable preorder flattening.
	for (ObjectModelReference& model : actor.model_references)
	{
		model.respawn_render_override_slot = -1;
	}
}

void respawn_effect_update(
	DeathEffectsRuntime& runtime,
	std::int16_t slot,
	const WorldObject& actor,
	float remaining_fraction)
{
	if (slot < 0
		|| static_cast<std::size_t>(slot) >= runtime.respawn.size())
	{
		return;
	}
	RespawnEffect& effect = runtime.respawn[slot];
	if (!effect.active)
	{
		return;
	}
	const float t = remaining_fraction;
	// AI_DeathmatchRespawnEffect_update 0x004b0f60 drives every RGB
	// channel of the cloned Respawn_Mesh with sin(pi * remaining).
	const float brightness = std::sin(kPi * t);
	const std::uint32_t color =
		0xff000000u
		| static_cast<std::uint32_t>(alpha_byte(brightness))
			* 0x00010101u;
	for (TransitionVertex& vertex : effect.portal.vertices)
	{
		vertex.color = color;
	}
	for (TransitionVertex& vertex : effect.projection.vertices)
	{
		// The WProject mesh is restored to (1, 0, 0, 1) on every update
		// at 0x004b10f7..0x004b113e.
		vertex.color = 0xff0000ffu;
	}
	// FUN_004268c0 at 0x004b0fdd cosine-interpolates the shared modifier
	// plane from GameObject+0x5a8 (minimum Z) to +0x5b4 (maximum Z).
	// `remaining_fraction` runs from one to zero, so the ship is revealed
	// from its maximum-Z edge back through its complete model tree.
	const float clip_fraction =
		(1.0f - std::cos(kPi * t)) * 0.5f;
	effect.model_clip_z =
		actor.bounds_min.z
			+ (actor.bounds_max.z - actor.bounds_min.z)
				* clip_fraction;
	const glm::vec3 portal_local_position{
		0.0f, 0.0f, effect.model_clip_z};
	effect.portal.position =
		actor.position + actor.orientation * portal_local_position;
	effect.portal.orientation =
		actor.orientation
			* math::rotation_from_euler({
				0.0f,
				0.0f,
				0.5f * kPi * clip_fraction,
			});
	effect.portal.active = true;

	// WProject_Mesh remains rooted at the actor's maximum-Z bound. Its target
	// descends with the reveal plane while circling four times; the sine pulse
	// expands and contracts that circle. Retail stretches the three authored
	// ribbons to the resulting target distance on every update.
	// Respawn_Mesh geometry +0x20 is its maximum-X bound, not its sphere
	// radius. The authored quad's maximum X is half GameObject+0x59c.
	const float portal_max_x =
		effect.portal.vertices.empty()
			? 0.0f
			: std::abs(effect.portal.vertices.front().position.x);
	const float projector_radius = brightness * portal_max_x;
	const float phase = 4.0f * kPi * t;
	const glm::vec3 projection_local_position{
		0.0f, 0.0f, actor.bounds_max.z};
	const glm::vec3 projection_local_target{
		std::sin(phase) * projector_radius,
		std::cos(phase) * projector_radius,
		effect.model_clip_z,
	};
	const glm::vec3 projection_direction =
		projection_local_target - projection_local_position;
	const float projection_length = glm::length(projection_direction);
	for (std::uint16_t side = 0; side < 3; ++side)
	{
		const std::size_t base = 4u + static_cast<std::size_t>(side) * 4u;
		effect.projection.vertices[base + 1].position.z =
			projection_length;
		effect.projection.vertices[base + 2].position.z =
			projection_length;
	}
	effect.projection.position =
		actor.position + actor.orientation * projection_local_position;
	effect.projection.orientation =
		actor.orientation
			* respawn_projector_orientation(projection_direction);
	effect.projection.active = true;
	effect.submitted = true;
}
}
