#include "Misc.h"
#include "RuntimeSnapshot.h"
#include "WorldContextFNV.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <cstring>
#include <cmath>

#pragma comment(lib, "advapi32.lib")

namespace Misc {

    std::string Trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    std::string ToLower(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return result;
    }

    std::string ToUpper(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return std::toupper(c); });
        return result;
    }

    long long GetCurrentTimeMillis() {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    }

    std::string GetGameTimeStamp() {
        const long long gameTimestamp = WorldContextFNV::GetGameTimestamp();
        if (gameTimestamp > 0) {
            return std::to_string(gameTimestamp);
        }
        return "0";
    }

    std::string GetPlayerLocation() {
        const std::string location = WorldContextFNV::GetPlayerLocation();
        if (!location.empty()) {
            return location;
        }
        return "Unknown Location";
    }

    std::string GetPlayerName() {
        return Trim(RuntimeSnapshot::GetGameState().playerName);
    }

    uint32_t GetPlayerFormId() {
        return RuntimeSnapshot::GetGameState().playerFormId;
    }

    bool GetPlayerPitchDegrees(float& outPitchDegrees) {
        float pitch = RuntimeSnapshot::GetGameState().playerPitch;
        if (!std::isfinite(pitch)) {
            return false;
        }

        // FNV stores actor rotations in radians. Values may already be normalized,
        // but wrap defensively so the sky threshold remains stable.
        constexpr float kPi = 3.141592654f;
        while (pitch > kPi) {
            pitch -= 2.0f * kPi;
        }
        while (pitch < -kPi) {
            pitch += 2.0f * kPi;
        }

        outPitchDegrees = pitch * (180.0f / kPi);
        return std::isfinite(outPitchDegrees);
    }

    std::string MD5Hash(const std::string& input) {
        HCRYPTPROV hProv = 0;
        HCRYPTHASH hHash = 0;
        BYTE rgbHash[16];
        DWORD cbHash = 16;
        std::stringstream ss;

        if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
            return "";
        }

        if (!CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash)) {
            CryptReleaseContext(hProv, 0);
            return "";
        }

        if (!CryptHashData(hHash, (const BYTE*)input.c_str(), (DWORD)input.length(), 0)) {
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            return "";
        }

        if (!CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &cbHash, 0)) {
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            return "";
        }

        // Convert to hex string
        ss << std::hex << std::setfill('0');
        for (DWORD i = 0; i < cbHash; i++) {
            ss << std::setw(2) << (int)rgbHash[i];
        }

        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);

        return ss.str();
    }
}
