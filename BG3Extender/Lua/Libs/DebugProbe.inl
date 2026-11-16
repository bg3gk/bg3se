// --- Function probes: a record of the calls of any function, and rules that act on a call ------
//
// A probe is a Detours hook on a function's entry whose detour is one of a fixed pool of stubs.
// A stub takes sixteen integer arguments (the four register arguments and twelve stack slots),
// so it can stand in front of any function of at most sixteen parameters; the floating-point
// registers of the first four parameters and of the result are read and set by generated code.
// The stub records the call, applies the probe's rules and calls the function with the same
// values. Nothing of Lua runs on the game's thread: records go to a ring that Lua reads, and
// rules are plain data. Developer mode only.

namespace {

constexpr uint32_t ProbeSlots = 64;
constexpr uint32_t ProbeArgs = 16;
constexpr uint32_t ProbeRing = 4096;
constexpr uint32_t ProbeRules = 4;
constexpr uint32_t ProbeSnaps = 2;
constexpr uint32_t ProbeSnapBytes = 64;
constexpr uint32_t ProbeCallers = 16;
constexpr uint32_t ProbeFrames = 32;

using ProbeFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

enum ProbeRuleKind : uint8_t { ProbeRuleNone = 0, ProbeRuleReturn = 1, ProbeRuleArgument = 2, ProbeRuleSkip = 3 };

struct ProbeRule
{
    uint8_t Kind{ ProbeRuleNone };
    bool Float{ false };            // the value goes to the floating-point register, not the integer one
    uint8_t Argument{ 0 };          // the argument an Argument rule writes, from 0
    int8_t WhenArgument{ -1 };      // -1: no condition on an argument
    uint64_t Value{ 0 };
    uint64_t WhenValue{ 0 };
    uint64_t CallerFrom{ 0 };       // 0, 0: any caller
    uint64_t CallerTo{ 0 };
    volatile LONG64 Remaining{ -1 };// -1: every call; 0: spent
    volatile LONG64 Fired{ 0 };
};

struct ProbeCaller
{
    volatile LONG64 Address{ 0 };
    volatile LONG64 Count{ 0 };
};

struct Probe
{
    volatile LONG Enabled{ 0 };
    uint8_t Owner{ 0 };
    uint64_t Address{ 0 };
    void* Original{ nullptr };      // the trampoline while attached, the function itself otherwise
    uint8_t Args{ 4 };
    bool Record{ true };
    bool Returns{ false };
    uint8_t Chain{ 0 };              // frames of the return chain an entry record keeps; 0: none
    int8_t SnapArg[ProbeSnaps]{ -1, -1 };
    uint8_t SnapSize[ProbeSnaps]{ 0, 0 };
    ProbeRule Rules[ProbeRules];
    volatile LONG64 Hits{ 0 };
    ProbeCaller Callers[ProbeCallers];
    volatile LONG64 OtherCallers{ 0 };
    volatile LONG64 Overhead{ 0 };  // performance-counter ticks spent in the probe itself, the function's own time left out
};

struct ProbeHit
{
    uint64_t Sequence{ 0 };
    uint64_t Time{ 0 };
    uint64_t Caller{ 0 };
    uint64_t Return{ 0 };
    uint64_t Args[ProbeArgs]{};
    uint64_t Floats[4]{};           // XMM0-3 on entry, as raw bits
    uint64_t ReturnFloat{ 0 };      // XMM0 on return, as raw bits
    uint64_t Frames[ProbeFrames]{};
    uint32_t Thread{ 0 };
    uint8_t Slot{ 0 };
    uint8_t Owner{ 0 };
    uint8_t Phase{ 0 };             // 0: entry, 1: return
    uint8_t Depth{ 0 };
    uint8_t ArgCount{ 0 };
    uint8_t FrameCount{ 0 };
    uint8_t Rules{ 0 };             // bit i: rule i acted on this call
    uint8_t SnapLen[ProbeSnaps]{};
    uint8_t Snap[ProbeSnaps][ProbeSnapBytes]{};
};

Probe gProbes[ProbeSlots];
ProbeHit* gProbeRing{ nullptr };
std::atomic<uint64_t> gProbeHead{ 0 };
std::atomic<uint64_t> gProbeRead[3]{};
std::atomic<uint64_t> gProbeSequence{ 0 };
std::atomic_flag gProbeLock = ATOMIC_FLAG_INIT;
thread_local uint32_t tProbeDepth{ 0 };

// Copies bytes that may not be readable: a fault is caught, not asked about beforehand (asking the
// system about a page costs far more than the copy).
bool ProbeGuardedCopy(uint8_t* to, uint8_t const* from, uint32_t size)
{
    __try {
        memcpy(to, from, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ProbeSnapshot(Probe const& p, uint64_t const* a, ProbeHit& hit)
{
    for (uint32_t s = 0; s < ProbeSnaps; s++) {
        hit.SnapLen[s] = 0;
        if (p.SnapArg[s] < 0 || p.SnapSize[s] == 0) continue;
        auto from = (uint8_t const*)a[p.SnapArg[s]];
        if ((uint64_t)from < 0x10000) continue;
        if (ProbeGuardedCopy(hit.Snap[s], from, p.SnapSize[s])) hit.SnapLen[s] = p.SnapSize[s];
    }
}

void ProbePush(ProbeHit const& hit)
{
    while (gProbeLock.test_and_set(std::memory_order_acquire)) {}
    gProbeRing[gProbeHead.load(std::memory_order_relaxed) % ProbeRing] = hit;
    gProbeHead.fetch_add(1, std::memory_order_release);
    gProbeLock.clear(std::memory_order_release);
}

void ProbeCountCaller(Probe& p, uint64_t caller)
{
    for (uint32_t i = 0; i < ProbeCallers; i++) {
        auto seen = (uint64_t)p.Callers[i].Address;
        if (seen == 0) {
            seen = (uint64_t)InterlockedCompareExchange64(&p.Callers[i].Address, (LONG64)caller, 0);
            if (seen == 0) seen = caller;
        }
        if (seen == caller) {
            InterlockedIncrement64(&p.Callers[i].Count);
            return;
        }
    }
    InterlockedIncrement64(&p.OtherCallers);
}

// Whether a rule acts on this call; a rule with a count is spent one call at a time.
bool ProbeRuleActs(ProbeRule& rule, uint64_t const* a, uint64_t caller)
{
    if (rule.Kind == ProbeRuleNone) return false;
    if (rule.WhenArgument >= 0 && a[rule.WhenArgument] != rule.WhenValue) return false;
    if (rule.CallerTo != 0 && (caller < rule.CallerFrom || caller >= rule.CallerTo)) return false;
    for (;;) {
        LONG64 left = rule.Remaining;
        if (left < 0) break;
        if (left == 0) return false;
        if (InterlockedCompareExchange64(&rule.Remaining, left - 1, left) == left) break;
    }
    InterlockedIncrement64(&rule.Fired);
    return true;
}

// The floating-point side of a call. The first four parameters of a function travel in RCX, RDX,
// R8, R9 when they are integers and in XMM0-3 when they are floating-point, and a floating-point
// result comes back in XMM0. C++ cannot name those registers, so a page of generated code does:
// four readers and a writer of the registers, and for every slot a thunk that loads XMM0-3 from
// four thread-local values and jumps to the probed function, so that it receives the registers
// exactly as its caller left them (or as a rule changed them).
uint8_t* gProbeCode{ nullptr };
DWORD gProbeTls[4]{ TLS_OUT_OF_INDEXES, TLS_OUT_OF_INDEXES, TLS_OUT_OF_INDEXES, TLS_OUT_OF_INDEXES };
using XmmReader = uint64_t (*)();
using XmmWriter = void (*)(uint64_t);
XmmReader gReadXmm[4]{};
XmmWriter gWriteXmm0{ nullptr };
using XmmAllReader = void (*)(uint64_t*);
XmmAllReader gReadXmmAll{ nullptr };
void* gProbeThunks[ProbeSlots]{};
bool gProbeFloats{ false };

void ProbeBuildCode()
{
    if (gProbeCode != nullptr) return;
    for (auto& index : gProbeTls) {
        index = TlsAlloc();
        if (index == TLS_OUT_OF_INDEXES || index >= 64 + 1024) return;
    }
    gProbeCode = (uint8_t*)VirtualAlloc(nullptr, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (gProbeCode == nullptr) return;

    uint8_t* at = gProbeCode;
    static const uint8_t modrmRead[4] = { 0xC0, 0xC8, 0xD0, 0xD8 };
    for (int i = 0; i < 4; i++) {
        gReadXmm[i] = (XmmReader)at;
        const uint8_t code[] = { 0x66, 0x48, 0x0F, 0x7E, modrmRead[i], 0xC3 };          // movq rax, xmmN; ret
        memcpy(at, code, sizeof(code));
        at += 8;
    }
    {
        gReadXmmAll = (XmmAllReader)at;
        const uint8_t code[] = { 0x66, 0x0F, 0xD6, 0x01,                                  // movq [rcx], xmm0
                                 0x66, 0x0F, 0xD6, 0x49, 0x08,                            // movq [rcx+8], xmm1
                                 0x66, 0x0F, 0xD6, 0x51, 0x10,                            // movq [rcx+16], xmm2
                                 0x66, 0x0F, 0xD6, 0x59, 0x18,                            // movq [rcx+24], xmm3
                                 0xC3 };
        memcpy(at, code, sizeof(code));
        at += 24;
    }
    {
        gWriteXmm0 = (XmmWriter)at;
        const uint8_t code[] = { 0x66, 0x48, 0x0F, 0x6E, 0xC1, 0xC3 };                    // movq xmm0, rcx; ret
        memcpy(at, code, sizeof(code));
        at += 8;
    }

    static const uint8_t modrmLoad[4] = { 0xC2, 0xCA, 0xD2, 0xDA };
    at = gProbeCode + 0x100;
    for (uint32_t slot = 0; slot < ProbeSlots; slot++) {
        gProbeThunks[slot] = at;
        uint8_t* w = at;
        for (int i = 0; i < 4; i++) {
            // A thread's first 64 values lie in its environment block at 0x1480; the further ones
            // in a block of their own, pointed to from 0x1780 (made when the thread first sets one)
            bool inner = gProbeTls[i] < 64;
            uint32_t offset = inner ? 0x1480 + gProbeTls[i] * 8 : 0x1780;
            const uint8_t load[] = { 0x65, 0x4C, 0x8B, 0x14, 0x25 };                      // mov r10, gs:[offset]
            memcpy(w, load, sizeof(load)); w += sizeof(load);
            memcpy(w, &offset, 4); w += 4;
            if (!inner) {
                uint32_t further = (gProbeTls[i] - 64) * 8;
                const uint8_t deref[] = { 0x4D, 0x8B, 0x92 };                             // mov r10, [r10+further]
                memcpy(w, deref, sizeof(deref)); w += sizeof(deref);
                memcpy(w, &further, 4); w += 4;
            }
            const uint8_t move[] = { 0x66, 0x49, 0x0F, 0x6E, modrmLoad[i] };              // movq xmmN, r10
            memcpy(w, move, sizeof(move)); w += sizeof(move);
        }
        uint64_t original = (uint64_t)&gProbes[slot].Original;
        *w++ = 0x49; *w++ = 0xBA;                                                         // mov r10, &Original
        memcpy(w, &original, 8); w += 8;
        *w++ = 0x41; *w++ = 0xFF; *w++ = 0x22;                                            // jmp [r10]
        at += 0x80;
    }
    FlushInstructionCache(GetCurrentProcess(), gProbeCode, 0x3000);
    gProbeFloats = true;
}

__declspec(noinline) uint64_t ProbeDispatch(uint32_t slot, void* returnAddress, uint64_t* a, uint64_t* x)
{
    Probe& p = gProbes[slot];
    // Through the slot's thunk the function gets XMM0-3 back; without the generated code it is
    // called directly and must take no floating-point parameter
    auto original = (ProbeFn)(gProbeFloats ? gProbeThunks[slot] : p.Original);
    auto pass = [&]() {
        if (gProbeFloats) {
            for (int i = 0; i < 4; i++) TlsSetValue(gProbeTls[i], (LPVOID)x[i]);
        }
    };
    if (!p.Enabled || gProbeRing == nullptr) {
        pass();
        return original(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
    }

    LARGE_INTEGER began;
    QueryPerformanceCounter(&began);
    auto caller = (uint64_t)returnAddress;
    InterlockedIncrement64(&p.Hits);
    ProbeCountCaller(p, caller);

    bool skip = false, setReturn = false, setReturnFloat = false;
    uint64_t skipValue = 0, returnValue = 0;
    uint8_t acted = 0;
    for (uint32_t r = 0; r < ProbeRules; r++) {
        auto& rule = p.Rules[r];
        if (!ProbeRuleActs(rule, a, caller)) continue;
        acted |= (uint8_t)(1u << r);
        switch (rule.Kind) {
        case ProbeRuleArgument:
            if (rule.Float && rule.Argument < 4) x[rule.Argument] = rule.Value; else a[rule.Argument] = rule.Value;
            break;
        case ProbeRuleReturn: setReturn = true; setReturnFloat = rule.Float; returnValue = rule.Value; break;
        case ProbeRuleSkip: skip = true; setReturnFloat = rule.Float; skipValue = rule.Value; break;
        default: break;
        }
    }

    ProbeHit hit;
    bool record = p.Record;
    if (record) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        hit.Sequence = gProbeSequence.fetch_add(1) + 1;
        hit.Time = (uint64_t)now.QuadPart;
        hit.Caller = caller;
        hit.Thread = GetCurrentThreadId();
        hit.Slot = (uint8_t)slot;
        hit.Owner = p.Owner;
        hit.Phase = 0;
        hit.Depth = (uint8_t)tProbeDepth;
        hit.ArgCount = p.Args;
        hit.Rules = acted;
        for (uint32_t i = 0; i < p.Args; i++) hit.Args[i] = a[i];
        for (int i = 0; i < 4; i++) hit.Floats[i] = x[i];
        if (p.Chain > 0) {
            void* frames[ProbeFrames + 2];
            auto n = RtlCaptureStackBackTrace(2, p.Chain, frames, nullptr);
            hit.FrameCount = (uint8_t)n;
            for (uint32_t i = 0; i < n; i++) hit.Frames[i] = (uint64_t)frames[i];
        }
        ProbeSnapshot(p, a, hit);
        ProbePush(hit);
    }

    LARGE_INTEGER before, after, done;
    QueryPerformanceCounter(&before);
    tProbeDepth++;
    uint64_t result = 0, floatResult = 0;
    if (skip) {
        if (setReturnFloat) floatResult = skipValue; else result = skipValue;
    } else {
        pass();
        result = original(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
        if (gProbeFloats) floatResult = gReadXmm[0]();     // XMM0 as the function left it, read before anything else runs
    }
    tProbeDepth--;
    QueryPerformanceCounter(&after);
    if (setReturn) {
        if (setReturnFloat) floatResult = returnValue; else result = returnValue;
    }

    if (record && p.Returns) {
        hit.Phase = 1;
        hit.Return = result;
        hit.ReturnFloat = floatResult;
        hit.FrameCount = 0;
        ProbeSnapshot(p, a, hit);
        ProbePush(hit);
    }
    QueryPerformanceCounter(&done);
    InterlockedAdd64(&p.Overhead, (before.QuadPart - began.QuadPart) + (done.QuadPart - after.QuadPart));
    // The last thing before the return: the caller of a function that returns a floating-point
    // value reads XMM0
    if (gProbeFloats) gWriteXmm0(floatResult);
    return result;
}

template <uint32_t I>
__declspec(noinline) uint64_t ProbeStub(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
    uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12, uint64_t a13,
    uint64_t a14, uint64_t a15)
{
    // XMM0-3 first, in one call, before any code of this function can use them: even setting an
    // array to zero is done through XMM0, so the array is left unset until the registers are read
    uint64_t x[4];
    if (gProbeFloats) {
        gReadXmmAll(x);
    } else {
        x[0] = x[1] = x[2] = x[3] = 0;
    }
    uint64_t a[ProbeArgs] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15 };
    return ProbeDispatch(I, _ReturnAddress(), a, x);
}

template <std::size_t... I>
std::array<void*, sizeof...(I)> MakeProbeStubs(std::index_sequence<I...>)
{
    return { { (void*)&ProbeStub<(uint32_t)I>... } };
}

std::array<void*, ProbeSlots> gProbeStubs = MakeProbeStubs(std::make_index_sequence<ProbeSlots>{});

// Attach or detach one probe's detour with every other thread enlisted, so that Detours moves a
// thread that stands in the bytes being rewritten.
LONG ProbePatch(Probe& p, uint32_t slot, bool attach)
{
    std::vector<HANDLE> threads;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
        if (Thread32First(snap, &entry)) {
            do {
                if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, entry.th32ThreadID);
                if (thread == nullptr) continue;
                DetourUpdateThread(thread);
                threads.push_back(thread);
            } while (Thread32Next(snap, &entry));
        }
        CloseHandle(snap);
    }
    LONG error = attach ? DetourAttach(&p.Original, gProbeStubs[slot]) : DetourDetach(&p.Original, gProbeStubs[slot]);
    if (error == NO_ERROR) {
        error = DetourTransactionCommit();
    } else {
        DetourTransactionAbort();
    }
    for (auto thread : threads) CloseHandle(thread);
    return error;
}

void PushCodeAddress(lua_State* L, uint64_t addr)
{
    HMODULE module{ nullptr };
    lua_newtable(L);
    settable(L, "Address", (int64_t)addr);
    if (addr != 0 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)addr, &module) && module != nullptr) {
        wchar_t path[MAX_PATH];
        DWORD len = GetModuleFileNameW(module, path, (DWORD)std::size(path));
        STDString name;
        if (len > 0) {
            std::wstring_view view(path, len);
            auto slash = view.find_last_of(L"\\/");
            name = ToStdUTF8(slash == std::wstring_view::npos ? view : view.substr(slash + 1)).c_str();
        }
        settable(L, "Module", name);
        settable(L, "RVA", (int64_t)((uint8_t const*)addr - (uint8_t const*)module));
    }
}

bool FieldInteger(lua_State* L, int table, char const* name, int64_t& out)
{
    lua_getfield(L, table, name);
    bool has = lua_isinteger(L, -1) || lua_isnumber(L, -1);
    if (has) out = (int64_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return has;
}

bool FieldBool(lua_State* L, int table, char const* name, bool fallback)
{
    lua_getfield(L, table, name);
    bool value = lua_isnil(L, -1) ? fallback : (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    return value;
}

char const* ProbeRuleName(uint8_t kind)
{
    switch (kind) {
    case ProbeRuleReturn: return "Return";
    case ProbeRuleArgument: return "Argument";
    case ProbeRuleSkip: return "Skip";
    default: return "None";
    }
}

} // anonymous namespace

// A function of a known shape, for checking the probes themselves: a double in XMM0, a float in
// XMM1, an integer in R8, a double in XMM3, an integer and a double on the stack, a double back.
__declspec(noinline) double ProbeTestTarget(double a, float b, int64_t c, double d, int64_t e, double f)
{
    return a * (double)b + (double)c + d + (double)e + f;
}

// Ext.Debug.GetProbeTestAddress() -> the address of the self-test function, to put a probe on.
int64_t GetProbeTestAddress(lua_State* L)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("GetProbeTestAddress() is only available in developer mode");
        return 0;
    }
    return (int64_t)&ProbeTestTarget;
}

