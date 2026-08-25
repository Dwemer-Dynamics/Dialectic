#include "Config.h"
#include "Logger.h"
#include "VoiceRecorder.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

// Forward declare logging function from main.cpp for backwards compatibility
void Log(const char* fmt, ...);

namespace Config {
    static constexpr const char* kDefaultServerHost = "127.0.0.1";
    static constexpr int kDefaultServerPort = 8085;
    static constexpr const char* kDefaultServerPath = "DialecticServer/main.php";

    // Server configuration
    std::string serverHost = kDefaultServerHost;
    int serverPort = kDefaultServerPort;
    std::string serverPath = kDefaultServerPath;
    std::string localSoundcachePath = "";
    
    // Player configuration
    std::string playerName = "";
    
    // Audio configuration
    float preClipMs = 100.0f;
    float postClipMs = 100.0f;
    int animationResolution = 500;
    float animationIntensity = 1.0f;
    float voiceVolume = 75.0f;
    float headVoiceVolume = 100.0f;
    bool audio3DPlaybackEnabled = true;
    float audio3DPanStrength = 1.0f;
    bool audioInvertHeading = false;
    float audioDistanceScale = 2.0f;
    float audioPlaybackDropoffInteriorPercent = 70.0f;
    float audioPlaybackDropoffExteriorPercent = 70.0f;
    
    // Behavior configuration
    bool enableCombatDialogue = false;
    bool cancelDialogueOnCombat = true;
    int aiResponseTimeout = 30;
    bool pauseDialogueOnMenu = true;
    bool suppressVanillaDialogueDuringAI = true;
    bool faceTargetDuringAIResponse = true;
    bool showAISubtitles = true;
    bool sceneSafetyEnabled = true;

    // Vanilla/radiant dialogue capture configuration
    bool dialogueCaptureEnabled = true;
    bool dialogueCaptureRadiant = true;
    bool dialogueCaptureDialogueMenu = true;
    bool dialogueCapturePlayerMenuChoices = true;
    int dialogueCaptureRepeatWindowMs = 4000;
    bool dialogueCaptureRequireKnownSpeaker = false;
    
    // Voice recording configuration
    int silenceThreshold = 500;
    int maxRecordingSeconds = 60;
    std::string voiceRecordingPreferredDeviceName = "Windows default";
    std::string voiceRecordingDetectedEndpointId;
    bool voiceRecordingSaveLastWav = false;
    bool openMicEnabled = false;
    float openMicSensitivity = 1000.0f;
    float openMicEndDelaySeconds = 1.0f;
    bool openMicMuted = false;
    
    // Distance thresholds
    float distanceActivatingNpcInterior = 1200.0f;
    float distanceActivatingNpcExterior = 2400.0f;

    // Auto-activation configuration
    bool autoActivateEnabled = true;
    bool autoAddHostile = false;
    bool autoAddCreatures = false;

    // RPG event trigger configuration
    bool rpgCombatBarksEnabled = true;
    int rpgCombatBarkPeriodSeconds = 30;

    // Spatial audio configuration
    bool spatialAudioEnabled = true;
    float spatialMaxAirDistance = 5600.0f;
    float spatialImmediateDistance = 210.0f;
    float spatialAutoHearingDistance = 784.0f;
    float spatialDistanceScaler = 1.0f;
    float spatialInteriorHearingDistance = 1050.0f;
    float spatialExteriorHearingDistance = 1750.0f;
    float spatialMinDistanceFactor = 0.1f;
    float spatialInteriorBaseModifier = 1.0f;
    float spatialExteriorBaseModifier = 0.7f;
    float spatialOpenDoorPenaltyBase = 0.85f;
    float spatialAroundCornerPenalty = 0.60f;
    float spatialDoorTriangulationPercentTolerance = 0.20f;
    float spatialDoorTriangulationAbsoluteTolerance = 80.0f;
    float spatialMinimumAudibleVolume = 0.15f;
    float spatialPathRatioReject = 4.0f;
    float spatialPathRatioDistanceReject = 2.5f;
    float spatialPathRatioDistanceRejectMinAir = 500.0f;
    float spatialPathComplexityStartRatio = 1.2f;
    float spatialPathComplexityScale = 0.6f;
    float spatialPathComplexityMin = 0.3f;
    float spatialNavmeshSnapDistance = 450.0f;
    bool spatialAllowPathUnavailableFallback = true;

    // World context configuration
    bool worldContextEnabled = true;
    float worldContextUpdateSeconds = 60.0f;
    bool worldContextSendOnChange = true;
    bool worldContextSendBeforePlayerInput = true;
    bool worldContextIncludeWeather = true;
    bool worldContextIncludeCell = true;
    bool worldContextIncludeWorldspace = true;

    // Nearby actor snapshot configuration
    bool nearbyActorsEnabled = true;
    float nearbyActorsUpdateSeconds = 5.0f;
    bool nearbyActorsSendOnChange = true;
    bool nearbyActorsSendBeforePlayerInput = true;
    int nearbyActorsMaxActors = 12;
    float nearbyActorsMaxDistance = 2400.0f;

    // Activity status snapshot configuration
    bool activityStatusEnabled = true;
    float activityStatusUpdateSeconds = 5.0f;
    bool activityStatusSendOnChange = true;
    bool activityStatusSendBeforePlayerInput = true;
    int activityStatusMaxActors = 16;
    float activityStatusMaxDistance = 2400.0f;

    // Nearby item snapshot configuration
    bool nearbyItemsEnabled = true;
    float nearbyItemsUpdateSeconds = 5.0f;
    bool nearbyItemsSendOnChange = true;
    bool nearbyItemsSendBeforePlayerInput = true;
    int nearbyItemsMaxItems = 32;
    float nearbyItemsMaxDistance = 1000.0f;
    bool nearbyItemsIncludeStealing = true;
    bool nearbyItemsIncludeLookingAt = true;
    bool nearbyItemsIncludeHeldItem = true;
    bool nearbyItemsHeldItemPriority = true;

    // Points of interest snapshot configuration
    bool pointsOfInterestEnabled = true;
    float pointsOfInterestUpdateSeconds = 60.0f;
    bool pointsOfInterestSendOnChange = true;
    bool pointsOfInterestSendBeforePlayerInput = true;
    int pointsOfInterestMaxPois = 16;
    float pointsOfInterestMaxDistance = 1600.0f;
    bool pointsOfInterestIncludeDoors = true;
    bool pointsOfInterestIncludeLocked = true;
    bool pointsOfInterestIncludeLookingAt = true;

    // Dynamic profile trigger configuration
    int dynamicProfileTimerMinutes = 30;
    bool dynamicProfileTimerIncludeNarrator = true;

    // Bored/idle event trigger configuration
    bool boredEventsEnabled = true;
    int boredEventTimerSeconds = 60;
    bool boredAvoidInMenu = true;
    bool boredAvoidInDialogue = true;
    bool boredAvoidInCombat = true;
    bool boredAvoidWhenSneaking = true;
    bool boredAvoidWhenVoiceInputActive = true;
    int boredRecentSpeechCooldownSeconds = 20;

    // Narrator interaction configuration
    bool narratorModeEnabled = false;

    // Mode selector configuration
    int currentModeIndex = 0;
    std::string currentMode = "STANDARD";
    int currentProfileModelSlot = 1;

    // Rechat launch policy configuration
    bool rechatEnabled = true;
    int rechatMaxDepth = 10;
    bool rechatSmartLaunch = true;
    bool rechatRetryOnEmpty = true;
    bool rechatAvoidWhenSneaking = true;
    bool rechatAvoidInMenu = true;
    int rechatEndConversationCooldown = 60;
    
