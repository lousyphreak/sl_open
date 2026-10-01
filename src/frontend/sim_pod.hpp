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
enum class SimPodPhase : std::uint8_t
{
	controls_up,
	training_transition,
	active,
	controls_down,
	complete,
};

enum class SimPodAction : std::uint8_t
{
	none,
	changed_page,
	launch_mission,
	exit,
};

struct SimPodResult
{
	SimPodAction action{SimPodAction::none};
	std::uint8_t mission{};
};

struct SimPod
{
	SimPodPhase phase{SimPodPhase::active};
	std::uint8_t page{};
	std::int8_t hovered{-1};
	float pointer_x{320.0f};
	float pointer_y{240.0f};
	bool late_campaign{};
};

void sim_pod_reset(SimPod& pod, bool late_campaign);
const char* sim_pod_movie(const SimPod& pod);
void sim_pod_movie_finished(SimPod& pod);
void sim_pod_set_pointer(
	SimPod& pod,
	float x,
	float y,
	bool inside);
SimPodResult sim_pod_select(SimPod& pod);
bool sim_pod_begin_exit(SimPod& pod);
void sim_pod_build(
	const SimPod& pod,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now);
}
