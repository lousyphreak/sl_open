#include "assets/gameplay_model.hpp"

#include "assets/sro_chunks.hpp"
#include "assets/texture_cache.hpp"
#include "io/endian.hpp"
#include "io/vfs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace sl_open::assets
{
namespace
{
struct SourceLod
{
	const std::uint8_t* descriptor{};
	std::uint16_t descriptor_stride{};
	const SroChunk* vertices{};
	const SroChunk* polygons{};
	const SroChunk* materials{};
};

struct SourceNode
{
	const std::uint8_t* record{};
	std::uint16_t stride{};
	std::vector<SourceLod> lods;
	const SroChunk* collision{};
	const SroChunk* portals{};
	std::vector<const SroChunk*> collision_polygons;
	std::vector<GameplaySequence> sequences;
	std::vector<GameplayPointGroup> point_groups;
};

float read_float(const std::uint8_t* bytes)
{
	const std::uint32_t bits = io::read_le32(bytes);
	float value{};
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

glm::vec3 read_vec3(const std::uint8_t* bytes)
{
	return {
		read_float(bytes),
		read_float(bytes + 4),
		read_float(bytes + 8),
	};
}

std::uint32_t add_material(
	GameplayModel& model,
	const TextureCache& texture_cache,
	const char* basename,
	bool require_alternate)
{
	for (std::uint32_t index = 0; index < model.materials.size(); ++index)
	{
		if (std::strcmp(model.materials[index].basename, basename) == 0)
		{
			GameplayMaterial& material = model.materials[index];
			if (require_alternate && !material.has_alternate)
			{
				char alternate[66]{};
				const int written = std::snprintf(
					alternate, sizeof(alternate), "l%s", basename);
				if (written <= 0
					|| static_cast<std::size_t>(written)
						>= sizeof(alternate)
					|| !texture_cache_decode(
						texture_cache,
						alternate,
						material.alternate_image))
				{
					return UINT32_MAX;
				}
				material.has_alternate = true;
			}
			return index;
		}
	}
	GameplayMaterial material;
	std::snprintf(
		material.basename,
		sizeof(material.basename),
		"%s",
		basename);
	if (!texture_cache_decode(texture_cache, basename, material.image))
	{
		return UINT32_MAX;
	}
	if (require_alternate)
	{
		char alternate[66]{};
		const int written = std::snprintf(
			alternate, sizeof(alternate), "l%s", basename);
		if (written <= 0
			|| static_cast<std::size_t>(written) >= sizeof(alternate)
			|| !texture_cache_decode(
				texture_cache,
				alternate,
				material.alternate_image))
		{
			return UINT32_MAX;
		}
		material.has_alternate = true;
	}
	model.materials.push_back(static_cast<GameplayMaterial&&>(material));
	return static_cast<std::uint32_t>(model.materials.size() - 1);
}

std::uint32_t pack_rgb(const glm::vec3& color)
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

bool material_mode_uses_texture(std::uint8_t mode)
{
	// SRO_build_lod_mesh (LANCER.EXE 0x004a3040) enables material byte +2
	// only for these modes. Modes 0..2 and the default/9 path leave the
	// texture stage disabled even though the source polygon still contains
	// a material index.
	return mode == 3 || mode == 4 || mode == 5
		|| mode == 6 || mode == 7 || mode == 8 || mode == 10;
}

bool build_lod(
	const SourceLod& source,
	const SourceLod* next,
	std::uint32_t node_flags,
	const TextureCache& texture_cache,
	GameplayModel& model,
	GameplayLod& lod,
	bool planet)
{
	if (source.descriptor == nullptr
		|| source.descriptor_stride < 4
		|| source.vertices == nullptr
		|| source.vertices->stride < 0x1c
		|| source.polygons == nullptr
		|| source.polygons->stride < 0x48
		|| source.materials == nullptr
		|| source.materials->stride < 0x40)
	{
		return false;
	}
	lod.threshold = read_float(source.descriptor);

	std::vector<std::uint32_t> material_map(
		source.materials->count, UINT32_MAX);

	std::vector<GameplayVertex> output_vertices;
	std::vector<std::uint32_t> output_indices;
	std::vector<glm::vec3> output_normals;
	std::vector<glm::vec3> output_secondary_normals;
	constexpr std::uint32_t kFaceSuppressed = 0x00000001u;
	constexpr std::uint32_t kFaceDoubleSided = 0x00000002u;
	std::uint16_t current_light_channel = 0;
	std::uint16_t next_light_channel = 0;
	auto source_normal = [&](std::uint32_t polygon_index) {
		const std::uint8_t* polygon =
			source.polygons->data
			+ polygon_index * source.polygons->stride;
		return glm::vec3{
			read_float(polygon + 0x30),
			read_float(polygon + 0x34),
			read_float(polygon + 0x38),
		};
	};
	auto can_merge_fan = [&](std::uint32_t polygon_index) {
		if (source.polygons->stride < 0x50)
		{
			return false;
		}
		const std::uint8_t* polygon =
			source.polygons->data
			+ polygon_index * source.polygons->stride;
		if (io::read_le32(polygon + 0x48) != 1
			|| (polygon_index != 0
				&& io::read_le32(
					source.polygons->data
					+ (polygon_index - 1) * source.polygons->stride
					+ 0x4c) != 0))
		{
			return false;
		}
		const std::uint32_t continuation_count =
			io::read_le32(polygon + 0x4c);
		if (continuation_count
			> source.polygons->count - polygon_index - 1)
		{
			return false;
		}
		const glm::vec3 normal = source_normal(polygon_index);
		for (std::uint32_t continuation = 1;
			continuation <= continuation_count;
			++continuation)
		{
			if (glm::dot(
					normal,
					source_normal(polygon_index + continuation))
				< 0.9990000128746033f)
			{
				return false;
			}
		}
		return true;
	};
	std::vector<glm::vec3> planet_normals;
	if (planet)
	{
		// GameObject_create_runtime, 0x00467c43..0x00467cd6: center
		// each LOD using its unique source points, then rebuild face and
		// vertex normals before expanding material/UV seams for the GPU.
		planet_normals.resize(source.vertices->count, glm::vec3{0.0f});
		for (std::uint32_t vertex = 0; vertex < source.vertices->count; ++vertex)
		{
			const glm::vec3 point = read_vec3(
				source.vertices->data + vertex * source.vertices->stride);
			lod.origin_offset += point;
			lod.original_radius = std::max(lod.original_radius, glm::length(point));
		}
		lod.origin_offset *= 1.0f / static_cast<float>(source.vertices->count);
		for (std::uint32_t polygon_index = 0;
			polygon_index < source.polygons->count;)
		{
			const std::uint8_t* polygon = source.polygons->data
				+ polygon_index * source.polygons->stride;
			const bool fan = can_merge_fan(polygon_index);
			const std::uint32_t continuations = fan
				? io::read_le32(polygon + 0x4c) : 0;
			glm::vec3 points[3];
			for (std::uint32_t corner = 0; corner < 3; ++corner)
			{
				points[corner] = read_vec3(source.vertices->data
					+ io::read_le32(polygon + 0x0c + corner * 4)
						* source.vertices->stride) - lod.origin_offset;
			}
			glm::vec3 normal = glm::cross(
				points[1] - points[0], points[2] - points[0]);
			if (!fan && io::read_le32(polygon + 0x48) == 3)
			{
				normal = -normal;
			}
			normal = glm::normalize(normal);
			for (std::uint32_t corner = 0; corner < 3; ++corner)
			{
				planet_normals[io::read_le32(polygon + 0x0c + corner * 4)] += normal;
			}
			for (std::uint32_t continuation = 1;
				continuation <= continuations; ++continuation)
			{
				const std::uint8_t* next_polygon = polygon
					+ continuation * source.polygons->stride;
				planet_normals[io::read_le32(next_polygon + 0x14)] += normal;
			}
			polygon_index += continuations + 1;
		}
		for (glm::vec3& normal : planet_normals)
		{
			normal = glm::normalize(normal);
		}
	}
	auto append_corner = [&](
		const std::uint8_t* polygon,
		std::uint32_t corner) {
		const std::uint32_t source_vertex =
			io::read_le32(polygon + 0x0c + corner * 4);
		if (source_vertex >= source.vertices->count)
		{
			std::fprintf(
				stderr,
				"Gameplay LOD source vertex out of range: "
				"vertex=%u count=%u\n",
				source_vertex,
				source.vertices->count);
			return false;
		}
		const std::uint8_t* vertex =
			source.vertices->data
			+ source_vertex * source.vertices->stride;
		const glm::vec3 normal = planet ? planet_normals[source_vertex] : glm::vec3{
			read_float(vertex + 0x0c),
			read_float(vertex + 0x10),
			read_float(vertex + 0x14),
		};
		const glm::vec3 position = glm::vec3{
			read_float(vertex),
			read_float(vertex + 4),
			read_float(vertex + 8),
		} - lod.origin_offset;
		glm::vec3 secondary_normal = normal;
		if ((node_flags & 0x0010u) != 0
			&& next != nullptr
			&& next->vertices != nullptr
			&& source.vertices->stride >= 0x20)
		{
			const std::uint32_t next_vertex =
				io::read_le32(vertex + 0x1c);
			// SRO tag-4 uses -1 when this vertex has no correspondence in
			// the following LOD. Retail leaves the current normal in place
			// for that vertex during the final-quarter LOD lighting blend.
			if (next_vertex == UINT32_MAX)
			{
				// secondary_normal was initialized from the current LOD.
			}
			else if (next_vertex >= next->vertices->count
				|| next->vertices->stride < 0x1c)
			{
				std::fprintf(
					stderr,
					"Gameplay LOD secondary vertex out of range: "
					"vertex=%u count=%u stride=%u\n",
					next_vertex,
					next->vertices->count,
					next->vertices->stride);
				return false;
			}
			else
			{
				const std::uint8_t* mapped =
					next->vertices->data
						+ next_vertex * next->vertices->stride;
				secondary_normal = {
					read_float(mapped + 0x0c),
					read_float(mapped + 0x10),
					read_float(mapped + 0x14),
				};
			}
		}
		output_vertices.push_back({
			position.x,
			position.y,
			position.z,
			0xff000000u,
			read_float(polygon + 0x18 + corner * 4),
			read_float(polygon + 0x24 + corner * 4),
		});
		output_normals.push_back(normal);
		output_secondary_normals.push_back(secondary_normal);
		// SR_mesh_calculate_bounds (LANCER.EXE 0x004c3f10), called after
		// SRO_build_lod_mesh at 0x004a3c5c, derives these values from the
		// completed mesh vertex stream rather than the parent SRO node.
		if (output_vertices.size() == 1)
		{
			lod.bounds_min = position;
			lod.bounds_max = position;
		}
		else
		{
			lod.bounds_min = glm::min(lod.bounds_min, position);
			lod.bounds_max = glm::max(lod.bounds_max, position);
		}
		lod.radius = std::max(lod.radius, glm::length(position));
		output_indices.push_back(
			static_cast<std::uint32_t>(output_indices.size()));
		return true;
	};
	auto apply_retail_face_winding = [&](std::uint16_t face_kind) {
		// SRO_build_lod_mesh (LANCER.EXE 0x004a3040) retains an ordinary
		// source face's kind, but assigns kind zero to a merged fan.
		// srd3d's mesh geometry builders triangulate kinds zero and one as
		// fans, while kinds two and three are alternating strips with kind
		// three reversing the first triangle. A standalone kind-three face
		// therefore reaches D3D with vertices 0,2,1; the other kinds retain
		// vertices 0,1,2.
		//
		// Our imported mesh expands every face into an independent triangle
		// list and therefore has no face-kind metadata at draw time. Apply the
		// kind-three reversal here so GPU backface culling sees the same
		// winding as retail.
		if (face_kind == 3)
		{
			const std::size_t count = output_indices.size();
			std::swap(output_indices[count - 2], output_indices[count - 1]);
		}
	};

	// SRO_build_cloak_lod_mesh (LANCER.EXE 0x004a3cb0) builds a second,
	// single-material mesh directly from the authored vertex and polygon
	// streams for cloak-capable models. Cloak_create_node (0x00462eb0)
	// clones this mesh rather than the ordinary material-split SRO.
	std::vector<GameplayVertex> cloak_vertices;
	std::vector<std::uint32_t> cloak_indices;
	if (model.cloak_mesh_available)
	{
		cloak_vertices.reserve(source.vertices->count);
		for (std::uint32_t vertex_index = 0;
			vertex_index < source.vertices->count;
			++vertex_index)
		{
			const std::uint8_t* vertex =
				source.vertices->data
					+ vertex_index * source.vertices->stride;
			cloak_vertices.push_back({
				read_float(vertex),
				read_float(vertex + 4),
				read_float(vertex + 8),
				0xff000000u,
				0.0f,
				0.0f,
			});
		}
		auto append_cloak_corner = [&](
			const std::uint8_t* polygon,
			std::uint32_t corner)
		{
			const std::uint32_t source_vertex =
				io::read_le32(polygon + 0x0c + corner * 4);
			if (source_vertex >= source.vertices->count)
			{
				return false;
			}
			cloak_indices.push_back(source_vertex);
			return true;
		};
		for (std::uint32_t polygon_index = 0;
			polygon_index < source.polygons->count;)
		{
			const std::uint8_t* polygon =
				source.polygons->data
					+ polygon_index * source.polygons->stride;
			const std::uint32_t face_policy =
				io::read_le32(polygon + 8);
			const std::size_t before = cloak_indices.size();
			lod.cloak_sections.push_back({
				static_cast<std::uint32_t>(before),
				0,
				UINT32_MAX,
				0,
				0,
				0,
				false,
				(face_policy & kFaceSuppressed) != 0,
				(face_policy & kFaceDoubleSided) != 0,
			});
			if (can_merge_fan(polygon_index))
			{
				const std::uint32_t continuation_count =
					io::read_le32(polygon + 0x4c);
				for (std::uint32_t triangle = 0;
					triangle <= continuation_count;
					++triangle)
				{
					const std::uint8_t* previous =
						triangle == 0
							? polygon
							: source.polygons->data
								+ (polygon_index + triangle - 1)
									* source.polygons->stride;
					const std::uint8_t* current =
						triangle == 0
							? polygon
							: source.polygons->data
								+ (polygon_index + triangle)
									* source.polygons->stride;
					if (!append_cloak_corner(polygon, 0)
						|| !append_cloak_corner(
							previous, triangle == 0 ? 1 : 2)
						|| !append_cloak_corner(current, 2))
					{
						return false;
					}
				}
				polygon_index += continuation_count + 1;
			}
			else
			{
				for (std::uint32_t corner = 0; corner < 3; ++corner)
				{
					if (!append_cloak_corner(polygon, corner))
					{
						return false;
					}
				}
				// SRO_build_cloak_lod_mesh 0x004a3ec1 assigns primitive
				// kind three to every standalone cloak face, independently
				// of the source face kind. The driver therefore consumes its
				// first triangle as 0,2,1.
				std::swap(
					cloak_indices[cloak_indices.size() - 2],
					cloak_indices[cloak_indices.size() - 1]);
				++polygon_index;
			}
			lod.cloak_sections.back().index_count =
				static_cast<std::uint32_t>(
					cloak_indices.size() - before);
		}
	}

	for (std::uint32_t polygon_index = 0;
		polygon_index < source.polygons->count;)
	{
		const std::uint8_t* polygon =
			source.polygons->data
			+ polygon_index * source.polygons->stride;
		const std::uint32_t packed_mode = io::read_le32(polygon + 4);
		const std::uint8_t mode =
			static_cast<std::uint8_t>(packed_mode & 0x0f);
		const std::uint8_t modifier =
			static_cast<std::uint8_t>((packed_mode >> 4) & 0x0f);
		// SRO_build_lod_mesh (LANCER.EXE 0x004a3040) copies these two
		// authored bits into the driver's per-face policy byte. The normal
		// gameplay mesh pass initializes its face mask to all bits at
		// LANCER.EXE 0x004c4d00, so suppressed faces never reach drawing and
		// double-sided faces bypass only the facing test in 0x004c6280.
		const std::uint32_t face_policy = io::read_le32(polygon + 8);
		const bool face_suppressed =
			(face_policy & kFaceSuppressed) != 0;
		const bool double_sided =
			(face_policy & kFaceDoubleSided) != 0;
		const std::uint32_t source_material = io::read_le32(polygon);
		if (source_material >= material_map.size())
		{
			return false;
		}
		const bool uses_texture = material_mode_uses_texture(mode);
		const std::uint32_t texture =
			uses_texture ? material_map[source_material] : UINT32_MAX;
		std::uint32_t resolved_texture = texture;
		if (uses_texture && resolved_texture == UINT32_MAX)
		{
			const std::uint8_t* material =
				source.materials->data
				+ source_material * source.materials->stride;
			const void* end = std::memchr(material, 0, 64);
			if (end == nullptr)
			{
				return false;
			}
			const std::size_t length =
				static_cast<const std::uint8_t*>(end) - material;
			char basename[65]{};
			std::memcpy(basename, material, length);
			resolved_texture =
				add_material(
					model,
					texture_cache,
					basename,
					(node_flags & 0x0080u) != 0
						&& mode == 6);
			if (resolved_texture == UINT32_MAX)
			{
				std::fprintf(
					stderr,
					"Gameplay LOD material decode failed: %s "
					"(alternate=%u)\n",
					basename,
					(node_flags & 0x0080u) != 0
							&& mode == 6
						? 1u
						: 0u);
				return false;
			}
			material_map[source_material] = resolved_texture;
		}
		const bool requires_luminous_texture =
			(node_flags & 0x0080u) != 0 && mode == 6;
		if (requires_luminous_texture
			&& resolved_texture < model.materials.size()
			&& !model.materials[resolved_texture].has_alternate
			&& add_material(
				model,
				texture_cache,
				model.materials[resolved_texture].basename,
				true) == UINT32_MAX)
		{
			return false;
		}
		const bool material_changed =
			lod.sections.empty()
			|| lod.sections.back().texture != resolved_texture
			|| lod.sections.back().mode != mode
			|| lod.sections.back().modifier != modifier
			|| lod.sections.back().lines != (mode == 1);
		const bool transparent_face =
			mode == 2 || mode == 4 || mode == 5
				|| mode == 8 || mode == 10;
		if (material_changed)
		{
			current_light_channel = next_light_channel++;
		}
		if (material_changed
			// srd3d's SRO callback queues transparent driver faces one at a
			// time through 0x10002400. Keep one GPU section per authored
			// transparent face so the renderer can reproduce that queue's
			// reverse face order without reversing triangles inside a fan.
			|| transparent_face
			|| lod.sections.back().suppressed != face_suppressed
			|| lod.sections.back().double_sided != double_sided)
		{
			lod.sections.push_back({
				static_cast<std::uint32_t>(output_indices.size()),
				0,
				resolved_texture,
				mode,
				modifier,
				current_light_channel,
				mode == 1,
				face_suppressed,
				double_sided,
			});
		}
		const std::size_t before = output_indices.size();
		const std::uint16_t section_index = static_cast<std::uint16_t>(
			lod.sections.size() - 1);
		if (mode == 1)
		{
			const std::uint32_t suppressed =
				io::read_le32(polygon + 0x44);
			constexpr std::uint8_t edges[][2] = {
				{0, 1}, {1, 2}, {2, 0},
			};
			for (std::uint32_t edge = 0; edge < 3; ++edge)
			{
				if ((suppressed & (1u << edge)) != 0)
				{
					continue;
				}
				const std::uint32_t edge_first =
					static_cast<std::uint32_t>(output_indices.size());
				if (!append_corner(polygon, edges[edge][0])
					|| !append_corner(polygon, edges[edge][1]))
				{
					return false;
				}
				const std::uint32_t first_corner =
					static_cast<std::uint32_t>(lod.face_corners.size());
				lod.face_corners.push_back(edge_first);
				lod.face_corners.push_back(edge_first + 1);
				lod.faces.push_back({
					edge_first,
					2,
					first_corner,
					2,
					section_index,
				});
			}
			++polygon_index;
		}
		else if (can_merge_fan(polygon_index))
		{
			const std::uint32_t continuation_count =
				io::read_le32(polygon + 0x4c);
			const std::uint32_t first_corner =
				static_cast<std::uint32_t>(lod.face_corners.size());
			for (std::uint32_t triangle = 0;
				triangle <= continuation_count;
				++triangle)
			{
				const std::uint8_t* previous =
					triangle == 0
						? polygon
						: source.polygons->data
							+ (polygon_index + triangle - 1)
								* source.polygons->stride;
				const std::uint8_t* current =
					triangle == 0
						? polygon
						: source.polygons->data
							+ (polygon_index + triangle)
								* source.polygons->stride;
				if (!append_corner(polygon, 0)
					|| !append_corner(previous, triangle == 0 ? 1 : 2)
					|| !append_corner(current, 2))
				{
					return false;
				}
				apply_retail_face_winding(0);
				if (triangle == 0)
				{
					lod.face_corners.push_back(
						static_cast<std::uint32_t>(before));
					lod.face_corners.push_back(
						static_cast<std::uint32_t>(before + 1));
				}
				lod.face_corners.push_back(
					static_cast<std::uint32_t>(output_indices.size() - 1));
			}
			lod.faces.push_back({
				static_cast<std::uint32_t>(before),
				static_cast<std::uint32_t>(
					output_indices.size() - before),
				first_corner,
				static_cast<std::uint16_t>(continuation_count + 3),
				section_index,
			});
			polygon_index += continuation_count + 1;
		}
		else
		{
			for (std::uint32_t corner = 0; corner < 3; ++corner)
			{
				if (!append_corner(polygon, corner))
				{
					return false;
				}
			}
			apply_retail_face_winding(static_cast<std::uint16_t>(
				io::read_le32(polygon + 0x48)));
			const std::uint32_t first_corner =
				static_cast<std::uint32_t>(lod.face_corners.size());
			lod.face_corners.push_back(
				static_cast<std::uint32_t>(before));
			lod.face_corners.push_back(
				static_cast<std::uint32_t>(before + 1));
			lod.face_corners.push_back(
				static_cast<std::uint32_t>(before + 2));
			lod.faces.push_back({
				static_cast<std::uint32_t>(before),
				3,
				first_corner,
				3,
				section_index,
			});
			++polygon_index;
		}
		lod.sections.back().index_count += static_cast<std::uint32_t>(
			output_indices.size() - before);
	}
	if (output_vertices.size() > UINT32_MAX
		|| output_indices.size() > UINT32_MAX
		|| cloak_vertices.size() > UINT32_MAX
		|| cloak_indices.size() > UINT32_MAX
		|| !lod.vertices.allocate(
			output_vertices.size() * sizeof(GameplayVertex))
		|| !lod.indices.allocate(
			output_indices.size() * sizeof(std::uint32_t))
		|| !lod.cloak_vertices.allocate(
			cloak_vertices.size() * sizeof(GameplayVertex))
		|| !lod.cloak_indices.allocate(
			cloak_indices.size() * sizeof(std::uint32_t)))
	{
		std::fprintf(
			stderr,
			"Gameplay LOD output allocation failed: vertices=%zu "
			"indices=%zu cloak_vertices=%zu cloak_indices=%zu\n",
			output_vertices.size(),
			output_indices.size(),
			cloak_vertices.size(),
			cloak_indices.size());
		return false;
	}
	std::memcpy(
		lod.vertices.data,
		output_vertices.data(),
		lod.vertices.size);
	std::memcpy(
		lod.indices.data,
		output_indices.data(),
		lod.indices.size);
	if (!cloak_vertices.empty())
	{
		std::memcpy(
			lod.cloak_vertices.data,
			cloak_vertices.data(),
			lod.cloak_vertices.size);
	}
	if (!cloak_indices.empty())
	{
		std::memcpy(
			lod.cloak_indices.data,
			cloak_indices.data(),
			lod.cloak_indices.size);
	}
	lod.normals = static_cast<std::vector<glm::vec3>&&>(
		output_normals);
	lod.secondary_normals =
		static_cast<std::vector<glm::vec3>&&>(
			output_secondary_normals);
	lod.static_lighting_rgb.assign(
		output_vertices.size(), glm::vec3{0.0f});
	lod.vertex_count = static_cast<std::uint32_t>(output_vertices.size());
	lod.index_count = static_cast<std::uint32_t>(output_indices.size());
	lod.cloak_vertex_count =
		static_cast<std::uint32_t>(cloak_vertices.size());
	lod.cloak_index_count =
		static_cast<std::uint32_t>(cloak_indices.size());
	// Retail still constructs a valid mesh record when every edge of every
	// line face is masked. Absence of output primitives is not a parse
	// failure.
	return source.polygons->count != 0;
}

bool finite_vec3(const glm::vec3& value);
bool finite_mat3(const glm::mat3& value);

bool build_collision_tree(
	const SourceNode& source,
	GameplayCollisionTree& output)
{
	output = {};
	if (source.collision == nullptr)
	{
		return true;
	}
	if (source.collision->stride < 0x40
		|| source.collision_polygons.size() != source.collision->count
		|| source.lods.empty())
	{
		return false;
	}
	const SourceLod& lod = source.lods[0];
	if (lod.vertices == nullptr
		|| lod.vertices->stride < 0x1c
		|| lod.polygons == nullptr
		|| lod.polygons->stride < 0x48)
	{
		return false;
	}

	auto polygon_normal = [&](std::uint32_t polygon_index)
	{
		const std::uint8_t* polygon =
			lod.polygons->data
			+ polygon_index * lod.polygons->stride;
		return glm::vec3{
			read_float(polygon + 0x30),
			read_float(polygon + 0x34),
			read_float(polygon + 0x38),
		};
	};
	auto can_merge_fan = [&](std::uint32_t polygon_index)
	{
		if (lod.polygons->stride < 0x50)
		{
			return false;
		}
		const std::uint8_t* polygon =
			lod.polygons->data
			+ polygon_index * lod.polygons->stride;
		if (io::read_le32(polygon + 0x48) != 1
			|| (polygon_index != 0
				&& io::read_le32(
					lod.polygons->data
					+ (polygon_index - 1) * lod.polygons->stride
					+ 0x4c) != 0))
		{
			return false;
		}
		const std::uint32_t continuation_count =
			io::read_le32(polygon + 0x4c);
		if (continuation_count
			> lod.polygons->count - polygon_index - 1)
		{
			return false;
		}
		const glm::vec3 normal = polygon_normal(polygon_index);
		for (std::uint32_t continuation = 1;
			continuation <= continuation_count;
			++continuation)
		{
			if (glm::dot(
					normal,
					polygon_normal(polygon_index + continuation))
				< 0.9990000128746033f)
			{
				return false;
			}
		}
		return true;
	};
	auto vertex_data = [&](const std::uint8_t* polygon,
		std::uint32_t corner, glm::vec3& position, glm::vec3& normal)
	{
		const std::uint32_t vertex_index =
			io::read_le32(polygon + 0x0c + corner * 4);
		if (vertex_index >= lod.vertices->count)
		{
			return false;
		}
		const std::uint8_t* vertex =
			lod.vertices->data
			+ vertex_index * lod.vertices->stride;
		position = {
			read_float(vertex),
			read_float(vertex + 4),
			read_float(vertex + 8),
		};
		normal = {
			read_float(vertex + 0x0c),
			read_float(vertex + 0x10),
			read_float(vertex + 0x14),
		};
		return finite_vec3(position) && finite_vec3(normal);
	};

	std::vector<std::uint32_t> source_polygon_map(
		lod.polygons->count, UINT32_MAX);
	for (std::uint32_t polygon_index = 0;
		polygon_index < lod.polygons->count;)
	{
		const std::uint8_t* polygon =
			lod.polygons->data
			+ polygon_index * lod.polygons->stride;
		const std::uint8_t mode = static_cast<std::uint8_t>(
			io::read_le32(polygon + 4) & 0x0f);
		const std::uint32_t output_polygon =
			static_cast<std::uint32_t>(output.polygons.size());
		GameplayCollisionPolygon record;
		record.first_triangle =
			static_cast<std::uint32_t>(output.triangles.size());
		record.plane_normal = polygon_normal(polygon_index);
		if (!finite_vec3(record.plane_normal))
		{
			return false;
		}
		if (mode == 1)
		{
			source_polygon_map[polygon_index] = output_polygon;
			output.polygons.push_back(record);
			++polygon_index;
			continue;
		}

		const std::uint32_t continuation_count =
			can_merge_fan(polygon_index)
				? io::read_le32(polygon + 0x4c)
				: 0;
		glm::vec3 root;
		glm::vec3 previous;
		glm::vec3 current;
		glm::vec3 root_normal;
		glm::vec3 previous_normal;
		glm::vec3 current_normal;
		if (!vertex_data(polygon, 0, root, root_normal)
			|| !vertex_data(
				polygon, 1, previous, previous_normal)
			|| !vertex_data(
				polygon, 2, current, current_normal))
		{
			return false;
		}
		GameplayCollisionTriangle triangle;
		triangle.points[0] = root;
		triangle.points[1] = previous;
		triangle.points[2] = current;
		triangle.normals[0] = root_normal;
		triangle.normals[1] = previous_normal;
		triangle.normals[2] = current_normal;
		output.triangles.push_back(triangle);
		++record.triangle_count;
		source_polygon_map[polygon_index] = output_polygon;
		previous = current;
		previous_normal = current_normal;
		for (std::uint32_t continuation = 1;
			continuation <= continuation_count;
			++continuation)
		{
			const std::uint8_t* next =
				lod.polygons->data
				+ (polygon_index + continuation)
					* lod.polygons->stride;
			if (!vertex_data(next, 2, current, current_normal))
			{
				return false;
			}
			triangle.points[0] = root;
			triangle.points[1] = previous;
			triangle.points[2] = current;
			triangle.normals[0] = root_normal;
			triangle.normals[1] = previous_normal;
			triangle.normals[2] = current_normal;
			output.triangles.push_back(triangle);
			++record.triangle_count;
			source_polygon_map[polygon_index + continuation] =
				output_polygon;
			previous = current;
			previous_normal = current_normal;
		}
		output.polygons.push_back(record);
		polygon_index += continuation_count + 1;
	}

	output.nodes.resize(source.collision->count);
	for (std::uint32_t index = 0;
		index < source.collision->count;
		++index)
	{
		const std::uint8_t* serialized =
			source.collision->data
			+ index * source.collision->stride;
		const SroChunk& references =
			*source.collision_polygons[index];
		GameplayCollisionNode& node = output.nodes[index];
		for (std::uint32_t row = 0; row < 3; ++row)
		{
			for (std::uint32_t column = 0; column < 3; ++column)
			{
				const float value = read_float(
					serialized + 4 + (row * 3 + column) * 4);
				if (!std::isfinite(value))
				{
					return false;
				}
				node.orientation[column][row] = value;
			}
			node.half_extents[row] =
				read_float(serialized + 0x28 + row * 4);
			node.center[row] =
				read_float(serialized + 0x34 + row * 4);
		}
		if (!finite_vec3(node.half_extents)
			|| !finite_vec3(node.center)
			|| glm::any(glm::lessThan(node.half_extents, glm::vec3{0.0f})))
		{
			return false;
		}
		if (references.count == 0)
		{
			if (source.collision->stride < 0x48)
			{
				return false;
			}
			node.child_a = io::read_le32(serialized + 0x40);
			node.child_b = io::read_le32(serialized + 0x44);
			if (node.child_a >= source.collision->count
				|| node.child_b >= source.collision->count)
			{
				return false;
			}
			continue;
		}
		if (references.stride < 4)
		{
			return false;
		}
		node.polygons.reserve(references.count);
		for (std::uint32_t reference = 0;
			reference < references.count;
			++reference)
		{
			const std::uint32_t source_polygon = io::read_le32(
				references.data + reference * references.stride);
			if (source_polygon >= source_polygon_map.size()
				|| source_polygon_map[source_polygon] == UINT32_MAX)
			{
				return false;
			}
			node.polygons.push_back(
				source_polygon_map[source_polygon]);
		}
	}
	return true;
}

bool finite_vec3(const glm::vec3& value)
{
	return std::isfinite(value.x)
		&& std::isfinite(value.y)
		&& std::isfinite(value.z);
}

bool finite_mat3(const glm::mat3& value)
{
	return finite_vec3(value[0])
		&& finite_vec3(value[1])
		&& finite_vec3(value[2]);
}
}

bool parse_gameplay_model(
	sl_open::Blob stored,
	const TextureCache& texture_cache,
	GameplayModel& model,
	bool force_cloak_mesh,
	bool planet)
{
	model = {};
	model.cloak_mesh_available = force_cloak_mesh;
	SroChunks chunks;
	if (!sro_chunks_parse(static_cast<sl_open::Blob&&>(stored), chunks))
	{
		return false;
	}

	std::size_t cursor = 0;
	const SroChunk* root = sro_chunk_request(chunks, cursor, 0);
	const SroChunk* model_records = sro_chunk_request(chunks, cursor, 1);
	// SRO_read_array_chunk (0x004a2eb0) accepts older record generations.
	// The Screamer pod has 0x104-byte nodes, before hit points at +0x104.
	if (root == nullptr || root->count != 1
		|| model_records == nullptr
		|| model_records->stride < 0x104
		|| model_records->count == 0)
	{
		return false;
	}
	if (root->stride >= 0x14)
	{
		model.camera_offset = read_vec3(root->data + 0x08);
	}
	// SRO_load copies only the bytes supplied by the source generation.
	// The oldest 0x14-byte root predates flags_14; later roots expose the
	// retail cloak-capability bit at +0x14.
	if (root->stride >= 0x18)
	{
		model.root_flags = io::read_le32(root->data + 0x14);
		model.cloak_mesh_available =
			model.cloak_mesh_available
			|| (model.root_flags & kGameplayModelRootCloak) != 0;
	}

	std::vector<SourceNode> source_nodes(model_records->count);
	for (std::uint32_t index = 0; index < model_records->count; ++index)
	{
		SourceNode& source_node = source_nodes[index];
		source_node.record =
			model_records->data + index * model_records->stride;
		source_node.stride = model_records->stride;
		const SroChunk* lods = sro_chunk_request(chunks, cursor, 2);
		const SroChunk* collision = sro_chunk_request(chunks, cursor, 7);
		source_node.collision = collision;
		const SroChunk* locators = sro_chunk_request(chunks, cursor, 9);
		const SroChunk* sequences = sro_chunk_request(chunks, cursor, 10);
		const SroChunk* groups = sro_chunk_request(chunks, cursor, 13);
		source_node.portals = sro_chunk_request(chunks, cursor, 15);
		if (lods == nullptr || lods->stride < 4 || lods->count == 0)
		{
			return false;
		}
		source_node.lods.resize(lods->count);
		for (std::uint32_t lod = 0; lod < lods->count; ++lod)
		{
			source_node.lods[lod] = {
				lods->data + lod * lods->stride,
				lods->stride,
				sro_chunk_request(chunks, cursor, 4),
				sro_chunk_request(chunks, cursor, 3),
				sro_chunk_request(chunks, cursor, 6),
			};
		}
		for (std::uint32_t node = 0;
			node < (collision == nullptr ? 0 : collision->count);
			++node)
		{
			const SroChunk* polygons = sro_chunk_request(chunks, cursor, 8);
			if (polygons == nullptr)
			{
				return false;
			}
			source_node.collision_polygons.push_back(polygons);
		}
		for (std::uint32_t sequence = 0;
			sequence < (sequences == nullptr ? 0 : sequences->count);
			++sequence)
		{
			const SroChunk* keys = sro_chunk_request(chunks, cursor, 11);
			const SroChunk* events = sro_chunk_request(chunks, cursor, 12);
			if (sequences->stride < 8
				|| keys == nullptr || keys->stride < 0x1c
				|| events == nullptr || events->stride < 0x0c)
			{
				return false;
			}
			const std::uint8_t* record =
				sequences->data + sequence * sequences->stride;
			GameplaySequence retained;
			retained.duration = static_cast<std::int32_t>(
				io::read_le32(record));
			retained.default_mode = static_cast<std::int16_t>(
				io::read_le16(record + 4));
			const std::size_t name_capacity =
				std::min<std::size_t>(
					18, sequences->stride - 6);
			const void* name_end =
				std::memchr(record + 6, 0, name_capacity);
			const std::size_t name_length =
				name_end == nullptr
					? name_capacity
					: static_cast<const std::uint8_t*>(
						name_end) - (record + 6);
			std::memcpy(
				retained.name, record + 6, name_length);
			retained.keys.resize(keys->count);
			for (std::uint32_t key = 0;
				key < keys->count;
				++key)
			{
				const std::uint8_t* source =
					keys->data + key * keys->stride;
				GameplaySequenceKey& output =
					retained.keys[key];
				output.time = static_cast<std::int32_t>(
					io::read_le32(source));
				output.euler = read_vec3(source + 4);
				output.translation = read_vec3(source + 0x10);
			}
			retained.events.resize(events->count);
			for (std::uint32_t event = 0;
				event < events->count;
				++event)
			{
				const std::uint8_t* source =
					events->data + event * events->stride;
				GameplaySequenceEvent& output =
					retained.events[event];
				output.time = static_cast<std::int32_t>(
					io::read_le32(source));
				output.type = static_cast<std::int32_t>(
					io::read_le32(source + 4));
				output.parameter = static_cast<std::int32_t>(
					io::read_le32(source + 8));
			}
			source_node.sequences.push_back(
				static_cast<GameplaySequence&&>(retained));
		}
		for (std::uint32_t group = 0;
			group < (groups == nullptr ? 0 : groups->count);
			++group)
		{
			const SroChunk* points = sro_chunk_request(chunks, cursor, 14);
			if (groups->stride < 4
				|| points == nullptr
				|| (points->count != 0 && points->stride < 0x14))
			{
				return false;
			}
			GameplayPointGroup retained;
			retained.type = static_cast<std::int32_t>(
				io::read_le32(
					groups->data + group * groups->stride));
			retained.points.reserve(points->count);
			for (std::uint32_t point = 0;
				point < points->count;
				++point)
			{
				const std::uint8_t* item =
					points->data + point * points->stride;
				const std::uint32_t polygon =
					io::read_le32(item + 4);
				const SroChunk* polygons =
					source_node.lods[0].polygons;
				if (polygons == nullptr
					|| polygons->stride < 0x3c
					|| polygon >= polygons->count)
				{
					return false;
				}
				const glm::vec3 position = read_vec3(
					item + 8);
				const glm::vec3 direction = read_vec3(
					polygons->data
						+ polygon * polygons->stride
						+ 0x30);
				if (!finite_vec3(position)
					|| !finite_vec3(direction))
				{
					return false;
				}
				retained.points.push_back({
					position,
					direction,
					polygon,
				});
			}
			source_node.point_groups.push_back(
				static_cast<GameplayPointGroup&&>(retained));
		}
		for (std::uint32_t locator = 0;
			locator < (locators == nullptr ? 0 : locators->count);
			++locator)
		{
			const std::uint8_t* record =
				locators->data + locator * locators->stride;
			SroLocator source;
			if (!sro_locator_decode(*locators, locator, source))
			{
				return false;
			}
			GameplayLocator retained;
			retained.type = static_cast<std::int16_t>(source.type);
			retained.subtype = static_cast<std::int16_t>(source.values[0]);
			retained.source_node = static_cast<std::uint16_t>(index);
			retained.position = source.position;
			retained.basis = source.basis;
			for (std::uint32_t parameter = 0; parameter < 4; ++parameter)
			{
				retained.parameters[parameter] = source.values[parameter + 1];
			}
			if (locators->stride >= 0x54)
			{
				retained.dimensions = read_vec3(record + 0x48);
			}
			if (locators->stride >= 0x60)
			{
				retained.on_time = static_cast<std::int32_t>(
					io::read_le32(record + 0x54));
				retained.off_time = static_cast<std::int32_t>(
					io::read_le32(record + 0x58));
				retained.phase = static_cast<std::int32_t>(
					io::read_le32(record + 0x5c));
			}
			if (locators->stride >= 0x68)
			{
				retained.exporter_id =
					io::read_le32(record + 0x64);
			}
			if (locators->stride >= 0x7c)
			{
				retained.light_radius = read_float(record + 0x74);
				retained.light_intensity = read_float(record + 0x78);
			}
			if (!finite_vec3(retained.position)
				|| !finite_mat3(retained.basis)
				|| !finite_vec3(retained.dimensions)
				|| !std::isfinite(retained.light_radius)
				|| !std::isfinite(retained.light_intensity))
			{
				return false;
			}
			model.locators.push_back(retained);
			if (retained.type != 0)
			{
				continue;
			}
			if (model.hardpoints.size() == kMaximumGameplayHardpoints)
			{
				return false;
			}
			GameplayHardpoint hardpoint;
			hardpoint.position = retained.position;
			hardpoint.basis = retained.basis;
			hardpoint.source_node = retained.source_node;
			for (std::uint32_t tier = 0;
				tier < kGameplayLoadoutTiers;
				++tier)
			{
				hardpoint.default_loadout[tier] = source.values[tier];
			}
			model.hardpoints.push_back(hardpoint);
		}
	}
	// SRO tag 16 is the root-level 0x4c-byte gun-clearance table.
	// AI_NewAttackRun_begin (LANCER.EXE 0x004065d0) consumes its retained
	// exporter direction at +0x00 even though ordinary gun clearance uses
	// only the 64-byte mask at +0x0c.
	const SroChunk* gun_clearance = sro_chunk_request(chunks, cursor, 16);
	if (gun_clearance != nullptr)
	{
		if (gun_clearance->stride < 0x0c)
		{
			return false;
		}
		model.gun_clearance_directions.reserve(gun_clearance->count);
		model.gun_clearance_masks.reserve(gun_clearance->count);
		for (std::uint32_t index = 0;
			index < gun_clearance->count;
			++index)
		{
			const std::uint8_t* record =
				gun_clearance->data + index * gun_clearance->stride;
			const glm::vec3 direction{
				read_float(record),
				read_float(record + 4),
				read_float(record + 8),
			};
			if (!finite_vec3(direction))
			{
				return false;
			}
			model.gun_clearance_directions.push_back(direction);
			std::array<std::uint8_t, 64> mask{};
			if (gun_clearance->stride >= 0x4c)
			{
				std::memcpy(mask.data(), record + 0x0c, mask.size());
			}
			model.gun_clearance_masks.push_back(mask);
		}
	}

	model.nodes.resize(source_nodes.size());
	bool static_light_partition[2]{};
	for (const GameplayLocator& locator : model.locators)
	{
		if (locator.type != 4
			|| !(locator.light_intensity > 0.0f)
			|| locator.on_time + locator.off_time != 0
			|| locator.source_node >= source_nodes.size())
		{
			continue;
		}
		const std::uint32_t source_flags = io::read_le32(
			source_nodes[locator.source_node].record + 0xf0);
		static_light_partition[
			(source_flags & 0x0004u) != 0 ? 1 : 0] = true;
	}
	glm::vec3 first_mass_moment{0.0f};
	float total_mass = 0.0f;
	for (std::uint32_t index = 0; index < source_nodes.size(); ++index)
	{
		const SourceNode& source = source_nodes[index];
		GameplayNode& node = model.nodes[index];
		node.sequences = source.sequences;
		node.point_groups = source.point_groups;
		const void* name_end = std::memchr(source.record, 0, 64);
		if (name_end == nullptr)
		{
			return false;
		}
		std::memcpy(
			node.name,
			source.record,
			static_cast<const std::uint8_t*>(name_end) - source.record);
		node.parent = static_cast<std::int32_t>(
			io::read_le32(source.record + 0x94));
		node.model_type = io::read_le32(source.record + 0x40);
		node.part_group_id = io::read_le32(source.record + 0xd4);
		node.flags = io::read_le32(source.record + 0xf0);
		if (static_light_partition[
				(node.flags & 0x0004u) != 0 ? 1 : 0])
		{
			// SRO_propagate_static_light_flags
			// (LANCER.EXE 0x004a4070) derives bit 0x40 for every
			// model in a partition containing at least one qualifying
			// non-cyclic type-four light.
			node.flags |= 0x0040u;
		}
		node.gun_mount_kind =
			io::read_le16(source.record + 0xf4);
		node.gun_part_slot =
			io::read_le32(source.record + 0xf8);
		if (source.stride >= 0x108)
		{
			node.maximum_hit_points = static_cast<std::int32_t>(
				io::read_le32(source.record + 0x104));
		}
		if (source.stride >= 0x10c)
		{
			node.damage_group_selector =
				io::read_le32(source.record + 0x108);
		}
		if (node.parent >= static_cast<std::int32_t>(source_nodes.size())
			|| node.parent < -1)
		{
			return false;
		}
		for (std::uint32_t axis = 0; axis < 3; ++axis)
		{
			node.position[axis] =
				read_float(source.record + 0x44 + axis * 4);
			node.bounds_min[axis] =
				read_float(source.record + 0x50 + axis * 4);
			node.bounds_max[axis] =
				read_float(source.record + 0x5c + axis * 4);
			node.rest_translation[axis] =
				read_float(source.record + 0x98 + axis * 4);
			node.suppress_anim_rotation[axis] =
				io::read_le32(source.record + 0xc8 + axis * 4);
			node.joint_min_degrees[axis] =
				read_float(source.record + 0xd8 + axis * 4);
			node.joint_max_degrees[axis] =
				read_float(source.record + 0xe4 + axis * 4);
		}
		const glm::vec3 source_first_moment{
			read_float(source.record + 0x80),
			read_float(source.record + 0x84),
			read_float(source.record + 0x88),
		};
		const float source_volume =
			read_float(source.record + 0x8c);
		const float source_density =
			read_float(source.record + 0x90);
		for (std::uint32_t row = 0; row < 3; ++row)
		{
			for (std::uint32_t column = 0; column < 3; ++column)
			{
				node.basis[column][row] = read_float(
					source.record + 0xa4 + (row * 3 + column) * 4);
			}
		}
		if (!finite_vec3(node.position)
			|| !finite_mat3(node.basis)
			|| !finite_vec3(node.rest_translation)
			|| !finite_vec3(node.joint_min_degrees)
			|| !finite_vec3(node.joint_max_degrees)
			|| !finite_vec3(node.bounds_min)
			|| !finite_vec3(node.bounds_max)
			|| !finite_vec3(source_first_moment)
			|| !std::isfinite(source_volume)
			|| !std::isfinite(source_density))
		{
			return false;
		}
		// object_accumulate_model_mass (LANCER.EXE 0x004764a0) skips
		// authored hidden/DEST alternatives when building the initial live
		// tree's mass center.
		if ((node.flags & 0x0004u) == 0)
		{
			first_mass_moment += source_density
				* (source_first_moment + source_volume * node.position);
			total_mass += source_density * source_volume;
		}
		node.lods.resize(source.lods.size());
		for (std::uint32_t lod = 0; lod < source.lods.size(); ++lod)
		{
			if (!build_lod(
				source.lods[lod],
				lod + 1 < source.lods.size()
					? &source.lods[lod + 1]
					: nullptr,
				node.flags,
				texture_cache,
				model,
				node.lods[lod],
				planet))
			{
				std::fprintf(
					stderr,
					"Gameplay model parse failed while building "
					"node %u LOD %u\n",
					index,
					lod);
				return false;
			}
		}
		if (!build_collision_tree(source, node.collision))
		{
			std::fprintf(
				stderr,
				"Gameplay model parse failed while building "
				"collision tree for node %u\n",
				index);
			return false;
		}
		if (source.portals != nullptr)
		{
			if (source.portals->stride < 0x10
				|| source.lods.empty()
				|| source.lods[0].vertices == nullptr
				|| source.lods[0].vertices->stride < 0x0c)
			{
				return false;
			}
			node.portals.reserve(source.portals->count);
			const SroChunk& vertices = *source.lods[0].vertices;
			for (std::uint32_t portal_index = 0;
				portal_index < source.portals->count;
				++portal_index)
			{
				const std::uint8_t* record =
					source.portals->data
					+ portal_index * source.portals->stride;
				GameplayPortal portal;
				const std::int32_t fourth =
					static_cast<std::int32_t>(
						io::read_le32(record + 0x0c));
				portal.vertex_count = fourth < 0 ? 3 : 4;
				for (std::uint8_t vertex_index = 0;
					vertex_index < portal.vertex_count;
					++vertex_index)
				{
					const std::uint32_t source_vertex =
						io::read_le32(record + vertex_index * 4);
					if (source_vertex >= vertices.count)
					{
						return false;
					}
					portal.vertices[vertex_index] = read_vec3(
						vertices.data
						+ source_vertex * vertices.stride);
				}
				const glm::vec3 edge_a =
					portal.vertices[1] - portal.vertices[0];
				const glm::vec3 edge_b =
					portal.vertices[2] - portal.vertices[0];
				const glm::vec3 normal = glm::cross(edge_a, edge_b);
				const float length = glm::length(normal);
				if (!(length > 0.0f) || !std::isfinite(length))
				{
					return false;
				}
				portal.plane_normal = normal / length;
				portal.plane_constant = glm::dot(
					portal.plane_normal,
					portal.vertices[0]);
				node.portals.push_back(portal);
			}
		}
	}
	for (const GameplayLocator& locator : model.locators)
	{
		if (locator.type != 4
			|| !(locator.light_intensity > 0.0f)
			|| locator.on_time + locator.off_time != 0
			|| locator.source_node >= model.nodes.size())
		{
			continue;
		}
		const GameplayNode& source_node =
			model.nodes[locator.source_node];
		glm::vec3 light_color{0.0f};
		// SRO_bake_static_lights (LANCER.EXE 0x004a4310) builds this
		// tuple with three independent tests. Unlike the dynamic
		// constructor, its exact path does not assign colors 4 or 5.
		if (locator.subtype == 2 || locator.subtype == 3)
		{
			light_color.r = 1.0f;
		}
		if (locator.subtype == 1 || locator.subtype == 2)
		{
			light_color.g = 1.0f;
		}
		if (locator.subtype == 0)
		{
			light_color.b = 1.0f;
		}
		const glm::vec3 light_model_position =
			source_node.position + locator.position;
		const float effective_radius =
			locator.light_intensity * locator.light_radius;
		const float radius_squared =
			effective_radius * effective_radius;
		for (GameplayNode& target_node : model.nodes)
		{
			if (((target_node.flags ^ source_node.flags) & 0x0004u) != 0)
			{
				continue;
			}
			const glm::vec3 light_position =
				light_model_position - target_node.position;
			for (GameplayLod& lod : target_node.lods)
			{
				auto* vertices =
					reinterpret_cast<GameplayVertex*>(
						lod.vertices.data);
				for (std::uint32_t vertex_index = 0;
					vertex_index < lod.vertex_count;
					++vertex_index)
				{
					const glm::vec3 vertex_position{
						vertices[vertex_index].x,
						vertices[vertex_index].y,
						vertices[vertex_index].z,
					};
					const glm::vec3 to_light =
						light_position - vertex_position;
					const float distance_squared =
						glm::dot(to_light, to_light);
					const float normal_dot = glm::dot(
						to_light, lod.normals[vertex_index]);
					if (!(distance_squared < radius_squared)
						|| !(normal_dot > 0.0f))
					{
						continue;
					}
					const float distance =
						std::sqrt(distance_squared);
					const float attenuation =
						(1.0f / distance
							+ distance / radius_squared
							- 2.0f / effective_radius)
						* normal_dot;
					glm::vec3& color =
						lod.static_lighting_rgb[vertex_index];
					color += attenuation
						* locator.light_intensity
						* light_color;
					color = glm::min(color, glm::vec3{1.0f});
					vertices[vertex_index].rgba = pack_rgb(color);
				}
			}
		}
	}
	if (total_mass > 0.0f)
	{
		model.center_of_mass = first_mass_moment / total_mass;
	}
	model.total_mass = total_mass;
	glm::mat3 inertia{0.0f};
	for (std::uint32_t index = 0; index < source_nodes.size(); ++index)
	{
		if ((model.nodes[index].flags & 0x0004u) != 0)
		{
			// GameObject_finalize_model_tree reaches hidden models while
			// publishing bounds, but skips their mass/inertia contribution
			// at LANCER.EXE 0x00476824.
			continue;
		}
		const std::uint8_t* record = source_nodes[index].record;
		const float sxx = read_float(record + 0x68);
		const float syy = read_float(record + 0x6c);
		const float szz = read_float(record + 0x70);
		const float sxy = read_float(record + 0x74);
		const float syz = read_float(record + 0x78);
		const float sxz = read_float(record + 0x7c);
		const float mx = read_float(record + 0x80);
		const float my = read_float(record + 0x84);
		const float mz = read_float(record + 0x88);
		const float volume = read_float(record + 0x8c);
		const float density = read_float(record + 0x90);
		const glm::vec3 offset =
			model.nodes[index].position - model.center_of_mass;
		const float ixx = density * (
			syy + szz
			+ volume * (offset.y * offset.y + offset.z * offset.z)
			+ 2.0f * offset.y * my
			+ 2.0f * offset.z * mz);
		const float iyy = density * (
			sxx + szz
			+ volume * (offset.x * offset.x + offset.z * offset.z)
			+ 2.0f * offset.x * mx
			+ 2.0f * offset.z * mz);
		const float izz = density * (
			sxx + syy
			+ volume * (offset.x * offset.x + offset.y * offset.y)
			+ 2.0f * offset.x * mx
			+ 2.0f * offset.y * my);
		const float ixy = -density * (
			sxy + offset.x * my + offset.y * mx
			+ volume * offset.x * offset.y);
		const float ixz = -density * (
			sxz + offset.x * mz + offset.z * mx
			+ volume * offset.x * offset.z);
		const float iyz = -density * (
			syz + offset.y * mz + offset.z * my
			+ volume * offset.y * offset.z);
		inertia[0][0] += ixx;
		inertia[1][1] += iyy;
		inertia[2][2] += izz;
		inertia[1][0] += ixy;
		inertia[0][1] += ixy;
		inertia[2][0] += ixz;
		inertia[0][2] += ixz;
		inertia[2][1] += iyz;
		inertia[1][2] += iyz;
	}
	if (!finite_mat3(inertia))
	{
		return false;
	}
	model.inertia_tensor = inertia;
	float radius_squared = 0.0f;
	for (const GameplayNode& node : model.nodes)
	{
		if (node.lods.empty())
		{
			continue;
		}
		const GameplayLod& lod = node.lods[0];
		const auto* vertices =
			reinterpret_cast<const GameplayVertex*>(lod.vertices.data);
		for (std::uint32_t index = 0;
			index < lod.vertex_count;
			++index)
		{
			const glm::vec3 local{
				vertices[index].x,
				vertices[index].y,
				vertices[index].z,
			};
			const glm::vec3 centered =
				local + lod.origin_offset + node.position - model.center_of_mass;
			radius_squared = std::max(
				radius_squared, glm::dot(centered, centered));
		}
	}
	// object_accumulate_model_inertia_and_bounds
	// (LANCER.EXE 0x00476768..0x0047681e) derives the aggregate radius
	// from the transformed live LOD-0 point streams. Serialized tag-1
	// bounds are not a substitute; some shipped primary bodies store zero.
	model.radius = std::sqrt(radius_squared);
	return true;
}

bool load_gameplay_model(
	io::Vfs& vfs,
	const char* path,
	const TextureCache& texture_cache,
	GameplayModel& model)
{
	sl_open::Blob stored;
	return io::vfs_read_all(vfs, path, stored)
		&& parse_gameplay_model(
			static_cast<sl_open::Blob&&>(stored), texture_cache, model);
}
}
