#include "game/model_animation.hpp"

#include "assets/gameplay_model.hpp"
#include "game/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace sl_open::game
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = kPi * 2.0f;

bool ascii_case_equal(const char* left, const char* right)
{
	if (left == nullptr || right == nullptr)
	{
		return false;
	}
	for (;; ++left, ++right)
	{
		char a = *left;
		char b = *right;
		if (a >= 'A' && a <= 'Z')
		{
			a = static_cast<char>(a - 'A' + 'a');
		}
		if (b >= 'A' && b <= 'Z')
		{
			b = static_cast<char>(b - 'A' + 'a');
		}
		if (a != b)
		{
			return false;
		}
		if (a == '\0')
		{
			return true;
		}
	}
}

void mark_parent_chain(
	WorldObject& object,
	std::uint16_t model_reference)
{
	while (model_reference < object.model_references.size())
	{
		ObjectModelReference& model =
			object.model_references[model_reference];
		if (model.sequence_active)
		{
			return;
		}
		model.sequence_active = true;
		if (model.parent_reference < 0)
		{
			return;
		}
		model_reference =
			static_cast<std::uint16_t>(model.parent_reference);
	}
}

void sample_sequence(
	const assets::GameplaySequence& sequence,
	float time,
	glm::vec3& euler,
	glm::vec3& translation)
{
	euler = glm::vec3{0.0f};
	translation = glm::vec3{0.0f};
	if (sequence.keys.empty())
	{
		return;
	}

	glm::vec3 previous_euler{0.0f};
	glm::vec3 previous_translation{0.0f};
	float previous_time = 0.0f;
	for (const assets::GameplaySequenceKey& key : sequence.keys)
	{
		euler = key.euler;
		translation = key.translation;
		const float key_time = static_cast<float>(key.time);
		if (time <= key_time)
		{
			if (previous_time != key_time)
			{
				const float fraction =
					(time - previous_time)
					/ (key_time - previous_time);
				euler =
					previous_euler * (1.0f - fraction)
					+ euler * fraction;
				translation =
					previous_translation * (1.0f - fraction)
					+ translation * fraction;
			}
			return;
		}
		previous_euler = euler;
		previous_translation = translation;
		previous_time = key_time;
	}
}

glm::mat4 compose_local_pose(
	const ObjectModelReference& model,
	const glm::vec3& euler,
	const glm::vec3& translation)
{
	const glm::mat3 rotation =
		glm::transpose(model.source_local_basis)
		* math::rotation_from_euler(euler)
		* model.source_local_basis;
	const glm::vec3 position =
		model.exported_position - model.parent_anchor
		+ model.rest_translation
		- rotation * (model.rest_translation - translation);
	// translate(position) * rotation * scale(runtime_scale), written
	// directly as an affine matrix. The general mat4 products produce these
	// exact columns but are especially expensive in the unoptimized build.
	glm::mat4 local{1.0f};
	local[0] = glm::vec4(
		rotation[0] * model.runtime_scale.x, 0.0f);
	local[1] = glm::vec4(
		rotation[1] * model.runtime_scale.y, 0.0f);
	local[2] = glm::vec4(
		rotation[2] * model.runtime_scale.z, 0.0f);
	local[3] = glm::vec4(position, 1.0f);
	return model.parent_pose_prefix * local;
}

bool compute_current_transform(
	WorldObject& object,
	std::uint16_t model_reference,
	std::uint8_t* state)
{
	if (model_reference >= object.model_references.size())
	{
		return false;
	}
	if (state[model_reference] == 2)
	{
		return true;
	}
	if (state[model_reference] == 1)
	{
		return false;
	}
	state[model_reference] = 1;
	ObjectModelReference& model =
		object.model_references[model_reference];
	glm::mat4 parent{1.0f};
	if (model.parent_reference >= 0)
	{
		const std::uint16_t parent_reference =
			static_cast<std::uint16_t>(model.parent_reference);
		if (!compute_current_transform(
			object, parent_reference, state))
		{
			return false;
		}
		parent =
			object.model_references[parent_reference].local_transform;
	}
	model.local_transform =
		parent
		* compose_local_pose(
			model,
			model.current_pose_euler,
			model.current_pose_translation);
	state[model_reference] = 2;
	return true;
}

void recompute_all_current_transforms(WorldObject& object)
{
	std::vector<std::uint8_t> state(
		object.model_references.size(), 0);
	for (std::uint16_t model_reference = 0;
		model_reference < object.model_references.size();
		++model_reference)
	{
		compute_current_transform(
			object, model_reference, state.data());
	}
}

