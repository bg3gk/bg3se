// GateKit addition: the render thread's frame skipped, the game's Lua collector held during a load,
// and the counters that measure both (RenderCut.h). Included once, by ScriptExtender.cpp.
#include <Extender/Shared/RenderCut.h>
#include <Extender/Shared/StartupTimeline.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <psapi.h>

BEGIN_SE()

enum class RenderThreadUpdateHookTag {};
using RenderThreadUpdateHookType = WrappableFunction<RenderThreadUpdateHookTag, RenderThread__UpdateProc>;
RenderThreadUpdateHookType* RenderThreadUpdateHookType::gHook;

enum class LuaCStepHookTag {};
using LuaCStepHookType = WrappableFunction<LuaCStepHookTag, LuaC_StepProc>;
LuaCStepHookType* LuaCStepHookType::gHook;

END_SE()

BEGIN_NS(render_cut)

namespace
{
    RenderThreadUpdateHookType gRenderHook;
    LuaCStepHookType gLuaCStepHook;

    bool gInstalled{ false };
    bool gLuaGcInstalled{ false };
    std::atomic<int> gMode{ 0 };
    std::atomic<uint64_t> gFrames{ 0 };
    std::atomic<uint64_t> gSkipped{ 0 };
    std::atomic<uint64_t> gClientTicks{ 0 };
    std::atomic<uint64_t> gServerTicks{ 0 };
    std::atomic<uint64_t> gGcHeld{ 0 };
    std::atomic<uint64_t> gGcReleased{ 0 };

    HANDLE gClientThread{ nullptr };
    HANDLE gServerThread{ nullptr };
    HANDLE gMainThread{ nullptr };
    DWORD gServerThreadId{ 0 };

    // server thread only
    bool gServerLoading{ false };

    // The game's Lua 5.3: lua_State+0x18 is its global_State; global_State+0x10 totalbytes,
    // +0x18 GCdebt (luaE_setdebt keeps their sum, the bytes in use).
    constexpr std::size_t LuaStateGlobalOffset = 0x18;
    constexpr std::size_t GlobalTotalBytesOffset = 0x10;
    constexpr std::size_t GlobalDebtOffset = 0x18;
    // What a held step sets the debt to: Lua 5.3's own for a stopped collector (luaC_step with
    // gcrunning 0: luaE_setdebt(g, -GCSTEPSIZE*10)). The step is asked for again after that many
    // bytes; once the load is over it runs as the game's. No state is remembered, so a state
    // closed during the load is never touched again.
    constexpr int64_t HeldDebt = 24000;

    HANDLE OpenCurrentThread()
    {
        return OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
    }

    double ThreadCpuMs(HANDLE thread)
    {
        if (thread == nullptr) return -1.0;
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return -1.0;
        auto k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
        auto u = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
        return (double)(k + u) / 10000.0;
    }

    void SetDebt(uint8_t* g, int64_t debt)
    {
        auto& total = *reinterpret_cast<int64_t*>(g + GlobalTotalBytesOffset);
        auto& current = *reinterpret_cast<int64_t*>(g + GlobalDebtOffset);
        auto inUse = total + current;
        total = inUse - debt;
        current = debt;
    }

    // The render frames (rf::RenderFrame) of ls::RenderFrameSystem, as ls::RenderThread::Update
    // walks them: the app (BaseApp, AppInstance) +0x1D8 is the engine's entity world, its +0x30 the
    // systems (0xF8 bytes each, the system first), the system's index the game's
    // TypeIndex<ls::RenderFrameSystem> of ecs::EntityWorld::SystemsContext (the extender's index
    // symbols); the system +0x18 its frames, +0x24 their count. A frame whose +0x40 is 2 is gone;
    // its byte +0x42F0 is the flag Update reads before it begins, records, submits and presents the
    // frame (rf::RenderFrame::IsEnabled by its use; the Toolkit exports SetIsEnabled).
    constexpr std::size_t AppEngineWorldOffset = 0x1D8;
    constexpr std::size_t WorldSystemsOffset = 0x30;
    constexpr std::size_t SystemEntrySize = 0xF8;
    constexpr std::size_t FrameSystemFramesOffset = 0x18;
    constexpr std::size_t FrameSystemCountOffset = 0x24;
    constexpr std::size_t FrameStateOffset = 0x40;
    constexpr std::size_t FrameEnabledOffset = 0x42F0;

