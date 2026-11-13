#pragma once

#include <GameDefinitions/Base/Base.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>

BEGIN_NS(extui)

// The finished frame handed to another program on the graphics card, so that it never has to be
// brought back into memory.
//
// A screenshot copies the back buffer down into system memory and encodes a PNG, which costs most
// of a second; that is right for writing a file and hopeless for watching a game. What a screen
// recorder does instead is keep one texture both programs can see and copy the frame into it on
// the card. This does that: one shared texture the size of the back buffer, filled in the present
// hook with a single CopyResource, and a small block of shared memory saying what the texture is
// called and how far along the game is. Nothing crosses into system memory and nothing is encoded.
//
// Three names are involved, all in the session's own object namespace and all carrying the game's
// process id, so several games on one machine never collide:
//
//   Local\GateKitFrameShare_<pid>            the block of shared memory below
//   Local\GateKitFrame_<pid>_<generation>    the shared texture (a new one after a resize)
//
// The reader opens the block, reads the texture's name, opens the texture with
// ID3D11Device1::OpenSharedResourceByName on the adapter the block names, and from then on watches
// Frame. The keyed mutex both sides take on key 0 keeps a half-written frame from being drawn: a
// side that cannot have it at once skips that frame rather than holding the other one up. The game
// never waits.
//
// The block is created as soon as the backend presents its first frame, whether anything is
// mirroring or not, because Presents is what measures the cost of all this: the frame rate with
// mirroring on and off is a difference of that counter over time.
struct FrameShareHeader
{
    static constexpr uint32_t ExpectedMagic{ 0x53464B47 };   // 'GKFS'
    static constexpr uint32_t ExpectedVersion{ 1 };

    uint32_t Magic;
    uint32_t Version;
    // Mirroring is on. Written by Lua (Ext.ClientRender.MirrorEnable), read by the render thread
    // and by whoever is watching.
    uint32_t Enabled;
    uint32_t Width;
    uint32_t Height;
    uint32_t Format;            // DXGI_FORMAT of the shared texture: the back buffer's own
    uint32_t HDR10;             // the frame is PQ-encoded Rec.2020; a reader has to convert it
    uint32_t Generation;        // bumped whenever the texture is made again; the name changes with it
    uint32_t Reserved0;
    uint32_t Reserved1;

    uint64_t Frame;             // copies finished
    uint64_t Presents;          // frames the game has shown, mirrored or not
    uint64_t FrameQpc;          // QueryPerformanceCounter when the last copy was queued
    uint64_t QpcFrequency;      // its frequency, so a reader needs no assumption
    uint64_t AdapterLuid;       // the adapter the texture lives on (LUID as written by DXGI)
    uint64_t CopyTicks;         // QPC ticks spent in the copy, in all
    uint64_t CopyCount;         // copies those ticks are from
    uint64_t Skipped;           // frames not copied because the reader held the texture

    wchar_t TextureName[64];
};

class FrameShare
{
public:
    ~FrameShare()
    {
        ReleaseTexture();
        if (header_ != nullptr) {
            UnmapViewOfFile(header_);
            header_ = nullptr;
        }
        if (section_ != nullptr) {
            CloseHandle(section_);
            section_ = nullptr;
        }
    }

    // Lua. True when mirroring is on afterwards; false when there is nowhere to put the frame yet.
    bool Enable(bool enabled)
    {
        if (!EnsureSection()) return false;
        header_->Enabled = enabled ? 1u : 0u;
        return header_->Enabled != 0;
    }

    std::optional<FrameMirrorInfo> Status()
    {
        if (!EnsureSection()) return {};
        FrameMirrorInfo info;
        info.Enabled = header_->Enabled != 0;
        info.HDR10 = header_->HDR10 != 0;
        info.Width = header_->Width;
        info.Height = header_->Height;
        info.Format = header_->Format;
        info.Frame = header_->Frame;
        info.Presents = header_->Presents;
        info.Skipped = header_->Skipped;
        auto frequency = header_->QpcFrequency ? header_->QpcFrequency : 1;
        info.CopyMicroseconds = header_->CopyCount == 0
            ? 0.0
            : (double)header_->CopyTicks * 1000000.0 / (double)frequency / (double)header_->CopyCount;
        info.Section = sectionName_;
        info.TextureName = textureName_;
        return info;
    }

    // The rendering thread, inside the present hook, with the game's frame finished and nothing of
    // the extender's drawn into it yet. Everything that is not the copy itself is counting.
    void OnPresent(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain, bool hdr10)
    {
        if (!EnsureSection()) return;
        header_->Presents++;

        if (header_->Enabled == 0) {
            if (texture_ != nullptr) {
                ReleaseTexture();
                PublishNothing();
            }
            return;
        }

        if (device == nullptr || context == nullptr || swapChain == nullptr) return;

        ID3D11Texture2D* back{ nullptr };
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || back == nullptr) return;

        D3D11_TEXTURE2D_DESC desc;
        back->GetDesc(&desc);

        if (!EnsureTexture(device, desc)) {
            back->Release();
            return;
        }

        header_->HDR10 = hdr10 ? 1u : 0u;

        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        // Zero milliseconds: a reader that is drawing the last frame keeps it, and the game goes on
        // rather than waiting for it.
        if (mutex_->AcquireSync(0, 0) == WAIT_OBJECT_0) {
            context->CopyResource(texture_, back);
            mutex_->ReleaseSync(0);

            LARGE_INTEGER end;
            QueryPerformanceCounter(&end);
            header_->CopyTicks += (uint64_t)(end.QuadPart - start.QuadPart);
            header_->CopyCount++;
            header_->FrameQpc = (uint64_t)end.QuadPart;
            header_->Frame++;
        } else {
            header_->Skipped++;
        }

