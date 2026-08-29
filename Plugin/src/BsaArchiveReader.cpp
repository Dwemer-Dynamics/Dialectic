#include "BsaArchiveReader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace BsaArchiveReader {
    namespace {
        constexpr uint32_t kBsaMagic = 0x00415342;
        constexpr uint32_t kArchiveCompressed = 0x00000004;
        constexpr uint32_t kArchiveEmbeddedNames = 0x00000100;
        constexpr uint32_t kFileCompressionToggle = 0x40000000;
        constexpr uint32_t kFileSizeMask = 0x3FFFFFFF;
        constexpr uint64_t kFolderRecordSize = 16;
        constexpr uint64_t kFileRecordSize = 16;

        struct Header {
            uint32_t version = 0;
            uint32_t folderRecordOffset = 0;
            uint32_t archiveFlags = 0;
            uint32_t folderCount = 0;
            uint32_t fileCount = 0;
            uint32_t totalFolderNameLength = 0;
            uint32_t totalFileNameLength = 0;
            uint32_t fileFlags = 0;
        };

        struct FolderRecord {
            uint64_t hash = 0;
            uint32_t fileCount = 0;
            uint32_t offset = 0;
        };

        struct PendingFile {
            std::string folder;
            uint32_t size = 0;
            uint32_t offset = 0;
        };

        template <class T>
        bool ReadValue(std::ifstream& stream, T& value) {
            stream.read(reinterpret_cast<char*>(&value), sizeof(T));
            return stream.good();
        }

        bool Fail(std::string* error, const std::string& message) {
            if (error) {
                *error = message;
            }
            return false;
        }

        std::string ReadNullTerminated(std::ifstream& stream, uint64_t maxEnd) {
            std::string value;
            char ch = 0;
            while (stream.good() && static_cast<uint64_t>(stream.tellg()) < maxEnd && stream.get(ch)) {
                if (ch == '\0') {
                    break;
                }
                value.push_back(ch);
            }
            return value;
        }
    }

    std::string NormalizeAssetPath(std::string value) {
        std::replace(value.begin(), value.end(), '/', '\\');
        while (!value.empty() && (value.front() == '\\' || std::isspace(static_cast<unsigned char>(value.front())))) {
            value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    bool FindEntries(const std::string& archivePath,
                     const std::unordered_set<std::string>& requestedPaths,
                     std::unordered_map<std::string, Entry>& entries,
                     std::string* error) {
        entries.clear();
        if (requestedPaths.empty()) {
            return true;
        }

        std::ifstream stream(archivePath, std::ios::binary);
        if (!stream.is_open()) {
            return Fail(error, "archive_open_failed");
        }

        stream.seekg(0, std::ios::end);
        const std::streamoff archiveSizeSigned = stream.tellg();
        if (archiveSizeSigned < 36) {
            return Fail(error, "archive_too_small");
        }
        const uint64_t archiveSize = static_cast<uint64_t>(archiveSizeSigned);
        stream.seekg(0, std::ios::beg);

        uint32_t magic = 0;
        Header header;
        if (!ReadValue(stream, magic) || magic != kBsaMagic ||
            !ReadValue(stream, header.version) ||
            !ReadValue(stream, header.folderRecordOffset) ||
            !ReadValue(stream, header.archiveFlags) ||
            !ReadValue(stream, header.folderCount) ||
            !ReadValue(stream, header.fileCount) ||
            !ReadValue(stream, header.totalFolderNameLength) ||
            !ReadValue(stream, header.totalFileNameLength) ||
            !ReadValue(stream, header.fileFlags)) {
            return Fail(error, "invalid_bsa_header");
        }
        if (header.version != 103 && header.version != 104) {
            return Fail(error, "unsupported_bsa_version");
        }
        if (header.folderCount > 100000 || header.fileCount > 2000000) {
            return Fail(error, "implausible_bsa_counts");
        }

        const uint64_t folderRecordsEnd = static_cast<uint64_t>(header.folderRecordOffset) +
            static_cast<uint64_t>(header.folderCount) * kFolderRecordSize;
        if (header.folderRecordOffset < 36 || folderRecordsEnd > archiveSize) {
            return Fail(error, "folder_records_out_of_bounds");
        }

        stream.seekg(header.folderRecordOffset, std::ios::beg);
        std::vector<FolderRecord> folders(header.folderCount);
        for (auto& folder : folders) {
            if (!ReadValue(stream, folder.hash) ||
                !ReadValue(stream, folder.fileCount) ||
                !ReadValue(stream, folder.offset)) {
                return Fail(error, "folder_record_read_failed");
            }
        }

        std::vector<PendingFile> pendingFiles;
        pendingFiles.reserve(header.fileCount);
        for (const auto& folder : folders) {
            if (folder.offset < header.totalFileNameLength) {
                return Fail(error, "folder_offset_underflow");
            }
            const uint64_t physicalOffset = static_cast<uint64_t>(folder.offset) - header.totalFileNameLength;
            if (physicalOffset >= archiveSize) {
                return Fail(error, "folder_block_out_of_bounds");
            }
            stream.seekg(static_cast<std::streamoff>(physicalOffset), std::ios::beg);

            uint8_t folderNameLength = 0;
            if (!ReadValue(stream, folderNameLength) || folderNameLength == 0) {
                return Fail(error, "folder_name_read_failed");
            }
            std::string folderName(folderNameLength, '\0');
            stream.read(folderName.data(), folderNameLength);
            if (!stream.good()) {
                return Fail(error, "folder_name_read_failed");
            }
            while (!folderName.empty() && folderName.back() == '\0') {
                folderName.pop_back();
            }

            for (uint32_t i = 0; i < folder.fileCount; ++i) {
                uint64_t fileHash = 0;
                PendingFile file;
                file.folder = folderName;
                if (!ReadValue(stream, fileHash) ||
                    !ReadValue(stream, file.size) ||
                    !ReadValue(stream, file.offset)) {
                    return Fail(error, "file_record_read_failed");
                }
                pendingFiles.push_back(std::move(file));
            }
        }
        if (pendingFiles.size() != header.fileCount) {
            return Fail(error, "file_count_mismatch");
        }

        const uint64_t fileNamesOffset = folderRecordsEnd +
            static_cast<uint64_t>(header.totalFolderNameLength) +
            static_cast<uint64_t>(header.folderCount) +
            static_cast<uint64_t>(header.fileCount) * kFileRecordSize;
        const uint64_t fileNamesEnd = fileNamesOffset + header.totalFileNameLength;
        if (fileNamesEnd > archiveSize) {
            return Fail(error, "file_names_out_of_bounds");
        }
        stream.seekg(static_cast<std::streamoff>(fileNamesOffset), std::ios::beg);

        const bool archiveCompressed = (header.archiveFlags & kArchiveCompressed) != 0;
        const bool embeddedNames = (header.archiveFlags & kArchiveEmbeddedNames) != 0;
        for (const auto& file : pendingFiles) {
            const std::string fileName = ReadNullTerminated(stream, fileNamesEnd);
            if (fileName.empty()) {
                return Fail(error, "file_name_read_failed");
            }
            const std::string assetPath = NormalizeAssetPath(file.folder + "\\" + fileName);
            if (requestedPaths.find(assetPath) == requestedPaths.end()) {
                continue;
            }

            const uint32_t storedSize = file.size & kFileSizeMask;
            const bool compressionToggled = (file.size & kFileCompressionToggle) != 0;
            if (storedSize == 0 || static_cast<uint64_t>(file.offset) + storedSize > archiveSize) {
                continue;
            }
            Entry entry;
            entry.archivePath = archivePath;
            entry.assetPath = assetPath;
            entry.dataOffset = file.offset;
            entry.storedSize = storedSize;
            entry.compressed = archiveCompressed != compressionToggled;
            entry.embeddedName = embeddedNames;
            entries.emplace(assetPath, std::move(entry));
        }

        return true;
    }

    bool ReadEntry(const Entry& entry, std::string& data, size_t maxBytes, std::string* error) {
        data.clear();
        if (entry.compressed) {
            return Fail(error, "compressed_entry_unsupported");
        }
        if (entry.storedSize == 0 || entry.storedSize > maxBytes) {
            return Fail(error, "entry_size_invalid");
        }

        std::ifstream stream(entry.archivePath, std::ios::binary);
        if (!stream.is_open()) {
            return Fail(error, "archive_open_failed");
        }
        stream.seekg(static_cast<std::streamoff>(entry.dataOffset), std::ios::beg);

        uint32_t remaining = entry.storedSize;
        if (entry.embeddedName) {
            uint8_t embeddedNameLength = 0;
            if (!ReadValue(stream, embeddedNameLength) || remaining <= static_cast<uint32_t>(embeddedNameLength) + 1) {
                return Fail(error, "embedded_name_invalid");
            }
            stream.seekg(embeddedNameLength, std::ios::cur);
            remaining -= static_cast<uint32_t>(embeddedNameLength) + 1;
        }
        if (remaining == 0 || remaining > maxBytes) {
            return Fail(error, "entry_payload_size_invalid");
        }

        data.resize(remaining);
        stream.read(data.data(), static_cast<std::streamsize>(remaining));
        if (stream.gcount() != static_cast<std::streamsize>(remaining)) {
            data.clear();
            return Fail(error, "entry_payload_read_failed");
        }
        return true;
    }
}
