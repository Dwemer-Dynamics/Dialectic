#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace BsaArchiveReader {
    struct Entry {
        std::string archivePath;
        std::string assetPath;
        uint64_t dataOffset = 0;
        uint32_t storedSize = 0;
        bool compressed = false;
        bool embeddedName = false;
    };

    std::string NormalizeAssetPath(std::string value);

    bool FindEntries(const std::string& archivePath,
                     const std::unordered_set<std::string>& requestedPaths,
                     std::unordered_map<std::string, Entry>& entries,
                     std::string* error = nullptr);

    bool ReadEntry(const Entry& entry,
                   std::string& data,
                   size_t maxBytes = 32 * 1024 * 1024,
                   std::string* error = nullptr);
}
