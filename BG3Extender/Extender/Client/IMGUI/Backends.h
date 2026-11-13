#pragma once

#include <GameDefinitions/Base/Base.h>

BEGIN_NS(extui)

using TextureOpaqueHandle = void*;

struct TextureLoadResult
{
    TextureOpaqueHandle OpaqueHandle;
    uint32_t Width;
    uint32_t Height;
};

// What the extender UI is drawn with and into; the formats are DXGI_FORMAT (DX11) or VkFormat (Vulkan) values
struct BackendInfo
{
    char const* Renderer{ "" };
    int32_t SwapChainFormat{ 0 };
    // DX11 only: the target bound when the UI is drawn (the format of its view and of the texture behind it)
    int32_t RenderTargetViewFormat{ 0 };
    int32_t RenderTargetFormat{ 0 };
    // The swap chain is HDR10 (10 bit, ST.2084 on an HDR display) and the UI's colours are converted for it
    bool HDR10{ false };
    // Brightness sRGB white is shown at in that case
    float PaperWhiteNits{ 0.0f };
};

class RenderingBackend
{
public:
    virtual ~RenderingBackend();
    virtual void EnableHooks() = 0;
    virtual void DisableHooks() = 0;
    virtual void InitializeUI() = 0;
    virtual void DestroyUI() = 0;
    virtual void NewFrame() = 0;
    virtual void FinishFrame() = 0;
    virtual void ClearFrame() = 0;
    virtual std::optional<TextureLoadResult> RegisterTexture(TextureDescriptor* descriptor) = 0;
    virtual void UnregisterTexture(TextureOpaqueHandle opaqueHandle) = 0;
    virtual std::optional<ImTextureID> BindTexture(TextureOpaqueHandle opaqueHandle) = 0;
    virtual bool IsInitialized() = 0;
    virtual glm::ivec2 GetViewportSize() = 0;
    virtual BackendInfo GetBackendInfo() = 0;
    // A texture made from RGBA8 pixels (an image file a mod brings), and its release.
    // A backend that cannot make one returns nothing.
    virtual std::optional<TextureOpaqueHandle> CreateTexture(uint8_t const* rgba, uint32_t width, uint32_t height) { return {}; }
    virtual void DestroyTexture(TextureOpaqueHandle opaqueHandle) {}
    // Brightness of sRGB white on an HDR display, in nits; no effect on SDR output
    virtual void SetPaperWhite(float nits) {}
    // Asks for the next frame the game shows to be written as a PNG, before anything of the
    // extender's is drawn over it. The path is below the Script Extender folder of the player's
    // profile, as Ext.IO.SaveFile() takes it. False when one is already on its way, or from a
    // backend that cannot read its frames.
    virtual bool RequestScreenshot(StringView path) { return false; }
    // Whether a requested screenshot has still to be taken or written
    virtual bool ScreenshotPending() { return false; }
};

END_NS()
