// Logger.h - File logging for Dialectic
#ifndef DIALECTIC_LOGGER_H
#define DIALECTIC_LOGGER_H

#include <cstdint>

namespace Logger {

enum class Level {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4
};

struct Diagnostics {
    std::uint64_t linesWritten{0};
    std::uint64_t bytesWritten{0};
    std::uint64_t flushes{0};
    std::uint64_t totalWriteUs{0};
    std::uint64_t maxWriteUs{0};
    std::uint64_t totalLockWaitUs{0};
    std::uint64_t maxLockWaitUs{0};
    std::uint64_t slowWrites{0};
};

void Initialize();
void Shutdown();
void SetLogLevel(Level level);
void LogTrace(const char* fmt, ...);
void LogDebug(const char* fmt, ...);
void LogInfo(const char* fmt, ...);
void LogWarning(const char* fmt, ...);
void LogError(const char* fmt, ...);
void Log(Level level, const char* fmt, ...);
void LogSeparator();
void LogSection(const char* section);
Diagnostics GetDiagnostics();

} // namespace Logger

#endif // DIALECTIC_LOGGER_H
