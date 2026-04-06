#include <iostream>
#include "log_window.h"

namespace PacketCodecTests {
    void RunAll();
}

int RunBaseMathTests();
int RunRngTests();

// Mock logging implementation for standalone tests to link against without GUI dependencies
extern "C" {
void LogWindow_Init(void) {}
void LogWindow_Shutdown(void) {}
void LogWindow_Log(LogLevel level, const char* fmt, ...) {}
void LogWindow_LogV(LogLevel level, const char* fmt, va_list args) {}
void LogWindow_LogCat(LogCategory cat, LogLevel level, const char* fmt, ...) {}
void LogWindow_LogCatV(LogCategory cat, LogLevel level, const char* fmt, va_list args) {}
void LogWindow_LogRateLimited(LogLevel level, unsigned int intervalMs, const char* fmt, ...) {}
bool LogWindow_LogOnChange_Int(const char* name, int* prevValue, int newValue, LogLevel level, const char* fmt, ...) { return false; }
bool LogWindow_LogOnChange_Bool(const char* name, bool* prevValue, bool newValue, LogLevel level, const char* fmt, ...) { return false; }
void LogWindow_LogPacket(LogLevel level, const char* fmt, ...) {}
void LogWindow_LogPacketV(LogLevel level, const char* fmt, va_list args) {}
void LogWindow_LogNet(LogLevel level, const char* fmt, ...) {}
void LogWindow_LogNetV(LogLevel level, const char* fmt, va_list args) {}
void LogWindow_LogGekko(LogLevel level, const char* fmt, ...) {}
void LogWindow_LogGekkoV(LogLevel level, const char* fmt, va_list args) {}
void LogWindow_SetCategoryEnabled(LogCategory cat, bool enabled) {}
bool LogWindow_IsCategoryEnabled(LogCategory cat) { return true; }
void LogWindow_SetAllCategoriesEnabled(bool enabled) {}
const char* LogWindow_GetCategoryName(LogCategory cat) { return "MOCK"; }
void LogWindow_Render(bool* pOpen) {}
void LogWindow_Clear(void) {}
int LogWindow_GetCount(void) { return 0; }
void LogWindow_SetMaxEntries(int max) {}
void LogWindow_SetAutoScroll(bool enabled) {}
bool LogWindow_GetAutoScroll(void) { return false; }
void LogWindow_SetMinLevel(LogLevel level) {}
LogLevel LogWindow_GetMinLevel(void) { return LOG_DEBUG; }
bool LogWindow_WriteToFile(const char* filename) { return false; }
void LogWindow_RenderContent(void) {}

// Missing win32 stubs for timing/random? Usually windows.h handles GetTickCount.
}

int main() {
    std::cout << "========================================\n";
    std::cout << "  ALICE SENKI 2 ROLLBACK - TEST SUITE\n";
    std::cout << "========================================\n\n";

    int total_failures = 0;

    total_failures += RunBaseMathTests();
    std::cout << "\n";

    total_failures += RunRngTests();
    std::cout << "\n";
    
    PacketCodecTests::RunAll();
    std::cout << "\n";

    std::cout << "========================================\n";
    if (total_failures == 0) {
        std::cout << "  ALL STANDALONE TESTS PASSED!\n";
    } else {
        std::cout << "  " << total_failures << " TESTS FAILED.\n";
    }
    std::cout << "========================================\n";

    return total_failures > 0 ? 1 : 0;
}