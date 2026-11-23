// GateKit addition: the tick waits slept instead of spun, and the opens of chosen resource classes
// stubbed (Headless.h). Included once, by ScriptExtender.cpp.
#include <Extender/Shared/Headless.h>
#include <Extender/Shared/StartupTimeline.h>
#include <atomic>
#include <vector>
#include <detours.h>

BEGIN_SE()

enum class TickWaitHookTag {};
using TickWaitHookType = WrappableFunction<TickWaitHookTag, TickWaitProc>;
TickWaitHookType* TickWaitHookType::gHook;

enum class GenerateTreasureHookTag {};
using GenerateTreasureHookType = WrappableFunction<GenerateTreasureHookTag, GenerateTreasureProc>;
GenerateTreasureHookType* GenerateTreasureHookType::gHook;

END_SE()

BEGIN_NS(headless)

namespace
{
    TickWaitHookType gTickWaitHook;

    bool gIdleSleepInstalled{ false };
    std::atomic<uint64_t> gWaits{ 0 };
    std::atomic<uint64_t> gSlept{ 0 };
    std::atomic<uint64_t> gSleptUs{ 0 };
    std::atomic<uint64_t> gStubbedOpens{ 0 };
    std::vector<std::string> gStubExtensions;
    int64_t gFrequency{ 0 };

    // The tick wait's object (unplaced::TickWait's first argument): an ls::PreciseClock at +0, then
    // the period at +0x18, the time the last wait measured at +0x20 and the carry (how far the last
    // wait overran) at +0x28, all in microseconds (read from the function's body; the server's
    // worker holds it at +0x78, its period +0x90 = 33333 read live).
    constexpr std::size_t PeriodOffset = 0x18;
    constexpr std::size_t MeasuredOffset = 0x20;
    constexpr std::size_t CarryOffset = 0x28;

    // A wait longer than this sleeps first; the game's own wait keeps the last Margin.
    constexpr int64_t SleepAboveUs = 3000;
    constexpr int64_t MarginUs = 1500;

    thread_local HANDLE tTimer{ nullptr };
    thread_local bool tTimerTried{ false };

    HANDLE Timer()
    {
        if (!tTimerTried) {
            tTimerTried = true;
            tTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        }
        return tTimer;
    }

    int64_t NowUs()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (int64_t)((double)now.QuadPart * 1000000.0 / (double)gFrequency);
    }

    void TickWaitWrapper(TickWaitProc* next, void* clock, int64_t lessUs)
    {
        gWaits.fetch_add(1, std::memory_order_relaxed);
        auto c = reinterpret_cast<uint8_t*>(clock);
        auto period = *reinterpret_cast<int64_t*>(c + PeriodOffset);
        auto carry = *reinterpret_cast<int64_t*>(c + CarryOffset);
        auto remaining = period - carry - lessUs;
        auto timer = remaining > SleepAboveUs ? Timer() : nullptr;
        if (timer == nullptr) {
            next(clock, lessUs);
            return;
        }
        auto began = NowUs();
        LARGE_INTEGER due;
        due.QuadPart = -(remaining - MarginUs) * 10;   // relative, 100 ns units
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        }
        auto slept = NowUs() - began;
        if (slept < 0) slept = 0;
        next(clock, lessUs + slept);
        // the game's measured time is the whole wait; its carry is how far the whole wait overran
        *reinterpret_cast<int64_t*>(c + MeasuredOffset) += slept;
        if (slept > remaining) *reinterpret_cast<int64_t*>(c + CarryOffset) += slept - remaining;
        gSlept.fetch_add(1, std::memory_order_relaxed);
        gSleptUs.fetch_add((uint64_t)slept, std::memory_order_relaxed);
    }

    std::vector<std::string> SplitExtensions(std::string const& list)
    {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= list.size()) {
            auto end = list.find(',', start);
            if (end == std::string::npos) end = list.size();
            auto item = list.substr(start, end - start);
            while (!item.empty() && item.front() == ' ') item.erase(item.begin());
            while (!item.empty() && item.back() == ' ') item.pop_back();
            for (auto& ch : item) ch = (char)tolower((unsigned char)ch);
            if (!item.empty()) out.push_back(item);
            start = end + 1;
        }
        return out;
    }
}

void Install(bool idleSleep, std::string const& stubExtensions)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    gFrequency = f.QuadPart;
    auto& sym = GetStaticSymbols();
    if (idleSleep && sym.unplaced__TickWait != nullptr) {
        gTickWaitHook.Wrap(sym.unplaced__TickWait);
        gTickWaitHook.SetWrapper(&TickWaitWrapper);
        gIdleSleepInstalled = true;
        startup_timeline::RecordStep("IdleSleep on", startup_timeline::StepBegin);
    }
    gStubExtensions = SplitExtensions(stubExtensions);
    if (!gStubExtensions.empty()) {
        startup_timeline::RecordStep("StubFileExtensions on", startup_timeline::StepBegin);
    }
}