    // Exclusion lists (script/INI controlled)
    std::unordered_set<std::string> excludedRaces;
    std::unordered_set<std::string> excludedNPCNames;
    std::unordered_set<uint32_t> excludedFormIDs;

    // Helper to convert string to lowercase for case-insensitive comparison
    static std::string ToLower(const std::string& str) {
        std::string result = str;
        for (char& c : result) {
            if (c >= 'A' && c <= 'Z') c += 32;
        }
        return result;
    }

    const char* GetDefaultINIPath() {
        return "Data\\NVSE\\Plugins\\dialectic.ini";
    }

    const char* GetCustomINIPath() {
        return "Data\\NVSE\\Plugins\\dialectic_custom.ini";
    }

    static std::string Trim(const std::string& str);

    static bool TryReadCustomINIValue(const char* section, const char* key, std::string& valueOut) {
        constexpr const char* kMissingValue = "__DIALECTIC_MISSING__";
        char customValue[1024] = {};
        GetPrivateProfileStringA(
            section,
            key,
            kMissingValue,
            customValue,
            static_cast<DWORD>(sizeof(customValue)),
            GetCustomINIPath());
        if (std::strcmp(customValue, kMissingValue) == 0) {
            return false;
        }

        valueOut = customValue;
        return true;
    }

    int ReadINIInt(const char* section, const char* key, int fallback) {
        std::string customValue;
        if (TryReadCustomINIValue(section, key, customValue)) {
            try {
                return std::stoi(Trim(customValue));
            } catch (...) {
                Logger::LogWarning("Invalid custom INI integer [%s] %s=%s", section, key, customValue.c_str());
            }
        }

        return GetPrivateProfileIntA(section, key, fallback, GetDefaultINIPath());
    }

    bool WriteCustomINIValue(const char* section, const char* key, const char* value) {
        return WritePrivateProfileStringA(section, key, value, GetCustomINIPath()) != FALSE;
    }

    static void EnsureCustomINIExists() {
        std::ifstream existing(GetCustomINIPath());
        if (existing.is_open()) {
            return;
        }

        std::ofstream customFile(GetCustomINIPath(), std::ios::out | std::ios::trunc);
        if (!customFile.is_open()) {
            Logger::LogWarning("Could not create user configuration at %s", GetCustomINIPath());
            return;
        }

        customFile
            << "; DIALECTIC user overrides\n"
            << "; This file is created and maintained locally. Mod updates replace\n"
            << "; dialectic.ini defaults but must not replace this custom file.\n\n";
        Logger::LogInfo("Created user configuration: %s", GetCustomINIPath());
    }

    // Collapses the three retired selector bindings into the new control hotkey once.
    static void MigrateDialecticControlHotkey() {
        std::string configuredValue;
        if (TryReadCustomINIValue("Hotkeys", "DialecticControl", configuredValue)) {
            return;
        }

        static constexpr const char* kLegacyKeys[] = {
            "ToggleModes",
            "ToggleLLMModel",
            "DynamicProfileMenu"
        };

        int migratedScanCode = 0;
        const char* migratedFrom = nullptr;
        for (const char* legacyKey : kLegacyKeys) {
            const int scanCode = ReadINIInt("Hotkeys", legacyKey, 0);
            if (scanCode > 0) {
                migratedScanCode = scanCode;
                migratedFrom = legacyKey;
                break;
            }
        }

        const std::string value = std::to_string(migratedScanCode);
        if (!WriteCustomINIValue("Hotkeys", "DialecticControl", value.c_str())) {
            Logger::LogWarning("Could not persist migrated Dialectic Control hotkey");
            return;
        }

        if (migratedFrom) {
            Logger::LogInfo("Migrated [Hotkeys] %s=%d to DialecticControl",
                migratedFrom,
                migratedScanCode);
        }
    }

    static bool ParsePort(const std::string& rawPort, int& portOut, const char* source) {
        try {
            const int parsed = std::stoi(Trim(rawPort));
            if (parsed <= 0 || parsed > 65535) {
                Logger::LogError("%s: Invalid port range: %s", source, rawPort.c_str());
                return false;
            }
            portOut = parsed;
            return true;
        } catch (...) {
            Logger::LogError("%s: Invalid port number: %s", source, rawPort.c_str());
            return false;
        }
    }

    static bool ReadCustomServerOverride(std::string& hostOut, int& portOut, std::string& pathOut) {
        const std::string customIniPath = GetCustomINIPath();
        std::ifstream customFile(customIniPath);
        if (!customFile.is_open()) {
            return false;
        }

        Logger::LogInfo("Reading custom Dialectic server override: %s", customIniPath.c_str());

        std::string currentSection;
        std::string host;
        std::string portText;
        std::string path;
        std::string line;

        while (std::getline(customFile, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') {
                continue;
            }

            if (line[0] == '[' && line[line.length() - 1] == ']') {
                currentSection = line.substr(1, line.length() - 2);
                std::transform(currentSection.begin(), currentSection.end(), currentSection.begin(), [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
                continue;
            }

            const size_t equalsPos = line.find('=');
            if (equalsPos == std::string::npos) {
                continue;
            }

            std::string key = Trim(line.substr(0, equalsPos));
            std::string value = Trim(line.substr(equalsPos + 1));
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });

            if (!currentSection.empty() && currentSection != "SERVER") {
                continue;
            }

            if (key == "SERVER" || key == "HOST") {
                host = value;
            } else if (key == "PORT") {
                portText = value;
            } else if (key == "PATH") {
                path = value;
            }
        }

        customFile.close();

        if (host.empty() && portText.empty() && path.empty()) {
            Logger::LogDebug("No custom server override configured; auto-discovery remains enabled");
            return false;
        }

        if (!host.empty()) {
            hostOut = host;
        }

        if (!portText.empty() && !ParsePort(portText, portOut, "Custom server override")) {
            return false;
        }

        if (!path.empty()) {
            pathOut = path;
        }

        if (hostOut.empty() || portOut <= 0) {
            Logger::LogError("Custom server override did not resolve to a valid host and port");
            return false;
        }

        Logger::LogInfo("Using custom server override: http://%s:%d/%s",
            hostOut.c_str(),
            portOut,
            pathOut.c_str());
        return true;
    }

