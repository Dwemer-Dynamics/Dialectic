#include "WorldDataSyncFNV.h"

#include "Console.h"
#include "HTTPManager.h"
#include "Logger.h"
#include "RuntimeGeneration.h"
#include "TaskManager.h"
#include "XNVSEAdapter.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace WorldDataSyncFNV {
namespace {

constexpr size_t kLocationBatchSize = 150;
constexpr uint32_t kCompressedRecordFlag = 0x00040000;
constexpr uint32_t kMapMarkerBaseFormId = 0x00000010;

struct FactionRow {
    std::string name;
    uint32_t formId = 0;
};

struct LocationRow {
    std::string name;
    uint32_t formId = 0;
    std::string formIdHex;
    std::string worldspace;
    std::vector<std::string> tags;
    bool isInterior = false;
    float x = 0.0f;
    float y = 0.0f;
    std::string refs;
    bool cleared = false;
};

struct LoadedPluginRow {
    std::string name;
    std::string lowerName;
    uint32_t compileIndex = 0;
};

struct LoadedPluginData {
    LoadedPluginRow plugin;
    std::vector<uint8_t> data;
    std::vector<std::string> masters;
};

std::atomic<bool> g_syncRequested(false);
std::atomic<bool> g_syncInProgress(false);
std::atomic<bool> g_completed(false);
bool g_nativeMarkerCaptureStarted = false;
std::chrono::steady_clock::time_point g_lastAttempt;
std::mutex g_locationMutex;
std::vector<LocationRow> g_cachedLocations;

std::string Trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

std::string NormalizeText(const std::string& input) {
    std::string value = input;
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\0') {
            ch = ' ';
        }
    }
    return Trim(value);
}

std::string FormatHex8(uint32_t value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string CompactText(const std::string& value) {
    std::string key;
    for (char ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            key.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return key;
}

bool ContainsCompact(const std::string& haystack, const std::string& needle) {
    const std::string compactHaystack = CompactText(haystack);
    const std::string compactNeedle = CompactText(needle);
    return !compactNeedle.empty() && compactHaystack.find(compactNeedle) != std::string::npos;
}

std::string LocationSearchText(const LocationRow& location) {
    std::ostringstream out;
    out << location.name << " "
        << location.formIdHex << " "
        << location.worldspace << " "
        << location.refs << " ";
    for (const std::string& tag : location.tags) {
        out << tag << " ";
    }
    return out.str();
}

bool IsPluginFileName(const std::string& name) {
    const std::string lower = ToLowerCopy(name);
    return lower.ends_with(".esm") || lower.ends_with(".esp");
}

std::string MarkerTypeName(uint16_t markerType) {
    switch (markerType) {
        case 1: return "City";
        case 2: return "Settlement";
        case 3: return "Encampment";
        case 4: return "NaturalLandmark";
        case 5: return "Cave";
        case 6: return "Factory";
        case 7: return "Memorial";
        case 8: return "Military";
        case 9: return "Office";
        case 10: return "TownRuins";
        case 11: return "UrbanRuins";
        case 12: return "SewerRuins";
        case 13: return "Metro";
        case 14: return "Vault";
        default: return "MapMarker";
    }
}

std::vector<FactionRow> CollectFactions() {
    std::vector<FactionRow> factions;
    std::vector<XNVSEAdapter::NativeFaction> native;
    if (!XNVSEAdapter::CaptureNativeFactions(native)) {
        Logger::LogWarning("[WORLD_DATA] Native faction snapshot unavailable");
        return factions;
    }
    factions.reserve(native.size());
    for (auto& faction : native) {
        factions.push_back({std::move(faction.name), faction.formId});
    }

    std::sort(factions.begin(), factions.end(), [](const FactionRow& a, const FactionRow& b) {
        return ToLowerCopy(a.name) < ToLowerCopy(b.name);
    });
    return factions;
}

uint16_t ReadU16(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + sizeof(uint16_t) > data.size()) {
        return 0;
    }
    uint16_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

uint32_t ReadU32(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + sizeof(uint32_t) > data.size()) {
        return 0;
    }
    uint32_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

float ReadFloat(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + sizeof(float) > data.size()) {
        return 0.0f;
    }
    float value = 0.0f;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

std::string ReadType4(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) {
        return "";
    }
    return std::string(reinterpret_cast<const char*>(data.data() + offset), 4);
}

std::string ReadPluginString(const std::vector<uint8_t>& data, size_t offset, size_t length) {
    if (offset >= data.size()) {
        return "";
    }
    const size_t end = std::min(data.size(), offset + length);
    std::string value(reinterpret_cast<const char*>(data.data() + offset), end - offset);
    const size_t nul = value.find('\0');
    if (nul != std::string::npos) {
        value.resize(nul);
    }
    return NormalizeText(value);
}

std::vector<LoadedPluginRow> CollectLoadedPluginRows() {
    std::vector<LoadedPluginRow> plugins;
    std::vector<XNVSEAdapter::NativeLoadedPlugin> native;
    if (!XNVSEAdapter::CaptureNativeLoadedPlugins(native)) {
        Logger::LogWarning("[WORLD_DATA] Native loaded-plugin snapshot unavailable");
        return plugins;
    }
    std::set<std::string> seen;
    for (const auto& entry : native) {
        const std::string& name = entry.name;
        if (name.empty() || !IsPluginFileName(name)) {
            continue;
        }

        const std::string lower = ToLowerCopy(name);
        if (!seen.insert(lower).second) {
            continue;
        }

        plugins.push_back({name, lower, entry.compileIndex});
    }

    return plugins;
}

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& data) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return false;
    }

    const std::streamsize size = input.tellg();
    if (size <= 0) {
        return false;
    }

    data.resize(static_cast<size_t>(size));
    input.seekg(0, std::ios::beg);
    return input.read(reinterpret_cast<char*>(data.data()), size).good();
}

