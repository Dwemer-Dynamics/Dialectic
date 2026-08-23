#include "ActionManager.h"

#include "ActorPositionResolverFNV.h"
#include "AgentManager.h"
#include "Config.h"
#include "Console.h"
#include "GameLoop.h"
#include "GameThreadDispatcher.h"
#include "HTTPManager.h"
#include "Logger.h"
#include "Misc.h"
#include "QuestJournalFNV.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "TaskManager.h"
#include "TargetManager.h"
#include "TradeManager.h"
#include "WorldContextFNV.h"
#include "WorldDataSyncFNV.h"
#include "XNVSEAdapter.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ActionManager {

void UpdateNativeAttackStates();

namespace {

constexpr const char* kNearbyItemsPath = "Data\\NVSE\\Plugins\\dialectic_nearby_items.tmp";
constexpr const char* kNearbyPoiPath = "Data\\NVSE\\Plugins\\dialectic_nearby_pois.tmp";
constexpr const char* kNearbyFurniturePath = "Data\\NVSE\\Plugins\\dialectic_nearby_furniture.tmp";
constexpr int kInventoryOpenCooldownMs = 2500;
constexpr auto kActorInspectionTimeout = std::chrono::milliseconds(850);

std::atomic<uint64_t> g_requestCounter{0};

struct ActionRequest {
    std::string action;
    std::string speaker;
    std::string target;
    std::string item;
    std::string location;
    std::string instruction;
    int amount = 1;
    uint32_t speakerFormId = 0;
    uint32_t targetFormId = 0;
    uint32_t itemRefId = 0;
    uint32_t itemBaseId = 0;
    uint32_t locationFormId = 0;
    int itemInventoryIndex = -1;
    int itemInventoryCount = 0;
    int itemInventoryType = 0;
    uint64_t runtimeGeneration = 0;
    bool narratorAuthority = false;
};

struct NativePackageState {
    ActionRequest request;
    std::uint64_t generation{0};
    std::chrono::steady_clock::time_point startedAt{};
};

struct NativeAttackState {
    ActionRequest request;
    XNVSEAdapter::NativeCombatActorState speakerBefore;
    XNVSEAdapter::NativeCombatActorState targetBefore;
    std::uint64_t generation{0};
    std::chrono::steady_clock::time_point startedAt{};
    bool cleanupRequested{false};
    bool speakerRestored{false};
    bool targetRestored{false};
    std::string cleanupReason;
};

struct NativePickupState {
    ActionRequest request;
    std::uint64_t generation{0};
    std::chrono::steady_clock::time_point startedAt{};
};

std::mutex g_actionGateMutex;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_lastInventoryOpenByActor;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_lastActionBridgeRequestByKey;
std::mutex g_nativePackageMutex;
std::unordered_map<std::uint32_t, NativePackageState> g_nativePackageStates;
std::set<std::uint32_t> g_pendingNativeCleanupRefs;
std::mutex g_nativeAttackMutex;
std::unordered_map<std::uint32_t, NativeAttackState> g_nativeAttackStates;
std::mutex g_nativePickupMutex;
std::unordered_map<std::uint32_t, NativePickupState> g_nativePickupStates;
std::chrono::steady_clock::time_point g_lastManagerUpdate;

std::string Trim(std::string value) {
    const char* whitespace = " \t\r\n";
    const size_t start = value.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(whitespace);
    std::string trimmed = value.substr(start, end - start + 1);
    while (trimmed.size() >= 2) {
        const std::string suffix = trimmed.substr(trimmed.size() - 2);
        if (suffix != "\\n" && suffix != "\\r") {
            break;
        }
        trimmed.erase(trimmed.size() - 2);
        const size_t nextEnd = trimmed.find_last_not_of(whitespace);
        if (nextEnd == std::string::npos) {
            return "";
        }
        trimmed.erase(nextEnd + 1);
    }
    return trimmed;
}

std::string DecodeEscapedLineBreaks(std::string value) {
    size_t pos = 0;
    while ((pos = value.find("\\r", pos)) != std::string::npos) {
        value.replace(pos, 2, "\r");
        ++pos;
    }

    pos = 0;
    while ((pos = value.find("\\n", pos)) != std::string::npos) {
        value.replace(pos, 2, "\n");
        ++pos;
    }

    return value;
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string CompactActionKey(const std::string& value) {
    std::string key;
    for (char ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            key.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return key;
}

std::string ResolveCompactActionName(const std::string& actionName) {
    const std::string key = CompactActionKey(actionName);
    if (key.empty()) {
        return "";
    }

    static const std::vector<std::pair<const char*, const char*>> aliases = {
        {"attack", "Attack"},
        {"checkinventory", "CheckInventory"},
        {"listinventory", "CheckInventory"},
        {"comecloser", "ComeCloser"},
        {"consume", "Consume"},
        {"decreasewalkspeed", "DecreaseWalkSpeed"},
        {"endconversation", "EndConversation"},
        {"equip", "EquipItem"},
        {"equipitem", "EquipItem"},
        {"follow", "Follow"},
        {"followplayer", "FollowPlayer"},
        {"stopfollow", "StopFollowing"},
        {"stopfollowing", "StopFollowing"},
        {"stopfollowingplayer", "StopFollowing"},
        {"stopfollowingme", "StopFollowing"},
        {"dismissfollower", "StopFollowing"},
        {"leaveparty", "StopFollowing"},
        {"givecapsto", "GiveCapsTo"},
        {"givegoldto", "GiveCapsTo"},
        {"giveitemto", "GiveItemTo"},
        {"increasewalkspeed", "IncreaseWalkSpeed"},
        {"inspect", "Inspect"},
        {"inspectsurroundings", "InspectSurroundings"},
        {"makefollower", "MakeFollower"},
        {"joinplayerparty", "MakeFollower"},
        {"jointoplayersquad", "MakeFollower"},
        {"moveto", "MoveTo"},
        {"barter", "Barter"},
        {"showbartermenu", "Barter"},
        {"openinventory", "OpenInventory"},
        {"tradeitems", "OpenInventory"},
        {"exchangeitems", "OpenInventory"},
        {"openinventory2", "OpenInventory"},
        {"acceptgift", "OpenInventory"},
        {"pickupitem", "PickupItem"},
        {"readquests", "ReadQuests"},
        {"readquestjournal", "ReadQuests"},
        {"relax", "Relax"},
        {"relaxhere", "Relax"},
        {"directorcommand", "DirectorCommand"},
        {"spawncaps", "SpawnCaps"},
        {"spawngold", "SpawnCaps"},
        {"spawnitem", "SpawnItem"},
        {"teleportactor", "TeleportActor"},
        {"teleportnpc", "TeleportActor"},
        {"killtarget", "KillTarget"},
        {"sheatheweapon", "SheatheWeapon"},
        {"stopwalk", "StopWalk"},
        {"takeaseat", "TakeASeat"},
        {"takecapsfromplayer", "TakeCapsFromPlayer"},
        {"takegoldfromplayer", "TakeCapsFromPlayer"},
        {"travelto", "TravelTo"},
        {"traveltoraw", "TravelTo"},
        {"unequip", "UnequipItem"},
        {"unequipitem", "UnequipItem"},
        {"waithere", "WaitHere"},
        {"talk", "Talk"},
        {"justtalk", "Talk"},
    };

    for (const auto& alias : aliases) {
        if (key == alias.first) {
            return alias.second;
        }
    }

    std::string playerKey = CompactActionKey(Config::playerName);
    if (playerKey.empty()) {
        playerKey = CompactActionKey(Misc::GetPlayerName());
    }

    if (!playerKey.empty()) {
        if (key == "follow" + playerKey) {
            return "FollowPlayer";
        }
        if (key == "stopfollowing" + playerKey || key == "stopfollow" + playerKey ||
            key == "dismiss" + playerKey || key == "dismiss" + playerKey + "fromparty" ||
            key == "leave" + playerKey + "party") {
            return "StopFollowing";
        }
        if (key == "join" + playerKey + "party" || key == "jointo" + playerKey + "squad") {
            return "MakeFollower";
        }
        if (key == "takecapsfrom" + playerKey || key == "takegoldfrom" + playerKey) {
            return "TakeCapsFromPlayer";
        }
    }

    return "";
}

bool EqualsIgnoreCase(const std::string& left, const std::string& right) {
    return ToLower(left) == ToLower(right);
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(value);
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        parts.push_back(Trim(part));
    }
    return parts;
}

std::string ExtractJsonStringValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return "";
    }

    size_t colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) {
        return "";
    }

    size_t quotePos = json.find('"', colonPos + 1);
    if (quotePos == std::string::npos) {
        return "";
    }

    std::string value;
    bool escaped = false;
    for (size_t i = quotePos + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            switch (c) {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: value.push_back(c); break;
            }
            escaped = false;
            continue;
        }

        if (c == '\\') {
            escaped = true;
            continue;
        }

        if (c == '"') {
            break;
        }

        value.push_back(c);
    }

    return value;
}

std::string ExtractJsonNumberValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return "";
    }

    size_t colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) {
        return "";
    }

    size_t start = json.find_first_not_of(" \t\r\n", colonPos + 1);
    if (start == std::string::npos) {
        return "";
    }

    size_t end = start;
    while (end < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-' || json[end] == '+')) {
        ++end;
    }

    return Trim(json.substr(start, end - start));
}

std::vector<std::string> ExtractJsonStringArrayValue(const std::string& json, const std::string& key) {
    std::vector<std::string> values;
    const std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return values;
    }

    size_t colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) {
        return values;
    }

    size_t arrayStart = json.find('[', colonPos + 1);
    if (arrayStart == std::string::npos) {
        return values;
    }

    bool escaped = false;
    bool inString = false;
    std::string current;
    for (size_t i = arrayStart + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (!inString) {
            if (c == ']') {
                break;
            }
            if (c == '"') {
                inString = true;
                current.clear();
            }
            continue;
        }

        if (escaped) {
            switch (c) {
                case '"': current.push_back('"'); break;
                case '\\': current.push_back('\\'); break;
                case 'n': current.push_back('\n'); break;
                case 'r': current.push_back('\r'); break;
                case 't': current.push_back('\t'); break;
                default: current.push_back(c); break;
            }
            escaped = false;
            continue;
        }

        if (c == '\\') {
            escaped = true;
            continue;
        }

        if (c == '"') {
            values.push_back(Trim(current));
            inString = false;
            continue;
        }

        current.push_back(c);
    }

    return values;
}

uint32_t ParseHexFormId(const std::string& value) {
    std::string text = Trim(value);
    if (text.empty()) {
        return 0;
    }

    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) {
        text = text.substr(2);
    }

    std::string hexOnly;
    for (char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            hexOnly.push_back(c);
        } else {
            break;
        }
    }

    if (hexOnly.empty()) {
        return 0;
    }

    try {
        return static_cast<uint32_t>(std::stoul(hexOnly, nullptr, 16));
    } catch (...) {
        return 0;
    }
}

std::string RawHexFormId(uint32_t formId) {
    if (formId == 0) {
        return "";
    }

    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << formId;
    return stream.str();
}

uint32_t FormModIndex(uint32_t formId) {
    return (formId >> 24) & 0xFF;
}

uint32_t FormLocalIndex(uint32_t formId) {
    return formId & 0x00FFFFFF;
}

std::string NormalizeActionName(std::string actionName) {
    actionName = Trim(actionName);

    if (actionName == "GiveGoldTo") {
        return "GiveCapsTo";
    }
    if (actionName == "TakeGoldFromPlayer") {
        return "TakeCapsFromPlayer";
    }
    if (actionName == "ReadQuestJournal") {
        return "ReadQuests";
    }
    if (actionName == "TradeItems") {
        return "OpenInventory";
    }
    if (actionName == "AcceptGift") {
        return "OpenInventory";
    }
    if (actionName == "JustTalk") {
        return "Talk";
    }

    const std::string compactResolved = ResolveCompactActionName(actionName);
    if (!compactResolved.empty()) {
        return compactResolved;
    }

    return actionName;
}

const std::set<std::string>& CanonicalActions() {
    static const std::set<std::string> actions = {
        "Attack",
        "CheckInventory",
        "ComeCloser",
        "Consume",
        "DecreaseWalkSpeed",
        "EndConversation",
        "EquipItem",
        "Follow",
        "FollowPlayer",
        "GiveCapsTo",
        "GiveItemTo",
        "IncreaseWalkSpeed",
        "Inspect",
        "InspectSurroundings",
        "MakeFollower",
        "MoveTo",
        "Barter",
        "OpenInventory",
        "PickupItem",
        "ReadQuests",
        "Relax",
        "DirectorCommand",
        "SpawnCaps",
        "SpawnItem",
        "TeleportActor",
        "KillTarget",
        "SheatheWeapon",
        "StopFollowing",
        "StopWalk",
        "TakeASeat",
        "TakeCapsFromPlayer",
        "TravelTo",
        "UnequipItem",
        "WaitHere",
        "Talk",
    };
    return actions;
}

int ActionCodeForAction(const std::string& action) {
    if (action == "Attack") return 1;
    if (action == "OpenInventory") return 2;
    if (action == "Barter") return 3;
    if (action == "MakeFollower") return 4;
    if (action == "FollowPlayer") return 5;
    if (action == "ComeCloser") return 6;
    if (action == "MoveTo") return 7;
    if (action == "Follow") return 8;
    if (action == "GiveCapsTo") return 9;
    if (action == "TakeCapsFromPlayer") return 10;
    if (action == "GiveItemTo") return 11;
    if (action == "PickupItem") return 12;
    if (action == "Consume") return 13;
    if (action == "IncreaseWalkSpeed") return 14;
    if (action == "DecreaseWalkSpeed") return 15;
    if (action == "SheatheWeapon") return 16;
    if (action == "StopWalk") return 17;
    if (action == "WaitHere") return 18;
    if (action == "EndConversation") return 19;
    if (action == "TakeASeat") return 20;
    if (action == "TravelTo") return 21;
    if (action == "CheckInventory") return 22;
    if (action == "Inspect") return 23;
    if (action == "InspectSurroundings") return 24;
    if (action == "ReadQuests") return 25;
    if (action == "StopFollowing") return 26;
    if (action == "SpawnCaps") return 27;
    if (action == "SpawnItem") return 28;
    if (action == "TeleportActor") return 29;
    if (action == "KillTarget") return 30;
    if (action == "EquipItem") return 31;
    if (action == "UnequipItem") return 32;
    if (action == "Relax") return 33;
    return 0;
}

bool IsNarratorPluginAction(const std::string& action) {
    return action == "ReadQuests" || action == "SpawnCaps" ||
           action == "SpawnItem" || action == "TeleportActor" ||
           action == "KillTarget";
}

