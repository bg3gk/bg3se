#include "stdafx.h"
#include <GameHooks/DataLibraries.h>
#include <Extender/Shared/ExtensionState.h>
#include <Extender/ScriptExtender.h>
#include <GameDefinitions/Symbols.h>
#include <string>
#include <functional>
#include <chrono>
#include <psapi.h>
#include <DbgHelp.h>
#include "resource.h"

// GateKit: a plain C entry for a native mod in any language (loaded by any native loader, found by
// GetModuleHandle + GetProcAddress on this DLL) - the address of a function or global the game has,
// by name: the extender's own symbols and the names of GateKit's registry table. Read only; the
// names are filled when the extender has mapped the game (before that every answer is null / 0).
extern "C" void* GateKitSymbolAddress(char const* name)
{
    if (name == nullptr || bg3se::gExtender == nullptr) return nullptr;
    auto const& mappings = bg3se::gExtender->GetLibraryManager().Mappings();
    if (auto n = mappings.Named.find(name); n != mappings.Named.end()) return n->second;
    if (auto s = mappings.StaticSymbols.find(name); s != mappings.StaticSymbols.end())
        return *(void**)((uint8_t*)&bg3se::GetStaticSymbols() + s->second.Offset);
    return nullptr;
}

// The number of names of GateKit's registry table the extender read (found or not).
extern "C" int GateKitSymbolCount()
{
    if (bg3se::gExtender == nullptr) return 0;
    return (int)bg3se::gExtender->GetLibraryManager().Mappings().Named.size();
}

// The name at an index of GateKit's registry table, in a stable order (sorted), or null past the end.
// The pointer stays valid while the extender lives.
extern "C" char const* GateKitSymbolName(int index)
{
    static std::vector<std::string> sorted;
    if (bg3se::gExtender == nullptr || index < 0) return nullptr;
    auto const& named = bg3se::gExtender->GetLibraryManager().Mappings().Named;
    if (sorted.size() != named.size()) {
        sorted.clear();
        for (auto const& entry : named) sorted.push_back(entry.first);
        std::sort(sorted.begin(), sorted.end());
    }
    return index < (int)sorted.size() ? sorted[index].c_str() : nullptr;
}

namespace bg3se
{
    _Post_writable_byte_size_(size) void* BG3Alloc(std::size_t size)
    {
        auto const& sym = GetStaticSymbols();
        // Fallback in case allocator is not available
        if (sym.ls__GlobalAllocator__Alloc && sym.ls__GlobalAllocator__Free && sym.ls__GlobalAllocator__Get) {
            return sym.ls__GlobalAllocator__Alloc(sym.ls__GlobalAllocator__Get(), size, 2, 0, 8);
        } else {
            return malloc(size);
        }
    }

    void BG3Free(_Post_ptr_invalid_ _Pre_valid_ void* ptr) noexcept
    {
        auto const& sym = GetStaticSymbols();
        if (sym.ls__GlobalAllocator__Alloc && sym.ls__GlobalAllocator__Free && sym.ls__GlobalAllocator__Get) {
            sym.ls__GlobalAllocator__Free(sym.ls__GlobalAllocator__Get(), ptr);
        } else {
            return free(ptr);
        }
    }

    LibraryManager::LibraryManager()
        : symbolMapper_(mappings_)
    {}

