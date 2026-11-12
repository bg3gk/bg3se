#pragma once

#include <GameDefinitions/Base/Base.h>
#include <Extender/Client/IMGUI/Backends.h>
#include <Extender/Client/IMGUI/TextureMirror.inl>
#include <Extender/ScriptExtender.h>
#include <CoreLib/Wrappers.h>
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <imgui_internal.h>
#include <dxgi1_6.h>
#include <dxgi1_2.h>
#include <d3d11.h>

BEGIN_SE()

using D3D11CreateDeviceProc = HRESULT WINAPI (_In_opt_ IDXGIAdapter*,
    D3D_DRIVER_TYPE, HMODULE, UINT,
    _In_reads_opt_(FeatureLevels) CONST D3D_FEATURE_LEVEL*,
    UINT FeatureLevels, UINT, _COM_Outptr_opt_ ID3D11Device**,
    _Out_opt_ D3D_FEATURE_LEVEL*, _COM_Outptr_opt_ ID3D11DeviceContext**);

using CreateDXGIFactory1Proc = HRESULT WINAPI (REFIID riid, _COM_Outptr_ void** ppFactory);

using DXGICreateSwapChainForHwndProc = HRESULT STDMETHODCALLTYPE (
    IDXGIFactory2* pFactory,
    _In_  IUnknown* pDevice,
    _In_  HWND hWnd,
    _In_  const DXGI_SWAP_CHAIN_DESC1* pDesc,
    _In_opt_  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    _In_opt_  IDXGIOutput* pRestrictToOutput,
    _COM_Outptr_  IDXGISwapChain1** ppSwapChain);

using DXGISwapChainPresentProc = HRESULT STDMETHODCALLTYPE(
    IDXGISwapChain* pSwapChain,
    /* [in] */ UINT SyncInterval,
    /* [in] */ UINT Flags);

using DXGISwapChainResizeBuffersProc = HRESULT STDMETHODCALLTYPE(
    IDXGISwapChain* pSwapChain,
    /* [in] */ UINT BufferCount,
    /* [in] */ UINT Width,
    /* [in] */ UINT Height,
    /* [in] */ DXGI_FORMAT NewFormat,
    /* [in] */ UINT SwapChainFlags);

enum class D3D11CreateDeviceHookTag{};
using D3D11CreateDeviceHookType = WrappableFunction<D3D11CreateDeviceHookTag, D3D11CreateDeviceProc>;
D3D11CreateDeviceHookType* D3D11CreateDeviceHookType::gHook;

enum class CreateDXGIFactory1HookTag{};
using CreateDXGIFactory1HookType = WrappableFunction<CreateDXGIFactory1HookTag, CreateDXGIFactory1Proc>;
CreateDXGIFactory1HookType* CreateDXGIFactory1HookType::gHook;

enum class DXGICreateSwapChainForHwndHookTag{};
using DXGICreateSwapChainForHwndHookType = WrappableFunction<DXGICreateSwapChainForHwndHookTag, DXGICreateSwapChainForHwndProc>;
DXGICreateSwapChainForHwndHookType* DXGICreateSwapChainForHwndHookType::gHook;

enum class DXGISwapChainPresentHookTag{};
using DXGISwapChainPresentHookType = WrappableFunction<DXGISwapChainPresentHookTag, DXGISwapChainPresentProc>;
DXGISwapChainPresentHookType* DXGISwapChainPresentHookType::gHook;

enum class DXGISwapChainResizeBuffersHookTag{};
using DXGISwapChainResizeBuffersHookType = WrappableFunction<DXGISwapChainResizeBuffersHookTag, DXGISwapChainResizeBuffersProc>;
DXGISwapChainResizeBuffersHookType* DXGISwapChainResizeBuffersHookType::gHook;

END_SE()

BEGIN_NS(extui)

class DX11Backend : public RenderingBackend
{
public:
    DX11Backend(IMGUIManager& ui) : ui_(ui) {}

