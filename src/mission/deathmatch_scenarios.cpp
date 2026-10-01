#include "mission/deathmatch_scenarios.hpp"

#include "ai/runtime.hpp"
#include "assets/ship_stats.hpp"
#include "core/mission_log.hpp"
#include "game/attachments.hpp"
#include "game/disruption_effects.hpp"
#include "game/world.hpp"
#include "hud/runtime.hpp"
#include "localization/language.hpp"
#include "mission/network_runtime.hpp"
#include "mission/runtime.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>

namespace sl_open::mission
{
namespace
{
constexpr std::uint16_t kScenarioMissions[6]{
	85, 82, 81, 83, 84, 87,
};
constexpr std::uint16_t kBeaconType = 0x8e;
constexpr std::uint16_t kBeaconGateType = 0x8d;
constexpr std::uint16_t kPowerupType = 0x8c;
constexpr std::uint16_t kProximityMineType = 0x6f;
constexpr std::uint16_t kRelayType = 0xd7;
constexpr std::uint16_t kBeaconSpawnType = 992;
constexpr std::uint16_t kDarkTowerType = 0x48;
constexpr std::uint32_t kTargetableFlag = 0x00000200u;
constexpr std::uint8_t kBroadcastDestination = UINT8_MAX;

enum class DeathmatchPowerup : std::int8_t
{
	invulnerability = 0,
	cloak = 1,
	proximity_mine = 2,
	fuel = 3,
	repair = 4,
	missile = 5,
	countermeasure = 6,
	half_maximum_speed = 7,
	shields_down = 8,
	reverse_yoke = 9,
};

constexpr std::uint8_t kPowerupFlagActive = 0x01;
constexpr std::uint8_t kPowerupFlagAutomatic = 0x02;
constexpr std::uint8_t kPowerupFlagCountdown = 0x04;

struct DeathmatchPowerupDefinition
{
	std::uint8_t shape{};
	std::uint8_t flags{};
	std::uint8_t bar_offset{};
	std::uint32_t duration{};
	std::uint16_t weight{};
};

// DMPowerup's 0x0050c508 table. The callbacks are represented explicitly
// below, but every authored record value is retained here so selection,
// activation, expiration, and HUD behavior share one source of truth.
constexpr DeathmatchPowerupDefinition kPowerups[kDeathmatchPowerupCount]{
	{2, 0x07, 0, 1500, 100},
	{0, 0x07, 0, 1500, 100},
	{4, 0x01, 0, UINT32_MAX, 100},
	{6, 0x03, 0, 200, 250},
	{5, 0x03, 0, 200, 30},
	{1, 0x01, 0, UINT32_MAX, 150},
	{9, 0x03, 0, 200, 100},
	{8, 0x0f, 6, 2500, 20},
	{3, 0x0f, 2, 1500, 20},
	{7, 0x0f, 6, 1000, 20},
};

struct BitWriter
{
	std::uint8_t* bytes{};
	std::size_t capacity{};
	std::uint16_t bit{};
	bool valid{true};

	void write(std::uint32_t value, std::uint8_t width)
	{
		if (!valid || static_cast<std::size_t>(bit) + width
				> capacity * 8u)
		{
			valid = false;
			return;
		}
		for (std::uint8_t index = 0; index < width; ++index)
		{
			const std::uint16_t destination =
				static_cast<std::uint16_t>(bit + index);
			if ((value & (1u << index)) != 0)
			{
				bytes[destination >> 3] = static_cast<std::uint8_t>(
					bytes[destination >> 3]
					| (1u << (destination & 7u)));
			}
		}
		bit = static_cast<std::uint16_t>(bit + width);
	}
};

struct BitReader
{
	const std::uint8_t* bytes{};
	std::size_t byte_count{};
	std::uint16_t bit_count{};
	std::uint16_t bit{};
	bool valid{true};

