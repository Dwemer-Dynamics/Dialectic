// WorldContextFNV.h - FNV world context bridge for Dialectic prompts

#pragma once

#include <string>

namespace WorldContextFNV {

struct Context {
    bool resolved = false;
    std::string location;
    std::string cellFormId;
    std::string worldspace;
    std::string worldspaceFormId;
    bool isInterior = false;
    bool interiorKnown = false;
    std::string weather;
    std::string weatherName;
    std::string weatherEditorId;
    std::string weatherFormId;
    int gameYear = 0;
    int gameMonth = 0;
    int gameDay = 0;
    float gameHour = 0.0f;
    float gameDaysPassed = 0.0f;
    long long gamets = 0;
    float playerX = 0.0f;
    float playerY = 0.0f;
    float playerZ = 0.0f;
    bool playerPositionKnown = false;
};

Context GetCurrent();
std::string GetPlayerLocation();
long long GetGameTimestamp();
void Update();
void SendNow(bool force = false);

} // namespace WorldContextFNV
