#include "ImportDataSyncFNV.h"
#include "HTTPManager.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ImportDataSyncFNV {
namespace {
    constexpr std::uintmax_t kMaxCsvImportSize = 10 * 1024 * 1024;

    std::string ToLower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    bool EndsWith(const std::string& value, const std::string& suffix) {
        if (value.size() < suffix.size()) {
            return false;
        }
        return value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::vector<std::filesystem::path> FindBiographyImportFiles(const std::filesystem::path& directoryPath) {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        if (!std::filesystem::exists(directoryPath, ec) || ec) {
            Logger::LogInfo("ImportDataSyncFNV: Dialectic import directory does not exist: %s",
                directoryPath.string().c_str());
            return files;
        }

        for (const auto& entry : std::filesystem::directory_iterator(directoryPath, ec)) {
            if (ec) {
                Logger::LogError("ImportDataSyncFNV: Error scanning import directory: %s", ec.message().c_str());
                break;
            }
            if (!entry.is_regular_file(ec) || ec) {
                continue;
            }
            const std::string filename = ToLower(entry.path().filename().string());
            if (EndsWith(filename, "_bios.csv")) {
                files.push_back(entry.path());
                Logger::LogInfo("ImportDataSyncFNV: Found biography import file: %s", entry.path().filename().string().c_str());
            }
        }
        return files;
    }

    std::string ReadCsvFile(const std::filesystem::path& filePath) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(filePath, ec);
        if (ec) {
            Logger::LogError("ImportDataSyncFNV: Could not stat CSV file %s: %s",
                filePath.string().c_str(),
                ec.message().c_str());
            return "";
        }
        if (size > kMaxCsvImportSize) {
            Logger::LogError("ImportDataSyncFNV: CSV file %s is too large (%llu bytes, max 10MB)",
                filePath.string().c_str(),
                static_cast<unsigned long long>(size));
            return "";
        }

        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            Logger::LogError("ImportDataSyncFNV: Failed to open CSV file: %s", filePath.string().c_str());
            return "";
        }

        return std::string(
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>());
    }
}

void DetectAndUploadImportDataFiles(const std::function<bool()>& cancelRequested) {
    Logger::LogInfo("ImportDataSyncFNV: Starting CSV import data detection");

    const std::filesystem::path dialecticPath = std::filesystem::path("Data") / "Dialectic";
    const std::vector<std::filesystem::path> biographyFiles = FindBiographyImportFiles(dialecticPath);
    if (biographyFiles.empty()) {
        Logger::LogInfo("ImportDataSyncFNV: No *_bios.csv files found in %s", dialecticPath.string().c_str());
        return;
    }

    Logger::LogInfo("ImportDataSyncFNV: Found %zu biography import file(s)", biographyFiles.size());
    for (const auto& filePath : biographyFiles) {
        if (cancelRequested && cancelRequested()) {
            Logger::LogInfo("ImportDataSyncFNV: CSV import detection cancelled");
            return;
        }
        const std::string csvContent = ReadCsvFile(filePath);
        if (csvContent.empty()) {
            continue;
        }

        const std::string filename = filePath.filename().string();
        const std::string response = HTTPManager::UploadCSVFile(csvContent, filename, "biography_import");
        if (!response.empty()) {
            Logger::LogInfo("ImportDataSyncFNV: Successfully uploaded biography import file %s (response=%s)",
                filename.c_str(),
                response.c_str());
        } else {
            Logger::LogError("ImportDataSyncFNV: Failed to upload biography import file %s", filename.c_str());
        }
    }
}

}
