#include "XNVSEAdapter.h"

#include "Logger.h"

#include "nvse/prefix.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameData.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/GameProcess.h"
#include "nvse/GameUI.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <atomic>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace XNVSEAdapter {
namespace {

constexpr std::uintptr_t kPlayerSingletonAddress = 0x011DEA3C;
constexpr std::uintptr_t kInterfaceManagerAddress = 0x011D8A80;
constexpr std::uintptr_t kMenuVisibilityAddress = 0x011F308F;
constexpr std::uintptr_t kDataHandlerSingletonAddress = 0x011C3F2C;
constexpr std::uintptr_t kQueueUiMessageAddress = 0x007052F0;

struct GuardedActorBase {
    TESActorBase* actorBase{nullptr};
    BGSVoiceType* originalVoice{nullptr};
    std::unordered_set<std::uint32_t> actorRefs;
};

std::unordered_map<TESActorBase*, GuardedActorBase> g_guardedActorBases;
std::unordered_map<std::uint32_t, TESActorBase*> g_guardedActorRefs;
BGSVoiceType* g_silentVoiceType = nullptr;
std::unordered_map<std::uint32_t, TESObjectREFR*> g_knownRuntimeReferences;
std::unordered_map<std::uint32_t, TESObjectREFR*> g_knownActorReferences;
std::unordered_map<std::uint32_t, std::vector<NativeEquipmentItem>> g_actorEquipmentCache;
Tile* g_passiveSubtitleTile = nullptr;
Tile* g_passiveSubtitleTextTile = nullptr;
Script* g_faceTargetFunction = nullptr;
Script* g_stopLookFunction = nullptr;
Script* g_dialecticControlMenuFunction = nullptr;
Script* g_modeMenuFunction = nullptr;
Script* g_llmModelMenuFunction = nullptr;
Script* g_dynamicProfileMenuFunction = nullptr;
Script* g_pipVisionToggleMenusFunction = nullptr;
Script* g_pipVisionCaptureFunction = nullptr;
Script* g_mfgPhonemeFunction = nullptr;
Script* g_mfgResetFunction = nullptr;
Script* g_haltActorFunction = nullptr;
Script* g_simpleActionFunction = nullptr;
Script* g_packageActionFunction = nullptr;
Script* g_cccManagedQueryFunction = nullptr;
Script* g_cccCompanionCommandFunction = nullptr;
Script* g_attackActionFunction = nullptr;
Script* g_restoreCombatActorFunction = nullptr;
Script* g_inventoryActionFunction = nullptr;
Script* g_addItemToActorFunction = nullptr;
Script* g_teleportActorFunction = nullptr;
Script* g_killActorFunction = nullptr;
Script* g_pickupTransferFunction = nullptr;
Script* g_openTeammateContainerFunction = nullptr;
Script* g_stopFollowingFunction = nullptr;
Script* g_queryMerchantContainerFunction = nullptr;
Script* g_queryOffersServicesFunction = nullptr;
Script* g_openBarterMenuFunction = nullptr;
Script* g_lineOfSightFunction = nullptr;
Script* g_doorStateFunction = nullptr;

struct CachedDoorState {
    int openState{0};
    bool locked{false};
    std::chrono::steady_clock::time_point capturedAt{};
};
std::unordered_map<std::uint32_t, CachedDoorState> g_doorStateCache;

struct SceneCellCache {
    std::uint32_t playerCellFormId{0};
    std::uint32_t worldspaceFormId{0};
    std::vector<TESObjectCELL*> cells;
};
SceneCellCache g_sceneCellCache;

struct MapMarkerCaptureState {
    DataHandler* dataHandler{nullptr};
    UInt32 nextCell{0};
    std::size_t nextReference{0};
    bool active{false};
    bool complete{false};
    std::vector<NativeMapMarker> markers;
    std::unordered_set<std::uint32_t> seen;
};
MapMarkerCaptureState g_mapMarkerCapture;
std::mutex g_mapMarkerMutex;

std::mutex g_callbackMutex;
MessageCallback g_callback;
PlayerInventoryChangeCallback g_playerInventoryChangeCallback;
const NVSEInterface* g_nvse = nullptr;
NVSEMessagingInterface* g_messaging = nullptr;
NVSEScriptInterface* g_scriptInterface = nullptr;
NVSEEventManagerInterface* g_eventManager = nullptr;
PluginHandle g_pluginHandle = kPluginHandle_Invalid;
std::atomic<bool> g_initialized{false};
std::array<bool, 6> g_playerInventoryEventHandlers{};

bool IsPlayerInventoryEventSource(void* parameters) {
    if (!parameters) return false;
    auto** arguments = static_cast<void**>(parameters);
    auto* source = arguments ? static_cast<TESObjectREFR*>(arguments[0]) : nullptr;
    __try {
        return source && source->refID == 0x00000014;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void NotifyPlayerInventoryChange(void* parameters, const char* reason) {
    if (!IsPlayerInventoryEventSource(parameters)) return;

    PlayerInventoryChangeCallback callback;
    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        callback = g_playerInventoryChangeCallback;
    }
    if (callback) callback(reason);
}

void OnPlayerInventoryAdded(TESObjectREFR*, void* parameters) {
    NotifyPlayerInventoryChange(parameters, "onadd");
}

void OnPlayerInventoryDropped(TESObjectREFR*, void* parameters) {
    NotifyPlayerInventoryChange(parameters, "ondrop");
}

void OnPlayerInventoryDropItem(TESObjectREFR*, void* parameters) {
    NotifyPlayerInventoryChange(parameters, "ondropitem");
}

void OnPlayerInventoryEquipped(TESObjectREFR*, void* parameters) {
    NotifyPlayerInventoryChange(parameters, "onequip");
}

void OnPlayerInventoryUnequipped(TESObjectREFR*, void* parameters) {
    NotifyPlayerInventoryChange(parameters, "onunequip");
}

void OnPlayerInventorySold(TESObjectREFR*, void* parameters) {
    NotifyPlayerInventoryChange(parameters, "onsell");
}

struct InventoryEventBinding {
    const char* name;
    NVSEEventManagerInterface::NativeEventHandler handler;
};

const std::array<InventoryEventBinding, 6> kPlayerInventoryEventBindings{{
    {"onadd", OnPlayerInventoryAdded},
    {"ondrop", OnPlayerInventoryDropped},
    {"ondropitem", OnPlayerInventoryDropItem},
    {"onactorequip", OnPlayerInventoryEquipped},
    {"onactorunequip", OnPlayerInventoryUnequipped},
    {"onsell", OnPlayerInventorySold},
}};

constexpr std::size_t kFaceGenPhonemeKeyFrameOffset = 0x4C;
constexpr std::size_t kFaceGenAlternateKeyFrameOffset = 0x74;
constexpr std::size_t kFaceGenKeyframeValuesOffset = 0x0C;
constexpr std::size_t kFaceGenKeyframeCountOffset = 0x10;
constexpr std::size_t kFaceNodeAnimationDataOffset = 0x0AC;
constexpr std::size_t kFaceNodeAnimationUpdateOffset = 0x0D6;
constexpr std::size_t kFaceNodeInDialogueOffset = 0x0E2;

std::size_t g_faceGenBankOffset = 0;
std::uint64_t g_faceGenApplyCount = 0;
std::uint64_t g_faceGenFailureCount = 0;
std::uint64_t g_faceGenRejectCount = 0;
std::uint32_t g_faceGenSessionActorFormId = 0;
TESObjectREFR* g_faceGenSessionActor = nullptr;
std::uintptr_t g_faceGenSessionNodeA = 0;
std::uintptr_t g_faceGenSessionNodeB = 0;
bool g_faceGenSessionArmed = false;
std::atomic<std::uint64_t> g_nativeFaceGenAttempts{0};
std::atomic<std::uint64_t> g_nativeFaceGenApplied{0};
std::atomic<std::uint64_t> g_nativeFaceGenFailed{0};
std::atomic<std::uint64_t> g_nativeSubtitleAttempts{0};
std::atomic<std::uint64_t> g_nativeSubtitleApplied{0};
std::atomic<std::uint64_t> g_nativeSubtitleFailed{0};
std::atomic<std::uint64_t> g_nativeDialogueGuardAttempts{0};
std::atomic<std::uint64_t> g_nativeDialogueGuardApplied{0};
std::atomic<std::uint64_t> g_nativeDialogueGuardFailed{0};
std::atomic<std::uint64_t> g_nativeDialogueGuardRestores{0};
std::atomic<std::uint64_t> g_nativeFacingAttempts{0};
std::atomic<std::uint64_t> g_nativeFacingApplied{0};
std::atomic<std::uint64_t> g_nativeFacingFailed{0};

template <typename T>
bool SafeRead(std::uintptr_t address, T& value) {
    if (address == 0 || IsBadReadPtr(reinterpret_cast<const void*>(address), sizeof(T))) {
        return false;
    }
    __try {
        value = *reinterpret_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeCopyReadable(void* destination, const void* source, std::size_t size) {
    if (!destination || !source || size == 0 || IsBadReadPtr(source, size)) {
        return false;
    }
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsReadableCell(const TESObjectCELL* cell) {
    return cell && !IsBadReadPtr(cell, sizeof(TESObjectCELL));
}

bool IsAttachedSceneCell(const TESObjectCELL* cell, const TESObjectCELL* playerCell) {
    if (!IsReadableCell(cell) || !IsReadableCell(playerCell)) {
        return false;
    }
    if (cell == playerCell) {
        return true;
    }
    if (playerCell->IsInterior() || cell->IsInterior()) {
        return false;
    }
    if (!playerCell->worldSpace || cell->worldSpace != playerCell->worldSpace) {
        return false;
    }
    return cell->cellState == TESObjectCELL::kCellLoadState_Attached && !cell->cellDetached;
}

const std::vector<TESObjectCELL*>& GetSceneCells(TESObjectCELL* playerCell) {
    static const std::vector<TESObjectCELL*> empty;
    if (!IsReadableCell(playerCell)) {
        return empty;
    }

    const std::uint32_t playerCellFormId = playerCell->refID;
    const std::uint32_t worldspaceFormId = playerCell->worldSpace ? playerCell->worldSpace->refID : 0;
    const bool cacheFresh =
        g_sceneCellCache.playerCellFormId == playerCellFormId &&
        g_sceneCellCache.worldspaceFormId == worldspaceFormId &&
        g_sceneCellCache.cells.size() == 1 &&
        g_sceneCellCache.cells.front() == playerCell;
    if (cacheFresh) {
        return g_sceneCellCache.cells;
    }

    SceneCellCache refreshed;
    refreshed.playerCellFormId = playerCellFormId;
    refreshed.worldspaceFormId = worldspaceFormId;
    refreshed.cells.reserve(1);
    refreshed.cells.push_back(playerCell);

    const bool changed = refreshed.cells.size() != g_sceneCellCache.cells.size() ||
        refreshed.playerCellFormId != g_sceneCellCache.playerCellFormId ||
        refreshed.worldspaceFormId != g_sceneCellCache.worldspaceFormId;
    g_sceneCellCache = std::move(refreshed);
    if (changed) {
        Logger::LogInfo("[NATIVE_SCENE] source=current_cell cells=%zu player_cell=0x%08X worldspace=0x%08X interior=%d",
            g_sceneCellCache.cells.size(), playerCellFormId, worldspaceFormId,
            playerCell->IsInterior() ? 1 : 0);
    }
    return g_sceneCellCache.cells;
}

template <typename T>
struct RawSimpleArray {
    void* vtable{nullptr};
    T* data{nullptr};
    std::uint32_t size{0};
    std::uint32_t capacity{0};
};

struct RawDoorPortal {
    TESObjectREFR* door{nullptr};
    std::uint16_t triangleIndex{0};
    std::uint16_t padding{0};
};

constexpr std::size_t kNavMeshVerticesOffset = 0x28;
constexpr std::size_t kNavMeshTrianglesOffset = 0x38;
constexpr std::size_t kNavMeshDoorPortalsOffset = 0x58;
constexpr std::size_t kMaxNativeNavMeshes = 64;
constexpr std::size_t kMaxNativeNavVertices = 100000;
constexpr std::size_t kMaxNativeNavTriangles = 50000;

bool IsWritableMemory(std::uintptr_t address, std::size_t size) {
    if (address == 0 || size == 0) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0 ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    constexpr DWORD writable =
        PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & writable) == 0) return false;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return address <= regionEnd && size <= regionEnd - address;
}

template <typename T>
bool SafeWrite(std::uintptr_t address, T value) {
    if (!IsWritableMemory(address, sizeof(T))) return false;
    __try {
        *reinterpret_cast<T*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float ClampFaceGenValue(float value) {
    if (!std::isfinite(value)) return 0.0f;
    return (std::max)(0.0f, (std::min)(1.0f, value));
}

bool ApplyFaceGenBank(std::uintptr_t animationData,
                      std::size_t bankOffset,
                      std::uint32_t actorFormId,
                      int phoneme,
                      int intensity,
                      int decayIntensity,
                      bool reset) {
    if (animationData == 0) return false;

    const std::uintptr_t keyframe = animationData + bankOffset;
    std::uintptr_t values = 0;
    std::uint32_t reportedCount = 0;
    if (!SafeRead(keyframe + kFaceGenKeyframeValuesOffset, values) ||
        !SafeRead(keyframe + kFaceGenKeyframeCountOffset, reportedCount)) {
        return false;
    }

    std::uint32_t slotCount = reportedCount;
    if ((bankOffset == kFaceGenPhonemeKeyFrameOffset ||
         bankOffset == kFaceGenAlternateKeyFrameOffset) && reportedCount == 15) {
        slotCount = 16;
    }
    if (slotCount != 16 || !IsWritableMemory(values, sizeof(float) * slotCount)) {
        ++g_faceGenRejectCount;
        if (g_faceGenRejectCount <= 8 || g_faceGenRejectCount % 40 == 0) {
            Logger::LogWarning(
                "[NATIVE_LIPSYNC] rejected bank actor=0x%08X data=%p bank=0x%zX values=%p count=%u",
                actorFormId,
                reinterpret_cast<void*>(animationData),
                bankOffset,
                reinterpret_cast<void*>(values),
                reportedCount);
        }
        return false;
    }

    std::array<float, 16> current{};
    __try {
        const auto* faceValues = reinterpret_cast<const float*>(values);
        for (std::uint32_t i = 0; i < slotCount; ++i) {
            current[i] = faceValues[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    for (std::uint32_t i = 0; i < slotCount; ++i) {
        if (!std::isfinite(current[i]) || std::abs(current[i]) > 20.0f) {
            return false;
        }
    }

    const bool active = !reset && phoneme >= 0 && phoneme < static_cast<int>(slotCount) && intensity > 0;
    const float activeValue = active ? ClampFaceGenValue(static_cast<float>(intensity) / 100.0f) : 0.0f;
    const float decayStep = reset
        ? 1.0f
        : ClampFaceGenValue(static_cast<float>((std::max)(1, decayIntensity)) / 100.0f);

    __try {
        auto* faceValues = reinterpret_cast<float*>(values);
        for (std::uint32_t i = 0; i < slotCount; ++i) {
            float next = 0.0f;
            if (!reset && active && static_cast<int>(i) == phoneme) {
                next = activeValue;
            } else if (!reset) {
                next = ClampFaceGenValue(current[i] - decayStep);
            }
            faceValues[i] = ClampFaceGenValue(next);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    ++g_faceGenApplyCount;
    if (reset || g_faceGenApplyCount <= 5 || g_faceGenApplyCount % 20 == 0) {
        Logger::LogInfo(
            "[NATIVE_LIPSYNC] FaceGen applied count=%llu actor=0x%08X bank=0x%zX phoneme=%d intensity=%d decay=%d reset=%d",
            static_cast<unsigned long long>(g_faceGenApplyCount),
            actorFormId,
            bankOffset,
            phoneme,
            intensity,
            decayIntensity,
            reset ? 1 : 0);
    }
    return true;
}

bool ApplyFaceGenCandidate(std::uintptr_t animationData,
                           std::uint32_t actorFormId,
                           int phoneme,
                           int intensity,
                           int decayIntensity,
                           bool reset) {
    if (animationData == 0) return false;
    if (reset) {
        bool resetAny = false;
        resetAny = ApplyFaceGenBank(animationData, kFaceGenPhonemeKeyFrameOffset,
                                    actorFormId, -1, 0, 100, true) || resetAny;
        resetAny = ApplyFaceGenBank(animationData, kFaceGenAlternateKeyFrameOffset,
                                    actorFormId, -1, 0, 100, true) || resetAny;
        return resetAny;
    }
    if (g_faceGenBankOffset != 0 &&
        ApplyFaceGenBank(animationData, g_faceGenBankOffset, actorFormId,
                         phoneme, intensity, decayIntensity, false)) {
        return true;
    }
    if (ApplyFaceGenBank(animationData, kFaceGenPhonemeKeyFrameOffset, actorFormId,
                         phoneme, intensity, decayIntensity, false)) {
        g_faceGenBankOffset = kFaceGenPhonemeKeyFrameOffset;
        return true;
    }
    return false;
}

void TouchFaceNode(std::uintptr_t nodeAddress, bool reset) {
    if (nodeAddress == 0) return;
    SafeWrite<std::uint8_t>(nodeAddress + kFaceNodeAnimationUpdateOffset, 1);
    SafeWrite<std::uint8_t>(nodeAddress + kFaceNodeInDialogueOffset, reset ? 0 : 1);
}

void ClearFaceGenSession() {
    g_faceGenSessionActorFormId = 0;
    g_faceGenSessionActor = nullptr;
    g_faceGenSessionNodeA = 0;
    g_faceGenSessionNodeB = 0;
    g_faceGenSessionArmed = false;
}

bool IsMenuVisible(std::uint32_t menuType) {
    if (menuType < kMenuType_Min || menuType > kMenuType_Max) {
        return false;
    }
    __try {
        const auto* visibility = reinterpret_cast<const UInt8*>(kMenuVisibilityAddress);
        return visibility[menuType] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MapMessageType(UInt32 type, LifecycleEvent& event) {
    switch (type) {
        case NVSEMessagingInterface::kMessage_PostLoad: event = LifecycleEvent::PostLoad; return true;
        case NVSEMessagingInterface::kMessage_ExitGame:
        case NVSEMessagingInterface::kMessage_ExitGame_Console: event = LifecycleEvent::ExitGame; return true;
        case NVSEMessagingInterface::kMessage_ExitToMainMenu: event = LifecycleEvent::ExitToMainMenu; return true;
        case NVSEMessagingInterface::kMessage_LoadGame: event = LifecycleEvent::LoadGame; return true;
        case NVSEMessagingInterface::kMessage_SaveGame: event = LifecycleEvent::SaveGame; return true;
        case NVSEMessagingInterface::kMessage_PreLoadGame: event = LifecycleEvent::PreLoadGame; return true;
        case NVSEMessagingInterface::kMessage_PostLoadGame: event = LifecycleEvent::PostLoadGame; return true;
        case NVSEMessagingInterface::kMessage_NewGame: event = LifecycleEvent::NewGame; return true;
        case NVSEMessagingInterface::kMessage_DeferredInit: event = LifecycleEvent::DeferredInit; return true;
        case NVSEMessagingInterface::kMessage_MainGameLoop: event = LifecycleEvent::MainGameLoop; return true;
        case NVSEMessagingInterface::kMessage_ReloadConfig: event = LifecycleEvent::ReloadConfig; return true;
        case NVSEMessagingInterface::kMessage_OnRefSet3D: event = LifecycleEvent::RefSet3D; return true;
        case NVSEMessagingInterface::kMessage_OnRefUnset3D: event = LifecycleEvent::RefUnset3D; return true;
        case NVSEMessagingInterface::kMessage_OnRefAttach: event = LifecycleEvent::RefAttached; return true;
        case NVSEMessagingInterface::kMessage_OnCellStateChange: event = LifecycleEvent::CellStateChanged; return true;
        case NVSEMessagingInterface::kMessage_OnCellRefsLoaded: event = LifecycleEvent::CellRefsLoaded; return true;
        case NVSEMessagingInterface::kMessage_OnNonPersistentFormLoad: event = LifecycleEvent::FormLoaded; return true;
        case NVSEMessagingInterface::kMessage_OnNonPersistentFormUnload: event = LifecycleEvent::FormUnloaded; return true;
        default: return false;
    }
}

std::string CopyGameString(const String& value) {
    if (!value.m_data || value.m_dataLen == 0) {
        return {};
    }
    const std::size_t length = std::min<std::size_t>(value.m_dataLen, 1024);
    return std::string(value.m_data, strnlen_s(value.m_data, length));
}

std::string CopyGameCString(const char* value) {
    if (!value) {
        return {};
    }
    return std::string(value, strnlen_s(value, 1024));
}

std::string CopyFormName(TESForm* form) {
    if (!form) return {};
    using DynamicCast = void* (*)(void*, UInt32, const void*, const void*, UInt32);
    constexpr std::uintptr_t kDynamicCastAddress = 0x00EC43FB;
    constexpr std::uintptr_t kRttiTesForm = 0x01183028;
    constexpr std::uintptr_t kRttiTesFullName = 0x01183158;
    auto dynamicCast = reinterpret_cast<DynamicCast>(kDynamicCastAddress);
    auto* fullName = static_cast<TESFullName*>(dynamicCast(
        form, 0, reinterpret_cast<const void*>(kRttiTesForm),
        reinterpret_cast<const void*>(kRttiTesFullName), 0));
    return fullName ? CopyGameString(fullName->name) : std::string{};
}

// Match xNVSE GetValue semantics, including ingestibles that store value directly.
int ResolveBaseItemValue(TESForm* form) {
    if (!form) return 0;
    if (form->typeID == kFormType_AlchemyItem) {
        return static_cast<int>(static_cast<AlchemyItem*>(form)->value);
    }

    using DynamicCast = void* (*)(void*, UInt32, const void*, const void*, UInt32);
    constexpr std::uintptr_t kDynamicCastAddress = 0x00EC43FB;
    constexpr std::uintptr_t kDynamicCastNoGoreAddress = 0x00EC438B;
    constexpr std::uintptr_t kRttiTesForm = 0x01183028;
    constexpr std::uintptr_t kRttiTesValueForm = 0x01186B6C;
    const auto dynamicCastAddress = (g_nvse && g_nvse->isNogore)
        ? kDynamicCastNoGoreAddress
        : kDynamicCastAddress;
    auto dynamicCast = reinterpret_cast<DynamicCast>(dynamicCastAddress);
    auto* valueForm = static_cast<TESValueForm*>(dynamicCast(
        form, 0, reinterpret_cast<const void*>(kRttiTesForm),
        reinterpret_cast<const void*>(kRttiTesValueForm), 0));
    return valueForm ? static_cast<int>(valueForm->value) : 0;
}

TESObjectREFR* FindLoadedReference(PlayerCharacter* player, std::uint32_t formId) {
    if (!player || formId == 0) return nullptr;
    if (player->refID == formId) return player;
    if (!player->parentCell) return nullptr;
    std::size_t visited = 0;
    for (auto iterator = player->parentCell->objectList.Begin();
         !iterator.End() && visited < 4096; ++iterator, ++visited) {
        TESObjectREFR* reference = iterator.Get();
        if (reference && reference->refID == formId) return reference;
    }
    return nullptr;
}

TESObjectREFR* FindKnownReference(PlayerCharacter* player, std::uint32_t formId) {
    if (!player || formId == 0) return nullptr;
    if (player->refID == formId) return player;

    const auto resolveCached = [formId](auto& cache) -> TESObjectREFR* {
        const auto it = cache.find(formId);
        if (it == cache.end()) return nullptr;
        __try {
            if (it->second && it->second->refID == formId) return it->second;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        cache.erase(it);
        return nullptr;
    };
    if (TESObjectREFR* actor = resolveCached(g_knownActorReferences)) return actor;
    if (TESObjectREFR* runtime = resolveCached(g_knownRuntimeReferences)) return runtime;

    auto* interfaceManager = *reinterpret_cast<InterfaceManager**>(kInterfaceManagerAddress);
    if (interfaceManager && interfaceManager->crosshairRef &&
        interfaceManager->crosshairRef->refID == formId) {
        return interfaceManager->crosshairRef;
    }

    // Full cell traversal is the expensive fallback. Active speakers are
    // normally present in the native actor cache after the first frame.
    return FindLoadedReference(player, formId);
}

TESObjectREFR* ResolveFaceGenSessionActor(PlayerCharacter* player, std::uint32_t actorFormId) {
    if (g_faceGenSessionActorFormId == actorFormId && g_faceGenSessionActor) {
        __try {
            if (g_faceGenSessionActor->refID == actorFormId) {
                return g_faceGenSessionActor;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        ClearFaceGenSession();
    }

    TESObjectREFR* reference = FindKnownReference(player, actorFormId);
    if (!reference) {
        return nullptr;
    }
    __try {
        if (reference->refID != actorFormId) {
            return nullptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }

    g_faceGenSessionActorFormId = actorFormId;
    g_faceGenSessionActor = reference;
    g_faceGenSessionNodeA = 0;
    g_faceGenSessionNodeB = 0;
    g_faceGenSessionArmed = false;
    return reference;
}

BGSVoiceType* FindSilentVoiceType() {
    if (g_silentVoiceType && g_silentVoiceType->typeID == kFormType_BGSVoiceType) {
        return g_silentVoiceType;
    }

    auto** singleton = reinterpret_cast<DataHandler**>(kDataHandlerSingletonAddress);
    DataHandler* dataHandler = singleton ? *singleton : nullptr;
    if (!dataHandler) {
        return nullptr;
    }

    for (auto iterator = dataHandler->voiceTypeList.Begin(); !iterator.End(); ++iterator) {
        BGSVoiceType* voice = iterator.Get();
        if (!voice) {
            continue;
        }
        const char* editorId = voice->GetName();
        if (editorId && _stricmp(editorId, "NVDLC01FemaleUnqueNoDialogue") == 0) {
            g_silentVoiceType = voice;
            return voice;
        }
    }
    return nullptr;
}

void RemoveDialogueGuard(std::uint32_t actorFormId) {
    const auto actorIt = g_guardedActorRefs.find(actorFormId);
    if (actorIt == g_guardedActorRefs.end()) {
        return;
    }

    TESActorBase* actorBase = actorIt->second;
    g_guardedActorRefs.erase(actorIt);
    const auto baseIt = g_guardedActorBases.find(actorBase);
    if (baseIt == g_guardedActorBases.end()) {
        return;
    }

    baseIt->second.actorRefs.erase(actorFormId);
    if (!baseIt->second.actorRefs.empty()) {
        return;
    }

    if (actorBase) {
        actorBase->baseData.voiceType = baseIt->second.originalVoice;
    }
    g_guardedActorBases.erase(baseIt);
}

BGSVoiceType* ResolveActorVoice(TESActorBase* actorBase) {
    if (!actorBase) {
        return nullptr;
    }
    const auto guarded = g_guardedActorBases.find(actorBase);
    if (guarded != g_guardedActorBases.end()) {
        return guarded->second.originalVoice;
    }
    return actorBase->baseData.GetVoiceType();
}

Tile* FindTileByName(Tile* root, const char* name, std::size_t depth = 0) {
    if (!root || !name || depth > 32) {
        return nullptr;
    }
    const std::string tileName = CopyGameString(root->name);
    if (!tileName.empty() && _stricmp(tileName.c_str(), name) == 0) {
        return root;
    }
    for (auto iterator = root->childList.Begin(); !iterator.End(); ++iterator) {
        Tile::ChildNode* childNode = iterator.Get();
        if (!childNode || !childNode->child) {
            continue;
        }
        if (Tile* match = FindTileByName(childNode->child, name, depth + 1)) {
            return match;
        }
    }
    return nullptr;
}

bool ResolvePassiveSubtitleTiles() {
    auto* interfaceManager = *reinterpret_cast<InterfaceManager**>(kInterfaceManagerAddress);
    if (!interfaceManager || !interfaceManager->menuRoot) {
        return false;
    }
    if (!g_passiveSubtitleTile) {
        g_passiveSubtitleTile = FindTileByName(interfaceManager->menuRoot, "DialecticPassiveSubtitle");
    }
    if (g_passiveSubtitleTile && !g_passiveSubtitleTextTile) {
        g_passiveSubtitleTextTile = FindTileByName(g_passiveSubtitleTile, "Text");
    }
    return g_passiveSubtitleTile && g_passiveSubtitleTextTile;
}

TESContainer* ResolveContainer(TESObjectREFR* reference) {
    if (!reference || !reference->baseForm) return nullptr;
    if (reference->baseForm->typeID == kFormType_TESNPC ||
        reference->baseForm->typeID == kFormType_TESCreature) {
        return &static_cast<TESActorBase*>(reference->baseForm)->container;
    }
    if (reference->baseForm->typeID == kFormType_TESObjectCONT) {
        return &static_cast<TESObjectCONT*>(reference->baseForm)->container;
    }
    return nullptr;
}

bool IsPlayerTeammate(PlayerCharacter* player, Actor* actor) {
    if (!player || !actor) return false;
    for (auto iterator = player->teammates.Begin(); !iterator.End(); ++iterator) {
        if (iterator.Get() == actor) return true;
    }
    return false;
}

void CaptureEquippedItems(Actor* actor, std::vector<NativeEquipmentItem>& equipment) {
    equipment.clear();
    if (!actor) {
        return;
    }

    auto* changes = static_cast<ExtraContainerChanges*>(
        actor->extraDataList.GetByType(kExtraData_ContainerChanges));
    ExtraContainerChanges::EntryDataList* entries = changes ? changes->GetEntryDataList() : nullptr;
    if (!entries) {
        return;
    }

    std::unordered_set<std::uint32_t> seen;
    for (auto iterator = entries->Begin(); !iterator.End(); ++iterator) {
        ExtraContainerChanges::EntryData* entry = iterator.Get();
        if (!entry || !entry->type || !entry->extendData) {
            continue;
        }

        bool equipped = false;
        float condition = -1.0f;
        for (auto extra = entry->extendData->Begin(); !extra.End(); ++extra) {
            ExtraDataList* list = extra.Get();
            if (!list) {
                continue;
            }
            if (list->GetByType(kExtraData_Worn) || list->GetByType(kExtraData_WornLeft)) {
                equipped = true;
                auto* health = static_cast<ExtraHealth*>(list->GetByType(kExtraData_Health));
                if (health) {
                    condition = health->health;
                }
                break;
            }
        }
        if (!equipped || !seen.insert(entry->type->refID).second) {
            continue;
        }

        NativeEquipmentItem item;
        item.name = CopyFormName(entry->type);
        item.baseFormId = entry->type->refID;
        item.type = entry->type->typeID;
        item.condition = condition;
        equipment.push_back(std::move(item));
    }
}

bool QueryDoorState(TESObjectREFR* door, CachedDoorState& state) {
    if (!door || !g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunction) {
        return false;
    }
    if (!g_doorStateFunction) {
        g_doorStateFunction = g_scriptInterface->CompileScript(R"(
int iOpenState
int iLocked
begin function {}
    let iOpenState := GetOpenState
    let iLocked := GetLocked
    SetFunctionValue (iOpenState * 10) + iLocked
end
)");
        if (!g_doorStateFunction) {
            Logger::LogError("[NATIVE_SPATIAL] failed to compile door state function");
            return false;
        }
    }
    alignas(NVSEArrayVarInterface::Element)
        unsigned char resultStorage[sizeof(NVSEArrayVarInterface::Element)]{};
    auto* result = reinterpret_cast<NVSEArrayVarInterface::Element*>(resultStorage);
    if (!g_scriptInterface->CallFunction(g_doorStateFunction, door, nullptr, result, 0)) {
        return false;
    }
    const int encoded = static_cast<int>(std::lround(result->GetNumber()));
    state.openState = encoded / 10;
    state.locked = (encoded % 10) != 0;
    state.capturedAt = std::chrono::steady_clock::now();
    return true;
}

bool TryExtractMessageFormId(LifecycleEvent event,
                             const void* data,
                             std::uint32_t dataLength,
                             std::uint32_t& formId) {
    formId = 0;
    __try {
        const void* formPointer = nullptr;
        switch (event) {
            case LifecycleEvent::RefSet3D:
            case LifecycleEvent::CellStateChanged:
                if (data && dataLength >= sizeof(void*)) {
                    formPointer = *static_cast<void* const*>(data);
                }
                break;
            case LifecycleEvent::RefUnset3D:
            case LifecycleEvent::RefAttached:
            case LifecycleEvent::CellRefsLoaded:
            case LifecycleEvent::FormLoaded:
            case LifecycleEvent::FormUnloaded:
                formPointer = data;
                break;
            default:
                break;
        }
        if (formPointer) formId = static_cast<const TESForm*>(formPointer)->refID;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        formId = 0;
        return false;
    }
}

struct RawDialoguePrompt {
    const String* prompt{nullptr};
    const String* fallbackPrompt{nullptr};
    TESForm* targetBase{nullptr};
    std::uint32_t targetFormId{0};
    std::uint32_t topicFormId{0};
    std::uint32_t parentTopicFormId{0};
    std::uint8_t formType{0};
};

bool TryCaptureRawDialoguePrompt(const void* speakerReference,
                                 const void* topicOrInfo,
                                 RawDialoguePrompt& raw) {
    raw = {};
    if (!topicOrInfo) return false;
    __try {
        const TESForm* topicForm = static_cast<const TESForm*>(topicOrInfo);
        raw.formType = topicForm->typeID;
        raw.topicFormId = topicForm->refID;
        if (topicForm->typeID == kFormType_TESTopicInfo) {
            const auto* topicInfo = static_cast<const TESTopicInfo*>(topicForm);
            raw.prompt = &topicInfo->prompt;

            // The runtime TESTopicInfo layout carries its parent topic at 0x50.
            // Many FNV/TTW choices leave INFO.prompt empty and display this topic name.
            const auto* parentTopic = *reinterpret_cast<TESTopic* const*>(
                reinterpret_cast<std::uintptr_t>(topicInfo) + 0x50);
            if (parentTopic && parentTopic->typeID == kFormType_TESTopic) {
                raw.parentTopicFormId = parentTopic->refID;
                raw.fallbackPrompt = &parentTopic->fullName.name;
            }
        } else if (topicForm->typeID == kFormType_TESTopic) {
            const auto* topic = static_cast<const TESTopic*>(topicForm);
            raw.prompt = &topic->fullName.name;
        }
        const auto* reference = static_cast<const TESObjectREFR*>(speakerReference);
        if (reference) {
            raw.targetFormId = reference->refID;
            raw.targetBase = reference->baseForm;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        raw = {};
        return false;
    }
}

void OnNVSEMessage(NVSEMessagingInterface::Message* source) {
    if (!source) {
        return;
    }
    LifecycleEvent event;
    if (!MapMessageType(source->type, event)) {
        return;
    }

    Message message;
    message.event = event;
    message.data = source->data;
    message.dataLength = source->dataLen;
    if (event == LifecycleEvent::PostLoadGame) {
        // xNVSE encodes the load result directly in the data pointer value.
        message.flag = source->data != nullptr;
    }
    TryExtractMessageFormId(event, source->data, source->dataLen, message.formId);
    if (source->data && source->dataLen > 0 &&
        (event == LifecycleEvent::LoadGame || event == LifecycleEvent::SaveGame ||
         event == LifecycleEvent::PreLoadGame || event == LifecycleEvent::ReloadConfig)) {
        const char* text = static_cast<const char*>(source->data);
        const std::size_t maxLength = static_cast<std::size_t>(source->dataLen);
        message.text.assign(text, strnlen_s(text, maxLength));
    }

    MessageCallback callback;
    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        callback = g_callback;
    }
    if (callback) {
        callback(message);
    }
}

} // namespace

bool Initialize(const void* nvseInterface, std::uint32_t pluginHandle, MessageCallback callback) {
    if (!nvseInterface) {
        return false;
    }
    g_nvse = static_cast<const NVSEInterface*>(nvseInterface);
    g_pluginHandle = static_cast<PluginHandle>(pluginHandle);
    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        g_callback = std::move(callback);
    }

    g_messaging = static_cast<NVSEMessagingInterface*>(g_nvse->QueryInterface(kInterface_Messaging));
    g_scriptInterface = static_cast<NVSEScriptInterface*>(g_nvse->QueryInterface(kInterface_Script));
    g_eventManager = static_cast<NVSEEventManagerInterface*>(g_nvse->QueryInterface(kInterface_EventManager));
    if (!g_messaging || !g_messaging->RegisterListener) {
        Logger::LogWarning("XNVSEAdapter: messaging interface unavailable");
        return false;
    }
    if (!g_messaging->RegisterListener(g_pluginHandle, "NVSE", OnNVSEMessage)) {
        Logger::LogError("XNVSEAdapter: failed to register NVSE lifecycle listener");
        g_messaging = nullptr;
        return false;
    }
    std::size_t inventoryHandlerCount = 0;
    if (g_eventManager && g_eventManager->SetNativeEventHandler) {
        for (std::size_t index = 0; index < kPlayerInventoryEventBindings.size(); ++index) {
            const auto& binding = kPlayerInventoryEventBindings[index];
            g_playerInventoryEventHandlers[index] =
                g_eventManager->SetNativeEventHandler(binding.name, binding.handler);
            if (g_playerInventoryEventHandlers[index]) {
                ++inventoryHandlerCount;
            } else {
                Logger::LogWarning(
                    "XNVSEAdapter: failed to register player inventory event handler %s",
                    binding.name);
            }
        }
    } else {
        Logger::LogWarning(
            "XNVSEAdapter: event manager unavailable; player inventory will use explicit triggers and reconciliation");
    }
    g_initialized.store(true, std::memory_order_release);
    Logger::LogInfo("XNVSEAdapter: initialized messaging_version=%u script_interface=%d inventory_events=%zu runtime_dir=%s",
        g_messaging->version,
        g_scriptInterface ? 1 : 0,
        inventoryHandlerCount,
        RuntimeDirectory().c_str());
    return true;
}

void Shutdown() {
    g_initialized.store(false, std::memory_order_release);
    if (g_eventManager && g_eventManager->RemoveNativeEventHandler) {
        for (std::size_t index = 0; index < kPlayerInventoryEventBindings.size(); ++index) {
            if (!g_playerInventoryEventHandlers[index]) continue;
            const auto& binding = kPlayerInventoryEventBindings[index];
            g_eventManager->RemoveNativeEventHandler(binding.name, binding.handler);
            g_playerInventoryEventHandlers[index] = false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        g_callback = {};
        g_playerInventoryChangeCallback = {};
    }
    g_messaging = nullptr;
    g_scriptInterface = nullptr;
    g_eventManager = nullptr;
    g_faceTargetFunction = nullptr;
    g_stopLookFunction = nullptr;
    g_dialecticControlMenuFunction = nullptr;
    g_modeMenuFunction = nullptr;
    g_llmModelMenuFunction = nullptr;
    g_dynamicProfileMenuFunction = nullptr;
    g_pipVisionToggleMenusFunction = nullptr;
    g_pipVisionCaptureFunction = nullptr;
    g_mfgPhonemeFunction = nullptr;
    g_mfgResetFunction = nullptr;
    g_haltActorFunction = nullptr;
    g_simpleActionFunction = nullptr;
    g_packageActionFunction = nullptr;
    g_cccManagedQueryFunction = nullptr;
    g_cccCompanionCommandFunction = nullptr;
    g_attackActionFunction = nullptr;
    g_restoreCombatActorFunction = nullptr;
    g_inventoryActionFunction = nullptr;
    g_addItemToActorFunction = nullptr;
    g_teleportActorFunction = nullptr;
    g_killActorFunction = nullptr;
    g_pickupTransferFunction = nullptr;
    g_openTeammateContainerFunction = nullptr;
    g_stopFollowingFunction = nullptr;
    g_queryMerchantContainerFunction = nullptr;
    g_queryOffersServicesFunction = nullptr;
    g_openBarterMenuFunction = nullptr;
    g_lineOfSightFunction = nullptr;
    g_doorStateFunction = nullptr;
    g_knownRuntimeReferences.clear();
    g_knownActorReferences.clear();
    g_actorEquipmentCache.clear();
    g_doorStateCache.clear();
    g_sceneCellCache = {};
    {
        std::lock_guard<std::mutex> mapLock(g_mapMarkerMutex);
        g_mapMarkerCapture = {};
    }
    g_nvse = nullptr;
    g_pluginHandle = kPluginHandle_Invalid;
}

void SetPlayerInventoryChangeCallback(PlayerInventoryChangeCallback callback) {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_playerInventoryChangeCallback = std::move(callback);
}

bool HasPlayerInventoryEventHooks() {
    return std::any_of(g_playerInventoryEventHandlers.begin(), g_playerInventoryEventHandlers.end(),
        [](bool registered) { return registered; });
}

bool IsInitialized() {
    return g_initialized.load(std::memory_order_acquire);
}

bool HasMessaging() {
    return g_messaging != nullptr;
}

std::uint32_t MessagingVersion() {
    return g_messaging ? g_messaging->version : 0;
}

std::string RuntimeDirectory() {
    if (!g_nvse || !g_nvse->GetRuntimeDirectory) {
        return {};
    }
    const char* path = g_nvse->GetRuntimeDirectory();
    return path ? path : "";
}

bool CaptureNativeGameState(NativeGameState& state) {
    state = {};
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    auto* interfaceManager = *reinterpret_cast<InterfaceManager**>(kInterfaceManagerAddress);
    if (!player || !interfaceManager) {
        return false;
    }

        state.playerFormId = player->refID;
        state.inGame = state.playerFormId != 0;
        if (player->baseForm && player->baseForm->typeID == kFormType_TESNPC) {
            state.playerName = CopyGameString(static_cast<TESNPC*>(player->baseForm)->fullName.name);
        }
        state.playerX = player->posX;
        state.playerY = player->posY;
        state.playerZ = player->posZ;
        state.playerPitch = player->rotX;
        state.playerYaw = player->rotZ;
        state.player3DLoaded = player->renderState && player->renderState->niNode;
        state.inCombat = player->IsInCombat();
        if (player->actorMover) {
            state.playerSneaking = (player->actorMover->Unk_08() & (1u << 9)) != 0;
        }
        state.crosshairFormId = interfaceManager->crosshairRef ? interfaceManager->crosshairRef->refID : 0;

        if (player->parentCell) {
            state.cellFormId = player->parentCell->refID;
            state.cellName = CopyGameString(player->parentCell->fullName.name);
            if (player->parentCell->worldSpace) {
                state.worldspaceFormId = player->parentCell->worldSpace->refID;
                state.worldspaceName = CopyGameString(player->parentCell->worldSpace->fullName.name);
            }
        }

        state.pauseMenuOpen = IsMenuVisible(kMenuType_Start);
        state.dialogueMenuOpen = IsMenuVisible(kMenuType_Dialog);
        state.barterMenuOpen = IsMenuVisible(kMenuType_Barter);
        state.containerMenuOpen = IsMenuVisible(kMenuType_Container);
        state.loadingMenuOpen = IsMenuVisible(kMenuType_Loading);
        state.pipboyOpen = IsMenuVisible(kMenuType_Inventory) ||
            IsMenuVisible(kMenuType_Stats) ||
            IsMenuVisible(kMenuType_Map);
        const bool explicitMenu = state.pauseMenuOpen || state.dialogueMenuOpen ||
            state.barterMenuOpen || state.containerMenuOpen || state.loadingMenuOpen ||
            state.pipboyOpen;
        const bool interfaceMenuMode = (interfaceManager->flags & 0x2) == 0;
        state.inMenu = explicitMenu || interfaceMenuMode;
        state.paused = state.pauseMenuOpen || state.pipboyOpen || state.barterMenuOpen ||
            state.containerMenuOpen || state.loadingMenuOpen;
        state.valid = true;
    return true;
}

bool CaptureNativeActorInspection(std::uint32_t actorFormId, NativeActorInspection& inspection) {
    inspection = {};
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* reference = FindKnownReference(player, actorFormId);
    if (!reference || !reference->baseForm) {
        return false;
    }

    const bool validCharacter = reference->typeID == kFormType_Character &&
        reference->baseForm->typeID == kFormType_TESNPC;
    const bool validCreature = reference->typeID == kFormType_Creature &&
        reference->baseForm->typeID == kFormType_TESCreature;
    if (!validCharacter && !validCreature) {
        return false;
    }

    auto* actor = static_cast<Actor*>(reference);
    auto* actorBase = static_cast<TESActorBase*>(reference->baseForm);
    inspection.formId = reference->refID;
    inspection.name = CopyGameString(actorBase->fullName.name);
    if (reference->baseForm->typeID == kFormType_TESNPC) {
        auto* npc = static_cast<TESNPC*>(actorBase);
        TESRace* race = npc->race.race ? npc->race.race : npc->race1EC;
        if (race) {
            inspection.raceName = CopyGameString(race->fullName.name);
        }
    }
    CaptureEquippedItems(actor, inspection.equipment);
    return true;
}

bool CaptureNativeActors(std::vector<NativeActorState>& actors, bool refreshEquipment) {
    actors.clear();
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    if (!player || !player->parentCell) return false;

    std::vector<Actor*> candidates;
    candidates.reserve(256);
    std::unordered_set<std::uint32_t> seen;
    seen.reserve(512);
    auto addActor = [&](Actor* actor) {
        if (!actor || actor == player || actor->refID == 0 || !seen.insert(actor->refID).second) return;
        candidates.push_back(actor);
    };

    std::size_t visited = 0;
    for (TESObjectCELL* sceneCell : GetSceneCells(player->parentCell)) {
        if (!IsAttachedSceneCell(sceneCell, player->parentCell)) {
            continue;
        }
        for (auto iterator = sceneCell->objectList.Begin();
             !iterator.End() && visited < 8192; ++iterator, ++visited) {
            TESObjectREFR* reference = iterator.Get();
            if (!reference ||
                (reference->typeID != kFormType_Character && reference->typeID != kFormType_Creature)) {
                continue;
            }
            addActor(static_cast<Actor*>(reference));
        }
        if (visited >= 8192) break;
    }

    g_knownActorReferences.clear();
    actors.reserve(candidates.size());
    for (Actor* actor : candidates) {
        TESObjectREFR* reference = actor;
        TESObjectCELL* cell = reference->parentCell;
        if (!cell || !reference->baseForm ||
            (reference->typeID != kFormType_Character && reference->typeID != kFormType_Creature)) {
            continue;
        }

        const std::uint8_t baseType = reference->baseForm->typeID;
        const bool validCharacter = reference->typeID == kFormType_Character &&
            baseType == kFormType_TESNPC;
        const bool validCreature = reference->typeID == kFormType_Creature &&
            baseType == kFormType_TESCreature;
        if (!validCharacter && !validCreature) {
            continue;
        }

        NativeActorState state;
        auto* actorBase = static_cast<TESActorBase*>(reference->baseForm);
        state.formId = reference->refID;
        state.baseFormId = reference->baseForm->refID;
        state.cellFormId = cell->refID;
        state.worldspaceFormId = cell->worldSpace ? cell->worldSpace->refID : 0;
        state.referenceType = reference->typeID;
        state.baseType = baseType;
        state.creature =
            reference->typeID == kFormType_Creature || state.baseType == kFormType_TESCreature;
        state.deleted = reference->IsDeleted() || reference->IsTaken() ||
            (reference->flags & 0x00000800U) != 0;
        state.loaded3D = reference->renderState && reference->renderState->niNode;
        state.interior = cell->IsInterior();
        state.scale = reference->scale;
        state.x = reference->posX;
        state.y = reference->posY;
        state.z = reference->posZ;
        state.yaw = reference->rotZ;
        if (actorBase &&
            (state.baseType == kFormType_TESNPC || state.baseType == kFormType_TESCreature)) {
            state.name = CopyGameString(actorBase->fullName.name);
            BGSVoiceType* voiceType = ResolveActorVoice(actorBase);
            state.voiceFormId = voiceType ? voiceType->refID : 0;
            state.voiceName = voiceType ? CopyGameCString(voiceType->GetName()) : "";
            state.level = actorBase->baseData.level;
            state.healthMax = actorBase->avOwner.Fn_01(eActorVal_Health);
            state.actionPointsMax = actorBase->avOwner.Fn_01(eActorVal_ActionPoints);
            state.female = state.baseType == kFormType_TESNPC && actorBase->baseData.IsFemale();
            if (state.baseType == kFormType_TESNPC) {
                auto* npc = static_cast<TESNPC*>(actorBase);
                TESRace* race = npc->race.race ? npc->race.race : npc->race1EC;
                if (race) {
                    state.raceFormId = race->refID;
                    state.raceName = CopyGameString(race->fullName.name);
                }
            }
        }
        state.inCombat = actor->IsInCombat();
        Actor* combatTarget = actor->GetCombatTarget();
        state.combatTargetFormId = combatTarget ? combatTarget->refID : 0;
        state.hostileToPlayer = combatTarget == player || player->GetCombatTarget() == actor;
        state.playerTeammate = IsPlayerTeammate(player, actor);
        state.health = actor->avOwner.Fn_03(eActorVal_Health);
        state.actionPoints = actor->avOwner.Fn_03(eActorVal_ActionPoints);
        state.dead = state.health <= 0.0f;
        if (actor->baseProcess) {
            state.weaponDrawn = actor->baseProcess->IsWeaponOut();
            state.facingStateKnown = true;
            state.sitSleepState = actor->baseProcess->GetSitSleepState();
            state.animationAction = actor->baseProcess->GetCurrentAnimAction();
            state.seated = state.sitSleepState == HighProcess::kSitSleepState_Sitting;
            state.animationBusy = state.animationAction != HighProcess::kAnimAction_None;
            TESPackage* package = actor->baseProcess->GetCurrentPackage();
            state.packageFormId = package ? package->refID : 0;
            ExtraContainerChanges::EntryData* weapon = actor->baseProcess->GetWeaponInfo();
            state.equippedWeaponFormId = weapon && weapon->type ? weapon->type->refID : 0;
        }
        if (refreshEquipment) {
            CaptureEquippedItems(actor, state.equipment);
            g_actorEquipmentCache[state.formId] = state.equipment;
        } else {
            const auto cached = g_actorEquipmentCache.find(state.formId);
            if (cached != g_actorEquipmentCache.end()) {
                state.equipment = cached->second;
            }
        }
        if (actor->actorMover) {
            const UInt32 movementFlags = actor->actorMover->Unk_08();
            state.moving = (movementFlags & 1u) != 0;
            state.running = (movementFlags & (1u << 8)) != 0;
            state.sneaking = (movementFlags & (1u << 9)) != 0;
        }
        const float dx = state.x - player->posX;
        const float dy = state.y - player->posY;
        const float dz = state.z - player->posZ;
        state.distanceToPlayer = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
        g_knownActorReferences[state.formId] = reference;
        actors.push_back(std::move(state));
    }

    return true;
}
bool CaptureNativeReferences(std::vector<NativeReferenceState>& references) {
    references.clear();
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    auto* interfaceManager = *reinterpret_cast<InterfaceManager**>(kInterfaceManagerAddress);
    if (!player || !player->parentCell || !interfaceManager) {
        return false;
    }

    const std::uint32_t crosshairFormId = interfaceManager->crosshairRef
        ? interfaceManager->crosshairRef->refID : 0;
    std::size_t visited = 0;
    std::size_t missingDoorQueries = 0;
    std::size_t staleDoorQueries = 0;
    const auto captureNow = std::chrono::steady_clock::now();
    references.reserve(512);
    for (TESObjectCELL* sceneCell : GetSceneCells(player->parentCell)) {
        if (!IsAttachedSceneCell(sceneCell, player->parentCell)) {
            continue;
        }
        for (auto iterator = sceneCell->objectList.Begin();
             !iterator.End() && visited < 16384; ++iterator, ++visited) {
            TESObjectREFR* reference = iterator.Get();
            if (!reference || reference == player || !reference->baseForm) {
                continue;
            }
            const std::uint8_t baseType = reference->baseForm->typeID;
            if (baseType == kFormType_TESNPC || baseType == kFormType_TESCreature) {
                continue;
            }

            NativeReferenceState state;
            state.name = CopyFormName(reference->baseForm);
            state.formId = reference->refID;
            state.baseFormId = reference->baseForm->refID;
            state.cellFormId = reference->parentCell ? reference->parentCell->refID : 0;
            state.baseType = baseType;
            state.deleted = (reference->flags & TESObjectREFR::kFlags_Deleted) != 0;
            state.taken = reference->IsTaken();
            state.loaded3D = reference->renderState && reference->renderState->niNode;
            state.crosshair = state.formId != 0 && state.formId == crosshairFormId;
            state.x = reference->posX;
            state.y = reference->posY;
            state.z = reference->posZ;
            state.yaw = reference->rotZ;
            const float dx = state.x - player->posX;
            const float dy = state.y - player->posY;
            const float dz = state.z - player->posZ;
            state.distanceToPlayer = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));

            if (baseType == kFormType_TESObjectDOOR) {
                auto cachedDoor = g_doorStateCache.find(state.formId);
                const bool missing = cachedDoor == g_doorStateCache.end();
                const bool stale = !missing &&
                    captureNow - cachedDoor->second.capturedAt > std::chrono::seconds(3);
                const bool queryMissing = missing && missingDoorQueries < 8;
                const bool queryStale = stale && staleDoorQueries < 2;
                if (queryMissing || queryStale) {
                    CachedDoorState refreshed;
                    if (QueryDoorState(reference, refreshed)) {
                        g_doorStateCache[state.formId] = refreshed;
                        cachedDoor = g_doorStateCache.find(state.formId);
                    }
                    if (queryMissing) ++missingDoorQueries;
                    if (queryStale) ++staleDoorQueries;
                }
                if (cachedDoor != g_doorStateCache.end()) {
                    state.openStateKnown = true;
                    state.openState = cachedDoor->second.openState;
                    state.locked = cachedDoor->second.locked;
                }
                auto* teleport = static_cast<ExtraTeleport*>(
                    reference->extraDataList.GetByType(kExtraData_Teleport));
                TESObjectREFR* linkedDoor = teleport && teleport->data
                    ? teleport->data->linkedDoor : nullptr;
                TESObjectCELL* destinationCell = linkedDoor ? linkedDoor->parentCell : nullptr;
                if (destinationCell) {
                    state.teleportDoor = true;
                    state.destinationCellFormId = destinationCell->refID;
                    state.destinationName = CopyGameString(destinationCell->fullName.name);
                }
            }

            if (state.formId != 0 && state.baseFormId != 0 && !state.deleted && !state.taken) {
                references.push_back(std::move(state));
            }
        }
        if (visited >= 16384) break;
    }
    return true;
}

bool CaptureNativeNavScene(NativeNavSceneState& scene) {
    scene = {};
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectCELL* cell = player ? player->parentCell : nullptr;
    if (!cell || (cell->cellState != TESObjectCELL::kCellLoadState_Loaded &&
                  cell->cellState != TESObjectCELL::kCellLoadState_Attached)) {
        return false;
    }

    scene.cellFormId = cell->refID;
    scene.worldspaceFormId = cell->worldSpace ? cell->worldSpace->refID : 0;
    scene.interior = cell->IsInterior();
    scene.valid = true;

    auto* navArrayAddress = static_cast<RawSimpleArray<void*>*>(cell->navmeshes);
    if (!navArrayAddress) {
        scene.complete = true;
        return true;
    }

    RawSimpleArray<void*> navArray;
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(navArrayAddress), navArray) ||
        navArray.size > kMaxNativeNavMeshes || navArray.capacity < navArray.size ||
        (navArray.size > 0 && !navArray.data)) {
        Logger::LogWarning("[NATIVE_NAV] rejected current-cell navmesh array cell=0x%08X size=%u capacity=%u",
            scene.cellFormId, navArray.size, navArray.capacity);
        return true;
    }

    std::vector<void*> meshPointers(navArray.size);
    if (!meshPointers.empty() && !SafeCopyReadable(
            meshPointers.data(), navArray.data, meshPointers.size() * sizeof(void*))) {
        Logger::LogWarning("[NATIVE_NAV] failed to copy navmesh pointer array cell=0x%08X",
            scene.cellFormId);
        return true;
    }

    std::size_t totalVertices = 0;
    std::size_t totalTriangles = 0;
    scene.meshes.reserve(meshPointers.size());
    for (void* meshPointer : meshPointers) {
        const std::uintptr_t meshAddress = reinterpret_cast<std::uintptr_t>(meshPointer);
        if (meshAddress == 0) continue;

        auto* form = reinterpret_cast<TESForm*>(meshPointer);
        std::uint8_t formType = 0;
        std::uint32_t formId = 0;
        RawSimpleArray<NavMeshVertex> vertexArray;
        RawSimpleArray<NavMeshTriangle> triangleArray;
        RawSimpleArray<RawDoorPortal> doorPortalArray;
        if (!SafeRead(reinterpret_cast<std::uintptr_t>(&form->typeID), formType) ||
            !SafeRead(reinterpret_cast<std::uintptr_t>(&form->refID), formId) ||
            formType != kFormType_NavMesh || formId == 0 ||
            !SafeRead(meshAddress + kNavMeshVerticesOffset, vertexArray) ||
            !SafeRead(meshAddress + kNavMeshTrianglesOffset, triangleArray) ||
            !SafeRead(meshAddress + kNavMeshDoorPortalsOffset, doorPortalArray)) {
            continue;
        }

        const bool invalidArrays =
            vertexArray.size > kMaxNativeNavVertices ||
            triangleArray.size > kMaxNativeNavTriangles ||
            doorPortalArray.size > kMaxNativeNavTriangles ||
            vertexArray.capacity < vertexArray.size ||
            triangleArray.capacity < triangleArray.size ||
            doorPortalArray.capacity < doorPortalArray.size ||
            (vertexArray.size > 0 && !vertexArray.data) ||
            (triangleArray.size > 0 && !triangleArray.data) ||
            (doorPortalArray.size > 0 && !doorPortalArray.data) ||
            totalVertices + vertexArray.size > kMaxNativeNavVertices ||
            totalTriangles + triangleArray.size > kMaxNativeNavTriangles;
        if (invalidArrays) {
            Logger::LogWarning(
                "[NATIVE_NAV] rejected mesh=0x%08X vertices=%u/%u triangles=%u/%u portals=%u/%u",
                formId,
                vertexArray.size, vertexArray.capacity,
                triangleArray.size, triangleArray.capacity,
                doorPortalArray.size, doorPortalArray.capacity);
            scene.meshes.clear();
            return true;
        }

        std::vector<NavMeshVertex> rawVertices(vertexArray.size);
        std::vector<NavMeshTriangle> rawTriangles(triangleArray.size);
        std::vector<RawDoorPortal> rawDoorPortals(doorPortalArray.size);
        if ((!rawVertices.empty() && !SafeCopyReadable(
                rawVertices.data(), vertexArray.data, rawVertices.size() * sizeof(NavMeshVertex))) ||
            (!rawTriangles.empty() && !SafeCopyReadable(
                rawTriangles.data(), triangleArray.data, rawTriangles.size() * sizeof(NavMeshTriangle))) ||
            (!rawDoorPortals.empty() && !SafeCopyReadable(
                rawDoorPortals.data(), doorPortalArray.data, rawDoorPortals.size() * sizeof(RawDoorPortal)))) {
            Logger::LogWarning("[NATIVE_NAV] failed to copy mesh geometry mesh=0x%08X", formId);
            scene.meshes.clear();
            return true;
        }

        NativeNavMeshState mesh;
        mesh.formId = formId;
        mesh.vertices.reserve(rawVertices.size());
        for (const auto& vertex : rawVertices) {
            mesh.vertices.push_back({
                vertex.coords[kCoords_X],
                vertex.coords[kCoords_Y],
                vertex.coords[kCoords_Z]
            });
        }
        mesh.triangles.reserve(rawTriangles.size());
        for (const auto& triangle : rawTriangles) {
            NativeNavTriangle item;
            for (std::size_t index = 0; index < 3; ++index) {
                item.vertices[index] = triangle.verticesIndex[index];
                item.neighbors[index] = triangle.sides[index];
            }
            item.flags = triangle.flags;
            mesh.triangles.push_back(item);
        }

        for (const auto& portal : rawDoorPortals) {
            if (portal.triangleIndex >= mesh.triangles.size() || !portal.door) continue;
            std::uint32_t doorFormId = 0;
            if (SafeRead(reinterpret_cast<std::uintptr_t>(&portal.door->refID), doorFormId)) {
                mesh.triangles[portal.triangleIndex].doorFormId = doorFormId;
            }
        }

        totalVertices += mesh.vertices.size();
        totalTriangles += mesh.triangles.size();
        scene.meshes.push_back(std::move(mesh));
    }

    scene.complete = true;
    Logger::LogInfo(
        "[NATIVE_NAV] captured cell=0x%08X meshes=%zu vertices=%zu triangles=%zu interior=%d",
        scene.cellFormId, scene.meshes.size(), totalVertices, totalTriangles, scene.interior ? 1 : 0);
    return true;
}

bool CaptureNativeQuest(NativeQuestState& quest) {
    quest = {};
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    if (!player) {
        return false;
    }
    TESQuest* activeQuest = player->quest;
    if (!activeQuest) {
        return true;
    }

    quest.valid = true;
    quest.formId = activeQuest->refID;
    quest.name = CopyGameString(activeQuest->fullName.name);
    quest.editorId = CopyGameString(activeQuest->editorName);
    std::size_t visited = 0;
    for (auto iterator = player->questObjectiveList.Begin();
         !iterator.End() && visited < 128; ++iterator, ++visited) {
        BGSQuestObjective* objective = iterator.Get();
        if (!objective || objective->quest != activeQuest ||
            (objective->status & BGSQuestObjective::eQObjStatus_displayed) == 0 ||
            (objective->status & BGSQuestObjective::eQObjStatus_completed) != 0) {
            continue;
        }
        NativeQuestObjective item;
        item.objectiveId = static_cast<int>(objective->objectiveId);
        item.text = CopyGameString(objective->displayText);
        if (!item.text.empty()) {
            quest.objectives.push_back(std::move(item));
        }
    }
    return true;
}

bool CaptureNativeInventory(std::uint32_t ownerFormId, std::vector<NativeInventoryItem>& items) {
    items.clear();
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* owner = FindKnownReference(player, ownerFormId);
    TESContainer* container = ResolveContainer(owner);
    if (!owner || !container) return false;

    struct MutableItem {
        TESForm* form{nullptr};
        int count{0};
        bool equipped{false};
        float condition{-1.0f};
    };
    std::unordered_map<std::uint32_t, MutableItem> merged;
    for (auto iterator = container->formCountList.Begin(); !iterator.End(); ++iterator) {
        TESContainer::FormCount* entry = iterator.Get();
        if (!entry || !entry->form || entry->form->typeID == kFormType_TESLevItem) continue;
        auto& item = merged[entry->form->refID];
        item.form = entry->form;
        item.count += entry->count;
    }

    auto* changes = static_cast<ExtraContainerChanges*>(
        owner->extraDataList.GetByType(kExtraData_ContainerChanges));
    ExtraContainerChanges::EntryDataList* entries = changes ? changes->GetEntryDataList() : nullptr;
    if (entries) {
        for (auto iterator = entries->Begin(); !iterator.End(); ++iterator) {
            ExtraContainerChanges::EntryData* entry = iterator.Get();
            if (!entry || !entry->type) continue;
            auto& item = merged[entry->type->refID];
            item.form = entry->type;
            item.count += entry->countDelta;
            if (entry->extendData) {
                for (auto extra = entry->extendData->Begin(); !extra.End(); ++extra) {
                    ExtraDataList* list = extra.Get();
                    if (list && (list->GetByType(kExtraData_Worn) || list->GetByType(kExtraData_WornLeft))) {
                        item.equipped = true;
                    }
                    if (list) {
                        auto* health = static_cast<ExtraHealth*>(list->GetByType(kExtraData_Health));
                        if (health) item.condition = health->health;
                    }
                }
            }
        }
    }

    items.reserve(merged.size());
    for (const auto& pair : merged) {
        const MutableItem& source = pair.second;
        if (!source.form || source.count <= 0) continue;
        NativeInventoryItem item;
        item.name = CopyFormName(source.form);
        item.baseFormId = source.form->refID;
        item.type = source.form->typeID;
        item.count = source.count;
        item.value = ResolveBaseItemValue(source.form);
        item.equipped = source.equipped;
        item.condition = source.condition;
        items.push_back(std::move(item));
    }
    return true;
}

bool CaptureNativeLoadedPlugins(std::vector<NativeLoadedPlugin>& plugins) {
    plugins.clear();
    auto** singleton = reinterpret_cast<DataHandler**>(kDataHandlerSingletonAddress);
    DataHandler* dataHandler = singleton ? *singleton : nullptr;
    if (!dataHandler) {
        return false;
    }
    const UInt32 count = std::min<UInt32>(dataHandler->modList.loadedModCount, 0xFF);
    plugins.reserve(count);
    std::unordered_set<std::string> seen;
    for (UInt32 index = 0; index < count; ++index) {
        ModInfo* mod = dataHandler->modList.loadedMods[index];
        if (!mod || mod->name[0] == '\0') {
            continue;
        }
        std::string name(mod->name, strnlen_s(mod->name, sizeof(mod->name)));
        std::string key = name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (!seen.insert(key).second) {
            continue;
        }
        plugins.push_back({std::move(name), static_cast<std::uint32_t>(mod->modIndex)});
    }
    return true;
}

bool CaptureNativeFactions(std::vector<NativeFaction>& factions) {
    factions.clear();
    auto** singleton = reinterpret_cast<DataHandler**>(kDataHandlerSingletonAddress);
    DataHandler* dataHandler = singleton ? *singleton : nullptr;
    if (!dataHandler) {
        return false;
    }
    std::unordered_set<std::uint32_t> seen;
    for (auto iterator = dataHandler->factionList.Begin(); !iterator.End(); ++iterator) {
        TESFaction* faction = iterator.Get();
        if (!faction || faction->refID == 0 || !seen.insert(faction->refID).second) {
            continue;
        }
        std::string name = CopyGameString(faction->fullName.name);
        if (!name.empty()) {
            factions.push_back({std::move(name), faction->refID});
        }
    }
    return true;
}

void BeginNativeMapMarkerCapture() {
    std::lock_guard<std::mutex> lock(g_mapMarkerMutex);
    g_mapMarkerCapture = {};
    auto** singleton = reinterpret_cast<DataHandler**>(kDataHandlerSingletonAddress);
    g_mapMarkerCapture.dataHandler = singleton ? *singleton : nullptr;
    g_mapMarkerCapture.active = g_mapMarkerCapture.dataHandler != nullptr;
    if (g_mapMarkerCapture.active) {
        g_mapMarkerCapture.markers.reserve(512);
        g_mapMarkerCapture.seen.reserve(1024);
        Logger::LogInfo("[NATIVE_WORLD] incremental map-marker capture started cells=%u",
            g_mapMarkerCapture.dataHandler->cellArray.Length());
    }
}

bool AdvanceNativeMapMarkerCapture(std::vector<NativeMapMarker>& markers,
                                   bool& complete,
                                   std::uint32_t cellBudget,
                                   std::uint32_t referenceBudget) {
    markers.clear();
    complete = false;
    if (!g_mapMarkerCapture.active && !g_mapMarkerCapture.complete) {
        BeginNativeMapMarkerCapture();
    }
    std::lock_guard<std::mutex> lock(g_mapMarkerMutex);
    if (!g_mapMarkerCapture.dataHandler) return false;
    if (g_mapMarkerCapture.complete) {
        markers = g_mapMarkerCapture.markers;
        complete = true;
        return true;
    }

    const UInt32 cellCount =
        (std::min<UInt32>)(g_mapMarkerCapture.dataHandler->cellArray.Length(), 100000);
    const std::uint32_t maxCells = (std::max<std::uint32_t>)(1, cellBudget);
    const std::uint32_t maxReferences = (std::max<std::uint32_t>)(1, referenceBudget);
    std::uint32_t cellsCompleted = 0;
    std::uint32_t referencesVisited = 0;

    while (g_mapMarkerCapture.nextCell < cellCount &&
           cellsCompleted < maxCells && referencesVisited < maxReferences) {
        TESObjectCELL* cell =
            g_mapMarkerCapture.dataHandler->cellArray.Get(g_mapMarkerCapture.nextCell);
        if (!cell) {
            ++g_mapMarkerCapture.nextCell;
            g_mapMarkerCapture.nextReference = 0;
            ++cellsCompleted;
            continue;
        }

        auto iterator = cell->objectList.Begin();
        std::size_t referenceIndex = 0;
        while (!iterator.End() && referenceIndex < g_mapMarkerCapture.nextReference) {
            ++iterator;
            ++referenceIndex;
        }

        while (!iterator.End() && referencesVisited < maxReferences && referenceIndex < 5000) {
            TESObjectREFR* reference = iterator.Get();
            ++iterator;
            ++referenceIndex;
            ++referencesVisited;
            g_mapMarkerCapture.nextReference = referenceIndex;
            if (!reference || reference->refID == 0 ||
                !g_mapMarkerCapture.seen.insert(reference->refID).second) {
                continue;
            }
            auto* marker = reinterpret_cast<ExtraMapMarker*>(
                reference->extraDataList.GetByType(kExtraData_MapMarker));
            if (!marker || !marker->data) continue;

            std::string name = CopyGameString(marker->data->fullName.name);
            if (name.empty()) name = CopyGameString(cell->fullName.name);
            if (name.empty()) continue;

            NativeMapMarker result;
            result.name = std::move(name);
            result.formId = reference->refID;
            result.flags = marker->data->flags;
            result.markerType = marker->data->type;
            result.interior = cell->worldSpace == nullptr;
            result.x = reference->posX;
            result.y = reference->posY;
            result.worldspaceName = cell->worldSpace
                ? CopyGameString(cell->worldSpace->fullName.name)
                : CopyGameString(cell->fullName.name);
            g_mapMarkerCapture.markers.push_back(std::move(result));
        }

        if (iterator.End() || referenceIndex >= 5000) {
            ++g_mapMarkerCapture.nextCell;
            g_mapMarkerCapture.nextReference = 0;
            ++cellsCompleted;
        }
    }

    if (g_mapMarkerCapture.nextCell >= cellCount) {
        g_mapMarkerCapture.active = false;
        g_mapMarkerCapture.complete = true;
        markers = g_mapMarkerCapture.markers;
        complete = true;
        Logger::LogInfo("[NATIVE_WORLD] incremental map-marker capture complete cells=%u markers=%zu",
            cellCount, markers.size());
    } else if (g_mapMarkerCapture.nextCell % 1000 < maxCells) {
        Logger::LogDebug("[NATIVE_WORLD] map-marker progress cell=%u/%u markers=%zu",
            g_mapMarkerCapture.nextCell, cellCount, g_mapMarkerCapture.markers.size());
    }
    return true;
}

bool CaptureNativeMapMarkers(std::vector<NativeMapMarker>& markers) {
    std::lock_guard<std::mutex> lock(g_mapMarkerMutex);
    markers.clear();
    if (!g_mapMarkerCapture.complete) return false;
    markers = g_mapMarkerCapture.markers;
    return true;
}
bool CaptureNativeDialoguePrompt(const void* speakerReference,
                                 const void* topicOrInfo,
                                 NativeDialoguePrompt& prompt) {
    prompt = {};
    RawDialoguePrompt raw;
    if (!TryCaptureRawDialoguePrompt(speakerReference, topicOrInfo, raw)) {
        prompt.source = "invalid_native_pointer";
        return false;
    }
    prompt.formType = raw.formType;
    prompt.topicFormId = raw.topicFormId;
    prompt.parentTopicFormId = raw.parentTopicFormId;
    prompt.targetFormId = raw.targetFormId;
    prompt.targetName = CopyFormName(raw.targetBase);
    if (raw.formType != kFormType_TESTopicInfo && raw.formType != kFormType_TESTopic) {
        prompt.source = "unsupported_topic_type";
        return true;
    }
    prompt.prompt = raw.prompt ? CopyGameString(*raw.prompt) : std::string{};
    if (!prompt.prompt.empty()) {
        prompt.source = raw.formType == kFormType_TESTopicInfo ? "topic_info_prompt" : "topic_fullname";
        return true;
    }
    prompt.prompt = raw.fallbackPrompt ? CopyGameString(*raw.fallbackPrompt) : std::string{};
    prompt.source = prompt.prompt.empty() ? "empty_topic_info" : "parent_topic";
    return true;
}

bool UpdateNativeDialogueGuards(const std::vector<std::uint32_t>& actorFormIds) {
    g_nativeDialogueGuardAttempts.fetch_add(1, std::memory_order_relaxed);
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    BGSVoiceType* silentVoice = FindSilentVoiceType();
    if (!player || !silentVoice) {
        g_nativeDialogueGuardFailed.fetch_add(1, std::memory_order_relaxed);
        Logger::LogWarning("[NATIVE_DIALOGUE_GUARD] unavailable player=%p silent_voice=%p",
            player, silentVoice);
        return false;
    }

    std::unordered_set<std::uint32_t> desired;
    std::unordered_map<std::uint32_t, TESActorBase*> resolved;
    for (std::uint32_t actorFormId : actorFormIds) {
        if (actorFormId == 0 || actorFormId == player->refID) {
            continue;
        }
        TESObjectREFR* reference = FindLoadedReference(player, actorFormId);
        if (!reference || !reference->baseForm ||
            (reference->baseForm->typeID != kFormType_TESNPC &&
             reference->baseForm->typeID != kFormType_TESCreature)) {
            g_nativeDialogueGuardFailed.fetch_add(1, std::memory_order_relaxed);
            Logger::LogWarning("[NATIVE_DIALOGUE_GUARD] actor unavailable ref=0x%08X", actorFormId);
            return false;
        }
        desired.insert(actorFormId);
        resolved.emplace(actorFormId, static_cast<TESActorBase*>(reference->baseForm));
    }

    std::vector<std::uint32_t> remove;
    remove.reserve(g_guardedActorRefs.size());
    for (const auto& entry : g_guardedActorRefs) {
        if (!desired.contains(entry.first)) {
            remove.push_back(entry.first);
        }
    }
    for (std::uint32_t actorFormId : remove) {
        RemoveDialogueGuard(actorFormId);
    }

    for (const auto& entry : resolved) {
        const std::uint32_t actorFormId = entry.first;
        TESActorBase* actorBase = entry.second;
        if (g_guardedActorRefs.contains(actorFormId)) {
            continue;
        }

        auto baseIt = g_guardedActorBases.find(actorBase);
        if (baseIt == g_guardedActorBases.end()) {
            GuardedActorBase guarded;
            guarded.actorBase = actorBase;
            guarded.originalVoice = actorBase->baseData.GetVoiceType();
            guarded.actorRefs.insert(actorFormId);
            actorBase->baseData.voiceType = silentVoice;
            g_guardedActorBases.emplace(actorBase, std::move(guarded));
        } else {
            baseIt->second.actorRefs.insert(actorFormId);
        }
        g_guardedActorRefs.emplace(actorFormId, actorBase);
        const auto guardedIt = g_guardedActorBases.find(actorBase);
        const std::uint32_t originalVoiceFormId =
            guardedIt != g_guardedActorBases.end() && guardedIt->second.originalVoice
            ? guardedIt->second.originalVoice->refID : 0;
        Logger::LogInfo(
            "[NATIVE_DIALOGUE_GUARD] muted ref=0x%08X base=0x%08X original_voice=0x%08X silent_voice=0x%08X",
            actorFormId,
            actorBase->refID,
            originalVoiceFormId,
            silentVoice->refID);
    }

    g_nativeDialogueGuardApplied.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void RestoreNativeDialogueGuards() {
    const std::size_t guardedRefs = g_guardedActorRefs.size();
    for (auto& entry : g_guardedActorBases) {
        if (entry.second.actorBase) {
            entry.second.actorBase->baseData.voiceType = entry.second.originalVoice;
        }
    }
    g_guardedActorRefs.clear();
    g_guardedActorBases.clear();
    if (guardedRefs > 0) {
        g_nativeDialogueGuardRestores.fetch_add(1, std::memory_order_relaxed);
        Logger::LogInfo("[NATIVE_DIALOGUE_GUARD] restored refs=%zu", guardedRefs);
    }
}

bool SetNativePassiveSubtitle(const std::string& text) {
    if (text.empty()) {
        ClearNativePassiveSubtitle();
        return true;
    }
    g_nativeSubtitleAttempts.fetch_add(1, std::memory_order_relaxed);
    if (!ResolvePassiveSubtitleTiles()) {
        g_nativeSubtitleFailed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    CALL_MEMBER_FN(g_passiveSubtitleTextTile, SetStringValue)(Tile::kTileValue_string, text.c_str(), true);
    CALL_MEMBER_FN(g_passiveSubtitleTile, SetFloatValue)(Tile::kTileValue_visible, 1.0f, true);
    CALL_MEMBER_FN(g_passiveSubtitleTile, SetFloatValue)(Tile::kTileValue_alpha, 255.0f, true);
    g_nativeSubtitleApplied.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ClearNativePassiveSubtitle() {
    if (!ResolvePassiveSubtitleTiles()) {
        g_passiveSubtitleTile = nullptr;
        g_passiveSubtitleTextTile = nullptr;
        return;
    }
    CALL_MEMBER_FN(g_passiveSubtitleTextTile, SetStringValue)(Tile::kTileValue_string, "", true);
    CALL_MEMBER_FN(g_passiveSubtitleTile, SetFloatValue)(Tile::kTileValue_visible, 0.0f, true);
    CALL_MEMBER_FN(g_passiveSubtitleTile, SetFloatValue)(Tile::kTileValue_alpha, 0.0f, true);
}

void InvalidateNativePresentation() {
    g_passiveSubtitleTile = nullptr;
    g_passiveSubtitleTextTile = nullptr;
}

void InvalidateNativeObjectCache() {
    g_knownRuntimeReferences.clear();
    g_knownActorReferences.clear();
    g_actorEquipmentCache.clear();
    g_doorStateCache.clear();
    g_sceneCellCache = {};
    {
        std::lock_guard<std::mutex> lock(g_mapMarkerMutex);
        g_mapMarkerCapture = {};
    }
    g_faceGenBankOffset = 0;
    ClearFaceGenSession();
}

bool ApplyNativeFacing(std::uint32_t speakerFormId, std::uint32_t targetFormId, float yawDegrees) {
    g_nativeFacingAttempts.fetch_add(1, std::memory_order_relaxed);
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt || speakerFormId == 0 || targetFormId == 0) {
        g_nativeFacingFailed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* speaker = FindLoadedReference(player, speakerFormId);
    TESObjectREFR* target = FindLoadedReference(player, targetFormId);
    if (!speaker || !target) {
        g_nativeFacingFailed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (!g_faceTargetFunction) {
        static constexpr const char* kFaceTargetSource = R"(
float fYaw
begin function {fYaw}
    SetAngle Z fYaw
end
)";
        g_faceTargetFunction = g_scriptInterface->CompileScript(kFaceTargetSource);
        if (!g_faceTargetFunction) {
            g_nativeFacingFailed.fetch_add(1, std::memory_order_relaxed);
            Logger::LogError("[NATIVE_FACING] failed to compile facing function");
            return false;
        }
    }

    UInt32 yawBits = 0;
    static_assert(sizeof(yawBits) == sizeof(yawDegrees));
    std::memcpy(&yawBits, &yawDegrees, sizeof(yawBits));
    const bool applied = g_scriptInterface->CallFunctionAlt(
        g_faceTargetFunction, speaker, 1, yawBits);
    if (!applied) {
        g_nativeFacingFailed.fetch_add(1, std::memory_order_relaxed);
        Logger::LogWarning("[NATIVE_FACING] call failed speaker=0x%08X target=0x%08X",
            speakerFormId, targetFormId);
    } else {
        g_nativeFacingApplied.fetch_add(1, std::memory_order_relaxed);
    }
    return applied;
}

bool ClearNativeFacing(std::uint32_t speakerFormId) {
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt || speakerFormId == 0) {
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* speaker = FindLoadedReference(player, speakerFormId);
    if (!speaker) {
        return false;
    }
    if (!g_stopLookFunction) {
        static constexpr const char* kStopLookSource = R"(
begin function {}
    StopLook
end
)";
        g_stopLookFunction = g_scriptInterface->CompileScript(kStopLookSource);
        if (!g_stopLookFunction) {
            Logger::LogError("[NATIVE_FACING] failed to compile stop-look function");
            return false;
        }
    }
    return g_scriptInterface->CallFunctionAlt(g_stopLookFunction, speaker, 0);
}

bool OpenNativeToolMenu(NativeToolMenu menu) {
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt) {
        return false;
    }

    Script** function = nullptr;
    const char* source = nullptr;
    const char* name = "unknown";
    switch (menu) {
        case NativeToolMenu::DialecticControl:
            function = &g_dialecticControlMenuFunction;
            name = "dialectic_control";
            source = R"(
begin function {}
    MessageBoxExAlt (CompileScript "Dialectic/DialecticControlMenuSelect.gek") "^DIALECTIC Control^Choose a setting:|Chat Modes|LLM Model|Dynamic Profiles|Close Menu"
end
)";
            break;
        case NativeToolMenu::Mode:
            function = &g_modeMenuFunction;
            name = "mode";
            source = R"(
begin function {}
    MessageBoxExAlt (CompileScript "Dialectic/ModeMenuSelect.gek") "^DIALECTIC Chat Modes^Select active chat mode:|Standard|Whisper|Close|Shout|Narrator|Director|Inject Event|Inject & Chat|Cheat Mode|Close Menu"
end
)";
            break;
        case NativeToolMenu::LlmModel:
            function = &g_llmModelMenuFunction;
            name = "llm_model";
            source = R"(
begin function {}
    MessageBoxExAlt (CompileScript "Dialectic/LLMModelMenuSelect.gek") "^DIALECTIC LLM Model^Select active LLM connector slot:|Standard LLM|Fast LLM|Powerful LLM|Experimental LLM|Close Menu"
end
)";
            break;
        case NativeToolMenu::DynamicProfile:
            function = &g_dynamicProfileMenuFunction;
            name = "dynamic_profile";
            source = R"(
begin function {}
    MessageBoxExAlt (CompileScript "Dialectic/DynamicProfileMenuSelect.gek") "^DIALECTIC Dynamic Profiles^Select profile update target:|Target NPC|Nearby AI NPCs|Narrator|Close Menu"
end
)";
            break;
    }

    if (!function || !source) return false;
    if (!*function) {
        *function = g_scriptInterface->CompileScript(source);
        if (!*function) {
            Logger::LogError("[NATIVE_UI] failed to compile %s menu function", name);
            return false;
        }
    }
    const bool opened = g_scriptInterface->CallFunctionAlt(*function, nullptr, 0);
    Logger::LogInfo("[NATIVE_UI] %s menu request dispatched success=%d", name, opened ? 1 : 0);
    return opened;
}

bool ToggleNativePipVisionMenus() {
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt) {
        return false;
    }

    if (!g_pipVisionToggleMenusFunction) {
        static constexpr const char* kToggleMenusSource = R"(
begin function {}
    Con_ToggleMenus
end
)";
        g_pipVisionToggleMenusFunction = g_scriptInterface->CompileScript(kToggleMenusSource);
        if (!g_pipVisionToggleMenusFunction) {
            Logger::LogError("[PIPVISION] failed to compile HUD toggle function");
            return false;
        }
    }

    const bool toggled = g_scriptInterface->CallFunctionAlt(
        g_pipVisionToggleMenusFunction, nullptr, 0);
    Logger::LogInfo("[PIPVISION] HUD toggle dispatched success=%d", toggled ? 1 : 0);
    return toggled;
}

bool CaptureNativePipVisionScreenshot() {
    if (!g_scriptInterface || !g_scriptInterface->CompileScript || !g_scriptInterface->CallFunction) {
        return false;
    }

    const HWND gameWindow = GetForegroundWindow();
    DWORD foregroundProcessId = 0;
    if (!gameWindow || GetWindowThreadProcessId(gameWindow, &foregroundProcessId) == 0 ||
        foregroundProcessId != GetCurrentProcessId()) {
        Logger::LogWarning("[PIPVISION] Fallout window is not foreground during capture");
        return false;
    }

    RECT clientRect{};
    POINT clientTopLeft{};
    POINT clientBottomRight{};
    if (!GetClientRect(gameWindow, &clientRect)) {
        Logger::LogWarning("[PIPVISION] failed to read Fallout client rectangle error=%lu", GetLastError());
        return false;
    }
    clientBottomRight.x = clientRect.right;
    clientBottomRight.y = clientRect.bottom;
    if (!ClientToScreen(gameWindow, &clientTopLeft) || !ClientToScreen(gameWindow, &clientBottomRight) ||
        clientBottomRight.x <= clientTopLeft.x || clientBottomRight.y <= clientTopLeft.y) {
        Logger::LogWarning("[PIPVISION] failed to resolve Fallout client screen bounds error=%lu", GetLastError());
        return false;
    }

    if (!g_pipVisionCaptureFunction) {
        static constexpr const char* kCaptureSource = R"(
int iXStart
int iXEnd
int iYStart
int iYEnd
begin function {iXStart, iXEnd, iYStart, iYEnd}
    SetFunctionValue 0
    if GetPluginVersion "SUP NVSE Plugin" < 855
        return
    endif
    DeleteScreenshot "Dialectic" "pipvision_capture.jpg"
    CaptureScreenshotAlt "Dialectic" "pipvision_capture" iXStart iXEnd iYStart iYEnd 0 0 90
    SetFunctionValue 1
end
)";
        g_pipVisionCaptureFunction = g_scriptInterface->CompileScript(kCaptureSource);
        if (!g_pipVisionCaptureFunction) {
            Logger::LogError("[PIPVISION] failed to compile SUP screenshot function");
            return false;
        }
    }

    alignas(NVSEArrayVarInterface::Element)
        unsigned char resultStorage[sizeof(NVSEArrayVarInterface::Element)]{};
    auto* result = reinterpret_cast<NVSEArrayVarInterface::Element*>(resultStorage);
    Logger::LogInfo("[PIPVISION] Fallout client capture bounds left=%ld top=%ld right=%ld bottom=%ld",
        clientTopLeft.x, clientTopLeft.y, clientBottomRight.x, clientBottomRight.y);
    if (!g_scriptInterface->CallFunction(g_pipVisionCaptureFunction, nullptr, nullptr, result, 4,
            static_cast<UInt32>(clientTopLeft.x), static_cast<UInt32>(clientBottomRight.x),
            static_cast<UInt32>(clientTopLeft.y), static_cast<UInt32>(clientBottomRight.y))) {
        Logger::LogWarning("[PIPVISION] SUP screenshot function call failed");
        return false;
    }
    return result->GetNumber() > 0.0;
}

bool ApplyNativeMfg(std::uint32_t actorFormId, int phoneme, int intensity, bool reset) {
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt || actorFormId == 0) {
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* actor = FindLoadedReference(player, actorFormId);
    if (!actor) {
        return false;
    }

    if (reset) {
        if (!g_mfgResetFunction) {
            static constexpr const char* kResetSource = R"(
begin function {}
    MFG Reset
end
)";
            g_mfgResetFunction = g_scriptInterface->CompileScript(kResetSource);
            if (!g_mfgResetFunction) {
                Logger::LogError("[NATIVE_LIPSYNC] failed to compile MFG reset function");
                return false;
            }
        }
        return g_scriptInterface->CallFunctionAlt(g_mfgResetFunction, actor, 0);
    }

    if (!g_mfgPhonemeFunction) {
        static constexpr const char* kPhonemeSource = R"(
int iPhoneme
int iIntensity
begin function {iPhoneme, iIntensity}
    MFG Phoneme iPhoneme iIntensity
end
)";
        g_mfgPhonemeFunction = g_scriptInterface->CompileScript(kPhonemeSource);
        if (!g_mfgPhonemeFunction) {
            Logger::LogError("[NATIVE_LIPSYNC] failed to compile MFG phoneme function");
            return false;
        }
    }
    return g_scriptInterface->CallFunctionAlt(
        g_mfgPhonemeFunction, actor, 2, static_cast<UInt32>(phoneme), static_cast<UInt32>(intensity));
}

bool ApplyNativeFaceGenLipSync(std::uint32_t actorFormId,
                               int phoneme,
                               int intensity,
                               int decayIntensity,
                               bool reset) {
    g_nativeFaceGenAttempts.fetch_add(1, std::memory_order_relaxed);
    if (actorFormId == 0) {
        g_nativeFaceGenFailed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool applied = false;
    __try {
        auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
        TESObjectREFR* reference = ResolveFaceGenSessionActor(player, actorFormId);
        if (!reference || reference->refID != actorFormId || !reference->baseForm ||
            (reference->baseForm->typeID != kFormType_TESNPC &&
             reference->baseForm->typeID != kFormType_TESCreature)) {
            g_nativeFaceGenFailed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        Actor* actor = static_cast<Actor*>(reference);
        BaseProcess* baseProcess = actor->baseProcess;
        if (!baseProcess || baseProcess->processLevel > BaseProcess::kProcessLevel_MiddleHigh) {
            g_nativeFaceGenFailed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        MiddleHighProcess* process = static_cast<MiddleHighProcess*>(baseProcess);
        const std::uintptr_t processAnimationData =
            reinterpret_cast<std::uintptr_t>(process->unk178);
        const std::uintptr_t faceNodeA = reinterpret_cast<std::uintptr_t>(process->unk248);
        const std::uintptr_t faceNodeB = reinterpret_cast<std::uintptr_t>(process->unk24C);
        std::uintptr_t nodeAAnimationData = 0;
        std::uintptr_t nodeBAnimationData = 0;
        if (faceNodeA != 0) {
            SafeRead(faceNodeA + kFaceNodeAnimationDataOffset, nodeAAnimationData);
        }
        if (faceNodeB != 0) {
            SafeRead(faceNodeB + kFaceNodeAnimationDataOffset, nodeBAnimationData);
        }

        applied = ApplyFaceGenCandidate(processAnimationData, actorFormId,
                                        phoneme, intensity, decayIntensity, reset);
        if (!applied && nodeAAnimationData != 0 && nodeAAnimationData != processAnimationData) {
            applied = ApplyFaceGenCandidate(nodeAAnimationData, actorFormId,
                                            phoneme, intensity, decayIntensity, reset);
        }
        if (!applied && nodeBAnimationData != 0 &&
            nodeBAnimationData != processAnimationData && nodeBAnimationData != nodeAAnimationData) {
            applied = ApplyFaceGenCandidate(nodeBAnimationData, actorFormId,
                                            phoneme, intensity, decayIntensity, reset);
        }
        if (applied) {
            const bool faceNodesChanged = faceNodeA != g_faceGenSessionNodeA ||
                faceNodeB != g_faceGenSessionNodeB;
            if (reset || !g_faceGenSessionArmed || faceNodesChanged) {
                TouchFaceNode(faceNodeA, reset);
                TouchFaceNode(faceNodeB, reset);
                g_faceGenSessionNodeA = faceNodeA;
                g_faceGenSessionNodeB = faceNodeB;
                g_faceGenSessionArmed = !reset;
            }
            if (reset) {
                ClearFaceGenSession();
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        applied = false;
        ClearFaceGenSession();
    }

    if (!applied) {
        g_nativeFaceGenFailed.fetch_add(1, std::memory_order_relaxed);
        ++g_faceGenFailureCount;
        if (g_faceGenFailureCount <= 5 || g_faceGenFailureCount % 40 == 0) {
            Logger::LogWarning(
                "[NATIVE_LIPSYNC] FaceGen unavailable count=%llu actor=0x%08X phoneme=%d reset=%d",
                static_cast<unsigned long long>(g_faceGenFailureCount),
                actorFormId,
                phoneme,
                reset ? 1 : 0);
        }
    } else {
        g_nativeFaceGenApplied.fetch_add(1, std::memory_order_relaxed);
    }
    return applied;
}

bool ResetNativeLipSync(std::uint32_t actorFormId) {
    if (actorFormId == 0) {
        return false;
    }

    // FaceGen owns the viseme banks while MFG can retain engine-level facial
    // state. Clear both at line boundaries so an interrupted line cannot leave
    // either source holding the actor's mouth open.
    const bool faceGenReset = ApplyNativeFaceGenLipSync(actorFormId, -1, 0, 100, true);
    const bool mfgReset = ApplyNativeMfg(actorFormId, -1, 0, true);
    Logger::LogInfo(
        "[NATIVE_LIPSYNC] reset actor=0x%08X facegen=%d mfg=%d",
        actorFormId,
        faceGenReset ? 1 : 0,
        mfgReset ? 1 : 0);
    return faceGenReset || mfgReset;
}

bool HaltNativeActor(std::uint32_t actorFormId) {
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt || actorFormId == 0) {
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* actor = FindLoadedReference(player, actorFormId);
    if (!actor) {
        return false;
    }
    if (!g_haltActorFunction) {
        static constexpr const char* kHaltSource = R"(
begin function {}
    RemoveScriptPackage
    SetRestrained 0
    StopCombat
    RemoveFromFaction DialecticAttackFaction
    RemoveFromFaction DialecticMoveToFaction
    RemoveFromFaction DialecticFollowFaction
    RemoveFromFaction DialecticTravelFaction
    RemoveFromFaction DialecticWaitFaction
    RemoveFromFaction DialecticSeatFaction
    SetWeaponOut 0
    EvaluatePackage
end
)";
        g_haltActorFunction = g_scriptInterface->CompileScript(kHaltSource);
        if (!g_haltActorFunction) {
            Logger::LogError("[NATIVE_ACTION] failed to compile halt function");
            return false;
        }
    }
    return g_scriptInterface->CallFunctionAlt(g_haltActorFunction, actor, 0);
}

bool ExecuteSimpleNativeAction(std::uint32_t actorFormId, int actionCode) {
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt || actorFormId == 0) {
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* actor = FindLoadedReference(player, actorFormId);
    if (!actor) {
        return false;
    }
    if (!g_simpleActionFunction) {
        static constexpr const char* kSimpleActionSource = R"(
int iActionCode
begin function {iActionCode}
    if eval iActionCode == 14
        ModAV SpeedMult 20
    elseif eval iActionCode == 15
        ModAV SpeedMult -20
    elseif eval iActionCode == 16
        SetWeaponOut 0
    elseif eval iActionCode == 17
        RemoveScriptPackage
        SetRestrained 0
        RemoveFromFaction DialecticMoveToFaction
        RemoveFromFaction DialecticFollowFaction
        RemoveFromFaction DialecticTravelFaction
        RemoveFromFaction DialecticWaitFaction
        RemoveFromFaction DialecticSeatFaction
        StopCombat
        EvaluatePackage
    elseif eval iActionCode == 19
        RemoveScriptPackage
        StopCombat
        SetRestrained 0
        RemoveFromFaction DialecticMoveToFaction
        RemoveFromFaction DialecticFollowFaction
        RemoveFromFaction DialecticTravelFaction
        RemoveFromFaction DialecticWaitFaction
        RemoveFromFaction DialecticSeatFaction
        SetWeaponOut 0
        ForceActorDetectionValue 0
        EvaluatePackage
    else
        SetFunctionValue 0
        return
    endif
    SetFunctionValue 1
end
)";
        g_simpleActionFunction = g_scriptInterface->CompileScript(kSimpleActionSource);
        if (!g_simpleActionFunction) {
            Logger::LogError("[NATIVE_ACTION] failed to compile simple action function");
            return false;
        }
    }
    return g_scriptInterface->CallFunctionAlt(
        g_simpleActionFunction, actor, 1, static_cast<UInt32>(actionCode));
}

bool ExecuteNativePackageAction(std::uint32_t actorFormId, std::uint32_t targetFormId, int actionCode) {
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt || actorFormId == 0) {
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* actor = FindLoadedReference(player, actorFormId);
    if (!actor) {
        return false;
    }
    if (targetFormId == 0 && (actionCode == 4 || actionCode == 5 || actionCode == 6)) {
        targetFormId = player ? player->refID : 0x14;
    } else if (targetFormId == 0 && (actionCode == 18 || actionCode == 33)) {
        targetFormId = actorFormId;
    }
    TESObjectREFR* target = targetFormId == 0 ? nullptr : FindLoadedReference(player, targetFormId);
    if ((actionCode == 7 || actionCode == 8 || actionCode == 20 || actionCode == 21) && !target) {
        return false;
    }

    if (!g_packageActionFunction) {
        static constexpr const char* kPackageActionSource = R"(
int iActionCode
int iTargetMod
int iTargetLocal
ref rTarget
begin function {iActionCode, iTargetMod, iTargetLocal}
    let rTarget := BuildRef iTargetMod iTargetLocal
    RemoveScriptPackage
    SetRestrained 0
    StopCombat
    RemoveFromFaction DialecticMoveToFaction
    RemoveFromFaction DialecticFollowFaction
    RemoveFromFaction DialecticTravelFaction
    RemoveFromFaction DialecticWaitFaction
    RemoveFromFaction DialecticSeatFaction
    SetWeaponOut 0
    EvaluatePackage

    if eval iActionCode == 4 || iActionCode == 5
        SetPlayerTeammate 1
        AddToFaction DialecticFollowFaction 0
        SetPackageTargetReference DialecticFollowTargetPackage PlayerRef
        SetPackageTargetDistance DialecticFollowTargetPackage 192
        AddScriptPackage DialecticFollowTargetPackage
    elseif eval iActionCode == 6
        AddToFaction DialecticMoveToFaction 0
        SetPackageLocationReference DialecticMoveToTargetPackage PlayerRef
        SetPackageTargetDistance DialecticMoveToTargetPackage 96
        AddScriptPackage DialecticMoveToTargetPackage
    elseif eval iActionCode == 7 && rTarget
        AddToFaction DialecticMoveToFaction 0
        SetPackageLocationReference DialecticMoveToTargetPackage rTarget
        SetPackageTargetDistance DialecticMoveToTargetPackage 96
        AddScriptPackage DialecticMoveToTargetPackage
    elseif eval iActionCode == 8 && rTarget
        AddToFaction DialecticFollowFaction 0
        SetPackageTargetReference DialecticFollowTargetPackage rTarget
        SetPackageTargetDistance DialecticFollowTargetPackage 192
        AddScriptPackage DialecticFollowTargetPackage
    elseif eval iActionCode == 18
        AddToFaction DialecticWaitFaction 0
        SetPackageLocationReference DialecticWaitPackage rTarget
        SetPackageTargetDistance DialecticWaitPackage 64
        AddScriptPackage DialecticWaitPackage
    elseif eval iActionCode == 33
        AddToFaction DialecticWaitFaction 0
        SetPackageLocationReference DialecticWaitPackage rTarget
        SetPackageTargetDistance DialecticWaitPackage 96
        AddScriptPackage DialecticWaitPackage
    elseif eval iActionCode == 20 && rTarget
        AddToFaction DialecticSeatFaction 0
        SetPackageTargetReference DialecticSeatPackage rTarget
        SetPackageLocationReference DialecticSeatPackage rTarget
        SetPackageTargetDistance DialecticSeatPackage 96
        AddScriptPackage DialecticSeatPackage
    elseif eval iActionCode == 21 && rTarget
        AddToFaction DialecticTravelFaction 0
        SetPackageLocationReference DialecticTravelToPackage rTarget
        SetPackageTargetDistance DialecticTravelToPackage 160
        AddScriptPackage DialecticTravelToPackage
    else
        SetFunctionValue 0
        return
    endif
    EvaluatePackage
    SetFunctionValue 1
end
)";
        g_packageActionFunction = g_scriptInterface->CompileScript(kPackageActionSource);
        if (!g_packageActionFunction) {
            Logger::LogError("[NATIVE_ACTION] failed to compile package action function");
            return false;
        }
    }

    const UInt32 targetMod = (targetFormId >> 24) & 0xFF;
    const UInt32 targetLocal = targetFormId & 0x00FFFFFF;
    return g_scriptInterface->CallFunctionAlt(g_packageActionFunction, actor, 3,
        static_cast<UInt32>(actionCode), targetMod, targetLocal);
}

bool CaptureNativeCombatActorState(std::uint32_t actorFormId, NativeCombatActorState& state) {
    state = {};
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    auto* reference = FindLoadedReference(player, actorFormId);
    if (!reference || (reference->baseForm->typeID != kFormType_TESNPC &&
                       reference->baseForm->typeID != kFormType_TESCreature)) {
        return false;
    }
    auto* actor = static_cast<Actor*>(reference);
    state.valid = true;
    state.player = actor == player;
    state.formId = actor->refID;
    state.playerTeammate = !state.player && IsPlayerTeammate(player, actor);
    state.aggression = static_cast<int>(std::lround(actor->avOwner.Fn_03(eActorVal_Aggression)));
    state.confidence = static_cast<int>(std::lround(actor->avOwner.Fn_03(eActorVal_Confidence)));
    state.assistance = static_cast<int>(std::lround(actor->avOwner.Fn_03(eActorVal_Assistance)));
    return true;
}

bool ExecuteNativeAttack(std::uint32_t speakerFormId, std::uint32_t targetFormId) {
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt || speakerFormId == 0 || targetFormId == 0) {
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* speaker = FindLoadedReference(player, speakerFormId);
    TESObjectREFR* target = FindLoadedReference(player, targetFormId);
    if (!speaker || !target || speaker == target) {
        return false;
    }

    if (!g_attackActionFunction) {
        static constexpr const char* kAttackSource = R"(
int iSpeakerMod
int iSpeakerLocal
int iTargetMod
int iTargetLocal
ref rSpeaker
ref rTarget
begin function {iSpeakerMod, iSpeakerLocal, iTargetMod, iTargetLocal}
    let rSpeaker := BuildRef iSpeakerMod iSpeakerLocal
    let rTarget := BuildRef iTargetMod iTargetLocal
    if eval !(rSpeaker) || !(rTarget) || !(rTarget.IsActor)
        SetFunctionValue 0
        return
    endif
    SetPlayerTeammate 0
    SetRestrained 0
    SetAV Aggression 3
    SetAV Confidence 4
    SetAV Assistance 0
    ForceActorDetectionValue 100
    SetWeaponOut 1
    RemoveScriptPackage
    StopCombat
    AddToFaction DialecticAttackFaction 0
    if eval rTarget != PlayerRef
        rTarget.SetPlayerTeammate 0
        rTarget.SetRestrained 0
        rTarget.SetAV Aggression 3
        rTarget.SetAV Confidence 4
        rTarget.SetAV Assistance 0
        rTarget.ForceActorDetectionValue 100
        rTarget.RemoveScriptPackage
        rTarget.StopCombat
        rTarget.AddToFaction DialecticWaitFaction 0
    endif
    SetEnemy DialecticAttackFaction DialecticWaitFaction 0 0
    SetPackageLocationReference DialecticAttackPackage rTarget
    SetPackageTargetReference DialecticAttackPackage rTarget
    SetPackageTargetDistance DialecticAttackPackage 512
    AddScriptPackage DialecticAttackPackage
    if eval rTarget != PlayerRef
        rTarget.StartCombat rSpeaker
        rTarget.EvaluatePackage
    endif
    StartCombat rTarget
    EvaluatePackage
    SetFunctionValue 1
end
)";
        g_attackActionFunction = g_scriptInterface->CompileScript(kAttackSource);
        if (!g_attackActionFunction) {
            Logger::LogError("[NATIVE_ACTION] failed to compile attack function");
            return false;
        }
    }

    const UInt32 speakerMod = (speakerFormId >> 24) & 0xFF;
    const UInt32 speakerLocal = speakerFormId & 0x00FFFFFF;
    const UInt32 targetMod = (targetFormId >> 24) & 0xFF;
    const UInt32 targetLocal = targetFormId & 0x00FFFFFF;
    return g_scriptInterface->CallFunctionAlt(g_attackActionFunction, speaker, 4,
        speakerMod, speakerLocal, targetMod, targetLocal);
}

bool RestoreNativeCombatActorState(const NativeCombatActorState& state) {
    if (!state.valid || state.player || state.formId == 0 || !g_scriptInterface ||
        !g_scriptInterface->CompileScript || !g_scriptInterface->CallFunctionAlt) {
        return state.player;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* actor = FindLoadedReference(player, state.formId);
    if (!actor) {
        return false;
    }
    if (!g_restoreCombatActorFunction) {
        static constexpr const char* kRestoreSource = R"(
int iTeammate
int iAggression
int iConfidence
int iAssistance
begin function {iTeammate, iAggression, iConfidence, iAssistance}
    ForceActorDetectionValue 0
    RemoveScriptPackage
    StopCombat
    RemoveFromFaction DialecticAttackFaction
    RemoveFromFaction DialecticWaitFaction
    SetAV Aggression iAggression
    SetAV Confidence iConfidence
    SetAV Assistance iAssistance
    SetPlayerTeammate iTeammate
    SetWeaponOut 0
    SetRestrained 0
    EvaluatePackage
end
)";
        g_restoreCombatActorFunction = g_scriptInterface->CompileScript(kRestoreSource);
        if (!g_restoreCombatActorFunction) {
            Logger::LogError("[NATIVE_ACTION] failed to compile combat restore function");
            return false;
        }
    }
    return g_scriptInterface->CallFunctionAlt(g_restoreCombatActorFunction, actor, 4,
        static_cast<UInt32>(state.playerTeammate ? 1 : 0),
        static_cast<UInt32>(state.aggression < 0 ? 0 : state.aggression),
        static_cast<UInt32>(state.confidence < 0 ? 0 : state.confidence),
        static_cast<UInt32>(state.assistance < 0 ? 0 : state.assistance));
}

bool ExecuteNativeInventoryAction(std::uint32_t speakerFormId,
                                  std::uint32_t targetFormId,
                                  std::uint32_t itemBaseFormId,
                                  int amount,
                                  int actionCode) {
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt || speakerFormId == 0 || amount <= 0) {
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* speaker = FindLoadedReference(player, speakerFormId);
    if (!speaker) {
        return false;
    }
    if ((actionCode == 9 || actionCode == 10 || actionCode == 11) &&
        !FindLoadedReference(player, targetFormId)) {
        return false;
    }
    if ((actionCode == 11 || actionCode == 13 || actionCode == 31 || actionCode == 32) &&
        itemBaseFormId == 0) {
        return false;
    }

    if (!g_inventoryActionFunction) {
        static constexpr const char* kInventorySource = R"(
int iActionCode
int iTargetMod
int iTargetLocal
int iItemMod
int iItemLocal
int iAmount
ref rTarget
ref rItem
begin function {iActionCode, iTargetMod, iTargetLocal, iItemMod, iItemLocal, iAmount}
    let rTarget := BuildRef iTargetMod iTargetLocal
    let rItem := BuildRef iItemMod iItemLocal
    if eval iActionCode == 9 && rTarget
        if eval GetItemCount Caps001 >= iAmount
            RemoveItemTarget Caps001 rTarget iAmount 1
        endif
    elseif eval iActionCode == 10 && rTarget
        if eval PlayerRef.GetItemCount Caps001 >= iAmount
            PlayerRef.RemoveItemTarget Caps001 rTarget iAmount 1
        endif
    elseif eval iActionCode == 11 && rTarget && rItem
        if eval GetItemCount rItem >= iAmount
            RemoveItemTarget rItem rTarget iAmount 1
        endif
    elseif eval iActionCode == 13 && rItem
        if eval GetItemCount rItem > 0
            EquipItem rItem 1 1
        endif
    elseif eval iActionCode == 31 && rItem
        if eval GetItemCount rItem > 0
            EquipItem rItem 1 1
        endif
    elseif eval iActionCode == 32 && rItem
        if eval GetItemCount rItem > 0
            UnequipItem rItem 1
        endif
    else
        SetFunctionValue 0
        return
    endif
    SetFunctionValue 1
end
)";
        g_inventoryActionFunction = g_scriptInterface->CompileScript(kInventorySource);
        if (!g_inventoryActionFunction) {
            Logger::LogError("[NATIVE_ACTION] failed to compile inventory action function");
            return false;
        }
    }

    const UInt32 targetMod = (targetFormId >> 24) & 0xFF;
    const UInt32 targetLocal = targetFormId & 0x00FFFFFF;
    const UInt32 itemMod = (itemBaseFormId >> 24) & 0xFF;
    const UInt32 itemLocal = itemBaseFormId & 0x00FFFFFF;
    return g_scriptInterface->CallFunctionAlt(g_inventoryActionFunction, speaker, 6,
        static_cast<UInt32>(actionCode), targetMod, targetLocal, itemMod, itemLocal,
        static_cast<UInt32>(amount));
}

bool ExecuteNativeCompanionCommand(std::uint32_t actorFormId,
                                   int actionCode,
                                   bool& handled,
                                   bool& usedCcc) {
    handled = false;
    usedCcc = false;
    if (actionCode != 4 && actionCode != 5 && actionCode != 33) {
        return false;
    }
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunction || actorFormId == 0) {
        return false;
    }

    std::vector<NativeLoadedPlugin> plugins;
    if (!CaptureNativeLoadedPlugins(plugins)) {
        return false;
    }
    const bool cccLoaded = std::any_of(plugins.begin(), plugins.end(), [](const NativeLoadedPlugin& plugin) {
        std::string name = plugin.name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return name == "jip companions command & control.esp";
    });
    if (!cccLoaded) {
        return false;
    }

    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* actor = FindLoadedReference(player, actorFormId);
    if (!actor) {
        return false;
    }
    if (!g_cccManagedQueryFunction) {
        g_cccManagedQueryFunction = g_scriptInterface->CompileScript(R"(
begin function {}
    SetFunctionValue CCCInFaction JIPCCCIsHired
end
)");
    }
    if (!g_cccManagedQueryFunction) {
        Logger::LogWarning("[NATIVE_ACTION] JIP CCC detected but managed companion query did not compile");
        return false;
    }

    alignas(NVSEArrayVarInterface::Element)
        unsigned char managedStorage[sizeof(NVSEArrayVarInterface::Element)]{};
    auto* managedResult = reinterpret_cast<NVSEArrayVarInterface::Element*>(managedStorage);
    if (!g_scriptInterface->CallFunction(g_cccManagedQueryFunction, actor, nullptr, managedResult, 0) ||
        managedResult->GetNumber() == 0.0) {
        return false;
    }
    handled = true;
    usedCcc = true;

    if (!g_cccCompanionCommandFunction) {
        g_cccCompanionCommandFunction = g_scriptInterface->CompileScript(R"(
int iActionCode
int iSlot
int iTask
ref rSelf
begin function {iActionCode}
    let rSelf := GetSelf
    if eval CCCInFaction JIPCCCIsHired == 0
        SetFunctionValue 0
        return
    endif
    let iSlot := rSelf.Call JIPCCCGetSlot
    if eval iSlot != -1
        let iTask := JIPCCCActiveTasks.aTaskData[iSlot][0]
        Call JIPCCCAbortTask iSlot, iTask, rSelf, 3
    endif
    RemoveScriptPackage
    SetRestrained 0
    StopCombat
    RemoveFromFaction DialecticMoveToFaction
    RemoveFromFaction DialecticFollowFaction
    RemoveFromFaction DialecticTravelFaction
    RemoveFromFaction DialecticWaitFaction
    RemoveFromFaction DialecticSeatFaction
    if eval iActionCode == 33
        SetFactionRank JIPCCCCurrentTask 13
        AddScriptPackage JIPCCCRelax
    elseif eval iActionCode == 4 || iActionCode == 5
        RemoveFromFaction JIPCCCCurrentTask
        SetFactionRank JIPCCCFollowState 1
        CCCSetFollowState 1
        rSelf.Call JIPCCCAddPackages
    else
        SetFunctionValue 0
        return
    endif
    EvaluatePackage
    SetFunctionValue 1
end
)");
    }
    if (!g_cccCompanionCommandFunction) {
        Logger::LogWarning("[NATIVE_ACTION] JIP CCC companion command did not compile");
        return false;
    }

    alignas(NVSEArrayVarInterface::Element)
        unsigned char commandStorage[sizeof(NVSEArrayVarInterface::Element)]{};
    auto* commandResult = reinterpret_cast<NVSEArrayVarInterface::Element*>(commandStorage);
    if (!g_scriptInterface->CallFunction(g_cccCompanionCommandFunction, actor, nullptr,
            commandResult, 1, static_cast<UInt32>(actionCode))) {
        return false;
    }
    return commandResult->GetNumber() != 0.0;
}

bool CaptureNativePlayerSurvivalState(NativePlayerSurvivalState& state) {
    state = {};
    PlayerCharacter* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    if (!player) {
        return false;
    }

    auto readNeed = [player](UInt32 actorValue) {
        const float value = player->avOwner.Fn_03(actorValue);
        return std::isfinite(value) && value > 0.0f ? value : 0.0f;
    };

    state.valid = true;
    state.hardcoreEnabled = player->isHardcore;
    state.dehydration = readNeed(eActorVal_Dehydration);
    state.hunger = readNeed(eActorVal_Hunger);
    state.sleepDeprivation = readNeed(eActorVal_Sleepdeprevation);
    state.radiation = readNeed(eActorVal_RadLevel);
    return true;
}

bool AddNativeItemToActor(std::uint32_t targetFormId,
                          std::uint32_t itemBaseFormId,
                          int amount,
                          std::string& failureReason) {
    failureReason.clear();
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt) {
        failureReason = "script_interface_unavailable";
        return false;
    }
    if (targetFormId == 0 || itemBaseFormId == 0 || amount <= 0 || amount > 1000000) {
        failureReason = "invalid_arguments";
        return false;
    }

    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* target = FindLoadedReference(player, targetFormId);
    if (!target || !target->baseForm ||
        (target != player && target->baseForm->typeID != kFormType_TESNPC &&
         target->baseForm->typeID != kFormType_TESCreature)) {
        failureReason = "target_not_loaded_actor";
        return false;
    }

    if (!g_addItemToActorFunction) {
        static constexpr const char* kAddItemSource = R"(
int iItemMod
int iItemLocal
int iAmount
ref rItem
begin function {iItemMod, iItemLocal, iAmount}
    let rItem := BuildRef iItemMod iItemLocal
    if eval !(rItem) || iAmount <= 0
        SetFunctionValue 0
        return
    endif
    AddItem rItem iAmount 1
    SetFunctionValue 1
end
)";
        g_addItemToActorFunction = g_scriptInterface->CompileScript(kAddItemSource);
        if (!g_addItemToActorFunction) {
            failureReason = "add_item_script_compile_failed";
            return false;
        }
    }

