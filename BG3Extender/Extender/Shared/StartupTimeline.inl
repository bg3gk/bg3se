// The start's timeline (StartupTimeline.h). Included once, by ScriptExtenderClient.cpp.
#include <Extender/Shared/StartupTimeline.h>
#include <GameDefinitions/Components/All.h>
#include <GameDefinitions/UI.h>
#include <cstdio>
#include <cstring>
#include <mutex>

BEGIN_NS(startup_timeline)

namespace
{
    // The clock: QueryPerformanceCounter, anchored at the extender's load against the process's
    // creation time, so that Ms counts from the process start.
    struct Clock
    {
        LARGE_INTEGER Frequency{};
        LARGE_INTEGER Anchor{};
        double AnchorMs{ 0.0 };

        Clock()
        {
            QueryPerformanceFrequency(&Frequency);
            QueryPerformanceCounter(&Anchor);

            FILETIME creation{}, exit{}, kernel{}, user{}, now{};
            GetSystemTimePreciseAsFileTime(&now);
            if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
                ULARGE_INTEGER c{}, n{};
                c.LowPart = creation.dwLowDateTime; c.HighPart = creation.dwHighDateTime;
                n.LowPart = now.dwLowDateTime; n.HighPart = now.dwHighDateTime;
                if (n.QuadPart > c.QuadPart) {
                    AnchorMs = (double)(n.QuadPart - c.QuadPart) / 10000.0;
                }
            }
        }

