#include <Extender/ScriptExtender.h>

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

void RegisterRenderLib()
{
    DECLARE_MODULE(Render, Client)
    BEGIN_MODULE()
    MODULE_FUNCTION(Screenshot)
    MODULE_FUNCTION(ScreenshotPending)
    MODULE_FUNCTION(MirrorEnable)
    MODULE_FUNCTION(MirrorInfo)
    END_MODULE()
}

END_NS()
