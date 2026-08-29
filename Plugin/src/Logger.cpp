// Logger.cpp - File logging for Dialectic

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdarg.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>

#include "Logger.h"

// Internal state
static FILE* s_logFile = NULL;
static std::atomic<int> s_logLevel{static_cast<int>(Logger::Level::Info)};
static bool s_initialized = false;
static int s_linesSinceFlush = 0;
static std::chrono::steady_clock::time_point s_lastFlushTime;
static std::mutex s_logMutex;
static Logger::Diagnostics s_diagnostics;

static void GetTimestamp(char* buffer, int bufSize) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    sprintf_s(buffer, bufSize, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static const char* GetLevelStr(int level) {
    switch (level) {
        case 0: return "[TRACE]";
        case 1: return "[DEBUG]";
        case 2: return "[INFO ]";
        case 3: return "[WARN ]";
        case 4: return "[ERROR]";
        default: return "[?????]";
    }
}

static void GetGameDir(char* buffer, int bufSize) {
    DWORD size = GetModuleFileNameA(NULL, buffer, bufSize);
    if (size > 0) {
        for (int i = (int)size - 1; i >= 0; --i) {
            if (buffer[i] == '\\' || buffer[i] == '/') {
                buffer[i] = '\0';
                return;
            }
        }
    }
    buffer[0] = '\0';
}

static void WriteLog(int level, const char* msg) {
    const auto writeStartedAt = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(s_logMutex);
    const auto lockAcquiredAt = std::chrono::steady_clock::now();
    if (!s_initialized || !s_logFile) return;
    if (level < s_logLevel.load(std::memory_order_relaxed)) return;
    
    char ts[64];
    GetTimestamp(ts, 64);
    const int bytesWritten = fprintf(s_logFile, "%s %s %s\n", ts, GetLevelStr(level), msg);

    ++s_linesSinceFlush;
    const auto now = std::chrono::steady_clock::now();
    const bool flushBySeverity = level >= static_cast<int>(Logger::Level::Warning);
    const bool flushByCount = s_linesSinceFlush >= 32;
    const bool flushByTime = s_lastFlushTime.time_since_epoch().count() == 0 ||
        now - s_lastFlushTime >= std::chrono::seconds(1);
    if (flushBySeverity || flushByCount || flushByTime) {
        fflush(s_logFile);
        ++s_diagnostics.flushes;
        s_linesSinceFlush = 0;
        s_lastFlushTime = now;
    }

    const auto completedAt = std::chrono::steady_clock::now();
    const auto lockWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(
        lockAcquiredAt - writeStartedAt).count();
    const auto writeUs = std::chrono::duration_cast<std::chrono::microseconds>(
        completedAt - lockAcquiredAt).count();
    const std::uint64_t safeLockWaitUs = lockWaitUs > 0 ? static_cast<std::uint64_t>(lockWaitUs) : 0;
    const std::uint64_t safeWriteUs = writeUs > 0 ? static_cast<std::uint64_t>(writeUs) : 0;
    ++s_diagnostics.linesWritten;
    if (bytesWritten > 0) {
        s_diagnostics.bytesWritten += static_cast<std::uint64_t>(bytesWritten);
    }
    s_diagnostics.totalLockWaitUs += safeLockWaitUs;
    s_diagnostics.totalWriteUs += safeWriteUs;
    s_diagnostics.maxLockWaitUs = (std::max)(s_diagnostics.maxLockWaitUs, safeLockWaitUs);
    s_diagnostics.maxWriteUs = (std::max)(s_diagnostics.maxWriteUs, safeWriteUs);
    if (safeLockWaitUs + safeWriteUs >= 2000) {
        ++s_diagnostics.slowWrites;
    }
}

void Logger::Initialize() {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (s_initialized) return;
    
    char gameDir[MAX_PATH];
    char logPath[MAX_PATH];
    logPath[0] = '\0';
    
    GetGameDir(gameDir, MAX_PATH);

    char documentsDir[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(
            NULL,
            CSIDL_PERSONAL | CSIDL_FLAG_CREATE,
            NULL,
            SHGFP_TYPE_CURRENT,
            documentsDir))) {
        std::filesystem::path logDirectory =
            std::filesystem::path(documentsDir) / "My Games" / "FalloutNV" / "NVSE";
        std::error_code directoryError;
        std::filesystem::create_directories(logDirectory, directoryError);
        const std::string documentsLogPath = (logDirectory / "dialectic.log").string();
        if (documentsLogPath.size() < MAX_PATH) {
            strcpy_s(logPath, MAX_PATH, documentsLogPath.c_str());
            fopen_s(&s_logFile, logPath, "w");
        }
    }
    
    if (!s_logFile && gameDir[0] != '\0') {
        sprintf_s(logPath, MAX_PATH, "%s\\dialectic.log", gameDir);
        fopen_s(&s_logFile, logPath, "w");
    }
    
    if (!s_logFile) {
        strcpy_s(logPath, MAX_PATH, "Data\\NVSE\\Plugins\\dialectic.log");
        fopen_s(&s_logFile, logPath, "w");
    }
    
    if (!s_logFile) {
        strcpy_s(logPath, MAX_PATH, "dialectic.log");
        fopen_s(&s_logFile, logPath, "w");
    }
    
    s_initialized = (s_logFile != NULL);
    s_linesSinceFlush = 0;
    s_lastFlushTime = std::chrono::steady_clock::now();
    s_diagnostics = {};
    
    if (s_initialized) {
        char ts[64];
        GetTimestamp(ts, 64);
        fprintf(s_logFile, "========================================\n");
        fprintf(s_logFile, "  Dialectic for Fallout New Vegas\n");
        fprintf(s_logFile, "  Log started: %s\n", ts);
        fprintf(s_logFile, "  Log path: %s\n", logPath);
        fprintf(s_logFile, "========================================\n");
        fflush(s_logFile);
    }
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (s_initialized && s_logFile) {
        char ts[64];
        GetTimestamp(ts, 64);
        fprintf(s_logFile, "========================================\n");
        fprintf(s_logFile, "  Log ended: %s\n", ts);
        fprintf(s_logFile, "========================================\n");
        fflush(s_logFile);
        fclose(s_logFile);
        s_logFile = NULL;
    }
    s_initialized = false;
}

