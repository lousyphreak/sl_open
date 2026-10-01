#pragma once

#include "ai/types.hpp"
#include "assets/ship_stats.hpp"
#include "core/math.hpp"
#include "game/disruption_effects.hpp"
#include "game/death_effects.hpp"
#include "game/particle_emitters.hpp"
#include "game/runtime_limits.hpp"
#include "game/shields.hpp"
#include "game/transition_effects.hpp"
#include "mission/dte.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace sl_open::assets
{
struct GameplayCollisionTree;
struct GameplayFace;
struct GameplayLocator;
struct GameplayPointGroup;
struct GameplayPortal;
struct GameplaySection;
struct GameplaySequence;
struct GameplayVertex;
}

namespace sl_open::mission
{
struct Runtime;
}

namespace sl_open::game
{
// GameObject runtime flags recovered from the +0x08 field. Keep the raw
// retail values confined to this declaration block; call sites should state
// the behavior they select rather than repeat a bit number.
inline constexpr std::uint32_t kObjectFlagRenderSuppressed = 0x00000001u;
inline constexpr std::uint32_t kObjectFlagCompound = 0x00000002u;
inline constexpr std::uint32_t kObjectFlagPlaceholder = 0x00000020u;
inline constexpr std::uint32_t kObjectFlagDestroyed = 0x00000040u;
inline constexpr std::uint32_t kObjectFlagDisabled = 0x00000400u;
inline constexpr std::uint32_t kObjectFlagAvoidanceDisabled = 0x00100000u;
inline constexpr std::uint32_t kObjectFlagSimulationSuspended = 0x00200000u;
inline constexpr std::uint32_t kObjectFlagKinematic = 0x00400000u;

// These are exact masks tested by the retail spatial-query consumers. Their
// slightly different membership is intentional: the nearby-list rebuild
// excludes simulation-suspended objects, while dynamic avoidance and camera
// queries exclude objects already marked destroyed.
inline constexpr std::uint32_t kObjectSpatialQueryExcludedFlags =
	kObjectFlagPlaceholder | kObjectFlagDestroyed | kObjectFlagDisabled;
inline constexpr std::uint32_t kObjectSimulationExcludedFlags =
	kObjectFlagPlaceholder | kObjectFlagDisabled
	| kObjectFlagSimulationSuspended;

// Independent GameObject +0x0c publication state. These values overlap the
// runtime-flag bit numbers but belong to a different field and meaning.
inline constexpr std::uint32_t kObjectStatePublishPosition = 0x00000001u;
inline constexpr std::uint32_t kObjectStatePublishOrientation = 0x00000002u;
inline constexpr std::uint32_t kObjectStateScoopActive = 0x00000004u;
inline constexpr std::uint32_t kObjectStatePublishTransform =
	kObjectStatePublishPosition | kObjectStatePublishOrientation;

// Independent GameObject +0x2c transform-state publication.
inline constexpr std::uint32_t kObjectTransformChanged = 0x00000001u;
inline constexpr std::uint32_t kObjectTransformAllStatesSynchronized =
	0x00000006u;

struct ObjectHandle
{
	std::uint16_t index{UINT16_MAX};
	std::uint16_t generation{};
};

constexpr bool operator==(ObjectHandle left, ObjectHandle right)
{
	return left.index == right.index
		&& left.generation == right.generation;
}

constexpr bool operator!=(ObjectHandle left, ObjectHandle right)
{
	return !(left == right);
}

// Exact identities of the replaceable retail callback stored at
// GameObject+0x640. Consumers such as Ripper engine-flare rendering branch
// on callback identity, so throttle sign alone is not an equivalent model.
enum class FlightCallbackMode : std::uint8_t
{
	standard_forward,          // 0x004744c0
	standard_reverse,          // 0x004744d0
	linear_no_exhaust,         // 0x004744e0
	linear_with_exhaust,       // 0x00474570
	damp_velocity_0_97,        // 0x00474610
	follow_quadratic_curve,    // 0x00474640
	jump_transition,           // 0x004746d0
	provider_forward,          // 0x00474770
	provider_reverse,          // 0x00474930
	damp_velocity_0_99,        // 0x00474b00
	none,                      // null callback
};

struct FlightDemand
{
	float throttle{};
	float roll{};
	float pitch{};
	float yaw{};
	float strafe{};
	bool afterburner{};
	bool reverse{};
	bool linear_no_exhaust{};
	bool linear_with_exhaust{};
};

struct ObjectComponent
{
	glm::vec3 local_position{0.0f};
	glm::vec3 local_center{0.0f};
	float radius{};
	float health{};
	float maximum_health{};
	std::uint32_t runtime_flags{};
	std::uint32_t model_type{};
	std::uint32_t part_group_id{};
	std::uint32_t damage_group_selector{};
	std::uint16_t model_node{};
	std::int16_t model_reference{-1};
	std::uint16_t protection_state{};
};

struct CloakMeshRuntime
{
	// The retail SR mesh owns one dynamic float4 color stream and one
	// dynamic float2 texture-coordinate stream. LOD selection changes the
	// geometry viewed through those streams; it does not recreate the cloak
	// callbacks or their retained vertex values.
	std::vector<const std::vector<assets::GameplayVertex>*> normal_lods;
	std::vector<const std::vector<assets::GameplayVertex>*> secondary_lods;
	std::vector<glm::vec3> lod_bounds_min;
	std::vector<glm::vec3> lod_bounds_max;
	glm::mat4 local_transform{1.0f};
	std::vector<float> hit_alpha;
	std::vector<glm::vec2> secondary_uv;
	std::uint32_t initialized_secondary_vertices{};
	mutable std::uint16_t selected_lod{};
	bool installed{};
	bool eligible{};
};

struct ObjectModelReference
{
	const assets::GameplayCollisionTree* collision{};
	const std::vector<assets::GameplayPortal>* portals{};
	const std::vector<assets::GameplayPointGroup>* point_groups{};
	// LANCER.EXE 0x0046bf20 splits the live render mesh, preserving every
	// face attribute stream and material. These point at the retained LOD0
	// source used to reproduce that operation after the object disappears.
	const std::vector<assets::GameplayVertex>* explosion_vertices{};
	const std::vector<std::uint16_t>* explosion_indices{};
	const std::vector<assets::GameplayFace>* explosion_faces{};
	const std::vector<std::uint32_t>* explosion_face_corners{};
	const std::vector<assets::GameplaySection>* explosion_sections{};
	const std::vector<glm::vec3>* explosion_normals{};
	const std::vector<glm::vec3>* explosion_secondary_normals{};
	const std::vector<glm::vec3>* explosion_static_lighting{};
	CloakMeshRuntime cloak;
	char name[65]{};
	glm::mat4 local_transform{1.0f};
	// Object_model_update_scene publishes the articulated render-node pose
	// once per admitted mission frame. Gameplay owners which run before the
	// next publication (weapon lead and gun collision in particular) read
	// that retained live node, not the newly integrated animation pose.
	glm::mat4 scene_transform{1.0f};
	glm::mat4 base_transform{1.0f};
	glm::mat4 rest_local_transform{1.0f};
	// Embedded locator models are live child model trees in retail. Their
	// root model first crosses the locator transform before applying its
	// ordinary authored/animated local pose; ordinary object models retain
	// the identity prefix.
	glm::mat4 parent_pose_prefix{1.0f};
	glm::mat3 source_local_basis{1.0f};
	// Static joint frame expressed in the owning object's model space.
	// Embedded roots include their locator basis here so turret tracking
	// solves in the same frame in which the child object was instantiated.
	glm::mat3 object_space_joint_basis{1.0f};
	glm::vec3 exported_position{0.0f};
	glm::vec3 parent_anchor{0.0f};
	glm::vec3 rest_translation{0.0f};
	glm::vec3 joint_euler_delta{0.0f};
	glm::vec3 joint_min_degrees{0.0f};
	glm::vec3 joint_max_degrees{0.0f};
	glm::vec3 sequence_euler{0.0f};
	glm::vec3 sequence_translation{0.0f};
	glm::vec3 previous_pose_euler{0.0f};
	glm::vec3 current_pose_euler{0.0f};
	glm::vec3 previous_pose_translation{0.0f};
	glm::vec3 current_pose_translation{0.0f};
	// SR model node scale (the retail field written at +0xc0). Transition
	// controllers use uniform values for fixed-gate rings and a non-uniform
	// root value for the ordinary WarpOut ship stretch.
	glm::vec3 runtime_scale{1.0f};
	glm::vec3 bounds_min{0.0f};
	glm::vec3 bounds_max{0.0f};
	// First retail mesh quad retained for Ripper Grab's paired cargo-beam
	// endpoints (types 0x91/0xe1 use edge midpoints 0-1 and 2-3).
	glm::vec3 mesh_quad_vertices[4]{};
	std::uint8_t mesh_quad_vertex_count{};
	const std::vector<assets::GameplaySequence>* sequences{};
	const std::vector<assets::GameplayLocator>* locators{};
	float radius{};
	float health{};
	float maximum_health{};
	std::uint32_t source_flags{};
	std::uint32_t model_type{};
	std::uint32_t part_group_id{};
	std::uint32_t damage_group_selector{};
	std::uint32_t runtime_flags{};
	std::uint32_t render_flags{};
	// SR render-object +0xac. Deathmatch respawn recursively attaches one
	// shared Respawn_Material override to every node in the object's model
	// tree, then AI_DeathmatchRespawnEffect_end clears every attachment.
	std::int16_t respawn_render_override_slot{-1};
	// SR scene node +0xfc. GameObject_finalize_model_tree assigns this
	// over the complete primary-plus-locator scene hierarchy, in preorder.
	// Only tag-1 model nodes consume an ID, but intervening locator/helper
	// nodes determine where embedded model trees occur in that ordering.
	std::uint32_t network_model_id{UINT32_MAX};
	// SR render-object +0xdc. Launch cinematics set the exact compiled
	// light-exclusion mask 0x3b on selected temporary-scene nodes.
	std::uint32_t light_exclusion_mask{};
	// SR mesh-data +0x68: one enable byte at the head of each 0x14-byte
	// material record. Authored model flag 0x80 exposes these records to
	// the object-light commands, across every retained LOD variant.
	std::vector<std::uint8_t> light_channels;
	std::uint32_t suppress_anim_rotation[3]{};
	std::uint16_t source_node{};
	std::uint16_t explosion_source_model{};
	std::uint16_t owner_scope{};
	// ExplodeGeneric/ExplodeUlysses reparent retained render-object
	// subtrees beneath one of two controller-owned scene nodes. This is the
	// retained parent identity; both nodes share a transform while active.
	std::uint8_t explosion_portal_group{};
	std::int16_t parent_reference{-1};
	std::int16_t component_index{-1};
	std::int16_t sequence_index{-1};
	float sequence_time{};
	float sequence_rate{};
	std::int16_t sequence_mode{};
	bool removed{};
	bool explosion_source_attachment{};
	bool sequence_active{};
	bool forcefield{};
};

struct AttachmentSlot
{
	glm::vec3 local_position{0.0f};
	glm::mat3 local_orientation{1.0f};
	glm::mat4 hardpoint_from_model{1.0f};
	std::int16_t default_loadout[5]{-1, -1, -1, -1, -1};
	std::int16_t model_reference{-1};
	std::int16_t definition_index{-1};
	std::int16_t kind{-1};
	std::int32_t remaining_count{};
	std::uint32_t ownership_generation{};
	std::vector<CloakMeshRuntime> cloak_models;
	std::vector<std::uint16_t> cloak_model_order;
	bool cloak_install_pending{};
	bool live_model{};
	bool alternate_model{};
};

struct GunMount
{
	glm::vec3 local_position{0.0f};
	glm::mat3 local_orientation{1.0f};
	std::uint32_t part_group_id{};
	std::uint32_t pair_group_id{};
	std::uint32_t action_tick{};
	std::uint32_t next_fire_tick{};
	glm::mat4 emitter_from_part{1.0f};
	std::int16_t emitter_reference{-1};
	std::array<std::int16_t, 5> part_references{
		-1, -1, -1, -1, -1};
	std::array<std::uint8_t, 64> clearance_mask{};
	std::uint16_t target_object{UINT16_MAX};
	std::int16_t target_component{-1};
	float desired_yaw{};
	float desired_pitch{};
	std::uint32_t target_search_deadline{};
	float deploy_scalar{};
	std::uint32_t autonomous_deadline{};
	std::uint16_t autonomous_target{UINT16_MAX};
	// Kind-three mount storage comes from SR_MEM_allocate's zero-filled
	// block. Gun_kind_three_update enters its authored reload cycle before
	// installing the first six-round load.
	std::uint8_t autonomous_rounds{};
	std::uint8_t autonomous_state{};
	std::int8_t mount_kind{-1};
	std::uint8_t bullet_type{};
	std::uint8_t alternating_side{};
	bool has_clearance_mask{};
};

struct GunPair
{
	std::int8_t first{-1};
	std::int8_t second{-1};
};

struct EmbeddedModelTree
{
	std::uint16_t runtime_reference_base{};
	std::uint16_t runtime_reference_count{};
	std::uint16_t parent_runtime_reference{};
	std::uint16_t parent_scope_base{};
	std::uint16_t source_locator{};
	std::uint16_t attachment_definition{};
};

enum class CloakPhase : std::uint8_t
{
	none,
	cloaking,
	cloaked,
	decloaking,
};

struct ExhaustHazardVolume
{
	// Transform from the live outer model reference which owns the tag-2
	// attachment branch to the exhaust locator. Retail instantiates a
	// dedicated Engine_Mesh child at this pose.
	glm::mat4 locator_from_driver{1.0f};
	// Engine_Mesh is the canonical unit plume built by LANCER.EXE
	// 0x00469400. ExhaustHazard_accumulate_tree_exposure reads these helper
	// bounds, not the owning ship node's mesh bounds.
	glm::vec3 bounds_min{-1.0f, -1.0f, 0.0f};
	glm::vec3 bounds_max{1.0f, 1.0f, 1.0f};
	glm::vec3 dimensions{0.0f};
	std::uint16_t driver_model_reference{UINT16_MAX};
};

struct WorldObject
{
	glm::vec3 previous_position{0.0f};
	glm::vec3 position{0.0f};
	glm::mat3 previous_orientation{1.0f};
	glm::mat3 orientation{1.0f};
	// The root Surrender scene node is published from the two physics
	// snapshots at render time. Cameras and models consume this pose; game
	// logic continues to consume the current physics snapshot above.
	glm::vec3 scene_position{0.0f};
	glm::mat3 scene_orientation{1.0f};
	// Scripted object parenting writes the child's physics pose from the
	// parent's current fixed-service pose. Retain the relationship separately
	// so render publication can compose the child from the parent's
	// interpolated scene pose without changing gameplay snapshots.
	ObjectHandle scene_attachment_parent;
	glm::vec3 scene_attachment_position{0.0f};
	glm::mat3 scene_attachment_orientation{1.0f};
	std::int16_t scene_attachment_model{-1};
	bool scene_attachment_active{};
	glm::vec3 linear_velocity{0.0f};
	// GameObject+0x520 and +0x51c/+0x530. The ordinary collision solver
	// queues a central impulse, and the immediately following
	// GameObject_integrate consumes the complete accumulator before
	// recomputing the current pose from the retained service snapshot.
	float physics_mass{};
	glm::vec3 accumulated_linear_impulse{0.0f};
	glm::vec3 accumulated_angular_impulse{0.0f};
	std::uint32_t accumulated_impulse_count{};
	glm::vec3 bounds_min{0.0f};
	glm::vec3 bounds_max{0.0f};
	// GameObject+0x524. Launch locator placement applies this authored
	// model-space center after the locator basis.
	glm::vec3 center_of_mass{0.0f};
	// GameObject+0x548: inverse of the aggregate authored volume-integral
	// inertia tensor in the object's body frame.
	glm::mat3 inverse_inertia{0.0f};
	// GameObject+0x628. The tag-0 resource root publishes this authored
	// local camera displacement for cockpit/left/right/rear camera modes.
	glm::vec3 camera_offset{0.0f};
	// Retail retains the four control requests on GameObject. Preserve the
	// complete last applied demand so literal no-op AI callbacks do not
	// accidentally replace those fields with zero in this value-based port.
	FlightDemand control_demand;
	ai::ObjectState ai;
	float angular_x{};
	float angular_y{};
	float angular_z{};
	// Semantic equivalent of GameObject+0x56c. Retail retains angular
	// motion as a matrix, which matters when ejection composes a fixed
	// tumble with the ship's existing rotation before switching callbacks.
	glm::mat3 inertial_angular_step{1.0f};
	float speed{};
	float throttle{};
	float radius{};
	float primary_shields[4]{};
	float secondary_shields[4]{};
	float auxiliary_shields[2]{};
	float gun_energy{};
	float weapons_health{1.0f};
	float engines_health{1.0f};
	float shields_health{1.0f};
	float engine_component_scale{1.0f};
	float exhaust_scalar{};
	float effect_scale{1.0f};
	float power_cursor_x{1.0f};
	float power_cursor_y{1.0f};
	float gun_recharge_scale{1.0f};
	float engine_power_scale{1.0f};
	float shield_recharge_scale{1.0f};
	ObjectComponent components[kMaxObjectComponents];
	glm::vec3 attack_run_directions[kMaxObjectComponents]{};
	std::vector<ObjectModelReference> model_references;
	// Number of tag-1 models in the owning SRO. Embedded child-object model
	// trees follow this prefix in the implementation-only stable namespace.
	std::uint16_t primary_model_reference_count{};
	// Locator types one and five instantiate complete child model trees.
	// Keep their ranges separate from the source-ordered outer range while
	// allowing animation, weapons, damage, and rendering to use one stable
	// reference namespace.
	std::vector<EmbeddedModelTree> embedded_model_trees;
	// Recursive scene order used by Cloak_create_primary/secondary. The
	// source model array itself is not recursive because locator models live
	// in separately retained ranges.
	std::vector<std::uint16_t> cloak_model_order;
	std::vector<ExhaustHazardVolume> exhaust_hazard_volumes;
	AttachmentSlot attachments[20];
	GunMount gun_mounts[20];
	GunPair gun_pairs[20];
	std::uint64_t component_targetable_override_mask{};
	std::uint64_t component_targetable_value_mask{};
	std::uint64_t component_protection_override_mask{};
	std::uint16_t component_protection_override[
		kMaxObjectComponents]{};
	std::int32_t afterburner_fuel{};
	std::int32_t ammunition{};
	std::int32_t score{};
	std::int16_t chaff_count{};
	std::uint32_t random_seed{};
	// GameObject+0x634: the object-model light cycle adds this 0..99
	// construction-time phase to the 100 Hz gameplay tick, then converts
	// that sum to the authored millisecond light-cycle domain.
	std::uint16_t random_phase{};
	std::uint32_t creation_serial{};
	std::uint32_t runtime_flags{};
	// GameObject+0x0c. This is distinct from the ordinary gameplay flags at
	// +0x08 and the transform-state flags at +0x2c. Scoop Up owns bit four
	// while active; multiplayer state publication also consumes bits one
	// and two from this field.
	std::uint32_t state_publication_flags{};
	// GameObject+0x750. Opcode 0x1d writes the current low seven bits and
	// advances this value modulo 128. The receive path uses the same field
	// for its modulo-128 freshness window because DirectPlay does not
	// loop a sender's own object-state records back to that sender.
	std::uint8_t network_state_sequence{};
	// GameObject+0x2c. Position publication during FixedGateJumpIn marks
	// the retail transform-state bits 1 and 2 independently of +0x08.
	std::uint32_t transform_state_flags{};
	std::uint16_t active_weapon_selection_bits{};
	std::uint16_t generation{};
	std::uint16_t type{};
	std::uint16_t hud_icon{};
	// Object_component_destruction_effect_dispatch creates several authored
	// fragment object types and then removes different named pieces from the
	// shared fragment model. UINT8_MAX is the ordinary, unmodified model.
	std::uint8_t explosion_model_variant{UINT8_MAX};
	std::uint16_t mission_index{UINT16_MAX};
	// Stable equivalent of retail GameObject+0x698. Before each Executor
	// mission-reference callback, later expansion members retain the first
	// visited mission object while the first member receives null. An
	// exhaustive LANCER.EXE field-xref census found only the publisher at
	// 0x0045d738 and no retail reader.
	std::uint16_t first_expanded_mission_object{UINT16_MAX};
	std::uint16_t nav_point{UINT16_MAX};
	std::uint16_t escort_point{UINT16_MAX};
	std::uint16_t turret_target{UINT16_MAX};
	std::int16_t turret_target_component{-1};
	std::int16_t turret_component{-1};
	// GameObject+0x70c: creation seeds rand()%100, the death scheduler uses
	// `<40` to select Eject Spin, and Executor WillsBlag forces 100.
	// Exhaustive field xrefs prove this is a persistent roll, not a timer.
	std::uint32_t ejection_roll{};
	std::uint16_t gun_frame_shape{UINT16_MAX};
	std::uint8_t group{UINT8_MAX};
	std::uint8_t pilot{UINT8_MAX};
	std::uint8_t loadout_index{};
	// Multiplayer's launch owner copies the twenty definitions from the
	// selected player's 0x54-byte launch-table entry before the live object
	// is finalized. Keeping that decoded row on the live object makes model
	// initialization and later respawns consume the same snapshot.
	std::int16_t multiplayer_player_loadout[20]{};
	bool multiplayer_player_loadout_valid{};
	// GameObject+0x694: the latest attacker/retaliation target. Krasny's
	// fixed-gate split also stores its gate target here before queue clear.
	std::uint16_t last_attacker_index{UINT16_MAX};
	float attack_pressure{};
	std::uint32_t gun_action_deadline{};
	std::uint32_t missile_action_deadline{};
	std::uint32_t countermeasure_deadline{};
	// GameObject+0x654: shockwaves use a 50-tick half-open-shell
	// immunity window so one expanding wave cannot affect an object twice.
	std::uint32_t shockwave_immunity_deadline{};
	std::uint32_t cloak_transition_tick{};
	std::uint32_t cloak_last_update_tick{};
	std::uint32_t cloak_last_hit_tick{};
	glm::vec3 cloak_last_hit_position{0.0f};
	float cloak_phase_value{};
	// GameObject+0x658/+0x65c/+0x660. Unlike explode.cpp's 64 automatic
	// emitters, each live object owns this persistent damage emitter directly.
	ParticleEmitter damage_emitter;
	std::uint8_t damage_effect_severity{};
	// GameObject+0x610 bits used by the two-stage Ulysses destruction
	// callback and its 900-tick explosion controller.
	std::uint8_t ulysses_destruction_state{};
	float cloak_normal_alpha{1.0f};
	bool cloak_meshes_initialized{};
	std::uint32_t last_flyby_sound_tick{};
	float nova_charge{};
	// GameObject+0x6ac/+0x6b0. Find New Target consumes this retained
	// exclusion pair; an accepted Back Off comms order stores the player's
	// target here for 3000 gameplay ticks.
	std::uint16_t find_target_exclusion_index{UINT16_MAX};
	std::uint32_t find_target_exclusion_deadline{};
	std::uint16_t excluded_interaction_index{UINT16_MAX};
	std::uint16_t selected_target_index{UINT16_MAX};
	std::int16_t selected_target_component{-1};
	// GameObject+0x618 is one shared retained interaction link used by Dock,
	// Launch, Ripper Grab, Scoop Up, and ejection. It must not be split by
	// command family: each new owner overwrites the same retail field and
	// the collision broad phase consumes that single value.
	std::uint16_t interaction_target_link{UINT16_MAX};
	// GameObject+0x61c is Dock's second retained pair link.
	std::uint16_t docking_pair_link{UINT16_MAX};
	std::uint32_t attacker_count{};
	std::uint16_t dynamic_neighbors[10]{};
	std::uint16_t proximity_neighbors[10]{};
	// GameObject+0x710..+0x71f. Each byte is the connected-player mask for
	// one AI_sequence_sync point.
	std::uint8_t ai_sequence_sync[16]{};
	std::int16_t collision_class{};
	std::int16_t allegiance_class{2};
	std::int16_t object_class{};
	// GameObject+0x74c. MissionGroup_rebuild_allegiance_lists stamps the
	// owning group's compiled class here independently of the object's ship
	// type and current allegiance. Player comms uses class zero to decide
	// which friendly pilots may receive orders and status requests.
	std::int16_t mission_group_class{-1};
	std::uint8_t component_count{};
	std::uint8_t attack_run_direction_count{};
	std::uint8_t attachment_count{};
	std::uint8_t gun_mount_count{};
	std::uint8_t gun_pair_count{};
	std::uint8_t gun_group_count{};
	std::uint8_t selected_gun_group{};
	std::uint8_t gun_sync_frame{};
	std::uint8_t alternating_gun_side{};
	std::uint8_t protection_state{};
	// GameObject+0x764. The active deathmatch-scenario callback at
	// 0x004b2300 advances this player counter from -1 through six; Dark
	// Reign towers immediately drop a player while the counter is live.
	std::int32_t deathmatch_scenario_counter{-1};
	std::int16_t sound3d_slot{-1};
	CloakPhase cloak_phase{CloakPhase::none};
	std::uint8_t dynamic_neighbor_count{};
	std::uint8_t proximity_neighbor_count{};
	std::uint32_t engine_component_count{};
	std::uint32_t shield_generator_component_count{};
	bool gun_synchronized{};
	bool components_initialized{};
	bool component_destruction_pending{};
	bool active{};
	bool visible{};
	bool targetable{};
	bool player_slot{};
	bool player{};
	bool hostile{};
	bool incoming_missile{};
	bool primary_weapon_requested{};
	bool nova_trigger_held{};
	bool secondary_weapon_requested{};
	bool missiles_disabled{};
	bool guns_disabled{};
	bool engines_disabled{};
	bool eject_disabled{};
	bool disabled{};
	bool lights_disabled{};
	bool do_not_disturb{};
	bool avoidance_disabled{};
	bool reverse_thrust_active{};
	bool afterburner_active{};
	bool match_speed_active{};
	bool blindfire_supported{};
	bool blindfire_enabled{};
	// GameObject+0x674. HUD target-lead capture owns this per-frame flag;
	// projectile creation consumes the retained world-space lead point.
	bool blindfire_active{};
	glm::vec3 blindfire_aim_point{0.0f};
	bool cloak_supported{};
	bool spectral_supported{};
	bool special_player_loadout{};
	bool external_trail_active{};
	// update_command's recovered applies-flight result. The original keeps
	// this ownership in the active callback; the value port retains it
	// explicitly until the next 25 Hz ordinary-motion service.
	bool ordinary_motion_enabled{};
	FlightCallbackMode flight_callback_mode{
		FlightCallbackMode::standard_forward};
};

enum class WorldMissionEventType : std::uint8_t
{
	shot_at,
	cloaked,
	decloaked,
};

struct WorldMissionEvent
{
	std::uint16_t source_mission_index{UINT16_MAX};
	std::uint16_t attacker_mission_index{UINT16_MAX};
	std::uint8_t selector{UINT8_MAX};
	WorldMissionEventType type{WorldMissionEventType::shot_at};
};

struct WorldSoundEvent
{
	enum class Binding : std::uint8_t
	{
		explicit_transform,
		missile,
		object,
		model_frame,
	};