glm::vec3 interpolate_euler(
	const glm::vec3& previous,
	const glm::vec3& current,
	float fraction)
{
	glm::vec3 delta = current - previous;
	for (std::uint32_t axis = 0; axis < 3; ++axis)
	{
		if (delta[axis] > kPi)
		{
			delta[axis] -= kTwoPi;
		}
		if (delta[axis] < -kPi)
		{
			delta[axis] += kTwoPi;
		}
	}
	return previous + delta * fraction;
}

glm::mat4 compute_render_transform(
	const WorldObject& object,
	std::uint16_t model_reference,
	float service_fraction)
{
	if (model_reference >= object.model_references.size())
	{
		return glm::mat4{1.0f};
	}
	const ObjectModelReference& model =
		object.model_references[model_reference];
	glm::mat4 parent{1.0f};
	if (model.parent_reference >= 0)
	{
		parent = compute_render_transform(
			object,
			static_cast<std::uint16_t>(model.parent_reference),
			service_fraction);
	}
	const glm::vec3 euler = interpolate_euler(
		model.previous_pose_euler,
		model.current_pose_euler,
		service_fraction);
	const glm::vec3 translation =
		model.previous_pose_translation
			* (1.0f - service_fraction)
		+ model.current_pose_translation * service_fraction;
	return parent * compose_local_pose(model, euler, translation);
}

bool compute_scene_transform(
	WorldObject& object,
	std::uint16_t model_reference,
	float service_fraction,
	std::uint8_t* state)
{
	if (model_reference >= object.model_references.size())
	{
		return false;
	}
	if (state[model_reference] == 2)
	{
		return true;
	}
	if (state[model_reference] == 1)
	{
		return false;
	}
	state[model_reference] = 1;
	ObjectModelReference& model =
		object.model_references[model_reference];
	glm::mat4 parent{1.0f};
	if (model.parent_reference >= 0)
	{
		const std::uint16_t parent_reference =
			static_cast<std::uint16_t>(model.parent_reference);
		if (!compute_scene_transform(
			object,
			parent_reference,
			service_fraction,
			state))
		{
			return false;
		}
		parent = object.model_references[parent_reference].scene_transform;
	}
	const glm::vec3 euler = interpolate_euler(
		model.previous_pose_euler,
		model.current_pose_euler,
		service_fraction);
	const glm::vec3 translation =
		model.previous_pose_translation * (1.0f - service_fraction)
			+ model.current_pose_translation * service_fraction;
	model.scene_transform =
		parent * compose_local_pose(model, euler, translation);
	state[model_reference] = 2;
	return true;
}

bool event_crossed(
	std::int32_t time,
	std::int32_t first_begin,
	std::int32_t first_end,
	std::int32_t second_begin,
	std::int32_t second_end)
{
	return (time >= first_begin && time < first_end)
		|| (time >= second_begin && time < second_end);
}

std::int32_t rounded_time(float value)
{
	return static_cast<std::int32_t>(std::nearbyint(value));
}

void dispatch_events(
	World& world,
	WorldObject& object,
	std::uint16_t model_reference,
	const assets::GameplaySequence& sequence,
	std::int32_t first_begin,
	std::int32_t first_end,
	std::int32_t second_begin,
	std::int32_t second_end,
	std::uint32_t simulation_tick,
	const ModelAnimationEventHandler& handler)
{
	for (const assets::GameplaySequenceEvent& event :
		sequence.events)
	{
		if (!event_crossed(
			event.time,
			first_begin,
			first_end,
			second_begin,
			second_end))
		{
			continue;
		}
		if (event.type == 0 && handler.fire_type0 != nullptr)
		{
			handler.fire_type0(
				handler.context,
				world,
				object,
				model_reference,
				simulation_tick);
		}
		else if (event.type == 2
			&& handler.flash_type2 != nullptr)
		{
			handler.flash_type2(
				handler.context,
				world,
				object,
				model_reference,
				simulation_tick);
		}
	}
}

