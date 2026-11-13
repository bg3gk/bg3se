#include <Extender/ScriptExtender.h>
#include <Extender/Client/FrameCap.inl>
#include <Extender/Client/WindowFocus.h>

/// <lua_module>Render</lua_module>
BEGIN_NS(ecl::lua::render)

// Ext.ClientRender.Screenshot(path)
//
// Asks for the next frame the game shows to be written as a PNG file. `path` is below the Script
// Extender folder of the player's profile, the same place Ext.IO.SaveFile() writes to, and the
// file appears there once the frame has been taken and encoded - a few frames later, not at once.
//
// The picture is the game as the renderer presented it and nothing else: it is copied out of the
// swap chain before the extender draws any of its own windows, so no overlay, no debug tool and
// no console is in it, and the game is neither paused nor moved to take it. It does not need the
// window to be in front or even visible.
//
// False when a screenshot is already on its way, when the renderer is not up yet, or when it
// cannot read its frames (only the DX11 backend can so far).
bool Screenshot(StringView path)
{
    return gExtender->IMGUI().RequestScreenshot(path);
}

// Whether a screenshot asked for earlier has still to be taken or written
bool ScreenshotPending()
{
    return gExtender->IMGUI().ScreenshotPending();
}

// Ext.ClientRender.MirrorEnable(enabled)
//
// Starts or stops handing every finished frame to another program on the graphics card. The
// renderer copies the frame the game is about to show into a texture that other program opens by
// name, so nothing is brought back into memory and nothing is encoded: the cost to the game is one
// GPU-to-GPU copy of one frame. It is what lets a window elsewhere show the game at the game's own
// frame rate, where a screenshot taken over and over could not.
//
// True when mirroring is on afterwards. False when the renderer is not up yet, when the graphics
// card refused the shared texture, or from a backend that cannot do it (only DX11 can so far).
// What is shared, and how a reader finds it, is in Extender/Client/IMGUI/FrameShare.inl.
bool MirrorEnable(bool enabled)
{
    return gExtender->IMGUI().EnableFrameMirror(enabled);
}

// Ext.ClientRender.MirrorInfo()
//
// What the frame mirror is doing, or nil from a backend that cannot do it:
//
//   Enabled             mirroring is on
//   Section             the shared block of memory a reader opens first
//   TextureName         the shared texture named in it (empty while there is none)
//   Width, Height       its size: the back buffer's own
//   Format              its DXGI_FORMAT
//   HDR10               the frame is PQ-encoded Rec. 2020 and a reader has to convert it
//   Frame               copies finished
//   Presents            frames the game has shown, mirrored or not - the one to measure with
//   Skipped             frames not copied because the reader was holding the texture
//   CopyMicroseconds    the average cost of the copy on the rendering thread
UserReturn MirrorInfo(lua_State* L)
{
    auto info = gExtender->IMGUI().FrameMirrorStatus();
    if (!info) {
        push(L, nullptr);
        return 1;
    }

    lua_newtable(L);
    settable(L, "Enabled", info->Enabled);
    settable(L, "Section", info->Section);
    settable(L, "TextureName", info->TextureName);
    settable(L, "Width", info->Width);
    settable(L, "Height", info->Height);
    settable(L, "Format", info->Format);
    settable(L, "HDR10", info->HDR10);
    settable(L, "Frame", info->Frame);
    settable(L, "Presents", info->Presents);
    settable(L, "Skipped", info->Skipped);
    settable(L, "CopyMicroseconds", info->CopyMicroseconds);
    return 1;
}

// Ext.ClientRender.FrameCap()
//
// How fast the game is allowed to draw, or nil when the engine's layout is not the one this build
// knows (see Extender/Client/FrameCap.inl, which refuses to write anything in that case):
//
//   FrameCap               frames a second asked for; 0 is no cap
//   EffectiveFrameCap      frames a second actually in force right now
//   LostFocusFrameCap      the cap used while the window is not in front; 0 is no throttle
//   MicrosecondsPerFrame   what the engine's clock derives from the cap in force
//   HasFocus               whether the engine believes its window is the one in front
//   YieldOnLostFocus       the engine's own switch for throttling in the background
UserReturn FrameCap(lua_State* L)
{
    auto app = FrameCapAccess::Probe();
    if (!app) {
        push(L, nullptr);
        return 1;
    }

    lua_newtable(L);
    settable(L, "FrameCap", app->FrameCap());
    settable(L, "EffectiveFrameCap", app->EffectiveFrameCap());
    settable(L, "LostFocusFrameCap", app->LostFocusFrameCap());
    settable(L, "MicrosecondsPerFrame", app->MicrosecondsPerFrame());
    settable(L, "HasFocus", app->HasFocus());
    settable(L, "YieldOnLostFocus", FrameCapAccess::YieldOnLostFocus());
    return 1;
}

