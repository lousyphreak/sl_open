#include "game/transition_effects.hpp"

#include "assets/gameplay_model.hpp"
#include "core/math.hpp"
#include "core/mission_log.hpp"
#include "game/model_animation.hpp"
#include "game/retained_components.hpp"
#include "game/world.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace sl_open::game
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr std::uint32_t kWGateEmitterDurationTicks = 1000;

float cosine_lerp(float start, float end, float phase)
{
	return glm::mix(
		start,
		end,
		std::cos(phase * kPi) * -0.5f + 0.5f);
}

float square_root_lerp(float start, float end, float phase)
{
	return glm::mix(
		start,
		end,
		phase == 0.0f ? 0.0f : std::sqrt(phase));
}

float wgate_axial_curve(float start, float end, float phase)
{
	const float factor = phase < 1.0f
		? std::cos(phase * kPi) * -0.5f + 0.5f
		: (std::cos(kPi * phase) * 0.5f + 0.5f)
				* 0.2f
			+ 0.8f;
	return glm::mix(start, end, factor);
}

float fade_after_ninety_percent(float value, float phase)
{
	return phase < 0.9f
		? value
		: glm::mix(value, 0.0f, (phase - 0.9f) * 10.0f);
}

bool initialize_wgate_emitter(
	const ParticleRuntime& particles,
	ParticleEmitter& emitter,
	ParticleEmitterStyle style,
	std::uint32_t tick,
	float speed_base,
	float speed_random)
{
	if (!particle_emitter_initialize(
			particles,
			emitter,
			static_cast<std::uint32_t>(style),
			kWGateEmitterDurationTicks,
			tick))
	{
		return false;
	}
	emitter.direction_center = {0.0f, 0.0f, -1.0f};
	emitter.direction_spread = {0.15f, 0.15f, 0.15f};
	emitter.speed_base = speed_base;
	emitter.speed_random = speed_random;
	return true;
}

