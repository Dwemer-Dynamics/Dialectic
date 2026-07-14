#pragma once

#include <cstdint>
#include <string>

namespace TradeManager {

struct TradeSessionRequest {
    uint64_t requestId = 0;
    std::string action;
    std::string speakerName;
    uint32_t speakerFormId = 0;
    uint32_t inventoryOwnerFormId = 0;
    std::string tradeMode;
};

void Initialize();
void Shutdown();
void BeginPendingSession(const TradeSessionRequest& request);
void CancelAll(const char* reason);
void Update();

} // namespace TradeManager