bool IsNarratorMutatingAction(const std::string& action) {
    return action == "SpawnCaps" || action == "SpawnItem" ||
           action == "TeleportActor" || action == "KillTarget";
}

bool IsPlayerTargetName(const std::string& name) {
    const std::string clean = ToLower(Trim(name));
    if (clean.empty()) {
        return false;
    }

    if (clean == "player" || clean == "#player_name#" || clean == "me" || clean == "courier") {
        return true;
    }

    const std::string configuredPlayer = ToLower(Trim(Config::playerName));
    if (!configuredPlayer.empty() && clean == configuredPlayer) {
        return true;
    }

    const std::string gamePlayer = ToLower(Trim(Misc::GetPlayerName()));
    return !gamePlayer.empty() && clean == gamePlayer;
}

uint32_t ResolveActorByName(const std::string& name, uint32_t avoidFormId = 0) {
    const std::string cleanName = Trim(name);
    if (cleanName.empty()) {
        return 0;
    }

    if (IsPlayerTargetName(cleanName)) {
        return Misc::GetPlayerFormId() != 0 ? Misc::GetPlayerFormId() : 0x00000014;
    }

    uint32_t formId = AgentManager::FindAgentFormIdByName(cleanName);
    if (formId != 0 && formId != avoidFormId) {
        return formId;
    }

    const auto& currentTarget = TargetManager::GetCurrentTarget();
    if (currentTarget.formId != 0 &&
        currentTarget.formId != avoidFormId &&
        currentTarget.isActor &&
        EqualsIgnoreCase(currentTarget.name, cleanName)) {
        return currentTarget.formId;
    }

    for (const auto& position : ActorPositionResolverFNV::GetRecentActorPositions()) {
        if (position.formId != 0 &&
            position.formId != avoidFormId &&
            EqualsIgnoreCase(position.actorName, cleanName)) {
            return position.formId;
        }
    }

    return 0;
}

uint32_t ResolveSpeakerFormId(const std::string& speaker) {
    uint32_t formId = ResolveActorByName(speaker);
    if (formId != 0) {
        return formId;
    }

    const auto& currentTarget = TargetManager::GetCurrentTarget();
    if (currentTarget.formId != 0 && currentTarget.isActor) {
        return currentTarget.formId;
    }

    return 0;
}

uint32_t ResolveTargetFormId(const std::string& target, uint32_t speakerFormId) {
    uint32_t formId = ResolveActorByName(target, speakerFormId);
    if (formId != 0) {
        return formId;
    }

    if (Trim(target).empty()) {
        return 0;
    }

    const auto& currentTarget = TargetManager::GetCurrentTarget();
    if (currentTarget.formId != 0 &&
        currentTarget.formId != speakerFormId &&
        currentTarget.isActor) {
        return currentTarget.formId;
    }

    return 0;
}

uint32_t ExtractLeadingFormId(const std::string& value) {
    std::string clean = Trim(value);
    if (clean.empty()) {
        return 0;
    }

    while (!clean.empty() && (clean.front() == '`' || clean.front() == '"' || clean.front() == '\'')) {
        clean.erase(clean.begin());
    }

    const size_t colonPos = clean.find(':');
    std::string candidate = colonPos == std::string::npos ? clean : clean.substr(0, colonPos);
    while (!candidate.empty() && (candidate.back() == '`' || candidate.back() == '"' || candidate.back() == '\'')) {
        candidate.pop_back();
    }
    return ParseHexFormId(candidate);
}

int NormalizeAmount(const std::string& amount, const std::string& fallback) {
    std::string value = Trim(amount);
    if (value.empty()) {
        value = Trim(fallback);
    }

    try {
        const int parsed = std::stoi(value);
        return parsed > 0 ? parsed : 1;
    } catch (...) {
        return 1;
    }
}

bool LooksLikeJsonObject(const std::string& value) {
    const std::string text = Trim(value);
    return text.size() >= 2 && text.front() == '{' && text.back() == '}';
}

uint32_t ParseActionFormId(const std::string& value) {
    std::string text = Trim(value);
    if (text.empty()) {
        return 0;
    }

    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0 || text.size() == 8) {
        return ParseHexFormId(text);
    }

    const bool decimalOnly = std::all_of(text.begin(), text.end(),
        [](unsigned char c) { return std::isdigit(c); });
    if (decimalOnly) {
        try {
            return static_cast<uint32_t>(std::stoul(text, nullptr, 10));
        } catch (...) {
            return 0;
        }
    }

    return ParseHexFormId(text);
}

bool ApplyJsonActionPayload(ActionRequest& request, const std::string& payload) {
    if (!LooksLikeJsonObject(payload)) {
        return false;
    }

    bool changed = false;
    std::string target = Trim(ExtractJsonStringValue(payload, "target"));
    const std::string location = Trim(ExtractJsonStringValue(payload, "location"));
    const std::string destination = Trim(ExtractJsonStringValue(payload, "destination"));
    if (request.action == "TravelTo" && target.empty()) {
        target = location.empty() ? destination : location;
    }
    if (!target.empty() && (request.target.empty() || request.action == "TravelTo")) {
        request.target = target;
        changed = true;
    }
    if (!location.empty() || !destination.empty()) {
        request.location = location.empty() ? destination : location;
        changed = true;
    }

    const std::string item = Trim(ExtractJsonStringValue(payload, "item"));
    if (!item.empty() && request.item.empty()) {
        request.item = item;
        changed = true;
    }

    const std::string amountNumber = ExtractJsonNumberValue(payload, "amount");
    const std::string amountString = ExtractJsonStringValue(payload, "amount");
    if (!Trim(amountNumber).empty() || !Trim(amountString).empty()) {
        const int amount = NormalizeAmount(amountNumber, amountString);
        if (amount > 0) {
            request.amount = amount;
            changed = true;
        }
    }

    uint32_t targetFormId = ParseActionFormId(ExtractJsonStringValue(payload, "target_refid"));
    if (targetFormId == 0) {
        targetFormId = ParseActionFormId(ExtractJsonStringValue(payload, "target_formid"));
    }
    if (targetFormId != 0 && request.targetFormId == 0) {
        request.targetFormId = targetFormId;
        changed = true;
    }

    const uint32_t locationFormId = ParseActionFormId(ExtractJsonStringValue(payload, "location_refid"));
    if (locationFormId != 0 && request.locationFormId == 0) {
        request.locationFormId = locationFormId;
        if (request.action == "TravelTo" && request.targetFormId == 0) {
            request.targetFormId = locationFormId;
        }
        changed = true;
    }

    uint32_t itemRefId = ParseActionFormId(ExtractJsonStringValue(payload, "item_refid"));
    if (itemRefId != 0 && request.itemRefId == 0) {
        request.itemRefId = itemRefId;
        changed = true;
    }

    uint32_t itemBaseId = ParseActionFormId(ExtractJsonStringValue(payload, "item_baseid"));
    if (itemBaseId == 0) {
        itemBaseId = ParseActionFormId(ExtractJsonStringValue(payload, "baseid"));
    }
    if (itemBaseId != 0 && request.itemBaseId == 0) {
        request.itemBaseId = itemBaseId;
        changed = true;
    }

    return changed;
}

bool ContainsIgnoreCase(const std::string& value, const std::string& needle) {
    const std::string haystack = ToLower(value);
    const std::string target = ToLower(Trim(needle));
    return !target.empty() && haystack.find(target) != std::string::npos;
}

float DistanceBetween(
    const ActorPositionResolverFNV::Vector3& a,
    const ActorPositionResolverFNV::Vector3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

std::string FormatRefId(uint32_t formId) {
    if (formId == 0) {
        return "";
    }

    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << formId;
    return stream.str();
}

std::string SanitizeFuncretSegment(std::string value) {
    value = Trim(value);
    for (char& ch : value) {
        if (ch == '@') {
            ch = ' ';
        } else if (ch == '|' || ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
    }

    std::string compact;
    compact.reserve(value.size());
    bool lastSpace = false;
    for (char ch : value) {
        const bool isSpace = std::isspace(static_cast<unsigned char>(ch)) != 0;
        if (isSpace) {
            if (!lastSpace) {
                compact.push_back(' ');
            }
            lastSpace = true;
        } else {
            compact.push_back(ch);
            lastSpace = false;
        }
    }
    return Trim(compact);
}

std::string ActionTargetLabel(const ActionRequest& request) {
    if (request.action == "InspectSurroundings") {
        return "surroundings";
    }
    if (request.action == "ReadQuests") {
        if (!request.target.empty()) {
            return request.target;
        }
        if (!request.item.empty()) {
            return request.item;
        }
        return "quests";
    }
    if (!request.target.empty()) {
        return request.target;
    }
    if (!request.item.empty()) {
        return request.item;
    }
    if (!request.speaker.empty()) {
        return request.speaker;
    }
    if (request.targetFormId != 0) {
        return FormatRefId(request.targetFormId);
    }
    if (request.speakerFormId != 0) {
        return FormatRefId(request.speakerFormId);
    }
    return "result";
}

void SendFuncretResult(const ActionRequest& request, const std::string& result) {
    const std::string target = SanitizeFuncretSegment(ActionTargetLabel(request));
    const std::string speaker = SanitizeFuncretSegment(request.speaker);
    const std::string cleanResult = SanitizeFuncretSegment(result);
    if (request.action.empty() || cleanResult.empty()) {
        return;
    }

    std::ostringstream speakerId;
    if (request.speakerFormId != 0) {
        speakerId << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
                  << request.speakerFormId << std::dec;
    }

    std::ostringstream payload;
    payload << "{"
            << "\"schema\":\"dialectic.action_result.v1\","
            << "\"action\":\"" << HTTPManager::EscapeJson(SanitizeFuncretSegment(request.action)) << "\","
            << "\"speaker\":\"" << HTTPManager::EscapeJson(speaker.empty() ? request.speaker : speaker) << "\",";
    if (request.speakerFormId != 0) {
        payload << "\"speaker_refid\":\"" << speakerId.str() << "\",";
    }
    payload
            << "\"target\":\"" << HTTPManager::EscapeJson(target.empty() ? "result" : target) << "\","
            << "\"result\":\"" << HTTPManager::EscapeJson(cleanResult) << "\""
            << "}";

    Logger::LogInfo("ActionManager: Sending funcret for %s speaker=[%s] target=[%s] result=[%s]",
        request.action.c_str(),
        request.speaker.c_str(),
        target.c_str(),
        cleanResult.size() > 180 ? cleanResult.substr(0, 180).c_str() : cleanResult.c_str());

    HTTPManager::SendEvent("funcret", payload.str());
}

void TrackNativePackageAction(const ActionRequest& request) {
    if (request.speakerFormId == 0) {
        return;
    }
    NativePackageState state;
    state.request = request;
    state.generation = RuntimeGeneration::Current();
    state.startedAt = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_nativePackageMutex);
    g_pendingNativeCleanupRefs.erase(request.speakerFormId);
    g_nativePackageStates[request.speakerFormId] = std::move(state);
}

void ClearNativePackageState(std::uint32_t speakerFormId) {
    std::lock_guard<std::mutex> lock(g_nativePackageMutex);
    g_nativePackageStates.erase(speakerFormId);
}

std::vector<std::uint32_t> ClearAllNativePackageStates() {
    std::vector<std::uint32_t> actors;
    std::lock_guard<std::mutex> lock(g_nativePackageMutex);
    actors.reserve(g_nativePackageStates.size());
    for (const auto& entry : g_nativePackageStates) {
        actors.push_back(entry.first);
    }
    g_nativePackageStates.clear();
    return actors;
}

void QueueNativePackageCleanup(std::uint32_t actorFormId) {
    if (actorFormId == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_nativePackageMutex);
    g_pendingNativeCleanupRefs.insert(actorFormId);
}

bool BeginNativeAttack(const ActionRequest& request) {
    if (request.speakerFormId == 0 || request.targetFormId == 0 ||
        request.speakerFormId == request.targetFormId) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_nativeAttackMutex);
        if (g_nativeAttackStates.find(request.speakerFormId) != g_nativeAttackStates.end()) {
            Logger::LogWarning("[NATIVE_ACTION] refused overlapping attack speaker=0x%08X",
                request.speakerFormId);
            return false;
        }
    }

    NativeAttackState state;
    state.request = request;
    state.generation = RuntimeGeneration::Current();
    state.startedAt = std::chrono::steady_clock::now();
    if (!XNVSEAdapter::CaptureNativeCombatActorState(request.speakerFormId, state.speakerBefore) ||
        !XNVSEAdapter::CaptureNativeCombatActorState(request.targetFormId, state.targetBefore)) {
        return false;
    }
    state.targetRestored = state.targetBefore.player;
    if (!XNVSEAdapter::ExecuteNativeAttack(request.speakerFormId, request.targetFormId)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_nativeAttackMutex);
    g_nativeAttackStates[request.speakerFormId] = std::move(state);
    return true;
}

void RequestNativeAttackCleanup(const char* reason) {
    std::lock_guard<std::mutex> lock(g_nativeAttackMutex);
    for (auto& entry : g_nativeAttackStates) {
        entry.second.cleanupRequested = true;
        entry.second.cleanupReason = reason ? reason : "cancelled";
    }
}

int NativeInventoryCount(std::uint32_t ownerFormId, std::uint32_t itemBaseFormId, bool& captured) {
    std::vector<XNVSEAdapter::NativeInventoryItem> items;
    captured = XNVSEAdapter::CaptureNativeInventory(ownerFormId, items);
    if (!captured) {
        return 0;
    }
    for (const auto& item : items) {
        if (item.baseFormId == itemBaseFormId) {
            return item.count;
        }
    }
    return 0;
}

bool NativeInventoryEquipped(std::uint32_t ownerFormId, std::uint32_t itemBaseFormId,
                             bool& captured) {
    std::vector<XNVSEAdapter::NativeInventoryItem> items;
    captured = XNVSEAdapter::CaptureNativeInventory(ownerFormId, items);
    if (!captured) {
        return false;
    }
    for (const auto& item : items) {
        if (item.baseFormId == itemBaseFormId) {
            return item.equipped;
        }
    }
    return false;
}

