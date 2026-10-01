#pragma once

#include <cstdint>

namespace sl_open::frontend
{
constexpr std::uint8_t kNoHubNode = 0xff;
constexpr std::uint8_t kMaxHubDestinations = 4;

enum class HubAction : std::uint8_t
{
	room,
	briefing,
	itac,
	fish_room,
	sim_pod,
	medals,
	news,
	cd_player,
};

struct HubRegion
{
	std::int16_t x{};
	std::int16_t y{};
	std::int16_t width{};
	std::int16_t height{};
};

struct HubNode
{
	HubRegion region;
	const char* primary_movie{};
	const char* loop_movie{};
	std::uint16_t label_id{};
	std::uint8_t destinations[kMaxHubDestinations]{
		kNoHubNode, kNoHubNode, kNoHubNode, kNoHubNode};
	std::uint8_t destination_count{};
	HubAction action{HubAction::room};
	std::int16_t selection_sound{-1};
};

struct CampaignHub
{
	std::uint8_t node{};
	std::uint8_t cursor_edge_state{11};
	std::int8_t hovered{-1};
	float pointer_x{320.0f};
	float pointer_y{240.0f};
	bool late_campaign{};
	bool loop_active{};
	bool action_pending{};
	bool fish_active{};
	std::uint8_t fish_movie_index{};
};

const HubNode& campaign_hub_node(const CampaignHub& hub);
void campaign_hub_reset(CampaignHub& hub, bool late_campaign);
void campaign_hub_set_pointer(
	CampaignHub& hub,
	float x,
	float y,
	bool inside);
void campaign_hub_update_cursor(CampaignHub& hub);
std::uint8_t campaign_hub_select(const CampaignHub& hub);
bool campaign_hub_selection_enters_briefing(
	const CampaignHub& hub,
	std::uint8_t destination);
void campaign_hub_enter(CampaignHub& hub, std::uint8_t node);
std::uint8_t campaign_hub_return_node(
	const CampaignHub& hub,
	HubAction action);
const char* campaign_hub_begin_fish_cycle(CampaignHub& hub);
const char* campaign_hub_next_fish_movie(CampaignHub& hub);
}
