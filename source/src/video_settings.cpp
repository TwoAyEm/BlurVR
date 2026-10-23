// Where the display mode lives.  See video_settings.h.
#include "video_settings.h"
#include "config.h"
#include "core.h"

static const char* const kKeys[5] = {"width", "height", "windowed", "x", "y"};
struct DisplayMode {
    std::string value[5];
    bool has[5] = {false, false, false, false, false};
};

static bool g_sessionOpen = false; // VR is running and settings.xml holds the VR mode until VideoSettingsEnd
static DisplayMode g_flat;         // the flat game's mode, put back at the end

static std::string SettingsPath()
{
    char appData[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", appData, sizeof(appData));
    if (!n || n >= sizeof(appData))
        return "";
    return std::string(appData) + "\\bizarre creations\\blur\\settings.xml";
}
static std::string SidecarPath()
{
    return std::string(g_modDir) + "display_backup.ini";
}

static bool ReadFileBytes(const std::string& path, std::string& out)
{
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (16 << 20);
    out.clear();
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(file, &out[0], (DWORD)out.size(), &got, NULL) && got == out.size());
    }
    CloseHandle(file);
    return ok;
}
// (written beside the file and moved over it: a reader never sees half a file)
static bool WriteFileBytes(const std::string& path, const std::string& data)
{
    std::string temp = path + ".blurvr-tmp";
    HANDLE file = CreateFileA(temp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    bool ok = data.empty() || (WriteFile(file, data.data(), (DWORD)data.size(), &put, NULL) && put == data.size());
    ok = CloseHandle(file) && ok;
    if (ok)
        ok = MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok)
        DeleteFileA(temp.c_str());
    return ok;
}

// The <shared_graphical_settings ... /> element, where the game keeps its display mode: [begin, end) with end at its '>'.
static bool FindTag(const std::string& text, size_t* begin, size_t* end)
{
    size_t b = text.find("<shared_graphical_settings");
    if (b == std::string::npos)
        return false;
    size_t e = text.find('>', b);
    if (e == std::string::npos)
        return false;
    *begin = b;
    *end = e;
    return true;
}
// The value of attribute `name` inside the element (the name must follow a blank, so "x" never matches "max_x").
static bool FindAttr(const std::string& text, size_t begin, size_t end, const char* name, size_t* valueStart, size_t* valueLength)
{
    std::string needle = std::string(" ") + name + "=\"";
    size_t p = text.find(needle, begin);
    if (p == std::string::npos || p >= end)
        return false;
    size_t v = p + needle.size();
    size_t q = text.find('"', v);
    if (q == std::string::npos || q >= end)
        return false;
    *valueStart = v;
    *valueLength = q - v;
    return true;
}
static bool ReadMode(const std::string& text, DisplayMode& mode)
{
    size_t begin, end;
    if (!FindTag(text, &begin, &end))
        return false;
    for (int i = 0; i < 5; i++) {
        size_t start, length;
        if (FindAttr(text, begin, end, kKeys[i], &start, &length)) {
            mode.value[i] = text.substr(start, length);
            mode.has[i] = true;
        }
    }
    return mode.has[0] && mode.has[1];
}
// Put the mode's values into the element (only attributes that are already there).  True when something changed.
static bool PatchMode(std::string& text, const DisplayMode& mode)
{
    bool changed = false;
    for (int i = 0; i < 5; i++) {
        size_t begin, end, start, length;
        if (!mode.has[i] || !FindTag(text, &begin, &end) || !FindAttr(text, begin, end, kKeys[i], &start, &length))
            continue;
        if (text.compare(start, length, mode.value[i]) != 0) {
            text.replace(start, length, mode.value[i]);
            changed = true;
        }
    }
    return changed;
}

// BlurVR\display_backup.ini: [Flat] the flat game's mode, [Session] Open=1 from the VR start until the VR end put it back.
static bool ReadSidecar(DisplayMode& mode, bool* open)
{
    std::string path = SidecarPath();
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    char buf[64];
    for (int i = 0; i < 5; i++) {
        GetPrivateProfileStringA("Flat", kKeys[i], "", buf, sizeof(buf), path.c_str());
        if (buf[0]) {
            mode.value[i] = buf;
            mode.has[i] = true;
        }
    }
    *open = GetPrivateProfileIntA("Session", "Open", 0, path.c_str()) != 0;
    return mode.has[0] && mode.has[1];
}
static void WriteSidecar(const DisplayMode& mode, bool open)
{
    std::string path = SidecarPath();
    for (int i = 0; i < 5; i++)
        WritePrivateProfileStringA("Flat", kKeys[i], mode.has[i] ? mode.value[i].c_str() : NULL, path.c_str());
    WritePrivateProfileStringA("Session", "Open", open ? "1" : "0", path.c_str());
}

