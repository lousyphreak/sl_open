#pragma once

#include <cstdint>

namespace sl_open::hud
{
constexpr float kRetailHudWidth = 640.0f;
constexpr float kRetailHudHeight = 480.0f;

enum class Anchor : std::uint8_t
{
	top_left,
	top,
	top_right,
	left,
	center,
	right,
	bottom_left,
	bottom,
	bottom_right,
};

struct Rect
{
	float x{};
	float y{};
	float width{};
	float height{};
};

struct Layout
{
	float width{};
	float height{};
	float element_scale{1.0f};
};

Layout make_layout(
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	float ui_scale = 1.0f);
Rect place(
	const Layout& layout,
	const Rect& retail_rect,
	Anchor anchor);
}
