#pragma once

#include "render/frontend_renderer.hpp"

namespace sl_open::render
{
bool loadout_renderer_build(
	const FrontendRenderer& renderer,
	LoadoutRenderer& destination,
	const assets::TextureImage& panels,
	const assets::TextureImage (&disc)[4],
	const assets::TextureImage& glow,
	const assets::TextureImage& hardpoints,
	const assets::ShipModel (&ships)[12],
	const assets::ShipModel (&guns)[12],
	const assets::TextureImage (&ship_textures)[12],
	const assets::ShipModel (&missiles)[10],
	const assets::TextureImage& missile_texture);
void loadout_renderer_commit(
	LoadoutRenderer& destination,
	LoadoutRenderer& source);
void loadout_renderer_shutdown(LoadoutRenderer& renderer);
}
