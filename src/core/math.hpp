#pragma once

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>

static_assert(sizeof(glm::vec3) == sizeof(float) * 3);
static_assert(sizeof(glm::mat3) == sizeof(float) * 9);
static_assert(sizeof(glm::mat4) == sizeof(float) * 16);

namespace sl_open::math
{
// Preserve Surrender's recovered Euler convention and near-gimbal branch.
// Generic GLM Euler helpers use different named rotation orders.
inline glm::mat3 rotation_from_euler(const glm::vec3& euler)
{
	const float sine_x = std::sin(euler.x);
	const float cosine_x = std::cos(euler.x);
	const float sine_y = std::sin(euler.y);
	const float cosine_y = std::cos(euler.y);
	const float sine_z = std::sin(euler.z);
	const float cosine_z = std::cos(euler.z);
	return {
		{
			cosine_z * cosine_y,
			sine_z * cosine_x + cosine_z * sine_y * sine_x,
			sine_z * sine_x - cosine_z * sine_y * cosine_x,
		},
		{
			-sine_z * cosine_y,
			cosine_z * cosine_x - sine_z * sine_y * sine_x,
			cosine_z * sine_x + sine_z * sine_y * cosine_x,
		},
		{
			sine_y,
			-cosine_y * sine_x,
			cosine_y * cosine_x,
		},
	};
}

inline glm::vec3 rotation_to_euler(const glm::mat3& matrix)
{
	const float horizontal = std::sqrt(
		matrix[0][0] * matrix[0][0]
			+ matrix[1][0] * matrix[1][0]);
	if (horizontal > 0.000016f)
	{
		return {
			std::atan2(-matrix[2][1], matrix[2][2]),
			std::atan2(matrix[2][0], horizontal),
			std::atan2(-matrix[1][0], matrix[0][0]),
		};
	}
	return {
		std::atan2(matrix[1][2], matrix[1][1]),
		std::atan2(matrix[2][0], horizontal),
		0.0f,
	};
}

// Object_model_update_scene, LANCER.EXE 0x0049a460. Surrender does not
// quaternion-slerp object roots. It recovers the previous-to-current local
// Euler delta, scales that delta by the four-phase service fraction, and
// composes the partial rotation onto the previous basis.
inline glm::mat3 interpolate_scene_orientation(
	const glm::mat3& previous,
	const glm::mat3& current,
	float fraction)
{
	const glm::mat3 local_delta = glm::transpose(previous) * current;
	return previous
		* rotation_from_euler(rotation_to_euler(local_delta) * fraction);
}

inline glm::mat3 postrotate(
	const glm::mat3& matrix,
	float angle,
	const glm::vec3& axis)
{
	return glm::mat3(glm::rotate(glm::mat4(matrix), angle, axis));
}

inline glm::mat4 model_transform(
	const glm::mat3& orientation,
	float scale,
	const glm::vec3& translation)
{
	glm::mat4 transform{orientation};
	transform[0] *= scale;
	transform[1] *= scale;
	transform[2] *= scale;
	transform[3] = glm::vec4(translation, 1.0f);
	return transform;
}

// This matches bx::mtxSRT: translation followed by negative X/Y/Z rotations
// and local-axis scale in the matrix consumed by bgfx.
inline glm::mat4 srt(
	const glm::vec3& scale,
	const glm::vec3& euler,
	const glm::vec3& translation)
{
	glm::mat4 transform = glm::translate(glm::mat4{1.0f}, translation);
	transform = glm::rotate(
		transform, -euler.x, glm::vec3{1.0f, 0.0f, 0.0f});
	transform = glm::rotate(
		transform, -euler.y, glm::vec3{0.0f, 1.0f, 0.0f});
	transform = glm::rotate(
		transform, -euler.z, glm::vec3{0.0f, 0.0f, 1.0f});
	return glm::scale(transform, scale);
}

inline glm::mat4 perspective_lh(
	float vertical_fov_radians,
	float aspect,
	float near_plane,
	float far_plane,
	bool homogeneous_depth)
{
	return homogeneous_depth
		? glm::perspectiveLH_NO(
			vertical_fov_radians, aspect, near_plane, far_plane)
		: glm::perspectiveLH_ZO(
			vertical_fov_radians, aspect, near_plane, far_plane);
}

// Surrender projects positive camera-space Y toward increasing framebuffer Y.
// GLM's perspective matrices instead project positive Y toward the top of the
// viewport, so reproduce the retail screen convention at the projection
// boundary without reflecting the simulation or camera bases.
inline glm::mat4 perspective_lh_framebuffer_y_down(
	float vertical_fov_radians,
	float aspect,
	float near_plane,
	float far_plane,
	bool homogeneous_depth)
{
	glm::mat4 projection = perspective_lh(
		vertical_fov_radians,
		aspect,
		near_plane,
		far_plane,
		homogeneous_depth);
	projection[1][1] = -projection[1][1];
	return projection;
}

// SR_mesh_project, LANCER.EXE 0x004c6c00/0x004c6d40. Surrender stores
// projected depth as near_z / camera_z: the near plane is 1 and distance
// tends toward 0 with no finite far plane. Keep positive camera-space Y
// pointing toward increasing framebuffer Y at this same boundary.
inline glm::mat4 perspective_lh_reverse_infinite_framebuffer_y_down(
	float vertical_fov_radians,
	float aspect,
	float near_plane,
	bool homogeneous_depth)
{
	const float vertical_scale =
		1.0f / std::tan(vertical_fov_radians * 0.5f);
	glm::mat4 projection{0.0f};
	projection[0][0] = vertical_scale / aspect;
	projection[1][1] = -vertical_scale;
	projection[2][2] = homogeneous_depth ? -1.0f : 0.0f;
	projection[2][3] = 1.0f;
	projection[3][2] = homogeneous_depth
		? 2.0f * near_plane
		: near_plane;
	return projection;
}

// Render mission geometry relative to the camera before converting to the
// float matrices consumed by bgfx. Retail transforms world positions into
// camera space on the CPU; retaining 10-million-unit world translations in
// both the model and view matrices loses visible sub-object precision.
inline glm::mat4 camera_relative_transform(
	glm::mat4 world_transform,
	const glm::vec3& camera_position)
{
	world_transform[3].x -= camera_position.x;
	world_transform[3].y -= camera_position.y;
	world_transform[3].z -= camera_position.z;
	return world_transform;
}

inline glm::mat4 camera_rotation_view(const glm::mat3& camera_orientation)
{
	return glm::mat4(glm::transpose(camera_orientation));
}

// Conservative sphere visibility helper for independently culled environment
// objects. Gameplay model roots use their sphere only to request detailed
// per-LOD clipping; Object_model_tree_render (LANCER.EXE 0x0049b390) does not
// reject the model tree at this stage.
inline bool model_bounding_sphere_visible(
	const glm::vec3& camera_space_center,
	float radius,
	float horizontal_tangent,
	float vertical_tangent,
	float horizontal_pixel_scale,
	float near_plane)
{
	if (horizontal_pixel_scale * radius < camera_space_center.z
		|| radius < near_plane - camera_space_center.z)
	{
		return false;
	}
	const float horizontal_extent =
		camera_space_center.z * horizontal_tangent
		+ radius * std::sqrt(
			1.0f + horizontal_tangent * horizontal_tangent);
	const float vertical_extent =
		camera_space_center.z * vertical_tangent
		+ radius * std::sqrt(
			1.0f + vertical_tangent * vertical_tangent);
	return std::abs(camera_space_center.x) <= horizontal_extent
		&& std::abs(camera_space_center.y) <= vertical_extent;
}

// SR_mesh_project, LANCER.EXE 0x004c6c00. Inputs are the camera-plane
// coordinates after perspective divide and the positive horizontal/vertical
// projection bounds. Unlike an OpenGL viewport conversion, positive Y maps
// directly toward increasing framebuffer Y.
inline glm::vec2 camera_plane_to_framebuffer(
	const glm::vec2& camera_plane,
	const glm::vec2& half_extents,
	const glm::vec2& framebuffer_size)
{
	return (camera_plane / half_extents + glm::vec2{1.0f})
		* 0.5f * framebuffer_size;
}

inline glm::mat4 orthographic_lh(
	float left,
	float right,
	float bottom,
	float top,
	float near_plane,
	float far_plane,
	bool homogeneous_depth)
{
	return homogeneous_depth
		? glm::orthoLH_NO(left, right, bottom, top, near_plane, far_plane)
		: glm::orthoLH_ZO(left, right, bottom, top, near_plane, far_plane);
}
}
