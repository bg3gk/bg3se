// --- Coverage: which functions run, and who calls them, while the game is played ----------------
//
// A counter stands at the entry of each of many functions at once (tens of thousands). It is a few
// instructions of generated code and no more: it writes the pair "this function, the address that
// called it" into a shared ring and jumps on into the function, touching no argument register.
// The ring is folded into counts every tick of either side: calls per function and calls per pair.
// A function called more often than a limit is taken off by itself, so that the game does not slow.
// Developer mode only.

namespace {

constexpr uint64_t CoverRingEntries = 1ull << 22;        // 16 bytes each
constexpr uint32_t CoverStubBytes = 96;
constexpr uint32_t CoverBatch = 256;

struct CoverSlot
{
    uint64_t Address{ 0 };
    void* Original{ nullptr };          // the trampoline while attached, the function itself otherwise
    uint8_t* Stub{ nullptr };
    uint64_t Hits{ 0 };
    bool Attached{ false };
    bool Hot{ false };
    // set by the counter itself at every call: that the function ran never depends on the ring
    uint8_t Seen{ 0 };
};

struct CoverEdgeKey
{
    uint32_t Slot;
    uint64_t Caller;
    bool operator==(CoverEdgeKey const& o) const { return Slot == o.Slot && Caller == o.Caller; }
};

struct CoverEdgeHash
{
    std::size_t operator()(CoverEdgeKey const& k) const { return (std::size_t)(k.Caller * 0x9E3779B97F4A7C15ull) ^ k.Slot; }
};

// The ring: the next index at +0, the entries from +0x10, each the caller and the slot's number
// plus one (0: not written yet)
uint64_t* gCoverRing{ nullptr };
uint64_t gCoverRead{ 0 };
uint64_t gCoverLost{ 0 };
uint64_t gCoverCalls{ 0 };
uint64_t gCoverHotLimit{ 200000 };
std::deque<CoverSlot>* gCoverSlots{ nullptr };     // a deque: a stub holds a pointer into its slot, which must never move
std::vector<uint8_t*>* gCoverCode{ nullptr };
std::unordered_map<CoverEdgeKey, uint64_t, CoverEdgeHash>* gCoverEdges{ nullptr };
bool gCoverActive{ false };
std::vector<uint32_t> gCoverHotQueue;     // called more often than the limit: taken off at the next tick

// The stub leaves every register and the flags as it found them: some functions of the runtime
// take their arguments in RAX, R10 or R11, and a counter must be invisible to all of them. The
// pointer it jumps through lies in the stub itself (the cell), where Detours writes the
// trampoline's address.
constexpr uint32_t CoverCellOffset = 88;

void** CoverCell(uint8_t* stub) { return (void**)(stub + CoverCellOffset); }

uint8_t* CoverBuildStub(uint8_t* at, uint32_t slot, uint64_t function, uint8_t* seen)
{
    uint8_t* w = at;
    auto bytes = [&](std::initializer_list<uint8_t> list) { for (auto b : list) *w++ = b; };
    auto imm64 = [&](uint64_t v) { memcpy(w, &v, 8); w += 8; };
    auto imm32 = [&](uint32_t v) { memcpy(w, &v, 4); w += 4; };
    bytes({ 0x9C });                                         // pushfq
    bytes({ 0x50 });                                         // push rax
    bytes({ 0x41, 0x52 });                                   // push r10
    bytes({ 0x41, 0x53 });                                   // push r11
    bytes({ 0x48, 0x8B, 0x44, 0x24, 0x20 });                 // mov rax, [rsp+0x20]    the caller
    bytes({ 0x49, 0xBA }); imm64((uint64_t)gCoverRing);      // mov r10, ring
    bytes({ 0x41, 0xBB }); imm32(1);                         // mov r11d, 1
    bytes({ 0xF0, 0x4D, 0x0F, 0xC1, 0x1A });                 // lock xadd [r10], r11   r11: this call's index
    bytes({ 0x49, 0x81, 0xE3 }); imm32((uint32_t)(CoverRingEntries - 1));   // and r11, mask
    bytes({ 0x49, 0xC1, 0xE3, 0x04 });                       // shl r11, 4
    bytes({ 0x4B, 0x89, 0x44, 0x1A, 0x10 });                 // mov [r10+r11+0x10], rax
    bytes({ 0x4B, 0xC7, 0x44, 0x1A, 0x18 }); imm32(slot + 1);// mov qword [r10+r11+0x18], slot + 1
    bytes({ 0x49, 0xBA }); imm64((uint64_t)seen);            // mov r10, &slot.Seen
    bytes({ 0x41, 0xC6, 0x02, 0x01 });                       // mov byte [r10], 1
    bytes({ 0x41, 0x5B });                                   // pop r11
    bytes({ 0x41, 0x5A });                                   // pop r10
    bytes({ 0x58 });                                         // pop rax
    bytes({ 0x9D });                                         // popfq
    bytes({ 0xFF, 0x25 }); imm32((uint32_t)(CoverCellOffset - (uint32_t)(w + 4 - at)));   // jmp [the cell]
    *CoverCell(at) = (void*)function;
    return at;
}

// Whether an address is where a function of its own begins, by the executable's own table of
// functions: an address inside a function, at a split-off part of one, or in no function at all
// is not patched.
bool CoverIsFunctionStart(uint64_t address)
{
    DWORD64 base = 0;
    auto entry = RtlLookupFunctionEntry(address, &base, nullptr);
    if (entry == nullptr || base + entry->BeginAddress != address) return false;
    auto flags = *(uint8_t const*)(base + entry->UnwindData) >> 3;
    return (flags & UNW_FLAG_CHAININFO) == 0;
}

// Folds what the counters wrote since the last time into the counts. The newest few entries are
// left for the next time: a counter may have taken its index and not written its entry yet.
void CoverDrain(bool all)
{
    if (gCoverRing == nullptr || gCoverSlots == nullptr) return;
    uint64_t head = gCoverRing[0];
    uint64_t upto = all ? head : (head > 64 ? head - 64 : 0);
    if (upto <= gCoverRead) return;
    if (upto - gCoverRead > CoverRingEntries) {
        gCoverLost += (upto - gCoverRead) - CoverRingEntries;
        gCoverRead = upto - CoverRingEntries;
    }
    auto& slots = *gCoverSlots;
    for (uint64_t n = gCoverRead; n < upto; n++) {
        uint64_t* entry = gCoverRing + 2 + (n & (CoverRingEntries - 1)) * 2;
        uint64_t caller = entry[0], slot = entry[1];
        entry[1] = 0;
        if (slot == 0 || slot > slots.size()) continue;
        if (++slots[slot - 1].Hits == gCoverHotLimit + 1) gCoverHotQueue.push_back((uint32_t)(slot - 1));
        gCoverCalls++;
        (*gCoverEdges)[CoverEdgeKey{ (uint32_t)(slot - 1), caller }]++;
    }
    gCoverRead = upto;
}

// Attaches or detaches the slots [from, to) in one transaction with every other thread enlisted.
LONG CoverPatch(std::vector<HANDLE> const& threads, uint32_t from, uint32_t to, bool attach)
{
    auto& slots = *gCoverSlots;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    for (auto thread : threads) DetourUpdateThread(thread);
    LONG error = NO_ERROR;
    for (uint32_t i = from; i < to && error == NO_ERROR; i++) {
        auto& s = slots[i];
        if (attach) {
            if (s.Attached || s.Hot) continue;
            error = DetourAttach(CoverCell(s.Stub), s.Stub);
        } else {
            if (!s.Attached) continue;
            error = DetourDetach(CoverCell(s.Stub), s.Stub);
        }
    }
    if (error != NO_ERROR) {
        DetourTransactionAbort();
        return error;
    }
    error = DetourTransactionCommit();
    if (error == NO_ERROR) {
        for (uint32_t i = from; i < to; i++) {
            auto& s = slots[i];
            if (attach) { if (!s.Hot) s.Attached = true; } else s.Attached = false;
        }
    }
    return error;
}

std::vector<HANDLE> CoverThreads()
{
    std::vector<HANDLE> threads;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return threads;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
    if (Thread32First(snap, &entry)) {
        do {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, entry.th32ThreadID);
            if (thread != nullptr) threads.push_back(thread);
        } while (Thread32Next(snap, &entry));
    }
    CloseHandle(snap);
    return threads;
}

