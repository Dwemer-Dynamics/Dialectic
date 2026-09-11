#pragma once

#include <string>

namespace MultiplayerSharing {
// All presentation and session transitions run on the game thread; network tasks use snapshots.
bool IsListener();
void Update();
void Reset();
void Shutdown();
void Publish(const std::string& speaker, const std::string& text,
             const std::string& cacheKey, const std::string& utteranceId);
void Control(const char* operation);
}
