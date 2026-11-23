// The mods' interface on the first loading screen (EarlyModUI.h).
#include <Extender/Client/EarlyModUI.h>
#include <Extender/Client/GameModules.h>
#include <Extender/Shared/StartupTimeline.h>
#include <GameDefinitions/Module.h>
#include <atomic>
#include <vector>

BEGIN_NS(ecl::early_mod_ui)

namespace
{
    // gui::GameUI, as gui::GameUI::UpdateFromUI (bg3_dx11.exe+0x1210210, build 25605617) uses it in
    // its merge block:
    // +0x0108 an object whose +0xE8 bit 0 the block clears around the merge and sets again;
    // +0x0480 the ui::ModManager (RegisterMods' `this`);
    // +0x07C0 the ui::StateMachine (ApplyMods' `this`), with +0x2D88 the value it is applied with;
    // +0x2B90 the ui::TextureProvider (made by the update's first-frame set-up, before the block);
    // +0x2D94 that set-up's state (1: asked, not yet run);
    // +0x2D95 flags, bit 3 cleared after a merge;
    // +0x2D9A the mods' interface pending (raised by gui::GameUI::OnModuleLoaded);
    // +0x3038 the lock the update holds around the block and the state machine's update.
    constexpr std::size_t FlagOwnerOffset = 0x108;
    constexpr std::size_t FlagOwnerFlagsOffset = 0xE8;
    constexpr std::size_t ModManagerOffset = 0x480;
    constexpr std::size_t StateMachineOffset = 0x7C0;
    constexpr std::size_t ApplyModsValueOffset = 0x2D88;
    constexpr std::size_t TextureProviderOffset = 0x2B90;
    constexpr std::size_t SetUpStateOffset = 0x2D94;
    constexpr uint8_t SetUpAsked = 1;
    constexpr std::size_t FlagsOffset = 0x2D95;
    constexpr uint8_t FlagsClearedAfterMerge = 0xF7;
    constexpr std::size_t ModsPendingOffset = 0x2D9A;
    constexpr std::size_t LockOffset = 0x3038;

    // ui::TextureProvider+0xF0: the ui::ModManager whose mods' data (ui::ModData, 0xAA8 bytes each,
    // the array at +0x08 with its count at +0x14) the block walks; a mod's data with a bit of 0x60
    // at +0x50 gets its texture metadata loaded.
    constexpr std::size_t ProviderModManagerOffset = 0xF0;
    constexpr std::size_t ModManagerDataOffset = 0x08;
    constexpr std::size_t ModManagerCountOffset = 0x14;
    constexpr std::size_t ModDataSize = 0xAA8;
    constexpr std::size_t ModDataFlagsOffset = 0x50;
    constexpr uint8_t ModDataTextureFlags = 0x60;

    // RegisterMods walks its ObjectSet<ls::Module> with this stride.
    static_assert(sizeof(Module) == 0x118, "ls::Module as ui::ModManager::RegisterMods reads it");

    constexpr char const* EarlyParent = "early";
    constexpr char const* GameParent = "game";

    enum Stage : int
    {
        StageNone,
        StageReady,
        StageDone
    };

    std::atomic<int> gStage{ StageNone };
    bool gStartSeen{ false };
    // The module set taken at StartLoading; handed to the interface's thread through gStage.
    Array<Module>* gModules{ nullptr };
    char gNames[64]{};
    // The game's pending byte as the last update saw it (the interface's thread only).
    bool gGamePending{ false };
    // The early set's modules (their UUIDs, in order), written before gStage is Ready; and whether
    // the early merge ran and the game's first merge is still to be compared with it (the
    // interface's thread only).
    std::vector<FixedString> gEarlySet;
    bool gEarlyDone{ false };

    void Row(int32_t from, int32_t to, char const* name, char const* parent)
    {
        startup_timeline::Record(startup_timeline::Side::Client, startup_timeline::What::EarlyModInterface,
            from, to, name, parent);
    }

    bool SymbolsFound()
    {
        auto const& sym = GetStaticSymbols();
        return sym.gui__GameUI__UpdateFromUI != nullptr
            && sym.ui__ModManager__RegisterMods != nullptr
            && sym.ui__TextureProvider__LoadTextureMetadataInternal != nullptr
            && sym.Noesis__GUI__GetApplicationResources != nullptr
            && sym.Noesis__ResourceDictionary__GetMergedDictionaries != nullptr
            && sym.gui__GameUI__ResetModdedLibs != nullptr
            && sym.ui__StateMachine__ApplyMods != nullptr;
    }