void service_model(
	World& world,
	WorldObject& object,
	std::uint16_t model_reference,
	std::uint32_t simulation_tick,
	const ModelAnimationEventHandler& handler)
{
	ObjectModelReference& model =
		object.model_references[model_reference];
	if (!model.sequence_active)
	{
		return;
	}

	model.previous_pose_euler = model.current_pose_euler;
	model.previous_pose_translation =
		model.current_pose_translation;
	if (model.sequences == nullptr
		|| model.sequence_mode == 0
		|| model.sequence_rate == 0.0f)
	{
		model.sequence_active = false;
		return;
	}
	if (model.sequence_index < 0
		|| static_cast<std::size_t>(model.sequence_index)
			>= model.sequences->size())
	{
		return;
	}

	const assets::GameplaySequence& sequence =
		(*model.sequences)[
			static_cast<std::size_t>(model.sequence_index)];
	const float old_time = model.sequence_time;
	model.sequence_time += model.sequence_rate;
	float pose_time = model.sequence_time;
	std::int32_t first_begin = 0;
	std::int32_t first_end = 0;
	std::int32_t second_begin = 0;
	std::int32_t second_end = 0;

	const float duration = static_cast<float>(sequence.duration);
	if (duration <= 0.0f)
	{
		return;
	}
	if (model.sequence_mode == 1)
	{
		if (model.sequence_time >= duration)
		{
			model.sequence_time = duration;
			model.sequence_rate = 0.0f;
		}
		if (model.sequence_time <= 0.0f)
		{
			model.sequence_time = 0.0f;
			model.sequence_rate = 0.0f;
		}
		pose_time = model.sequence_time;
		first_begin = rounded_time(old_time);
		first_end = rounded_time(model.sequence_time);
		second_begin = first_begin;
		second_end = first_end;
	}
	else if (model.sequence_mode == 2)
	{
		const std::int32_t old_integer = rounded_time(old_time);
		first_begin = old_integer;
		first_end = rounded_time(model.sequence_time);
		second_begin = old_integer;
		second_end = first_end;
		if (model.sequence_time > duration)
		{
			second_begin = 0;
			do
			{
				model.sequence_time -= duration;
				first_begin = rounded_time(old_time);
				first_end = rounded_time(duration);
				second_end = rounded_time(model.sequence_time);
			}
			while (model.sequence_time > duration);
		}
		pose_time = model.sequence_time;
	}
	else if (model.sequence_mode == 3)
	{
		const float period = duration + duration;
		while (model.sequence_time > period)
		{
			model.sequence_time -= period;
		}
		pose_time = model.sequence_time > duration
			? period - model.sequence_time
			: model.sequence_time;
	}
	else
	{
		return;
	}

	sample_sequence(
		sequence,
		pose_time,
		model.sequence_euler,
		model.sequence_translation);
	for (std::uint32_t axis = 0; axis < 3; ++axis)
	{
		if (model.suppress_anim_rotation[axis] != 0)
		{
			model.sequence_euler[axis] = 0.0f;
		}
	}
	model.current_pose_euler =
		model.sequence_euler + model.joint_euler_delta;
	model.current_pose_translation =
		model.sequence_translation;
	// object_apply_model_sequence_pose (0x00499f40) publishes the sampled
	// transform before object_advance_model_sequences dispatches crossed
	// events, so event-fired guns and lights observe this new pose.
	recompute_all_current_transforms(object);
	dispatch_events(
		world,
		object,
		model_reference,
		sequence,
		first_begin,
		first_end,
		second_begin,
		second_end,
		simulation_tick,
		handler);
}
}

bool model_animation_start_index(
	WorldObject& object,
	std::uint16_t model_reference,
	std::int16_t sequence,
	float time,
	std::int16_t mode,
	float rate)
{
	if (model_reference >= object.model_references.size())
	{
		return false;
	}
	ObjectModelReference& model =
		object.model_references[model_reference];
	if (model.sequences == nullptr
		|| sequence < 0
		|| static_cast<std::size_t>(sequence)
			>= model.sequences->size())
	{
		return false;
	}
	if (mode == -1)
	{
		mode = (*model.sequences)[
			static_cast<std::size_t>(sequence)].default_mode;
	}
	model.sequence_index = sequence;
	model.sequence_mode = mode;
	if (time >= 0.0f)
	{
		model.sequence_time = time;
	}
	model.sequence_rate = rate;
	if (mode != 0)
	{
		mark_parent_chain(object, model_reference);
	}
	return true;
}

bool model_animation_start_named(
	WorldObject& object,
	std::uint16_t model_reference,
	const char* name,
	float time,
	std::int16_t mode,
	float rate)
{
	if (model_reference >= object.model_references.size())
	{
		return false;
	}
	const ObjectModelReference& model =
		object.model_references[model_reference];
	if (model.sequences == nullptr)
	{
		return false;
	}
	for (std::size_t index = 0;
		index < model.sequences->size();
		++index)
	{
		if (ascii_case_equal(
			(*model.sequences)[index].name, name))
		{
			return model_animation_start_index(
				object,
				model_reference,
				static_cast<std::int16_t>(index),
				time,
				mode,
				rate);
		}
	}
	return false;
}

