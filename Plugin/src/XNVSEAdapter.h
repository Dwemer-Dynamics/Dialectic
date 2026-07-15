#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace XNVSEAdapter {

enum class LifecycleEvent {
    PostLoad,
    ExitGame,
    ExitToMainMenu,
    LoadGame,
    SaveGame,
    PreLoadGame,
    PostLoadGame,
    NewGame,
    DeferredInit,
    MainGameLoop,
    ReloadConfig,
    RefSet3D,
    RefUnset3D,
    RefAttached,
    CellStateChanged,
    CellRefsLoaded,
    FormLoaded,
    FormUnloaded
};

enum class NativeToolMenu {
    Mode,
    LlmModel,
    DynamicProfile
};

struct Message {
    LifecycleEvent event{LifecycleEvent::PostLoad};
    const void* data{nullptr};
    std::uint32_t dataLength{0};
    std::uint32_t formId{0};
    std::string text;
};

struct NativeGameState {
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
};

struct NativeEquipmentItem {
    std::string name;
    std::uint32_t baseFormId{0};
    std::uint8_t type{0};
    float condition{-1.0f};
};

struct NativeActorState {
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
    std::vector<NativeEquipmentItem> equipment;
};

struct NativeInventoryItem {
    std::string name;
    std::uint32_t baseFormId{0};
    std::uint8_t type{0};
    int count{0};
    bool equipped{false};
    float condition{-1.0f};
};

