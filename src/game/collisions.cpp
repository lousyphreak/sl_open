#include "game/collisions.hpp"

#include "ai/runtime.hpp"
#include "assets/gameplay_model.hpp"
#include "assets/ship_stats.hpp"
#include "game/damage.hpp"
#include "game/model_animation.hpp"
#include "game/shields.hpp"
#include "game/world.hpp"
#include "mission/events.hpp"
#include "mission/network_runtime.hpp"
#include "mission/runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace sl_open::game
{
namespace
{
constexpr std::uint32_t kMaximumCollisionPasses = 10;
constexpr float kImpulseDamageScale = 0.2f;
constexpr float kCollisionDamageScale = 0.5f;
constexpr float kFixedObjectDamageScale = 0.02f;
constexpr float kFriendlyPlayerCollisionScale = 0.25f;
constexpr float kSeparationScale = 1.1f;

struct CollisionRecord
{
	std::uint16_t object_index{UINT16_MAX};
	float effective_radius{};
	float high_x{};
};

glm::vec3 retail_normalize(glm::vec3 value)
{
	// Vec3_normalize, LANCER.EXE 0x004c1370, deliberately publishes this
	// denormal-sized Z value instead of a unit vector for a zero input.
	const float length = std::sqrt(
		value.x * value.x
		+ value.y * value.y
		+ value.z * value.z);
	if (length == 0.0f)
	{
		return {0.0f, 0.0f, 0x1p-120f};
	}
	const float inverse = 1.0f / length;
	return {
		value.x * inverse,
		value.y * inverse,
		value.z * inverse,
	};
}

int compare_collision_records(
	const std::array<CollisionRecord, kMaxGameObjects>& records,
	std::uint16_t left,
	std::uint16_t right)
{
	// The retail comparator at 0x00468f60 intentionally never returns zero.
	// Its reversed sign places the greatest cached high-X value first.
	return records[left].high_x < records[right].high_x
		? 1
		: -1;
}

void retail_sort_range(
	std::array<std::uint16_t, kMaxGameObjects>& order,
	const std::array<CollisionRecord, kMaxGameObjects>& records,
	std::size_t first,
	std::size_t last)
{
	const std::size_t count = last - first + 1;
	if (count <= 8)
	{
		// MSVCRT qsort's small-partition selection pass at 0x004d0a8e.
		while (first < last)
		{
			std::size_t selected = first;
			for (std::size_t scan = first + 1;
				scan <= last;
				++scan)
			{
				if (compare_collision_records(
						records,
						order[scan],
						order[selected]) > 0)
				{
					selected = scan;
				}
			}
			std::swap(order[selected], order[last]);
			--last;
		}
		return;
	}

	// This is the exact middle-pivot partition used by the executable's
	// bundled qsort at 0x004d093a. Reproducing it matters because the
	// comparator treats equal values as ordered and collision response is
	// pair-order dependent.
	std::swap(order[first + count / 2], order[first]);
	std::size_t left = first;
	std::size_t right = last + 1;
	for (;;)
	{
		do
		{
			++left;
		}
		while (left <= last
			&& compare_collision_records(
				records, order[left], order[first]) < 1);
		do
		{
			--right;
		}
		while (right > first
			&& compare_collision_records(
				records, order[right], order[first]) > -1);
		if (left > right)
		{
			break;
		}
		std::swap(order[left], order[right]);
	}
	std::swap(order[first], order[right]);
	if (first + 1 < right)
	{
		retail_sort_range(order, records, first, right - 1);
	}
	if (left < last)
	{
		retail_sort_range(order, records, left, last);
	}
}

void retail_sort_collision_records(
	std::array<std::uint16_t, kMaxGameObjects>& order,
	const std::array<CollisionRecord, kMaxGameObjects>& records,
	std::size_t count)
{
	if (count >= 2)
	{
		retail_sort_range(order, records, 0, count - 1);
	}
}

bool collision_excluded_by_docking(
	const WorldObject& first,
	std::uint16_t first_index,
	const WorldObject& second,
	std::uint16_t second_index)
{
	return first.interaction_target_link == second_index
		|| first.docking_pair_link == second_index
		|| second.interaction_target_link == first_index
		|| second.docking_pair_link == first_index;
}

bool has_network_player_exclusion(const WorldObject& object)
{
	// Only DMBeacon and the communications relay set GameObject+0x624 in
	// the shipped executable.
	return object.type == 0x8e || object.type == 0xd7;
}

bool collision_excluded_by_network_player(
	const mission::Runtime& mission,
	const WorldObject& first,
	std::uint16_t first_index,
	const WorldObject& second,
	std::uint16_t second_index)
{
	if (mission.network.role == mission::NetworkRole::offline)
	{
		return false;
	}
	return (has_network_player_exclusion(first)
			&& second_index < mission::kNetworkPlayerCapacity)
		|| (has_network_player_exclusion(second)
			&& first_index < mission::kNetworkPlayerCapacity);
}

std::uint8_t collision_bank(
	const WorldObject& object,
	const glm::vec3& contact)
{
	// GameObject_select_bank_for_point, LANCER.EXE 0x00463ca0. Z wins
	// exact ties, and the Y coordinate is deliberately ignored.
	const glm::vec3 local =
		glm::transpose(object.orientation)
			* (contact - object.position);
	const float normalized_x =
		local.x / (object.bounds_max.x - object.bounds_min.x);
	const float normalized_z =
		local.z / (object.bounds_max.z - object.bounds_min.z);
	if (std::abs(normalized_x) > std::abs(normalized_z))
	{
		return normalized_x > 0.0f ? 1 : 0;
	}
	return normalized_z > 0.0f ? 2 : 3;
}

bool ripper_ignores_impulse(const WorldObject& object)
{
	return object.type == 0x1f
		&& object.ai.command_count != 0
		&& object.ai.commands[0].id == 12;
}

glm::vec3 accumulate_rigid_sphere_impulse(
	WorldObject& first,
	WorldObject& second,
	const glm::vec3& normal_first_from_second,
	std::uint32_t pass)
{
	// RigidBody_collide, LANCER.EXE 0x00464e80. The sphere contact lies on
	// the line through both mass centers, so all four angular-contact terms
	// reduce exactly to zero and only the two linear inverse masses remain.
	glm::vec3 direction = -normal_first_from_second;
	const glm::vec3 relative_velocity =
		first.linear_velocity - second.linear_velocity;
	if (glm::dot(relative_velocity, direction) < 0.0f)
	{
		direction *=
			(-static_cast<float>(pass + 1u) * 5.0f)
			/ std::sqrt(
				relative_velocity.x * relative_velocity.x
				+ relative_velocity.y * relative_velocity.y
				+ relative_velocity.z * relative_velocity.z);
	}
	direction = retail_normalize(direction);

	float inverse_mass_sum = 0.0f;
	if ((first.runtime_flags & kObjectFlagKinematic) == 0)
	{
		inverse_mass_sum += 1.0f / first.physics_mass;
	}
	if ((second.runtime_flags & kObjectFlagKinematic) == 0)
	{
		inverse_mass_sum += 1.0f / second.physics_mass;
	}
	float magnitude =
		glm::dot(relative_velocity, direction)
		* -2.0f / inverse_mass_sum;
	if (magnitude == 0.0f)
	{
		// Retail substitutes one only after the division.
		magnitude = 1.0f;
	}
	const glm::vec3 impulse = direction * magnitude;
	if ((first.runtime_flags & kObjectFlagKinematic) == 0
		&& !ripper_ignores_impulse(first))
	{
		first.accumulated_linear_impulse += impulse;
		++first.accumulated_impulse_count;
	}
	if ((second.runtime_flags & kObjectFlagKinematic) == 0
		&& !ripper_ignores_impulse(second))
	{
		second.accumulated_linear_impulse -= impulse;
		++second.accumulated_impulse_count;
	}
	return impulse;
}

void apply_sphere_collision_damage(
	World& world,
	mission::Runtime& mission,
	WorldObject& target,
	std::uint16_t target_index,
	std::uint16_t attacker_index,
	std::uint8_t bank,
	float damage,
	const glm::vec3& contact,
	const assets::ShipStatsTable& stats,
	bool ripple_before_primary,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick)
{
	// The Ripper is excluded from the complete ordinary sphere-damage
	// branch, not merely from structural mutation.
	if (target.type == 0x1f)
	{
		return;
	}
	if (attacker_index == world.player.index
		&& target.allegiance_class == 0
		&& target_index >= mission.player_prefix_count)
	{
		damage *= kFriendlyPlayerCollisionScale;
	}

	// The two extra player shield banks at 0x0051cf78/0x0051cf34 consume
	// the hit first. If they break, retail deliberately forwards the full
	// original damage rather than only the amount which crossed zero.
	if (target_index == world.player.index
		&& (bank == 2 || bank == 3)
		&& target.auxiliary_shields[bank - 2] > 0.0f)
	{
		float& auxiliary = target.auxiliary_shields[bank - 2];
		auxiliary -= damage;
		if (auxiliary > 0.0f)
		{
			return;
		}
		auxiliary = 0.0f;
	}

	if (target.primary_shields[bank] < 0.0f)
	{
		(void)apply_structural_bank_damage(
			world,
			mission,
			target,
			stats,
			bank,
			damage,
			attacker_index,
			2,
			impact_feedback_enabled,
			simulation_tick);
		return;
	}

	// 0x00465ca0 contains a real call-order asymmetry: the first object's
	// sphere ripple precedes primary-bank damage, while the second object's
	// ripple follows it.
	if (ripple_before_primary)
	{
		shields_register_hit(
			world,
			target,
			-1,
			contact,
			0,
			simulation_tick);
	}
	(void)apply_primary_bank_damage(
		world,
		mission,
		target,
		stats,
		bank,
		damage,
		1.0f,
		attacker_index,
		2,
		impact_feedback_enabled,
		simulation_tick);
	if (!ripple_before_primary)
	{
		shields_register_hit(
			world,
			target,
			-1,
			contact,
			0,
			simulation_tick);
	}
}

void collision_reintegrate(
	WorldObject& object,
	const assets::ShipStatsTable& stats,
	std::uint8_t camera_mode)
{
	if (!object.active
		|| object.type >= assets::kShipStatsCount
		|| (object.runtime_flags & 0x00000010u) != 0)
	{
		return;
	}
	object.transform_state_flags |= kObjectTransformChanged;
	if ((object.runtime_flags & kObjectFlagSimulationSuspended) != 0
		&& object.accumulated_impulse_count == 0
		&& (object.runtime_flags & 0x00000008u) == 0)
	{
		return;
	}

	// GameObject_integrate always recomputes its current pose from the
	// service phase's retained base sample. world_step_object necessarily
	// captures a sample itself, so retain and restore the original one
	// around its callback/force-consumption work.
	const glm::vec3 retained_position = object.previous_position;
	const glm::mat3 retained_orientation = object.previous_orientation;
	world_step_object(
		object,
		stats,
		object.control_demand,
		camera_mode);
	object.previous_position = retained_position;
	object.previous_orientation = retained_orientation;
	object.position =
		retained_position + object.linear_velocity;
	object.orientation =
		retained_orientation * object.inertial_angular_step;
	object.speed = std::sqrt(
		object.linear_velocity.x * object.linear_velocity.x
		+ object.linear_velocity.y * object.linear_velocity.y
		+ object.linear_velocity.z * object.linear_velocity.z);
}

void force_collision_position(
	WorldObject& object,
	const glm::vec3& position)
{
	// Object_model_set_position, LANCER.EXE 0x0049b600, updates every
	// position cache and the live scene root synchronously.
	object.previous_position = position;
	object.position = position;
	object.scene_position = position;
	object.transform_state_flags |= kObjectTransformChanged;
}

glm::mat4 object_transform(
	const WorldObject& object,
	bool previous)
{
	glm::mat4 transform{
		previous
			? object.previous_orientation
			: object.orientation};
	transform[3] = glm::vec4{
		previous
			? object.previous_position
			: object.position,
		1.0f};
	return transform;
}

glm::mat4 model_world_transform(
	const WorldObject& object,
	std::uint16_t model_index,
	bool previous)
{
	const glm::mat4 local =
		previous
			? model_animation_render_transform(
				object, model_index, 0.0f)
			: object.model_references[model_index].local_transform;
	return object_transform(object, previous) * local;
}

glm::vec3 point_to_local(
	const glm::mat4& transform,
	const glm::vec3& point)
{
	// The retail transform owner uses the transpose of the retained
	// orientation. Collision models are rigid; runtime transition scale is
	// deliberately left in that matrix, matching 0x004c2310.
	return glm::transpose(glm::mat3(transform))
		* (point - glm::vec3(transform[3]));
}

glm::vec3 point_to_world(
	const glm::mat4& transform,
	const glm::vec3& point)
{
	return glm::vec3(transform[3])
		+ glm::mat3(transform) * point;
}

bool segment_intersects_triangle(
	const glm::vec3& previous,
	const glm::vec3& current,
	const glm::vec3& a,
	const glm::vec3& b,
	const glm::vec3& c)
{
	// segment_intersect_triangle, LANCER.EXE 0x004ad700. All finite
	// segment and barycentric boundaries are inclusive.
	const glm::vec3 direction = current - previous;
	const glm::vec3 edge_a = b - a;
	const glm::vec3 edge_b = c - a;
	const glm::vec3 cross_direction = glm::cross(direction, edge_b);
	const float determinant = glm::dot(edge_a, cross_direction);
	if (determinant == 0.0f)
	{
		return false;
	}
	const float inverse = 1.0f / determinant;
	const glm::vec3 from_a = previous - a;
	const float u = glm::dot(from_a, cross_direction) * inverse;
	if (u < 0.0f || u > 1.0f)
	{
		return false;
	}
	const glm::vec3 cross_edge = glm::cross(from_a, edge_a);
	const float v = glm::dot(direction, cross_edge) * inverse;
	if (v < 0.0f || u + v > 1.0f)
	{
		return false;
	}
	const float parameter = glm::dot(edge_b, cross_edge) * inverse;
	return parameter >= 0.0f && parameter <= 1.0f;
}

struct TriangleClosestPoint
{
	glm::vec3 point{0.0f};
	float weights[3]{};
	float distance_squared{
		std::numeric_limits<float>::max()};
	bool valid{};
};

TriangleClosestPoint closest_point_on_triangle(
	const assets::GameplayCollisionTriangle& triangle,
	const glm::vec3& query)
{
	TriangleClosestPoint result;
	const glm::vec3 edge_a =
		triangle.points[1] - triangle.points[0];
	const glm::vec3 edge_b =
		triangle.points[2] - triangle.points[0];
	const glm::vec3 from_a = query - triangle.points[0];
	const float aa = glm::dot(edge_a, edge_a);
	const float ab = glm::dot(edge_a, edge_b);
	const float bb = glm::dot(edge_b, edge_b);
	const float aq = glm::dot(edge_a, from_a);
	const float bq = glm::dot(edge_b, from_a);
	const float determinant = aa * bb - ab * ab;
	if (determinant != 0.0f)
	{
		const float weight_b =
			(bb * aq - ab * bq) / determinant;
		const float weight_c =
			(aa * bq - ab * aq) / determinant;
		const float weight_a = 1.0f - weight_b - weight_c;
		if (weight_a >= 0.0f
			&& weight_b >= 0.0f
			&& weight_c >= 0.0f)
		{
			result.point =
				triangle.points[0] * weight_a
				+ triangle.points[1] * weight_b
				+ triangle.points[2] * weight_c;
			result.weights[0] = weight_a;
			result.weights[1] = weight_b;
			result.weights[2] = weight_c;
			const glm::vec3 delta = result.point - query;
			result.distance_squared = glm::dot(delta, delta);
			result.valid = true;
			return result;
		}
	}

	// simplex_closest_point, 0x00478360, scans these three edges in this
	// exact order and replaces the retained edge only on strict distance.
	constexpr std::uint8_t edges[3][2] = {
		{0, 1}, {0, 2}, {1, 2},
	};
	for (const auto& edge : edges)
	{
		const glm::vec3 segment =
			triangle.points[edge[1]] - triangle.points[edge[0]];
		const float length_squared = glm::dot(segment, segment);
		if (length_squared == 0.0f)
		{
			continue;
		}
		const float fraction =
			glm::dot(query - triangle.points[edge[0]], segment)
			/ length_squared;
		if (fraction < 0.0f || fraction > 1.0f)
		{
			continue;
		}
		const glm::vec3 point =
			triangle.points[edge[0]] + segment * fraction;
		const glm::vec3 delta = point - query;
		const float distance_squared = glm::dot(delta, delta);
		if (distance_squared < result.distance_squared)
		{
			result = {};
			result.point = point;
			result.weights[edge[0]] = 1.0f - fraction;
			result.weights[edge[1]] = fraction;
			result.distance_squared = distance_squared;
			result.valid = true;
		}
	}
	if (result.valid)
	{
		return result;
	}

	for (std::uint8_t vertex = 0; vertex < 3; ++vertex)
	{
		const glm::vec3 delta = triangle.points[vertex] - query;
		const float distance_squared = glm::dot(delta, delta);
		if (distance_squared < result.distance_squared)
		{
			result = {};
			result.point = triangle.points[vertex];
			result.weights[vertex] = 1.0f;
			result.distance_squared = distance_squared;
			result.valid = true;
		}
	}
	return result;
}

struct CompoundContact
{
	glm::vec3 model_point{0.0f};
	glm::vec3 model_normal{0.0f};
	float distance_squared{};
	std::uint16_t model_index{UINT16_MAX};
	bool hit{};
};

bool collision_node_overlaps_sphere(
	const assets::GameplayCollisionNode& node,
	const glm::vec3& center,
	float radius)
{
	const glm::vec3 local =
		glm::transpose(node.orientation) * (center - node.center);
	for (std::uint32_t axis = 0; axis < 3; ++axis)
	{
		if (local[axis] < -node.half_extents[axis] - radius
			|| local[axis] > node.half_extents[axis] + radius)
		{
			return false;
		}
	}
	return true;
}

void query_collision_polygons(
	const assets::GameplayCollisionTree& tree,
	const assets::GameplayCollisionNode& leaf,
	const glm::vec3& query,
	CompoundContact& retained)
{
	for (const std::uint32_t polygon_index : leaf.polygons)
	{
		if (polygon_index >= tree.polygons.size())
		{
			continue;
		}
		const assets::GameplayCollisionPolygon& polygon =
			tree.polygons[polygon_index];
		if (polygon.triangle_count == 0
			|| polygon.first_triangle >= tree.triangles.size())
		{
			continue;
		}
		const assets::GameplayCollisionTriangle& first_triangle =
			tree.triangles[polygon.first_triangle];
		const float plane_distance = glm::dot(
			query - first_triangle.points[0],
			polygon.plane_normal);
		if (plane_distance < 0.0f
			|| plane_distance * plane_distance
				> retained.distance_squared)
		{
			continue;
		}
		for (std::uint32_t fan = 0;
			fan < polygon.triangle_count;
			++fan)
		{
			const std::uint32_t triangle_index =
				polygon.first_triangle + fan;
			if (triangle_index >= tree.triangles.size())
			{
				break;
			}
			const assets::GameplayCollisionTriangle& triangle =
				tree.triangles[triangle_index];
			const TriangleClosestPoint closest =
				closest_point_on_triangle(triangle, query);
			if (!closest.valid
				|| !(closest.distance_squared
					< retained.distance_squared))
			{
				continue;
			}
			glm::vec3 normal;
			if (closest.weights[0] > 0.0f
				&& closest.weights[1] > 0.0f
				&& closest.weights[2] > 0.0f)
			{
				normal = polygon.plane_normal;
			}
			else
			{
				normal =
					triangle.normals[0] * closest.weights[0]
					+ triangle.normals[1] * closest.weights[1]
					+ triangle.normals[2] * closest.weights[2];
			}
			if (glm::dot(normal, closest.point - query) < 0.0f)
			{
				normal = -normal;
			}
			retained.model_point = closest.point;
			retained.model_normal = normal;
			retained.distance_squared = closest.distance_squared;
			retained.hit = true;
		}
	}
}

void query_collision_node(
	const assets::GameplayCollisionTree& tree,
	std::uint32_t node_index,
	const glm::vec3& local_center,
	float radius,
	std::uint16_t model_index,
	CompoundContact& retained)
{
	if (node_index >= tree.nodes.size())
	{
		return;
	}
	const assets::GameplayCollisionNode& node =
		tree.nodes[node_index];
	if (!collision_node_overlaps_sphere(node, local_center, radius))
	{
		return;
	}
	if (!node.polygons.empty())
	{
		const float previous_distance = retained.distance_squared;
		query_collision_polygons(tree, node, local_center, retained);
		if (retained.distance_squared < previous_distance)
		{
			retained.model_index = model_index;
		}
		return;
	}
	// CollisionTree_traverse pushes A and then B; its LIFO work list
	// consequently visits B first. Recursing in that pop order removes the
	// transient container without changing any tie-breaking behavior.
	if (node.child_b != UINT32_MAX)
	{
		query_collision_node(
			tree,
			node.child_b,
			local_center,
			radius,
			model_index,
			retained);
	}
	if (node.child_a != UINT32_MAX)
	{
		query_collision_node(
			tree,
			node.child_a,
			local_center,
			radius,
			model_index,
			retained);
	}
}

void query_model_collision_tree(
	const WorldObject& compound,
	std::uint16_t model_index,
	const glm::vec3& world_center,
	float radius,
	CompoundContact& retained)
{
	const ObjectModelReference& model =
		compound.model_references[model_index];
	if (model.collision == nullptr
		|| model.collision->nodes.empty()
		|| model.removed
		|| (model.runtime_flags & 0x00a0u) != 0)
	{
		return;
	}
	const glm::mat4 transform =
		model_world_transform(compound, model_index, false);
	const glm::vec3 local_center =
		point_to_local(transform, world_center);
	query_collision_node(
		*model.collision,
		0,
		local_center,
		radius,
		model_index,
		retained);
}

template<typename Visitor>
void visit_model_tree(
	const WorldObject& object,
	Visitor&& visitor)
{
	const std::size_t count = object.model_references.size();
	// Compound finalization assigns a dense, unique zero-based ID in the
	// exact wrapper-tree preorder. Resolve each ID in that order without
	// allocating or sorting inside the collision service.
	for (std::uint32_t network_id = 0;
		network_id < count;
		++network_id)
	{
		for (std::uint16_t index = 0; index < count; ++index)
		{
			if (object.model_references[index].network_model_id
				== network_id)
			{
				visitor(index);
				break;
			}
		}
	}
}

CompoundContact query_compound_contact(
	const WorldObject& ordinary,
	const WorldObject& compound)
{
	CompoundContact contact;
	contact.distance_squared = ordinary.radius * ordinary.radius;
	visit_model_tree(
		compound,
		[&](std::uint16_t model_index)
		{
			query_model_collision_tree(
				compound,
				model_index,
				ordinary.position,
				ordinary.radius,
				contact);
		});
	return contact;
}

void service_type83_crossing(
	mission::Runtime& mission,
	const WorldObject& ordinary,
	const WorldObject& trigger)
{
	const glm::vec3 previous =
		glm::transpose(trigger.previous_orientation)
			* (ordinary.previous_position
				- trigger.previous_position);
	const glm::vec3 current =
		glm::transpose(trigger.orientation)
			* (ordinary.position - trigger.position);
	if (previous.z > 0.0f || current.z <= 0.0f)
	{
		return;
	}
	const glm::vec3 delta = current - previous;
	const glm::vec3 crossing =
		previous + delta * (-previous.z / delta.z);
	const float radius =
		(trigger.bounds_max.y - trigger.bounds_min.y) * 0.5f;
	if (crossing.x * crossing.x + crossing.y * crossing.y
		>= radius * radius)
	{
		return;
	}
	(void)mission::events_emit_direct(
		mission,
		mission::EventType::jumped_through_hoop,
		trigger.mission_index,
		nullptr,
		0);
}

void service_player_portal_crossings(
	World& world,
	mission::Runtime& mission,
	const WorldObject& ordinary,
	std::uint16_t ordinary_index,
	const WorldObject& compound)
{
	if (ordinary_index != world.player.index)
	{
		return;
	}
	for (std::uint16_t model_index = 0;
		model_index < compound.model_references.size();
		++model_index)
	{
		const ObjectModelReference& model =
			compound.model_references[model_index];
		if (model.removed
			|| model.portals == nullptr
			|| model.portals->empty())
		{
			continue;
		}
		const glm::mat4 previous_transform =
			model_world_transform(compound, model_index, true);
		const glm::mat4 current_transform =
			model_world_transform(compound, model_index, false);
		const glm::vec3 previous =
			point_to_local(
				previous_transform, ordinary.previous_position);
		const glm::vec3 current =
			point_to_local(current_transform, ordinary.position);
		for (const assets::GameplayPortal& portal : *model.portals)
		{
			const float previous_distance =
				glm::dot(portal.plane_normal, previous)
				- portal.plane_constant;
			const float current_distance =
				glm::dot(portal.plane_normal, current)
				- portal.plane_constant;
			if ((previous_distance < 0.0f)
				== (current_distance < 0.0f))
			{
				continue;
			}
			bool crossed = segment_intersects_triangle(
				previous,
				current,
				portal.vertices[0],
				portal.vertices[1],
				portal.vertices[2]);
			if (!crossed && portal.vertex_count == 4)
			{
				crossed = segment_intersects_triangle(
					previous,
					current,
					portal.vertices[0],
					portal.vertices[2],
					portal.vertices[3]);
			}
			if (!crossed)
			{
				continue;
			}
			const bool entered = previous_distance < 0.0f;
			if (compound.type == 0x45)
			{
				world.player_inside_type45_compound = entered;
			}
			(void)mission::events_emit_direct(
				mission,
				entered
					? mission::EventType::inside_object
					: mission::EventType::outside_object,
				ordinary.mission_index,
				nullptr,
				0);
		}
	}
}

float angular_contact_denominator(
	const WorldObject& object,
	const glm::mat3& contact_orientation,
	const glm::vec3& lever,
	const glm::vec3& normal)
{
	const glm::vec3 crossed = glm::cross(lever, normal);
	const glm::vec3 body_crossed =
		glm::transpose(contact_orientation) * crossed;
	const glm::vec3 world_response =
		contact_orientation * (object.inverse_inertia * body_crossed);
	return glm::dot(glm::cross(world_response, lever), normal);
}

void accumulate_contact_impulse(
	WorldObject& object,
	const glm::vec3& impulse,
	const glm::vec3& contact)
{
	object.accumulated_linear_impulse += impulse;
	// GameObject_accumulate_impulse, 0x004763c0, uses this operand order
	// and subtracts the retained previous body position.
	object.accumulated_angular_impulse += glm::cross(
		impulse, contact - object.previous_position);
	++object.accumulated_impulse_count;
}

glm::vec3 accumulate_compound_impulse(
	WorldObject& ordinary,
	WorldObject& compound,
	std::uint16_t hit_model_index,
	const glm::vec3& model_contact,
	glm::vec3 normal,
	std::uint32_t pass)
{
	const glm::mat4 model_current =
		model_world_transform(compound, hit_model_index, false);
	const glm::mat4 model_previous =
		model_world_transform(compound, hit_model_index, true);
	const glm::vec3 ordinary_current = ordinary.position;
	const glm::vec3 ordinary_previous = ordinary.previous_position;
	const glm::vec3 compound_current =
		point_to_world(model_current, model_contact);
	const glm::vec3 compound_previous =
		point_to_world(model_previous, model_contact);
	glm::vec3 relative_movement =
		(ordinary_current - ordinary_previous)
		- (compound_current - compound_previous);
	if (glm::dot(relative_movement, relative_movement) == 0.0f
		&& (compound.runtime_flags & kObjectFlagCompound) != 0)
	{
		relative_movement = normal * (
			ordinary.radius
			- glm::length(ordinary_previous - compound_previous));
	}
	if (glm::dot(relative_movement, normal) < 0.0f)
	{
		relative_movement *=
			(-static_cast<float>(pass + 1u) * 5.0f)
			/ glm::length(relative_movement);
	}
	normal = retail_normalize(normal);

	float denominator = 0.0f;
	if ((ordinary.runtime_flags & kObjectFlagKinematic) == 0)
	{
		denominator += 1.0f / ordinary.physics_mass;
		denominator += angular_contact_denominator(
			ordinary,
			ordinary.orientation,
			ordinary_current - ordinary.position,
			normal);
	}
	if ((compound.runtime_flags & kObjectFlagKinematic) == 0)
	{
		denominator += 1.0f / compound.physics_mass;
		denominator += angular_contact_denominator(
			compound,
			glm::mat3(model_current),
			compound_current - glm::vec3(model_current[3]),
			normal);
	}
	float magnitude =
		glm::dot(relative_movement, normal)
		* -2.0f / denominator;
	if (magnitude == 0.0f)
	{
		magnitude = 1.0f;
	}
	const glm::vec3 impulse = normal * magnitude;
	if ((ordinary.runtime_flags & kObjectFlagKinematic) == 0
		&& !ripper_ignores_impulse(ordinary))
	{
		accumulate_contact_impulse(
			ordinary, impulse, ordinary_current);
	}
	if ((compound.runtime_flags & kObjectFlagKinematic) == 0
		&& !ripper_ignores_impulse(compound))
	{
		accumulate_contact_impulse(
			compound, -impulse, compound_current);
	}
	return impulse;
}

void synchronize_component_health(
	WorldObject& object,
	const ObjectModelReference& model)
{
	if (model.component_index >= 0
		&& model.component_index < object.component_count)
	{
		object.components[model.component_index].health = model.health;
	}
}

bool same_model_sibling_scope(
	const ObjectModelReference& left,
	const ObjectModelReference& right)
{
	return left.owner_scope == right.owner_scope
		&& left.parent_reference == right.parent_reference;
}

void publish_component_damage_events(
	World& world,
	WorldObject& target,
	std::uint16_t target_index,
	const ObjectModelReference& event_source,
	const ObjectModelReference& damage_model,
	std::uint16_t attacker_index,
	std::uint8_t cause)
{
	if (attacker_index == world.player.index
		&& world.smart_target_enabled
		&& target.allegiance_class == 1)
	{
		if (damage_model.component_index >= 0
			&& damage_model.component_index < target.component_count)
		{
			damage_select_target(
				world, target, damage_model.component_index);
		}
		else if (world.selected_target.index != target_index
			|| world.selected_target.generation != target.generation)
		{
			damage_select_target(world, target, -1);
		}
	}
	if (world.damage_event_suppressed)
	{
		return;
	}
	const WorldObject* attacker =
		attacker_index < kMaxGameObjects
			? &world.objects[attacker_index]
			: nullptr;
	if (cause != 4)
	{
		world_emit_mission_event(
			world,
			WorldMissionEventType::shot_at,
			target,
			attacker,
			UINT8_MAX);
	}
	for (const ObjectModelReference& candidate
		: target.model_references)
	{
		const bool same_proxy =
			event_source.damage_group_selector != 0
				? candidate.damage_group_selector
					== event_source.damage_group_selector
				: candidate.part_group_id
					== event_source.part_group_id;
		if (same_model_sibling_scope(candidate, event_source)
			&& same_proxy
			&& (candidate.source_flags & 0x0002u) != 0
			&& candidate.component_index >= 0
			&& candidate.component_index <= UINT8_MAX)
		{
			world_emit_mission_event(
				world,
				WorldMissionEventType::shot_at,
				target,
				attacker,
				static_cast<std::uint8_t>(
					candidate.component_index));
			break;
		}
	}
}

void apply_collision_component_damage(
	World& world,
	mission::Runtime& mission,
	WorldObject& target,
	std::uint16_t target_index,
	std::uint16_t hit_model_index,
	float raw_damage,
	std::uint16_t attacker_index,
	std::uint8_t cause)
{
	if (hit_model_index >= target.model_references.size()
		|| (target.runtime_flags & kObjectFlagSimulationSuspended) != 0
		|| cause == 2)
	{
		return;
	}
	ObjectModelReference& event_source =
		target.model_references[hit_model_index];
	if ((event_source.source_flags & 0x0004u) != 0)
	{
		return;
	}
	ObjectModelReference* damage_model = &event_source;
	const float damage = scale_damage_by_difficulty(
		world, mission, target, raw_damage, attacker_index);
	const bool damage_owned =
		mission.network.role == mission::NetworkRole::offline
		|| (attacker_index < kMaxGameObjects
			&& mission::network_local_owns_object(
				mission.network,
				attacker_index,
				mission.player_prefix_count,
				world.player.index));
	if (!damage_owned)
	{
		publish_component_damage_events(
			world,
			target,
			target_index,
			event_source,
			*damage_model,
			attacker_index,
			cause);
		return;
	}
	if (damage_model->part_group_id != 0)
	{
		for (std::uint16_t index = 0;
			index < target.model_references.size();
			++index)
		{
			ObjectModelReference& candidate =
				target.model_references[index];
			if (same_model_sibling_scope(candidate, *damage_model)
				&& candidate.part_group_id
					== damage_model->part_group_id
				&& candidate.maximum_health > 0.0f)
			{
				damage_model = &candidate;
				break;
			}
		}
	}
	const bool network_friendly_engine =
		mission.network.role != mission::NetworkRole::offline
		&& attacker_index == world.player.index
		&& target.allegiance_class == 0
		&& damage_model->model_type == 5;
	if (damage_model->maximum_health == 0.0f
		|| network_friendly_engine
		|| (damage_model->maximum_health > 2499.0f
			&& (damage < 500.0f
				|| ((damage_model->source_flags & 0x0200u) == 0
					&& cause != 3
					&& cause != 4))))
	{
		publish_component_damage_events(
			world,
			target,
			target_index,
			event_source,
			*damage_model,
			attacker_index,
			cause);
		return;
	}

	float applied_damage = damage;
	if (target.protection_state != 5
		&& (target.runtime_flags & 0x00004000u) != 0
		&& applied_damage < 1000.0f)
	{
		applied_damage *= 0.25f;
	}
	const float previous_health = damage_model->health;
	// The wire/no-owner sentinel represents signed -1 in retail and must
	// not satisfy the non-player protection comparison.
	const bool attacker_is_non_player =
		attacker_index != UINT16_MAX
		&& attacker_index >= mission.player_prefix_count;
	bool protected_health =
		target.protection_state == 2
		|| (target.protection_state == 1
			&& attacker_is_non_player);
	if (damage_model->component_index >= 0
		&& damage_model->component_index < target.component_count)
	{
		const std::uint16_t protection =
			target.components[
				damage_model->component_index].protection_state;
		protected_health =
			protected_health
			|| protection == 2
			|| (protection == 1 && attacker_is_non_player);
	}
	const float updated_health =
		previous_health - applied_damage;
	const bool protected_crossing =
		updated_health < 0.0f && protected_health;
	if (!protected_crossing)
	{
		damage_model->health = updated_health;
		if (mission.network.role != mission::NetworkRole::offline
			&& damage_model->health < 0.0f
			&& previous_health >= 0.0f
			&& !mission::network_local_owns_object(
				mission.network,
				target_index,
				mission.player_prefix_count,
				world.player.index))
		{
			damage_model->health = 0.0f;
		}
		synchronize_component_health(target, *damage_model);
	}
	target.last_attacker_index = attacker_index;
	if (damage_model->health < 0.0f
		&& mission::network_local_owns_object(
			mission.network,
			target_index,
			mission.player_prefix_count,
			world.player.index))
	{
		target.component_destruction_pending = true;
	}
	if (mission.network.role != mission::NetworkRole::offline)
	{
		// Deferred records retain the full +0xfc ID in retail, while the
		// opcode-0x20/0x21 packet writer emits its low eight bits. There is
		// no saturation: IDs above 255 alias modulo 256 on the wire.
		if (damage_model->network_model_id != UINT32_MAX)
		{
			(void)mission::network_defer_component_damage(
				mission.network,
				target,
				*damage_model,
				damage_model->network_model_id,
				target_index,
				mission.player_prefix_count,
				world.player.index,
				applied_damage);
		}
	}
	publish_component_damage_events(
		world,
		target,
		target_index,
		event_source,
		*damage_model,
		attacker_index,
		cause);
}

void apply_compound_collision_damage(
	World& world,
	mission::Runtime& mission,
	WorldObject& ordinary,
	std::uint16_t ordinary_index,
	std::uint16_t compound_index,
	std::uint8_t bank,
	float damage,
	const glm::vec3& contact,
	const assets::ShipStatsTable& stats,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick)
{
	bool auxiliary_absorbed = false;
	if (ordinary_index == world.player.index
		&& (bank == 2 || bank == 3)
		&& ordinary.auxiliary_shields[bank - 2] > 0.0f)
	{
		float& auxiliary = ordinary.auxiliary_shields[bank - 2];
		auxiliary -= damage;
		if (auxiliary > 0.0f)
		{
			auxiliary_absorbed = true;
		}
		else
		{
			auxiliary = 0.0f;
		}
	}
	if (auxiliary_absorbed)
	{
		return;
	}
	const float scaled_damage = damage * kCollisionDamageScale;
	if (ordinary.primary_shields[bank] < 0.0f)
	{
		(void)apply_structural_bank_damage(
			world,
			mission,
			ordinary,
			stats,
			bank,
			scaled_damage,
			compound_index,
			2,
			impact_feedback_enabled,
			simulation_tick);
		return;
	}
	(void)apply_primary_bank_damage(
		world,
		mission,
		ordinary,
		stats,
		bank,
		scaled_damage,
		1.0f,
		compound_index,
		2,
		impact_feedback_enabled,
		simulation_tick);
	shields_register_hit(
		world,
		ordinary,
		-1,
		contact,
		0,
		simulation_tick);
}

bool compound_sphere_collision(
	World& world,
	mission::Runtime& mission,
	WorldObject& ordinary,
	std::uint16_t ordinary_index,
	WorldObject& compound,
	std::uint16_t compound_index,
	const assets::ShipStatsTable& stats,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick,
	std::uint32_t pass,
	bool response)
{
	service_player_portal_crossings(
		world,
		mission,
		ordinary,
		ordinary_index,
		compound);
	CompoundContact contact =
		query_compound_contact(ordinary, compound);
	if (!contact.hit
		|| contact.model_index >= compound.model_references.size())
	{
		return false;
	}
	if (!response)
	{
		return true;
	}
	const glm::mat4 model_transform =
		model_world_transform(
			compound, contact.model_index, false);
	const glm::vec3 world_contact =
		point_to_world(model_transform, contact.model_point);
	const glm::vec3 world_normal =
		glm::mat3(model_transform) * contact.model_normal;
	const glm::vec3 impulse = accumulate_compound_impulse(
		ordinary,
		compound,
		contact.model_index,
		contact.model_point,
		world_normal,
		pass);
	const std::uint8_t bank =
		collision_bank(ordinary, world_contact);
	const float damage =
		glm::length(impulse)
		* kImpulseDamageScale / ordinary.physics_mass;

	if (ordinary.collision_class == 5)
	{
		std::int16_t active_command = -1;
		if (compound.ai.command_count != 0)
		{
			active_command = compound.ai.commands[0].id;
		}
		if ((compound.runtime_flags & kObjectFlagCompound) != 0
			&& (compound.runtime_flags & 0x20000000u) == 0
			&& active_command != 0x73
			&& active_command != 0x74
			&& active_command != 0x14
			&& active_command != 0x13
			&& active_command != 5
			&& active_command != 4)
		{
			const glm::vec3 ordinary_forward_in_compound =
				glm::transpose(compound.orientation)
				* ordinary.previous_orientation[2];
			const std::int16_t command =
				ordinary_forward_in_compound.x < 0.0f
					? 0x73
					: 0x74;
			(void)ai::command_push(world,
				compound,
				command,
				ai::TargetKind::none,
				UINT16_MAX,
				-1,
				-1,
				-1);
		}
		apply_collision_component_damage(
			world,
			mission,
			compound,
			compound_index,
			contact.model_index,
			5001.0f,
			ordinary_index,
			3);

		const ObjectModelReference& hit_model =
			compound.model_references[contact.model_index];
		const std::uint16_t owner_scope = hit_model.owner_scope;
		bool scope_root =
			hit_model.parent_reference < 0
			|| compound.model_references[
				static_cast<std::uint16_t>(
					hit_model.parent_reference)].owner_scope
					!= owner_scope;
		std::uint16_t structural_model = UINT16_MAX;
		for (std::uint16_t index = 0;
			index < compound.model_references.size();
			++index)
		{
			const ObjectModelReference& candidate =
				compound.model_references[index];
			if (candidate.owner_scope == owner_scope
				&& !candidate.removed
				&& (candidate.runtime_flags & 0x0020u) == 0
				&& candidate.model_type == 1)
			{
				structural_model = index;
				break;
			}
		}
		if (hit_model.part_group_id != 0
			&& hit_model.maximum_health < 1000.0f
			&& structural_model != UINT16_MAX
			&& (!scope_root
				|| (std::strcmp(hit_model.name, "Ulysses Fin") != 0
					&& hit_model.model_type != 1
					&& compound.model_references[
						structural_model].part_group_id
						!= hit_model.part_group_id)))
		{
			apply_collision_component_damage(
				world,
				mission,
				compound,
				compound_index,
				structural_model,
				5001.0f,
				ordinary_index,
				4);
		}
		(void)ai::schedule_death_command(
			ordinary,
			world,
			mission,
			0,
			false);
		return true;
	}

	apply_compound_collision_damage(
		world,
		mission,
		ordinary,
		ordinary_index,
		compound_index,
		bank,
		damage,
		world_contact,
		stats,
		impact_feedback_enabled,
		simulation_tick);
	if (compound.model_references[contact.model_index].forcefield)
	{
		shields_register_hit(
			world,
			compound,
			static_cast<std::int16_t>(contact.model_index),
			world_contact,
			0,
			simulation_tick);
	}
	return true;
}

bool select_compound_collision(
	World& world,
	mission::Runtime& mission,
	WorldObject& first,
	std::uint16_t first_index,
	WorldObject& second,
	std::uint16_t second_index,
	const assets::ShipStatsTable& stats,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick,
	std::uint32_t pass,
	bool response)
{
	WorldObject* ordinary = &second;
	WorldObject* compound = &first;
	std::uint16_t ordinary_index = second_index;
	std::uint16_t compound_index = first_index;
	if ((first.runtime_flags & kObjectFlagCompound) == 0)
	{
		ordinary = &first;
		compound = &second;
		ordinary_index = first_index;
		compound_index = second_index;
	}
	if (compound->type == 0x83)
	{
		service_type83_crossing(
			mission, *ordinary, *compound);
	}
	return compound_sphere_collision(
		world,
		mission,
		*ordinary,
		ordinary_index,
		*compound,
		compound_index,
		stats,
		impact_feedback_enabled,
		simulation_tick,
		pass,
		response);
}

bool standard_sphere_collision(
	World& world,
	mission::Runtime& mission,
	WorldObject& first,
	std::uint16_t first_index,
	WorldObject& second,
	std::uint16_t second_index,
	const assets::ShipStatsTable& stats,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick,
	std::uint32_t pass)
{
	const glm::vec3 normal_first_from_second =
		retail_normalize(first.position - second.position);
	const glm::vec3 contact =
		second.position
		+ normal_first_from_second * second.radius;
	const std::uint8_t first_bank =
		collision_bank(first, contact);
	const std::uint8_t second_bank =
		collision_bank(second, contact);
	const glm::vec3 impulse = accumulate_rigid_sphere_impulse(
		first,
		second,
		normal_first_from_second,
		pass);

	const float lesser_mass =
		first.physics_mass <= second.physics_mass
			? first.physics_mass
			: second.physics_mass;
	float damage =
		std::sqrt(
			impulse.x * impulse.x
			+ impulse.y * impulse.y
			+ impulse.z * impulse.z)
		* kImpulseDamageScale / lesser_mass
		* kCollisionDamageScale;
	if (((first.runtime_flags | second.runtime_flags)
			& kObjectFlagKinematic) != 0)
	{
		damage *= kFixedObjectDamageScale;
	}

	apply_sphere_collision_damage(
		world,
		mission,
		first,
		first_index,
		second_index,
		first_bank,
		damage,
		contact,
		stats,
		true,
		impact_feedback_enabled,
		simulation_tick);
	apply_sphere_collision_damage(
		world,
		mission,
		second,
		second_index,
		first_index,
		second_bank,
		damage,
		contact,
		stats,
		false,
		impact_feedback_enabled,
		simulation_tick);

	collision_reintegrate(first, stats, world.camera_mode);
	collision_reintegrate(second, stats, world.camera_mode);
	const float combined_radius = first.radius + second.radius;
	const glm::vec3 separation = first.position - second.position;
	if (separation.x * separation.x
			+ separation.y * separation.y
			+ separation.z * separation.z
		< combined_radius * combined_radius)
	{
		const glm::vec3 direction =
			retail_normalize(separation);
		const glm::vec3 midpoint =
			(first.position + second.position) * 0.5f;
		const glm::vec3 first_position =
			midpoint
			+ direction * (first.radius * kSeparationScale);
		const glm::vec3 second_position =
			midpoint
			+ direction * (second.radius * -kSeparationScale);
		force_collision_position(first, first_position);
		force_collision_position(second, second_position);
	}
	return true;
}

bool dispatch_collision(
	World& world,
	mission::Runtime& mission,
	std::uint16_t first_index,
	std::uint16_t second_index,
	const assets::ShipStatsTable& stats,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick,
	std::uint32_t pass)
{
	WorldObject* first = &world.objects[first_index];
	WorldObject* second = &world.objects[second_index];
	std::int16_t first_class = first->collision_class;
	std::int16_t second_class = second->collision_class;

	// Collision_dispatch, LANCER.EXE 0x00466170, normalizes class five into
	// the first slot before entering either the ordinary or compound path.
	if (first_class == 5 || second_class == 5)
	{
		if (first->type == second->type)
		{
			return false;
		}
		if (second_class == 5)
		{
			std::swap(first, second);
			std::swap(first_index, second_index);
			std::swap(first_class, second_class);
		}
		if ((first->runtime_flags & kObjectFlagDestroyed) != 0)
		{
			return false;
		}
	}

	const bool first_compound =
		(first->runtime_flags & kObjectFlagCompound) != 0;
	const bool second_compound =
		(second->runtime_flags & kObjectFlagCompound) != 0;
	if (first_compound || second_compound)
	{
		if ((first_compound && second_compound)
			|| first->type == 0xbc
			|| second->type == 0xbc)
		{
			world.damage_event_suppressed = false;
			return false;
		}
		std::uint32_t collision_count = 0;
		world.damage_event_suppressed = false;
		while (collision_count < 9
			&& select_compound_collision(
				world,
				mission,
				*first,
				first_index,
				*second,
				second_index,
				stats,
				impact_feedback_enabled,
				simulation_tick,
				pass,
				true))
		{
			world.damage_event_suppressed = false;
			if (first_class == 5)
			{
				return false;
			}
			collision_reintegrate(
				*first, stats, world.camera_mode);
			collision_reintegrate(
				*second, stats, world.camera_mode);
			++collision_count;
			world.damage_event_suppressed = true;
		}
		world.damage_event_suppressed = false;
		if (collision_count == 0)
		{
			return false;
		}
		return collision_count != 9;
	}

	if (first_class == second_class
		&& (first_class == 5 || first_class == 6))
	{
		return false;
	}
	if (first->type == 0x71 && second->type == 0x71)
	{
		return false;
	}
	if (first_class == 7 || second_class == 7)
	{
		if (first_class != 1 && second_class != 1)
		{
			return false;
		}
		WorldObject* class_seven = first;
		WorldObject* class_one = second;
		std::uint16_t class_one_index = second_index;
		if (second_class == 7)
		{
			std::swap(class_seven, class_one);
			class_one_index = first_index;
		}
		const float damage =
			mission.network.role == mission::NetworkRole::offline
				? 500.0f
				: 5000.0f;
		(void)apply_primary_bank_damage(
			world,
			mission,
			*class_one,
			stats,
			2,
			damage,
			1.0f,
			class_one_index,
			2,
			impact_feedback_enabled,
			simulation_tick);
		(void)ai::schedule_death_command(
			*class_seven,
			world,
			mission,
			0,
			false);
		if (mission.network.role != mission::NetworkRole::offline)
		{
			class_one->last_attacker_index =
				class_one_index
					== class_seven->deathmatch_scenario_counter
				? UINT16_MAX
				: static_cast<std::uint16_t>(
					class_seven->deathmatch_scenario_counter);
		}
		// Retail deliberately reports no resolved collision for this pair.
		return false;
	}
	if (first_class == 5)
	{
		(void)apply_primary_bank_damage(
			world,
			mission,
			*second,
			stats,
			2,
			5001.0f,
			1.0f,
			second_index,
			3,
			impact_feedback_enabled,
			simulation_tick);
		(void)ai::schedule_death_command(
			*first,
			world,
			mission,
			0,
			true);
		return true;
	}
	return standard_sphere_collision(
		world,
		mission,
		*first,
		first_index,
		*second,
		second_index,
		stats,
		impact_feedback_enabled,
		simulation_tick,
		pass);
}
}

void world_service_collisions(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick)
{
	std::array<CollisionRecord, kMaxGameObjects> records{};
	std::array<std::uint16_t, kMaxGameObjects> order{};
	std::size_t count = 0;

	const auto append_object =
		[&](std::uint16_t object_index)
		{
			const WorldObject& object =
				world.objects[object_index];
			if (!object.active
				|| object.type >= 0x100
				|| (object.runtime_flags & 0x00000420u) != 0
				|| (object.runtime_flags & 0x00000004u) != 0)
			{
				return;
			}
			CollisionRecord& record = records[count];
			record.object_index = object_index;
			record.effective_radius =
				object.radius * object.effect_scale;
			record.high_x =
				object.position.x + record.effective_radius;
			order[count] = static_cast<std::uint16_t>(count);
			++count;
		};

	// Retail traverses every ordinary live slot in ascending order and
	// substitutes the separately allocated local player for its final slot.
	for (std::uint16_t object_index = 0;
		object_index < kMaxGameObjects;
		++object_index)
	{
		if (object_index != world.player.index)
		{
			append_object(object_index);
		}
	}
	if (world.player.index < kMaxGameObjects)
	{
		append_object(world.player.index);
	}

	for (std::uint32_t pass = 0;
		pass < kMaximumCollisionPasses;
		++pass)
	{
		bool resolved_collision = false;
		retail_sort_collision_records(order, records, count);
		for (std::size_t first_slot = 0;
			first_slot < count;
			++first_slot)
		{
			const CollisionRecord& first_record =
				records[order[first_slot]];
			const std::uint16_t first_index =
				first_record.object_index;
			WorldObject& first = world.objects[first_index];
			if (!first.active)
			{
				continue;
			}
			const float low_x =
				first.position.x - first_record.effective_radius;
			for (std::size_t second_slot = first_slot + 1;
				second_slot < count;
				++second_slot)
			{
				const CollisionRecord& second_record =
					records[order[second_slot]];
				if (second_record.high_x < low_x)
				{
					break;
				}
				const std::uint16_t second_index =
					second_record.object_index;
				WorldObject& second =
					world.objects[second_index];
				if (!second.active
					|| collision_excluded_by_docking(
						first,
						first_index,
						second,
						second_index)
					|| collision_excluded_by_network_player(
						mission,
						first,
						first_index,
						second,
						second_index))
				{
					continue;
				}
				const glm::vec3 delta =
					first.position - second.position;
				const float combined_radius =
					first.radius + second.radius;
				if (delta.x * delta.x
						+ delta.y * delta.y
						+ delta.z * delta.z
						>= combined_radius * combined_radius
					|| !dispatch_collision(
						world,
						mission,
						first_index,
						second_index,
						stats,
						impact_feedback_enabled,
						simulation_tick,
						pass))
				{
					continue;
				}
				resolved_collision = true;
				// This traversal-slot indexing is a literal retail bug at
				// 0x00469224: it does not use order[first/second_slot].
				records[first_slot].high_x =
					records[first_slot].effective_radius
					+ first.position.x;
				records[second_slot].high_x =
					records[second_slot].effective_radius
					+ second.position.x;
			}
		}
		if (!resolved_collision)
		{
			break;
		}
	}
}
}