        back->Release();
    }

    // The backend is going away, or its device is.
    void Reset()
    {
        ReleaseTexture();
        if (header_ != nullptr) PublishNothing();
    }

private:
    bool EnsureSection()
    {
        if (header_ != nullptr) return true;

        char name[64];
        sprintf_s(name, "Local\\GateKitFrameShare_%u", GetCurrentProcessId());
        sectionName_ = name;

        auto wide = FromUTF8(name);
        section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            (DWORD)sizeof(FrameShareHeader), wide.c_str());
        if (section_ == nullptr) {
            ERR("Could not make the shared block for frame mirroring (%d)", GetLastError());
            return false;
        }

        header_ = (FrameShareHeader*)MapViewOfFile(section_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FrameShareHeader));
        if (header_ == nullptr) {
            ERR("Could not map the shared block for frame mirroring (%d)", GetLastError());
            CloseHandle(section_);
            section_ = nullptr;
            return false;
        }

        memset(header_, 0, sizeof(FrameShareHeader));
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        header_->QpcFrequency = (uint64_t)frequency.QuadPart;
        header_->Version = FrameShareHeader::ExpectedVersion;
        header_->Magic = FrameShareHeader::ExpectedMagic;
        return true;
    }

    bool EnsureTexture(ID3D11Device* device, D3D11_TEXTURE2D_DESC const& back)
    {
        if (texture_ != nullptr && back.Width == header_->Width && back.Height == header_->Height
            && (uint32_t)back.Format == header_->Format) {
            return true;
        }

        ReleaseTexture();

        if (back.SampleDesc.Count > 1) {
            // CopyResource cannot resolve; nothing here can be done about it
            PublishNothing();
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = back.Width;
        desc.Height = back.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = back.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        auto hr = device->CreateTexture2D(&desc, nullptr, &texture_);
        if (FAILED(hr) || texture_ == nullptr) {
            ERR("Could not make the shared texture for frame mirroring (%x)", hr);
            texture_ = nullptr;
            PublishNothing();
            header_->Enabled = 0;
            return false;
        }

        if (FAILED(texture_->QueryInterface(IID_PPV_ARGS(&mutex_))) || mutex_ == nullptr) {
            ERR("The shared texture has no keyed mutex");
            ReleaseTexture();
            PublishNothing();
            header_->Enabled = 0;
            return false;
        }

        auto generation = header_->Generation + 1;
        char name[64];
        sprintf_s(name, "Local\\GateKitFrame_%u_%u", GetCurrentProcessId(), generation);
        textureName_ = name;
        auto wide = FromUTF8(name);

        IDXGIResource1* resource{ nullptr };
        if (FAILED(texture_->QueryInterface(IID_PPV_ARGS(&resource))) || resource == nullptr) {
            ERR("The shared texture cannot be named (no IDXGIResource1)");
            ReleaseTexture();
            PublishNothing();
            header_->Enabled = 0;
            return false;
        }

        hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            wide.c_str(), &shared_);
        resource->Release();
        if (FAILED(hr) || shared_ == nullptr) {
            ERR("Could not share the frame texture (%x)", hr);
            ReleaseTexture();
            PublishNothing();
            header_->Enabled = 0;
            return false;
        }

        header_->AdapterLuid = AdapterLuid(device);
        header_->Width = back.Width;
        header_->Height = back.Height;
        header_->Format = (uint32_t)back.Format;
        // The name last of all, and the generation with it: a reader that sees a new generation
        // knows every other field beside it is already the new texture's.
        wcscpy_s(header_->TextureName, wide.c_str());
        header_->Generation = generation;
        return true;
    }

    static uint64_t AdapterLuid(ID3D11Device* device)
    {
        uint64_t luid{ 0 };
        IDXGIDevice* dxgiDevice{ nullptr };
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && dxgiDevice != nullptr) {
            IDXGIAdapter* adapter{ nullptr };
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter != nullptr) {
                DXGI_ADAPTER_DESC desc;
                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    luid = ((uint64_t)(uint32_t)desc.AdapterLuid.HighPart << 32) | (uint32_t)desc.AdapterLuid.LowPart;
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }
        return luid;
    }

    // There is no texture to open; the generation moves so that a reader lets go of the old one.
    void PublishNothing()
    {
        header_->TextureName[0] = 0;
        header_->Width = 0;
        header_->Height = 0;
        header_->Format = 0;
        header_->Generation++;
        textureName_.clear();
    }

    void ReleaseTexture()
    {
        if (mutex_ != nullptr) {
            mutex_->Release();
            mutex_ = nullptr;
        }
        if (texture_ != nullptr) {
            texture_->Release();
            texture_ = nullptr;
        }
        if (shared_ != nullptr) {
            CloseHandle(shared_);
            shared_ = nullptr;
        }
    }

    static std::wstring FromUTF8(char const* text)
    {
        std::wstring wide;
        while (*text) wide.push_back((wchar_t)(unsigned char)*text++);   // the names are ASCII
        return wide;
    }

    HANDLE section_{ nullptr };
    FrameShareHeader* header_{ nullptr };
    ID3D11Texture2D* texture_{ nullptr };
    IDXGIKeyedMutex* mutex_{ nullptr };
    HANDLE shared_{ nullptr };
    STDString sectionName_;
    STDString textureName_;
};

END_NS()