bool TryExecuteNativeInventoryAction(const ActionRequest& request, int actionCode, bool& handled) {
    handled = false;
    const std::uint32_t capsFormId = 0x0000000F;
    std::uint32_t sourceFormId = request.speakerFormId;
    std::uint32_t targetFormId = request.targetFormId;
    std::uint32_t itemBaseFormId = request.itemBaseId;
    if (actionCode == 9) {
        itemBaseFormId = capsFormId;
    } else if (actionCode == 10) {
        sourceFormId = Misc::GetPlayerFormId() != 0 ? Misc::GetPlayerFormId() : 0x00000014;
        targetFormId = request.speakerFormId;
        itemBaseFormId = capsFormId;
    }
    const bool equipmentAction = actionCode == 31 || actionCode == 32;
    if (sourceFormId == 0 || (actionCode != 13 && !equipmentAction && targetFormId == 0) ||
        itemBaseFormId == 0 || request.amount <= 0) {
        return false;
    }

    bool sourceCaptured = false;
    bool targetCaptured = actionCode == 13 || equipmentAction;
    const int sourceBefore = NativeInventoryCount(sourceFormId, itemBaseFormId, sourceCaptured);
    bool equippedBeforeCaptured = false;
    const bool equippedBefore = equipmentAction
        ? NativeInventoryEquipped(sourceFormId, itemBaseFormId, equippedBeforeCaptured)
        : false;
    int targetBefore = 0;
    if (actionCode != 13 && !equipmentAction) {
        targetBefore = NativeInventoryCount(targetFormId, itemBaseFormId, targetCaptured);
    }
    if (!sourceCaptured || !targetCaptured || (equipmentAction && !equippedBeforeCaptured)) {
        return false;
    }
    handled = true;
    if (sourceBefore < request.amount) {
        SendFuncretResult(request, request.action + " failed because item_not_in_source_inventory.");
        Logger::LogWarning("[NATIVE_ACTION] %s rejected source=0x%08X item=0x%08X count=%d amount=%d",
            request.action.c_str(), sourceFormId, itemBaseFormId, sourceBefore, request.amount);
        return false;
    }
    if (actionCode == 31 && equippedBefore) {
        SendFuncretResult(request, "EquipItem completed because the item was already equipped.");
        return true;
    }
    if (actionCode == 32 && !equippedBefore) {
        SendFuncretResult(request, "UnequipItem failed because item_not_equipped.");
        return false;
    }
    if (!XNVSEAdapter::ExecuteNativeInventoryAction(request.speakerFormId, targetFormId,
            itemBaseFormId, request.amount, actionCode)) {
        handled = false;
        return false;
    }

    if (equipmentAction) {
        bool equippedAfterCaptured = false;
        const bool equippedAfter = NativeInventoryEquipped(
            sourceFormId, itemBaseFormId, equippedAfterCaptured);
        const bool expectedEquipped = actionCode == 31;
        if (!equippedAfterCaptured || equippedAfter != expectedEquipped) {
            SendFuncretResult(request, request.action +
                " failed because equipment_state_not_observed.");
            Logger::LogWarning("[NATIVE_ACTION] %s state mismatch item=0x%08X equipped=%d->%d expected=%d",
                request.action.c_str(), itemBaseFormId, equippedBefore ? 1 : 0,
                equippedAfter ? 1 : 0, expectedEquipped ? 1 : 0);
            return false;
        }
        AgentManager::RequestActorSnapshot(request.speakerFormId, request.speaker);
    } else if (actionCode != 13) {
        bool sourceAfterCaptured = false;
        bool targetAfterCaptured = false;
        const int sourceAfter = NativeInventoryCount(sourceFormId, itemBaseFormId, sourceAfterCaptured);
        const int targetAfter = NativeInventoryCount(targetFormId, itemBaseFormId, targetAfterCaptured);
        if (!sourceAfterCaptured || !targetAfterCaptured ||
            sourceAfter > sourceBefore - request.amount ||
            targetAfter < targetBefore + request.amount) {
            SendFuncretResult(request, request.action + " failed because inventory_delta_not_observed.");
            Logger::LogWarning("[NATIVE_ACTION] %s delta mismatch item=0x%08X source=%d->%d target=%d->%d amount=%d",
                request.action.c_str(), itemBaseFormId, sourceBefore, sourceAfter,
                targetBefore, targetAfter, request.amount);
            return false;
        }
    }

    SendFuncretResult(request, request.action + " completed successfully.");
    Console::Print("[DIALECTIC] Action: %s", request.action.c_str());
    Logger::LogInfo("[NATIVE_ACTION] completed inventory action=%s speaker=0x%08X target=0x%08X item=0x%08X amount=%d without bridge transport",
        request.action.c_str(), request.speakerFormId, targetFormId, itemBaseFormId, request.amount);
    return true;
}

bool CompleteNativePickup(const ActionRequest& request) {
    bool beforeCaptured = false;
    const int before = NativeInventoryCount(request.speakerFormId, request.itemBaseId, beforeCaptured);
    if (!beforeCaptured || !XNVSEAdapter::TransferNativeWorldReferenceToActor(
            request.speakerFormId, request.itemRefId)) {
        return false;
    }
    bool afterCaptured = false;
    const int after = NativeInventoryCount(request.speakerFormId, request.itemBaseId, afterCaptured);
    if (!afterCaptured || after <= before) {
        SendFuncretResult(request, "PickupItem failed because inventory_delta_not_observed.");
        Logger::LogWarning("[NATIVE_ACTION] pickup delta mismatch speaker=0x%08X item_ref=0x%08X item_base=0x%08X count=%d->%d",
            request.speakerFormId, request.itemRefId, request.itemBaseId, before, after);
        return true;
    }
    SendFuncretResult(request, "PickupItem completed successfully.");
    Console::Print("[DIALECTIC] Action: PickupItem");
    Logger::LogInfo("[NATIVE_ACTION] pickup completed speaker=0x%08X item_ref=0x%08X item_base=0x%08X count=%d->%d without bridge transport",
        request.speakerFormId, request.itemRefId, request.itemBaseId, before, after);
    return true;
}

bool BeginNativePickup(const ActionRequest& request, bool& handled) {
    handled = false;
    RuntimeSnapshot::ActorState speaker;
    RuntimeSnapshot::ReferenceState item;
    if (!RuntimeSnapshot::TryGetActor(request.speakerFormId, speaker) ||
        !RuntimeSnapshot::TryGetReference(request.itemRefId, item) ||
        item.baseFormId != request.itemBaseId || item.deleted || item.taken || !item.loaded3D) {
        return false;
    }
    handled = true;
    const float dx = speaker.x - item.x;
    const float dy = speaker.y - item.y;
    const float dz = speaker.z - item.z;
    const float distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
    if (distance <= 128.0f) {
        if (!CompleteNativePickup(request)) {
            handled = false;
            return false;
        }
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(g_nativePickupMutex);
        if (g_nativePickupStates.find(request.speakerFormId) != g_nativePickupStates.end()) {
            SendFuncretResult(request, "PickupItem failed because actor_already_has_pending_pickup.");
            return false;
        }
    }
    if (!XNVSEAdapter::ExecuteNativePackageAction(request.speakerFormId, request.itemRefId, 7)) {
        handled = false;
        return false;
    }
    NativePickupState state;
    state.request = request;
    state.generation = RuntimeGeneration::Current();
    state.startedAt = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_nativePickupMutex);
        g_nativePickupStates[request.speakerFormId] = std::move(state);
    }
    Logger::LogInfo("[NATIVE_ACTION] pickup movement started speaker=0x%08X item_ref=0x%08X distance=%.1f",
        request.speakerFormId, request.itemRefId, distance);
    return true;
}

void CancelNativePickups(const char* reason) {
    std::vector<std::uint32_t> actors;
    {
        std::lock_guard<std::mutex> lock(g_nativePickupMutex);
        actors.reserve(g_nativePickupStates.size());
        for (const auto& entry : g_nativePickupStates) {
            actors.push_back(entry.first);
        }
        g_nativePickupStates.clear();
    }
    for (const std::uint32_t actorFormId : actors) {
        if (!XNVSEAdapter::HaltNativeActor(actorFormId)) {
            QueueNativePackageCleanup(actorFormId);
        }
    }
    if (!actors.empty()) {
        Logger::LogInfo("[NATIVE_ACTION] cancelled %zu native pickup(s) reason=%s",
            actors.size(), reason ? reason : "cancelled");
    }
}

std::vector<std::pair<uint32_t, std::string>> BuildHaltTargetSnapshot() {
    std::vector<std::pair<uint32_t, std::string>> targets;
    const TargetManager::TargetInfo& currentTarget = TargetManager::GetCurrentTarget();
    if (currentTarget.formId != 0 && currentTarget.isActor) {
        targets.emplace_back(currentTarget.formId, currentTarget.name);
    }

    const auto agents = AgentManager::GetRegisteredAgentSnapshot();
    targets.insert(targets.end(), agents.begin(), agents.end());
    {
        std::lock_guard<std::mutex> lock(g_nativePackageMutex);
        for (const auto& entry : g_nativePackageStates) {
            targets.emplace_back(entry.first, entry.second.request.speaker);
        }
    }
    std::sort(targets.begin(), targets.end(),
        [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
    targets.erase(std::unique(targets.begin(), targets.end(),
        [](const auto& left, const auto& right) {
            return left.first == right.first;
        }), targets.end());
    return targets;
}

bool ShouldGeneratePluginResult(const std::string& action) {
    return action == "CheckInventory" ||
           action == "Inspect" ||
           action == "InspectSurroundings" ||
           action == "ReadQuests";
}

struct NearbyItem {
    std::string refId;
    std::string baseId;
    std::string name;
    int type = 0;
    float distance = 0.0f;
    bool lookingAt = false;
    bool holding = false;
};

struct NearbyPoi {
    std::string refId;
    std::string baseId;
    std::string name;
    std::string destinationCellId;
    std::string destinationName;
    float distance = 0.0f;
    bool lookingAt = false;
    bool locked = false;
};

struct NearbyFurniture {
    std::string refId;
    std::string baseId;
    std::string name;
    float distance = 0.0f;
    bool lookingAt = false;
};

bool ParseBoolFlag(const std::string& value) {
    const std::string cleaned = ToLower(Trim(value));
    return cleaned == "1" || cleaned == "true" || cleaned == "yes";
}

std::vector<NearbyItem> ReadNearbyItems() {
    std::vector<NearbyItem> items;
    std::ifstream input(kNearbyItemsPath, std::ios::binary);
    if (!input.is_open()) {
        return items;
    }

    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        bool heldLine = false;
        if (line.rfind("held=", 0) == 0) {
            line = line.substr(5);
            heldLine = true;
        } else if (line.rfind("item=", 0) == 0) {
            line = line.substr(5);
        } else {
            continue;
        }

        const std::vector<std::string> parts = Split(line, '^');
        if (parts.size() < 6) {
            continue;
        }

        NearbyItem item;
        item.refId = parts[0];
        item.baseId = parts[1];
        item.name = parts[2];
        try {
            item.type = std::stoi(Trim(parts[3]));
        } catch (...) {
            item.type = 0;
        }
        try {
            item.distance = std::stof(parts[4]);
        } catch (...) {
            item.distance = 0.0f;
        }
        item.holding = heldLine;
        if (parts.size() >= 7) {
            item.lookingAt = ParseBoolFlag(parts[6]);
        }
        if (parts.size() >= 9) {
            item.holding = item.holding || ParseBoolFlag(parts[8]);
        }
        if (!item.refId.empty() && !item.name.empty()) {
            items.push_back(item);
        }
    }

    std::sort(items.begin(), items.end(), [](const NearbyItem& a, const NearbyItem& b) {
        if (a.holding != b.holding) {
            return a.holding;
        }
        if (a.lookingAt != b.lookingAt) {
            return a.lookingAt;
        }
        return a.distance < b.distance;
    });
    return items;
}

std::vector<NearbyPoi> ReadNearbyPois() {
    std::vector<NearbyPoi> pois;
    std::ifstream input(kNearbyPoiPath, std::ios::binary);
    if (!input.is_open()) {
        return pois;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();

    std::string line;
    std::istringstream lines(DecodeEscapedLineBreaks(buffer.str()));
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.rfind("poi=", 0) != 0) {
            continue;
        }

        const std::vector<std::string> parts = Split(line.substr(4), '^');
        if (parts.size() < 10) {
            continue;
        }

        NearbyPoi poi;
        poi.refId = parts[0];
        poi.baseId = parts[1];
        poi.name = parts[2];
        poi.destinationCellId = parts[3];
        poi.destinationName = parts[4];
        try {
            poi.distance = std::stof(parts[5]);
        } catch (...) {
            poi.distance = 0.0f;
        }
        poi.lookingAt = ParseBoolFlag(parts[8]);
        poi.locked = ParseBoolFlag(parts[9]);
        if (!poi.refId.empty()) {
            pois.push_back(poi);
        }
    }

    std::sort(pois.begin(), pois.end(), [](const NearbyPoi& a, const NearbyPoi& b) {
        if (a.lookingAt != b.lookingAt) {
            return a.lookingAt;
        }
        return a.distance < b.distance;
    });
    return pois;
}

std::vector<NearbyFurniture> ReadNearbyFurniture() {
    std::vector<NearbyFurniture> furniture;
    std::ifstream input(kNearbyFurniturePath, std::ios::binary);
    if (!input.is_open()) {
        return furniture;
    }

    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.rfind("furniture=", 0) != 0) {
            continue;
        }

        const std::vector<std::string> parts = Split(line.substr(10), '^');
        if (parts.size() < 6) {
            continue;
        }

        NearbyFurniture entry;
        entry.refId = parts[0];
        entry.baseId = parts[1];
        entry.name = parts[2];
        try {
            entry.distance = std::stof(parts[4]);
        } catch (...) {
            entry.distance = 0.0f;
        }
        entry.lookingAt = ParseBoolFlag(parts[5]);
        if (!entry.refId.empty() && !entry.name.empty()) {
            furniture.push_back(entry);
        }
    }

    std::sort(furniture.begin(), furniture.end(), [](const NearbyFurniture& a, const NearbyFurniture& b) {
        if (a.lookingAt != b.lookingAt) {
            return a.lookingAt;
        }
        return a.distance < b.distance;
    });
    return furniture;
}

std::string PoiDisplayName(const NearbyPoi& poi) {
    if (!poi.destinationName.empty() && !EqualsIgnoreCase(poi.destinationName, "Unknown")) {
        return poi.destinationName;
    }
    if (!poi.name.empty() && !EqualsIgnoreCase(poi.name, "Unknown")) {
        return poi.name;
    }
    return poi.refId;
}

