// The mods' hints (ModHints.h): each module's Public/<Folder>/Hints/Hints.lsx loaded through the
// game's own ls::FileFormatIO::LoadFile and composed into ecl::HintManager's list; composed again
// when the game loads another module list.
#include <Extender/Client/ModHints.h>
#include <Extender/Shared/StartupTimeline.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

BEGIN_NS(ecl::mod_hints)

namespace
{
    // ecl::HintManager (an ls::FileFormatIO; dx11 build 25605617, as its constructor inlined in
    // bg3_dx11.exe+0x1818E40 and its destructor, vtable slot 1, bg3_dx11.exe+0x17F10C0 show it):
    // +0x00 vtable, +0x08 the array of hints (pointer, capacity uint32 at +0x10, count uint32 at
    // +0x14), +0x18 and +0x38 two ls::ScratchString, +0x58 HintDuration (float, 5.0).
    constexpr std::size_t ManagerSize = 0x60;
    constexpr std::size_t HintsOffset = 0x08;
    constexpr std::size_t ScratchStringOffsets[] = { 0x18, 0x38 };
    constexpr std::size_t HintDurationOffset = 0x58;
    constexpr float DefaultHintDuration = 5.0f;
    constexpr std::size_t DestructorSlot = 1;

    // The array is the game's new[]: the element count it was made for in the 8 bytes before the
    // first element, every one of them constructed (bg3_dx11.exe+0x17F18C0 makes it; the destructor
    // destroys that many and frees from the count).
    constexpr std::size_t ArrayHeaderSize = 8;

    // ecl::Hint, 0x38 bytes: +0x00 HintKey (FixedString), +0x04 Description (TranslatedString),
    // +0x14 Controller, +0x15 Keyboard, +0x18 WikiPage (string), +0x30 Internal.
    constexpr std::size_t HintSize = 0x38;

    // The game's own file, as App::InitializeResourceManager names it: Hints/Hints.lsx under the
    // path root GameMod (Public/Game).
    constexpr char const* GameFile = "Hints/Hints.lsx";

    struct HintBytes
    {
        uint8_t Bytes[HintSize];

        uint32_t Key() const
        {
            return *reinterpret_cast<uint32_t const*>(Bytes);
        }
    };

    struct HintArray
    {
        HintBytes* Data;
        uint32_t Capacity;
        uint32_t Count;
    };

    constexpr char const* GameSource = "Game";
    constexpr char const* AtStartLoading = "StartLoading";
    constexpr char const* AtLoadModule = "LoadModule";

    // gRecord: the composition in the game's list; gPending*: a reload's composition waiting for
    // the interface's thread. All under gLock.
    std::mutex gLock;
    HintsRecord gRecord;
    HintsRecord gPendingRecord;
    HintBytes* gPendingData{ nullptr };
    uint32_t gPendingCount{ 0 };
    std::atomic<bool> gHasPending{ false };
    // The game's ecl::HintManager vtable, as the first composition found it.
    void* gVtable{ nullptr };

    // The list being composed, as bytes moved out of the game's objects, and who gave each entry.
    struct Work
    {
        std::vector<HintBytes> List;
        std::vector<ComposedHint> Entries;
        std::unordered_map<uint32_t, std::size_t> ByKey;
        // Entries the composition drops (replaced ones, constructed slots past a count), destroyed
        // at the end by the game's destructor.
        std::vector<HintBytes> Dropped;
        bool Changed{ false };
    };

    HintArray* ArrayOf(uint8_t* manager)
    {
        return reinterpret_cast<HintArray*>(manager + HintsOffset);
    }

    // An array of `count` elements in the game's form, filled from `from` (moved as bytes).
    HintBytes* NewArray(HintBytes const* from, std::size_t count)
    {
        if (count == 0) return nullptr;
        auto block = reinterpret_cast<uint8_t*>(GameAllocRaw(ArrayHeaderSize + count * HintSize));
        *reinterpret_cast<uint64_t*>(block) = count;
        auto data = reinterpret_cast<HintBytes*>(block + ArrayHeaderSize);
        memcpy(data, from, count * HintSize);
        return data;
    }

    // Frees an array's memory without destroying its elements (they were moved out).
    void FreeArrayMemory(HintBytes* data)
    {
        if (data != nullptr) {
            GameFree(reinterpret_cast<uint8_t*>(data) - ArrayHeaderSize);
        }
    }

