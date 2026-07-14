#pragma once

namespace WorldDataSyncFNV {
    void RequestSync();
    bool IsSyncing();
    bool HasCompleted();
    bool ResolveLocationByName(const char* name, unsigned int& formId, char* displayName, unsigned int displayNameSize);
    void Update();
}