    const UInt32 itemMod = (itemBaseFormId >> 24) & 0xFF;
    const UInt32 itemLocal = itemBaseFormId & 0x00FFFFFF;
    if (!g_scriptInterface->CallFunctionAlt(g_addItemToActorFunction, target, 3,
            itemMod, itemLocal, static_cast<UInt32>(amount))) {
        failureReason = "add_item_call_failed";
        return false;
    }
    return true;
}

bool TeleportNativeActor(std::uint32_t targetFormId,
                         std::uint32_t destinationFormId,
                         std::string& failureReason) {
    failureReason.clear();
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt) {
        failureReason = "script_interface_unavailable";
        return false;
    }
    if (targetFormId == 0 || destinationFormId == 0) {
        failureReason = "invalid_arguments";
        return false;
    }

    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* target = FindLoadedReference(player, targetFormId);
    if (!target || !target->baseForm ||
        (target != player && target->baseForm->typeID != kFormType_TESNPC &&
         target->baseForm->typeID != kFormType_TESCreature)) {
        failureReason = "target_not_loaded_actor";
        return false;
    }

    if (!g_teleportActorFunction) {
        static constexpr const char* kTeleportSource = R"(
int iDestinationMod
int iDestinationLocal
ref rDestination
begin function {iDestinationMod, iDestinationLocal}
    let rDestination := BuildRef iDestinationMod iDestinationLocal
    if eval !(rDestination)
        SetFunctionValue 0
        return
    endif
    MoveTo rDestination
    SetFunctionValue 1
end
)";
        g_teleportActorFunction = g_scriptInterface->CompileScript(kTeleportSource);
        if (!g_teleportActorFunction) {
            failureReason = "teleport_script_compile_failed";
            return false;
        }
    }

    const UInt32 destinationMod = (destinationFormId >> 24) & 0xFF;
    const UInt32 destinationLocal = destinationFormId & 0x00FFFFFF;
    if (!g_scriptInterface->CallFunctionAlt(g_teleportActorFunction, target, 2,
            destinationMod, destinationLocal)) {
        failureReason = "teleport_call_failed";
        return false;
    }
    return true;
}

