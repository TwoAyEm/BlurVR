// Stand-in for Blur.exe in tools/exit_restore_check.py: loads the proxy and ends the way the game does.
//
//   exit_restore_check <proxy dll> terminate|exit|other <settings.xml>
//
//   terminate  TerminateProcess(GetCurrentProcess(), 0), which is how Blur ends (Blur.exe 0x42F826): no DLL_PROCESS_DETACH
//   exit       returns from main (ExitProcess, DLL_PROCESS_DETACH with the process exiting)
//   other      first TerminateProcess of another process, then prints whether settings.xml still holds the VR mode (width="4032"), then ends like `terminate`
#include <windows.h>
#include <stdio.h>
#include <string.h>

static bool SettingsHoldVrMode(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    char text[4096] = {0};
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    return strstr(text, "width=\"4032\"") != NULL;
}

int main(int argc, char** argv)
{
    if (argc < 4)
        return 2;
    if (!LoadLibraryA(argv[1])) {
        printf("could not load %s (error %lu)\n", argv[1], GetLastError());
        return 2;
    }
    const char* how = argv[2];
    if (!strcmp(how, "other")) {
        STARTUPINFOA startup;
        memset(&startup, 0, sizeof(startup));
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child;
        char command[] = "cmd.exe /c ping -n 30 127.0.0.1 >nul";
        if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child)) {
            printf("could not start a child process\n");
            return 2;
        }
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, 5000);
        printf("after TerminateProcess of another process: settings.xml %s the VR mode\n", SettingsHoldVrMode(argv[3]) ? "still holds" : "no longer holds");
        fflush(stdout);
    }
    if (!strcmp(how, "exit"))
        return 0;
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