// Ext.ClientRender.SetFrameCap(fps)
//
// Caps the game at `fps` frames a second, in front or not: the background cap is set with it, so
// that a copy nobody is looking at draws at the rate it was given rather than the rate the engine
// would have chosen for it. `fps` of 0 lifts both, which is also how the background throttle is
// turned off.
//
// This is the same arithmetic BaseApp::SetFrameCap does, written into the same fields; nothing is
// hooked and nothing is patched. False when the engine is not up yet or its layout has moved.
bool SetFrameCap(uint32_t fps)
{
    if (fps > FrameCapAccess::MaxFrameCap) {
        OsiError("Frame cap out of range: " << fps);
        return false;
    }

    auto app = FrameCapAccess::Probe();
    if (!app) return false;

    app->SetFrameCap((uint16_t)fps);
    app->SetLostFocusFrameCap((uint16_t)fps);
    return true;
}

// Ext.ClientRender.SetLostFocusFrameCap(fps)
//
// The cap that applies only while the window is not the one in front, on its own. 0 lifts it, so
// that a copy in the background draws as fast as one in front. Of use for measuring what the
// engine does by itself; SetFrameCap is the one to reach for.
bool SetLostFocusFrameCap(uint32_t fps)
{
    if (fps > FrameCapAccess::MaxFrameCap) {
        OsiError("Frame cap out of range: " << fps);
        return false;
    }

    auto app = FrameCapAccess::Probe();
    if (!app) return false;

    app->SetLostFocusFrameCap((uint16_t)fps);
    return true;
}

// Ext.ClientRender.KeepInBackground(enabled)
//
// Whether this copy may bring its own window to the front. With it on, every attempt - the game's
// own and SDL_RaiseWindow's, which calls SetForegroundWindow with no condition at all - is
// refused and written down instead of being passed on, so a copy started by a tool cannot take
// the keyboard from whoever is using the machine.
//
// Nothing is hidden, moved or disabled by this: a person who clicks the window still gets it. It
// starts from the KeepWindowInBackground setting and can be turned off here once a copy is meant
// to be played in. Returns whether it is on afterwards.
bool KeepInBackground(bool enabled)
{
    windowfocus::KeepInBackground(enabled);
    return windowfocus::KeepingInBackground();
}

// Ext.ClientRender.KeepOffScreen(enabled)
//
// Puts this copy's window where no screen reaches, or brings it back. Meant for a copy something
// else draws - a deck's tile - and set from KeepWindowOffScreen at startup, which is the only
// moment early enough for the window never to be seen at all. The window stays a shown window, so
// pictures, keys and the list of copies all still find it.
bool KeepOffScreen(bool enabled)
{
    windowfocus::KeepOffScreen(enabled);
    return enabled;
}

// Ext.ClientRender.ForegroundLog()
//
// Every attempt to bring this copy's window to the front since the game started, oldest first:
//
//   Seconds    when, from the moment the hooks went in
//   Window     the window asked for
//   Caller     module+offset of the code that asked, innermost first - this is the answer to
//              "who raises this window", with the game's own modules named
//   Function   SetForegroundWindow or SetActiveWindow
//   Refused    whether it was refused (KeepInBackground was on at that moment)
//
// It is kept whether or not anything is being refused, because the question "does this copy ever
// ask" is worth answering on its own.
UserReturn ForegroundLog(lua_State* L)
{
    auto attempts = windowfocus::Attempts();
    lua_newtable(L);
    int index = 1;
    for (auto const& attempt : attempts) {
        push(L, index++);
        lua_newtable(L);
        settable(L, "Seconds", attempt.Seconds);
        settable(L, "Window", attempt.Window);
        settable(L, "Caller", attempt.Caller);
        settable(L, "Function", attempt.Function);
        settable(L, "Refused", attempt.Refused);
        lua_settable(L, -3);
    }
    return 1;
}

void RegisterRenderLib()
{
    DECLARE_MODULE(Render, Client)
    BEGIN_MODULE()
    MODULE_FUNCTION(Screenshot)
    MODULE_FUNCTION(ScreenshotPending)
    MODULE_FUNCTION(MirrorEnable)
    MODULE_FUNCTION(MirrorInfo)
    MODULE_FUNCTION(FrameCap)
    MODULE_FUNCTION(SetFrameCap)
    MODULE_FUNCTION(SetLostFocusFrameCap)
    MODULE_FUNCTION(KeepInBackground)
    MODULE_FUNCTION(KeepOffScreen)
    MODULE_FUNCTION(ForegroundLog)
    END_MODULE()
}

END_NS()