    int32_t const* gFrameSystemIndex{ nullptr };
    bool gFrameSystemLooked{ false };
    std::vector<uint8_t*> gDisabledFrames;   // main thread only: the frames this turned off

    int32_t const* FindFrameSystemIndex()
    {
        if (gFrameSystemLooked) return gFrameSystemIndex;
        gFrameSystemLooked = true;
        auto const& sym = GetStaticSymbols();
        for (auto const& [index, info] : sym.IndexSymbolToNameMaps) {
            if (info.name == nullptr || strstr(info.name, "ls::RenderFrameSystem") == nullptr) continue;
            auto ctx = sym.IndexSymbolToContextMaps.find(info.context);
            if (ctx == sym.IndexSymbolToContextMaps.end() || ctx->second == nullptr
                || strstr(ctx->second, "SystemsContext") == nullptr) continue;
            gFrameSystemIndex = index;
            break;
        }
        return gFrameSystemIndex;
    }

    constexpr uint32_t MaxFrames = 64;

    // The frames now into `out` (none when anything along the way is not there); returns the count.
    uint32_t ReadFrames(uint8_t* app, int32_t index, uint8_t** out)
    {
        uint32_t n = 0;
        __try {
            auto world = *reinterpret_cast<uint8_t**>(app + AppEngineWorldOffset);
            if (world == nullptr) return 0;
            auto systems = *reinterpret_cast<uint8_t**>(world + WorldSystemsOffset);
            if (systems == nullptr) return 0;
            auto system = *reinterpret_cast<uint8_t**>(systems + (std::size_t)index * SystemEntrySize);
            if (system == nullptr) return 0;
            auto frames = *reinterpret_cast<uint8_t***>(system + FrameSystemFramesOffset);
            auto count = *reinterpret_cast<uint32_t*>(system + FrameSystemCountOffset);
            if (frames == nullptr || count > MaxFrames) return 0;
            for (uint32_t i = 0; i < count; i++) {
                auto f = frames[i];
                if (f != nullptr && *reinterpret_cast<int32_t*>(f + FrameStateOffset) != 2) out[n++] = f;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        return n;
    }

    std::vector<uint8_t*> Frames()
    {
        auto app = GetStaticSymbols().AppInstance;
        auto index = FindFrameSystemIndex();
        if (app == nullptr || *app == nullptr || index == nullptr || *index < 0 || *index > 4096) return {};
        uint8_t* raw[MaxFrames];
        auto n = ReadFrames(reinterpret_cast<uint8_t*>(*app), *index, raw);
        return std::vector<uint8_t*>(raw, raw + n);
    }

    // Turns the frames off (this frame not drawn) or gives back the ones this turned off.
    void SetFramesDrawn(bool drawn)
    {
        if (drawn && gDisabledFrames.empty()) return;
        auto frames = Frames();
        if (drawn) {
            for (auto f : frames) {
                if (std::find(gDisabledFrames.begin(), gDisabledFrames.end(), f) != gDisabledFrames.end()) {
                    f[FrameEnabledOffset] = 1;
                }
            }
            gDisabledFrames.clear();
            return;
        }
        for (auto f : frames) {
            if (f[FrameEnabledOffset] != 0) {
                f[FrameEnabledOffset] = 0;
                if (std::find(gDisabledFrames.begin(), gDisabledFrames.end(), f) == gDisabledFrames.end()) {
                    gDisabledFrames.push_back(f);
                }
            }
        }
    }

    // Mode 1 and the skipped frames of mode N: the render frames turned off for this update, and the
    // rest of ls::RenderThread::Update run as the game's - its waits for the game's threads, the
    // window's messages, the tasks other threads dispatched to this one, the resource manager and
    // the render commands (a client thread waits on those; skipping the whole update hung it).
    // The client state callback can stop pacing while the render thread is running.
    std::atomic<uint32_t> gPaceUs{ 0 };
    HANDLE gPaceTimer{ nullptr };
    LARGE_INTEGER gPaceFreq{};
    LARGE_INTEGER gLastBegin{};
    std::atomic<uint64_t> gPacedSleeps{ 0 };
    std::atomic<uint64_t> gPacedSleptUs{ 0 };

    // After a skipped update: sleep until gPaceUs after the update began (nothing when it already took
    // that long). A drawn update is never paced: the game's own frame cap paces it.
    void PaceSkipped(LARGE_INTEGER began)
    {
        auto paceUs = gPaceUs.load(std::memory_order_relaxed);
        if (paceUs == 0) return;
        if (gPaceFreq.QuadPart == 0) QueryPerformanceFrequency(&gPaceFreq);
        if (gPaceTimer == nullptr) {
            gPaceTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (gPaceTimer == nullptr) gPaceTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
            if (gPaceTimer == nullptr) { gPaceUs = 0; return; }
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        auto elapsedUs = (int64_t)((now.QuadPart - began.QuadPart) * 1000000 / gPaceFreq.QuadPart);
        auto restUs = (int64_t)paceUs - elapsedUs;
        if (restUs <= 200) return;
        LARGE_INTEGER due;
        due.QuadPart = -restUs * 10;   // relative, 100 ns units
        if (SetWaitableTimer(gPaceTimer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(gPaceTimer, INFINITE);
            gPacedSleeps.fetch_add(1, std::memory_order_relaxed);
            gPacedSleptUs.fetch_add((uint64_t)restUs, std::memory_order_relaxed);
        }
    }

    void RenderWrapper(RenderThread__UpdateProc* next, void* self)
    {
        if (gMainThread == nullptr) gMainThread = OpenCurrentThread();
        LARGE_INTEGER began;
        QueryPerformanceCounter(&began);
        auto frame = gFrames.fetch_add(1);
        auto mode = gMode.load(std::memory_order_relaxed);
        bool drawn = mode == 0 || (mode >= 2 && frame % (uint64_t)mode == 0);
        SetFramesDrawn(drawn);
        if (!drawn) gSkipped.fetch_add(1);
        next(self);
        if (!drawn) PaceSkipped(began);
    }

    bool ServerLoadingState(int32_t to)
    {
        using S = esv::GameState;
        auto s = (S)to;
        return s == S::UnloadLevel || s == S::UnloadSession || s == S::UnloadModule || s == S::LoadModule
            || s == S::LoadSession || s == S::LoadLevel || s == S::Sync;
    }

    void LuaCStepWrapper(LuaC_StepProc* next, void* L)
    {
        if (gServerLoading && L != nullptr && GetCurrentThreadId() == gServerThreadId) {
            auto g = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(L) + LuaStateGlobalOffset);
            if (g != nullptr) {
                SetDebt(g, -HeldDebt);
                gGcHeld.fetch_add(1);
                return;
            }
        }
        next(L);
    }
}

namespace
{
    // RenderCutFromRunning: the skip is still to begin at the client's first Running.
    std::atomic<bool> gSkipAtRunning{ false };
    // RenderCutSkipPaceStartOnly: the pace is still to end at the client's first Running.
    std::atomic<bool> gPaceEndAtRunning{ false };
}

void Install(bool renderCut, bool skipFromStart, bool luaGc, bool skipFromRunning, uint32_t skipPaceUs, bool paceStartOnly)
{
    auto& sym = GetStaticSymbols();
    if (renderCut && sym.ls__RenderThread__Update != nullptr) {
        gRenderHook.Wrap(sym.ls__RenderThread__Update);
        gRenderHook.SetWrapper(&RenderWrapper);
        gInstalled = true;
        gPaceUs = skipPaceUs;
        gPaceEndAtRunning = paceStartOnly && skipPaceUs != 0;
        gMode = skipFromStart && !skipFromRunning ? 1 : 0;
        gSkipAtRunning = skipFromRunning;
    }
    if (luaGc && sym.lua__luaC_step != nullptr) {
        gLuaCStepHook.Wrap(sym.lua__luaC_step);
        gLuaCStepHook.SetWrapper(&LuaCStepWrapper);
        gLuaGcInstalled = true;
    }
}

int SetMode(int mode)
{
    auto before = gMode.load();
    if (mode >= 0) {
        gMode = mode;
        char text[64];
        snprintf(text, sizeof(text), "RenderCut mode %d -> %d", before, mode);
        startup_timeline::RecordStep(text, startup_timeline::StepBegin);
    }
    return before;
}

Counters Read()
{
    Counters c{};
    c.Installed = gInstalled;
    c.LuaGcInstalled = gLuaGcInstalled;
    c.Mode = gMode.load();
    c.Frames = gFrames.load();
    c.Skipped = gSkipped.load();
    c.RenderFrames = (uint32_t)Frames().size();
    c.RenderFramesOff = (uint32_t)gDisabledFrames.size();
    c.ClientTicks = gClientTicks.load();
    c.ServerTicks = gServerTicks.load();
    c.GcHeld = gGcHeld.load();
    c.GcReleased = gGcReleased.load();
    c.ClientCpuMs = ThreadCpuMs(gClientThread);
    c.ServerCpuMs = ThreadCpuMs(gServerThread);
    c.MainCpuMs = ThreadCpuMs(gMainThread);
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        auto k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
        auto u = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
        c.ProcessCpuMs = (double)(k + u) / 10000.0;
    }
    PROCESS_MEMORY_COUNTERS_EX mem{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem))) {
        c.PrivateMB = (double)mem.PrivateUsage / (1024.0 * 1024.0);
        c.WorkingSetMB = (double)mem.WorkingSetSize / (1024.0 * 1024.0);
    }
    c.Ms = startup_timeline::NowMs();
    c.SkipPaceUs = gPaceUs;
    c.PacedSleeps = gPacedSleeps.load();
    c.PacedSleptMs = (double)gPacedSleptUs.load() / 1000.0;
    return c;
}