bool ReadPluginFile(const std::string& pluginName, std::vector<uint8_t>& data) {
    if (ReadWholeFile("Data\\" + pluginName, data)) {
        return true;
    }
    return ReadWholeFile(pluginName, data);
}

std::vector<std::string> ReadPluginMasters(const std::vector<uint8_t>& data) {
    std::vector<std::string> masters;
    if (data.size() < 24 || ReadType4(data, 0) != "TES4") {
        return masters;
    }

    const size_t payloadStart = 24;
    const size_t payloadEnd = std::min<size_t>(data.size(), payloadStart + ReadU32(data, 4));
    for (size_t offset = payloadStart; offset + 6 <= payloadEnd;) {
        const std::string type = ReadType4(data, offset);
        uint32_t size = ReadU16(data, offset + 4);
        offset += 6;
        if (offset + size > payloadEnd) {
            break;
        }
        if (type == "MAST") {
            const std::string master = ReadPluginString(data, offset, size);
            if (!master.empty()) {
                masters.push_back(master);
            }
        }
        offset += size;
    }
    return masters;
}

uint32_t ResolvePluginFormId(
    uint32_t localFormId,
    const LoadedPluginRow& plugin,
    const std::vector<std::string>& masters,
    const std::map<std::string, uint32_t>& compileIndexByPluginName) {
    if (localFormId == 0) {
        return 0;
    }

    const uint32_t localIndex = (localFormId >> 24) & 0xFF;
    const uint32_t objectId = localFormId & 0x00FFFFFF;
    uint32_t compileIndex = plugin.compileIndex;

    if (localIndex < masters.size()) {
        const std::string masterKey = ToLowerCopy(masters[localIndex]);
        const auto found = compileIndexByPluginName.find(masterKey);
        if (found == compileIndexByPluginName.end()) {
            return 0;
        }
        compileIndex = found->second;
    }

    return ((compileIndex & 0xFF) << 24) | objectId;
}

