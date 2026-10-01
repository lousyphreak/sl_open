#pragma once

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>

namespace sl_open
{
struct LanguageTable;
}

namespace sl_open::assets
{
struct ShipStatsTable;
}

namespace sl_open::game
{
struct FlightDemand;
struct World;
struct WorldObject;
}

namespace sl_open::hud
{
struct Runtime;
}

namespace sl_open::mission
{
struct NetworkOutboundMessage;
struct Runtime;

constexpr std::size_t kDeathmatchScenarioStateBytes = 96;
constexpr std::uint8_t kDeathmatchScenarioPlayerCapacity = 8;
constexpr std::uint8_t kDeathmatchScenarioBeaconCapacity = 6;
constexpr std::uint8_t kDeathmatchScenarioMessageCapacity = 16;
constexpr std::uint8_t kDeathmatchPowerupCount = 10;
constexpr std::uint8_t kDeathmatchMinePoolCapacity = 20;

enum class DeathmatchScenario : std::int8_t
{
	none = -1,
	asteroid_field = 0,
	nuclear_threat = 1,
	dark_reign = 2,
	tag_bomb = 3,
	hunt_the_shadow = 4,
	vampires = 5,
};

enum class DeathmatchScenarioMessageKind : std::uint8_t
{
	restart,
	need_more_players,
	nuclear_all_beacons,
	nuclear_success,
	nuclear_kill,
	tag_no_bomb,
	tag_passed,
	tag_exploded,
	shadow_none,
	shadow_assigned,
	shadow_replaced,
	shadow_kill,
	vampire_none,
	vampire_assigned,
	vampire_last_human,
	vampire_last_left,
};

struct DeathmatchScenarioMessage
{
	DeathmatchScenarioMessageKind kind{};
	std::int8_t primary{-1};
	std::int8_t secondary{-1};
};

// Mutable state owned by one mission runtime. These fields are the
// value-backed equivalents of LANCER.EXE's scenario globals at
// 0x005db508..0x005db600; they deliberately do not live in process globals,
// so a stopped/restarted mission cannot leak a holder or snapshot latch.
struct DeathmatchScenarioState
{
	glm::vec3 arena_center{0.0f};
	float arena_radius{};

	glm::vec3 nuclear_snapshot_position[
		kDeathmatchScenarioBeaconCapacity]{};
	std::int16_t nuclear_snapshot_counter[
		kDeathmatchScenarioPlayerCapacity]{
			-1, -1, -1, -1, -1, -1, -1, -1,
		};
	std::uint16_t nuclear_beacon[
		kDeathmatchScenarioBeaconCapacity]{
			UINT16_MAX, UINT16_MAX, UINT16_MAX,
			UINT16_MAX, UINT16_MAX, UINT16_MAX,
		};
	std::uint16_t nuclear_victim[
		kDeathmatchScenarioPlayerCapacity]{
			UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX,
			UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX,
		};
	std::uint16_t nuclear_victim_generation[
		kDeathmatchScenarioPlayerCapacity]{};
	std::uint32_t nuclear_detonation_tick{};
	std::uint8_t nuclear_snapshot_visible{};
	std::uint8_t nuclear_beacon_count{};
	std::uint8_t nuclear_victim_count{};
	std::int8_t nuclear_owner{-1};
	bool nuclear_effect_active{};
	// DAT_005db318 is shared by PickupGeneric drops. Successive loose
	// scenario objects cycle through the owner's six local cardinal axes.
	std::uint8_t pickup_drop_cursor{};

	std::uint16_t dark_relay{UINT16_MAX};
	std::uint16_t dark_tower{UINT16_MAX};
	std::int8_t dark_holder{-1};
	std::int8_t dark_snapshot_holder{-1};

	std::int8_t tag_holder{-1};
	std::int8_t tag_last_tagger{-1};
	std::int8_t tag_snapshot_holder{-1};
	std::int8_t tag_snapshot_tagger{-1};
	std::int8_t tag_pending{};
	std::int32_t tag_timer{4400};
	std::uint32_t tag_previous_tick{};
	bool tag_bomb_death{};

	std::int8_t shadow_holder{-1};
	std::int8_t shadow_snapshot_holder{-1};

	bool vampire[kDeathmatchScenarioPlayerCapacity]{};
	std::uint8_t vampire_snapshot_mask{};

	// PickupGeneric and DMPowerup_create_pool retain one active effect and
	// linked pickup per player, plus a twenty-object circular proximity-mine
	// pool. These are the value-backed equivalents of GameObject+0x754..+0x760
	// and DAT_005db30c/DAT_005db3dc.
	std::int8_t powerup_active[kDeathmatchScenarioPlayerCapacity]{
		-1, -1, -1, -1, -1, -1, -1, -1,
	};
	std::uint16_t powerup_pickup[kDeathmatchScenarioPlayerCapacity]{
		UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX,
		UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX,
	};
	std::uint32_t powerup_expiry[kDeathmatchScenarioPlayerCapacity]{
		UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
		UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
	};
	std::uint32_t powerup_acquired[kDeathmatchScenarioPlayerCapacity]{};
	// DAT_005db3e0..005db3fc. Ordinary multiplayer death sets the victim's
	// pickup eligibility to gameplay tick + 100 while the respawn update is
	// in flight. Retail initializes these signed deadlines to -1.
	std::int32_t player_pickup_ready_tick[
		kDeathmatchScenarioPlayerCapacity]{
			-1, -1, -1, -1, -1, -1, -1, -1,
		};
	std::uint16_t mine_pool[kDeathmatchMinePoolCapacity]{
		UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX,
		UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX,
		UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX,
		UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX,
	};
	std::uint8_t mine_pool_count{};
	std::uint8_t mine_pool_cursor{};
	bool powerup_pool_initialized{};