bool ResolveNearbyFurniture(ActionRequest& request) {
    if (request.targetFormId != 0) {
        return true;
    }

    const std::vector<NearbyFurniture> furniture = ReadNearbyFurniture();
    if (furniture.empty()) {
        return false;
    }

    const std::string target = Trim(request.target);
    const NearbyFurniture* selected = nullptr;
    if (!target.empty()) {
        for (const auto& candidate : furniture) {
            if (ContainsIgnoreCase(candidate.name, target) || ContainsIgnoreCase(candidate.baseId, target)) {
                selected = &candidate;
                break;
            }
        }
    }
    if (!selected) {
        selected = &furniture.front();
    }

    request.targetFormId = ParseHexFormId(selected->refId);
    request.target = selected->name.empty() ? "nearby furniture" : selected->name;
    return request.targetFormId != 0;
}

bool ResolveNearbyItem(ActionRequest& request) {
    const std::vector<NearbyItem> items = ReadNearbyItems();
    if (items.empty()) {
        return request.itemRefId != 0 && request.itemBaseId != 0;
    }

    const std::string itemText = Trim(request.item.empty() ? request.target : request.item);
    const NearbyItem* selected = nullptr;
    if (request.itemRefId != 0) {
        for (const auto& candidate : items) {
            if (ParseHexFormId(candidate.refId) == request.itemRefId) {
                selected = &candidate;
                break;
            }
        }
    }
    if (!selected && !itemText.empty()) {
        for (const auto& candidate : items) {
            if (ContainsIgnoreCase(candidate.refId, itemText) ||
                ContainsIgnoreCase(candidate.baseId, itemText) ||
                ContainsIgnoreCase(candidate.name, itemText)) {
                selected = &candidate;
                break;
            }
        }
    }
    if (!selected) {
        return false;
    }

    request.itemRefId = ParseHexFormId(selected->refId);
    const uint32_t nearbyBaseId = ParseHexFormId(selected->baseId);
    if (nearbyBaseId != 0 && (request.itemBaseId == 0 || request.itemBaseId == request.itemRefId)) {
        request.itemBaseId = nearbyBaseId;
    }
    if (!selected->name.empty()) {
        request.item = selected->name;
    }
    return request.itemRefId != 0 && request.itemBaseId != 0;
}

bool ResolveNearbyPoi(ActionRequest& request) {
    if (request.targetFormId != 0) {
        return true;
    }

    const std::string target = Trim(request.target);
    const std::vector<NearbyPoi> pois = ReadNearbyPois();
    const NearbyPoi* selected = nullptr;
    if (!pois.empty() && !target.empty()) {
        for (const auto& candidate : pois) {
            if (ContainsIgnoreCase(candidate.name, target) ||
                ContainsIgnoreCase(candidate.destinationName, target) ||
                ContainsIgnoreCase(candidate.destinationCellId, target) ||
                ContainsIgnoreCase(candidate.baseId, target)) {
                selected = &candidate;
                break;
            }
        }
    }
    if (!selected && !target.empty()) {
        unsigned int worldLocationFormId = 0;
        char worldLocationName[256] = {};
        if (WorldDataSyncFNV::ResolveLocationByName(target.c_str(), worldLocationFormId, worldLocationName, sizeof(worldLocationName))) {
            request.targetFormId = worldLocationFormId;
            if (worldLocationName[0] != '\0') {
                request.target = worldLocationName;
            }
            Logger::LogInfo("ActionManager: TravelTo target [%s] resolved to world marker 0x%08X",
                target.c_str(),
                request.targetFormId);
            return request.targetFormId != 0;
        }
    }
    if (!selected) {
        if (!target.empty()) {
            Logger::LogWarning("ActionManager: TravelTo target [%s] did not match any nearby POI or world location", target.c_str());
            return false;
        }
        if (pois.empty()) {
            return false;
        }
        selected = &pois.front();
    }

    request.targetFormId = ParseHexFormId(selected->refId);
    request.target = PoiDisplayName(*selected);
    return request.targetFormId != 0;
}

AgentManager::NPCData CollectSnapshotForAction(uint32_t actorRefId, const std::string& nameHint,
                                               const TaskManager::CancellationToken* token = nullptr) {
    RuntimeSnapshot::ActorState nativeActor;
    if (actorRefId != 0 && !RuntimeSnapshot::TryGetActor(actorRefId, nativeActor)) {
        AgentManager::RequestActorSnapshot(actorRefId, nameHint);
        AgentManager::WaitForFreshActorSnapshot(actorRefId, 850,
            [token]() { return token && token->IsCancellationRequested(); });
    }

    AgentManager::NPCData data = AgentManager::CollectNPCData(actorRefId);
    if (data.displayName == "Unknown NPC" && !nameHint.empty()) {
        data.displayName = nameHint;
    }
    return data;
}

bool ResolveInventoryItemBase(ActionRequest& request, std::string& errorReason) {
    const bool equipmentAction = request.action == "EquipItem" || request.action == "UnequipItem";
    if (request.action != "GiveItemTo" && request.action != "Consume" && !equipmentAction) {
        return true;
    }

    const std::string itemText = Trim(request.item.empty() ? request.target : request.item);
    if (itemText.empty() && request.itemBaseId == 0) {
        errorReason = "missing_item";
        return false;
    }
    if (request.speakerFormId == 0) {
        errorReason = "missing_speaker";
        return false;
    }

    AgentManager::NPCData data = CollectSnapshotForAction(request.speakerFormId, request.speaker);
    const AgentManager::InventoryItem* selected = nullptr;
    int selectedIndex = -1;
    for (size_t index = 0; index < data.inventory.size(); ++index) {
        const auto& candidate = data.inventory[index];
        if (candidate.count <= 0 || candidate.baseid.empty()) {
            continue;
        }
        const uint32_t candidateBaseId = ParseHexFormId(candidate.baseid);
        const bool baseMatches = request.itemBaseId != 0 && candidateBaseId == request.itemBaseId;
        const bool itemMatches = !itemText.empty() &&
            (EqualsIgnoreCase(candidate.name, itemText) ||
             EqualsIgnoreCase(candidate.baseid, itemText) ||
             ContainsIgnoreCase(candidate.name, itemText) ||
             ContainsIgnoreCase(itemText, candidate.name) ||
             ContainsIgnoreCase(candidate.baseid, itemText));

        if (baseMatches || itemMatches) {
            selected = &candidate;
            selectedIndex = static_cast<int>(index);
            break;
        }
    }

    if (!selected) {
        errorReason = request.itemBaseId != 0 ? "item_not_in_speaker_inventory" : "item_not_found";
        return false;
    }

    request.itemBaseId = ParseHexFormId(selected->baseid);
    request.item = selected->name.empty() ? itemText : selected->name;
    request.itemInventoryIndex = selectedIndex;
    request.itemInventoryCount = selected->count;
    request.itemInventoryType = selected->type;

    if (equipmentAction && selected->type != 0x18 && selected->type != 0x28) {
        errorReason = "item_not_equippable";
        return false;
    }
    if (request.action == "UnequipItem" && !selected->equipped) {
        errorReason = "item_not_equipped";
        return false;
    }

    if (request.action == "GiveItemTo") {
        const std::string selectedName = selected->name.empty() ? itemText : selected->name;
        if (EqualsIgnoreCase(selectedName, "Magical Companion Ammo")) {
            errorReason = "companion_only_item";
            return false;
        }
    }

    if (request.amount <= 0) {
        request.amount = 1;
    }
    if (selected->count > 0 && request.amount > selected->count) {
        request.amount = selected->count;
    }
    if (request.itemBaseId == 0) {
        errorReason = "item_baseid_unresolved";
        return false;
    }
    Logger::LogInfo("ActionManager: Resolved %s item for %s: item=%s base=0x%08X count=%d type=%d inventoryIndex=%d",
        request.action.c_str(),
        request.speaker.c_str(),
        request.item.c_str(),
        request.itemBaseId,
        request.itemInventoryCount,
        request.itemInventoryType,
        request.itemInventoryIndex);
    return true;
}

bool ShouldSuppressInventoryOpen(const ActionRequest& request) {
    if (request.action != "OpenInventory" && request.action != "Barter") {
        return false;
    }

    const std::string key = FormatRefId(request.speakerFormId) + ":" + request.action;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_actionGateMutex);
    const auto existing = g_lastInventoryOpenByActor.find(key);
    if (existing != g_lastInventoryOpenByActor.end()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - existing->second).count();
        if (elapsed >= 0 && elapsed < kInventoryOpenCooldownMs) {
            Logger::LogInfo("ActionManager: Suppressed rapid %s reopen for %s after %lldms",
                request.action.c_str(),
                key.c_str(),
                static_cast<long long>(elapsed));
            return true;
        }
    }
    g_lastInventoryOpenByActor[key] = now;
    return false;
}

bool ShouldSuppressDuplicateBridgeAction(const ActionRequest& request, const char* source) {
    std::ostringstream key;
    key << request.action << ':'
        << RawHexFormId(request.speakerFormId) << ':'
        << RawHexFormId(request.targetFormId) << ':'
        << RawHexFormId(request.itemRefId) << ':'
        << RawHexFormId(request.itemBaseId) << ':'
        << request.target << ':'
        << request.item << ':'
        << request.amount;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_actionGateMutex);
    for (auto it = g_lastActionBridgeRequestByKey.begin(); it != g_lastActionBridgeRequestByKey.end();) {
        if (now - it->second > std::chrono::seconds(3)) {
            it = g_lastActionBridgeRequestByKey.erase(it);
        } else {
            ++it;
        }
    }

    const std::string actionKey = key.str();
    const auto existing = g_lastActionBridgeRequestByKey.find(actionKey);
    if (existing != g_lastActionBridgeRequestByKey.end() &&
        now - existing->second < std::chrono::milliseconds(1500)) {
        Logger::LogInfo("%s: Suppressed duplicate action bridge request action=%s speaker=0x%08X target=0x%08X",
            source ? source : "ActionManager",
            request.action.c_str(),
            request.speakerFormId,
            request.targetFormId);
        return true;
    }

    g_lastActionBridgeRequestByKey[actionKey] = now;
    return false;
}

std::string FormatEquipmentList(const AgentManager::NPCData& data, size_t maxItems = 12) {
    std::vector<std::string> parts;
    for (const auto& item : data.equipment) {
        if (item.name.empty()) {
            continue;
        }

        std::ostringstream part;
        part << (item.slotName.empty() ? "slot" : item.slotName) << ": " << item.name;
        if (!item.baseid.empty()) {
            part << " (" << item.baseid << ")";
        }
        parts.push_back(part.str());
        if (parts.size() >= maxItems) {
            break;
        }
    }

    const std::vector<std::pair<const char*, std::string>> fallback = {
        {"face", data.head},
        {"head", data.hair},
        {"body", data.upperBody},
        {"left hand", data.leftHand},
        {"right hand", data.rightHand},
        {"weapon", data.weapon},
        {"body addon 1", data.upperBodyAddon},
        {"body addon 2", data.lowerBodyAddon},
    };
    for (const auto& entry : fallback) {
        if (parts.size() >= maxItems) {
            break;
        }
        if (entry.second.empty()) {
            continue;
        }
        parts.push_back(std::string(entry.first) + ": " + entry.second);
    }

    if (parts.empty()) {
        return "none detected";
    }

    std::ostringstream joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            joined << "; ";
        }
        joined << parts[i];
    }
    return joined.str();
}

std::string FormatNativeEquipmentList(
    const std::vector<XNVSEAdapter::NativeEquipmentItem>& equipment,
    size_t maxItems = 12) {
    if (equipment.empty()) {
        return "none equipped";
    }

    std::ostringstream result;
    size_t emitted = 0;
    for (const auto& item : equipment) {
        if (item.name.empty()) {
            continue;
        }
        if (emitted > 0) {
            result << "; ";
        }
        result << item.name;
        ++emitted;
        if (emitted >= maxItems) {
            break;
        }
    }
    return emitted == 0 ? "none equipped" : result.str();
}