// Takes off the functions called more often than the limit since they were put on.
void CoverCool()
{
    if (!gCoverActive || gCoverSlots == nullptr) return;
    auto& slots = *gCoverSlots;
    if (gCoverHotQueue.empty()) return;
    std::vector<uint32_t> hot;
    hot.swap(gCoverHotQueue);
    auto threads = CoverThreads();
    // All of them in one transaction: the threads are stopped once, not once for each function
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    for (auto thread : threads) DetourUpdateThread(thread);
    LONG error = NO_ERROR;
    for (auto i : hot) {
        if (!slots[i].Attached) continue;
        error = DetourDetach(CoverCell(slots[i].Stub), slots[i].Stub);
        if (error != NO_ERROR) break;
    }
    if (error == NO_ERROR) error = DetourTransactionCommit(); else DetourTransactionAbort();
    if (error == NO_ERROR) {
        for (auto i : hot) { slots[i].Attached = false; slots[i].Hot = true; }
    } else {
        for (auto i : hot) {
            if (slots[i].Attached && CoverPatch(threads, i, i + 1, false) == NO_ERROR) slots[i].Hot = true;
        }
    }
    for (auto thread : threads) CloseHandle(thread);
}

} // anonymous namespace

// The counts are folded and the hot functions taken off by a thread of the instrument's own, not
// on the game's threads: a tick of the game must not wait for a million entries to be counted.
bool gCoverWorker{ false };