    bool LibraryManager::FindLibraries(uint32_t gameRevision)
    {
        RegisterSymbols();

        SymbolMappingLoader loader(mappings_);
        PreRegisterLibraries(loader);
        if (!loader.LoadBuiltinMappings(IDR_BINARY_MAPPINGS)) {
            ERR("Failed to load symbol mapping table");
            CriticalInitFailed = true;
        }

        // GateKit: the registry's own table beside this DLL, if there is one (RegistryMappings.xml):
        // every function the registry names and places, found by its pattern and kept by its name.
        {
            wchar_t path[1024];
            auto len = GetModuleFileNameW(gCoreLibPlatformInterface.ThisModule, path, (DWORD)std::size(path));
            if (len > 0 && len < std::size(path)) {
                std::wstring file(path, len);
                auto sep = file.find_last_of(L"\\/");
                if (sep != std::wstring::npos) {
                    file = file.substr(0, sep + 1) + L"RegistryMappings.xml";
                    loader.LoadNamedMappings(file);
                }
            }
        }

#if defined(_DEBUG)
        if (!CriticalInitFailed) {
            // Debug check for symbol xml -> engine bindings
            for (auto const& sym : mappings_.StaticSymbols) {
                if (!sym.second.Bound) {
                    WARN("No mapping exists for engine symbol: %s", sym.first.c_str());
                }
            }
        }
#endif

        if (!BindApp()) {
            ERR("LibraryManager::FindLibraries(): Unable to bind BG3 module.");
            return false;
        }

        RegisterLibraries(symbolMapper_);
        symbolMapper_.MapAllSymbols(false);

        CriticalInitFailed = CriticalInitFailed || symbolMapper_.HasFailedCriticalMappings();
        InitFailed = InitFailed || symbolMapper_.HasFailedMappings();

        if (gExtender->GetConfig().DisableLauncher && !CriticalInitFailed) {
            ApplyCodePatch("KillLauncher");
        }

        auto const& sym = GetStaticSymbols();
        gCoreLibPlatformInterface.Alloc = &BG3Alloc;
        gCoreLibPlatformInterface.Free = &BG3Free;
        gCoreLibPlatformInterface.ls__GlobalStringTable__MainTable__CreateFromString = sym.ls__GlobalStringTable__MainTable__CreateFromString;
        gCoreLibPlatformInterface.ls__FixedString__GetString = sym.ls__FixedString__GetString;
        gCoreLibPlatformInterface.ls__FixedString__IncRef = sym.ls__FixedString__IncRef;
        gCoreLibPlatformInterface.ls__GlobalStringTable__MainTable__DecRef = sym.ls__GlobalStringTable__MainTable__DecRef;
        gCoreLibPlatformInterface.ls__gGlobalStringTable = sym.ls__gGlobalStringTable;

        return !CriticalInitFailed;
    }

    bool LibraryManager::PostStartupFindLibraries()
    {
        if (PostLoaded) {
            return !CriticalInitFailed;
        }

        auto initStart = std::chrono::high_resolution_clock::now();

        symbolMapper_.MapAllSymbols(true);

        if (!CriticalInitFailed) {
            GFS.Initialize();
            InitializeEnumerations();
        }

        auto initEnd = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(initEnd - initStart).count();
        DEBUG("LibraryManager::PostStartupFindLibraries() took %lld ms", ms);

        PostLoaded = true;
        return !CriticalInitFailed;
    }

    void LibraryManager::ApplyCodePatches()
    {
        if (gExtender->GetConfig().EnableAchievements && !WasPatchApplied("ls::ModuleSettings::IsModded")) {
            if (ApplyCodePatch("ls::ModuleSettings::IsModded") && ApplyCodePatch("esv::SavegameManager::ThrowError")) {
                DEBUG("Modded achievements enabled.");
            } else {
                ERR("Couldn't enable modded achievements (symbol not mapped)");
            }
        }

        if (gExtender->GetConfig().DisableStoryPatching && GetStaticSymbols().ls__GlobalSwitches) {
            (*GetStaticSymbols().ls__GlobalSwitches)->DisableStoryPatching = true;
        }
    }

    bool LibraryManager::WasPatchApplied(std::string const& mapping)
    {
        auto it = mappings_.Mappings.find(mapping);
        if (it == mappings_.Mappings.end()) {
            ERR("Cannot apply patch - no such mapping: '%s'", mapping.c_str());
            return false;
        }

        bool applied = false;
        for (auto& patch : it->second.Patches) {
            applied = patch.WasApplied || applied;
        }

        return applied;
    }

    bool LibraryManager::ApplyCodePatch(std::string const& mapping)
    {
        auto it = mappings_.Mappings.find(mapping);
        if (it == mappings_.Mappings.end()) {
            ERR("Cannot apply patch - no such mapping: '%s'", mapping.c_str());
            return false;
        }

        if (it->second.Patches.empty()) {
            ERR("Cannot apply patch - mapping '%s' contains no patch entries", mapping.c_str());
            return false;
        }

        bool hasChanges = false;
        for (auto& patch : it->second.Patches) {
            hasChanges = ApplyPatch(patch) || hasChanges;
        }

        return hasChanges;
    }


    bool LibraryManager::UndoCodePatch(std::string const& mapping)
    {
        auto it = mappings_.Mappings.find(mapping);
        if (it == mappings_.Mappings.end()) {
            ERR("Cannot undo patch - no such mapping: '%s'", mapping.c_str());
            return false;
        }

        bool hasChanges = false;
        for (auto& patch : it->second.Patches) {
            hasChanges = UndoPatch(patch) || hasChanges;
        }

        return hasChanges;
    }


    bool LibraryManager::ApplyPatch(SymbolMappings::Patch& patch)
    {
        if (patch.WasApplied) {
            return true;
        }
        
        if (patch.ResolvedRef == nullptr) {
            return false;
        }

        WriteAnchor code(patch.ResolvedRef, patch.Bytes.size());
        patch.OriginalBytes.resize(patch.Bytes.size());
        memcpy(patch.OriginalBytes.data(), patch.ResolvedRef, patch.OriginalBytes.size());
        memcpy(const_cast<uint8_t*>(patch.ResolvedRef), patch.Bytes.data(), patch.Bytes.size());
        patch.WasApplied = true;
        return true;
    }