	std::uint32_t read(std::uint8_t width)
	{
		if (!valid || static_cast<std::uint32_t>(bit) + width
				> bit_count
			|| static_cast<std::size_t>(bit + width + 7u) / 8u
				> byte_count)
		{
			valid = false;
			return 0;
		}
		std::uint32_t value = 0;
		for (std::uint8_t index = 0; index < width; ++index)
		{
			const std::uint16_t source =
				static_cast<std::uint16_t>(bit + index);
			if ((bytes[source >> 3] & (1u << (source & 7u))) != 0)
			{
				value |= 1u << index;
			}
		}
		bit = static_cast<std::uint16_t>(bit + width);
		return value;
	}
};

DeathmatchScenario selected_scenario(const Runtime& runtime)
{
	const std::int8_t selected = runtime.network.deathmatch_scenario;
	return selected >= 0 && selected < 6
		? static_cast<DeathmatchScenario>(selected)
		: DeathmatchScenario::none;
}

bool is_authority(const NetworkRuntime& network)
{
	return network.role != NetworkRole::client;
}

bool valid_player_index(std::uint16_t player)
{
	return player < kDeathmatchScenarioPlayerCapacity;
}

bool connected_player(
	const Runtime& runtime,
	const game::World& world,
	std::uint16_t player)
{
	return valid_player_index(player)
		&& player < runtime.network.player_count
		&& runtime.network.connected[player]
		&& player < std::size(world.objects)
		&& world.objects[player].active;
}

std::uint8_t connected_player_count(const Runtime& runtime)
{
	std::uint8_t count = 0;
	for (std::uint8_t player = 0;
		player < kDeathmatchScenarioPlayerCapacity;
		++player)
	{
		if (player < runtime.network.player_count
			&& runtime.network.connected[player])
		{
			++count;
		}
	}
	return count;
}

void queue_event(
	DeathmatchScenarioState& state,
	DeathmatchScenarioMessageKind kind,
	std::int8_t primary = -1,
	std::int8_t secondary = -1)
{
	if (state.message_count == kDeathmatchScenarioMessageCapacity)
	{
		std::move(
			std::begin(state.messages) + 1,
			std::end(state.messages),
			std::begin(state.messages));
		--state.message_count;
	}
	state.messages[state.message_count++] = {
		kind, primary, secondary,
	};
}

std::uint16_t find_first_type(
	const game::World& world,
	std::uint16_t type,
	bool hidden_only = false)
{
	for (std::uint16_t index = 0;
		index < std::size(world.objects);
		++index)
	{
		const game::WorldObject& object = world.objects[index];
		if (object.active && object.type == type
			&& (!hidden_only
				|| (object.runtime_flags & game::kObjectFlagDisabled) != 0))
		{
			return index;
		}
	}
	return UINT16_MAX;
}

std::uint8_t collect_type(
	const game::World& world,
	std::uint16_t type,
	std::uint16_t* output,
	std::uint8_t capacity)
{
	std::uint8_t count = 0;
	for (std::uint16_t index = 0;
		index < std::size(world.objects) && count < capacity;
		++index)
	{
		if (world.objects[index].active
			&& world.objects[index].type == type)
		{
			output[count++] = index;
		}
	}
	return count;
}

void set_hidden(game::WorldObject& object, bool hidden)
{
	if (hidden)
	{
		object.runtime_flags |= game::kObjectFlagDisabled;
	}
	else
	{
		object.runtime_flags &= ~game::kObjectFlagDisabled;
	}
}

void compute_arena(
	DeathmatchScenarioState& state,
	const game::World& world)
{
	glm::vec3 minimum{std::numeric_limits<float>::max()};
	glm::vec3 maximum{-std::numeric_limits<float>::max()};
	bool found = false;
	for (std::uint16_t index = kDeathmatchScenarioPlayerCapacity;
		index < std::size(world.objects);
		++index)
	{
		const game::WorldObject& object = world.objects[index];
		const std::uint16_t type = object.type;
		if (!object.active
			|| type == 0x6f
			|| type <= 0x0b
			|| type == kBeaconType
			|| type >= 1000
			|| (type >= 0x5f && type <= 0x6a)
			|| (type >= 0xc9 && type <= 0xd3))
		{
			continue;
		}
		const glm::vec3 radius{object.radius};
		minimum = glm::min(minimum, object.position - radius);
		maximum = glm::max(maximum, object.position + radius);
		found = true;
	}
	if (!found)
	{
		state.arena_center = {};
		state.arena_radius = 0.0f;
		return;
	}
	state.arena_center = (minimum + maximum) * 0.5f;
	state.arena_radius = 0.0f;
	for (std::uint16_t index = kDeathmatchScenarioPlayerCapacity;
		index < std::size(world.objects);
		++index)
	{
		const game::WorldObject& object = world.objects[index];
		const std::uint16_t type = object.type;
		if (!object.active
			|| type == 0x6f
			|| type <= 0x0b
			|| type == kBeaconType
			|| type >= 1000
			|| (type >= 0x5f && type <= 0x6a)
			|| (type >= 0xc9 && type <= 0xd3))
		{
			continue;
		}
		state.arena_radius = std::max(
			state.arena_radius,
			glm::distance(state.arena_center, object.position)
				+ object.radius);
	}
}

bool queue_scenario_message(
	Runtime& runtime,
	NetworkGameplayOpcode opcode,
	NetworkDelivery delivery,
	std::uint8_t destination,
	std::int8_t player = -1,
	std::int8_t secondary = -1,
	std::uint16_t object = UINT16_MAX,
	bool flag = false)
{
	if (runtime.network.role == NetworkRole::offline)
	{
		return false;
	}
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode = opcode;
	message.delivery = delivery;
	message.source_player = runtime.network.local_player;
	message.destination_player = destination;
	message.scenario_player = player;
	message.scenario_secondary_player = secondary;
	message.scenario_object = object;
	message.scenario_flag = flag;
	return network_queue_gameplay_message(runtime.network, message);
}

bool queue_broadcast(
	Runtime& runtime,
	NetworkGameplayOpcode opcode,
	std::int8_t player = -1,
	std::int8_t secondary = -1,
	std::uint16_t object = UINT16_MAX,
	bool flag = false)
{
	return queue_scenario_message(
		runtime,
		opcode,
		NetworkDelivery::broadcast_guaranteed,
		kBroadcastDestination,
		player,
		secondary,
		object,
		flag);
}

void publish_forced_object_state(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t object_index)
{
	if (runtime.network.role == NetworkRole::offline
		|| object_index >= std::size(world.objects)
		|| !world.objects[object_index].active)
	{
		return;
	}
	// PickupGenericDrop, DMPowerup proximity-mine creation and the
	// out-of-arena reset owner all set the two sticky transform bits before
	// their forced, guaranteed, absolute opcode-0x1d publication.
	world.objects[object_index].state_publication_flags |=
		game::kObjectStatePublishTransform;
	(void)network_publish_object_state(
		runtime.network,
		world,
		stats,
		object_index,
		-1,
		true,
		true);
}

void publish_drop_pickup(
	Runtime& runtime,
	std::uint16_t type)
{
	if (runtime.network.role == NetworkRole::offline)
	{
		return;
	}
	// DPGMESSAGE_DROPPICKUP (0x4f) follows the guaranteed forced state
	// sample and carries only the nine-bit object type. Receivers use the
	// same first-hidden-object search before exposing that synchronized
	// object.
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode =
		NetworkGameplayOpcode::deathmatch_reposition_object;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = runtime.network.local_player;
	message.destination_player = kBroadcastDestination;
	message.scenario_object = type;
	(void)network_queue_gameplay_message(runtime.network, message);
}

void publish_unhide(
	Runtime& runtime,
	std::uint16_t object)
{
	(void)queue_broadcast(
		runtime,
		NetworkGameplayOpcode::deathmatch_unhide_object,
		-1,
		-1,
		object);
}

void unhide_object(
	Runtime& runtime,
	game::World& world,
	std::uint16_t object,
	bool publish)
{
	if (object >= std::size(world.objects)
		|| !world.objects[object].active)
	{
		return;
	}
	set_hidden(world.objects[object], false);
	world.objects[object].visible = true;
	world.objects[object].targetable = true;
	world.objects[object].runtime_flags |= kTargetableFlag;
	if (publish)
	{
		publish_unhide(runtime, object);
	}
}

bool valid_powerup(std::int8_t powerup)
{
	return powerup >= 0
		&& powerup < static_cast<std::int8_t>(kDeathmatchPowerupCount);
}

std::uint32_t powerup_expiry_tick(
	std::uint32_t simulation_tick,
	std::uint32_t duration_ticks)
{
	return duration_ticks == UINT32_MAX
		? UINT32_MAX
		: simulation_tick + duration_ticks;
}

void publish_powerup_activation(
	Runtime& runtime,
	std::uint8_t player,
	std::uint8_t powerup)
{
	if (runtime.network.role == NetworkRole::offline)
	{
		return;
	}
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode =
		NetworkGameplayOpcode::deathmatch_powerup_activate;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = runtime.network.local_player;
	message.destination_player = kBroadcastDestination;
	message.scenario_player = static_cast<std::int8_t>(player);
	message.scenario_powerup = static_cast<std::int8_t>(powerup);
	(void)network_queue_gameplay_message(runtime.network, message);
}

void publish_proximity_mine(
	Runtime& runtime,
	std::uint8_t player,
	std::uint16_t mine)
{
	if (runtime.network.role == NetworkRole::offline)
	{
		return;
	}
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode =
		NetworkGameplayOpcode::deathmatch_proximity_mine;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = runtime.network.local_player;
	message.destination_player = kBroadcastDestination;
	message.scenario_player = static_cast<std::int8_t>(player);
	message.scenario_object = mine;
	(void)network_queue_gameplay_message(runtime.network, message);
}

void initialize_powerup_pool(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats)
{
	DeathmatchScenarioState& state = runtime.deathmatch;
	if (state.powerup_pool_initialized
		|| (!runtime.network.deathmatch_mode
			&& !network_is_deathmatch_mission(runtime.mission_number)))
	{
		return;
	}
	state.powerup_pool_initialized = true;
	state.mine_pool_count = 0;
	state.mine_pool_cursor = 0;
	for (std::uint16_t& mine : state.mine_pool)
	{
		mine = UINT16_MAX;
	}

	for (std::uint8_t ordinal = 0;
		ordinal < kDeathmatchMinePoolCapacity;
		++ordinal)
	{
		const game::ObjectHandle handle = game::world_create(
			world,
			kProximityMineType,
			glm::vec3{0.0f},
			glm::mat3{1.0f},
			stats,
			false);
		game::WorldObject* mine = game::world_resolve(world, handle);
		if (mine == nullptr)
		{
			diagnostics::mission_log(
				"deathmatch mine pool truncated requested=%u created=%u",
				static_cast<unsigned>(kDeathmatchMinePoolCapacity),
				static_cast<unsigned>(state.mine_pool_count));
			break;
		}
		set_hidden(*mine, true);
		mine->visible = false;
		mine->targetable = false;
		mine->runtime_flags &= ~kTargetableFlag;
		state.mine_pool[state.mine_pool_count++] = handle.index;
	}
}

std::int8_t select_powerup(Runtime& runtime, game::World& world)
{
	const bool omit_cloak =
		selected_scenario(runtime) == DeathmatchScenario::hunt_the_shadow;
	std::uint32_t total = 0;
	for (std::uint8_t index = 0;
		index < kDeathmatchPowerupCount;
		++index)
	{
		if (!omit_cloak
			|| index != static_cast<std::uint8_t>(
				DeathmatchPowerup::cloak))
		{
			total += kPowerups[index].weight;
		}
	}
	if (total == 0)
	{
		return static_cast<std::int8_t>(
			DeathmatchPowerup::invulnerability);
	}
	const float sample =
		static_cast<float>(game::world_rand15(world)) / 32768.0f;
	float threshold = 0.0f;
	for (std::uint8_t index = 0;
		index < kDeathmatchPowerupCount;
		++index)
	{
		if (omit_cloak
			&& index == static_cast<std::uint8_t>(
				DeathmatchPowerup::cloak))
		{
			continue;
		}
		threshold += static_cast<float>(kPowerups[index].weight)
			/ static_cast<float>(total);
		if (sample < threshold)
		{
			return static_cast<std::int8_t>(index);
		}
	}
	// DMPowerup_select's sentinel record resolves a floating-point
	// fallthrough to record zero.
	return static_cast<std::int8_t>(
		DeathmatchPowerup::invulnerability);
}

void expire_powerup_effect(
	game::World& world,
	std::uint8_t player,
	std::int8_t powerup,
	std::uint32_t simulation_tick)
{
	if (!valid_player_index(player)
		|| player >= std::size(world.objects)
		|| !world.objects[player].active)
	{
		return;
	}
	game::WorldObject& object = world.objects[player];
	switch (static_cast<DeathmatchPowerup>(powerup))
	{
	case DeathmatchPowerup::invulnerability:
		object.protection_state = 0;
		break;
	case DeathmatchPowerup::cloak:
		(void)game::world_force_decloak(
			world, object, simulation_tick);
		break;
	default:
		break;
	}
}

bool recreate_mine(
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t index,
	const glm::vec3& position,
	const glm::mat3& orientation,
	game::WorldObject*& mine)
{
	if (index >= std::size(world.objects))
	{
		return false;
	}
	if (world.objects[index].active)
	{
		const game::ObjectHandle old{
			index, world.objects[index].generation};
		if (!game::world_destroy(world, old))
		{
			return false;
		}
	}
	const game::ObjectHandle handle = game::world_create_at(
		world,
		index,
		kProximityMineType,
		position,
		orientation,
		stats,
		false);
	mine = game::world_resolve(world, handle);
	return mine != nullptr;
}

void materialize_proximity_mine(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint8_t player,
	std::uint16_t requested_mine,
	const glm::vec3* received_position,
	const glm::mat3* received_orientation,
	bool publish)
{
	DeathmatchScenarioState& state = runtime.deathmatch;
	if (!valid_player_index(player)
		|| player >= std::size(world.objects)
		|| !world.objects[player].active
		|| state.mine_pool_count == 0)
	{
		return;
	}

	std::uint8_t pool_ordinal = state.mine_pool_cursor;
	if (requested_mine != UINT16_MAX)
	{
		bool found = false;
		for (std::uint8_t ordinal = 0;
			ordinal < state.mine_pool_count;
			++ordinal)
		{
			if (state.mine_pool[ordinal] == requested_mine)
			{
				pool_ordinal = ordinal;
				found = true;
				break;
			}
		}
		if (!found)
		{
			return;
		}
	}
	const std::uint16_t mine_index = state.mine_pool[pool_ordinal];
	if (mine_index >= std::size(world.objects))
	{
		return;
	}

	const game::WorldObject& owner = world.objects[player];
	glm::vec3 position;
	glm::mat3 orientation;
	if (received_position != nullptr
		&& received_orientation != nullptr)
	{
		position = *received_position;
		orientation = *received_orientation;
	}
	else
	{
		const float mine_radius = world.objects[mine_index].active
			? world.objects[mine_index].radius
			: 0.0f;
		position = owner.position
			+ owner.orientation[2]
				* ((owner.radius + mine_radius) * 0.5f);
		orientation = owner.orientation;
	}

	game::WorldObject* mine = nullptr;
	if (!recreate_mine(
			world,
			stats,
			mine_index,
			position,
			orientation,
			mine))
	{
		return;
	}
	// DMPowerup_materialize_proximity_mine recreates retail type 0x6f,
	// copies the owner's basis, records the three-bit owner at +0x764, and
	// clears every retained linear/angular control field.
	mine->runtime_flags = 0;
	mine->visible = true;
	mine->targetable = false;
	mine->deathmatch_scenario_counter = player;
	mine->last_attacker_index = UINT16_MAX;
	game::world_zero_motion_controls(*mine);
	state.powerup_active[player] = -1;
	state.powerup_expiry[player] = UINT32_MAX;
	state.mine_pool_cursor = static_cast<std::uint8_t>(
		(pool_ordinal + 1u) % state.mine_pool_count);
	if (publish)
	{
		publish_forced_object_state(
			runtime,
			world,
			stats,
			mine_index);
		publish_proximity_mine(
			runtime, player, mine_index);
	}
}

void activate_powerup_effect(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint8_t player,
	std::int8_t powerup,
	std::uint32_t simulation_tick,
	bool publish)
{
	if (!valid_powerup(powerup)
		|| !valid_player_index(player)
		|| player >= std::size(world.objects)
		|| !world.objects[player].active)
	{
		return;
	}
	game::WorldObject& object = world.objects[player];
	switch (static_cast<DeathmatchPowerup>(powerup))
	{
	case DeathmatchPowerup::invulnerability:
		object.protection_state = 4;
		break;
	case DeathmatchPowerup::cloak:
		(void)game::world_force_cloak(
			world, object, simulation_tick);
		break;
	case DeathmatchPowerup::proximity_mine:
		if (is_authority(runtime.network))
		{
			materialize_proximity_mine(
				runtime,
				world,
				stats,
				player,
				UINT16_MAX,
				nullptr,
				nullptr,
				true);
		}
		break;
	case DeathmatchPowerup::fuel:
		if (object.type < assets::kShipStatsCount)
		{
			const std::int32_t maximum =
				stats.records[object.type].object.afterburner_seconds * 100;
			object.afterburner_fuel = std::min(
				maximum,
				object.afterburner_fuel
					+ static_cast<std::int32_t>(
						std::trunc(
							static_cast<float>(maximum) * 0.25f)));
		}
		break;
	case DeathmatchPowerup::repair:
		if (object.type < assets::kShipStatsCount)
		{
			const assets::ObjectTypeStats& values =
				stats.records[object.type].object;
			const float primary =
				static_cast<float>(values.primary_bank_max * 6 - 1);
			const float structural = static_cast<float>(
				values.structural_bank_max * 6 - 1);
			std::fill(
				std::begin(object.primary_shields),
				std::end(object.primary_shields),
				primary);
			std::fill(
				std::begin(object.secondary_shields),
				std::end(object.secondary_shields),
				structural);
			game::world_update_shield_ratios(object, stats);
		}
		break;
	case DeathmatchPowerup::missile:
		runtime.deathmatch.powerup_active[player] = -1;
		runtime.deathmatch.powerup_expiry[player] = UINT32_MAX;
		break;
	case DeathmatchPowerup::countermeasure:
		object.chaff_count = static_cast<std::int16_t>(
			object.chaff_count + 3);
		break;
	case DeathmatchPowerup::shields_down:
		std::fill(
			std::begin(object.primary_shields),
			std::end(object.primary_shields),
			0.0f);
		game::world_update_shield_ratios(object, stats);
		break;
	case DeathmatchPowerup::half_maximum_speed:
	case DeathmatchPowerup::reverse_yoke:
		break;
	}
	(void)publish;
}

void trigger_powerup(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint8_t player,
	std::int8_t powerup,
	std::uint32_t simulation_tick,
	bool publish)
{
	if (!valid_powerup(powerup)
		|| !valid_player_index(player)
		|| player >= std::size(world.objects)
		|| !world.objects[player].active)
	{
		return;
	}
	DeathmatchScenarioState& state = runtime.deathmatch;
	const DeathmatchPowerupDefinition& definition =
		kPowerups[static_cast<std::uint8_t>(powerup)];
	state.powerup_active[player] = powerup;
	state.powerup_expiry[player] =
		powerup_expiry_tick(simulation_tick, definition.duration);

	// Manual effects return their physical pickup immediately before the
	// activation callback. Automatic effects retain it until expiration.
	if ((definition.flags & kPowerupFlagAutomatic) == 0)
	{
		unhide_object(
			runtime,
			world,
			state.powerup_pickup[player],
			player == runtime.network.local_player);
	}
	activate_powerup_effect(
		runtime,
		world,
		stats,
		player,
		powerup,
		simulation_tick,
		publish);
	if (publish)
	{
		publish_powerup_activation(
			runtime,
			player,
			static_cast<std::uint8_t>(powerup));
	}
}

void install_missile_powerup(
	Runtime& runtime,
	game::WorldObject& object,
	std::uint8_t player)
{
	// DMPowerup_Missile_pickup (0x004b1f30) discards every selected
	// hardpoint definition, retains only slot zero, installs definition zero
	// (one Screamer), rebuilds its deathmatch model, and publishes a one-round
	// HUD inventory.
	game::attachments_destroy_live_models(object);
	object.attachment_count = 1;
	object.attachments[0].definition_index = 0;
	object.attachments[0].kind = 0;
	object.attachments[0].remaining_count = 1;
	for (std::uint8_t index = 1;
		index < std::size(object.attachments);
		++index)
	{
		object.attachments[index].definition_index = -1;
		object.attachments[index].kind = -1;
		object.attachments[index].remaining_count = 0;
	}
	game::attachments_build_live_models(object, true, true);
	if (player == runtime.network.local_player)
	{
		++runtime.player_ordnance_rebuild_serial;
	}
}

void assign_powerup(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint8_t player,
	std::uint16_t pickup,
	std::int8_t selected,
	std::uint32_t simulation_tick)
{
	if (!valid_player_index(player)
		|| player >= std::size(world.objects)
		|| !world.objects[player].active)
	{
		return;
	}
	DeathmatchScenarioState& state = runtime.deathmatch;
	if (pickup != UINT16_MAX)
	{
		state.powerup_pickup[player] = pickup;
	}
	// The peer which owns the recipient chooses the random record. Every
	// other peer waits for opcode 0x3d's explicit four-bit assignment.
	if (player != runtime.network.local_player && selected < 0)
	{
		return;
	}
	if (selected < 0)
	{
		selected = select_powerup(runtime, world);
	}
	if (!valid_powerup(selected))
	{
		return;
	}
	state.powerup_active[player] = selected;
	state.powerup_acquired[player] = simulation_tick;
	const DeathmatchPowerupDefinition& definition =
		kPowerups[static_cast<std::uint8_t>(selected)];
	if ((definition.flags & kPowerupFlagAutomatic) != 0)
	{
		trigger_powerup(
			runtime,
			world,
			stats,
			player,
			selected,
			simulation_tick,
			true);
	}
	state.powerup_expiry[player] =
		powerup_expiry_tick(simulation_tick, definition.duration);
	if (static_cast<DeathmatchPowerup>(selected)
		== DeathmatchPowerup::missile)
	{
		install_missile_powerup(
			runtime, world.objects[player], player);
	}
}

void pickup_generic(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint8_t player,
	std::uint16_t pickup,
	std::uint32_t simulation_tick,
	bool publish)
{
	if (!valid_player_index(player)
		|| pickup >= std::size(world.objects)
		|| !world.objects[pickup].active
		|| world.objects[pickup].type != kPowerupType
		|| !deathmatch_scenarios_powerup_allowed(runtime, player)
		|| runtime.deathmatch.powerup_active[player] != -1)
	{
		return;
	}
	set_hidden(world.objects[pickup], true);
	world.objects[pickup].visible = false;
	world.objects[pickup].targetable = false;
	world.objects[pickup].runtime_flags &= ~kTargetableFlag;
	if (publish)
	{
		(void)queue_broadcast(
			runtime,
			NetworkGameplayOpcode::deathmatch_pickup,
			static_cast<std::int8_t>(player),
			-1,
			pickup);
	}
	assign_powerup(
		runtime,
		world,
		stats,
		player,
		pickup,
		-1,
		simulation_tick);
}

void service_powerup_expiration(
	Runtime& runtime,
	game::World& world,
	std::uint32_t simulation_tick)
{
	DeathmatchScenarioState& state = runtime.deathmatch;
	for (std::uint8_t player = 0;
		player < kDeathmatchScenarioPlayerCapacity;
		++player)
	{
		const std::int8_t powerup = state.powerup_active[player];
		const std::uint32_t expiry = state.powerup_expiry[player];
		if (!valid_powerup(powerup)
			|| expiry == UINT32_MAX
			|| simulation_tick <= expiry)
		{
			continue;
		}
		expire_powerup_effect(
			world, player, powerup, simulation_tick);
		unhide_object(
			runtime,
			world,
			state.powerup_pickup[player],
			player == runtime.network.local_player);
		state.powerup_active[player] = -1;
		state.powerup_expiry[player] = UINT32_MAX;
	}
}

void clear_player_powerup(
	Runtime& runtime,
	game::World& world,
	std::uint8_t player,
	std::uint32_t simulation_tick)
{
	if (!valid_player_index(player))
	{
		return;
	}
	DeathmatchScenarioState& state = runtime.deathmatch;
	const std::int8_t powerup = state.powerup_active[player];
	if (!valid_powerup(powerup))
	{
		return;
	}
	expire_powerup_effect(
		world, player, powerup, simulation_tick);
	state.powerup_active[player] = -1;
	state.powerup_expiry[player] = UINT32_MAX;
	unhide_object(
		runtime,
		world,
		state.powerup_pickup[player],
		player == runtime.network.local_player);
}

void place_object_at(
	game::WorldObject& object,
	const glm::vec3& position,
	const glm::mat3& orientation)
{
	object.previous_position = position;
	object.position = position;
	object.scene_position = position;
	object.previous_orientation = orientation;
	object.orientation = orientation;
	object.scene_orientation = orientation;
	set_hidden(object, false);
	object.runtime_flags |= kTargetableFlag;
}

void place_object_position_at(
	game::WorldObject& object,
	const glm::vec3& position)
{
	object.previous_position = position;
	object.position = position;
	object.scene_position = position;
	set_hidden(object, false);
	object.runtime_flags |= kTargetableFlag;
}

void drop_hidden_object_near_player(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t player,
	std::uint16_t type,
	bool publish)
{
	if (!connected_player(runtime, world, player))
	{
		return;
	}
	const std::uint16_t object_index =
		find_first_type(world, type, true);
	if (object_index == UINT16_MAX)
	{
		return;
	}
	const game::WorldObject& owner = world.objects[player];
	game::WorldObject& object = world.objects[object_index];
	if (!is_authority(runtime.network))
	{
		set_hidden(object, false);
		object.runtime_flags |= kTargetableFlag;
		return;
	}
	constexpr glm::vec3 kDropDirections[6]{
		{1.0f, 0.0f, 0.0f},
		{-1.0f, 0.0f, 0.0f},
		{0.0f, 1.0f, 0.0f},
		{0.0f, -1.0f, 0.0f},
		{0.0f, 0.0f, 1.0f},
		{0.0f, 0.0f, -1.0f},
	};
	DeathmatchScenarioState& state = runtime.deathmatch;
	const glm::vec3 local_offset =
		kDropDirections[state.pickup_drop_cursor] * (owner.radius * 0.8f);
	const glm::vec3 position =
		owner.position + owner.orientation * local_offset;
	place_object_position_at(object, position);
	state.pickup_drop_cursor = static_cast<std::uint8_t>(
		(state.pickup_drop_cursor + 1u) % std::size(kDropDirections));
	if (publish)
	{
		publish_forced_object_state(
			runtime,
			world,
			stats,
			object_index);
		publish_drop_pickup(runtime, type);
	}
}

std::uint8_t nuclear_total(
	const Runtime& runtime,
	const game::World& world)
{
	std::int32_t total = 0;
	for (std::uint8_t player = 0;
		player < kDeathmatchScenarioPlayerCapacity;
		++player)
	{
		if (!connected_player(runtime, world, player))
		{
			continue;
		}
		total += std::max(
			0, world.objects[player].deathmatch_scenario_counter);
	}
	for (std::uint8_t ordinal = 0;
		ordinal < runtime.deathmatch.nuclear_beacon_count;
		++ordinal)
	{
		const std::uint16_t index =
			runtime.deathmatch.nuclear_beacon[ordinal];
		if (index < std::size(world.objects)
			&& world.objects[index].active
			&& (world.objects[index].runtime_flags & game::kObjectFlagDisabled) == 0)
		{
			++total;
		}
	}
	return static_cast<std::uint8_t>(
		std::clamp(total, 0, 255));
}

bool reset_object_to_marker(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t object_index,
	std::uint16_t marker_type,
	bool publish)
{
	if (object_index >= std::size(world.objects)
		|| !world.objects[object_index].active)
	{
		return false;
	}
	game::WorldObject& object = world.objects[object_index];
	set_hidden(object, false);

	for (std::uint16_t marker_index = 0;
		marker_index < std::size(world.objects);
		++marker_index)
	{
		const game::WorldObject& marker = world.objects[marker_index];
		if (!marker.active || marker.type != marker_type)
		{
			continue;
		}
		bool collision = false;
		for (std::uint16_t sibling_index = 0;
			sibling_index < std::size(world.objects);
			++sibling_index)
		{
			const game::WorldObject& sibling =
				world.objects[sibling_index];
			if (sibling_index != object_index
				&& sibling.active
				&& sibling.type == object.type
				&& glm::distance(marker.position, sibling.position)
					< object.radius * 2.0f)
			{
				collision = true;
				break;
			}
		}
		if (collision)
		{
			continue;
		}

		place_object_at(object, marker.position, marker.orientation);
		if (publish)
		{
			publish_forced_object_state(
				runtime,
				world,
				stats,
				object_index);
		}
		return true;
	}
	return false;
}

void reset_beacons_to_markers(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats)
{
	DeathmatchScenarioState& state = runtime.deathmatch;
	for (std::uint8_t ordinal = 0;
		ordinal < state.nuclear_beacon_count;
		++ordinal)
	{
		(void)reset_object_to_marker(
			runtime,
			world,
			stats,
			state.nuclear_beacon[ordinal],
			kBeaconSpawnType,
			runtime.network.role != NetworkRole::offline);
	}
}

bool out_of_arena(
	const DeathmatchScenarioState& state,
	const game::WorldObject& object)
{
	return state.arena_radius > 0.0f
		&& glm::distance(state.arena_center, object.position)
			> state.arena_radius - 500.0f;
}

void send_scenario_state(
	Runtime& runtime,
	const game::World& world,
	std::uint8_t destination)
{
	if (runtime.network.role == NetworkRole::offline
		|| destination >= runtime.network.player_count
		|| !runtime.network.connected[destination])
	{
		return;
	}
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode = NetworkGameplayOpcode::deathmatch_state;
	message.delivery = NetworkDelivery::directed_guaranteed;
	message.source_player = runtime.network.local_player;
	message.destination_player = destination;
	if (!deathmatch_scenarios_serialize(
			runtime,
			world,
			message.scenario_state,
			message.scenario_state_bits))
	{
		return;
	}
	(void)network_queue_gameplay_message(runtime.network, message);
}

void broadcast_state_except_local(
	Runtime& runtime,
	const game::World& world)
{
	for (std::uint8_t player = 0;
		player < runtime.network.player_count;
		++player)
	{
		if (player != runtime.network.local_player
			&& runtime.network.connected[player])
		{
			send_scenario_state(runtime, world, player);
		}
	}
}

void request_scenario_state(Runtime& runtime)
{
	if (runtime.network.role != NetworkRole::client)
	{
		return;
	}
	(void)queue_scenario_message(
		runtime,
		NetworkGameplayOpcode::deathmatch_state_request,
		NetworkDelivery::broadcast_guaranteed,
		kBroadcastDestination,
		static_cast<std::int8_t>(runtime.network.local_player));
}

void nuclear_reset_player(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t player,
	bool publish)
{
	if (!valid_player_index(player)
		|| player >= std::size(world.objects)
		|| !world.objects[player].active)
	{
		return;
	}
	game::WorldObject& object = world.objects[player];
	if (object.deathmatch_scenario_counter != -1)
	{
		if (publish)
		{
			const std::int32_t count =
				std::max(0, object.deathmatch_scenario_counter);
			for (std::int32_t dropped = 0; dropped < count; ++dropped)
			{
				drop_hidden_object_near_player(
					runtime,
					world,
					stats,
					player,
					kBeaconType,
					true);
			}
		}
		object.deathmatch_scenario_counter = -1;
	}
	if (publish)
	{
		(void)queue_broadcast(
			runtime,
			NetworkGameplayOpcode::nuclear_reset_player,
			static_cast<std::int8_t>(player));
	}
}

void nuclear_success(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t player,
	std::uint32_t simulation_tick,
	bool publish)
{
	if (!connected_player(runtime, world, player))
	{
		return;
	}
	const std::uint16_t gate = find_first_type(world, kBeaconGateType);
	if (gate == UINT16_MAX)
	{
		diagnostics::mission_log(
			"deathmatch nuclear missing beacon gate");
		return;
	}
	queue_event(
		runtime.deathmatch,
		DeathmatchScenarioMessageKind::nuclear_success,
		static_cast<std::int8_t>(player));
	DeathmatchScenarioState& state = runtime.deathmatch;
	state.nuclear_owner = static_cast<std::int8_t>(player);
	state.nuclear_victim_count = 0;
	state.nuclear_detonation_tick = simulation_tick + 1000u;
	state.nuclear_effect_active = true;
	const game::WorldObject& gate_object = world.objects[gate];
	const float final_radius = state.arena_radius * 1.1f;
	for (std::uint8_t victim = 0;
		victim < kDeathmatchScenarioPlayerCapacity;
		++victim)
	{
		if (victim == player
			|| !connected_player(runtime, world, victim))
		{
			continue;
		}
		const game::WorldObject& candidate = world.objects[victim];
		if ((candidate.runtime_flags & game::kObjectFlagDisabled) != 0
			|| candidate.ai.command_count == 0
			|| candidate.type == 0x6d
			|| candidate.type == 0x6e
			|| candidate.type == kDarkTowerType
			|| candidate.type == 0xa8
			|| glm::distance(
					gate_object.position, candidate.position)
				> final_radius)
		{
			continue;
		}
		const std::uint8_t slot = state.nuclear_victim_count++;
		state.nuclear_victim[slot] = victim;
		state.nuclear_victim_generation[slot] =
			candidate.generation;
	}
	(void)game::shockwave_create(
		world,
		gate_object.position,
		gate_object.orientation,
		glm::vec3{0.0f},
		0,
		final_radius,
		1000.0f,
		world.objects[player].allegiance_class,
		player,
		simulation_tick);
	world.player_camera_disturbance =
		std::max(world.player_camera_disturbance, 2.0f);
	for (std::uint8_t ordinal = 0;
		ordinal < runtime.deathmatch.nuclear_beacon_count;
		++ordinal)
	{
		const std::uint16_t beacon =
			runtime.deathmatch.nuclear_beacon[ordinal];
		if (beacon < std::size(world.objects)
			&& world.objects[beacon].active)
		{
			world.objects[beacon].runtime_flags |= kTargetableFlag;
		}
	}
	world.objects[player].deathmatch_scenario_counter = -1;
	reset_beacons_to_markers(runtime, world, stats);
	if (publish)
	{
		(void)queue_broadcast(
			runtime,
			NetworkGameplayOpcode::nuclear_success,
			static_cast<std::int8_t>(player));
	}
}

void nuclear_pickup(
	Runtime& runtime,
	game::World& world,
	std::uint16_t player,
	std::uint16_t object,
	bool publish)
{
	if (!connected_player(runtime, world, player)
		|| object >= std::size(world.objects)
		|| !world.objects[object].active
		|| world.objects[object].type != kBeaconType
		|| (world.objects[object].runtime_flags & game::kObjectFlagDisabled) != 0)
	{
		return;
	}
	game::WorldObject& owner = world.objects[player];
	owner.deathmatch_scenario_counter =
		owner.deathmatch_scenario_counter == -1
			? 1
			: std::min(
				6, owner.deathmatch_scenario_counter + 1);
	set_hidden(world.objects[object], true);
	if (owner.deathmatch_scenario_counter == 6)
	{
		queue_event(
			runtime.deathmatch,
			DeathmatchScenarioMessageKind::nuclear_all_beacons,
			static_cast<std::int8_t>(player));
	}
	if (publish)
	{
		(void)queue_broadcast(
			runtime,
			NetworkGameplayOpcode::deathmatch_pickup,
			static_cast<std::int8_t>(player),
			-1,
			object);
	}
}

void dark_set_holder(
	Runtime& runtime,
	game::World& world,
	std::int8_t holder)
{
	DeathmatchScenarioState& state = runtime.deathmatch;
	state.dark_holder = holder;
	for (std::uint8_t player = 0;
		player < kDeathmatchScenarioPlayerCapacity;
		++player)
	{
		if (player < std::size(world.objects)
			&& world.objects[player].active)
		{
			world.objects[player].deathmatch_scenario_counter =
				holder == static_cast<std::int8_t>(player) ? 1 : -1;
		}
	}
	if (state.dark_relay < std::size(world.objects)
		&& world.objects[state.dark_relay].active)
	{
		set_hidden(
			world.objects[state.dark_relay], holder >= 0);
	}
}

void dark_pickup(
	Runtime& runtime,
	game::World& world,
	std::uint16_t player,
	std::uint16_t object,
	bool publish)
{
	if (!connected_player(runtime, world, player)
		|| object >= std::size(world.objects)
		|| !world.objects[object].active
		|| world.objects[object].type != kRelayType
		|| (world.objects[object].runtime_flags & game::kObjectFlagDisabled) != 0)
	{
		return;
	}
	runtime.deathmatch.dark_relay = object;
	dark_set_holder(
		runtime, world, static_cast<std::int8_t>(player));
	const std::uint16_t tower_index = runtime.deathmatch.dark_tower;
	if (tower_index < std::size(world.objects)
		&& world.objects[tower_index].active)
	{
		game::WorldObject& tower = world.objects[tower_index];
		if (tower.ai.command_count != 0)
		{
			const ai::Command& active = tower.ai.commands[0];
			if ((active.id == 122 || active.id == 110)
				&& active.target_kind == ai::TargetKind::object
				&& active.target == player)
			{
				ai::command_clear(world, tower);
			}
		}
	}
	if (publish)
	{
		(void)queue_broadcast(
			runtime,
			NetworkGameplayOpcode::deathmatch_pickup,
			static_cast<std::int8_t>(player),
			-1,
			object);
	}
}

void dark_drop(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t player,
	bool publish)
{
	if (!valid_player_index(player)
		|| runtime.deathmatch.dark_holder
			!= static_cast<std::int8_t>(player))
	{
		return;
	}
	dark_set_holder(runtime, world, -1);
	drop_hidden_object_near_player(
		runtime,
		world,
		stats,
		player,
		kRelayType,
		publish);
	runtime.deathmatch.dark_relay =
		find_first_type(world, kRelayType);
	if (publish)
	{
		(void)queue_broadcast(
			runtime,
			NetworkGameplayOpcode::dark_reign_drop,
			static_cast<std::int8_t>(player));
	}
}

void tag_set(
	Runtime& runtime,
	game::World& world,
	std::int8_t holder,
	std::int8_t tagger,
	bool publish)
{
	DeathmatchScenarioState& state = runtime.deathmatch;
	const std::int8_t published_tagger = tagger;
	if (holder < 0)
	{
		queue_event(
			state, DeathmatchScenarioMessageKind::tag_no_bomb);
		state.tag_holder = -1;
		state.tag_last_tagger = -1;
	}
	else
	{
		if (tagger >= 0)
		{
			queue_event(
				state,
				DeathmatchScenarioMessageKind::tag_passed,
				tagger,
				holder);
		}
		else
		{
			tagger = state.tag_holder;
		}
		state.tag_holder = holder;
		state.tag_last_tagger = tagger;
	}
	if (publish)
	{
		(void)queue_broadcast(
			runtime,
			NetworkGameplayOpcode::tag_bomb_assignment,
			holder,
			published_tagger);
	}
	(void)world;
}

void tag_detonate(
	Runtime& runtime,
	game::World& world,
	bool publish)
{
	DeathmatchScenarioState& state = runtime.deathmatch;
	if (state.tag_holder < 0
		|| !connected_player(
			runtime,
			world,
			static_cast<std::uint16_t>(state.tag_holder)))
	{
		return;
	}
	game::WorldObject& victim =
		world.objects[static_cast<std::uint8_t>(state.tag_holder)];
	if (publish)
	{
		(void)ai::schedule_death_command(
			victim, world, runtime, 0, false);
		state.tag_bomb_death = true;
	}
	if (state.tag_last_tagger >= 0)
	{
		queue_event(
			state,
			DeathmatchScenarioMessageKind::tag_exploded,
			state.tag_last_tagger,
			state.tag_holder);
		victim.last_attacker_index =
			static_cast<std::uint16_t>(0xfffeu);
		if (state.tag_last_tagger
			== static_cast<std::int8_t>(
				runtime.network.local_player))
		{
			(void)runtime_add_player_score(
				runtime,
				world,
				runtime.network.local_player,
				3,
				true);
		}
	}
	if (publish)
	{
		(void)queue_broadcast(
			runtime,
			NetworkGameplayOpcode::tag_bomb_detonate);
	}
}

void shadow_set(
	Runtime& runtime,
	game::World& world,
	std::int8_t holder,
	bool publish,
	std::uint32_t simulation_tick)
{
	DeathmatchScenarioState& state = runtime.deathmatch;
	const std::int8_t previous = state.shadow_holder;
	if (previous >= 0
		&& static_cast<std::uint8_t>(previous)
			< std::size(world.objects)
		&& world.objects[static_cast<std::uint8_t>(previous)].active)
	{
		game::WorldObject& former =
			world.objects[static_cast<std::uint8_t>(previous)];
		former.runtime_flags |= kTargetableFlag;
		former.targetable = true;
	}
	if (holder < 0)
	{
		queue_event(
			state, DeathmatchScenarioMessageKind::shadow_none);
	}
	else
	{
		if (previous >= 0 && previous != holder)
		{
			queue_event(
				state,
				DeathmatchScenarioMessageKind::shadow_replaced,
				holder,
				previous);
		}
		else
		{
			queue_event(
				state,
				DeathmatchScenarioMessageKind::shadow_assigned,
				holder);
		}
			game::WorldObject& shadow =
				world.objects[static_cast<std::uint8_t>(holder)];
			(void)game::world_force_cloak(
				world, shadow, simulation_tick);
			shadow.runtime_flags &= ~kTargetableFlag;
			shadow.targetable = false;
		if (holder == static_cast<std::int8_t>(
				runtime.network.local_player)
			&& previous >= 0 && previous != holder)
		{
			(void)runtime_add_player_score(
				runtime,
				world,
				runtime.network.local_player,
				5,
				true);
		}
	}
	state.shadow_holder = holder;
	if (publish)
	{
		(void)queue_broadcast(
			runtime,
			NetworkGameplayOpcode::shadow_assignment,
			holder);
	}
}

void shadow_kill(
	Runtime& runtime,
	game::World& world,
	std::uint16_t victim,
	bool publish)
{
	DeathmatchScenarioState& state = runtime.deathmatch;
	if (!valid_player_index(victim)
		|| state.shadow_holder < 0)
	{
		return;
	}
	queue_event(
		state,
		DeathmatchScenarioMessageKind::shadow_kill,
		state.shadow_holder,
		static_cast<std::int8_t>(victim));
	if (victim < std::size(world.objects)
		&& world.objects[victim].active)
	{
		world.objects[victim].last_attacker_index =
			static_cast<std::uint16_t>(0xfffeu);
	}
	if (state.shadow_holder == static_cast<std::int8_t>(
			runtime.network.local_player))
	{
		(void)runtime_add_player_score(
			runtime,
			world,
			runtime.network.local_player,
			1,
			true);
	}
	if (publish)
	{
		(void)queue_broadcast(
			runtime,
			NetworkGameplayOpcode::shadow_kill,
			static_cast<std::int8_t>(victim));
	}
}

void vampire_set(
	Runtime& runtime,
	game::World& world,
	std::uint16_t player,
	bool infected,
	bool publish)
{
	if (!valid_player_index(player))
	{
		return;
	}
	DeathmatchScenarioState& state = runtime.deathmatch;
	state.vampire[player] = infected;
	if (infected)
	{
		queue_event(
			state,
			DeathmatchScenarioMessageKind::vampire_assigned,
			static_cast<std::int8_t>(player));
		std::uint8_t total = 0;
		std::uint8_t infected_count = 0;
		for (std::uint8_t candidate = 0;
			candidate < kDeathmatchScenarioPlayerCapacity;
			++candidate)
		{
			if (candidate >= runtime.network.player_count
				|| !runtime.network.connected[candidate])
			{
				continue;
			}
			++total;
			if (state.vampire[candidate])
			{
				++infected_count;
			}
		}
		if (total != 0 && infected_count == total)
		{
			queue_event(
				state,
				DeathmatchScenarioMessageKind::vampire_last_human,
				static_cast<std::int8_t>(player));
			if (player == runtime.network.local_player)
			{
				(void)runtime_add_player_score(
					runtime, world, player, 5, true);
			}
			std::fill(
				std::begin(state.vampire),
				std::end(state.vampire),
				false);
			queue_event(
				state,
				DeathmatchScenarioMessageKind::vampire_none);
		}
	}
	else
	{
		const bool any = std::any_of(
			std::begin(state.vampire),
			std::end(state.vampire),
			[](bool value) { return value; });
		if (!any)
		{
			queue_event(
				state,
				DeathmatchScenarioMessageKind::vampire_none);
		}
	}
	if (publish)
	{
		(void)queue_broadcast(
			runtime,
			NetworkGameplayOpcode::vampire_assignment,
			static_cast<std::int8_t>(player),
			-1,
			UINT16_MAX,
			infected);
	}
}

void pickup_special(
	Runtime& runtime,
	game::World& world,
	std::uint16_t player,
	std::uint16_t object,
	bool publish)
{
	if (object >= std::size(world.objects)
		|| !world.objects[object].active)
	{
		return;
	}
	if (world.objects[object].type == kBeaconType
		&& selected_scenario(runtime)
			== DeathmatchScenario::nuclear_threat)
	{
		nuclear_pickup(runtime, world, player, object, publish);
	}
	else if (world.objects[object].type == kRelayType
		&& selected_scenario(runtime)
			== DeathmatchScenario::dark_reign)
	{
		dark_pickup(runtime, world, player, object, publish);
	}
}

void service_special_proximity(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t simulation_tick)
{
	for (std::uint8_t player = 0;
		player < kDeathmatchScenarioPlayerCapacity;
		++player)
	{
		if (!connected_player(runtime, world, player))
		{
			continue;
		}
		game::WorldObject& owner = world.objects[player];
		if (owner.ai.command_count != 0
			&& (owner.ai.commands[0].id == 11
				|| owner.ai.commands[0].id == 121))
		{
			continue;
		}
		if (runtime.deathmatch.player_pickup_ready_tick[player]
			> static_cast<std::int32_t>(simulation_tick))
		{
			continue;
		}
		for (std::uint16_t object = 0;
			object < std::size(world.objects);
			++object)
		{
			game::WorldObject& pickup = world.objects[object];
			if (!pickup.active
				|| (pickup.type != kPowerupType
					&& pickup.type != kBeaconType
					&& pickup.type != kRelayType)
				|| (pickup.runtime_flags & game::kObjectFlagDisabled) != 0
				|| glm::distance(owner.position, pickup.position)
					>= owner.radius + pickup.radius)
			{
				continue;
			}
			if (connected_player_count(runtime) == 1)
			{
				if (runtime.deathmatch.last_update_tick
					- runtime.deathmatch.pickup_warning_tick > 30)
				{
					queue_event(
						runtime.deathmatch,
						DeathmatchScenarioMessageKind::
							need_more_players);
				}
				runtime.deathmatch.pickup_warning_tick =
					runtime.deathmatch.last_update_tick;
				return;
			}
				if (pickup.type == kPowerupType)
				{
					pickup_generic(
						runtime,
						world,
						stats,
						player,
						object,
						simulation_tick,
						true);
					continue;
				}
			// The host sees all eight replicated player poses and is the
			// sole authoritative owner for special pickups. Opcode 0x3c
			// then applies the same callback on every peer.
			if (is_authority(runtime.network))
			{
				pickup_special(
					runtime, world, player, object, true);
			}
		}
	}
}

const char* player_name(
	const Runtime& runtime,
	const LanguageTable& language,
	std::int8_t player,
	char (&fallback)[48])
{
	if (player >= 0
		&& player < static_cast<std::int8_t>(
			kDeathmatchScenarioPlayerCapacity)
		&& runtime.network.player_name[
			static_cast<std::uint8_t>(player)][0] != '\0')
	{
		return runtime.network.player_name[
			static_cast<std::uint8_t>(player)];
	}
	std::snprintf(
		fallback,
		sizeof(fallback),
		"%s %u",
		language_text(language, 0xbf),
		player >= 0 ? static_cast<unsigned>(player + 1) : 0u);
	return fallback;
}

void format_scenario_message(
	const Runtime& runtime,
	const LanguageTable& language,
	const DeathmatchScenarioMessage& event,
	char (&output)[100])
{
	char first_fallback[48];
	char second_fallback[48];
	const char* first = player_name(
		runtime, language, event.primary, first_fallback);
	const char* second = player_name(
		runtime, language, event.secondary, second_fallback);
	const bool local_primary =
		event.primary == static_cast<std::int8_t>(
			runtime.network.local_player);
	switch (event.kind)
	{
	case DeathmatchScenarioMessageKind::restart:
		std::snprintf(
			output, sizeof(output), "%s",
			language_text(language, 0x59d));
		break;
	case DeathmatchScenarioMessageKind::need_more_players:
		std::snprintf(
			output, sizeof(output), "%s",
			language_text(language, 0x5b9));
		break;
	case DeathmatchScenarioMessageKind::nuclear_all_beacons:
		std::snprintf(
			output, sizeof(output),
			language_text(language, 0x2eb), first);
		break;
	case DeathmatchScenarioMessageKind::nuclear_success:
		std::snprintf(
			output, sizeof(output),
			language_text(language, 0x300), first);
		break;
	case DeathmatchScenarioMessageKind::nuclear_kill:
		std::snprintf(
			output, sizeof(output),
			language_text(language, 0x301),
			first,
			second,
			local_primary
				? language_text(language, 0x2f4)
				: "");
		break;
	case DeathmatchScenarioMessageKind::tag_no_bomb:
		std::snprintf(
			output, sizeof(output), "%s",
			language_text(language, 0x2e3));
		break;
	case DeathmatchScenarioMessageKind::tag_passed:
		std::snprintf(
			output, sizeof(output),
			language_text(language, 0x305), first, second);
		break;
	case DeathmatchScenarioMessageKind::tag_exploded:
		std::snprintf(
			output, sizeof(output),
			language_text(language, 0x2e1),
			first,
			second,
			local_primary
				? language_text(language, 0x5b8)
				: "");
		break;
	case DeathmatchScenarioMessageKind::shadow_none:
		std::snprintf(
			output, sizeof(output), "%s",
			language_text(language, 0x2e5));
		break;
	case DeathmatchScenarioMessageKind::shadow_assigned:
		std::snprintf(
			output, sizeof(output),
			language_text(language, 0x2dd), first);
		break;
	case DeathmatchScenarioMessageKind::shadow_replaced:
		std::snprintf(
			output, sizeof(output),
			language_text(language, 0x2e7),
			first,
			second,
			local_primary
				? language_text(language, 0x2f3)
				: "");
		break;
	case DeathmatchScenarioMessageKind::shadow_kill:
		std::snprintf(
			output, sizeof(output),
			language_text(language, 0x2e6),
			first,
			second,
			event.primary == static_cast<std::int8_t>(
					runtime.network.local_player)
				? language_text(language, 0x2f4)
				: "");
		break;
	case DeathmatchScenarioMessageKind::vampire_none:
		std::snprintf(
			output, sizeof(output), "%s",
			language_text(language, 0x2e9));
		break;
	case DeathmatchScenarioMessageKind::vampire_assigned:
		std::snprintf(
			output, sizeof(output),
			language_text(language, 0x2ea), first);
		break;
	case DeathmatchScenarioMessageKind::vampire_last_human:
		std::snprintf(
			output, sizeof(output),
			language_text(language, 0x2ec),
			first,
			local_primary
				? language_text(language, 0x2f3)
				: "");
		break;
	case DeathmatchScenarioMessageKind::vampire_last_left:
		std::snprintf(
			output, sizeof(output), "%s",
			language_text(language, 0x5ba));
		break;
	}
}
}

void deathmatch_scenarios_select(Runtime& runtime)
{
	if (runtime.network.deathmatch_scenario_selected)
	{
		return;
	}
	runtime.network.deathmatch_scenario = -1;
	runtime.deathmatch.snapshot_pending = false;
	for (std::uint8_t scenario = 0;
		scenario < std::size(kScenarioMissions);
		++scenario)
	{
		if (runtime.mission_number == kScenarioMissions[scenario])
		{
			runtime.network.deathmatch_scenario =
				static_cast<std::int8_t>(scenario);
		}
	}
	runtime.network.deathmatch_scenario_selected = true;
	if (runtime.network.deathmatch_scenario >= 0)
	{
		runtime.network.deathmatch_mode = true;
	}
	diagnostics::mission_log(
		"deathmatch scenario select mission=%u index=%d",
		static_cast<unsigned>(runtime.mission_number),
		static_cast<int>(runtime.network.deathmatch_scenario));
}

void deathmatch_scenarios_initialize(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t simulation_tick)
{
	deathmatch_scenarios_select(runtime);
	DeathmatchScenarioState& state = runtime.deathmatch;
	initialize_powerup_pool(runtime, world, stats);
	const bool received_snapshot = state.snapshot_pending;
	compute_arena(state, world);
	switch (selected_scenario(runtime))
	{
	case DeathmatchScenario::nuclear_threat:
	{
		state.nuclear_beacon_count = collect_type(
			world,
			kBeaconType,
			state.nuclear_beacon,
			kDeathmatchScenarioBeaconCapacity);
		if (is_authority(runtime.network)
			&& !state.snapshot_pending)
		{
			for (std::uint8_t player = 0;
				player < kDeathmatchScenarioPlayerCapacity;
				++player)
			{
				if (connected_player(runtime, world, player))
				{
					world.objects[player].
						deathmatch_scenario_counter = -1;
				}
			}
			reset_beacons_to_markers(runtime, world, stats);
		}
		else if (state.snapshot_pending)
		{
			for (std::uint8_t ordinal = 0;
				ordinal < state.nuclear_beacon_count;
				++ordinal)
			{
				game::WorldObject& beacon = world.objects[
					state.nuclear_beacon[ordinal]];
				place_object_at(
					beacon,
					state.nuclear_snapshot_position[ordinal],
					beacon.orientation);
				set_hidden(
					beacon,
					(state.nuclear_snapshot_visible
						& (1u << ordinal)) == 0);
			}
			for (std::uint8_t player = 0;
				player < kDeathmatchScenarioPlayerCapacity;
				++player)
			{
				if (player != runtime.network.local_player
					&& connected_player(runtime, world, player))
				{
					world.objects[player].
						deathmatch_scenario_counter =
						state.nuclear_snapshot_counter[player];
				}
			}
		}
		break;
	}
	case DeathmatchScenario::dark_reign:
		state.dark_relay = find_first_type(world, kRelayType);
		state.dark_tower = find_first_type(world, kDarkTowerType);
		dark_set_holder(
			runtime,
			world,
			state.snapshot_pending
				? state.dark_snapshot_holder
				: -1);
		if (state.dark_holder < 0
			&& state.dark_relay < std::size(world.objects)
			&& network_local_owns_object(
				runtime.network,
				state.dark_relay,
				runtime.player_prefix_count,
				world.player.index))
		{
			unhide_object(
				runtime,
				world,
				state.dark_relay,
				runtime.network.role != NetworkRole::offline);
		}
		break;
	case DeathmatchScenario::tag_bomb:
		state.tag_holder = state.snapshot_pending
			? state.tag_snapshot_holder : -1;
		state.tag_last_tagger = state.snapshot_pending
			? state.tag_snapshot_tagger : -1;
		state.tag_timer = 4400;
		state.tag_previous_tick = simulation_tick;
		state.tag_pending = 0;
		state.tag_bomb_death = false;
		if (is_authority(runtime.network))
		{
			queue_event(
				state,
				DeathmatchScenarioMessageKind::tag_no_bomb);
		}
		break;
	case DeathmatchScenario::hunt_the_shadow:
		state.shadow_holder = -1;
		if (state.snapshot_pending)
		{
			shadow_set(
				runtime,
				world,
				state.shadow_snapshot_holder,
				false,
				simulation_tick);
		}
		else
		{
			queue_event(
				state,
				DeathmatchScenarioMessageKind::shadow_none);
		}
		break;
	case DeathmatchScenario::vampires:
		std::fill(
			std::begin(state.vampire),
			std::end(state.vampire),
			false);
		if (state.snapshot_pending)
		{
			for (std::uint8_t player = 0;
				player < kDeathmatchScenarioPlayerCapacity;
				++player)
			{
				state.vampire[player] =
					(state.vampire_snapshot_mask
						& (1u << player)) != 0;
			}
		}
		else
		{
			queue_event(
				state,
				DeathmatchScenarioMessageKind::vampire_none);
		}
		break;
	default:
		break;
	}
	state.snapshot_pending = false;
	state.initialized = true;
	state.last_update_tick = simulation_tick;
	if (runtime.network.role == NetworkRole::client
		&& !received_snapshot)
	{
		request_scenario_state(runtime);
	}
}

void deathmatch_scenarios_update(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t simulation_tick)
{
	DeathmatchScenarioState& state = runtime.deathmatch;
	if (!state.initialized)
	{
		deathmatch_scenarios_initialize(
			runtime, world, stats, simulation_tick);
	}
	state.last_update_tick = simulation_tick;
	if (state.nuclear_effect_active
		&& simulation_tick > state.nuclear_detonation_tick)
	{
		state.nuclear_effect_active = false;
		for (std::uint8_t ordinal = 0;
			ordinal < state.nuclear_victim_count;
			++ordinal)
		{
			const std::uint16_t victim =
				state.nuclear_victim[ordinal];
			if (victim >= std::size(world.objects))
			{
				continue;
			}
			game::WorldObject& object = world.objects[victim];
			if (!object.active
				|| object.generation
					!= state.nuclear_victim_generation[ordinal]
				|| object.ai.command_count == 0
				|| object.ai.commands[0].id == 11)
			{
				continue;
			}
			if (victim < kDeathmatchScenarioPlayerCapacity)
			{
				object.last_attacker_index =
					static_cast<std::uint16_t>(0xfffeu);
				queue_event(
					state,
					DeathmatchScenarioMessageKind::nuclear_kill,
					state.nuclear_owner,
					static_cast<std::int8_t>(victim));
				if (state.nuclear_owner
					== static_cast<std::int8_t>(
						runtime.network.local_player))
				{
					(void)runtime_add_player_score(
						runtime,
						world,
						runtime.network.local_player,
						1,
						true);
				}
			}
			(void)ai::schedule_death_command(
				object, world, runtime, 0, true);
		}
		state.nuclear_victim_count = 0;
	}
	switch (selected_scenario(runtime))
	{
	case DeathmatchScenario::nuclear_threat:
		if (is_authority(runtime.network))
		{
			if ((simulation_tick % 50u) == 0)
			{
				for (std::uint8_t ordinal = 0;
					ordinal < state.nuclear_beacon_count;
					++ordinal)
				{
					const std::uint16_t beacon_index =
						state.nuclear_beacon[ordinal];
					if (beacon_index < std::size(world.objects)
						&& world.objects[beacon_index].active
						&& (world.objects[beacon_index].runtime_flags
							& game::kObjectFlagDisabled) == 0
						&& out_of_arena(
							state,
							world.objects[beacon_index]))
					{
						(void)reset_object_to_marker(
							runtime,
							world,
							stats,
							beacon_index,
							kBeaconSpawnType,
							true);
					}
				}
			}
			if ((simulation_tick % 25u) == 0)
			{
				std::int32_t total = nuclear_total(runtime, world);
				if (total < kDeathmatchScenarioBeaconCapacity)
				{
					for (std::uint8_t ordinal = 0;
						ordinal < state.nuclear_beacon_count;
						++ordinal)
					{
						const std::uint16_t beacon =
							state.nuclear_beacon[ordinal];
						if (beacon < std::size(world.objects)
							&& world.objects[beacon].active
								&& (world.objects[beacon].runtime_flags
									& game::kObjectFlagDisabled) != 0)
						{
							(void)reset_object_to_marker(
								runtime,
								world,
								stats,
								beacon,
								kBeaconSpawnType,
								true);
						}
					}
				}
				else if (total > kDeathmatchScenarioBeaconCapacity)
				{
					std::int32_t removed = 0;
					const std::int32_t excess =
						total - kDeathmatchScenarioBeaconCapacity;
					for (std::uint8_t ordinal = 0;
						ordinal < state.nuclear_beacon_count
							&& removed < excess;
						++ordinal)
					{
						const std::uint16_t beacon =
							state.nuclear_beacon[ordinal];
						if (beacon < std::size(world.objects)
							&& world.objects[beacon].active
							&& (world.objects[beacon].runtime_flags
								& game::kObjectFlagDisabled) == 0)
						{
							set_hidden(world.objects[beacon], true);
							++removed;
						}
					}
					if (removed < excess)
					{
						std::uint8_t player =
							static_cast<std::uint8_t>(
								game::world_rand15(world)
									% kDeathmatchScenarioPlayerCapacity);
						std::uint8_t misses = 0;
						while (removed < excess
							&& misses
								< kDeathmatchScenarioPlayerCapacity)
						{
							if (connected_player(runtime, world, player)
								&& world.objects[player].
									deathmatch_scenario_counter > 0)
							{
								--world.objects[player].
									deathmatch_scenario_counter;
								if (world.objects[player].
									deathmatch_scenario_counter == 0)
								{
									world.objects[player].
										deathmatch_scenario_counter = -1;
								}
								++removed;
								misses = 0;
							}
							else
							{
								++misses;
							}
							player = static_cast<std::uint8_t>(
								(player + 1u)
									% kDeathmatchScenarioPlayerCapacity);
						}
					}
					broadcast_state_except_local(runtime, world);
				}
			}
			for (std::uint8_t player = 0;
				player < kDeathmatchScenarioPlayerCapacity;
				++player)
			{
				if (!connected_player(runtime, world, player)
					|| world.objects[player].
						deathmatch_scenario_counter != 6)
				{
					continue;
				}
				const std::uint16_t gate =
					find_first_type(world, kBeaconGateType);
				if (gate != UINT16_MAX
					&& glm::distance(
						world.objects[player].position,
						world.objects[gate].position) < 2500.0f)
				{
					nuclear_success(
						runtime,
						world,
						stats,
						player,
						simulation_tick,
						true);
					broadcast_state_except_local(runtime, world);
				}
			}
		}
		else if ((simulation_tick % 25u) == 0
			&& nuclear_total(runtime, world)
				!= kDeathmatchScenarioBeaconCapacity
			&& simulation_tick - state.last_nuclear_request_tick >= 25u)
		{
			state.last_nuclear_request_tick = simulation_tick;
			request_scenario_state(runtime);
		}
		break;
	case DeathmatchScenario::dark_reign:
		if (state.dark_relay == UINT16_MAX
			|| state.dark_relay >= std::size(world.objects)
			|| !world.objects[state.dark_relay].active)
		{
			state.dark_relay = find_first_type(world, kRelayType);
		}
		if (state.dark_tower == UINT16_MAX
			|| state.dark_tower >= std::size(world.objects)
			|| !world.objects[state.dark_tower].active)
		{
			state.dark_tower = find_first_type(world, kDarkTowerType);
		}
		if (is_authority(runtime.network)
			&& (simulation_tick % 50u) == 0
			&& state.dark_relay < std::size(world.objects)
			&& world.objects[state.dark_relay].active
			&& (world.objects[state.dark_relay].runtime_flags
				& game::kObjectFlagDisabled) == 0
			&& out_of_arena(
				state, world.objects[state.dark_relay]))
		{
			(void)reset_object_to_marker(
				runtime,
				world,
				stats,
				state.dark_relay,
				kBeaconSpawnType,
				true);
		}
		if (is_authority(runtime.network)
			&& (simulation_tick % 300u) == 0)
		{
			for (std::uint8_t player = 0;
				player < runtime.network.player_count;
				++player)
			{
				if (runtime.network.connected[player])
				{
					send_scenario_state(runtime, world, player);
				}
			}
		}
		{
			std::int8_t holder = -1;
			for (std::uint8_t player = 0;
				player < kDeathmatchScenarioPlayerCapacity;
				++player)
			{
				if (connected_player(runtime, world, player)
					&& world.objects[player].
						deathmatch_scenario_counter != -1)
				{
					holder = static_cast<std::int8_t>(player);
				}
			}
			dark_set_holder(runtime, world, holder);
		}
		if (state.dark_tower < std::size(world.objects)
			&& world.objects[state.dark_tower].active)
		{
			game::WorldObject& tower =
				world.objects[state.dark_tower];
			const bool locally_owned =
				network_local_owns_object(
					runtime.network,
					state.dark_tower,
					runtime.player_prefix_count,
					world.player.index);
			bool forbidden =
				tower.ai.command_count != 0
				&& (tower.ai.commands[0].id == 122
					|| tower.ai.commands[0].id == 33
					|| tower.ai.commands[0].id == 110);
			for (std::uint8_t index = 0;
				index < tower.ai.deferred_command_count;
				++index)
			{
				forbidden = forbidden
					|| tower.ai.deferred_commands[index].command.id == 33;
			}
			if (locally_owned && !forbidden)
			{
				std::uint8_t candidates[
					kDeathmatchScenarioPlayerCapacity];
				std::uint8_t count = 0;
				for (std::uint8_t player = 0;
					player < kDeathmatchScenarioPlayerCapacity;
					++player)
				{
					if (!connected_player(runtime, world, player)
						|| world.objects[player].
							deathmatch_scenario_counter != -1)
					{
						continue;
					}
					if (runtime.network.team_mode
						&& state.dark_holder >= 0
						&& runtime.network.object_team[player]
							== runtime.network.object_team[
								static_cast<std::uint8_t>(
									state.dark_holder)])
					{
						continue;
					}
					candidates[count++] = player;
				}
				if (count != 0)
				{
					const std::uint8_t target = candidates[
						game::world_rand15(world) % count];
					ai::Command command;
					command.id = 33;
					command.target_kind = ai::TargetKind::object;
					command.target = target;
					command.target_component = -1;
					command.selector = 0;
					(void)ai::command_defer(
						tower,
						command,
						simulation_tick,
						1000,
						static_cast<std::uint8_t>(
							game::world_rand15(world)));
				}
			}
		}
		break;
	case DeathmatchScenario::tag_bomb:
		if (state.tag_holder
			== static_cast<std::int8_t>(
				runtime.network.local_player)
			&& connected_player(
				runtime,
				world,
				runtime.network.local_player))
		{
			const game::WorldObject& holder = world.objects[
				runtime.network.local_player];
			const std::int16_t command =
				holder.ai.command_count == 0
					? -1 : holder.ai.commands[0].id;
			if (command == 121)
			{
				state.tag_previous_tick = simulation_tick;
			}
			else if (command != 11)
			{
				const std::uint32_t elapsed =
					simulation_tick - state.tag_previous_tick;
				state.tag_timer = std::bit_cast<std::int32_t>(
					std::bit_cast<std::uint32_t>(state.tag_timer)
						- elapsed);
				state.tag_previous_tick = simulation_tick;
				if (state.tag_timer <= 0)
				{
					tag_detonate(runtime, world, true);
				}
			}
			else
			{
				state.tag_previous_tick = simulation_tick;
			}
		}
		else
		{
			state.tag_previous_tick = simulation_tick;
		}
		break;
	default:
		break;
	}
	service_special_proximity(
		runtime, world, stats, simulation_tick);
	service_powerup_expiration(runtime, world, simulation_tick);
}

void deathmatch_scenarios_player_death(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t player,
	std::uint32_t simulation_tick)
{
	if (!valid_player_index(player))
	{
		return;
	}
	clear_player_powerup(
		runtime,
		world,
		static_cast<std::uint8_t>(player),
		simulation_tick);
	switch (selected_scenario(runtime))
	{
	case DeathmatchScenario::nuclear_threat:
		if (is_authority(runtime.network))
		{
			nuclear_reset_player(
				runtime, world, stats, player, true);
			broadcast_state_except_local(runtime, world);
		}
		break;
	case DeathmatchScenario::dark_reign:
		if (is_authority(runtime.network)
			&& runtime.deathmatch.dark_holder
				== static_cast<std::int8_t>(player))
		{
			dark_drop(
				runtime, world, stats, player, true);
		}
		break;
	case DeathmatchScenario::tag_bomb:
		if (player != runtime.network.local_player)
		{
			break;
		}
		if (runtime.deathmatch.tag_holder
			== static_cast<std::int8_t>(player)
			&& runtime.deathmatch.tag_bomb_death)
		{
			tag_set(runtime, world, -1, -1, true);
			runtime.deathmatch.tag_bomb_death = false;
			runtime.deathmatch.tag_timer = 4400;
		}
		else if (runtime.deathmatch.tag_holder < 0)
		{
			runtime.deathmatch.tag_timer = 4400;
			const std::uint16_t attacker =
				world.objects[player].last_attacker_index;
			runtime.deathmatch.tag_pending =
				attacker < kDeathmatchScenarioPlayerCapacity
					? static_cast<std::int8_t>(attacker + 1)
					: -1;
		}
		break;
	case DeathmatchScenario::hunt_the_shadow:
		if (player != runtime.network.local_player)
		{
			break;
		}
		{
			const std::uint16_t attacker =
				world.objects[player].last_attacker_index;
			const std::int8_t replacement =
				attacker < kDeathmatchScenarioPlayerCapacity
					? static_cast<std::int8_t>(attacker)
					: -1;
			if (runtime.deathmatch.shadow_holder < 0)
			{
				if (replacement >= 0)
				{
					shadow_set(
						runtime,
						world,
						replacement,
						true,
						simulation_tick);
				}
			}
			else if (runtime.deathmatch.shadow_holder
				== static_cast<std::int8_t>(player))
			{
				shadow_set(
					runtime,
					world,
					replacement,
					true,
					simulation_tick);
			}
			else if (attacker
				== static_cast<std::uint16_t>(
					runtime.deathmatch.shadow_holder))
			{
				shadow_kill(runtime, world, player, true);
			}
		}
		break;
	case DeathmatchScenario::vampires:
		if (player != runtime.network.local_player
			|| connected_player_count(runtime) == 1)
		{
			break;
		}
		{
			bool any_infected = false;
			for (std::uint8_t candidate = 0;
				candidate < kDeathmatchScenarioPlayerCapacity;
				++candidate)
			{
				any_infected = any_infected
					|| (candidate < runtime.network.player_count
						&& runtime.network.connected[candidate]
						&& runtime.deathmatch.vampire[candidate]);
			}
			if (!any_infected)
			{
				vampire_set(
					runtime, world, player, true, true);
				break;
			}
			const std::uint16_t attacker =
				world.objects[player].last_attacker_index;
			if (!runtime.deathmatch.vampire[player]
				&& attacker < kDeathmatchScenarioPlayerCapacity
				&& runtime.deathmatch.vampire[attacker])
			{
				vampire_set(
					runtime, world, player, true, true);
			}
		}
		break;
	default:
		break;
	}
}

void deathmatch_scenarios_player_leave(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t player)
{
	if (!valid_player_index(player))
	{
		return;
	}
	clear_player_powerup(
		runtime,
		world,
		static_cast<std::uint8_t>(player),
		runtime.deathmatch.last_update_tick);
	switch (selected_scenario(runtime))
	{
	case DeathmatchScenario::nuclear_threat:
		if (is_authority(runtime.network))
		{
			nuclear_reset_player(
				runtime, world, stats, player, true);
			broadcast_state_except_local(runtime, world);
		}
		break;
	case DeathmatchScenario::dark_reign:
		if (runtime.deathmatch.dark_holder
			== static_cast<std::int8_t>(player))
		{
			dark_set_holder(runtime, world, -1);
			if (runtime.deathmatch.dark_relay
				< std::size(world.objects))
			{
				unhide_object(
					runtime,
					world,
					runtime.deathmatch.dark_relay,
					false);
			}
		}
		break;
	case DeathmatchScenario::tag_bomb:
		if (runtime.deathmatch.tag_holder
			== static_cast<std::int8_t>(player))
		{
			tag_set(runtime, world, -1, -1, false);
		}
		break;
	case DeathmatchScenario::hunt_the_shadow:
		if (runtime.deathmatch.shadow_holder
			== static_cast<std::int8_t>(player))
		{
			shadow_set(
				runtime,
				world,
				-1,
				false,
				runtime.deathmatch.last_update_tick);
		}
		break;
	case DeathmatchScenario::vampires:
	{
		std::uint8_t connected = 0;
		std::uint8_t infected = 0;
		for (std::uint8_t candidate = 0;
			candidate < kDeathmatchScenarioPlayerCapacity;
			++candidate)
		{
			if (candidate < runtime.network.player_count
				&& runtime.network.connected[candidate])
			{
				++connected;
				infected += runtime.deathmatch.vampire[candidate]
					? 1 : 0;
			}
		}
		if (infected == static_cast<std::uint8_t>(
				connected - (connected != 0 ? 1 : 0)))
		{
			std::fill(
				std::begin(runtime.deathmatch.vampire),
				std::end(runtime.deathmatch.vampire),
				false);
			queue_event(
				runtime.deathmatch,
				DeathmatchScenarioMessageKind::vampire_last_left);
			queue_event(
				runtime.deathmatch,
				DeathmatchScenarioMessageKind::vampire_none);
		}
		if (runtime.deathmatch.vampire[player])
		{
			vampire_set(runtime, world, player, false, false);
		}
		break;
	}
	default:
		break;
	}
}

void deathmatch_scenarios_player_weapon_hit(
	Runtime& runtime,
	game::World& world,
	std::uint16_t victim,
	std::uint16_t attacker,
	std::uint32_t simulation_tick)
{
	if (selected_scenario(runtime)
		!= DeathmatchScenario::tag_bomb
		|| victim != runtime.network.local_player
		|| attacker >= kDeathmatchScenarioPlayerCapacity
		|| runtime.deathmatch.tag_holder
			!= static_cast<std::int8_t>(attacker))
	{
		return;
	}
	tag_set(
		runtime,
		world,
		static_cast<std::int8_t>(victim),
		static_cast<std::int8_t>(attacker),
		true);
	runtime.deathmatch.tag_previous_tick = simulation_tick;
}

void deathmatch_scenarios_post_respawn(
	Runtime& runtime,
	game::World& world,
	std::uint16_t player)
{
	if (selected_scenario(runtime) != DeathmatchScenario::tag_bomb
		|| player != runtime.network.local_player
		|| runtime.deathmatch.tag_pending == 0)
	{
		return;
	}
	std::int8_t tagger = static_cast<std::int8_t>(
		runtime.deathmatch.tag_pending - 1);
	if (tagger == -2)
	{
		tagger = -1;
	}
	tag_set(
		runtime,
		world,
		static_cast<std::int8_t>(player),
		tagger,
		true);
	runtime.deathmatch.tag_pending = 0;
}

bool deathmatch_scenarios_powerup_allowed(
	const Runtime& runtime,
	std::uint16_t player)
{
	if (!valid_player_index(player))
	{
		return false;
	}
	switch (selected_scenario(runtime))
	{
	case DeathmatchScenario::hunt_the_shadow:
		return runtime.deathmatch.shadow_holder
			!= static_cast<std::int8_t>(player);
	case DeathmatchScenario::vampires:
		return !runtime.deathmatch.vampire[player];
	default:
		return true;
	}
}

bool deathmatch_scenarios_activate_powerup(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t player,
	std::uint32_t simulation_tick)
{
	if (!valid_player_index(player))
	{
		return false;
	}
	const std::int8_t powerup =
		runtime.deathmatch.powerup_active[player];
	if (!valid_powerup(powerup))
	{
		return false;
	}
	if (static_cast<DeathmatchPowerup>(powerup)
		== DeathmatchPowerup::missile)
	{
		trigger_powerup(
			runtime,
			world,
			stats,
			static_cast<std::uint8_t>(player),
			powerup,
			simulation_tick,
			true);
		// Retail continues into the ordinary launcher after consuming the
		// Missile powerup, firing the installed definition-zero Screamer.
		return false;
	}
	const DeathmatchPowerupDefinition& definition =
		kPowerups[static_cast<std::uint8_t>(powerup)];
	if ((definition.flags & kPowerupFlagAutomatic) != 0)
	{
		return true;
	}
	trigger_powerup(
		runtime,
		world,
		stats,
		static_cast<std::uint8_t>(player),
		powerup,
		simulation_tick,
		true);
	return true;
}

void deathmatch_scenarios_apply_player_controls(
	const Runtime& runtime,
	std::uint16_t player,
	game::FlightDemand& demand)
{
	if (!valid_player_index(player))
	{
		return;
	}
	const std::int8_t powerup =
		runtime.deathmatch.powerup_active[player];
	if (powerup == static_cast<std::int8_t>(
			DeathmatchPowerup::half_maximum_speed))
	{
		demand.throttle = std::min(demand.throttle, 0.5f);
	}
	else if (powerup == static_cast<std::int8_t>(
			DeathmatchPowerup::reverse_yoke))
	{
		demand.roll = -demand.roll;
		demand.pitch = -demand.pitch;
		demand.yaw = -demand.yaw;
		demand.strafe = -demand.strafe;
	}
}

bool deathmatch_scenarios_shield_recharge_allowed(
	const Runtime& runtime,
	std::uint16_t player)
{
	if (!valid_player_index(player))
	{
		return true;
	}
	if (runtime.deathmatch.powerup_active[player]
		== static_cast<std::int8_t>(
			DeathmatchPowerup::shields_down))
	{
		return false;
	}
	return selected_scenario(runtime)
			!= DeathmatchScenario::hunt_the_shadow
		|| runtime.deathmatch.shadow_holder
			!= static_cast<std::int8_t>(player);
}

bool deathmatch_scenarios_serialize(
	const Runtime& runtime,
	const game::World& world,
	std::uint8_t (&bytes)[kDeathmatchScenarioStateBytes],
	std::uint16_t& bit_count)
{
	std::memset(bytes, 0, sizeof(bytes));
	BitWriter writer{
		bytes,
		std::size(bytes),
		0,
		true,
	};
	switch (selected_scenario(runtime))
	{
	case DeathmatchScenario::nuclear_threat:
		for (std::uint8_t ordinal = 0;
			ordinal < kDeathmatchScenarioBeaconCapacity;
			++ordinal)
		{
			glm::vec3 position =
				runtime.deathmatch.nuclear_snapshot_position[ordinal];
			if (ordinal < runtime.deathmatch.nuclear_beacon_count)
			{
				const std::uint16_t beacon =
					runtime.deathmatch.nuclear_beacon[ordinal];
				if (beacon < std::size(world.objects)
					&& world.objects[beacon].active)
				{
					position = world.objects[beacon].position;
				}
			}
			writer.write(std::bit_cast<std::uint32_t>(position.x), 32);
			writer.write(std::bit_cast<std::uint32_t>(position.y), 32);
			writer.write(std::bit_cast<std::uint32_t>(position.z), 32);
		}
		for (std::uint8_t ordinal = 0;
			ordinal < kDeathmatchScenarioBeaconCapacity;
			++ordinal)
		{
			bool visible = false;
			if (ordinal < runtime.deathmatch.nuclear_beacon_count)
			{
				const std::uint16_t beacon =
					runtime.deathmatch.nuclear_beacon[ordinal];
				visible = beacon < std::size(world.objects)
					&& world.objects[beacon].active
					&& (world.objects[beacon].runtime_flags
						& game::kObjectFlagDisabled) == 0;
			}
			writer.write(visible ? 1u : 0u, 1);
		}
		for (std::uint8_t player = 0;
			player < kDeathmatchScenarioPlayerCapacity;
			++player)
		{
			const std::int32_t counter =
				player < std::size(world.objects)
					&& world.objects[player].active
				? world.objects[player].deathmatch_scenario_counter
				: -1;
			writer.write(
				counter == -1
					? 0x200u
					: static_cast<std::uint32_t>(counter) & 0x3ffu,
				10);
		}
		break;
	case DeathmatchScenario::dark_reign:
		writer.write(
			runtime.deathmatch.dark_holder < 0
				? 8u
				: static_cast<std::uint8_t>(
					runtime.deathmatch.dark_holder),
			4);
		break;
	case DeathmatchScenario::tag_bomb:
		writer.write(
			runtime.deathmatch.tag_holder < 0
				? 8u
				: static_cast<std::uint8_t>(
					runtime.deathmatch.tag_holder),
			4);
		writer.write(
			runtime.deathmatch.tag_last_tagger < 0
				? 8u
				: static_cast<std::uint8_t>(
					runtime.deathmatch.tag_last_tagger),
			4);
		break;
	case DeathmatchScenario::hunt_the_shadow:
		writer.write(
			runtime.deathmatch.shadow_holder < 0
				? 8u
				: static_cast<std::uint8_t>(
					runtime.deathmatch.shadow_holder),
			4);
		break;
	case DeathmatchScenario::vampires:
		for (std::uint8_t player = 0;
			player < kDeathmatchScenarioPlayerCapacity;
			++player)
		{
			writer.write(
				runtime.deathmatch.vampire[player] ? 1u : 0u,
				1);
		}
		break;
	case DeathmatchScenario::asteroid_field:
	case DeathmatchScenario::none:
		break;
	}
	bit_count = writer.bit;
	return writer.valid;
}

bool deathmatch_scenarios_deserialize(
	Runtime& runtime,
	const std::uint8_t* bytes,
	std::size_t byte_count,
	std::uint16_t bit_count)
{
	if (bytes == nullptr
		&& bit_count != 0)
	{
		return false;
	}
	BitReader reader{
		bytes,
		byte_count,
		bit_count,
		0,
		true,
	};
	DeathmatchScenarioState& state = runtime.deathmatch;
	switch (selected_scenario(runtime))
	{
	case DeathmatchScenario::nuclear_threat:
		for (std::uint8_t ordinal = 0;
			ordinal < kDeathmatchScenarioBeaconCapacity;
			++ordinal)
		{
			glm::vec3& position =
				state.nuclear_snapshot_position[ordinal];
			position.x = std::bit_cast<float>(reader.read(32));
			position.y = std::bit_cast<float>(reader.read(32));
			position.z = std::bit_cast<float>(reader.read(32));
		}
		state.nuclear_snapshot_visible = 0;
		for (std::uint8_t ordinal = 0;
			ordinal < kDeathmatchScenarioBeaconCapacity;
			++ordinal)
		{
			if (reader.read(1) != 0)
			{
				state.nuclear_snapshot_visible =
					static_cast<std::uint8_t>(
						state.nuclear_snapshot_visible
						| (1u << ordinal));
			}
		}
		for (std::uint8_t player = 0;
			player < kDeathmatchScenarioPlayerCapacity;
			++player)
		{
			const std::uint16_t encoded =
				static_cast<std::uint16_t>(reader.read(10));
			state.nuclear_snapshot_counter[player] =
				encoded == 0x200u
					? -1
					: static_cast<std::int16_t>(encoded);
		}
		break;
	case DeathmatchScenario::dark_reign:
	{
		const std::uint8_t holder =
			static_cast<std::uint8_t>(reader.read(4));
		state.dark_snapshot_holder =
			holder == 8 ? -1 : static_cast<std::int8_t>(holder);
		break;
	}
	case DeathmatchScenario::tag_bomb:
	{
		const std::uint8_t holder =
			static_cast<std::uint8_t>(reader.read(4));
		const std::uint8_t tagger =
			static_cast<std::uint8_t>(reader.read(4));
		state.tag_snapshot_holder =
			holder == 8 ? -1 : static_cast<std::int8_t>(holder);
		state.tag_snapshot_tagger =
			tagger == 8 ? -1 : static_cast<std::int8_t>(tagger);
		break;
	}
	case DeathmatchScenario::hunt_the_shadow:
	{
		const std::uint8_t holder =
			static_cast<std::uint8_t>(reader.read(4));
		state.shadow_snapshot_holder =
			holder == 8 ? -1 : static_cast<std::int8_t>(holder);
		break;
	}
	case DeathmatchScenario::vampires:
		state.vampire_snapshot_mask = 0;
		for (std::uint8_t player = 0;
			player < kDeathmatchScenarioPlayerCapacity;
			++player)
		{
			if (reader.read(1) != 0)
			{
				state.vampire_snapshot_mask =
					static_cast<std::uint8_t>(
						state.vampire_snapshot_mask
						| (1u << player));
			}
		}
		break;
	case DeathmatchScenario::asteroid_field:
	case DeathmatchScenario::none:
		break;
	}
	if (!reader.valid || reader.bit != bit_count)
	{
		return false;
	}
	state.snapshot_pending = true;
	return true;
}

bool deathmatch_scenarios_receive(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const NetworkOutboundMessage& message,
	std::uint32_t simulation_tick)
{
	if (message.kind != NetworkOutboundKind::gameplay)
	{
		return false;
	}
	initialize_powerup_pool(runtime, world, stats);
	const auto valid_message_player =
		[&](std::int8_t player)
		{
			return player >= 0
				&& player < static_cast<std::int8_t>(
					kDeathmatchScenarioPlayerCapacity);
		};
	switch (message.opcode)
	{
	case NetworkGameplayOpcode::deathmatch_restart:
		queue_event(
			runtime.deathmatch,
			DeathmatchScenarioMessageKind::restart);
		return true;
	case NetworkGameplayOpcode::deathmatch_state_request:
		if (runtime.network.role != NetworkRole::host
			|| !valid_message_player(message.scenario_player))
		{
			return false;
		}
		send_scenario_state(
			runtime,
			world,
			static_cast<std::uint8_t>(message.scenario_player));
		return true;
	case NetworkGameplayOpcode::deathmatch_state:
		if (!deathmatch_scenarios_deserialize(
				runtime,
				message.scenario_state,
				(message.scenario_state_bits + 7u) / 8u,
				message.scenario_state_bits))
		{
			return false;
		}
			deathmatch_scenarios_initialize(
				runtime, world, stats, simulation_tick);
		return true;
	case NetworkGameplayOpcode::tag_bomb_assignment:
		if (selected_scenario(runtime)
				!= DeathmatchScenario::tag_bomb
			|| message.scenario_player < -1
			|| message.scenario_player >= 8
			|| message.scenario_secondary_player < -1
			|| message.scenario_secondary_player >= 8)
		{
			return false;
		}
		tag_set(
			runtime,
			world,
			message.scenario_player,
			message.scenario_secondary_player,
			false);
		return true;
	case NetworkGameplayOpcode::tag_bomb_detonate:
		if (selected_scenario(runtime)
			!= DeathmatchScenario::tag_bomb)
		{
			return false;
		}
		tag_detonate(runtime, world, false);
		return true;
	case NetworkGameplayOpcode::dark_reign_tower_state:
		if (selected_scenario(runtime)
				!= DeathmatchScenario::dark_reign
			|| message.tower_state != 7)
		{
			return false;
		}
		if (runtime.deathmatch.dark_tower < std::size(world.objects)
			&& world.objects[
				runtime.deathmatch.dark_tower].active)
		{
			ai::command_clear(
				world,
				world.objects[runtime.deathmatch.dark_tower]);
		}
		return true;
	case NetworkGameplayOpcode::vampire_assignment:
		if (selected_scenario(runtime)
				!= DeathmatchScenario::vampires
			|| !valid_message_player(message.scenario_player))
		{
			return false;
		}
		vampire_set(
			runtime,
			world,
			static_cast<std::uint8_t>(message.scenario_player),
			message.scenario_flag,
			false);
		return true;
	case NetworkGameplayOpcode::shadow_assignment:
		if (selected_scenario(runtime)
				!= DeathmatchScenario::hunt_the_shadow
			|| message.scenario_player < -1
			|| message.scenario_player >= 8)
		{
			return false;
		}
		shadow_set(
			runtime,
			world,
			message.scenario_player,
			false,
			simulation_tick);
		return true;
	case NetworkGameplayOpcode::shadow_kill:
		if (selected_scenario(runtime)
				!= DeathmatchScenario::hunt_the_shadow
			|| !valid_message_player(message.scenario_player))
		{
			return false;
		}
		shadow_kill(
			runtime,
			world,
			static_cast<std::uint8_t>(message.scenario_player),
			false);
		return true;
	case NetworkGameplayOpcode::nuclear_reset_player:
		if (selected_scenario(runtime)
				!= DeathmatchScenario::nuclear_threat
			|| !valid_message_player(message.scenario_player))
		{
			return false;
		}
		nuclear_reset_player(
			runtime,
			world,
			stats,
			static_cast<std::uint8_t>(message.scenario_player),
			false);
		return true;
	case NetworkGameplayOpcode::dark_reign_drop:
		if (selected_scenario(runtime)
				!= DeathmatchScenario::dark_reign
			|| !valid_message_player(message.scenario_player))
		{
			return false;
		}
		dark_drop(
			runtime,
			world,
			stats,
			static_cast<std::uint8_t>(message.scenario_player),
			false);
		return true;
	case NetworkGameplayOpcode::nuclear_success:
		if (selected_scenario(runtime)
				!= DeathmatchScenario::nuclear_threat
			|| !valid_message_player(message.scenario_player))
		{
			return false;
		}
		nuclear_success(
			runtime,
			world,
			stats,
			static_cast<std::uint8_t>(message.scenario_player),
			simulation_tick,
			false);
		return true;
	case NetworkGameplayOpcode::deathmatch_pickup:
		if (!valid_message_player(message.scenario_player)
			|| message.scenario_object >= std::size(world.objects)
			|| !world.objects[message.scenario_object].active)
		{
			return false;
		}
		if (world.objects[message.scenario_object].type
			== kPowerupType)
		{
			pickup_generic(
				runtime,
				world,
				stats,
				static_cast<std::uint8_t>(message.scenario_player),
				message.scenario_object,
				simulation_tick,
				false);
		}
		else
		{
			pickup_special(
				runtime,
				world,
				static_cast<std::uint8_t>(message.scenario_player),
				message.scenario_object,
				false);
		}
		return true;
	case NetworkGameplayOpcode::deathmatch_powerup_activate:
		if (!valid_message_player(message.scenario_player)
			|| !valid_powerup(message.scenario_powerup))
		{
			return false;
		}
		trigger_powerup(
			runtime,
			world,
			stats,
			static_cast<std::uint8_t>(message.scenario_player),
			message.scenario_powerup,
			simulation_tick,
			false);
		return true;
	case NetworkGameplayOpcode::deathmatch_unhide_object:
		if (message.scenario_object >= std::size(world.objects))
		{
			return false;
		}
			unhide_object(
				runtime, world, message.scenario_object, false);
			return true;
	case NetworkGameplayOpcode::deathmatch_proximity_mine:
		if (!valid_message_player(message.scenario_player)
			|| message.scenario_object >= std::size(world.objects)
			|| !world.objects[message.scenario_object].active)
		{
			return false;
		}
		{
		// Guaranteed opcode 0x1d has already synchronized the retained
		// hidden pool object's pose. The following opcode 0x4b recreates it
		// at that pose; 0x4b itself carries no transform.
		const glm::vec3 position =
			world.objects[message.scenario_object].position;
		const glm::mat3 orientation =
			world.objects[message.scenario_object].orientation;
		materialize_proximity_mine(
			runtime,
			world,
			stats,
			static_cast<std::uint8_t>(message.scenario_player),
			message.scenario_object,
			&position,
			&orientation,
			false);
		return true;
		}
	case NetworkGameplayOpcode::deathmatch_reposition_object:
		if (message.scenario_object > UINT8_MAX)
		{
			return false;
		}
		{
		const std::uint16_t dropped = find_first_type(
			world,
			message.scenario_object,
			true);
		if (dropped == UINT16_MAX)
		{
			return false;
		}
		unhide_object(runtime, world, dropped, false);
		return true;
		}
	default:
		return false;
	}
}

void deathmatch_scenarios_publish_restart(Runtime& runtime)
{
	// Only Nuclear Threat has the nonzero +0x3c record member tested by
	// Deathmatch_notify_restart (0x004b2eb0).
	if (selected_scenario(runtime)
		!= DeathmatchScenario::nuclear_threat)
	{
		return;
	}
	queue_event(
		runtime.deathmatch,
		DeathmatchScenarioMessageKind::restart);
	(void)queue_broadcast(
		runtime,
		NetworkGameplayOpcode::deathmatch_restart);
}

DeathmatchScenarioHudStatus deathmatch_scenarios_hud_status(
	const Runtime& runtime,
	const game::World& world)
{
	const std::uint8_t local = runtime.network.local_player;
	if (local >= std::size(world.objects)
		|| !world.objects[local].active)
	{
		return {};
	}
	switch (selected_scenario(runtime))
	{
	case DeathmatchScenario::nuclear_threat:
		return {
			0x0b,
			UINT16_MAX,
			std::max(
				0,
				world.objects[local].
					deathmatch_scenario_counter),
			true,
			true,
		};
	case DeathmatchScenario::dark_reign:
		return {
			0x0c,
			UINT16_MAX,
			0,
			false,
			world.objects[local].
				deathmatch_scenario_counter != -1,
		};
	case DeathmatchScenario::tag_bomb:
		return {
			0x0a,
			0x2d5,
			runtime.deathmatch.tag_timer / 100 + 1,
			true,
			runtime.deathmatch.tag_holder
				== static_cast<std::int8_t>(local),
		};
	case DeathmatchScenario::hunt_the_shadow:
		return {
			0x0d,
			UINT16_MAX,
			0,
			false,
			runtime.deathmatch.shadow_holder
				== static_cast<std::int8_t>(local),
		};
	case DeathmatchScenario::vampires:
		return {
			0x0e,
			UINT16_MAX,
			0,
			false,
			runtime.deathmatch.vampire[local],
		};
	default:
		return {};
	}
}

DeathmatchPowerupHudStatus deathmatch_powerup_hud_status(
	const Runtime& runtime,
	const game::World& world,
	std::uint32_t simulation_tick)
{
	const std::uint8_t player = runtime.network.local_player;
	if (!valid_player_index(player)
		|| player >= std::size(world.objects)
		|| !world.objects[player].active)
	{
		return {};
	}
	const DeathmatchScenarioState& state = runtime.deathmatch;
	const std::int8_t powerup = state.powerup_active[player];
	if (!valid_powerup(powerup))
	{
		return {};
	}
	const std::uint32_t expiry = state.powerup_expiry[player];
	if (expiry != UINT32_MAX && simulation_tick >= expiry)
	{
		return {};
	}
	const std::uint32_t acquired = state.powerup_acquired[player];
	const std::uint32_t elapsed = simulation_tick - acquired;
	if (elapsed <= 100u
		&& ((simulation_tick + acquired) % 50u) >= 25u)
	{
		return {};
	}
	const DeathmatchPowerupDefinition& definition =
		kPowerups[static_cast<std::uint8_t>(powerup)];
	DeathmatchPowerupHudStatus result;
	result.shape = definition.shape;
	result.bar_offset = definition.bar_offset;
	result.draw_bar =
		(definition.flags & kPowerupFlagCountdown) != 0;
	result.visible = true;
	if (result.draw_bar
		&& expiry != UINT32_MAX
		&& definition.duration != 0)
	{
		const float remaining = static_cast<float>(
			expiry - simulation_tick);
		const float rounded = std::nearbyint(
			remaining / static_cast<float>(definition.duration)
				* -31.0f);
		result.bar_length = static_cast<std::uint8_t>(
			std::clamp(
				-static_cast<std::int32_t>(rounded),
				0,
				31));
	}
	return result;
}

bool deathmatch_scenarios_target_label(
	const Runtime& runtime,
	const game::WorldObject& target,
	const LanguageTable& language,
	char* output,
	std::size_t output_size)
{
	if (output == nullptr || output_size == 0)
	{
		return false;
	}
	output[0] = '\0';
	const std::uint16_t player = target.mission_index;
	// The retail callbacks identify player records through object+4 <= 8.
	// In the value runtime player mission indices are the stable equivalent.
	if (player >= kDeathmatchScenarioPlayerCapacity)
	{
		return false;
	}
	switch (selected_scenario(runtime))
	{
	case DeathmatchScenario::nuclear_threat:
	{
		const std::int32_t count = std::max(
			0, target.deathmatch_scenario_counter);
		std::snprintf(
			output,
			output_size,
			"%d %s",
			count,
			language_text(
				language, count == 1 ? 0x2d6 : 0x2d7));
		return true;
	}
	case DeathmatchScenario::dark_reign:
		if (target.deathmatch_scenario_counter == -1)
		{
			return false;
		}
		std::snprintf(
			output,
			output_size,
			"%s",
			language_text(language, 0x2d8));
		return true;
	case DeathmatchScenario::tag_bomb:
		std::snprintf(
			output,
			output_size,
			"%s",
			language_text(
				language,
				runtime.deathmatch.tag_holder
						== static_cast<std::int8_t>(player)
					? 0x2d9 : 0x2da));
		return true;
	case DeathmatchScenario::vampires:
		std::snprintf(
			output,
			output_size,
			"%s",
			language_text(
				language,
				runtime.deathmatch.vampire[player]
					? 0x2db : 0x2dc));
		return true;
	default:
		return false;
	}
}

std::uint16_t deathmatch_scenarios_scoreboard_shape(
	const Runtime& runtime,
	const game::World& world,
	std::uint8_t player)
{
	if (player >= kDeathmatchScenarioPlayerCapacity)
	{
		return UINT16_MAX;
	}
	switch (selected_scenario(runtime))
	{
	case DeathmatchScenario::nuclear_threat:
		return 0x10;
	case DeathmatchScenario::dark_reign:
		return player < std::size(world.objects)
			&& world.objects[player].active
			&& world.objects[player].deathmatch_scenario_counter != -1
				? 0x11 : UINT16_MAX;
	case DeathmatchScenario::tag_bomb:
		return runtime.deathmatch.tag_holder
				== static_cast<std::int8_t>(player)
			? 0x0f : UINT16_MAX;
	case DeathmatchScenario::vampires:
		return runtime.deathmatch.vampire[player]
			? 0x12 : UINT16_MAX;
	default:
		return UINT16_MAX;
	}
}

std::int32_t deathmatch_scenarios_scoreboard_value(
	const Runtime& runtime,
	const game::World& world,
	std::uint8_t player)
{
	if (selected_scenario(runtime)
			!= DeathmatchScenario::nuclear_threat
		|| player >= kDeathmatchScenarioPlayerCapacity
		|| player >= std::size(world.objects)
		|| !world.objects[player].active)
	{
		return -1;
	}
	return std::max(
		0, world.objects[player].deathmatch_scenario_counter);
}

void deathmatch_scenarios_flush_messages(
	Runtime& runtime,
	hud::Runtime& hud,
	const LanguageTable& language,
	std::uint32_t simulation_tick)
{
	for (std::uint8_t index = 0;
		index < runtime.deathmatch.message_count;
		++index)
	{
		char text[100]{};
		format_scenario_message(
			runtime,
			language,
			runtime.deathmatch.messages[index],
			text);
		if (text[0] != '\0')
		{
			hud::runtime_enqueue_message(
				hud, text, simulation_tick);
		}
	}
	runtime.deathmatch.message_count = 0;
}
}
