#pragma once

#include "frontend/campaign_frontend.hpp"

namespace sl_open::frontend
{
void campaign_save_browser_set_pointer(
	CampaignFrontend& frontend,
	float x,
	float y);
CampaignSelection campaign_save_browser_back(CampaignFrontend& frontend);
CampaignSelection campaign_save_browser_select(
	CampaignFrontend& frontend,
	campaign::CampaignStore& store,
	campaign::CampaignState& campaign);
}
