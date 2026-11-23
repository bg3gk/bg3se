// GateKit addition: the load's trace (LoadTrace.h). Included once, by ScriptExtender.cpp.
#include <Extender/Shared/LoadTrace.h>
#include <Extender/Shared/StartupTimeline.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

BEGIN_NS(load_trace)

namespace
{
    std::atomic<bool> gEnabled{ false };
    uint32_t gSampleMs{ 5 };

    double Now()
    {
        return startup_timeline::NowMs();
    }

    void* Allocate(std::size_t bytes)
    {
        return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }

    std::size_t gCommitted{ 0 };

    // ---- the engine's opens (ls::FileReader)
    struct OpenRow
    {
        double Ms;
        float Us;
        uint32_t Thread;
        uint32_t Mapping;
        uint32_t Flags;
        uint64_t Size;
        uint8_t Loaded;
        char Path[163];
    };
    constexpr uint32_t MaxOpens = 200000;
    OpenRow* gOpens{ nullptr };
    std::atomic<uint32_t> gOpenNext{ 0 };
    std::atomic<uint32_t> gOpenDropped{ 0 };

    // ---- the system's files and reads
    constexpr std::size_t PathLength = 200;
    struct PhysicalFile
    {
        char Path[PathLength];
    };
    constexpr uint32_t MaxFiles = 65536;
    PhysicalFile* gFiles{ nullptr };
    uint32_t gFileCount{ 0 };
    SRWLOCK gFileLock = SRWLOCK_INIT;
    std::unordered_map<HANDLE, uint32_t>* gHandles{ nullptr };
    std::unordered_map<std::string, uint32_t>* gPaths{ nullptr };

    struct ReadRow
    {
        double Ms;
        float Us;
        uint32_t File;
        uint32_t Thread;
        uint32_t Bytes;
    };
    constexpr uint32_t MaxReads = 2000000;
    ReadRow* gReads{ nullptr };
    std::atomic<uint32_t> gReadNext{ 0 };
    std::atomic<uint32_t> gReadDropped{ 0 };

    // ---- the states, with the process's counters and its threads' CPU
    struct ThreadTime
    {
        uint32_t Tid;
        uint64_t Cpu;   // 100 ns
    };
    constexpr int MaxSnapThreads = 512;
    struct StateRow
    {
        double Ms;
        int Side;
        int32_t From, To;
        uint64_t IoRead, IoReadOps, Private, WorkingSet, PageFaults;
        double ProcessCpuMs;
        int ThreadCount;
        ThreadTime Threads[MaxSnapThreads];
    };
    constexpr uint32_t MaxStates = 512;
    StateRow* gStates{ nullptr };
    uint32_t gStateCount{ 0 };
    std::mutex gStateLock;

    // ---- the threads' names and roles
    std::mutex gNameLock;
    std::unordered_map<uint32_t, std::string>* gThreadNames{ nullptr };
    std::unordered_map<uint32_t, std::string>* gRoles{ nullptr };
    thread_local char const* tRoles[4]{};
    thread_local int tRoleCount{ 0 };

    using GetThreadDescriptionProc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    GetThreadDescriptionProc gGetThreadDescription{ nullptr };

    // ---- the sampler
    constexpr int MaxFrames = 40;
    struct SampleRow
    {
        double Ms;
        uint32_t Tid;
        uint16_t Depth;
        uint16_t Running;   // 1: the thread used CPU since its last sample
        uint64_t Frames[MaxFrames];
    };
    constexpr uint32_t MaxSamples = 150000;
    SampleRow* gSamples{ nullptr };
    std::atomic<uint32_t> gSampleNext{ 0 };
    std::atomic<uint32_t> gSampleDropped{ 0 };
    DWORD gSamplerTid{ 0 };

    // The modules' unwind tables, the sampler's own copy (no lock while a thread is suspended).
    struct Module
    {
        uintptr_t Base;
        uintptr_t End;
        RUNTIME_FUNCTION const* Functions;
        uint32_t Count;
    };
    constexpr int MaxModules = 512;
    Module gModules[MaxModules];
    int gModuleCount{ 0 };