bool KillNativeActor(std::uint32_t targetFormId,
                     std::string& failureReason) {
    failureReason.clear();
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    if (!player || targetFormId == 0) {
        failureReason = "invalid_target";
        return false;
    }
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt) {
        failureReason = "script_interface_unavailable";
        return false;
    }

    TESObjectREFR* target = FindLoadedReference(player, targetFormId);
    if (!target || !target->baseForm ||
        (target->baseForm->typeID != kFormType_TESNPC &&
         target->baseForm->typeID != kFormType_TESCreature)) {
        failureReason = "target_not_loaded_actor";
        return false;
    }

    if (!g_killActorFunction) {
        static constexpr const char* kKillSource = R"(
begin function {}
    Kill
    SetFunctionValue 1
end
)";
        g_killActorFunction = g_scriptInterface->CompileScript(kKillSource);
        if (!g_killActorFunction) {
            failureReason = "kill_script_compile_failed";
            return false;
        }
    }

    if (!g_scriptInterface->CallFunctionAlt(g_killActorFunction, target, 0)) {
        failureReason = "kill_call_failed";
        return false;
    }
    return true;
}

bool TransferNativeWorldReferenceToActor(std::uint32_t actorFormId,
                                         std::uint32_t itemReferenceFormId) {
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt || actorFormId == 0 || itemReferenceFormId == 0) {
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* actor = FindLoadedReference(player, actorFormId);
    TESObjectREFR* item = FindLoadedReference(player, itemReferenceFormId);
    if (!actor || !item || item->baseForm->typeID == kFormType_TESNPC ||
        item->baseForm->typeID == kFormType_TESCreature) {
        return false;
    }
    if (!g_pickupTransferFunction) {
        static constexpr const char* kPickupSource = R"(
int iActorMod
int iActorLocal
int iItemMod
int iItemLocal
ref rActor
ref rItem
ref rBase
float fBefore
float fAfter
begin function {iActorMod, iActorLocal, iItemMod, iItemLocal}
    let rActor := BuildRef iActorMod iActorLocal
    let rItem := BuildRef iItemMod iItemLocal
    if eval !(rActor) || !(rItem)
        SetFunctionValue 0
        return
    endif
    let rBase := rItem.GetBaseForm
    let fBefore := rActor.GetItemCount rBase
    rItem.MoveToContainer rActor 1
    let fAfter := rActor.GetItemCount rBase
    if eval fAfter <= fBefore
        rItem.Activate rActor 0
    endif
    rActor.RemoveScriptPackage
    rActor.RemoveFromFaction DialecticMoveToFaction
    rActor.EvaluatePackage
    SetFunctionValue 1
end
)";
        g_pickupTransferFunction = g_scriptInterface->CompileScript(kPickupSource);
        if (!g_pickupTransferFunction) {
            Logger::LogError("[NATIVE_ACTION] failed to compile pickup transfer function");
            return false;
        }
    }
    const UInt32 actorMod = (actorFormId >> 24) & 0xFF;
    const UInt32 actorLocal = actorFormId & 0x00FFFFFF;
    const UInt32 itemMod = (itemReferenceFormId >> 24) & 0xFF;
    const UInt32 itemLocal = itemReferenceFormId & 0x00FFFFFF;
    return g_scriptInterface->CallFunctionAlt(g_pickupTransferFunction, actor, 4,
        actorMod, actorLocal, itemMod, itemLocal);
}

