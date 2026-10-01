#include "frontend/mission_briefing.hpp"

#include "frontend/gui_render.hpp"
#include "localization/language.hpp"
#include "media/bink_player.hpp"
#include "render/frontend_renderer.hpp"

#include <algorithm>
#include <cstdio>

namespace sl_open::frontend
{
namespace
{
constexpr const char* kMissionMovies[] = {
	"new_m01.bik", "new_m02.bik", "new_m03.bik", "new_m04.bik",
	"new_m05.bik", "new_m06.bik", "new_m07.bik", "new_m08.bik",
	"new_m09.bik", "new_m10.bik", "new_m11.bik", "new_m01.bik",
	"new_m01.bik", "new_m14.bik", "new_m15.bik", "new_m16.bik",
	"new_m01.bik", "new_m18.bik", "new_m19.bik", "new_m20.bik",
	"new_m21.bik", "new_m01.bik", "new_m23.bik", "new_m24.bik",
	"new_m25.bik", "new_m26.bik", "new_m27.bik", "new_m28.bik",
};

constexpr std::uint16_t kEarlyOffsets[] = {
	0, 99, 0, 0, 0, 0, 137, 0, 0, 61, 0, 0};
constexpr std::uint16_t kEarlyDurations[] = {
	60, 37, 60, 60, 60, 60, 42, 60, 60, 37, 60, 60};
constexpr std::uint16_t kLateOffsets[] = {
	0, 85, 0, 0, 0, 0, 123, 0, 0, 47, 0, 0};
constexpr std::uint16_t kLateDurations[] = {
	46, 37, 46, 46, 46, 46, 42, 46, 46, 37, 46, 46};
constexpr std::uint64_t kAnimationFrameMilliseconds = 67;

void build_room(
	const MissionBriefing& briefing,
	const render::FrontendRenderer& renderer,
	const media::BinkMovie& movie,
	render::FrontendCommands& commands,
	bool exit_room)
{
	const std::uint8_t side = briefing.late_campaign ? 1 : 0;
	const render::FrontendTexture* shapes =
		briefing.late_campaign
			? (exit_room
				? renderer.briefing.late_exit
				: renderer.briefing.late)
			: (exit_room
				? renderer.briefing.early_exit
				: renderer.briefing.early);
	render::frontend_indexed_quad(
		commands,
		shapes[0],
		renderer.briefing.palettes[side],
		0.0f,
		0.0f);

	std::uint32_t animated_shape = 2;
	if (exit_room)
	{
		animated_shape = std::min<std::uint32_t>(
			2 + briefing.animation_frame,
			render::kBriefingExitShapeCount - 1);
	}
	else
	{
		const std::uint16_t* offsets =
			briefing.late_campaign ? kLateOffsets : kEarlyOffsets;
		animated_shape =
			offsets[briefing.animation_phase] + 2 + briefing.animation_frame;
	}
	render::frontend_indexed_quad(
		commands,
		shapes[animated_shape],
		renderer.briefing.palettes[side],
		briefing.late_campaign ? 1.0f : 469.0f,
		briefing.late_campaign ? 122.0f : 116.0f);

	if (!exit_room)
	{
		media::bink_movie_append(
			movie,
			commands,
			briefing.late_campaign ? 200.0f : 97.0f,
			briefing.late_campaign ? 42.0f : 29.0f);
	}
}
}

void mission_briefing_reset(
	MissionBriefing& briefing,
	std::uint8_t mission,
	std::uint64_t now)
{
	briefing = {};
	briefing.mission = std::clamp<std::uint8_t>(mission, 1, 29);
	briefing.late_campaign = briefing.mission >= 19;
	briefing.entered_at = now;
	briefing.animation_at = now;
}

bool mission_briefing_wait_finished(
	MissionBriefing& briefing,
	std::uint64_t now)
{
	if (briefing.phase != MissionBriefingPhase::awaiting_clearance
		|| now - briefing.entered_at < 250)
	{
		return false;
	}
	briefing.phase = briefing.late_campaign
		? MissionBriefingPhase::late_amon
		: MissionBriefingPhase::room_entry;
	briefing.entered_at = now;
	return true;
}

const char* mission_briefing_movie(const MissionBriefing& briefing)
{
	switch (briefing.phase)
	{
	case MissionBriefingPhase::late_amon:
		return "amonoff_.bik";
	case MissionBriefingPhase::room_entry:
		return briefing.late_campaign
			? "briefing room 350.bik"
			: "rel_c2bre.bik";
	case MissionBriefingPhase::mission_video:
		return briefing.mission == 29
			? nullptr
			: kMissionMovies[briefing.mission - 1];
	case MissionBriefingPhase::to_hologram:
		return briefing.late_campaign ? "br2hol.bik" : "rel_br2holo.bik";
	case MissionBriefingPhase::from_hologram:
		return briefing.late_campaign ? "hol2br.bik" : "rel_holo2br.bik";
	default:
		return nullptr;
	}
}

bool mission_briefing_movie_finished(
	MissionBriefing& briefing,
	std::uint64_t now)
{
	switch (briefing.phase)
	{
	case MissionBriefingPhase::late_amon:
		briefing.phase = MissionBriefingPhase::room_entry;
		break;
	case MissionBriefingPhase::room_entry:
		briefing.phase = MissionBriefingPhase::mission_video;
		break;
	case MissionBriefingPhase::mission_video:
		briefing.phase = briefing.mission == 29
			? MissionBriefingPhase::complete
			: MissionBriefingPhase::to_hologram;
		break;
	case MissionBriefingPhase::to_hologram:
		briefing.phase = MissionBriefingPhase::hologram;
		break;
	case MissionBriefingPhase::from_hologram:
		briefing.phase = MissionBriefingPhase::exit_room;
		break;
	default:
		return false;
	}
	briefing.entered_at = now;
	briefing.animation_at = now;
	briefing.animation_phase = 0;
	briefing.animation_frame = 0;
	return true;
}

bool mission_briefing_begin_hologram_exit(
	MissionBriefing& briefing,
	std::uint64_t now)
{
	if (briefing.phase != MissionBriefingPhase::hologram)
	{
		return false;
	}
	briefing.phase = MissionBriefingPhase::from_hologram;
	briefing.entered_at = now;
	return true;
}

bool mission_briefing_exit_finished(
	MissionBriefing& briefing,
	std::uint64_t)
{
	if (briefing.phase != MissionBriefingPhase::exit_room
		|| briefing.animation_frame < 60)
	{
		return false;
	}
	briefing.phase = MissionBriefingPhase::complete;
	return true;
}

bool mission_briefing_trigger_exit_speech(MissionBriefing& briefing)
{
	if (briefing.phase != MissionBriefingPhase::exit_room
		|| briefing.animation_frame < 17
		|| briefing.exit_speech_triggered)
	{
		return false;
	}
	briefing.exit_speech_triggered = true;
	return true;
}

void mission_briefing_pointer(
	MissionBriefing& briefing,
	float x,
	float y)
{
	briefing.pointer_x = x;
	briefing.pointer_y = y;
}

void mission_briefing_update_animation(
	MissionBriefing& briefing,
	std::uint64_t now)
{
	if (briefing.phase != MissionBriefingPhase::mission_video
		&& briefing.phase != MissionBriefingPhase::exit_room)
	{
		return;
	}
	while (now - briefing.animation_at >= kAnimationFrameMilliseconds)
	{
		briefing.animation_at += kAnimationFrameMilliseconds;
		if (briefing.phase == MissionBriefingPhase::exit_room)
		{
			if (briefing.animation_frame < 60)
			{
				++briefing.animation_frame;
			}
			continue;
		}
		const std::uint16_t* durations =
			briefing.late_campaign ? kLateDurations : kEarlyDurations;
		if (briefing.animation_frame++ == durations[briefing.animation_phase])
		{
			briefing.animation_frame = 0;
			briefing.animation_phase =
				static_cast<std::uint8_t>((briefing.animation_phase + 1) % 12);
		}
	}
}

void mission_briefing_build(
	const MissionBriefing& briefing,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	const media::BinkMovie& movie,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	gui::begin_screen(commands);
	if (briefing.phase == MissionBriefingPhase::awaiting_clearance)
	{
		render::frontend_rgba_quad(
			commands,
			renderer.briefing.doors[briefing.late_campaign ? 1 : 0],
			0.0f,
			0.0f,
			640.0f,
			480.0f);
		char message[96];
		std::snprintf(
			message,
			sizeof(message),
			"%s %s",
			language_text(language, 0x292),
			language_text(language, 0x293));
		render::frontend_text(
			commands,
			renderer,
			message,
			440.0f,
			320.0f,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);
		return;
	}
	if (briefing.phase == MissionBriefingPhase::mission_video)
	{
		build_room(briefing, renderer, movie, commands, false);
	}
	else if (briefing.phase == MissionBriefingPhase::exit_room)
	{
		build_room(briefing, renderer, movie, commands, true);
	}
	else
	{
		media::bink_movie_append(movie, commands, 0.0f, 0.0f);
	}
	if (briefing.phase == MissionBriefingPhase::hologram)
	{
		gui::animated_cursor(
			commands,
			renderer.shell.cursor,
			renderer.shell.cursor_palette,
			briefing.pointer_x,
			briefing.pointer_y,
			now,
			60);
	}
}
}
