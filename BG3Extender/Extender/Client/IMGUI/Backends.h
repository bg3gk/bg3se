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

// What a backend says about handing its frames to another program on the graphics card
// (see FrameShare.inl). Everything but the two names is read straight out of the shared block,
// so a reader that already has the block does not need this.
struct FrameMirrorInfo
{
    bool Enabled{ false };
    // The frame is HDR10 (PQ, Rec. 2020) and a reader has to convert it
    bool HDR10{ false };
    uint32_t Width{ 0 };
    uint32_t Height{ 0 };
    // DXGI_FORMAT of the shared texture: the back buffer's own
    uint32_t Format{ 0 };
    // Copies finished, and frames the game has shown whether mirroring or not
    uint64_t Frame{ 0 };
    uint64_t Presents{ 0 };
    // Frames not copied because the reader was holding the texture
    uint64_t Skipped{ 0 };
    // The average cost of the copy on the rendering thread
    double CopyMicroseconds{ 0.0 };
    STDString Section;
    STDString TextureName;
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
    // Starts or stops copying every finished frame into a texture another program can open on the
    // graphics card, so that a game can be watched without its frames ever coming back into
    // memory. False from a backend that cannot do it (only DX11 can so far).
    virtual bool EnableFrameMirror(bool enabled) { return false; }
    // What that mirror is doing, or nothing from a backend that cannot do it
    virtual std::optional<FrameMirrorInfo> FrameMirrorStatus() { return {}; }
};

END_NS()