bool OpenNativeTeammateContainer(std::uint32_t actorFormId) {
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt || actorFormId == 0) {
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* actor = FindLoadedReference(player, actorFormId);
    if (!actor) {
        return false;
    }
    if (!g_openTeammateContainerFunction) {
        static constexpr const char* kOpenContainerSource = R"(
begin function {}
    SetRestrained 0
    StopCombat
    EvaluatePackage
    OpenTeammateContainer 1
end
)";
        g_openTeammateContainerFunction = g_scriptInterface->CompileScript(kOpenContainerSource);
        if (!g_openTeammateContainerFunction) {
            Logger::LogError("[NATIVE_ACTION] failed to compile teammate container function");
            return false;
        }
    }
    return g_scriptInterface->CallFunctionAlt(g_openTeammateContainerFunction, actor, 0);
}

bool ExecuteNativeStopFollowing(std::uint32_t actorFormId) {
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt || actorFormId == 0) {
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* actor = FindLoadedReference(player, actorFormId);
    if (!actor) {
        return false;
    }
    if (!g_stopFollowingFunction) {
        static constexpr const char* kStopFollowingSource = R"(
int iActorMod
int iActorLocal
ref rSpeaker
begin function {iActorMod, iActorLocal}
    let rSpeaker := BuildRef iActorMod iActorLocal
    RemoveScriptPackage
    StopCombat
    SetRestrained 0
    SetPlayerTeammate 0
    RemoveFromFaction FollowerFaction
    RemoveFromFaction DialecticMoveToFaction
    RemoveFromFaction DialecticFollowFaction
    RemoveFromFaction DialecticTravelFaction
    RemoveFromFaction DialecticWaitFaction
    RemoveFromFaction DialecticSeatFaction
    SetAV Assistance 0
    RemovePerk CompanionSuite
    SetWeaponOut 0
    ForceActorDetectionValue 0
    EvaluatePackage
    if eval rSpeaker == CraigBooneREF
        set VNPCFollowers.bHumanoidInParty to 0
        set CraigBooneREF.Waiting to 0
        set VNPCFollowers.bBooneHired to 0
        set VNPCFollowers.bBooneFired to 1
        set VNPCFollowers.bBooneL38 to 0
        Player.RemoveFromFaction VBooneFaction
        Player.RemovePerk Spotting
    elseif eval rSpeaker == ArcadeREF
        set VNPCFollowers.bHumanoidInParty to 0
        set ArcadeREF.Waiting to 0
        set VNPCFollowers.ArcadeHired to 0
        set VNPCFollowers.ArcadeFired to 1
        set VNPCFollowers.bArcadeL38 to 0
        Player.RemovePerk BetterHealing
    elseif eval rSpeaker == RoseofSharonCassidyREF
        set VNPCFollowers.bHumanoidInParty to 0
        set RoseofSharonCassidyREF.Waiting to 0
        set VNPCFollowers.bCassHired to 0
        set VNPCFollowers.bCassFired to 1
        set VNPCFollowers.bCassL38 to 0
        Player.RemovePerk WhiskeyRose
        StopQuest vCassTimer
    elseif eval rSpeaker == LilyREF
        set VNPCFollowers.bHumanoidInParty to 0
        set LilyREF.Waiting to 0
        set VNPCFollowers.bLilyHired to 0
        set VNPCFollowers.bLilyFired to 1
        set VNPCFollowers.bLilyL38 to 0
        Player.RemovePerk StealthGirl
        StopQuest LilysMedicineTimer
    elseif eval rSpeaker == VeronicaREF
        set VNPCFollowers.bHumanoidInParty to 0
        set VeronicaREF.Waiting to 0
        set VNPCFollowers.bVeronicaHired to 0
        set VNPCFollowers.bVeronicaFired to 1
        set VNPCFollowers.bVeronicaL38 to 0
        Player.RemovePerk ScribeAssistant
    elseif eval rSpeaker == RaulREF
        set VNPCFollowers.bHumanoidInParty to 0
        set RaulREF.Waiting to 0
        set VNPCFollowers.RaulHired to 0
        set VNPCFollowers.RaulFired to 1
        set VNPCFollowers.bRaulL38 to 0
        Player.RemovePerk RegularMaintenance
    elseif eval rSpeaker == RexREF
        set VNPCFollowers.bCritterInParty to 0
        set RexREF.Waiting to 0
        set VNPCFollowers.RexHired to 0
        set VNPCFollowers.RexFired to 1
        set VNPCFollowers.bRexL38 to 0
        Player.RemovePerk SearchAndMark
    elseif eval rSpeaker == EDE1REF || rSpeaker == EDE2REF || rSpeaker == EDE3REF
        set VNPCFollowers.bCritterInParty to 0
        EDE1REF.SetPlayerTeammate 0
        EDE1REF.RemoveFromFaction FollowerFaction
        EDE1REF.SetAV Assistance 0
        EDE1REF.RemovePerk CompanionSuite
        EDE2REF.SetPlayerTeammate 0
        EDE2REF.RemoveFromFaction FollowerFaction
        EDE2REF.SetAV Assistance 0
        EDE2REF.RemovePerk CompanionSuite
        EDE3REF.SetPlayerTeammate 0
        EDE3REF.RemoveFromFaction FollowerFaction
        EDE3REF.SetAV Assistance 0
        EDE3REF.RemovePerk CompanionSuite
        set EDE1REF.Waiting to 0
        set EDE2REF.Waiting to 0
        set EDE3REF.Waiting to 0
        set VNPCFollowers.bEDEHired to 0
        set VNPCFollowers.bEDEFired to 1
        set VNPCFollowers.bEDEL38 to 0
        EDE1REF.SetAV Aggression 0
        EDE2REF.SetAV Aggression 0
        EDE3REF.SetAV Aggression 0
        Player.RemovePerk EnhancedSensors
    endif
    if eval VNPCFollowers.bHumanoidInParty == 0 && VNPCFollowers.bCritterInParty == 0
        set VNPCFollowers.bPlayerHasFollower to 0
    else
        set VNPCFollowers.bPlayerHasFollower to 1
    endif
end
)";
        g_stopFollowingFunction = g_scriptInterface->CompileScript(kStopFollowingSource);
        if (!g_stopFollowingFunction) {
            Logger::LogError("[NATIVE_ACTION] failed to compile stop-following function");
            return false;
        }
    }
    const UInt32 actorMod = (actorFormId >> 24) & 0xFF;
    const UInt32 actorLocal = actorFormId & 0x00FFFFFF;
    return g_scriptInterface->CallFunctionAlt(g_stopFollowingFunction, actor, 2,
        actorMod, actorLocal);
}