void CollectWorldspaceNames(
    const std::vector<uint8_t>& data,
    size_t start,
    size_t end,
    const LoadedPluginRow& plugin,
    const std::vector<std::string>& masters,
    const std::map<std::string, uint32_t>& compileIndexByPluginName,
    std::map<uint32_t, std::string>& worldspaceNames,
    const TaskManager::CancellationToken& token) {
    size_t offset = start;
    while (offset + 24 <= end && offset + 24 <= data.size()) {
        if (token.IsCancellationRequested()) return;
        const std::string type = ReadType4(data, offset);
        const uint32_t size = ReadU32(data, offset + 4);
        if (type == "GRUP") {
            if (size < 24 || offset + size > end || offset + size > data.size()) {
                break;
            }
            CollectWorldspaceNames(
                data,
                offset + 24,
                offset + size,
                plugin,
                masters,
                compileIndexByPluginName,
                worldspaceNames,
                token);
            offset += size;
            continue;
        }

        if (size > data.size() - offset - 24 || offset + 24 + size > end) {
            break;
        }

        if (type == "WRLD" && (ReadU32(data, offset + 8) & kCompressedRecordFlag) == 0) {
            const uint32_t runtimeWorldspaceId = ResolvePluginFormId(
                ReadU32(data, offset + 12),
                plugin,
                masters,
                compileIndexByPluginName);
            const size_t payloadStart = offset + 24;
            const size_t payloadEnd = payloadStart + size;
            std::string editorId;
            std::string fullName;
            uint32_t extendedSize = 0;

            for (size_t subOffset = payloadStart; subOffset + 6 <= payloadEnd;) {
                const std::string subType = ReadType4(data, subOffset);
                uint32_t subSize = ReadU16(data, subOffset + 4);
                subOffset += 6;

                if (subType == "XXXX" && subSize == 4 && subOffset + 4 <= payloadEnd) {
                    extendedSize = ReadU32(data, subOffset);
                    subOffset += subSize;
                    continue;
                }
                if (extendedSize != 0) {
                    subSize = extendedSize;
                    extendedSize = 0;
                }
                if (subOffset + subSize > payloadEnd) {
                    break;
                }

                if (subType == "FULL") {
                    fullName = ReadPluginString(data, subOffset, subSize);
                } else if (subType == "EDID") {
                    editorId = ReadPluginString(data, subOffset, subSize);
                }

                subOffset += subSize;
            }

            const std::string worldspaceName = !fullName.empty() ? fullName : editorId;
            if (runtimeWorldspaceId != 0 && !worldspaceName.empty()) {
                worldspaceNames[runtimeWorldspaceId] = worldspaceName;
            }
        }

        offset += 24 + size;
    }
}