// Ext.Debug.CallProbeTest(a, b, c, d, e, f) -> a * b + c + d + e + f, computed by the self-test
// function called as any function of the game is called.
double CallProbeTest(lua_State* L, double a, double b, int64_t c, double d, int64_t e, double f)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("CallProbeTest() is only available in developer mode");
        return 0;
    }
    double (*volatile target)(double, float, int64_t, double, int64_t, double) = &ProbeTestTarget;
    return target(a, (float)b, c, d, e, f);
}

// Ext.Debug.AddProbe(address, [options]) -> slot (1-64), or nil and why. Puts a probe on the
// function that begins at `address`. options: Args (how many integer arguments a record keeps, 1-16,
// default 4), Record (default true; false counts calls and callers only), Return (also record the
// return value, default false), Chain (a short return chain in the entry record, default false),
// Snapshots = { { Argument = n, Size = bytes } } (at most two: bytes read through an argument that
// is a pointer, on entry and again on return; Size at most 64).
UserReturn AddProbe(lua_State* L, int64_t address)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("AddProbe() is only available in developer mode");
        return 0;
    }
    WARN_ONCE("Function probes are a development tool for studying the game's code; they should not be used in production!");
    auto owner = OwnerOf(L);
    if (address == 0 || !IsReadableRange((uint8_t const*)address, 16)) {
        lua_pushnil(L);
        lua_pushstring(L, "the address is not readable code");
        return 2;
    }

    std::lock_guard _(gInstrumentMutex);
    if (gProbeRing == nullptr) gProbeRing = new ProbeHit[ProbeRing];
    ProbeBuildCode();

    int slot = -1;
    for (uint32_t i = 0; i < ProbeSlots; i++) {
        if (gProbes[i].Address == (uint64_t)address && gProbes[i].Enabled) {
            lua_pushnil(L);
            lua_pushstring(L, "a probe is on this address already");
            return 2;
        }
        if (slot < 0 && !gProbes[i].Enabled && gProbes[i].Address == 0) slot = (int)i;
    }
    if (slot < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no free probe slot");
        return 2;
    }

    Probe& p = gProbes[slot];
    p.Owner = owner;
    p.Args = 4;
    p.Record = true;
    p.Returns = false;
    p.Chain = 0;
    for (uint32_t s = 0; s < ProbeSnaps; s++) { p.SnapArg[s] = -1; p.SnapSize[s] = 0; }
    for (auto& rule : p.Rules) { rule.Kind = ProbeRuleNone; rule.Fired = 0; rule.Remaining = -1; }
    for (auto& c : p.Callers) { c.Address = 0; c.Count = 0; }
    p.Hits = 0;
    p.OtherCallers = 0;
    p.Overhead = 0;

    if (lua_istable(L, 2)) {
        int64_t n;
        if (FieldInteger(L, 2, "Args", n)) p.Args = (uint8_t)std::clamp<int64_t>(n, 1, ProbeArgs);
        p.Record = FieldBool(L, 2, "Record", true);
        p.Returns = FieldBool(L, 2, "Return", false);
        if (FieldInteger(L, 2, "Chain", n)) p.Chain = (uint8_t)std::clamp<int64_t>(n, 0, ProbeFrames);
        else if (FieldBool(L, 2, "Chain", false)) p.Chain = 6;
        lua_getfield(L, 2, "Snapshots");
        if (lua_istable(L, -1)) {
            for (uint32_t s = 0; s < ProbeSnaps; s++) {
                lua_rawgeti(L, -1, (int)s + 1);
                if (lua_istable(L, -1)) {
                    int64_t arg = 0, size = 0;
                    int item = lua_gettop(L);
                    if (FieldInteger(L, item, "Argument", arg) && FieldInteger(L, item, "Size", size)
                        && arg >= 1 && arg <= ProbeArgs && size >= 1 && size <= ProbeSnapBytes) {
                        p.SnapArg[s] = (int8_t)(arg - 1);
                        p.SnapSize[s] = (uint8_t)size;
                    }
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    p.Address = (uint64_t)address;
    p.Original = (void*)address;
    InterlockedExchange(&p.Enabled, 1);
    LONG error = ProbePatch(p, (uint32_t)slot, true);
    if (error != NO_ERROR) {
        InterlockedExchange(&p.Enabled, 0);
        p.Address = 0;
        p.Original = nullptr;
        lua_pushnil(L);
        lua_pushfstring(L, "the function could not be hooked (Detours error %d)", (int)error);
        return 2;
    }

    lua_pushinteger(L, slot + 1);
    return 1;
}

// Ext.Debug.SetProbeRule(slot, index, [rule]) -> true. Sets rule `index` (1-4) of a probe of the
// calling side, or clears it without a rule. rule: Kind ("Return": the call returns Value;
// "Argument": argument number Argument is Value on entry; "Skip": the function is not called and
// the call returns Value), Value, Argument, Count (how many calls it acts on; omitted: every
// call), WhenArgument and WhenValue (only calls whose argument number WhenArgument equals
// WhenValue), CallerFrom and CallerTo (only calls whose return address is in [CallerFrom, CallerTo)).
bool SetProbeRule(lua_State* L, int32_t slot, int32_t index)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("SetProbeRule() is only available in developer mode");
        return 0;
    }
    auto owner = OwnerOf(L);
    if (slot < 1 || slot > (int)ProbeSlots || index < 1 || index > (int)ProbeRules) {
        luaL_error(L, "SetProbeRule: slot is 1-%d and index 1-%d", (int)ProbeSlots, (int)ProbeRules);
        return false;
    }

    std::lock_guard _(gInstrumentMutex);
    Probe& p = gProbes[slot - 1];
    if (!p.Enabled || p.Owner != owner) {
        luaL_error(L, "SetProbeRule: no probe of this side in slot %d", slot);
        return false;
    }

    ProbeRule& rule = p.Rules[index - 1];
    rule.Kind = ProbeRuleNone;                  // off while it is rewritten
    if (!lua_istable(L, 3)) return true;

    lua_getfield(L, 3, "Kind");
    STDString kind = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    uint8_t parsed = kind == "Return" ? ProbeRuleReturn : kind == "Argument" ? ProbeRuleArgument
        : kind == "Skip" ? ProbeRuleSkip : ProbeRuleNone;
    if (parsed == ProbeRuleNone) {
        luaL_error(L, "SetProbeRule: Kind is Return, Argument or Skip");
        return false;
    }

    int64_t value = 0, argument = 1, count = -1, whenArgument = 0, whenValue = 0, from = 0, to = 0;
    FieldInteger(L, 3, "Value", value);
    FieldInteger(L, 3, "Argument", argument);
    FieldInteger(L, 3, "Count", count);
    bool when = FieldInteger(L, 3, "WhenArgument", whenArgument);
    FieldInteger(L, 3, "WhenValue", whenValue);
    FieldInteger(L, 3, "CallerFrom", from);
    FieldInteger(L, 3, "CallerTo", to);
    // FloatValue (a number) or Float32Value: the value is floating-point and goes to XMM
    rule.Float = false;
    lua_getfield(L, 3, "FloatValue");
    if (lua_isnumber(L, -1)) { double d = lua_tonumber(L, -1); memcpy(&value, &d, 8); rule.Float = true; }
    lua_pop(L, 1);
    lua_getfield(L, 3, "Float32Value");
    if (lua_isnumber(L, -1)) { float f = (float)lua_tonumber(L, -1); value = 0; memcpy(&value, &f, 4); rule.Float = true; }
    lua_pop(L, 1);
    if (argument < 1 || argument > ProbeArgs || (when && (whenArgument < 1 || whenArgument > ProbeArgs))) {
        luaL_error(L, "SetProbeRule: an argument's number is 1-%d", (int)ProbeArgs);
        return false;
    }

    rule.Value = (uint64_t)value;
    rule.Argument = (uint8_t)(argument - 1);
    rule.WhenArgument = when ? (int8_t)(whenArgument - 1) : (int8_t)-1;
    rule.WhenValue = (uint64_t)whenValue;
    rule.CallerFrom = (uint64_t)from;
    rule.CallerTo = (uint64_t)to;
    rule.Fired = 0;
    rule.Remaining = count < 0 ? -1 : count;
    rule.Kind = parsed;
    return true;
}

// Ext.Debug.GetProbeHits([max]) -> the records of the calling side's probes since the last read,
// oldest first: { Slot, Sequence, Phase ("Entry" or "Return"), Depth, Thread, Time, Caller,
// Args = { ... }, Return, Rules = { index, ... }, Chain = { ... }, Snapshots = { hex or false } }.
// A return record repeats its entry's Sequence.
UserReturn GetProbeHits(lua_State* L, std::optional<int32_t> max)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("GetProbeHits() is only available in developer mode");
        return 0;
    }
    auto owner = OwnerOf(L);
    lua_newtable(L);
    if (gProbeRing == nullptr) return 1;

    uint64_t head = gProbeHead.load();
    uint64_t read = gProbeRead[owner].load();
    if (head - read > ProbeRing) read = head - ProbeRing;
    uint64_t limit = (uint64_t)max.value_or(0x7fffffff);

    static const char hex[] = "0123456789ABCDEF";
    int index = 1;
    uint64_t n = read;
    auto hit = std::make_unique<ProbeHit>();
    for (; n < head && (uint64_t)(index - 1) < limit; n++) {
        while (gProbeLock.test_and_set(std::memory_order_acquire)) {}
        *hit = gProbeRing[n % ProbeRing];
        gProbeLock.clear(std::memory_order_release);
        if (hit->Owner != owner) continue;

        lua_newtable(L);
        settable(L, "Slot", (int32_t)(hit->Slot + 1));
        settable(L, "Sequence", (int64_t)hit->Sequence);
        settable(L, "Phase", hit->Phase == 0 ? "Entry" : "Return");
        settable(L, "Depth", (int32_t)hit->Depth);
        settable(L, "Thread", (int32_t)hit->Thread);
        settable(L, "Time", (int64_t)hit->Time);
        if (hit->Phase == 1) {
            settable(L, "Return", (int64_t)hit->Return);
            double d; float f;
            memcpy(&d, &hit->ReturnFloat, 8);
            memcpy(&f, &hit->ReturnFloat, 4);
            settable(L, "ReturnDouble", d);
            settable(L, "ReturnFloat", (double)f);
        }

        // XMM0-3 on entry, each read both ways: a parameter is one or the other, or neither
        lua_pushstring(L, "Doubles");
        lua_newtable(L);
        for (int i = 0; i < 4; i++) {
            double d;
            memcpy(&d, &hit->Floats[i], 8);
            lua_pushnumber(L, d);
            lua_rawseti(L, -2, i + 1);
        }
        lua_rawset(L, -3);
        lua_pushstring(L, "Floats");
        lua_newtable(L);
        for (int i = 0; i < 4; i++) {
            float f;
            memcpy(&f, &hit->Floats[i], 4);
            lua_pushnumber(L, (double)f);
            lua_rawseti(L, -2, i + 1);
        }
        lua_rawset(L, -3);

        lua_pushstring(L, "Caller");
        PushCodeAddress(L, hit->Caller);
        lua_rawset(L, -3);

        lua_pushstring(L, "Args");
        lua_newtable(L);
        for (uint32_t i = 0; i < hit->ArgCount; i++) {
            lua_pushinteger(L, (lua_Integer)hit->Args[i]);
            lua_rawseti(L, -2, (int)i + 1);
        }
        lua_rawset(L, -3);

        lua_pushstring(L, "Rules");
        lua_newtable(L);
        int acted = 1;
        for (uint32_t r = 0; r < ProbeRules; r++) {
            if (hit->Rules & (1u << r)) {
                lua_pushinteger(L, (lua_Integer)r + 1);
                lua_rawseti(L, -2, acted++);
            }
        }
        lua_rawset(L, -3);

        lua_pushstring(L, "Chain");
        lua_newtable(L);
        for (uint32_t f = 0; f < hit->FrameCount; f++) {
            PushCodeAddress(L, hit->Frames[f]);
            lua_rawseti(L, -2, (int)f + 1);
        }
        lua_rawset(L, -3);

        lua_pushstring(L, "Snapshots");
        lua_newtable(L);
        for (uint32_t s = 0; s < ProbeSnaps; s++) {
            if (hit->SnapLen[s] == 0) {
                lua_pushboolean(L, 0);
            } else {
                char text[ProbeSnapBytes * 2];
                for (uint32_t b = 0; b < hit->SnapLen[s]; b++) {
                    text[b * 2] = hex[hit->Snap[s][b] >> 4];
                    text[b * 2 + 1] = hex[hit->Snap[s][b] & 15];
                }
                lua_pushlstring(L, text, (size_t)hit->SnapLen[s] * 2);
            }
            lua_rawseti(L, -2, (int)s + 1);
        }
        lua_rawset(L, -3);

        lua_rawseti(L, -2, index++);
    }

    gProbeRead[owner].store(n);
    return 1;
}