bool ResolveNativeTradeMenu(std::uint32_t actorFormId,
                            bool preferBarter,
                            NativeTradeMenuInfo& info) {
    info = {};
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunction || actorFormId == 0) {
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* actor = FindLoadedReference(player, actorFormId);
    if (!actor) {
        return false;
    }
    if (!g_queryMerchantContainerFunction) {
        g_queryMerchantContainerFunction = g_scriptInterface->CompileScript(R"(
ref rMerchant
begin function {}
    let rMerchant := GetMerchantContainer
    SetFunctionValue rMerchant
end
)");
    }
    if (!g_queryOffersServicesFunction) {
        g_queryOffersServicesFunction = g_scriptInterface->CompileScript(R"(
begin function {}
    SetFunctionValue GetOffersServicesNow
end
)");
    }
    if (!g_queryMerchantContainerFunction || !g_queryOffersServicesFunction) {
        Logger::LogError("[NATIVE_ACTION] failed to compile trade menu query functions");
        return false;
    }

    alignas(NVSEArrayVarInterface::Element)
        unsigned char merchantStorage[sizeof(NVSEArrayVarInterface::Element)]{};
    alignas(NVSEArrayVarInterface::Element)
        unsigned char offersStorage[sizeof(NVSEArrayVarInterface::Element)]{};
    auto* merchantResult = reinterpret_cast<NVSEArrayVarInterface::Element*>(merchantStorage);
    auto* offersResult = reinterpret_cast<NVSEArrayVarInterface::Element*>(offersStorage);
    const bool merchantCall = g_scriptInterface->CallFunction(
        g_queryMerchantContainerFunction, actor, nullptr, merchantResult, 0);
    const bool offersCall = g_scriptInterface->CallFunction(
        g_queryOffersServicesFunction, actor, nullptr, offersResult, 0);
    if (!merchantCall || !offersCall) {
        return false;
    }
    TESForm* merchantForm = merchantResult->GetTESForm();
    const std::uint32_t merchantFormId = merchantResult->GetFormID();
    if (merchantForm && merchantFormId != 0) {
        g_knownRuntimeReferences[merchantFormId] = static_cast<TESObjectREFR*>(merchantForm);
    }
    const bool offersServices = offersResult->GetNumber() > 0.0;
    info.valid = true;
    info.actorFormId = actorFormId;
    info.barter = preferBarter && (merchantFormId != 0 || offersServices);
    info.inventoryOwnerFormId = info.barter && merchantFormId != 0
        ? merchantFormId : actorFormId;
    return true;
}

