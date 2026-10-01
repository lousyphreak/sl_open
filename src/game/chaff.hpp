#pragma once

#include "assets/missile_stats.hpp"
#include "assets/pilot_stats.hpp"
#include "game/world.hpp"

#include <cstdint>

namespace sl_open::game
{
struct MissileRuntime;

constexpr std::uint32_t kMaxChaff = 100;
constexpr std::uint32_t kMaxChaffParticles = 1000;

struct ChaffRecord
{
	glm::vec3 position{0.0f};
	glm::vec3 velocity{0.0f};
	glm::mat3 orientation{1.0f};
	std::uint32_t expiry_tick{};
	std::uint16_t source_index{UINT16_MAX};
	bool active{};
};

struct ChaffParticle
{
	glm::vec3 position{0.0f};
	glm::vec3 velocity{0.0f};
	std::uint32_t birth_tick{};
	std::uint16_t lifetime_ticks{};
	bool active{};
};

struct ChaffRuntime
{
	ChaffRecord records[kMaxChaff];
	ChaffParticle particles[kMaxChaffParticles];
	glm::vec3 model_bounds_min{0.0f};
	glm::vec3 model_bounds_max{0.0f};
	std::uint16_t particle_high_water{};
	std::uint16_t live_count{};
	bool model_bounds_ready{};
	bool pool_exhaustion_reported{};
};

void chaff_reset(ChaffRuntime& runtime);
void chaff_set_model_bounds(
	ChaffRuntime& runtime,
	const glm::vec3& minimum,
	const glm::vec3& maximum);
bool chaff_deploy(
	ChaffRuntime& runtime,
	MissileRuntime& missiles,
	World& world,
	const assets::MissileStatsTable& missile_stats,
	const assets::PilotStatsTable& pilot_stats,
	ObjectHandle source,
	std::uint32_t simulation_tick,
	std::uint32_t& random_seed);
void chaff_step(
	ChaffRuntime& runtime,
	MissileRuntime& missiles,
	World& world,
	std::uint32_t simulation_tick,
	std::uint32_t elapsed_ticks,
	const glm::vec3& camera_position,
	const glm::mat3& camera_orientation,
	std::uint32_t& random_seed);
const ChaffRecord* chaff_resolve(
	const ChaffRuntime& runtime,
	std::int16_t slot);
void chaff_destroy(
	ChaffRuntime& runtime,
	MissileRuntime& missiles,
	World& world,
	std::int16_t slot,
	std::uint32_t simulation_tick,
	const char* reason);
}
