#pragma once

#include <cstddef>
#include <cstdint>

namespace sl_open
{
struct LanguageTable;
}

namespace sl_open::assets
{
struct PilotStatsTable;
struct ShipStatsTable;
}

namespace sl_open::game
{
struct World;
struct WorldObject;
}

namespace sl_open::mission
{
struct PresentationRequest;
struct Runtime;

enum class CommsPlaybackMode : std::uint8_t
{
	immediate,
	queue,
	only_if_idle,
};

struct QueuedComm
{
	std::int32_t category{};
	std::int16_t voice_id{};
	char fm8_path[50]{};
	char voice_path[52]{};
	std::int32_t speaker_id{-1};
	std::int32_t expires_at{-1};
};

static_assert(sizeof(QueuedComm) == 0x74);
static_assert(offsetof(QueuedComm, category) == 0x00);
static_assert(offsetof(QueuedComm, voice_id) == 0x04);
static_assert(offsetof(QueuedComm, fm8_path) == 0x06);
static_assert(offsetof(QueuedComm, voice_path) == 0x38);
static_assert(offsetof(QueuedComm, speaker_id) == 0x6c);
static_assert(offsetof(QueuedComm, expires_at) == 0x70);

struct PendingCommsResponse
{
	std::int16_t active{};
	std::int32_t speaker_id{-1};
	std::int32_t listener_id{-1};
	std::int16_t response_flag{};
	std::int16_t unknown_0e{-1};
	std::int16_t static_voice_id{-1};
	std::int32_t expires_at{-1};
	char fm8_path[50]{};
	char voice_path[50]{};
};

struct CommsMenuOption
{
	char label[100]{};
	std::uint16_t language_id{UINT16_MAX};
	std::uint16_t secondary_language_id{UINT16_MAX};
	std::int16_t command{-1};
	std::int16_t target{-1};
};

struct PlayerCommsState
{
	QueuedComm queue[5];
	PendingCommsResponse responses[5];
	CommsMenuOption options[32];
	std::int16_t queue_count{};
	std::int16_t queue_write{};
	std::int16_t queue_read{};
	std::int16_t current_command{-1};
	std::int16_t current_target{-1};
	// DAT_00529fbc retains the player selected by command 23 for handlers
	// reached through the retail -2 directed-player sentinel.
	std::int16_t remote_selected_player{-1};
	std::int16_t chat_destination{-1};
	// DAT_0057e05c retains the carrier used by launch, landing, and base
	// comms rather than searching the live object table for a carrier type.
	std::uint16_t carrier_world_index{UINT16_MAX};
	std::int16_t option_count{};
	std::int16_t active_voice{};
	std::uint16_t title_language_id{332};
	std::uint16_t remote_command_target{};
	std::int32_t active_speaker{-1};
	std::int32_t saved_ejected_pilot{-1};
	std::uint32_t saved_ejection_rescue_at{};
	std::uint32_t next_return_prompt{};
	std::uint32_t next_jump_warning{};
	std::uint32_t next_lock_warning{};
	std::uint32_t next_player_kill{};
	std::uint32_t next_damage_taunt{};
	std::uint32_t next_landing_request{};
	std::uint8_t jump_warning_stage{};
	std::uint8_t speech_hud_state{};
	std::uint8_t remote_command_source{UINT8_MAX};
	std::uint8_t remote_command_kind{};
	std::uint8_t remote_response{};
	bool menu_open{};
	bool all_channels_open{true};
	bool backup_requested{};
	bool return_prompt_latched{};
	bool jump_warning_latched{};
	bool chat_requested{};
	bool open_panel_requested{};
	// DAT_00588735 is latched once the local player is the first live
	// multiplayer player slot. Co-op landing permission reads this latch.
	bool multiplayer_landing_enabled{};
	// The same transition posts language string 0x558 for a non-host co-op
	// player. The HUD owner consumes this after the simulation update.
	bool multiplayer_landing_notice_pending{};
};

void player_comms_reset(PlayerCommsState& state);
const char* player_comms_voice_prefix(
	std::uint16_t pilot,
	bool hostile);
bool player_comms_busy(
	const PlayerCommsState& state,
	const PresentationRequest& presentation);
bool player_comms_play_or_queue(
	Runtime& mission,
	game::World& world,
	const char* fm8_path,
	const char* voice_path,
	CommsPlaybackMode mode,
	std::int16_t voice_id,
	std::int32_t category,
	std::int32_t speaker_id,
	std::int32_t lifetime,
	std::uint32_t simulation_tick);
bool player_comms_play_live_pilot(
	Runtime& mission,
	game::World& world,
	std::uint16_t speaker_world_index,
	std::uint32_t face_variant,
	const char* voice_path,
	CommsPlaybackMode mode,
	std::int32_t category,
	std::int32_t lifetime,
	std::uint32_t simulation_tick);
bool player_comms_play_compiled_pilot(
	Runtime& mission,
	game::World& world,
	std::uint16_t pilot,
	std::uint32_t face_variant,
	const char* voice_path,
	CommsPlaybackMode mode,
	std::int32_t category,
	std::int32_t lifetime,
	std::uint32_t simulation_tick);
void player_comms_service(
	Runtime& mission,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t gameplay_tick,
	std::uint32_t script_tick,
	std::uint32_t simulation_tick);

void player_comms_open_menu(
	Runtime& mission,
	game::World& world,
	const assets::ShipStatsTable& stats);
bool player_comms_shortcut_target_valid(const game::World& world);
void player_comms_execute_shortcut(
	Runtime& mission,
	game::World& world,
	const assets::PilotStatsTable& pilot_stats,
	std::uint8_t shortcut,
	std::uint32_t gameplay_tick);
void player_comms_close_menu(Runtime& mission);
void player_comms_select_option(
	Runtime& mission,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const assets::PilotStatsTable& pilot_stats,
	std::uint8_t one_based_option,
	std::uint32_t gameplay_tick);
bool player_comms_receive_network_command(
	Runtime& mission,
	game::World& world,
	std::uint8_t source_player,
	std::uint8_t command,
	std::uint16_t target_object);
void player_comms_receive_network_landing(
	Runtime& mission,
	game::World& world,
	std::uint32_t gameplay_tick);
bool player_comms_take_chat_request(
	PlayerCommsState& state,
	std::int16_t& destination);

void player_comms_on_player_destroyed_target(
	Runtime& mission,
	game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t target_world_index,
	std::uint32_t gameplay_tick,
	std::uint32_t simulation_tick);
void player_comms_on_pilot_death(
	Runtime& mission,
	game::World& world,
	std::uint16_t pilot_world_index,
	std::uint32_t simulation_tick);
void player_comms_on_pilot_ejected(
	Runtime& mission,
	game::World& world,
	std::uint16_t pilot_world_index,
	std::uint32_t simulation_tick);
void player_comms_on_player_damaged(
	Runtime& mission,
	game::World& world,
	std::uint16_t attacker_world_index,
	std::uint32_t simulation_tick);
void player_comms_on_player_launch(
	Runtime& mission,
	game::World& world,
	std::uint16_t carrier_world_index,
	std::uint32_t simulation_tick);
void player_comms_retain_carrier(
	Runtime& mission,
	std::uint16_t carrier_world_index);
}
