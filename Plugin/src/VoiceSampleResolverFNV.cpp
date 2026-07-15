#include "VoiceSampleResolverFNV.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace VoiceSampleResolverFNV {
namespace {

std::mutex g_cacheMutex;
std::mutex g_scanMutex;
std::vector<std::filesystem::path> g_archives;
bool g_archivesLoaded = false;
std::unordered_map<std::string, BsaArchiveReader::Entry> g_entryCache;
std::unordered_set<std::string> g_missingCache;

std::string Trim(std::string value) {
    const char* whitespace = " \t\r\n";
    const size_t start = value.find_first_not_of(whitespace);
    if (start == std::string::npos) return "";
    return value.substr(start, value.find_last_not_of(whitespace) - start + 1);
}

std::string NormalizeSeparators(std::string value) {
    value = Trim(std::move(value));
    std::replace(value.begin(), value.end(), '/', '\\');
    return value;
}

bool StartsWithInsensitive(const std::string& value, const std::string& prefix) {
    if (value.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool ReadBinaryFile(const std::string& path, std::string& data) {
    data.clear();
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0 || size > 32 * 1024 * 1024) return false;
    file.seekg(0, std::ios::beg);
    data.resize(static_cast<size_t>(size));
    file.read(data.data(), static_cast<std::streamsize>(size));
    return file.good() || file.gcount() == static_cast<std::streamsize>(size);
}

const std::vector<std::filesystem::path>& GetArchives() {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    if (g_archivesLoaded) return g_archives;
    g_archivesLoaded = true;
    std::error_code ec;
    const std::filesystem::path dataRoot("Data");
    if (!std::filesystem::exists(dataRoot, ec) || ec) return g_archives;
    for (std::filesystem::directory_iterator it(
             dataRoot, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::string extension = it->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (it->is_regular_file(ec) && !ec && extension == ".bsa") {
            g_archives.push_back(it->path());
        }
    }
    std::sort(g_archives.begin(), g_archives.end());
    return g_archives;
}

bool DeadlineReached(const std::chrono::steady_clock::time_point& deadline) {
    return deadline != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= deadline;
}

} // namespace

std::string BuildOriginalName(const std::string& voiceFile) {
    std::string path = NormalizeSeparators(voiceFile);
    if (StartsWithInsensitive(path, "Data\\Sound\\Voice\\")) return path.substr(5);
    if (StartsWithInsensitive(path, "Sound\\Voice\\")) return path;
    return "Sound\\Voice\\" + path;
}

std::string BuildDataPath(const std::string& voiceFile) {
    std::string path = NormalizeSeparators(voiceFile);
    if (StartsWithInsensitive(path, "Data\\")) return path;
    if (StartsWithInsensitive(path, "Sound\\Voice\\")) return "Data\\" + path;
    return "Data\\Sound\\Voice\\" + path;
}

std::string BuildBundledPath(const std::string& voiceFile) {
    std::string path = NormalizeSeparators(voiceFile);
    if (StartsWithInsensitive(path, "Data\\Sound\\Voice\\")) path = path.substr(17);
    else if (StartsWithInsensitive(path, "Sound\\Voice\\")) path = path.substr(12);
    else if (StartsWithInsensitive(path, "Data\\")) path = path.substr(5);
    return "Data\\Dialectic\\voice_samples\\" + path;
}

bool ReadSource(const std::string& loosePath,
                const BsaArchiveReader::Entry* archiveEntry,
                std::string& data,
                std::string* error) {
    if (archiveEntry) {
        return BsaArchiveReader::ReadEntry(*archiveEntry, data, 32 * 1024 * 1024, error);
    }
    if (ReadBinaryFile(loosePath, data)) return true;
    if (error) *error = "loose_file_missing_or_empty";
    return false;
}

bool FindArchiveEntries(const std::unordered_set<std::string>& requestedPaths,
                        std::unordered_map<std::string, BsaArchiveReader::Entry>& entries,
                        ArchiveLookupSummary& summary,
                        std::chrono::steady_clock::time_point deadline,
                        const std::function<bool()>& cancelRequested) {
    entries.clear();
    summary = ArchiveLookupSummary{};
    std::unordered_set<std::string> unresolved;
    for (const std::string& rawPath : requestedPaths) {
        const std::string path = BsaArchiveReader::NormalizeAssetPath(rawPath);
        if (!path.empty()) unresolved.insert(path);
    }

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        for (auto it = unresolved.begin(); it != unresolved.end();) {
            const auto found = g_entryCache.find(*it);
            if (found != g_entryCache.end()) {
                entries.emplace(*it, found->second);
                ++summary.cacheHits;
                it = unresolved.erase(it);
            } else if (g_missingCache.find(*it) != g_missingCache.end()) {
                ++summary.cacheHits;
                it = unresolved.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (unresolved.empty()) {
        summary.entriesFound = static_cast<int>(entries.size());
        return !entries.empty();
    }

    std::lock_guard<std::mutex> scanLock(g_scanMutex);
    const auto& archives = GetArchives();
    summary.archivesAvailable = static_cast<int>(archives.size());
    for (const auto& archivePath : archives) {
        if (unresolved.empty()) break;
        if (cancelRequested && cancelRequested()) {
            summary.cancelled = true;
            break;
        }
        if (DeadlineReached(deadline)) {
            summary.timedOut = true;
            break;
        }
        ++summary.archivesScanned;
        std::unordered_map<std::string, BsaArchiveReader::Entry> matches;
        std::string archiveError;
        if (!BsaArchiveReader::FindEntries(archivePath.string(), unresolved, matches, &archiveError)) {
            continue;
        }
        std::lock_guard<std::mutex> cacheLock(g_cacheMutex);
        for (auto& match : matches) {
            g_entryCache[match.first] = match.second;
            entries[match.first] = std::move(match.second);
            unresolved.erase(match.first);
        }
    }

    if (!summary.timedOut && !summary.cancelled) {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_missingCache.insert(unresolved.begin(), unresolved.end());
    }
    summary.entriesFound = static_cast<int>(entries.size());
    return !entries.empty();
}

bool ResolveAndRead(const std::string& voiceFile,
                    std::string& data,
                    std::string& source,
                    std::string* error) {
    const std::string dataPath = BuildDataPath(voiceFile);
    if (ReadBinaryFile(dataPath, data)) {
        source = dataPath;
        return true;
    }
    const std::string bundledPath = BuildBundledPath(voiceFile);
    if (ReadBinaryFile(bundledPath, data)) {
        source = bundledPath;
        return true;
    }

    const std::string originalName = BuildOriginalName(voiceFile);
    const std::string assetPath = BsaArchiveReader::NormalizeAssetPath(originalName);
    std::unordered_map<std::string, BsaArchiveReader::Entry> entries;
    ArchiveLookupSummary summary;
    FindArchiveEntries({assetPath}, entries, summary,
        std::chrono::steady_clock::now() + std::chrono::seconds(45));
    const auto found = entries.find(assetPath);
    if (found == entries.end()) {
        if (error) {
            *error = "voice_asset_not_found;archives=" + std::to_string(summary.archivesAvailable) +
                ";scanned=" + std::to_string(summary.archivesScanned);
        }
        return false;
    }
    if (!ReadSource("", &found->second, data, error)) return false;
    source = found->second.archivePath + "::" + found->second.assetPath;
    return true;
}

void ClearCache() {
    std::lock_guard<std::mutex> scanLock(g_scanMutex);
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_archives.clear();
    g_archivesLoaded = false;
    g_entryCache.clear();
    g_missingCache.clear();
}

} // namespace VoiceSampleResolverFNV