void Logger::SetLogLevel(Logger::Level level) {
    s_logLevel.store(static_cast<int>(level), std::memory_order_relaxed);
}

void Logger::LogTrace(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, 2048, fmt, args);
    va_end(args);
    WriteLog(0, buf);
}

void Logger::LogDebug(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, 2048, fmt, args);
    va_end(args);
    WriteLog(1, buf);
}

void Logger::LogInfo(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, 2048, fmt, args);
    va_end(args);
    WriteLog(2, buf);
}

void Logger::LogWarning(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, 2048, fmt, args);
    va_end(args);
    WriteLog(3, buf);
}

void Logger::LogError(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, 2048, fmt, args);
    va_end(args);
    WriteLog(4, buf);
}

void Logger::Log(Logger::Level level, const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, 2048, fmt, args);
    va_end(args);
    WriteLog(static_cast<int>(level), buf);
}

void Logger::LogSeparator() {
    if (s_initialized && s_logFile) {
        fprintf(s_logFile, "----------------------------------------\n");
        fflush(s_logFile);
    }
}

void Logger::LogSection(const char* section) {
    if (s_initialized && s_logFile) {
        fprintf(s_logFile, "\n======== %s ========\n", section);
        fflush(s_logFile);
    }
}

Logger::Diagnostics Logger::GetDiagnostics() {
    std::lock_guard<std::mutex> lock(s_logMutex);
    return s_diagnostics;
}
