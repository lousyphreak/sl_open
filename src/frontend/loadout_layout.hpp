#pragma once

#include "core/math.hpp"

#include <cmath>
#include <cstdint>

namespace sl_open::frontend
{
inline constexpr float kLoadoutShipScales[12] = {
	0.009f, 0.010f, 0.008f, 0.009f,
	0.0084f, 0.010f, 0.007f, 0.0067f,
	0.006f, 0.0071f, 0.006f, 0.008f,
};
inline constexpr glm::vec3 kLoadoutSelectorPositions[12] = {
	{-98.0f, -23.0f, -1.0f}, {-99.0f, 11.0f, -1.0f},
	{-89.0f, 42.0f, -1.0f}, {-72.0f, 67.0f, -1.0f},
	{-48.0f, 85.0f, -1.0f}, {-18.0f, 97.0f, -1.0f},
	{17.0f, 97.0f, -1.0f}, {48.0f, 85.0f, -1.0f},
	{71.0f, 67.0f, -1.0f}, {89.0f, 42.0f, -1.0f},
	{97.0f, 11.0f, -1.0f}, {96.0f, -23.0f, -1.0f},
};
// LANCER.EXE 0x004ea480; zero denotes the enlarged ship position.
inline constexpr std::int8_t kLoadoutMissileSlots[4][10] = {
	{3, -1, 4, 5, 7, -1, -1, -1, -1, 6},
	{2, 9, 3, 4, 8, 6, -1, 7, -1, 5},
	{1, 8, 2, 3, 7, 5, -1, 6, 9, 4},
	{1, 8, 2, 3, 7, 5, 10, 6, 9, 4},
};

struct LoadoutButtonDefinition
{
	float width;
	float height;
	float x;
	float y;
	float a;
	float b;
	float c;
	float d;
	bool missile_only;
};
// Object/animation order: missiles, launch, ships, guns, default, clear.
inline constexpr LoadoutButtonDefinition kLoadoutButtons[] = {
	{2.33309984f, 1.66649997f, 10.0f, -3.8f,
		0.37890625f, 0.234375f, 0.578125f, 0.47265625f, false},
	{2.33309984f, 1.66649997f, 10.0f, -8.35f,
		0.1796875f, 0.47265625f, 0.37890625f, 0.7109375f, false},
	{2.33309984f, 1.66649997f, 10.0f, -5.8f,
		0.1796875f, 0.0f, 0.37890625f, 0.234375f, false},
	{2.33309984f, 1.66649997f, 10.0f, -1.8f,
		0.37890625f, 0.0f, 0.578125f, 0.234375f, false},
	{2.33309984f, 1.66649997f, 4.2f, -8.35f,
		0.1796875f, 0.7109375f, 0.37890625f, 0.94921875f, true},
	{2.33309984f, 1.66649997f, 6.7f, -8.35f,
		0.1796875f, 0.234375f, 0.37890625f, 0.47265625f, true},
};

inline glm::mat4 loadout_view()
{
	constexpr glm::vec3 eye{-2.8f, -6.5f, -16.85f};
	constexpr glm::vec3 forward{0.06694987f, 0.35894290f, 0.93095529f};
	constexpr glm::vec3 up{-0.02992819f, 0.93335134f, -0.35771443f};
	return glm::lookAtLH(eye, eye + forward, up);
}

inline glm::vec3 loadout_selector_position(std::uint8_t slot)
{
	const glm::vec3& position = kLoadoutSelectorPositions[slot];
	return {1.499999f + position.x * 0.078125f,
		3.749997f + position.z, 1.449999f - position.y * 0.078125f};
}

inline glm::mat3 loadout_selector_orientation(const glm::vec3& position)
{
	// Loadout_orient_models_toward_disc flattens the target's Y first.
	const glm::vec3 direction = glm::normalize(
		glm::vec3{1.499999f, position.y, 1.449999f} - position);
	glm::mat3 orientation = math::postrotate(glm::mat3{1.0f},
		std::atan2(direction.x, direction.z), {0.0f, 1.0f, 0.0f});
	orientation = math::postrotate(orientation,
		glm::pi<float>() - std::atan2(direction.y, glm::dot(orientation[2], direction)),
		{1.0f, 0.0f, 0.0f});
	return math::postrotate(orientation, glm::pi<float>(), {0.0f, 0.0f, 1.0f});
}
}
