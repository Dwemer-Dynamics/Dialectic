#include "SpatialSnapshotManagerFNV.h"

#include "ActorPositionResolverFNV.h"
#include "RuntimeSnapshot.h"
#include "SpatialAwarenessFNV.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

namespace SpatialSnapshotManagerFNV {
namespace {

std::mutex g_mutex;
std::uint32_t g_cellFormId{0};
std::uint64_t g_generation{0};
std::vector<std::uint32_t> g_candidates;
std::size_t g_nextCandidate{0};
std::uint64_t g_evaluations{0};
float g_anchorX{0.0f};
float g_anchorY{0.0f};
float g_anchorZ{0.0f};
std::chrono::steady_clock::time_point g_lastRebuild;

bool PlayerMoved(const RuntimeSnapshot::GameState& game) {
    const float dx = game.playerX - g_anchorX;
    const float dy = game.playerY - g_anchorY;
    const float dz = game.playerZ - g_anchorZ;
    return dx * dx + dy * dy + dz * dz >= 128.0f * 128.0f;
}

void Rebuild(const RuntimeSnapshot::GameState& game,
             const std::vector<RuntimeSnapshot::ActorState>& actors,
             std::chrono::steady_clock::time_point now) {
    std::vector<const RuntimeSnapshot::ActorState*> sorted;
    sorted.reserve(actors.size());
    for (const auto& actor : actors) {
        if (actor.formId == 0 || actor.formId == game.playerFormId || actor.dead || actor.deleted ||
            !actor.loaded3D || actor.cellFormId != game.cellFormId || !std::isfinite(actor.distanceToPlayer)) continue;
        sorted.push_back(&actor);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto* first, const auto* second) {
        return first->distanceToPlayer < second->distanceToPlayer;
    });
    constexpr std::size_t kMaxCandidates = 32;
    if (sorted.size() > kMaxCandidates) sorted.resize(kMaxCandidates);

    g_candidates.clear();
    g_candidates.reserve(sorted.size());
    for (const auto* actor : sorted) g_candidates.push_back(actor->formId);
    g_cellFormId = game.cellFormId;
    g_generation = game.generation;
    g_nextCandidate = 0;
    g_anchorX = game.playerX;
    g_anchorY = game.playerY;
    g_anchorZ = game.playerZ;
    g_lastRebuild = now;
}

} // namespace

void Update(std::size_t maxEvaluations) {
    if (maxEvaluations == 0) return;
    const auto game = RuntimeSnapshot::GetGameState();
    if (!game.valid || !game.inGame || game.cellFormId == 0) return;
    const auto actors = RuntimeSnapshot::GetActors();
    const auto now = std::chrono::steady_clock::now();

    std::vector<std::uint32_t> work;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_generation != game.generation || g_cellFormId != game.cellFormId ||
            g_lastRebuild.time_since_epoch().count() == 0 || PlayerMoved(game) ||
            now - g_lastRebuild >= std::chrono::seconds(2)) {
            Rebuild(game, actors, now);
        }
        while (!g_candidates.empty() && work.size() < maxEvaluations) {
            if (g_nextCandidate >= g_candidates.size()) g_nextCandidate = 0;
            work.push_back(g_candidates[g_nextCandidate++]);
        }
    }

    const auto player = ActorPositionResolverFNV::ResolvePlayer();
    if (!player.resolved) return;
    std::uint64_t completed = 0;
    for (const std::uint32_t formId : work) {
        const auto actor = ActorPositionResolverFNV::ResolveActor(formId);
        if (!actor.resolved || !ActorPositionResolverFNV::IsPositionInPlayerScene(actor)) continue;
        SpatialAwarenessFNV::Evaluate(player, actor);
        ++completed;
    }
    if (completed > 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_evaluations += completed;
    }
}

void Invalidate() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cellFormId = 0;
    g_generation = 0;
    g_candidates.clear();
    g_nextCandidate = 0;
    g_anchorX = g_anchorY = g_anchorZ = 0.0f;
    g_lastRebuild = {};
}

Status GetStatus() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return {g_cellFormId, g_generation, g_candidates.size(), g_nextCandidate, g_evaluations};
}

} // namespace SpatialSnapshotManagerFNV