// Ext.Debug.GetProbes([all]) -> { { Slot, Owner, Address (a code address table), Hits, Args, Record,
// Return, Chain, Callers = { { Caller, Count } }, OtherCallers, Rules = { { Index, Kind, Value,
// Remaining, Fired } } } } for the calling side's probes, or both sides' with all = true.
UserReturn GetProbes(lua_State* L, std::optional<bool> all)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("GetProbes() is only available in developer mode");
        return 0;
    }
    auto owner = OwnerOf(L);
    std::lock_guard _(gInstrumentMutex);
    lua_newtable(L);
    int index = 1;
    for (uint32_t i = 0; i < ProbeSlots; i++) {
        auto& p = gProbes[i];
        if (!p.Enabled || (!all.value_or(false) && p.Owner != owner)) continue;
        lua_newtable(L);
        settable(L, "Slot", (int32_t)(i + 1));
        settable(L, "Owner", OwnerName(p.Owner));
        settable(L, "Hits", (int64_t)p.Hits);
        settable(L, "Args", (int32_t)p.Args);
        settable(L, "Record", p.Record);
        settable(L, "Return", p.Returns);
        settable(L, "Chain", (int32_t)p.Chain);
        settable(L, "OverheadTicks", (int64_t)p.Overhead);
        settable(L, "Floats", gProbeFloats);
        settable(L, "OtherCallers", (int64_t)p.OtherCallers);

        lua_pushstring(L, "Address");
        PushCodeAddress(L, p.Address);
        lua_rawset(L, -3);

        lua_pushstring(L, "Callers");
        lua_newtable(L);
        int c = 1;
        for (auto& caller : p.Callers) {
            if (caller.Address == 0) continue;
            lua_newtable(L);
            settable(L, "Count", (int64_t)caller.Count);
            lua_pushstring(L, "Caller");
            PushCodeAddress(L, (uint64_t)caller.Address);
            lua_rawset(L, -3);
            lua_rawseti(L, -2, c++);
        }
        lua_rawset(L, -3);

        lua_pushstring(L, "Rules");
        lua_newtable(L);
        int r = 1;
        for (uint32_t k = 0; k < ProbeRules; k++) {
            auto& rule = p.Rules[k];
            if (rule.Kind == ProbeRuleNone) continue;
            lua_newtable(L);
            settable(L, "Index", (int32_t)(k + 1));
            settable(L, "Kind", ProbeRuleName(rule.Kind));
            settable(L, "Value", (int64_t)rule.Value);
            settable(L, "Remaining", (int64_t)rule.Remaining);
            settable(L, "Fired", (int64_t)rule.Fired);
            lua_rawseti(L, -2, r++);
        }
        lua_rawset(L, -3);

        lua_rawseti(L, -2, index++);
    }
    return 1;
}