    // ---- the hooks of the system's files
    using CreateFileWProc = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using CreateFileAProc = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using CreateFile2Proc = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, void*);
    using ReadFileProc = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
    CreateFileWProc gCreateFileW{ nullptr };
    CreateFileAProc gCreateFileA{ nullptr };
    CreateFile2Proc gCreateFile2{ nullptr };
    ReadFileProc gReadFile{ nullptr };

    LARGE_INTEGER gFrequency{};

    double UsSince(LARGE_INTEGER const& from)
    {
        LARGE_INTEGER to;
        QueryPerformanceCounter(&to);
        return (double)(to.QuadPart - from.QuadPart) * 1000000.0 / (double)gFrequency.QuadPart;
    }

    // The path's end, ASCII (anything else as '?'), at most PathLength - 1 characters.
    template <class Char>
    void TailOf(Char const* path, char* out, std::size_t size)
    {
        if (path == nullptr) {
            out[0] = 0;
            return;
        }
        std::size_t n = 0;
        while (path[n] != 0) n++;
        auto keep = size - 1;
        auto from = (n > keep) ? n - keep : 0;
        std::size_t j = 0;
        for (auto i = from; i < n; i++) {
            auto c = (unsigned)path[i];
            out[j++] = (c >= 32 && c < 127) ? (char)c : '?';
        }
        out[j] = 0;
    }

    void Register(HANDLE handle, char const* path)
    {
        if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return;
        AcquireSRWLockExclusive(&gFileLock);
        uint32_t index = 0;
        auto it = gPaths->find(path);
        if (it != gPaths->end()) {
            index = it->second;
        } else if (gFileCount < MaxFiles) {
            index = gFileCount++;
            strncpy_s(gFiles[index].Path, path, _TRUNCATE);
            gPaths->insert(std::make_pair(std::string(path), index));
        }
        (*gHandles)[handle] = index;
        ReleaseSRWLockExclusive(&gFileLock);
    }

    HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
        DWORD flags, HANDLE templ)
    {
        auto handle = gCreateFileW(name, access, share, sa, disposition, flags, templ);
        if (handle != INVALID_HANDLE_VALUE) {
            char path[PathLength];
            TailOf(name, path, sizeof(path));
            Register(handle, path);
        }
        return handle;
    }

    HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
        DWORD flags, HANDLE templ)
    {
        auto handle = gCreateFileA(name, access, share, sa, disposition, flags, templ);
        if (handle != INVALID_HANDLE_VALUE) {
            char path[PathLength];
            TailOf(name, path, sizeof(path));
            Register(handle, path);
        }
        return handle;
    }

    HANDLE WINAPI HookCreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition, void* params)
    {
        auto handle = gCreateFile2(name, access, share, disposition, params);
        if (handle != INVALID_HANDLE_VALUE) {
            char path[PathLength];
            TailOf(name, path, sizeof(path));
            Register(handle, path);
        }
        return handle;
    }

    BOOL WINAPI HookReadFile(HANDLE handle, LPVOID buffer, DWORD bytes, LPDWORD read, LPOVERLAPPED overlapped)
    {
        LARGE_INTEGER began;
        QueryPerformanceCounter(&began);
        auto ms = Now();
        auto result = gReadFile(handle, buffer, bytes, read, overlapped);
        auto us = UsSince(began);

        uint32_t index = 0;
        bool known = false;
        AcquireSRWLockShared(&gFileLock);
        auto it = gHandles->find(handle);
        if (it != gHandles->end()) {
            index = it->second;
            known = true;
        }
        ReleaseSRWLockShared(&gFileLock);
        // Pipes and handles opened before the hooks: not files of the load.
        if (!known) return result;

        auto slot = gReadNext.fetch_add(1);
        if (slot >= MaxReads) {
            gReadNext.store(MaxReads);
            gReadDropped.fetch_add(1);
            return result;
        }
        auto& r = gReads[slot];
        r.Ms = ms;
        r.Us = (float)us;
        r.File = index;
        r.Thread = GetCurrentThreadId();
        r.Bytes = (result && read != nullptr && overlapped == nullptr) ? *read : bytes;
        return result;
    }

    // ---- names of threads
    std::string ThreadNameOf(HANDLE thread)
    {
        if (gGetThreadDescription == nullptr || thread == nullptr) return {};
        PWSTR text{ nullptr };
        std::string name;
        if (SUCCEEDED(gGetThreadDescription(thread, &text)) && text != nullptr) {
            char buf[64];
            TailOf(text, buf, sizeof(buf));
            name = buf;
            LocalFree(text);
        }
        return name;
    }

    void NoteThreadName(uint32_t tid, HANDLE thread)
    {
        {
            std::lock_guard _(gNameLock);
            if (gThreadNames->find(tid) != gThreadNames->end()) return;
        }
        auto name = ThreadNameOf(thread);
        std::lock_guard _(gNameLock);
        (*gThreadNames)[tid] = name;
    }

    // The process's threads (ids) now.
    int ListThreads(uint32_t* out, int max)
    {
        auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return 0;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        auto pid = GetCurrentProcessId();
        int n = 0;
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == pid && n < max) out[n++] = entry.th32ThreadID;
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return n;
    }

    uint64_t ThreadCpu(HANDLE thread)
    {
        FILETIME c{}, e{}, k{}, u{};
        if (!GetThreadTimes(thread, &c, &e, &k, &u)) return 0;
        ULARGE_INTEGER kk{}, uu{};
        kk.LowPart = k.dwLowDateTime; kk.HighPart = k.dwHighDateTime;
        uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
        return kk.QuadPart + uu.QuadPart;
    }

    // ---- the sampler's unwinding (plain reads only, guarded by the caller)
    void RefreshModules()
    {
        HMODULE modules[MaxModules];
        DWORD needed = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
        int n = (int)(needed / sizeof(HMODULE));
        if (n > MaxModules) n = MaxModules;
        int count = 0;
        for (int i = 0; i < n; i++) {
            auto base = reinterpret_cast<uint8_t const*>(modules[i]);
            auto dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
            auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
            auto const& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            auto& m = gModules[count++];
            m.Base = (uintptr_t)base;
            m.End = (uintptr_t)base + nt->OptionalHeader.SizeOfImage;
            m.Functions = reinterpret_cast<RUNTIME_FUNCTION const*>(base + dir.VirtualAddress);
            m.Count = (dir.VirtualAddress != 0) ? dir.Size / sizeof(RUNTIME_FUNCTION) : 0;
        }
        std::sort(gModules, gModules + count, [](Module const& a, Module const& b) { return a.Base < b.Base; });
        gModuleCount = count;
    }

    Module const* FindModule(uintptr_t address)
    {
        int lo = 0, hi = gModuleCount - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            auto const& m = gModules[mid];
            if (address < m.Base) hi = mid - 1;
            else if (address >= m.End) lo = mid + 1;
            else return &m;
        }
        return nullptr;
    }

    RUNTIME_FUNCTION const* FindFunction(Module const& m, uintptr_t address)
    {
        auto rva = (uint32_t)(address - m.Base);
        int lo = 0, hi = (int)m.Count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            auto const& f = m.Functions[mid];
            if (rva < f.BeginAddress) hi = mid - 1;
            else if (rva >= f.EndAddress) lo = mid + 1;
            else return &f;
        }
        return nullptr;
    }

    // The function's own start: a chained entry (a part of a function) followed to its primary one.
    uintptr_t PrimaryStart(Module const& m, RUNTIME_FUNCTION const* f)
    {
        for (int guard = 0; guard < 8; guard++) {
            auto info = reinterpret_cast<uint8_t const*>(m.Base + (f->UnwindData & ~1u));
            auto flags = info[0] >> 3;
            if ((flags & UNW_FLAG_CHAININFO) == 0) break;
            auto codes = (uint32_t)info[2];
            auto chained = reinterpret_cast<RUNTIME_FUNCTION const*>(info + 4 + ((codes + 1) & ~1u) * 2);
            f = chained;
        }
        return m.Base + f->BeginAddress;
    }

    int Unwind(CONTEXT& ctx, uint64_t* frames)
    {
        int depth = 0;
        __try {
            while (depth < MaxFrames && ctx.Rip != 0) {
                auto m = FindModule((uintptr_t)ctx.Rip);
                if (m == nullptr) {
                    frames[depth++] = ctx.Rip | (1ull << 63);   // outside every known module
                    break;
                }
                auto f = FindFunction(*m, (uintptr_t)ctx.Rip);
                if (f == nullptr) {
                    // a leaf: the return address is on top of the stack
                    frames[depth++] = ctx.Rip | (1ull << 63);
                    ctx.Rip = *reinterpret_cast<DWORD64 const*>(ctx.Rsp);
                    ctx.Rsp += 8;
                    continue;
                }
                frames[depth++] = PrimaryStart(*m, f);
                PVOID handlerData{ nullptr };
                DWORD64 establisher{ 0 };
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, (DWORD64)m->Base, ctx.Rip, const_cast<RUNTIME_FUNCTION*>(f), &ctx,
                    &handlerData, &establisher, nullptr);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return depth;
    }

    bool IsRoleThread(uint32_t tid)
    {
        std::lock_guard _(gNameLock);
        return gRoles->find(tid) != gRoles->end();
    }

    struct Watched
    {
        uint32_t Tid;
        HANDLE Handle;
        uint64_t LastCpu;
        bool Role;
    };

    DWORD WINAPI SamplerMain(LPVOID)
    {
        std::vector<Watched> threads;
        double refreshedMs = -1.0e9;
        uint32_t ids[2048];
        CONTEXT ctx;
        for (;;) {
            Sleep(gSampleMs);
            auto now = Now();
            if (now - refreshedMs >= 500.0) {
                refreshedMs = now;
                RefreshModules();
                int n = ListThreads(ids, (int)std::size(ids));
                // gone threads
                for (auto it = threads.begin(); it != threads.end();) {
                    bool alive = std::find(ids, ids + n, it->Tid) != ids + n;
                    if (!alive) {
                        CloseHandle(it->Handle);
                        it = threads.erase(it);
                    } else {
                        ++it;
                    }
                }
                for (int i = 0; i < n; i++) {
                    if (ids[i] == gSamplerTid) continue;
                    bool known = false;
                    for (auto& w : threads) {
                        if (w.Tid == ids[i]) {
                            known = true;
                            w.Role = IsRoleThread(w.Tid);
                            break;
                        }
                    }
                    if (known) continue;
                    auto h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, ids[i]);
                    if (h == nullptr) continue;
                    NoteThreadName(ids[i], h);
                    threads.push_back(Watched{ ids[i], h, ThreadCpu(h), IsRoleThread(ids[i]) });
                }
            }

            for (auto& w : threads) {
                auto cpu = ThreadCpu(w.Handle);
                bool running = cpu != w.LastCpu;
                w.LastCpu = cpu;
                if (!running && !w.Role) continue;

                auto slot = gSampleNext.fetch_add(1);
                if (slot >= MaxSamples) {
                    gSampleNext.store(MaxSamples);
                    gSampleDropped.fetch_add(1);
                    continue;
                }
                auto& s = gSamples[slot];
                s.Ms = Now();
                s.Tid = w.Tid;
                s.Running = running ? 1 : 0;
                s.Depth = 0;
                if (SuspendThread(w.Handle) == (DWORD)-1) continue;
                memset(&ctx, 0, sizeof(ctx));
                ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(w.Handle, &ctx)) {
                    s.Depth = (uint16_t)Unwind(ctx, s.Frames);
                }
                ResumeThread(w.Handle);
            }
        }
        return 0;
    }

    void SnapshotState(StateRow& row)
    {
        IO_COUNTERS io{};
        GetProcessIoCounters(GetCurrentProcess(), &io);
        row.IoRead = io.ReadTransferCount;
        row.IoReadOps = io.ReadOperationCount;
        PROCESS_MEMORY_COUNTERS_EX mem{};
        mem.cb = sizeof(mem);
        K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem));
        row.Private = mem.PrivateUsage;
        row.WorkingSet = mem.WorkingSetSize;
        row.PageFaults = mem.PageFaultCount;
        FILETIME c{}, e{}, k{}, u{};
        row.ProcessCpuMs = 0;
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
            ULARGE_INTEGER kk{}, uu{};
            kk.LowPart = k.dwLowDateTime; kk.HighPart = k.dwHighDateTime;
            uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
            row.ProcessCpuMs = (double)(kk.QuadPart + uu.QuadPart) / 10000.0;
        }
        uint32_t ids[MaxSnapThreads];
        int n = ListThreads(ids, MaxSnapThreads);
        row.ThreadCount = 0;
        for (int i = 0; i < n; i++) {
            auto h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, ids[i]);
            if (h == nullptr) continue;
            NoteThreadName(ids[i], h);
            row.Threads[row.ThreadCount++] = ThreadTime{ ids[i], ThreadCpu(h) };
            CloseHandle(h);
        }
    }

    bool Hook(void** target, void* detour)
    {
        if (*target == nullptr) return false;
        return DetourAttach(target, detour) == NO_ERROR;
    }
}

