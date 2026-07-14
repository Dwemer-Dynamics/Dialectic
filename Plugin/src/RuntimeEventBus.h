#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RuntimeEventBus {

enum class EventType {
    PostLoad,
    PreLoadGame,
    LoadGame,
    PostLoadGame,
    NewGame,
    SaveGame,
    ExitToMainMenu,
    ExitGame,
    ReloadConfig,
    CellChanged,
    CellStateChanged,
    CellReferencesLoaded,
    WorldspaceChanged,
    MenuStateChanged,
    CombatStateChanged,
    ActorAttached,
    ActorDetached
};

struct Event {
    EventType type{EventType::PostLoad};
    std::uint64_t generation{0};
    std::uint64_t sequence{0};
    std::uint32_t formId{0};
    std::uint32_t previousFormId{0};
    bool flag{false};
    std::string text;
};

void Publish(Event event);
std::vector<Event> Drain(std::size_t maxEvents = 128);
void Clear();
std::size_t PendingCount();

} // namespace RuntimeEventBus