    // A second ecl::HintManager, as the game's constructor makes one: the vtable, an empty array,
    // the two ScratchStrings as ls::ScratchString::ScratchString(0, -1) leaves them, HintDuration 5.
    uint8_t* NewManager(void* vtable)
    {
        auto m = reinterpret_cast<uint8_t*>(GameAllocRaw(ManagerSize));
        memset(m, 0, ManagerSize);
        *reinterpret_cast<void**>(m) = vtable;
        for (auto off : ScratchStringOffsets) {
            *reinterpret_cast<uint32_t*>(m + off + 0x10) = 0xffffffffu;
            *reinterpret_cast<uint32_t*>(m + off + 0x14) = 0xffffffffu;
            m[off + 0x18] = 1;
            m[off + 0x19] = 1;
        }
        *reinterpret_cast<float*>(m + HintDurationOffset) = DefaultHintDuration;
        return m;
    }

    // The game's destructor (vtable slot 1, with 1: destroy what the array holds, then free it all).
    void DestroyManager(uint8_t* manager)
    {
        using DestructorProc = void* (void* self, uint64_t flags);
        auto vtable = *reinterpret_cast<void***>(manager);
        reinterpret_cast<DestructorProc*>(vtable[DestructorSlot])(manager, 1);
    }

    // Destroys entries moved out of the game's arrays: in a "graveyard" manager, by its destructor.
    void DestroyEntries(void* vtable, std::vector<HintBytes> const& entries)
    {
        if (entries.empty()) return;
        auto graveyard = NewManager(vtable);
        auto arr = ArrayOf(graveyard);
        arr->Data = NewArray(entries.data(), entries.size());
        arr->Capacity = (uint32_t)entries.size();
        arr->Count = (uint32_t)entries.size();
        DestroyManager(graveyard);
    }

    // Moves a loaded manager's entries out (into `out`; its constructed slots past the count into
    // `dropped`) and frees its array's memory, leaving it empty.
    void MoveOut(uint8_t* manager, std::vector<HintBytes>& out, std::vector<HintBytes>& dropped)
    {
        auto arr = ArrayOf(manager);
        if (arr->Data != nullptr) {
            out.insert(out.end(), arr->Data, arr->Data + arr->Count);
            if (arr->Capacity > arr->Count) {
                dropped.insert(dropped.end(), arr->Data + arr->Count, arr->Data + arr->Capacity);
            }
        }
        FreeArrayMemory(arr->Data);
        arr->Data = nullptr;
        arr->Capacity = 0;
        arr->Count = 0;
    }

    STDString ToSTD(StringView sv)
    {
        return STDString(sv.data(), sv.size());
    }

    STDString KeyText(uint32_t index)
    {
        // FixedStringBase: no reference is taken or given back.
        FixedStringBase fs;
        fs.Index = index;
        return ToSTD(fs.GetStringView());
    }

    STDString Describe(HintSource const& s)
    {
        if (s.Source == GameSource) return "the game";
        STDString d = s.Name.empty() ? STDString("a mod") : s.Name;
        return d + " (" + s.Source + ")";
    }

    // The game's entries start the list.
    void TakeGameEntries(Work& w, std::vector<HintBytes> const& game)
    {
        for (auto const& hint : game) {
            ComposedHint e;
            e.HintKey = KeyText(hint.Key());
            e.From.Source = GameSource;
            w.ByKey.insert(std::make_pair(hint.Key(), w.List.size()));
            w.List.push_back(hint);
            w.Entries.push_back(std::move(e));
        }
    }