void CoverWork()
{
    for (;;) {
        if (!gCoverActive) { Sleep(50); continue; }
        {
            std::lock_guard _(gInstrumentMutex);
            CoverDrain(false);
            CoverCool();
        }
        Sleep(2);
    }
}

// Called every tick of either side (OnExtenderTick): nothing is left for it to do.
void CoverTick()
{
}
// Called every tick of either side (OnExtenderTick).

// Puts a counter on the function that begins at each address; the caller holds gInstrumentMutex.
// False when there is no memory for the ring.
bool CoverPlace(std::vector<uint64_t> const& addresses, uint32_t& asked, uint32_t& placed, uint32_t& refused)
{
    if (gCoverRing == nullptr) {
        gCoverRing = (uint64_t*)VirtualAlloc(nullptr, 16 + CoverRingEntries * 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        gCoverSlots = new std::deque<CoverSlot>();
        gCoverCode = new std::vector<uint8_t*>();
        gCoverEdges = new std::unordered_map<CoverEdgeKey, uint64_t, CoverEdgeHash>();
        if (gCoverRing == nullptr) return false;
    }

    auto& slots = *gCoverSlots;
    std::unordered_set<uint64_t> have;
    for (auto const& s : slots) have.insert(s.Address);

    uint32_t first = (uint32_t)slots.size();
    uint8_t* block = nullptr;
    uint32_t used = 0, blockStubs = 0;
    asked = placed = refused = 0;
    for (auto address : addresses) {
        asked++;
        if (address == 0 || have.count(address) || !IsReadableRange((uint8_t const*)address, 16) || !CoverIsFunctionStart(address)) continue;
        have.insert(address);
        if (block == nullptr || used == blockStubs) {
            blockStubs = 4096;
            block = (uint8_t*)VirtualAlloc(nullptr, (SIZE_T)blockStubs * CoverStubBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (block == nullptr) break;
            gCoverCode->push_back(block);
            used = 0;
        }
        CoverSlot slot;
        slot.Address = address;
        slot.Original = (void*)address;
        slot.Stub = block + (SIZE_T)used * CoverStubBytes;
        used++;
        slots.push_back(slot);
    }
    // Only now are the slots at their final places: build each stub around its own slot
    for (uint32_t i = first; i < slots.size(); i++) CoverBuildStub(slots[i].Stub, i, slots[i].Address, &slots[i].Seen);
    for (auto code : *gCoverCode) FlushInstructionCache(GetCurrentProcess(), code, (SIZE_T)4096 * CoverStubBytes);

    DetourSetIgnoreTooSmall(TRUE);
    auto threads = CoverThreads();
    for (uint32_t from = first; from < slots.size(); from += CoverBatch) {
        uint32_t to = (uint32_t)std::min<std::size_t>(slots.size(), (std::size_t)from + CoverBatch);
        if (CoverPatch(threads, from, to, true) != NO_ERROR) {
            // one of the batch cannot be patched: each on its own, so that only it is refused
            for (uint32_t i = from; i < to; i++) CoverPatch(threads, i, i + 1, true);
        }
        for (uint32_t i = from; i < to; i++) {
            // a function too small to patch is left as it was: its pointer still is the function
            if (slots[i].Attached && *CoverCell(slots[i].Stub) == (void*)slots[i].Address) slots[i].Attached = false;
            if (slots[i].Attached) placed++; else refused++;
        }
    }
    for (auto thread : threads) CloseHandle(thread);
    gCoverActive = true;
    if (!gCoverWorker) {
        gCoverWorker = true;
        std::thread(CoverWork).detach();
    }
    return true;
}

// Ext.Debug.StartCoverage(addresses, [hotLimit]) -> { Asked, Placed, Refused }. Puts a counter on the
// function that begins at each address of the list. A function Detours cannot patch (too short)
// is refused and the rest go on. hotLimit (default 200,000): a function called more often than
// this since the start is taken off by itself. A second start adds to the first.
UserReturn StartCoverage(lua_State* L)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("StartCoverage() is only available in developer mode");
        return 0;
    }
    WARN_ONCE("Coverage counters are a development tool for studying the game's code; they should not be used in production!");
    luaL_checktype(L, 1, LUA_TTABLE);
    std::lock_guard _(gInstrumentMutex);
    if (lua_isinteger(L, 2)) gCoverHotLimit = (uint64_t)lua_tointeger(L, 2);

    std::vector<uint64_t> addresses;
    auto count = (uint32_t)lua_rawlen(L, 1);
    addresses.reserve(count);
    for (uint32_t i = 1; i <= count; i++) {
        lua_rawgeti(L, 1, i);
        addresses.push_back((uint64_t)lua_tointeger(L, -1));
        lua_pop(L, 1);
    }
    uint32_t asked = 0, placed = 0, refused = 0;
    if (!CoverPlace(addresses, asked, placed, refused)) return luaL_error(L, "StartCoverage: no memory for the ring");

    lua_newtable(L);
    settable(L, "Asked", (int64_t)asked);
    settable(L, "Placed", (int64_t)placed);
    settable(L, "Refused", (int64_t)refused);
    return 1;
}

