#include "mission/network_runtime.hpp"

#include "ai/runtime.hpp"
#include "assets/ship_stats.hpp"
#include "core/math.hpp"
#include "core/mission_log.hpp"
#include "game/world.hpp"
#include "mission/deathmatch_scenarios.hpp"
#include "mission/runtime.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>

namespace sl_open::mission
{
namespace
{
constexpr std::uint8_t kBroadcastDestination = UINT8_MAX;
constexpr float kBankDeltaDecodeScale = 0x1.83060cp-5f;
constexpr float kBankStateDecodeScale = 0x1.861862p-5f;
constexpr float kComponentStateDecodeScale = 0x1.041042p-7f;
constexpr float kReceivedImpactScale = 0.05000000074505806f;
constexpr float kObjectDemandEncodeScale = 31.0f;
constexpr float kObjectDemandDecodeScale = 0.032258063554763794f;
constexpr float kObjectAngularEncodeScale = 4.933803081512451f;
constexpr float kObjectAngularDecodeScale = 0.20268340408802032f;
constexpr float kObjectVelocityEncodeScale = 127.0f;
constexpr float kObjectVelocityDecodeScale = 0.007874015718698502f;
constexpr float kObjectPositionEncodeScale = 0.0625f;
constexpr float kObjectPositionDecodeScale = 16.0f;
constexpr float kObjectOrientationEncodeScale = 162.9746551513672f;
constexpr float kObjectOrientationDecodeScale = 0.006135923322290182f;
constexpr float kObjectPositionModeOneLimit = 1048576.0f;
constexpr float kObjectPublicationDistanceSquared =
	999999995904.0f;
constexpr std::uint32_t kObjectPublicationBitBudget = 0x640u;

std::uint16_t retained_retail_bit_count(
	const NetworkOutboundMessage& message)
{
	if (message.retail_bit_count != 0)
	{
		return message.retail_bit_count;
	}
	switch (message.kind)
	{
	case NetworkOutboundKind::object_state:
	{
		std::uint16_t bits = 29;
		if (message.object_state_motion_present)
		{
			bits = static_cast<std::uint16_t>(bits + 69u);
		}
		switch (message.object_state_position_mode)
		{
		case 1:
			bits = static_cast<std::uint16_t>(bits + 63u);
			break;
		case 2:
			bits = static_cast<std::uint16_t>(bits + 87u);
			break;
		case 3:
			bits = static_cast<std::uint16_t>(bits + 48u);
			break;
		default:
			break;
		}
		if (message.object_state_orientation_present)
		{
			bits = static_cast<std::uint16_t>(bits + 30u);
		}
		return bits;
	}
	case NetworkOutboundKind::ai_sequence_sync:
		// DPGMESSAGE_RIPPERSYNC: opcode seven, object nine, sync four.
		return 20;
	case NetworkOutboundKind::ai_deferred_command:
		// DPGMESSAGE_AI_DECISION's common prefix is opcode seven, object
		// nine, delay eleven, seed eight and command ID eight. Command 105
		// has a compact 9/8/5/11-bit payload; every other command copies
		// the complete 24-byte command tail.
		return message.command_id == 105 ? 76 : 235;
	case NetworkOutboundKind::chat:
		// DirectPlay chat bypasses all four gameplay bit buffers.
		return 0;
	case NetworkOutboundKind::gameplay:
		break;
	}

	switch (message.opcode)
	{
	case NetworkGameplayOpcode::landing:
		// DPGMESSAGE_LANDING contains only its seven-bit opcode.
		return 7;
	case NetworkGameplayOpcode::component_damage:
		return 49;
	case NetworkGameplayOpcode::component_state:
		return 31;
	case NetworkGameplayOpcode::bank_damage:
		return static_cast<std::uint16_t>(
			33u + std::popcount(
				static_cast<unsigned>(message.damage_bank_mask))
				* 7u);
	case NetworkGameplayOpcode::bank_state:
		return 81;
	case NetworkGameplayOpcode::pause_state:
		// Pause_send, 0x004baae0: opcode seven, desired state one, reason four.
		return 12;
	case NetworkGameplayOpcode::deathmatch_restart:
		return 7;
	case NetworkGameplayOpcode::deathmatch_state_request:
		return 10;
	case NetworkGameplayOpcode::deathmatch_state:
		return static_cast<std::uint16_t>(
			7u + message.scenario_state_bits);
	case NetworkGameplayOpcode::deathmatch_respawn:
		return 19;
	case NetworkGameplayOpcode::tag_bomb_assignment:
		return 15;
	case NetworkGameplayOpcode::tag_bomb_detonate:
		return 7;
	case NetworkGameplayOpcode::dark_reign_tower_state:
		return 10;
	case NetworkGameplayOpcode::vampire_assignment:
		return 11;
	case NetworkGameplayOpcode::shadow_assignment:
		return 11;
	case NetworkGameplayOpcode::shadow_kill:
	case NetworkGameplayOpcode::nuclear_reset_player:
	case NetworkGameplayOpcode::dark_reign_drop:
	case NetworkGameplayOpcode::nuclear_success:
		return 10;
	case NetworkGameplayOpcode::session_script_ready:
		return 10;
	case NetworkGameplayOpcode::session_script_start:
		return 15;
	case NetworkGameplayOpcode::player_stats:
		return 71;
	case NetworkGameplayOpcode::deathmatch_pickup:
		return 19;
	case NetworkGameplayOpcode::deathmatch_powerup_activate:
		return 14;
	case NetworkGameplayOpcode::deathmatch_unhide_object:
		return 12;
	case NetworkGameplayOpcode::player_attack_request:
	case NetworkGameplayOpcode::player_backoff_request:
		// Opcodes 0x3f/0x40 append the sender's nine-bit TargetRef object.
		return 16;
	case NetworkGameplayOpcode::player_help_request:
		return 7;
	case NetworkGameplayOpcode::cloak_state:
		// Cloak_send, 0x004bab60: seven-bit opcode plus desired-state bit.
		return 8;
	case NetworkGameplayOpcode::player_ejected:
		return 16;
	case NetworkGameplayOpcode::script_sync_ready:
		return 14;
	case NetworkGameplayOpcode::script_sync_restart:
		return 22;
	case NetworkGameplayOpcode::friendly_fire:
		return 8;
	case NetworkGameplayOpcode::deathmatch_proximity_mine:
		return 19;
	case NetworkGameplayOpcode::player_departure:
		return 11;
	case NetworkGameplayOpcode::player_target_reference:
		// FUN_004bb980: opcode seven, object nine, component seven.
		return 23;
	case NetworkGameplayOpcode::deathmatch_reposition_object:
		return 16;
	case NetworkGameplayOpcode::object_state:
		break;
	}
	return 0;
}

const char* role_name(NetworkRole role)
{
	switch (role)
	{
	case NetworkRole::host: return "host";
	case NetworkRole::client: return "client";
	default: return "offline";
	}
}

bool queue_message(
	NetworkRuntime& network,
	const NetworkOutboundMessage& message,
	std::uint8_t* queued_index = nullptr)
{
	if (network.role == NetworkRole::offline)
	{
		return false;
	}
	if (network.outbound_count >= kNetworkOutboundCapacity)
	{
		if (!network.outbound_overflow_logged)
		{
			network.outbound_overflow_logged = true;
			diagnostics::mission_log(
				"network outbound queue full capacity=%u",
				static_cast<unsigned>(kNetworkOutboundCapacity));
		}
		return false;
	}
	const std::uint8_t write = static_cast<std::uint8_t>(
		(network.outbound_read + network.outbound_count)
		% kNetworkOutboundCapacity);
	network.outbound[write] = message;
	network.outbound_damage[write] = {};
	++network.outbound_count;
	if (queued_index != nullptr)
	{
		*queued_index = write;
	}
	return true;
}

bool is_damage_opcode(NetworkGameplayOpcode opcode)
{
	return opcode == NetworkGameplayOpcode::component_damage
		|| opcode == NetworkGameplayOpcode::component_state
		|| opcode == NetworkGameplayOpcode::bank_damage
		|| opcode == NetworkGameplayOpcode::bank_state;
}

bool damage_record_current(
	const NetworkOutboundMessage& message,
	const NetworkDamageAccumulator& accumulator)
{
	return !is_damage_opcode(message.opcode)
		|| (accumulator.target != nullptr
			&& accumulator.target->generation
				== accumulator.target_generation);
}

void discard_stale_damage_records(NetworkRuntime& network)
{
	const std::uint8_t original_count = network.outbound_count;
	std::uint8_t retained_count = 0;
	for (std::uint8_t offset = 0; offset < original_count; ++offset)
	{
		const std::uint8_t source = static_cast<std::uint8_t>(
			(network.outbound_read + offset)
			% kNetworkOutboundCapacity);
		if (!damage_record_current(
				network.outbound[source],
				network.outbound_damage[source]))
		{
			continue;
		}
		const std::uint8_t destination =
			static_cast<std::uint8_t>(
				(network.outbound_read + retained_count)
				% kNetworkOutboundCapacity);
		if (destination != source)
		{
			network.outbound[destination] =
				network.outbound[source];
			network.outbound_damage[destination] =
				network.outbound_damage[source];
		}
		++retained_count;
	}
	for (std::uint8_t offset = retained_count;
		offset < original_count;
		++offset)
	{
		const std::uint8_t index = static_cast<std::uint8_t>(
			(network.outbound_read + offset)
			% kNetworkOutboundCapacity);
		network.outbound[index] = {};
		network.outbound_damage[index] = {};
	}
	network.outbound_count = retained_count;
	if (retained_count == 0)
	{
		network.outbound_read = 0;
		network.outbound_overflow_logged = false;
	}
}

void discard_directed_messages_for_player(
	NetworkRuntime& network,
	std::uint8_t player)
{
	const std::uint8_t original_count = network.outbound_count;
	std::uint8_t retained_count = 0;
	for (std::uint8_t offset = 0; offset < original_count; ++offset)
	{
		const std::uint8_t source = static_cast<std::uint8_t>(
			(network.outbound_read + offset)
			% kNetworkOutboundCapacity);
		if (network.outbound[source].destination_player == player)
		{
			continue;
		}
		const std::uint8_t destination =
			static_cast<std::uint8_t>(
				(network.outbound_read + retained_count)
				% kNetworkOutboundCapacity);
		if (destination != source)
		{
			network.outbound[destination] =
				network.outbound[source];
			network.outbound_damage[destination] =
				network.outbound_damage[source];
		}
		++retained_count;
	}
	for (std::uint8_t offset = retained_count;
		offset < original_count;
		++offset)
	{
		const std::uint8_t index = static_cast<std::uint8_t>(
			(network.outbound_read + offset)
			% kNetworkOutboundCapacity);
		network.outbound[index] = {};
		network.outbound_damage[index] = {};
	}
	network.outbound_count = retained_count;
	if (retained_count == 0)
	{
		network.outbound_read = 0;
		network.outbound_overflow_logged = false;
	}
}

void discard_session_handshake_messages(NetworkRuntime& network)
{
	const std::uint8_t original_count = network.outbound_count;
	std::uint8_t retained_count = 0;
	for (std::uint8_t offset = 0; offset < original_count; ++offset)
	{
		const std::uint8_t source = static_cast<std::uint8_t>(
			(network.outbound_read + offset)
			% kNetworkOutboundCapacity);
		const NetworkOutboundMessage& message =
			network.outbound[source];
		if (message.kind == NetworkOutboundKind::gameplay
			&& (message.opcode
					== NetworkGameplayOpcode::session_script_ready
				|| message.opcode
					== NetworkGameplayOpcode::session_script_start))
		{
			continue;
		}
		const std::uint8_t destination =
			static_cast<std::uint8_t>(
				(network.outbound_read + retained_count)
				% kNetworkOutboundCapacity);
		if (destination != source)
		{
			network.outbound[destination] =
				network.outbound[source];
			network.outbound_damage[destination] =
				network.outbound_damage[source];
		}
		++retained_count;
	}
	for (std::uint8_t offset = retained_count;
		offset < original_count;
		++offset)
	{
		const std::uint8_t index = static_cast<std::uint8_t>(
			(network.outbound_read + offset)
			% kNetworkOutboundCapacity);
		network.outbound[index] = {};
		network.outbound_damage[index] = {};
	}
	network.outbound_count = retained_count;
	if (retained_count == 0)
	{
		network.outbound_read = 0;
		network.outbound_overflow_logged = false;
	}
}

std::uint8_t damage_priority(
	NetworkGameplayOpcode opcode,
	bool destroyed)
{
	if (opcode == NetworkGameplayOpcode::component_damage)
	{
		return 0;
	}
	if (opcode == NetworkGameplayOpcode::component_state
		&& !destroyed)
	{
		return 10;
	}
	return 20;
}

struct DeferredDamageMessage
{
	NetworkOutboundMessage* message{};
	NetworkDamageAccumulator* accumulator{};
	bool inserted{};
};

DeferredDamageMessage find_or_queue_damage(
	NetworkRuntime& network,
	NetworkGameplayOpcode opcode,
	std::uint16_t object_index,
	std::uint32_t component_network_id,
	bool destroyed)
{
	discard_stale_damage_records(network);
	for (std::uint8_t offset = 0;
		offset < network.outbound_count;
		++offset)
	{
		const std::uint8_t index = static_cast<std::uint8_t>(
			(network.outbound_read + offset)
			% kNetworkOutboundCapacity);
		NetworkOutboundMessage& candidate = network.outbound[index];
		if (candidate.kind != NetworkOutboundKind::gameplay
			|| candidate.opcode != opcode
			|| candidate.object_index != object_index)
		{
			continue;
		}
		if ((opcode == NetworkGameplayOpcode::component_damage
			|| opcode == NetworkGameplayOpcode::component_state)
			&& network.outbound_damage[index].component_network_id
				!= component_network_id)
		{
			continue;
		}
		return {
			&candidate,
			&network.outbound_damage[index],
			false,
		};
	}

	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode = opcode;
	message.delivery = NetworkDelivery::broadcast_conditional;
	message.source_player = network.local_player;
	message.destination_player = kBroadcastDestination;
	message.object_index =
		static_cast<std::uint16_t>(object_index & 0x01ffu);
	// FUN_004ba810/FUN_004ba8b0 retain the complete model-node +0xfc ID
	// in the deferred record. The packet serializer alone writes eight
	// bits, thereby truncating modulo 256 without a range check.
	message.damage_component_id =
		static_cast<std::uint8_t>(component_network_id);

	// FUN_004ba6b0 gives every object twenty deferred-record slots. When
	// full, it replaces the first record whose numeric priority is greater
	// than the incoming priority. Only the four damage record families are
	// represented in this semantic queue, so apply the same replacement
	// rule among those records before falling back to its global capacity.
	const std::uint8_t incoming_priority =
		damage_priority(opcode, destroyed);
	std::uint8_t object_damage_count = 0;
	for (std::uint8_t offset = 0;
		offset < network.outbound_count;
		++offset)
	{
		const std::uint8_t index = static_cast<std::uint8_t>(
			(network.outbound_read + offset)
			% kNetworkOutboundCapacity);
		const NetworkOutboundMessage& candidate =
			network.outbound[index];
		if (candidate.kind == NetworkOutboundKind::gameplay
			&& candidate.object_index == message.object_index
			&& is_damage_opcode(candidate.opcode))
		{
			++object_damage_count;
		}
	}
	if (object_damage_count >= 20)
	{
		for (std::uint8_t offset = 0;
			offset < network.outbound_count;
			++offset)
		{
			const std::uint8_t index = static_cast<std::uint8_t>(
				(network.outbound_read + offset)
				% kNetworkOutboundCapacity);
			NetworkOutboundMessage& candidate =
				network.outbound[index];
			if (candidate.kind != NetworkOutboundKind::gameplay
				|| candidate.object_index != message.object_index
				|| !is_damage_opcode(candidate.opcode)
				|| candidate.deferred_priority
					<= incoming_priority)
			{
				continue;
			}
			message.deferred_priority = incoming_priority;
			candidate = message;
			network.outbound_damage[index] = {};
			return {
				&candidate,
				&network.outbound_damage[index],
				true,
			};
		}
		return {};
	}

	message.deferred_priority = incoming_priority;
	std::uint8_t index = 0;
	if (!queue_message(network, message, &index))
	{
		return {};
	}
	return {
		&network.outbound[index],
		&network.outbound_damage[index],
		true,
	};
}

int retail_round(float value)
{
	// FUN_004c3330 uses x87 FISTP under the default round-to-nearest mode.
	// Masked x87 invalid conversion produces the signed-integer indefinite
	// value. Avoid C++'s undefined out-of-range float-to-int conversion
	// while preserving that result for NaN, infinities and overflow.
	const double rounded = std::nearbyint(
		static_cast<double>(value));
	if (!std::isfinite(rounded)
		|| rounded
			> static_cast<double>(
				std::numeric_limits<int>::max())
		|| rounded
			< static_cast<double>(
				std::numeric_limits<int>::min()))
	{
		return std::numeric_limits<int>::min();
	}
	return static_cast<int>(rounded);
}

std::int32_t sign_extend_low_bits(
	std::int32_t value,
	std::uint8_t width)
{
	const std::uint32_t sign = 1u << (width - 1u);
	const std::uint32_t mask = (1u << width) - 1u;
	const std::uint32_t encoded =
		static_cast<std::uint32_t>(value) & mask;
	return static_cast<std::int32_t>((encoded ^ sign) - sign);
}

bool object_has_stats(
	const game::WorldObject& object,
	const assets::ShipStatsTable& stats)
{
	return stats.ready && object.type < assets::kShipStatsCount;
}

std::uint8_t connected_player_count(
	const NetworkRuntime& network)
{
	std::uint8_t count = 0;
	for (std::uint8_t player = 0;
		player < network.player_count;
		++player)
	{
		if (network.connected[player])
		{
			++count;
		}
	}
	return count;
}

std::uint32_t queued_retail_bits(
	const NetworkRuntime& network,
	std::uint8_t connected_count)
{
	const std::uint32_t remote_count =
		connected_count == 0 ? 0u : connected_count - 1u;
	std::uint32_t total = 0;
	for (std::uint8_t offset = 0;
		offset < network.outbound_count;
		++offset)
	{
		const std::uint8_t index = static_cast<std::uint8_t>(
			(network.outbound_read + offset)
			% kNetworkOutboundCapacity);
		const NetworkOutboundMessage& message =
			network.outbound[index];
		const std::uint32_t bits =
			retained_retail_bit_count(message);
		switch (message.delivery)
		{
		case NetworkDelivery::broadcast_conditional:
		case NetworkDelivery::broadcast_guaranteed:
			total += bits * remote_count;
			break;
		case NetworkDelivery::directed_conditional:
		case NetworkDelivery::directed_guaranteed:
			total += bits;
			break;
		}
	}
	return total;
}

std::uint16_t object_state_bit_count(
	bool motion_present,
	std::uint8_t position_mode,
	bool orientation_present)
{
	std::uint16_t bits = 29;
	if (motion_present)
	{
		bits = static_cast<std::uint16_t>(bits + 69u);
	}
	switch (position_mode)
	{
	case 1:
		bits = static_cast<std::uint16_t>(bits + 63u);
		break;
	case 2:
		bits = static_cast<std::uint16_t>(bits + 87u);
		break;
	case 3:
		bits = static_cast<std::uint16_t>(bits + 48u);
		break;
	default:
		break;
	}
	if (orientation_present)
	{
		bits = static_cast<std::uint16_t>(bits + 30u);
	}
	return bits;
}

bool active_command_is(
	const game::WorldObject& object,
	std::int16_t command_id)
{
	return object.ai.command_count != 0
		&& object.ai.commands[0].id == command_id;
}

std::uint8_t encode_component_state(
	float value,
	float maximum)
{
	if (value < 0.0f)
	{
		return 0x7fu;
	}
	if (!(maximum > 0.0f))
	{
		return 0;
	}
	// FUN_004ba8b0 evaluates the ratio in x87 precision, stores it as a
	// float in the deferred record, and FUN_004c3330 later rounds that float
	// using the active round-to-nearest mode.
	const float normalized = static_cast<float>(
		static_cast<double>(value) * 126.0
		/ static_cast<double>(maximum));
	return static_cast<std::uint8_t>(std::min(
		retail_round(normalized),
		126));
}

std::uint8_t encode_bank_delta(
	float value,
	std::int32_t base_maximum)
{
	const std::int32_t maximum = base_maximum * 6;
	if (maximum <= 0)
	{
		return 0;
	}
	// Opcode 0x22 also stores the normalized x87 result as a float before
	// invoking FUN_004c3330.
	const float normalized = static_cast<float>(
		static_cast<double>(value) * 127.0
		/ static_cast<double>(maximum));
	return static_cast<std::uint8_t>(std::min(
		retail_round(normalized),
		127));
}

std::uint8_t encode_bank_state(
	float value,
	std::int32_t base_maximum)
{
	if (value < 0.0f)
	{
		return 0x7fu;
	}
	const std::int32_t maximum = base_maximum * 6;
	if (maximum <= 0)
	{
		return 0;
	}
	// Unlike opcodes 0x20-0x22, opcode 0x23 converts the live bank ratio
	// with MSVC's __ftol helper (FUN_004cf28c), which temporarily selects
	// truncate-toward-zero. There is no intermediate float store.
	const int encoded = static_cast<int>(
		static_cast<double>(value) * 126.0
		/ static_cast<double>(maximum));
	return static_cast<std::uint8_t>(std::min(encoded, 126));
}

void populate_bank_state(
	NetworkOutboundMessage& message,
	const game::WorldObject& target,
	std::int32_t primary_bank_maximum,
	std::int32_t structural_bank_maximum)
{
	message.delivery = NetworkDelivery::broadcast_conditional;
	for (std::uint8_t bank = 0; bank < 4; ++bank)
	{
		message.damage_bank_value[bank] =
			encode_bank_state(
				target.primary_shields[bank],
				primary_bank_maximum);
		message.damage_bank_value[bank + 4] =
			encode_bank_state(
				target.secondary_shields[bank],
				structural_bank_maximum);
		if (target.secondary_shields[bank] < 0.0f)
		{
			// Opcode 0x23 selects send class three as soon as any
			// structural bank uses the destroyed-state sentinel.
			message.delivery =
				NetworkDelivery::broadcast_guaranteed;
		}
	}
	message.damage_source_index = static_cast<std::uint16_t>(
		target.last_attacker_index & 0x01ffu);
}

bool finalize_damage_message(
	NetworkOutboundMessage& message,
	const NetworkDamageAccumulator& accumulator)
{
	const game::WorldObject* target = accumulator.target;
	if (!damage_record_current(message, accumulator))
	{
		// Retail embeds this table in the GameObject. Destroying/reusing an
		// object therefore discards its records instead of sending them for
		// the replacement occupying that live slot.
		return false;
	}
	switch (message.opcode)
	{
	case NetworkGameplayOpcode::component_damage:
		message.damage_component_value =
			static_cast<std::uint16_t>(
				retail_round(accumulator.component));
		message.delivery =
			accumulator.component < 2500.0f
				? NetworkDelivery::broadcast_conditional
				: NetworkDelivery::broadcast_guaranteed;
		if (target != nullptr)
		{
			message.damage_source_index =
				static_cast<std::uint16_t>(
					target->last_attacker_index & 0x01ffu);
		}
		break;
	case NetworkGameplayOpcode::bank_damage:
		message.delivery =
			NetworkDelivery::broadcast_conditional;
		if (target != nullptr)
		{
			message.damage_source_index =
				static_cast<std::uint16_t>(
					target->last_attacker_index & 0x01ffu);
		}
		for (std::uint8_t bank = 0; bank < 8; ++bank)
		{
			if ((message.damage_bank_mask & (1u << bank)) == 0)
			{
				continue;
			}
			message.damage_bank_value[bank] =
				encode_bank_delta(
					accumulator.bank[bank],
					bank < 4
						? accumulator.primary_bank_maximum
						: accumulator.structural_bank_maximum);
			if (accumulator.bank[bank] > 300.0f)
			{
				message.delivery =
					NetworkDelivery::broadcast_guaranteed;
			}
		}
		break;
	case NetworkGameplayOpcode::bank_state:
		if (target != nullptr)
		{
			populate_bank_state(
				message,
				*target,
				accumulator.primary_bank_maximum,
				accumulator.structural_bank_maximum);
		}
		break;
	default:
		break;
	}
	return true;
}

std::uint16_t decode_object_reference(std::uint16_t encoded)
{
	encoded = static_cast<std::uint16_t>(encoded & 0x01ffu);
	return encoded == 0x01ffu ? UINT16_MAX : encoded;
}

game::ObjectModelReference* find_network_model(
	game::WorldObject& object,
	std::uint8_t network_model_id)
{
	// The packet field is only eight bits, while construction assigns the
	// complete sequential +0xfc ID. Retail consumes the decoded byte as a
	// direct child-array index, so a truncated ID aliases the original
	// 0..255 model rather than matching a later full ID with the same low
	// byte.
	for (game::ObjectModelReference& model : object.model_references)
	{
		if (model.network_model_id == network_model_id)
		{
			return &model;
		}
	}
	return nullptr;
}

void synchronize_component_health(
	game::WorldObject& object,
	const game::ObjectModelReference& model)
{
	if (model.component_index >= 0
		&& model.component_index < object.component_count)
	{
		object.components[model.component_index].health = model.health;
	}
}

float decode_bank_value(
	std::int32_t encoded,
	std::int32_t base_maximum,
	float scale)
{
	// The receiver performs FILD/FIMUL/FMUL in x87 precision and rounds only
	// when storing the result to the live float bank.
	return static_cast<float>(
		static_cast<double>(base_maximum)
		* static_cast<double>(encoded)
		* static_cast<double>(scale));
}

void publish_received_player_bank_feedback(
	game::World& world,
	const game::WorldObject& target,
	std::uint8_t bank,
	float damage,
	bool impact_feedback_enabled)
{
	if (world.player.index
			!= static_cast<std::uint16_t>(
				&target - std::begin(world.objects))
		|| world.player.generation != target.generation)
	{
		return;
	}
	if (impact_feedback_enabled
		&& world.player_camera_disturbance < 1.0f)
	{
		world.player_camera_disturbance = std::min(
			1.0f,
			world.player_camera_disturbance
				+ damage * kReceivedImpactScale);
	}
	if (bank < 4)
	{
		world.player_schematic_hits[bank] = 1;
	}
}

bool defer_missing_component_state(
	NetworkRuntime& network,
	const game::WorldObject& target,
	std::uint16_t target_index,
	std::uint8_t component_network_id)
{
	// FUN_004ba980 is the malformed/out-of-sync model recovery used by the
	// opcode-0x20 receiver: it publishes an authoritative destroyed
	// component record even though the requested live model is absent.
	DeferredDamageMessage publication = find_or_queue_damage(
		network,
		NetworkGameplayOpcode::component_state,
		target_index,
		component_network_id,
		true);
	if (publication.message == nullptr
		|| publication.accumulator == nullptr)
	{
		return false;
	}
	NetworkOutboundMessage& response = *publication.message;
	NetworkDamageAccumulator& accumulator =
		*publication.accumulator;
	accumulator.component_network_id = component_network_id;
	accumulator.target = &target;
	accumulator.target_generation = target.generation;
	response.damage_component_value = 0x7fu;
	response.delivery = NetworkDelivery::broadcast_guaranteed;
	response.deferred_priority = 20;
	return true;
}

bool local_owns_object_impl(
	const NetworkRuntime& network,
	std::uint16_t live_object_index,
	std::uint16_t player_prefix_count,
	std::uint16_t local_player_index)
{
	// GameObject_network_is_local_owner, LANCER.EXE 0x004b5590. Player
	// prefix slots are owned directly. Ordinary later objects are striped
	// over connected players using the compact ordinal table built by
	// 0x004bbe40.
	if (live_object_index < player_prefix_count)
	{
		return live_object_index == local_player_index;
	}
	if (network.deathmatch_mode)
	{
		// FUN_004b5590 gives every non-player object to the DirectPlay host
		// in deathmatch, including free-for-all games.
		return network.role == NetworkRole::host;
	}
	// DAT_0057e04e is initialized to 399 and has no later writer in the
	// retail executable. Co-op always treats that reserved object as local.
	if (live_object_index == 399)
	{
		return true;
	}
	std::uint8_t connected_count = 0;
	std::uint8_t local_ordinal = 0;
	for (std::uint8_t player = 0;
		player < network.player_count;
		++player)
	{
		if (!network.connected[player])
		{
			continue;
		}
		if (player == network.local_player)
		{
			local_ordinal = connected_count;
		}
		++connected_count;
	}
	return connected_count == 0
		|| (live_object_index - local_ordinal) % connected_count == 0;
}

bool queue_session_script_ready(NetworkRuntime& network)
{
	if (network.authority_player >= network.player_count
		|| !network.connected[network.authority_player])
	{
		return false;
	}
	NetworkOutboundMessage message;
	message.opcode = NetworkGameplayOpcode::session_script_ready;
	message.delivery = NetworkDelivery::directed_guaranteed;
	message.source_player = network.local_player;
	// Client-side GameObjects_network_fixed_tick
	// (0x004777fe..0x0047780a) publishes the local three-bit slot through
	// the client's guaranteed owner buffer. DirectPlay routes that buffer
	// to the current session authority; host migration does not renumber
	// the gameplay player prefix.
	message.destination_player = network.authority_player;
	message.sync_index = network.local_player;
	if (!queue_message(network, message))
	{
		return false;
	}
	diagnostics::mission_log(
		"network tx opcode=0x37 session-ready player=%u authority=%u",
		static_cast<unsigned>(network.local_player),
		static_cast<unsigned>(network.authority_player));
	return true;
}

void queue_session_script_start(
	NetworkRuntime& network,
	std::uint8_t destination)
{
	NetworkOutboundMessage message;
	message.opcode = NetworkGameplayOpcode::session_script_start;
	message.delivery = NetworkDelivery::directed_guaranteed;
	message.source_player = network.local_player;
	message.destination_player = destination;
	message.one_way_latency = network.one_way_latency[destination];
	if (queue_message(network, message))
	{
		diagnostics::mission_log(
			"network tx opcode=0x38 session-start player=%u latency=%u",
			static_cast<unsigned>(destination),
			static_cast<unsigned>(message.one_way_latency));
	}
}

void queue_script_sync_ready(
	NetworkRuntime& network,
	std::uint8_t sync_index)
{
	NetworkOutboundMessage message;
	message.opcode = NetworkGameplayOpcode::script_sync_ready;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = network.local_player;
	message.destination_player = kBroadcastDestination;
	// Opcode 0x47 serializes seven bits.
	message.sync_index = static_cast<std::uint8_t>(sync_index & 0x7fu);
	if (queue_message(network, message))
	{
		diagnostics::mission_log(
			"network tx opcode=0x47 script-sync-ready index=%u player=%u",
			static_cast<unsigned>(message.sync_index),
			static_cast<unsigned>(network.local_player));
	}
}

void queue_script_sync_restart(
	NetworkRuntime& network,
	std::uint8_t sync_index)
{
	NetworkOutboundMessage message;
	message.opcode = NetworkGameplayOpcode::script_sync_restart;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = network.local_player;
	message.destination_player = kBroadcastDestination;
	message.sync_index = static_cast<std::uint8_t>(sync_index & 0x7fu);
	// MultiplayerScriptSync_command 0x00459ea3..0x00459ec9 invokes
	// FUN_004bb230 once for the host's local slot. FUN_004b6790 returns
	// zero for that local DirectPlay ID, and the host's guaranteed owner
	// buffer is then broadcast to the clients.
	message.one_way_latency = 0;
	if (queue_message(network, message))
	{
		diagnostics::mission_log(
			"network tx opcode=0x48 script-sync-restart index=%u "
			"latency=%u",
			static_cast<unsigned>(message.sync_index),
			static_cast<unsigned>(message.one_way_latency));
	}
}

void discover_spawn_objects(Runtime& runtime, game::World& world)
{
	NetworkRuntime& network = runtime.network;
	deathmatch_scenarios_select(runtime);
	if (network.spawn_objects_initialized)
	{
		return;
	}
	network.spawn_count = 0;
	for (std::uint16_t mission_index = 0;
		mission_index < runtime.object_count;
		++mission_index)
	{
		if (runtime.objects[mission_index].type != 993)
		{
			continue;
		}
		const game::WorldObject* marker =
			runtime_resolve_object(runtime, mission_index, world);
		if (marker == nullptr
			|| network.spawn_count >= std::size(network.spawn_objects))
		{
			continue;
		}
		network.spawn_objects[network.spawn_count++] =
			static_cast<std::uint16_t>(
				marker - std::begin(world.objects));
	}
	network.spawn_objects_initialized = true;
	diagnostics::mission_log(
		"network spawn markers discovered count=%u",
		static_cast<unsigned>(network.spawn_count));
}

bool spawn_is_free(
	const NetworkRuntime& network,
	const game::World& world,
	const game::WorldObject& respawning,
	std::uint16_t spawn)
{
	if (spawn >= std::size(world.objects)
		|| !world.objects[spawn].active)
	{
		return false;
	}
	const game::WorldObject& marker = world.objects[spawn];
	for (std::uint8_t player = 0;
		player < network.player_count;
		++player)
	{
		if (!network.connected[player]
			|| player >= std::size(world.objects)
			|| !world.objects[player].active)
		{
			continue;
		}
		const game::WorldObject& existing = world.objects[player];
		const float clearance =
			1.2f * (existing.radius + respawning.radius);
		if (glm::length(marker.position - existing.position)
			< clearance)
		{
			return false;
		}
	}
	return true;
}
}

void network_runtime_reset(NetworkRuntime& network)
{
	network = {};
	network.player_count = 1;
	network.connected[0] = true;
	std::fill(
		std::begin(network.object_team),
		std::end(network.object_team),
		-1);
	network.deathmatch_scenario = -1;
	network.first_random_spawn = true;
	network.gameplay_ready = true;
	network.object_state_interval = 10;
}

bool network_local_owns_object(
	const NetworkRuntime& network,
	std::uint16_t live_object_index,
	std::uint16_t player_prefix_count,
	std::uint16_t local_player_index)
{
	return network.role == NetworkRole::offline
		|| local_owns_object_impl(
			network,
			live_object_index,
			player_prefix_count,
			local_player_index);
}

bool network_defer_bank_damage(
	NetworkRuntime& network,
	const game::WorldObject& target,
	const assets::ShipStatsTable& stats,
	std::uint16_t target_index,
	std::uint16_t player_prefix_count,
	std::uint16_t local_player_index,
	std::uint8_t bank,
	float damage)
{
	if (network.role == NetworkRole::offline
		|| target_index >= game::kMaxGameObjects
		|| target.type >= assets::kShipStatsCount
		|| bank >= 8)
	{
		return false;
	}

	const bool authoritative = local_owns_object_impl(
		network,
		target_index,
		player_prefix_count,
		local_player_index);
	const NetworkGameplayOpcode opcode =
		authoritative
			? NetworkGameplayOpcode::bank_state
			: NetworkGameplayOpcode::bank_damage;
	DeferredDamageMessage publication = find_or_queue_damage(
		network, opcode, target_index, 0, false);
	if (publication.message == nullptr
		|| publication.accumulator == nullptr)
	{
		return false;
	}

	NetworkOutboundMessage& message = *publication.message;
	NetworkDamageAccumulator& accumulator =
		*publication.accumulator;
	accumulator.target = &target;
	accumulator.target_generation = target.generation;
	const assets::ObjectTypeStats& type =
		stats.records[target.type].object;
	accumulator.primary_bank_maximum =
		type.primary_bank_max;
	accumulator.structural_bank_maximum =
		type.structural_bank_max;
	if (authoritative)
	{
		// FUN_004baa80 retains only one opcode-0x23 record; its eventual
		// serializer samples the live latest-attacker field and all eight
		// banks. finalize_damage_message repeats that sampling on dequeue.
	}
	else
	{
		// FUN_004ba9f0 accumulates by bank in floating point and sets the
		// corresponding mask bit. The 7-bit value is rounded only from the
		// complete sum, never by adding separately rounded hits.
		accumulator.bank[bank] += damage;
		message.damage_bank_mask = static_cast<std::uint8_t>(
			message.damage_bank_mask | (1u << bank));
	}
	finalize_damage_message(message, accumulator);

	if (publication.inserted)
	{
		diagnostics::mission_log(
			"network defer opcode=0x%02x bank object=%u owner=%u",
			static_cast<unsigned>(opcode),
			static_cast<unsigned>(target_index),
			authoritative ? 1u : 0u);
	}
	return true;
}

bool network_defer_component_damage(
	NetworkRuntime& network,
	const game::WorldObject& target,
	const game::ObjectModelReference& component,
	std::uint32_t component_network_id,
	std::uint16_t target_index,
	std::uint16_t player_prefix_count,
	std::uint16_t local_player_index,
	float damage)
{
	if (network.role == NetworkRole::offline
		|| target_index >= game::kMaxGameObjects)
	{
		return false;
	}

	const bool authoritative = local_owns_object_impl(
		network,
		target_index,
		player_prefix_count,
		local_player_index);
	const bool destroyed = component.health < 0.0f;
	const NetworkGameplayOpcode opcode =
		authoritative
			? NetworkGameplayOpcode::component_state
			: NetworkGameplayOpcode::component_damage;
	DeferredDamageMessage publication = find_or_queue_damage(
		network,
		opcode,
		target_index,
		component_network_id,
		destroyed);
	if (publication.message == nullptr
		|| publication.accumulator == nullptr)
	{
		return false;
	}

	NetworkOutboundMessage& message = *publication.message;
	NetworkDamageAccumulator& accumulator =
		*publication.accumulator;
	accumulator.component_network_id = component_network_id;
	accumulator.target = &target;
	accumulator.target_generation = target.generation;
	if (authoritative)
	{
		// FUN_004ba8b0 refreshes the authoritative state in place. A
		// negative state is the wire sentinel 0x7f and promotes the retained
		// record to priority 20 even when it was originally created at 10.
		message.damage_component_value =
			encode_component_state(
				component.health,
				component.maximum_health);
		message.delivery = destroyed
			? NetworkDelivery::broadcast_guaranteed
			: NetworkDelivery::broadcast_conditional;
		if (destroyed)
		{
			message.deferred_priority = 20;
		}
	}
	else
	{
		// Opcode 0x20 is keyed by the model's live-array ID. Its last
		// attacker is sampled from the owning GameObject on every update.
		accumulator.component += damage;
	}
	finalize_damage_message(message, accumulator);

	if (publication.inserted)
	{
		diagnostics::mission_log(
			"network defer opcode=0x%02x component object=%u "
			"source=%u owner=%u",
			static_cast<unsigned>(opcode),
			static_cast<unsigned>(target_index),
			static_cast<unsigned>(component_network_id),
			authoritative ? 1u : 0u);
	}
	return true;
}

bool network_receive_damage(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const NetworkOutboundMessage& message,
	bool impact_feedback_enabled)
{
	NetworkRuntime& network = runtime.network;
	if (network.role == NetworkRole::offline
		|| !is_damage_opcode(message.opcode))
	{
		return false;
	}
	const std::uint16_t target_index =
		static_cast<std::uint16_t>(message.object_index & 0x01ffu);
	if (target_index >= game::kMaxGameObjects
		|| !world.objects[target_index].active)
	{
		return false;
	}
	game::WorldObject& target = world.objects[target_index];

	switch (message.opcode)
	{
	case NetworkGameplayOpcode::component_damage:
	{
		// Opcode 0x20, LANCER.EXE 0x004b8369..0x004b84d0:
		// object(9), latest attacker(9), live model ID(8), damage(16).
		target.last_attacker_index =
			decode_object_reference(message.damage_source_index);
		game::ObjectModelReference* component =
			find_network_model(
				target, message.damage_component_id);
		if (component == nullptr)
		{
			(void)defer_missing_component_state(
				network,
				target,
				target_index,
				message.damage_component_id);
			diagnostics::mission_log(
				"network rx opcode=0x20 component-damage object=%u "
				"model=%u missing=1",
				static_cast<unsigned>(target_index),
				static_cast<unsigned>(
					message.damage_component_id));
			return true;
		}

		const bool attacker_is_non_player =
			target.last_attacker_index != UINT16_MAX
			&& target.last_attacker_index
				>= runtime.player_prefix_count;
		bool protected_health =
			target.protection_state == 2
			|| (target.protection_state == 1
				&& attacker_is_non_player);
		if (component->component_index >= 0
			&& component->component_index
				< target.component_count)
		{
			const std::uint16_t component_protection =
				target.components[
					component->component_index].protection_state;
			protected_health =
				protected_health
				|| component_protection == 2
				|| (component_protection == 1
					&& attacker_is_non_player);
		}
		const float damage = static_cast<float>(
			message.damage_component_value);
		const float updated_health = component->health - damage;
		if (updated_health >= 0.0f || !protected_health)
		{
			component->health = updated_health;
			synchronize_component_health(target, *component);
		}

		const bool authoritative = local_owns_object_impl(
			network,
			target_index,
			runtime.player_prefix_count,
			world.player.index);
		if (authoritative)
		{
			if (component->health < 0.0f)
			{
				target.component_destruction_pending = true;
			}
			(void)network_defer_component_damage(
				network,
				target,
				*component,
				component->network_model_id,
				target_index,
				runtime.player_prefix_count,
				world.player.index,
				damage);
		}
		diagnostics::mission_log(
			"network rx opcode=0x20 component-damage object=%u "
			"attacker=%u model=%u damage=%.1f health=%.1f owner=%u",
			static_cast<unsigned>(target_index),
			static_cast<unsigned>(target.last_attacker_index),
			static_cast<unsigned>(message.damage_component_id),
			damage,
			component->health,
			authoritative ? 1u : 0u);
		return true;
	}
	case NetworkGameplayOpcode::component_state:
	{
		// Opcode 0x21, LANCER.EXE 0x004b84d5..0x004b858d:
		// object(9), live model ID(8), normalized state(7).
		game::ObjectModelReference* component =
			find_network_model(
				target, message.damage_component_id);
		if (component == nullptr)
		{
			return true;
		}
		const std::uint8_t encoded = static_cast<std::uint8_t>(
			message.damage_component_value & 0x7fu);
		if (encoded == 0x7fu)
		{
			component->health = -1.0f;
			target.component_destruction_pending = true;
		}
		else
		{
			component->health = static_cast<float>(
				static_cast<double>(component->maximum_health)
				* static_cast<double>(encoded)
				* static_cast<double>(
					kComponentStateDecodeScale));
		}
		synchronize_component_health(target, *component);
		diagnostics::mission_log(
			"network rx opcode=0x21 component-state object=%u "
			"model=%u state=%u health=%.1f",
			static_cast<unsigned>(target_index),
			static_cast<unsigned>(message.damage_component_id),
			static_cast<unsigned>(encoded),
			component->health);
		return true;
	}
	case NetworkGameplayOpcode::bank_damage:
	{
		// Opcode 0x22, LANCER.EXE 0x004b86d2..0x004b88bd:
		// object(9), latest attacker(9), bank mask(8), then one seven-bit
		// delta for every selected bank.
		const bool authoritative = local_owns_object_impl(
			network,
			target_index,
			runtime.player_prefix_count,
			world.player.index);
		target.last_attacker_index =
			decode_object_reference(message.damage_source_index);
		if (target.type >= assets::kShipStatsCount)
		{
			return true;
		}
		const assets::ObjectTypeStats& type =
			stats.records[target.type].object;
		const bool attacker_is_non_player =
			target.last_attacker_index != UINT16_MAX
			&& target.last_attacker_index
				>= runtime.player_prefix_count;
		for (std::uint8_t wire_bank = 0;
			wire_bank < 8;
			++wire_bank)
		{
			if ((message.damage_bank_mask
					& (1u << wire_bank)) == 0)
			{
				continue;
			}
			const std::uint8_t encoded =
				static_cast<std::uint8_t>(
					message.damage_bank_value[wire_bank]
					& 0x7fu);
			const std::uint8_t bank =
				static_cast<std::uint8_t>(wire_bank & 3u);
			const float damage = decode_bank_value(
				encoded,
				wire_bank < 4
					? type.primary_bank_max
					: type.structural_bank_max,
				kBankDeltaDecodeScale);
			publish_received_player_bank_feedback(
				world,
				target,
				bank,
				damage,
				impact_feedback_enabled);
			if (wire_bank < 4)
			{
				// The authoritative receiver applies primary deltas
				// literally, including already-negative and protected
				// banks. Those gates were evaluated by the sender.
				target.primary_shields[bank] -= damage;
				continue;
			}

			const float updated =
				target.secondary_shields[bank] - damage;
			const bool protected_crossing =
				updated < 0.0f
				&& (target.protection_state == 2
					|| (target.protection_state == 1
						&& attacker_is_non_player));
			if (target.protection_state == 4
				|| protected_crossing)
			{
				continue;
			}
			target.secondary_shields[bank] = updated;
			if (updated < 0.0f && authoritative)
			{
				(void)ai::schedule_death_command(
					target,
					world,
					runtime,
					1,
					false);
			}
		}
		if (authoritative)
		{
			// FUN_004baa80 retains one opcode-0x23 record and samples all
			// eight live banks when it is eventually serialized.
			(void)network_defer_bank_damage(
				network,
				target,
				stats,
				target_index,
				runtime.player_prefix_count,
				world.player.index,
				0,
				0.0f);
		}
		game::world_update_shield_ratios(target, stats);
		diagnostics::mission_log(
			"network rx opcode=0x22 bank-damage object=%u "
			"attacker=%u mask=0x%02x owner=%u",
			static_cast<unsigned>(target_index),
			static_cast<unsigned>(target.last_attacker_index),
			static_cast<unsigned>(message.damage_bank_mask),
			authoritative ? 1u : 0u);
		return true;
	}
	case NetworkGameplayOpcode::bank_state:
	{
		// Opcode 0x23, LANCER.EXE 0x004b8592..0x004b86cd:
		// object(9), latest attacker(9), then four primary/structural
		// seven-bit state pairs. Sentinel 127 is converted to signed -1
		// before the normal maximum-bank scaling.
		target.last_attacker_index =
			decode_object_reference(message.damage_source_index);
		if (target.type >= assets::kShipStatsCount)
		{
			return true;
		}
		const assets::ObjectTypeStats& type =
			stats.records[target.type].object;
		for (std::uint8_t bank = 0; bank < 4; ++bank)
		{
			const std::uint8_t primary_encoded =
				static_cast<std::uint8_t>(
					message.damage_bank_value[bank]
					& 0x7fu);
			const std::uint8_t structural_encoded =
				static_cast<std::uint8_t>(
					message.damage_bank_value[bank + 4]
					& 0x7fu);
			target.primary_shields[bank] = decode_bank_value(
				primary_encoded == 0x7fu
					? -1
					: primary_encoded,
				type.primary_bank_max,
				kBankStateDecodeScale);
			target.secondary_shields[bank] = decode_bank_value(
				structural_encoded == 0x7fu
					? -1
					: structural_encoded,
				type.structural_bank_max,
				kBankStateDecodeScale);
			if (target.secondary_shields[bank] < 0.0f)
			{
				(void)ai::schedule_death_command(
					target,
					world,
					runtime,
					1,
					false);
			}
		}
		for (std::uint8_t bank = 0; bank < 4; ++bank)
		{
			if (target.secondary_shields[bank] < 0.0f)
			{
				// Retail performs this second post-loop death selection in
				// addition to the per-bank call above. The command owner
				// naturally makes subsequent calls no-ops once death has
				// been selected.
				(void)ai::schedule_death_command(
					target,
					world,
					runtime,
					1,
					false);
				break;
			}
		}
		game::world_update_shield_ratios(target, stats);
		diagnostics::mission_log(
			"network rx opcode=0x23 bank-state object=%u attacker=%u",
			static_cast<unsigned>(target_index),
			static_cast<unsigned>(target.last_attacker_index));
		return true;
	}
	default:
		break;
	}
	return false;
}

void network_publish_ai_sequence_sync(
	NetworkRuntime& network,
	std::uint16_t object_index,
	std::uint8_t sync_index)
{
	if (network.role == NetworkRole::offline)
	{
		return;
	}
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::ai_sequence_sync;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = network.local_player;
	message.destination_player = kBroadcastDestination;
	message.object_index = object_index;
	message.sync_index = sync_index;
	if (queue_message(network, message))
	{
		diagnostics::mission_log(
			"network tx ai-sequence-sync object=%u index=%u player=%u",
			static_cast<unsigned>(object_index),
			static_cast<unsigned>(sync_index),
			static_cast<unsigned>(network.local_player));
	}
}

bool network_receive_ai_sequence_sync(
	NetworkRuntime& network,
	game::World& world,
	std::uint16_t object_index,
	std::uint8_t sync_index,
	std::uint8_t source_player)
{
	if (object_index >= game::kMaxGameObjects
		|| sync_index >= 16
		|| source_player >= network.player_count
		|| !network.connected[source_player])
	{
		return false;
	}
	game::WorldObject& object = world.objects[object_index];
	if (!object.active)
	{
		return false;
	}
	object.ai_sequence_sync[sync_index] |=
		static_cast<std::uint8_t>(1u << source_player);
	return true;
}

void network_publish_ai_deferred_command(
	NetworkRuntime& network,
	std::uint16_t object_index,
	const ai::Command& command,
	std::uint32_t delay_ticks,
	std::uint8_t publication_seed)
{
	if (network.role == NetworkRole::offline)
	{
		return;
	}
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::ai_deferred_command;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = network.local_player;
	message.destination_player = kBroadcastDestination;
	message.object_index = object_index;
	message.command_id = command.id;
	message.command_selector = command.selector;
	message.command_target = command.target;
	message.command_target_component = command.target_component;
	message.command_sequence = command.sequence;
	std::copy(
		std::begin(command.state),
		std::end(command.state),
		std::begin(message.command_state));
	message.command_target_kind =
		static_cast<std::uint8_t>(command.target_kind);
	message.delay_ticks = delay_ticks;
	message.publication_seed = publication_seed;
	if (queue_message(network, message))
	{
		diagnostics::mission_log(
			"network tx ai-deferred object=%u command=%d delay=%u seed=%u",
			static_cast<unsigned>(object_index),
			static_cast<int>(command.id),
			static_cast<unsigned>(delay_ticks),
			static_cast<unsigned>(publication_seed));
	}
}

void network_publish_dark_reign_tower_state(
	NetworkRuntime& network,
	std::uint8_t state)
{
	if (network.role == NetworkRole::offline)
	{
		return;
	}
	// Deathmatch_send_dark_reign_state, LANCER.EXE 0x004bafc0, selects
	// gameplay opcode 0x30 and appends exactly three state bits.
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode =
		NetworkGameplayOpcode::dark_reign_tower_state;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = network.local_player;
	message.destination_player = kBroadcastDestination;
	message.tower_state = state & 7u;
	if (queue_message(network, message))
	{
		diagnostics::mission_log(
			"network tx dark-reign-state state=%u",
			static_cast<unsigned>(message.tower_state));
	}
}

bool network_receive_ai_deferred_command(
	game::World& world,
	const NetworkOutboundMessage& message,
	std::uint32_t current_tick)
{
	if (message.kind != NetworkOutboundKind::ai_deferred_command
		|| message.object_index >= game::kMaxGameObjects)
	{
		return false;
	}
	game::WorldObject& object = world.objects[message.object_index];
	if (!object.active)
	{
		return false;
	}
	ai::Command command;
	command.id = message.command_id;
	command.selector = message.command_selector;
	command.target = message.command_target;
	command.target_component = message.command_target_component;
	command.sequence = message.command_sequence;
	std::copy(
		std::begin(message.command_state),
		std::end(message.command_state),
		std::begin(command.state));
	command.target_kind =
		static_cast<ai::TargetKind>(message.command_target_kind);
	return ai::command_defer(
		object,
		command,
		current_tick,
		message.delay_ticks,
		message.publication_seed);
}

bool network_is_deathmatch_mission(std::uint16_t mission)
{
	// Deathmatch_is_mission (LANCER.EXE 0x004b2d30) scans these six
	// 0x40-byte records and returns true on the first mission-number match.
	constexpr std::uint16_t missions[6]{85, 82, 81, 83, 84, 87};
	return std::find(
			std::begin(missions), std::end(missions), mission)
		!= std::end(missions);
}

void network_set_gameplay_ready(
	NetworkRuntime& network,
	bool ready)
{
	network.gameplay_ready = ready;
}

void network_runtime_shutdown(NetworkRuntime& network)
{
	if (network.role != NetworkRole::offline
		|| network.session_sync_active
		|| network.outbound_count != 0)
	{
		diagnostics::mission_log(
			"network shutdown role=%s queued=%u session_sync=%u",
			role_name(network.role),
			static_cast<unsigned>(network.outbound_count),
			network.session_sync_active ? 1u : 0u);
	}
	network_runtime_reset(network);
}

void network_runtime_configure(
	NetworkRuntime& network,
	NetworkRole role,
	std::uint8_t local_player,
	std::uint8_t player_count,
	const bool connected[kNetworkPlayerCapacity],
	const std::uint8_t one_way_latency[kNetworkPlayerCapacity])
{
	const std::uint32_t tick = network.network_tick;
	network_runtime_reset(network);
	network.network_tick = tick;
	network.role = role;
	network.player_count = std::clamp<std::uint8_t>(
		player_count, 1, kNetworkPlayerCapacity);
	network.local_player = std::min<std::uint8_t>(
		local_player,
		static_cast<std::uint8_t>(network.player_count - 1));
	std::uint8_t connected_mask = 0;
	for (std::uint8_t player = 0;
		player < kNetworkPlayerCapacity;
		++player)
	{
		network.connected[player] =
			player < network.player_count && connected[player];
		network.one_way_latency[player] =
			one_way_latency[player];
		if (network.connected[player])
		{
			connected_mask = static_cast<std::uint8_t>(
				connected_mask | (1u << player));
		}
	}
	network.connected[network.local_player] = true;
	connected_mask = static_cast<std::uint8_t>(
		connected_mask | (1u << network.local_player));
	diagnostics::mission_log(
		"network configure role=%s local=%u players=%u connected=0x%02x",
		role_name(network.role),
		static_cast<unsigned>(network.local_player),
		static_cast<unsigned>(network.player_count),
		static_cast<unsigned>(connected_mask));
}

bool network_update_authority(
	NetworkRuntime& network,
	NetworkRole role,
	std::uint8_t authority_player)
{
	if (role == NetworkRole::offline
		|| authority_player >= network.player_count
		|| !network.connected[authority_player]
		|| ((role == NetworkRole::host)
			!= (authority_player == network.local_player)))
	{
		return false;
	}
	const NetworkRole previous_role = network.role;
	const std::uint8_t previous_authority =
		network.authority_player;
	if (previous_role == role
		&& previous_authority == authority_player)
	{
		return true;
	}
	network.role = role;
	network.authority_player = authority_player;
	if (network.session_sync_active)
	{
		// Discard only 0x37/0x38 records routed under the old authority.
		// Script-sync 0x47/0x48 state and masks belong to a separate
		// protocol and deliberately survive migration.
		discard_session_handshake_messages(network);
		std::fill(
			std::begin(network.session_ready),
			std::end(network.session_ready),
			false);
		network.session_phase_active = false;
		network.session_start_received = false;
		network.session_start_latency = 0;
		network.session_phase_tick = network.network_tick;
		if (role == NetworkRole::host)
		{
			network.session_ready[network.local_player] = true;
		}
	}
	diagnostics::mission_log(
		"network authority role=%s player=%u previous-role=%s "
		"previous-player=%u handshake=%u",
		role_name(role),
		static_cast<unsigned>(authority_player),
		role_name(previous_role),
		static_cast<unsigned>(previous_authority),
		network.session_sync_active ? 1u : 0u);
	return true;
}

void network_runtime_configure_teams(
	NetworkRuntime& network,
	bool deathmatch_mode,
	bool team_mode,
	const std::int32_t team[kNetworkPlayerCapacity],
	std::int8_t configured_team_count)
{
	network.deathmatch_mode = deathmatch_mode;
	network.team_mode = team_mode;
	network.configured_team_count = std::clamp<std::int8_t>(
		configured_team_count, -1, 4);
	std::fill(
		std::begin(network.object_team),
		std::end(network.object_team),
		-1);
	std::uint8_t assigned_mask = 0;
	for (std::uint8_t player = 0;
		player < network.player_count;
		++player)
	{
		if (!network.connected[player])
		{
			continue;
		}
		network.object_team[player] = team[player];
		assigned_mask = static_cast<std::uint8_t>(
			assigned_mask | (1u << player));
	}
	diagnostics::mission_log(
		"network teams deathmatch=%u mode=%u assigned=0x%02x "
		"configured=%d",
		deathmatch_mode ? 1u : 0u,
		team_mode ? 1u : 0u,
		static_cast<unsigned>(assigned_mask),
		static_cast<int>(network.configured_team_count));
}

void network_set_player_metadata(
	NetworkRuntime& network,
	std::uint8_t player,
	const char* name,
	std::uint32_t latency)
{
	if (player >= kNetworkPlayerCapacity)
	{
		return;
	}
	std::snprintf(
		network.player_name[player],
		sizeof(network.player_name[player]),
		"%s",
		name == nullptr ? "" : name);
	network.player_latency[player] =
		player == network.local_player ? 0u : latency;
}

bool network_runtime_pop_outbound(
	NetworkRuntime& network,
	NetworkOutboundMessage& message)
{
	while (network.outbound_count != 0)
	{
		const std::uint8_t index = network.outbound_read;
		const bool valid = finalize_damage_message(
			network.outbound[index],
			network.outbound_damage[index]);
		if (valid)
		{
			message = network.outbound[index];
		}
		network.outbound[index] = {};
		network.outbound_damage[index] = {};
		network.outbound_read = static_cast<std::uint8_t>(
			(network.outbound_read + 1)
			% kNetworkOutboundCapacity);
		--network.outbound_count;
		if (network.outbound_count == 0)
		{
			network.outbound_read = 0;
			network.outbound_overflow_logged = false;
		}
		if (valid)
		{
			return true;
		}
	}
	return false;
}

bool network_queue_gameplay_message(
	NetworkRuntime& network,
	const NetworkOutboundMessage& message)
{
	if (message.kind != NetworkOutboundKind::gameplay)
	{
		return false;
	}
	return queue_message(network, message);
}

bool network_publish_pause_state(
	NetworkRuntime& network,
	bool active,
	NetworkPauseReason reason)
{
	const std::uint8_t raw_reason =
		static_cast<std::uint8_t>(reason);
	if (network.role == NetworkRole::offline
		|| network.local_player >= kNetworkPlayerCapacity
		|| raw_reason >= 16u)
	{
		return false;
	}

	// Pause_send, LANCER.EXE 0x004baae0. FUN_004b9920 receives flags three,
	// selecting the guaranteed broadcast buffer, before the serializer writes
	// the one-bit desired state and complete four-bit reason.
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode = NetworkGameplayOpcode::pause_state;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = network.local_player;
	message.destination_player = kBroadcastDestination;
	message.pause_active = active;
	message.pause_reason = reason;
	if (!queue_message(network, message))
	{
		return false;
	}
	diagnostics::mission_log(
		"network tx opcode=0x27 pause=%u reason=%u player=%u",
		active ? 1u : 0u,
		static_cast<unsigned>(raw_reason),
		static_cast<unsigned>(network.local_player));
	return true;
}

bool network_publish_cloak_state(
	NetworkRuntime& network,
	bool active)
{
	if (network.role == NetworkRole::offline
		|| network.local_player >= kNetworkPlayerCapacity)
	{
		return false;
	}

	// Cloak_send, LANCER.EXE 0x004bab60, selects send class three and
	// broadcasts opcode 0x44 followed by the requested state bit.
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode = NetworkGameplayOpcode::cloak_state;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = network.local_player;
	message.destination_player = kBroadcastDestination;
	message.scenario_flag = active;
	if (!queue_message(network, message))
	{
		return false;
	}
	diagnostics::mission_log(
		"network tx opcode=0x44 cloak=%u player=%u",
		active ? 1u : 0u,
		static_cast<unsigned>(network.local_player));
	return true;
}

void network_publish_player_stats(
	NetworkRuntime& network,
	std::uint16_t local_world_index)
{
	if (network.role == NetworkRole::offline
		|| local_world_index >= kNetworkPlayerCapacity)
	{
		return;
	}
	// Deathmatch_send_score, LANCER.EXE 0x004b9f20, sends opcode 0x3b
	// from the local live-object slot at 0x005883fa, followed by the
	// complete signed 32-bit local kill and death totals.
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode = NetworkGameplayOpcode::player_stats;
	// Deathmatch_send_score passes send class two, the conditional /
	// non-guaranteed broadcast used by the retail transport.
	message.delivery = NetworkDelivery::broadcast_conditional;
	message.source_player =
		static_cast<std::uint8_t>(local_world_index);
	message.destination_player = kBroadcastDestination;
	message.player_kills =
		network.player_kills[local_world_index];
	message.player_deaths =
		network.player_deaths[local_world_index];
	if (queue_message(network, message))
	{
		diagnostics::mission_log(
			"network tx opcode=0x3b player=%u kills=%d deaths=%d",
			static_cast<unsigned>(local_world_index),
			message.player_kills,
			message.player_deaths);
	}
}

bool network_receive_player_stats(
	Runtime& runtime,
	game::World& world,
	std::uint8_t source_player,
	std::int32_t kills,
	std::int32_t deaths)
{
	NetworkRuntime& network = runtime.network;
	if (source_player >= network.player_count
		|| source_player >= kNetworkPlayerCapacity
		|| !network.connected[source_player])
	{
		return false;
	}
	// Opcode 0x3b's receiver at 0x004b7679..0x004b76be converts both
	// absolute fields to wrapping deltas before invoking the ordinary
	// kill/death accumulators.
	const std::int32_t kill_delta = std::bit_cast<std::int32_t>(
		std::bit_cast<std::uint32_t>(kills)
			- std::bit_cast<std::uint32_t>(
				network.player_kills[source_player]));
	const std::int32_t death_delta = std::bit_cast<std::int32_t>(
		std::bit_cast<std::uint32_t>(deaths)
			- std::bit_cast<std::uint32_t>(
				network.player_deaths[source_player]));
	(void)runtime_add_player_score(
		runtime, world, source_player, kill_delta);
	(void)runtime_add_player_death(
		runtime, world, source_player, death_delta);
	diagnostics::mission_log(
		"network rx opcode=0x3b player=%u kills=%d deaths=%d",
		static_cast<unsigned>(source_player),
		kills,
		deaths);
	return true;
}

bool network_publish_chat(
	NetworkRuntime& network,
	std::int16_t destination_player,
	const char (&text)[kNetworkChatBytes])
{
	if (network.role == NetworkRole::offline
		|| (destination_player != -1
			&& (destination_player < 0
				|| destination_player >= network.player_count
				|| !network.connected[destination_player])))
	{
		return false;
	}
	// Deathmatch_send_chat, LANCER.EXE 0x004b9f60, uses DirectPlay's
	// guaranteed chat path and maps -1 to its broadcast DPID.
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::chat;
	message.delivery =
		destination_player == -1
			? NetworkDelivery::broadcast_guaranteed
			: NetworkDelivery::directed_guaranteed;
	message.source_player = network.local_player;
	message.destination_player =
		destination_player == -1
			? kBroadcastDestination
			: static_cast<std::uint8_t>(destination_player);
	std::memcpy(message.chat_text, text, sizeof(message.chat_text));
	message.chat_text[sizeof(message.chat_text) - 1] = '\0';
	if (!queue_message(network, message))
	{
		return false;
	}
	diagnostics::mission_log(
		"network tx chat player=%u destination=%d",
		static_cast<unsigned>(network.local_player),
		static_cast<int>(destination_player));
	return true;
}

bool network_publish_player_comms_command(
	NetworkRuntime& network,
	NetworkGameplayOpcode opcode,
	std::int16_t destination_player,
	std::uint16_t target_object)
{
	if (network.role == NetworkRole::offline
		|| (opcode != NetworkGameplayOpcode::player_attack_request
			&& opcode
				!= NetworkGameplayOpcode::player_backoff_request
			&& opcode
				!= NetworkGameplayOpcode::player_help_request)
		|| (destination_player != -1
			&& (destination_player < 0
				|| destination_player >= network.player_count
				|| destination_player == network.local_player
				|| !network.connected[destination_player])))
	{
		return false;
	}
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode = opcode;
	message.delivery =
		destination_player == -1
			? NetworkDelivery::broadcast_conditional
			: NetworkDelivery::directed_conditional;
	message.source_player = network.local_player;
	message.destination_player =
		destination_player == -1
			? kBroadcastDestination
			: static_cast<std::uint8_t>(destination_player);
	if (opcode != NetworkGameplayOpcode::player_help_request)
	{
		message.object_index =
			static_cast<std::uint16_t>(target_object & 0x01ffu);
	}
	if (!queue_message(network, message))
	{
		return false;
	}
	diagnostics::mission_log(
		"network tx player-comms opcode=0x%02x player=%u "
		"destination=%d target=%d",
		static_cast<unsigned>(opcode),
		static_cast<unsigned>(network.local_player),
		static_cast<int>(destination_player),
		target_object == UINT16_MAX
			? -1
			: static_cast<int>(target_object));
	return true;
}

bool network_publish_landing(NetworkRuntime& network)
{
	if (network.role == NetworkRole::offline)
	{
		return false;
	}
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode = NetworkGameplayOpcode::landing;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = network.local_player;
	message.destination_player = kBroadcastDestination;
	return queue_message(network, message);
}

bool network_receive_player_comms_command(
	Runtime& runtime,
	game::World& world,
	const NetworkOutboundMessage& message)
{
	if (message.kind != NetworkOutboundKind::gameplay
		|| (message.opcode
				!= NetworkGameplayOpcode::player_attack_request
			&& message.opcode
				!= NetworkGameplayOpcode::player_backoff_request
			&& message.opcode
				!= NetworkGameplayOpcode::player_help_request)
		|| (message.delivery
				!= NetworkDelivery::broadcast_conditional
			&& message.delivery
				!= NetworkDelivery::directed_conditional)
		|| message.source_player >= runtime.network.player_count
		|| !runtime.network.connected[message.source_player])
	{
		return false;
	}
	const std::uint16_t target =
		message.opcode == NetworkGameplayOpcode::player_help_request
			// Opcode 0x41 has no target field and does not write
			// DAT_005dcd0c (0x004b7d18..0x004b7d3f). Normally the
			// response service has already cleared it to zero; an
			// overlapping prompt deliberately retains the prior request.
			? runtime.player_comms.remote_command_target
			: decode_object_reference(message.object_index);
	const std::uint8_t command =
		message.opcode == NetworkGameplayOpcode::player_attack_request
			? 1
			: message.opcode
					== NetworkGameplayOpcode::player_backoff_request
				? 2
				: 3;
	return player_comms_receive_network_command(
		runtime,
		world,
		message.source_player,
		command,
		target);
}

bool network_publish_player_target_reference(
	NetworkRuntime& network,
	std::uint16_t target_object,
	std::int16_t target_component)
{
	if (network.role == NetworkRole::offline)
	{
		return false;
	}
	const std::uint16_t encoded_object =
		static_cast<std::uint16_t>(target_object & 0x01ffu);
	const std::uint16_t encoded_component =
		static_cast<std::uint16_t>(target_component) & 0x007fu;
	const std::uint16_t canonical_object =
		encoded_object == 0x01ffu
			? UINT16_MAX
			: encoded_object;
	const std::int16_t canonical_component =
		encoded_component == 0x007fu
			? -1
			: static_cast<std::int16_t>(encoded_component);

	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode =
		NetworkGameplayOpcode::player_target_reference;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = network.local_player;
	message.destination_player = kBroadcastDestination;
	message.object_index = encoded_object;
	message.command_target_component = canonical_component;
	if (!queue_message(network, message))
	{
		return false;
	}
	network.published_target_object = canonical_object;
	network.published_target_component = canonical_component;
	diagnostics::mission_log(
		"network tx opcode=0x4d target=%d component=%d player=%u",
		canonical_object == UINT16_MAX
			? -1
			: static_cast<int>(canonical_object),
		static_cast<int>(canonical_component),
		static_cast<unsigned>(network.local_player));
	return true;
}

bool network_receive_player_target_reference(
	NetworkRuntime& network,
	game::World& world,
	const NetworkOutboundMessage& message)
{
	if (message.kind != NetworkOutboundKind::gameplay
		|| message.opcode
			!= NetworkGameplayOpcode::player_target_reference
		|| message.delivery
			!= NetworkDelivery::broadcast_guaranteed
		|| message.source_player >= network.player_count
		|| !network.connected[message.source_player]
		|| message.object_index > 0x01ffu
		|| message.command_target_component < -1
		|| message.command_target_component > 0x7e)
	{
		return false;
	}
	game::WorldObject& player =
		world.objects[message.source_player];
	ai::Command* control = nullptr;
	for (std::uint8_t index = 0;
		index < player.ai.command_count;
		++index)
	{
		if (player.ai.commands[index].id == 101)
		{
			control = &player.ai.commands[index];
			break;
		}
	}
	// The opcode-0x4d receiver resolves the authenticated source player's
	// AI_MultiplayerControl command through FUN_004028b0 and silently
	// ignores a valid packet when that retained command is absent.
	if (control == nullptr)
	{
		return true;
	}
	const std::uint16_t target =
		decode_object_reference(message.object_index);
	control->target_kind = ai::TargetKind::world_object;
	control->target = target;
	control->target_component =
		message.command_target_component;
	player.selected_target_index = target;
	player.selected_target_component =
		message.command_target_component;
	diagnostics::mission_log(
		"network rx opcode=0x4d player=%u target=%d component=%d",
		static_cast<unsigned>(message.source_player),
		player.selected_target_index == UINT16_MAX
			? -1
			: static_cast<int>(player.selected_target_index),
		static_cast<int>(player.selected_target_component));
	return true;
}

void network_service_player_target_reference(
	NetworkRuntime& network,
	const game::World& world)
{
	if (network.role == NetworkRole::offline)
	{
		return;
	}
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	bool has_player_control = false;
	if (player != nullptr)
	{
		for (std::uint8_t index = 0;
			index < player->ai.command_count;
			++index)
		{
			if (player->ai.commands[index].id == 100)
			{
				has_player_control = true;
				break;
			}
		}
	}
	// FUN_004bb980 publishes only while FUN_00402860 can resolve the
	// local AI_PlayerControl command that owns the TargetRef.
	if (!has_player_control)
	{
		return;
	}
	const std::uint16_t target = world.selected_target.index;
	const std::int16_t component = world.target_component;
	if (network.published_target_object == target
		&& network.published_target_component == component)
	{
		return;
	}
	(void)network_publish_player_target_reference(
		network, target, component);
}

bool network_receive_chat(
	NetworkRuntime& network,
	std::uint8_t source_player,
	const char* text)
{
	if (network.role == NetworkRole::offline
		|| source_player >= network.player_count
		|| !network.connected[source_player]
		|| text == nullptr)
	{
		return false;
	}
	if (network.inbound_chat_count >= kNetworkChatCapacity)
	{
		if (!network.inbound_chat_overflow_logged)
		{
			network.inbound_chat_overflow_logged = true;
			diagnostics::mission_log(
				"network inbound chat queue full capacity=%u",
				static_cast<unsigned>(kNetworkChatCapacity));
		}
		return false;
	}
	const std::uint8_t write = static_cast<std::uint8_t>(
		(network.inbound_chat_read + network.inbound_chat_count)
		% kNetworkChatCapacity);
	NetworkChatMessage& message = network.inbound_chat[write];
	message = {};
	message.source_player = source_player;
	std::snprintf(
		message.text, sizeof(message.text), "%s", text);
	++network.inbound_chat_count;
	return true;
}

bool network_pop_chat(
	NetworkRuntime& network,
	NetworkChatMessage& message)
{
	if (network.inbound_chat_count == 0)
	{
		return false;
	}
	message = network.inbound_chat[network.inbound_chat_read];
	network.inbound_chat[network.inbound_chat_read] = {};
	network.inbound_chat_read = static_cast<std::uint8_t>(
		(network.inbound_chat_read + 1) % kNetworkChatCapacity);
	--network.inbound_chat_count;
	if (network.inbound_chat_count == 0)
	{
		network.inbound_chat_read = 0;
		network.inbound_chat_overflow_logged = false;
	}
	return true;
}

void network_mark_session_ready(NetworkRuntime& network)
{
	if (network.role == NetworkRole::offline)
	{
		return;
	}
	const bool was_active = network.session_sync_active;
	network.session_phase_active = false;
	network.session_sync_active = true;
	network.session_start_received = false;
	if (network.role == NetworkRole::host)
	{
		network.session_ready[network.local_player] = true;
	}
	if (!was_active)
	{
		diagnostics::mission_log(
			"network session sync begin role=%s local=%u",
			role_name(network.role),
			static_cast<unsigned>(network.local_player));
	}
}

void network_fixed_tick(
	NetworkRuntime& network,
	std::uint32_t network_tick)
{
	network.network_tick = network_tick;
	if (!network.session_sync_active)
	{
		return;
	}
	if (network.role == NetworkRole::host)
	{
		if (!network.session_phase_active)
		{
			for (std::uint8_t player = 0;
				player < network.player_count;
				++player)
			{
				if (player != network.local_player
					&& network.connected[player]
					&& !network.session_ready[player])
				{
					return;
				}
			}
			std::fill(
				std::begin(network.session_ready),
				std::end(network.session_ready),
				false);
			for (std::uint8_t player = 0;
				player < network.player_count;
				++player)
			{
				if (player != network.local_player
					&& network.connected[player])
				{
					queue_session_script_start(network, player);
				}
			}
			network.session_phase_active = true;
			network.session_phase_tick = network.network_tick;
		}
		if (network.script_sync_delay + network.session_phase_tick
			<= network.network_tick)
		{
			network.session_sync_active = false;
			network.session_phase_active = false;
			diagnostics::mission_log(
				"network session sync complete role=host tick=%u",
				network.network_tick);
		}
		return;
	}
	if (network.role == NetworkRole::client)
	{
		network.session_start_latency = 0;
		if (!network.session_phase_active)
		{
			if (!queue_session_script_ready(network))
			{
				return;
			}
			network.session_phase_active = true;
			network.session_start_received = false;
			network.session_phase_tick = network.network_tick;
			return;
		}
		if (network.session_start_received
			&& network.session_phase_tick < network.network_tick)
		{
			network.session_sync_active = false;
			diagnostics::mission_log(
				"network session sync complete role=client tick=%u "
				"latency=%u",
				network.network_tick,
				static_cast<unsigned>(
					network.session_start_latency));
		}
	}
}

bool network_script_execution_stalled(const NetworkRuntime& network)
{
	return network.session_sync_active;
}

void network_receive_session_script_ready(
	NetworkRuntime& network,
	std::uint8_t ready_player)
{
	if (network.role != NetworkRole::host
		|| ready_player >= network.player_count)
	{
		return;
	}
	if (!network.session_ready[ready_player])
	{
		network.session_ready[ready_player] = true;
		diagnostics::mission_log(
			"network rx opcode=0x37 session-ready player=%u",
			static_cast<unsigned>(ready_player));
	}
}

void network_receive_session_script_start(
	NetworkRuntime& network,
	std::uint8_t one_way_latency)
{
	if (network.role != NetworkRole::client)
	{
		return;
	}
	const bool changed = !network.session_start_received
		|| network.session_start_latency != one_way_latency;
	network.session_start_latency = one_way_latency;
	network.session_start_received = true;
	if (changed)
	{
		diagnostics::mission_log(
			"network rx opcode=0x38 session-start latency=%u",
			static_cast<unsigned>(one_way_latency));
	}
}

bool network_script_sync_poll(
	NetworkRuntime& network,
	ScriptSyncState& sync,
	std::uint8_t sync_index)
{
	if (network.role == NetworkRole::offline)
	{
		return true;
	}
	if (sync.state == 2)
	{
		// MultiplayerScriptSync_command 0x00459e30 uses a signed `jge`
		// against the network tick and completes only on strict less-than.
		if (static_cast<std::int32_t>(sync.deadline)
			< static_cast<std::int32_t>(network.network_tick))
		{
			sync = {};
			diagnostics::mission_log(
				"network script sync complete index=%u tick=%u",
				static_cast<unsigned>(sync_index),
				network.network_tick);
			return true;
		}
		return false;
	}
	if (sync.state == 1)
	{
		if (network.role != NetworkRole::host)
		{
			return false;
		}
		for (std::uint8_t player = 0;
			player < network.player_count;
			++player)
		{
			if (player == network.local_player
				|| !network.connected[player])
			{
				continue;
			}
			if ((sync.ready_mask & (1u << player)) == 0)
			{
				return false;
			}
		}
		queue_script_sync_restart(network, sync_index);
		sync.state = 2;
		sync.ready_mask = 0;
		sync.deadline =
			network.network_tick + network.script_sync_delay;
		diagnostics::mission_log(
			"network script sync restart index=%u deadline=%u",
			static_cast<unsigned>(sync_index),
			sync.deadline);
		return false;
	}
	if (sync.state == 0)
	{
		if (network.role == NetworkRole::client)
		{
			queue_script_sync_ready(network, sync_index);
		}
		sync.state = 1;
		diagnostics::mission_log(
			"network script sync collect index=%u role=%s",
			static_cast<unsigned>(sync_index),
			role_name(network.role));
	}
	return false;
}

void network_receive_script_sync_ready(
	NetworkRuntime& network,
	ScriptSyncState& sync,
	std::uint8_t sync_index,
	std::uint8_t source_player)
{
	if (source_player >= kNetworkPlayerCapacity)
	{
		return;
	}
	const std::uint32_t bit = 1u << source_player;
	if ((sync.ready_mask & bit) == 0)
	{
		sync.ready_mask |= bit;
		diagnostics::mission_log(
			"network rx opcode=0x47 script-sync-ready index=%u "
			"player=%u mask=0x%02x",
			static_cast<unsigned>(sync_index & 0x7fu),
			static_cast<unsigned>(source_player),
			static_cast<unsigned>(sync.ready_mask & 0xffu));
	}
	(void)network;
}

void network_receive_script_sync_restart(
	NetworkRuntime& network,
	ScriptSyncState& sync,
	std::uint8_t sync_index,
	std::uint8_t one_way_latency)
{
	const bool changed = sync.state != 2
		|| sync.deadline
			!= network.network_tick + one_way_latency;
	sync.state = 2;
	sync.deadline = network.network_tick + one_way_latency;
	if (changed)
	{
		diagnostics::mission_log(
			"network rx opcode=0x48 script-sync-restart index=%u "
			"deadline=%u latency=%u",
			static_cast<unsigned>(sync_index & 0x7fu),
			sync.deadline,
			static_cast<unsigned>(one_way_latency));
	}
}

bool network_reset_player_to_spawn(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t player,
	std::int16_t requested_spawn,
	bool publish)
{
	NetworkRuntime& network = runtime.network;
	if (player >= std::size(world.objects)
		|| !world.objects[player].active)
	{
		return false;
	}
	discover_spawn_objects(runtime, world);
	game::WorldObject& before = world.objects[player];
	const bool local_player = player == world.player.index;
	const std::int32_t local_score = before.score;
	const std::uint16_t mission_index = before.mission_index;
	if (mission_index >= runtime.object_count)
	{
		return false;
	}
	const float gun_recharge_scale = before.gun_recharge_scale;
	const float engine_power_scale = before.engine_power_scale;
	const float shield_recharge_scale = before.shield_recharge_scale;
	const game::ObjectHandle old_handle =
		runtime.objects[mission_index].live;
	if (!game::world_mark_departed(world, old_handle)
		|| !runtime_activate_object(runtime, mission_index, world, stats))
	{
		return false;
	}
	game::WorldObject* respawning =
		runtime_resolve_object(runtime, mission_index, world);
	if (respawning == nullptr)
	{
		return false;
	}
	respawning->gun_recharge_scale = gun_recharge_scale;
	respawning->engine_power_scale = engine_power_scale;
	respawning->shield_recharge_scale = shield_recharge_scale;
	// Deathmatch_respawn reconstructs the live object, but retail's local
	// score at 0x00562df4 is independent of that object's lifetime.
	if (local_player)
	{
		respawning->score = local_score;
	}
	ai::command_clear(world, *respawning);
	ai::command_push(world,
		*respawning,
		local_player ? 100 : 101,
		ai::TargetKind::none,
		UINT16_MAX);
	ai::command_push(world,
		*respawning, 121, ai::TargetKind::none, UINT16_MAX);
	if (respawning->type < assets::kShipStatsCount)
	{
		respawning->afterburner_fuel =
			(stats.records[respawning->type].object.afterburner_seconds / 2)
			* 100;
	}
	respawning->runtime_flags &= ~game::kObjectFlagDisabled;

	std::uint16_t selected_spawn = UINT16_MAX;
	if (requested_spawn >= 0)
	{
		selected_spawn =
			static_cast<std::uint16_t>(requested_spawn);
	}
	else
	{
		std::uint16_t candidates[game::kMaxMissionObjects];
		std::uint16_t candidate_count = 0;
		for (std::uint16_t ordinal = 0;
			ordinal < network.spawn_count;
			++ordinal)
		{
			const std::uint16_t spawn =
				network.spawn_objects[ordinal];
			if (spawn_is_free(
					network, world, *respawning, spawn))
			{
				candidates[candidate_count++] = ordinal;
			}
		}
		if (candidate_count == 0)
		{
			diagnostics::mission_log(
				"network respawn failed player=%u reason=no-free-spawn",
				static_cast<unsigned>(player));
			return false;
		}
		game::WorldObject* local =
			game::world_resolve(world, world.player);
		if (local == nullptr)
		{
			return false;
		}
		const int private_random =
			static_cast<int>(game::world_object_rand15(*local));
		const int global_random =
			static_cast<int>(game::world_rand15(world));
		const std::uint16_t ordinal =
			candidates[static_cast<std::uint16_t>(
				std::abs(global_random - private_random)
				% candidate_count)];
		selected_spawn = network.spawn_objects[ordinal];
		if (local_player
			&& network.first_random_spawn
			&& player < network.spawn_count)
		{
			selected_spawn = network.spawn_objects[player];
			network.first_random_spawn = false;
		}
	}
	if (selected_spawn >= std::size(world.objects)
		|| !world.objects[selected_spawn].active)
	{
		return false;
	}
	const game::WorldObject& marker = world.objects[selected_spawn];
	respawning->previous_position = marker.position;
	respawning->position = marker.position;
	respawning->previous_orientation = marker.orientation;
	respawning->orientation = marker.orientation;

	if (publish && network.role != NetworkRole::offline)
	{
		NetworkOutboundMessage respawn_message;
		respawn_message.opcode =
			NetworkGameplayOpcode::deathmatch_respawn;
		respawn_message.delivery =
			NetworkDelivery::broadcast_guaranteed;
		respawn_message.source_player =
			static_cast<std::uint8_t>(world.player.index);
		respawn_message.destination_player =
			kBroadcastDestination;
		respawn_message.object_index = player;
		respawn_message.spawn_object_index = selected_spawn;
		queue_message(network, respawn_message);

		DeferredDamageMessage shield_publication =
			find_or_queue_damage(
				network,
				NetworkGameplayOpcode::bank_state,
				player,
				0,
				false);
		if (shield_publication.message != nullptr
			&& shield_publication.accumulator != nullptr
			&& respawning->type < assets::kShipStatsCount)
		{
			NetworkDamageAccumulator& accumulator =
				*shield_publication.accumulator;
			accumulator.target = respawning;
			accumulator.target_generation =
				respawning->generation;
			const assets::ObjectTypeStats& type =
				stats.records[respawning->type].object;
			accumulator.primary_bank_maximum =
				type.primary_bank_max;
			accumulator.structural_bank_maximum =
				type.structural_bank_max;
			(void)finalize_damage_message(
				*shield_publication.message,
				accumulator);
		}
	}
	diagnostics::mission_log(
		"network respawn player=%u spawn=%u publish=%u",
		static_cast<unsigned>(player),
		static_cast<unsigned>(selected_spawn),
		publish && network.role != NetworkRole::offline ? 1u : 0u);
	return true;
}

bool network_receive_deathmatch_respawn(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint8_t player,
	std::uint16_t spawn_object)
{
	if (network_reset_player_to_spawn(
			runtime,
			world,
			stats,
			static_cast<std::uint8_t>(player & 0x7u),
			static_cast<std::int16_t>(spawn_object & 0x1ffu),
			false))
	{
		diagnostics::mission_log(
			"network rx opcode=0x2c respawn player=%u spawn=%u",
			static_cast<unsigned>(player & 0x7u),
			static_cast<unsigned>(spawn_object & 0x1ffu));
		return true;
	}
	return false;
}

void network_publish_player_ejected(
	NetworkRuntime& network,
	std::uint16_t object_index)
{
	// network_send_player_ejected, LANCER.EXE 0x004bb160.
	NetworkOutboundMessage message;
	message.opcode = NetworkGameplayOpcode::player_ejected;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = network.local_player;
	message.destination_player = kBroadcastDestination;
	message.object_index = static_cast<std::uint16_t>(
		object_index & 0x01ffu);
	if (queue_message(network, message))
	{
		diagnostics::mission_log(
			"network tx opcode=0x45 player-ejected object=%u",
			static_cast<unsigned>(message.object_index));
	}
}

bool network_receive_player_ejected(
	NetworkRuntime& network,
	game::World& world,
	std::uint16_t object_index)
{
	if (network.role == NetworkRole::offline
		|| object_index >= std::size(world.objects))
	{
		return false;
	}
	game::WorldObject& object = world.objects[object_index];
	if (!object.active
		|| object.eject_disabled
		|| object.ai.command_count == 0)
	{
		return false;
	}
	const std::int16_t current = object.ai.commands[0].id;
	if (current != 100 && current != 101 && current != 118)
	{
		return false;
	}
	object.runtime_flags &= ~0x00000800u;
	const bool inserted = ai::command_push(world,
		object, 30, ai::TargetKind::none, UINT16_MAX);
	if (inserted)
	{
		diagnostics::mission_log(
			"network rx opcode=0x45 player-ejected object=%u",
			static_cast<unsigned>(object_index));
	}
	return inserted;
}

bool network_publish_object_state(
	NetworkRuntime& network,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t object_index,
	std::int16_t reference_index,
	bool force_state,
	bool force_absolute)
{
	if (network.role == NetworkRole::offline
		|| object_index >= std::size(world.objects)
		|| !world.objects[object_index].active
		|| reference_index < -1
		|| reference_index >= network.player_count)
	{
		return false;
	}

	game::WorldObject& object = world.objects[object_index];
	if ((active_command_is(object, 17)
			|| active_command_is(object, 119))
		&& object.ai.work.stage != 0)
	{
		// Ship Follow Curve and its backwards variant own a dependent
		// transform after their first stage. Retail refuses to serialize
		// that object even for forced publications.
		return false;
	}

	bool motion_present =
		ai::command_requires_network_motion(object);
	std::uint8_t position_mode =
		(object.state_publication_flags & game::kObjectStatePublishPosition) != 0
			? std::uint8_t{1}
			: std::uint8_t{0};
	bool orientation_present =
		(object.state_publication_flags
			& game::kObjectStatePublishOrientation) != 0;
	if (!force_state
		&& !motion_present
		&& position_mode == 0
		&& !orientation_present)
	{
		return false;
	}
	if (force_state)
	{
		motion_present = true;
		position_mode = 1;
		orientation_present = true;
	}

	// The sequence is sampled and advanced as soon as retail opens the
	// destination bit buffer. This deliberately precedes the stats-null
	// motion suppression, allowing a header-only command-flag packet.
	const std::uint8_t sequence = static_cast<std::uint8_t>(
		object.network_state_sequence & 0x7fu);
	object.network_state_sequence = static_cast<std::uint8_t>(
		(sequence + 1u) & 0x7fu);

	if (position_mode == 1
		&& (std::abs(object.position.x)
				> kObjectPositionModeOneLimit
			|| std::abs(object.position.y)
				> kObjectPositionModeOneLimit
			|| std::abs(object.position.z)
				> kObjectPositionModeOneLimit))
	{
		position_mode = 2;
	}
	if (force_absolute)
	{
		position_mode = 2;
		motion_present = true;
		orientation_present = true;
	}
	if (!object_has_stats(object, stats))
	{
		motion_present = false;
	}

	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::object_state;
	message.opcode = NetworkGameplayOpcode::object_state;
	message.delivery =
		reference_index == -1
			? (force_state
				? NetworkDelivery::broadcast_guaranteed
				: NetworkDelivery::broadcast_conditional)
			: (force_state
				? NetworkDelivery::directed_guaranteed
				: NetworkDelivery::directed_conditional);
	message.source_player = network.local_player;
	message.destination_player =
		reference_index == -1
			? kBroadcastDestination
			: static_cast<std::uint8_t>(reference_index);
	message.object_index = object_index;
	message.object_state_sequence = sequence;
	message.object_state_position_mode = position_mode;
	message.object_state_motion_present = motion_present;
	message.object_state_orientation_present =
		orientation_present;
	message.object_state_transform_sync = force_absolute;

	if (motion_present)
	{
		const float demands[4]{
			object.control_demand.pitch,
			object.control_demand.yaw,
			object.control_demand.roll,
			object.control_demand.throttle,
		};
		for (std::uint8_t index = 0; index < 4; ++index)
		{
			message.object_state_demand[index] =
				static_cast<std::int8_t>(std::clamp(
					retail_round(
						demands[index]
							* kObjectDemandEncodeScale),
					-31,
					31));
		}

		const float angular[3]{
			object.angular_x,
			object.angular_y,
			object.angular_z,
		};
		for (std::uint8_t index = 0; index < 3; ++index)
		{
			message.object_state_angular[index] =
				static_cast<std::int8_t>(sign_extend_low_bits(
					retail_round(
						angular[index]
							* kObjectAngularEncodeScale),
					6));
		}

		const float maximum_speed =
			stats.records[object.type].flight.max_speed;
		for (std::uint8_t index = 0; index < 3; ++index)
		{
			const int encoded = std::clamp(
				retail_round(
					object.linear_velocity[index]
						* kObjectVelocityEncodeScale
						/ maximum_speed),
				-255,
				255);
			message.object_state_velocity[index] =
				static_cast<std::int16_t>(encoded);
		}
	}

	if (position_mode != 0)
	{
		const std::uint8_t width =
			position_mode == 1
				? std::uint8_t{21}
				: position_mode == 2
					? std::uint8_t{29}
					: std::uint8_t{16};
		glm::vec3 encoded_position = object.position;
		if (position_mode == 3
			&& reference_index >= 0
			&& static_cast<std::size_t>(reference_index)
				< std::size(world.objects))
		{
			encoded_position -=
				world.objects[reference_index].position;
		}
		for (std::uint8_t index = 0; index < 3; ++index)
		{
			message.object_state_position[index] =
				sign_extend_low_bits(
					retail_round(
						encoded_position[index]
							* kObjectPositionEncodeScale),
					width);
		}
	}

	if (orientation_present)
	{
		const glm::vec3 euler =
			math::rotation_to_euler(object.orientation);
		for (std::uint8_t index = 0; index < 3; ++index)
		{
			message.object_state_orientation[index] =
				static_cast<std::uint16_t>(
					retail_round(
						euler[index]
							* kObjectOrientationEncodeScale)
					& 0x03ff);
		}
	}
	message.retail_bit_count = object_state_bit_count(
		motion_present,
		position_mode,
		orientation_present);
	const bool queued = queue_message(network, message);
	if (queued && force_state)
	{
		diagnostics::mission_log(
			"network tx opcode=0x1d object=%u sequence=%u "
			"motion=%u position=%u orientation=%u sync=%u",
			static_cast<unsigned>(object_index),
			static_cast<unsigned>(sequence),
			motion_present ? 1u : 0u,
			static_cast<unsigned>(position_mode),
			orientation_present ? 1u : 0u,
			force_absolute ? 1u : 0u);
	}
	return queued;
}

void network_service_object_states(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t network_tick)
{
	NetworkRuntime& network = runtime.network;
	network.network_tick = network_tick;
	if (network.role == NetworkRole::offline
		|| network.next_object_state_service_tick > network_tick)
	{
		return;
	}

	network.next_object_state_service_tick =
		(network_tick / 5u + 1u) * 5u;
	if (network.object_state_interval == 0)
	{
		network.object_state_interval = 10;
	}
	if ((network_tick - network.last_object_publication_tick)
			/ network.object_state_interval == 0)
	{
		return;
	}

	const std::uint8_t connected_count =
		connected_player_count(network);
	const std::uint16_t local_index = network.local_player;
	if (connected_count == 0
		|| local_index >= std::size(world.objects)
		|| !world.objects[local_index].active
		|| active_command_is(world.objects[local_index], 8))
	{
		// Land suppresses the body without advancing the retained
		// publication time; the five-tick outer service remains live.
		return;
	}

	const std::uint32_t scaled_interval = std::min<std::uint32_t>(
		100u,
		network.deathmatch_mode
			? network.object_state_interval
			: network.object_state_interval * 4u);
	if (scaled_interval
		< network_tick - network.local_object_state_tick)
	{
		// Retail advances this timestamp before the runtime-flag and
		// command-priority gates, even when no local packet is produced.
		network.local_object_state_tick = network_tick;
		const game::WorldObject& local = world.objects[local_index];
		if ((local.runtime_flags & 0x10000c40u) == 0
			&& !ai::command_has_positive_priority(local))
		{
			(void)network_publish_object_state(
				network,
				world,
				stats,
				local_index,
				-1,
				false,
				false);
		}
	}

	const std::uint16_t high_water =
		std::min<std::uint16_t>(
			world.object_high_water,
			static_cast<std::uint16_t>(
				std::size(world.objects)));
	const std::uint16_t player_prefix =
		std::min(runtime.player_prefix_count, high_water);
	if (high_water > player_prefix)
	{
		const std::uint16_t attempts = static_cast<std::uint16_t>(
			high_water - player_prefix);
		for (std::uint16_t attempt = 0;
			attempt < attempts;
			++attempt)
		{
			if (queued_retail_bits(network, connected_count)
				> kObjectPublicationBitBudget)
			{
				break;
			}

			++network.object_state_cursor;
			if (network.object_state_cursor >= high_water)
			{
				network.object_state_cursor = player_prefix;
			}
			const std::uint16_t candidate_index =
				network.object_state_cursor;
			game::WorldObject& candidate =
				world.objects[candidate_index];
			if (!candidate.active
				|| (candidate.runtime_flags & 0x00000420u) != 0
				|| ai::command_has_positive_priority(candidate)
				|| !local_owns_object_impl(
					network,
					candidate_index,
					runtime.player_prefix_count,
					local_index)
				|| (object_has_stats(candidate, stats)
					&& (candidate.collision_class == 6
						|| candidate.collision_class == 7))
				|| (candidate.state_publication_flags
					& game::kObjectStateScoopActive) != 0
				|| (runtime.active_camera_mode == 13
					&& candidate_index
						== runtime.director.static_object))
			{
				continue;
			}

			bool close_to_player = false;
			for (std::uint8_t player = 0;
				player < network.player_count;
				++player)
			{
				if (!network.connected[player]
					|| player >= std::size(world.objects))
				{
					continue;
				}
				const game::WorldObject& player_object =
					world.objects[player];
				if ((player_object.runtime_flags
						& 0x10000840u) != 0)
				{
					continue;
				}
				const glm::vec3 delta =
					player_object.position - candidate.position;
				if (glm::dot(delta, delta)
					< kObjectPublicationDistanceSquared)
				{
					close_to_player = true;
					break;
				}
			}
			if (close_to_player)
			{
				(void)network_publish_object_state(
					network,
					world,
					stats,
					candidate_index,
					-1,
					false,
					false);
			}
		}
	}
	network.last_object_publication_tick = network_tick;
}

bool network_receive_object_state(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const NetworkOutboundMessage& message)
{
	NetworkRuntime& network = runtime.network;
	if (network.role == NetworkRole::offline
		|| message.kind != NetworkOutboundKind::object_state
		|| message.opcode != NetworkGameplayOpcode::object_state
		|| message.object_index >= std::size(world.objects)
		|| message.object_state_position_mode > 3)
	{
		return false;
	}
	game::WorldObject& object =
		world.objects[message.object_index];
	if (!object.active)
	{
		return false;
	}

	const std::uint8_t incoming = static_cast<std::uint8_t>(
		message.object_state_sequence & 0x7fu);
	const std::uint8_t previous = static_cast<std::uint8_t>(
		object.network_state_sequence & 0x7fu);
	std::int16_t distance = static_cast<std::int16_t>(
		incoming) - static_cast<std::int16_t>(previous);
	if (distance < 0)
	{
		distance = static_cast<std::int16_t>(distance + 128);
	}
	if (distance > 63)
	{
		return true;
	}
	// Duplicates (distance zero) are accepted. Freshness is committed
	// before any gameplay suppression gate below.
	object.network_state_sequence = incoming;

	const bool deathmatch_respawn_suppressed =
		network.deathmatch_mode
		&& active_command_is(object, 121);
	const bool priority_suppressed =
		ai::command_has_positive_priority(object);
	const bool state_suppressed =
		(object.state_publication_flags & game::kObjectStateScoopActive) != 0;
	const bool director_suppressed =
		runtime.active_camera_mode == 13
		&& message.object_index == runtime.director.static_object;
	bool landing_suppressed = false;
	if (network.local_player < std::size(world.objects))
	{
		landing_suppressed = active_command_is(
			world.objects[network.local_player],
			8);
	}
	if (deathmatch_respawn_suppressed
		|| priority_suppressed
		|| landing_suppressed
		|| director_suppressed
		|| state_suppressed)
	{
		return true;
	}

	if (message.object_state_motion_present)
	{
		object.control_demand.pitch =
			static_cast<float>(sign_extend_low_bits(
				message.object_state_demand[0], 6))
			* kObjectDemandDecodeScale;
		object.control_demand.yaw =
			static_cast<float>(sign_extend_low_bits(
				message.object_state_demand[1], 6))
			* kObjectDemandDecodeScale;
		object.control_demand.roll =
			static_cast<float>(sign_extend_low_bits(
				message.object_state_demand[2], 6))
			* kObjectDemandDecodeScale;
		object.control_demand.throttle =
			static_cast<float>(sign_extend_low_bits(
				message.object_state_demand[3], 6))
			* kObjectDemandDecodeScale;
		object.angular_x =
			static_cast<float>(sign_extend_low_bits(
				message.object_state_angular[0], 6))
			* kObjectAngularDecodeScale;
		object.angular_y =
			static_cast<float>(sign_extend_low_bits(
				message.object_state_angular[1], 6))
			* kObjectAngularDecodeScale;
		object.angular_z =
			static_cast<float>(sign_extend_low_bits(
				message.object_state_angular[2], 6))
			* kObjectAngularDecodeScale;
		object.inertial_angular_step =
			math::rotation_from_euler({
				object.angular_x,
				object.angular_y,
				object.angular_z,
			});
		if (object_has_stats(object, stats))
		{
			const float maximum_speed =
				stats.records[object.type].flight.max_speed;
			for (std::uint8_t index = 0; index < 3; ++index)
			{
				object.linear_velocity[index] =
					static_cast<float>(sign_extend_low_bits(
						message.object_state_velocity[index],
						9))
					* maximum_speed
					* kObjectVelocityDecodeScale;
			}
			object.speed = glm::length(object.linear_velocity);
		}
	}

	const std::uint8_t position_mode =
		message.object_state_position_mode;
	if (position_mode != 0)
	{
		const std::uint8_t width =
			position_mode == 1
				? std::uint8_t{21}
				: position_mode == 2
					? std::uint8_t{29}
					: std::uint8_t{16};
		glm::vec3 position;
		for (std::uint8_t index = 0; index < 3; ++index)
		{
			position[index] = static_cast<float>(
				sign_extend_low_bits(
					message.object_state_position[index],
					width))
				* kObjectPositionDecodeScale;
		}
		if (position_mode == 3
			&& network.local_player < std::size(world.objects))
		{
			position +=
				world.objects[network.local_player].position;
		}
		object.previous_position = position;
		object.position = position;
		if (position_mode == 2
			&& message.object_state_transform_sync)
		{
			object.scene_position = position;
		}
	}

	if (message.object_state_orientation_present)
	{
		glm::vec3 euler;
		for (std::uint8_t index = 0; index < 3; ++index)
		{
			euler[index] = static_cast<float>(
				message.object_state_orientation[index]
					& 0x03ffu)
				* kObjectOrientationDecodeScale;
		}
		object.orientation = math::rotation_from_euler(euler);
		if (message.object_state_transform_sync)
		{
			object.previous_orientation = object.orientation;
			object.scene_orientation = object.orientation;
		}
	}
	return true;
}

void network_flag_local_friendly_fire(
	NetworkRuntime& network,
	game::WorldObject& offender,
	bool promote_all_players)
{
	const std::uint8_t previous =
		offender.ai.friendly_fire_status;
	offender.ai.friendly_fire_status = 1;
	if (promote_all_players && network.role != NetworkRole::offline)
	{
		offender.ai.friendly_fire_status = 2;
		NetworkOutboundMessage message;
		message.opcode = NetworkGameplayOpcode::friendly_fire;
		message.delivery = NetworkDelivery::broadcast_guaranteed;
		message.source_player = network.local_player;
		message.destination_player = kBroadcastDestination;
		message.all_players = true;
		if (queue_message(network, message))
		{
			diagnostics::mission_log(
				"network tx opcode=0x49 friendly-fire all=1 player=%u",
				static_cast<unsigned>(network.local_player));
		}
	}
	if (previous != offender.ai.friendly_fire_status)
	{
		diagnostics::mission_log(
			"network friendly-fire local-status=%u promote-all=%u",
			static_cast<unsigned>(
				offender.ai.friendly_fire_status),
			promote_all_players
				&& network.role != NetworkRole::offline
					? 1u : 0u);
	}
}

void network_publish_friendly_fire_status(NetworkRuntime& network)
{
	if (network.role == NetworkRole::offline)
	{
		return;
	}
	NetworkOutboundMessage message;
	message.opcode = NetworkGameplayOpcode::friendly_fire;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = network.local_player;
	message.destination_player = kBroadcastDestination;
	message.all_players = false;
	if (queue_message(network, message))
	{
		diagnostics::mission_log(
			"network tx opcode=0x49 friendly-fire all=0 player=%u",
			static_cast<unsigned>(network.local_player));
	}
}

void network_receive_friendly_fire(
	NetworkRuntime& network,
	game::World& world,
	std::uint8_t source_player,
	bool all_players)
{
	if (!all_players)
	{
		if (source_player >= network.player_count
			|| source_player >= std::size(world.objects)
			|| !world.objects[source_player].active)
		{
			return;
		}
		game::WorldObject& source = world.objects[source_player];
		if (source.ai.friendly_fire_status < 1)
		{
			source.ai.friendly_fire_status = 1;
			diagnostics::mission_log(
				"network rx opcode=0x49 friendly-fire player=%u "
				"status=1",
				static_cast<unsigned>(source_player));
		}
		return;
	}
	std::uint8_t promoted = 0;
	for (std::uint8_t player = 0;
		player < network.player_count
			&& player < std::size(world.objects);
		++player)
	{
		game::WorldObject& object = world.objects[player];
		if (object.active && object.ai.friendly_fire_status < 1)
		{
			object.ai.friendly_fire_status = 3;
			++promoted;
		}
	}
	if (promoted != 0)
	{
		diagnostics::mission_log(
			"network rx opcode=0x49 friendly-fire all=1 promoted=%u",
			static_cast<unsigned>(promoted));
	}
}

bool network_publish_player_departure(
	NetworkRuntime& network,
	std::uint8_t player)
{
	if (network.role == NetworkRole::offline
		|| player >= network.player_count
		|| !network.connected[player])
	{
		return false;
	}
	// FUN_004bb950 has no authority gate: any online peer may broadcast
	// opcode 0x4c guaranteed for an arbitrary retained four-bit slot. The
	// pause screen uses that path after its per-player latency latch trips.
	NetworkOutboundMessage message;
	message.kind = NetworkOutboundKind::gameplay;
	message.opcode = NetworkGameplayOpcode::player_departure;
	message.delivery = NetworkDelivery::broadcast_guaranteed;
	message.source_player = network.local_player;
	message.destination_player = kBroadcastDestination;
	message.departure_player = player;
	if (!queue_message(network, message))
	{
		return false;
	}
	diagnostics::mission_log(
		"network tx opcode=0x4c departure player=%u",
		static_cast<unsigned>(player));
	return true;
}

bool network_receive_player_departure(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint8_t player)
{
	NetworkRuntime& network = runtime.network;
	if (network.role == NetworkRole::offline
		|| player >= network.player_count
		|| !network.connected[player])
	{
		return false;
	}
	// FUN_004b6f80 0x004b90da..0x004b9115 compares the selected slot's
	// DirectPlay ID with the local ID before invoking the ordinary removal
	// owner. The local branch publishes coordinator state nine and does not
	// remove its own topology record.
	if (player == network.local_player)
	{
		runtime.gameplay_state = 9;
		diagnostics::mission_log(
			"network rx opcode=0x4c local-abort player=%u",
			static_cast<unsigned>(player));
		return true;
	}

	// Deathmatch_player_left (0x004b1730) observes the player as connected;
	// in particular, the Vampire scenario counts the departing slot before
	// deciding whether the last infected player left. Preserve that order.
	deathmatch_scenarios_player_leave(
		runtime, world, stats, player);
	network.connected[player] = false;
	network.one_way_latency[player] = 0;
	network.player_name[player][0] = '\0';
	network.player_kills[player] = 0;
	network.player_deaths[player] = 0;
	network.player_latency[player] = 0;
	network.player_ship[player] = -1;
	std::fill(
		std::begin(network.player_loadout[player]),
		std::end(network.player_loadout[player]),
		std::int16_t{-1});
	network.player_loadout_valid[player] = false;
	network.session_ready[player] = false;
	network.object_team[player] = -1;
	const std::uint32_t player_bit = 1u << player;
	for (ScriptSyncState& sync : runtime.script_sync)
	{
		sync.ready_mask &= ~player_bit;
	}
	discard_directed_messages_for_player(network, player);
	diagnostics::mission_log(
		"network rx opcode=0x4c departure player=%u",
		static_cast<unsigned>(player));
	return true;
}
}