bool OpenNativeTradeMenu(const NativeTradeMenuInfo& info) {
    if (!info.valid || !g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunctionAlt) {
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* actor = FindLoadedReference(player, info.actorFormId);
    if (!actor) {
        return false;
    }
    if (!info.barter) {
        return OpenNativeTeammateContainer(info.actorFormId);
    }
    if (!g_openBarterMenuFunction) {
        g_openBarterMenuFunction = g_scriptInterface->CompileScript(R"(
begin function {}
    SetRestrained 0
    StopCombat
    EvaluatePackage
    ShowBarterMenu 0
end
)");
        if (!g_openBarterMenuFunction) {
            Logger::LogError("[NATIVE_ACTION] failed to compile barter menu function");
            return false;
        }
    }
    return g_scriptInterface->CallFunctionAlt(g_openBarterMenuFunction, actor, 0);
}

bool QueryNativeLineOfSight(std::uint32_t sourceFormId,
                            std::uint32_t targetFormId,
                            bool& hasLineOfSight) {
    hasLineOfSight = false;
    if (!g_scriptInterface || !g_scriptInterface->CompileScript ||
        !g_scriptInterface->CallFunction || sourceFormId == 0 || targetFormId == 0) {
        return false;
    }
    auto* player = *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
    TESObjectREFR* source = FindLoadedReference(player, sourceFormId);
    TESObjectREFR* target = FindLoadedReference(player, targetFormId);
    if (!source || !target) {
        return false;
    }
    if (!g_lineOfSightFunction) {
        g_lineOfSightFunction = g_scriptInterface->CompileScript(R"(
int iTargetMod
int iTargetLocal
ref rTarget
begin function {iTargetMod, iTargetLocal}
    let rTarget := BuildRef iTargetMod iTargetLocal
    if eval !(rTarget)
        SetFunctionValue 0
        return
    endif
    SetFunctionValue GetLOS rTarget
end
)");
        if (!g_lineOfSightFunction) {
            Logger::LogError("[NATIVE_SPATIAL] failed to compile LOS function");
            return false;
        }
    }
    alignas(NVSEArrayVarInterface::Element)
        unsigned char resultStorage[sizeof(NVSEArrayVarInterface::Element)]{};
    auto* result = reinterpret_cast<NVSEArrayVarInterface::Element*>(resultStorage);
    const UInt32 targetMod = (targetFormId >> 24) & 0xFF;
    const UInt32 targetLocal = targetFormId & 0x00FFFFFF;
    if (!g_scriptInterface->CallFunction(g_lineOfSightFunction, source, nullptr,
            result, 2, targetMod, targetLocal)) {
        return false;
    }
    hasLineOfSight = result->GetNumber() > 0.0;
    return true;
}

bool QueueNativeNotification(const std::string& message, std::uint32_t emotion) {
    if (message.empty()) {
        return false;
    }
    using QueueUiMessage = bool (*)(const char*, UInt32, const char*, const char*, float, bool);
    auto queueMessage = reinterpret_cast<QueueUiMessage>(kQueueUiMessageAddress);
    __try {
        return queueMessage(message.c_str(), emotion, nullptr, nullptr, 3.0f, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logger::LogError("[NATIVE_PRESENTATION] QueueUIMessage raised an exception");
        return false;
    }
}

NativePresentationDiagnostics GetNativePresentationDiagnostics() {
    NativePresentationDiagnostics diagnostics;
    diagnostics.faceGenAttempts = g_nativeFaceGenAttempts.load(std::memory_order_relaxed);
    diagnostics.faceGenApplied = g_nativeFaceGenApplied.load(std::memory_order_relaxed);
    diagnostics.faceGenFailed = g_nativeFaceGenFailed.load(std::memory_order_relaxed);
    diagnostics.subtitleAttempts = g_nativeSubtitleAttempts.load(std::memory_order_relaxed);
    diagnostics.subtitleApplied = g_nativeSubtitleApplied.load(std::memory_order_relaxed);
    diagnostics.subtitleFailed = g_nativeSubtitleFailed.load(std::memory_order_relaxed);
    diagnostics.dialogueGuardAttempts = g_nativeDialogueGuardAttempts.load(std::memory_order_relaxed);
    diagnostics.dialogueGuardApplied = g_nativeDialogueGuardApplied.load(std::memory_order_relaxed);
    diagnostics.dialogueGuardFailed = g_nativeDialogueGuardFailed.load(std::memory_order_relaxed);
    diagnostics.dialogueGuardRestores = g_nativeDialogueGuardRestores.load(std::memory_order_relaxed);
    diagnostics.facingAttempts = g_nativeFacingAttempts.load(std::memory_order_relaxed);
    diagnostics.facingApplied = g_nativeFacingApplied.load(std::memory_order_relaxed);
    diagnostics.facingFailed = g_nativeFacingFailed.load(std::memory_order_relaxed);
    return diagnostics;
}

} // namespace XNVSEAdapter
