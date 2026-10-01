#pragma once

#include <cstdint>

namespace sl_open
{
struct LanguageTable;
}

namespace sl_open::render
{
struct FrontendCommands;
struct FrontendRenderer;
}

namespace sl_open::frontend
{
constexpr std::uint8_t kMultiplayerLobbyPlayerCapacity = 8;
constexpr std::uint8_t kCooperativeLobbyPlayerCapacity = 4;
constexpr std::uint8_t kMultiplayerLobbyChatLineCapacity = 8;
constexpr std::uint8_t kMultiplayerLobbyCallsignBytes = 20;
constexpr std::uint8_t kMultiplayerLobbySessionNameBytes = 64;
constexpr std::uint8_t kMultiplayerLobbyChatBytes = 64;
constexpr std::uint8_t kMultiplayerLobbyLoadoutCapacity = 20;

enum class MultiplayerLobbyKind : std::uint8_t
{
	cooperative,
	deathmatch,
};

enum class MultiplayerLobbyRole : std::uint8_t
{
	host,
	client,
};

// FUN_0044b950 and the six 0x40-byte records at 0x0050c798 use this visual
// order.  The values are the retail authored mission numbers.
enum class DeathmatchScenario : std::uint8_t
{
	nuclear_threat = 85,
	dark_reign = 82,
	tag_bomb = 81,
	hunt_the_shadow = 83,
	vampires = 84,
	asteroid_field = 87,
};

struct MultiplayerLobbyPlayer
{
	// Opaque session-owned identity.  The frontend only returns the selected
	// value; it never treats it as a DirectPlay or socket identifier.
	std::uint64_t identity{};
	const char* callsign{};
	std::int8_t team{-1};
	std::int8_t selected_ship{-1};
	bool connected{};
	bool local{};
	bool ready{};
};

// Cooperative NEW creates this state at mission 1.  LOAD temporarily leaves
// the lobby for FUN_00431730 and returns the complete selected campaign state.
// The opaque identity keeps branch/progression state in its owning coordinator;
// mission, ship, and loadout are copied here because they are the exact inputs
// needed by the lobby -> shared loadout -> gameplay handoff.
struct MultiplayerCooperativeCampaignView
{
	std::uint64_t identity{};
	const std::int16_t* loadout{};
	const char* callsign{};
	const char* save_name{};
	std::uint16_t mission{1};
	std::int16_t selected_ship{-1};
	std::uint8_t loadout_count{};
	bool valid{};
	bool loaded_save{};
};

// All storage remains owned by the session/transport layer.  The lobby has no
// synthetic roster and cannot make a disconnected peer appear ready.
struct MultiplayerLobbyView
{
	MultiplayerLobbyPlayer players[kMultiplayerLobbyPlayerCapacity]{};
	const char* chat_lines[kMultiplayerLobbyChatLineCapacity]{};
	MultiplayerCooperativeCampaignView cooperative_campaign;
	const char* callsign{};
	const char* host_callsign{};
	const char* session_name{};
	const char* chat_entry{};
	std::uint16_t cooperative_mission{1};
	std::uint8_t player_count{};
	std::uint8_t chat_line_count{};
	DeathmatchScenario deathmatch_scenario{
		DeathmatchScenario::asteroid_field};
	MultiplayerLobbyKind kind{MultiplayerLobbyKind::cooperative};
	MultiplayerLobbyRole role{MultiplayerLobbyRole::client};
	bool cooperative_saved_game{};
	bool local_ready{};
	bool ai_turrets{};
	bool target_players{true};
	bool teamplay{};
	bool launch_allowed{};
	bool teamplay_supported{};
};

enum class MultiplayerLobbyFocus : std::uint8_t
{
	none,
	callsign,
	session_name,
	chat,
};

enum class MultiplayerLobbyActionType : std::uint8_t
{
	none,
	focus_callsign,
	focus_session_name,
	focus_chat,
	send_chat,
	select_player,
	remove_player,
	new_cooperative_game,
	load_cooperative_game,
	start_cooperative_game,
	toggle_ready,
	start_deathmatch,
	select_deathmatch_scenario,
	toggle_ai_turrets,
	toggle_target_players,
	toggle_teamplay,
	select_team,
	select_ship,
	edit_text,
	backspace_text,
	back_to_browser,
	main_menu,
	quit,
};

struct MultiplayerLobbyAction
{
	MultiplayerLobbyActionType type{MultiplayerLobbyActionType::none};
	std::uint64_t player_identity{};
	std::uint16_t mission{};
	std::int8_t team{-1};
	std::int8_t selected_ship{-1};
	DeathmatchScenario scenario{DeathmatchScenario::asteroid_field};
	MultiplayerLobbyFocus text_field{MultiplayerLobbyFocus::none};
	char text[kMultiplayerLobbyChatBytes]{};
	std::int16_t loadout[kMultiplayerLobbyLoadoutCapacity]{};
	std::uint8_t loadout_count{};
	std::uint64_t campaign_identity{};
	bool value{};
};

struct MultiplayerLobby
{
	float pointer_x{320.0f};
	float pointer_y{240.0f};
	std::int16_t description_scroll{};
	std::int16_t description_scroll_max{};
	std::int8_t hovered{-1};
	std::int8_t tooltip_hover{-1};
	std::int8_t selected_player{-1};
	MultiplayerLobbyFocus focus{MultiplayerLobbyFocus::none};
	MultiplayerLobbyKind kind{MultiplayerLobbyKind::cooperative};
	MultiplayerLobbyRole role{MultiplayerLobbyRole::client};
	std::uint64_t entered_at{};
	std::uint64_t hovered_at{};
	bool team_picker_open{};
	bool ship_picker_open{};
};

void multiplayer_lobby_enter(
	MultiplayerLobby& lobby,
	MultiplayerLobbyKind kind,
	MultiplayerLobbyRole role,
	std::uint64_t now);
void multiplayer_lobby_set_pointer(
	MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view,
	float x,
	float y,
	bool inside);
MultiplayerLobbyAction multiplayer_lobby_select(
	MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view);
MultiplayerLobbyAction multiplayer_lobby_submit_chat(
	MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view);
void multiplayer_lobby_begin_chat(MultiplayerLobby& lobby);
MultiplayerLobbyAction multiplayer_lobby_text(
	const MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view,
	const char* text);
MultiplayerLobbyAction multiplayer_lobby_backspace(
	const MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view);
void multiplayer_lobby_build(
	MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now);

std::uint16_t deathmatch_scenario_mission(DeathmatchScenario scenario);
bool deathmatch_scenario_supports_teamplay(DeathmatchScenario scenario);
}