void ParsePluginRecordLocations(
    const std::vector<uint8_t>& data,
    size_t start,
    size_t end,
    const LoadedPluginRow& plugin,
    const std::vector<std::string>& masters,
    const std::map<std::string, uint32_t>& compileIndexByPluginName,
    const std::map<uint32_t, std::string>& worldspaceNames,
    const std::string& currentWorldspace,
    std::vector<LocationRow>& locations,
    std::set<uint32_t>& seenFormIds,
    size_t& compressedSkipped,
    const TaskManager::CancellationToken& token) {
    size_t offset = start;
    while (offset + 24 <= end && offset + 24 <= data.size()) {
        if (token.IsCancellationRequested()) return;
        const std::string type = ReadType4(data, offset);
        const uint32_t size = ReadU32(data, offset + 4);
        if (type == "GRUP") {
            if (size < 24 || offset + size > end || offset + size > data.size()) {
                break;
            }
            std::string childWorldspace = currentWorldspace;
            const uint32_t groupLabel = ReadU32(data, offset + 8);
            const uint32_t groupType = ReadU32(data, offset + 12);
            if (groupType == 1) {
                const uint32_t runtimeWorldspaceId = ResolvePluginFormId(
                    groupLabel,
                    plugin,
                    masters,
                    compileIndexByPluginName);
                const auto foundWorldspace = worldspaceNames.find(runtimeWorldspaceId);
                if (foundWorldspace != worldspaceNames.end()) {
                    childWorldspace = foundWorldspace->second;
                }
            }
            ParsePluginRecordLocations(
                data,
                offset + 24,
                offset + size,
                plugin,
                masters,
                compileIndexByPluginName,
                worldspaceNames,
                childWorldspace,
                locations,
                seenFormIds,
                compressedSkipped,
                token);
            offset += size;
            continue;
        }

        if (size > data.size() - offset - 24 || offset + 24 + size > end) {
            break;
        }

        const uint32_t flags = ReadU32(data, offset + 8);
        const uint32_t localRecordFormId = ReadU32(data, offset + 12);
        const size_t payloadStart = offset + 24;
        const size_t payloadEnd = payloadStart + size;

        if (type == "REFR") {
            if ((flags & kCompressedRecordFlag) != 0) {
                ++compressedSkipped;
            } else {
                uint32_t baseFormId = 0;
                std::string name;
                std::string editorId;
                uint16_t markerFlags = 0;
                uint16_t markerType = 0;
                float x = 0.0f;
                float y = 0.0f;
                bool hasMapMarkerSubrecord = false;

                uint32_t extendedSize = 0;
                for (size_t subOffset = payloadStart; subOffset + 6 <= payloadEnd;) {
                    const std::string subType = ReadType4(data, subOffset);
                    uint32_t subSize = ReadU16(data, subOffset + 4);
                    subOffset += 6;

                    if (subType == "XXXX" && subSize == 4 && subOffset + 4 <= payloadEnd) {
                        extendedSize = ReadU32(data, subOffset);
                        subOffset += subSize;
                        continue;
                    }

                    if (extendedSize != 0) {
                        subSize = extendedSize;
                        extendedSize = 0;
                    }
                    if (subOffset + subSize > payloadEnd) {
                        break;
                    }

                    if (subType == "NAME" && subSize >= 4) {
                        baseFormId = ResolvePluginFormId(ReadU32(data, subOffset), plugin, masters, compileIndexByPluginName);
                    } else if (subType == "XMRK") {
                        hasMapMarkerSubrecord = true;
                    } else if (subType == "FNAM" && subSize >= 1) {
                        markerFlags = data[subOffset];
                    } else if (subType == "TNAM" && subSize >= 2) {
                        markerType = ReadU16(data, subOffset);
                    } else if (subType == "FULL") {
                        name = ReadPluginString(data, subOffset, subSize);
                    } else if (subType == "EDID") {
                        editorId = ReadPluginString(data, subOffset, subSize);
                    } else if (subType == "DATA" && subSize >= 8) {
                        x = ReadFloat(data, subOffset);
                        y = ReadFloat(data, subOffset + sizeof(float));
                    }

                    subOffset += subSize;
                }

                if (baseFormId == kMapMarkerBaseFormId && hasMapMarkerSubrecord) {
                    const uint32_t runtimeFormId = ResolvePluginFormId(
                        localRecordFormId,
                        plugin,
                        masters,
                        compileIndexByPluginName);
                    if (runtimeFormId != 0 && seenFormIds.insert(runtimeFormId).second) {
                        if (name.empty()) {
                            name = editorId;
                        }
                        if (!name.empty()) {
                            LocationRow row;
                            row.name = name;
                            row.formId = runtimeFormId;
                            row.formIdHex = FormatHex8(runtimeFormId);
                            row.worldspace = !currentWorldspace.empty() ? currentWorldspace : plugin.name;
                            row.tags = { "MapMarker", MarkerTypeName(markerType), plugin.name };
                            row.isInterior = false;
                            row.x = x;
                            row.y = y;
                            row.refs = FormatHex8(runtimeFormId);
                            row.cleared = markerFlags != 0;
                            locations.push_back(row);
                        }
                    }
                }
            }
        }

        offset += 24 + size;
    }
}