    // Every module's file of `modules`, in its order, composed onto the list.
    void ComposeModules(Work& w, game_modules::ModuleList const& modules, void* vtable, HintsRecord& rec)
    {
        auto const& sym = GetStaticSymbols();
        for (auto const& mod : modules.Modules) {
            if (mod.Folder.empty()) continue;

            HintFile file;
            file.Mod.Source = mod.Uuid;
            file.Mod.Name = mod.Name;
            file.Mod.Folder = mod.Folder;
            file.Path = "Public/" + mod.Folder + "/Hints/Hints.lsx";

            file.Found = sym.FileExists(file.Path, PathRootType::Data);
            if (!file.Found) {
                rec.Files.push_back(std::move(file));
                continue;
            }

            auto temp = NewManager(vtable);
            Path path;
            path.Name = sym.ToPath(file.Path, PathRootType::Data);
            file.Loaded = sym.ls__FileFormatIO__LoadFile(temp, &path);

            std::vector<HintBytes> loaded;
            MoveOut(temp, loaded, w.Dropped);
            DestroyManager(temp);

            file.Count = (uint32_t)loaded.size();
            for (auto const& hint : loaded) {
                if (hint.Key() == FixedStringId::NullIndex) {
                    rec.Warnings.push_back(Describe(file.Mod) + ": a hint without a HintKey is left out");
                    w.Dropped.push_back(hint);
                    continue;
                }

                auto it = w.ByKey.find(hint.Key());
                if (it == w.ByKey.end()) {
                    w.ByKey.insert(std::make_pair(hint.Key(), w.List.size()));
                    w.List.push_back(hint);
                    ComposedHint e;
                    e.HintKey = KeyText(hint.Key());
                    e.From = file.Mod;
                    w.Entries.push_back(std::move(e));
                    file.Added++;
                } else {
                    auto& e = w.Entries[it->second];
                    if (e.From.Source == file.Mod.Source) {
                        rec.Warnings.push_back(Describe(file.Mod) + ": the hint " + e.HintKey
                            + " is given twice in its file; the later one is kept");
                    } else {
                        rec.Warnings.push_back(Describe(file.Mod) + " replaces the hint " + e.HintKey
                            + " given by " + Describe(e.From));
                    }
                    w.Dropped.push_back(w.List[it->second]);
                    w.List[it->second] = hint;
                    e.Replaced = e.From;
                    e.From = file.Mod;
                    file.Replaced++;
                }
                w.Changed = true;
            }

            if (!file.Loaded) {
                rec.Warnings.push_back(Describe(file.Mod) + ": " + file.Path + " exists but the game could not load it");
            }
            rec.Files.push_back(std::move(file));
        }
    }

    int FilesFound(HintsRecord const& rec)
    {
        return (int)std::count_if(rec.Files.begin(), rec.Files.end(), [](HintFile const& f) { return f.Found; });
    }

    void Row(HintsRecord const& rec, char const* moment)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d mods' files of %d modules", FilesFound(rec), (int)rec.Modules.Modules.size());
        startup_timeline::Record(startup_timeline::Side::Client, startup_timeline::What::HintsComposed,
            (int32_t)rec.GameCount, (int32_t)rec.Count, buf, moment);
    }

    void Log(HintsRecord const& rec, char const* when)
    {
        INFO("Mod hints (%s): %d of the game, %d after %d mods' files of %d modules (%s), %d warnings", when,
            (int)rec.GameCount, (int)rec.Count, FilesFound(rec), (int)rec.Modules.Modules.size(),
            rec.Modules.Source.c_str(), (int)rec.Warnings.size());
        for (auto const& w : rec.Warnings) {
            WARN("Mod hints: %s", w.c_str());
        }
    }
}

HintsRecord GetHintsRecord()
{
    std::lock_guard<std::mutex> guard(gLock);
    HintsRecord rec = gRecord;
    rec.Pending = gHasPending.load();
    return rec;
}

void OnStartLoading()
{
    std::lock_guard<std::mutex> guard(gLock);
    if (gRecord.Seen) return;
    gRecord.Seen = true;
    gRecord.Moment = AtStartLoading;
    gRecord.Enabled = gExtender->GetConfig().ModHints;
    if (!gRecord.Enabled) return;

    auto const& sym = GetStaticSymbols();
    if (sym.ecl__HintManager == nullptr || *sym.ecl__HintManager == nullptr || sym.ls__FileFormatIO__LoadFile == nullptr) {
        gRecord.Warnings.push_back("The game's hint list or its file loader was not found; no mod's hints are read");
        return;
    }

    gRecord.Modules = game_modules::OnStartLoading();
    gRecord.Warnings.insert(gRecord.Warnings.end(), gRecord.Modules.Warnings.begin(), gRecord.Modules.Warnings.end());

    auto manager = reinterpret_cast<uint8_t*>(*sym.ecl__HintManager);
    auto vtable = *reinterpret_cast<void**>(manager);
    gVtable = vtable;
    auto real = ArrayOf(manager);
    gRecord.GameCount = real->Count;

    Work w;
    std::vector<HintBytes> game(real->Data, real->Data + real->Count);
    TakeGameEntries(w, game);

    ComposeModules(w, gRecord.Modules, vtable, gRecord);

    if (w.Changed) {
        // Constructed slots past the count go with the old array (unchanged, they stay in it).
        if (real->Data != nullptr && real->Capacity > real->Count) {
            w.Dropped.insert(w.Dropped.end(), real->Data + real->Count, real->Data + real->Capacity);
        }
        auto old = real->Data;
        real->Data = NewArray(w.List.data(), w.List.size());
        real->Capacity = (uint32_t)w.List.size();
        real->Count = (uint32_t)w.List.size();
        FreeArrayMemory(old);
        gRecord.Written = true;
    }
    DestroyEntries(vtable, w.Dropped);

    gRecord.Count = real->Count;
    gRecord.Entries = std::move(w.Entries);
    gRecord.Compositions = 1;

    Row(gRecord, AtStartLoading);
    Log(gRecord, "start");
}

