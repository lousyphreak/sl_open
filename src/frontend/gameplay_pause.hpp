#pragma once

#include <cstddef>
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
constexpr std::size_t kGameplayPausePlayerCapacity = 8;
constexpr std::size_t kGameplayPauseTeamCapacity = 4;
constexpr std::size_t kGameplayPauseMessageCapacity = 4;

enum class GameplayPauseAction : std::uint8_t
{
	none,
	leave_mission,
	restart,
	continue_mission,
	audio,
	controls,
	video,
	acknowledge_termination,
	reject_player,
};

enum class GameplayPauseMode : std::uint8_t
{
	offline,
	online,
	network_termination,
};

// Rendering data owned by the live mission/network state. Keeping this view
// free of transport and mission-runtime pointers lets the online-pause layer
// preserve the retail topology-slot identity while App performs the modern
// gameplay-to-lobby mapping.
struct GameplayPausePlayer
{
	const char* name{};
	std::int32_t kills{};
	std::int32_t deaths{};
	// Retail's smoothed one-way link age, in 10 ms network ticks.
	std::uint32_t latency{};
	std::int32_t team{-1};
	std::int32_t scenario_value{-1};
	std::uint16_t scenario_shape{UINT16_MAX};
	bool connected{};
	// Latched for this pause after latency first exceeds 100.
	bool rejectable{};
};

struct GameplayPauseView
{
	GameplayPausePlayer players[kGameplayPausePlayerCapacity]{};
	std::int32_t team_score[kGameplayPauseTeamCapacity]{};
	std::int32_t team_deaths[kGameplayPauseTeamCapacity]{};
	// Oldest first, matching retail's four-entry message queue.
	const char* recent_messages[kGameplayPauseMessageCapacity]{};
	const char* chat_text{};
	std::uint8_t recent_message_count{};
	// Player fields use gameplay topology-slot identity.
	std::uint8_t local_player{UINT8_MAX};
	std::uint8_t pause_owner{UINT8_MAX};
	// Zero is player-requested; one is automatic bad-link pause.
	std::uint8_t pause_reason{};
	std::int8_t configured_team_count{-1};
	bool deathmatch_mode{};
	bool team_mode{};
	bool chat_active{};
	bool online_overlay{};
};

struct GameplayPause
{
	float pointer_x{};
	float pointer_y{};
	std::int8_t hovered{-1};
	std::uint8_t hovered_reject_player{UINT8_MAX};
	std::uint64_t entered_at{};
	GameplayPauseMode mode{GameplayPauseMode::offline};
};

struct GameplayPauseSelection
{
	GameplayPauseAction action{GameplayPauseAction::none};
	std::uint8_t player{UINT8_MAX};
};

void gameplay_pause_reset(
	GameplayPause& pause,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now,
	bool online = false);
void gameplay_pause_reset_network_termination(
	GameplayPause& pause,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now);
void gameplay_pause_set_pointer(
	GameplayPause& pause,
	const render::FrontendRenderer& renderer,
	float x,
	float y,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	bool inside);
void gameplay_pause_set_pointer(
	GameplayPause& pause,
	const GameplayPauseView& view,
	const render::FrontendRenderer& renderer,
	float x,
	float y,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	bool inside);
GameplayPauseAction gameplay_pause_select(const GameplayPause& pause);
GameplayPauseSelection gameplay_pause_select(
	const GameplayPause& pause,
	const GameplayPauseView& view);
void gameplay_pause_build(
	const GameplayPause& pause,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now);
void gameplay_pause_build(
	const GameplayPause& pause,
	const GameplayPauseView& view,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now);
}
