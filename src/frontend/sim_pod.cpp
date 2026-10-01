#include "frontend/sim_pod.hpp"

#include "frontend/gui_render.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

namespace sl_open::frontend
{
namespace
{
struct Region
{
	std::int16_t x;
	std::int16_t y;
	std::int16_t width;
	std::int16_t height;
};

struct Entry
{
	Region region;
	std::uint8_t shape;
	std::uint8_t next_page;
	std::uint16_t label;
	std::uint8_t mission;
};

constexpr Entry kMainPage[] = {
	{{265, 164, 111, 111}, 4, 1, 0x101, 0},
	{{265, 328, 111, 111}, 5, 5, 0x102, 29},
	{{546, 18, 80, 80}, 7, 1, 0x104, 0},
};

constexpr Entry kTrainingPage[] = {
	{{100, 164, 111, 111}, 2, 2, 0x106, 31},
	{{265, 164, 111, 111}, 1, 3, 0x105, 30},
	{{265, 328, 111, 111}, 3, 4, 0x107, 32},
	{{546, 18, 80, 80}, 6, 1, 0x104, 0},
	{{546, 114, 80, 80}, 8, 5, 0x102, 29},
};

const Entry* entries(const SimPod& pod, std::uint8_t& count)
{
	if (pod.page == 1)
	{
		count = static_cast<std::uint8_t>(
			sizeof(kTrainingPage) / sizeof(kTrainingPage[0]));
		return kTrainingPage;
	}
	count = static_cast<std::uint8_t>(
		sizeof(kMainPage) / sizeof(kMainPage[0]));
	return kMainPage;
}

}

void sim_pod_reset(SimPod& pod, bool late_campaign)
{
	pod = {};
	pod.late_campaign = late_campaign;
	pod.phase = late_campaign
		? SimPodPhase::controls_up
		: SimPodPhase::active;
}

const char* sim_pod_movie(const SimPod& pod)
{
	switch (pod.phase)
	{
	case SimPodPhase::controls_up:
		return "inter/simpod/hud_controls_up_.bik";
	case SimPodPhase::training_transition:
		return "inter/simpod/training.bik";
	case SimPodPhase::controls_down:
		return "inter/simpod/hud_controls_down.bik";
	default:
		return nullptr;
	}
}

void sim_pod_movie_finished(SimPod& pod)
{
	if (pod.phase == SimPodPhase::controls_up)
	{
		pod.phase = SimPodPhase::active;
	}
	else if (pod.phase == SimPodPhase::training_transition)
	{
		pod.page = 1;
		pod.phase = SimPodPhase::active;
	}
	else if (pod.phase == SimPodPhase::controls_down)
	{
		pod.phase = SimPodPhase::complete;
	}
}

void sim_pod_set_pointer(
	SimPod& pod,
	float x,
	float y,
	bool inside)
{
	pod.pointer_x = x;
	pod.pointer_y = y;
	pod.hovered = -1;
	if (!inside || pod.phase != SimPodPhase::active)
	{
		return;
	}
	std::uint8_t count = 0;
	const Entry* page_entries = entries(pod, count);
	for (std::uint8_t index = 0; index < count; ++index)
	{
		if (gui::hit_open(page_entries[index].region, x, y))
		{
			pod.hovered = static_cast<std::int8_t>(index);
			break;
		}
	}
}

SimPodResult sim_pod_select(SimPod& pod)
{
	if (pod.phase != SimPodPhase::active || pod.hovered < 0)
	{
		return {};
	}
	std::uint8_t count = 0;
	const Entry* page_entries = entries(pod, count);
	const std::uint8_t selection = static_cast<std::uint8_t>(pod.hovered);
	if (selection >= count)
	{
		return {};
	}
	const Entry& entry = page_entries[selection];
	if ((pod.page == 0 && selection == 2)
		|| (pod.page == 1 && selection == 3))
	{
		sim_pod_begin_exit(pod);
		return {SimPodAction::exit, 0};
	}
	if (entry.mission != 0)
	{
		return {SimPodAction::launch_mission, entry.mission};
	}
	if (pod.page == 0 && entry.next_page == 1)
	{
		pod.phase = SimPodPhase::training_transition;
	}
	else
	{
		pod.page = entry.next_page;
	}
	pod.hovered = -1;
	return {SimPodAction::changed_page, 0};
}

bool sim_pod_begin_exit(SimPod& pod)
{
	if (pod.phase != SimPodPhase::active)
	{
		return false;
	}
	pod.phase = pod.late_campaign
		? SimPodPhase::controls_down
		: SimPodPhase::complete;
	pod.hovered = -1;
	return true;
}

void sim_pod_build(
	const SimPod& pod,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	render::frontend_rgba_quad(
		commands,
		renderer.sim_pod.backgrounds[pod.page == 1 ? 1 : 0],
		0.0f,
		0.0f,
		640.0f,
		480.0f);
	if (pod.hovered >= 0)
	{
		std::uint8_t count = 0;
		const Entry* page_entries = entries(pod, count);
		const std::uint8_t selected = static_cast<std::uint8_t>(pod.hovered);
		if (selected < count)
		{
			const Entry& entry = page_entries[selected];
			render::frontend_indexed_quad(
				commands,
				renderer.sim_pod.shapes[entry.shape - 1],
				renderer.sim_pod.palette,
				static_cast<float>(entry.region.x + 1),
				static_cast<float>(entry.region.y + 1));
			const char* label = language_text(language, entry.label);
			constexpr float scale = 0.62f;
			render::frontend_itac_text(
				commands,
				renderer,
				label,
				gui::aligned_x(
					320.0f,
					gui::text_width(
						renderer.itac.glyphs,
						renderer.itac.glyph_count,
						label,
						scale),
					gui::TextAlign::center),
				440.0f,
				renderer.shell.font_white_palette,
				0xffffffff,
				scale);
		}
	}
	const char* heading = language_text(
		language, pod.page == 1 ? 0x101 : 0x3c6);
	render::frontend_itac_text(
		commands,
		renderer,
		heading,
		42.0f,
		85.0f,
		renderer.shell.font_white_palette,
		pod.page == 1 ? 0x00e200ff : 0xc18415ff,
		0.62f);
	gui::animated_cursor(
		commands,
		renderer.shell.cursor,
		renderer.shell.cursor_palette,
		pod.pointer_x,
		pod.pointer_y,
		now,
		60);
}
}
