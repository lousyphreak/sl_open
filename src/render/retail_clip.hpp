#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/geometric.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace sl_open::render::retail_clip
{
constexpr std::uint8_t kLeftPlane = 0x01u;
constexpr std::uint8_t kRightPlane = 0x02u;
constexpr std::uint8_t kBottomPlane = 0x04u;
constexpr std::uint8_t kTopPlane = 0x08u;
constexpr std::uint8_t kNearPlane = 0x10u;
constexpr std::uint8_t kFrustumPlanes = 0x1fu;
constexpr std::size_t kMaximumPolygonVertices = 64;

struct Frustum
{
	float horizontal_tangent{};
	float vertical_tangent{};
	float near_distance{};
};

struct Vertex
{
	glm::vec3 local_position{0.0f};
	glm::vec3 camera_position{0.0f};
	glm::vec4 diffuse{1.0f};
	glm::vec2 primary_uv{0.0f};
	glm::vec2 environment_uv{0.0f};
};

inline std::uint8_t classify(
	const glm::vec3& position,
	const Frustum& frustum)
{
	std::uint8_t flags = 0;
	if (position.z < frustum.near_distance)
	{
		flags |= kNearPlane;
	}
	if (position.x < -frustum.horizontal_tangent * position.z)
	{
		flags |= kLeftPlane;
	}
	else if (position.x
		> frustum.horizontal_tangent * position.z)
	{
		flags |= kRightPlane;
	}
	if (position.y < -frustum.vertical_tangent * position.z)
	{
		flags |= kBottomPlane;
	}
	else if (position.y
		> frustum.vertical_tangent * position.z)
	{
		flags |= kTopPlane;
	}
	return flags;
}

inline float plane_distance(
	const glm::vec3& position,
	std::uint8_t plane,
	const Frustum& frustum)
{
	switch (plane)
	{
	case kNearPlane:
		return frustum.near_distance - position.z;
	case kLeftPlane:
		return -position.x
			- frustum.horizontal_tangent * position.z;
	case kRightPlane:
		return position.x
			- frustum.horizontal_tangent * position.z;
	case kBottomPlane:
		return -position.y
			- frustum.vertical_tangent * position.z;
	case kTopPlane:
		return position.y
			- frustum.vertical_tangent * position.z;
	default:
		return 0.0f;
	}
}

inline Vertex interpolate(
	const Vertex& from,
	const Vertex& to,
	float fraction)
{
	Vertex output;
	output.local_position =
		from.local_position
			+ (to.local_position - from.local_position) * fraction;
	output.camera_position =
		from.camera_position
			+ (to.camera_position - from.camera_position) * fraction;
	output.diffuse =
		from.diffuse + (to.diffuse - from.diffuse) * fraction;
	output.primary_uv =
		from.primary_uv
			+ (to.primary_uv - from.primary_uv) * fraction;
	output.environment_uv =
		from.environment_uv
			+ (to.environment_uv - from.environment_uv) * fraction;
	return output;
}

class Polygon
{
public:
	bool clip(
		std::span<const Vertex> input,
		const Frustum& frustum)
	{
		if (input.size() < 2
			|| input.size() > kMaximumPolygonVertices)
		{
			clear();
			return false;
		}
		initial_count_ = static_cast<std::uint32_t>(input.size());
		total_count_ = initial_count_;
		live_count_ = initial_count_;
		head_ = 0;
		for (std::uint32_t index = 0;
			index < initial_count_;
			++index)
		{
			Slot& slot = slots_[index];
			slot.vertex = input[index];
			slot.flags = classify(slot.vertex.camera_position, frustum);
			slot.next =
				static_cast<std::int32_t>(
					(index + 1u) % initial_count_);
			slot.previous =
				static_cast<std::int32_t>(
					(index + initial_count_ - 1u)
						% initial_count_);
			slot.live = true;
		}

		// srD3D's SR_clip_face (srd3d.dll 0x1000beb0) clips in this
		// exact order. Generated vertices are appended to a 64-slot pool,
		// while removals only unlink slots; both details affect traversal
		// and floating-point interpolation order.
		for (const std::uint8_t plane : {
			kNearPlane,
			kLeftPlane,
			kRightPlane,
			kBottomPlane,
			kTopPlane,
		})
		{
			for (std::uint32_t slot_index = 0;
				slot_index < total_count_;
				++slot_index)
			{
				if (!slots_[slot_index].live
					|| (slots_[slot_index].flags & plane) == 0)
				{
					continue;
				}
				clip_outside_vertex(slot_index, plane, frustum);
				if (live_count_ < initial_count_)
				{
					clear();
					return false;
				}
			}
		}
		return live_count_ >= initial_count_;
	}

	bool clip_local_half_space(const glm::vec4& plane)
	{
		if (live_count_ < initial_count_)
		{
			clear();
			return false;
		}
		std::array<Vertex, kMaximumPolygonVertices> input;
		std::array<Vertex, kMaximumPolygonVertices> output;
		const std::uint32_t input_count = live_count_;
		for (std::uint32_t index = 0;
			index < input_count;
			++index)
		{
			input[index] = vertex(index);
		}
		std::uint32_t output_count = 0;
		const auto distance =
			[&plane](const Vertex& candidate)
			{
				return glm::dot(
					plane,
					glm::vec4(candidate.local_position, 1.0f));
			};
		for (std::uint32_t index = 0;
			index < input_count;
			++index)
		{
			const Vertex& from =
				input[(index + input_count - 1u) % input_count];
			const Vertex& to = input[index];
			const float from_distance = distance(from);
			const float to_distance = distance(to);
			const bool from_inside = from_distance >= 0.0f;
			const bool to_inside = to_distance >= 0.0f;
			if (from_inside != to_inside)
			{
				if (output_count >= output.size())
				{
					clear();
					return false;
				}
				const float denominator =
					from_distance - to_distance;
				const float fraction =
					denominator != 0.0f
						? from_distance / denominator
						: 0.0f;
				output[output_count++] =
					interpolate(from, to, fraction);
			}
			if (to_inside)
			{
				if (output_count >= output.size())
				{
					clear();
					return false;
				}
				output[output_count++] = to;
			}
		}
		if (output_count < initial_count_)
		{
			clear();
			return false;
		}
		total_count_ = output_count;
		live_count_ = output_count;
		head_ = 0;
		for (std::uint32_t index = 0;
			index < output_count;
			++index)
		{
			Slot& slot = slots_[index];
			slot = {};
			slot.vertex = output[index];
			slot.next = static_cast<std::int32_t>(
				(index + 1u) % output_count);
			slot.previous = static_cast<std::int32_t>(
				(index + output_count - 1u) % output_count);
			slot.live = true;
		}
		return true;
	}

	std::uint32_t size() const
	{
		return live_count_;
	}

	const Vertex& vertex(std::uint32_t ordered_index) const
	{
		std::int32_t slot = head_;
		for (std::uint32_t index = 0;
			index < ordered_index;
			++index)
		{
			slot = slots_[slot].next;
		}
		return slots_[slot].vertex;
	}

private:
	struct Slot
	{
		Vertex vertex;
		std::int32_t next{-1};
		std::int32_t previous{-1};
		std::uint8_t flags{};
		bool live{};
	};

	void clear()
	{
		live_count_ = 0;
		head_ = -1;
	}

	void replace_with_intersection(
		std::uint32_t destination,
		std::uint32_t from,
		std::uint32_t to,
		std::uint8_t plane,
		const Frustum& frustum)
	{
		const float from_distance = plane_distance(
			slots_[from].vertex.camera_position,
			plane,
			frustum);
		float to_distance = plane_distance(
			slots_[to].vertex.camera_position,
			plane,
			frustum);
		// SR_clip_vertex_interpolate clamps a numerically outside endpoint
		// selected as the inside end to the plane before calculating t.
		if (to_distance > 0.0f)
		{
			to_distance = 0.0f;
		}
		const float denominator = from_distance - to_distance;
		const float fraction =
			denominator != 0.0f
				? from_distance / denominator
				: 0.0f;
		slots_[destination].vertex = interpolate(
			slots_[from].vertex,
			slots_[to].vertex,
			fraction);
		slots_[destination].flags =
			static_cast<std::uint8_t>(
				classify(
					slots_[destination].vertex.camera_position,
					frustum)
				& ~plane);
	}

	void remove(std::uint32_t slot_index)
	{
		Slot& slot = slots_[slot_index];
		const std::int32_t next = slot.next;
		const std::int32_t previous = slot.previous;
		if (next == static_cast<std::int32_t>(slot_index))
		{
			clear();
			return;
		}
		if (head_ == static_cast<std::int32_t>(slot_index))
		{
			head_ = next;
		}
		slots_[previous].next = next;
		slots_[next].previous = previous;
		slot.live = false;
		slot.next = -1;
		slot.previous = -1;
		slot.flags = 0;
		--live_count_;
	}

	std::uint32_t insert_before(std::uint32_t slot_index)
	{
		if (total_count_ >= kMaximumPolygonVertices)
		{
			clear();
			return UINT32_MAX;
		}
		const std::uint32_t inserted = total_count_++;
		Slot& current = slots_[slot_index];
		Slot& output = slots_[inserted];
		output = {};
		output.live = true;
		output.next = static_cast<std::int32_t>(slot_index);
		output.previous = current.previous;
		slots_[current.previous].next =
			static_cast<std::int32_t>(inserted);
		current.previous = static_cast<std::int32_t>(inserted);
		++live_count_;
		return inserted;
	}

	void clip_outside_vertex(
		std::uint32_t slot_index,
		std::uint8_t plane,
		const Frustum& frustum)
	{
		if (!slots_[slot_index].live)
		{
			return;
		}
		const float outside_distance = plane_distance(
			slots_[slot_index].vertex.camera_position,
			plane,
			frustum);
		if (outside_distance < 0.0f)
		{
			return;
		}
		const std::uint32_t next =
			static_cast<std::uint32_t>(slots_[slot_index].next);
		if (initial_count_ == 2)
		{
			if ((slots_[next].flags & plane) != 0)
			{
				clear();
				return;
			}
			replace_with_intersection(
				slot_index, slot_index, next, plane, frustum);
			return;
		}

		const std::uint32_t previous =
			static_cast<std::uint32_t>(
				slots_[slot_index].previous);
		if ((slots_[previous].flags & plane) != 0)
		{
			if ((slots_[next].flags & plane) != 0)
			{
				remove(slot_index);
			}
			else
			{
				replace_with_intersection(
					slot_index,
					slot_index,
					next,
					plane,
					frustum);
			}
			return;
		}
		if ((slots_[next].flags & plane) != 0)
		{
			replace_with_intersection(
				slot_index,
				slot_index,
				previous,
				plane,
				frustum);
			return;
		}

		const std::uint32_t inserted = insert_before(slot_index);
		if (inserted == UINT32_MAX)
		{
			return;
		}
		replace_with_intersection(
			inserted,
			slot_index,
			previous,
			plane,
			frustum);
		replace_with_intersection(
			slot_index,
			slot_index,
			next,
			plane,
			frustum);
	}

	std::array<Slot, kMaximumPolygonVertices> slots_{};
	std::uint32_t initial_count_{};
	std::uint32_t total_count_{};
	std::uint32_t live_count_{};
	std::int32_t head_{-1};
};
}
