#include "VoiceSampleBatchUploadFNV.h"
#include "BsaArchiveReader.h"
#include "HTTPManager.h"
#include "VoiceSampleOverridesFNV.h"
#include "VoiceSampleResolverFNV.h"
#include "Console.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

void Log(const char* fmt, ...);

namespace VoiceSampleBatchUploadFNV {
    namespace {
        constexpr auto kBatchTimeout = std::chrono::minutes(5);

        struct VoiceSampleCandidate {
            std::string voiceType;
            std::string dataPath;
            std::string originalName;
            std::string transcript;
            std::string source;
            uintmax_t fileSize = 0;
            BsaArchiveReader::Entry archiveEntry;
            bool hasArchiveEntry = false;
        };

        std::string Trim(const std::string& value) {
            const char* whitespace = " \t\r\n";
            const size_t start = value.find_first_not_of(whitespace);
            if (start == std::string::npos) {
                return "";
            }
            const size_t end = value.find_last_not_of(whitespace);
            return value.substr(start, end - start + 1);
        }

        std::string ToLower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return value;
        }

        bool StartsWithInsensitive(const std::string& value, const std::string& prefix) {
            if (value.size() < prefix.size()) {
                return false;
            }
            for (size_t i = 0; i < prefix.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(value[i])) !=
                    std::tolower(static_cast<unsigned char>(prefix[i]))) {
                    return false;
                }
            }
            return true;
        }

        std::string NormalizePathSeparators(std::string value) {
            value = Trim(value);
            std::replace(value.begin(), value.end(), '/', '\\');
            return value;
        }

        bool ReadCandidateAudio(const VoiceSampleCandidate& candidate, std::string& data) {
            std::string error;
            if (VoiceSampleResolverFNV::ReadSource(
                    candidate.dataPath,
                    candidate.hasArchiveEntry ? &candidate.archiveEntry : nullptr,
                    data,
                    &error)) {
                return true;
            }
            if (candidate.hasArchiveEntry) {
                Log("[VOICE_BATCH] Could not read archive sample %s from %s: %s",
                    candidate.originalName.c_str(),
                    candidate.archiveEntry.archivePath.c_str(),
                    error.c_str());
            }
            return false;
        }

        bool TimedOut(const std::chrono::steady_clock::time_point& deadline) {
            return std::chrono::steady_clock::now() >= deadline;
        }

        bool IsVoiceAudioExtension(const std::filesystem::path& filePath) {
            const std::string ext = ToLower(filePath.extension().string());
            return ext == ".ogg" || ext == ".wav" || ext == ".xwm" || ext == ".fuz";
        }

        bool TryGetFileSize(const std::string& path, uintmax_t& size) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec) || ec) {
                return false;
            }

            size = std::filesystem::file_size(path, ec);
            return !ec && size > 0;
        }

        bool PreferReadablePath(VoiceSampleCandidate& candidate, const std::string& candidatePath) {
            uintmax_t size = 0;
            if (!TryGetFileSize(candidatePath, size)) {
                return false;
            }

            candidate.dataPath = candidatePath;
            candidate.fileSize = size;
            return true;
        }

        std::string VoiceTypeFromOriginalName(const std::string& originalName) {
            const std::string path = NormalizePathSeparators(originalName);
            const std::string lowerPath = ToLower(path);
            const std::string prefix = "sound\\voice\\";
            if (!StartsWithInsensitive(lowerPath, prefix)) {
                return "";
            }

            const size_t pluginSep = path.find('\\', prefix.size());
            if (pluginSep == std::string::npos) {
                return "";
            }

            const size_t voiceSep = path.find('\\', pluginSep + 1);
            if (voiceSep == std::string::npos || voiceSep <= pluginSep + 1) {
                return "";
            }

            return ToLower(path.substr(pluginSep + 1, voiceSep - pluginSep - 1));
        }

        std::string OriginalNameFromDiskPath(const std::filesystem::path& filePath) {
            std::string path = NormalizePathSeparators(filePath.lexically_normal().string());
            const std::string lowerPath = ToLower(path);
            const std::string dataPrefix = "data\\";
            const size_t dataPos = lowerPath.find(dataPrefix);
            if (dataPos != std::string::npos) {
                path = path.substr(dataPos + dataPrefix.size());
            }

            if (StartsWithInsensitive(path, "Sound\\Voice\\")) {
                return path;
            }
            return "";
        }

        void UpsertCandidate(std::unordered_map<std::string, VoiceSampleCandidate>& candidates,
                             const VoiceSampleCandidate& candidate) {
            if (candidate.voiceType.empty() || candidate.originalName.empty()) {
                return;
            }

            auto it = candidates.find(candidate.voiceType);
            if (it == candidates.end()) {
                candidates.emplace(candidate.voiceType, candidate);
                return;
            }

            const bool candidateHasFile = candidate.fileSize > 0;
            const bool existingHasFile = it->second.fileSize > 0;
            const bool candidateIsLoose = candidateHasFile && !candidate.hasArchiveEntry;
            const bool existingIsLoose = existingHasFile && !it->second.hasArchiveEntry;
            if ((candidateIsLoose && !existingIsLoose) ||
                (candidateHasFile && !existingHasFile) ||
                (candidateHasFile && existingHasFile && candidateIsLoose == existingIsLoose &&
                 candidate.fileSize > it->second.fileSize)) {
                it->second = candidate;
            }
        }

        void CollectArchiveCandidates(std::unordered_map<std::string, VoiceSampleCandidate>& candidates,
                                      int& archiveMappings,
                                      const std::chrono::steady_clock::time_point& deadline,
                                      bool& timedOut,
                                      const std::function<bool()>& cancelRequested) {
            std::unordered_map<std::string, std::string> requestedVoiceByPath;
            std::unordered_set<std::string> requestedPaths;
            for (const auto& pair : candidates) {
                if (pair.second.fileSize > 0) {
                    continue;
                }
                const std::string normalizedPath = BsaArchiveReader::NormalizeAssetPath(pair.second.originalName);
                if (!normalizedPath.empty()) {
                    requestedVoiceByPath[normalizedPath] = pair.first;
                    requestedPaths.insert(normalizedPath);
                }
            }
            if (requestedPaths.empty()) {
                return;
            }

            std::unordered_map<std::string, BsaArchiveReader::Entry> matches;
            VoiceSampleResolverFNV::ArchiveLookupSummary lookup;
            VoiceSampleResolverFNV::FindArchiveEntries(
                requestedPaths, matches, lookup, deadline, cancelRequested);
            timedOut = lookup.timedOut;
            Log("[VOICE_BATCH] Archive lookup available=%d scanned=%d cache_hits=%d found=%d timed_out=%d cancelled=%d",
                lookup.archivesAvailable,
                lookup.archivesScanned,
                lookup.cacheHits,
                lookup.entriesFound,
                lookup.timedOut ? 1 : 0,
                lookup.cancelled ? 1 : 0);
            for (auto& match : matches) {
                const auto voiceIt = requestedVoiceByPath.find(match.first);
                if (voiceIt == requestedVoiceByPath.end()) continue;
                auto candidateIt = candidates.find(voiceIt->second);
                if (candidateIt == candidates.end() || candidateIt->second.fileSize > 0) continue;
                candidateIt->second.archiveEntry = std::move(match.second);
                candidateIt->second.hasArchiveEntry = true;
                candidateIt->second.fileSize = candidateIt->second.archiveEntry.storedSize;
                candidateIt->second.source += ":bsa:" +
                    std::filesystem::path(candidateIt->second.archiveEntry.archivePath).filename().string();
                ++archiveMappings;
            }
        }

        std::unordered_map<std::string, VoiceSampleCandidate> CollectVoiceSampleCandidates(
            int& csvMappings,
            int& looseMappings,
            int& archiveMappings,
            const std::chrono::steady_clock::time_point& deadline,
            bool& timedOut,
            const std::function<bool()>& cancelRequested) {
            std::unordered_map<std::string, VoiceSampleCandidate> candidates;

            VoiceSampleOverridesFNV::ReloadVoiceCSVData();
            const auto csvOverrides = VoiceSampleOverridesFNV::GetAllVoiceSampleOverrides();
            csvMappings = static_cast<int>(csvOverrides.size());
            for (const auto& mapping : csvOverrides) {
                if (cancelRequested && cancelRequested()) return candidates;
                VoiceSampleCandidate candidate;
                candidate.voiceType = ToLower(Trim(mapping.voiceType));
                candidate.dataPath = VoiceSampleResolverFNV::BuildDataPath(mapping.voiceFile);
                candidate.originalName = VoiceSampleResolverFNV::BuildOriginalName(mapping.voiceFile);
                candidate.transcript = mapping.transcript;
                candidate.source = "csv:" + mapping.sourceFile;
                if (!PreferReadablePath(candidate, candidate.dataPath)) {
                    const std::string bundledPath = VoiceSampleResolverFNV::BuildBundledPath(mapping.voiceFile);
                    if (PreferReadablePath(candidate, bundledPath)) {
                        candidate.source += ":bundled";
                    }
                }
                UpsertCandidate(candidates, candidate);
            }

            const std::filesystem::path voiceRoot = std::filesystem::path("Data") / "Sound" / "Voice";
            std::error_code ec;
            if (!std::filesystem::exists(voiceRoot, ec) || ec) {
                Log("[VOICE_BATCH] Loose voice root not found: %s", voiceRoot.string().c_str());
            } else {
                for (std::filesystem::recursive_directory_iterator it(
                     voiceRoot,
                     std::filesystem::directory_options::skip_permission_denied,
                     ec),
                 end;
                 it != end;
                 it.increment(ec)) {
                if (cancelRequested && cancelRequested()) {
                    break;
                }
                if (TimedOut(deadline)) {
                    timedOut = true;
                    break;
                }

                if (ec) {
                    ec.clear();
                    continue;
                }

                if (!it->is_regular_file(ec) || ec || !IsVoiceAudioExtension(it->path())) {
                    ec.clear();
                    continue;
                }

                const std::string originalName = OriginalNameFromDiskPath(it->path());
                if (originalName.empty()) {
                    continue;
                }

                const std::string voiceType = VoiceTypeFromOriginalName(originalName);
                if (voiceType.empty()) {
                    continue;
                }

                VoiceSampleCandidate candidate;
                candidate.voiceType = voiceType;
                candidate.dataPath = NormalizePathSeparators(it->path().lexically_normal().string());
                candidate.originalName = originalName;
                candidate.source = "loose";
                std::error_code sizeEc;
                candidate.fileSize = std::filesystem::file_size(it->path(), sizeEc);
                if (sizeEc || candidate.fileSize == 0) {
                    continue;
                }

                ++looseMappings;
                UpsertCandidate(candidates, candidate);
                }
            }

            CollectArchiveCandidates(candidates, archiveMappings, deadline, timedOut, cancelRequested);

            return candidates;
        }
    }

    BatchUploadResult SendAllVoiceSamples(BatchUploadSummary& summary,
                                          const std::function<bool()>& cancelRequested) {
        summary = BatchUploadSummary{};
        const auto deadline = std::chrono::steady_clock::now() + kBatchTimeout;
        Log("[VOICE_BATCH] Request accepted; loading Fallout voice mappings");

        bool scanTimedOut = false;
        auto candidates = CollectVoiceSampleCandidates(
            summary.csvMappings,
            summary.looseMappings,
            summary.archiveMappings,
            deadline,
            scanTimedOut,
            cancelRequested);
        summary.totalMappings = static_cast<int>(candidates.size());
        summary.timedOut = scanTimedOut;
        summary.cancelled = cancelRequested && cancelRequested();
        Log("[VOICE_BATCH] Candidate scan complete mappings=%d csv=%d loose_files_seen=%d archive_samples=%d timed_out=%d cancelled=%d",
            summary.totalMappings,
            summary.csvMappings,
            summary.looseMappings,
            summary.archiveMappings,
            summary.timedOut ? 1 : 0,
            summary.cancelled ? 1 : 0);
        if (summary.cancelled) return BatchUploadResult::Cancelled;

        if (candidates.empty()) {
            Log("[VOICE_BATCH] No Fallout voice sample mappings were loaded");
            Console::Print("[Dialectic] No voice sample mappings found");
            return summary.timedOut ? BatchUploadResult::TimedOut : BatchUploadResult::NoSamplesUploaded;
        }

        Log("[VOICE_BATCH] Starting upload of %d Fallout voice sample mappings (csv=%d loose_files_seen=%d archive_samples=%d)",
            summary.totalMappings,
            summary.csvMappings,
            summary.looseMappings,
            summary.archiveMappings);
        Console::Print("[Dialectic] Uploading Fallout voice samples...");

        std::vector<VoiceSampleCandidate> orderedCandidates;
        orderedCandidates.reserve(candidates.size());
        for (const auto& pair : candidates) {
            orderedCandidates.push_back(pair.second);
        }
        std::sort(orderedCandidates.begin(), orderedCandidates.end(),
            [](const VoiceSampleCandidate& left, const VoiceSampleCandidate& right) {
                return left.voiceType < right.voiceType;
            });

        for (const auto& candidate : orderedCandidates) {
            if (cancelRequested && cancelRequested()) {
                summary.cancelled = true;
                break;
            }
            if (TimedOut(deadline)) {
                summary.timedOut = true;
                break;
            }

            std::string audioData;
            if (!ReadCandidateAudio(candidate, audioData)) {
                ++summary.missing;
                Log("[VOICE_BATCH] Missing voice sample for %s: %s",
                    candidate.voiceType.c_str(),
                    candidate.hasArchiveEntry ? candidate.archiveEntry.archivePath.c_str() : candidate.dataPath.c_str());
                continue;
            }

            const std::string response = HTTPManager::UploadVoiceSample(
                audioData,
                candidate.voiceType,
                candidate.originalName,
                candidate.transcript);
            if (response.empty()) {
                ++summary.failed;
                Log("[VOICE_BATCH] Upload failed for %s from %s",
                    candidate.voiceType.c_str(),
                    candidate.originalName.c_str());
                continue;
            }

            ++summary.uploaded;
            Log("[VOICE_BATCH] Uploaded %s from %s (%s, %llu bytes)",
                candidate.voiceType.c_str(),
                candidate.originalName.c_str(),
                candidate.source.c_str(),
                static_cast<unsigned long long>(candidate.fileSize));
        }

        Log("[VOICE_BATCH] Complete: mappings=%d csv=%d loose_files_seen=%d archive_samples=%d uploaded=%d missing=%d failed=%d timedOut=%d cancelled=%d",
            summary.totalMappings,
            summary.csvMappings,
            summary.looseMappings,
            summary.archiveMappings,
            summary.uploaded,
            summary.missing,
            summary.failed,
            summary.timedOut ? 1 : 0,
            summary.cancelled ? 1 : 0);
        if (summary.cancelled) return BatchUploadResult::Cancelled;
        Console::Print("[Dialectic] Voice samples: %d uploaded, %d missing, %d failed",
            summary.uploaded,
            summary.missing,
            summary.failed);

        if (summary.timedOut) {
            return BatchUploadResult::TimedOut;
        }
        return summary.uploaded > 0 ? BatchUploadResult::Success : BatchUploadResult::NoSamplesUploaded;
    }
}
