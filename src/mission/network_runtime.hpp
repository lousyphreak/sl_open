#pragma once

#include "game/runtime_limits.hpp"

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>

namespace sl_open::assets
{
struct ShipStatsTable;
}

namespace sl_open::ai
{
struct Command;
}

namespace sl_open::game
{
struct ObjectModelReference;
struct World;
struct WorldObject;
}

namespace sl_open::mission
{
struct Runtime;
struct ScriptSyncState;

constexpr std::uint8_t kNetworkPlayerCapacity = 8;
constexpr std::uint8_t kNetworkOutboundCapacity = 64;
constexpr std::uint8_t kNetworkChatCapacity = 16;
constexpr std::size_t kNetworkPlayerNameBytes = 32;
constexpr std::size_t kNetworkChatBytes = 64;
constexpr std::size_t kNetworkPlayerLoadoutSlots = 20;

enum class NetworkRole : std::uint8_t
{
	offline,
	host,
	client,
};

enum class NetworkPauseReason : std::uint8_t
{
	player_request = 0,
	connection_stall = 1,
};

// Retained gameplay-message numbers from LANCER.EXE's 80-entry DirectPlay
// opcode table. The reimplementation keeps semantic messages here so a
// transport adapter cannot silently change payload widths or delivery class.
enum class NetworkGameplayOpcode : std::uint8_t
{
	landing = 0x1c,
	object_state = 0x1d,
	component_damage = 0x20,
	component_state = 0x21,
	bank_damage = 0x22,
	bank_state = 0x23,
	// Executor respawn publication uses the same authoritative-bank-state
	// command.
	shield_strength = bank_state,
	pause_state = 0x27,
	deathmatch_restart = 0x29,
	deathmatch_state_request = 0x2a,
	deathmatch_state = 0x2b,
	deathmatch_respawn = 0x2c,
	tag_bomb_assignment = 0x2d,
	tag_bomb_detonate = 0x2e,
	dark_reign_tower_state = 0x30,
	vampire_assignment = 0x31,
	shadow_assignment = 0x32,
	shadow_kill = 0x33,
	nuclear_reset_player = 0x34,
	dark_reign_drop = 0x35,
	nuclear_success = 0x36,
	session_script_ready = 0x37,
	session_script_start = 0x38,
	player_stats = 0x3b,
	deathmatch_pickup = 0x3c,
	deathmatch_powerup_activate = 0x3d,
	deathmatch_unhide_object = 0x3e,
	player_attack_request = 0x3f,
	player_backoff_request = 0x40,
	player_help_request = 0x41,
	cloak_state = 0x44,
	player_ejected = 0x45,
	script_sync_ready = 0x47,
	script_sync_restart = 0x48,
	friendly_fire = 0x49,
	deathmatch_proximity_mine = 0x4b,
	player_departure = 0x4c,
	player_target_reference = 0x4d,
	deathmatch_reposition_object = 0x4f,
};

enum class NetworkDelivery : std::uint8_t
{
	broadcast_conditional,
	broadcast_guaranteed,
	directed_guaranteed,
	directed_conditional,
};

enum class NetworkOutboundKind : std::uint8_t
{
	gameplay,
	object_state,
	// Transitional spelling retained so an independently built transport
	// adapter can consume the new semantic record without a numeric ABI
	// change. Production publishers use object_state exclusively.
	forced_object_state = object_state,
	ai_sequence_sync,
	ai_deferred_command,
	chat,
};

struct NetworkOutboundMessage
{
	NetworkOutboundKind kind{NetworkOutboundKind::gameplay};
	NetworkGameplayOpcode opcode{};
	NetworkDelivery delivery{};
	std::uint8_t source_player{};
	// UINT8_MAX is the semantic broadcast destination.
	std::uint8_t destination_player{UINT8_MAX};
	// Gameplay opcode 0x27 contributes one desired-state bit followed by the
	// complete four-bit pause reason. Values two through fifteen are unnamed
	// but remain valid retail payloads.
	NetworkPauseReason pause_reason{NetworkPauseReason::player_request};
	bool pause_active{};
	// Exact decoded fields used by this command family. Fields which do not
	// belong to an opcode remain zero.
	std::uint16_t object_index{UINT16_MAX};
	std::uint16_t spawn_object_index{UINT16_MAX};
	std::int16_t command_id{-1};
	std::int16_t command_selector{};
	std::uint16_t command_target{UINT16_MAX};
	std::int16_t command_target_component{-1};
	std::int16_t command_sequence{};
	std::uint32_t command_state[4]{};
	std::uint8_t sync_index{};
	std::uint8_t one_way_latency{};
	std::uint8_t command_target_kind{};
	std::uint8_t publication_seed{};
	std::uint8_t tower_state{};
	// Deathmatch scenario command fields. Player fields are decoded values;
	// -1 corresponds to the retail all-ones sentinel (8 in four-bit holder
	// fields). scenario_state retains the variable-width opcode-0x2b
	// callback payload without imposing transport framing.
	std::int8_t scenario_player{-1};
	std::int8_t scenario_secondary_player{-1};
	std::int8_t scenario_powerup{-1};
	std::uint16_t scenario_object{UINT16_MAX};
	// Opcode 0x4c carries the departing topology slot in four bits. Keep it
	// separate from the scenario player fields, whose all-ones values have
	// scenario-specific sentinel meanings.
	std::uint8_t departure_player{UINT8_MAX};
	std::uint16_t scenario_state_bits{};
	std::uint8_t scenario_state[96]{};
	// Single-bit payload shared by opcode 0x32's Shadow assignment and
	// opcode 0x44's requested cloak state.
	bool scenario_flag{};
	std::uint32_t delay_ticks{};
	bool all_players{};
	// Deferred damage-family payloads. LANCER.EXE serializes object and
	// latest-attacker references in nine bits, component live-model IDs in
	// eight bits, bank values in seven bits, and the component-damage
	// delta in sixteen bits. Opcode 0x22 consumes only the bank values
	// selected by damage_bank_mask; opcode 0x23 consumes all eight in
	// primary/structural pairs.
	std::uint16_t damage_source_index{UINT16_MAX};
	std::uint16_t damage_component_value{};
	std::uint8_t damage_component_id{};
	std::uint8_t damage_bank_mask{};
	std::uint8_t damage_bank_value[8]{};
	// Retail's per-object deferred table uses lower numeric values as
	// stronger replacement priorities (0, 10, or 20 for this family).
	std::uint8_t deferred_priority{};
	std::int32_t player_kills{};
	std::int32_t player_deaths{};
	// Opcode 0x1d retains the already-quantized retail fields. Keeping
	// these values semantic-but-quantized makes truncation, saturation and
	// receive reconstruction identical across every transport adapter.
	std::uint8_t object_state_sequence{};
	std::uint8_t object_state_position_mode{};
	bool object_state_motion_present{};
	bool object_state_orientation_present{};
	bool object_state_transform_sync{};
	// pitch, yaw, roll, throttle: signed six-bit fields.
	std::int8_t object_state_demand[4]{};
	// angular X, Y, Z: signed six-bit fields.
	std::int8_t object_state_angular[3]{};
	// world velocity X, Y, Z: signed nine-bit fields.
	std::int16_t object_state_velocity[3]{};
	// position X, Y, Z: signed 21/29/16-bit fields according to mode.
	std::int32_t object_state_position[3]{};
	// Euler X, Y, Z: unsigned ten-bit fields.
	std::uint16_t object_state_orientation[3]{};
	// Exact retail packet contribution, including the seven-bit opcode.
	// This is queue-accounting metadata, not an additional retail field.
	std::uint16_t retail_bit_count{};
	char chat_text[kNetworkChatBytes]{};
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
};

struct NetworkChatMessage
{
	std::uint8_t source_player{};
	char text[kNetworkChatBytes]{};
};

// Raw values retained only while a damage publication is deferred. Retail
// merges floating-point damage first and rounds once when the record is
// serialized; keeping these values outside NetworkOutboundMessage prevents a
// transport adapter from mistaking them for wire fields.
struct NetworkDamageAccumulator
{
	float bank[8]{};
	float component{};
	// Component deferred records compare and retain the complete model-node
	// +0xfc ID. Only the eventual opcode-0x20/0x21 packet field truncates
	// that value to eight bits.
	std::uint32_t component_network_id{UINT32_MAX};
	// Damage records live on the retail GameObject itself, so opcode 0x20,
	// 0x22, and 0x23 sample mutable owner fields when the deferred record is
	// serialized. WorldObject storage is fixed for a mission; retain that
	// equivalent stable reference until network_runtime_pop_outbound.
	const game::WorldObject* target{};
	std::int32_t primary_bank_maximum{};
	std::int32_t structural_bank_maximum{};
	std::uint16_t target_generation{};
};

struct NetworkRuntime
{
	NetworkOutboundMessage outbound[kNetworkOutboundCapacity];
	NetworkDamageAccumulator
		outbound_damage[kNetworkOutboundCapacity];
	NetworkChatMessage inbound_chat[kNetworkChatCapacity];
	bool connected[kNetworkPlayerCapacity]{};
	std::uint8_t one_way_latency[kNetworkPlayerCapacity]{};
	char player_name[kNetworkPlayerCapacity][kNetworkPlayerNameBytes]{};
	std::int32_t player_kills[kNetworkPlayerCapacity]{};
	std::int32_t player_deaths[kNetworkPlayerCapacity]{};
	std::uint32_t player_latency[kNetworkPlayerCapacity]{};
	// The retail launch table at 0x00588400 has eight 0x54-byte entries:
	// one selected-ship dword followed by twenty hardpoint-definition
	// dwords. These decoded values must be present before player-object
	// construction so the initial model finalization and every respawn
	// rebuild the same ordnance.
	std::int16_t player_ship[kNetworkPlayerCapacity]{};
	std::int16_t player_loadout[
		kNetworkPlayerCapacity][kNetworkPlayerLoadoutSlots]{};
	bool player_loadout_valid[kNetworkPlayerCapacity]{};
	bool session_ready[kNetworkPlayerCapacity]{};
	std::uint16_t spawn_objects[game::kMaxMissionObjects]{};
	std::uint16_t published_target_object{UINT16_MAX};
	std::int16_t published_target_component{-1};
	std::uint32_t network_tick{};
	// Retail's 0x005dd52c has only zero writers in the shipped executable.
	std::uint32_t script_sync_delay{};
	std::uint32_t session_phase_tick{};
	// GameObjects_network_service_state, LANCER.EXE 0x004bbef0.
	std::uint32_t next_object_state_service_tick{};
	std::uint32_t last_object_publication_tick{};
	std::uint32_t local_object_state_tick{};
	std::uint32_t object_state_interval{10};
	std::uint16_t object_state_cursor{};
	std::uint8_t outbound_read{};
	std::uint8_t outbound_count{};
	std::uint8_t inbound_chat_read{};
	std::uint8_t inbound_chat_count{};
	std::uint8_t player_count{1};
	std::uint8_t local_player{};
	// DirectPlay begins with gameplay slot zero as authority. Host
	// migration changes this ordinal without renumbering the player prefix.
	std::uint8_t authority_player{};
	std::uint16_t spawn_count{};
	std::uint8_t session_start_latency{};
	std::int8_t deathmatch_scenario{-1};
	NetworkRole role{NetworkRole::offline};
	bool session_sync_active{};
	bool session_phase_active{};
	bool session_start_received{};
	bool spawn_objects_initialized{};
	bool deathmatch_scenario_selected{};
	bool first_random_spawn{true};
	bool outbound_overflow_logged{};
	bool inbound_chat_overflow_logged{};
	// Retail DAT_0050c2f8 and DAT_005dae30. Damage owners suppress
	// same-team mutation in team deathmatch while retaining their HUD and
	// mission-event tails.
	bool deathmatch_mode{};
	bool team_mode{};
	// DAT_0050c2e4 is -1 during ordinary gameplay. Lobby/setup owners can
	// force a larger 0..4 team table so empty configured teams remain
	// visible in the scoreboard.
	std::int8_t configured_team_count{-1};
	std::int32_t object_team[game::kMaxGameObjects]{};
	// FUN_004b14f0 adds every player-score adjustment to the owning
	// team's aggregate while team mode is active.
	std::int32_t team_score[4]{};
	// FUN_004b1580 maintains the parallel team death aggregate.
	std::int32_t team_deaths[4]{};
	// Retail deathmatch respawn-targetable option. Respawn begin copies it
	// to GameObject runtime flag 0x200 (the targetable bit).
	bool respawn_targetable{true};
	// DirectPlay gameplay-ready byte 0x00588735. This is independent of
	// the script-start handshake/stall at 0x005dcc8c.
	bool gameplay_ready{true};
};

void network_runtime_reset(NetworkRuntime& network);
void network_runtime_shutdown(NetworkRuntime& network);
bool network_is_deathmatch_mission(std::uint16_t mission);

// This is the transport/session binding boundary. It retains the retail
// eight-slot topology without choosing a socket or packet framing.
void network_runtime_configure(
	NetworkRuntime& network,
	NetworkRole role,
	std::uint8_t local_player,
	std::uint8_t player_count,
	const bool connected[kNetworkPlayerCapacity],
	const std::uint8_t one_way_latency[kNetworkPlayerCapacity]);
bool network_update_authority(
	NetworkRuntime& network,
	NetworkRole role,
	std::uint8_t authority_player);
// Retail initializes disconnected team slots to -1 and copies the lobby
// assignment for each connected player. Deathmatch mode also changes ordinary
// object ownership from co-op striping to host authority. Call after
// network_runtime_configure.
void network_runtime_configure_teams(
	NetworkRuntime& network,
	bool deathmatch_mode,
	bool team_mode,
	const std::int32_t team[kNetworkPlayerCapacity],
	std::int8_t configured_team_count = -1);
void network_set_player_metadata(
	NetworkRuntime& network,
	std::uint8_t player,
	const char* name,
	std::uint32_t latency);

bool network_runtime_pop_outbound(
	NetworkRuntime& network,
	NetworkOutboundMessage& message);
// Shared semantic queue boundary used by the standalone deathmatch-scenario
// callback layer. The caller supplies the exact opcode, destination, payload,
// and delivery class.
bool network_queue_gameplay_message(
	NetworkRuntime& network,
	const NetworkOutboundMessage& message);
// Pause_send, LANCER.EXE 0x004baae0. The local owner broadcasts the desired
// state and raw four-bit reason through the guaranteed gameplay buffer.
bool network_publish_pause_state(
	NetworkRuntime& network,
	bool active,
	NetworkPauseReason reason);
bool network_publish_cloak_state(
	NetworkRuntime& network,
	bool active);
void network_publish_player_stats(
	NetworkRuntime& network,
	std::uint16_t local_world_index);
bool network_receive_player_stats(
	Runtime& runtime,
	game::World& world,
	std::uint8_t source_player,
	std::int32_t kills,
	std::int32_t deaths);
bool network_publish_chat(
	NetworkRuntime& network,
	std::int16_t destination_player,
	const char (&text)[kNetworkChatBytes]);
bool network_publish_player_comms_command(
	NetworkRuntime& network,
	NetworkGameplayOpcode opcode,
	std::int16_t destination_player,
	std::uint16_t target_object);
bool network_publish_landing(NetworkRuntime& network);
bool network_receive_player_comms_command(
	Runtime& runtime,
	game::World& world,
	const NetworkOutboundMessage& message);
bool network_publish_player_target_reference(
	NetworkRuntime& network,
	std::uint16_t target_object,
	std::int16_t target_component);
bool network_receive_player_target_reference(
	NetworkRuntime& network,
	game::World& world,
	const NetworkOutboundMessage& message);
void network_service_player_target_reference(
	NetworkRuntime& network,
	const game::World& world);
bool network_receive_chat(
	NetworkRuntime& network,
	std::uint8_t source_player,
	const char* text);
bool network_pop_chat(
	NetworkRuntime& network,
	NetworkChatMessage& message);
bool network_local_owns_object(
	const NetworkRuntime& network,
	std::uint16_t live_object_index,
	std::uint16_t player_prefix_count,
	std::uint16_t local_player_index);

// GameObject deferred damage publications, LANCER.EXE 0x004ba9f0 and
// 0x004ba810. The target's latest-attacker field and post-damage bank/model
// state must already have been updated when these are called.
//
// component_network_id is the sequential ID written to instantiated model
// offset +0xfc by FUN_00466ba0's recursive preorder walk. It is deliberately
// explicit: neither ObjectModelReference::source_node nor its index in
// target.model_references is this ID for every model tree.
bool network_defer_bank_damage(
	NetworkRuntime& network,
	const game::WorldObject& target,
	const assets::ShipStatsTable& stats,
	std::uint16_t target_index,
	std::uint16_t player_prefix_count,
	std::uint16_t local_player_index,
	std::uint8_t bank,
	float damage);
bool network_defer_component_damage(
	NetworkRuntime& network,
	const game::WorldObject& target,
	const game::ObjectModelReference& component,
	std::uint32_t component_network_id,
	std::uint16_t target_index,
	std::uint16_t player_prefix_count,
	std::uint16_t local_player_index,
	float damage);

// Gameplay receive cases 0x20..0x23 from LANCER.EXE
// 0x004b8369..0x004b88bd. A transport adapter decodes the retail bit fields
// into NetworkOutboundMessage's damage fields, then passes that semantic
// message here. Delta packets mutate the retained local copy and make the
// owning peer enqueue the corresponding authoritative state packet.
bool network_receive_damage(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const NetworkOutboundMessage& message,
	bool impact_feedback_enabled);

void network_publish_ai_sequence_sync(
	NetworkRuntime& network,
	std::uint16_t object_index,
	std::uint8_t sync_index);
bool network_receive_ai_sequence_sync(
	NetworkRuntime& network,
	game::World& world,
	std::uint16_t object_index,
	std::uint8_t sync_index,
	std::uint8_t source_player);
void network_publish_ai_deferred_command(
	NetworkRuntime& network,
	std::uint16_t object_index,
	const ai::Command& command,
	std::uint32_t delay_ticks,
	std::uint8_t publication_seed);
void network_publish_dark_reign_tower_state(
	NetworkRuntime& network,
	std::uint8_t state);
bool network_receive_ai_deferred_command(
	game::World& world,
	const NetworkOutboundMessage& message,
	std::uint32_t current_tick);
void network_set_gameplay_ready(
	NetworkRuntime& network,
	bool ready);

// Executor command 45 and its GameObjects_network_fixed_tick consumer.
void network_mark_session_ready(NetworkRuntime& network);
void network_fixed_tick(
	NetworkRuntime& network,
	std::uint32_t network_tick);
bool network_script_execution_stalled(const NetworkRuntime& network);
void network_receive_session_script_ready(
	NetworkRuntime& network,
	std::uint8_t ready_player);
void network_receive_session_script_start(
	NetworkRuntime& network,
	std::uint8_t one_way_latency);

// Executor command 86 and gameplay opcodes 0x47/0x48.
bool network_script_sync_poll(
	NetworkRuntime& network,
	ScriptSyncState& sync,
	std::uint8_t sync_index);
void network_receive_script_sync_ready(
	NetworkRuntime& network,
	ScriptSyncState& sync,
	std::uint8_t sync_index,
	std::uint8_t source_player);
void network_receive_script_sync_restart(
	NetworkRuntime& network,
	ScriptSyncState& sync,
	std::uint8_t sync_index,
	std::uint8_t one_way_latency);

// Executor command 55 and opcode 0x2c.
bool network_reset_player_to_spawn(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t player,
	std::int16_t requested_spawn,
	bool publish);
bool network_receive_deathmatch_respawn(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint8_t player,
	std::uint16_t spawn_object);

void network_publish_player_ejected(
	NetworkRuntime& network,
	std::uint16_t object_index);
bool network_receive_player_ejected(
	NetworkRuntime& network,
	game::World& world,
	std::uint16_t object_index);

// LANCER.EXE's complete opcode-0x1d state encoder. reference_index -1
// selects broadcast; a nonnegative player slot selects directed delivery.
bool network_publish_object_state(
	NetworkRuntime& network,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t object_index,
	std::int16_t reference_index,
	bool force_state,
	bool force_absolute);
// Five-tick publication/scan owner, including local cadence, ownership,
// proximity and retail queued-bit budget gates.
void network_service_object_states(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t network_tick);
bool network_receive_object_state(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const NetworkOutboundMessage& message);

// FriendlyFire_flag_local_offender, mission-frame consequence publication,
// and opcode 0x49's two receiver branches.
void network_flag_local_friendly_fire(
	NetworkRuntime& network,
	game::WorldObject& offender,
	bool promote_all_players);
void network_publish_friendly_fire_status(NetworkRuntime& network);
void network_receive_friendly_fire(
	NetworkRuntime& network,
	game::World& world,
	std::uint8_t source_player,
	bool all_players);

// FUN_004bb950's guaranteed opcode-0x4c REJECT publication has no authority
// gate and retains an arbitrary four-bit target slot. FUN_004b6f80's receiver
// deliberately processes scenario departure state before clearing a remote
// topology slot. A packet naming the local slot selects gameplay coordinator
// state nine instead.
bool network_publish_player_departure(
	NetworkRuntime& network,
	std::uint8_t player);
bool network_receive_player_departure(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint8_t player);
}