    static bool IsTcpEndpointReachable(const std::string& host, int port, int timeoutMs) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* result = nullptr;
        const std::string portText = std::to_string(port);
        if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0 || !result) {
            return false;
        }

        bool reachable = false;
        for (addrinfo* ptr = result; ptr != nullptr && !reachable; ptr = ptr->ai_next) {
            SOCKET testSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
            if (testSocket == INVALID_SOCKET) {
                continue;
            }

            u_long nonBlocking = 1;
            ioctlsocket(testSocket, FIONBIO, &nonBlocking);

            int connectResult = connect(testSocket, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen));
            if (connectResult == 0) {
                reachable = true;
            } else {
                const int error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINVAL) {
                    fd_set writeSet;
                    fd_set errorSet;
                    FD_ZERO(&writeSet);
                    FD_ZERO(&errorSet);
                    FD_SET(testSocket, &writeSet);
                    FD_SET(testSocket, &errorSet);

                    timeval timeout{};
                    timeout.tv_sec = timeoutMs / 1000;
                    timeout.tv_usec = (timeoutMs % 1000) * 1000;

                    const int selected = select(0, nullptr, &writeSet, &errorSet, &timeout);
                    if (selected > 0 && FD_ISSET(testSocket, &writeSet)) {
                        int socketError = 0;
                        int socketErrorLen = sizeof(socketError);
                        if (getsockopt(testSocket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &socketErrorLen) == 0 &&
                            socketError == 0) {
                            reachable = true;
                        }
                    }
                }
            }

            closesocket(testSocket);
        }

        freeaddrinfo(result);
        return reachable;
    }

    static bool ApplyDiscoveryBody(const std::string& body, const char* gameKey) {
        // Launcher discovery returns HOST:PORT, e.g. 172.20.10.2:8088.
        size_t colonPos = body.find(':');
        if (colonPos == std::string::npos) {
            Logger::LogError("Auto-discovery: Invalid response for game=%s: %s", gameKey, body.c_str());
            return false;
        }

        std::string discoveredHost = Trim(body.substr(0, colonPos));
        std::string portStr = Trim(body.substr(colonPos + 1));

        if (discoveredHost.empty() || portStr.empty()) {
            Logger::LogError("Auto-discovery: Empty server or port for game=%s", gameKey);
            return false;
        }

        int discoveredPort = 0;
        if (!ParsePort(portStr, discoveredPort, "Auto-discovery")) {
            return false;
        }

        if (!IsTcpEndpointReachable(discoveredHost, discoveredPort, 750)) {
            Logger::LogWarning("Auto-discovery: Rejected unreachable endpoint for game=%s: %s:%d",
                gameKey,
                discoveredHost.c_str(),
                discoveredPort);
            return false;
        }

        serverHost = discoveredHost;
        serverPort = discoveredPort;

        Logger::LogInfo("Auto-discovery successful for game=%s: %s:%d", gameKey, serverHost.c_str(), serverPort);
        return true;
    }

    static bool DiscoverServerFromProxyForGame(const char* gameKey) {
        Logger::LogDebug("Attempting auto-discovery via local proxy (port 7135, game=%s)...", gameKey);
        
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            Logger::LogError("Auto-discovery: WSAStartup failed");
            return false;
        }

        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) {
            Logger::LogError("Auto-discovery: Socket creation failed");
            WSACleanup();
            return false;
        }

        // Set 3-second timeout
        DWORD timeout = 3000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

        sockaddr_in proxyAddr;
        proxyAddr.sin_family = AF_INET;
        proxyAddr.sin_port = htons(7135);  // local launcher/proxy discovery port
        inet_pton(AF_INET, "127.0.0.1", &proxyAddr.sin_addr);

        if (connect(sock, (sockaddr*)&proxyAddr, sizeof(proxyAddr)) == SOCKET_ERROR) {
            Logger::LogWarning("Auto-discovery: Cannot connect to local proxy on localhost:7135");
            closesocket(sock);
            WSACleanup();
            return false;
        }

        // Send HTTP GET request for this game key. The launcher also supports
        // fallback aliases, handled by DiscoverServerFromProxy().
        std::string request = std::string("GET /discover?game=") + gameKey +
            " HTTP/1.1\r\nHost: localhost:7135\r\nConnection: close\r\n\r\n";
        if (send(sock, request.c_str(), (int)request.length(), 0) == SOCKET_ERROR) {
            Logger::LogError("Auto-discovery: Failed to send discovery request");
            closesocket(sock);
            WSACleanup();
            return false;
        }

        // Read response
        char buffer[1024] = {0};
        int bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
        closesocket(sock);
        WSACleanup();

        if (bytesReceived <= 0) {
            Logger::LogWarning("Auto-discovery: No response from local proxy");
            return false;
        }

        std::string response(buffer, bytesReceived);
        
        // Find the HTTP body (after double CRLF)
        size_t bodyStart = response.find("\r\n\r\n");
        if (bodyStart == std::string::npos) {
            Logger::LogError("Auto-discovery: Invalid response format");
            return false;
        }
        
        return ApplyDiscoveryBody(Trim(response.substr(bodyStart + 4)), gameKey);
    }

    // Auto-discovery via the Dwemer launcher/proxy (port 7135).
    static bool DiscoverServerFromProxy() {
        static const char* kDiscoveryGameKeys[] = {
            "dialectic",
            "fallout",
            "fnv",
            "newvegas",
        };

        for (const char* gameKey : kDiscoveryGameKeys) {
            if (DiscoverServerFromProxyForGame(gameKey)) {
                return true;
            }
        }

        Logger::LogWarning("Auto-discovery: No Dialectic launcher aliases resolved");
        return false;
    }

    static std::string Trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    static void ResolveServerConnection() {
        const std::string fallbackHost = serverHost.empty() ? kDefaultServerHost : serverHost;
        const int fallbackPort = serverPort > 0 ? serverPort : kDefaultServerPort;
        const std::string fallbackPath = serverPath.empty() ? kDefaultServerPath : serverPath;

        std::string overrideHost = fallbackHost;
        int overridePort = fallbackPort;
        std::string overridePath = fallbackPath;
        if (ReadCustomServerOverride(overrideHost, overridePort, overridePath)) {
            serverHost = overrideHost;
            serverPort = overridePort;
            serverPath = overridePath.empty() ? kDefaultServerPath : overridePath;
            return;
        }

        if (DiscoverServerFromProxy()) {
            serverPath = kDefaultServerPath;
            Logger::LogInfo("Using auto-discovered configuration: http://%s:%d/%s",
                serverHost.c_str(),
                serverPort,
                serverPath.c_str());
            return;
        }

        serverHost = fallbackHost;
        serverPort = fallbackPort;
        serverPath = fallbackPath;
        Logger::LogInfo("Using fallback Dialectic server configuration: http://%s:%d/%s",
            serverHost.c_str(),
            serverPort,
            serverPath.c_str());
    }

    static void LoadInternal(bool resolveConnection) {
        Logger::LogSection(resolveConnection ? "LOADING CONFIGURATION" : "RELOADING RUNTIME SETTINGS");
        
        EnsureCustomINIExists();
        MigrateDialecticControlHotkey();
        const std::string defaultIniPath = GetDefaultINIPath();
        const std::string customIniPath = GetCustomINIPath();
        Logger::LogDebug("Default INI path: %s", defaultIniPath.c_str());
        Logger::LogDebug("Custom INI path: %s", customIniPath.c_str());

        for (const std::string& iniPath : { defaultIniPath, customIniPath }) {
            std::ifstream iniFile(iniPath);
            if (!iniFile.is_open()) {
                if (iniPath == defaultIniPath) {
                    Logger::LogWarning("Default INI not found at %s; using compiled defaults", iniPath.c_str());
                }
                continue;
            }

            Logger::LogInfo("Reading INI file: %s", iniPath.c_str());
            std::string line;
            std::string currentSection;
            
            while (std::getline(iniFile, line)) {
                line = Trim(line);
                
                if (line.empty() || line[0] == ';' || line[0] == '#')
                    continue;
                
                if (line[0] == '[' && line[line.length() - 1] == ']') {
                    currentSection = line.substr(1, line.length() - 2);
                    continue;
                }
                
                size_t equalsPos = line.find('=');
                if (equalsPos == std::string::npos)
                    continue;
                
                std::string key = Trim(line.substr(0, equalsPos));
                std::string value = Trim(line.substr(equalsPos + 1));
                
                if (currentSection == "Server" && resolveConnection) {
                    if (key == "Host") serverHost = value;
                    else if (key == "Port") ParsePort(value, serverPort, iniPath.c_str());
                    else if (key == "Path") serverPath = value;
                    else if (key == "LocalSoundcachePath") localSoundcachePath = value;
                }
                else if (currentSection == "Player") {
                // Player name is detected from the live game reference, not loaded from INI.
                }
            else if (currentSection == "Audio") {
                if (key == "PreClipMs") preClipMs = std::stof(value);
                else if (key == "PostClipMs") postClipMs = std::stof(value);
                else if (key == "AnimationResolution") animationResolution = std::stoi(value);
                else if (key == "AnimationIntensity") animationIntensity = std::stof(value);
                else if (key == "VoiceVolume") voiceVolume = std::stof(value);
                else if (key == "HeadVoiceVolume") headVoiceVolume = std::stof(value);
                else if (key == "Enable3DPlayback" || key == "Playback3D") audio3DPlaybackEnabled = (value == "1" || value == "true");
                else if (key == "Playback2D" || key == "Force2D") audio3DPlaybackEnabled = !(value == "1" || value == "true");
                else if (key == "PanStrength" || key == "3DPanStrength") audio3DPanStrength = std::stof(value);
                else if (key == "InvertHeading") audioInvertHeading = (value == "1" || value == "true");
                else if (key == "DistanceScale" || key == "VoiceDistanceScale") audioDistanceScale = std::stof(value);
                else if (key == "PlaybackDropoffInteriorPercent") audioPlaybackDropoffInteriorPercent = std::stof(value);
                else if (key == "PlaybackDropoffExteriorPercent") audioPlaybackDropoffExteriorPercent = std::stof(value);
            }
            else if (currentSection == "Behavior") {
                if (key == "EnableCombatDialogue") enableCombatDialogue = (value == "1" || value == "true");
                else if (key == "CancelDialogueOnCombat") cancelDialogueOnCombat = (value == "1" || value == "true");
                else if (key == "AIResponseTimeout") aiResponseTimeout = std::stoi(value);
                else if (key == "PauseDialogueOnMenu") pauseDialogueOnMenu = (value == "1" || value == "true");
                else if (key == "ShowAISubtitles" || key == "EnableAISubtitles") showAISubtitles = (value == "1" || value == "true");
                else if (key == "SceneSafety" || key == "RestrictOnScene") sceneSafetyEnabled = (value == "1" || value == "true");
            }
            else if (currentSection == "DialogueCapture") {
                if (key == "Enabled") dialogueCaptureEnabled = (value == "1" || value == "true");
                else if (key == "CaptureRadiant") dialogueCaptureRadiant = (value == "1" || value == "true");
                else if (key == "CaptureDialogueMenu") dialogueCaptureDialogueMenu = (value == "1" || value == "true");
                else if (key == "CapturePlayerMenuChoices") dialogueCapturePlayerMenuChoices = (value == "1" || value == "true");
                else if (key == "MinRepeatWindowMs" || key == "RepeatWindowMs") dialogueCaptureRepeatWindowMs = std::max(0, std::stoi(value));
                else if (key == "RequireKnownSpeaker") dialogueCaptureRequireKnownSpeaker = (value == "1" || value == "true");
            }
            else if (currentSection == "VoiceRecording") {
                if (key == "SilenceThreshold") silenceThreshold = std::stoi(value);
                else if (key == "MaxRecordingSeconds") maxRecordingSeconds = std::stoi(value);
                else if (key == "CurrentDevice") voiceRecordingPreferredDeviceName = value;
                else if (key == "DetectedEndpointId") voiceRecordingDetectedEndpointId = value;
                else if (key == "SaveLastRecording") voiceRecordingSaveLastWav = (value == "1" || value == "true");
            }
            else if (currentSection == "OpenMic") {
                if (key == "Enabled") openMicEnabled = (value == "1" || value == "true");
                else if (key == "Sensitivity") openMicSensitivity = std::stof(value);
                else if (key == "EndDelaySeconds" || key == "EndDelay") openMicEndDelaySeconds = std::stof(value);
                else if (key == "Muted") openMicMuted = (value == "1" || value == "true");
            }
            else if (currentSection == "Distance") {
                if (key == "ActivatingNpcInterior") distanceActivatingNpcInterior = std::stof(value);
                else if (key == "ActivatingNpcExterior") distanceActivatingNpcExterior = std::stof(value);
            }
            else if (currentSection == "AutoActivate") {
                if (key == "Enabled") autoActivateEnabled = (value == "1" || value == "true");
                else if (key == "AutoAddHostile") autoAddHostile = (value == "1" || value == "true");
                else if (key == "AutoAddCreatures") autoAddCreatures = (value == "1" || value == "true");
            }
            else if (currentSection == "RpgEvents") {
                if (key == "CombatBarksEnabled") rpgCombatBarksEnabled = (value == "1" || value == "true");
                else if (key == "CombatBarkPeriodSeconds") rpgCombatBarkPeriodSeconds = std::max(5, std::stoi(value));
            }
            else if (currentSection == "SpatialAudio") {
                if (key == "Enabled") spatialAudioEnabled = (value == "1" || value == "true");
                else if (key == "Enable3DPlayback" || key == "Playback3D") audio3DPlaybackEnabled = (value == "1" || value == "true");
                else if (key == "Playback2D" || key == "Force2D") audio3DPlaybackEnabled = !(value == "1" || value == "true");
                else if (key == "PanStrength" || key == "3DPanStrength") audio3DPanStrength = std::stof(value);
                else if (key == "InvertHeading") audioInvertHeading = (value == "1" || value == "true");
                else if (key == "MaxAirDistance") spatialMaxAirDistance = std::stof(value);
                else if (key == "ImmediateDistance") spatialImmediateDistance = std::stof(value);
                else if (key == "AutoHearingDistance") spatialAutoHearingDistance = std::stof(value);
                else if (key == "DistanceScaler") spatialDistanceScaler = std::stof(value);
                else if (key == "InteriorHearingDistance" || key == "InteriorMaxDistance") spatialInteriorHearingDistance = std::stof(value);
                else if (key == "ExteriorHearingDistance" || key == "ExteriorMaxDistance") spatialExteriorHearingDistance = std::stof(value);
                else if (key == "MinDistanceFactor") spatialMinDistanceFactor = std::stof(value);
                else if (key == "InteriorBaseModifier") spatialInteriorBaseModifier = std::stof(value);
                else if (key == "ExteriorBaseModifier") spatialExteriorBaseModifier = std::stof(value);
                else if (key == "OpenDoorPenaltyBase") spatialOpenDoorPenaltyBase = std::stof(value);
                else if (key == "AroundCornerPenalty") spatialAroundCornerPenalty = std::stof(value);
                else if (key == "DoorTriangulationPercentTolerance") spatialDoorTriangulationPercentTolerance = std::stof(value);
                else if (key == "DoorTriangulationAbsoluteTolerance") spatialDoorTriangulationAbsoluteTolerance = std::stof(value);
                else if (key == "MinimumAudibleVolume") spatialMinimumAudibleVolume = std::stof(value);
                else if (key == "PathRatioReject") spatialPathRatioReject = std::stof(value);
                else if (key == "PathRatioDistanceReject") spatialPathRatioDistanceReject = std::stof(value);
                else if (key == "PathRatioDistanceRejectMinAir") spatialPathRatioDistanceRejectMinAir = std::stof(value);
                else if (key == "PathComplexityStartRatio") spatialPathComplexityStartRatio = std::stof(value);
                else if (key == "PathComplexityScale") spatialPathComplexityScale = std::stof(value);
                else if (key == "PathComplexityMin") spatialPathComplexityMin = std::stof(value);
                else if (key == "NavmeshSnapDistance") spatialNavmeshSnapDistance = std::stof(value);
                else if (key == "AllowPathUnavailableFallback") spatialAllowPathUnavailableFallback = (value == "1" || value == "true");
            }
            else if (currentSection == "WorldContext") {
                if (key == "Enabled") worldContextEnabled = (value == "1" || value == "true");
                else if (key == "UpdateSeconds") worldContextUpdateSeconds = std::stof(value);
                else if (key == "SendOnChange") worldContextSendOnChange = (value == "1" || value == "true");
                else if (key == "SendBeforePlayerInput") worldContextSendBeforePlayerInput = (value == "1" || value == "true");
                else if (key == "IncludeWeather") worldContextIncludeWeather = (value == "1" || value == "true");
                else if (key == "IncludeCell") worldContextIncludeCell = (value == "1" || value == "true");
                else if (key == "IncludeWorldspace") worldContextIncludeWorldspace = (value == "1" || value == "true");
            }
            else if (currentSection == "NearbyActors") {
                if (key == "Enabled") nearbyActorsEnabled = (value == "1" || value == "true");
                else if (key == "UpdateSeconds") nearbyActorsUpdateSeconds = std::stof(value);
                else if (key == "SendOnChange") nearbyActorsSendOnChange = (value == "1" || value == "true");
                else if (key == "SendBeforePlayerInput") nearbyActorsSendBeforePlayerInput = (value == "1" || value == "true");
                else if (key == "MaxActors") nearbyActorsMaxActors = std::stoi(value);
                else if (key == "MaxDistance") nearbyActorsMaxDistance = std::stof(value);
            }
            else if (currentSection == "ActivityStatus") {
                if (key == "Enabled") activityStatusEnabled = (value == "1" || value == "true");
                else if (key == "UpdateSeconds") activityStatusUpdateSeconds = std::stof(value);
                else if (key == "SendOnChange") activityStatusSendOnChange = (value == "1" || value == "true");
                else if (key == "SendBeforePlayerInput") activityStatusSendBeforePlayerInput = (value == "1" || value == "true");
                else if (key == "MaxActors") activityStatusMaxActors = std::stoi(value);
                else if (key == "MaxDistance") activityStatusMaxDistance = std::stof(value);
            }
            else if (currentSection == "NearbyItems") {
                if (key == "Enabled") nearbyItemsEnabled = (value == "1" || value == "true");
                else if (key == "UpdateSeconds") nearbyItemsUpdateSeconds = std::stof(value);
                else if (key == "SendOnChange") nearbyItemsSendOnChange = (value == "1" || value == "true");
                else if (key == "SendBeforePlayerInput") nearbyItemsSendBeforePlayerInput = (value == "1" || value == "true");
                else if (key == "MaxItems") nearbyItemsMaxItems = std::stoi(value);
                else if (key == "MaxDistance") nearbyItemsMaxDistance = std::stof(value);
                else if (key == "IncludeStealing") nearbyItemsIncludeStealing = (value == "1" || value == "true");
                else if (key == "IncludeLookingAt") nearbyItemsIncludeLookingAt = (value == "1" || value == "true");
                else if (key == "IncludeHeldItem") nearbyItemsIncludeHeldItem = (value == "1" || value == "true");
                else if (key == "HeldItemPriority") nearbyItemsHeldItemPriority = (value == "1" || value == "true");
            }
            else if (currentSection == "PointsOfInterest") {
                if (key == "Enabled") pointsOfInterestEnabled = (value == "1" || value == "true");
                else if (key == "UpdateSeconds") pointsOfInterestUpdateSeconds = std::stof(value);
                else if (key == "SendOnChange") pointsOfInterestSendOnChange = (value == "1" || value == "true");
                else if (key == "SendBeforePlayerInput") pointsOfInterestSendBeforePlayerInput = (value == "1" || value == "true");
                else if (key == "MaxPois") pointsOfInterestMaxPois = std::stoi(value);
                else if (key == "MaxDistance") pointsOfInterestMaxDistance = std::stof(value);
                else if (key == "IncludeDoors") pointsOfInterestIncludeDoors = (value == "1" || value == "true");
                else if (key == "IncludeLocked") pointsOfInterestIncludeLocked = (value == "1" || value == "true");
                else if (key == "IncludeLookingAt") pointsOfInterestIncludeLookingAt = (value == "1" || value == "true");
            }
            else if (currentSection == "DynamicProfile") {
                if (key == "TimerMinutes" || key == "UpdateMinutes") dynamicProfileTimerMinutes = std::max(1, std::stoi(value));
                else if (key == "IncludeNarrator") dynamicProfileTimerIncludeNarrator = (value == "1" || value == "true");
            }
            else if (currentSection == "BoredEvents") {
                if (key == "Enabled") boredEventsEnabled = true;
                else if (key == "TimerSeconds" || key == "BoredEventTimerSeconds") boredEventTimerSeconds = std::max(5, std::stoi(value));
                else if (key == "AvoidInMenu") boredAvoidInMenu = (value == "1" || value == "true");
                else if (key == "AvoidInDialogue") boredAvoidInDialogue = (value == "1" || value == "true");
                else if (key == "AvoidInCombat") boredAvoidInCombat = (value == "1" || value == "true");
                else if (key == "AvoidWhenSneaking") boredAvoidWhenSneaking = (value == "1" || value == "true");
                else if (key == "AvoidWhenVoiceInputActive") boredAvoidWhenVoiceInputActive = (value == "1" || value == "true");
                else if (key == "RecentSpeechCooldownSeconds") boredRecentSpeechCooldownSeconds = std::max(0, std::stoi(value));
            }
            else if (currentSection == "Narrator") {
                // Runtime mode is server-owned. Keep this section ignored for old INIs.
            }
            else if (currentSection == "Modes") {
                // Runtime mode is server-owned. Keep this section ignored for old INIs.
            }
            else if (currentSection == "Rechat") {
                if (key == "Enabled") rechatEnabled = (value == "1" || value == "true");
                else if (key == "MaxDepth") rechatMaxDepth = std::stoi(value);
                else if (key == "SmartLaunch") rechatSmartLaunch = (value == "1" || value == "true");
                else if (key == "RetryOnEmpty") rechatRetryOnEmpty = (value == "1" || value == "true");
                else if (key == "AvoidWhenSneaking") rechatAvoidWhenSneaking = (value == "1" || value == "true");
                else if (key == "AvoidInMenu") rechatAvoidInMenu = (value == "1" || value == "true");
                else if (key == "EndConversationCooldown") rechatEndConversationCooldown = std::stoi(value);
            }
            else if (currentSection == "ExcludedRaces") {
                // Value is the race name to exclude
                if (!value.empty()) {
                    excludedRaces.insert(ToLower(value));
                    Logger::LogDebug("Excluding race: %s", value.c_str());
                }
            }
            else if (currentSection == "ExcludedNPCs") {
                // Value is the NPC name to exclude
                if (!value.empty()) {
                    excludedNPCNames.insert(ToLower(value));
                    Logger::LogDebug("Excluding NPC: %s", value.c_str());
                }
            }
            else if (currentSection == "ExcludedFormIDs") {
                // Value is hex form ID (without 0x prefix)
                if (!value.empty()) {
                    try {
                        uint32_t formId = std::stoul(value, nullptr, 16);
                        excludedFormIDs.insert(formId);
                        Logger::LogDebug("Excluding FormID: 0x%08X", formId);
                    } catch (...) {
                        Logger::LogWarning("Invalid FormID: %s", value.c_str());
                    }
                }
            }
        }
            
            iniFile.close();
        }

        if (resolveConnection) {
            ResolveServerConnection();
        }

        // Always protect AI speech from overlapping vanilla/radiant dialogue.
        suppressVanillaDialogueDuringAI = true;
        // Always attempt speaker attention during AI speech.
        faceTargetDuringAIResponse = true;
        
        Logger::LogInfo("Loaded %zu excluded races, %zu excluded NPCs, %zu excluded FormIDs",
            excludedRaces.size(), excludedNPCNames.size(), excludedFormIDs.size());

        static const char* kModeNames[] = {
            "STANDARD",
            "WHISPER",
            "CLOSE",
            "SHOUT",
            "NARRATOR",
            "DIRECTOR",
            "INJECTION_LOG",
            "INJECTION_CHAT",
            "CHEATMODE"
        };
        spatialAudioEnabled = true;
        worldContextEnabled = true;
        nearbyActorsEnabled = true;
        activityStatusEnabled = true;
        nearbyItemsEnabled = true;
        pointsOfInterestEnabled = true;

        if (resolveConnection) {
            currentModeIndex = 0;
            currentMode = kModeNames[0];
            currentProfileModelSlot = 1;
            narratorModeEnabled = false;
        }

    }

    void Load() {
        LoadInternal(true);
    }

    void LoadRuntimeSettings() {
        LoadInternal(false);
    }

    void Save() {
        std::string iniPath = GetCustomINIPath();

        // Do not persist an auto-discovered endpoint as a manual override when an
        // unrelated MCM setting is saved. Preserve only explicit custom values.
        std::string customServerHost = kDefaultServerHost;
        int customServerPort = kDefaultServerPort;
        std::string customServerPath = kDefaultServerPath;
        const bool hasCustomServerOverride = ReadCustomServerOverride(
            customServerHost,
            customServerPort,
            customServerPath);
        std::string customSoundcachePath;
        const bool hasCustomSoundcachePath = TryReadCustomINIValue(
            "Server",
            "LocalSoundcachePath",
            customSoundcachePath);

        const int hotkeyTalkToNPC = ReadINIInt("Hotkeys", "TalkToNPC", 0);
        const int hotkeyStopTalking = ReadINIInt("Hotkeys", "StopTalking", 0);
        const int hotkeyToggleVoice = ReadINIInt("Hotkeys", "ToggleVoice", 0);
        const int hotkeyManualActivate = ReadINIInt("Hotkeys", "ManualActivate", 0);
        const int hotkeyOpenMenu = ReadINIInt("Hotkeys", "OpenMenu", 0);
        const int hotkeyQuickCommand = ReadINIInt("Hotkeys", "QuickCommand", 0);
        const int hotkeyDialecticControl = ReadINIInt("Hotkeys", "DialecticControl", 0);
        const int hotkeyOpenMicMute = ReadINIInt("Hotkeys", "OpenMicMute", 0);
        const int hotkeyPipVision = ReadINIInt("Hotkeys", "PipVision", 0);

        std::ofstream iniFile(iniPath);
        
        if (!iniFile.is_open()) {
            return;
        }

    iniFile << "; DIALECTIC user configuration\n";
        iniFile << "; This file overrides dialectic.ini and is preserved across mod updates.\n\n";
        
        if (hasCustomServerOverride || hasCustomSoundcachePath) {
            iniFile << "[Server]\n";
            if (hasCustomServerOverride) {
                iniFile << "Host=" << customServerHost << "\n";
                iniFile << "Port=" << customServerPort << "\n";
                iniFile << "Path=" << customServerPath << "\n";
            }
            if (hasCustomSoundcachePath) {
                iniFile << "LocalSoundcachePath=" << customSoundcachePath << "\n";
            }
            iniFile << "\n";
        }
        
        iniFile << "[Hotkeys]\n";
        iniFile << "; Hotkeys use Fallout DirectInput scan codes. Set them in MCM to enable.\n";
        iniFile << "TalkToNPC=" << hotkeyTalkToNPC << "\n";
        iniFile << "StopTalking=" << hotkeyStopTalking << "\n";
        iniFile << "ToggleVoice=" << hotkeyToggleVoice << "\n";
        iniFile << "ManualActivate=" << hotkeyManualActivate << "\n";
        iniFile << "OpenMenu=" << hotkeyOpenMenu << "\n";
        iniFile << "QuickCommand=" << hotkeyQuickCommand << "\n";
        iniFile << "DialecticControl=" << hotkeyDialecticControl << "\n";
        iniFile << "OpenMicMute=" << hotkeyOpenMicMute << "\n";
        iniFile << "PipVision=" << hotkeyPipVision << "\n\n";
        
        iniFile << "[Audio]\n";
        iniFile << "; Player-heard AI voice playback settings.\n";
        iniFile << "VoiceVolume=" << voiceVolume << "\n";
        iniFile << "; Narrator and player TTS volume relative to VoiceVolume. 100 keeps the current level.\n";
        iniFile << "HeadVoiceVolume=" << headVoiceVolume << "\n";
        iniFile << "PreClipMs=" << preClipMs << "\n";
        iniFile << "PostClipMs=" << postClipMs << "\n";
        iniFile << "AnimationResolution=" << animationResolution << "\n";
        iniFile << "AnimationIntensity=" << animationIntensity << "\n";
        iniFile << "; Set 0 for flat 2D playback.\n";
        iniFile << "Enable3DPlayback=" << (audio3DPlaybackEnabled ? "1" : "0") << "\n";
        iniFile << "; 1.0 is natural stereo separation; higher values make actor position more noticeable.\n";
        iniFile << "PanStrength=" << audio3DPanStrength << "\n";
        iniFile << "; Flip left/right heading if FNV orientation is reversed on your setup.\n";
        iniFile << "InvertHeading=" << (audioInvertHeading ? "1" : "0") << "\n";
        iniFile << "; AI voice distance falloff curve. Higher values drop volume faster with distance.\n";
        iniFile << "DistanceScale=" << audioDistanceScale << "\n";
        iniFile << "; Player-heard playback dropoff tuning. 70 is the default reference value.\n";
        iniFile << "PlaybackDropoffInteriorPercent=" << audioPlaybackDropoffInteriorPercent << "\n";
        iniFile << "PlaybackDropoffExteriorPercent=" << audioPlaybackDropoffExteriorPercent << "\n\n";
        
        iniFile << "[Behavior]\n";
        iniFile << "EnableCombatDialogue=" << (enableCombatDialogue ? "1" : "0") << "\n";
        iniFile << "; Queue/audio cancellation when the player enters combat.\n";
        iniFile << "CancelDialogueOnCombat=" << (cancelDialogueOnCombat ? "1" : "0") << "\n";
        iniFile << "AIResponseTimeout=" << aiResponseTimeout << "\n";
        iniFile << "; Pause AI dialogue while normal menus or the Pip-Boy are open.\n";
        iniFile << "PauseDialogueOnMenu=" << (pauseDialogueOnMenu ? "1" : "0") << "\n";
        iniFile << "; Show passive Fallout-style subtitles while AI audio plays.\n";
        iniFile << "ShowAISubtitles=" << (showAISubtitles ? "1" : "0") << "\n";
        iniFile << "; Scene safety. Avoid actors currently controlled by interrupt/dialogue scene packages.\n";
        iniFile << "SceneSafety=" << (sceneSafetyEnabled ? "1" : "0") << "\n\n";

        iniFile << "[DialogueCapture]\n";
        iniFile << "; Passive capture of vanilla/radiant FNV dialogue into the eventlog. This never starts AI generation.\n";
        iniFile << "Enabled=" << (dialogueCaptureEnabled ? "1" : "0") << "\n";
        iniFile << "CaptureRadiant=" << (dialogueCaptureRadiant ? "1" : "0") << "\n";
        iniFile << "CaptureDialogueMenu=" << (dialogueCaptureDialogueMenu ? "1" : "0") << "\n";
        iniFile << "CapturePlayerMenuChoices=" << (dialogueCapturePlayerMenuChoices ? "1" : "0") << "\n";
        iniFile << "MinRepeatWindowMs=" << dialogueCaptureRepeatWindowMs << "\n";
        iniFile << "RequireKnownSpeaker=" << (dialogueCaptureRequireKnownSpeaker ? "1" : "0") << "\n\n";
        
        iniFile << "[VoiceRecording]\n";
        iniFile << "; Silence threshold for voice recording (audio below this value is considered silence)\n";
        iniFile << "SilenceThreshold=" << silenceThreshold << "\n";
        iniFile << "; Maximum recording duration in seconds\n";
        iniFile << "MaxRecordingSeconds=" << maxRecordingSeconds << "\n";
        iniFile << "; Keep the latest WAV submitted to STT for diagnostics.\n";
        iniFile << "SaveLastRecording=" << (voiceRecordingSaveLastWav ? "1" : "0") << "\n";
        iniFile << "; Preferred device retained automatically. Windows default follows the OS setting.\n";
        iniFile << "CurrentDevice=" << voiceRecordingPreferredDeviceName << "\n";
        iniFile << "; Stable Core Audio endpoint selected automatically from a recording with real signal.\n";
        iniFile << "DetectedEndpointId=" << voiceRecordingDetectedEndpointId << "\n\n";

        iniFile << "[OpenMic]\n";
        iniFile << "; Open mic voice activity detection. Disabled by default.\n";
        iniFile << "Enabled=" << (openMicEnabled ? "1" : "0") << "\n";
        iniFile << "; RMS threshold for starting capture. Higher values require louder speech.\n";
        iniFile << "Sensitivity=" << openMicSensitivity << "\n";
        iniFile << "; Seconds of silence before open mic recording is submitted.\n";
        iniFile << "EndDelaySeconds=" << openMicEndDelaySeconds << "\n";
        iniFile << "Muted=" << (openMicMuted ? "1" : "0") << "\n\n";
        
        iniFile << "[Distance]\n";
        iniFile << "ActivatingNpcInterior=" << distanceActivatingNpcInterior << "\n";
        iniFile << "ActivatingNpcExterior=" << distanceActivatingNpcExterior << "\n\n";
        
        iniFile << "[AutoActivate]\n";
        iniFile << "; Master toggle for auto-activation of nearby NPCs\n";
        iniFile << "Enabled=" << (autoActivateEnabled ? "1" : "0") << "\n";
        iniFile << "; Include hostile NPCs as AI agents?\n";
        iniFile << "AutoAddHostile=" << (autoAddHostile ? "1" : "0") << "\n";
        iniFile << "; Allow all race/form categories? 0 limits auto activation to humans, non-feral ghouls, super mutants, and non-animal robots.\n";
        iniFile << "AutoAddCreatures=" << (autoAddCreatures ? "1" : "0") << "\n\n";

        iniFile << "[RpgEvents]\n";
        iniFile << "; Combat bark controls. Combat dialogue itself is controlled by [Behavior] EnableCombatDialogue.\n";
        iniFile << "CombatBarksEnabled=" << (rpgCombatBarksEnabled ? "1" : "0") << "\n";
        iniFile << "CombatBarkPeriodSeconds=" << rpgCombatBarkPeriodSeconds << "\n\n";

        iniFile << "[SpatialAudio]\n";
        iniFile << "; Spatial awareness for NPC hearing, auto-activation, and listener selection. Player-heard playback is configured in [Audio].\n";
        iniFile << "Enabled=1\n";
        iniFile << "; Spatial awareness guard rails. AutoHearingDistance defaults to 11.2m at 70 FNV units/meter.\n";
        iniFile << "MaxAirDistance=" << spatialMaxAirDistance << "\n";
        iniFile << "ImmediateDistance=" << spatialImmediateDistance << "\n";
        iniFile << "AutoHearingDistance=" << spatialAutoHearingDistance << "\n";
        iniFile << "; Awareness falloff curve. This does not control player-heard voice panning.\n";
        iniFile << "DistanceScaler=" << spatialDistanceScaler << "\n";
        iniFile << "InteriorHearingDistance=" << spatialInteriorHearingDistance << "\n";
        iniFile << "ExteriorHearingDistance=" << spatialExteriorHearingDistance << "\n";
        iniFile << "MinDistanceFactor=" << spatialMinDistanceFactor << "\n";
        iniFile << "InteriorBaseModifier=" << spatialInteriorBaseModifier << "\n";
        iniFile << "ExteriorBaseModifier=" << spatialExteriorBaseModifier << "\n";
        iniFile << "OpenDoorPenaltyBase=" << spatialOpenDoorPenaltyBase << "\n";
        iniFile << "AroundCornerPenalty=" << spatialAroundCornerPenalty << "\n";
        iniFile << "DoorTriangulationPercentTolerance=" << spatialDoorTriangulationPercentTolerance << "\n";
        iniFile << "DoorTriangulationAbsoluteTolerance=" << spatialDoorTriangulationAbsoluteTolerance << "\n";
        iniFile << "MinimumAudibleVolume=" << spatialMinimumAudibleVolume << "\n";
        iniFile << "; Path policy. These apply only when a native/script path provider reports real path data.\n";
        iniFile << "PathRatioReject=" << spatialPathRatioReject << "\n";
        iniFile << "PathRatioDistanceReject=" << spatialPathRatioDistanceReject << "\n";
        iniFile << "PathRatioDistanceRejectMinAir=" << spatialPathRatioDistanceRejectMinAir << "\n";
        iniFile << "PathComplexityStartRatio=" << spatialPathComplexityStartRatio << "\n";
        iniFile << "PathComplexityScale=" << spatialPathComplexityScale << "\n";
        iniFile << "PathComplexityMin=" << spatialPathComplexityMin << "\n";
        iniFile << "NavmeshSnapDistance=" << spatialNavmeshSnapDistance << "\n";
        iniFile << "; Keep current distance/LOS fallback when no FNV path provider is available.\n";
        iniFile << "AllowPathUnavailableFallback=" << (spatialAllowPathUnavailableFallback ? "1" : "0") << "\n\n";

        iniFile << "[WorldContext]\n";
        iniFile << "; FNV world prompt context: location, weather, and in-game date/time.\n";
        iniFile << "Enabled=1\n";
        iniFile << "UpdateSeconds=" << worldContextUpdateSeconds << "\n";
        iniFile << "SendOnChange=" << (worldContextSendOnChange ? "1" : "0") << "\n";
        iniFile << "SendBeforePlayerInput=" << (worldContextSendBeforePlayerInput ? "1" : "0") << "\n";
        iniFile << "IncludeWeather=" << (worldContextIncludeWeather ? "1" : "0") << "\n";
        iniFile << "IncludeCell=" << (worldContextIncludeCell ? "1" : "0") << "\n";
        iniFile << "IncludeWorldspace=" << (worldContextIncludeWorldspace ? "1" : "0") << "\n\n";

        iniFile << "[NearbyActors]\n";
        iniFile << "; Structured nearby actor snapshots for <nearby_actors> prompt context.\n";
        iniFile << "Enabled=1\n";
        iniFile << "UpdateSeconds=" << nearbyActorsUpdateSeconds << "\n";
        iniFile << "SendOnChange=" << (nearbyActorsSendOnChange ? "1" : "0") << "\n";
        iniFile << "SendBeforePlayerInput=" << (nearbyActorsSendBeforePlayerInput ? "1" : "0") << "\n";
        iniFile << "MaxActors=" << nearbyActorsMaxActors << "\n";
        iniFile << "MaxDistance=" << nearbyActorsMaxDistance << "\n\n";

        iniFile << "[ActivityStatus]\n";
        iniFile << "; Live actor condition/activity snapshots for <activity> and <condition> prompt context.\n";
        iniFile << "Enabled=1\n";
        iniFile << "UpdateSeconds=" << activityStatusUpdateSeconds << "\n";
        iniFile << "SendOnChange=" << (activityStatusSendOnChange ? "1" : "0") << "\n";
        iniFile << "SendBeforePlayerInput=" << (activityStatusSendBeforePlayerInput ? "1" : "0") << "\n";
        iniFile << "MaxActors=" << activityStatusMaxActors << "\n";
        iniFile << "MaxDistance=" << activityStatusMaxDistance << "\n\n";

        iniFile << "[NearbyItems]\n";
        iniFile << "; Structured nearby item snapshots for <nearby_items> prompt context.\n";
        iniFile << "Enabled=1\n";
        iniFile << "UpdateSeconds=" << nearbyItemsUpdateSeconds << "\n";
        iniFile << "SendOnChange=" << (nearbyItemsSendOnChange ? "1" : "0") << "\n";
        iniFile << "SendBeforePlayerInput=" << (nearbyItemsSendBeforePlayerInput ? "1" : "0") << "\n";
        iniFile << "MaxItems=" << nearbyItemsMaxItems << "\n";
        iniFile << "MaxDistance=" << nearbyItemsMaxDistance << "\n";
        iniFile << "IncludeStealing=" << (nearbyItemsIncludeStealing ? "1" : "0") << "\n";
        iniFile << "IncludeLookingAt=" << (nearbyItemsIncludeLookingAt ? "1" : "0") << "\n";
        iniFile << "IncludeHeldItem=" << (nearbyItemsIncludeHeldItem ? "1" : "0") << "\n";
        iniFile << "HeldItemPriority=" << (nearbyItemsHeldItemPriority ? "1" : "0") << "\n\n";

        iniFile << "[PointsOfInterest]\n";
        iniFile << "; Structured door/location snapshots for <points_of_interest> prompt context.\n";
        iniFile << "Enabled=1\n";
        iniFile << "UpdateSeconds=" << pointsOfInterestUpdateSeconds << "\n";
        iniFile << "SendOnChange=" << (pointsOfInterestSendOnChange ? "1" : "0") << "\n";
        iniFile << "SendBeforePlayerInput=" << (pointsOfInterestSendBeforePlayerInput ? "1" : "0") << "\n";
        iniFile << "MaxPois=" << pointsOfInterestMaxPois << "\n";
        iniFile << "MaxDistance=" << pointsOfInterestMaxDistance << "\n";
        iniFile << "IncludeDoors=" << (pointsOfInterestIncludeDoors ? "1" : "0") << "\n";
        iniFile << "IncludeLocked=" << (pointsOfInterestIncludeLocked ? "1" : "0") << "\n";
        iniFile << "IncludeLookingAt=" << (pointsOfInterestIncludeLookingAt ? "1" : "0") << "\n\n";

        iniFile << "[DynamicProfile]\n";
        iniFile << "; Periodic dynamic profile refresh. Manual hotkeys live in [Hotkeys].\n";
        iniFile << "; Always enabled; TimerMinutes controls how often background refreshes run.\n";
        iniFile << "TimerMinutes=" << dynamicProfileTimerMinutes << "\n";
        iniFile << "IncludeNarrator=" << (dynamicProfileTimerIncludeNarrator ? "1" : "0") << "\n\n";

        iniFile << "[BoredEvents]\n";
        iniFile << "; Idle/bored NPC comments. Always enabled; server-side BORED_EVENT controls probability.\n";
        iniFile << "TimerSeconds=" << boredEventTimerSeconds << "\n";
        iniFile << "AvoidInMenu=" << (boredAvoidInMenu ? "1" : "0") << "\n";
        iniFile << "AvoidInDialogue=" << (boredAvoidInDialogue ? "1" : "0") << "\n";
        iniFile << "AvoidInCombat=" << (boredAvoidInCombat ? "1" : "0") << "\n";
        iniFile << "AvoidWhenSneaking=" << (boredAvoidWhenSneaking ? "1" : "0") << "\n";
        iniFile << "AvoidWhenVoiceInputActive=" << (boredAvoidWhenVoiceInputActive ? "1" : "0") << "\n";
        iniFile << "RecentSpeechCooldownSeconds=" << boredRecentSpeechCooldownSeconds << "\n\n";

        iniFile << "[Rechat]\n";
        iniFile << "; Plugin-side rechat launch policy. Server controls rechat probability and mode.\n";
        iniFile << "Enabled=" << (rechatEnabled ? "1" : "0") << "\n";
        iniFile << "MaxDepth=" << rechatMaxDepth << "\n";
        iniFile << "SmartLaunch=" << (rechatSmartLaunch ? "1" : "0") << "\n";
        iniFile << "RetryOnEmpty=" << (rechatRetryOnEmpty ? "1" : "0") << "\n";
        iniFile << "AvoidWhenSneaking=" << (rechatAvoidWhenSneaking ? "1" : "0") << "\n";
        iniFile << "AvoidInMenu=" << (rechatAvoidInMenu ? "1" : "0") << "\n";
        iniFile << "EndConversationCooldown=" << rechatEndConversationCooldown << "\n\n";

        iniFile << "[Tools]\n";
        iniFile << "InitializeDialectic=0\n";
        iniFile << "SendWorldData=0\n";
        iniFile << "SendVoiceSamples=0\n";
        iniFile << "UpdateTargetProfile=0\n";
        iniFile << "UpdateNearbyProfiles=0\n";
        iniFile << "UpdateNarratorProfile=0\n\n";
        
        iniFile << "[ExcludedRaces]\n";
        iniFile << "; Races to exclude from AI agents (by name, one per line)\n";
        iniFile << "; Example: Race1=Deathclaw\n";
        int raceIdx = 1;
        for (const auto& race : excludedRaces) {
            iniFile << "Race" << raceIdx++ << "=" << race << "\n";
        }
        iniFile << "\n";
        
        iniFile << "[ExcludedNPCs]\n";
        iniFile << "; Specific NPCs to never make AI agents (by name)\n";
        iniFile << "; Example: NPC1=Yes Man\n";
        int npcIdx = 1;
        for (const auto& npc : excludedNPCNames) {
            iniFile << "NPC" << npcIdx++ << "=" << npc << "\n";
        }
        iniFile << "\n";
        
        iniFile << "[ExcludedFormIDs]\n";
        iniFile << "; Exclude by form ID (hex, no 0x prefix)\n";
        iniFile << "; Example: FormID1=00123456\n";
        int formIdx = 1;
        for (const auto& formId : excludedFormIDs) {
            char hexBuf[16];
            sprintf_s(hexBuf, "%08X", formId);
            iniFile << "FormID" << formIdx++ << "=" << hexBuf << "\n";
        }
        
        iniFile.close();
    }

    void SetServerHost(const std::string& host) {
        serverHost = host;
    }

    void SetServerPort(int port) {
        serverPort = port;
    }

    void SetServerPath(const std::string& path) {
        serverPath = path;
    }

    // Exclusion checking functions
    bool IsRaceExcluded(const std::string& raceName) {
        return excludedRaces.find(ToLower(raceName)) != excludedRaces.end();
    }

    bool IsNPCExcluded(const std::string& npcName) {
        return excludedNPCNames.find(ToLower(npcName)) != excludedNPCNames.end();
    }

    bool IsFormIDExcluded(uint32_t formId) {
        return excludedFormIDs.find(formId) != excludedFormIDs.end();
    }

    // Runtime exclusion management
    void AddExcludedRace(const std::string& raceName) {
        excludedRaces.insert(ToLower(raceName));
    }

    void AddExcludedNPC(const std::string& npcName) {
        excludedNPCNames.insert(ToLower(npcName));
    }

    void AddExcludedFormID(uint32_t formId) {
        excludedFormIDs.insert(formId);
    }

    void RemoveExcludedRace(const std::string& raceName) {
        excludedRaces.erase(ToLower(raceName));
    }

    void RemoveExcludedNPC(const std::string& npcName) {
        excludedNPCNames.erase(ToLower(npcName));
    }

    void RemoveExcludedFormID(uint32_t formId) {
        excludedFormIDs.erase(formId);
    }
}
