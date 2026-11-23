#include <Extender/Client/DirectSaveLoad.h>
#include <Extender/Shared/StartupTimeline.h>

// WideCharToMultiByte of kernel32, as stringapiset.h declares it: the extender's windows.h
// (GameDefinitions/Base/WindowsSlim.h) leaves the code pages out with NONLS.
extern "C" __declspec(dllimport) int __stdcall WideCharToMultiByte(UINT codePage, DWORD flags,
    wchar_t const* wide, int wideLength, char* narrow, int narrowSize, char const* defaultChar, BOOL* usedDefault);

BEGIN_NS(ecl::direct_save_load)

namespace
{
    constexpr char const* Parent = "DirectSaveLoad";

    Record gRecord;

    // One entry of the client machine's queue of target states: what
    // ecl::GameStateMachine::SetTargetState appends ({0, state}) and SetTargetStateAfterFade
    // ({fade, state}).
    struct QueuedState
    {
        int32_t Fade;
        void* State;
    };

    // The queue at ecl::GameStateMachine +0x10, a ring: the entries, their capacity, how many are
    // queued, the first (-1 when none) and the next free one - as the game's own push
    // (SetTargetState) and ClearQueue use them.
    struct StateQueue
    {
        QueuedState* Entries;
        int32_t Capacity;
        int32_t Count;
        int32_t Head;
        int32_t Tail;
    };

    StateQueue* QueueOf(void* machine)
    {
        return reinterpret_cast<StateQueue*>(reinterpret_cast<uint8_t*>(machine) + 0x10);
    }

    // An ecl::GameState's id (+0x10, what GameState::GameStateToString is given).
    GameState IdOf(void* state)
    {
        return static_cast<GameState>(*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(state) + 0x10));
    }

    STDString NameOf(GameState state)
    {
        auto label = EnumInfo<GameState>::Find(state);
        return label ? STDString(label.GetString()) : STDString(std::to_string((uint32_t)state).c_str());
    }

    std::vector<QueuedState> Entries(StateQueue const& queue)
    {
        std::vector<QueuedState> entries;
        if (queue.Entries == nullptr || queue.Count <= 0 || queue.Head < 0 || queue.Capacity <= 0) {
            return entries;
        }
        for (int32_t i = 0; i < queue.Count; i++) {
            entries.push_back(queue.Entries[(queue.Head + i) % queue.Capacity]);
        }
        return entries;
    }

    std::vector<STDString> Names(std::vector<QueuedState> const& entries)
    {
        std::vector<STDString> names;
        for (auto const& entry : entries) {
            names.push_back(entry.State ? NameOf(IdOf(entry.State)) : STDString("null"));
        }
        return names;
    }

    STDString Joined(std::vector<STDString> const& names)
    {
        STDString text;
        for (auto const& name : names) {
            if (!text.empty()) text += ", ";
            text += name;
        }
        return text;
    }

    void Row(int32_t count, char const* name)
    {
        startup_timeline::Record(startup_timeline::Side::Client, startup_timeline::What::Step,
            startup_timeline::StepAt, count, name, Parent);
    }

    void Finish(int32_t count, char const* outcome)
    {
        gRecord.Outcome = outcome;
        Row(count, outcome);
        INFO("DirectSaveLoad: %s; the client's queue was [%s], is [%s]", outcome,
            Joined(gRecord.QueueBefore).c_str(), Joined(gRecord.QueueAfter).c_str());
    }

    // The menu's states as ecl::EocClient::Load queues them, in its order.
    constexpr GameState MenuStates[] = { GameState::LoadMenu, GameState::StopLoading, GameState::Menu };
}

Record const& GetRecord()
{
    return gRecord;
}