// Ext.Debug.RemoveProbe([slot]) -> how many probes were removed. Without a slot, removes every
// probe of the calling side; the other side's probes are left alone.
int32_t RemoveProbe(lua_State* L, std::optional<int32_t> slot)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("RemoveProbe() is only available in developer mode");
        return 0;
    }
    auto owner = OwnerOf(L);
    if (slot && (*slot < 1 || *slot > (int)ProbeSlots)) {
        luaL_error(L, "RemoveProbe: slot is 1-%d", (int)ProbeSlots);
        return 0;
    }

    std::lock_guard _(gInstrumentMutex);
    int32_t cleared = 0;
    for (uint32_t i = 0; i < ProbeSlots; i++) {
        if (slot && (uint32_t)(*slot - 1) != i) continue;
        Probe& p = gProbes[i];
        if (!p.Enabled || p.Owner != owner) continue;
        InterlockedExchange(&p.Enabled, 0);
        for (auto& rule : p.Rules) rule.Kind = ProbeRuleNone;
        if (ProbePatch(p, i, false) == NO_ERROR) {
            p.Address = 0;                      // the slot is free again
            cleared++;
        }
    }
    return cleared;
}

// --- Addresses of what Lua already holds ---------------------------------------------------------

// Ext.Debug.GetObjectAddress(object) -> address, kind; or nil and why. The address of the game's
// object behind a Lua reference to it (an object, an array, a map or a set), so that a pointer a
// probe or a watch reports can be told to be this object. kind: "Object", "Array", "Map", "Set".
UserReturn GetObjectAddress(lua_State* L)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("GetObjectAddress() is only available in developer mode");
        return 0;
    }
    CppObjectMetadata meta;
    if (!lua_try_get_lightcppobject(L, 1, meta)) {
        lua_pushnil(L);
        lua_pushstring(L, "not a reference to an object of the game");
        return 2;
    }

    char const* kind = nullptr;
    switch (meta.MetatableTag) {
    case MetatableTag::ObjectRef: kind = "Object"; break;
    case MetatableTag::Array: kind = "Array"; break;
    case MetatableTag::Map: kind = "Map"; break;
    case MetatableTag::Set: kind = "Set"; break;
    default: break;
    }
    if (kind == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, "this kind of value keeps no address of an object");
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)(uint64_t)meta.Ptr);
    lua_pushstring(L, kind);
    return 2;
}