void OnClientTick()
{
    if (gClientThread == nullptr) gClientThread = OpenCurrentThread();
    gClientTicks.fetch_add(1, std::memory_order_relaxed);
}

void OnClientState(int32_t from, int32_t to)
{
    if (!gInstalled || (ecl::GameState)to != ecl::GameState::Running) return;
    if (gPaceEndAtRunning.exchange(false)) {
        gPaceUs.store(0, std::memory_order_relaxed);
        startup_timeline::RecordStep("RenderCut pace off (the client's first Running)", startup_timeline::StepBegin);
    }
    if (!gSkipAtRunning.load()) return;
    gSkipAtRunning = false;
    gMode = 1;
    startup_timeline::RecordStep("RenderCut mode 0 -> 1 (the client's first Running)", startup_timeline::StepBegin);
}

void OnServerTick()
{
    if (gServerThread == nullptr) {
        gServerThread = OpenCurrentThread();
        gServerThreadId = GetCurrentThreadId();
    }
    gServerTicks.fetch_add(1, std::memory_order_relaxed);
}

void OnServerState(int32_t from, int32_t to)
{
    if (!gLuaGcInstalled) return;
    if (gServerThreadId == 0) gServerThreadId = GetCurrentThreadId();
    auto loading = ServerLoadingState(to);
    if (loading && !gServerLoading) {
        startup_timeline::RecordStep("FastLoad Lua collector: held", startup_timeline::StepBegin);
    }
    if (!loading && gServerLoading) {
        gGcReleased.fetch_add(1);
        char text[80];
        snprintf(text, sizeof(text), "FastLoad Lua collector: given back (%llu steps held so far)",
            (unsigned long long)gGcHeld.load());
        startup_timeline::RecordStep(text, startup_timeline::StepBegin);
    }
    gServerLoading = loading;
}

END_NS()