bool Enabled()
{
    return gEnabled.load(std::memory_order_relaxed);
}

void Install(uint32_t sampleMs)
{
    if (gEnabled.load()) return;
    QueryPerformanceFrequency(&gFrequency);
    gSampleMs = (sampleMs == 0) ? 5 : sampleMs;

    gOpens = static_cast<OpenRow*>(Allocate(sizeof(OpenRow) * MaxOpens));
    gFiles = static_cast<PhysicalFile*>(Allocate(sizeof(PhysicalFile) * MaxFiles));
    gReads = static_cast<ReadRow*>(Allocate(sizeof(ReadRow) * MaxReads));
    gStates = static_cast<StateRow*>(Allocate(sizeof(StateRow) * MaxStates));
    gSamples = static_cast<SampleRow*>(Allocate(sizeof(SampleRow) * MaxSamples));
    gCommitted = sizeof(OpenRow) * MaxOpens + sizeof(PhysicalFile) * MaxFiles + sizeof(ReadRow) * MaxReads
        + sizeof(StateRow) * MaxStates + sizeof(SampleRow) * MaxSamples;
    if (!gOpens || !gFiles || !gReads || !gStates || !gSamples) return;
    gHandles = new std::unordered_map<HANDLE, uint32_t>();
    gPaths = new std::unordered_map<std::string, uint32_t>();
    gThreadNames = new std::unordered_map<uint32_t, std::string>();
    gRoles = new std::unordered_map<uint32_t, std::string>();
    strncpy_s(gFiles[0].Path, "<unnamed>", _TRUNCATE);
    gFileCount = 1;

    auto kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 != nullptr) {
        gGetThreadDescription = reinterpret_cast<GetThreadDescriptionProc>(GetProcAddress(kernel32, "GetThreadDescription"));
    }

    auto kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase != nullptr) {
        gCreateFileW = reinterpret_cast<CreateFileWProc>(GetProcAddress(kernelbase, "CreateFileW"));
        gCreateFileA = reinterpret_cast<CreateFileAProc>(GetProcAddress(kernelbase, "CreateFileA"));
        gCreateFile2 = reinterpret_cast<CreateFile2Proc>(GetProcAddress(kernelbase, "CreateFile2"));
        gReadFile = reinterpret_cast<ReadFileProc>(GetProcAddress(kernelbase, "ReadFile"));
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        Hook(reinterpret_cast<void**>(&gCreateFileW), reinterpret_cast<void*>(&HookCreateFileW));
        Hook(reinterpret_cast<void**>(&gCreateFileA), reinterpret_cast<void*>(&HookCreateFileA));
        Hook(reinterpret_cast<void**>(&gCreateFile2), reinterpret_cast<void*>(&HookCreateFile2));
        Hook(reinterpret_cast<void**>(&gReadFile), reinterpret_cast<void*>(&HookReadFile));
        DetourTransactionCommit();
    }

    gEnabled.store(true);
    startup_timeline::RecordStep("LoadTrace on", startup_timeline::StepBegin);

    auto sampler = CreateThread(nullptr, 0, &SamplerMain, nullptr, 0, &gSamplerTid);
    if (sampler != nullptr) {
        SetThreadPriority(sampler, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(sampler);
    }
}

