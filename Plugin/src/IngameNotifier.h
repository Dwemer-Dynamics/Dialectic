#pragma once

#include <string>

namespace IngameNotifier {

enum class Level {
    Info,
    Success,
    Warning,
    Error
};

void Notify(const std::string& message, Level level = Level::Info);
void NotifyRawDialecticLine(const std::string& message);

} // namespace IngameNotifier