    ~DX11Backend() override
    {
        IMGUI_DEBUG("Destroying DX backend");
        DestroyUI();
        DisableHooks();
    }

    void EnableHooks() override
    {
        if (CreateDeviceHook_.IsWrapped()) return;

        IMGUI_DEBUG("DX11Backend::EnableHooks()");

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        auto dx11Lib = LoadLibraryW(L"d3d11.dll");
        auto createDevice = (D3D11CreateDeviceProc*)GetProcAddress(dx11Lib, "D3D11CreateDevice");

        auto dxgiLib = LoadLibraryW(L"dxgi.dll");
        auto createDxgiFactory = (CreateDXGIFactory1Proc*)GetProcAddress(dxgiLib, "CreateDXGIFactory1");

        CreateDeviceHook_.Wrap(ResolveFunctionTrampoline(createDevice));
        CreateDxgiFactoryHook_.Wrap(ResolveFunctionTrampoline(createDxgiFactory));
        DetourTransactionCommit();

        CreateDeviceHook_.SetPostHook(&DX11Backend::D3D11CreateDeviceHooked, this);
        CreateDxgiFactoryHook_.SetPostHook(&DX11Backend::CreateDXGIFactory1Hooked, this);
        DXGICreateSwapChainForHwndHook_.SetPostHook(&DX11Backend::DXGICreateSwapChainForHwndHooked, this);
        DXGISwapChainPresentHook_.SetPreHook(&DX11Backend::DXGISwapChainPresentHooked, this);
        DXGISwapChainResizeBuffersHook_.SetPostHook(&DX11Backend::DXGISwapChainResizeBuffersHooked, this);
    }

    void DisableHooks() override
    {
        if (!CreateDeviceHook_.IsWrapped()) return;

        IMGUI_DEBUG("DX11Backend::DisableHooks()");

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        CreateDeviceHook_.Unwrap();
        CreateDxgiFactoryHook_.Unwrap();
        DetourTransactionCommit();
    }

    void InitializeUI() override
    {
        if (initialized_) return;

        IMGUI_DEBUG("DX11Backend::InitializeUI()");
        ImGui_ImplDX11_Init(device_, context_);

        initializationFailed_ = false;
        renderObjectsCreated_ = false;
        initialized_ = true;
    }

    void DestroyUI() override
    {
        if (!initialized_) return;

        IMGUI_DEBUG("DX11Backend::DestroyUI()");
        mirroredTextures_.Destroy(&ImGui_ImplDX11_UpdateTexture);
        ImGui_ImplDX11_Shutdown();
        initialized_ = false;
        renderObjectsCreated_ = false;
    }

    void NewFrame() override
    {
        curViewport_ = (curViewport_ + 1) % viewports_.size();
        GImGui->Viewports[0] = &viewports_[curViewport_].Viewport;

        if (!renderObjectsCreated_ && !initializationFailed_) {
            std::lock_guard _(globalResourceLock_);
            OPTICK_EVENT("CreateDeviceObjects", Optick::Category::Rendering);
            IMGUI_DEBUG("Re-initializing DX11 render objects");
            initializationFailed_ = !ImGui_ImplDX11_CreateDeviceObjects();
            if (initializationFailed_) {
                ERR("Failed to initialize IMGUI DX11 backend!");
            } else {
                renderObjectsCreated_ = true;
            }
        }

        if (!initializationFailed_) {
            ImGui_ImplDX11_NewFrame();
        }
    }

    void FinishFrame() override
    {
        OPTICK_EVENT(Optick::Category::Rendering);
        std::lock_guard _(globalResourceLock_);
        auto& vp = viewports_[curViewport_].Viewport;
        auto& drawLists = viewports_[curViewport_].ClonedDrawLists;

        for (auto list : drawLists) {
            list->~ImDrawList();
            IM_FREE(list);
        }

        drawLists.clear();

        for (int i = 0; i < vp.DrawDataP.CmdLists.Size; i++) {
            auto list = vp.DrawDataP.CmdLists[i];
            auto drawList = list->CloneOutput();
            vp.DrawDataP.CmdLists[i] = drawList;
            drawLists.push_back(drawList);
        }

        mirroredTextures_.Sync(vp.DrawDataP);
        drawViewport_ = curViewport_;
    }