    void AppendName(char const* name)
    {
        auto used = strlen(gNames);
        if (used + 2 >= sizeof(gNames)) return;
        if (used > 0) {
            gNames[used++] = ',';
            gNames[used] = 0;
        }
        auto n = strlen(name);
        if (n > sizeof(gNames) - 1 - used) n = sizeof(gNames) - 1 - used;
        memcpy(gNames + used, name, n);
        gNames[used + n] = 0;
    }

    // The resources' part of gui::GameUI::UpdateFromUI's merge block, in its order and with its
    // arguments; the caller holds the GameUI lock. The block's last step, the state machine's
    // ui::StateMachine::ApplyMods, is left to the game's own merge at the end of LoadModule: run this
    // early, it rebuilt the states while gui::TooltipManager still held a connection to the old ones,
    // and the tooltips' settings reload crashed on it now and then. False when the game's code faulted.
    bool RunResourcesMerge(uint8_t* ui, Array<Module> const* modules, bool& registered, int32_t& metadata)
    {
        auto const& sym = GetStaticSymbols();
        __try {
            registered = sym.ui__ModManager__RegisterMods(ui + ModManagerOffset, modules);
            if (!registered) return true;

            auto provider = *reinterpret_cast<uint8_t**>(ui + TextureProviderOffset);
            auto manager = *reinterpret_cast<uint8_t**>(provider + ProviderModManagerOffset);
            auto data = *reinterpret_cast<uint8_t**>(manager + ModManagerDataOffset);
            auto count = *reinterpret_cast<int32_t*>(manager + ModManagerCountOffset);
            if (data != nullptr) {
                for (int32_t i = 0; i < count; i++) {
                    auto modData = data + (std::size_t)i * ModDataSize;
                    if ((modData[ModDataFlagsOffset] & ModDataTextureFlags) != 0) {
                        sym.ui__TextureProvider__LoadTextureMetadataInternal(provider, modData);
                        metadata++;
                    }
                }
            }

            auto owner = *reinterpret_cast<uint8_t**>(ui + FlagOwnerOffset);
            owner[FlagOwnerFlagsOffset] &= 0xFE;
            auto merged = sym.Noesis__ResourceDictionary__GetMergedDictionaries(sym.Noesis__GUI__GetApplicationResources());
            sym.gui__GameUI__ResetModdedLibs(ui, merged);
            owner[FlagOwnerFlagsOffset] |= 1;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool ReadyForMerge(uint8_t const* ui)
    {
        __try {
            return *reinterpret_cast<void* const*>(ui + TextureProviderOffset) != nullptr
                && *reinterpret_cast<void* const*>(ui + FlagOwnerOffset) != nullptr
                && ui[SetUpStateOffset] != SetUpAsked;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void FreeModules()
    {
        delete gModules;
        gModules = nullptr;
    }
}

void OnStartLoading()
{
    if (gStartSeen) return;
    gStartSeen = true;

    if (!gExtender->GetConfig().EarlyModInterface) {
        Row(-1, -1, "off: the switch EarlyModInterface", EarlyParent);
        return;
    }
    if (!SymbolsFound()) {
        Row(-1, -1, "not run: the game's merge block was not found", EarlyParent);
        WARN("Early mods' interface: the game's merge block was not found; the mods' interface comes at the game's own time");
        return;
    }
    auto modManager = GetStaticSymbols().GetModManagerClient();
    if (modManager == nullptr) {
        Row(-1, -1, "not run: no client mod manager", EarlyParent);
        return;
    }

    // The game's module list (GameModules.h): with the game's resolver, the very set the game gives
    // RegisterMods at its own merge (its LoadOrderedModules), taken as the resolver left it.
    auto const& list = game_modules::OnStartLoading();
    auto modules = new Array<Module>();
    uint32_t missing = 0;
    if (list.Source == "LoadModFromSettings") {
        for (auto const& mod : modManager->LoadOrderedModules) {
            modules->push_back(mod);
        }
    } else {
        // The fallback: the settings' modules found among the available ones, the base left out.
        auto const& base = modManager->BaseModule.Info;
        for (auto const& mod : modManager->Settings.Mods) {
            bool isBase = (base.ModuleUUID && base.ModuleUUID == mod.ModuleUUID)
                || (base.ModuleUUIDString && base.ModuleUUIDString == mod.ModuleUUIDString);
            if (isBase) continue;

            Module const* found{ nullptr };
            for (auto const& available : modManager->AvailableMods) {
                if ((mod.ModuleUUID && available.Info.ModuleUUID == mod.ModuleUUID)
                    || (mod.ModuleUUIDString && available.Info.ModuleUUIDString == mod.ModuleUUIDString)) {
                    found = &available;
                    break;
                }
            }
            if (found == nullptr) {
                missing++;
                WARN("Early mods' interface: the enabled mod %s is not among the available modules; left out", mod.Folder.c_str());
                continue;
            }
            modules->push_back(*found);
        }
    }
    gEarlySet.clear();
    for (auto const& mod : *modules) {
        AppendName(mod.Info.Directory.c_str());
        gEarlySet.push_back(mod.Info.ModuleUUIDString);
    }

    if (modules->empty()) {
        delete modules;
        Row(0, -1, "not run: no module to merge", EarlyParent);
        return;
    }

    INFO("Early mods' interface: %d modules taken from %s (%d not available): %s", (int)modules->size(),
        list.Source.c_str(), (int)missing, gNames);
    gModules = modules;
    gStage.store(StageReady, std::memory_order_release);
}

void BeforeGameUIUpdate(void* gameUI)
{
    auto ui = reinterpret_cast<uint8_t*>(gameUI);
    if (ui == nullptr) return;

    // The game's own merge runs in this call when its pending byte is up: a row when it rises,
    // saying whether the set it merges (the client's LoadOrderedModules) is the early merge's.
    bool pending = ui[ModsPendingOffset] != 0;
    if (pending && !gGamePending) {
        auto modManager = GetStaticSymbols().GetModManagerClient();
        char const* name = "the game's merge (LoadOrderedModules)";
        if (modManager != nullptr && gEarlyDone) {
            bool same = modManager->LoadOrderedModules.size() == gEarlySet.size();
            for (uint32_t i = 0; same && i < gEarlySet.size(); i++) {
                same = modManager->LoadOrderedModules[i].Info.ModuleUUIDString == gEarlySet[i];
            }
            name = same ? "the game's merge (LoadOrderedModules): the early set"
                : "the game's merge (LoadOrderedModules): not the early set";
            gEarlyDone = false;
        }
        Row(modManager ? (int32_t)modManager->LoadOrderedModules.size() : -1, -1, name, GameParent);
    }
    gGamePending = pending;

    if (gStage.load(std::memory_order_acquire) != StageReady) return;

    if (pending) {
        // The game merges in this very call: nothing earlier is left to do.
        gStage.store(StageDone);
        Row((int32_t)gModules->size(), -1, "not run: the game's own merge came first", EarlyParent);
        FreeModules();
        return;
    }

    // The update's first-frame set-up (the texture provider) runs inside this call before the
    // block's place: wait for a call that finds it done.
    if (!ReadyForMerge(ui)) return;

    // Early, only the mods' resources (their Resources.xaml and libraries, what the loading screen's
    // templates are looked up in); the states (ApplyMods) come with the game's own merge.
    gStage.store(StageDone);
    auto lock = reinterpret_cast<CRITICAL_SECTION*>(ui + LockOffset);
    bool registered{ false };
    int32_t metadata{ 0 };
    EnterCriticalSection(lock);
    bool ran = RunResourcesMerge(ui, gModules, registered, metadata);
    LeaveCriticalSection(lock);

    char name[64];
    if (!ran) {
        snprintf(name, sizeof(name), "faulted: %s", gNames);
        ERR("Early mods' interface: the game's resources merge faulted with %d modules", (int)gModules->size());
    } else {
        gEarlyDone = true;
        snprintf(name, sizeof(name), "%s", gNames);
        INFO("Early mods' interface: RegisterMods(%d modules) answered %s; %d mods' texture metadata; resources merged",
            (int)gModules->size(), registered ? "true" : "false", (int)metadata);
    }
    Row((int32_t)gModules->size(), ran ? (registered ? 1 : 0) : -2, name, EarlyParent);
    FreeModules();
}

END_NS()