	DeathmatchScenarioMessage
		messages[kDeathmatchScenarioMessageCapacity];
	std::uint8_t message_count{};
	std::uint32_t pickup_warning_tick{};
	std::uint32_t last_nuclear_request_tick{};
	std::uint32_t last_update_tick{};
	bool snapshot_pending{};
	bool initialized{};
};

struct DeathmatchScenarioHudStatus
{
	std::uint16_t shape{UINT16_MAX};
	std::uint16_t suffix_language_id{UINT16_MAX};
	std::int32_t value{};
	bool draw_value{};
	bool visible{};
};

struct DeathmatchPowerupHudStatus
{
	std::uint8_t shape{UINT8_MAX};
	std::uint8_t bar_offset{};
	std::uint8_t bar_length{};
	bool draw_bar{};
	bool visible{};
};

// Record-table selection and the once-per-session callback at 0x004b2ca0 /
// 0x004b2cf0.
void deathmatch_scenarios_select(Runtime& runtime);
void deathmatch_scenarios_initialize(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t simulation_tick);

// Per admitted, nonzero-delta mission-frame callback followed by
// PickupGeneric proximity service, matching 0x004b2d10 then 0x004b18e0.
void deathmatch_scenarios_update(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t simulation_tick);

// Scenario record callbacks.
void deathmatch_scenarios_player_death(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t player,
	std::uint32_t simulation_tick);
void deathmatch_scenarios_player_leave(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t player);
void deathmatch_scenarios_player_weapon_hit(
	Runtime& runtime,
	game::World& world,
	std::uint16_t victim,
	std::uint16_t attacker,
	std::uint32_t simulation_tick);
void deathmatch_scenarios_post_respawn(
	Runtime& runtime,
	game::World& world,
	std::uint16_t player);
bool deathmatch_scenarios_powerup_allowed(
	const Runtime& runtime,
	std::uint16_t player);
// Player_launch_selected_ordnance calls this before its ordinary launch path.
// true consumes the action (manual mine or an already-active automatic
// effect); false preserves the ordinary ordnance launch (no effect, or the
// Missile pickup after its activation callback has run).
bool deathmatch_scenarios_activate_powerup(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t player,
	std::uint32_t simulation_tick);
void deathmatch_scenarios_apply_player_controls(
	const Runtime& runtime,
	std::uint16_t player,
	game::FlightDemand& demand);
bool deathmatch_scenarios_shield_recharge_allowed(
	const Runtime& runtime,
	std::uint16_t player);

// The variable-width record serialization callbacks. bit_count is the exact
// retail field width (662/4/8/4/8 bits for the five stateful scenarios).
bool deathmatch_scenarios_serialize(
	const Runtime& runtime,
	const game::World& world,
	std::uint8_t (&bytes)[kDeathmatchScenarioStateBytes],
	std::uint16_t& bit_count);
bool deathmatch_scenarios_deserialize(
	Runtime& runtime,
	const std::uint8_t* bytes,
	std::size_t byte_count,
	std::uint16_t bit_count);

// Semantic gameplay-message receiver for opcodes 0x29, 0x2a, 0x2b,
// 0x2d..0x36, 0x3c, 0x3e, and 0x4f.
bool deathmatch_scenarios_receive(
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const NetworkOutboundMessage& message,
	std::uint32_t simulation_tick);
void deathmatch_scenarios_publish_restart(Runtime& runtime);

// Scenario HUD callback, target-info callback, scoreboard-row callback, and
// timed-message bridge.
DeathmatchScenarioHudStatus deathmatch_scenarios_hud_status(
	const Runtime& runtime,
	const game::World& world);
DeathmatchPowerupHudStatus deathmatch_powerup_hud_status(
	const Runtime& runtime,
	const game::World& world,
	std::uint32_t simulation_tick);
bool deathmatch_scenarios_target_label(
	const Runtime& runtime,
	const game::WorldObject& target,
	const LanguageTable& language,
	char* output,
	std::size_t output_size);
std::uint16_t deathmatch_scenarios_scoreboard_shape(
	const Runtime& runtime,
	const game::World& world,
	std::uint8_t player);
std::int32_t deathmatch_scenarios_scoreboard_value(
	const Runtime& runtime,
	const game::World& world,
	std::uint8_t player);
void deathmatch_scenarios_flush_messages(
	Runtime& runtime,
	hud::Runtime& hud,
	const LanguageTable& language,
	std::uint32_t simulation_tick);
}