void Role(char const* role)
{
    if (!Enabled()) return;
    // a thread may have several roles: each is added once
    for (int i = 0; i < tRoleCount; i++) if (tRoles[i] == role) return;
    if (tRoleCount >= (int)std::size(tRoles)) return;
    tRoles[tRoleCount++] = role;
    std::lock_guard _(gNameLock);
    auto& text = (*gRoles)[GetCurrentThreadId()];
    if (!text.empty()) text += ", ";
    text += role;
}

void OnState(int side, int32_t from, int32_t to)
{
    if (!Enabled()) return;
    std::lock_guard _(gStateLock);
    if (gStateCount >= MaxStates) return;
    auto& row = gStates[gStateCount];
    row.Ms = Now();
    row.Side = side;
    row.From = from;
    row.To = to;
    SnapshotState(row);
    gStateCount++;
}

double FileReaderBegin()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart;
}

void FileReaderEnd(double beganTicks, char const* path, std::size_t length, uint32_t mapping, uint32_t flags,
    uint64_t size, bool loaded)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    auto us = ((double)t.QuadPart - beganTicks) * 1000000.0 / (double)gFrequency.QuadPart;
    auto slot = gOpenNext.fetch_add(1);
    if (slot >= MaxOpens) {
        gOpenNext.store(MaxOpens);
        gOpenDropped.fetch_add(1);
        return;
    }
    auto& r = gOpens[slot];
    r.Ms = Now() - us / 1000.0;
    r.Us = (float)us;
    r.Thread = GetCurrentThreadId();
    r.Mapping = mapping;
    r.Flags = flags;
    r.Size = size;
    r.Loaded = loaded ? 1 : 0;
    auto keep = sizeof(r.Path) - 1;
    auto from = (length > keep) ? length - keep : 0;
    auto n = length - from;
    memcpy(r.Path, path + from, n);
    r.Path[n] = 0;
}

namespace
{
    std::atomic<uint8_t*> gServerWorker{ nullptr };
    constexpr std::size_t ServerWorkerTickUsOffset = 0x90;
}

void ServerWorker(void* worker)
{
    if (!Enabled()) return;
    gServerWorker.store(static_cast<uint8_t*>(worker));
}

int64_t ServerTickUs(int64_t setTo)
{
    auto worker = gServerWorker.load();
    if (worker == nullptr) return -1;
    auto period = reinterpret_cast<int64_t volatile*>(worker + ServerWorkerTickUsOffset);
    auto before = *period;
    if (setTo >= 0) *period = setTo;
    return before;
}

namespace
{
    constexpr std::size_t PrepareRunningPollerOffset = 0x20;
    std::atomic<float> gPrepareRunningMax{ -1.0f };
    PollerSeen gPoller{};
    bool gWasPreparing{ false };

