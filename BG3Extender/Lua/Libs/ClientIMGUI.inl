#include <Extender/ScriptExtender.h>
#include <Extender/Client/IMGUI/Objects.h>

/// <lua_module>IMGUI</lua_module>
BEGIN_NS(ecl::lua::imgui)

ImguiHandle NewWindow(lua_State* L, char const* name)
{
    auto& ui = ClientState::FromLua(L)->IMGUI();
    auto window = ui.CreateRenderable<extui::Window>();
    window->Label = name;

    if (window->Label.empty()) {
        WARN_ONCE("Creating window with empty label - window will not be drawn until a Label or IDContext is assigned!");
    }

    return window;
}

void EnableDemo(lua_State* L, bool enabled)
{
    WARN("EnableDemo() is deprecated, use ToggleDebugTools() instead");
}

void ToggleDebugTools(lua_State* L, GuiDebugToolFlags flags)
{
    ClientState::FromLua(L)->IMGUI().ToggleDebugTools(flags);
}

bool LoadFont(FixedString const& name, char const* path, float size)
{
    if (!gExtender->IMGUI().WasUIInitialized()) {
        ERR("LoadFont() called before IMGUI was initialized!");
        return false;
    }

    OPTICK_EVENT();
    return gExtender->IMGUI().LoadFont(name, path, size);
}

void SetScale(float scale)
{
    WARN("Ext.IMGUI.SetScale() is deprecated; UI scaling is managed by SE");
}

void SetUIScaleMultiplier(float scale)
{
    gExtender->IMGUI().SetUIScaleMultiplier(scale);
}

void SetFontScaleMultiplier(float scale)
{
    gExtender->IMGUI().SetFontScaleMultiplier(scale);
}

glm::ivec2 GetViewportSize()
{
    return gExtender->IMGUI().GetViewportSize();
}

// The clipboard is read and written through Win32 directly instead of ImGui::GetClipboardText():
// Lua does not run on the thread that owns the ImGui context.
std::optional<STDString> GetClipboardText()
{
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(NULL)) {
        return {};
    }

    std::optional<STDString> text;
    auto handle = GetClipboardData(CF_UNICODETEXT);
    if (handle != NULL) {
        auto data = (wchar_t const*)GlobalLock(handle);
        if (data != nullptr) {
            text.emplace(ToUTF8(WStringView(data)));
            GlobalUnlock(handle);
        }
    }

    CloseClipboard();
    return text;
}

bool SetClipboardText(StringView text)
{
    auto wide = FromUTF8(text);
    auto bytes = (wide.size() + 1) * sizeof(wchar_t);
    auto handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (handle == NULL) {
        return false;
    }

    auto data = (wchar_t*)GlobalLock(handle);
    memcpy(data, wide.c_str(), bytes);
    GlobalUnlock(handle);

    if (!OpenClipboard(NULL)) {
        GlobalFree(handle);
        return false;
    }

    EmptyClipboard();
    // On success the clipboard owns the memory
    bool ok = SetClipboardData(CF_UNICODETEXT, handle) != NULL;
    if (!ok) {
        GlobalFree(handle);
    }

    CloseClipboard();
    return ok;
}

void RegisterIMGUILib()
{
    DECLARE_MODULE(IMGUI, Client)
    BEGIN_MODULE()
    MODULE_FUNCTION(NewWindow)
    MODULE_FUNCTION(EnableDemo)
    MODULE_FUNCTION(ToggleDebugTools)
    MODULE_FUNCTION(LoadFont)
    MODULE_FUNCTION(SetScale)
    MODULE_FUNCTION(SetUIScaleMultiplier)
    MODULE_FUNCTION(SetFontScaleMultiplier)
    MODULE_FUNCTION(GetViewportSize)
    MODULE_FUNCTION(GetClipboardText)
    MODULE_FUNCTION(SetClipboardText)
    END_MODULE()
}

END_NS()