// Captures live race and equipment on the game thread without consulting fallback NPC stats.
bool CaptureActorInspectionForAction(
    uint32_t actorRef,
    uint64_t generation,
    XNVSEAdapter::NativeActorInspection& inspection,
    const TaskManager::CancellationToken* token) {
    struct CaptureResult {
        bool ok{false};
        XNVSEAdapter::NativeActorInspection inspection;
    };

    auto completion = std::make_shared<std::promise<CaptureResult>>();
    std::future<CaptureResult> future = completion->get_future();
    auto complete = [completion](CaptureResult result) {
        try {
            completion->set_value(std::move(result));
        } catch (...) {
        }
    };
    auto capture = [actorRef, complete]() {
        CaptureResult result;
        result.ok = XNVSEAdapter::CaptureNativeActorInspection(actorRef, result.inspection);
        complete(std::move(result));
    };

    if (GameThreadDispatcher::IsGameThread()) {
        capture();
    } else {
        const std::string key = "actor_inspection:" + std::to_string(actorRef);
        const bool queued = GameThreadDispatcher::Enqueue(
            "actor_inspection", key, generation, std::move(capture),
            [complete](const char*) { complete({}); });
        if (!queued) {
            complete({});
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + kActorInspectionTimeout;
    while (future.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready) {
        if ((token && token->IsCancellationRequested()) ||
            std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }

    CaptureResult captured = future.get();
    if (!captured.ok) {
        return false;
    }
    inspection = std::move(captured.inspection);
    return true;
}

std::string BuildInventoryResult(const ActionRequest& request, const TaskManager::CancellationToken* token = nullptr) {
    const uint32_t actorRef = request.speakerFormId != 0 ? request.speakerFormId : request.targetFormId;
    AgentManager::NPCData data = CollectSnapshotForAction(actorRef, request.speaker, token);
    const std::string actorName = !data.displayName.empty() ? data.displayName : (!request.speaker.empty() ? request.speaker : "actor");
    const std::string filter = !request.item.empty() ? request.item : request.target;

    if (data.inventory.empty()) {
        std::ostringstream result;
        result << actorName << " has no inventory items in the current snapshot.";
        const std::string equipment = FormatEquipmentList(data, 8);
        if (equipment != "none detected") {
            result << " Equipped: " << equipment << ".";
        }
        return result.str();
    }

    std::ostringstream result;
    result << "Inventory for " << actorName << ": ";
    size_t emitted = 0;
    bool anyMatch = false;
    for (const auto& item : data.inventory) {
        if (!filter.empty() &&
            !ContainsIgnoreCase(item.name, filter) &&
            !ContainsIgnoreCase(item.baseid, filter)) {
            continue;
        }

        anyMatch = true;
        if (emitted > 0) {
            result << "; ";
        }
        result << item.name << " x" << item.count;
        if (!item.baseid.empty()) {
            result << " (" << item.baseid << ")";
        }
        if (item.equipped) {
            result << " equipped";
        }
        if (item.condition >= 0.0f) {
            result << " condition " << std::fixed << std::setprecision(0) << item.condition;
        }
        if (!item.ammo.empty()) {
            result << " ammo " << item.ammo;
        }
        if (!item.mods.empty()) {
            result << " mods ";
            for (size_t i = 0; i < item.mods.size(); ++i) {
                if (i > 0) {
                    result << ",";
                }
                result << item.mods[i];
            }
        }
        ++emitted;
        if (emitted >= 24) {
            result << "; more items omitted";
            break;
        }
    }

    if (!filter.empty() && !anyMatch) {
        result << "no matching item found for " << filter << ".";
    }
    return result.str();
}

std::string BuildInspectResult(const ActionRequest& request, const TaskManager::CancellationToken* token = nullptr) {
    const uint32_t actorRef = request.targetFormId != 0 ? request.targetFormId : request.speakerFormId;
    const std::string hint = !request.target.empty() ? request.target : request.speaker;
    const std::string fallbackName = hint.empty() ? "actor" : hint;
    if (actorRef == 0) {
        return "There is no one here to inspect.";
    }

    XNVSEAdapter::NativeActorInspection inspection;
    if (!CaptureActorInspectionForAction(actorRef, request.runtimeGeneration, inspection, token)) {
        Logger::LogWarning("ActionManager: Inspect could not capture live race/equipment for 0x%08X", actorRef);
        return "You cannot get a clear enough look at " + fallbackName +
            " to identify their race or equipment.";
    }

    const std::string actorName = inspection.name.empty() ? fallbackName : inspection.name;
    Logger::LogInfo("ActionManager: Inspect captured live actor 0x%08X race=%s equipment=%zu",
        inspection.formId,
        inspection.raceName.empty() ? "unavailable" : inspection.raceName.c_str(),
        inspection.equipment.size());

    std::ostringstream result;
    result << actorName << ". Race: "
           << (inspection.raceName.empty() ? "unavailable" : inspection.raceName)
           << ". Equipment: " << FormatNativeEquipmentList(inspection.equipment) << ".";
    return result.str();
}

std::string BuildSurroundingsResult() {
    std::ostringstream result;
    const WorldContextFNV::Context world = WorldContextFNV::GetCurrent();
    if (!world.location.empty()) {
        result << "Location: " << world.location << ". ";
    }

    const auto player = ActorPositionResolverFNV::ResolvePlayer();
    std::vector<ActorPositionResolverFNV::PositionResult> actors = ActorPositionResolverFNV::GetRecentActorPositions();
    std::sort(actors.begin(), actors.end(), [&player](const auto& a, const auto& b) {
        if (player.resolved && a.resolved && b.resolved) {
            return DistanceBetween(player.position, a.position) < DistanceBetween(player.position, b.position);
        }
        return a.actorName < b.actorName;
    });

    size_t actorCount = 0;
    result << "Nearby actors: ";
    for (const auto& actor : actors) {
        if (!actor.resolved || actor.formId == 0 || actor.formId == 0x00000014 || actor.actorName.empty()) {
            continue;
        }
        if (actorCount > 0) {
            result << "; ";
        }
        result << actor.actorName << " " << FormatRefId(actor.formId);
        if (player.resolved) {
            result << " " << std::fixed << std::setprecision(0) << DistanceBetween(player.position, actor.position) << " units away";
        }
        if (!actor.race.empty()) {
            result << " race " << actor.race;
        }
        ++actorCount;
        if (actorCount >= 8) {
            break;
        }
    }
    if (actorCount == 0) {
        result << "none detected";
    }

    const std::vector<NearbyItem> items = ReadNearbyItems();
    result << ". Nearby items: ";
    for (size_t i = 0; i < items.size() && i < 8; ++i) {
        if (i > 0) {
            result << "; ";
        }
        result << items[i].name << " " << items[i].refId << " " << std::fixed << std::setprecision(0) << items[i].distance << " units";
        if (items[i].holding) {
            result << " held";
        } else if (items[i].lookingAt) {
            result << " looked at";
        }
    }
    if (items.empty()) {
        result << "none detected";
    }

    const std::vector<NearbyPoi> pois = ReadNearbyPois();
    result << ". Points of interest: ";
    for (size_t i = 0; i < pois.size() && i < 6; ++i) {
        if (i > 0) {
            result << "; ";
        }
        result << PoiDisplayName(pois[i]) << " " << pois[i].refId << " " << std::fixed << std::setprecision(0) << pois[i].distance << " units";
        if (pois[i].locked) {
            result << " locked";
        }
    }
    if (pois.empty()) {
        result << "none detected";
    }

    const std::vector<NearbyFurniture> furniture = ReadNearbyFurniture();
    result << ". Nearby furniture: ";
    for (size_t i = 0; i < furniture.size() && i < 5; ++i) {
        if (i > 0) {
            result << "; ";
        }
        result << furniture[i].name << " " << furniture[i].refId << " " << std::fixed << std::setprecision(0) << furniture[i].distance << " units";
    }
    if (furniture.empty()) {
        result << "none detected";
    }
    result << ".";
    return result.str();
}

std::string BuildQuestResult(const ActionRequest& request) {
    const std::string filter = !request.item.empty() ? request.item : request.target;
    return QuestJournalFNV::BuildCurrentQuestResult(filter);
}

std::string BuildPluginResult(const ActionRequest& request, const TaskManager::CancellationToken& token) {
    if (request.action == "CheckInventory") {
        return BuildInventoryResult(request, &token);
    }
    if (request.action == "Inspect") {
        return BuildInspectResult(request, &token);
    }
    if (request.action == "InspectSurroundings") {
        return BuildSurroundingsResult();
    }
    if (request.action == "ReadQuests") {
        return BuildQuestResult(request);
    }
    return "";
}

void LaunchPluginResultWorker(ActionRequest request) {
    TaskManager::EnqueueScoped("action_result", request.action, request.runtimeGeneration, true,
        std::chrono::seconds(30), {request.speakerFormId, request.runtimeGeneration},
        [request](const TaskManager::CancellationToken& token) {
        if (token.IsCancellationRequested()) return;
        const std::string result = BuildPluginResult(request, token);
        if (token.IsCancellationRequested()) return;
        SendFuncretResult(request, result.empty() ? "No result was available." : result);
    });
}

std::string PlayerDisplayNameForAction() {
    std::string player = Trim(Misc::GetPlayerName());
    if (!player.empty()) {
        return player;
    }
    player = Trim(Config::playerName);
    return player.empty() ? "player" : player;
}

std::string TrimInstructionPhrase(std::string value) {
    value = Trim(value);
    while (!value.empty()) {
        const char ch = value.front();
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '#') {
            break;
        }
        value.erase(value.begin());
    }
    while (!value.empty()) {
        const char ch = value.back();
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '#') {
            break;
        }
        value.pop_back();
    }
    return Trim(value);
}

std::string StripInstructionActionHint(const std::string& instruction) {
    const std::string lower = ToLower(instruction);
    size_t pos = lower.find("(must use action");
    if (pos == std::string::npos) {
        pos = lower.find("must use action");
    }
    if (pos == std::string::npos) {
        return Trim(instruction);
    }
    return TrimInstructionPhrase(instruction.substr(0, pos));
}

std::string ResolveCanonicalActionPhrase(const std::string& actionPhrase) {
    const std::string fullCandidate = NormalizeActionName(actionPhrase);
    if (CanonicalActions().contains(fullCandidate)) {
        return fullCandidate;
    }

    std::istringstream stream(actionPhrase);
    std::vector<std::string> words;
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }

    while (words.size() > 1) {
        words.pop_back();
        std::ostringstream prefix;
        for (size_t i = 0; i < words.size(); ++i) {
            if (i > 0) {
                prefix << ' ';
            }
            prefix << words[i];
        }

        const std::string candidate = NormalizeActionName(prefix.str());
        if (CanonicalActions().contains(candidate)) {
            return candidate;
        }
    }

    return "";
}

std::string ExtractRolemasterRequiredAction(const std::string& instruction) {
    const std::string lower = ToLower(instruction);
    const std::vector<std::string> markers = {
        "must use action",
        "use action",
        "action:"
    };

    for (const auto& marker : markers) {
        const size_t markerPos = lower.find(marker);
        if (markerPos == std::string::npos) {
            continue;
        }

        size_t start = markerPos + marker.size();
        while (start < instruction.size()) {
            const unsigned char ch = static_cast<unsigned char>(instruction[start]);
            if (std::isalnum(ch) || instruction[start] == '_') {
                break;
            }
            ++start;
        }

        size_t end = start;
        while (end < instruction.size()) {
            const char ch = instruction[end];
            if (ch == ')' || ch == '(' || ch == '.' || ch == ',' || ch == ';' ||
                ch == '@' || ch == '\r' || ch == '\n') {
                break;
            }
            ++end;
        }

        const std::string candidate = ResolveCanonicalActionPhrase(instruction.substr(start, end - start));
        if (!candidate.empty()) {
            return candidate;
        }
    }

    return "";
}

int ExtractFirstPositiveInteger(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            continue;
        }

        size_t end = i;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) {
            ++end;
        }

        try {
            const int value = std::stoi(text.substr(i, end - i));
            if (value > 0) {
                return value;
            }
        } catch (...) {
            return 1;
        }
    }
    return 1;
}

std::string ExtractPhraseAfterAny(const std::string& text, const std::vector<std::string>& markers) {
    const std::string lower = ToLower(text);
    for (const auto& marker : markers) {
        const size_t pos = lower.find(ToLower(marker));
        if (pos == std::string::npos) {
            continue;
        }

        std::string phrase = text.substr(pos + marker.size());
        const std::string phraseLower = ToLower(phrase);
        size_t cut = phraseLower.find(" and ");
        if (cut == std::string::npos) {
            cut = phraseLower.find(" then ");
        }
        if (cut == std::string::npos) {
            cut = phraseLower.find(" while ");
        }
        if (cut != std::string::npos) {
            phrase = phrase.substr(0, cut);
        }
        return TrimInstructionPhrase(phrase);
    }
    return "";
}

std::string ExtractPhraseBetween(const std::string& text, const std::string& startMarker, const std::string& endMarker) {
    const std::string lower = ToLower(text);
    const size_t startPos = lower.find(ToLower(startMarker));
    if (startPos == std::string::npos) {
        return "";
    }

    const size_t phraseStart = startPos + startMarker.size();
    const size_t endPos = lower.find(ToLower(endMarker), phraseStart);
    if (endPos == std::string::npos || endPos <= phraseStart) {
        return "";
    }

    return TrimInstructionPhrase(text.substr(phraseStart, endPos - phraseStart));
}

