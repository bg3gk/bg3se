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

void RegisterRenderLib()
{
    DECLARE_MODULE(Render, Client)
    BEGIN_MODULE()
    MODULE_FUNCTION(Screenshot)
    MODULE_FUNCTION(ScreenshotPending)
    END_MODULE()
}

END_NS()
