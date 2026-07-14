#include "RuntimeSnapshot.h"

#include <mutex>

namespace RuntimeSnapshot {
namespace {

std::mutex g_mutex;
GameState g_gameState;
std::vector<ActorState> g_actors;
std::uint64_t g_actorGeneration = 0;
std::vector<ReferenceState> g_references;
std::uint64_t g_referenceGeneration = 0;
NavSceneState g_navScene;
QuestState g_quest;

} // namespace

void UpdateGameState(GameState state) {
    state.capturedAt = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_gameState = std::move(state);
}

GameState GetGameState() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_gameState;
}

bool TryGetFreshGameState(GameState& state, std::chrono::milliseconds maxAge) {
    state = GetGameState();
    if (!state.valid || state.capturedAt.time_since_epoch().count() == 0) {
        return false;
    }
    return std::chrono::steady_clock::now() - state.capturedAt <= maxAge;
}

void UpdateActors(std::vector<ActorState> actors, std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_actors = std::move(actors);
    g_actorGeneration = generation;
}

std::vector<ActorState> GetActors() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_actorGeneration != g_gameState.generation) {
        return {};
    }
    return g_actors;
}

bool TryGetActor(std::uint32_t formId, ActorState& actor) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (formId == 0 || g_actorGeneration != g_gameState.generation) {
        return false;
    }
    for (const ActorState& candidate : g_actors) {
        if (candidate.formId == formId) {
            actor = candidate;
            return true;
        }
    }
    return false;
}

bool IsActorInScene(const ActorState& actor, const GameState& gameState) {
    if (!gameState.valid || actor.formId == 0 || actor.deleted || actor.dead || !actor.loaded3D) {
        return false;
    }
    const bool sameCell = gameState.cellFormId != 0 && actor.cellFormId == gameState.cellFormId;
    const bool sameExteriorWorldspace = gameState.worldspaceFormId != 0 &&
        !actor.interior && actor.worldspaceFormId == gameState.worldspaceFormId;
    return sameCell || sameExteriorWorldspace;
}

void UpdateReferences(std::vector<ReferenceState> references, std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_references = std::move(references);
    g_referenceGeneration = generation;
}

std::vector<ReferenceState> GetReferences() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_referenceGeneration != g_gameState.generation) {
        return {};
    }
    return g_references;
}

bool TryGetReference(std::uint32_t formId, ReferenceState& reference) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (formId == 0 || g_referenceGeneration != g_gameState.generation) {
        return false;
    }
    for (const ReferenceState& candidate : g_references) {
        if (candidate.formId == formId) {
            reference = candidate;
            return true;
        }
    }
    return false;
}

void UpdateNavScene(NavSceneState scene) {
    scene.capturedAt = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_navScene = std::move(scene);
}

NavSceneState GetNavScene() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_navScene.valid || g_navScene.generation != g_gameState.generation) {
        return {};
    }
    return g_navScene;
}

void UpdateQuest(QuestState quest) {
    quest.capturedAt = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_quest = std::move(quest);
}

QuestState GetQuest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_quest.generation != g_gameState.generation) {
        return {};
    }
    return g_quest;
}

void RebaseGeneration(std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_gameState.valid) {
        g_gameState.generation = generation;
    }
    if (!g_actors.empty()) {
        g_actorGeneration = generation;
    }
    if (!g_references.empty()) {
        g_referenceGeneration = generation;
    }
    if (g_quest.valid) {
        g_quest.generation = generation;
    }
    if (g_navScene.valid) {
        g_navScene.generation = generation;
    }
}

void Clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_gameState = {};
    g_actors.clear();
    g_actorGeneration = 0;
    g_references.clear();
    g_referenceGeneration = 0;
    g_navScene = {};
    g_quest = {};
}

} // namespace RuntimeSnapshot
