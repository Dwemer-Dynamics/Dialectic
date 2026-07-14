#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace RuntimeSnapshot {

struct EquipmentItem {
    std::string name;
    std::uint32_t baseFormId{0};
    std::uint8_t type{0};
    float condition{-1.0f};
};

struct GameState {
    std::string cellName;
    std::string worldspaceName;
    std::string playerName;
    bool valid{false};
    bool inGame{false};
    bool inMenu{false};
    bool paused{false};
    bool pipboyOpen{false};
    bool pauseMenuOpen{false};
    bool dialogueMenuOpen{false};
    bool barterMenuOpen{false};
    bool containerMenuOpen{false};
    bool loadingMenuOpen{false};
    bool inCombat{false};
    bool player3DLoaded{false};
    bool playerSneaking{false};
    std::uint32_t playerFormId{0};
    std::uint32_t cellFormId{0};
    std::uint32_t worldspaceFormId{0};
    std::uint32_t crosshairFormId{0};
    float playerX{0.0f};
    float playerY{0.0f};
    float playerZ{0.0f};
    float playerPitch{0.0f};
    float playerYaw{0.0f};
    std::uint64_t generation{0};
    std::uint64_t frameSequence{0};
    std::chrono::steady_clock::time_point capturedAt{};
};

struct ActorState {
    std::string name;
    std::string raceName;
    std::uint32_t formId{0};
    std::uint32_t baseFormId{0};
    std::uint32_t cellFormId{0};
    std::uint32_t worldspaceFormId{0};
    std::uint32_t raceFormId{0};
    std::uint32_t voiceFormId{0};
    std::uint32_t combatTargetFormId{0};
    std::uint32_t packageFormId{0};
    std::uint32_t equippedWeaponFormId{0};
    std::uint8_t referenceType{0};
    std::uint8_t baseType{0};
    bool creature{false};
    bool deleted{false};
    bool loaded3D{false};
    bool interior{false};
    bool inCombat{false};
    bool hostileToPlayer{false};
    bool playerTeammate{false};
    bool female{false};
    bool dead{false};
    bool weaponDrawn{false};
    bool moving{false};
    bool running{false};
    bool sneaking{false};
    bool facingStateKnown{false};
    bool seated{false};
    bool animationBusy{false};
    int sitSleepState{0};
    int animationAction{-1};
    int level{0};
    float health{0.0f};
    float healthMax{0.0f};
    float actionPoints{0.0f};
    float actionPointsMax{0.0f};
    float distanceToPlayer{0.0f};
    float scale{1.0f};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float yaw{0.0f};
    std::vector<EquipmentItem> equipment;
};

struct ReferenceState {
    std::string name;
    std::string destinationName;
    std::uint32_t formId{0};
    std::uint32_t baseFormId{0};
    std::uint32_t cellFormId{0};
    std::uint32_t destinationCellFormId{0};
    std::uint8_t baseType{0};
    bool deleted{false};
    bool taken{false};
    bool loaded3D{false};
    bool crosshair{false};
    bool teleportDoor{false};
    bool openStateKnown{false};
    bool locked{false};
    int openState{0};
    float distanceToPlayer{0.0f};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float yaw{0.0f};
};

struct NavVertex {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct NavTriangle {
    std::int16_t vertices[3]{-1, -1, -1};
    std::int16_t neighbors[3]{-1, -1, -1};
    std::uint32_t flags{0};
    std::uint32_t doorFormId{0};
};

struct NavMeshState {
    std::uint32_t formId{0};
    std::vector<NavVertex> vertices;
    std::vector<NavTriangle> triangles;
};

struct NavSceneState {
    bool valid{false};
    bool interior{false};
    bool complete{false};
    std::uint32_t cellFormId{0};
    std::uint32_t worldspaceFormId{0};
    std::uint64_t generation{0};
    std::vector<NavMeshState> meshes;
    std::chrono::steady_clock::time_point capturedAt{};
};

struct QuestObjective {
    int objectiveId{0};
    std::string text;
};

struct QuestState {
    bool valid{false};
    std::uint32_t formId{0};
    std::string name;
    std::string editorId;
    std::vector<QuestObjective> objectives;
    std::uint64_t generation{0};
    std::chrono::steady_clock::time_point capturedAt{};
};

void UpdateGameState(GameState state);
GameState GetGameState();
bool TryGetFreshGameState(GameState& state, std::chrono::milliseconds maxAge);
void UpdateActors(std::vector<ActorState> actors, std::uint64_t generation);
std::vector<ActorState> GetActors();
bool TryGetActor(std::uint32_t formId, ActorState& actor);
bool IsActorInScene(const ActorState& actor, const GameState& gameState);
void UpdateReferences(std::vector<ReferenceState> references, std::uint64_t generation);
std::vector<ReferenceState> GetReferences();
bool TryGetReference(std::uint32_t formId, ReferenceState& reference);
void UpdateNavScene(NavSceneState scene);
NavSceneState GetNavScene();
void UpdateQuest(QuestState quest);
QuestState GetQuest();
void RebaseGeneration(std::uint64_t generation);
void Clear();

} // namespace RuntimeSnapshot
