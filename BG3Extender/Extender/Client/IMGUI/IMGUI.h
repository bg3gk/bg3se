#pragma once

#include <Extender/Client/IMGUI/Backends.h>
#include <imgui.h>

BEGIN_NS(extui)

inline ImVec2 ToImVec(glm::vec2 const& v)
{
    return ImVec2(v.x, v.y);
}

inline ImVec4 ToImVec(glm::vec4 const& v)
{
    return ImVec4(v.x, v.y, v.z, v.w);
}

inline ImVec2 ToImVec(std::optional<glm::vec2> const& v, ImVec2 defaultVal = ImVec2(0.0f, 0.0f))
{
    return v ? ImVec2(v->x, v->y) : defaultVal;
}

class IMGUIObjectManager;

class IMGUITextureLoader
{
public:
    // Number of frames to wait before releasing rendering resources
    static constexpr unsigned DeleteAfterFrames = 3;

    void BindRenderer(RenderingBackend*);
    void Update();
    std::optional<TextureLoadResult> IncTextureRef(FixedString const& textureGuid);
    bool DecTextureRef(TextureOpaqueHandle id, FixedString const& textureGuid);

private:
    struct TextureRefCount
    {
        TextureDescriptor* Descriptor{ nullptr };
        TextureLoadResult LoadResult;
        uint32_t RefCount{ 0 };
    };
    
    struct TextureUnloadRequest
    {
        TextureOpaqueHandle Id{ 0 };
        uint32_t WaitForFrames{ 0 };
    };

    RenderingBackend* renderer_{ nullptr };
    HashMap<FixedString, TextureRefCount> refCounts_;
    HashMap<FixedString, TextureUnloadRequest> pendingUnloads_;
};

class IMGUIManager
{
public:
    struct FontData
    {
        FixedString Key;
        STDString Path;
        float SizePixels{ .0 };
        ImFont* Font{ nullptr };
        // A second font file whose glyphs in [MergeFirst, MergeLast] are added to this font
        STDString MergePath;
        uint32_t MergeFirst{ 0 };
        uint32_t MergeLast{ 0 };
    };

    IMGUIManager(SDLManager& sdl);
    ~IMGUIManager();

    inline bool WasUIInitialized() const
    {
        return initialized_;
    }

    void EnableHooks();
    void DisableHooks();

    void InitializeUI();
    void DestroyUI();
    void EnableUI(bool enabled);
    void SetObjects(IMGUIObjectManager* objects);
    std::optional<TextureLoadResult> RegisterTexture(FixedString const& textureGuid);
    void UnregisterTexture(TextureOpaqueHandle id, FixedString const& textureGuid);
    std::optional<ImTextureID> BindTexture(TextureOpaqueHandle opaqueHandle);
    // Textures from image files (png, jpg, tga, bmp) in the game's data, a mod's pak included.
    // Counted by use; an unused one is kept for a while before its texture is released.
    std::optional<TextureLoadResult> RegisterFileTexture(FixedString const& path);
    void UnregisterFileTexture(FixedString const& path);
    bool LoadFont(FixedString const& name, char const* path, float size,
        char const* mergePath = nullptr, uint32_t mergeFirst = 0, uint32_t mergeLast = 0);
    FontData* GetFont(FixedString const& name);
    Array<FixedString> GetFontNames();
    void SetScale(float scale);
    void SetUIScaleMultiplier(float scale);
    void SetFontScaleMultiplier(float scale);
    glm::ivec2 GetViewportSize();
    BackendInfo GetBackendInfo();
    void SetPaperWhite(float nits);

    void OnRenderBackendInitialized();
    void Update();

private:
    SDLManager& sdl_;
    std::unique_ptr<RenderingBackend> renderer_;
    HashMap<FixedString, FontData> fonts_;
    float scale_{ 1.0f };
    float uiScaleMultiplier_{ 1.0f };
    float fontScaleMultiplier_{ 1.0f };
    float requestedScale_{ 1.0f };
    float requestedUiScaleMultiplier_{ 1.0f };
    float requestedFontScaleMultiplier_{ 1.0f };
    IMGUITextureLoader textureLoader_;

    IMGUIObjectManager* objects_{ nullptr };

    bool initialized_{ false };
    bool enableUI_{ false };
    unsigned frameNo_{ 0 };

    bool LoadFont(FontData& request);

    struct FileTexture
    {
        TextureLoadResult LoadResult;
        uint32_t RefCount{ 0 };
        // Frames left until release once nothing uses the texture
        uint32_t ReleaseCountdown{ 0 };
    };

    // Long enough for "ask its size, then show it" not to decode the file twice, and for
    // frames already handed to the renderer to finish
    static constexpr uint32_t FileTextureReleaseFrames = 120;
    HashMap<FixedString, FileTexture> fileTextures_;
    void UpdateFileTextures();
    void OnViewportUpdated();
    void UpdateStyle();
};

END_NS()