std::uint8_t color_channel(float value)
{
	return static_cast<std::uint8_t>(std::lrint(
		std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

std::uint32_t pack_effect_color(
	float red,
	float green,
	float blue,
	float alpha = 1.0f)
{
	// bgfx's normalized Uint8 color attribute consumes little-endian ABGR:
	// the least-significant byte is the shader's red channel.
	return static_cast<std::uint32_t>(color_channel(alpha)) << 24
		| static_cast<std::uint32_t>(color_channel(blue)) << 16
		| static_cast<std::uint32_t>(color_channel(green)) << 8
		| color_channel(red);
}

void append_quad(
	TransitionMesh& mesh,
	const glm::vec3& a,
	const glm::vec3& b,
	const glm::vec3& c,
	const glm::vec3& d,
	std::uint32_t color)
{
	if (mesh.vertices.size() > UINT16_MAX - 4)
	{
		return;
	}
	const std::uint16_t base =
		static_cast<std::uint16_t>(mesh.vertices.size());
	mesh.vertices.push_back({a, color, {0.0f, 0.0f}});
	mesh.vertices.push_back({b, color, {1.0f, 0.0f}});
	mesh.vertices.push_back({c, color, {1.0f, 1.0f}});
	mesh.vertices.push_back({d, color, {0.0f, 1.0f}});
	mesh.base_positions.insert(mesh.base_positions.end(), {a, b, c, d});
	mesh.indices.insert(
		mesh.indices.end(),
		{base, static_cast<std::uint16_t>(base + 1),
			static_cast<std::uint16_t>(base + 2),
		 base, static_cast<std::uint16_t>(base + 2),
			static_cast<std::uint16_t>(base + 3)});
}

TransitionMesh make_jump_trail(bool large)
{
	TransitionMesh mesh;
	mesh.texture = TransitionTexture::jump_trail;
	mesh.blend = TransitionBlend::additive;
	mesh.double_sided = true;
	mesh.active = true;
	const float width = large ? 1000.0f : 100.0f;
	const float height = width;
	const float length = large ? 40000.0f : 20000.0f;
	// Object flag 0x80000 selects the runtime color stream. The constructor
	// leaves it dark; JumpTrail_set_scale animates only the three ribbons.
	// The first quad is therefore an invisible cap, not a white exhaust sheet.
	const std::uint32_t color = pack_effect_color(0.0f, 0.0f, 0.0f);
	append_quad(
		mesh,
		{-width, -height, 0.0f},
		{width, -height, 0.0f},
		{width, height, 0.0f},
		{-width, height, 0.0f},
		color);
	for (std::uint32_t ribbon = 0; ribbon < 3; ++ribbon)
	{
		const float angle = static_cast<float>(ribbon) * kPi / 3.0f;
		const glm::vec3 left{
			-std::sin(angle) * width,
			-std::cos(angle) * height,
			0.0f,
		};
		const glm::vec3 right = -left;
		append_quad(
			mesh,
			left,
			left + glm::vec3{0.0f, 0.0f, length},
			right + glm::vec3{0.0f, 0.0f, length},
			right,
			color);
	}
	// JumpTrail_mesh_create writes the same inset UV rectangle to each
	// independent quad. Its ribbon order keeps vertices zero and three at
	// the engine end so JumpTrail_set_scale can animate a clean axial glow.
	constexpr std::array<glm::vec2, 4> kTrailUv{{
		{0.99f, 0.99f},
		{0.99f, 0.04f},
		{0.04f, 0.04f},
		{0.04f, 0.99f},
	}};
	for (std::size_t index = 0; index < mesh.vertices.size(); ++index)
	{
		mesh.vertices[index].uv = kTrailUv[index & 3u];
	}
	return mesh;
}

TransitionMesh make_jump_flare()
{
	TransitionMesh mesh;
	mesh.texture = TransitionTexture::jump_flare;
	mesh.blend = TransitionBlend::additive;
	mesh.double_sided = true;
	// Jump_initialize passes the complete 4-by-2 dimensions to the common
	// quad constructor at 0x0044f000, which halves both before emitting the
	// four positions. JumpFlare_mesh_create then applies the ship's X extent
	// as the retained uniform scale.
	append_quad(
		mesh,
		{-2.0f, -1.0f, 0.0f},
		{2.0f, -1.0f, 0.0f},
		{2.0f, 1.0f, 0.0f},
		{-2.0f, 1.0f, 0.0f},
		0xffffffffu);
	return mesh;
}

TransitionMesh make_jump_burst(bool jump_in)
{
	TransitionMesh mesh;
	mesh.texture = TransitionTexture::jump_burst;
	// JumpBurst_create (LANCER.EXE 0x00417e30) assigns retail blend
	// selector four: SRCALPHA/ONE.
	mesh.blend = TransitionBlend::source_alpha_additive;
	mesh.double_sided = true;
	mesh.active = true;
	mesh.vertices.reserve(73);
	mesh.indices.reserve(360);
	mesh.vertices.push_back({
		{0.0f, 0.0f, 0.0f},
		pack_effect_color(0.0f, 0.0f, 0.0f),
		{0.5f, 0.5f},
	});
	mesh.base_positions.push_back({0.0f, 0.0f, 0.0f});
	const float axial = jump_in ? 0.1f : 0.9f;
	const float radial = jump_in ? 500.0f : 1800.0f;
	for (std::uint32_t ring = 0; ring < 6; ++ring)
	{
		const float radius = jump_in && ring == 0
			? 0.0f
			: std::sin(
				static_cast<float>(ring)
					* 0.2f * kPi * 0.5f)
				* radial * 0.6f
				+ 30.0f;
		const float z = -static_cast<float>(ring) * axial * 1800.0f;
		for (std::uint32_t column = 0; column < 12; ++column)
		{
			const float angle =
				static_cast<float>(column) * kTwoPi / 12.0f;
			const glm::vec3 position{
				std::cos(angle) * radius,
				std::sin(angle) * radius,
				z,
			};
			mesh.vertices.push_back({
				position,
				0xffffffffu,
				{position.z * 9.259259e-6f,
				 position.y * 9.259259e-5f},
			});
			mesh.base_positions.push_back(
				mesh.vertices.back().position);
		}
	}
	for (std::uint16_t ring = 0; ring < 5; ++ring)
	{
		const std::uint16_t first = static_cast<std::uint16_t>(1 + ring * 12);
		const std::uint16_t second =
			static_cast<std::uint16_t>(first + 12);
		for (std::uint16_t column = 0; column < 12; ++column)
		{
			const std::uint16_t next =
				static_cast<std::uint16_t>((column + 1) % 12);
			mesh.indices.insert(
				mesh.indices.end(),
				{static_cast<std::uint16_t>(first + column),
				 static_cast<std::uint16_t>(second + column),
				 static_cast<std::uint16_t>(second + next),
				 static_cast<std::uint16_t>(first + column),
				 static_cast<std::uint16_t>(second + next),
				 static_cast<std::uint16_t>(first + next)});
		}
	}
	return mesh;
}

glm::mat4 model_local_transform(
	const WorldObject& actor,
	std::uint16_t model_reference)
{
	return model_animation_render_transform(
		actor, model_reference, 1.0f);
}

template<class Visitor>
void visit_jump_points(
	const WorldObject& actor,
	std::int32_t type,
	Visitor&& visitor)
{
	for (std::uint16_t model_index = 0;
		model_index < actor.model_references.size();
		++model_index)
	{
		const ObjectModelReference& model =
			actor.model_references[model_index];
		if (model.point_groups == nullptr)
		{
			continue;
		}
		const glm::mat4 transform =
			model_local_transform(actor, model_index);
		for (const assets::GameplayPointGroup& group
			: *model.point_groups)
		{
			if (group.type != type)
			{
				continue;
			}
			for (const assets::GameplayPoint& point : group.points)
			{
				visitor(
					glm::vec3(
						transform
							* glm::vec4(point.position, 1.0f)),
					glm::mat3(transform),
					model_index);
			}
		}
	}
}

void place_trail(
	TransitionMesh& trail,
	const glm::vec3& position,
	bool cargo_pod_visible)
{
	trail.position = position;
	// Jump_make_trail (LANCER.EXE 0x00417af0) parents every wrapper to
	// the actor root, not the retained node which supplied its type-seven
	// point. Its only orientation exception is a Ripper whose named Cargo
	// pod node is currently visible.
	trail.orientation = cargo_pod_visible
		? glm::mat3{1.0f}
		: math::rotation_from_euler({0.0f, kPi, 0.0f});
}

void fill_portal_colors(WGateContext& context)
{
	for (std::uint32_t row = 0;
		row <= context.axial_segments;
		++row)
	{
		const float phase = context.axial_segments == 0
			? 0.0f
			: static_cast<float>(row)
				/ static_cast<float>(context.axial_segments);
		for (std::uint32_t radial = 0;
			radial < context.radial_segments;
			++radial)
		{
			const std::size_t index =
				static_cast<std::size_t>(row)
					* context.radial_segments
				+ radial;
			if (index >= context.portal.vertices.size())
			{
				continue;
			}
			float alpha = row == 0 || row == context.axial_segments
				? 0.0f
				: 1.0f;
			float red = 0.65f * phase + 0.2f;
			float green = 0.05f + 0.15f * (1.0f - phase);
			float blue = 0.85f * (1.0f - phase) + 0.1f;
			if (context.mode == WGateMode::advanced)
			{
				std::swap(red, blue);
			}
			if (row + 1 == context.axial_segments)
			{
				red = green = blue = 1.0f;
			}
			context.portal.vertices[index].color =
				pack_effect_color(red, green, blue, alpha);
		}
	}
}

void rebuild_portal_positions(WGateContext& context, float ripple)
{
	for (std::uint32_t row = 0;
		row <= context.axial_segments;
		++row)
	{
		for (std::uint32_t radial = 0;
			radial < context.radial_segments;
			++radial)
		{
			const float angle =
				static_cast<float>(radial) * kTwoPi
					/ static_cast<float>(context.radial_segments);
			const float radius =
				context.row_radii[row]
				+ std::sin(
					static_cast<float>(row) + angle + ripple)
					* (context.mode == WGateMode::ship ? 20.0f : 300.0f);
			const std::size_t index =
				static_cast<std::size_t>(row)
					* context.radial_segments
				+ radial;
			context.portal.vertices[index].position = {
				std::cos(angle) * radius,
				std::sin(angle) * radius,
				context.row_positions[row],
			};
			if (index < context.portal.base_positions.size())
			{
				context.portal.base_positions[index] =
					context.portal.vertices[index].position;
			}
		}
	}
}

TransitionMesh make_beam(float radius)
{
	TransitionMesh mesh;
	mesh.texture = TransitionTexture::warp_secondary;
	// WGate_make_beam (LANCER.EXE 0x0041d2b0) uses retail blend selector
	// four, allowing authored vertex alpha to modulate the additive source.
	mesh.blend = TransitionBlend::source_alpha_additive;
	mesh.double_sided = true;
	mesh.active = true;
	// WGate_make_beam, LANCER.EXE 0x0041d2b0, initializes every
	// material vertex to the exact compiled red/orange values.
	const std::uint32_t color =
		pack_effect_color(0.99f, 0.04f, 0.04f, 0.99f);
	append_quad(
		mesh,
		{-radius, -radius, 0.0f},
		{radius, -radius, 0.0f},
		{radius, radius, 0.0f},
		{-radius, radius, 0.0f},
		color);
	for (std::uint32_t ribbon = 0; ribbon < 3; ++ribbon)
	{
		const float angle = static_cast<float>(ribbon) * kPi / 3.0f;
		const float x = std::sin(angle) * radius;
		const float y = std::cos(angle) * radius;
		append_quad(
			mesh,
			{-x, -y, 0.0f},
			{-x, -y, 400.0f},
			{x, y, 400.0f},
			{x, y, 0.0f},
			color);
	}
	return mesh;
}

glm::mat3 look_at_points(
	const glm::vec3& from,
	const glm::vec3& to)
{
	// SR_mat3_look_at_points, LANCER.EXE 0x004c1940, with zero roll.
	glm::vec3 delta = to - from;
	glm::mat3 orientation{1.0f};
	orientation = math::postrotate(
		orientation,
		std::atan2(delta.x, delta.z),
		glm::vec3{0.0f, 1.0f, 0.0f});
	delta = glm::transpose(orientation) * delta;
	return math::postrotate(
		orientation,
		-std::atan2(delta.y, delta.z),
		glm::vec3{1.0f, 0.0f, 0.0f});
}

void create_projector_portal(
	TransitionMesh& mesh,
	std::uint32_t radial_segments,
	std::uint32_t axial_segments)
{
	// WGate_make_mesh(mode 2), LANCER.EXE 0x0041dd70. The standalone
	// center vertex is deliberately retained even though the annular
	// triangle strips do not reference it: the retail deformation indices
	// start at vertex one.
	mesh = {};
	mesh.texture = TransitionTexture::warp_primary;
	mesh.blend = TransitionBlend::additive;
	mesh.double_sided = true;
	mesh.vertices.reserve(
		1u + radial_segments * (axial_segments + 1u));
	mesh.indices.reserve(radial_segments * axial_segments * 6u);
	mesh.vertices.push_back({{}, 0u, {}});
	constexpr double exponent = 1.2000000476837158;
	const float radial_reciprocal =
		1.0f / static_cast<float>(radial_segments);
	const float axial_factor = static_cast<float>(
		(8.0 * std::pow(8.0, exponent))
			/ (static_cast<double>(axial_segments)
				* std::pow(
					static_cast<double>(axial_segments),
					exponent)));
	const float mode_zero_radius = static_cast<float>(
		std::pow(
			static_cast<double>(axial_segments), exponent)
			* static_cast<double>(axial_segments)
			* axial_factor
			* 10.0);
	for (std::uint32_t row = 0; row <= axial_segments; ++row)
	{
		const float radius = static_cast<float>(
			std::pow(
				static_cast<double>(axial_segments - row),
				exponent)
				* static_cast<double>(axial_segments)
				* axial_factor
				* 240.0f
				* 10.0);
		for (std::uint32_t radial = 0;
			radial < radial_segments;
			++radial)
		{
			const float angle =
				static_cast<float>(radial) * kTwoPi
					* radial_reciprocal;
			mesh.vertices.push_back({
				{std::sin(angle) * radius,
				 std::cos(angle) * radius,
				 static_cast<float>(row * axial_segments) * 30.0f},
				0u,
				{static_cast<float>(row) * 0.125f,
				 std::cos(angle) * mode_zero_radius * 0.001f}});
		}
	}
	for (std::uint32_t row = 0; row < axial_segments; ++row)
	{
		for (std::uint32_t radial = 0;
			radial < radial_segments;
			++radial)
		{
			const std::uint16_t next = static_cast<std::uint16_t>(
				(radial + 1u) % radial_segments);
			const std::uint16_t a = static_cast<std::uint16_t>(
				1u + row * radial_segments + radial);
			const std::uint16_t b = static_cast<std::uint16_t>(
				1u + (row + 1u) * radial_segments + radial);
			const std::uint16_t c = static_cast<std::uint16_t>(
				1u + row * radial_segments + next);
			const std::uint16_t d = static_cast<std::uint16_t>(
				1u + (row + 1u) * radial_segments + next);
			mesh.indices.insert(mesh.indices.end(), {a, b, c, c, b, d});
		}
	}
	mesh.base_positions.reserve(mesh.vertices.size());
	for (const TransitionVertex& vertex : mesh.vertices)
	{
		mesh.base_positions.push_back(vertex.position);
	}
}

void color_projector_portal(
	TransitionMesh& mesh,
	std::uint32_t radial_segments,
	std::uint32_t axial_segments)
{
	if (mesh.vertices.empty())
	{
		return;
	}
	mesh.vertices[0].color = 0u;
	const float axial_reciprocal =
		1.0f / static_cast<float>(axial_segments);
	for (std::uint32_t row = 0; row <= axial_segments; ++row)
	{
		glm::vec4 color{0.0f};
		if (row != 0 && row != axial_segments)
		{
			if (row + 1u == axial_segments)
			{
				color = {0.625f, 0.05859375f, 0.1171875f, 1.0f};
			}
			else
			{
				const float phase =
					static_cast<float>(row) * axial_reciprocal;
				if (phase >= 0.3f)
				{
					const float factor = std::sqrt(
						(phase - 0.3f) * 1.4285714626f);
					color = {
						glm::mix(0.76953125f, 0.3125f, factor),
						glm::mix(0.4296875f, 0.0f, factor),
						glm::mix(0.19921875f, 0.046875f, factor),
						1.0f,
					};
				}
				else
				{
					const float factor = phase * 3.3333332539f;
					color = {
						glm::mix(1.0f, 0.76953125f, factor),
						glm::mix(1.0f, 0.4296875f, factor),
						glm::mix(0.859375f, 0.19921875f, factor),
						1.0f,
					};
				}
			}
		}
		const std::uint32_t packed =
			pack_effect_color(color.r, color.g, color.b, color.a);
		for (std::uint32_t radial = 0;
			radial < radial_segments;
			++radial)
		{
			mesh.vertices[
				1u + row * radial_segments + radial].color = packed;
		}
	}
}

bool projector_submit_roll(World& world)
{
	return static_cast<float>(world_rand15(world))
			* 0.000030518509447574615f
		< 0.8999999761581421f;
}

void create_portal_mesh(WGateContext& context)
{
	context.portal = {};
	context.portal.texture = TransitionTexture::warp_primary;
	context.portal.blend = TransitionBlend::additive;
	context.portal.double_sided = true;
	context.portal.active = true;
	const std::uint32_t vertex_count =
		context.radial_segments * (context.axial_segments + 1u);
	context.portal.vertices.reserve(vertex_count);
	context.portal.indices.reserve(
		context.radial_segments * context.axial_segments * 6u);
	for (std::uint32_t row = 0;
		row <= context.axial_segments;
		++row)
	{
		context.row_radii[row] = context.radius;
		context.row_positions[row] =
			static_cast<float>(row) * context.radius
				/ static_cast<float>(context.axial_segments);
		for (std::uint32_t radial = 0;
			radial < context.radial_segments;
			++radial)
		{
			context.portal.vertices.push_back({
				{},
				0xffffffffu,
				{static_cast<float>(radial)
						/ static_cast<float>(context.radial_segments),
				 static_cast<float>(row)
						/ static_cast<float>(context.axial_segments)},
			});
			context.portal.base_positions.push_back({});
		}
	}
	for (std::uint16_t row = 0; row < context.axial_segments; ++row)
	{
		for (std::uint16_t radial = 0;
			radial < context.radial_segments;
			++radial)
		{
			const std::uint16_t next =
				static_cast<std::uint16_t>(
					(radial + 1) % context.radial_segments);
			const std::uint16_t a =
				static_cast<std::uint16_t>(
					row * context.radial_segments + radial);
			const std::uint16_t b =
				static_cast<std::uint16_t>(
					row * context.radial_segments + next);
			const std::uint16_t c =
				static_cast<std::uint16_t>(
					(row + 1) * context.radial_segments + radial);
			const std::uint16_t d =
				static_cast<std::uint16_t>(
					(row + 1) * context.radial_segments + next);
			context.portal.indices.insert(
				context.portal.indices.end(),
				{a, c, d, a, d, b});
		}
	}
	rebuild_portal_positions(context, 0.0f);
	fill_portal_colors(context);
}

void create_wormhole_mesh(TransitionMesh& mesh)
{
	mesh = {};
	mesh.texture = TransitionTexture::warp_primary;
	mesh.blend = TransitionBlend::additive;
	mesh.double_sided = true;
	mesh.active = true;
	mesh.vertices.reserve(496);
	mesh.indices.reserve(2880);
	for (std::uint32_t ring = 0; ring < 31; ++ring)
	{
		for (std::uint32_t radial = 0; radial < 16; ++radial)
		{
			const float angle =
				static_cast<float>(radial) * kTwoPi / 16.0f;
			const float phase = static_cast<float>(ring) / 30.0f;
			const float alpha = ring == 0 || ring == 30 ? 0.0f : 1.0f;
			mesh.vertices.push_back({
				{std::cos(angle) * 10000.0f,
				 std::sin(angle) * 10000.0f,
				 static_cast<float>(ring) * 200000.0f},
				pack_effect_color(
					0.9f * phase + 0.1f,
					0.15f,
					0.9f * (1.0f - phase) + 0.1f,
					alpha),
				{static_cast<float>(radial) / 16.0f,
				 static_cast<float>(ring)},
			});
			mesh.base_positions.push_back(
				mesh.vertices.back().position);
		}
	}
	for (std::uint16_t ring = 0; ring < 30; ++ring)
	{
		for (std::uint16_t radial = 0; radial < 16; ++radial)
		{
			const std::uint16_t next =
				static_cast<std::uint16_t>((radial + 1) % 16);
			const std::uint16_t a =
				static_cast<std::uint16_t>(ring * 16 + radial);
			const std::uint16_t b =
				static_cast<std::uint16_t>(ring * 16 + next);
			const std::uint16_t c =
				static_cast<std::uint16_t>((ring + 1) * 16 + radial);
			const std::uint16_t d =
				static_cast<std::uint16_t>((ring + 1) * 16 + next);
			mesh.indices.insert(mesh.indices.end(), {a, c, d, a, d, b});
		}
	}
}
}

bool transition_effects_initialize(
	TransitionEffectsRuntime& runtime,
	std::uint8_t graphics_quality)
{
	transition_effects_shutdown(runtime);
	switch (graphics_quality)
	{
	case 0:
		runtime.wgate.radial_segments = 9;
		runtime.wgate.axial_segments = 6;
		break;
	case 1:
		runtime.wgate.radial_segments = 12;
		runtime.wgate.axial_segments = 8;
		break;
	default:
		runtime.wgate.radial_segments = 16;
		runtime.wgate.axial_segments = 12;
		break;
	}
	runtime.jump.initialized = true;
	runtime.wgate.initialized = true;
	runtime.wgate.camera_context_index = -1;
	std::fill(
		runtime.wgate.fixed_deformation_objects.begin(),
		runtime.wgate.fixed_deformation_objects.end(),
		UINT16_MAX);
	return true;
}

void transition_effects_reset(TransitionEffectsRuntime& runtime)
{
	const std::uint8_t radial = runtime.wgate.radial_segments;
	const std::uint8_t axial = runtime.wgate.axial_segments;
	const bool initialized =
		runtime.jump.initialized && runtime.wgate.initialized;
	runtime = {};
	runtime.wgate.radial_segments = radial == 0 ? 16 : radial;
	runtime.wgate.axial_segments = axial == 0 ? 12 : axial;
	runtime.wgate.camera_context_index = -1;
	std::fill(
		runtime.wgate.fixed_deformation_objects.begin(),
		runtime.wgate.fixed_deformation_objects.end(),
		UINT16_MAX);
	runtime.jump.initialized = initialized;
	runtime.wgate.initialized = initialized;
}

void transition_effects_shutdown(TransitionEffectsRuntime& runtime)
{
	runtime = {};
}

void transition_effects_release_owner(
	TransitionEffectsRuntime& runtime,
	ObjectHandle owner)
{
	for (JumpVisualContext& context : runtime.jump.contexts)
	{
		if (context.active
			&& context.owner_index == owner.index
			&& context.owner_generation == owner.generation)
		{
			context = {};
		}
	}
	if (runtime.jump.transition_owner_index == owner.index
		&& runtime.jump.transition_owner_generation
			== owner.generation)
	{
		runtime.jump.overlay_active = false;
		runtime.jump.environment_transition_active = false;
		runtime.jump.overlay_countdown = 0;
		runtime.jump.overlay_scalar = 0.0f;
		runtime.jump.transition_owner_index = UINT16_MAX;
		runtime.jump.transition_owner_generation = 0;
	}
	for (std::int16_t index = 0;
		index < static_cast<std::int16_t>(
			runtime.wgate.contexts.size());
		++index)
	{
		const WGateContext& context =
			runtime.wgate.contexts[index];
		if (context.active
			&& context.owner_index == owner.index
			&& context.owner_generation == owner.generation)
		{
			wgate_context_free(runtime.wgate, index);
		}
	}
	wgate_context_unregister_deformation(
		runtime.wgate, owner.index);
	if (runtime.wgate.warp_transition_owner_index == owner.index
		&& runtime.wgate.warp_transition_owner_generation
			== owner.generation)
	{
		runtime.wgate.warp_transition_active = false;
		runtime.wgate.warp_transition_owner_index = UINT16_MAX;
		runtime.wgate.warp_transition_owner_generation = 0;
		runtime.wgate.camera_context_index = -1;
	}
	if (runtime.wgate.fixed_transition_owner_index == owner.index
		&& runtime.wgate.fixed_transition_owner_generation
			== owner.generation)
	{
		runtime.wgate.fixed_gate_transition_active = false;
		runtime.wgate.fixed_tunnel_camera_active = false;
		runtime.wgate.fixed_transition_owner_index = UINT16_MAX;
		runtime.wgate.fixed_transition_owner_generation = 0;
		runtime.wgate.camera_context_index = -1;
	}
	if (runtime.wgate.fixed_departure_owner_index == owner.index
		&& runtime.wgate.fixed_departure_owner_generation
			== owner.generation)
	{
		runtime.wgate.fixed_departure_lock = false;
		runtime.wgate.fixed_departure_owner_index = UINT16_MAX;
		runtime.wgate.fixed_departure_owner_generation = 0;
	}
	if (runtime.wgate.projector_owner_index == owner.index
		&& runtime.wgate.projector_owner_generation
			== owner.generation)
	{
		for (WGateProjector& projector : runtime.wgate.projectors)
		{
			projector = {};
		}
		runtime.wgate.projector_portal = {};
		runtime.wgate.projector_submission_tick = UINT32_MAX;
		runtime.wgate.projector_owner_index = UINT16_MAX;
		runtime.wgate.projector_owner_generation = 0;
	}
	if (runtime.wgate.wormhole_owner_index == owner.index
		&& runtime.wgate.wormhole_owner_generation
			== owner.generation)
	{
		runtime.wgate.wormhole = {};
		runtime.wgate.wormhole_active = false;
		runtime.wgate.wormhole_owner_index = UINT16_MAX;
		runtime.wgate.wormhole_owner_generation = 0;
	}
}

bool transition_explosion_create(
	World& world,
	const glm::vec3& position,
	float size,
	std::uint32_t duration_ticks,
	std::uint32_t tick)
{
	return transition_explosion_create_delayed(
		world, position, size, duration_ticks, tick, 0);
}

bool transition_explosion_create_delayed(
	World& world,
	const glm::vec3& position,
	float size,
	std::uint32_t duration_ticks,
	std::uint32_t tick,
	std::uint32_t delay_ticks)
{
	return explosion_billboard_create(
		world.death_effects,
		world,
		position,
		glm::vec3{0.0f},
		ExplosionBillboardType::separate_frames,
		size,
		static_cast<std::int32_t>(duration_ticks),
		true,
		static_cast<std::int32_t>(delay_ticks),
		false,
		false,
		tick);
}

std::int16_t jump_visual_allocate(
	JumpEffectsRuntime& runtime,
	ObjectHandle owner)
{
	for (std::uint16_t index = 0; index < runtime.contexts.size(); ++index)
	{
		JumpVisualContext& context = runtime.contexts[index];
		if (context.active)
		{
			continue;
		}
		context = {};
		context.owner_index = owner.index;
		context.owner_generation = owner.generation;
		context.active = true;
		return static_cast<std::int16_t>(index);
	}
	if (!runtime.pool_saturation_reported)
	{
		diagnostics::mission_log(
			"jump visual pool exhausted capacity=%u",
			static_cast<unsigned>(kJumpVisualContextCount));
		runtime.pool_saturation_reported = true;
	}
	return -1;
}

JumpVisualContext* jump_visual_get(
	JumpEffectsRuntime& runtime,
	std::int16_t index)
{
	return index >= 0
			&& static_cast<std::size_t>(index) < runtime.contexts.size()
			&& runtime.contexts[index].active
		? &runtime.contexts[index]
		: nullptr;
}

void jump_visual_free(
	JumpEffectsRuntime& runtime,
	std::int16_t index)
{
	if (index < 0
		|| static_cast<std::size_t>(index) >= runtime.contexts.size())
	{
		return;
	}
	runtime.contexts[index] = {};
}

bool jump_visual_build_in(
	World& world,
	std::int16_t index,
	WorldObject& actor,
	bool large)
{
	JumpVisualContext* context =
		jump_visual_get(world.transition_effects.jump, index);
	if (context == nullptr)
	{
		return false;
	}
	context->jump_in_burst = make_jump_burst(true);
	// AI_JumpIn_update doubles only X/Y after constructing the jump-in
	// burst; Z and the jump-out burst retain their authored dimensions.
	for (std::size_t vertex = 0;
		vertex < context->jump_in_burst.vertices.size();
		++vertex)
	{
		context->jump_in_burst.vertices[vertex].position.x *= 2.0f;
		context->jump_in_burst.vertices[vertex].position.y *= 2.0f;
		if (vertex < context->jump_in_burst.base_positions.size())
		{
			context->jump_in_burst.base_positions[vertex].x *= 2.0f;
			context->jump_in_burst.base_positions[vertex].y *= 2.0f;
		}
	}
	jump_visual_set_burst_phase(
		context->jump_in_burst, 1.0f, 1.0f);
	context->jump_in_burst.position =
		{0.0f, 0.0f, 800.0f};
	context->jump_in_burst.orientation = glm::mat3{1.0f};
	context->flare = make_jump_flare();
	context->flare.position = actor.position;
	context->flare.orientation = actor.orientation;
	context->flare.scale = actor.bounds_max.x - actor.bounds_min.x;
	context->flare.active = false;
	context->jump_in_basis = actor.orientation;
	const ObjectModelReference* cargo_pod =
		retained_find_named_model(actor, "Cargo pod");
	const bool cargo_pod_visible =
		actor.type == 0x1fu
		&& cargo_pod != nullptr
		&& (cargo_pod->runtime_flags & 0x20u) == 0;
	visit_jump_points(
		actor,
		7,
		[&](
			const glm::vec3& point,
			const glm::mat3&,
			std::uint16_t)
		{
			if (context->trail_count >= context->trails.size())
			{
				return;
			}
			TransitionMesh& trail =
				context->trails[context->trail_count++];
			trail = make_jump_trail(large);
			place_trail(
				trail, point, cargo_pod_visible);
		});
	if (context->trail_count == 0)
	{
		context->trail_count = 1;
		context->trails[0] = make_jump_trail(large);
		place_trail(
			context->trails[0],
			{},
			cargo_pod_visible);
	}
	return true;
}

bool jump_visual_build_out(
	World& world,
	std::int16_t index,
	WorldObject& actor,
	bool large,
	const glm::vec3&)
{
	JumpVisualContext* context =
		jump_visual_get(world.transition_effects.jump, index);
	if (context == nullptr)
	{
		return false;
	}
	context->jump_out_burst = make_jump_burst(false);
	// Jump_make_burst (0x00417e30) initializes all six material rings
	// through Jump_set_out_burst_phase (0x00418150).
	jump_visual_set_out_burst_phase(
		context->jump_out_burst, 1.0f);
	context->jump_out_burst.position =
		{0.0f, 0.0f, -200.0f};
	context->jump_out_burst.orientation = glm::mat3{1.0f};
	context->flare = make_jump_flare();
	context->flare.active = false;
	const ObjectModelReference* cargo_pod =
		retained_find_named_model(actor, "Cargo pod");
	const bool cargo_pod_visible =
		actor.type == 0x1fu
		&& cargo_pod != nullptr
		&& (cargo_pod->runtime_flags & 0x20u) == 0;
	visit_jump_points(
		actor,
		7,
		[&](
			const glm::vec3& point,
			const glm::mat3&,
			std::uint16_t)
		{
			if (context->trail_count >= context->trails.size())
			{
				return;
			}
			TransitionMesh& trail =
				context->trails[context->trail_count++];
			trail = make_jump_trail(large);
			place_trail(
				trail, point, cargo_pod_visible);
		});
	if (context->trail_count == 0)
	{
		context->trail_count = 1;
		context->trails[0] = make_jump_trail(large);
		place_trail(
			context->trails[0],
			{},
			cargo_pod_visible);
	}
	visit_jump_points(
		actor,
		8,
		[&](
			const glm::vec3& point,
			const glm::mat3&,
			std::uint16_t)
		{
			if (context->light_count >= context->lights.size())
			{
				return;
			}
			context->lights[context->light_count++] = {
				point,
				{150.0f, 150.0f},
				0xffffffffu,
				TransitionTexture::jump_light,
				true,
			};
		});
	return true;
}

void jump_visual_set_trail_phase(
	JumpVisualContext& context,
	float phase)
{
	const float clamped = std::max(phase, 0.0f);
	for (std::uint8_t trail_index = 0;
		trail_index < context.trail_count;
		++trail_index)
	{
		TransitionMesh& trail = context.trails[trail_index];
		for (TransitionVertex& vertex : trail.vertices)
		{
			vertex.color =
				(vertex.color & 0x00ffffffu) | 0xff000000u;
		}
		const float retail_phase = clamped * 0.3f;
		const glm::vec3 endpoint{
			0.5f * retail_phase,
			0.7f * retail_phase,
			retail_phase,
		};
		for (std::size_t index = 4; index < trail.vertices.size(); ++index)
		{
			TransitionVertex& vertex = trail.vertices[index];
			const std::size_t ribbon_vertex = (index - 4u) & 3u;
			const glm::vec3 color =
				ribbon_vertex == 0u || ribbon_vertex == 3u
					? endpoint
					: glm::vec3{};
			vertex.color = pack_effect_color(
				color.r, color.g, color.b, 1.0f);
		}
		// This is the original per-vertex emissive animation. Geometry stays
		// resident and is submitted as a complete mesh; only the small dynamic
		// color stream changes with the jump phase.
		trail.active = true;
	}
}

void jump_visual_set_burst_phase(
	TransitionMesh& burst,
	float ring_phase,
	float core_phase)
{
	if (burst.vertices.size() < 73)
	{
		return;
	}
	for (std::uint32_t ring = 0; ring < 6; ++ring)
	{
		const float brightness =
			static_cast<float>(5u - ring)
				* ring_phase * 0.04f;
		for (std::uint32_t column = 0; column < 12; ++column)
		{
			TransitionVertex& vertex =
				burst.vertices[1 + ring * 12 + column];
			vertex.color = pack_effect_color(
				ring == 0
					? 0.5f * core_phase * core_phase
					: brightness,
				ring == 0
					? 0.21f * core_phase * core_phase
					: 0.0f,
				0.0f,
				ring == 0 ? core_phase : 1.0f);
		}
	}
	burst.active = true;
}

void jump_visual_set_out_burst_phase(
	TransitionMesh& burst,
	float phase)
{
	if (burst.vertices.size() < 73)
	{
		return;
	}
	// Jump_set_out_burst_phase, LANCER.EXE 0x00418150, walks the
	// constructor's six groups in order with multipliers 5..0.
	for (std::uint32_t ring = 0; ring < 6; ++ring)
	{
		const float brightness =
			static_cast<float>(5u - ring) * phase * 0.1f;
		for (std::uint32_t column = 0; column < 12; ++column)
		{
			burst.vertices[1 + ring * 12 + column].color =
				pack_effect_color(
					brightness,
					brightness,
					brightness,
					1.0f);
		}
	}
	burst.active = true;
}

std::int16_t wgate_context_allocate(
	World& world,
	WGateEffectsRuntime& runtime,
	WGateMode mode,
	ObjectHandle owner,
	const glm::vec3& position,
	const glm::mat3& orientation,
	std::uint32_t tick)
{
	for (std::uint8_t index = 0; index < runtime.contexts.size(); ++index)
	{
		WGateContext& context = runtime.contexts[index];
		if (context.active)
		{
			continue;
		}
		context = {};
		context.mode = mode;
		context.owner_index = owner.index;
		context.owner_generation = owner.generation;
		context.position = position;
		context.orientation = orientation;
		context.created_tick = tick;
		context.previous_tick = tick;
		context.radial_segments = runtime.radial_segments;
		context.axial_segments = runtime.axial_segments;
		const WorldObject* actor = world_resolve(world, owner);
		context.radius = 2000.0f;
		context.mesh_radius = 1000.0f;
		context.animation_radius =
			actor != nullptr
				&& (actor->runtime_flags & kObjectFlagCompound) != 0
				? 4000.0f
				: 900.0f;
		if (actor != nullptr && actor->type == 55)
		{
			context.radius = 15000.0f;
			context.axial_offset = 10000.0f;
			context.has_special_parameters = true;
		}
		else if (actor != nullptr && actor->type == 13)
		{
			context.radius = 100000.0f;
			context.mesh_radius = 100000.0f;
			context.animation_radius = 100000.0f;
			context.axial_offset = 50000.0f;
			context.has_special_parameters = true;
		}
		if (mode == WGateMode::prototype)
		{
			context.mesh_radius = 70.0f;
			context.animation_radius = 70.0f;
		}
		else if (mode == WGateMode::advanced)
		{
			context.mesh_radius = 40.0f;
			context.animation_radius = 40.0f;
		}
		else if (mode == WGateMode::boridin)
		{
			context.mesh_radius = 240.0f;
			context.animation_radius = 240.0f;
		}
		const float effect_radius = context.radius;
		context.radius = context.mesh_radius;
		create_portal_mesh(context);
		context.radius = effect_radius;
		if (mode == WGateMode::ship)
		{
			for (std::size_t beam_index = 0;
				beam_index < context.beams.size();
				++beam_index)
			{
				context.beams[beam_index] = make_beam(110.0f);
				context.beams[beam_index].active = false;
				initialize_wgate_emitter(
					world.particles,
					context.particle_emitters[beam_index],
					ParticleEmitterStyle::wgate,
					tick,
					10.0f,
					110.0f);
			}
		}
		else if (mode == WGateMode::prototype
			|| mode == WGateMode::advanced)
		{
			for (TransitionMesh& quad : context.fixed_quads)
			{
				quad.texture = TransitionTexture::warp_secondary;
				quad.blend = TransitionBlend::additive;
				quad.double_sided = true;
				quad.active = true;
				append_quad(
					quad,
					{-50000.0f, -50000.0f, 0.0f},
					{50000.0f, -50000.0f, 0.0f},
					{50000.0f, 50000.0f, 0.0f},
					{-50000.0f, 50000.0f, 0.0f},
					0xffffffffu);
			}
		}
		context.active = true;
		runtime.live_mask |= 1u << index;
		return index;
	}
	if (!runtime.context_saturation_reported)
	{
		diagnostics::mission_log(
			"warp context pool exhausted capacity=%u",
			static_cast<unsigned>(kWGateContextCount));
		runtime.context_saturation_reported = true;
	}
	return -1;
}

std::int16_t wgate_context_find_owner(
	const WGateEffectsRuntime& runtime,
	std::uint16_t owner_index)
{
	for (std::uint8_t index = 0; index < runtime.contexts.size(); ++index)
	{
		if (runtime.contexts[index].active
			&& runtime.contexts[index].owner_index == owner_index)
		{
			return index;
		}
	}
	return -1;
}

WGateContext* wgate_context_get(
	WGateEffectsRuntime& runtime,
	std::int16_t index)
{
	return index >= 0
			&& static_cast<std::size_t>(index) < runtime.contexts.size()
			&& runtime.contexts[index].active
		? &runtime.contexts[index]
		: nullptr;
}

void wgate_context_free(
	WGateEffectsRuntime& runtime,
	std::int16_t index)
{
	if (index < 0
		|| static_cast<std::size_t>(index) >= runtime.contexts.size())
	{
		return;
	}
	runtime.contexts[index] = {};
	runtime.live_mask &= ~(1u << static_cast<std::uint32_t>(index));
}

void wgate_context_set_alpha(WGateContext& context, float alpha)
{
	const std::uint32_t packed_alpha =
		static_cast<std::uint32_t>(color_channel(alpha)) << 24;
	for (TransitionVertex& vertex : context.portal.vertices)
	{
		vertex.color = (vertex.color & 0x00ffffffu) | packed_alpha;
	}
	for (TransitionMesh& quad : context.fixed_quads)
	{
		for (TransitionVertex& vertex : quad.vertices)
		{
			vertex.color = (vertex.color & 0x00ffffffu) | packed_alpha;
		}
	}
}

void wgate_context_set_ship_phase(
	WGateContext& context,
	WGateShipStage stage,
	float phase)
{
	context.phase = phase;
	const float clamped = std::clamp(phase, 0.0f, 1.0f);
	const float axial_extent =
		static_cast<float>(context.axial_segments)
		* context.animation_radius;
	float front = 0.0f;
	float radius = context.radius * 0.05f;
	float beam_motion = 0.0f;
	float beam_radius = context.radius * 1.07f;
	float opacity = 1.0f;
	bool collapse = false;
	switch (stage)
	{
	case WGateShipStage::warp_out_open:
		front = square_root_lerp(0.0f, axial_extent, clamped);
		radius = cosine_lerp(
			context.radius * 0.05f,
			context.radius,
			clamped);
		beam_motion = clamped < 0.2f
			? cosine_lerp(1.0f, 6.0f, clamped * 5.0f)
			: clamped < 0.7f
				? 6.0f
				: cosine_lerp(
					6.0f,
					0.0f,
					(clamped - 0.7f) * 3.3333332538604736f);
		opacity = fade_after_ninety_percent(1.0f, clamped);
		break;
	case WGateShipStage::warp_out_reverse:
		front = wgate_axial_curve(
			axial_extent, 0.0f, clamped);
		radius = context.radius;
		beam_motion = clamped < 0.3f
			? cosine_lerp(
				0.0f,
				-3.0f,
				clamped * 3.3333332538604736f)
			: -3.0f;
		break;
	case WGateShipStage::warp_out_depart:
		front = axial_extent;
		radius = context.radius;
		if (clamped >= 0.3f)
		{
			const float depart_phase =
				(clamped - 0.3f) * 1.4285714626312256f;
			beam_motion =
				cosine_lerp(-3.0f, 0.0f, depart_phase);
			collapse = true;
			front *= 1.0f - depart_phase;
		}
		else
		{
			beam_motion = -3.0f;
		}
		break;
	case WGateShipStage::warp_in_open:
	{
		// AI_WarpIn uses a signed triangle wave. WGate_axial_motion_curve's
		// cosine makes the negative return half contract symmetrically.
		const float envelope = clamped < 0.5f
			? clamped * 2.0f
			: (clamped - 1.0f) * 2.0f;
		front = wgate_axial_curve(
			0.0f, axial_extent, envelope);
		radius = cosine_lerp(
			context.radius * 0.05f,
			context.radius,
			envelope);
		beam_radius = cosine_lerp(
			0.05f,
			context.radius * 1.07f,
			envelope);
		beam_motion = 1.0f;
		break;
	}
	case WGateShipStage::warp_in_delay:
		front = axial_extent * (1.0f - clamped);
		radius = context.radius * (1.0f - clamped);
		beam_motion = 1.0f - clamped;
		collapse = true;
		break;
	}
	for (std::uint32_t row = 0; row <= context.axial_segments; ++row)
	{
		const float row_front =
			static_cast<float>(row)
				* context.animation_radius;
		const bool reached = front >= row_front;
		context.row_radii[row] = reached
			? radius
			: context.radius * 0.05f;
		if (collapse)
		{
			context.row_radii[row] *=
				std::clamp(
					front - row_front
						+ context.animation_radius,
					0.0f,
					context.animation_radius)
				/ context.animation_radius;
		}
		context.row_positions[row] =
			std::min(row_front, front);
	}
	rebuild_portal_positions(context, phase * kTwoPi);
	fill_portal_colors(context);
	for (TransitionVertex& vertex : context.portal.vertices)
	{
		const std::uint32_t alpha =
			static_cast<std::uint32_t>(
				color_channel(opacity)) << 24;
		vertex.color = (vertex.color & 0x00ffffffu) | alpha;
	}
	context.portal.position = context.position;
	context.portal.orientation = context.orientation;
	context.portal.active =
		front >= 800.0f && opacity > 0.0001f;
	const float beam_envelope =
		std::clamp(std::abs(beam_motion) / 6.0f, 0.0f, 1.0f);
	for (std::uint32_t index = 0; index < context.beams.size(); ++index)
	{
		const float angle =
			static_cast<float>(index) * kPi * 0.5f;
		const glm::vec3 local{
			std::cos(angle) * beam_radius,
			std::sin(angle) * beam_radius,
			0.0f,
		};
		TransitionMesh& beam = context.beams[index];
		beam.position =
			context.position
			+ context.orientation
				* (local
					+ glm::vec3{
						0.0f,
						0.0f,
						beam_motion
							* context.animation_radius});
		beam.orientation =
			context.orientation
			* math::rotation_from_euler({0.0f, 0.0f, angle});
		beam.active = std::abs(beam_motion) > 0.0001f;
		for (std::size_t vertex = 0;
			vertex < beam.vertices.size();
			++vertex)
		{
			if (vertex < beam.base_positions.size())
			{
				const glm::vec3 base = beam.base_positions[vertex];
				beam.vertices[vertex].position = {
					base.x,
					base.y,
					base.z * std::max(beam_envelope, 0.05f),
				};
			}
			beam.vertices[vertex].color =
				pack_effect_color(
					0.9f,
					0.2f,
					0.05f,
					beam_envelope);
		}
		TransitionBillboard& emitter = context.emitters[index];
		emitter.position = beam.position;
		emitter.half_extent =
			glm::vec2{100.0f * beam_envelope};
		emitter.color =
			pack_effect_color(1.0f, 0.3f, 0.05f, beam_envelope);
		emitter.texture = TransitionTexture::jump_light;
		emitter.active = beam.active;
	}
}

void wgate_context_scroll_uv(
	WGateContext& context,
	float u_delta,
	float v_delta)
{
	for (TransitionVertex& vertex : context.portal.vertices)
	{
		vertex.uv.x += u_delta;
		vertex.uv.y += v_delta;
	}
}

void wgate_context_register_deformation(
	WGateEffectsRuntime& runtime,
	std::uint16_t object_index)
{
	if (object_index == UINT16_MAX)
	{
		return;
	}
	for (std::uint8_t index = 0;
		index < runtime.fixed_deformation_count;
		++index)
	{
		if (runtime.fixed_deformation_objects[index] == object_index)
		{
			return;
		}
	}
	if (runtime.fixed_deformation_count
		< runtime.fixed_deformation_objects.size())
	{
		runtime.fixed_deformation_objects[
			runtime.fixed_deformation_count++] = object_index;
	}
}

void wgate_context_unregister_deformation(
	WGateEffectsRuntime& runtime,
	std::uint16_t object_index)
{
	for (std::uint8_t index = 0;
		index < runtime.fixed_deformation_count;
		++index)
	{
		if (runtime.fixed_deformation_objects[index] != object_index)
		{
			continue;
		}
		for (std::uint8_t move = index + 1;
			move < runtime.fixed_deformation_count;
			++move)
		{
			runtime.fixed_deformation_objects[move - 1] =
				runtime.fixed_deformation_objects[move];
		}
		--runtime.fixed_deformation_count;
		runtime.fixed_deformation_objects[
			runtime.fixed_deformation_count] = UINT16_MAX;
		return;
	}
}

void wgate_context_set_collapse_wave(
	WGateContext& context,
	float phase)
{
	const float vertex_count =
		static_cast<float>(context.portal.vertices.size());
	const float threshold =
		vertex_count * ((std::sin(phase * 1000.0f) + 1.0f) * 0.5f);
	std::size_t index = 0;
	for (std::uint32_t row = 0; row <= context.axial_segments; ++row)
	{
		for (std::uint32_t radial = 0;
			radial < context.radial_segments;
			++radial, ++index)
		{
			if (index >= context.portal.vertices.size()
				|| static_cast<float>(index) > threshold)
			{
				continue;
			}
			TransitionVertex& vertex = context.portal.vertices[index];
			if (row == 0 || row == context.axial_segments)
			{
				vertex.color = 0;
				continue;
			}
			const float front_phase = std::clamp(
				(threshold - static_cast<float>(index))
					/ (vertex_count - threshold),
				0.0f,
				1.0f);
			if (row == context.axial_segments - 1u)
			{
				const float brightness =
					cosine_lerp(0.0f, 0.5f, front_phase);
				vertex.color = pack_effect_color(
					0.0f, brightness * 0.5f, brightness, 1.0f);
			}
			else
			{
				const float brightness =
					cosine_lerp(0.0f, 1.0f, front_phase);
				vertex.color = pack_effect_color(
					brightness,
					brightness * 0.3f,
					brightness * 0.5f,
					1.0f);
			}
		}
	}
}

void wgate_context_set_collapse_front(
	WGateContext& context,
	float phase)
{
	const float vertex_count =
		static_cast<float>(context.portal.vertices.size());
	const float threshold = vertex_count * phase;
	std::size_t index = 0;
	for (std::uint32_t row = 0; row <= context.axial_segments; ++row)
	{
		for (std::uint32_t radial = 0;
			radial < context.radial_segments;
			++radial, ++index)
		{
			if (index >= context.portal.vertices.size()
				|| static_cast<float>(index) > threshold)
			{
				continue;
			}
			TransitionVertex& vertex = context.portal.vertices[index];
			if (row == 0 || row == context.axial_segments)
			{
				vertex.color = 0;
				continue;
			}
			const float front_phase = std::clamp(
				(threshold - static_cast<float>(index))
					/ (vertex_count - threshold),
				0.0f,
				1.0f);
			if (row == context.axial_segments - 1u)
			{
				const float brightness =
					cosine_lerp(0.0f, 0.5f, front_phase);
				vertex.color = pack_effect_color(
					brightness * 0.3f,
					brightness,
					brightness * 0.8f,
					1.0f);
			}
			else
			{
				const float brightness =
					cosine_lerp(0.0f, 1.0f, front_phase);
				vertex.color = pack_effect_color(
					brightness * 0.6f,
					brightness * 0.7f,
					brightness * 0.7f,
					1.0f);
			}
		}
	}
}

void transition_effects_service_fixed_gates(
	World& world,
	std::uint32_t tick,
	std::uint32_t elapsed_ticks)
{
	WGateEffectsRuntime& runtime = world.transition_effects.wgate;
	// Whiteout exposure is the shared world/HUD integer and is decayed by
	// mission_update_whiteout_overlay, not by the gate mesh service.
	(void)elapsed_ticks;
	for (WGateContext& context : runtime.contexts)
	{
		if (!context.active
			|| (context.mode != WGateMode::prototype
				&& context.mode != WGateMode::advanced))
		{
			continue;
		}
		// WGate_update_mesh (LANCER.EXE 0x0041fa50) reads the 100 Hz
		// gameplay tick directly. Prototype and advanced gates use the
		// same 41.7*radial-segment displacement, with independent 0.04
		// and 0.034 radians-per-tick waves. Their authored axial positions
		// do not oscillate.
		for (std::uint32_t row = 0;
			row <= context.axial_segments;
			++row)
		{
			for (std::uint32_t radial = 0;
				radial < context.radial_segments;
				++radial)
			{
				const std::uint32_t vertex =
					row * context.radial_segments + radial;
				if (vertex >= context.portal.vertices.size())
				{
					continue;
				}
				const float angle =
					static_cast<float>(radial) * kTwoPi
						/ static_cast<float>(
							context.radial_segments);
				const float displacement =
					static_cast<float>(context.radial_segments)
						* 41.70000076293945f;
				const std::uint32_t retail_vertex = vertex + 1u;
				const std::uint32_t phase_x =
					(retail_vertex << 3u)
						| (retail_vertex >> 29u);
				const std::uint32_t phase_y =
					(retail_vertex << 2u)
						| (retail_vertex >> 30u);
				TransitionVertex& output =
					context.portal.vertices[vertex];
				output.position = {
					std::sin(angle) * context.row_radii[row]
						+ std::sin(
							static_cast<float>(tick)
									* 0.03999999910593033f
								+ static_cast<float>(phase_x)
									* 0.30000001192092896f)
							* displacement,
					std::cos(angle) * context.row_radii[row]
						+ std::sin(
							static_cast<float>(tick)
									* 0.03400000184774399f
								+ static_cast<float>(phase_y)
									* 0.4000000059604645f)
							* displacement,
					context.row_positions[row]
						+ context.axial_offset,
				};
				if (vertex < context.portal.base_positions.size())
				{
					context.portal.base_positions[vertex] =
						output.position;
				}
			}
		}
		const glm::mat3 inverse_orientation =
			glm::transpose(context.orientation);
		for (std::uint8_t deformation = 0;
			deformation < runtime.fixed_deformation_count;
			++deformation)
		{
			const std::uint16_t object_index =
				runtime.fixed_deformation_objects[deformation];
			if (object_index >= std::size(world.objects)
				|| !world.objects[object_index].active)
			{
				continue;
			}
			const glm::vec3 local = inverse_orientation
				* (world.objects[object_index].position
					- context.position);
			if (local.z > 50000.0f)
			{
				continue;
			}
			for (TransitionVertex& vertex : context.portal.vertices)
			{
				const glm::vec2 delta{
					vertex.position.x - local.x,
					vertex.position.y - local.y,
				};
				const float distance = glm::length(delta);
				if (distance < 10000.0f)
				{
					vertex.position.z -=
						(1.0f - distance / 10000.0f)
						* std::max(0.0f, 50000.0f - local.z);
				}
			}
		}
		context.portal.position = context.position;
		context.portal.orientation = context.orientation;
		for (TransitionMesh& quad : context.fixed_quads)
		{
			quad.position = context.position;
			quad.orientation = context.orientation;
		}
	}
}

bool wgate_wormhole_create(
	WGateEffectsRuntime& runtime,
	const glm::vec3& position,
	const glm::mat3& orientation)
{
	create_wormhole_mesh(runtime.wormhole);
	runtime.wormhole.position = position;
	runtime.wormhole.orientation = orientation;
	runtime.wormhole_active = true;
	return true;
}

void wgate_wormhole_animate(
	WGateEffectsRuntime& runtime,
	std::uint32_t tick,
	std::uint32_t elapsed_ticks)
{
	if (!runtime.wormhole_active)
	{
		return;
	}
	for (std::uint32_t ring = 0; ring < 31; ++ring)
	{
		for (std::uint32_t radial = 0; radial < 16; ++radial)
		{
			const std::size_t index =
				static_cast<std::size_t>(ring) * 16 + radial;
			const float angle =
				static_cast<float>(radial) * kTwoPi / 16.0f;
			const float wave =
				std::sin(static_cast<float>(tick) * 0.004f
					+ static_cast<float>(ring) * 0.4f)
					* 800.0f
				+ std::sin(static_cast<float>(tick) * 0.007f
					+ static_cast<float>(radial) * 0.7f)
					* 300.0f;
			const float radius = 10000.0f + wave;
			runtime.wormhole.vertices[index].position.x =
				std::cos(angle) * radius;
			runtime.wormhole.vertices[index].position.y =
				std::sin(angle) * radius;
			runtime.wormhole.vertices[index].uv.x +=
				static_cast<float>(elapsed_ticks) * 0.0105f;
			runtime.wormhole.vertices[index].uv.y -=
				static_cast<float>(elapsed_ticks) * 0.0005f;
		}
	}
}

bool wgate_projectors_create(
	World& world,
	WGateEffectsRuntime& runtime,
	ObjectHandle owner,
	std::uint32_t tick)
{
	runtime.projector_owner_index = owner.index;
	runtime.projector_owner_generation = owner.generation;
	runtime.transition_start_tick = tick;
	runtime.projector_submission_tick = UINT32_MAX;
	for (WGateProjector& projector : runtime.projectors)
	{
		projector = {};
		projector.beam = make_beam(1400.0f);
		projector.beam.active = false;
		initialize_wgate_emitter(
			world.particles,
			projector.particle_emitter,
			ParticleEmitterStyle::wgate_large,
			tick,
			100.0f,
			20.0f);
		projector.active = true;
	}
	create_projector_portal(
		runtime.projector_portal,
		runtime.radial_segments,
		runtime.axial_segments);
	runtime.projector_portal.active = false;
	return true;
}

void wgate_projectors_update(
	World& world,
	WGateEffectsRuntime& runtime,
	const std::array<glm::vec3, 6>& anchor_positions,
	const glm::vec3& projector_position,
	const glm::mat3& projector_orientation,
	std::uint32_t tick,
	std::uint32_t elapsed_ticks,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward)
{
	const std::uint32_t age = tick - runtime.transition_start_tick;
	const float phase = static_cast<float>(age) * 0.002f;
	const float ramp = age > 500u
		? 2500.0f
		: 2500.0f * phase * phase;
	for (std::uint32_t index = 0;
		index < runtime.projectors.size();
		++index)
	{
		WGateProjector& projector = runtime.projectors[index];
		if (!projector.active)
		{
			continue;
		}
		const glm::vec3 local{
			static_cast<float>(index + 1) * 8571.4287f,
			0.0f,
			static_cast<float>((5 - static_cast<int>(index))
				* (5 - static_cast<int>(index)))
				* ramp
				+ 75000.0f,
		};
		const float roll =
			static_cast<float>(index) * kPi / 3.0f
			+ std::sin(static_cast<float>(tick) * 0.005f)
				* 2.1991148f;
		const glm::mat3 endpoint_orientation =
			projector_orientation
				* math::rotation_from_euler({
					0.0f,
					0.0f,
					(index & 1u) == 0 ? roll : -roll,
				});
		projector.position =
			projector_position + endpoint_orientation * local;
		projector.beam.position = anchor_positions[index];
		projector.beam.orientation = look_at_points(
			anchor_positions[index], projector.position);
		const float length =
			glm::distance(anchor_positions[index], projector.position);
		for (std::size_t vertex = 0;
			vertex < projector.beam.vertices.size();
			++vertex)
		{
			if (vertex >= 4u)
			{
				projector.beam.vertices[vertex].position.z =
					(vertex % 4u == 1u || vertex % 4u == 2u)
						? length
						: 0.0f;
			}
			const std::uint32_t lane =
				static_cast<std::uint32_t>(vertex % 4u);
			projector.beam.vertices[vertex].color =
				lane == 0u || lane == 3u
					? pack_effect_color(0.99f, 0.04f, 0.04f, 0.0f)
					: pack_effect_color(1.0f, 0.0f, 0.0f, 1.0f);
		}
		projector.particle_emitter.local_position = projector.position;
		projector.particle_emitter.local_basis = glm::mat3{1.0f};
		particle_emitter_service_owned(
			world,
			projector.particle_emitter,
			tick,
			elapsed_ticks,
			camera_position,
			camera_forward);
		projector.beam.active = projector_submit_roll(world);
	}
	runtime.projector_portal.position = projector_position;
	runtime.projector_portal.orientation = projector_orientation;
	const std::uint32_t radial = runtime.radial_segments;
	const std::uint32_t axial = runtime.axial_segments;
	for (std::uint32_t row = 0; row <= axial; ++row)
	{
		for (std::uint32_t column = 0; column < radial; ++column)
		{
			const std::size_t vertex =
				1u + static_cast<std::size_t>(row) * radial + column;
			if (vertex >= runtime.projector_portal.vertices.size())
			{
				continue;
			}
			runtime.projector_portal.vertices[vertex].position.z =
				static_cast<float>(row * row) * ramp + 44000.0f;
		}
	}
	color_projector_portal(
		runtime.projector_portal, radial, axial);
	if (projector_submit_roll(world))
	{
		for (std::uint32_t row = 0;
			row < std::min<std::uint32_t>(6u, axial + 1u);
			++row)
		{
			for (std::uint32_t column = 0; column < radial; ++column)
			{
				TransitionVertex& vertex =
					runtime.projector_portal.vertices[
						1u + row * radial + column];
				const glm::vec3 world_position =
					projector_position
						+ projector_orientation * vertex.position;
				const float distance =
					glm::distance(
						world_position,
						runtime.projectors[row].position);
				const float scale = std::clamp(
					(1.0f - std::clamp(
						distance * 0.00002f, 0.0f, 1.0f))
						* 3.0f,
					0.0f,
					1.0f);
				vertex.position *= scale;
			}
		}
		for (std::uint32_t row = 6u; row <= axial; ++row)
		{
			for (std::uint32_t column = 0; column < radial; ++column)
			{
				runtime.projector_portal.vertices[
					1u + row * radial + column].position =
						glm::vec3{0.0f};
			}
		}
		const float uv_elapsed =
			static_cast<float>(elapsed_ticks) * 0.001f;
		const float u_delta = uv_elapsed * -3.5f;
		const float v_delta = uv_elapsed * -0.6f;
		for (TransitionVertex& vertex
			: runtime.projector_portal.vertices)
		{
			vertex.uv.x += u_delta;
			vertex.uv.y += v_delta;
		}
	}
	runtime.projector_portal.active = projector_submit_roll(world);
	runtime.projector_submission_tick = tick;
}

void transition_effects_service_particles(
	World& world,
	std::uint32_t tick,
	std::uint32_t elapsed_ticks,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward)
{
	WGateEffectsRuntime& runtime = world.transition_effects.wgate;
	for (WGateContext& context : runtime.contexts)
	{
		if (!context.active || context.mode != WGateMode::ship)
		{
			continue;
		}
		for (std::size_t index = 0;
			index < context.particle_emitters.size();
			++index)
		{
			if (!context.emitters[index].active)
			{
				continue;
			}
			ParticleEmitter& emitter =
				context.particle_emitters[index];
			emitter.local_position = context.emitters[index].position;
			emitter.local_basis = context.beams[index].orientation;
			particle_emitter_service_owned(
				world,
				emitter,
				tick,
				elapsed_ticks,
				camera_position,
				camera_forward);
		}
	}
}
}
