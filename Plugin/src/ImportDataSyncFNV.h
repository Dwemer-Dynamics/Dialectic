#pragma once

#include <functional>

namespace ImportDataSyncFNV {
    void DetectAndUploadImportDataFiles(const std::function<bool()>& cancelRequested = {});
}
