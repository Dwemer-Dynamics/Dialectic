#include "BsaArchiveReader.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {
    template <class T>
    void WriteValue(std::ofstream& stream, T value) {
        stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    bool Expect(bool condition, const char* message) {
        if (!condition) {
            std::cerr << message << '\n';
        }
        return condition;
    }

    std::filesystem::path WriteFixture() {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "dialectic_bsa_reader_test.bsa";
        const std::string folder = "sound\\voice\\falloutnv.esm\\maleadult04\0";
        const std::string fileName = "sample.ogg\0";
        const std::string payload = "OggSdialectic-test-payload";

        constexpr uint32_t headerSize = 36;
        constexpr uint32_t folderRecordSize = 16;
        constexpr uint32_t fileRecordSize = 16;
        const uint32_t physicalFolderOffset = headerSize + folderRecordSize;
        const uint32_t totalFolderNameLength = static_cast<uint32_t>(folder.size());
        const uint32_t fileNamesOffset = headerSize + folderRecordSize + 1 + totalFolderNameLength + fileRecordSize;
        const uint32_t dataOffset = fileNamesOffset + static_cast<uint32_t>(fileName.size());
        const uint32_t virtualFolderOffset = physicalFolderOffset + static_cast<uint32_t>(fileName.size());

        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        WriteValue<uint32_t>(stream, 0x00415342);
        WriteValue<uint32_t>(stream, 104);
        WriteValue<uint32_t>(stream, headerSize);
        WriteValue<uint32_t>(stream, 0x00000003);
        WriteValue<uint32_t>(stream, 1);
        WriteValue<uint32_t>(stream, 1);
        WriteValue<uint32_t>(stream, totalFolderNameLength);
        WriteValue<uint32_t>(stream, static_cast<uint32_t>(fileName.size()));
        WriteValue<uint32_t>(stream, 0x00000018);

        WriteValue<uint64_t>(stream, 0);
        WriteValue<uint32_t>(stream, 1);
        WriteValue<uint32_t>(stream, virtualFolderOffset);

        WriteValue<uint8_t>(stream, static_cast<uint8_t>(folder.size()));
        stream.write(folder.data(), static_cast<std::streamsize>(folder.size()));
        WriteValue<uint64_t>(stream, 0);
        WriteValue<uint32_t>(stream, static_cast<uint32_t>(payload.size()));
        WriteValue<uint32_t>(stream, dataOffset);

        stream.write(fileName.data(), static_cast<std::streamsize>(fileName.size()));
        stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        return path;
    }
}

int main(int argc, char** argv) {
    if (argc == 3) {
        const std::string requested = BsaArchiveReader::NormalizeAssetPath(argv[2]);
        std::unordered_map<std::string, BsaArchiveReader::Entry> entries;
        std::string error;
        if (!BsaArchiveReader::FindEntries(argv[1], { requested }, entries, &error) || entries.empty()) {
            std::cerr << "archive lookup failed: " << error << '\n';
            return 1;
        }
        std::string data;
        if (!BsaArchiveReader::ReadEntry(entries.begin()->second, data, 32 * 1024 * 1024, &error)) {
            std::cerr << "archive read failed: " << error << '\n';
            return 1;
        }
        std::cout << entries.begin()->second.assetPath << " bytes=" << data.size() << '\n';
        return 0;
    }

    const auto fixture = WriteFixture();
    const std::string requested = "sound\\voice\\falloutnv.esm\\maleadult04\\sample.ogg";
    std::unordered_map<std::string, BsaArchiveReader::Entry> entries;
    std::string error;

    bool ok = BsaArchiveReader::FindEntries(fixture.string(), { requested }, entries, &error);
    ok = Expect(ok, error.c_str()) && ok;
    ok = Expect(entries.size() == 1, "expected one matching BSA entry") && ok;

    std::string data;
    if (!entries.empty()) {
        ok = Expect(BsaArchiveReader::ReadEntry(entries.begin()->second, data, 1024, &error), error.c_str()) && ok;
        ok = Expect(data == "OggSdialectic-test-payload", "BSA payload mismatch") && ok;

        auto compressed = entries.begin()->second;
        compressed.compressed = true;
        ok = Expect(!BsaArchiveReader::ReadEntry(compressed, data, 1024, &error),
                    "compressed entry should be rejected safely") && ok;
    }

    std::error_code removeError;
    std::filesystem::remove(fixture, removeError);
    return ok ? 0 : 1;
}
