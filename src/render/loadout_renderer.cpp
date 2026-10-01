#include "render/loadout_renderer.hpp"

#include "assets/image.hpp"
#include "assets/ship_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace sl_open::render
{
namespace
{
constexpr bgfx::ViewId kLoadoutView = 11;

// Retail table at 0x004ea480. Values index the 13-point layout directly;
// zero is the enlarged center position, so carousel slots start at one.
constexpr std::int8_t kMissileCarouselSlots[4][10] = {
	{3, -1, 4, 5, 7, -1, -1, -1, -1, 6},
	{2, 9, 3, 4, 8, 6, -1, 7, -1, 5},
	{1, 8, 2, 3, 7, 5, -1, 6, 9, 4},
	{1, 8, 2, 3, 7, 5, 10, 6, 9, 4},
};

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

bool upload_mesh(
	const bgfx::VertexLayout& layout,
	const assets::ShipRenderMesh& source,
	LoadoutMesh& destination)
{
	if (source.vertex_count == 0
		|| source.index_count == 0
		|| source.vertices.size > UINT32_MAX
		|| source.indices.size > UINT32_MAX)
	{
		return false;
	}
	destination.vertices = bgfx::createVertexBuffer(
		bgfx::copy(
			source.vertices.data,
			static_cast<std::uint32_t>(source.vertices.size)),
		layout);
	destination.indices = bgfx::createIndexBuffer(
		bgfx::copy(
			source.indices.data,
			static_cast<std::uint32_t>(source.indices.size)),
		BGFX_BUFFER_INDEX32);
	destination.index_count = source.index_count;
	return bgfx::isValid(destination.vertices)
		&& bgfx::isValid(destination.indices);
}

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
	const assets::ShipModel (&ships)[12],
	const assets::ShipModel (&guns)[12],
	const assets::TextureImage (&ship_textures)[12],
	const assets::ShipModel (&missiles)[10],
	const assets::TextureImage& missile_texture)
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
		if (!upload_mesh(
				renderer.layout,
				ships[ship].render,
				destination.ship_models[ship])
			|| !upload_mesh(
				renderer.layout,
				guns[ship].render,
				destination.gun_models[ship])
			|| !frontend_texture_upload(
				destination.ship_textures[ship],
				ship_textures[ship]))
		{
			loadout_renderer_shutdown(destination);
			return false;
		}
		destination.ship_hardpoint_counts[ship] =
			std::min<std::uint32_t>(ships[ship].hardpoint_count, 20);
		for (std::uint32_t hardpoint = 0;
			hardpoint < destination.ship_hardpoint_counts[ship];
			++hardpoint)
		{
			const assets::ShipHardpoint& source =
				ships[ship].hardpoints[hardpoint];
			LoadoutHardpoint& output =
				destination.ship_hardpoints[ship][hardpoint];
			output.position = source.position;
			output.basis = source.basis;
			std::copy(
				std::begin(source.default_loadout),
				std::end(source.default_loadout),
				std::begin(output.default_loadout));
		}
	}
	for (std::uint32_t missile = 0; missile < 10; ++missile)
	{
		if (!upload_mesh(
				renderer.layout,
				missiles[missile].render,
				destination.missile_models[missile]))
		{
			loadout_renderer_shutdown(destination);
			return false;
		}
	}
	if (!frontend_texture_upload(
			destination.missile_texture, missile_texture))
	{
		loadout_renderer_shutdown(destination);
		return false;
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
	auto invalidate_meshes = [](auto& meshes)
	{
		for (LoadoutMesh& mesh : meshes)
		{
			mesh = {};
		}
	};
	invalidate_meshes(source.ship_models);
	invalidate_meshes(source.gun_models);
	invalidate_meshes(source.missile_models);
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
	for (FrontendTexture& texture : renderer.ship_textures)
	{
		frontend_texture_shutdown(texture);
	}
	auto destroy_meshes = [](auto& meshes)
	{
		for (LoadoutMesh& mesh : meshes)
		{
			if (bgfx::isValid(mesh.vertices))
			{
				bgfx::destroy(mesh.vertices);
			}
			if (bgfx::isValid(mesh.indices))
			{
				bgfx::destroy(mesh.indices);
			}
			mesh = {};
		}
	};
	destroy_meshes(renderer.ship_models);
	destroy_meshes(renderer.gun_models);
	destroy_meshes(renderer.missile_models);
	frontend_texture_shutdown(renderer.missile_texture);
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
			const glm::vec3 eye{-2.8f, -6.5f, -16.85f};
			const glm::vec3 forward{
				0.06694987f, 0.35894290f, 0.93095529f};
			const glm::vec3 up{
				-0.02992819f, 0.93335134f, -0.35771443f};
			const glm::mat4 scene_view =
				glm::lookAtLH(eye, eye + forward, up);
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

			constexpr float scales[12] = {
				0.009f, 0.010f, 0.008f, 0.009f,
				0.0084f, 0.010f, 0.007f, 0.0067f,
				0.006f, 0.0071f, 0.006f, 0.008f,
			};
			constexpr glm::vec3 positions[12] = {
				{-98.0f, -23.0f, -1.0f},
				{-99.0f, 11.0f, -1.0f},
				{-89.0f, 42.0f, -1.0f},
				{-72.0f, 67.0f, -1.0f},
				{-48.0f, 85.0f, -1.0f},
				{-18.0f, 97.0f, -1.0f},
				{17.0f, 97.0f, -1.0f},
				{48.0f, 85.0f, -1.0f},
				{71.0f, 67.0f, -1.0f},
				{89.0f, 42.0f, -1.0f},
				{97.0f, 11.0f, -1.0f},
				{96.0f, -23.0f, -1.0f},
			};
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
			const float disc_scale = std::max(
				0.001f,
				activation_reverse
					? 1.0f - std::sqrt(activation_elapsed)
					: activation_elapsed * activation_elapsed);
			const float glow_scale = std::max(
				0.001f, cosine_ease(activation));
			const auto button_progress = [
				activation_reverse,
				activation_elapsed,
				&cosine_ease](std::uint8_t button) {
				const float elapsed_ms = activation_elapsed * 2000.0f;
				const float time = std::clamp(
					(elapsed_ms - button * 100.0f) / 500.0f,
					0.0f,
					1.0f);
				struct Progress
				{
					float position;
					float scale;
				};
				return activation_reverse
					? Progress{
						1.0f - cosine_ease(time),
						std::max(0.001f, 1.0f - time * time)}
					: Progress{
						cosine_ease(time),
						std::max(0.001f, time * time)};
			};
			const float disc_x = 1.499999f * layout_progress;
			const float disc_y = 3.749997f * layout_progress;
			const float disc_z =
				-2.0f + (1.449999f + 2.0f) * layout_progress;
			const std::uint8_t page = state.page;
			const std::uint8_t previous_page = state.previous_page;
			const float page_transition = state.page_transition;
			auto page_weight = [
				page, previous_page, page_transition](
				std::uint8_t requested) {
				if (page == previous_page)
				{
					return page == requested ? 1.0f : 0.0f;
				}
				if (page == requested)
				{
					return page_transition;
				}
				if (previous_page == requested)
				{
					return 1.0f - page_transition;
				}
				return 0.0f;
			};
			const float missile_page = cosine_ease(page_weight(1));
			const float gun_page = page_weight(2);
			const float page_flip = page == previous_page
				? 0.0f
				: std::sin(page_transition * 3.14159265358979323846f)
					* 1.57079632679489661923f;
			const float uv_rect[] = {0.0f, 0.0f, 1.0f, 1.0f};
			const float tint[] = {
				std::min(brightness, 1.0f),
				std::min(brightness, 1.0f),
				std::min(brightness, 1.0f),
				1.0f,
			};
			bgfx::setUniform(renderer.uv_rect_uniform, uv_rect);
			bgfx::setUniform(renderer.tint_uniform, tint);

			// The authored UV corners are retained in the loadout GPU mesh.
			const glm::mat4 disc_transform = sl_open::math::srt(
				glm::vec3{disc_scale},
				{
					glm::half_pi<float>(),
					0.0f,
					(layout_progress - 1.0f) * glm::two_pi<float>(),
				},
				{disc_x, disc_y, disc_z});
			for (std::uint32_t section = 0; section < 4; ++section)
			{
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
				const glm::vec3 model_position{
					disc_x + local.x * layout_progress,
					disc_y + local.z * layout_progress,
					disc_z - local.y * layout_progress,
				};
				const float scale = scales[item]
					* (0.1944444444f
						+ (1.0f - 0.1944444444f)
							* center_scale_weight)
					* (is_selected
						? 1.0f
						: 1.0f - std::max(missile_page, gun_page))
					* disc_scale;
				const glm::vec3 ring_position{
					1.499999f + ring.x * 0.078125f,
					3.749997f + ring.z,
					1.449999f - ring.y * 0.078125f,
				};
				const glm::vec3 direction = glm::normalize(
					glm::vec3{1.499999f, 3.749997f, 1.449999f}
						- ring_position);
				glm::mat3 ring_orientation{1.0f};
				const float heading =
					std::atan2(direction.x, direction.z);
				ring_orientation = sl_open::math::postrotate(
					ring_orientation,
					heading,
					{0.0f, 1.0f, 0.0f});
				const float transformed_z =
					glm::dot(ring_orientation[2], direction);
				const float elevation = glm::pi<float>()
					- std::atan2(direction.y, transformed_z);
				ring_orientation = sl_open::math::postrotate(
					ring_orientation,
					elevation,
					{1.0f, 0.0f, 0.0f});
				ring_orientation = sl_open::math::postrotate(
					ring_orientation,
					glm::pi<float>(),
					{0.0f, 0.0f, 1.0f});
				glm::mat3 center_orientation{1.0f};
				const float center_spin = ship_selection_active
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
				const LoadoutMesh& mesh =
					renderer.loadout_renderer.ship_models[item];
				bgfx::setTransform(glm::value_ptr(transform));
				bgfx::setVertexBuffer(0, mesh.vertices);
				bgfx::setIndexBuffer(mesh.indices, 0, mesh.index_count);
				bgfx::setTexture(
					0,
					renderer.texture_sampler,
					renderer.loadout_renderer.ship_textures[item].handle,
					BGFX_SAMPLER_MIN_POINT
						| BGFX_SAMPLER_MAG_POINT
						| BGFX_SAMPLER_MIP_POINT);
				bgfx::setState(
					BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_WRITE_Z
					| BGFX_STATE_DEPTH_TEST_LESS
					| BGFX_STATE_BLEND_ALPHA);
				bgfx::submit(kLoadoutView, renderer.rgba_program);

				if (is_selected && gun_page > 0.0f)
				{
					const glm::mat4 gun_transform =
						sl_open::math::model_transform(
							orientation,
							scale * gun_page,
							model_position);
					const LoadoutMesh& gun =
						renderer.loadout_renderer.gun_models[item];
					bgfx::setTransform(glm::value_ptr(gun_transform));
					bgfx::setVertexBuffer(0, gun.vertices);
					bgfx::setIndexBuffer(
						gun.indices, 0, gun.index_count);
					bgfx::setTexture(
						0,
						renderer.texture_sampler,
						renderer.loadout_renderer.ship_textures[item].handle,
						BGFX_SAMPLER_MIN_POINT
							| BGFX_SAMPLER_MAG_POINT
							| BGFX_SAMPLER_MIP_POINT);
					bgfx::setState(
						BGFX_STATE_WRITE_RGB
							| BGFX_STATE_WRITE_A
							| BGFX_STATE_WRITE_Z
							| BGFX_STATE_DEPTH_TEST_LESS
							| BGFX_STATE_BLEND_ALPHA);
					bgfx::submit(kLoadoutView, renderer.rgba_program);
				}

				if (is_selected)
				{
					const std::uint32_t hardpoint_count = std::min(
						renderer.loadout_renderer.ship_hardpoint_counts[item],
						20u);
					for (std::uint32_t hardpoint_index = 0;
						hardpoint_index < hardpoint_count;
						++hardpoint_index)
					{
						const LoadoutHardpoint& hardpoint =
							renderer.loadout_renderer.ship_hardpoints[item]
								[hardpoint_index];
						const std::int32_t missile_id = state.use_default_loadout
							? hardpoint.default_loadout[
								state.missile_layout_tier]
							: state.mounted_loadout[hardpoint_index];

						glm::mat3 attachment_orientation =
							orientation * hardpoint.basis;
						const glm::vec3 attachment_position =
							model_position
								+ orientation * hardpoint.position * scale;
						if (missile_id < 0 || missile_id >= 10)
						{
							if (missile_page < 0.999f)
							{
								continue;
							}
							const glm::mat4 hardpoint_transform =
								sl_open::math::model_transform(
									glm::mat3{1.0f},
									state.hardpoint_zoom,
									attachment_position
										+ attachment_orientation[1]
											* (0.05f * scale));
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
								kMissileCarouselSlots[
									state.missile_layout_tier][
										animation_missile];
							if (carousel_slot > 0)
							{
								const std::uint8_t ring_index =
									static_cast<std::uint8_t>(
										carousel_slot - 1);
								const glm::vec3 carousel =
									positions[ring_index];
								source_position = {
									1.499999f + carousel.x * 0.078125f,
									3.749997f + carousel.z,
									1.449999f - carousel.y * 0.078125f,
								};
								const glm::vec3 source_direction =
									glm::normalize(
										glm::vec3{
											1.499999f,
											3.749997f,
											1.449999f}
											- source_position);
								source_orientation = sl_open::math::postrotate(
									source_orientation,
									std::atan2(
										source_direction.x,
										source_direction.z),
									{0.0f, 1.0f, 0.0f});
								const float transformed_z =
									glm::dot(
										source_orientation[2],
										source_direction);
								source_orientation = sl_open::math::postrotate(
									source_orientation,
									glm::pi<float>()
										- std::atan2(
											source_direction.y,
											transformed_z),
									{1.0f, 0.0f, 0.0f});
								source_orientation = sl_open::math::postrotate(
									source_orientation,
									glm::pi<float>(),
									{0.0f, 0.0f, 1.0f});
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
							const LoadoutMesh& animation_mesh =
								renderer.loadout_renderer.missile_models[
									animation_missile];
							bgfx::setTransform(
								glm::value_ptr(animation_transform));
							bgfx::setVertexBuffer(
								0, animation_mesh.vertices);
							bgfx::setIndexBuffer(
								animation_mesh.indices,
								0,
								animation_mesh.index_count);
							bgfx::setTexture(
								0,
								renderer.texture_sampler,
								renderer.loadout_renderer.missile_texture.handle,
								BGFX_SAMPLER_MIN_POINT
									| BGFX_SAMPLER_MAG_POINT
									| BGFX_SAMPLER_MIP_POINT);
							bgfx::setState(
								BGFX_STATE_WRITE_RGB
									| BGFX_STATE_WRITE_A
									| BGFX_STATE_WRITE_Z
									| BGFX_STATE_DEPTH_TEST_LESS
									| BGFX_STATE_BLEND_ALPHA);
							bgfx::submit(
								kLoadoutView, renderer.rgba_program);
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
								(scale * 0.8f)
									+ (0.007f - scale * 0.8f)
										* missile_page,
								attachment_position);
						const LoadoutMesh& missile =
							renderer.loadout_renderer.missile_models[missile_id];
						bgfx::setTransform(
							glm::value_ptr(attachment_transform));
						bgfx::setVertexBuffer(0, missile.vertices);
						bgfx::setIndexBuffer(
							missile.indices, 0, missile.index_count);
						bgfx::setTexture(
							0,
							renderer.texture_sampler,
							renderer.loadout_renderer.missile_texture.handle,
							BGFX_SAMPLER_MIN_POINT
								| BGFX_SAMPLER_MAG_POINT
								| BGFX_SAMPLER_MIP_POINT);
						bgfx::setState(
							BGFX_STATE_WRITE_RGB
								| BGFX_STATE_WRITE_A
								| BGFX_STATE_WRITE_Z
								| BGFX_STATE_DEPTH_TEST_LESS
								| BGFX_STATE_BLEND_ALPHA);
						bgfx::submit(kLoadoutView, renderer.rgba_program);
						if (state.hovered_hardpoint
							== static_cast<std::int8_t>(hardpoint_index))
						{
							const float hover_tint[] = {
								0.25f, 0.55f, 0.25f, 1.0f};
							bgfx::setTransform(
								glm::value_ptr(attachment_transform));
							bgfx::setUniform(
								renderer.tint_uniform, hover_tint);
							bgfx::setVertexBuffer(0, missile.vertices);
							bgfx::setIndexBuffer(
								missile.indices, 0, missile.index_count);
							bgfx::setTexture(
								0,
								renderer.texture_sampler,
								renderer.white.handle,
								BGFX_SAMPLER_MIN_POINT
									| BGFX_SAMPLER_MAG_POINT
									| BGFX_SAMPLER_MIP_POINT);
							bgfx::setState(
								BGFX_STATE_WRITE_RGB
									| BGFX_STATE_WRITE_A
									| BGFX_STATE_DEPTH_TEST_LEQUAL
									| BGFX_STATE_BLEND_ADD);
							bgfx::submit(
								kLoadoutView, renderer.rgba_program);
							bgfx::setUniform(renderer.tint_uniform, tint);
						}
					}
				}
			}

			if (missile_page > 0.0f)
			{
				for (std::uint8_t missile = 0; missile < 10; ++missile)
				{
					if ((state.missile_mask & (1u << missile)) == 0)
					{
						continue;
					}
					const std::int8_t carousel_slot =
						kMissileCarouselSlots[
							state.missile_layout_tier][missile];
					if (carousel_slot <= 0)
					{
						continue;
					}
					const std::uint8_t ring_index =
						static_cast<std::uint8_t>(
							carousel_slot - 1);
					const glm::vec3 ring = positions[ring_index];
					const glm::vec3 position{
						1.499999f + ring.x * 0.078125f,
						3.749997f + ring.z,
						1.449999f - ring.y * 0.078125f,
					};
					const glm::vec3 direction = glm::normalize(
						glm::vec3{1.499999f, 3.749997f, 1.449999f}
							- position);
					glm::mat3 orientation{1.0f};
					orientation = sl_open::math::postrotate(
						orientation,
						std::atan2(direction.x, direction.z),
						{0.0f, 1.0f, 0.0f});
					const float transformed_z =
						glm::dot(orientation[2], direction);
					orientation = sl_open::math::postrotate(
						orientation,
						glm::pi<float>()
							- std::atan2(direction.y, transformed_z),
						{1.0f, 0.0f, 0.0f});
					orientation = sl_open::math::postrotate(
						orientation,
						glm::pi<float>(),
						{0.0f, 0.0f, 1.0f});
					const glm::mat4 transform =
						sl_open::math::model_transform(
							orientation,
							0.007f * missile_page * activation,
							position);
					const LoadoutMesh& mesh =
						renderer.loadout_renderer.missile_models[missile];
					bgfx::setTransform(glm::value_ptr(transform));
					bgfx::setVertexBuffer(0, mesh.vertices);
					bgfx::setIndexBuffer(
						mesh.indices, 0, mesh.index_count);
					bgfx::setTexture(
						0,
						renderer.texture_sampler,
						renderer.loadout_renderer.missile_texture.handle,
						BGFX_SAMPLER_MIN_POINT
							| BGFX_SAMPLER_MAG_POINT
							| BGFX_SAMPLER_MIP_POINT);
					bgfx::setState(
						BGFX_STATE_WRITE_RGB
							| BGFX_STATE_WRITE_A
							| BGFX_STATE_WRITE_Z
							| BGFX_STATE_DEPTH_TEST_LESS
							| BGFX_STATE_BLEND_ALPHA);
					bgfx::submit(kLoadoutView, renderer.rgba_program);
				}
			}

			struct Panel
			{
				float width;
				float height;
				float x;
				float y;
				float a;
				float b;
				float c;
				float d;
				bool missile_only;
			};
			constexpr Panel panels[] = {
				{2.33309984f, 1.66649997f, 10.0f, -3.8f,
					0.37890625f, 0.234375f, 0.578125f, 0.47265625f,
					false},
				{2.33309984f, 1.66649997f, 10.0f, -8.35f,
					0.1796875f, 0.47265625f, 0.37890625f, 0.7109375f,
					false},
				{2.33309984f, 1.66649997f, 10.0f, -5.8f,
					0.1796875f, 0.0f, 0.37890625f, 0.234375f,
					false},
				{2.33309984f, 1.66649997f, 10.0f, -1.8f,
					0.37890625f, 0.0f, 0.578125f, 0.234375f,
					false},
				{2.33309984f, 1.66649997f, 4.2f, -8.35f,
					0.1796875f, 0.7109375f, 0.37890625f, 0.94921875f,
					true},
				{2.33309984f, 1.66649997f, 6.7f, -8.35f,
					0.1796875f, 0.234375f, 0.37890625f, 0.47265625f,
					true},
			};
			for (std::uint8_t panel_index = 0;
				panel_index < std::size(panels);
				++panel_index)
			{
				const Panel& panel = panels[panel_index];
				if (panel.missile_only && missile_page < 0.999f)
				{
					continue;
				}
				const auto progress = button_progress(panel_index);
				const float half_width = panel.width * 0.5f;
				const float half_height = panel.height * 0.5f;
				const float epsilon = 0.001953125f;
				const float u0 = panel.b + epsilon;
				const float u1 = panel.d + epsilon;
				const float v0 = panel.a + epsilon;
				const float v1 = panel.c + epsilon;
				const FrontendVertex vertices[6] = {
					{-half_width, half_height, 0.0f, 0xffffffff, u0, v1},
					{half_width, half_height, 0.0f, 0xffffffff, u1, v1},
					{-half_width, -half_height, 0.0f, 0xffffffff, u0, v0},
					{half_width, half_height, 0.0f, 0xffffffff, u1, v1},
					{half_width, -half_height, 0.0f, 0xffffffff, u1, v0},
					{-half_width, -half_height, 0.0f, 0xffffffff, u0, v0},
				};
				FrameVertexBuffer vertex_buffer;
				if (get_available_frame_vertices(renderer.frame_geometry,
						6, renderer.layout) >= 6)
				{
					alloc_frame_vertex_buffer(renderer.frame_geometry,
						&vertex_buffer, 6, renderer.layout);
					std::memcpy(
						vertex_buffer.data, vertices, sizeof(vertices));
					const glm::mat4 panel_transform = sl_open::math::srt(
						glm::vec3{progress.scale},
						{
							0.0f,
							0.0f,
							(1.0f - progress.position)
								* glm::two_pi<float>(),
						},
						{
							panel.x * progress.position,
							panel.y * progress.position,
							0.0f,
						});
					const float full_uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
					bgfx::setTransform(glm::value_ptr(panel_transform));
					bgfx::setUniform(renderer.uv_rect_uniform, full_uv);
					bgfx::setUniform(renderer.tint_uniform, tint);
					set_frame_vertex_buffer(0, &vertex_buffer);
					bgfx::setTexture(
						0,
						renderer.texture_sampler,
						renderer.loadout_renderer.panels.handle);
					bgfx::setState(
						BGFX_STATE_WRITE_RGB
						| BGFX_STATE_WRITE_A
						| BGFX_STATE_DEPTH_TEST_LESS
						| BGFX_STATE_BLEND_ADD);
					bgfx::submit(kLoadoutView, renderer.rgba_program);
				}
			}

			auto submit_panel = [
				&renderer, &tint, layout_progress, glow_scale](
				const FrontendTexture& texture,
				float width,
				float height,
				float x,
				float y,
				float z,
				float rotation_y,
				float a,
				float b,
				float c,
				float d) {
				const float half_width = width * 0.5f;
				const float half_height = height * 0.5f;
				const float epsilon = 0.001953125f;
				const FrontendVertex vertices[6] = {
					{-half_width, half_height, 0.0f, 0xffffffff,
						b + epsilon, c + epsilon},
					{half_width, half_height, 0.0f, 0xffffffff,
						d + epsilon, c + epsilon},
					{-half_width, -half_height, 0.0f, 0xffffffff,
						b + epsilon, a + epsilon},
					{half_width, half_height, 0.0f, 0xffffffff,
						d + epsilon, c + epsilon},
					{half_width, -half_height, 0.0f, 0xffffffff,
						d + epsilon, a + epsilon},
					{-half_width, -half_height, 0.0f, 0xffffffff,
						b + epsilon, a + epsilon},
				};
				FrameVertexBuffer vertex_buffer;
				if (get_available_frame_vertices(renderer.frame_geometry,
						6, renderer.layout) < 6)
				{
					return;
				}
				alloc_frame_vertex_buffer(renderer.frame_geometry,
					&vertex_buffer, 6, renderer.layout);
				std::memcpy(
					vertex_buffer.data, vertices, sizeof(vertices));
				const glm::mat4 transform = sl_open::math::srt(
					glm::vec3{glow_scale},
					{
						0.0f,
						(1.0f - layout_progress) * glm::two_pi<float>()
							+ rotation_y,
						0.0f,
					},
					{
						x * layout_progress,
						y * layout_progress,
						z * layout_progress,
					});
				const float full_uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
				bgfx::setTransform(glm::value_ptr(transform));
				bgfx::setUniform(renderer.uv_rect_uniform, full_uv);
				bgfx::setUniform(renderer.tint_uniform, tint);
				set_frame_vertex_buffer(0, &vertex_buffer);
				bgfx::setTexture(
					0, renderer.texture_sampler, texture.handle);
				bgfx::setState(
					BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_DEPTH_TEST_LESS
					| BGFX_STATE_BLEND_ADD);
				bgfx::submit(kLoadoutView, renderer.rgba_program);
			};
			submit_panel(
				renderer.loadout_renderer.panels,
				9.9989996f,
				0.83324999f,
				-8.0f,
				-8.7f,
				0.0f,
				page_flip,
				0.0f,
				0.0f,
				0.09375f,
				0.99609375f);
			submit_panel(
				renderer.loadout_renderer.panels,
				8.9990997f,
				0.66659999f,
				-3.9f,
				-7.9f,
				0.0f,
				page_flip,
				0.08984375f,
				0.0f,
				0.1796875f,
				0.99609375f);

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
				glow_scale,
				page_flip](
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
				if (panel >= 3)
				{
					return;
				}
				const PanelSpace& space = panel_spaces[panel];
				const float local_x =
					-space.width * 0.5f
					+ (pixel_x + pixel_width * 0.5f)
						* space.width / 256.0f;
				const float local_y =
					-space.height * 0.5f
					+ (pixel_y - space.source_y + pixel_height * 0.5f)
						* space.height / space.source_height;
				const float half_width =
					pixel_width * space.width / 512.0f;
				const float half_height =
					pixel_height * space.height
					/ (space.source_height * 2.0f);
				const FrontendVertex vertices[6] = {
					{local_x - half_width, local_y + half_height, 0.0f,
						0xffffffff, u0, v1},
					{local_x + half_width, local_y + half_height, 0.0f,
						0xffffffff, u1, v1},
					{local_x - half_width, local_y - half_height, 0.0f,
						0xffffffff, u0, v0},
					{local_x + half_width, local_y + half_height, 0.0f,
						0xffffffff, u1, v1},
					{local_x + half_width, local_y - half_height, 0.0f,
						0xffffffff, u1, v0},
					{local_x - half_width, local_y - half_height, 0.0f,
						0xffffffff, u0, v0},
				};
				FrameVertexBuffer vertex_buffer;
				if (get_available_frame_vertices(renderer.frame_geometry,
						6, renderer.layout) < 6)
				{
					return;
				}
				alloc_frame_vertex_buffer(renderer.frame_geometry,
					&vertex_buffer, 6, renderer.layout);
				std::memcpy(
					vertex_buffer.data, vertices, sizeof(vertices));
				const glm::mat4 transform = sl_open::math::srt(
					glm::vec3{glow_scale},
					{
						0.0f,
						(1.0f - layout_progress) * glm::two_pi<float>()
							+ page_flip,
						0.0f,
					},
					{
						space.x * layout_progress,
						space.y * layout_progress,
						0.0f,
					});
				const float full_uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
				bgfx::setTransform(glm::value_ptr(transform));
				bgfx::setUniform(renderer.uv_rect_uniform, full_uv);
				bgfx::setUniform(
					renderer.tint_uniform,
					draw_tint == nullptr ? tint : draw_tint);
				set_frame_vertex_buffer(0, &vertex_buffer);
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
							font_height,
							static_cast<float>(glyph.x) / atlas.width,
							static_cast<float>(glyph.y) / atlas.height,
							static_cast<float>(glyph.x + glyph.width)
								/ atlas.width,
							static_cast<float>(
								glyph.y + font_height) / atlas.height,
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
					const std::uint32_t color = overlay.rgba != 0
						? 0x00c94fff
						: 0x00300fff;
					const float bar_tint[] = {
						static_cast<float>((color >> 24) & 0xff) / 255.0f,
						static_cast<float>((color >> 16) & 0xff) / 255.0f,
						static_cast<float>((color >> 8) & 0xff) / 255.0f,
						1.0f,
					};
					submit_panel_quad(
						2,
						overlay.x,
						overlay.y,
						overlay.width,
						overlay.height,
						0.0f,
						0.0f,
						1.0f,
						1.0f,
						renderer.white.handle,
						renderer.rgba_program,
						BGFX_INVALID_HANDLE,
						bar_tint,
						false);
				}
			}
}
}