std::vector<LocationRow> CollectLocationsFromPlugins(
    const std::vector<LoadedPluginRow>& plugins,
    std::set<uint32_t>& seenFormIds,
    const TaskManager::CancellationToken& token) {
    std::vector<LocationRow> locations;
    if (plugins.empty()) {
        Logger::LogWarning("[WORLD_DATA] No loaded plugins available for map marker scan");
        return locations;
    }

    std::map<std::string, uint32_t> compileIndexByPluginName;
    for (const LoadedPluginRow& plugin : plugins) {
        compileIndexByPluginName[plugin.lowerName] = plugin.compileIndex;
    }

    std::vector<LoadedPluginData> pluginData;
    pluginData.reserve(plugins.size());
    std::map<uint32_t, std::string> worldspaceNames;
    size_t pluginsRead = 0;
    for (const LoadedPluginRow& plugin : plugins) {
        if (token.IsCancellationRequested()) return {};
        std::vector<uint8_t> data;
        if (!ReadPluginFile(plugin.name, data)) {
            continue;
        }

        ++pluginsRead;
        const auto masters = ReadPluginMasters(data);
        CollectWorldspaceNames(data, 0, data.size(), plugin, masters, compileIndexByPluginName,
            worldspaceNames, token);
        if (token.IsCancellationRequested()) return {};
        pluginData.push_back({ plugin, std::move(data), masters });
    }

    size_t compressedSkipped = 0;
    for (const LoadedPluginData& loadedPlugin : pluginData) {
        if (token.IsCancellationRequested()) return {};
        ParsePluginRecordLocations(
            loadedPlugin.data,
            0,
            loadedPlugin.data.size(),
            loadedPlugin.plugin,
            loadedPlugin.masters,
            compileIndexByPluginName,
            worldspaceNames,
            "",
            locations,
            seenFormIds,
            compressedSkipped,
            token);
    }

    Logger::LogInfo(
        "[WORLD_DATA] Plugin map marker scan read %zu/%zu plugins, found %zu locations, skipped %zu compressed records",
        pluginsRead,
        plugins.size(),
        locations.size(),
        compressedSkipped);
    return locations;
}

std::vector<LocationRow> CollectNativeLocations(
    std::vector<XNVSEAdapter::NativeMapMarker> nativeMarkers) {
    std::vector<LocationRow> locations;
    std::set<uint32_t> seenFormIds;
    for (auto& marker : nativeMarkers) {
        if (marker.formId == 0 || !seenFormIds.insert(marker.formId).second) {
            continue;
        }
        LocationRow row;
        row.name = std::move(marker.name);
        row.formId = marker.formId;
        row.formIdHex = FormatHex8(marker.formId);
        row.worldspace = std::move(marker.worldspaceName);
        row.tags = {"MapMarker", MarkerTypeName(marker.markerType)};
        row.isInterior = marker.interior;
        row.x = marker.x;
        row.y = marker.y;
        row.refs = row.formIdHex;
        row.cleared = marker.flags != 0;
        locations.push_back(std::move(row));
    }

    std::sort(locations.begin(), locations.end(), [](const LocationRow& a, const LocationRow& b) {
        return ToLowerCopy(a.name) < ToLowerCopy(b.name);
    });
    return locations;
}

bool CopyLocationResult(const LocationRow& location, unsigned int& formId, char* displayName, unsigned int displayNameSize) {
    formId = location.formId;
    if (displayName && displayNameSize > 0) {
        const std::string name = !location.name.empty() ? location.name : location.formIdHex;
        const size_t copyLength = std::min<size_t>(name.size(), displayNameSize - 1);
        std::memcpy(displayName, name.data(), copyLength);
        displayName[copyLength] = '\0';
    }
    return formId != 0;
}

bool FindLocationInRows(const std::vector<LocationRow>& locations, const std::string& query, unsigned int& formId, char* displayName, unsigned int displayNameSize) {
    const std::string target = Trim(query);
    if (target.empty()) {
        return false;
    }

    const std::string compactTarget = CompactText(target);
    for (const LocationRow& location : locations) {
        if (CompactText(location.name) == compactTarget ||
            CompactText(location.formIdHex) == compactTarget ||
            CompactText(location.refs) == compactTarget) {
            return CopyLocationResult(location, formId, displayName, displayNameSize);
        }
    }

    for (const LocationRow& location : locations) {
        if (ContainsCompact(LocationSearchText(location), target)) {
            return CopyLocationResult(location, formId, displayName, displayNameSize);
        }
    }

    return false;
}

std::string JsonArray(const std::vector<std::string>& values) {
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            json << ",";
        }
        json << "\"" << HTTPManager::EscapeJson(values[i]) << "\"";
    }
    json << "]";
    return json.str();
}