std::string StripLeadingAmount(std::string value) {
    value = TrimInstructionPhrase(value);
    while (!value.empty() && std::isdigit(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    return TrimInstructionPhrase(value);
}

bool MentionsPlayer(const std::string& instruction) {
    if (ContainsIgnoreCase(instruction, "#PLAYER#") ||
        ContainsIgnoreCase(instruction, "#PLAYER_NAME#") ||
        ContainsIgnoreCase(instruction, " player") ||
        ContainsIgnoreCase(instruction, " courier") ||
        ContainsIgnoreCase(instruction, " me")) {
        return true;
    }

    const std::string configured = Trim(Config::playerName);
    if (!configured.empty() && ContainsIgnoreCase(instruction, configured)) {
        return true;
    }

    const std::string gamePlayer = Trim(Misc::GetPlayerName());
    return !gamePlayer.empty() && ContainsIgnoreCase(instruction, gamePlayer);
}

std::string InferActorTargetFromInstruction(const std::string& instruction, const std::string& speaker) {
    if (MentionsPlayer(instruction)) {
        return PlayerDisplayNameForAction();
    }

    const auto& currentTarget = TargetManager::GetCurrentTarget();
    if (currentTarget.isActor &&
        currentTarget.formId != 0 &&
        !currentTarget.name.empty() &&
        !EqualsIgnoreCase(currentTarget.name, speaker) &&
        ContainsIgnoreCase(instruction, currentTarget.name)) {
        return currentTarget.name;
    }

    for (const auto& position : ActorPositionResolverFNV::GetRecentActorPositions()) {
        if (position.formId == 0 ||
            position.actorName.empty() ||
            EqualsIgnoreCase(position.actorName, speaker)) {
            continue;
        }
        if (ContainsIgnoreCase(instruction, position.actorName)) {
            return position.actorName;
        }
    }

    return "";
}

std::string InferItemFromInstruction(const std::string& action, const std::string& instruction) {
    const std::vector<NearbyItem> nearbyItems = ReadNearbyItems();
    for (const auto& item : nearbyItems) {
        if (!item.name.empty() && ContainsIgnoreCase(instruction, item.name)) {
            return item.name;
        }
    }

    if (action == "GiveItemTo") {
        std::string item = ExtractPhraseBetween(instruction, "give ", " to ");
        if (item.empty()) {
            item = ExtractPhraseBetween(instruction, "hand ", " to ");
        }
        return StripLeadingAmount(item);
    }

    if (action == "PickupItem") {
        return StripLeadingAmount(ExtractPhraseAfterAny(instruction, {"pick up ", "pickup ", "grab ", "take "}));
    }

    if (action == "Consume") {
        return StripLeadingAmount(ExtractPhraseAfterAny(instruction, {"consume ", "eat ", "drink ", "use "}));
    }

    return "";
}

std::string InferTargetFromInstruction(const std::string& action, const std::string& instruction, const std::string& speaker) {
    if (action == "FollowPlayer" || action == "ComeCloser" ||
        action == "MakeFollower" || action == "TakeCapsFromPlayer") {
        return PlayerDisplayNameForAction();
    }

    const std::string actorTarget = InferActorTargetFromInstruction(instruction, speaker);
    if (!actorTarget.empty()) {
        return actorTarget;
    }

    if (action == "MoveTo") {
        return ExtractPhraseAfterAny(instruction, {"move to ", "go to ", "walk to ", "come to "});
    }

    if (action == "TravelTo") {
        return ExtractPhraseAfterAny(instruction, {"travel to ", "go to ", "head to ", "leave for "});
    }

    if (action == "TakeASeat") {
        return ExtractPhraseAfterAny(instruction, {"sit on ", "sit at ", "sit in ", "take a seat at "});
    }

    if (action == "GiveCapsTo" || action == "GiveItemTo") {
        return MentionsPlayer(instruction) ? PlayerDisplayNameForAction() : "";
    }

    return "";
}

bool TranslateRolemasterInstruction(ActionRequest& request, const std::vector<std::string>& args, const char* source) {
    if (args.size() < 2) {
        Logger::LogInfo("%s: Ignoring rolemaster instruction with %zu args",
            source ? source : "ActionManager",
            args.size());
        return false;
    }

    const std::string characterName = Trim(args[0]);
    const std::string instruction = StripInstructionActionHint(args[1]);
    const std::string rawInstruction = Trim(args[1]);
    const std::string action = ExtractRolemasterRequiredAction(rawInstruction);
    if (action.empty()) {
        Logger::LogInfo("%s: Ignoring rolemaster instruction without supported ACTION hint: [%s]",
            source ? source : "ActionManager",
            rawInstruction.c_str());
        return false;
    }

    request.action = action;
    request.instruction = instruction;
    if (!characterName.empty()) {
        request.speaker = characterName;
    }

    if (request.target.empty()) {
        request.target = InferTargetFromInstruction(action, instruction, request.speaker);
    }

    if (request.item.empty()) {
        request.item = InferItemFromInstruction(action, instruction);
    }

    if (request.amount <= 1) {
        request.amount = ExtractFirstPositiveInteger(instruction);
    }

    Logger::LogInfo("%s: Translated rolemaster instruction actor=[%s] action=%s target=[%s] item=[%s] amount=%d text=[%s]",
        source ? source : "ActionManager",
        request.speaker.c_str(),
        request.action.c_str(),
        request.target.c_str(),
        request.item.c_str(),
        request.amount,
        instruction.c_str());
    return true;
}

bool SendDirectorTalkInstruction(const ActionRequest& request, const char* source) {
    const std::string speaker = Trim(request.speaker);
    const std::string instruction = Trim(request.instruction.empty() ? request.target : request.instruction);
    if (speaker.empty() || instruction.empty()) {
        Logger::LogWarning("%s: Director Talk instruction missing speaker/text speaker=[%s] text=[%s]",
            source ? source : "ActionManager",
            speaker.c_str(),
            instruction.c_str());
        Console::Print("[DIALECTIC] Director talk instruction failed");
        return false;
    }

    const std::string playerName = PlayerDisplayNameForAction();
    std::ostringstream speakerId;
    speakerId << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
              << request.speakerFormId << std::dec;

    std::ostringstream payload;
    payload << "{"
            << "\"schema\":\"dialectic.input.v1\","
            << "\"npc\":\"" << HTTPManager::EscapeJson(speaker) << "\","
            << "\"npc_id\":\"" << speakerId.str() << "\","
            << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\","
            << "\"text\":\"" << HTTPManager::EscapeJson(instruction) << "\","
            << "\"skip_player_tts\":true,"
            << "\"dialectic_mode\":\"STANDARD\","
            << "\"mode\":\"STANDARD\","
            << "\"director_instruction\":true,"
            << "\"target\":{\"name\":\"" << HTTPManager::EscapeJson(speaker) << "\",\"refid\":\"" << speakerId.str() << "\"}";
    if (!request.target.empty()) {
        payload << ",\"listener\":\"" << HTTPManager::EscapeJson(request.target) << "\""
                << ",\"director_target\":\"" << HTTPManager::EscapeJson(request.target) << "\"";
    }
    payload << ",\"game\":\"fnv\""
            << "}";

    Logger::LogInfo("%s: Routing Director Talk instruction to inputtext speaker=[%s] target=[%s] text=[%s]",
        source ? source : "ActionManager",
        speaker.c_str(),
        request.target.c_str(),
        instruction.c_str());
    Console::Print("[DIALECTIC] Director: %s", speaker.c_str());
    HTTPManager::SendEvent("inputtext", payload.str());
    return true;
}

void ApplyStructuredCommandArgs(ActionRequest& request, const std::vector<std::string>& args) {
    if (!args.empty() && ApplyJsonActionPayload(request, args[0])) {
        return;
    }

    if (request.action == "Attack" || request.action == "Follow" || request.action == "MoveTo" ||
        request.action == "Inspect" || request.action == "TravelTo") {
        if (!args.empty() && request.target.empty()) {
            const uint32_t rawTargetFormId = request.action == "TravelTo" ? ParseActionFormId(args[0]) : 0;
            if (rawTargetFormId != 0) {
                request.targetFormId = rawTargetFormId;
                if (args.size() >= 2) {
                    request.target = args[1];
                }
            } else {
                request.target = args[0];
            }
        }
        return;
    }

    if (request.action == "GiveCapsTo") {
        if (!args.empty() && request.target.empty()) {
            request.target = args[0];
        }
        if (args.size() >= 2) {
            request.amount = NormalizeAmount(args[1], "");
        }
        return;
    }

    if (request.action == "TakeCapsFromPlayer") {
        if (!args.empty()) {
            request.amount = NormalizeAmount(args[0], "");
        }
        return;
    }

    if (request.action == "GiveItemTo") {
        if (!args.empty() && request.target.empty()) {
            request.target = args[0];
        }
        if (args.size() >= 2 && request.item.empty()) {
            request.item = args[1];
        }
        if (args.size() >= 3) {
            request.amount = NormalizeAmount(args[2], "");
        }
        return;
    }

    if (request.action == "PickupItem" || request.action == "Consume") {
        if (!args.empty() && request.item.empty()) {
            request.item = args[0];
        }
        return;
    }

    if (request.action == "SpawnCaps") {
        if (!args.empty() && request.target.empty()) {
            request.target = args[0];
        }
        if (args.size() >= 2) {
            request.amount = NormalizeAmount(args[1], "");
        }
        return;
    }

    if (request.action == "SpawnItem") {
        if (!args.empty() && request.target.empty()) {
            request.target = args[0];
        }
        if (args.size() >= 2 && request.item.empty()) {
            request.item = args[1];
        }
        if (args.size() >= 3) {
            request.amount = NormalizeAmount(args[2], "");
        }
        return;
    }

    if (request.action == "TeleportActor") {
        if (!args.empty() && request.target.empty()) {
            request.target = args[0];
        }
        if (args.size() >= 2 && request.location.empty()) {
            request.location = args[1];
        }
        return;
    }

    if (request.action == "KillTarget") {
        if (!args.empty() && request.target.empty()) {
            request.target = args[0];
        }
        return;
    }

    if (!args.empty() && request.target.empty()) {
        request.target = args[0];
    }
    if (args.size() >= 2 && request.item.empty()) {
        request.item = args[1];
    }
    if (args.size() >= 3) {
        request.amount = NormalizeAmount(args[2], "");
    }
}

bool BuildActionRequestFromRoleCommandJson(const std::string& lineObject,
                                           const char* source,
                                           ActionRequest& request,
                                           bool logUnsupported = true) {
    std::string actionField = Trim(ExtractJsonStringValue(lineObject, "action"));
    std::string commandName = Trim(ExtractJsonStringValue(lineObject, "command_name"));
    std::string command = commandName;
    if (command.empty()) {
        command = Trim(ExtractJsonStringValue(lineObject, "message"));
    }
    if (command.empty()) {
        command = Trim(ExtractJsonStringValue(lineObject, "text"));
    }
    if (command.empty()) {
        command = Trim(ExtractJsonStringValue(lineObject, "command"));
    }
    if (command.empty()) {
        command = Trim(ExtractJsonStringValue(lineObject, "action_code"));
    }

    request = ActionRequest{};
    request.speaker = Trim(ExtractJsonStringValue(lineObject, "speaker"));
    if (request.speaker.empty()) {
        request.speaker = Trim(ExtractJsonStringValue(lineObject, "character"));
    }
    request.target = Trim(ExtractJsonStringValue(lineObject, "target"));
    request.item = Trim(ExtractJsonStringValue(lineObject, "item"));
    request.location = Trim(ExtractJsonStringValue(lineObject, "location"));
    request.amount = NormalizeAmount(
        ExtractJsonNumberValue(lineObject, "amount"),
        ExtractJsonStringValue(lineObject, "amount"));
    const std::string actionSource = Trim(ExtractJsonStringValue(lineObject, "action_source"));
    const std::string authority = Trim(ExtractJsonStringValue(lineObject, "authority"));

    const std::vector<std::string> commandArgs = ExtractJsonStringArrayValue(lineObject, "command_args");
    bool translatedRolemasterInstruction = false;
    if (IsActionCommand(actionField) && !EqualsIgnoreCase(actionField, "rolecommand")) {
        request.action = NormalizeActionName(actionField);
    } else {
        request.action = NormalizeActionName(command);
    }

    if ((request.action == "Instruction" || request.action == "Suggestion") &&
        !commandArgs.empty()) {
        translatedRolemasterInstruction = TranslateRolemasterInstruction(request, commandArgs, source);
        if (!translatedRolemasterInstruction) {
            return false;
        }
    }

    if (!commandArgs.empty() && !translatedRolemasterInstruction) {
        ApplyStructuredCommandArgs(request, commandArgs);
    }

    if (request.action.empty() || !IsActionCommand(request.action)) {
        if (logUnsupported) {
            Logger::LogInfo("%s: Ignoring unsupported rolecommand action: [%s]",
                source ? source : "ActionManager",
                request.action.empty() ? command.c_str() : request.action.c_str());
        }
        return false;
    }

    request.narratorAuthority =
        EqualsIgnoreCase(actionSource, "narrator") &&
        EqualsIgnoreCase(authority, "narrator") &&
        EqualsIgnoreCase(request.speaker, "The Narrator") &&
        IsNarratorPluginAction(request.action);

    if (request.action == "TravelTo" && request.target.empty()) {
        request.target = Trim(ExtractJsonStringValue(lineObject, "location"));
    }
    if (request.action == "TeleportActor" && request.location.empty()) {
        request.location = Trim(ExtractJsonStringValue(lineObject, "item"));
    }
    if (request.action == "Consume" && request.item.empty()) {
        request.item = request.target;
    }
    if (request.action == "PickupItem") {
        request.itemRefId = ExtractLeadingFormId(request.item);
    }

    request.itemBaseId = ParseActionFormId(ExtractJsonStringValue(lineObject, "item_baseid"));
    if (request.itemBaseId == 0) {
        request.itemBaseId = ParseActionFormId(ExtractJsonStringValue(lineObject, "baseid"));
    }
    if (request.itemRefId == 0) {
        request.itemRefId = ParseActionFormId(ExtractJsonStringValue(lineObject, "item_refid"));
    }
    request.locationFormId = ParseActionFormId(ExtractJsonStringValue(lineObject, "location_refid"));
    if (request.itemBaseId == 0 && request.action != "PickupItem") {
        request.itemBaseId = ExtractLeadingFormId(request.item);
    }

    request.speakerFormId = ParseActionFormId(ExtractJsonStringValue(lineObject, "speaker_refid"));
    if (request.speakerFormId == 0) {
        request.speakerFormId = ParseActionFormId(ExtractJsonStringValue(lineObject, "speaker_formid"));
    }
    if (request.speakerFormId == 0 && !request.narratorAuthority) {
        request.speakerFormId = ResolveSpeakerFormId(request.speaker);
    }

    request.targetFormId = ParseActionFormId(ExtractJsonStringValue(lineObject, "target_refid"));
    if (request.targetFormId == 0) {
        request.targetFormId = ParseActionFormId(ExtractJsonStringValue(lineObject, "target_formid"));
    }
    if (request.targetFormId == 0) {
        request.targetFormId = ResolveTargetFormId(request.target, request.speakerFormId);
    }

    if (request.narratorAuthority &&
        (request.action == "SpawnCaps" || request.action == "SpawnItem" ||
         request.action == "TeleportActor") && request.targetFormId == 0 &&
        (request.target.empty() || IsPlayerTargetName(request.target))) {
        request.target = PlayerDisplayNameForAction();
        request.targetFormId = Misc::GetPlayerFormId() != 0
            ? Misc::GetPlayerFormId()
            : 0x00000014;
    }

    if ((request.action == "FollowPlayer" || request.action == "ComeCloser" ||
         request.action == "MakeFollower" || request.action == "TakeCapsFromPlayer") &&
        request.targetFormId == 0) {
        request.targetFormId = Misc::GetPlayerFormId() != 0 ? Misc::GetPlayerFormId() : 0x00000014;
    }

    if (request.action == "TakeASeat" && request.targetFormId == 0) {
        ResolveNearbyFurniture(request);
    }

    if (request.action == "TravelTo" && request.targetFormId == 0) {
        ResolveNearbyPoi(request);
    }

    if (request.action == "Follow" && IsPlayerTargetName(request.target)) {
        request.action = "FollowPlayer";
        request.target = PlayerDisplayNameForAction();
        request.targetFormId = Misc::GetPlayerFormId() != 0 ? Misc::GetPlayerFormId() : 0x00000014;
    }

    if (request.action == "PickupItem" &&
        (request.itemRefId == 0 || request.itemBaseId == 0 || request.itemBaseId == request.itemRefId)) {
        ResolveNearbyItem(request);
    }

    if (request.speakerFormId == 0 && !request.narratorAuthority && !IsPlayerTargetName(request.speaker)) {
        Logger::LogWarning("%s: Action %s missing speaker ref for [%s]",
            source ? source : "ActionManager",
            request.action.c_str(),
            request.speaker.c_str());
    }

    if (IsNarratorPluginAction(request.action) && !request.narratorAuthority) {
        Logger::LogWarning("%s: Rejected narrator-only action %s without narrator authority",
            source ? source : "ActionManager", request.action.c_str());
        return false;
    }

    return true;
}

bool ExecuteNarratorAction(ActionRequest request, const char* source) {
    const std::string sourceName = source ? source : "ActionManager";
    if (!request.narratorAuthority || !IsNarratorPluginAction(request.action)) {
        return false;
    }

    if (request.action == "ReadQuests") {
        LaunchPluginResultWorker(request);
        Console::Print("[DIALECTIC] Narrator action: ReadQuests");
        Logger::LogInfo("[NARRATOR_ACTION] launched ReadQuests result generation=%llu",
            static_cast<unsigned long long>(request.runtimeGeneration));
        return true;
    }

    if (!IsNarratorMutatingAction(request.action)) {
        SendFuncretResult(request, request.action + " failed because unsupported_narrator_action.");
        return false;
    }

    RuntimeSnapshot::GameState gameState;
    if (!RuntimeSnapshot::TryGetFreshGameState(gameState, std::chrono::milliseconds(1000))) {
        SendFuncretResult(request, request.action + " failed because game_state_unavailable.");
        return false;
    }

    if (request.targetFormId == 0) {
        SendFuncretResult(request, request.action + " failed because target_unresolved.");
        return false;
    }

    const bool targetIsPlayer = request.targetFormId == gameState.playerFormId ||
        request.targetFormId == 0x00000014;
    if (!targetIsPlayer) {
        RuntimeSnapshot::ActorState targetState;
        if (!RuntimeSnapshot::TryGetActor(request.targetFormId, targetState) ||
            targetState.deleted || targetState.dead || !targetState.loaded3D ||
            !RuntimeSnapshot::IsActorInScene(targetState, gameState)) {
            SendFuncretResult(request, request.action + " failed because target_not_in_current_scene.");
            return false;
        }
    }

    if (request.action == "SpawnItem" && request.itemBaseId == 0) {
        SendFuncretResult(request, "SpawnItem failed because item_base_unresolved.");
        return false;
    }
    if (request.action == "TeleportActor" && request.locationFormId == 0) {
        SendFuncretResult(request, "TeleportActor failed because destination_unresolved.");
        return false;
    }

    const int maximum = request.action == "SpawnCaps" ? 1000000 : 100;
    request.amount = std::max(1, std::min(request.amount, maximum));
    const std::string commandKey = request.action + ":" + FormatRefId(request.targetFormId);
    const std::uint64_t generation = request.runtimeGeneration;
    return GameThreadDispatcher::Enqueue("narrator_action", commandKey, generation,
        [request, sourceName]() {
            std::string failure;
            bool succeeded = false;
            if (request.action == "SpawnCaps") {
                succeeded = XNVSEAdapter::AddNativeItemToActor(
                    request.targetFormId, 0x0000000F, request.amount, failure);
            } else if (request.action == "SpawnItem") {
                succeeded = XNVSEAdapter::AddNativeItemToActor(
                    request.targetFormId, request.itemBaseId, request.amount, failure);
            } else if (request.action == "TeleportActor") {
                succeeded = XNVSEAdapter::TeleportNativeActor(
                    request.targetFormId, request.locationFormId, failure);
            } else if (request.action == "KillTarget") {
                succeeded = XNVSEAdapter::KillNativeActor(request.targetFormId, failure);
            }

            if (!succeeded) {
                if (failure.empty()) failure = "native_execution_failed";
                SendFuncretResult(request, request.action + " failed because " + failure + ".");
                Console::Print("[DIALECTIC] Narrator action failed: %s", request.action.c_str());
                Logger::LogWarning("[NARRATOR_ACTION] action=%s target=0x%08X failed reason=%s source=%s",
                    request.action.c_str(), request.targetFormId, failure.c_str(), sourceName.c_str());
                return;
            }

            SendFuncretResult(request, request.action + " completed successfully.");
            Console::Print("[DIALECTIC] Narrator action: %s", request.action.c_str());
            Logger::LogInfo("[NARRATOR_ACTION] action=%s target=0x%08X amount=%d item=0x%08X location=0x%08X source=%s succeeded",
                request.action.c_str(), request.targetFormId, request.amount,
                request.itemBaseId, request.locationFormId, sourceName.c_str());
        },
        [request](const char* reason) {
            Logger::LogWarning("[NARRATOR_ACTION] dropped action=%s target=0x%08X reason=%s",
                request.action.c_str(), request.targetFormId, reason ? reason : "unknown");
        });
}

bool ExecuteActionRequest(ActionRequest request, const char* source) {
    if (request.runtimeGeneration == 0) {
        request.runtimeGeneration = RuntimeGeneration::Current();
    } else if (!RuntimeGeneration::IsCurrent(request.runtimeGeneration)) {
        Logger::LogInfo("%s: Dropped stale action %s runtime_generation=%llu current=%llu",
            source ? source : "ActionManager",
            request.action.c_str(),
            static_cast<unsigned long long>(request.runtimeGeneration),
            static_cast<unsigned long long>(RuntimeGeneration::Current()));
        return false;
    }
    if (request.action == "Talk") {
        return SendDirectorTalkInstruction(request, source);
    }
    if (request.narratorAuthority) {
        return ExecuteNarratorAction(request, source);
    }

    RuntimeSnapshot::GameState nativeGameState;
    if (RuntimeSnapshot::TryGetFreshGameState(nativeGameState, std::chrono::milliseconds(500))) {
        RuntimeSnapshot::ActorState speakerState;
        if (request.speakerFormId == 0 || request.speakerFormId == nativeGameState.playerFormId ||
            !RuntimeSnapshot::TryGetActor(request.speakerFormId, speakerState) ||
            speakerState.deleted || speakerState.dead || !speakerState.loaded3D ||
            !RuntimeSnapshot::IsActorInScene(speakerState, nativeGameState)) {
            Logger::LogWarning("%s: Action %s rejected by native scene gate speaker=0x%08X",
                source ? source : "ActionManager", request.action.c_str(), request.speakerFormId);
            SendFuncretResult(request, request.action + " failed because speaker_not_in_current_scene.");
            return false;
        }

        const bool requiresActorTarget = request.action == "Attack" || request.action == "Follow" ||
            request.action == "GiveItemTo" || request.action == "GiveCapsTo";
        if (requiresActorTarget && request.targetFormId != 0 &&
            request.targetFormId != nativeGameState.playerFormId) {
            RuntimeSnapshot::ActorState targetState;
            if (!RuntimeSnapshot::TryGetActor(request.targetFormId, targetState) ||
                targetState.deleted || targetState.dead || !targetState.loaded3D ||
                !RuntimeSnapshot::IsActorInScene(targetState, nativeGameState)) {
                Logger::LogWarning("%s: Action %s rejected by native target gate target=0x%08X",
                    source ? source : "ActionManager", request.action.c_str(), request.targetFormId);
                SendFuncretResult(request, request.action + " failed because target_not_in_current_scene.");
                return false;
            }
        }
    }

    std::string itemResolutionError;
    if (!ResolveInventoryItemBase(request, itemResolutionError)) {
        Logger::LogWarning("%s: Action %s failed before bridge: %s actor=[%s] item=[%s]",
            source ? source : "ActionManager",
            request.action.c_str(),
            itemResolutionError.c_str(),
            request.speaker.c_str(),
            request.item.c_str());
        SendFuncretResult(request, request.action + " failed because " + itemResolutionError + ".");
        Console::Print("[DIALECTIC] Action failed: %s", itemResolutionError.c_str());
        return false;
    }

    if (request.action == "PickupItem" && request.itemRefId == 0) {
        Logger::LogWarning("%s: PickupItem failed before bridge: missing item ref item=[%s]",
            source ? source : "ActionManager",
            request.item.c_str());
        SendFuncretResult(request, "PickupItem failed because item_ref_unresolved.");
        Console::Print("[DIALECTIC] Action failed: item ref unresolved");
        return false;
    }

    if (request.action == "PickupItem" && request.itemBaseId == 0) {
        Logger::LogWarning("%s: PickupItem failed before bridge: missing item base item=[%s] ref=0x%08X",
            source ? source : "ActionManager",
            request.item.c_str(),
            request.itemRefId);
        SendFuncretResult(request, "PickupItem failed because item_base_unresolved.");
        Console::Print("[DIALECTIC] Action failed: item base unresolved");
        return false;
    }

    if (ShouldSuppressInventoryOpen(request)) {
        SendFuncretResult(request, request.action + " was ignored because the inventory menu was already opened recently.");
        return false;
    }

    if (ShouldSuppressDuplicateBridgeAction(request, source)) {
        return true;
    }

    const std::string commandKey = request.action + ":" + FormatRefId(request.speakerFormId);
    const std::uint64_t generation = request.runtimeGeneration;
    return GameThreadDispatcher::Enqueue("action", commandKey, generation,
        [request]() {
            const int actionCode = ActionCodeForAction(request.action);
            if (actionCode == 2 || actionCode == 3) {
                XNVSEAdapter::NativeTradeMenuInfo menuInfo;
                if (XNVSEAdapter::ResolveNativeTradeMenu(
                        request.speakerFormId, actionCode == 3, menuInfo)) {
                    const uint64_t requestId = Misc::GetCurrentTimeMillis() * 1000ULL + (++g_requestCounter);
                    TradeManager::TradeSessionRequest tradeRequest;
                    tradeRequest.requestId = requestId;
                    tradeRequest.action = request.action;
                    tradeRequest.speakerName = request.speaker;
                    tradeRequest.speakerFormId = request.speakerFormId;
                    tradeRequest.inventoryOwnerFormId = menuInfo.inventoryOwnerFormId;
                    tradeRequest.tradeMode = menuInfo.barter ? "barter" :
                        (actionCode == 3 ? "fallback_trade" : "trade");
                    TradeManager::BeginPendingSession(tradeRequest);
                    if (XNVSEAdapter::OpenNativeTradeMenu(menuInfo)) {
                        SendFuncretResult(request, request.action + " completed successfully.");
                        Console::Print("[DIALECTIC] Action: %s", request.action.c_str());
                        Logger::LogInfo("[NATIVE_ACTION] opened %s menu speaker=0x%08X inventory_owner=0x%08X request=%llu without bridge transport",
                            tradeRequest.tradeMode.c_str(), request.speakerFormId,
                            tradeRequest.inventoryOwnerFormId,
                            static_cast<unsigned long long>(requestId));
                        return;
                    }
                    TradeManager::CancelAll("native_menu_open_failed");
                }
            }
            if (actionCode == 1 && BeginNativeAttack(request)) {
                SendFuncretResult(request, "Attack started successfully.");
                Console::Print("[DIALECTIC] Action: Attack");
                Logger::LogInfo("[NATIVE_ACTION] started attack speaker=0x%08X target=0x%08X without bridge transport",
                    request.speakerFormId, request.targetFormId);
                return;
            }
            if (actionCode == 9 || actionCode == 10 || actionCode == 11 || actionCode == 13 ||
                actionCode == 31 || actionCode == 32) {
                bool handled = false;
                TryExecuteNativeInventoryAction(request, actionCode, handled);
                if (handled) {
                    return;
                }
            }
            if (actionCode == 12) {
                bool handled = false;
                BeginNativePickup(request, handled);
                if (handled) {
                    return;
                }
            }
            if (actionCode == 26 && XNVSEAdapter::ExecuteNativeStopFollowing(request.speakerFormId)) {
                ClearNativePackageState(request.speakerFormId);
                SendFuncretResult(request, "StopFollowing completed successfully.");
                Console::Print("[DIALECTIC] Action: StopFollowing");
                Logger::LogInfo("[NATIVE_ACTION] stopped following speaker=0x%08X without bridge transport",
                    request.speakerFormId);
                return;
            }
            const bool simpleNativeAction = actionCode == 14 || actionCode == 15 ||
                actionCode == 16 || actionCode == 17 || actionCode == 19;
            if (simpleNativeAction &&
                XNVSEAdapter::ExecuteSimpleNativeAction(request.speakerFormId, actionCode)) {
                if (actionCode == 17 || actionCode == 19) {
                    ClearNativePackageState(request.speakerFormId);
                }
                if (request.action == "EndConversation") {
                    GameLoop::ApplyEndConversationCooldown(request.speakerFormId, request.speaker);
                }
                SendFuncretResult(request, request.action + " completed successfully.");
                Console::Print("[DIALECTIC] Action: %s", request.action.c_str());
                Logger::LogInfo("[NATIVE_ACTION] completed action=%s speaker=0x%08X without bridge transport",
                    request.action.c_str(), request.speakerFormId);
                return;
            }

            const bool packageNativeAction = actionCode == 4 || actionCode == 5 ||
                actionCode == 6 || actionCode == 7 || actionCode == 8 ||
                actionCode == 18 || actionCode == 20 || actionCode == 21 || actionCode == 33;
            if ((actionCode == 4 || actionCode == 5 || actionCode == 33)) {
                bool handledByCompanionAdapter = false;
                bool usedCcc = false;
                const bool companionCommandSucceeded = XNVSEAdapter::ExecuteNativeCompanionCommand(
                    request.speakerFormId, actionCode, handledByCompanionAdapter, usedCcc);
                if (handledByCompanionAdapter) {
                    if (companionCommandSucceeded) {
                        TrackNativePackageAction(request);
                        SendFuncretResult(request, request.action + " started successfully.");
                        Console::Print("[DIALECTIC] Action: %s", request.action.c_str());
                        Logger::LogInfo("[NATIVE_ACTION] companion state action=%s speaker=0x%08X adapter=%s",
                            request.action.c_str(), request.speakerFormId, usedCcc ? "jip_ccc" : "dialectic");
                    } else {
                        SendFuncretResult(request, request.action + " failed because companion_adapter_rejected.");
                        Console::Print("[DIALECTIC] Action failed: %s", request.action.c_str());
                        Logger::LogWarning("[NATIVE_ACTION] companion adapter rejected action=%s speaker=0x%08X adapter=%s",
                            request.action.c_str(), request.speakerFormId, usedCcc ? "jip_ccc" : "dialectic");
                    }
                    return;
                }
            }
            if (packageNativeAction && XNVSEAdapter::ExecuteNativePackageAction(
                    request.speakerFormId, request.targetFormId, actionCode)) {
                TrackNativePackageAction(request);
                SendFuncretResult(request, request.action + " started successfully.");
                Console::Print("[DIALECTIC] Action: %s", request.action.c_str());
                Logger::LogInfo("[NATIVE_ACTION] started package action=%s speaker=0x%08X target=0x%08X without bridge transport",
                    request.action.c_str(), request.speakerFormId, request.targetFormId);
                return;
            }

            if (ShouldGeneratePluginResult(request.action)) {
                LaunchPluginResultWorker(request);
                Console::Print("[DIALECTIC] Action: %s", request.action.c_str());
                Logger::LogInfo("[NATIVE_ACTION] launched plugin result action=%s speaker=0x%08X without bridge transport",
                    request.action.c_str(), request.speakerFormId);
                return;
            }

            Logger::LogWarning("[NATIVE_ACTION] action=%s speaker=0x%08X could not be executed by the native runtime",
                request.action.c_str(), request.speakerFormId);
            SendFuncretResult(request, request.action + " failed because native_runtime_unavailable.");
            Console::Print("[DIALECTIC] Action failed: %s", request.action.c_str());
        },
        [request](const char* reason) {
            Logger::LogWarning("ActionManager: Dropped action=%s speaker=0x%08X reason=%s",
                request.action.c_str(), request.speakerFormId, reason ? reason : "unknown");
        });
}

} // namespace

bool IsActionCommand(const std::string& actionName) {
    const std::string normalized = NormalizeActionName(actionName);
    return CanonicalActions().find(normalized) != CanonicalActions().end();
}

bool HandleRoleCommandJson(const std::string& lineObject,
                           const char* source,
                           uint64_t runtimeGeneration) {
    std::string actionField = Trim(ExtractJsonStringValue(lineObject, "action"));
    std::string commandName = Trim(ExtractJsonStringValue(lineObject, "command_name"));
    std::string command = commandName;
    if (command.empty()) {
        command = Trim(ExtractJsonStringValue(lineObject, "message"));
    }
    if (command.empty()) {
        command = Trim(ExtractJsonStringValue(lineObject, "text"));
    }
    if (command.empty()) {
        command = Trim(ExtractJsonStringValue(lineObject, "command"));
    }
    if (command.empty()) {
        command = Trim(ExtractJsonStringValue(lineObject, "action_code"));
    }

    const std::vector<std::string> commandArgs = ExtractJsonStringArrayValue(lineObject, "command_args");

    if (command == "DebugNotification") {
        std::string notification;
        if (!commandArgs.empty()) {
            notification = commandArgs[0];
        }
        if (notification.empty() && !commandName.empty()) {
            notification = Trim(ExtractJsonStringValue(lineObject, "message"));
        }
        if (notification.empty()) {
            notification = "Command notification received.";
        }
        Console::Print("[DIALECTIC] %s", notification.c_str());
        return true;
    }

    if (command == "RefreshNPCVoice") {
        std::string npcName = Trim(ExtractJsonStringValue(lineObject, "npc"));
        if (npcName.empty() && !commandArgs.empty()) {
            npcName = commandArgs[0];
        }
        Logger::LogInfo("%s: RefreshNPCVoice command received for [%s]",
            source ? source : "ActionManager",
            npcName.c_str());
        return true;
    }

    ActionRequest request;
    if (!BuildActionRequestFromRoleCommandJson(lineObject, source, request, true)) {
        return false;
    }
    request.runtimeGeneration = runtimeGeneration != 0
        ? runtimeGeneration
        : RuntimeGeneration::Current();
    return ExecuteActionRequest(request, source);
}

int HaltAIActions(const char* source) {
    const std::vector<std::pair<uint32_t, std::string>> targets = BuildHaltTargetSnapshot();
    ClearAllNativePackageStates();
    RequestNativeAttackCleanup("halt_ai_actions");
    CancelNativePickups("halt_ai_actions");
    TradeManager::CancelAll("halt_ai_actions");
    GameThreadDispatcher::CancelByType("action", "halt_ai_actions");
    GameThreadDispatcher::CancelByType("action_native", "halt_ai_actions");

    auto halt = [targets]() {
        std::size_t failed = 0;
        for (const auto& target : targets) {
            if (target.first != 0 && !XNVSEAdapter::HaltNativeActor(target.first)) {
                ++failed;
            }
        }
        Logger::LogInfo("[NATIVE_ACTION] halt applied actors=%zu failed=%zu",
            targets.size(), failed);
    };
    if (GameThreadDispatcher::IsGameThread()) {
        halt();
    } else {
        GameThreadDispatcher::Enqueue("action_native", "halt", RuntimeGeneration::Current(), std::move(halt));
    }

    Logger::LogInfo("%s: Halt AI Actions requested for %zu actor(s)",
        source ? source : "ActionManager",
        targets.size());
    return static_cast<int>(targets.size());
}

void CancelNativeRuntimeActions(const char* reason) {
    RequestNativeAttackCleanup(reason);
    UpdateNativeAttackStates();
    CancelNativePickups(reason);
    const std::vector<std::uint32_t> actors = ClearAllNativePackageStates();
    std::size_t restored = 0;
    std::size_t deferred = 0;
    for (const std::uint32_t actorFormId : actors) {
        if (XNVSEAdapter::HaltNativeActor(actorFormId)) {
            ++restored;
        } else {
            QueueNativePackageCleanup(actorFormId);
            ++deferred;
        }
    }
    GameThreadDispatcher::CancelByType("action", reason ? reason : "runtime_invalidated");
    GameThreadDispatcher::CancelByType("action_native", reason ? reason : "runtime_invalidated");
    Logger::LogInfo("[NATIVE_ACTION] runtime cleanup reason=%s actors=%zu restored=%zu deferred=%zu",
        reason ? reason : "runtime_invalidated", actors.size(), restored, deferred);
}

void UpdateNativePickupStates() {
    std::vector<NativePickupState> states;
    {
        std::lock_guard<std::mutex> lock(g_nativePickupMutex);
        states.reserve(g_nativePickupStates.size());
        for (const auto& entry : g_nativePickupStates) {
            states.push_back(entry.second);
        }
    }
    const auto now = std::chrono::steady_clock::now();
    for (const NativePickupState& state : states) {
        const ActionRequest& request = state.request;
        bool remove = false;
        bool halt = false;
        std::string failure;
        if (state.generation != RuntimeGeneration::Current()) {
            remove = true;
            halt = true;
            failure = "stale_generation";
        } else {
            RuntimeSnapshot::ActorState speaker;
            RuntimeSnapshot::ReferenceState item;
            const bool speakerReady = RuntimeSnapshot::TryGetActor(request.speakerFormId, speaker) &&
                speaker.loaded3D && !speaker.deleted && !speaker.dead;
            const bool itemReady = RuntimeSnapshot::TryGetReference(request.itemRefId, item) &&
                item.loaded3D && !item.deleted && !item.taken;
            if (!speakerReady || !itemReady) {
                remove = true;
                halt = true;
                failure = !speakerReady ? "speaker_left_scene" : "item_left_scene";
            } else {
                const float dx = speaker.x - item.x;
                const float dy = speaker.y - item.y;
                const float dz = speaker.z - item.z;
                const float distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
                if (distance <= 128.0f) {
                    const bool completed = CompleteNativePickup(request);
                    remove = true;
                    halt = true;
                    if (!completed) {
                        failure = "native_transfer_unavailable";
                    }
                } else if (now - state.startedAt > std::chrono::minutes(2)) {
                    remove = true;
                    halt = true;
                    failure = "timeout";
                }
            }
        }
        if (!remove) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(g_nativePickupMutex);
            g_nativePickupStates.erase(request.speakerFormId);
        }
        if (halt && !XNVSEAdapter::HaltNativeActor(request.speakerFormId)) {
            QueueNativePackageCleanup(request.speakerFormId);
        }
        if (!failure.empty()) {
            if (failure != "stale_generation") {
                SendFuncretResult(request, "PickupItem failed because " + failure + ".");
            }
            Logger::LogWarning("[NATIVE_ACTION] pickup cancelled speaker=0x%08X item_ref=0x%08X reason=%s",
                request.speakerFormId, request.itemRefId, failure.c_str());
        }
    }
}

