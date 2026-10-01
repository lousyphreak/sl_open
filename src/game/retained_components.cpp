#include "game/retained_components.hpp"

#include <cmath>
#include <cstring>

namespace sl_open::game
{
namespace
{
constexpr std::uint32_t kNodeDestroyed = 0x0010u;
constexpr std::uint32_t kNodeHidden = 0x0020u;
constexpr std::uint32_t kMeshStaticLighting = 0x00040000u;

std::int16_t component_model_index(
	const WorldObject& object,
	std::int16_t component)
{
	if (component < 0 || component >= object.component_count)
	{
		return -1;
	}
	const std::int16_t retained =
		object.components[component].model_reference;
	if (retained >= 0
		&& retained
			< static_cast<std::int16_t>(object.model_references.size())
		&& !object.model_references[retained].removed)
	{
		return retained;
	}
	for (std::size_t index = 0;
		index < object.model_references.size();
		++index)
	{
		const ObjectModelReference& model = object.model_references[index];
		if (!model.removed && model.component_index == component)
		{
			return static_cast<std::int16_t>(index);
		}
	}
	return -1;
}

void clear_model_from_guns(
	WorldObject& object,
	std::int16_t model_index)
{
	for (std::uint8_t mount_index = 0;
		mount_index < object.gun_mount_count;
		++mount_index)
	{
		GunMount& mount = object.gun_mounts[mount_index];
		bool referenced = false;
		for (const std::int16_t part : mount.part_references)
		{
			referenced = referenced || part == model_index;
		}
		if (!referenced)
		{
			continue;
		}
		// Objects_runtime_node_destroy (LANCER.EXE 0x00499e30)
		// clears every gun record that retained the removed node.
		mount.mount_kind = -1;
		mount.target_object = UINT16_MAX;
		mount.target_component = -1;
		mount.part_references.fill(-1);
	}
}

void publish_pose(
	WorldObject& object,
	const glm::vec3& position,
	const glm::mat3& orientation)
{
	// Objects_set_all_position_states/Objects_set_all_orientation_states
	// (LANCER.EXE 0x0049b600/0x0049b650). The reimplementation has the
	// current and previous offline snapshots; publish both atomically.
	object.previous_position = position;
	object.position = position;
	object.previous_orientation = orientation;
	object.orientation = orientation;
	object.scene_position = position;
	object.scene_orientation = orientation;
}
}

ObjectModelReference* retained_find_named_model(
	WorldObject& object,
	const char* name)
{
	for (ObjectModelReference& model : object.model_references)
	{
		if (!model.removed && std::strcmp(model.name, name) == 0)
		{
			return &model;
		}
	}
	return nullptr;
}

const ObjectModelReference* retained_find_named_model(
	const WorldObject& object,
	const char* name)
{
	for (const ObjectModelReference& model : object.model_references)
	{
		if (!model.removed && std::strcmp(model.name, name) == 0)
		{
			return &model;
		}
	}
	return nullptr;
}

bool retained_set_named_model_hidden(
	WorldObject& object,
	const char* name,
	bool hidden)
{
	ObjectModelReference* model =
		retained_find_named_model(object, name);
	if (model == nullptr)
	{
		return false;
	}
	if (hidden)
	{
		model->runtime_flags |= kNodeHidden;
	}
	else
	{
		model->runtime_flags &= ~kNodeHidden;
	}
	return true;
}

bool retained_set_named_model_scale(
	WorldObject& object,
	const char* name,
	float scale)
{
	ObjectModelReference* model =
		retained_find_named_model(object, name);
	if (model == nullptr)
	{
		return false;
	}
	model->runtime_scale = glm::vec3{scale};
	return true;
}

void retained_set_root_model_scale(
	WorldObject& object,
	const glm::vec3& scale)
{
	for (ObjectModelReference& model : object.model_references)
	{
		if (!model.removed && model.parent_reference < 0)
		{
			model.runtime_scale = scale;
		}
	}
}

ObjectModelReference* retained_component_model(
	WorldObject& object,
	std::int16_t component)
{
	const std::int16_t model_index =
		component_model_index(object, component);
	return model_index < 0
		? nullptr
		: &object.model_references[model_index];
}

const ObjectModelReference* retained_component_model(
	const WorldObject& object,
	std::int16_t component)
{
	const std::int16_t model_index =
		component_model_index(object, component);
	return model_index < 0
		? nullptr
		: &object.model_references[model_index];
}

std::size_t retained_set_component_disabled(
	WorldObject& object,
	std::int16_t component,
	bool disabled)
{
	const std::int16_t selected_index =
		component_model_index(object, component);
	if (selected_index < 0)
	{
		return 0;
	}
	const ObjectModelReference& selected =
		object.model_references[selected_index];
	const std::uint16_t owner_scope = selected.owner_scope;
	const std::uint32_t part_group = selected.part_group_id;
	std::size_t changed = 0;
	for (ObjectModelReference& candidate : object.model_references)
	{
		if (candidate.removed
			|| candidate.owner_scope != owner_scope
			|| candidate.part_group_id != part_group)
		{
			continue;
		}
		// DisableObject's component visitor (LANCER.EXE 0x004583e0)
		// applies the request per sibling, reversed for authored DEST
		// alternates (source flag 0x04).
		const bool hidden =
			disabled != ((candidate.source_flags & 0x0004u) != 0);
		if (hidden)
		{
			candidate.runtime_flags |= kNodeHidden;
		}
		else
		{
			candidate.runtime_flags &= ~kNodeHidden;
		}
		++changed;
	}
	return changed;
}

std::size_t retained_set_lights_disabled(
	WorldObject& object,
	bool disabled)
{
	if (disabled)
	{
		object.runtime_flags |= 0x00002000u;
	}
	else
	{
		object.runtime_flags &= ~0x00002000u;
	}
	std::size_t changed = 0;
	for (ObjectModelReference& model : object.model_references)
	{
		if (model.removed || (model.source_flags & 0x0040u) == 0)
		{
			continue;
		}
		// DisableLights recursion (LANCER.EXE 0x00459090) switches the
		// Surrender mesh-data per-vertex base flag on every qualifying
		// retained node. The renderer consumes this flag per instance.
		if (disabled)
		{
			model.render_flags &= ~kMeshStaticLighting;
		}
		else
		{
			model.render_flags |= kMeshStaticLighting;
		}
		++changed;
	}
	return changed;
}

bool retained_set_model_light_channel(
	WorldObject& object,
	std::uint16_t model_reference,
	std::uint16_t channel,
	bool enabled)
{
	if (model_reference >= object.model_references.size())
	{
		return false;
	}
	ObjectModelReference& model =
		object.model_references[model_reference];
	if (model.removed
		|| (model.source_flags & 0x0080u) == 0
		|| channel >= model.light_channels.size())
	{
		return false;
	}
	model.light_channels[channel] = enabled ? 1u : 0u;
	return true;
}

std::size_t retained_set_model_lights(
	WorldObject& object,
	std::uint16_t model_reference,
	bool enabled)
{
	if (model_reference >= object.model_references.size())
	{
		return 0;
	}
	ObjectModelReference& model =
		object.model_references[model_reference];
	if (model.removed || (model.source_flags & 0x0080u) == 0)
	{
		return 0;
	}
	std::fill(
		model.light_channels.begin(),
		model.light_channels.end(),
		enabled ? 1u : 0u);
	return model.light_channels.size();
}

std::size_t retained_set_all_model_lights(
	WorldObject& object,
	bool enabled)
{
	std::size_t changed = 0;
	for (std::uint16_t model_reference = 0;
		model_reference < object.model_references.size();
		++model_reference)
	{
		changed += retained_set_model_lights(
			object, model_reference, enabled);
	}
	return changed;
}

std::size_t retained_destroy_component_group(
	WorldObject& object,
	std::int16_t component,
	bool keep_authored_hidden_parts)
{
	const std::int16_t selected_index =
		component_model_index(object, component);
	if (selected_index < 0)
	{
		return 0;
	}
	const ObjectModelReference& selected =
		object.model_references[selected_index];
	const std::uint16_t owner_scope = selected.owner_scope;
	const std::uint32_t part_group = selected.part_group_id;
	std::size_t removed_count = 0;
	for (std::size_t index = 0;
		index < object.model_references.size();
		++index)
	{
		ObjectModelReference& candidate = object.model_references[index];
		if (candidate.removed
			|| candidate.owner_scope != owner_scope
			|| candidate.part_group_id != part_group)
		{
			continue;
		}
		const bool authored_hidden =
			(candidate.runtime_flags & kNodeHidden) != 0;
		if (authored_hidden && keep_authored_hidden_parts)
		{
			candidate.runtime_flags &= ~kNodeHidden;
			continue;
		}
		// DestroySubObject's traversal (LANCER.EXE 0x00459750) performs
		// type effects only for the visible member of the group.
		if (!authored_hidden && candidate.model_type == 5
			&& object.engine_component_count != 0)
		{
			object.engine_component_scale -=
				1.0f
				/ static_cast<float>(object.engine_component_count);
		}
		if (!authored_hidden && candidate.model_type == 6)
		{
			object.runtime_flags &= ~0x00004000u;
		}
		clear_model_from_guns(
			object, static_cast<std::int16_t>(index));
		candidate.removed = true;
		candidate.runtime_flags |= kNodeDestroyed;
		if (candidate.component_index >= 0
			&& candidate.component_index < object.component_count)
		{
			object.components[
				candidate.component_index].model_reference = -1;
		}
		++removed_count;
	}
	return removed_count;
}

std::size_t retained_set_turret_target(
	WorldObject& object,
	std::int16_t component,
	std::uint16_t target_object)
{
	const std::int16_t model_index =
		component_model_index(object, component);
	if (model_index < 0)
	{
		return 0;
	}
	std::size_t changed = 0;
	for (std::uint8_t index = 0;
		index < object.gun_mount_count;
		++index)
	{
		GunMount& mount = object.gun_mounts[index];
		if (mount.mount_kind != 1
			|| mount.part_references[0] != model_index)
		{
			continue;
		}
		// TurretSetTarget's visitor (LANCER.EXE 0x00459bf0) writes the
		// resolved live-object index and clears the component selector on
		// every matching kind-one mount.
		mount.target_object = target_object;
		mount.target_component = -1;
		++changed;
	}
	return changed;
}

bool retained_replace_component(
	WorldObject& source,
	std::int16_t component,
	WorldObject& replacement)
{
	ObjectModelReference* model =
		retained_component_model(source, component);
	if (model == nullptr)
	{
		return false;
	}
	// SR_object_concate_parents at LANCER.EXE 0x4c3570 produces the same
	// root-to-node matrix retained in local_transform, including animated
	// ancestors and scale.
	const glm::vec3 local_position = glm::vec3(model->local_transform[3]);
	glm::mat3 replacement_orientation =
		source.orientation * glm::mat3(model->local_transform);
	if (replacement.type == 0x91u)
	{
		// ReplaceSubObject (LANCER.EXE 0x00459cf0) applies the
		// replacement-type-0x91 correction in this exact order.
		replacement_orientation *= glm::mat3(
			glm::rotate(
				glm::mat4{1.0f},
				glm::pi<float>(),
				glm::vec3{0.0f, 1.0f, 0.0f})
			* glm::rotate(
				glm::mat4{1.0f},
				-glm::half_pi<float>(),
				glm::vec3{1.0f, 0.0f, 0.0f}));
	}
	publish_pose(
		replacement,
		source.position + source.orientation * local_position,
		replacement_orientation);
	// Retail suppresses only the selected retained node. It does not
	// destroy the part group or mutate component health.
	model->runtime_flags |= kNodeHidden;
	return true;
}
}
