#pragma once

#include "IngameNotifier.h"

#include <cstddef>
#include <string>

namespace DialecticInitialization {

bool TryBegin();
void QueueNotice(std::string message, IngameNotifier::Level level);
void ReportVoiceProgress(std::size_t completed, std::size_t total);
void ReportVoiceFinished(bool success);
void ReportWorldProgress(std::string stage, std::size_t completed = 0, std::size_t total = 0);
void ReportWorldFinished(bool success);
void Update();

} // namespace DialecticInitialization