        double NowMs() const
        {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            return AnchorMs + (double)(t.QuadPart - Anchor.QuadPart) * 1000.0 / (double)Frequency.QuadPart;
        }
    };

    Clock gClock;
    Event gEvents[Capacity];
    std::atomic<bool> gReady[Capacity];
    std::atomic<uint32_t> gNext{ 0 };
    std::atomic<uint32_t> gDropped{ 0 };

    // Files opened through ls::FileReader, all of them, and the FileOpened rows taken; the rows
    // stop at the client's first game state.
    std::atomic<uint32_t> gFileOpens{ 0 };
    std::atomic<uint32_t> gFileRows{ 0 };
    std::atomic<bool> gClientStateSeen{ false };

    // ecl::GameControl+0x190 is the client's movie player (ecl::VideoManager): the queue's count at
    // +0x24, the video playing (or last played) at +0x30, the engine's player at +0x38 whose +0x10
    // is set while a video plays.
    constexpr std::size_t GameControlVideoManagerOffset = 0x190;
    constexpr std::size_t VideoManagerQueueCountOffset = 0x24;
    constexpr std::size_t VideoManagerCurrentOffset = 0x30;
    constexpr std::size_t VideoManagerPlayerOffset = 0x38;
    constexpr std::size_t PlayerPlayingOffset = 0x10;

    // ls::LoadingScreenManager: visible at +0x08, the active load state at +0x28 (the queue of
    // the next ones at +0x10, its count at +0x1c).
    constexpr std::size_t LoadingScreenVisibleOffset = 0x08;
    constexpr std::size_t LoadingScreenActiveOffset = 0x28;
    // Its progress (uint64 each): LocalProgressTotal +0x30, LocalProgress +0x38, ProgressTotal
    // +0x40, Progress +0x48 (the registry's ls::LoadingScreenManager, read per tick by R7).
    constexpr std::size_t LoadingScreenLocalTotalOffset = 0x30;
    constexpr std::size_t LoadingScreenLocalOffset = 0x38;
    constexpr std::size_t LoadingScreenTotalOffset = 0x40;
    constexpr std::size_t LoadingScreenProgressOffset = 0x48;
    // LocalProgress is recorded in tenths of LocalProgressTotal (a load's local ticks run to
    // hundreds); every change of the other three is recorded.
    constexpr uint64_t LocalProgressSteps = 10;

    // gui::DCLoadingScreen (dx11 build 25605617, as its constructor bg3_dx11.exe+0x1609420 and
    // OnAdvanceHintCommand bg3_dx11.exe+0x1609A50 use it): +0x350 VisibileHintIndex (the slot,
    // 0 or 1, written when a hint was drawn), +0x364 the state of the LCG the hint is drawn with
    // (index = ((state >> 16) & 0x7fff) % count, the last step being the drawn hint), +0x370 the
    // ecl::HintManager (+0x08 the hints, 0x38 bytes each, HintKey first; +0x14 the count).
    constexpr std::size_t ScreenVisibleHintOffset = 0x350;
    constexpr std::size_t ScreenHintSeedOffset = 0x364;
    constexpr std::size_t ScreenHintManagerOffset = 0x370;
    constexpr std::size_t HintManagerHintsOffset = 0x08;
    constexpr std::size_t HintManagerCountOffset = 0x14;
    constexpr std::size_t HintStride = 0x38;

    // The client's timelines whose playback state was seen last.
    struct TimelineSeen
    {
        uint64_t Entity;
        int32_t State;
        uint8_t Type;
        char Name[40];
    };

    constexpr int MaxTimelines = 128;
    TimelineSeen gTimelinesSeen[MaxTimelines];
    int gTimelinesSeenCount = 0;
    TimelineSeen gTimelinesNow[MaxTimelines];

    // ecl::GameStateMachine+0x28: the fade phase (uint32; 0 none, 1 fading out, 2 black, 3 fading
    // in), read and written by ecl::GameStateMachine::Update (bg3_dx11.exe+0x17EEAD0).
    constexpr std::size_t GameStateMachineFadeOffset = 0x28;

    // The interface's state machine, as read live (dx11 build 25605617; Script Extender's GameUI
    // layout puts StateMachine elsewhere, so Ext.UI.GetStateMachine() reads null there):
    // - ls::ResourceManager::UI (ui::GameUI) +0x7C0: the embedded ui::StateMachine (every
    //   ui::UIWidget+0x2F0 points there); its +0x50 is the ui::UIStateMachine.
    // - UIStateMachine+0x98: the root state instance ("Root").
    // - A state instance: +0x10 the array of its layers (pointer, capacity, count at +0x1C), +0x20
    //   its ui::UIState, whose +0x10 is its Name (a Noesis string).
    // - A layer: +0x00 the stack of instances on it (pointer, capacity, count at +0x0C), +0x10 a Guid.
    // - A Noesis string: +0x00 the length (uint32), +0x07 non-zero when the characters are kept
    //   inline at +0x08, otherwise +0x08 points at them (as the game's own state-activated
    //   listener bg3_dx11.exe+0x13C32E0 reads it).
    constexpr std::size_t GameUIStateMachineOffset = 0x7C0;
    constexpr std::size_t StateMachineComponentOffset = 0x50;
    constexpr std::size_t UIStateMachineRootOffset = 0x98;
    constexpr std::size_t InstanceLayersOffset = 0x10;
    constexpr std::size_t InstanceLayerCountOffset = 0x1C;
    constexpr std::size_t InstanceStateOffset = 0x20;
    constexpr std::size_t LayerStackCountOffset = 0x0C;
    constexpr std::size_t UIStateNameOffset = 0x10;

    // What the client poll saw last (the client's tick only touches these).
    struct ClientSeen
    {
        bool Started{ false };
        uint32_t Current{ 0xffffffff };
        int32_t Queue{ 0 };
        bool Playing{ false };
        void* ActiveLoadState{ nullptr };
        bool Visible{ false };
        uint32_t Fade{ 0 };
        uint64_t Progress{ 0 };
        uint64_t ProgressTotal{ 0 };
        uint64_t LocalTotal{ 0 };
        uint64_t LocalStep{ 0 };
    } gSeen;

    // The hint state before OnAdvanceHintCommand ran (its thread only).
    thread_local int32_t gHintSlotBefore = 0;

    // gui::LSImageSequence (a Noesis Image; dx11 build 25605617, as its OnLoaded
    // bg3_dx11.exe+0x12EDA00 uses it): +0x278 its images (ImageSource pointers; after OnLoaded in
    // the order they will be shown), +0x284 their count, then three TimeSpans (100 ns ticks):
    // +0x288 the fade in, +0x290 each image's slot (the storyboard lasts slot x count), +0x298 the
    // fade out (the opacity key frames at fade in, slot - fade out, slot).
    constexpr std::size_t SequenceImagesOffset = 0x278;
    constexpr std::size_t SequenceImageCountOffset = 0x284;
    constexpr std::size_t SequenceFadeInOffset = 0x288;
    constexpr std::size_t SequenceSlotOffset = 0x290;
    constexpr std::size_t SequenceFadeOutOffset = 0x298;
    constexpr uint32_t MaxSequenceImages = 64;
    // The visual tree is searched for the single image of a loading screen at most this deep and
    // this wide (the tree under the main canvas holds a few hundred visuals while a load runs).
    constexpr int MaxWallpaperSearchDepth = 64;
    constexpr int MaxWallpaperSearchNodes = 4096;

    // Every live wallpaper element is followed, at most this many at once; the tree is searched for
    // the ones no OnLoaded announced (single images, a slideshow made before the hook) at most this
    // often while a loading screen is up.
    constexpr int MaxWallpapers = 8;
    constexpr double WallpaperSearchEveryMs = 250.0;

    // The widget a wallpaper element lies in: its nearest ancestor of the visual tree that is a
    // ls.UIWidget, by its x:Name and the type of its DataContext (the game's loading screen is
    // `root` with ls.LoadingScreen; a mod's page is the widget its XAML file names).
    struct WallpaperOwner
    {
        char Name[sizeof(Event::Owner)];
        char Data[sizeof(Event::OwnerData)];
    };

    bool SameOwner(WallpaperOwner const& a, WallpaperOwner const& b)
    {
        return strcmp(a.Name, b.Name) == 0 && strcmp(a.Data, b.Data) == 0;
    }

    // A followed element (one reference of ours; the interface's update only).
    struct Wallpaper
    {
        Noesis::BaseComponent* Element;
        bool IsSequence;
        WallpaperOwner Owner;
        char Shown[64];
        int32_t Position;
        int32_t Count;
        int8_t Visible;
    };

    Wallpaper gWallpapers[MaxWallpapers];
    int gWallpaperCount = 0;
    double gWallpaperSearchedMs = -1.0e9;

    // The slideshows OnLoaded made ready since the last update, with one reference of ours each.
    struct PendingSequence
    {
        Noesis::BaseComponent* Element;
        WallpaperOwner Owner;
    };

    std::mutex gWallpaperLock;
    PendingSequence gWallpaperPending[MaxWallpapers];
    int gWallpaperPendingCount = 0;

    // A wallpaper row with its owner (defined with the record below).
    void RecordWallpaper(What what, int32_t from, int32_t to, char const* name, char const* parent,
        WallpaperOwner const& owner, int8_t visible);

    // The followed element at `index` is let go of: a WallpaperGone row when it had shown something.
    void ReleaseWallpaperAt(int index)
    {
        auto& w = gWallpapers[index];
        if (w.Shown[0] != 0) {
            RecordWallpaper(What::WallpaperGone, w.Position, w.Count, w.Shown,
                w.IsSequence ? "sequence" : "single", w.Owner, w.Visible);
        }
        w.Element->Release();
        for (int i = index + 1; i < gWallpaperCount; i++) {
            gWallpapers[i - 1] = gWallpapers[i];
        }
        gWallpaperCount--;
    }

    int FindFollowed(Noesis::BaseComponent const* element)
    {
        for (int i = 0; i < gWallpaperCount; i++) {
            if (gWallpapers[i].Element == element) return i;
        }
        return -1;
    }

    bool OwnerFollowed(WallpaperOwner const& owner)
    {
        for (int i = 0; i < gWallpaperCount; i++) {
            if (SameOwner(gWallpapers[i].Owner, owner)) return true;
        }
        return false;
    }

    // Follows `element` (its reference is handed over); the oldest is let go of when all slots are taken.
    void Follow(Noesis::BaseComponent* element, bool isSequence, WallpaperOwner const& owner)
    {
        if (gWallpaperCount >= MaxWallpapers) ReleaseWallpaperAt(0);
        auto& w = gWallpapers[gWallpaperCount++];
        w.Element = element;
        w.IsSequence = isSequence;
        w.Owner = owner;
        w.Shown[0] = 0;
        w.Position = 0;
        w.Count = 0;
        w.Visible = -1;
    }

    // The effective value of `object`'s dependency property named `name` (the animated or coerced
    // value over the base one, as Noesis keeps them), or null when the object holds none.
    void* ReadDependencyValue(Noesis::DependencyObject const* object, char const* name)
    {
        for (auto const& entry : object->mValues) {
            if (entry.key == nullptr) continue;
            auto keyName = entry.key->GetName().Str();
            if (keyName == nullptr || strcmp(keyName, name) != 0) continue;
            auto value = entry.value;
            if (value == nullptr || !value->flags.isInitialized) return nullptr;
            if (!value->flags.isComplex) return value->value.simple;
            auto complex = value->value.complex;
            void* effective = complex->base;
            if (value->flags.isAnimated) effective = complex->animated;
            if (value->flags.isCoerced) effective = complex->coerced;
            return effective;
        }
        return nullptr;
    }

    // An image source's name: a BitmapImage's UriSource without its folder and extension
    // (loadScreen_BGartwork_3), else the source's type name; "none" for no source. `uriOut`, when
    // given, gets the whole UriSource ("" without one).
    void NameImage(Noesis::BaseComponent const* source, char* out, std::size_t size, char* uriOut, std::size_t uriSize)
    {
        if (uriOut != nullptr) uriOut[0] = 0;
        if (source == nullptr) {
            strncpy_s(out, size, "none", _TRUNCATE);
            return;
        }
        auto uri = static_cast<Noesis::Uri const*>(ReadDependencyValue(static_cast<Noesis::DependencyObject const*>(source), "UriSource"));
        char const* text = (uri != nullptr) ? uri->mUri.Str() : nullptr;
        if (text == nullptr || text[0] == 0) {
            auto typeName = source->GetClassType()->GetName();
            strncpy_s(out, size, typeName ? typeName : "?", _TRUNCATE);
            return;
        }
        if (uriOut != nullptr) strncpy_s(uriOut, uriSize, text, _TRUNCATE);
        auto file = strrchr(text, '/');
        file = (file != nullptr) ? file + 1 : text;
        auto dot = strrchr(file, '.');
        auto n = (dot != nullptr) ? (std::size_t)(dot - file) : strlen(file);
        if (n > size - 1) n = size - 1;
        memcpy(out, file, n);
        out[n] = 0;
    }

    bool NameImageGuarded(Noesis::BaseComponent const* source, char* out, std::size_t size, char* uriOut, std::size_t uriSize)
    {
        __try {
            NameImage(source, out, size, uriOut, uriSize);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool EndsWith(char const* text, char const* tail)
    {
        auto n = strlen(text), m = strlen(tail);
        return n >= m && strcmp(text + n - m, tail) == 0;
    }

    // Depth first under `visual`: a gui::LSImageSequence, or an Image that is a Control's template
    // root and shows one of the loading screen's own pictures (UriSource under
    // Assets/LoadingScreen/): the single image of the loading screen's other looks
    // (LoadingSingleImage, in the LoadingImages control). Every one found goes into `found`.
    struct FoundWallpaper
    {
        Noesis::Visual* Element;
        bool IsSequence;
    };

    struct FoundWallpapers
    {
        FoundWallpaper Items[MaxWallpapers];
        int Count{ 0 };

        void Add(Noesis::Visual const* element, bool isSequence)
        {
            if (Count >= MaxWallpapers) return;
            Items[Count++] = { const_cast<Noesis::Visual*>(element), isSequence };
        }
    };

    void FindWallpapers(Noesis::Visual const* visual, int depth, int& budget, FoundWallpapers& found)
    {
        if (visual == nullptr || depth > MaxWallpaperSearchDepth || budget-- <= 0) return;
        auto typeName = visual->GetClassType()->GetName();
        if (typeName != nullptr) {
            if (EndsWith(typeName, "LSImageSequence")) {
                found.Add(visual, true);
                return;
            }
            // The template root of the LoadingImages control (a Control), not the overlay drawn
            // over every look (LoadingOverlay, loadScreen_BG, in a Grid).
            auto parent = visual->mVisualParent;
            auto parentType = (parent != nullptr) ? parent->GetClassType()->GetName() : nullptr;
            if (strcmp(typeName, "Image") == 0 && parentType != nullptr && strcmp(parentType, "Control") == 0) {
                auto source = static_cast<Noesis::BaseComponent const*>(ReadDependencyValue(visual, "Source"));
                char name[64], uri[256];
                NameImage(source, name, sizeof(name), uri, sizeof(uri));
                if (strstr(uri, "Assets/LoadingScreen/") != nullptr) {
                    found.Add(visual, false);
                    return;
                }
            }
        }
        auto count = visual->GetVisualChildrenCount();
        for (uint32_t i = 0; i < count && found.Count < MaxWallpapers; i++) {
            FindWallpapers(visual->GetVisualChild(i), depth + 1, budget, found);
        }
    }

    bool FindWallpapersGuarded(FoundWallpapers& found)
    {
        __try {
            auto resourceManager = GetStaticSymbols().ls__gGlobalResourceManager;
            if (resourceManager == nullptr || *resourceManager == nullptr) return false;
            auto ui = (*resourceManager)->UI;
            if (ui == nullptr) return false;
            auto root = reinterpret_cast<Noesis::Visual const*>(ui->NoesisUIManager.MainCanvas);
            int budget = MaxWallpaperSearchNodes;
            FindWallpapers(root, 0, budget, found);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // A flag of the interface held in the stored-value slot itself (as Ext.UI reads a Bool): 1 or 0,
    // and 0 when the object holds no value (a UIElement's IsVisible is stored once it is true).
    int8_t ReadStoredFlag(Noesis::DependencyObject const* object, char const* name)
    {
        auto value = ReadDependencyValue(object, name);
        return (((uintptr_t)value) & 0xff) != 0 ? 1 : 0;
    }

    // The widget `element` lies in (WallpaperOwner); empty names when it lies in none.
    void FindOwner(Noesis::BaseComponent const* element, WallpaperOwner& owner)
    {
        owner.Name[0] = 0;
        owner.Data[0] = 0;
        int depth = 0;
        for (auto v = static_cast<Noesis::Visual const*>(element); v != nullptr && depth < MaxWallpaperSearchDepth * 2;
            v = v->mVisualParent, depth++) {
            auto typeName = v->GetClassType()->GetName();
            if (typeName == nullptr || !EndsWith(typeName, "UIWidget")) continue;
            auto name = static_cast<Noesis::String const*>(ReadDependencyValue(v, "Name"));
            auto text = (name != nullptr) ? name->Str() : nullptr;
            strncpy_s(owner.Name, (text != nullptr) ? text : "", _TRUNCATE);
            auto data = static_cast<Noesis::BaseComponent const*>(ReadDependencyValue(v, "DataContext"));
            auto dataType = (data != nullptr) ? data->GetClassType()->GetName() : nullptr;
            strncpy_s(owner.Data, (dataType != nullptr) ? dataType : "", _TRUNCATE);
            return;
        }
    }

    int8_t ReadVisibleGuarded(Noesis::BaseComponent const* element)
    {
        __try {
            return ReadStoredFlag(static_cast<Noesis::DependencyObject const*>(element), "IsVisible");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    void FindOwnerGuarded(Noesis::BaseComponent const* element, WallpaperOwner& owner)
    {
        __try {
            FindOwner(element, owner);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            strncpy_s(owner.Name, "unreadable", _TRUNCATE);
            owner.Data[0] = 0;
        }
    }

    // The shown image of a followed element: its Source's name, and its position in the slideshow's
    // set (1..; 0 when it is not in it or the element is a single image) with the set's size, and
    // whether the element is visible (its IsVisible).
    bool ReadShownWallpaper(Noesis::BaseComponent const* element, bool isSequence, char* name, std::size_t size,
        int32_t& position, int32_t& count, int8_t& visible)
    {
        __try {
            auto source = static_cast<Noesis::BaseComponent const*>(
                ReadDependencyValue(static_cast<Noesis::DependencyObject const*>(element), "Source"));
            NameImage(source, name, size, nullptr, 0);
            visible = ReadStoredFlag(static_cast<Noesis::DependencyObject const*>(element), "IsVisible");
            position = 0;
            count = 1;
            if (isSequence) {
                auto base = reinterpret_cast<uint8_t const*>(element);
                auto images = *reinterpret_cast<void* const* const*>(base + SequenceImagesOffset);
                auto n = *reinterpret_cast<uint32_t const*>(base + SequenceImageCountOffset);
                count = (int32_t)n;
                if (images != nullptr && n <= MaxSequenceImages) {
                    for (uint32_t i = 0; i < n; i++) {
                        if (images[i] == source) {
                            position = (int32_t)i + 1;
                            break;
                        }
                    }
                }
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // The slideshow's set, read after its OnLoaded: the images in their order and the times.
    struct SequenceSet
    {
        uint32_t Count;
        int64_t FadeIn, Slot, FadeOut;
        Noesis::BaseComponent const* Images[MaxSequenceImages];
    };

    bool ReadSequenceSet(uint8_t const* sequence, SequenceSet& set)
    {
        __try {
            set.Count = *reinterpret_cast<uint32_t const*>(sequence + SequenceImageCountOffset);
            if (set.Count > MaxSequenceImages) return false;
            auto images = *reinterpret_cast<Noesis::BaseComponent const* const* const*>(sequence + SequenceImagesOffset);
            if (images == nullptr && set.Count > 0) return false;
            for (uint32_t i = 0; i < set.Count; i++) set.Images[i] = images[i];
            set.FadeIn = *reinterpret_cast<int64_t const*>(sequence + SequenceFadeInOffset);
            set.Slot = *reinterpret_cast<int64_t const*>(sequence + SequenceSlotOffset);
            set.FadeOut = *reinterpret_cast<int64_t const*>(sequence + SequenceFadeOutOffset);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // The interface's update: takes the slideshows OnLoaded left (a newer one of a widget supersedes
    // the one that widget had), lets go of the elements nobody else holds (their widget is gone),
    // finds the elements no OnLoaded announced while a loading screen is up (one per widget), and
    // records each element's shown image when it changed.
    void PollWallpaper()
    {
        PendingSequence pending[MaxWallpapers];
        int pendingCount{ 0 };
        {
            std::lock_guard _(gWallpaperLock);
            pendingCount = gWallpaperPendingCount;
            for (int i = 0; i < pendingCount; i++) pending[i] = gWallpaperPending[i];
            gWallpaperPendingCount = 0;
        }
        for (int i = 0; i < pendingCount; i++) {
            auto& p = pending[i];
            if (FindFollowed(p.Element) >= 0) {
                // found by the search before its OnLoaded: one reference of ours is enough
                p.Element->Release();
                continue;
            }
            for (int j = gWallpaperCount - 1; j >= 0; j--) {
                if (SameOwner(gWallpapers[j].Owner, p.Owner)) ReleaseWallpaperAt(j);
            }
            Follow(p.Element, true, p.Owner);
        }

        for (int i = gWallpaperCount - 1; i >= 0; i--) {
            if (gWallpapers[i].Element->GetNumReferences() <= 1) ReleaseWallpaperAt(i);
        }

        auto lsmGlobal = GetStaticSymbols().ls__gLoadingScreenManager;
        auto lsm = (lsmGlobal != nullptr) ? reinterpret_cast<uint8_t*>(*lsmGlobal) : nullptr;
        auto now = gClock.NowMs();
        if (lsm != nullptr && *(lsm + LoadingScreenVisibleOffset) != 0 && now - gWallpaperSearchedMs >= WallpaperSearchEveryMs) {
            gWallpaperSearchedMs = now;
            FoundWallpapers found;
            if (FindWallpapersGuarded(found)) {
                for (int i = 0; i < found.Count; i++) {
                    auto element = found.Items[i].Element;
                    if (FindFollowed(element) >= 0) continue;
                    WallpaperOwner owner;
                    FindOwnerGuarded(element, owner);
                    // a widget's element that OnLoaded announced is followed already
                    if (OwnerFollowed(owner)) continue;
                    element->AddReference();
                    Follow(element, found.Items[i].IsSequence, owner);
                }
            }
        }

        // Not bound to ls::LoadingScreenManager's visible flag: the widget stays on screen after
        // that flag is cleared (a window photo showed a change 1.6 s after LoadingScreenHidden).
        for (int i = 0; i < gWallpaperCount; i++) {
            auto& w = gWallpapers[i];
            char name[64];
            int32_t position{ 0 }, count{ 0 };
            int8_t visible{ -1 };
            if (!ReadShownWallpaper(w.Element, w.IsSequence, name, sizeof(name), position, count, visible)) continue;
            // A slideshow found before its OnLoaded has no source yet: nothing is shown.
            if (strcmp(name, "none") == 0 || strcmp(name, w.Shown) == 0) continue;
            strncpy_s(w.Shown, name, _TRUNCATE);
            w.Position = position;
            w.Count = count;
            w.Visible = visible;
            RecordWallpaper(What::WallpaperShown, position, count, name, w.IsSequence ? "sequence" : "single",
                w.Owner, visible);
        }
    }

    // The interface's states seen last (the interface's update only touches these).
    struct UiNode
    {
        void const* Instance;
        int32_t Depth;
        char Name[48];
        char Parent[32];
    };

    constexpr int MaxUiNodes = 64;
    constexpr int MaxUiDepth = 10;
    constexpr uint32_t MaxUiChildren = 32;

    UiNode gUiSeen[MaxUiNodes];
    int gUiSeenCount = 0;
    UiNode gUiNow[MaxUiNodes];

    // Copies a Noesis string into out; false when it does not look like one.
    bool ReadNoesisString(uint8_t const* str, char* out, std::size_t size)
    {
        auto length = *reinterpret_cast<uint32_t const*>(str);
        if (length >= 256) return false;
        auto chars = (str[7] != 0)
            ? reinterpret_cast<char const*>(str + 8)
            : *reinterpret_cast<char const* const*>(str + 8);
        if (chars == nullptr) length = 0;
        auto n = (length < size - 1) ? length : (uint32_t)(size - 1);
        memcpy(out, chars, n);
        out[n] = 0;
        return true;
    }

    // Walks the state tree from `instance` into gUiNow (depth first, layers in order, each layer's
    // stack bottom to top). Plain reads with no object that needs unwinding, so that the caller's
    // structured exception guard can stop at a bad pointer.
    void WalkUiTree(uint8_t const* instance, int depth, char const* parent, int& count)
    {
        if (instance == nullptr || depth > MaxUiDepth || count >= MaxUiNodes) return;

        auto& node = gUiNow[count++];
        node.Instance = instance;
        node.Depth = depth;
        node.Name[0] = 0;
        strncpy_s(node.Parent, parent, _TRUNCATE);
        auto state = *reinterpret_cast<uint8_t const* const*>(instance + InstanceStateOffset);
        if (state != nullptr) {
            ReadNoesisString(state + UIStateNameOffset, node.Name, sizeof(node.Name));
        }
        char const* name = node.Name;

        auto layers = *reinterpret_cast<uint8_t const* const* const*>(instance + InstanceLayersOffset);
        auto layerCount = *reinterpret_cast<uint32_t const*>(instance + InstanceLayerCountOffset);
        if (layers == nullptr || layerCount > MaxUiChildren) return;
        for (uint32_t i = 0; i < layerCount; i++) {
            auto layer = layers[i];
            if (layer == nullptr) continue;
            auto stack = *reinterpret_cast<uint8_t const* const* const*>(layer);
            auto stackCount = *reinterpret_cast<uint32_t const*>(layer + LayerStackCountOffset);
            if (stack == nullptr || stackCount > MaxUiChildren) continue;
            for (uint32_t j = 0; j < stackCount; j++) {
                WalkUiTree(stack[j], depth + 1, name, count);
            }
        }
    }

    // The tree's nodes into gUiNow; -1 when there is no state machine yet or a read faulted.
    int ReadUiTree()
    {
        int count = 0;
        __try {
            auto resourceManager = GetStaticSymbols().ls__gGlobalResourceManager;
            if (resourceManager == nullptr || *resourceManager == nullptr) return -1;
            auto ui = reinterpret_cast<uint8_t const*>((*resourceManager)->UI);
            if (ui == nullptr) return -1;
            auto machine = *reinterpret_cast<uint8_t const* const*>(ui + GameUIStateMachineOffset + StateMachineComponentOffset);
            if (machine == nullptr) return -1;
            auto root = *reinterpret_cast<uint8_t const* const*>(machine + UIStateMachineRootOffset);
            WalkUiTree(root, 0, "", count);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
        return count;
    }

    // The type of a load state, by its vtable as module+0xRVA; false if the object faulted.
    bool DescribeLoadState(void* object, char* out, std::size_t size)
    {
        if (object == nullptr) {
            snprintf(out, size, "none");
            return true;
        }

        uint8_t* vtable{ nullptr };
        __try {
            vtable = *reinterpret_cast<uint8_t**>(object);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        HMODULE module{ nullptr };
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(vtable), &module) && module != nullptr) {
            char path[MAX_PATH]{};
            GetModuleFileNameA(module, path, MAX_PATH);
            auto file = strrchr(path, '\\');
            snprintf(out, size, "%s+0x%llX", file ? file + 1 : path,
                (unsigned long long)(vtable - reinterpret_cast<uint8_t*>(module)));
        } else {
            snprintf(out, size, "0x%llX", (unsigned long long)vtable);
        }
        return true;
    }

    Event* TakeSlot(uint32_t& slot)
    {
        slot = gNext.fetch_add(1);
        if (slot >= Capacity) {
            gNext.store(Capacity);
            gDropped.fetch_add(1);
            return nullptr;
        }
        return &gEvents[slot];
    }

    void Fill(Event& e, double ms, Side side, What what, int32_t from, int32_t to, char const* name, char const* parent)
    {
        e.Ms = ms;
        e.EventSide = side;
        e.Kind = what;
        e.HasCounters = false;
        e.From = from;
        e.To = to;
        e.IoReadBytes = 0;
        e.IoReadOps = 0;
        e.CpuMs = 0.0;
        e.FileOpens = 0;
        e.Name[0] = 0;
        e.Parent[0] = 0;
        e.Owner[0] = 0;
        e.OwnerData[0] = 0;
        e.Visible = -1;
        if (name != nullptr) {
            strncpy_s(e.Name, name, _TRUNCATE);
        }
        if (parent != nullptr) {
            strncpy_s(e.Parent, parent, _TRUNCATE);
        }
    }

    void RecordWallpaper(What what, int32_t from, int32_t to, char const* name, char const* parent,
        WallpaperOwner const& owner, int8_t visible)
    {
        auto ms = gClock.NowMs();
        uint32_t slot;
        auto e = TakeSlot(slot);
        if (e == nullptr) return;
        Fill(*e, ms, Side::Client, what, from, to, name, parent);
        strncpy_s(e->Owner, owner.Name, _TRUNCATE);
        strncpy_s(e->OwnerData, owner.Data, _TRUNCATE);
        e->Visible = visible;
        gReady[slot].store(true, std::memory_order_release);
    }

    // The client's timeline entities (eoc::TimelineDataComponent, through its single-component
    // query, as Ext.Entity.GetAllEntitiesWithComponent walks it) into gTimelinesNow: entity,
    // PlaybackState, TimelineType and the timeline's resource (ecl::timeline::TimelineLoadedComponent);
    // -1 when the client has no entity world yet.
    int ReadTimelines()
    {
        auto& ecs = gExtender->GetClient().GetEntityHelpers();
        if (!ecs.HasEntityWorld()) return -1;
        auto world = ecs.GetEntityWorld();
        if (world == nullptr) return -1;
        auto const& meta = ecs.GetComponentMeta(ExtComponentType::TimelineData);
        if (!meta.ComponentIndex || meta.SingleComponentQuery == ecs::UndefinedQuery) return -1;
        if ((unsigned)meta.SingleComponentQuery >= world->Queries.Queries.size()) return -1;

        int count = 0;
        auto const& query = world->Queries.Queries[(unsigned)meta.SingleComponentQuery];
        for (auto const& storage : query.EntityStorages.values()) {
            for (auto const& handle : storage.Storage->InstanceToPageMap.keys()) {
                if (count >= MaxTimelines) return count;
                auto data = ecs.GetComponent<TimelineDataComponent>(handle);
                if (data == nullptr) continue;
                auto& t = gTimelinesNow[count++];
                t.Entity = handle.Handle;
                t.State = (int32_t)data->PlaybackState;
                t.Type = data->TimelineType;
                t.Name[0] = 0;
                auto loaded = ecs.GetComponent<ecl::timeline::TimelineLoadedComponent>(handle);
                if (loaded != nullptr) {
                    auto text = loaded->Timeline.ToString();
                    strncpy_s(t.Name, text.c_str(), _TRUNCATE);
                }
            }
        }
        return count;
    }

    // The hint the view model drew, if the last call drew one (its slot moved): its key, index and
    // the list's count. Plain reads under a guard.
    bool ReadDrawnHint(uint8_t const* screen, int32_t slotBefore, int32_t& index, int32_t& count, char* key, std::size_t size)
    {
        __try {
            auto slot = *reinterpret_cast<int32_t const*>(screen + ScreenVisibleHintOffset);
            if (slot == slotBefore) return false;
            auto seed = *reinterpret_cast<uint32_t const*>(screen + ScreenHintSeedOffset);
            auto manager = *reinterpret_cast<uint8_t const* const*>(screen + ScreenHintManagerOffset);
            if (manager == nullptr) return false;
            count = *reinterpret_cast<int32_t const*>(manager + HintManagerCountOffset);
            if (count <= 0) return false;
            index = (int32_t)(((seed >> 16) & 0x7fff) % (uint32_t)count);
            auto hints = *reinterpret_cast<uint8_t const* const*>(manager + HintManagerHintsOffset);
            FixedStringBase hintKey;
            hintKey.Index = *reinterpret_cast<uint32_t const*>(hints + (std::size_t)index * HintStride);
            auto text = hintKey.GetStringView();
            auto n = (text.size() < size - 1) ? text.size() : size - 1;
            memcpy(key, text.data(), n);
            key[n] = 0;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}

double NowMs()
{
    return gClock.NowMs();
}

void Record(Side side, What what, int32_t from, int32_t to, char const* name, char const* parent)
{
    auto ms = gClock.NowMs();
    uint32_t slot;
    auto e = TakeSlot(slot);
    if (e == nullptr) return;
    Fill(*e, ms, side, what, from, to, name, parent);
    gReady[slot].store(true, std::memory_order_release);
}

void RecordWithCounters(Side side, What what, int32_t from, int32_t to, char const* name)
{
    auto ms = gClock.NowMs();
    IO_COUNTERS io{};
    bool haveIo = GetProcessIoCounters(GetCurrentProcess(), &io) != 0;
    FILETIME creation{}, exit{}, kernel{}, user{};
    bool haveTimes = GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != 0;
    auto fileOpens = gFileOpens.load();

    if (side == Side::Client && what == What::State) {
        gClientStateSeen.store(true);
    }

    uint32_t slot;
    auto e = TakeSlot(slot);
    if (e == nullptr) return;
    Fill(*e, ms, side, what, from, to, name, nullptr);
    if (haveIo && haveTimes) {
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        e->HasCounters = true;
        e->IoReadBytes = io.ReadTransferCount;
        e->IoReadOps = io.ReadOperationCount;
        e->CpuMs = (double)(k.QuadPart + u.QuadPart) / 10000.0;
        e->FileOpens = fileOpens;
    }
    gReady[slot].store(true, std::memory_order_release);
}

void RecordStep(char const* name, int32_t phase)
{
    RecordWithCounters(Side::Extender, What::Step, phase, -1, name);
}

void FileOpened(char const* path, std::size_t length)
{
    gFileOpens.fetch_add(1);
    if (gClientStateSeen.load() || path == nullptr) return;
    if (gFileRows.fetch_add(1) >= MaxFileRows) return;

    // The end of the path, where the file's own name is.
    char tail[sizeof(Event::Name)];
    auto keep = sizeof(tail) - 1;
    auto from = (length > keep) ? length - keep : 0;
    auto n = length - from;
    memcpy(tail, path + from, n);
    tail[n] = 0;
    Record(Side::Extender, What::FileOpened, -1, -1, tail);
}

void PollClient(void* machine)
{
    auto gameControl = GetStaticSymbols().ecl__gGameControl;
    auto gc = (gameControl != nullptr) ? reinterpret_cast<uint8_t*>(*gameControl) : nullptr;
    auto vm = (gc != nullptr) ? *reinterpret_cast<uint8_t**>(gc + GameControlVideoManagerOffset) : nullptr;
    if (vm != nullptr) {
        auto current = *reinterpret_cast<uint32_t*>(vm + VideoManagerCurrentOffset);
        auto queue = *reinterpret_cast<int32_t*>(vm + VideoManagerQueueCountOffset);
        auto player = *reinterpret_cast<uint8_t**>(vm + VideoManagerPlayerOffset);
        bool playing = player != nullptr && *(player + PlayerPlayingOffset) != 0;

        // A start: another name, the queue shorter by one (the next video taken, also one of the
        // same name), or the player starting again.
        bool started = gSeen.Started && current != 0xffffffff
            && (current != gSeen.Current || queue < gSeen.Queue || (playing && !gSeen.Playing));
        if (!gSeen.Started && current != 0xffffffff && playing) started = true;

        if (gSeen.Playing && !playing) {
            Record(Side::Client, What::VideoStopped);
        }
        if (started) {
            auto name = reinterpret_cast<FixedString*>(vm + VideoManagerCurrentOffset)->GetStringView();
            char buf[64];
            snprintf(buf, sizeof(buf), "%.*s (queue %d)", (int)name.size(), name.data(), queue);
            Record(Side::Client, What::VideoStarted, -1, -1, buf);
        }

        gSeen.Started = true;
        gSeen.Current = current;
        gSeen.Queue = queue;
        gSeen.Playing = playing;
    }

    // The active load state is read here, after ecl::GameStateMachine::Update and on its thread:
    // App::Update calls that Update and ls::LoadingScreenManager::Update one after the other on
    // the same thread, and the manager's Update is the one that ends a load state, destroys it
    // and clears +0x28 in the same call (macOS ls::LoadingScreenManager::Update). So a pointer
    // read here is either null or a live state, the way the manager's own Update reads it; the
    // vtable read is guarded all the same.
    auto lsmGlobal = GetStaticSymbols().ls__gLoadingScreenManager;
    auto lsm = (lsmGlobal != nullptr) ? reinterpret_cast<uint8_t*>(*lsmGlobal) : nullptr;
    if (lsm != nullptr) {
        auto active = *reinterpret_cast<void**>(lsm + LoadingScreenActiveOffset);
        bool visible = *(lsm + LoadingScreenVisibleOffset) != 0;
        if (active != gSeen.ActiveLoadState) {
            char buf[64];
            if (!DescribeLoadState(active, buf, sizeof(buf))) {
                snprintf(buf, sizeof(buf), "unreadable");
            }
            Record(Side::Client, What::LoadState, -1, -1, buf);
            gSeen.ActiveLoadState = active;
        }
        if (visible != gSeen.Visible) {
            Record(Side::Client, visible ? What::LoadingScreenShown : What::LoadingScreenHidden);
            gSeen.Visible = visible;
        }

        auto localTotal = *reinterpret_cast<uint64_t*>(lsm + LoadingScreenLocalTotalOffset);
        auto local = *reinterpret_cast<uint64_t*>(lsm + LoadingScreenLocalOffset);
        auto total = *reinterpret_cast<uint64_t*>(lsm + LoadingScreenTotalOffset);
        auto progress = *reinterpret_cast<uint64_t*>(lsm + LoadingScreenProgressOffset);
        auto localStep = (localTotal != 0) ? (local * LocalProgressSteps / localTotal) : 0;
        if (progress != gSeen.Progress || total != gSeen.ProgressTotal || localTotal != gSeen.LocalTotal
            || localStep != gSeen.LocalStep) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%llu/%llu", (unsigned long long)local, (unsigned long long)localTotal);
            Record(Side::Client, What::LoadingProgress, (int32_t)progress, (int32_t)total, buf);
            gSeen.Progress = progress;
            gSeen.ProgressTotal = total;
            gSeen.LocalTotal = localTotal;
            gSeen.LocalStep = localStep;
        }
    }

    if (machine != nullptr) {
        auto fade = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(machine) + GameStateMachineFadeOffset);
        if (fade != gSeen.Fade) {
            Record(Side::Client, What::Fade, (int32_t)gSeen.Fade, (int32_t)fade);
            gSeen.Fade = fade;
        }
    }

    // The timelines: a row when one appears, changes its PlaybackState or goes away.
    auto count = ReadTimelines();
    if (count >= 0) {
        char type[32];
        for (int j = 0; j < count; j++) {
            auto& now = gTimelinesNow[j];
            int32_t before = -1;
            for (int i = 0; i < gTimelinesSeenCount; i++) {
                if (gTimelinesSeen[i].Entity == now.Entity) {
                    before = gTimelinesSeen[i].State;
                    // The timeline's resource stays named after its TimelineLoadedComponent went
                    // (the game removes it before the timeline's data on an unload).
                    if (now.Name[0] == 0) {
                        memcpy(now.Name, gTimelinesSeen[i].Name, sizeof(now.Name));
                    }
                    break;
                }
            }
            if (before != now.State) {
                snprintf(type, sizeof(type), "%d", (int)now.Type);
                Record(Side::Client, What::TimelinePlayback, before, now.State, now.Name, type);
            }
        }
        for (int i = 0; i < gTimelinesSeenCount; i++) {
            bool found = false;
            for (int j = 0; j < count && !found; j++) {
                found = gTimelinesNow[j].Entity == gTimelinesSeen[i].Entity;
            }
            if (!found) {
                snprintf(type, sizeof(type), "%d", (int)gTimelinesSeen[i].Type);
                Record(Side::Client, What::TimelinePlayback, gTimelinesSeen[i].State, -1, gTimelinesSeen[i].Name, type);
            }
        }
        memcpy(gTimelinesSeen, gTimelinesNow, sizeof(TimelineSeen) * count);
        gTimelinesSeenCount = count;
    }
}

void BeforeAdvanceHint(void* screen)
{
    gHintSlotBefore = -2;
    if (screen == nullptr) return;
    __try {
        gHintSlotBefore = *reinterpret_cast<int32_t const*>(reinterpret_cast<uint8_t const*>(screen) + ScreenVisibleHintOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gHintSlotBefore = -2;
    }
}

void AfterAdvanceHint(void* screen)
{
    if (screen == nullptr || gHintSlotBefore == -2) return;
    int32_t index{ -1 }, count{ 0 };
    char key[64];
    if (ReadDrawnHint(reinterpret_cast<uint8_t const*>(screen), gHintSlotBefore, index, count, key, sizeof(key))) {
        Record(Side::Client, What::HintShown, index, count, key);
    }
}

void AfterImageSequenceLoaded(void* sequence)
{
    if (sequence == nullptr) return;
    SequenceSet set;
    if (!ReadSequenceSet(reinterpret_cast<uint8_t const*>(sequence), set)) return;

    // The times in milliseconds (TimeSpan ticks are 100 ns).
    char times[32];
    snprintf(times, sizeof(times), "%lld/%lld/%lld", (long long)(set.Slot / 10000),
        (long long)(set.FadeIn / 10000), (long long)(set.FadeOut / 10000));
    auto control = reinterpret_cast<Noesis::BaseComponent*>(sequence);
    WallpaperOwner owner;
    FindOwnerGuarded(control, owner);
    auto visible = ReadVisibleGuarded(control);
    for (uint32_t i = 0; i < set.Count; i++) {
        char name[64];
        if (!NameImageGuarded(set.Images[i], name, sizeof(name), nullptr, 0)) {
            strncpy_s(name, "unreadable", _TRUNCATE);
        }
        RecordWallpaper(What::WallpaperSet, (int32_t)i + 1, (int32_t)set.Count, name, times, owner, visible);
    }

    // Kept for the interface's update (PollWallpaper), which follows it beside the other widgets'.
    control->AddReference();
    Noesis::BaseComponent* replaced{ nullptr };
    {
        std::lock_guard _(gWallpaperLock);
        // Two OnLoaded calls of one widget before one update: the older one was never shown.
        for (int i = 0; i < gWallpaperPendingCount && replaced == nullptr; i++) {
            if (SameOwner(gWallpaperPending[i].Owner, owner)) {
                replaced = gWallpaperPending[i].Element;
                gWallpaperPending[i].Element = control;
            }
        }
        if (replaced == nullptr) {
            if (gWallpaperPendingCount >= MaxWallpapers) {
                replaced = gWallpaperPending[0].Element;
                for (int i = 1; i < gWallpaperPendingCount; i++) gWallpaperPending[i - 1] = gWallpaperPending[i];
                gWallpaperPendingCount--;
            }
            gWallpaperPending[gWallpaperPendingCount++] = { control, owner };
        }
    }
    if (replaced != nullptr) replaced->Release();
}

void PollUi()
{
    PollWallpaper();

    auto count = ReadUiTree();
    if (count < 0) return;

    auto same = [](UiNode const& a, UiNode const& b) {
        return a.Instance == b.Instance && strcmp(a.Name, b.Name) == 0;
    };

    // Gone: in what was seen, not in the tree now (by instance and name).
    for (int i = 0; i < gUiSeenCount; i++) {
        bool found = false;
        for (int j = 0; j < count && !found; j++) {
            found = same(gUiNow[j], gUiSeen[i]);
        }
        if (!found) {
            Record(Side::Client, What::UIState, gUiSeen[i].Depth, 0, gUiSeen[i].Name, gUiSeen[i].Parent);
        }
    }

    // New: in the tree now, not seen before.
    for (int j = 0; j < count; j++) {
        bool found = false;
        for (int i = 0; i < gUiSeenCount && !found; i++) {
            found = same(gUiSeen[i], gUiNow[j]);
        }
        if (!found) {
            Record(Side::Client, What::UIState, gUiNow[j].Depth, 1, gUiNow[j].Name, gUiNow[j].Parent);
        }
    }

    memcpy(gUiSeen, gUiNow, sizeof(UiNode) * count);
    gUiSeenCount = count;
}

uint32_t Count()
{
    auto n = gNext.load();
    if (n > Capacity) n = Capacity;
    // Only the slots whose writing has finished.
    uint32_t ready = 0;
    while (ready < n && gReady[ready].load(std::memory_order_acquire)) ready++;
    return ready;
}

Event const& Get(uint32_t index)
{
    return gEvents[index];
}

bool IsReady(uint32_t index)
{
    return gReady[index].load(std::memory_order_acquire);
}

uint32_t Dropped()
{
    return gDropped.load();
}

END_NS()