bool model_animation_start_builtin(
	WorldObject& object,
	std::uint16_t model_reference,
	BuiltinModelSequence sequence,
	float time,
	std::int16_t mode,
	float rate)
{
	const char* names[] = {"startup", "fire", "deploy"};
	return model_animation_start_named(
		object,
		model_reference,
		names[static_cast<std::uint8_t>(sequence)],
		time,
		mode,
		rate);
}

void model_animation_start_named_direct_children(
	WorldObject& object,
	const char* name)
{
	for (std::uint16_t index = 0;
		index < object.model_references.size();
		++index)
	{
		if (object.model_references[index].owner_scope == 0
			&& object.model_references[index].parent_reference < 0)
		{
			model_animation_start_named(
				object, index, name, 0.0f, -1, 4.0f);
		}
	}
}

void model_animation_stop_named_direct_children(
	WorldObject& object,
	const char* name)
{
	for (std::uint16_t index = 0;
		index < object.model_references.size();
		++index)
	{
		ObjectModelReference& model =
			object.model_references[index];
		if (model.owner_scope == 0 && model.parent_reference < 0)
		{
			model_animation_start_named(
				object,
				index,
				name,
				model.sequence_time,
				0,
				4.0f);
		}
	}
}

void model_animation_reverse_named_direct_children(
	WorldObject& object,
	const char* name)
{
	for (std::uint16_t index = 0;
		index < object.model_references.size();
		++index)
	{
		ObjectModelReference& model =
			object.model_references[index];
		if (model.owner_scope == 0 && model.parent_reference < 0)
		{
			model_animation_start_named(
				object,
				index,
				name,
				model.sequence_time,
				-1,
				-4.0f);
		}
	}
}

void model_animation_initialize_object(WorldObject& object)
{
	// GameObject_finalize_model_tree calls Object_model_apply_sequence_pose(0)
	// for every retained model at 0x00476180 before it starts any named
	// playback. Runtime model records are zero-initialized, so sequence zero
	// supplies the authored construction pose even when its name is empty and
	// its playback mode is stopped. Several DEST alternates (including Baxter
	// Lower Dest) depend on a non-identity first key to occupy the same model
	// space as the ordinary hull.
	for (ObjectModelReference& model : object.model_references)
	{
		if (model.sequences == nullptr || model.sequences->empty())
		{
			continue;
		}
		model.sequence_index = 0;
		sample_sequence(
			(*model.sequences)[0],
			0.0f,
			model.sequence_euler,
			model.sequence_translation);
		for (std::uint32_t axis = 0; axis < 3; ++axis)
		{
			if (model.suppress_anim_rotation[axis] != 0)
			{
				model.sequence_euler[axis] = 0.0f;
			}
		}
		model.current_pose_euler =
			model.sequence_euler + model.joint_euler_delta;
		model.previous_pose_euler = model.current_pose_euler;
		model.current_pose_translation = model.sequence_translation;
		model.previous_pose_translation = model.current_pose_translation;
	}
	recompute_all_current_transforms(object);
	for (ObjectModelReference& model : object.model_references)
	{
		model.scene_transform = model.local_transform;
	}

	for (std::uint16_t index = 0;
		index < object.model_references.size();
		++index)
	{
		const ObjectModelReference& model =
			object.model_references[index];
		if (model.sequences != nullptr
			&& !model.sequences->empty())
		{
			model_animation_start_builtin(
				object,
				index,
				BuiltinModelSequence::startup,
				0.0f,
				-1,
				4.0f);
		}
	}
}

void model_animation_set_playback(
	WorldObject& object,
	std::uint16_t model_reference,
	std::int16_t mode,
	float rate)
{
	if (model_reference >= object.model_references.size())
	{
		return;
	}
	ObjectModelReference& model =
		object.model_references[model_reference];
	model.sequence_mode = mode;
	model.sequence_rate = rate;
	if (mode != 0)
	{
		// Object_model_mark_transform_dirty, LANCER.EXE 0x0049a2a0,
		// marks the complete retained parent chain after direct playback
		// field writes such as the kind-two gun callback performs.
		mark_parent_chain(object, model_reference);
	}
}