    void ClearFrame() override
    {
        drawViewport_ = -1;
    }
    
    bool IsInitialized() override
    {
        return initialized_;
    }

    glm::ivec2 GetViewportSize() override
    {
        return glm::ivec2(width_, height_);
    }

    BackendInfo GetBackendInfo() override
    {
        return info_;
    }

    std::optional<TextureOpaqueHandle> CreateTexture(uint8_t const* rgba, uint32_t width, uint32_t height) override
    {
        if (device_ == nullptr) return {};

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = rgba;
        data.SysMemPitch = width * 4;

        ID3D11Texture2D* texture{ nullptr };
        if (FAILED(device_->CreateTexture2D(&desc, &data, &texture)) || texture == nullptr) {
            return {};
        }

        ID3D11ShaderResourceView* view{ nullptr };
        auto result = device_->CreateShaderResourceView(texture, nullptr, &view);
        // The view keeps the texture alive
        texture->Release();
        if (FAILED(result) || view == nullptr) {
            return {};
        }

        return reinterpret_cast<TextureOpaqueHandle>(view);
    }

    void DestroyTexture(TextureOpaqueHandle opaqueHandle) override
    {
        if (opaqueHandle != nullptr) {
            reinterpret_cast<ID3D11ShaderResourceView*>(opaqueHandle)->Release();
        }
    }

    void SetPaperWhite(float nits) override
    {
        info_.PaperWhiteNits = std::clamp(nits, 10.0f, 10000.0f);
    }

    // The game presents HDR10 when the display is in HDR mode: a 10-bit swap chain whose values
    // are PQ-encoded. sRGB values written there as they are come out far too bright (0.5 is
    // already paper white), so the UI's colours have to be converted.
    void UpdateHDRState(IDXGISwapChain* swapChain)
    {
        bool hdr = false;
        if (info_.SwapChainFormat == (int32_t)DXGI_FORMAT_R10G10B10A2_UNORM) {
            IDXGIOutput* output{ nullptr };
            if (SUCCEEDED(swapChain->GetContainingOutput(&output)) && output != nullptr) {
                IDXGIOutput6* output6{ nullptr };
                if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), (void**)&output6)) && output6 != nullptr) {
                    DXGI_OUTPUT_DESC1 desc;
                    if (SUCCEEDED(output6->GetDesc1(&desc))) {
                        hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    }
                    output6->Release();
                }
                output->Release();
            }
        }

        info_.HDR10 = hdr;
        ImGui_ImplDX11_SetHDR10Output(hdr, info_.PaperWhiteNits);
    }

    std::optional<TextureLoadResult> RegisterTexture(TextureDescriptor* descriptor) override
    {
        if (descriptor->DX11.Views.size() != 1 || !descriptor->DX11.Views[0].View) {
            ERR("DX11 texture has no render view?");
            return {};
        }

        auto view = descriptor->DX11.Views[0].View;
        return TextureLoadResult{ reinterpret_cast<TextureOpaqueHandle>(view), descriptor->DX11.Width, descriptor->DX11.Height };
    }

    void UnregisterTexture(TextureOpaqueHandle opaqueHandle) override
    {
    }

    std::optional<ImTextureID> BindTexture(TextureOpaqueHandle opaqueHandle) override
    {
        return (ImTextureID)opaqueHandle;
    }

