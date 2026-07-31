// QuestJournalFNV.h - Sync Fallout quest journal bridge data to DialecticServer

#pragma once

#include <string>

namespace QuestJournalFNV {

void Update();
void SendNow(bool force = false);
void UpdateActiveQuestFromScript(const char* formId, const char* name, const char* editorId);
std::string BuildCurrentQuestResult(const std::string& filter);

} // namespace QuestJournalFNV
