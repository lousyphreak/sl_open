#include "frontend/campaign_hub.hpp"

#include "frontend/gui.hpp"

#include <algorithm>
#include <cstdlib>

namespace sl_open::frontend
{
namespace
{
constexpr HubRegion region(
	std::int16_t x,
	std::int16_t y,
	std::int16_t width,
	std::int16_t height)
{
	return {x, y, width, height};
}

constexpr HubNode kEarlyHub[] = {
	{region(0, 0, 0, 0), "rel_ladd_bunk.bik", "rel_doorloop.bik", 0xdb,
		{2, 3, 1}, 3},
	{region(170, 120, 296, 290), "rel_bunkroom2briefing_door.bik",
		"", 0xdb,
		{25}, 1, HubAction::room},
	{region(0, 0, 40, 479), "rel_t2itac.bik", "rel_itacloop.bik", 0xc3,
		{8, 19, 10}, 3},
	{region(562, 0, 78, 480), "rel_t2l.bik", "rel_tv_far_away_loop.bik", 0xc7,
		{4, 5, 17, 20}, 4},
	{region(0, 0, 40, 479), "rel_l2t.bik", "rel_doorloop.bik", 0xd9,
		{2, 3, 1}, 3},
	{region(562, 0, 78, 480), "rel_l2cd.bik", "rel_cdloop.bik", 0xc6,
		{14, 6, 15}, 3},
	{region(562, 0, 78, 480), "rel_cd2pod.bik", "rel_podmon_loop.bik", 0xca,
		{13, 7, 11}, 3},
	{region(562, 0, 78, 480), "rel_pod2itac.bik", "rel_itacloop.bik", 0xc3,
		{8, 19, 10}, 3},
	{region(150, 100, 150, 300), "bunk2itac_no_eye_recog.bik", "", 0xcc,
		{9}, 1, HubAction::itac},
	{region(181, 150, 340, 150), "rel_itac2bunk.bik", "rel_doorloop.bik", 0xce,
		{2, 3, 1}, 3},
	{region(0, 0, 40, 479), "rel_itac2pod.bik", "rel_podmon_loop.bik", 0xca,
		{13, 7, 11}, 3},
	{region(200, 100, 150, 400), "rel_podop2c.bik", "", 0xd8,
		{12}, 1, HubAction::sim_pod},
	{region(181, 150, 340, 150), "rel_pod2c.bik", "rel_doorloop.bik", 0xd9,
		{2, 3, 1}, 3},
	{region(0, 0, 40, 479), "rel_pod2cd.bik", "rel_cdloop.bik", 0xc6,
		{14, 6, 15}, 3},
	{region(0, 0, 40, 480), "rel_cd2l.bik", "rel_tv_far_away_loop.bik", 0xc7,
		{4, 5, 17, 20}, 4},
	{region(200, 150, 200, 250), "rel_bunk2cd.bik", "", 0x3c5,
		{}, 0, HubAction::cd_player},
	{region(181, 150, 340, 150), "rel_cd2bunk.bik", "rel_doorloop.bik", 0xdb,
		{2, 3, 1}, 3},
	{region(300, 0, 100, 100), "rel_c_tv.bik", "rel_tv_in_loop.bik", 0x298,
		{18}, 1, HubAction::news},
	{region(0, 400, 400, 480), "rel_tv_c.bik", "rel_tv_far_away_loop.bik", 0xc7,
		{4, 5, 17, 20}, 4},
	{region(562, 0, 78, 480), "rel_itac2t.bik", "rel_doorloop.bik", 0xd9,
		{2, 3, 1}, 3},
	{region(200, 200, 400, 400), "rel_c2lock.bik", "single_rel_c2lock.bik", 0xd6,
		{23, 21}, 2, HubAction::medals},
	{region(200, 200, 400, 400), "rel_locklup.bik", "single_rel_locklup.bik", 0xea,
		{22}, 1, HubAction::medals},
	{region(200, 200, 400, 400), "rel_lockldo.bik", "single_rel_lockldo.bik", 0xd5,
		{23, 21}, 2},
	{region(0, 0, 80, 400), "rel_lock2c.bik", "rel_doorloop.bik", 0xd5,
		{2, 3, 1}, 3},
	{region(0, 0, 0, 0), "rel_cap2itac.bik", "", 0xd3, {}, 0},
	{region(100, 0, 500, 480), "rel_c2bre.bik", "", 0xdb, {}, 0,
		HubAction::briefing},
};

constexpr const char* kFishMovies[] = {
	"move_a_.bik", "move_d_.bik", "move_a_.bik", "move_a_.bik",
	"move_b_.bik", "move_a_.bik", "move_a_.bik", "move_c_.bik",
	"move_a_.bik", "move_d_.bik", "move_d_.bik", "move_a_.bik",
	"move_a_.bik", "move_c_.bik",
};

constexpr HubNode kLateHub[] = {
	{region(220, 0, 200, 480), "bunk2wr.bik",
		"", 219, {99, 100}, 2, HubAction::briefing},
	{region(0, 0, 40, 479), "brd_p2i.bik",
		"b2iloop.bik", 195, {30, 49, 53}, 3, HubAction::room},
	{region(562, 101, 78, 380), "brd_p2d.bik",
		"", 196, {0, 3, 8}, 3, HubAction::room},
	{region(0, 0, 40, 479), "brd_d2p.bik",
		"brd2podl.bik", 197, {1, 2, 54, 51}, 4, HubAction::room},
	{region(0, 0, 40, 479), "brd_cd2d.bik",
		"", 196, {0, 3, 8}, 3, HubAction::room},
	{region(0, 0, 40, 479), "brd_l2cd.bik",
		"brd2cdl.bik", 198, {4, 7, 6}, 3, HubAction::room},
	{region(200, 0, 200, 479), "cd_play.bik",
		"brd2cdl.bik", 965, {255}, 1, HubAction::cd_player},
	{region(562, 0, 78, 480), "brd_cd2l.bik",
		"brd2lokl.bik", 199, {85, 5, 31}, 3, HubAction::room},
	{region(562, 0, 78, 480), "brd_d2cd.bik",
		"brd2cdl.bik", 198, {4, 7, 47}, 3, HubAction::room},
	{region(220, 0, 200, 480), "itac2dor.bik",
		"", 200, {0, 3, 8}, 3, HubAction::room},
	{region(562, 0, 78, 480), "ir_i2f.bik",
		"itac_fiskl.bik", 205, {19, 26, 21}, 3, HubAction::room},
	{region(562, 0, 78, 480), "ir_l2i.bik",
		"itac_itacl.bik", 195, {25, 10, 23}, 3, HubAction::room},
	{region(181, 150, 340, 150), "itac2lok.bik",
		"", 209, {255, 255, 255}, 3, HubAction::medals},
	{region(562, 0, 78, 480), "ir_cd2l.bik",
		"", 199, {11, 24, 12}, 3, HubAction::room},
	{region(0, 0, 40, 479), "ir_cd2d.bik",
		"itac_dl.bik", 196, {28, 16, 9}, 3, HubAction::room},
	{region(181, 150, 340, 150), "itac2cd.bik",
		"cd_cdl_.bik", 210, {43, 46, 6}, 3, HubAction::room},
	{region(562, 0, 78, 480), "ir_d2cd.bik",
		"itac_cdl.bik", 198, {14, 13, 15}, 3, HubAction::room},
	{region(562, 0, 78, 480), "ir_p2d.bik",
		"itac_dl.bik", 196, {28, 9, 16}, 3, HubAction::room},
	{region(260, 220, 80, 100), "itac2pod_hud.bik",
		"", 201, {70}, 1, HubAction::sim_pod, 3},
	{region(562, 0, 78, 480), "ir_f2p.bik",
		"itac_podl.bik", 202, {27, 17, 18}, 3, HubAction::room},
	{region(562, 0, 78, 480), "fish2rot.bik",
		"itac_dl.bik", 196, {28, 9, 16}, 3, HubAction::room},
	{region(181, 150, 340, 150), "itac2fis.bik",
		"", 203, {20}, 1, HubAction::fish_room},
	{region(562, 0, 78, 480), "ir_i2f.bik",
		"itac_fiskl.bik", 205, {19, 21, 26}, 3, HubAction::room},
	{region(181, 150, 340, 150), "itac2itac.bik",
		"", 204, {}, 0, HubAction::itac},
	{region(0, 0, 40, 479), "ir_l2cd.bik",
		"itac_cdl.bik", 198, {13, 14, 15}, 3, HubAction::room},
	{region(0, 0, 40, 479), "ir_i2l.bik",
		"", 199, {11, 24, 12}, 3, HubAction::room},
	{region(0, 0, 40, 479), "ir_f2i.bik",
		"itac_itacl.bik", 195, {22, 23, 25}, 3, HubAction::room},
	{region(0, 0, 40, 479), "ir_p2f.bik",
		"itac_fiskl.bik", 205, {19, 21, 26}, 3, HubAction::room},
	{region(0, 0, 40, 479), "ir_d2p.bik",
		"itac_podl.bik", 202, {27, 17, 18}, 3, HubAction::room},
	{region(181, 150, 340, 150), "itac2rot.bik",
		"itac_dl.bik", 206, {28, 9, 16}, 3, HubAction::room},
	{region(181, 150, 340, 150), "brd_itac.bik",
		"", 207, {29}, 1, HubAction::itac},
	{region(562, 0, 78, 480), "brd_l2i.bik",
		"b2iloop.bik", 195, {49, 30, 53}, 3, HubAction::room},
	{region(562, 0, 78, 480), "brd_cd2l.bik",
		"", 199, {85, 31, 48}, 3, HubAction::room},
	{region(562, 0, 78, 480), "cd_d2cd.bik",
		"cd_cdl_.bik", 198, {43, 46, 6}, 3, HubAction::room},
	{region(181, 150, 340, 150), "cd2door.bik",
		"", 200, {0, 3, 8}, 3, HubAction::room},
	{region(562, 0, 78, 480), "cd_p2d.bik",
		"", 196, {33, 34, 42}, 3, HubAction::room},
	{region(181, 150, 340, 150), "cd2itac.bik",
		"", 207, {36}, 1, HubAction::itac},
	{region(562, 0, 78, 480), "cd_l2i.bik",
		"", 195, {40, 38, 36}, 3, HubAction::room},
	{region(0, 208, 40, 90), "cd_i2l.bik",
		"", 199, {37, 44, 45}, 3, HubAction::room},
	{region(181, 150, 340, 150), "cd2pod_hud.bik",
		"", 208, {39}, 1, HubAction::sim_pod, 3},
	{region(562, 0, 78, 480), "cd_i2p.bik",
		"", 202, {41, 35, 39}, 3, HubAction::room},
	{region(0, 208, 40, 90), "cd_p2i.bik",
		"", 195, {40, 38, 36}, 3, HubAction::room},
	{region(0, 208, 40, 90), "cd_d2p.bik",
		"", 202, {35, 41, 39}, 3, HubAction::room},
	{region(0, 208, 40, 90), "cd_cd2d.bik",
		"", 196, {33, 34, 42}, 3, HubAction::room},
	{region(0, 208, 40, 90), "cd_l2cd.bik",
		"", 198, {43, 46}, 2, HubAction::room},
	{region(181, 150, 340, 150), "cd2lock.bik",
		"", 209, {255, 255, 255}, 3, HubAction::medals},
	{region(562, 0, 78, 480), "cd_cd2l.bik",
		"", 199, {44, 37, 45}, 3, HubAction::room},
	{region(181, 150, 340, 150), "brd2cd.bik",
		"cd_cdl_.bik", 210, {43, 46, 6}, 3, HubAction::room},
	{region(0, 208, 40, 90), "brd_l2cd.bik",
		"brd2cdl.bik", 198, {32, 4, 47}, 3, HubAction::room},
	{region(0, 208, 40, 90), "brd_i2l.bik",
		"brd2lokl.bik", 199, {85, 31, 48}, 3, HubAction::room},
	{region(0, 0, 0, 0), "b2iloop.bik",
		"b2iloop.bik", 211, {30, 49, 53}, 3, HubAction::room},
	{region(462, 0, 178, 100), "brd2tv.bik",
		"brd2tv_loop.bik", 664, {1}, 1, HubAction::news},
	{region(562, 0, 78, 480), "tv2brd.bik",
		"brd2podl.bik", 202, {1, 2, 54, 51}, 4, HubAction::room},
	{region(562, 0, 78, 480), "brd_i2p.bik",
		"brd2podl.bik", 202, {1, 2, 54, 51}, 4, HubAction::room},
	{region(181, 150, 340, 150), "door2pod_hud.bik",
		"", 216, {70}, 1, HubAction::sim_pod, 3},
	{region(220, 0, 200, 480), "lockzomo.bik",
		"lok_podl.bik", 213, {59, 71, 82}, 3, HubAction::room},
	{region(220, 0, 200, 480), "lock_lup.bik",
		"", 214, {55}, 1, HubAction::medals},
	{region(180, 0, 200, 480), "lock2itac.bik",
		"", 207, {57}, 1, HubAction::itac},
	{region(0, 0, 160, 480), "lock_i2l.bik",
		"", 199, {76, 79, 77}, 3, HubAction::room},
	{region(0, 0, 160, 480), "lock_p2i.bik",
		"lock_itacl.bik", 195, {57, 58, 83}, 3, HubAction::room},
	{region(220, 0, 200, 480), "pod2itac.bik",
		"", 207, {60}, 1, HubAction::itac},
	{region(562, 0, 78, 480), "pod_l2i.bik",
		"pod_2_itac_loop.bik", 195, {60, 69}, 2, HubAction::room},
	{region(562, 0, 78, 480), "pod_cd2l.bik",
		"pod_2_lock_loop.bik", 199, {67, 61, 68}, 3, HubAction::room},
	{region(562, 0, 78, 480), "pod_d2cd.bik",
		"", 198, {65, 62, 66}, 3, HubAction::room},
	{region(220, 0, 200, 480), "pod_2_door.bik",
		"", 200, {0, 3, 8}, 3, HubAction::room},
	{region(0, 0, 160, 480), "pod_cd2d.bik",
		"", 196, {63, 64}, 2, HubAction::room},
	{region(181, 150, 340, 150), "pod2cd.bik",
		"cd_cdl_.bik", 210, {43, 46, 6}, 3, HubAction::room},
	{region(0, 0, 160, 480), "pod_l2cd.bik",
		"", 198, {62, 65, 66}, 3, HubAction::room},
	{region(220, 0, 200, 480), "pod2lock.bik",
		"", 209, {255, 255, 255}, 3, HubAction::medals},
	{region(0, 0, 160, 480), "pod_i2l.bik",
		"pod_2_lock_loop.bik", 199, {67, 61, 68}, 3, HubAction::room},
	{region(181, 150, 340, 150), "pod2rot2.bik",
		"pod_2_itac_loop.bik", 215, {60, 69}, 2, HubAction::room, 2},
	{region(181, 150, 340, 150), "lock2pod_hud.bik",
		"", 216, {70}, 1, HubAction::sim_pod, 3},
	{region(0, 0, 40, 480), "lock_d2p.bik",
		"lok_podl.bik", 202, {82, 71, 59}, 3, HubAction::room},
	{region(220, 0, 200, 480), "lock2door.bik",
		"", 200, {0, 3, 8}, 3, HubAction::room},
	{region(0, 0, 160, 480), "lock_cd2d.bik",
		"lock2door_loop.bik", 196, {72, 73, 81}, 3, HubAction::room},
	{region(220, 0, 200, 480), "lock2cd.bik",
		"cd_cdl_.bik", 210, {43, 46, 6}, 3, HubAction::room},
	{region(0, 0, 160, 480), "lock_l2cd.bik",
		"lock_cdl.bik", 198, {74, 80, 75}, 3, HubAction::room},
	{region(220, 0, 200, 480), "",
		"", 214, {55}, 1, HubAction::medals},
	{region(0, 0, 160, 480), "lock_i2l.bik",
		"", 199, {76, 79, 77}, 3, HubAction::room},
	{region(562, 0, 78, 480), "lock_l2i.bik",
		"lock_itacl.bik", 195, {57, 58, 83}, 3, HubAction::room},
	{region(562, 0, 78, 480), "lock_cd2l.bik",
		"", 199, {76, 79, 77}, 3, HubAction::room},
	{region(562, 0, 78, 480), "lock_d2cd.bik",
		"lock_cdl.bik", 198, {74, 80, 75}, 3, HubAction::room},
	{region(562, 0, 78, 480), "lock_p2d.bik",
		"lock2door_loop.bik", 196, {72, 73, 81}, 3, HubAction::room},
	{region(562, 0, 78, 480), "lock_i2p.bik",
		"lok_podl.bik", 202, {59, 71, 82}, 3, HubAction::room},
	{region(562, 0, 78, 480), "lock_l2i.bik",
		"", 195, {57, 58, 83}, 3, HubAction::room},
	{region(181, 150, 340, 150), "brd2lok.bik",
		"", 209, {95, 56, 84}, 3, HubAction::medals},
	{region(181, 150, 340, 150), "lock2cd.bik",
		"cd_cdl_.bik", 210, {43, 46, 6}, 3, HubAction::room},
	{region(562, 0, 78, 480), "lock_cd2l.bik",
		"", 199, {95, 56, 84}, 3, HubAction::room},
	{region(562, 0, 78, 480), "lock_d2cd.bik",
		"", 198, {87, 94, 86}, 3, HubAction::room},
	{region(181, 150, 340, 150), "lock2door.bik",
		"", 200, {0, 3, 8}, 3, HubAction::room},
	{region(562, 0, 78, 480), "lock_p2d.bik",
		"", 196, {88, 89, 93}, 3, HubAction::room},
	{region(562, 0, 78, 480), "lock_i2p.bik",
		"", 202, {92, 90}, 2, HubAction::room},
	{region(0, 0, 40, 480), "lock_p2i.bik",
		"", 195, {91, 58, 57}, 3, HubAction::room},
	{region(0, 0, 40, 480), "lock_d2p.bik",
		"", 202, {90, 92, 71}, 3, HubAction::room},
	{region(0, 0, 40, 480), "lock_cd2d.bik",
		"", 196, {88, 89, 93}, 3, HubAction::room},
	{region(0, 0, 40, 480), "lock_l2cd.bik",
		"", 198, {86, 87, 94}, 3, HubAction::room},
	{region(0, 300, 80, 480), "rotldoor.bik",
		"", 217, {97}, 1, HubAction::room},
	{region(160, 100, 640, 380), "b2w.bik",
		"", 218, {99, 100}, 2, HubAction::room},
	{region(402, 0, 178, 480), "wrbu2br.bik",
		"", 219, {}, 0, HubAction::briefing},
	{region(462, 0, 178, 480), "wr_l2bre.bik",
		"", 220, {101, 98}, 2, HubAction::room},
	{region(0, 0, 160, 480), "wr_l2c.bik",
		"", 221, {102}, 1, HubAction::room},
	{region(0, 0, 160, 480), "wr_bre2l.bik",
		"", 221, {99, 100}, 2, HubAction::room},
	{region(462, 0, 178, 480), "wr_c2l.bik",
		"", 222, {99, 100}, 2, HubAction::room},
};

const HubNode* hub_nodes(const CampaignHub& hub)
{
	return hub.late_campaign ? kLateHub : kEarlyHub;
}

std::size_t hub_node_count(const CampaignHub& hub)
{
	return hub.late_campaign
		? sizeof(kLateHub) / sizeof(kLateHub[0])
		: sizeof(kEarlyHub) / sizeof(kEarlyHub[0]);
}
}

const HubNode& campaign_hub_node(const CampaignHub& hub)
{
	if (hub.node >= hub_node_count(hub))
	{
		std::abort();
	}
	return hub_nodes(hub)[hub.node];
}

void campaign_hub_reset(CampaignHub& hub, bool late_campaign)
{
	hub = {};
	hub.late_campaign = late_campaign;
	hub.node = late_campaign ? 50 : 0;
	hub.cursor_edge_state = 11;
}

void campaign_hub_set_pointer(
	CampaignHub& hub,
	float x,
	float y,
	bool inside)
{
	hub.pointer_x = std::clamp(x, 4.0f, 575.0f);
	hub.pointer_y = std::clamp(y, 0.0f, 439.0f);
	hub.hovered = -1;
	if (!inside || !hub.loop_active)
	{
		return;
	}
	const HubNode& node = campaign_hub_node(hub);
	const HubNode* nodes = hub_nodes(hub);
	for (std::uint8_t index = 0; index < node.destination_count; ++index)
	{
		const std::uint8_t destination = node.destinations[index];
		if (destination != kNoHubNode
			&& gui::hit_open(
				nodes[destination].region,
				hub.pointer_x,
				hub.pointer_y))
		{
			hub.hovered = static_cast<std::int8_t>(index);
			return;
		}
	}
}

void campaign_hub_update_cursor(CampaignHub& hub)
{
	const std::uint8_t target =
		hub.pointer_x < 40.0f ? 0
		: (hub.pointer_x > 562.0f ? 21 : 11);
	if (hub.cursor_edge_state < target)
	{
		hub.cursor_edge_state = static_cast<std::uint8_t>(
			std::min<std::uint16_t>(
				hub.cursor_edge_state + 2, target));
	}
	else if (hub.cursor_edge_state > target)
	{
		hub.cursor_edge_state = static_cast<std::uint8_t>(
			hub.cursor_edge_state - target > 2
				? hub.cursor_edge_state - 2
				: target);
	}
}

std::uint8_t campaign_hub_select(const CampaignHub& hub)
{
	const HubNode& node = campaign_hub_node(hub);
	return hub.hovered >= 0
		&& static_cast<std::uint8_t>(hub.hovered) < node.destination_count
			? node.destinations[hub.hovered]
			: kNoHubNode;
}

bool campaign_hub_selection_enters_briefing(
	const CampaignHub& hub,
	std::uint8_t destination)
{
	return !hub.late_campaign && hub.node == 1 && destination == 25;
}

void campaign_hub_enter(CampaignHub& hub, std::uint8_t node)
{
	if (node >= hub_node_count(hub))
	{
		return;
	}
	hub.node = node;
	hub.hovered = -1;
	hub.loop_active = false;
	hub.action_pending = false;
	hub.fish_active = false;
	hub.fish_movie_index = 0;
}

const char* campaign_hub_begin_fish_cycle(CampaignHub& hub)
{
	hub.fish_active = true;
	hub.action_pending = false;
	hub.fish_movie_index = 0;
	return kFishMovies[0];
}

const char* campaign_hub_next_fish_movie(CampaignHub& hub)
{
	hub.fish_movie_index = static_cast<std::uint8_t>(
		(hub.fish_movie_index + 1)
		% (sizeof(kFishMovies) / sizeof(kFishMovies[0])));
	return kFishMovies[hub.fish_movie_index];
}

std::uint8_t campaign_hub_return_node(
	const CampaignHub& hub,
	HubAction action)
{
	if (hub.late_campaign)
	{
		switch (action)
		{
		case HubAction::itac: return 29;
		case HubAction::sim_pod: return 70;
		case HubAction::cd_player: return 43;
		case HubAction::news: return 52;
		case HubAction::medals: return 55;
		default: return 50;
		}
	}
	switch (action)
	{
	case HubAction::itac: return 9;
	case HubAction::sim_pod: return 12;
	case HubAction::cd_player: return 16;
	case HubAction::news: return 18;
	case HubAction::medals: return 23;
	default: return 0;
	}
}
}