void OnGameStateChanged(GameState fromState, GameState toState)
{
    // The first exit from Init: its Do has run, and with it the save manager's TryLoad.
    if (gRecord.Seen || fromState != GameState::Init || toState != GameState::LoadModule) return;
    gRecord.Seen = true;
    gRecord.Enabled = gExtender->GetConfig().DirectSaveLoad;

    auto client = GetStaticSymbols().GetEoCClient();
    void* machine = client ? client->GameStateMachine : nullptr;
    if (machine == nullptr) {
        Finish(0, "not run: no client state machine");
        return;
    }

    auto& queue = *QueueOf(machine);
    auto entries = Entries(queue);
    gRecord.QueueBefore = Names(entries);
    gRecord.QueueAfter = gRecord.QueueBefore;

    if (!gRecord.Enabled) {
        Finish(0, "off: the switch DirectSaveLoad");
        return;
    }

    // Exactly the game's own queue: the menu's states, then the save's StartLoading and StartServer.
    constexpr std::size_t menu = std::size(MenuStates);
    bool menuFirst = entries.size() >= menu;
    for (std::size_t i = 0; menuFirst && i < menu; i++) {
        menuFirst = entries[i].State && IdOf(entries[i].State) == MenuStates[i];
    }
    if (!menuFirst) {
        Finish(0, "not run: the queue does not start with the menu's states");
        return;
    }
    if (entries.size() < menu + 2
        || !entries[menu].State || IdOf(entries[menu].State) != GameState::StartLoading
        || !entries[menu + 1].State || IdOf(entries[menu + 1].State) != GameState::StartServer) {
        Finish(0, "not run: no save's load queued behind the menu");
        return;
    }

    // The menu's states out, disposed of as ClearQueue does: the state's virtual destructor (slot
    // 0, the deleting one; their Initialize, called by SetTargetState, keeps nothing).
    std::vector<QueuedState> kept(entries.begin() + menu, entries.end());
    for (std::size_t i = 0; i < menu; i++) {
        using DeletingDestructor = void* (*)(void* self, uint64_t flags);
        auto vtable = *reinterpret_cast<DeletingDestructor**>(entries[i].State);
        vtable[0](entries[i].State, 1);
    }

    // The rest of the ring rewritten from its start, in the same order.
    for (std::size_t i = 0; i < kept.size(); i++) {
        queue.Entries[i] = kept[i];
    }
    queue.Count = (int32_t)kept.size();
    queue.Head = 0;
    queue.Tail = queue.Count == queue.Capacity ? 0 : queue.Count;

    gRecord.Applied = true;
    gRecord.QueueAfter = Names(Entries(queue));
    Finish((int32_t)menu, "the menu's states taken out: the save loads next");
}

// The save's name in any alphabet: the switch WideCommandLine.

namespace
{
    // The WinMain command line as the C runtime gives it (_get_wide_winmain_command_line, whose
    // narrow twin the game's WinMain gets): the process's command line after the program's name
    // and the blanks that follow it.
    std::wstring_view WinMainCommandLine()
    {
        wchar_t const* p = GetCommandLineW();
        if (p == nullptr) return {};
        bool quoted = false;
        while (*p > L' ' || (*p && quoted)) {
            if (*p == L'"') quoted = !quoted;
            p++;
        }
        while (*p && *p <= L' ') p++;
        return p;
    }

    // Split as the game splits its narrow line (ls::ArgumentString's constructor): at each space
    // with an even number of double quotes before it; then the quotes removed from each part.
    std::vector<std::wstring> SplitAsTheGame(std::wstring_view line)
    {
        std::vector<std::wstring> parts;
        std::wstring part;
        bool quoted = false;
        for (auto c : line) {
            if (c == L'"') {
                quoted = !quoted;
            } else if (c == L' ' && !quoted) {
                parts.push_back(part);
                part.clear();
            } else {
                part += c;
            }
        }
        parts.push_back(part);
        return parts;
    }

    std::vector<std::wstring> const& WideArguments()
    {
        static std::vector<std::wstring> const arguments = SplitAsTheGame(WinMainCommandLine());
        return arguments;
    }

    // CP_ACP and CP_UTF8 of WinNls.h, which NONLS leaves out.
    constexpr UINT AnsiCodePage = 0;
    constexpr UINT Utf8CodePage = 65001;

    std::string InCodePage(std::wstring const& text, UINT codePage)
    {
        if (text.empty()) return {};
        auto size = WideCharToMultiByte(codePage, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return {};
        std::string out((std::size_t)size, '\0');
        WideCharToMultiByte(codePage, 0, text.data(), (int)text.size(), out.data(), size, nullptr, nullptr);
        return out;
    }
}

void OnArgumentValue(char const* key, STDString* value, bool found)
{
    if (!found || key == nullptr || value == nullptr || value->empty()) return;

    auto const& arguments = WideArguments();
    for (std::size_t i = 0; i + 1 < arguments.size(); i++) {
        if (_stricmp(InCodePage(arguments[i], AnsiCodePage).c_str(), key) != 0) continue;

        // The same argument only: its ANSI form is what the game read.
        auto ansi = InCodePage(arguments[i + 1], AnsiCodePage);
        if (std::string_view(ansi) != std::string_view(value->data(), value->size())) return;
        auto utf8 = InCodePage(arguments[i + 1], Utf8CodePage);
        if (utf8.empty() || utf8 == ansi) return;

        *value = STDString(utf8.data(), utf8.size());
        char row[64];
        snprintf(row, sizeof(row), "%s given in UTF-8", key);
        startup_timeline::Record(startup_timeline::Side::Extender, startup_timeline::What::Step,
            startup_timeline::StepAt, (int32_t)utf8.size(), row, "WideCommandLine");
        return;
    }
}

END_NS()
