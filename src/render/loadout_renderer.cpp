#include "render/loadout_renderer.hpp"

#include "assets/image.hpp"
#include "assets/gameplay_model.hpp"
#include "frontend/loadout_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace sl_open::render
{
namespace
{
constexpr bgfx::ViewId kLoadoutView = 11;

struct FrontendVertex
{
	float x;
	float y;
	float z;
	std::uint32_t color;
	float u;
	float v;
};
static_assert(sizeof(FrontendVertex) == sizeof(float) * 6);

bool upload_loadout_decor(
	const bgfx::VertexLayout& layout, LoadoutRenderer& destination)
{
	constexpr std::uint16_t disc_source_indices[] = {
		3, 4, 0, 0, 4, 1, 4, 5, 1, 1, 5, 2,
		6, 7, 3, 3, 7, 4, 7, 8, 4, 4, 8, 5,
	};
	constexpr glm::vec2 disc_uv[6] = {
		{0.001953125f, 0.998046875f},
		{0.998046875f, 0.998046875f},
		{0.001953125f, 0.001953125f},
		{0.001953125f, 0.001953125f},
		{0.998046875f, 0.998046875f},
		{0.998046875f, 0.001953125f},
	};
	FrontendVertex disc_vertices[24];
	for (std::uint16_t index = 0; index < 24; ++index)
	{
		const std::uint16_t source = disc_source_indices[index];
		disc_vertices[index] = {
			-10.0f + (source % 3) * 10.0f,
			-10.0f + (source / 3) * 10.0f,
			0.0f,
			0xffffffff,
			disc_uv[index % 6].x,
			disc_uv[index % 6].y,
		};
	}
	constexpr FrontendVertex glow_vertices[6] = {
		{-10.0f, 10.0f, 0.0f, 0xffffffff, 0.0f, 1.0f},
		{10.0f, 10.0f, 0.0f, 0xffffffff, 1.0f, 1.0f},
		{-10.0f, -10.0f, 0.0f, 0xffffffff, 0.0f, 0.0f},
		{10.0f, 10.0f, 0.0f, 0xffffffff, 1.0f, 1.0f},
		{10.0f, -10.0f, 0.0f, 0xffffffff, 1.0f, 0.0f},
		{-10.0f, -10.0f, 0.0f, 0xffffffff, 0.0f, 0.0f},
	};
	constexpr float half_width = 0.35f;
	constexpr float half_height = 0.56f;
	constexpr FrontendVertex hardpoint_vertices[6] = {
		{-half_width, half_height, 0.0f, 0xffffffff, 0.0f, 1.0f},
		{half_width, half_height, 0.0f, 0xffffffff, 1.0f, 1.0f},
		{-half_width, -half_height, 0.0f, 0xffffffff, 0.0f, 0.0f},
		{half_width, half_height, 0.0f, 0xffffffff, 1.0f, 1.0f},
		{half_width, -half_height, 0.0f, 0xffffffff, 1.0f, 0.0f},
		{-half_width, -half_height, 0.0f, 0xffffffff, 0.0f, 0.0f},
	};
	destination.disc_vertices = bgfx::createVertexBuffer(
		bgfx::copy(disc_vertices, sizeof(disc_vertices)), layout);
	destination.glow_vertices = bgfx::createVertexBuffer(
		bgfx::copy(glow_vertices, sizeof(glow_vertices)), layout);
	destination.hardpoint_vertices = bgfx::createVertexBuffer(
		bgfx::copy(hardpoint_vertices, sizeof(hardpoint_vertices)), layout);
	return bgfx::isValid(destination.disc_vertices)
		&& bgfx::isValid(destination.glow_vertices)
		&& bgfx::isValid(destination.hardpoint_vertices);
}
}

bool loadout_renderer_build(
	const FrontendRenderer& renderer,
	LoadoutRenderer& destination,
	const assets::TextureImage& panels,
	const assets::TextureImage (&disc)[4],
	const assets::TextureImage& glow,
	const assets::TextureImage& hardpoints,
	const assets::GameplayModel (&ships)[12],
	const assets::GameplayModel (&guns)[12],
	const assets::GameplayModel (&missiles)[10])
{
	loadout_renderer_shutdown(destination);
	if (!renderer.ready
		|| panels.width != 256
		|| panels.height != 256
		|| !frontend_texture_upload(destination.panels, panels)
		|| !frontend_texture_upload(destination.glow, glow)
		|| !frontend_texture_upload(destination.hardpoints, hardpoints))
	{
		loadout_renderer_shutdown(destination);
		return false;
	}
	for (std::uint32_t index = 0; index < 4; ++index)
	{
		if (!frontend_texture_upload(destination.disc[index], disc[index]))
		{
			loadout_renderer_shutdown(destination);
			return false;
		}
	}
	if (!upload_loadout_decor(renderer.layout, destination))
	{
		loadout_renderer_shutdown(destination);
		return false;
	}

	for (std::uint32_t ship = 0; ship < 12; ++ship)
	{
		if (!model_renderer_upload(
				ships[ship],
				destination.ship_models[ship],
				renderer.lit_model_layout)
			|| !model_renderer_upload(
				guns[ship],
				destination.gun_models[ship],
				renderer.lit_model_layout))
		{
			loadout_renderer_shutdown(destination);
			return false;
		}
	}
	for (std::uint32_t missile = 0; missile < 10; ++missile)
	{
		if (!model_renderer_upload(
				missiles[missile],
				destination.missile_models[missile],
				renderer.lit_model_layout))
		{
			loadout_renderer_shutdown(destination);
			return false;
		}
	}
	destination.ready = true;
	return true;
}

void loadout_renderer_commit(
	LoadoutRenderer& destination,
	LoadoutRenderer& source)
{
	loadout_renderer_shutdown(destination);
	destination = static_cast<LoadoutRenderer&&>(source);
	source.disc_vertices = BGFX_INVALID_HANDLE;
	source.glow_vertices = BGFX_INVALID_HANDLE;
	source.hardpoint_vertices = BGFX_INVALID_HANDLE;
	source.ready = false;
}

void loadout_renderer_shutdown(LoadoutRenderer& renderer)
{
	if (bgfx::isValid(renderer.disc_vertices))
	{
		bgfx::destroy(renderer.disc_vertices);
		renderer.disc_vertices = BGFX_INVALID_HANDLE;
	}
	if (bgfx::isValid(renderer.glow_vertices))
	{
		bgfx::destroy(renderer.glow_vertices);
		renderer.glow_vertices = BGFX_INVALID_HANDLE;
	}
	if (bgfx::isValid(renderer.hardpoint_vertices))
	{
		bgfx::destroy(renderer.hardpoint_vertices);
		renderer.hardpoint_vertices = BGFX_INVALID_HANDLE;
	}
	frontend_texture_shutdown(renderer.panels);
	for (FrontendTexture& texture : renderer.disc)
	{
		frontend_texture_shutdown(texture);
	}
	frontend_texture_shutdown(renderer.glow);
	frontend_texture_shutdown(renderer.hardpoints);
	for (MissionGpuModel& model : renderer.ship_models)
	{
		model_renderer_shutdown(model);
	}
	for (MissionGpuModel& model : renderer.gun_models)
	{
		model_renderer_shutdown(model);
	}
	for (MissionGpuModel& model : renderer.missile_models)
	{
		model_renderer_shutdown(model);
	}
	renderer.ready = false;
}

void loadout_renderer_submit(
	const FrontendRenderer& renderer,
	const FrontendCommands& commands,
	std::uint32_t scene_index,
	std::uint16_t viewport_x,
	std::uint16_t viewport_y,
	std::uint16_t viewport_width,
	std::uint16_t viewport_height,
	float brightness)
{
	const LoadoutRenderState& state = commands.loadout;
	bgfx::setViewRect(
		kLoadoutView,
		viewport_x,
		viewport_y,
		viewport_width,
		viewport_height);
	bgfx::setViewMode(kLoadoutView, bgfx::ViewMode::Sequential);
	const glm::mat4 scene_view = frontend::loadout_view();
	glm::mat4 scene_projection =
		sl_open::math::perspective_lh_framebuffer_y_down(
		glm::radians(64.114135f),
		4.0f / 3.0f,
		1.0f,
		1000.0f,
		bgfx::getCaps()->homogeneousDepth);
	// Surrender projects both axes with an exact 383-pixel scale at
	// 640x480: (639 * .6, 479 * .8).
	scene_projection[0][0] = (639.0f * 0.6f) / 320.0f;
	scene_projection[1][1] = -(479.0f * 0.8f) / 240.0f;
	bgfx::setViewTransform(
		kLoadoutView,
		glm::value_ptr(scene_view),
		glm::value_ptr(scene_projection));
	bgfx::touch(kLoadoutView);

	const auto& scales = frontend::kLoadoutShipScales;
	const auto& positions = frontend::kLoadoutSelectorPositions;
	const std::uint8_t selected = state.selected_ship;
	const std::uint8_t previous = state.previous_ship;
	const std::uint8_t available = state.available_ships;
	const float ship_selection = state.ship_selection;
	const bool ship_selection_active =
		previous != selected && ship_selection < 1.0f;
	const float spin = state.spin;
	const float activation = state.activation;
	const bool activation_reverse = state.reverse;
	const float activation_elapsed = activation_reverse
		? 1.0f - activation
		: activation;
	const auto cosine_ease = [](float value) {
		return 0.5f - std::cos(
			std::clamp(value, 0.0f, 1.0f)
				* 3.14159265358979323846f) * 0.5f;
	};
	const float layout_progress = cosine_ease(activation);
	const float disc_scale = 0.001f + 0.999f * (activation_reverse
		? 1.0f - std::sqrt(activation_elapsed)
		: activation_elapsed * activation_elapsed);
	const float glow_scale = 0.001f + 0.999f * cosine_ease(activation);
	const auto button_progress = [&cosine_ease](float elapsed_ms, bool reverse) {
		const float time = std::clamp(
			elapsed_ms / 500.0f,
			0.0f,
			1.0f);
		struct Progress
		{
			float position;
			float scale;
		};
		return reverse
			? Progress{
				1.0f - cosine_ease(time),
				0.001f + 0.999f * (1.0f - time * time)}
			: Progress{
				cosine_ease(time),
				0.001f + 0.999f * time * time};
	};
	const float disc_x = 1.499999f;
	const float disc_y = 3.749997f;
	const float disc_z =
		1.449999f - 2.0f + 2.0f * layout_progress;
	const std::uint8_t page = state.page;
	const std::uint8_t previous_page = state.previous_page;
	const float page_transition = state.page_transition;
	const float page_elapsed = state.page_elapsed;
	const std::size_t hardpoint_count = renderer.loadout_renderer.ship_models[selected].hardpoints.size();
	const float zoom_duration = hardpoint_count == 0 ? 0.0f
		: 400.0f + (hardpoint_count - 1) * 80.0f;
	const float missile_page = page == 1
		? cosine_ease(page_elapsed / 1000.0f)
		: (previous_page == 1
			? 1.0f - cosine_ease((page_elapsed - zoom_duration) / 1000.0f) : 0.0f);
	const float gun_page = page == 2 ? cosine_ease(page_transition)
		: (previous_page == 2 ? 1.0f - cosine_ease(page_transition) : 0.0f);
	const auto panel_flip = [&cosine_ease](float t) {
		return cosine_ease(t) * glm::pi<float>()
			- (t >= 0.5f ? glm::pi<float>() : 0.0f);
	};
	const float name_flip = panel_flip(state.name_flip);
	const float info_flip = panel_flip(state.info_flip)
		* (page == 0 && previous_page != 0 ? -1.0f : 1.0f);
	const float uv_rect[] = {0.0f, 0.0f, 1.0f, 1.0f};
	const float tint[] = {
		std::min(brightness, 1.0f),
		std::min(brightness, 1.0f),
		std::min(brightness, 1.0f),
		1.0f,
	};
	// The authored UV corners are retained in the loadout GPU mesh.
	const glm::mat3 disc_orientation = sl_open::math::rotation_from_euler(
		{
			-glm::half_pi<float>(),
			0.0f,
			(layout_progress - 1.0f) * glm::two_pi<float>(),
		});
	const glm::vec3 disc_position{disc_x, disc_y, disc_z};
	const glm::mat4 disc_transform = sl_open::math::model_transform(
		disc_orientation, disc_scale, disc_position);
	for (std::uint32_t section = 0; section < 4; ++section)
	{
		bgfx::setUniform(renderer.uv_rect_uniform, uv_rect);
		bgfx::setUniform(renderer.tint_uniform, tint);
		bgfx::setTransform(glm::value_ptr(disc_transform));
		bgfx::setVertexBuffer(
			0, renderer.loadout_renderer.disc_vertices,
			section * 6, 6);
		bgfx::setTexture(
			0, renderer.texture_sampler,
			renderer.loadout_renderer.disc[section].handle);
		bgfx::setState(
			BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
			| BGFX_STATE_BLEND_ADD);
		bgfx::submit(kLoadoutView, renderer.rgba_program);
	}

	const glm::mat4 glow_transform = sl_open::math::srt(
		glm::vec3{glow_scale},
		{1.765725f, 0.0f, 0.0f},
		{1.000001f, 5.099997f, -3.8f});
	bgfx::setUniform(renderer.uv_rect_uniform, uv_rect);
	bgfx::setUniform(renderer.tint_uniform, tint);
	bgfx::setTransform(glm::value_ptr(glow_transform));
	bgfx::setVertexBuffer(
		0, renderer.loadout_renderer.glow_vertices);
	bgfx::setTexture(
		0, renderer.texture_sampler,
		renderer.loadout_renderer.glow.handle);
	bgfx::setState(
		BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
		| BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_ADD);
	bgfx::submit(kLoadoutView, renderer.rgba_program);

	for (std::uint8_t item = 0; item < available; ++item)
	{
		if ((state.available_ship_mask & (1u << item)) == 0)
		{
			continue;
		}
		const bool is_selected = item == selected;
		const std::uint8_t ring_index = static_cast<std::uint8_t>(
			(12 - available) / 2 + item);
		float center_weight = is_selected ? 1.0f : 0.0f;
		float center_scale_weight = center_weight;
		if (ship_selection_active)
		{
			if (is_selected)
			{
				center_weight = cosine_ease(ship_selection);
				center_scale_weight = ship_selection;
			}
			else if (item == previous)
			{
				center_weight =
					1.0f - cosine_ease(ship_selection);
				center_scale_weight = 1.0f - ship_selection;
			}
		}
		const glm::vec3 ring = positions[ring_index];
		const glm::vec3 local{
			glm::mix(ring.x, 23.0f, center_weight) * 0.078125f,
			glm::mix(ring.y, -23.0f, center_weight) * 0.078125f,
			glm::mix(ring.z, -6.0f, center_weight),
		};
		glm::vec3 model_position = disc_position + disc_orientation * local * disc_scale;
		if (is_selected && state.reverse)
		{
			// Loadout_layout_ship_models (0x004493b0) keeps the selected
			// ship's position fixed while the disc animation runs backward.
			model_position = {3.296874f, -2.250003f, 3.246874f};
		}
		float sink = 0.0f;
		if (!is_selected && !state.reverse && (page == 1 || previous_page == 1))
		{
			const std::uint8_t ordinal = item - (item > selected ? 1 : 0);
			const float t = std::clamp((page_elapsed
				- (page == 1 ? 0.0f : 800.0f) - ordinal * 56.0f) / 800.0f, 0.0f, 1.0f);
			sink = page == 1 ? t * t : (1.0f - t) * (1.0f - t);
			if (sink == 1.0f)
			{
				continue;
			}
			model_position.y += 2.0f * sink;
		}
		const float scale = scales[item]
			* (0.1944444444f + (1.0f - 0.1944444444f) * center_scale_weight)
			* disc_scale;
		const glm::vec3 ring_position = disc_position + disc_orientation
			* glm::vec3{ring.x * 0.078125f, ring.y * 0.078125f, ring.z} * disc_scale;
		const glm::mat3 ring_orientation = frontend::loadout_selector_orientation(ring_position);
		glm::mat3 center_orientation{1.0f};
		const float center_spin = page == 1
			? state.previous_spin : previous_page == 1 ? 0.0f : ship_selection_active
			? (is_selected
				? 0.0f
				: (item == previous
					? state.previous_spin
					: spin))
			: spin;
		center_orientation = sl_open::math::postrotate(
			center_orientation,
			center_spin,
			{0.0f, 1.0f, 0.0f});
		center_orientation = sl_open::math::postrotate(
			center_orientation,
			-0.75f,
			{1.0f, 0.0f, 0.0f});
		if (missile_page > 0.0f)
		{
			const glm::vec3 current_euler =
				sl_open::math::rotation_to_euler(center_orientation);
			constexpr glm::vec3 belly_up_euler{
				-glm::half_pi<float>(),
				glm::pi<float>(),
				0.0f,
			};
			center_orientation = sl_open::math::rotation_from_euler(
				glm::mix(
					current_euler,
					belly_up_euler,
					missile_page));
		}
		const glm::vec3 ring_euler =
			sl_open::math::rotation_to_euler(ring_orientation);
		const glm::vec3 center_euler =
			sl_open::math::rotation_to_euler(center_orientation);
		const glm::mat3 orientation =
			sl_open::math::rotation_from_euler(
				glm::mix(
					ring_euler,
					center_euler,
					center_weight));
		const glm::mat4 transform = sl_open::math::model_transform(
			orientation, scale, model_position);
		const float model_brightness = std::min(brightness, 1.0f);
		const glm::vec4 no_clip{0.0f, 0.0f, 0.0f, 1.0f};
		// 0x0044a110/0x004490e0 sweep the two complementary
		// material clip planes across eleven scene units.
		const float clip_x = model_position.x - 5.5f + gun_page * 11.0f;
		if (!is_selected || gun_page < 1.0f)
		{
			const glm::vec4 clip = is_selected && gun_page > 0.0f
				? glm::transpose(transform) * glm::vec4{1.0f, 0.0f, 0.0f, -clip_x}
				: no_clip;
			model_renderer_submit_preview(
				renderer, renderer.loadout_renderer.ship_models[item],
				kLoadoutView, transform,
				false,
				glm::vec3{model_brightness}, clip,
				is_selected || (ship_selection_active && item == previous)
					? 0 : state.selector_lod);
		}
		if (is_selected && gun_page > 0.0f)
		{
			const glm::vec4 clip = gun_page < 1.0f
				? glm::transpose(transform) * glm::vec4{-1.0f, 0.0f, 0.0f, clip_x}
				: no_clip;
			model_renderer_submit_preview(
				renderer, renderer.loadout_renderer.gun_models[item],
				kLoadoutView, transform, true,
				glm::vec3{model_brightness}, clip);
		}

		if (is_selected && !ship_selection_active && gun_page < 1.0f
			&& (state.reverse || activation == 1.0f))
		{
			const std::uint32_t hardpoint_count =
				renderer.loadout_renderer.ship_models[item].hardpoints.size();
			for (std::uint32_t hardpoint_index = 0;
				hardpoint_index < hardpoint_count;
				++hardpoint_index)
			{
				const assets::GameplayHardpoint& hardpoint =
					renderer.loadout_renderer.ship_models[item].hardpoints[hardpoint_index];
				const std::int32_t missile_id = state.mounted_loadout[hardpoint_index];

				glm::mat3 attachment_orientation =
					orientation * hardpoint.basis;
				const glm::vec3 attachment_position =
					model_position
						+ orientation * hardpoint.position * scale;
				if (missile_id < 0 || missile_id >= 10)
				{
					if (state.reverse || (missile_page < 0.999f && previous_page != 1))
					{
						continue;
					}
					const glm::mat4 hardpoint_transform =
						sl_open::math::model_transform(
							glm::mat3{1.0f},
							0.001f + 0.999f * std::clamp(
								(page_elapsed - (page == 1 ? 1000.0f : 0.0f)
									- hardpoint_index * 80.0f) / 400.0f,
								0.0f, 1.0f) * (page == 1 ? 1.0f : -1.0f)
								+ (page == 1 ? 0.0f : 0.999f),
							attachment_position
								+ attachment_orientation[1]
									* (0.05f * scale));
					bgfx::setUniform(renderer.uv_rect_uniform, uv_rect);
					bgfx::setUniform(renderer.tint_uniform, tint);
					bgfx::setTransform(
						glm::value_ptr(hardpoint_transform));
					bgfx::setVertexBuffer(
						0, renderer.loadout_renderer.hardpoint_vertices);
					bgfx::setTexture(
						0,
						renderer.texture_sampler,
						renderer.loadout_renderer.hardpoints.handle,
						BGFX_SAMPLER_MIN_POINT
							| BGFX_SAMPLER_MAG_POINT
							| BGFX_SAMPLER_MIP_POINT);
					bgfx::setState(
						BGFX_STATE_WRITE_RGB
							| BGFX_STATE_WRITE_A
							| BGFX_STATE_BLEND_ALPHA);
					bgfx::submit(
						kLoadoutView, renderer.rgba_program);
				}
				if (state.missile_animation_active[
						hardpoint_index])
				{
					const std::uint8_t animation_missile =
						std::min<std::uint8_t>(
							state.missile_animation_item[
									hardpoint_index],
							9);
					glm::mat3 source_orientation{1.0f};
					glm::vec3 source_position{0.0f};
					const std::int8_t carousel_slot =
						frontend::kLoadoutMissileSlots[
							state.missile_layout_tier][
								animation_missile];
					if (carousel_slot > 0)
					{
						const std::uint8_t ring_index =
							static_cast<std::uint8_t>(
								carousel_slot - 1);
						source_position = frontend::loadout_selector_position(ring_index);
						source_orientation = frontend::loadout_selector_orientation(source_position);
					}
					const glm::vec3 source_euler =
						sl_open::math::rotation_to_euler(
							source_orientation);
					constexpr glm::vec3 target_euler{
						-glm::half_pi<float>(),
						glm::pi<float>(),
						0.0f,
					};
					float progress = cosine_ease(
						state.missile_animation_progress[
								hardpoint_index]);
					if (state.missile_animation_removing[
							hardpoint_index])
					{
						progress = 1.0f - progress;
					}
					const glm::mat3 animation_orientation =
						sl_open::math::rotation_from_euler(
							glm::mix(
								source_euler,
								target_euler,
								progress));
					const glm::mat4 animation_transform =
						sl_open::math::model_transform(
							animation_orientation,
							0.007f,
							glm::mix(
								source_position,
								attachment_position,
								progress));
					model_renderer_submit_preview(
						renderer,
						renderer.loadout_renderer.missile_models[animation_missile],
						kLoadoutView, animation_transform,
						false,
						glm::vec3{model_brightness}, no_clip);
				}
				if (missile_id < 0 || missile_id >= 10)
				{
					continue;
				}
				attachment_orientation = sl_open::math::postrotate(
					attachment_orientation,
					-glm::pi<float>(),
					{0.0f, 1.0f, 0.0f});
				const glm::mat4 attachment_transform =
					sl_open::math::model_transform(
						attachment_orientation,
						scales[item] * 0.8f * disc_scale,
						attachment_position);
				model_renderer_submit_preview(
					renderer,
					renderer.loadout_renderer.missile_models[missile_id],
					kLoadoutView, attachment_transform,
					false,
					glm::vec3{model_brightness}, gun_page > 0.0f
						? glm::transpose(attachment_transform) * glm::vec4{1.0f, 0.0f, 0.0f, -clip_x}
						: no_clip);
			}
		}
	}

	if (!state.reverse && (page == 1 || previous_page == 1))
	{
		for (std::uint8_t missile = 0; missile < 10; ++missile)
		{
			if ((state.missile_mask & (1u << missile)) == 0)
			{
				continue;
			}
			const std::int8_t carousel_slot =
				frontend::kLoadoutMissileSlots[
					state.missile_layout_tier][missile];
			if (carousel_slot <= 0)
			{
				continue;
			}
			const std::uint8_t ring_index =
				static_cast<std::uint8_t>(
					carousel_slot - 1);
			const glm::vec3 ring = positions[ring_index];
			const float missile_t = std::clamp((page_elapsed
				- (page == 1 ? (available - 1) * 56.0f : 0.0f)
				- missile * 56.0f) / 800.0f, 0.0f, 1.0f);
			const float missile_sink = page == 1
				? (1.0f - missile_t) * (1.0f - missile_t) : missile_t * missile_t;
			if (missile_sink == 1.0f)
			{
				continue;
			}
			const glm::vec3 position{
				1.499999f + ring.x * 0.078125f,
				3.749997f + ring.z + 2.0f * missile_sink,
				1.449999f - ring.y * 0.078125f,
			};
			const glm::mat3 orientation = frontend::loadout_selector_orientation(position);
			const glm::mat4 transform =
				sl_open::math::model_transform(
					orientation,
					0.007f * disc_scale,
					position);
			model_renderer_submit_preview(
				renderer, renderer.loadout_renderer.missile_models[missile],
				kLoadoutView, transform,
				false,
				glm::vec3{std::min(brightness, 1.0f)},
				{0.0f, 0.0f, 0.0f, 1.0f});
		}
	}

	const auto& panels = frontend::kLoadoutButtons;
	for (std::uint8_t panel_index = 0;
		panel_index < std::size(panels);
		++panel_index)
	{
		const frontend::LoadoutButtonDefinition& panel = panels[panel_index];
		if (panel.missile_only && page != 1 && previous_page != 1)
		{
			continue;
		}
		if (panel_index == 1 && state.blink_launch)
		{
			continue;
		}
		// 0x00449d90 starts both missile-page buttons together, without
		// the 100 ms stagger used by the four main buttons.
		const auto progress = button_progress(
			panel.missile_only && !state.reverse ? page_elapsed
				: activation_elapsed * 2000.0f - (panel.missile_only ? 0.0f : panel_index * 100.0f),
			state.reverse || (panel.missile_only && page != 1));
		const glm::mat4 panel_transform = sl_open::math::model_transform(
			sl_open::math::rotation_from_euler(
				{0.0f, (1.0f - progress.position) * glm::two_pi<float>(), 0.0f}),
			progress.scale,
			{panel.x, panel.y, state.pressed_button == (panel_index == 0 ? 1 : (panel_index == 1 ? 0 : panel_index))
				? 0.2f : 0.0f})
			* glm::translate(glm::mat4{1.0f}, glm::vec3{-panel.width * 0.5f, -panel.height * 0.5f, 0.0f})
			* glm::scale(glm::mat4{1.0f}, glm::vec3{panel.width, panel.height, 1.0f});
		const float panel_uv[] = {panel.b + 0.001953125f, panel.a + 0.001953125f,
			panel.d - panel.b, panel.c - panel.a};
		bgfx::setTransform(glm::value_ptr(panel_transform));
		bgfx::setUniform(renderer.uv_rect_uniform, panel_uv);
		bgfx::setUniform(renderer.tint_uniform, tint);
		bgfx::setVertexBuffer(0, renderer.quad_vertices);
		bgfx::setIndexBuffer(renderer.quad_indices);
		bgfx::setTexture(0, renderer.texture_sampler, renderer.loadout_renderer.panels.handle);
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
			| BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_ADD);
		bgfx::submit(kLoadoutView, renderer.rgba_program);
	}

	// Retail creates these two arrow panels but never starts their rotation
	// animations or registers scroll callbacks (0x00443c20).
	constexpr glm::vec4 arrow_uv[] = {
		{0.49609375f, 0.3828125f, 0.1875f, 0.09375f},
		{0.49609375f, 0.48046875f, 0.1875f, 0.09375f},
	};
	for (std::uint8_t arrow = 0; arrow < 2; ++arrow)
	{
		constexpr float width = 0.999899983f;
		constexpr float height = 0.499949991f;
		const glm::mat4 transform = math::model_transform(
			math::rotation_from_euler({0.0f, glm::pi<float>(), 0.0f}), 1.0f,
			{-9.0f, 1.2f + arrow * 0.5f, 0.0f})
			* glm::translate(glm::mat4{1.0f}, glm::vec3{-width * 0.5f, -height * 0.5f, 0.0f})
			* glm::scale(glm::mat4{1.0f}, glm::vec3{width, height, 1.0f});
		const glm::vec4 uv = arrow_uv[arrow] + glm::vec4{0.001953125f, 0.001953125f, 0.0f, 0.0f};
		bgfx::setTransform(glm::value_ptr(transform));
		bgfx::setUniform(renderer.uv_rect_uniform, glm::value_ptr(uv));
		bgfx::setUniform(renderer.tint_uniform, tint);
		bgfx::setVertexBuffer(0, renderer.quad_vertices);
		bgfx::setIndexBuffer(renderer.quad_indices);
		bgfx::setTexture(0, renderer.texture_sampler, renderer.loadout_renderer.panels.handle);
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
			| BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_ADD);
		bgfx::submit(kLoadoutView, renderer.rgba_program);
	}

	auto submit_panel = [&renderer, &tint, layout_progress, activation](
		float width, float height, float x, float y, float rotation_x,
		float a, float b, float c, float d) {
		const glm::mat4 transform = sl_open::math::model_transform(
			sl_open::math::rotation_from_euler(
				{rotation_x, (1.0f - layout_progress) * glm::two_pi<float>(), 0.0f}),
			0.001f + 0.999f * activation,
			{x, y, 0.0f})
			* glm::translate(glm::mat4{1.0f}, glm::vec3{-width * 0.5f, -height * 0.5f, 0.0f})
			* glm::scale(glm::mat4{1.0f}, glm::vec3{width, height, 1.0f});
		const float uv[] = {b + 0.001953125f, a + 0.001953125f, d - b, c - a};
		bgfx::setTransform(glm::value_ptr(transform));
		bgfx::setUniform(renderer.uv_rect_uniform, uv);
		bgfx::setUniform(renderer.tint_uniform, tint);
		bgfx::setVertexBuffer(0, renderer.quad_vertices);
		bgfx::setIndexBuffer(renderer.quad_indices);
		bgfx::setTexture(0, renderer.texture_sampler, renderer.loadout_renderer.panels.handle);
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
			| BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_ADD);
		bgfx::submit(kLoadoutView, renderer.rgba_program);
	};
	submit_panel(9.9989996f, 0.83324999f, -8.0f, -8.7f, 0.0f,
		0.0f, 0.0f, 0.09375f, 0.99609375f);
	submit_panel(8.9990997f, 0.66659999f, -3.9f, -7.9f, name_flip,
		0.08984375f, 0.0f, 0.1796875f, 0.99609375f);

	struct PanelSpace
	{
		float width;
		float height;
		float x;
		float y;
		float source_y;
		float source_height;
	};
	constexpr PanelSpace panel_spaces[] = {
		{9.9989996f, 0.83324999f, -8.0f, -8.7f, 0.0f, 24.0f},
		{8.9990997f, 0.66659999f, -3.9f, -7.9f, 23.0f, 23.0f},
		{9.3323994f, 9.3323994f, -8.6f, -2.6f, 0.0f, 256.0f},
	};
	auto submit_panel_quad = [
		&renderer,
		&tint,
		&panel_spaces,
		layout_progress,
		activation,
		name_flip,
		info_flip](
		std::uint8_t panel,
		float pixel_x,
		float pixel_y,
		float pixel_width,
		float pixel_height,
		float u0,
		float v0,
		float u1,
		float v1,
		bgfx::TextureHandle texture,
		bgfx::ProgramHandle program,
		bgfx::TextureHandle palette,
		const float* draw_tint,
		bool replace_pixels) {
		const PanelSpace& space = panel_spaces[panel];
		const float width = pixel_width * space.width / 256.0f;
		const float height = pixel_height * space.height / space.source_height;
		const glm::vec3 origin{
			-space.width * 0.5f + pixel_x * space.width / 256.0f,
			-space.height * 0.5f + (pixel_y - space.source_y) * space.height / space.source_height,
			0.0f};
		const glm::mat4 transform = sl_open::math::model_transform(
			sl_open::math::rotation_from_euler(
				{panel == 1 ? name_flip : (panel == 2 ? info_flip : 0.0f),
					(1.0f - layout_progress) * glm::two_pi<float>(), 0.0f}),
			0.001f + 0.999f * activation,
			{space.x, space.y, 0.0f})
			* glm::translate(glm::mat4{1.0f}, origin)
			* glm::scale(glm::mat4{1.0f}, glm::vec3{width, height, 1.0f});
		const float uv[] = {u0, v0, u1 - u0, v1 - v0};
		bgfx::setTransform(glm::value_ptr(transform));
		bgfx::setUniform(renderer.uv_rect_uniform, uv);
		bgfx::setUniform(renderer.tint_uniform, draw_tint == nullptr ? tint : draw_tint);
		bgfx::setVertexBuffer(0, renderer.quad_vertices);
		bgfx::setIndexBuffer(renderer.quad_indices);
		bgfx::setTexture(0, renderer.texture_sampler, texture);
		if (bgfx::isValid(palette))
		{
			bgfx::setTexture(
				1, renderer.palette_sampler, palette);
		}
		bgfx::setState(
			BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_DEPTH_TEST_LESS
			| (replace_pixels
				? BGFX_STATE_BLEND_ALPHA
				: BGFX_STATE_BLEND_ADD));
		bgfx::submit(kLoadoutView, program);
	};
	for (std::uint32_t following = scene_index + 1;
		following < commands.count;
		++following)
	{
		const FrontendCommand& overlay = commands.items[following];
		if (overlay.type == FrontendCommandType::loadout_text)
		{
			const std::uint8_t panel = static_cast<std::uint8_t>(
				std::clamp(overlay.height, 0.0f, 2.0f));
			const bool title_font = panel != 2;
			const FrontendTexture& atlas = title_font
				? renderer.loadout.title_font
				: renderer.loadout.info_font;
			const FrontendGlyph* glyphs = title_font
				? renderer.loadout.title_glyphs
				: renderer.loadout.info_glyphs;
			const std::uint32_t glyph_count = title_font
				? renderer.loadout.title_glyph_count
				: renderer.loadout.info_glyph_count;
			const float font_height = static_cast<float>(
				title_font
					? renderer.loadout.title_font_height
					: renderer.loadout.info_font_height);
			if (panel == 2 && overlay.y >= 256.0f)
			{
				continue;
			}
			const float visible_height = panel == 2 ? std::min(font_height, 256.0f - overlay.y) : font_height;
			float text_x = overlay.x;
			for (const auto* character =
				reinterpret_cast<const std::uint8_t*>(overlay.text);
				*character != 0;
				++character)
			{
				if (*character >= glyph_count)
				{
					continue;
				}
				const FrontendGlyph& glyph = glyphs[*character];
				submit_panel_quad(
					panel,
					text_x,
					overlay.y,
					static_cast<float>(glyph.width),
					visible_height,
					static_cast<float>(glyph.x) / atlas.width,
					static_cast<float>(glyph.y) / atlas.height,
					static_cast<float>(glyph.x + glyph.width)
						/ atlas.width,
					static_cast<float>(
						glyph.y + visible_height) / atlas.height,
					atlas.handle,
					renderer.indexed_program,
					title_font
						? renderer.loadout.title_font_palette
						: renderer.loadout.info_font_palette,
					nullptr,
					title_font);
				text_x += glyph.width;
			}
		}
		else if (overlay.type == FrontendCommandType::loadout_bar)
		{
			const float bar_tint[] = {0.0f, 201.0f / 255.0f, 79.0f / 255.0f, 1.0f};
			const auto bar = [&](float x, float y, float w, float h) {
				submit_panel_quad(2, x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f,
					renderer.white.handle, renderer.rgba_program, BGFX_INVALID_HANDLE,
					bar_tint, false);
			};
			if (overlay.rgba != 0)
			{
				bar(overlay.x, overlay.y, overlay.width, overlay.height);
			}
			else
			{
				bar(overlay.x, overlay.y, overlay.width, 1.0f);
				bar(overlay.x, overlay.y + overlay.height - 1.0f, overlay.width, 1.0f);
				bar(overlay.x, overlay.y + 1.0f, 1.0f, overlay.height - 2.0f);
				bar(overlay.x + overlay.width - 1.0f, overlay.y + 1.0f, 1.0f, overlay.height - 2.0f);
			}
		}
	}
}
}
