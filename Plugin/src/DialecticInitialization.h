#pragma once

#include "IngameNotifier.h"

#include <string>

namespace DialecticInitialization {

void Begin();
void QueueNotice(std::string message, IngameNotifier::Level level);
void ReportVoiceFinished(bool success);
void ReportWorldFinished(bool success);
void Update();

} // namespace DialecticInitialization
