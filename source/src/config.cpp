// BlurVR.ini settings.  Every key has a default; see the settings reference in NOTES.md.
#include "config.h"
#include "core.h"

Config g_cfg;
static std::string IniFilePath()
{
    return std::string(g_modDir) + "BlurVR.ini";
}
// The text of a key.  A key that moved to another section is read from the new one first and, when it is not there, from oldSection, so a
// BlurVR.ini written for an earlier version keeps working.
static bool ReadIniText(const char* section, const char* key, const char* oldSection, char* buf, DWORD size)
{
    GetPrivateProfileStringA(section, key, "", buf, size, IniFilePath().c_str());
    if (!buf[0] && oldSection)
        GetPrivateProfileStringA(oldSection, key, "", buf, size, IniFilePath().c_str());
    return buf[0] != 0;
}
static int ReadIniInt(const char* section, const char* key, int defaultValue, const char* oldSection = NULL)
{
    char buf[64];
    return ReadIniText(section, key, oldSection, buf, sizeof(buf)) ? (int)strtol(buf, NULL, 0) : defaultValue;
}
static double ReadIniDouble(const char* section, const char* key, double defaultValue)
{
    char buf[64];
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), IniFilePath().c_str());
    return buf[0] ? atof(buf) : defaultValue;
}
// A size written as WxH (4936x2740).  False, with the outputs untouched, when the key is missing or is not a sensible size.
static bool ReadIniSize(const char* section, const char* key, int* width, int* height)
{
    char buf[64];
    int w = 0, h = 0;
    if (!ReadIniText(section, key, NULL, buf, sizeof(buf)) || sscanf(buf, "%d%*[xX, ]%d", &w, &h) != 2 || w < 640 || h < 360)
        return false;
    *width = w;
    *height = h;
    return true;
}
// DesktopView: left / right / both, or the numbers 1 / 2 / 0 that earlier versions used
static int ReadIniDesktopView(const char* section, const char* key, int defaultValue, const char* oldSection)
{
    char buf[64];
    if (!ReadIniText(section, key, oldSection, buf, sizeof(buf)))
        return defaultValue;
    if (!_stricmp(buf, "left"))
        return 1;
    if (!_stricmp(buf, "right"))
        return 2;
    if (!_stricmp(buf, "both"))
        return 0;
    return (int)strtol(buf, NULL, 0);
}
void LoadConfig()
{
    // Enabled is the master switch (0 = the mod does nothing).  Stereo says which half runs: 1 = VR, 0 = flat, where only the
    // [Display] settings apply and SteamVR is never touched.  g_cfg.vr is what the VR-only code asks.
    g_cfg.enabled = ReadIniInt("General", "Enabled", 1);
    g_cfg.stereo = ReadIniInt("General", "Stereo", 1, "Render");
    g_cfg.vr = g_cfg.enabled && g_cfg.stereo;
    g_cfg.log = ReadIniInt("General", "Log", 0);
    g_cfg.recenterKey = ReadIniInt("General", "RecenterKey", 0x78);
    g_cfg.headTrackingKey = ReadIniInt("General", "ToggleKey", 0x79);
    g_cfg.modeKey = ReadIniInt("General", "ModeKey", 0x77);
    g_cfg.startMode = ReadIniInt("General", "StartMode", 0);
    g_cfg.autoRecenter = ReadIniInt("General", "AutoRecenter", 1);
    g_cfg.countInDrop = ReadIniInt("Hud", "CountInDrop", 1);
    g_cfg.mirror3d = ReadIniInt("Hud", "Mirror3D", 1);
    g_cfg.mirrorConvergence = ReadIniDouble("Hud", "MirrorConvergence", 1.5);
    g_cfg.mirrorEyeScale = ReadIniDouble("Hud", "MirrorEyeScale", 1.0);
    g_cfg.mirrorHeadParallax = ReadIniDouble("Hud", "MirrorHeadParallax", 1.0);
    g_cfg.keyMirror = ReadIniInt("Debug", "KeyMirror3D", 0x66);
    g_cfg.trackRotation = ReadIniInt("Tracking", "Rotation", 1);
    g_cfg.trackPosition = ReadIniInt("Tracking", "Position", 1);
    g_cfg.positionScale = ReadIniDouble("Tracking", "PositionScale", 1.0);
    g_cfg.ipdScale = ReadIniDouble("Tracking", "IpdScale", 1.0);
    g_cfg.eyeTransform = ReadIniInt("Tracking", "EyeTransform", 1);
    g_cfg.oldVerticalTangents = ReadIniInt("Tracking", "OldVerticalTangents", 0);
    g_cfg.forceNoVsync = ReadIniInt("Display", "ForceNoVSync", 0, "Render");
    g_cfg.keyRepeatMs = ReadIniInt("Input", "KeyRepeatMs", 60);
    g_cfg.fovScale = ReadIniDouble("Render", "FovScale", 1.0);
    g_cfg.mainPassMinWidthFraction = ReadIniDouble("Render", "MainPassMinWidthFraction", 0.9);
    g_cfg.requireDepthBuffer = ReadIniInt("Render", "RequireDepthBuffer", 1);
    g_cfg.seamGutter = ReadIniInt("Render", "SeamGutter", 0);
    g_cfg.hudWorldLocked = ReadIniInt("Hud", "WorldLocked", 1);
    g_cfg.stereoHalfResPasses = ReadIniInt("Render", "HalfResPasses", 1);
    g_cfg.dropBackdropCopies = ReadIniInt("Hud", "DropBackdrop", 1);
    // [VR] Resolution=WxH: the size of the VR game's picture (both eyes side by side).  It is offered in the game's video options and
    // used for the start; the game's own settings.xml keeps the flat game's size (video_settings.cpp).  Flat, none of it applies.
    g_cfg.vrWidth = 2160;
    g_cfg.vrHeight = 1200;
    if (!ReadIniSize("VR", "Resolution", &g_cfg.vrWidth, &g_cfg.vrHeight)) { // an older BlurVR.ini: CustomWidth / CustomHeight under [Render]
        int width = ReadIniInt("Render", "CustomWidth", 0), height = ReadIniInt("Render", "CustomHeight", 0);
        if (width >= 640 && height >= 360) {
            g_cfg.vrWidth = width;
            g_cfg.vrHeight = height;
        }
    }
    if (g_cfg.vr) {
        g_cfg.startWidth = g_cfg.vrWidth;
        g_cfg.startHeight = g_cfg.vrHeight;
        g_cfg.startAtSavedSize = 1;
    } else { // the plain game keeps the monitor's own modes, and starts at the size it saved
        g_cfg.startWidth = g_cfg.startHeight = 0;
        g_cfg.startAtSavedSize = 0;
    }
    g_cfg.veilRace = ReadIniInt("Hud", "VeilRace", 120);
    g_cfg.veilResultsScreen = ReadIniInt("Hud", "VeilResults", 36);
    g_cfg.veilMenu = ReadIniInt("Hud", "VeilMenu", 100);
    g_cfg.dimStrongPct = ReadIniInt("Hud", "DimStrongPct", 60);
    g_cfg.hideFullscreenOverlay = ReadIniInt("Hud", "HideFullscreenOverlay", 1);
    g_cfg.threadedVrInit = ReadIniInt("General", "ThreadedInit", 1);
    g_cfg.deferVrStart = ReadIniInt("General", "DeferVR", 1);
    g_cfg.fixReflectionLookup = ReadIniInt("Render", "ReflectionLookupFix", 1);
    // [VR] Bloom (% of the game's glow around menus, UI and loading screens) and NoLensFlare only act in VR: flat they stay at "as the game draws it"
    g_cfg.bloomPct = g_cfg.vr ? ReadIniInt("VR", "Bloom", 100) : 100;
    g_cfg.skipLensFlares = g_cfg.vr ? ReadIniInt("VR", "NoLensFlare", 0) : 0;
    g_cfg.reflectionFlipMode = ReadIniInt("Render", "ReflectionFlip", -1);
    g_cfg.hudXOffset = ReadIniDouble("Hud", "XOffset", 0.0);
    g_cfg.hudScale = ReadIniDouble("Hud", "Scale", 0.6);
    g_cfg.hudDistance = ReadIniDouble("Hud", "Distance", 2.0);
    g_cfg.hudOffsetY = ReadIniDouble("Hud", "OffsetY", 0.0);
    g_cfg.menuDistance = ReadIniDouble("Menu", "Distance", 2.2);
    g_cfg.menuWidth = ReadIniDouble("Menu", "Width", 2.6);
    g_cfg.menuHeight = ReadIniDouble("Menu", "HeightOffset", 0.0);
    g_cfg.adjustViewRot = ReadIniInt("Debug", "AdjustViewRotation", 1);
    g_cfg.sharedTransport = ReadIniInt("VR", "SharedTransport", 1, "Render");
    g_cfg.perfEnabled = ReadIniInt("Perf", "Enabled", 0);
    g_cfg.keyPerf = ReadIniInt("Debug", "KeyPerf", 0x67);
    g_cfg.keyVerticalTangents = ReadIniInt("Debug", "KeyVerticalTangents", 0x69);
    g_cfg.keyDump = ReadIniInt("Debug", "KeyDump", 0x68);
    g_cfg.coverageEvery = ReadIniInt("Hud", "CoverageEvery", 8);
    g_cfg.desktopPresentEvery = ReadIniInt("VR", "DesktopPresentEvery", 1, "Render");
    g_cfg.desktopView = ReadIniDesktopView("VR", "DesktopView", 1, "Render");
    g_cfg.fitDesktopWindow = ReadIniInt("VR", "FitDesktopWindow", 1, "Render");
    g_cfg.foveationReport = ReadIniInt("Foveation", "Report", 1);
    g_cfg.cullHeadFrustum = ReadIniInt("Culling", "HeadFrustum", 1);
    g_cfg.cullMarginDeg = ReadIniDouble("Culling", "MarginDeg", 6.0);
    g_cfg.cullPullBack = ReadIniDouble("Culling", "PullBack", 0.3);
    char buf[32];
    GetPrivateProfileStringA("Matrix", "Layout", "auto", buf, sizeof(buf), IniFilePath().c_str());
    g_cfg.matrixLayoutOverride = (!_stricmp(buf, "row")) ? 1 : (!_stricmp(buf, "transposed")) ? 2 : 0;
}
