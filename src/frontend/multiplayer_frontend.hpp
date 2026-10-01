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
constexpr std::uint32_t kMaxVisibleMultiplayerSessions = 8;
// The retail editor configures its generic text owner with 0x11, but that
// owner restores the pre-insert string when the post-insert cursor reaches
// 0x10.  The visible value is consequently fifteen characters plus NUL.
constexpr std::uint32_t kMultiplayerIpAddressBytes = 16;

// LANCER.EXE 0x00433a87 dispatches these six choices in this exact visual
// order.  The values intentionally match the retail button indices.
enum class MultiplayerProvider : std::uint8_t
{
	local_network = 0,
	gaming_zone = 1,
	modem = 2,
	ipx = 3,
	internet = 4,
	serial = 5,
};

enum class MultiplayerGameType : std::uint8_t
{
	cooperative,
	deathmatch,
};

struct MultiplayerSessionId
{
	std::uint8_t bytes[16]{};
};

struct MultiplayerSession
{
	MultiplayerSessionId id;
	const char* name{};
	std::uint16_t mission{};
	std::uint16_t player_count{};
	MultiplayerGameType game_type{MultiplayerGameType::cooperative};
};

// Discovery and transport ownership stay outside the frontend.  A provider
// adapter can replace this view atomically whenever its session enumeration
// changes; the frontend retains only an opaque retail-sized session ID.
struct MultiplayerSessionView
{
	const MultiplayerSession* sessions{};
	std::uint32_t count{};
};

enum class MultiplayerScreen : std::uint8_t
{
	provider_selector,
	session_browser,
	direct_ip,
};

enum class MultiplayerActionType : std::uint8_t
{
	none,
	selection_changed,
	choose_provider,
	launch_gaming_zone,
	join_session,
	host_cooperative,
	host_deathmatch,
	find_ip_games,
	enter_ip_address,
	focus_ip_address,
	main_menu,
	quit,
};

struct MultiplayerAction
{
	MultiplayerActionType type{MultiplayerActionType::none};
	MultiplayerProvider provider{MultiplayerProvider::local_network};
	MultiplayerSessionId session;
	char ip_address[kMultiplayerIpAddressBytes]{};
};

struct MultiplayerFrontend
{
	MultiplayerScreen screen{MultiplayerScreen::provider_selector};
	MultiplayerProvider provider{MultiplayerProvider::local_network};
	float pointer_x{320.0f};
	float pointer_y{200.0f};
	std::int8_t hovered_provider{-1};
	std::int8_t hovered_footer{-1};
	std::int8_t hovered_action{-1};
	std::int8_t hovered_session{-1};
	std::int8_t hovered_direct_ip{-1};
	std::int16_t tooltip_text{-1};
	std::uint64_t entered_at{};
	std::uint64_t last_input_at{};
	std::uint64_t tooltip_show_at{};
	std::uint64_t tooltip_last_visible_at{};
	std::uint64_t ip_caret_toggle_at{};
	MultiplayerSessionId selected_session;
	char ip_address[kMultiplayerIpAddressBytes]{};
	bool provider_selected{};
	bool session_selected{};
	bool ip_address_focused{};
	bool ip_caret_visible{};
};

inline constexpr const char* kMultiplayerEnterMovie =
	"interface/main2mul.bik";
inline constexpr const char* kMultiplayerLobbyMovie =
	"interface/mulfade.bik";
inline constexpr const char* kMultiplayerLobbyAlternateMovie =
	"interface/mulfade2.bik";
inline constexpr const char* kMultiplayerLobbyBackground =
	"interface/mulfade.tga";
inline constexpr const char* kMultiplayerBackMovie =
	"interface/mul2main.bik";

void multiplayer_frontend_enter(
	MultiplayerFrontend& frontend,
	std::uint64_t now);
void multiplayer_frontend_open_browser(
	MultiplayerFrontend& frontend,
	MultiplayerProvider provider);
void multiplayer_frontend_open_direct_ip(
	MultiplayerFrontend& frontend,
	MultiplayerProvider provider,
	std::uint64_t now);
void multiplayer_frontend_set_ip_address(
	MultiplayerFrontend& frontend,
	const char* address);
void multiplayer_frontend_focus_ip_address(
	MultiplayerFrontend& frontend,
	bool focused,
	std::uint64_t now);
bool multiplayer_frontend_direct_ip_text(
	MultiplayerFrontend& frontend,
	const char* text);
bool multiplayer_frontend_direct_ip_backspace(
	MultiplayerFrontend& frontend);
void multiplayer_frontend_set_pointer(
	MultiplayerFrontend& frontend,
	const MultiplayerSessionView& sessions,
	float x,
	float y,
	bool inside,
	std::uint64_t now);
MultiplayerAction multiplayer_frontend_select(
	MultiplayerFrontend& frontend,
	const MultiplayerSessionView& sessions);
void multiplayer_frontend_build(
	MultiplayerFrontend& frontend,
	const MultiplayerSessionView& sessions,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now);
}