// Ext.Debug.GetSymbolAddress(name) -> address, or nil and why. The address the extender found
// for one of its own symbols (the names of GameHooks/DataLibrariesBG3Game.cpp, such as
// "stats__DealDamageFunctor__ApplyDamage"): a function's entry, or the place of a global. A name
// of GateKit's registry table (RegistryMappings.xml beside the DLL; a registry id such as
// "esv::combat::System::CheckForSurprise") is answered too.
UserReturn GetSymbolAddress(lua_State* L, STDString name)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("GetSymbolAddress() is only available in developer mode");
        return 0;
    }
    auto const& named = gExtender->GetLibraryManager().Mappings().Named;
    if (auto n = named.find(std::string(name.c_str())); n != named.end()) {
        if (n->second == nullptr) {
            lua_pushnil(L);
            lua_pushstring(L, "the registry's pattern for this name was not found in this executable");
            return 2;
        }
        lua_pushinteger(L, (lua_Integer)(uint64_t)n->second);
        return 1;
    }
    auto const& symbols = gExtender->GetLibraryManager().Mappings().StaticSymbols;
    auto it = symbols.find(std::string(name.c_str()));
    if (it == symbols.end()) {
        lua_pushnil(L);
        lua_pushstring(L, "neither the extender nor the registry's table has a symbol of this name");
        return 2;
    }

    auto address = *(void**)((uint8_t*)&GetStaticSymbols() + it->second.Offset);
    if (address == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, "the symbol was not found in this executable");
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)(uint64_t)address);
    return 1;
}