	glm::vec3 position{0.0f};
	glm::vec3 direction{0.0f, 0.0f, 1.0f};
	glm::vec3 velocity{0.0f};
	ObjectHandle object;
	std::uint32_t source_serial{};
	std::uint16_t source_index{UINT16_MAX};
	std::int16_t model_reference{-1};
	std::uint8_t definition{};
	std::uint8_t requested_class{};
	Binding binding{Binding::explicit_transform};
};

constexpr std::uint32_t kPlanetAtmosphereCapacity = 4;
constexpr std::uint32_t kExhaustHazardCapacity = kMaxGameObjects;

struct PlanetAtmosphere
{
	ObjectHandle owner;
	float spin_radians{};
};

struct World
{
	WorldObject objects[kMaxGameObjects];
	// The retail object constructor has the loaded model resource available
	// synchronously. Keep the renderer-published type radii here so runtime
	// constructors that immediately compose transforms (notably asteroid
	// Explode mode 3) observe the same model-derived radius.
	float model_radius_by_type[256]{};
	// Detached ordnance keeps the selected attachment SRO. Missile_destroy
	// reads that object's aggregate +0x59c radius for its explosion size.
	bool attachment_model_loaded[kAttachmentDefinitionCount][2]{};
	float attachment_model_radius[kAttachmentDefinitionCount][2]{};
	glm::vec3 attachment_model_bounds_min[kAttachmentDefinitionCount][2]{};
	glm::vec3 attachment_model_bounds_max[kAttachmentDefinitionCount][2]{};
	// The retail constructor also has the model root flags before any
	// mission command can address the new object.
	bool model_cloak_supported_by_type[256]{};
	ShieldRuntime shields;
	DisruptionEffectsRuntime disruption_effects;
	DeathEffectsRuntime death_effects;
	ParticleRuntime particles;
	TransitionEffectsRuntime transition_effects;
	WorldMissionEvent mission_events[kMaxMissionEvents];
	WorldSoundEvent sound_events[16];
	PlanetAtmosphere atmospheres[kPlanetAtmosphereCapacity];
	std::uint16_t exhaust_hazard_indices[kExhaustHazardCapacity]{};
	ObjectHandle player;
	// DAT_00566638 is the local player's requested cloak equipment state.
	// It changes immediately on a Cloak_set_active request, independently
	// of the gameplay flag retained throughout the visual decloak.
	bool player_cloak_control_active{};
	ObjectHandle action_center;
	float action_center_radius{220000.0f};
	// Retail's player TargetRef retains the selected live GameObject, not
	// the authored mission-object identity. A generation-bearing handle is
	// the port's stable equivalent and also prevents a recycled pool slot
	// from silently becoming the selected target.
	ObjectHandle selected_target;
	std::int16_t target_component{-1};
	std::uint16_t player_schematic_hits[4]{};
	std::uint16_t target_schematic_hits[4]{};
	// HUD smart-target state is consumed synchronously by the retail
	// primary/component/structural damage owners.
	bool smart_target_enabled{};
	// Compound collision retries suppress duplicate ShotAt publications
	// through LANCER.EXE's DAT_00545860 damage-owner guard.
	bool damage_event_suppressed{};
	// DAT_005883f8. Type-0x45 portal crossings toggle projectile
	// collision with that enclosing compound while the player is inside.
	bool player_inside_type45_compound{};
	// Damage owners refresh current-target status even when the selected
	// handle itself did not change.
	bool target_panel_refresh_requested{};
	std::uint8_t player_hit_distortion_triggers{};
	// DAT_00588724 is the one camera disturbance scalar written by flight,
	// damage, transitions, and large explosions.
	float player_camera_disturbance{};
	// DAT_00587cc8 is the shared integer whiteout exposure. Exhaust
	// hazards, fixed-gate transitions, and Uber explosions all replace it;
	// mission_update_whiteout_overlay owns its frame-delta decay.
	std::int32_t player_exhaust_exposure_percent{};
	std::uint16_t live_count{};
	std::uint16_t mission_slot_count{};
	// DAT_00539aa0 is a nonshrinking upper bound over constructed object
	// slots. Continuous network publication walks this range rather than
	// the number of currently active objects.
	std::uint16_t object_high_water{};
	std::uint16_t mission_event_count{};
	std::uint16_t exhaust_hazard_count{};
	std::uint8_t atmosphere_count{};
	std::uint8_t sound_read{};
	std::uint8_t sound_count{};
	std::uint32_t random_seed{};
	std::uint32_t next_creation_serial{};
	std::uint32_t ai_threat_clear_deadline{};
	// AI_DarkReignShoot's shared search accumulator at 0x004e1c24. Its
	// effect pool is a singleton, so towers deliberately share this value.
	std::int32_t ion_cannon_search_ticks{-1};
	std::uint32_t ion_cannon_warning_deadline{};
	std::uint16_t ion_cannon_effect_owner{UINT16_MAX};
	// Retail's active camera type at 0x00539a34. Shared object helpers
	// consult this even outside the camera runtime; notably camera mode 13
	// suppresses the engine-health term in effective maximum speed.
	std::uint8_t camera_mode{};
	// Retail's live-object player prefix at DAT_0058832c is the exact
	// iteration bound for multiplayer spectator selection and the
	// all-players-departed terminal scan.
	std::uint16_t player_prefix_count{1};
	// Missile behavior class two tests the retail network-session global in
	// addition to the live player-prefix bound. Co-op player missiles home;
	// only offline player-prefix missiles use the straight-flight branch.
	bool network_active{};
	// Multiplayer spectator state at DAT_0057e058 and its watched object
	// index at DAT_00588324. AI_ExplodeOrdinary_begin consults both before
	// installing a death camera for a remote player.
	std::uint16_t multiplayer_spectator_target{};
	bool multiplayer_spectator_active{};
	// Retail cinematic state at 0x00587cd4. Landing installs mode three;
	// camera mode 34 owns its exact clear.
	std::uint8_t cinematic_mode{};
	// Retail Device/Lmaps global at 0x005d5618. The object-light AI
	// callbacks are literal no-ops when this video option is disabled.
	bool light_maps_enabled{true};
	bool mission_event_overflow_reported{};
	bool atmosphere_overflow_reported{};
	bool exhaust_hazard_registry_valid{};
	bool player_in_exhaust{};
};

struct FlightServiceContext
{
	World& world;
	mission::Runtime& mission;
	const mission::DteFile& file;
	std::uint32_t simulation_tick{};
};

void world_reset(World& world, std::uint32_t random_seed);
void world_reserve_mission_slots(
	World& world,
	std::uint16_t count);
ObjectHandle world_create(
	World& world,
	std::uint16_t type,
	const glm::vec3& position,
	const glm::mat3& orientation,
	const assets::ShipStatsTable& stats,
	bool player,
	std::uint16_t mission_index = UINT16_MAX,
	std::uint8_t group = UINT8_MAX,
	std::uint8_t pilot = UINT8_MAX);
ObjectHandle world_create_at(
	World& world,
	std::uint16_t index,
	std::uint16_t type,
	const glm::vec3& position,
	const glm::mat3& orientation,
	const assets::ShipStatsTable& stats,
	bool player,
	std::uint16_t mission_index = UINT16_MAX,
	std::uint8_t group = UINT8_MAX,
	std::uint8_t pilot = UINT8_MAX);
ObjectHandle world_recreate(
	World& world,
	ObjectHandle existing,
	std::uint16_t type,
	const glm::vec3& position,
	const glm::mat3& orientation,
	const assets::ShipStatsTable& stats,
	bool player,
	std::uint16_t mission_index = UINT16_MAX,
	std::uint8_t group = UINT8_MAX,
	std::uint8_t pilot = UINT8_MAX);
bool world_mark_departed(World& world, ObjectHandle handle);
// GameObject_mark_destroyed, LANCER.EXE 0x00401f00. Component type-one
// destruction uses this synchronous non-visual death boundary.
bool world_mark_destroyed(
	World& world,
	mission::Runtime& mission,
	WorldObject& object);
bool world_destroy(World& world, ObjectHandle handle);
void world_release_planet_atmosphere(
	World& world,
	ObjectHandle owner);
void world_depart_planet_atmosphere(
	World& world,
	ObjectHandle owner);
bool world_type_has_planet_atmosphere(std::uint16_t type);
WorldObject* world_resolve(World& world, ObjectHandle handle);
const WorldObject* world_resolve(const World& world, ObjectHandle handle);
void world_step_object(
	WorldObject& object,
	const assets::ShipStatsTable& stats,
	const FlightDemand& demand,
	std::uint8_t camera_mode,
	const FlightServiceContext* service = nullptr);
void world_service_ordinary_motion(
	World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	std::uint32_t simulation_tick);
void world_service_collisions(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick);
void world_publish_scene_poses(
	World& world,
	std::uint8_t service_phase);
void world_set_scene_attachment(
	World& world,
	WorldObject& object,
	const WorldObject& parent,
	std::int16_t parent_model,
	const glm::vec3& local_position,
	const glm::mat3& local_orientation);
void world_clear_scene_attachment(WorldObject& object);
void world_zero_motion_controls(WorldObject& object);
void world_service_resources(
	World& world,
	const assets::ShipStatsTable& stats,
	const mission::Runtime& mission);
bool world_set_cloak_active(
	World& world,
	WorldObject& object,
	bool active,
	std::uint32_t simulation_tick);
bool world_force_cloak(
	World& world,
	WorldObject& object,
	std::uint32_t simulation_tick);
bool world_force_decloak(
	World& world,
	WorldObject& object,
	std::uint32_t simulation_tick);
void world_service_cloaks(
	World& world,
	std::uint32_t simulation_tick);
void world_register_cloak_hit(
	WorldObject& object,
	const glm::vec3& position,
	std::uint32_t simulation_tick);
void world_initialize_cloak_meshes(
	World& world,
	WorldObject& object);
void world_initialize_cloak_attachment(
	World& world,
	AttachmentSlot& attachment);
bool world_pop_sound(
	World& world,
	WorldSoundEvent& event);
void world_queue_sound_explicit(
	World& world,
	const glm::vec3& position,
	const glm::vec3& direction,
	const glm::vec3& velocity,
	std::uint8_t definition,
	std::uint8_t requested_class);
void world_queue_sound_object(
	World& world,
	ObjectHandle object,
	std::uint8_t definition,
	std::uint8_t requested_class);
void world_queue_sound_missile(
	World& world,
	std::uint16_t missile_index,
	std::uint32_t missile_serial,
	ObjectHandle owning_object,
	const glm::vec3& position,
	const glm::vec3& direction,
	const glm::vec3& velocity,
	std::uint8_t definition,
	std::uint8_t requested_class);
void world_queue_sound_model_frame(
	World& world,
	ObjectHandle object,
	std::int16_t model_reference,
	const glm::vec3& position,
	const glm::vec3& direction,
	const glm::vec3& velocity,
	std::uint8_t definition,
	std::uint8_t requested_class);
void world_update_shield_ratios(
	WorldObject& object,
	const assets::ShipStatsTable& stats);
void world_emit_mission_event(
	World& world,
	WorldMissionEventType type,
	const WorldObject& source,
	const WorldObject* attacker,
	std::uint8_t selector);
void world_service_component_destruction(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	std::uint32_t simulation_tick);
void world_spawn_capital_engine_breakaways(
	World& world,
	const assets::ShipStatsTable& stats,
	WorldObject& owner,
	std::uint32_t simulation_tick);
float world_effective_max_speed(
	const WorldObject& object,
	const assets::ShipStatsTable& stats,
	std::uint8_t camera_mode);
std::uint16_t world_object_rand15(WorldObject& object);
std::uint16_t world_rand15(World& world);
float world_object_rand_unit(WorldObject& object);
}