struct NativeReferenceState {
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

struct NativeNavVertex {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct NativeNavTriangle {
    std::int16_t vertices[3]{-1, -1, -1};
    std::int16_t neighbors[3]{-1, -1, -1};
    std::uint32_t flags{0};
    std::uint32_t doorFormId{0};
};

struct NativeNavMeshState {
    std::uint32_t formId{0};
    std::vector<NativeNavVertex> vertices;
    std::vector<NativeNavTriangle> triangles;
};

struct NativeNavSceneState {
    bool valid{false};
    bool interior{false};
    bool complete{false};
    std::uint32_t cellFormId{0};
    std::uint32_t worldspaceFormId{0};
    std::vector<NativeNavMeshState> meshes;
};

struct NativeQuestObjective {
    int objectiveId{0};
    std::string text;
};

struct NativeQuestState {
    bool valid{false};
    std::uint32_t formId{0};
    std::string name;
    std::string editorId;
    std::vector<NativeQuestObjective> objectives;
};

struct NativeCombatActorState {
    bool valid{false};
    bool player{false};
    bool playerTeammate{false};
    std::uint32_t formId{0};
    int aggression{0};
    int confidence{0};
    int assistance{0};
};

struct NativeTradeMenuInfo {
    bool valid{false};
    bool barter{false};
    std::uint32_t actorFormId{0};
    std::uint32_t inventoryOwnerFormId{0};
};

struct NativeLoadedPlugin {
    std::string name;
    std::uint32_t compileIndex{0};
};

struct NativeFaction {
    std::string name;
    std::uint32_t formId{0};
};

struct NativeMapMarker {
    std::string name;
    std::string worldspaceName;
    std::uint32_t formId{0};
    std::uint16_t flags{0};
    std::uint16_t markerType{0};
    bool interior{false};
    float x{0.0f};
    float y{0.0f};
};

struct NativeDialoguePrompt {
    std::string prompt;
    std::string targetName;
    std::string source;
    std::uint32_t targetFormId{0};
    std::uint32_t topicFormId{0};
    std::uint32_t parentTopicFormId{0};
    std::uint8_t formType{0};
};

struct NativePresentationDiagnostics {
    std::uint64_t faceGenAttempts{0};
    std::uint64_t faceGenApplied{0};
    std::uint64_t faceGenFailed{0};
    std::uint64_t subtitleAttempts{0};
    std::uint64_t subtitleApplied{0};
    std::uint64_t subtitleFailed{0};
    std::uint64_t dialogueGuardAttempts{0};
    std::uint64_t dialogueGuardApplied{0};
    std::uint64_t dialogueGuardFailed{0};
    std::uint64_t dialogueGuardRestores{0};
    std::uint64_t facingAttempts{0};
    std::uint64_t facingApplied{0};
    std::uint64_t facingFailed{0};
};

using MessageCallback = std::function<void(const Message&)>;
using PlayerInventoryChangeCallback = std::function<void(const char*)>;

bool Initialize(const void* nvseInterface, std::uint32_t pluginHandle, MessageCallback callback);
void Shutdown();
bool IsInitialized();
bool HasMessaging();
void SetPlayerInventoryChangeCallback(PlayerInventoryChangeCallback callback);
bool HasPlayerInventoryEventHooks();
std::uint32_t MessagingVersion();
std::string RuntimeDirectory();
bool CaptureNativeGameState(NativeGameState& state);
bool CaptureNativeActors(std::vector<NativeActorState>& actors, bool refreshEquipment = false);
bool CaptureNativeReferences(std::vector<NativeReferenceState>& references);
bool CaptureNativeNavScene(NativeNavSceneState& scene);
bool CaptureNativeQuest(NativeQuestState& quest);
bool CaptureNativeInventory(std::uint32_t ownerFormId, std::vector<NativeInventoryItem>& items);
bool CaptureNativeLoadedPlugins(std::vector<NativeLoadedPlugin>& plugins);
bool CaptureNativeFactions(std::vector<NativeFaction>& factions);
bool CaptureNativeMapMarkers(std::vector<NativeMapMarker>& markers);
void BeginNativeMapMarkerCapture();
bool AdvanceNativeMapMarkerCapture(std::vector<NativeMapMarker>& markers,
                                   bool& complete,
                                   std::uint32_t cellBudget = 32,
                                   std::uint32_t referenceBudget = 1500);
bool CaptureNativeDialoguePrompt(const void* speakerReference,
                                 const void* topicOrInfo,
                                 NativeDialoguePrompt& prompt);
bool UpdateNativeDialogueGuards(const std::vector<std::uint32_t>& actorFormIds);
void RestoreNativeDialogueGuards();
bool SetNativePassiveSubtitle(const std::string& text);
void ClearNativePassiveSubtitle();
void InvalidateNativePresentation();
void InvalidateNativeObjectCache();
bool ApplyNativeFacing(std::uint32_t speakerFormId, std::uint32_t targetFormId, float yawDegrees);
bool ClearNativeFacing(std::uint32_t speakerFormId);
bool OpenNativeToolMenu(NativeToolMenu menu);
bool ApplyNativeMfg(std::uint32_t actorFormId, int phoneme, int intensity, bool reset);
bool ApplyNativeFaceGenLipSync(std::uint32_t actorFormId,
                               int phoneme,
                               int intensity,
                               int decayIntensity,
                               bool reset);
bool ResetNativeLipSync(std::uint32_t actorFormId);
bool HaltNativeActor(std::uint32_t actorFormId);
bool ExecuteSimpleNativeAction(std::uint32_t actorFormId, int actionCode);
bool ExecuteNativePackageAction(std::uint32_t actorFormId, std::uint32_t targetFormId, int actionCode);
bool CaptureNativeCombatActorState(std::uint32_t actorFormId, NativeCombatActorState& state);
bool ExecuteNativeAttack(std::uint32_t speakerFormId, std::uint32_t targetFormId);
bool RestoreNativeCombatActorState(const NativeCombatActorState& state);
bool ExecuteNativeInventoryAction(std::uint32_t speakerFormId,
                                  std::uint32_t targetFormId,
                                  std::uint32_t itemBaseFormId,
                                  int amount,
                                  int actionCode);
bool TransferNativeWorldReferenceToActor(std::uint32_t actorFormId,
                                         std::uint32_t itemReferenceFormId);
bool OpenNativeTeammateContainer(std::uint32_t actorFormId);
bool ExecuteNativeStopFollowing(std::uint32_t actorFormId);
bool ResolveNativeTradeMenu(std::uint32_t actorFormId,
                            bool preferBarter,
                            NativeTradeMenuInfo& info);
bool OpenNativeTradeMenu(const NativeTradeMenuInfo& info);
bool QueryNativeLineOfSight(std::uint32_t sourceFormId,
                            std::uint32_t targetFormId,
                            bool& hasLineOfSight);
bool QueueNativeNotification(const std::string& message, std::uint32_t emotion);
NativePresentationDiagnostics GetNativePresentationDiagnostics();

} // namespace XNVSEAdapter
