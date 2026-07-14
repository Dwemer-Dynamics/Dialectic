// Console.h - In-game console printing for Dialectic

#pragma once

#include <string>

namespace Console {

// Print a message to the game console
void Print(const char* format, ...);

// Print with color (if supported)
void PrintColored(const char* format, ...);

// Show a message box in-game
void ShowMessage(const char* message);

// Execute a console command
void ExecuteCommand(const char* command);

} // namespace Console
