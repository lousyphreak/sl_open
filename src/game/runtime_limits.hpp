#pragma once

#include <cstddef>
#include <cstdint>

namespace sl_open::game
{
constexpr std::uint32_t kSimulationHz = 100;
constexpr std::uint32_t kSimulationTickMilliseconds = 10;
constexpr std::uint32_t kServiceHz = 25;
constexpr std::uint32_t kServiceTickInterval =
	kSimulationHz / kServiceHz;
static_assert(kSimulationHz % kServiceHz == 0);

constexpr std::size_t kMaxGameObjects = 400;
constexpr std::size_t kMaxObjectComponents = 60;
constexpr std::size_t kMaxMissionObjects = 512;
constexpr std::size_t kMaxMissionGroups = 256;
constexpr std::size_t kMaxMissionGlobals = 256;
constexpr std::size_t kMaxMissionReferenceSpans = 896;
constexpr std::size_t kMaxMissionTriggers = 1024;
constexpr std::size_t kMaxMissionReferenceSets = 128;
constexpr std::size_t kMaxMissionReferenceLinks = 768;
constexpr std::size_t kMaxAiCommandsPerObject = 20;
constexpr std::size_t kMaxExecutorContexts = 32;
constexpr std::size_t kMaxExecutorTimers = 16;
constexpr std::size_t kExecutorStackCells = 32;
constexpr std::size_t kExecutorWatchPairs = 9;
constexpr std::size_t kMaxMissionEvents = 1000;
constexpr std::size_t kMissionEventArguments = 8;

constexpr std::size_t kMaxGunProjectiles = 200;
constexpr std::size_t kMaxProjectileCandidates = 20;
constexpr std::size_t kMaxMissiles = 200;
constexpr std::size_t kMaxMissileTrails = 200;
constexpr std::size_t kMaxShieldEffects = 50;
constexpr std::size_t kShieldEffectHits = 8;

constexpr std::size_t kMaxShockwaves = 30;
constexpr std::size_t kMaxElectricRays = 100;
constexpr std::size_t kMaxSparks = 256;
constexpr std::size_t kMaxSharedParticles = 1000;
// Launch's style is bound to the default 1,000-slot ParticleArray at
// LANCER.EXE 0x0058a94c.
constexpr std::size_t kMaxLaunchParticles = kMaxSharedParticles;
constexpr std::size_t kMaxMeshDebris = 500;
// explode.cpp owns a second, independent 500-entry pool for meshes cut from
// live render objects. It is not the graphics-quality-scaled fragment pool.
constexpr std::size_t kMaxExplodingMeshes = 500;
constexpr std::size_t kMaxRocks = 300;
constexpr std::size_t kMaxAnimatedExplosions = 30;
constexpr std::size_t kMaxExplosionControllers = 10;
constexpr std::size_t kAttachmentDefinitionCount = 180;
constexpr std::size_t kMaxDestructionLights = 15;
constexpr std::size_t kMaxLargeExplosionCandidates = 80;
}
