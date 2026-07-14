// Console.cpp - In-game console printing for Dialectic

#include "Console.h"

#include "IngameNotifier.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

// Forward declare Log from main.cpp
void Log(const char* fmt, ...);

namespace Console {

// Game memory addresses for console functions
// These would need to be found via reverse engineering
// For now, we'll just log to file and hope NVSE script shows messages
namespace GameAddresses {
    typedef void (*PrintToConsole_t)(const char* message);
    static PrintToConsole_t PrintToConsole = nullptr;  // Needs to be found
}

void Print(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    // Log to file
    Log("Console: %s", buffer);

    if (_strnicmp(buffer, "[Dialectic]", 11) == 0 ||
        _strnicmp(buffer, "[DIALECTIC]", 11) == 0) {
        IngameNotifier::NotifyRawDialecticLine(buffer);
    }
    
    // Try to print to game console if we have the function pointer
    if (GameAddresses::PrintToConsole) {
        GameAddresses::PrintToConsole(buffer);
    }
}

void PrintColored(const char* format, ...) {
    // For now, just use regular print
    // Color support would require finding game's colored text functions
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    Print("%s", buffer);
}

void ShowMessage(const char* message) {
    // This would use the game's message box system
    // For now, just print to console
    Print("[MESSAGE] %s", message);
}

void ExecuteCommand(const char* command) {
    Log("Console: Executing command: %s", command);
    // In reality, we'd need to find the game's console command execution function
    // For now, just log it - the script system will need to handle this differently
    // We'll use a workaround by having the script already compiled and just trigger it
}

} // namespace Console