    bool LibraryManager::UndoPatch(SymbolMappings::Patch& patch)
    {
        if (!patch.WasApplied) return false;

        WriteAnchor code(patch.ResolvedRef, patch.OriginalBytes.size());
        memcpy(const_cast<uint8_t*>(patch.ResolvedRef), patch.OriginalBytes.data(), patch.OriginalBytes.size());
        patch.WasApplied = false;
        return true;
    }

    bool LibraryManager::GetGameVersion(GameVersionInfo & version)
    {
        HMODULE hGameModule = GetModuleHandleW(L"bg3.exe");
        if (hGameModule == NULL) {
            hGameModule = GetModuleHandleW(L"bg3_dx11.exe");
        }

        if (hGameModule == NULL) {
            return false;
        }

        auto hResource = FindResource(hGameModule, MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);
        if (hResource == NULL) return false;
        auto dwSize = SizeofResource(hGameModule, hResource);
        auto hData = LoadResource(hGameModule, hResource);
        if (hData == NULL) return false;
        auto pRes = LockResource(hData);
        if (pRes == NULL) return false;

        auto pResCopy = LocalAlloc(LMEM_FIXED, dwSize);
        CopyMemory(pResCopy, pRes, dwSize);

        UINT verLength;
        VS_FIXEDFILEINFO * fixedFileInfo;
        if (VerQueryValue(pResCopy, L"\\", (LPVOID*)&fixedFileInfo, &verLength) != TRUE) return false;

        version.Major = HIWORD(fixedFileInfo->dwFileVersionMS);
        version.Minor = LOWORD(fixedFileInfo->dwFileVersionMS);
        version.Revision = HIWORD(fixedFileInfo->dwFileVersionLS);
        version.Build = LOWORD(fixedFileInfo->dwFileVersionLS);

        LocalFree(pResCopy);
        FreeResource(hData);
        return true;
    }

    void LibraryManager::ShowStartupError(STDString const & msg, bool wait, bool exitGame)
    {
        ERR("STARTUP ERROR: %s", msg.c_str());

        if (GetStaticSymbols().ecl__EoCClient == nullptr
            || GetStaticSymbols().ecl__EoCClient__HandleError == nullptr
            || GetStaticSymbols().ls__GlobalAllocator__Alloc == nullptr) {
            return;
        }

        if (wait) {
            std::thread messageThread([this, msg, exitGame]() {
                unsigned retries{ 0 };
                while (!CanShowError() && retries < 1200) {
                    Sleep(100);
                    retries++;
                }

                if (retries < 1200) {
                    ShowStartupError(msg, exitGame);
                } else {
                    MessageBoxA(NULL, msg.c_str(), "Script Extender Error", 
                        MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND);
                }
            });
            messageThread.detach();
        } else {
            if (CanShowError()) {
                ShowStartupError(msg, exitGame);
            }
        }
    }

    void LibraryManager::ShowStartupError(STDString const & msg, bool exitGame)
    {
        if (!CanShowMessages()) return;

        // Abuse a deprecated string key for displaying custom text
        TranslatedString ts;
        ts.Handle.Handle = FixedString("h11018635g3003g46c6g8013g4630abe55cad");
        ts.Handle.Version = 1;

        // Create a new entry in the string repository text pool
        auto& texts = (*GetStaticSymbols().ls__gTranslatedStringRepository)->TranslatedStrings[0];
        auto tskRef = texts->Texts.try_get(ts.Handle);
        if (!tskRef) return;

        auto str = GameAlloc<STDString>(msg);
        texts->Strings.push_back(str);

        // Update reference to new string
        *tskRef = LSStringView(str->data(), str->size());

        GetStaticSymbols().ecl__EoCClient__HandleError(*GetStaticSymbols().ecl__EoCClient, ts, exitGame, ts, 0);
    }

    bool LibraryManager::CanShowMessages()
    {
        return GetStaticSymbols().GetClientState()
            && GetStaticSymbols().ecl__EoCClient__HandleError != nullptr
            && GetStaticSymbols().ls__GlobalAllocator__Alloc != nullptr;
    }

    bool LibraryManager::CanShowError()
    {
        if (!CanShowMessages()) return false;

        auto state = GetStaticSymbols().GetClientState();
        return state == ecl::GameState::Running
            || state == ecl::GameState::Paused
            || state == ecl::GameState::Menu
            || state == ecl::GameState::Lobby;
    }
}
