#include "frontend/credits.hpp"

#include "frontend/gui_render.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace sl_open::frontend
{
namespace
{
struct CreditLine
{
	std::uint16_t string_id;
	std::int16_t x;
	std::int16_t y;
	std::int16_t white;
};

struct CreditPage
{
	std::uint16_t first_line;
	std::uint16_t line_count;
	std::uint8_t palette_shape;
	std::uint8_t image_shape;
};

#include "frontend/credits_data.inc"

constexpr float kFadeStep = 0.03f;
// The original schedule uses 10 ms frontend-clock ticks: 750 ticks for each
// page followed by 150 ticks for its fade-out.
constexpr std::uint64_t kPageDurationMs = 7500;
constexpr std::uint64_t kFadeOutDurationMs = 1500;
}

void credits_reset(Credits& credits, std::uint64_t now)
{
	credits = {};
	credits.deadline = now + kPageDurationMs;
}

bool credits_update(Credits& credits, std::uint64_t now)
{
	if (credits.fade_state >= 16)
	{
		return false;
	}
	if ((credits.fade_state & 1u) == 0)
	{
		if (credits.deadline < now)
		{
			credits.deadline = now + kFadeOutDurationMs;
			++credits.fade_state;
		}
		else
		{
			credits.alpha = std::min(1.0f, credits.alpha + kFadeStep);
		}
	}
	else if (credits.deadline < now)
	{
		credits.deadline = now + kPageDurationMs;
		++credits.fade_state;
		credits.page = static_cast<std::uint8_t>(
			(credits.page + 1) % std::size(kCreditPages));
	}
	else
	{
		credits.alpha = std::max(0.0f, credits.alpha - kFadeStep);
	}
	return credits.fade_state < 16;
}

void credits_build(
	const Credits& credits,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	gui::begin_screen(commands);
	const CreditPage& page = kCreditPages[credits.page];
	const std::uint8_t alpha = static_cast<std::uint8_t>(
		std::clamp(credits.alpha, 0.0f, 1.0f) * 255.0f);
	const std::uint32_t tint = 0xffffff00u | alpha;
	const std::uint8_t visual = page.palette_shape / 2;
	render::frontend_indexed_quad(
		commands,
		renderer.credits.backgrounds[visual],
		renderer.credits.background_palettes[visual],
		0.0f,
		0.0f,
		tint);

	for (std::uint16_t index = 0; index < page.line_count; ++index)
	{
		const CreditLine& line = kCreditLines[page.first_line + index];
		const char* text = language_text(language, line.string_id);
		float x = static_cast<float>(line.x);
		if (line.x == 0)
		{
			x = 320.0f
				- render::frontend_credits_text_width(renderer, text) * 0.5f;
		}
		render::frontend_credits_text(
			commands,
			renderer,
			text,
			x,
			static_cast<float>(line.y),
			line.white
				? renderer.credits.white_palette
				: renderer.credits.orange_palette,
			tint);
	}
}
}
