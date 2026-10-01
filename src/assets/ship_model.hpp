#pragma once

#include "core/blob.hpp"
#include "core/math.hpp"

#include <cstdint>

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::assets
{
struct ShipRenderVertex
{
	float x;
	float y;
	float z;
	std::uint32_t rgba;
	float u;
	float v;
};
static_assert(sizeof(ShipRenderVertex) == sizeof(float) * 6);

struct ShipRenderMesh
{
	sl_open::Blob vertices;
	sl_open::Blob indices;
	std::uint32_t vertex_count{};
	std::uint32_t index_count{};
};

constexpr std::uint32_t kMaxShipHardpoints = 20;
constexpr std::uint32_t kShipDefaultLoadoutTiers = 4;

struct ShipHardpoint
{
	glm::vec3 position{0.0f};
	glm::mat3 basis{0.0f};
	std::int32_t default_loadout[kShipDefaultLoadoutTiers]{
		-1, -1, -1, -1};
};

struct ShipModel
{
	ShipRenderMesh render;
	ShipHardpoint hardpoints[kMaxShipHardpoints]{};
	std::uint32_t hardpoint_count{};
};

bool parse_ship_model(sl_open::Blob stored, ShipModel& model);
bool load_ship_model(
	sl_open::io::Vfs& vfs,
	const char* path,
	ShipModel& model);
}
