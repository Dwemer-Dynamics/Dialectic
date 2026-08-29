#pragma once

#include "BsaArchiveReader.h"

#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace VoiceSampleResolverFNV {

struct ArchiveLookupSummary {
    int archivesAvailable{0};
    int archivesScanned{0};
    int cacheHits{0};
    int entriesFound{0};
    bool timedOut{false};
    bool cancelled{false};
};

std::string BuildOriginalName(const std::string& voiceFile);
std::string BuildDataPath(const std::string& voiceFile);
std::string BuildBundledPath(const std::string& voiceFile);

bool ReadSource(const std::string& loosePath,
                const BsaArchiveReader::Entry* archiveEntry,
                std::string& data,
                std::string* error = nullptr);

bool FindArchiveEntries(const std::unordered_set<std::string>& requestedPaths,
                        std::unordered_map<std::string, BsaArchiveReader::Entry>& entries,
                        ArchiveLookupSummary& summary,
                        std::chrono::steady_clock::time_point deadline = {},
                        const std::function<bool()>& cancelRequested = {});

bool ResolveAndRead(const std::string& voiceFile,
                    std::string& data,
                    std::string& source,
                    std::string* error = nullptr);

void ClearCache();

} // namespace VoiceSampleResolverFNV
