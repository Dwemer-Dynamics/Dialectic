#pragma once

#include <functional>

namespace VoiceSampleBatchUploadFNV {
    enum class BatchUploadResult {
        Success = 0,
        NoSamplesUploaded = 1,
        TimedOut = 2,
        Cancelled = 3
    };

    struct BatchUploadSummary {
        int totalMappings = 0;
        int csvMappings = 0;
        int looseMappings = 0;
        int archiveMappings = 0;
        int uploaded = 0;
        int missing = 0;
        int failed = 0;
        bool timedOut = false;
        bool cancelled = false;
    };

    BatchUploadResult SendAllVoiceSamples(BatchUploadSummary& summary,
        const std::function<bool()>& cancelRequested = {},
        const std::function<void(int, int)>& progress = {});
}