void VideoSettingsStart()
{
    std::string settingsPath = SettingsPath();
    if (settingsPath.empty())
        return;
    DisplayMode flat;
    bool open = false;
    bool haveSidecar = ReadSidecar(flat, &open);
    std::string text;
    if (!g_cfg.vr) { // flat, or the mod is off: a VR run that was cut off may have left its mode in settings.xml
        if (haveSidecar && open && ReadFileBytes(settingsPath, text)) {
            if (PatchMode(text, flat) && !WriteFileBytes(settingsPath, text)) {
                Log("settings.xml: the last VR run did not end properly and the flat mode %sx%s could not be put back", flat.value[0].c_str(),
                    flat.value[1].c_str());
                return;
            }
            WriteSidecar(flat, false);
            Log("settings.xml: the last VR run did not end properly; the flat game's display mode %sx%s is back", flat.value[0].c_str(),
                flat.value[1].c_str());
        }
        return;
    }
    if (!ReadFileBytes(settingsPath, text)) {
        Log("VR display mode: %s not found, the game starts with its own settings", settingsPath.c_str());
        return;
    }
    DisplayMode current;
    if (!ReadMode(text, current)) {
        Log("VR display mode: no display mode in %s, the game starts with its own settings", settingsPath.c_str());
        return;
    }
    if (haveSidecar && open) {
        // the last VR run did not end properly: settings.xml still holds its mode and the flat one is in the sidecar
        Log("VR display mode: the last VR run did not end properly, the flat mode %sx%s from display_backup.ini is kept", flat.value[0].c_str(),
            flat.value[1].c_str());
    } else {
        flat = current;
        CopyFileA(settingsPath.c_str(), (settingsPath + ".blurvr-backup").c_str(), FALSE); // (the file as the game left it)
    }
    WriteSidecar(flat, true);
    DisplayMode vr;
    vr.value[0] = std::to_string(g_cfg.vrWidth);
    vr.value[1] = std::to_string(g_cfg.vrHeight);
    vr.value[2] = "1"; // (a picture that is not a monitor mode can only be a window)
    vr.has[0] = vr.has[1] = vr.has[2] = true;
    if (PatchMode(text, vr) && !WriteFileBytes(settingsPath, text)) {
        Log("VR display mode: settings.xml could not be written, the game starts with its own settings");
        WriteSidecar(flat, false);
        return;
    }
    g_flat = flat;
    g_sessionOpen = true;
    Log("VR display mode: the game starts at %dx%d in a window (BlurVR.ini [VR] Resolution); the flat mode %sx%s stays in settings.xml afterwards",
        g_cfg.vrWidth, g_cfg.vrHeight, flat.value[0].c_str(), flat.value[1].c_str());
}

void VideoSettingsEnd()
{
    if (!g_sessionOpen)
        return;
    g_sessionOpen = false;
    std::string settingsPath = SettingsPath(), text;
    if (!ReadFileBytes(settingsPath, text))
        return; // (the session stays open in the sidecar: the next start puts the flat mode back)
    if (PatchMode(text, g_flat) && !WriteFileBytes(settingsPath, text)) {
        Log("settings.xml: the flat game's display mode could not be put back, the next start will do it");
        return;
    }
    WriteSidecar(g_flat, false);
    Log("settings.xml: the flat game's display mode %sx%s is back", g_flat.value[0].c_str(), g_flat.value[1].c_str());
}

void VideoSettingsNoteMode(UINT width, UINT height)
{
    if (!g_cfg.vr || width < 640 || height < 360 || ((int)width == g_cfg.vrWidth && (int)height == g_cfg.vrHeight))
        return;
    char size[32];
    snprintf(size, sizeof(size), "%ux%u", width, height);
    std::string ini = std::string(g_modDir) + "BlurVR.ini";
    if (WritePrivateProfileStringA("VR", "Resolution", size, ini.c_str())) {
        g_cfg.vrWidth = (int)width;
        g_cfg.vrHeight = (int)height;
        Log("VR display mode: %s picked in the game's options, BlurVR.ini [VR] Resolution=%s", size, size);
    } else
        Log("VR display mode: BlurVR.ini could not be written, %s is not kept", size);
}
