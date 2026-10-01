#pragma once

#include <cstdint>

namespace sl_open
{
struct LanguageTable;
}

namespace sl_open::campaign
{
struct CampaignState;
}

namespace sl_open::render
{
struct FrontendCommands;
struct FrontendRenderer;
}

namespace sl_open::frontend
{
enum class MedalPhase : std::uint8_t
{
	zoom,
	opening,
	waiting,
	closing,
	complete,
};

struct MedalDisplay
{
	MedalPhase phase{MedalPhase::opening};
	std::int8_t hovered{-1};
	float pointer_x{320.0f};
	float pointer_y{240.0f};
	bool late_campaign{};
};

void medal_display_reset(MedalDisplay& display, bool late_campaign);
const char* medal_display_movie(const MedalDisplay& display);
bool medal_display_movie_finished(MedalDisplay& display);
bool medal_display_begin_close(MedalDisplay& display);
void medal_display_set_pointer(
	MedalDisplay& display,
	float x,
	float y,
	bool inside);
void medal_display_build(
	const MedalDisplay& display,
	const campaign::CampaignState& campaign,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint32_t movie_frame,
	std::uint64_t now);
}