// Ext.Debug.GetSymbolNames() -> the names GetSymbolAddress knows, sorted, each with whether it was
// found and where the name comes from: { { Name, Found, Source = "extender" | "registry" } }.
UserReturn GetSymbolNames(lua_State* L)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("GetSymbolNames() is only available in developer mode");
        return 0;
    }
    auto const& symbols = gExtender->GetLibraryManager().Mappings().StaticSymbols;
    auto const& named = gExtender->GetLibraryManager().Mappings().Named;
    std::vector<std::pair<std::string, bool>> names;
    for (auto const& entry : symbols) names.push_back({ entry.first, false });
    for (auto const& entry : named) names.push_back({ entry.first, true });
    std::sort(names.begin(), names.end());
    lua_newtable(L);
    int index = 1;
    for (auto const& [name, fromRegistry] : names) {
        auto address = fromRegistry ? named.at(name) : *(void**)((uint8_t*)&GetStaticSymbols() + symbols.at(name).Offset);
        lua_newtable(L);
        settable(L, "Name", name.c_str());
        settable(L, "Found", address != nullptr);
        settable(L, "Source", fromRegistry ? "registry" : "extender");
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

// Ext.Debug.GetPropertyAddress(object, name) -> { Address, Offset, Size, ObjectAddress }, or nil
// and why. Where a property of the game's object behind a Lua reference lies and how wide its
// field is: the object's own address plus the property's offset. Size is 0 for a property that
// is no plain field (a computed one). With GetElementAddress this follows a path such as
// Streams[2].State.NumRolls below a component's top level.
UserReturn GetPropertyAddress(lua_State* L)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("GetPropertyAddress() is only available in developer mode");
        return 0;
    }
    CppObjectMetadata meta;
    if (!lua_try_get_lightcppobject(L, 1, meta) || meta.MetatableTag != MetatableTag::ObjectRef) {
        lua_pushnil(L);
        lua_pushstring(L, "not a reference to an object of the game");
        return 2;
    }
    if (!lua_isstring(L, 2)) {
        lua_pushnil(L);
        lua_pushstring(L, "the property's name is a string");
        return 2;
    }

    auto& pm = LightObjectProxyMetatable::GetPropertyMap(meta);
    auto prop = pm.Properties.try_get(FixedStringUnhashed(lua_tostring(L, 2)));
    if (prop == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, "the object has no property of this name");
        return 2;
    }

    lua_newtable(L);
    settable(L, "Address", (int64_t)((uint64_t)meta.Ptr + prop->Offset));
    settable(L, "Offset", (int64_t)prop->Offset);
    settable(L, "Size", (int32_t)prop->Size);
    settable(L, "ObjectAddress", (int64_t)(uint64_t)meta.Ptr);
    return 1;
}