    bool ReadPoller(uint8_t* state, float* values, int32_t& quiet)
    {
        __try {
            auto p = state + PrepareRunningPollerOffset;
            values[0] = *reinterpret_cast<float*>(p);
            values[1] = *reinterpret_cast<float*>(p + 4);
            values[2] = *reinterpret_cast<float*>(p + 8);
            quiet = *reinterpret_cast<int32_t*>(p + 0xC);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}

void OnClientUpdate(void* machine, bool prepareRunning)
{
    if (!Enabled() || machine == nullptr) return;
    if (!prepareRunning) {
        gWasPreparing = false;
        return;
    }
    auto state = *reinterpret_cast<uint8_t**>(machine);   // ecl::GameStateMachine::CurrentState
    if (state == nullptr) return;
    float v[3];
    int32_t quiet{ 0 };
    if (!ReadPoller(state, v, quiet)) return;
    // a poller only: the least and most waits are seconds, the least not above the most
    if (!(v[0] >= 0.0f && v[0] <= 600.0f && v[1] >= 0.0f && v[1] <= 600.0f && v[0] <= v[1])) return;
    if (!gWasPreparing) {
        gWasPreparing = true;
        gPoller.FirstMin = v[0];
        gPoller.FirstMax = v[1];
        gPoller.Reads = 0;
        char text[96];
        snprintf(text, sizeof(text), "PrepareRunning poller least %.2f s, most %.2f s", v[0], v[1]);
        startup_timeline::RecordStep(text, startup_timeline::StepBegin);
    }
    gPoller.Min = v[0];
    gPoller.Max = v[1];
    gPoller.Waited = v[2];
    gPoller.Quiet = quiet;
    gPoller.Reads++;
    auto over = gPrepareRunningMax.load();
    if (over >= 0.0f && v[1] != over) {
        __try {
            auto p = state + PrepareRunningPollerOffset;
            if (*reinterpret_cast<float*>(p) > over) *reinterpret_cast<float*>(p) = over;
            *reinterpret_cast<float*>(p + 4) = over;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

float PrepareRunningMaxWait(float seconds, bool set)
{
    auto before = gPrepareRunningMax.load();
    if (set) gPrepareRunningMax.store(seconds);
    return before;
}

PollerSeen LastPoller()
{
    return gPoller;
}

void Reset()
{
    if (!Enabled()) return;
    gOpenNext.store(0);
    gOpenDropped.store(0);
    gReadNext.store(0);
    gReadDropped.store(0);
    gSampleNext.store(0);
    gSampleDropped.store(0);
    std::lock_guard _(gStateLock);
    gStateCount = 0;
}

namespace
{
    char const* ClientStateName(int32_t s)
    {
        static char const* const names[] = { "Unknown", "Init", "InitMenu", "InitNetwork", "InitConnection", "Idle",
            "LoadMenu", "Menu", "Exit", "SwapLevel", "LoadLevel", "LoadModule", "LoadSession", "UnloadLevel",
            "UnloadModule", "UnloadSession", "Paused", "PrepareRunning", "Running", "Disconnect", "Join", "Save",
            "StartLoading", "StopLoading", "StartServer", "Movie", "Installation", "ModReceiving", "Lobby",
            "BuildStory", "GeneratePsoCache", "LoadPsoCache", "AnalyticsSessionEnd" };
        return (s >= 0 && s < (int32_t)std::size(names)) ? names[s] : "?";
    }

    char const* ServerStateName(int32_t s)
    {
        static char const* const names[] = { "Unknown", "Uninitialized", "Init", "Idle", "Exit", "LoadLevel",
            "LoadModule", "LoadSession", "UnloadLevel", "UnloadModule", "UnloadSession", "Sync", "Paused", "Running",
            "Save", "Disconnect", "BuildStory", "ReloadStory" };
        return (s >= 0 && s < (int32_t)std::size(names)) ? names[s] : "?";
    }

    char const* StateName(int side, int32_t s)
    {
        return side == 1 ? ClientStateName(s) : ServerStateName(s);
    }

    // module+0xRVA of an address (a frame), resolved at dump time.
    std::string Describe(uint64_t frame, std::unordered_map<uint64_t, std::string>& cache)
    {
        auto it = cache.find(frame);
        if (it != cache.end()) return it->second;
        bool raw = (frame & (1ull << 63)) != 0;
        auto address = frame & ~(1ull << 63);
        HMODULE module{ nullptr };
        char text[128];
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module) && module != nullptr) {
            wchar_t wpath[MAX_PATH]{};
            GetModuleFileNameW(module, wpath, MAX_PATH);
            auto file = wcsrchr(wpath, L'\\');
            char name[64];
            TailOf(file ? file + 1 : wpath, name, sizeof(name));
            snprintf(text, sizeof(text), "%s%s+0x%llX", raw ? "~" : "", name,
                (unsigned long long)(address - (uint64_t)module));
        } else {
            snprintf(text, sizeof(text), "~0x%llX", (unsigned long long)address);
        }
        cache[frame] = text;
        return text;
    }

    std::string ExtensionOf(char const* path)
    {
        auto slash = strrchr(path, '/');
        auto bslash = strrchr(path, '\\');
        if (bslash > slash) slash = bslash;
        auto file = slash ? slash + 1 : path;
        auto dot = strrchr(file, '.');
        if (dot == nullptr) return "(none)";
        std::string ext(dot);
        for (auto& c : ext) c = (char)tolower((unsigned char)c);
        return ext;
    }

    // A stage: the time between one client state and the next.
    struct Stage
    {
        double Begin, End;
        int32_t From, To;
        int StateIndex;   // the client row in gStates; -1 for the time before the first
    };

    int StageOf(std::vector<Stage> const& stages, double ms)
    {
        int lo = 0, hi = (int)stages.size() - 1, found = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (stages[mid].Begin <= ms) {
                found = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        return found;
    }

    std::string ThreadLabel(uint32_t tid)
    {
        std::lock_guard _(gNameLock);
        std::string label = std::to_string(tid);
        auto role = gRoles->find(tid);
        if (role != gRoles->end()) label += " [" + role->second + "]";
        auto name = gThreadNames->find(tid);
        if (name != gThreadNames->end() && !name->second.empty()) label += " \"" + name->second + "\"";
        return label;
    }

    template <class T>
    std::vector<std::pair<T, double>> Top(std::unordered_map<T, double> const& m, std::size_t n)
    {
        std::vector<std::pair<T, double>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        if (v.size() > n) v.resize(n);
        return v;
    }
}

std::string Dump(char const* path, double sinceMs)
{
    if (!Enabled()) return "LoadTrace is off (ScriptExtenderSettings.json \"LoadTrace\": true)";
    FILE* out{ nullptr };
    if (fopen_s(&out, path, "w") != 0 || out == nullptr) return std::string("cannot write ") + path;

    auto now = Now();
    StateRow* last = static_cast<StateRow*>(Allocate(sizeof(StateRow)));
    if (last == nullptr) {
        fclose(out);
        return "out of memory";
    }
    last->Ms = now;
    last->Side = 0;
    SnapshotState(*last);

    std::vector<StateRow const*> states;
    {
        std::lock_guard _(gStateLock);
        for (uint32_t i = 0; i < gStateCount; i++) states.push_back(&gStates[i]);
    }

    // The stages: client states (side 1), the last one until now.
    std::vector<Stage> stages;
    stages.push_back(Stage{ 0.0, 0.0, -1, -1, -1 });
    for (int i = 0; i < (int)states.size(); i++) {
        if (states[i]->Side != 1) continue;
        stages.back().End = states[i]->Ms;
        stages.push_back(Stage{ states[i]->Ms, now, states[i]->From, states[i]->To, i });
    }
    stages.back().End = now;

    auto opens = std::min(gOpenNext.load(), MaxOpens);
    auto reads = std::min(gReadNext.load(), MaxReads);
    auto samples = std::min(gSampleNext.load(), MaxSamples);

    fprintf(out, "LoadTrace at %.1f ms, since %.1f ms; a sample every %u ms; the tracer's own commit %.1f MB\n",
        now, sinceMs, gSampleMs, gCommitted / 1048576.0);
    fprintf(out, "rows: opens %u (dropped %u), reads %u (dropped %u), samples %u (dropped %u), states %zu, files %u\n\n",
        opens, gOpenDropped.load(), reads, gReadDropped.load(), samples, gSampleDropped.load(), states.size(), gFileCount);

    fprintf(out, "== States (both sides)\nms\tside\tfrom\tto\tprivateMB\twsMB\tioReadMB\tioOps\tprocessCpuMs\tthreads\n");
    for (auto s : states) {
        if (s->Ms < sinceMs) continue;
        fprintf(out, "%.1f\t%s\t%s\t%s\t%.0f\t%.0f\t%.1f\t%llu\t%.0f\t%d\n", s->Ms, s->Side == 1 ? "client" : "server",
            StateName(s->Side, s->From), StateName(s->Side, s->To), s->Private / 1048576.0, s->WorkingSet / 1048576.0,
            s->IoRead / 1048576.0, (unsigned long long)s->IoReadOps, s->ProcessCpuMs, s->ThreadCount);
    }
    fprintf(out, "%.1f\tdump\t\t\t%.0f\t%.0f\t%.1f\t%llu\t%.0f\t%d\n\n", last->Ms, last->Private / 1048576.0,
        last->WorkingSet / 1048576.0, last->IoRead / 1048576.0, (unsigned long long)last->IoReadOps,
        last->ProcessCpuMs, last->ThreadCount);

    // per stage accumulators
    struct ExtStat { uint32_t N{ 0 }; double Bytes{ 0 }; double Us{ 0 }; };
    struct StageAcc
    {
        uint32_t Opens{ 0 };
        double OpenBytes{ 0 }, OpenUs{ 0 };
        std::unordered_map<std::string, ExtStat> ByExt;
        std::unordered_map<std::string, ExtStat> ByFolder;
        std::unordered_map<uint32_t, double> OpenUsByThread;
        uint32_t Reads{ 0 };
        double ReadBytes{ 0 }, ReadUs{ 0 };
        std::unordered_map<uint32_t, double> ReadBytesByFile, ReadUsByFile, ReadOpsByFile;
        std::unordered_map<uint32_t, double> ReadUsByThread, ReadBytesByThread;
        std::unordered_map<uint32_t, double> SamplesByThread;
        std::unordered_map<uint64_t, std::unordered_map<uint64_t, double>> Inclusive, Leaf;   // tid -> frame -> n
        std::unordered_map<uint64_t, std::unordered_map<std::string, double>> Modules;      // tid -> module -> n
        std::unordered_map<std::string, double> Stacks;   // "tid\tframes" -> n
    };
    std::vector<StageAcc> acc(stages.size());

    std::string opensPath = std::string(path) + ".opens.tsv";
    FILE* opensOut{ nullptr };
    fopen_s(&opensOut, opensPath.c_str(), "w");
    if (opensOut) fprintf(opensOut, "ms\tstage\tthread\tus\tsize\tmapping\tloaded\tpath\n");
    for (uint32_t i = 0; i < opens; i++) {
        auto const& r = gOpens[i];
        if (r.Ms < sinceMs) continue;
        int st = StageOf(stages, r.Ms);
        if (st < 0) continue;
        auto& a = acc[st];
        a.Opens++;
        a.OpenBytes += (double)r.Size;
        a.OpenUs += r.Us;
        auto& e = a.ByExt[ExtensionOf(r.Path)];
        e.N++;
        e.Bytes += (double)r.Size;
        e.Us += r.Us;
        a.OpenUsByThread[r.Thread] += r.Us;
        {
            // the folder under Data/ three levels deep (Public/Gustav/Flags), else the path's own folder
            std::string folder;
            auto data = strstr(r.Path, "Data/");
            if (data != nullptr) {
                auto p = data + 5;
                int slashes = 0;
                auto q = p;
                while (*q && slashes < 3) {
                    if (*q == '/') slashes++;
                    q++;
                }
                folder.assign(p, (std::size_t)(q - p));
            } else {
                auto slash = strrchr(r.Path, '/');
                folder = slash ? std::string(r.Path, (std::size_t)(slash - r.Path)) : std::string("(no folder)");
                if (folder.size() > 60) folder = "..." + folder.substr(folder.size() - 57);
            }
            auto& f = a.ByFolder[folder];
            f.N++;
            f.Bytes += (double)r.Size;
            f.Us += r.Us;
        }
        if (opensOut) fprintf(opensOut, "%.1f\t%d\t%u\t%.0f\t%llu\t%u\t%u\t%s\n", r.Ms, st, r.Thread, r.Us,
            (unsigned long long)r.Size, r.Mapping, r.Loaded, r.Path);
    }
    if (opensOut) fclose(opensOut);

    for (uint32_t i = 0; i < reads; i++) {
        auto const& r = gReads[i];
        if (r.Ms < sinceMs) continue;
        int st = StageOf(stages, r.Ms);
        if (st < 0) continue;
        auto& a = acc[st];
        a.Reads++;
        a.ReadBytes += r.Bytes;
        a.ReadUs += r.Us;
        a.ReadBytesByFile[r.File] += r.Bytes;
        a.ReadUsByFile[r.File] += r.Us;
        a.ReadOpsByFile[r.File] += 1;
        a.ReadUsByThread[r.Thread] += r.Us;
        a.ReadBytesByThread[r.Thread] += r.Bytes;
    }

    std::unordered_map<uint64_t, std::string> names;
    for (uint32_t i = 0; i < samples; i++) {
        auto const& s = gSamples[i];
        if (s.Ms < sinceMs || s.Depth == 0) continue;
        int st = StageOf(stages, s.Ms);
        if (st < 0) continue;
        auto& a = acc[st];
        a.SamplesByThread[s.Tid] += 1;
        auto& incl = a.Inclusive[s.Tid];
        for (int d = 0; d < s.Depth; d++) {
            bool seen = false;
            for (int e = 0; e < d && !seen; e++) seen = s.Frames[e] == s.Frames[d];
            if (!seen) incl[s.Frames[d]] += 1;
        }
        a.Leaf[s.Tid][s.Frames[0]] += 1;
        std::string key = std::to_string(s.Tid) + "\t";
        std::vector<std::string> modules;
        for (int d = 0; d < s.Depth; d++) {
            if (d) key += ";";
            auto text = Describe(s.Frames[d], names);
            key += text;
            auto plus = text.find('+');
            auto module = text.substr(text[0] == '~' ? 1 : 0, plus == std::string::npos ? std::string::npos : plus - (text[0] == '~' ? 1 : 0));
            if (std::find(modules.begin(), modules.end(), module) == modules.end()) modules.push_back(module);
        }
        for (auto const& m : modules) a.Modules[s.Tid][m] += 1;
        a.Stacks[key] += 1;
    }

    // thread CPU per stage: the client row that begins it against the next state row (any side) is
    // too fine; the stage's own client rows are used (begin row, next client row or the dump's).
    auto cpuOf = [](StateRow const* row, uint32_t tid) -> int64_t {
        for (int i = 0; i < row->ThreadCount; i++) if (row->Threads[i].Tid == tid) return (int64_t)row->Threads[i].Cpu;
        return -1;
    };

    for (std::size_t st = 0; st < stages.size(); st++) {
        auto const& stage = stages[st];
        if (stage.End < sinceMs) continue;
        auto const& a = acc[st];
        auto dur = stage.End - stage.Begin;
        if (stage.StateIndex < 0) {
            fprintf(out, "== Stage %zu: before the first client state, until %.1f ms\n", st, stage.End);
        } else {
            fprintf(out, "== Stage %zu: client %s -> %s at %.1f ms, lasts %.1f ms\n", st, ClientStateName(stage.From),
                ClientStateName(stage.To), stage.Begin, dur);
        }
        // the server's states inside
        for (auto s : states) {
            if (s->Side == 2 && s->Ms >= stage.Begin && s->Ms < stage.End) {
                fprintf(out, "   server %s -> %s at +%.1f ms\n", ServerStateName(s->From), ServerStateName(s->To),
                    s->Ms - stage.Begin);
            }
        }
        StateRow const* begin = (stage.StateIndex >= 0) ? states[stage.StateIndex] : nullptr;
        StateRow const* end = last;
        for (std::size_t j = (stage.StateIndex >= 0 ? stage.StateIndex + 1 : 0); j < states.size(); j++) {
            if (states[j]->Side == 1) {
                end = states[j];
                break;
            }
        }
        if (begin) {
            fprintf(out, "   memory: private %.0f -> %.0f MB (%+.0f), working set %.0f -> %.0f MB; io read %.1f MB in %llu ops (process counters); process CPU %.0f ms\n",
                begin->Private / 1048576.0, end->Private / 1048576.0, ((double)end->Private - (double)begin->Private) / 1048576.0,
                begin->WorkingSet / 1048576.0, end->WorkingSet / 1048576.0, ((double)end->IoRead - (double)begin->IoRead) / 1048576.0,
                (unsigned long long)(end->IoReadOps - begin->IoReadOps), end->ProcessCpuMs - begin->ProcessCpuMs);
        }
        fprintf(out, "   ls::FileReader opens: %u, %.1f MB, %.1f ms inside the opens\n", a.Opens, a.OpenBytes / 1048576.0, a.OpenUs / 1000.0);
        {
            std::vector<std::pair<std::string, ExtStat>> v(a.ByExt.begin(), a.ByExt.end());
            std::sort(v.begin(), v.end(), [](auto const& x, auto const& y) { return x.second.Us > y.second.Us; });
            int shown = 0;
            for (auto const& [ext, e] : v) {
                if (shown++ >= 18) break;
                fprintf(out, "      %-10s %6u files %9.2f MB %9.1f ms\n", ext.c_str(), e.N, e.Bytes / 1048576.0, e.Us / 1000.0);
            }
            std::vector<std::pair<std::string, ExtStat>> folders(a.ByFolder.begin(), a.ByFolder.end());
            std::sort(folders.begin(), folders.end(), [](auto const& x, auto const& y) { return x.second.Us > y.second.Us; });
            shown = 0;
            for (auto const& [folder, e] : folders) {
                if (shown++ >= 14) break;
                fprintf(out, "      in %-40s %6u files %9.2f MB %9.1f ms\n", folder.c_str(), e.N, e.Bytes / 1048576.0, e.Us / 1000.0);
            }
            for (auto const& [tid, us] : Top(a.OpenUsByThread, 6)) {
                fprintf(out, "      thread %s: %.1f ms in opens\n", ThreadLabel(tid).c_str(), us / 1000.0);
            }
        }
        fprintf(out, "   ReadFile: %u calls, %.1f MB, %.1f ms inside the calls\n", a.Reads, a.ReadBytes / 1048576.0, a.ReadUs / 1000.0);
        for (auto const& [file, bytes] : Top(a.ReadBytesByFile, 12)) {
            auto opsIt = a.ReadOpsByFile.find(file);
            auto usIt = a.ReadUsByFile.find(file);
            fprintf(out, "      %9.2f MB %7.0f ops %8.1f ms  %s\n", bytes / 1048576.0, opsIt->second, usIt->second / 1000.0,
                gFiles[file].Path);
        }
        for (auto const& [tid, us] : Top(a.ReadUsByThread, 6)) {
            auto b = a.ReadBytesByThread.find(tid);
            fprintf(out, "      thread %s: %.1f ms, %.1f MB\n", ThreadLabel(tid).c_str(), us / 1000.0, b->second / 1048576.0);
        }
        if (begin) {
            std::unordered_map<uint32_t, double> cpu;
            for (int i = 0; i < end->ThreadCount; i++) {
                auto tid = end->Threads[i].Tid;
                auto before = cpuOf(begin, tid);
                auto delta = (double)end->Threads[i].Cpu - (before >= 0 ? (double)before : 0.0);
                if (delta > 0) cpu[tid] = delta / 10000.0;
            }
            double total = 0;
            for (auto const& [tid, ms] : cpu) total += ms;
            fprintf(out, "   CPU by thread (state snapshots): %.0f ms in all, %zu threads busy\n", total, cpu.size());
            for (auto const& [tid, ms] : Top(cpu, 14)) {
                fprintf(out, "      %7.0f ms (%3.0f%% of the stage)  %s\n", ms, dur > 0 ? ms * 100.0 / dur : 0.0, ThreadLabel(tid).c_str());
            }
        }
        fprintf(out, "   samples by thread (each ~%u ms):\n", gSampleMs);
        for (auto const& [tid, n] : Top(a.SamplesByThread, 8)) {
            fprintf(out, "    thread %s: %.0f samples\n", ThreadLabel(tid).c_str(), n);
            auto mt = a.Modules.find(tid);
            if (mt != a.Modules.end()) {
                fprintf(out, "      modules on the stack:");
                for (auto const& [module, k] : Top(mt->second, 8)) {
                    fprintf(out, " %s %.0f%%;", module.c_str(), k * 100.0 / n);
                }
                fprintf(out, "\n");
            }
            auto it = a.Inclusive.find(tid);
            if (it != a.Inclusive.end()) {
                fprintf(out, "      inclusive:\n");
                for (auto const& [frame, k] : Top(it->second, 20)) {
                    fprintf(out, "        %5.0f %3.0f%%  %s\n", k, k * 100.0 / n, Describe(frame, names).c_str());
                }
            }
            auto lt = a.Leaf.find(tid);
            if (lt != a.Leaf.end()) {
                fprintf(out, "      leaf:\n");
                for (auto const& [frame, k] : Top(lt->second, 8)) {
                    fprintf(out, "        %5.0f %3.0f%%  %s\n", k, k * 100.0 / n, Describe(frame, names).c_str());
                }
            }
        }
        fprintf(out, "\n");
    }
    fclose(out);

    // the physical reads per stage and file
    std::string readsPath = std::string(path) + ".reads.tsv";
    FILE* readsOut{ nullptr };
    if (fopen_s(&readsOut, readsPath.c_str(), "w") == 0 && readsOut) {
        fprintf(readsOut, "stage\tstate\tMB\tops\tms\tpath\n");
        for (std::size_t st = 0; st < stages.size(); st++) {
            auto const& a = acc[st];
            for (auto const& [file, bytes] : Top(a.ReadBytesByFile, 100000)) {
                fprintf(readsOut, "%zu\t%s\t%.3f\t%.0f\t%.1f\t%s\n", st, ClientStateName(stages[st].To), bytes / 1048576.0,
                    a.ReadOpsByFile.find(file)->second, a.ReadUsByFile.find(file)->second / 1000.0, gFiles[file].Path);
            }
        }
        fclose(readsOut);
    }

    // the sampled stacks per stage, folded (leaf first)
    std::string stacksPath = std::string(path) + ".stacks.tsv";
    FILE* stacksOut{ nullptr };
    if (fopen_s(&stacksOut, stacksPath.c_str(), "w") == 0 && stacksOut) {
        fprintf(stacksOut, "stage\tstate\tsamples\tthread\tframes (leaf first)\n");
        for (std::size_t st = 0; st < stages.size(); st++) {
            for (auto const& [key, n] : Top(acc[st].Stacks, 400)) {
                fprintf(stacksOut, "%zu\t%s\t%.0f\t%s\n", st, ClientStateName(stages[st].To), n, key.c_str());
            }
        }
        fclose(stacksOut);
    }

    VirtualFree(last, 0, MEM_RELEASE);
    char summary[256];
    snprintf(summary, sizeof(summary), "%s: %zu stages, %u opens, %u reads, %u samples", path, stages.size(), opens, reads, samples);
    return summary;
}

END_NS()