void model_animation_recompute_pose(
	WorldObject& object,
	std::uint16_t model_reference)
{
	if (model_reference >= object.model_references.size())
	{
		return;
	}
	ObjectModelReference& model =
		object.model_references[model_reference];
	// Objects_rotate_model_euler_delta_clamped (LANCER.EXE 0x0049b520)
	// calls Object_model_mark_transform_dirty at 0x0049a2a0 before the
	// immediate 0x0049a140 pose recomputation.  Besides propagating through
	// the retained parent chain, that keeps the joint in the next fixed-rate
	// pose snapshot.  Without this publication the renderer repeatedly
	// interpolates an articulated turret from its construction pose whenever
	// the four-tick service phase wraps to zero.
	mark_parent_chain(object, model_reference);
	model.current_pose_euler =
		model.sequence_euler + model.joint_euler_delta;
	recompute_all_current_transforms(object);
}

glm::mat4 model_animation_render_transform(
	const WorldObject& object,
	std::uint16_t model_reference,
	float service_fraction)
{
	return compute_render_transform(
		object,
		model_reference,
		std::clamp(service_fraction, 0.0f, 1.0f));
}

void model_animation_publish_scene_transforms(
	WorldObject& object,
	float service_fraction,
	std::vector<std::uint8_t>& traversal_state)
{
	const float fraction = std::clamp(service_fraction, 0.0f, 1.0f);
	traversal_state.assign(object.model_references.size(), 0);
	for (std::uint16_t model_reference = 0;
		model_reference < object.model_references.size();
		++model_reference)
	{
		compute_scene_transform(
			object,
			model_reference,
			fraction,
			traversal_state.data());
	}
}

const assets::GameplayLocator* model_animation_find_locator(
	const WorldObject& object,
	std::uint16_t model_reference,
	std::int16_t type,
	std::uint16_t ordinal)
{
	if (model_reference >= object.model_references.size())
	{
		return nullptr;
	}
	const ObjectModelReference& model =
		object.model_references[model_reference];
	if (model.locators == nullptr)
	{
		return nullptr;
	}
	for (const assets::GameplayLocator& locator : *model.locators)
	{
		if (locator.source_node != model.source_node
			|| locator.type != type)
		{
			continue;
		}
		if (ordinal-- == 0)
		{
			return &locator;
		}
	}
	return nullptr;
}

glm::mat4 model_animation_locator_transform(
	const WorldObject& object,
	std::uint16_t model_reference,
	const assets::GameplayLocator& locator,
	float service_fraction)
{
	const glm::mat4 model_transform =
		model_animation_render_transform(
			object, model_reference, service_fraction);
	return model_transform
		* glm::translate(glm::mat4{1.0f}, locator.position)
		* glm::mat4(locator.basis);
}

void model_animation_service(
	World& world,
	std::uint32_t simulation_tick,
	const ModelAnimationEventHandler& events)
{
	for (WorldObject& object : world.objects)
	{
		// GameObjects_quarter_tick_update (0x004774d0) excludes object
		// flag mask 0x420 before entering the retained model forest.
		if (!object.active
			|| (object.runtime_flags & 0x00000420u) != 0)
		{
			continue;
		}
		bool marked = false;
		std::vector<std::vector<std::uint16_t>> children(
			object.model_references.size());
		std::vector<std::uint16_t> roots;
		for (std::uint16_t model_reference = 0;
			model_reference < object.model_references.size();
			++model_reference)
		{
			const std::int16_t parent =
				object.model_references[model_reference]
					.parent_reference;
			if (parent < 0
				|| static_cast<std::size_t>(parent)
					>= object.model_references.size())
			{
				roots.push_back(model_reference);
			}
			else
			{
				children[static_cast<std::size_t>(parent)]
					.push_back(model_reference);
			}
		}
		const auto service_tree =
			[&](auto&& self,
				std::uint16_t model_reference,
				bool parent_traversable) -> void
		{
			ObjectModelReference& model =
				object.model_references[model_reference];
			const bool eligible =
				parent_traversable
				&& model.sequence_active
				&& !model.removed
				&& (model.runtime_flags & 0x00a0u) == 0;
			if (eligible)
			{
				marked = true;
				service_model(
					world,
					object,
					model_reference,
					simulation_tick,
					events);
			}
			for (const std::uint16_t child
				: children[model_reference])
			{
				self(self, child, eligible);
			}
			if (eligible && model.parent_reference >= 0)
			{
				// object_advance_model_sequences (0x00476c90) restores
				// the parent's 0x800 mark whenever it admits a marked
				// child, keeping otherwise-static ancestor links live.
				object.model_references[
					static_cast<std::size_t>(
						model.parent_reference)]
					.sequence_active = true;
			}
		};
		for (const std::uint16_t root : roots)
		{
			service_tree(service_tree, root, true);
		}
		if (marked)
		{
			recompute_all_current_transforms(object);
		}
	}
}
}
