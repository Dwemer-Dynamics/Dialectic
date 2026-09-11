#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace MultiplayerSharing {
// All presentation and session transitions run on the game thread; network tasks use snapshots.
bool IsListener();
bool IsHost();
// Queue MCM actions until the menu closes; clipboard access is explicitly user initiated.
void SetupAction(int action);
void Update();
void Reset();
void Shutdown();
void Publish(const std::string& speaker, const std::string& text,
             const std::string& cacheKey, const std::string& utteranceId,
             const std::vector<uint8_t>& audio = {});
void Control(const char* operation);
}