void OnModuleLoaded(game_modules::ModuleList const& loaded)
{
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (!gRecord.Seen || !gRecord.Enabled || gRecord.Compositions == 0 || gVtable == nullptr) return;
        auto const& against = gHasPending.load() ? gPendingRecord.Modules : gRecord.Modules;
        if (loaded.SameModules(against)) return;
    }

    auto const& sym = GetStaticSymbols();
    HintsRecord rec;
    rec.Seen = true;
    rec.Enabled = true;
    rec.Written = true;
    rec.Moment = AtLoadModule;
    rec.Modules = loaded;

    // The game's own file, loaded anew through the same loader the game used for it.
    auto temp = NewManager(gVtable);
    Path path;
    path.Name = sym.ToPath(GameFile, PathRootType::GameMod);
    bool gameLoaded = sym.ls__FileFormatIO__LoadFile(temp, &path);

    Work w;
    std::vector<HintBytes> game;
    MoveOut(temp, game, w.Dropped);
    DestroyManager(temp);

    if (!gameLoaded || game.empty()) {
        w.Dropped.insert(w.Dropped.end(), game.begin(), game.end());
        DestroyEntries(gVtable, w.Dropped);
        startup_timeline::Record(startup_timeline::Side::Client, startup_timeline::What::HintsComposed,
            0, -1, "not composed: the game's hint file was not loaded", "reload");
        WARN("Mod hints: the module list changed, but the game's %s could not be loaded again; the list is kept", path.Name.c_str());
        return;
    }

    rec.GameCount = (uint32_t)game.size();
    TakeGameEntries(w, game);
    ComposeModules(w, loaded, gVtable, rec);
    rec.Count = (uint32_t)w.List.size();
    rec.Entries = std::move(w.Entries);

    auto data = NewArray(w.List.data(), w.List.size());
    DestroyEntries(gVtable, w.Dropped);

    std::vector<HintBytes> superseded;
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (gHasPending.load()) {
            // A composition no one took yet: this one supersedes it.
            superseded.assign(gPendingData, gPendingData + gPendingCount);
            FreeArrayMemory(gPendingData);
        }
        gPendingData = data;
        gPendingCount = (uint32_t)w.List.size();
        gPendingRecord = rec;
        gHasPending.store(true);
    }
    DestroyEntries(gVtable, superseded);

    Row(rec, "reload");
    Log(rec, "reload");
}

void OnInterfaceThread()
{
    if (!gHasPending.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> guard(gLock);
    if (!gHasPending.load()) return;

    auto const& sym = GetStaticSymbols();
    if (sym.ecl__HintManager == nullptr || *sym.ecl__HintManager == nullptr) return;
    auto manager = reinterpret_cast<uint8_t*>(*sym.ecl__HintManager);
    auto real = ArrayOf(manager);

    std::vector<HintBytes> old;
    if (real->Data != nullptr) {
        old.assign(real->Data, real->Data + real->Capacity);
    }
    auto oldData = real->Data;
    real->Data = gPendingData;
    real->Capacity = gPendingCount;
    real->Count = gPendingCount;
    FreeArrayMemory(oldData);
    DestroyEntries(gVtable, old);

    gPendingRecord.Compositions = gRecord.Compositions + 1;
    gRecord = std::move(gPendingRecord);
    gPendingRecord = HintsRecord{};
    gPendingData = nullptr;
    gPendingCount = 0;
    gHasPending.store(false);

    Row(gRecord, "installed");
}

END_NS()