// Ext.Debug.GetCoverageStatus() -> { Functions, Attached, Hot, Seen, Calls, Pairs, Lost }: how many
// functions carry a counter now, how many were taken off as called too often, how many were
// called at all, the calls and the pairs counted, and the calls the ring lost.
UserReturn GetCoverageStatus(lua_State* L)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("GetCoverageStatus() is only available in developer mode");
        return 0;
    }
    std::lock_guard _(gInstrumentMutex);
    CoverDrain(false);
    uint64_t attached = 0, hot = 0, seen = 0;
    if (gCoverSlots != nullptr) {
        for (auto const& s : *gCoverSlots) {
            if (s.Attached) attached++;
            if (s.Hot) hot++;
            if (s.Hits > 0 || s.Seen) seen++;
        }
    }
    lua_newtable(L);
    settable(L, "Functions", (int64_t)(gCoverSlots ? gCoverSlots->size() : 0));
    settable(L, "Attached", (int64_t)attached);
    settable(L, "Hot", (int64_t)hot);
    settable(L, "Seen", (int64_t)seen);
    settable(L, "Calls", (int64_t)gCoverCalls);
    settable(L, "Pairs", (int64_t)(gCoverEdges ? gCoverEdges->size() : 0));
    settable(L, "Lost", (int64_t)gCoverLost);
    return 1;
}

