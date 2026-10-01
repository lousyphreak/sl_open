#include "assets/ship_model.hpp"

#include "assets/sro_chunks.hpp"
#include "io/endian.hpp"
#include "io/vfs.hpp"

#include <cstring>
#include <vector>

namespace sl_open::assets
{
namespace
{
struct ModelMesh
{
	const SroChunk* detailed_vertices{};
	const SroChunk* detailed_polygons{};
	glm::vec3 position{0.0f};
};

float read_float(const std::uint8_t* bytes)
{
	const std::uint32_t bits = sl_open::io::read_le32(bytes);
	float value{};
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

bool build_render_mesh(
	const std::vector<ModelMesh>& models,
	ShipRenderMesh& mesh)
{
	std::size_t triangle_count = 0;
	for (const ModelMesh& model : models)
	{
		if (model.detailed_vertices != nullptr
			&& model.detailed_polygons != nullptr)
		{
			triangle_count += model.detailed_polygons->count;
		}
	}
	const std::size_t vertex_count = triangle_count * 3;
	if (vertex_count == 0
		|| vertex_count > UINT32_MAX
		|| !mesh.vertices.allocate(vertex_count * sizeof(ShipRenderVertex))
		|| !mesh.indices.allocate(vertex_count * sizeof(std::uint32_t)))
	{
		mesh = {};
		return false;
	}

	auto* output_vertices =
		reinterpret_cast<ShipRenderVertex*>(mesh.vertices.data);
	auto* output_indices =
		reinterpret_cast<std::uint32_t*>(mesh.indices.data);
	std::uint32_t output = 0;
	for (const ModelMesh& model : models)
	{
		const SroChunk* vertices = model.detailed_vertices;
		const SroChunk* polygons = model.detailed_polygons;
		if (vertices == nullptr || polygons == nullptr)
		{
			continue;
		}
		for (std::uint32_t polygon = 0; polygon < polygons->count; ++polygon)
		{
			const std::uint8_t* source =
				polygons->data + polygon * polygons->stride;
			const std::uint32_t source_indices[] = {
				sl_open::io::read_le32(source + 0x0c),
				sl_open::io::read_le32(source + 0x10),
				sl_open::io::read_le32(source + 0x14),
			};
			for (std::uint32_t corner = 0; corner < 3; ++corner)
			{
				if (source_indices[corner] >= vertices->count)
				{
					mesh = {};
					return false;
				}
				const std::uint8_t* vertex =
					vertices->data + source_indices[corner] * vertices->stride;
				const float x = read_float(vertex);
				const float y = read_float(vertex + 4);
				const float z = read_float(vertex + 8);
				output_vertices[output + corner] = {
					x + model.position[0],
					y + model.position[1],
					z + model.position[2],
					0xffffffff,
					read_float(source + 0x18 + corner * 4),
					read_float(source + 0x24 + corner * 4),
				};
			}
			output_indices[output] = output;
			output_indices[output + 1] = output + 1;
			output_indices[output + 2] = output + 2;
			output += 3;
		}
	}
	mesh.vertex_count = output;
	mesh.index_count = output;
	return true;
}

}

bool parse_ship_model(sl_open::Blob stored, ShipModel& model)
{
	model = {};
	SroChunks chunks;
	if (!sro_chunks_parse(static_cast<sl_open::Blob&&>(stored), chunks))
	{
		return false;
	}

	std::size_t cursor = 0;
	const SroChunk* root = sro_chunk_request(chunks, cursor, 0);
	const SroChunk* model_records = sro_chunk_request(chunks, cursor, 1);
	if (root == nullptr || root->count != 1
		|| model_records == nullptr
		|| model_records->stride < 0x50
		|| model_records->count == 0)
	{
		return false;
	}

	std::vector<ModelMesh> models(model_records->count);
	for (std::uint32_t model_index = 0;
		model_index < model_records->count;
		++model_index)
	{
		const std::uint8_t* record =
			model_records->data + model_index * model_records->stride;
		models[model_index].position[0] = read_float(record + 0x44);
		models[model_index].position[1] = read_float(record + 0x48);
		models[model_index].position[2] = read_float(record + 0x4c);

		const SroChunk* lods = sro_chunk_request(chunks, cursor, 2);
		const SroChunk* nodes = sro_chunk_request(chunks, cursor, 7);
		const SroChunk* locators = sro_chunk_request(chunks, cursor, 9);
		const SroChunk* sequences = sro_chunk_request(chunks, cursor, 10);
		const SroChunk* groups = sro_chunk_request(chunks, cursor, 13);
		sro_chunk_request(chunks, cursor, 15);
		if (lods == nullptr || lods->count == 0)
		{
			return false;
		}
		for (std::uint32_t locator = 0;
			locator < (locators == nullptr ? 0 : locators->count);
			++locator)
		{
			SroLocator source;
			if (!sro_locator_decode(*locators, locator, source))
			{
				return false;
			}
			if (source.type != 0)
			{
				continue;
			}
			if (model.hardpoint_count >= kMaxShipHardpoints)
			{
				return false;
			}
			ShipHardpoint& hardpoint =
				model.hardpoints[model.hardpoint_count++];
			for (std::uint32_t axis = 0; axis < 3; ++axis)
			{
				hardpoint.position[axis] =
					models[model_index].position[axis]
					+ source.position[axis];
			}
			hardpoint.basis = source.basis;
			for (std::uint32_t tier = 0;
				tier < kShipDefaultLoadoutTiers;
				++tier)
			{
				hardpoint.default_loadout[tier] = source.values[tier];
			}
		}
		for (std::uint32_t lod = 0; lod < lods->count; ++lod)
		{
			const SroChunk* vertices = sro_chunk_request(chunks, cursor, 4);
			const SroChunk* polygons = sro_chunk_request(chunks, cursor, 3);
			const SroChunk* materials = sro_chunk_request(chunks, cursor, 6);
			if (vertices == nullptr || vertices->stride < 0x0c
				|| polygons == nullptr || polygons->stride < 0x18
				|| materials == nullptr)
			{
				return false;
			}
			if (lod == 0)
			{
				models[model_index].detailed_vertices = vertices;
				models[model_index].detailed_polygons = polygons;
			}
		}
		for (std::uint32_t node = 0;
			node < (nodes == nullptr ? 0 : nodes->count);
			++node)
		{
			if (sro_chunk_request(chunks, cursor, 8) == nullptr)
			{
				return false;
			}
		}
		for (std::uint32_t sequence = 0;
			sequence < (sequences == nullptr ? 0 : sequences->count);
			++sequence)
		{
			if (sro_chunk_request(chunks, cursor, 11) == nullptr
				|| sro_chunk_request(chunks, cursor, 12) == nullptr)
			{
				return false;
			}
		}
		for (std::uint32_t group = 0;
			group < (groups == nullptr ? 0 : groups->count);
			++group)
		{
			if (sro_chunk_request(chunks, cursor, 14) == nullptr)
			{
				return false;
			}
		}
	}

	return build_render_mesh(models, model.render);
}

bool load_ship_model(
	sl_open::io::Vfs& vfs,
	const char* path,
	ShipModel& model)
{
	sl_open::Blob stored;
	return sl_open::io::vfs_read_all(vfs, path, stored)
		&& parse_ship_model(static_cast<sl_open::Blob&&>(stored), model);
}
}