// Ext.Debug.GetElementAddress(array, index) -> { Address, Size }, or nil and why. Where element
// `index` (from 1) of an array of the game lies and how wide an element is.
UserReturn GetElementAddress(lua_State* L)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("GetElementAddress() is only available in developer mode");
        return 0;
    }
    CppObjectMetadata meta;
    if (!lua_try_get_lightcppobject(L, 1, meta) || meta.MetatableTag != MetatableTag::Array) {
        lua_pushnil(L);
        lua_pushstring(L, "not a reference to an array of the game");
        return 2;
    }

    auto impl = gExtender->GetPropertyMapManager().GetArrayProxy(meta.PropertyMapTag);
    auto index = (unsigned)luaL_checkinteger(L, 2);
    auto address = impl != nullptr ? impl->GetElementAddress(meta, index) : nullptr;
    if (address == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, "no such element, or a collection that keeps no storage of its elements: take the element in Lua and ask GetObjectAddress");
        return 2;
    }

    lua_newtable(L);
    settable(L, "Address", (int64_t)(uint64_t)address);
    settable(L, "Size", (int32_t)impl->GetElementSize());
    // An array of bits: the address and the size are the word's, the element is this bit of it
    if (impl->GetElementBit(index) >= 0) settable(L, "Bit", (int32_t)impl->GetElementBit(index));
    return 1;
}
