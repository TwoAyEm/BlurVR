// BlurVR.ini settings.  Every key has a default; see the settings reference in NOTES.md.
#include "config.h"
#include "core.h"

Config g_cfg;
static std::string IniFilePath()
{
    return std::string(g_modDir) + "BlurVR.ini";
}
static int ReadIniInt(const char* section, const char* key, int defaultValue)
{
    char buf[64];
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), IniFilePath().c_str());
    return buf[0] ? (int)strtol(buf, NULL, 0) : defaultValue;
}
static double ReadIniDouble(const char* section, const char* key, double defaultValue)
{
    char buf[64];
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), IniFilePath().c_str());
    return buf[0] ? atof(buf) : defaultValue;
}
void LoadConfig()
{
    g_cfg.enabled = ReadIniInt("General", "Enabled", 1);
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
    g_cfg.stereo = ReadIniInt("Render", "Stereo", 1);
    g_cfg.forceNoVsync = ReadIniInt("Render", "ForceNoVSync", 1);
    g_cfg.maxFps = ReadIniInt("Render", "MaxFps", 0);
    g_cfg.fovScale = ReadIniDouble("Render", "FovScale", 1.0);
    g_cfg.mainPassMinWidthFraction = ReadIniDouble("Render", "MainPassMinWidthFraction", 0.9);
    g_cfg.requireDepthBuffer = ReadIniInt("Render", "RequireDepthBuffer", 1);
    g_cfg.seamGutter = ReadIniInt("Render", "SeamGutter", 0);
    g_cfg.hudWorldLocked = ReadIniInt("Hud", "WorldLocked", 1);
    g_cfg.stereoHalfResPasses = ReadIniInt("Render", "HalfResPasses", 1);
    g_cfg.dropBackdropCopies = ReadIniInt("Hud", "DropBackdrop", 1);
    g_cfg.customWidth = ReadIniInt("Render", "CustomWidth", 2160);
    g_cfg.customHeight = ReadIniInt("Render", "CustomHeight", 1200);
    g_cfg.veilRace = ReadIniInt("Hud", "VeilRace", 120);
    g_cfg.veilResultsScreen = ReadIniInt("Hud", "VeilResults", 36);
    g_cfg.veilMenu = ReadIniInt("Hud", "VeilMenu", 100);
    g_cfg.loadingGlow = ReadIniInt("Hud", "LoadingGlow", 1);
    g_cfg.uiGlowPct = ReadIniInt("Hud", "UiGlow", 100);
    if (ReadIniInt("Render", "NoBloom", 0)) // one switch for all the menu / loading-screen glow
        g_cfg.loadingGlow = g_cfg.uiGlowPct = 0;
    g_cfg.dimStrongPct = ReadIniInt("Hud", "DimStrongPct", 60);
    g_cfg.hideFullscreenOverlay = ReadIniInt("Hud", "HideFullscreenOverlay", 1);
    g_cfg.threadedVrInit = ReadIniInt("General", "ThreadedInit", 1);
    g_cfg.deferVrStart = ReadIniInt("General", "DeferVR", 1);
    g_cfg.fixReflectionLookup = ReadIniInt("Render", "ReflectionLookupFix", 1);
    g_cfg.skipLensFlares = ReadIniInt("Render", "NoLensFlare", 0);
    g_cfg.reflectionFlipMode = ReadIniInt("Render", "ReflectionFlip", -1);
    g_cfg.hudXOffset = ReadIniDouble("Hud", "XOffset", 0.0);
    g_cfg.hudScale = ReadIniDouble("Hud", "Scale", 0.6);
    g_cfg.hudDistance = ReadIniDouble("Hud", "Distance", 2.0);
    g_cfg.hudOffsetY = ReadIniDouble("Hud", "OffsetY", 0.0);
    g_cfg.menuDistance = ReadIniDouble("Menu", "Distance", 2.2);
    g_cfg.menuWidth = ReadIniDouble("Menu", "Width", 2.6);
    g_cfg.menuHeight = ReadIniDouble("Menu", "HeightOffset", 0.0);
    g_cfg.adjustViewRot = ReadIniInt("Debug", "AdjustViewRotation", 1);
    g_cfg.sharedTransport = ReadIniInt("Render", "SharedTransport", 0);
    g_cfg.perfEnabled = ReadIniInt("Perf", "Enabled", 0);
    g_cfg.keyPerf = ReadIniInt("Debug", "KeyPerf", 0x67);
    g_cfg.keyVerticalTangents = ReadIniInt("Debug", "KeyVerticalTangents", 0x69);
    g_cfg.keyDump = ReadIniInt("Debug", "KeyDump", 0x68);
    g_cfg.startAtSavedSize = ReadIniInt("Render", "StartAtSavedSize", 1);
    g_cfg.coverageEvery = ReadIniInt("Hud", "CoverageEvery", 8);
    g_cfg.desktopPresentEvery = ReadIniInt("Render", "DesktopPresentEvery", 1);
    g_cfg.desktopView = ReadIniInt("Render", "DesktopView", 1);
    g_cfg.fitDesktopWindow = ReadIniInt("Render", "FitDesktopWindow", 1);
    g_cfg.startWidth = ReadIniInt("Render", "StartWidth", 0);
    g_cfg.startHeight = ReadIniInt("Render", "StartHeight", 0);
    g_cfg.foveationReport = ReadIniInt("Foveation", "Report", 1);
    g_cfg.cullHeadFrustum = ReadIniInt("Culling", "HeadFrustum", 1);
    g_cfg.cullMarginDeg = ReadIniDouble("Culling", "MarginDeg", 6.0);
    g_cfg.cullPullBack = ReadIniDouble("Culling", "PullBack", 0.3);
    char buf[32];
    GetPrivateProfileStringA("Matrix", "Layout", "auto", buf, sizeof(buf), IniFilePath().c_str());
    g_cfg.matrixLayoutOverride = (!_stricmp(buf, "row")) ? 1 : (!_stricmp(buf, "transposed")) ? 2 : 0;
}
