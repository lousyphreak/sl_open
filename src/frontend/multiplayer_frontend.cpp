#include "frontend/multiplayer_frontend.hpp"

#include "frontend/gui_render.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace sl_open::frontend
{
namespace
{
// FUN_00432fc0 lays these eight fixed hit records out contiguously.  The
// strict-edge comparison is FUN_0043eb30.
constexpr gui::Rect kProviderRegions[] = {
	{62.0f, 233.0f, 28.0f, 20.0f},
	{251.0f, 233.0f, 28.0f, 20.0f},
	{444.0f, 233.0f, 28.0f, 20.0f},
	{62.0f, 255.0f, 28.0f, 20.0f},
	{251.0f, 255.0f, 28.0f, 20.0f},
	{444.0f, 255.0f, 28.0f, 20.0f},
};
constexpr gui::Rect kFooterRegions[] = {
	{292.0f, 441.0f, 25.0f, 16.0f},
	{324.0f, 441.0f, 25.0f, 16.0f},
};
constexpr gui::Rect kBrowserActionRegions[] = {
	{455.0f, 290.0f, 178.0f, 20.0f},
	{455.0f, 339.0f, 178.0f, 20.0f},
	{455.0f, 389.0f, 178.0f, 20.0f},
	{455.0f, 439.0f, 178.0f, 20.0f},
};
// The direct TCP/IP owner uses a different four-record set.  Its action
// indices are FIND GAMES, cooperative host, deathmatch host, then editor
// focus; FUN_00432fc0 dispatches those indices in that order.
constexpr gui::Rect kDirectIpRegions[] = {
	{372.0f, 305.0f, 28.0f, 20.0f},
	{258.0f, 339.0f, 28.0f, 20.0f},
	{258.0f, 373.0f, 28.0f, 20.0f},
	{198.0f, 305.0f, 169.0f, 21.0f},
};

constexpr std::uint16_t kProviderTooltip[] = {
	0x580, 0x582, 0x584, 0x581, 0x583, 0x585,
};
constexpr std::uint16_t kActionTooltip[] = {
	0x586, 0x587, 0x588, 0x5a5,
};
constexpr std::uint16_t kDirectIpTooltip[] = {
	0x58a, 0x587, 0x588, 0x589,
};

constexpr float kProviderX[] = {
	62.0f, 251.0f, 444.0f, 62.0f, 251.0f, 444.0f,
};
constexpr float kProviderY[] = {
	233.0f, 233.0f, 233.0f, 255.0f, 255.0f, 255.0f,
};
constexpr float kActionY[] = {292.0f, 339.0f, 389.0f, 439.0f};
constexpr std::uint16_t kActionLabels[][2] = {
	{0x5ac, 0x5ad},
	{0x13c, 0x13d},
	{0x13c, 0x13e},
	{0x171, 0xffff},
};
constexpr std::uint64_t kIpCaretToggleMs = 250;

bool session_id_equal(
	const MultiplayerSessionId& left,
	const MultiplayerSessionId& right)
{
	return std::memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

std::uint32_t visible_session_count(const MultiplayerSessionView& view)
{
	if (view.sessions == nullptr)
	{
		return 0;
	}
	return std::min(view.count, kMaxVisibleMultiplayerSessions);
}

const MultiplayerSession* selected_session(
	const MultiplayerFrontend& frontend,
	const MultiplayerSessionView& view)
{
	if (!frontend.session_selected || view.sessions == nullptr)
	{
		return nullptr;
	}
	for (std::uint32_t index = 0;
		index < visible_session_count(view);
		++index)
	{
		if (session_id_equal(
			frontend.selected_session, view.sessions[index].id))
		{
			return &view.sessions[index];
		}
	}
	return nullptr;
}

void draw_aligned_text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	gui::TextAlign alignment,
	bgfx::TextureHandle palette)
{
	x = gui::aligned_x(
		x,
		gui::text_width(
			renderer.shell.glyphs,
			renderer.shell.glyph_count,
			text),
		alignment);
	render::frontend_text(
		commands, renderer, text, x, y, palette, 0xffffffff);
}

void draw_frame(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	float x,
	float y,
	float width,
	float height)
{
	// FUN_00435c60 draws the retail three-tone one-pixel bevel.  Its RGB
	// factors at 0x004dc6c4..0x004dc6d0 are reproduced here at alpha 1.
	constexpr std::uint32_t bright = 0x00a7ffff;
	constexpr std::uint32_t middle = 0x008586ff;
	constexpr std::uint32_t dark = 0x0057cdff;
	render::frontend_rgba_quad(
		commands, renderer.white, x, y, width, 1.0f, bright);
	render::frontend_rgba_quad(
		commands, renderer.white, x, y, 1.0f, height, bright);
	render::frontend_rgba_quad(
		commands, renderer.white, x + 1.0f, y + 1.0f,
		width - 2.0f, 1.0f, middle);
	render::frontend_rgba_quad(
		commands, renderer.white, x + 1.0f, y + 1.0f,
		1.0f, height - 2.0f, middle);
	render::frontend_rgba_quad(
		commands, renderer.white, x, y + height - 1.0f,
		width, 1.0f, dark);
	render::frontend_rgba_quad(
		commands, renderer.white, x + width - 1.0f, y,
		1.0f, height, dark);
}

void reset_ip_caret(
	MultiplayerFrontend& frontend,
	std::uint64_t now)
{
	frontend.ip_caret_visible = true;
	frontend.ip_caret_toggle_at = now + kIpCaretToggleMs;
}

void update_ip_caret(
	MultiplayerFrontend& frontend,
	std::uint64_t now)
{
	// Retail compares a deadline against its 100 Hz timer, resets the
	// deadline to "now + 25", and flips one bit.  It deliberately does not
	// replay missed intervals after a stalled frame.
	if (frontend.ip_address_focused
		&& frontend.ip_caret_toggle_at < now)
	{
		frontend.ip_caret_visible = !frontend.ip_caret_visible;
		frontend.ip_caret_toggle_at = now + kIpCaretToggleMs;
	}
}

void draw_tooltip(
	MultiplayerFrontend& frontend,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	if (frontend.tooltip_text < 0
		|| now < frontend.tooltip_show_at)
	{
		return;
	}
	const char* text = language_text(
		language, static_cast<std::uint16_t>(frontend.tooltip_text));
	const float width = gui::text_width(
		renderer.shell.glyphs,
		renderer.shell.glyph_count,
		text);
	float x = frontend.pointer_x;
	float y = frontend.pointer_y + 38.0f;
	if (y > 450.0f)
	{
		y = frontend.pointer_y - 19.0f;
	}
	x = std::min(x, 634.0f - width);
	// FUN_00440d80 passes the raw retail B5G6R5 value 100 to the outline
	// rasterizer.  Expanded to RGBA8, that is (0, 12, 33, 255).
	constexpr std::uint32_t tooltip_border = 0x000c21ff;
	render::frontend_rgba_quad(
		commands,
		renderer.white,
		x - 1.0f,
		y - 3.0f,
		width + 7.0f,
		18.0f,
		0x000000ff);
	render::frontend_rgba_quad(
		commands, renderer.white, x - 1.0f, y - 3.0f,
		width + 7.0f, 1.0f, tooltip_border);
	render::frontend_rgba_quad(
		commands, renderer.white, x - 1.0f, y + 15.0f,
		width + 7.0f, 1.0f, tooltip_border);
	render::frontend_rgba_quad(
		commands, renderer.white, x - 1.0f, y - 3.0f,
		1.0f, 19.0f, tooltip_border);
	render::frontend_rgba_quad(
		commands, renderer.white, x + width + 5.0f, y - 3.0f,
		1.0f, 19.0f, tooltip_border);
	render::frontend_text(
		commands,
		renderer,
		text,
		x + 2.0f,
		y - 2.0f,
		renderer.shell.font_white_palette,
		0xffffffff);
	frontend.tooltip_last_visible_at = now;
}

MultiplayerAction make_action(
	MultiplayerActionType type,
	MultiplayerProvider provider)
{
	MultiplayerAction result;
	result.type = type;
	result.provider = provider;
	return result;
}

MultiplayerAction make_ip_action(
	MultiplayerActionType type,
	const MultiplayerFrontend& frontend)
{
	MultiplayerAction result = make_action(type, frontend.provider);
	std::snprintf(
		result.ip_address,
		sizeof(result.ip_address),
		"%s",
		frontend.ip_address);
	return result;
}
}

void multiplayer_frontend_enter(
	MultiplayerFrontend& frontend,
	std::uint64_t now)
{
	frontend = {};
	frontend.pointer_x = 320.0f;
	frontend.pointer_y = 200.0f;
	frontend.entered_at = now;
	frontend.last_input_at = now;
	frontend.tooltip_show_at = now;
	frontend.hovered_provider = -1;
	frontend.hovered_footer = -1;
	frontend.hovered_action = -1;
	frontend.hovered_session = -1;
	frontend.hovered_direct_ip = -1;
	frontend.tooltip_text = -1;
}

void multiplayer_frontend_open_browser(
	MultiplayerFrontend& frontend,
	MultiplayerProvider provider)
{
	if (provider == MultiplayerProvider::internet)
	{
		multiplayer_frontend_open_direct_ip(
			frontend, provider, frontend.last_input_at);
		return;
	}
	frontend.screen = MultiplayerScreen::session_browser;
	frontend.provider = provider;
	frontend.provider_selected = true;
	frontend.hovered_action = -1;
	frontend.hovered_session = -1;
	frontend.hovered_direct_ip = -1;
	frontend.session_selected = false;
	multiplayer_frontend_focus_ip_address(
		frontend, false, frontend.last_input_at);
}

void multiplayer_frontend_open_direct_ip(
	MultiplayerFrontend& frontend,
	MultiplayerProvider provider,
	std::uint64_t now)
{
	frontend.last_input_at = now;
	frontend.screen = MultiplayerScreen::direct_ip;
	frontend.provider = provider;
	frontend.provider_selected = true;
	frontend.hovered_action = -1;
	frontend.hovered_session = -1;
	frontend.hovered_direct_ip = -1;
	frontend.session_selected = false;
	multiplayer_frontend_focus_ip_address(frontend, true, now);
}

void multiplayer_frontend_set_ip_address(
	MultiplayerFrontend& frontend,
	const char* address)
{
	char value[kMultiplayerIpAddressBytes]{};
	std::snprintf(
		value,
		sizeof(value),
		"%s",
		address == nullptr ? "" : address);
	std::memcpy(frontend.ip_address, value, sizeof(value));
}

void multiplayer_frontend_focus_ip_address(
	MultiplayerFrontend& frontend,
	bool focused,
	std::uint64_t now)
{
	frontend.ip_address_focused =
		focused && frontend.screen == MultiplayerScreen::direct_ip;
	if (frontend.ip_address_focused)
	{
		reset_ip_caret(frontend, now);
	}
	else
	{
		frontend.ip_caret_visible = false;
		frontend.ip_caret_toggle_at = 0;
	}
}

bool multiplayer_frontend_direct_ip_text(
	MultiplayerFrontend& frontend,
	const char* text)
{
	if (frontend.screen != MultiplayerScreen::direct_ip
		|| !frontend.ip_address_focused
		|| text == nullptr)
	{
		return false;
	}
	std::size_t length = std::strlen(frontend.ip_address);
	const std::size_t original_length = length;
	for (const unsigned char* it =
			reinterpret_cast<const unsigned char*>(text);
		*it != 0 && length + 1 < sizeof(frontend.ip_address);
		++it)
	{
		// FUN_004812f0 consumes the retail keyboard queue as single-byte
		// text. SDL supplies UTF-8, so retain only its printable ASCII
		// subset rather than inserting partial multibyte sequences.
		if (*it >= 0x20 && *it < 0x7f)
		{
			frontend.ip_address[length++] = static_cast<char>(*it);
		}
	}
	frontend.ip_address[length] = '\0';
	return length != original_length;
}

bool multiplayer_frontend_direct_ip_backspace(
	MultiplayerFrontend& frontend)
{
	if (frontend.screen != MultiplayerScreen::direct_ip
		|| !frontend.ip_address_focused)
	{
		return false;
	}
	const std::size_t length = std::strlen(frontend.ip_address);
	if (length == 0)
	{
		return false;
	}
	frontend.ip_address[length - 1] = '\0';
	return true;
}

void multiplayer_frontend_set_pointer(
	MultiplayerFrontend& frontend,
	const MultiplayerSessionView& sessions,
	float x,
	float y,
	bool inside,
	std::uint64_t now)
{
	frontend.pointer_x = x;
	frontend.pointer_y = y;
	frontend.last_input_at = now;
	frontend.hovered_provider = -1;
	frontend.hovered_footer = -1;
	frontend.hovered_action = -1;
	frontend.hovered_session = -1;
	frontend.hovered_direct_ip = -1;
	std::int16_t tooltip = -1;

	if (inside)
	{
		for (std::uint32_t index = 0;
			index < std::size(kProviderRegions);
			++index)
		{
			if (gui::hit_open(kProviderRegions[index], x, y))
			{
				frontend.hovered_provider =
					static_cast<std::int8_t>(index);
				tooltip = static_cast<std::int16_t>(
					kProviderTooltip[index]);
				break;
			}
		}
		for (std::uint32_t index = 0;
			frontend.hovered_provider < 0
				&& index < std::size(kFooterRegions);
			++index)
		{
			if (gui::hit_open(kFooterRegions[index], x, y))
			{
				frontend.hovered_footer =
					static_cast<std::int8_t>(index);
				break;
			}
		}
		if (frontend.screen == MultiplayerScreen::session_browser)
		{
			const std::uint32_t action_count =
				frontend.provider == MultiplayerProvider::local_network
					? 4u
					: 3u;
			for (std::uint32_t index = 0;
				frontend.hovered_provider < 0
					&& frontend.hovered_footer < 0
					&& index < action_count;
				++index)
			{
				if (gui::hit_open(kBrowserActionRegions[index], x, y))
				{
					frontend.hovered_action =
						static_cast<std::int8_t>(index);
					tooltip = static_cast<std::int16_t>(
						kActionTooltip[index]);
					break;
				}
			}
			for (std::uint32_t index = 0;
				frontend.hovered_provider < 0
					&& frontend.hovered_footer < 0
					&& frontend.hovered_action < 0
					&& index < visible_session_count(sessions);
				++index)
			{
				if (gui::hit_open(
					{66.0f, 292.0f + index * 12.0f, 353.0f, 12.0f},
					x,
					y))
				{
					frontend.hovered_session =
						static_cast<std::int8_t>(index);
					break;
				}
			}
		}
		else if (frontend.screen == MultiplayerScreen::direct_ip)
		{
			for (std::uint32_t index = 0;
				frontend.hovered_provider < 0
					&& frontend.hovered_footer < 0
					&& index < std::size(kDirectIpRegions);
				++index)
			{
				if (gui::hit_open(kDirectIpRegions[index], x, y))
				{
					frontend.hovered_direct_ip =
						static_cast<std::int8_t>(index);
					tooltip = static_cast<std::int16_t>(
						kDirectIpTooltip[index]);
					break;
				}
			}
		}
	}

	if (tooltip != frontend.tooltip_text)
	{
		frontend.tooltip_text = tooltip;
		frontend.tooltip_show_at =
			frontend.tooltip_last_visible_at + 30 < now
				? now + 100
				: now;
	}
}

MultiplayerAction multiplayer_frontend_select(
	MultiplayerFrontend& frontend,
	const MultiplayerSessionView& sessions)
{
	if (frontend.hovered_footer == 0)
	{
		return make_action(
			MultiplayerActionType::main_menu, frontend.provider);
	}
	if (frontend.hovered_footer == 1)
	{
		return make_action(
			MultiplayerActionType::quit, frontend.provider);
	}
	if (frontend.hovered_provider >= 0)
	{
		const auto provider = static_cast<MultiplayerProvider>(
			frontend.hovered_provider);
		frontend.provider = provider;
		frontend.provider_selected = true;
		frontend.screen = MultiplayerScreen::provider_selector;
		frontend.session_selected = false;
		std::memset(
			frontend.ip_address, 0, sizeof(frontend.ip_address));
		multiplayer_frontend_focus_ip_address(
			frontend, false, frontend.last_input_at);
		return make_action(
			provider == MultiplayerProvider::gaming_zone
				? MultiplayerActionType::launch_gaming_zone
				: MultiplayerActionType::choose_provider,
			provider);
	}
	if (frontend.screen == MultiplayerScreen::direct_ip)
	{
		switch (frontend.hovered_direct_ip)
		{
		case 0:
		{
			MultiplayerAction result = make_ip_action(
				MultiplayerActionType::find_ip_games, frontend);
			// DirectPlay is recreated with the typed address and the same
			// provider choice then falls through to the ordinary session
			// enumeration owner.
			frontend.screen = MultiplayerScreen::session_browser;
			frontend.hovered_direct_ip = -1;
			frontend.session_selected = false;
			multiplayer_frontend_focus_ip_address(
				frontend, false, frontend.last_input_at);
			return result;
		}
		case 1:
			return make_action(
				MultiplayerActionType::host_cooperative,
				frontend.provider);
		case 2:
			return make_action(
				MultiplayerActionType::host_deathmatch,
				frontend.provider);
		case 3:
			multiplayer_frontend_focus_ip_address(
				frontend, true, frontend.last_input_at);
			return make_action(
				MultiplayerActionType::focus_ip_address,
				frontend.provider);
		default:
			return {};
		}
	}
	if (frontend.screen != MultiplayerScreen::session_browser)
	{
		return {};
	}
	if (frontend.hovered_action == 0)
	{
		const MultiplayerSession* session =
			selected_session(frontend, sessions);
		if (session == nullptr)
		{
			return {};
		}
		MultiplayerAction result = make_action(
			MultiplayerActionType::join_session, frontend.provider);
		result.session = session->id;
		return result;
	}
	if (frontend.hovered_action == 1)
	{
		return make_action(
			MultiplayerActionType::host_cooperative, frontend.provider);
	}
	if (frontend.hovered_action == 2)
	{
		return make_action(
			MultiplayerActionType::host_deathmatch, frontend.provider);
	}
	if (frontend.hovered_action == 3
		&& frontend.provider == MultiplayerProvider::local_network)
	{
		multiplayer_frontend_open_direct_ip(
			frontend, frontend.provider, frontend.last_input_at);
		return make_action(
			MultiplayerActionType::enter_ip_address, frontend.provider);
	}
	if (frontend.hovered_session >= 0
		&& sessions.sessions != nullptr
		&& static_cast<std::uint32_t>(frontend.hovered_session)
			< visible_session_count(sessions))
	{
		const MultiplayerSession& session =
			sessions.sessions[frontend.hovered_session];
		if (frontend.session_selected
			&& session_id_equal(frontend.selected_session, session.id))
		{
			MultiplayerAction result = make_action(
				MultiplayerActionType::join_session, frontend.provider);
			result.session = session.id;
			return result;
		}
		frontend.selected_session = session.id;
		frontend.session_selected = true;
		return make_action(
			MultiplayerActionType::selection_changed, frontend.provider);
	}
	return {};
}

void multiplayer_frontend_build(
	MultiplayerFrontend& frontend,
	const MultiplayerSessionView& sessions,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	gui::begin_screen(commands);
	render::frontend_rgba_quad(
		commands,
		renderer.multiplayer.background,
		0.0f,
		0.0f,
		render::kFrontendWidth,
		render::kFrontendHeight);

	const bgfx::TextureHandle gold = renderer.shell.font_gold_palette;
	const bgfx::TextureHandle white = renderer.shell.font_white_palette;
	draw_aligned_text(
		commands, renderer, language_text(language, 0xfc),
		133.0f, 218.0f, gui::TextAlign::center, gold);
	draw_aligned_text(
		commands, renderer, language_text(language, 0xfd),
		320.0f, 218.0f, gui::TextAlign::center, gold);
	draw_aligned_text(
		commands, renderer, language_text(language, 0xfe),
		511.0f, 218.0f, gui::TextAlign::center, gold);
	draw_aligned_text(
		commands, renderer, language_text(language, 0xbb),
		286.0f, 439.0f, gui::TextAlign::right,
		frontend.hovered_footer == 0 ? white : gold);
	draw_aligned_text(
		commands, renderer, language_text(language, 0xbc),
		352.0f, 439.0f, gui::TextAlign::left,
		frontend.hovered_footer == 1 ? white : gold);

	for (std::uint32_t index = 0;
		index < std::size(kProviderRegions);
		++index)
	{
		render::frontend_indexed_quad(
			commands,
			renderer.multiplayer.provider_button,
			renderer.multiplayer.screen_palette,
			kProviderX[index],
			kProviderY[index]);
	}
	render::frontend_indexed_quad(
		commands,
		renderer.multiplayer.provider_button,
		renderer.multiplayer.screen_palette,
		292.0f,
		441.0f);
	render::frontend_indexed_quad(
		commands,
		renderer.multiplayer.provider_button,
		renderer.multiplayer.screen_palette,
		324.0f,
		441.0f);

	if (frontend.provider_selected)
	{
		const std::uint32_t selected =
			static_cast<std::uint32_t>(frontend.provider);
		render::frontend_indexed_quad(
			commands,
			renderer.multiplayer.provider_button_selected,
			renderer.multiplayer.screen_palette,
			kProviderX[selected],
			kProviderY[selected]);
	}
	if (frontend.hovered_footer >= 0)
	{
		render::frontend_indexed_quad(
			commands,
			renderer.multiplayer.provider_button_selected,
			renderer.multiplayer.screen_palette,
			frontend.hovered_footer == 0 ? 292.0f : 324.0f,
			441.0f);
	}

	if (frontend.screen == MultiplayerScreen::session_browser
		&& frontend.provider_selected)
	{
		draw_frame(
			commands, renderer, 62.0f, 294.0f, 353.0f, 120.0f);
		const std::uint32_t action_count =
			frontend.provider == MultiplayerProvider::local_network
				? 4u
				: 3u;
		for (std::uint32_t index = 0; index < action_count; ++index)
		{
			const bool hovered =
				frontend.hovered_action
					== static_cast<std::int8_t>(index);
			render::frontend_indexed_quad(
				commands,
				hovered
					? renderer.multiplayer.action_button_hover
					: renderer.multiplayer.action_button,
				renderer.multiplayer.screen_palette,
				455.0f,
				kActionY[index]);
			const bgfx::TextureHandle palette = hovered ? white : gold;
			render::frontend_text(
				commands,
				renderer,
				language_text(language, kActionLabels[index][0]),
				487.0f,
				kActionY[index] - 4.0f,
				palette);
			if (kActionLabels[index][1] != 0xffff)
			{
				render::frontend_text(
					commands,
					renderer,
					language_text(language, kActionLabels[index][1]),
					487.0f,
					kActionY[index] + 8.0f,
					palette);
			}
		}

		render::frontend_text(
			commands, renderer, language_text(language, 0x137),
			66.0f, 276.0f, gold);
		render::frontend_text(
			commands, renderer, language_text(language, 0x5a4),
			228.0f, 276.0f, gold);
		render::frontend_text(
			commands, renderer, language_text(language, 0x138),
			308.0f, 276.0f, gold);
		render::frontend_text(
			commands, renderer, language_text(language, 0x139),
			411.0f, 276.0f, gold);

		const std::uint32_t session_count =
			visible_session_count(sessions);
		if (session_count == 0)
		{
			render::frontend_text(
				commands, renderer, language_text(language, 0x13a),
				66.0f, 294.0f, gold);
		}
		else if (sessions.sessions != nullptr)
		{
			for (std::uint32_t index = 0;
				index < session_count;
				++index)
			{
				const MultiplayerSession& session =
					sessions.sessions[index];
				const bool selected = frontend.session_selected
					&& session_id_equal(
						frontend.selected_session, session.id);
				const bgfx::TextureHandle palette =
					selected ? white : gold;
				const float y = 294.0f + index * 12.0f;
				render::frontend_text(
					commands,
					renderer,
					session.name == nullptr ? "" : session.name,
					66.0f,
					y,
					palette);
				if (session.game_type == MultiplayerGameType::cooperative)
				{
					char mission[12];
					std::snprintf(
						mission,
						sizeof(mission),
						"%u",
						static_cast<unsigned>(session.mission));
					render::frontend_text(
						commands, renderer, mission,
						228.0f, y, palette);
				}
				char players[12];
				std::snprintf(
					players,
					sizeof(players),
					"%u",
					static_cast<unsigned>(session.player_count));
				render::frontend_text(
					commands, renderer, players,
					308.0f, y, palette);
				render::frontend_text(
					commands,
					renderer,
					language_text(
						language,
						session.game_type
								== MultiplayerGameType::deathmatch
							? 0x13e
							: 0x294),
					411.0f,
					y,
					palette);
			}
		}
	}
	else if (frontend.screen == MultiplayerScreen::direct_ip
		&& frontend.provider_selected)
	{
		update_ip_caret(frontend, now);
		draw_aligned_text(
			commands,
			renderer,
			language_text(language, 0x171),
			320.0f,
			285.0f,
			gui::TextAlign::center,
			gold);
		draw_frame(
			commands, renderer, 198.0f, 304.0f, 169.0f, 21.0f);

		char find_games[96]{};
		std::snprintf(
			find_games,
			sizeof(find_games),
			"%s",
			language_text(language, 0x57e));
		char* games = std::strchr(find_games, ' ');
		if (games != nullptr)
		{
			*games++ = '\0';
		}
		else
		{
			games = find_games + std::strlen(find_games);
		}
		render::frontend_text(
			commands, renderer, find_games, 404.0f, 300.0f, gold);
		render::frontend_text(
			commands, renderer, games, 404.0f, 312.0f, gold);
		render::frontend_text(
			commands,
			renderer,
			language_text(language, 0x13c),
			290.0f,
			334.0f,
			gold);
		render::frontend_text(
			commands,
			renderer,
			language_text(language, 0x13d),
			290.0f,
			346.0f,
			gold);
		render::frontend_text(
			commands,
			renderer,
			language_text(language, 0x13c),
			290.0f,
			368.0f,
			gold);
		render::frontend_text(
			commands,
			renderer,
			language_text(language, 0x13e),
			290.0f,
			380.0f,
			gold);

		constexpr float button_x[] = {372.0f, 258.0f, 258.0f};
		constexpr float button_y[] = {305.0f, 339.0f, 373.0f};
		for (std::uint32_t index = 0; index < std::size(button_x); ++index)
		{
			render::frontend_indexed_quad(
				commands,
				renderer.multiplayer.action_button,
				renderer.multiplayer.screen_palette,
				button_x[index],
				button_y[index]);
			if (frontend.hovered_direct_ip
				== static_cast<std::int8_t>(index))
			{
				render::frontend_indexed_quad(
					commands,
					renderer.multiplayer.action_button_hover,
					renderer.multiplayer.screen_palette,
					button_x[index],
					button_y[index]);
			}
		}

		const bgfx::TextureHandle address_palette =
			frontend.ip_address_focused ? white : gold;
		render::frontend_text(
			commands,
			renderer,
			frontend.ip_address,
			202.0f,
			307.0f,
			address_palette);
		if (frontend.ip_address_focused
			&& frontend.ip_caret_visible)
		{
			const float caret_x = 202.0f
				+ gui::text_width(
					renderer.shell.glyphs,
					renderer.shell.glyph_count,
					frontend.ip_address);
			render::frontend_text(
				commands,
				renderer,
				"_",
				caret_x,
				307.0f,
				address_palette);
		}
	}

	draw_tooltip(frontend, language, renderer, commands, now);
	const std::uint64_t elapsed =
		now > frontend.entered_at ? now - frontend.entered_at : 0;
	gui::animated_cursor(
		commands,
		renderer.multiplayer.cursor,
		renderer.multiplayer.cursor_palette,
		frontend.pointer_x,
		frontend.pointer_y,
		elapsed,
		40);
}
}