void UpdateNativeAttackStates() {
    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t generation = RuntimeGeneration::Current();
    std::vector<std::uint32_t> candidates;
    {
        std::lock_guard<std::mutex> lock(g_nativeAttackMutex);
        for (auto& entry : g_nativeAttackStates) {
            NativeAttackState& state = entry.second;
            if (state.generation != generation) {
                state.cleanupRequested = true;
                state.cleanupReason = "stale_generation";
            }
            RuntimeSnapshot::ActorState speaker;
            RuntimeSnapshot::ActorState target;
            const bool speakerPresent = RuntimeSnapshot::TryGetActor(state.request.speakerFormId, speaker);
            const bool targetIsPlayer = state.targetBefore.player;
            const bool targetPresent = targetIsPlayer ||
                RuntimeSnapshot::TryGetActor(state.request.targetFormId, target);
            if (!state.cleanupRequested &&
                ((speakerPresent && (speaker.dead || speaker.deleted)) ||
                 (!targetIsPlayer && targetPresent && (target.dead || target.deleted)))) {
                state.cleanupRequested = true;
                state.cleanupReason = "combatant_dead";
            }
            if (!state.cleanupRequested && now - state.startedAt >= std::chrono::seconds(12)) {
                const bool speakerInCombat = speakerPresent && speaker.inCombat;
                bool targetInCombat = false;
                if (targetIsPlayer) {
                    const RuntimeSnapshot::GameState game = RuntimeSnapshot::GetGameState();
                    targetInCombat = game.valid && game.inCombat;
                } else {
                    targetInCombat = targetPresent && target.inCombat;
                }
                if (!speakerInCombat && !targetInCombat) {
                    state.cleanupRequested = true;
                    state.cleanupReason = "combat_ended";
                }
            }
            if (state.cleanupRequested) {
                candidates.push_back(entry.first);
            }
        }
    }

    for (const std::uint32_t speakerFormId : candidates) {
        NativeAttackState state;
        {
            std::lock_guard<std::mutex> lock(g_nativeAttackMutex);
            const auto it = g_nativeAttackStates.find(speakerFormId);
            if (it == g_nativeAttackStates.end()) {
                continue;
            }
            state = it->second;
        }
        const bool speakerRestored = state.speakerRestored ||
            XNVSEAdapter::RestoreNativeCombatActorState(state.speakerBefore);
        const bool targetRestored = state.targetRestored ||
            XNVSEAdapter::RestoreNativeCombatActorState(state.targetBefore);
        bool completed = false;
        {
            std::lock_guard<std::mutex> lock(g_nativeAttackMutex);
            const auto it = g_nativeAttackStates.find(speakerFormId);
            if (it == g_nativeAttackStates.end()) {
                continue;
            }
            it->second.speakerRestored = speakerRestored;
            it->second.targetRestored = targetRestored;
            completed = speakerRestored && targetRestored;
            if (completed) {
                g_nativeAttackStates.erase(it);
            }
        }
        if (completed) {
            Logger::LogInfo("[NATIVE_ACTION] attack cleanup speaker=0x%08X target=0x%08X reason=%s restored=1",
                state.request.speakerFormId, state.request.targetFormId,
                state.cleanupReason.c_str());
        }
    }
}