private:
    struct ViewportInfo
    {
        ImGuiViewportP Viewport;
        Array<ImDrawList*> ClonedDrawLists;
    };

    void WINAPI D3D11CreateDeviceHooked(IDXGIAdapter* pAdapter,
        D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels,
        UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice,
        D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext, HRESULT result)
    {
        IMGUI_DEBUG("D3D11CreateDevice -> %p, %d", *ppDevice, result);
        IMGUI_DEBUG("DriverType %x, Software %p, Flags %x, FeatureLevels %d, SDKVersion %x, ppImmediateContext %p", DriverType, Software, Flags, FeatureLevels, SDKVersion, ppImmediateContext);

        for (uint32_t i = 0; i < FeatureLevels; i++) {
            IMGUI_DEBUG("FeatureLevel=%x", pFeatureLevels[i]);
        }

        if (!SUCCEEDED(result)) return;

        if (device_ == nullptr && ppImmediateContext != nullptr 
            // Filter out extra device created by nvidia overlay
            && FeatureLevels == 1
            && (*pFeatureLevels == D3D_FEATURE_LEVEL_11_1 || *pFeatureLevels == D3D_FEATURE_LEVEL_11_0)) {
            adapter_ = pAdapter;
            context_ = *ppImmediateContext;
            device_ = *ppDevice;
            IMGUI_DEBUG("Bind: Adapter %p, Context %p, Device %p", adapter_, context_, device_);
        }
    }

    void WINAPI CreateDXGIFactory1Hooked(REFIID riid, void** ppFactory, HRESULT result)
    {
        IMGUI_DEBUG("CreateDXGIFactory1 -> %p, %d", *ppFactory, result);
        IMGUI_DEBUG("RIID: %08x %04x %04x %016llx", riid.Data1, riid.Data2, riid.Data3, *(uint64_t*)&riid.Data4[0]);

        if (!SUCCEEDED(result)) return;
        if (riid != IID_IDXGIFactory1) return;

        if (DXGICreateSwapChainForHwndHook_.IsWrapped()) return;

        factory_ = (IDXGIFactory2*)*ppFactory;
        auto createSwapChain = (*(void***)factory_)[15];

        IMGUI_DEBUG("Hooking DXGICreateSwapChainForHwnd");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DXGICreateSwapChainForHwndHook_.Wrap(ResolveFunctionTrampoline(createSwapChain));
        DetourTransactionCommit();
    }

    void STDMETHODCALLTYPE DXGICreateSwapChainForHwndHooked(
        IDXGIFactory2* pFactory,
        IUnknown* pDevice,
        HWND hWnd,
        const DXGI_SWAP_CHAIN_DESC1* pDesc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
        IDXGIOutput* pRestrictToOutput,
        IDXGISwapChain1** ppSwapChain, 
        HRESULT result)
    {
        IMGUI_DEBUG("DXGICreateSwapChainForHwnd -> %p, %d", *ppSwapChain, result);
        IMGUI_DEBUG("Device %p, HWND %d, %d x %d, fmt %d, usage %d, flags %x", pDevice, hWnd, pDesc->Width, pDesc->Height,
            pDesc->Format, pDesc->BufferUsage, pDesc->Flags);

        if (!SUCCEEDED(result)) return;
        
        swapChain_ = *ppSwapChain;
        width_ = pDesc->Width;
        height_ = pDesc->Height;
        info_.SwapChainFormat = (int32_t)pDesc->Format;

        if (DXGISwapChainPresentHook_.IsWrapped()) return;

        auto createSwapChain = (*(void***)swapChain_)[8];
        auto resizeBuffers = (*(void***)swapChain_)[13];

        IMGUI_DEBUG("Hooking DXGISwapChainPresent");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DXGISwapChainPresentHook_.Wrap(ResolveFunctionTrampoline(createSwapChain));
        DXGISwapChainResizeBuffersHook_.Wrap(ResolveFunctionTrampoline(resizeBuffers));
        DetourTransactionCommit();

        if (swapChain_ != nullptr && device_ != nullptr) {
            ui_.OnRenderBackendInitialized();
        }
    }

    void STDMETHODCALLTYPE DXGISwapChainPresentHooked(
        IDXGISwapChain* pSwapChain,
        /* [in] */ UINT SyncInterval,
        /* [in] */ UINT Flags)
    {
        std::lock_guard _(globalResourceLock_);
        if (pSwapChain != swapChain_
            || !initialized_
            || initializationFailed_
            || !renderObjectsCreated_
            || drawViewport_ == -1) {
            return;
        }

        auto& vp = viewports_[drawViewport_].Viewport;
        if (!vp.DrawDataP.Valid) return;

        mirroredTextures_.BeginRender();
        // Looking up the display is not free; twice a second is enough to follow an HDR toggle
        if ((hdrCheckCountdown_--) <= 0) {
            hdrCheckCountdown_ = 30;
            UpdateHDRState(pSwapChain);
        } else {
            ImGui_ImplDX11_SetHDR10Output(info_.HDR10, info_.PaperWhiteNits);
        }

        // The UI is drawn into whatever target the game left bound; note what that is
        ID3D11RenderTargetView* target{ nullptr };
        context_->OMGetRenderTargets(1, &target, nullptr);
        if (target != nullptr) {
            D3D11_RENDER_TARGET_VIEW_DESC viewDesc;
            target->GetDesc(&viewDesc);
            info_.RenderTargetViewFormat = (int32_t)viewDesc.Format;

            ID3D11Resource* resource{ nullptr };
            target->GetResource(&resource);
            if (resource != nullptr) {
                D3D11_RESOURCE_DIMENSION dimension;
                resource->GetType(&dimension);
                if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                    D3D11_TEXTURE2D_DESC textureDesc;
                    static_cast<ID3D11Texture2D*>(resource)->GetDesc(&textureDesc);
                    info_.RenderTargetFormat = (int32_t)textureDesc.Format;
                }
                resource->Release();
            }
            target->Release();
        }

        ImGui_ImplDX11_RenderDrawData(&vp.DrawDataP);
    }

    void STDMETHODCALLTYPE DXGISwapChainResizeBuffersHooked(
        IDXGISwapChain* pSwapChain,
        /* [in] */ UINT BufferCount,
        /* [in] */ UINT Width,
        /* [in] */ UINT Height,
        /* [in] */ DXGI_FORMAT NewFormat,
        /* [in] */ UINT SwapChainFlags,
        HRESULT result)
    {
        if (!SUCCEEDED(result)) return;
        if (pSwapChain != swapChain_) return;

        IMGUI_DEBUG("DXGISwapChainResizeBuffers -> (%d, %d)", Width, Height);
        width_ = Width;
        height_ = Height;
        if (NewFormat != DXGI_FORMAT_UNKNOWN) {
            info_.SwapChainFormat = (int32_t)NewFormat;
        }
    }


    IMGUIManager& ui_;
    std::array<ViewportInfo, 3> viewports_;
    int32_t drawViewport_{ -1 };
    int32_t curViewport_{ 0 };
    TextureMirrorSet mirroredTextures_;

    IDXGIAdapter* adapter_{ nullptr };
    ID3D11DeviceContext* context_{ nullptr };
    ID3D11Device* device_{ nullptr };
    IDXGIFactory1* factory_{ nullptr };
    IDXGISwapChain* swapChain_{ nullptr };

    bool initialized_{ false };
    bool renderObjectsCreated_{ false };
    bool initializationFailed_{ false };
    std::mutex globalResourceLock_;
    uint32_t width_{ 0 };
    uint32_t height_{ 0 };
    BackendInfo info_{ .Renderer = "DX11", .PaperWhiteNits = 200.0f };
    int32_t hdrCheckCountdown_{ 0 };

    D3D11CreateDeviceHookType CreateDeviceHook_;
    CreateDXGIFactory1HookType CreateDxgiFactoryHook_;
    DXGICreateSwapChainForHwndHookType DXGICreateSwapChainForHwndHook_;
    DXGISwapChainPresentHookType DXGISwapChainPresentHook_;
    DXGISwapChainResizeBuffersHookType DXGISwapChainResizeBuffersHook_;
};

END_NS()