// The record's format (Ext.Debug.SaveCoverage): a text file of
// "F <module> <rva> <calls> <hot>" for every function called, "E <module> <rva> <module> <rva>
// <calls>" for every pair of a function and the address that called it. With reset the counts
// start again from zero, so that a file holds one stretch of play.
// Writes the counts to a file and, with reset, starts them again; the caller holds
// gInstrumentMutex. Null when all went well, else what went wrong.
char const* CoverWrite(char const* path, bool reset, uint64_t& functions, uint64_t& pairs)
{
    functions = pairs = 0;
    CoverDrain(false);
    if (gCoverSlots == nullptr) return "nothing is covered";

    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || file == nullptr) return "the file cannot be written";

    std::unordered_map<HMODULE, std::string> names;
    auto place = [&](uint64_t address, std::string& module, uint64_t& rva) {
        HMODULE handle{ nullptr };
        module = "?";
        rva = address;
        if (address != 0 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)address, &handle) && handle != nullptr) {
            auto it = names.find(handle);
            if (it == names.end()) {
                char full[MAX_PATH]{ 0 };
                GetModuleFileNameA(handle, full, MAX_PATH);
                auto slash = strrchr(full, '\\');
                it = names.emplace(handle, slash ? slash + 1 : full).first;
            }
            module = it->second;
            rva = address - (uint64_t)handle;
        }
    };

    auto& slots = *gCoverSlots;
    std::string module, callerModule;
    uint64_t rva = 0, callerRva = 0;
    for (auto const& s : slots) {
        if (s.Hits == 0 && !s.Seen) continue;
        place(s.Address, module, rva);
        fprintf(file, "F %s %llX %llu %d\n", module.c_str(), (unsigned long long)rva, (unsigned long long)s.Hits, s.Hot ? 1 : 0);
        functions++;
    }
    for (auto const& edge : *gCoverEdges) {
        place(slots[edge.first.Slot].Address, module, rva);
        place(edge.first.Caller, callerModule, callerRva);
        fprintf(file, "E %s %llX %s %llX %llu\n", module.c_str(), (unsigned long long)rva, callerModule.c_str(),
            (unsigned long long)callerRva, (unsigned long long)edge.second);
        pairs++;
    }
    fprintf(file, "L %llu\n", (unsigned long long)gCoverLost);
    fclose(file);

    if (reset) {
        // the hot ones keep their count: it is what says they were taken off
        for (auto& s : slots) { if (!s.Hot) s.Hits = 0; s.Seen = 0; }
        gCoverEdges->clear();
        gCoverLost = 0;
    }
    return nullptr;
}

// Ext.Debug.SaveCoverage(path, [reset]) -> { Functions, Pairs }: CoverWrite for Lua.
UserReturn SaveCoverage(lua_State* L, STDString path, std::optional<bool> reset)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("SaveCoverage() is only available in developer mode");
        return 0;
    }
    std::lock_guard _(gInstrumentMutex);
    uint64_t functions = 0, pairs = 0;
    if (auto problem = CoverWrite(path.c_str(), reset.value_or(false), functions, pairs)) {
        return luaL_error(L, "SaveCoverage: %s", problem);
    }

    lua_newtable(L);
    settable(L, "Functions", (int64_t)functions);
    settable(L, "Pairs", (int64_t)pairs);
    return 1;
}