std::string BuildFactionsPayload(const std::vector<FactionRow>& factions) {
    std::ostringstream json;
    json << "{\"type\":\"world_factions\",\"replace\":true,\"factions\":[";
    for (size_t i = 0; i < factions.size(); ++i) {
        if (i > 0) {
            json << ",";
        }
        json << "{";
        json << "\"name\":\"" << HTTPManager::EscapeJson(factions[i].name) << "\",";
        json << "\"formid\":\"" << FormatHex8(factions[i].formId) << "\"";
        json << "}";
    }
    json << "]}";
    return json.str();
}

std::string BuildLocationsPayload(const std::vector<LocationRow>& locations, size_t start, size_t count, bool replace) {
    std::ostringstream json;
    json << "{\"type\":\"world_locations\",\"replace\":" << (replace ? "true" : "false") << ",\"locations\":[";
    const size_t end = std::min(locations.size(), start + count);
    for (size_t i = start; i < end; ++i) {
        if (i > start) {
            json << ",";
        }
        const LocationRow& location = locations[i];
        json << "{";
        json << "\"name\":\"" << HTTPManager::EscapeJson(location.name) << "\",";
        json << "\"formid\":\"" << location.formId << "\",";
        json << "\"formid_hex\":\"" << location.formIdHex << "\",";
        json << "\"worldspace\":\"" << HTTPManager::EscapeJson(location.worldspace) << "\",";
        json << "\"tags\":" << JsonArray(location.tags) << ",";
        json << "\"is_interior\":" << (location.isInterior ? "true" : "false") << ",";
        json << "\"x\":" << location.x << ",";
        json << "\"y\":" << location.y << ",";
        json << "\"refs\":\"" << HTTPManager::EscapeJson(location.refs) << "\",";
        json << "\"cleared\":" << (location.cleared ? "true" : "false");
        json << "}";
    }
    json << "]}";
    return json.str();
}

bool SendWorldData(const std::vector<FactionRow>& factions, const std::vector<LocationRow>& locations,
                   const TaskManager::CancellationToken& token) {
    if (token.IsCancellationRequested()) return false;
    const std::string factionResponse = HTTPManager::SendJson("gamedata.php", BuildFactionsPayload(factions));
    if (factionResponse.find("OK") == std::string::npos) {
        Logger::LogWarning("[WORLD_DATA] Faction upload returned non-OK response");
        return false;
    }

    if (locations.empty()) {
        Logger::LogWarning("[WORLD_DATA] Location scan found zero rows; leaving existing server locations unchanged");
        return true;
    }

    for (size_t start = 0; start < locations.size(); start += kLocationBatchSize) {
        if (token.IsCancellationRequested()) return false;
        const bool replace = start == 0;
        const std::string response = HTTPManager::SendJson(
            "gamedata.php",
            BuildLocationsPayload(locations, start, kLocationBatchSize, replace));
        if (response.find("OK") == std::string::npos) {
            Logger::LogWarning("[WORLD_DATA] Location batch upload returned non-OK response at offset %zu", start);
            return false;
        }
    }

    return true;
}

