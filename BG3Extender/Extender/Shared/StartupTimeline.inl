// The start's timeline (StartupTimeline.h). Included once, by ScriptExtenderClient.cpp.
#include <Extender/Shared/StartupTimeline.h>
#include <cstdio>
#include <cstring>

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
    } gSeen;

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
        if (name != nullptr) {
            strncpy_s(e.Name, name, _TRUNCATE);
        }
        if (parent != nullptr) {
            strncpy_s(e.Parent, parent, _TRUNCATE);
        }
    }
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
    }

    if (machine != nullptr) {
        auto fade = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(machine) + GameStateMachineFadeOffset);
        if (fade != gSeen.Fade) {
            Record(Side::Client, What::Fade, (int32_t)gSeen.Fade, (int32_t)fade);
            gSeen.Fade = fade;
        }
    }
}

void PollUi()
{
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

uint32_t Dropped()
{
    return gDropped.load();
}

END_NS()