void UpdateNativePackageStates() {
    struct Cleanup {
        std::uint32_t actorFormId{0};
        std::string reason;
    };
    std::vector<Cleanup> cleanup;
    std::vector<std::uint32_t> deferredCleanup;
    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t generation = RuntimeGeneration::Current();
    {
        std::lock_guard<std::mutex> lock(g_nativePackageMutex);
        for (const std::uint32_t actorFormId : g_pendingNativeCleanupRefs) {
            RuntimeSnapshot::ActorState actor;
            if (RuntimeSnapshot::TryGetActor(actorFormId, actor) && actor.loaded3D && !actor.deleted) {
                deferredCleanup.push_back(actorFormId);
            }
        }
        for (auto it = g_nativePackageStates.begin(); it != g_nativePackageStates.end();) {
            const NativePackageState& state = it->second;
            const std::string& action = state.request.action;
            const bool persistentCompanionState = action == "MakeFollower" || action == "FollowPlayer" ||
                action == "WaitHere" || action == "Relax";
            bool shouldCleanup = state.generation != generation && !persistentCompanionState;
            std::string reason = shouldCleanup ? "stale_generation" : "";

            RuntimeSnapshot::ActorState speaker;
            const bool speakerReady = RuntimeSnapshot::TryGetActor(it->first, speaker) &&
                !speaker.deleted && !speaker.dead && speaker.loaded3D;
            if (!shouldCleanup && !speakerReady && !persistentCompanionState) {
                shouldCleanup = true;
                reason = "speaker_left_scene";
            }

            if (!shouldCleanup && (action == "ComeCloser" || action == "MoveTo" || action == "TravelTo")) {
                float distance = speaker.distanceToPlayer;
                float threshold = action == "TravelTo" ? 180.0f : 110.0f;
                if (state.request.targetFormId != 0 && state.request.targetFormId != 0x14) {
                    RuntimeSnapshot::ActorState targetActor;
                    RuntimeSnapshot::ReferenceState targetRef;
                    float targetX = 0.0f;
                    float targetY = 0.0f;
                    float targetZ = 0.0f;
                    bool targetReady = false;
                    if (RuntimeSnapshot::TryGetActor(state.request.targetFormId, targetActor)) {
                        targetX = targetActor.x;
                        targetY = targetActor.y;
                        targetZ = targetActor.z;
                        targetReady = targetActor.loaded3D && !targetActor.deleted && !targetActor.dead;
                    } else if (RuntimeSnapshot::TryGetReference(state.request.targetFormId, targetRef)) {
                        targetX = targetRef.x;
                        targetY = targetRef.y;
                        targetZ = targetRef.z;
                        targetReady = targetRef.loaded3D && !targetRef.deleted && !targetRef.taken;
                    }
                    if (!targetReady) {
                        shouldCleanup = true;
                        reason = "target_left_scene";
                    } else {
                        const float dx = speaker.x - targetX;
                        const float dy = speaker.y - targetY;
                        const float dz = speaker.z - targetZ;
                        distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
                    }
                }
                if (!shouldCleanup && distance <= threshold) {
                    shouldCleanup = true;
                    reason = "destination_reached";
                }
                const auto timeout = action == "ComeCloser" ? std::chrono::seconds(30) : std::chrono::minutes(3);
                if (!shouldCleanup && now - state.startedAt > timeout) {
                    shouldCleanup = true;
                    reason = "timeout";
                }
            }

            if (shouldCleanup) {
                cleanup.push_back({it->first, reason});
                it = g_nativePackageStates.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const std::uint32_t actorFormId : deferredCleanup) {
        if (!XNVSEAdapter::HaltNativeActor(actorFormId)) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(g_nativePackageMutex);
            g_pendingNativeCleanupRefs.erase(actorFormId);
        }
        Logger::LogInfo("[NATIVE_ACTION] deferred package cleanup actor=0x%08X completed",
            actorFormId);
    }

    for (const Cleanup& item : cleanup) {
        if (!XNVSEAdapter::HaltNativeActor(item.actorFormId)) {
            QueueNativePackageCleanup(item.actorFormId);
        }
        Logger::LogInfo("[NATIVE_ACTION] package cleanup actor=0x%08X reason=%s",
            item.actorFormId, item.reason.c_str());
    }
}

static bool HasActiveWork() {
    {
        std::lock_guard<std::mutex> lock(g_nativePackageMutex);
        if (!g_nativePackageStates.empty() || !g_pendingNativeCleanupRefs.empty()) {
            return true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_nativeAttackMutex);
        if (!g_nativeAttackStates.empty()) {
            return true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_nativePickupMutex);
        if (!g_nativePickupStates.empty()) {
            return true;
        }
    }
    return false;
}

void Update() {
    const auto now = std::chrono::steady_clock::now();
    const auto interval = HasActiveWork()
        ? std::chrono::milliseconds(50)
        : std::chrono::milliseconds(1000);
    if (g_lastManagerUpdate.time_since_epoch().count() != 0 &&
        now - g_lastManagerUpdate < interval) {
        return;
    }
    g_lastManagerUpdate = now;

    UpdateNativeAttackStates();
    UpdateNativePickupStates();
    UpdateNativePackageStates();
}

} // namespace ActionManager
