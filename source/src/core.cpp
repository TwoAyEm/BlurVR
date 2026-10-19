// Module location, log file and logging.
#include "core.h"

HMODULE g_selfModule;
char g_gameDir[MAX_PATH]; // folder containing Blur.exe / this d3d9.dll
char g_modDir[MAX_PATH];  // <root>\BlurVR\ (ini, log, openvr_api.dll)
bool g_logEnabled;        // the file is truncated once at start-up; no handle to it is kept open afterwards
char g_logPath[MAX_PATH]; // lines are appended through a fresh, non-inheritable handle each time. A long-lived (inheritable)
                          // handle was inherited by a helper process the game starts, which kept the file open and made
                          // the log look truncated (stale size) long after the game had closed

void Log(const char* fmt, ...)
{
    if (!g_logEnabled)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1024];
    int n = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    size_t length = strlen(line);
    line[length++] = '\n';
    HANDLE file = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, line, (DWORD)length, &written, NULL);
    CloseHandle(file);
}
