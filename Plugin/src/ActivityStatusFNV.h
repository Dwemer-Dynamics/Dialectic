// ActivityStatusFNV.h - Live actor activity snapshots for DialecticServer prompts

#pragma once

#include <cstdint>
#include <string>

namespace ActivityStatusFNV {

struct AutomaticDialogueState {
    bool available = false;
    bool dead = false;
    bool unconscious = false;
    bool sleeping = false;
};

inline const char* AutomaticDialogueBlockReason(const AutomaticDialogueState& state) {
    if (!state.available) return nullptr;
    if (state.dead) return "actor is dead";
    if (state.unconscious) return "actor is unconscious";
    if (state.sleeping) return "actor is sleeping";
    return nullptr;
}

void Update();
void SendNow(bool force = false);
bool IsAutomaticDialogueAllowed(std::uint32_t formId, std::string* reason = nullptr);

} // namespace ActivityStatusFNV
