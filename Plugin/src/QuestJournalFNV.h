// QuestJournalFNV.h - Sync Fallout quest journal bridge data to DialecticServer

#pragma once

namespace QuestJournalFNV {

void Update();
void SendNow(bool force = false);
void UpdateActiveQuestFromScript(const char* formId, const char* name, const char* editorId);

} // namespace QuestJournalFNV