bool Stubbed(char const* path, std::size_t length)
{
    if (gStubExtensions.empty() || path == nullptr) return false;
    for (auto const& ext : gStubExtensions) {
        if (length < ext.size()) continue;
        auto tail = path + length - ext.size();
        bool same = true;
        for (std::size_t i = 0; i < ext.size(); i++) {
            if ((char)tolower((unsigned char)tail[i]) != ext[i]) { same = false; break; }
        }
        if (same) {
            gStubbedOpens.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

Counters Read()
{
    Counters c{};
    c.IdleSleepInstalled = gIdleSleepInstalled;
    c.Waits = gWaits.load();
    c.Slept = gSlept.load();
    c.SleptMs = (double)gSleptUs.load() / 1000.0;
    c.StubExtensions = (uint32_t)gStubExtensions.size();
    c.StubbedOpens = gStubbedOpens.load();
    return c;
}

// ---- TreasurePin (W3 round 6) ------------------------------------------------------------------

namespace
{
    GenerateTreasureHookType gTreasureHook;
    bool gTreasureInstalled{ false };
    std::atomic<bool> gTreasurePinned{ false };
    std::atomic<uint32_t> gTreasureSeed{ 0 };
    std::atomic<uint64_t> gTreasureCalls{ 0 };
    std::atomic<uint64_t> gTreasureKeyed{ 0 };
    std::atomic<uint64_t> gTreasureUnkeyed{ 0 };

    // The server's saved random: esv::EocServer +0xB8, an ls::Random { InitialSeed, CurrentSeed }
    // (esv::inventory::TreasureGenerationUnitTestHelper::GetSavedRandom returns this place).
    uint32_t* SavedRandom()
    {
        auto server = GetStaticSymbols().esv__EoCServer;
        if (server == nullptr || *server == nullptr) return nullptr;
        return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(*server) + 0xB8);
    }

    // FNV-1a over the seed and the UUID's 16 bytes: one seed an entity, the same every run.
    uint32_t Mix(uint32_t seed, Guid const& uuid)
    {
        uint32_t h = 2166136261u;
        auto step = [&h](uint8_t b) { h ^= b; h *= 16777619u; };
        for (int i = 0; i < 4; i++) step((uint8_t)(seed >> (8 * i)));
        auto bytes = reinterpret_cast<uint8_t const*>(&uuid);
        for (std::size_t i = 0; i < sizeof(Guid); i++) step(bytes[i]);
        return h;
    }

    bool GenerateTreasureWrapper(GenerateTreasureProc* next, void* self, void* view, uint64_t* entity,
        uint64_t* finder, int32_t* table, int32_t level, uint8_t flag, int64_t out)
    {
        gTreasureCalls.fetch_add(1, std::memory_order_relaxed);
        if (gTreasurePinned.load(std::memory_order_acquire) && entity != nullptr) {
            auto random = SavedRandom();
            auto& ecs = gExtender->GetServer().GetEntityHelpers();
            UuidComponent* uuid = nullptr;
            if (random != nullptr && ecs.HasEntityWorld()) {
                uuid = ecs.GetComponent<UuidComponent>(EntityHandle(*entity));
            }
            if (uuid != nullptr) {
                auto seed = Mix(gTreasureSeed.load(), uuid->EntityUuid);
                random[0] = seed;
                random[1] = seed;
                gTreasureKeyed.fetch_add(1, std::memory_order_relaxed);
            } else {
                gTreasureUnkeyed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return next(self, view, entity, finder, table, level, flag, out);
    }
}

void InstallTreasurePin(bool on)
{
    auto& sym = GetStaticSymbols();
    if (on && sym.esv__inventory__TreasureGenerationSystem__GenerateTreasure != nullptr) {
        gTreasureHook.Wrap(sym.esv__inventory__TreasureGenerationSystem__GenerateTreasure);
        gTreasureHook.SetWrapper(&GenerateTreasureWrapper);
        gTreasureInstalled = true;
        startup_timeline::RecordStep("TreasurePin installed", startup_timeline::StepBegin);
    }
}

void PinTreasure(bool on, uint32_t seed)
{
    gTreasureSeed.store(seed);
    gTreasurePinned.store(on && gTreasureInstalled, std::memory_order_release);
}

// ---- LargeAllocTraceMB (W3 round 7) ------------------------------------------------------------

namespace
{
    using VirtualAllocProc = LPVOID WINAPI(LPVOID, SIZE_T, DWORD, DWORD);
    VirtualAllocProc* gRealVirtualAlloc{ nullptr };
    uint64_t gLargeAllocBytes{ 0 };
    std::atomic<int> gLargeAllocRows{ 0 };
    constexpr int MaxLargeAllocs = 64;
    constexpr int LargeAllocFrames = 8;

    // "module+0xRVA" of an address (the module's file name without its folder).
    void NameAddress(void* address, char* out, std::size_t size)
    {
        HMODULE module{ nullptr };
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(address), &module) || module == nullptr) {
            snprintf(out, size, "0x%llx", (unsigned long long)address);
            return;
        }
        char path[MAX_PATH]{};
        GetModuleFileNameA(module, path, MAX_PATH);
        auto name = strrchr(path, '\\');
        name = name ? name + 1 : path;
        snprintf(out, size, "%s+0x%llx", name,
            (unsigned long long)(reinterpret_cast<uint8_t*>(address) - reinterpret_cast<uint8_t*>(module)));
    }

    LPVOID WINAPI LargeVirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect)
    {
        auto result = gRealVirtualAlloc(address, size, type, protect);
        if (size >= gLargeAllocBytes && gLargeAllocRows.fetch_add(1) < MaxLargeAllocs) {
            void* frames[LargeAllocFrames + 1]{};
            auto n = CaptureStackBackTrace(1, LargeAllocFrames, frames, nullptr);
            char text[64];
            snprintf(text, sizeof(text), "VirtualAlloc %llu MB t%lx p%lx -> %llx", (unsigned long long)(size >> 20),
                type, protect, (unsigned long long)result);
            startup_timeline::RecordStep(text, startup_timeline::StepBegin);
            for (USHORT i = 0; i < n; i++) {
                char where[56];
                NameAddress(frames[i], where, sizeof(where));
                snprintf(text, sizeof(text), "  <- %s", where);
                startup_timeline::RecordStep(text, startup_timeline::StepBegin);
            }
        }
        return result;
    }
}

void InstallLargeAllocTrace(uint32_t megabytes)
{
    if (megabytes == 0) return;
    gLargeAllocBytes = (uint64_t)megabytes << 20;
    gRealVirtualAlloc = &::VirtualAlloc;
    if (DetourAttach(reinterpret_cast<void**>(&gRealVirtualAlloc), reinterpret_cast<void*>(&LargeVirtualAlloc)) == NO_ERROR) {
        startup_timeline::RecordStep("LargeAllocTrace on", startup_timeline::StepBegin);
    }
}

// Exit diagnostics

namespace
{
    using ExitProcessProc = void WINAPI(UINT);
    using TerminateProcessProc = BOOL WINAPI(HANDLE, UINT);
    ExitProcessProc* gRealExitProcess{ nullptr };
    TerminateProcessProc* gRealTerminateProcess{ nullptr };
    std::wstring gExitFolder;
    std::atomic<bool> gExitWritten{ false };
    constexpr int ExitFrames = 24;
    constexpr uint32_t ExitTimelineRows = 32;

    // The last exceptions raised in the process (first chance, any thread), so a process that a
    // filter ends after an unhandled exception says which one: code, place, thread, and for an
    // access violation the kind (0 read, 1 write, 8 execute) and the address touched.
    struct SeenException
    {
        DWORD Code;
        DWORD Thread;
        void* Address;
        ULONG_PTR Kind;
        ULONG_PTR Touched;
        double Ms;
    };
    constexpr uint32_t SeenExceptions = 8;
    SeenException gSeen[SeenExceptions]{};
    uint64_t gSeenNext{ 0 };
    std::atomic_flag gSeenBusy = ATOMIC_FLAG_INIT;

    LONG CALLBACK WitnessException(EXCEPTION_POINTERS* info)
    {
        auto record = info->ExceptionRecord;
        // a debugger's output and a thread's name are raised as exceptions too: not faults
        if (record->ExceptionCode == 0x40010006 || record->ExceptionCode == 0x4001000A || record->ExceptionCode == 0x406D1388) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // Skip a sample if another callback is publishing one. An exception can interrupt any thread.
        if (gSeenBusy.test_and_set(std::memory_order_acquire)) return EXCEPTION_CONTINUE_SEARCH;
        auto& seen = gSeen[gSeenNext++ % SeenExceptions];
        seen.Code = record->ExceptionCode;
        seen.Thread = GetCurrentThreadId();
        seen.Address = record->ExceptionAddress;
        seen.Kind = record->NumberParameters >= 1 ? record->ExceptionInformation[0] : 0;
        seen.Touched = record->NumberParameters >= 2 ? record->ExceptionInformation[1] : 0;
        seen.Ms = startup_timeline::NowMs();
        gSeenBusy.clear(std::memory_order_release);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void WriteExit(char const* how, UINT code)
    {
        if (gExitFolder.empty() || gExitWritten.exchange(true)) return;
        wchar_t path[MAX_PATH];
        if (_snwprintf_s(path, _TRUNCATE, L"%ls\\exit-%lu.txt", gExitFolder.c_str(), GetCurrentProcessId()) < 0) return;
        auto file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        char line[192];
        DWORD written{ 0 };
        auto put = [&]() { WriteFile(file, line, (DWORD)strlen(line), &written, nullptr); };
        snprintf(line, sizeof(line), "%s %u (0x%08X) at %.1f ms on thread %lu\r\n", how, code, code,
            startup_timeline::NowMs(), GetCurrentThreadId());
        put();
        void* frames[ExitFrames]{};
        auto n = CaptureStackBackTrace(2, ExitFrames, frames, nullptr);
        for (USHORT i = 0; i < n; i++) {
            char where[96];
            NameAddress(frames[i], where, sizeof(where));
            snprintf(line, sizeof(line), "  <- %s\r\n", where);
            put();
        }
        SeenException snapshot[SeenExceptions]{};
        uint64_t raised{ 0 };
        if (!gSeenBusy.test_and_set(std::memory_order_acquire)) {
            memcpy(snapshot, gSeen, sizeof(snapshot));
            raised = gSeenNext;
            gSeenBusy.clear(std::memory_order_release);
        }
        for (auto i = raised > SeenExceptions ? raised - SeenExceptions : 0; i < raised; i++) {
            auto const& seen = snapshot[i % SeenExceptions];
            char where[96];
            NameAddress(seen.Address, where, sizeof(where));
            snprintf(line, sizeof(line), "  exception 0x%08lX at %s on thread %lu at %.1f ms (kind %llu, address 0x%llx)\r\n",
                seen.Code, where, seen.Thread, seen.Ms, (unsigned long long)seen.Kind, (unsigned long long)seen.Touched);
            put();
        }
        auto count = startup_timeline::Count();
        for (auto i = count > ExitTimelineRows ? count - ExitTimelineRows : 0; i < count; i++) {
            if (!startup_timeline::IsReady(i)) continue;
            auto const& e = startup_timeline::Get(i);
            char const* from = "";
            char const* to = "";
            if (e.Kind == startup_timeline::What::State && e.EventSide == startup_timeline::Side::Server) {
                from = EnumInfo<esv::GameState>::Find((esv::GameState)e.From).GetString();
                to = EnumInfo<esv::GameState>::Find((esv::GameState)e.To).GetString();
            } else if (e.Kind == startup_timeline::What::State) {
                from = EnumInfo<ecl::GameState>::Find((ecl::GameState)e.From).GetString();
                to = EnumInfo<ecl::GameState>::Find((ecl::GameState)e.To).GetString();
            }
            snprintf(line, sizeof(line), "  %10.1f ms  side %d  kind %d  %d -> %d  %s%s%s%s\r\n", e.Ms, (int)e.EventSide,
                (int)e.Kind, e.From, e.To, from, *from ? " -> " : "", to, e.Name);
            put();
        }
        FlushFileBuffers(file);
        CloseHandle(file);
    }

    void WINAPI WitnessExitProcess(UINT code)
    {
        WriteExit("ExitProcess", code);
        gRealExitProcess(code);
    }

    BOOL WINAPI WitnessTerminateProcess(HANDLE process, UINT code)
    {
        if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId()) {
            WriteExit("TerminateProcess", code);
        }
        return gRealTerminateProcess(process, code);
    }
}

void InstallExitWitness(bool on, std::wstring const& folder)
{
    if (!on || folder.empty()) return;
    gExitFolder = folder;
    gRealExitProcess = &::ExitProcess;
    gRealTerminateProcess = &::TerminateProcess;
    auto exited = DetourAttach(reinterpret_cast<void**>(&gRealExitProcess), reinterpret_cast<void*>(&WitnessExitProcess));
    auto terminated = DetourAttach(reinterpret_cast<void**>(&gRealTerminateProcess), reinterpret_cast<void*>(&WitnessTerminateProcess));
    AddVectoredExceptionHandler(1, &WitnessException);
    if (exited == NO_ERROR && terminated == NO_ERROR) {
        startup_timeline::RecordStep("ExitWitness on", startup_timeline::StepBegin);
    }
}

TreasureCounters ReadTreasure()
{
    TreasureCounters c{};
    c.Installed = gTreasureInstalled;
    c.Pinned = gTreasurePinned.load();
    c.Seed = gTreasureSeed.load();
    c.Calls = gTreasureCalls.load();
    c.Keyed = gTreasureKeyed.load();
    c.Unkeyed = gTreasureUnkeyed.load();
    return c;
}

END_NS()