// Ext.Debug.StopCoverage() -> how many counters were taken off. The counts stay for SaveCoverage.
int64_t StopCoverage(lua_State* L)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("StopCoverage() is only available in developer mode");
        return 0;
    }
    std::lock_guard _(gInstrumentMutex);
    if (gCoverSlots == nullptr) return 0;
    auto& slots = *gCoverSlots;
    auto threads = CoverThreads();
    int64_t removed = 0;
    for (uint32_t from = 0; from < slots.size(); from += CoverBatch) {
        uint32_t to = (uint32_t)std::min<std::size_t>(slots.size(), (std::size_t)from + CoverBatch);
        uint32_t before = 0;
        for (uint32_t i = from; i < to; i++) if (slots[i].Attached) before++;
        if (CoverPatch(threads, from, to, false) != NO_ERROR) {
            for (uint32_t i = from; i < to; i++) CoverPatch(threads, i, i + 1, false);
        }
        uint32_t after = 0;
        for (uint32_t i = from; i < to; i++) if (slots[i].Attached) after++;
        removed += before - after;
    }
    for (auto thread : threads) CloseHandle(thread);
    CoverDrain(true);
    gCoverActive = false;
    return removed;
}

// --- The start's coverage -------------------------------------------------------------------------
// Lua comes only with LoadModule; the start before it is covered from the setting "CoverageAtStart":
// a file of the executable's function starts, one hexadecimal offset a line. The counters are put on
// as soon as the extender has started - before the engine's start - and at every state the client
// enters, up to the main menu, the counts so far are saved beside that file and start again
// ("<file>.<NN>-<state>.txt"). The counters stay on afterwards, for Ext.Debug.SaveCoverage.

std::string gCoverAtStart;
uint32_t gCoverAtStartSaved{ 0 };
bool gCoverAtStartDone{ false };

void CoverAtStartBegin()
{
    auto const& config = gExtender->GetConfig();
    if (!config.DeveloperMode || config.CoverageAtStart.empty()) return;

    FILE* file = nullptr;
    if (fopen_s(&file, config.CoverageAtStart.c_str(), "rb") != 0 || file == nullptr) {
        ERR("CoverageAtStart: %s cannot be read", config.CoverageAtStart.c_str());
        return;
    }
    auto base = (uint64_t)GetModuleHandleW(nullptr);
    std::vector<uint64_t> addresses;
    char line[64];
    while (fgets(line, sizeof(line), file) != nullptr) {
        char* end = nullptr;
        auto rva = strtoull(line, &end, 16);
        if (end != line && rva != 0) addresses.push_back(base + rva);
    }
    fclose(file);

    std::lock_guard _(gInstrumentMutex);
    uint32_t asked = 0, placed = 0, refused = 0;
    if (!CoverPlace(addresses, asked, placed, refused)) {
        ERR("CoverageAtStart: no memory for the ring");
        return;
    }
    gCoverAtStart = config.CoverageAtStart;
    INFO("CoverageAtStart: %u functions asked, %u counted, %u refused", asked, placed, refused);
}

void CoverAtStartState(char const* state)
{
    if (gCoverAtStart.empty() || gCoverAtStartDone) return;
    std::lock_guard _(gInstrumentMutex);
    char path[1024];
    snprintf(path, sizeof(path), "%s.%02u-%s.txt", gCoverAtStart.c_str(), ++gCoverAtStartSaved, state);
    uint64_t functions = 0, pairs = 0;
    if (auto problem = CoverWrite(path, true, functions, pairs)) {
        ERR("CoverageAtStart: %s: %s", path, problem);
    }
    if (strcmp(state, "Menu") == 0) gCoverAtStartDone = true;
}