void TrySyncNow(std::vector<XNVSEAdapter::NativeMapMarker> nativeMarkers) {
    if (g_syncInProgress.exchange(true)) {
        return;
    }

    Console::Print("[Dialectic] Syncing Fallout factions and locations...");
    Logger::LogInfo("[WORLD_DATA] Collecting Fallout factions and map marker locations");
    auto factions = CollectFactions();
    auto nativeLocations = CollectNativeLocations(std::move(nativeMarkers));
    auto plugins = CollectLoadedPluginRows();

    const std::size_t factionCount = factions.size();
    if (!TaskManager::Enqueue("gamedata", "world_data", RuntimeGeneration::Current(), false,
            std::chrono::minutes(2),
            [factions = std::move(factions), nativeLocations = std::move(nativeLocations),
             plugins = std::move(plugins), factionCount](const TaskManager::CancellationToken& token) mutable {
                if (token.IsCancellationRequested()) {
                    g_syncInProgress = false;
                    return;
                }
                std::set<uint32_t> seenFormIds;
                auto locations = CollectLocationsFromPlugins(plugins, seenFormIds, token);
                if (token.IsCancellationRequested()) {
                    g_syncInProgress = false;
                    return;
                }
                for (auto& location : nativeLocations) {
                    if (location.formId != 0 && seenFormIds.insert(location.formId).second) {
                        locations.push_back(std::move(location));
                    }
                }
                std::sort(locations.begin(), locations.end(), [](const LocationRow& a, const LocationRow& b) {
                    return ToLowerCopy(a.name) < ToLowerCopy(b.name);
                });
                {
                    std::lock_guard<std::mutex> lock(g_locationMutex);
                    g_cachedLocations = locations;
                }
                const std::size_t locationCount = locations.size();
                Logger::LogInfo("[WORLD_DATA] Discovered %zu factions and %zu locations",
                    factionCount, locationCount);
                const bool success = SendWorldData(factions, locations, token);
                if (success) {
                    g_completed = true;
                    g_syncRequested = false;
                    Logger::LogInfo("[WORLD_DATA] Synced %zu factions and %zu locations",
                        factionCount, locationCount);
                } else {
                    Logger::LogWarning("[WORLD_DATA] Sync failed");
                }
                g_syncInProgress = false;
            })) {
        Logger::LogWarning("[WORLD_DATA] Could not queue upload task");
        g_syncInProgress = false;
    }
}

} // namespace

void RequestSync() {
    g_completed = false;
    g_syncRequested = true;
    g_nativeMarkerCaptureStarted = false;
    Console::Print("[Dialectic] Queued Fallout faction/location sync");
    Logger::LogInfo("[WORLD_DATA] Sync requested");
}

bool IsSyncing() {
    return g_syncInProgress.load();
}

bool HasCompleted() {
    return g_completed.load();
}

bool ResolveLocationByName(const char* name, unsigned int& formId, char* displayName, unsigned int displayNameSize) {
    formId = 0;
    if (displayName && displayNameSize > 0) {
        displayName[0] = '\0';
    }

    const std::string query = Trim(name ? name : "");
    if (query.empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_locationMutex);
        if (FindLocationInRows(g_cachedLocations, query, formId, displayName, displayNameSize)) {
            Logger::LogInfo("[WORLD_DATA] Resolved location [%s] to cached marker 0x%08X", query.c_str(), formId);
            return true;
        }
    }

    std::vector<XNVSEAdapter::NativeMapMarker> nativeMarkers;
    if (!XNVSEAdapter::CaptureNativeMapMarkers(nativeMarkers)) {
        RequestSync();
        Logger::LogWarning("[WORLD_DATA] Location cache not ready for [%s]; queued incremental sync", query.c_str());
        return false;
    }
    std::vector<LocationRow> locations = CollectNativeLocations(std::move(nativeMarkers));
    {
        std::lock_guard<std::mutex> lock(g_locationMutex);
        g_cachedLocations = locations;
        if (FindLocationInRows(g_cachedLocations, query, formId, displayName, displayNameSize)) {
            Logger::LogInfo("[WORLD_DATA] Resolved location [%s] to marker 0x%08X after on-demand scan", query.c_str(), formId);
            return true;
        }
    }

    Logger::LogWarning("[WORLD_DATA] Could not resolve location [%s] from %zu scanned markers", query.c_str(), locations.size());
    return false;
}

void Update() {
    if (!g_syncRequested.load() || g_syncInProgress.load()) {
        return;
    }

    if (!g_nativeMarkerCaptureStarted) {
        XNVSEAdapter::BeginNativeMapMarkerCapture();
        g_nativeMarkerCaptureStarted = true;
    }

    std::vector<XNVSEAdapter::NativeMapMarker> nativeMarkers;
    bool captureComplete = false;
    if (!XNVSEAdapter::AdvanceNativeMapMarkerCapture(nativeMarkers, captureComplete) ||
        !captureComplete) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    g_lastAttempt = now;
    g_nativeMarkerCaptureStarted = false;
    TrySyncNow(std::move(nativeMarkers));
}

} // namespace WorldDataSyncFNV
