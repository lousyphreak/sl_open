#include "hud/layout.hpp"

#include <algorithm>

namespace sl_open::hud
{
Layout make_layout(
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	float ui_scale)
{
	const float width = static_cast<float>(drawable_width);
	const float height = static_cast<float>(drawable_height);
	const float fit_scale = std::min(
		width / kRetailHudWidth,
		height / kRetailHudHeight);
	return {
		width,
		height,
		std::max(0.0f, fit_scale * ui_scale),
	};
}

Rect place(
	const Layout& layout,
	const Rect& retail_rect,
	Anchor anchor)
{
	const float scale = layout.element_scale;
	const float width = retail_rect.width * scale;
	const float height = retail_rect.height * scale;
	const float left = retail_rect.x * scale;
	const float right =
		(kRetailHudWidth - retail_rect.x - retail_rect.width) * scale;
	const float top = retail_rect.y * scale;
	const float bottom =
		(kRetailHudHeight - retail_rect.y - retail_rect.height) * scale;
	const float center_x =
		layout.width * 0.5f
		+ (retail_rect.x + retail_rect.width * 0.5f
			- kRetailHudWidth * 0.5f) * scale
		- width * 0.5f;
	const float center_y =
		layout.height * 0.5f
		+ (retail_rect.y + retail_rect.height * 0.5f
			- kRetailHudHeight * 0.5f) * scale
		- height * 0.5f;

	Rect result{0.0f, 0.0f, width, height};
	switch (anchor)
	{
	case Anchor::top_left:
		result.x = left;
		result.y = top;
		break;
	case Anchor::top:
		result.x = center_x;
		result.y = top;
		break;
	case Anchor::top_right:
		result.x = layout.width - right - width;
		result.y = top;
		break;
	case Anchor::left:
		result.x = left;
		result.y = center_y;
		break;
	case Anchor::center:
		result.x = center_x;
		result.y = center_y;
		break;
	case Anchor::right:
		result.x = layout.width - right - width;
		result.y = center_y;
		break;
	case Anchor::bottom_left:
		result.x = left;
		result.y = layout.height - bottom - height;
		break;
	case Anchor::bottom:
		result.x = center_x;
		result.y = layout.height - bottom - height;
		break;
	case Anchor::bottom_right:
		result.x = layout.width - right - width;
		result.y = layout.height - bottom - height;
		break;
	}
	return result;
}
}
