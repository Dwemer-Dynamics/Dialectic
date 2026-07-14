// Logger.h - File logging for Dialectic
#ifndef DIALECTIC_LOGGER_H
#define DIALECTIC_LOGGER_H

namespace Logger {

enum class Level {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4
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

} // namespace Logger

#endif // DIALECTIC_LOGGER_H
