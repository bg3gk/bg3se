#pragma once

// Roll events. A roll passes through up to four nested functions of the roll manager:
//   RollDiceWithBoostsAgainstDC   a roll against a difficulty on the stream named by its key
//   InternalRollDiceWithBoosts    a formula with the roller's boosts
//   InternalRollDice              the dice of one formula
//   DrawNumber                    one die
// A mod sees one roll, whichever of them carried it out: BeforeRoll and Roll are thrown once, on
// the outermost call. RollDie (one die) is thrown in developer mode only.

BEGIN_NS(esv::lua)

namespace rollevents
{
    enum class Kind : uint8_t
    {
        Die,
        Dice,
        Boosted,
        AgainstDC
    };

    struct Frame
    {
        Kind CallKind;
        uint32_t Sequence;
        eoc::roll::stream::Stream* Stream;
        uint32_t BoostedCalls;
    };

    // The roll calls in progress on this thread, innermost last. They nest (4 -> 3 -> 2 -> 1) and
    // a Lua handler may roll again from inside an event.
    struct CallStack
    {
        static constexpr uint32_t MaxDepth = 32;
        Frame Frames[MaxDepth];
        uint32_t Depth{ 0 };
    };

    thread_local CallStack gCalls;
    std::atomic<uint32_t> gSequence{ 0 };

    struct GameModule
    {
        uintptr_t Base{ 0 };
        uintptr_t End{ 0 };
        char Name[64]{ 0 };
    };

    GameModule const& GetGameModule()
    {
        static GameModule module = []() {
            GameModule m;
            auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
            auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            m.Base = reinterpret_cast<uintptr_t>(base);
            m.End = m.Base + nt->OptionalHeader.SizeOfImage;
            char path[MAX_PATH]{ 0 };
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            auto slash = strrchr(path, '\\');
            strncpy_s(m.Name, slash ? slash + 1 : path, _TRUNCATE);
            return m;
        }();
        return module;
    }

    // The return address of the call into the hooked function: the first frame above the wrapper
    // that lies inside the game's module. The hooked function itself has no frame of its own yet
    // (the detour jumps before its prologue runs), so this is the caller's call site.
    uintptr_t CaptureGameCaller()
    {
        void* frames[24];
        auto count = RtlCaptureStackBackTrace(1, (DWORD)std::size(frames), frames, nullptr);
        auto const& module = GetGameModule();
        for (USHORT i = 0; i < count; i++) {
            auto address = reinterpret_cast<uintptr_t>(frames[i]);
            if (address >= module.Base && address < module.End) {
                return address;
            }
        }

        return 0;
    }

    class Scope
    {
    public:
        Scope(Kind kind, eoc::roll::stream::Stream* stream)
        {
            auto& calls = gCalls;
            Parent = calls.Depth > 0 ? calls.Frames[calls.Depth - 1].Sequence : 0;
            Sequence = ++gSequence;
            if (kind == Kind::Boosted) {
                for (auto i = calls.Depth; i > 0; i--) {
                    if (calls.Frames[i - 1].CallKind == Kind::AgainstDC) {
                        calls.Frames[i - 1].BoostedCalls++;
                        break;
                    }
                }
            }

            if (calls.Depth < CallStack::MaxDepth) {
                calls.Frames[calls.Depth++] = Frame{ kind, Sequence, stream, 0 };
                pushed_ = true;
            }

            Depth = calls.Depth;
        }

        ~Scope()
        {
            if (pushed_) {
                gCalls.Depth--;
            }
        }

        Scope(Scope const&) = delete;
        Scope& operator = (Scope const&) = delete;

        uint32_t Nearest(Kind kind) const
        {
            auto& calls = gCalls;
            auto top = pushed_ ? calls.Depth : 0;
            for (auto i = top; i > 0; i--) {
                if (calls.Frames[i - 1].CallKind == kind) return calls.Frames[i - 1].Sequence;
            }
            return 0;
        }

        eoc::roll::stream::Stream* NearestStream() const
        {
            auto& calls = gCalls;
            auto top = pushed_ ? calls.Depth : 0;
            for (auto i = top; i > 0; i--) {
                if (calls.Frames[i - 1].Stream) return calls.Frames[i - 1].Stream;
            }
            return nullptr;
        }

        uint32_t BoostedCalls() const
        {
            return pushed_ ? gCalls.Frames[Depth - 1].BoostedCalls : 0;
        }

        uint32_t Sequence{ 0 };
        uint32_t Parent{ 0 };
        uint32_t Depth{ 0 };

    private:
        bool pushed_{ false };
    };

    // The client draws through the same code; its rolls are none of the server Lua's business.
    inline bool IsClientThread()
    {
        return GetCurrentContextType() == ContextType::Client;
    }

    eoc::roll::stream::Stream* FindStream(eoc::roll::stream::RollManager* manager, uint64_t key)
    {
        if (manager == nullptr) return nullptr;
        for (auto& stream : manager->Streams) {
            if (stream.State.Frame == key) return &stream;
        }
        return nullptr;
    }

    // The game's own rule (InternalRollDiceWithBoosts): a natural 1 fails critically, a natural
    // at or above the threshold succeeds critically.
    RollCritical CriticalOf(int32_t natural, int32_t threshold)
    {
        if (natural == 1) return RollCritical::Fail;
        return natural >= threshold ? RollCritical::Success : RollCritical::None;
    }

    struct Outcome
    {
        int32_t Total;
        int32_t Natural;
        int32_t Discarded;
        RollCritical Critical;
    };

    // What a handler changed, written back. A changed Natural with Total left alone moves Total by
    // the same amount; with Critical left alone (and a threshold known) Critical follows the game's rule.
    int32_t ApplyOutcome(Outcome const& before, Outcome after, StatsRollResult* result, int32_t returned,
        std::optional<int32_t> threshold)
    {
        if (after.Natural != before.Natural) {
            if (after.Total == before.Total) {
                after.Total = before.Total + (after.Natural - before.Natural);
            }
            if (after.Critical == before.Critical && threshold) {
                after.Critical = CriticalOf(after.Natural, *threshold);
            }
        }

        if (after.Total == before.Total && after.Natural == before.Natural
            && after.Discarded == before.Discarded && after.Critical == before.Critical) {
            return returned;
        }

        if (result != nullptr) {
            result->Total = after.Total;
            result->NaturalRoll = after.Natural;
            result->DiscardedDiceTotal = after.Discarded;
            result->Critical = after.Critical;
        }

        return after.Total;
    }

    std::optional<int32_t> ThresholdOf(StatsRollResult const* result)
    {
        if (result != nullptr && result->CriticalThreshold) {
            return (int32_t)*result->CriticalThreshold;
        }
        return {};
    }

    // The roller reference the game passes: +0 EntityHandle, +8 EntityWorld*, +0x10 cached boosts
    // container (may be null), +0x30 "fetch when the cache is null".
    struct RollerRef
    {
        EntityHandle Entity;
        void* World;
        void* CachedBoosts;
        uint8_t Unknown[0x18];
        uint8_t FetchIfNull;
        uint8_t Pad[7];
    };
    static_assert(sizeof(RollerRef) == 0x38);
    static_assert(offsetof(RollerRef, CachedBoosts) == 0x10);
    static_assert(offsetof(RollerRef, FetchIfNull) == 0x30);
    static_assert(offsetof(StatsRollResult, Critical) == 0xC);
    static_assert(offsetof(StatsRollResult, CriticalThreshold) == 0x10);
    static_assert(offsetof(eoc::roll::stream::Stream, StreamType) == 0x50);
    static_assert(offsetof(eoc::roll::stream::Stream, State) == 0x58);
    static_assert(sizeof(eoc::roll::stream::Stream) == 0x78);
    static_assert(offsetof(Roll, RollType) == 0xC);
    static_assert(offsetof(Roll, Advantage) == 0xD);
    static_assert(offsetof(Roll, Disadvantage) == 0xE);

    EntityHandle RollerOf(void* roller)
    {
        return roller != nullptr ? reinterpret_cast<RollerRef*>(roller)->Entity : EntityHandle{};
    }

    template <class T>
    void FillRoll(T& evt, Roll const* roll)
    {
        if (roll == nullptr) return;
        evt.RollType = roll->RollType;
        evt.Advantage = roll->Advantage;
        evt.Disadvantage = roll->Disadvantage;
        evt.DieType = roll->Roll.DiceValue;
        evt.Count = roll->Roll.AmountOfDices;
        evt.Bonus = roll->Roll.DiceAdditionalValue;
        evt.Negate = roll->Roll.DiceNegative;
    }

    template <class T>
    void FillOutcome(T& evt, StatsRollResult const* result, int32_t total, int32_t bonus)
    {
        evt.Total = total;
        evt.HasResult = result != nullptr;
        if (result != nullptr) {
            evt.Natural = result->NaturalRoll;
            evt.Discarded = result->DiscardedDiceTotal;
            evt.Critical = result->Critical;
        } else {
            evt.Natural = total - bonus;
        }
    }

    template <class T>
    Outcome OutcomeOf(T const& evt)
    {
        return Outcome{ evt.Total, evt.Natural, evt.Discarded, evt.Critical };
    }
}

using namespace rollevents;


namespace rollevents
{
    // The dice drawn for the roll being made on this thread, in the order they were drawn
    thread_local std::vector<int32_t> tDice;
    thread_local bool tRolling{ false };

    // Who asked for the roll, for a developer: the return address into the game and the game's
    // frames above it. Walking the stack is not free, so it is done in developer mode only.
    void FillCallers(STDString& caller, STDString& chain)
    {
        if (!gExtender->GetConfig().DeveloperMode) return;

        auto const& module = GetGameModule();
        void* frames[32];
        auto count = RtlCaptureStackBackTrace(1, (DWORD)std::size(frames), frames, nullptr);
        char text[96];
        int kept = 0;
        for (USHORT i = 0; i < count && kept < 24; i++) {
            auto address = reinterpret_cast<uintptr_t>(frames[i]);
            if (address < module.Base || address >= module.End) continue;
            if (kept == 0) {
                sprintf_s(text, "%s+0x%llX", module.Name, (unsigned long long)(address - module.Base));
                caller = text;
            }
            sprintf_s(text, kept == 0 ? "0x%llX" : " 0x%llX", (unsigned long long)(address - module.Base));
            chain += text;
            kept++;
        }
    }

    // The cause of the innermost condition or functor open on this thread, into the event
    template <class TEvent>
    void FillCause(TEvent& evt)
    {
        auto const& causes = RollCauses();
        if (causes.empty()) return;
        auto const& cause = causes.back();
        evt.Source = cause.Source;
        evt.Target = cause.Target;
        if (cause.Spell != FixedString::NullIndex) { FixedStringBase name; name.Index = cause.Spell; evt.Spell = FixedString(name); }
        if (cause.Status != FixedString::NullIndex) { FixedStringBase name; name.Index = cause.Status; evt.Status = FixedString(name); }
    }

    // One roll as a player understands it: the outermost of the roll manager's nested calls. The
    // inner calls of the same roll pass through untouched.
    template <class TState, class TNext>
    int32_t LogicalRoll(TState& state, Scope const& scope, Kind kind, eoc::roll::stream::Stream* stream, void* roller, Roll* roll,
        RollDefinition* dice, int32_t dc, StatsRollResult* result, TNext&& next)
    {
        if (scope.Parent != 0) return next(dc);

        auto& hints = state.GetEventHints();
        bool throwBefore = hints.ShouldThrowEvent(EventID<BeforeRollEvent>);
        bool throwAfter = hints.ShouldThrowEvent(EventID<RollEvent>);
        if (!throwBefore && !throwAfter) return next(dc);

        auto definition = roll != nullptr ? &roll->Roll : dice;
        RollEventParams evt;
        evt.Roller = RollerOf(roller);
        if (roll != nullptr) {
            evt.RollType = roll->RollType;
            evt.Advantage = roll->Advantage;
            evt.Disadvantage = roll->Disadvantage;
        }
        if (definition != nullptr) {
            evt.DiceType = definition->DiceValue;
            evt.DiceCount = definition->AmountOfDices;
            evt.Bonus = definition->DiceAdditionalValue;
        }
        if (dc != -1) evt.DC = dc;
        if (stream != nullptr) {
            evt.HasStream = true;
            evt.Stream = stream->StreamType;
            evt.StreamId = stream->State.Frame;
        }
        FillCause(evt);
        FillCallers(evt.Caller, evt.CallerChain);

        // What a handler changes before the roll is put into the game's own roll for the length
        // of the call and taken back afterwards: the caller's structures are left as they were.
        bool oldAdvantage = evt.Advantage, oldDisadvantage = evt.Disadvantage;
        int32_t oldBonus = evt.Bonus;
        if (throwBefore) {
            BeforeRollEventParams before;
            static_cast<BeforeRollEventParams&>(before) = static_cast<BeforeRollEventParams const&>(evt);
            state.template ThrowEvent<BeforeRollEvent>(before);

            if (dc != -1 && before.DC) dc = *before.DC;
            if (roll != nullptr) {
                roll->Advantage = before.Advantage;
                roll->Disadvantage = before.Disadvantage;
            }
            if (definition != nullptr) definition->DiceAdditionalValue = before.Bonus;
            static_cast<BeforeRollEventParams&>(evt) = static_cast<BeforeRollEventParams const&>(before);
            if (dc != -1) evt.DC = dc;
        }

        tDice.clear();
        tRolling = true;
        auto total = next(dc);
        tRolling = false;

        if (roll != nullptr) {
            roll->Advantage = oldAdvantage;
            roll->Disadvantage = oldDisadvantage;
        }
        if (definition != nullptr) definition->DiceAdditionalValue = oldBonus;

        if (!throwAfter) return total;

        for (auto die : tDice) evt.Dice.push_back(die);
        evt.Total = total;
        if (result != nullptr) {
            evt.Natural = result->NaturalRoll;
            evt.Discarded = result->DiscardedDiceTotal;
            evt.Critical = result->Critical;
        } else {
            evt.Natural = total - evt.Bonus;
        }
        // A roll against a difficulty that never reached the dice with the roller's boosts was
        // decided beforehand: the roll manager's weighted mode (Karmic Dice)
        evt.Forced = kind == Kind::AgainstDC && stream != nullptr && scope.BoostedCalls() == 0;

        auto before = Outcome{ evt.Total, evt.Natural, evt.Discarded, evt.Critical };
        state.template ThrowEvent<RollEvent>(evt);
        return ApplyOutcome(before, Outcome{ evt.Total, evt.Natural, evt.Discarded, evt.Critical }, result, total, ThresholdOf(result));
    }
}

RollEventHooks::RollEventHooks(ExtensionState& state)
    : state_(state)
{
    auto& hooks = gExtender->GetEngineHooks();
    hooks.eoc__roll__stream__DrawNumber.SetWrapper(&RollEventHooks::OnDrawNumber, this);
    hooks.eoc__roll__stream__RollManager__InternalRollDice.SetWrapper(&RollEventHooks::OnRollDice, this);
    hooks.eoc__roll__stream__RollManager__InternalRollDiceWithBoosts.SetWrapper(&RollEventHooks::OnRollBoosted, this);
    hooks.eoc__roll__stream__RollManager__RollDiceWithBoostsAgainstDC.SetWrapper(&RollEventHooks::OnRollAgainstDC, this);
    hooks.thoth__shared__Machine__CheckStaticCondition.SetWrapper(&RollEventHooks::OnCheckCondition, this);
    hooks.esv__interrupt__AdjustConditionRoll.SetWrapper(&RollEventHooks::OnAdjustConditionRoll, this);
}

RollEventHooks::~RollEventHooks()
{
    auto& hooks = gExtender->GetEngineHooks();
    hooks.eoc__roll__stream__DrawNumber.ClearHook();
    hooks.eoc__roll__stream__RollManager__InternalRollDice.ClearHook();
    hooks.eoc__roll__stream__RollManager__InternalRollDiceWithBoosts.ClearHook();
    hooks.eoc__roll__stream__RollManager__RollDiceWithBoostsAgainstDC.ClearHook();
    hooks.thoth__shared__Machine__CheckStaticCondition.ClearHook();
    hooks.esv__interrupt__AdjustConditionRoll.ClearHook();
}

std::vector<RollCause>& RollCauses()
{
    thread_local std::vector<RollCause> causes;
    return causes;
}

// A name read through a pointer the game holds in a context; none when the pointer leads nowhere
static uint32_t ReadNameIndex(uint8_t const* from)
{
    __try {
        return *reinterpret_cast<uint32_t const*>(from);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FixedString::NullIndex;
    }
}

// The context of a stats condition (ls::thoth::shared::ContextData): the two parties, the hit the
// condition is asked about (whose spell is the one cast) and the status it belongs to.
uint64_t RollEventHooks::OnCheckCondition(thoth::shared::Machine__CheckStaticConditionProc* next, void* machine, void* result,
    int32_t index, void* context, uint64_t a5, uint64_t a6)
{
    if (context == nullptr || IsClientThread()) return next(machine, result, index, context, a5, a6);

    auto bytes = reinterpret_cast<uint8_t const*>(context);
    RollCause cause;
    cause.Source = *reinterpret_cast<EntityHandle const*>(bytes);
    cause.Target = *reinterpret_cast<EntityHandle const*>(bytes + 0x10);
    auto hit = *reinterpret_cast<uint8_t const* const*>(bytes + 0x68);
    if (hit != nullptr) cause.Spell = ReadNameIndex(hit + 0x150);
    cause.Status = *reinterpret_cast<uint32_t const*>(bytes + 0xD4);

    auto& causes = RollCauses();
    causes.push_back(cause);
    auto answer = next(machine, result, index, context, a5, a6);
    causes.pop_back();
    return answer;
}

bool RollEventHooks::OnAdjustConditionRoll(esv::interrupt::AdjustConditionRollProc* next, void* adjustments, ConditionRoll* roll)
{
    if (roll == nullptr || IsClientThread() || !state_.GetEventHints().ShouldThrowEvent(EventID<RollAdjustedEvent>)) {
        return next(adjustments, roll);
    }

    auto stats = std::get_if<StatsRoll>(&roll->Roll);
    RollAdjustedEventParams evt;
    if (stats != nullptr) {
        evt.OldNatural = stats->Result.NaturalRoll;
        evt.OldTotal = stats->Result.Total;
        evt.OldCritical = stats->Result.Critical;
    }

    auto changed = next(adjustments, roll);

    if (stats == nullptr) return changed;
    evt.Natural = stats->Result.NaturalRoll;
    evt.Total = stats->Result.Total;
    evt.Critical = stats->Result.Critical;
    // The function is asked for every roll interrupts were used on; an event only when it changed one
    if (!changed && evt.Natural == evt.OldNatural && evt.Total == evt.OldTotal && evt.Critical == evt.OldCritical) return changed;

    evt.Roll = roll;
    evt.RollUuid = roll->RollUuid;
    evt.DC = roll->Difficulty;
    evt.OutcomeChanged = changed;
    state_.ThrowEvent<RollAdjustedEvent>(evt);
    return changed;
}

uint64_t RollEventHooks::OnDrawNumber(eoc::roll::stream::DrawNumberProc* next, void* generator, uint64_t min, uint64_t max)
{
    if (IsClientThread()) return next(generator, min, max);

    auto value = next(generator, min, max);
    if (tRolling) tDice.push_back((int32_t)value);

    // A single die is a developer's concern: a mod listens to the roll
    if (!gExtender->GetConfig().DeveloperMode || !state_.GetEventHints().ShouldThrowEvent(EventID<RollDieEvent>)) return value;

    RollDieEventParams evt;
    evt.Min = min;
    evt.Max = max;
    evt.Value = value;
    FillCallers(evt.Caller, evt.CallerChain);
    state_.ThrowEvent<RollDieEvent>(evt);
    if (evt.Value != value && tRolling && !tDice.empty()) tDice.back() = (int32_t)evt.Value;
    return evt.Value;
}

int32_t RollEventHooks::OnRollDice(eoc::roll::stream::RollManager__InternalRollDiceProc* next, eoc::roll::stream::RollManager* self,
    eoc::roll::stream::Stream* stream, RollDefinition* dice, bool updateDebts, int32_t dc, bool side, float probability,
    bool maximize, StatsRollResult* result, void* perDie)
{
    if (IsClientThread()) return next(self, stream, dice, updateDebts, dc, side, probability, maximize, result, perDie);

    Scope scope(Kind::Dice, stream);
    return LogicalRoll(state_, scope, Kind::Dice, stream, nullptr, nullptr, dice, dc, result, [&](int32_t difficulty) {
        return next(self, stream, dice, updateDebts, difficulty, side, probability, maximize, result, perDie);
    });
}

int32_t RollEventHooks::OnRollBoosted(eoc::roll::stream::RollManager__InternalRollDiceWithBoostsProc* next, eoc::roll::stream::RollManager* self,
    void* env, void* roller, void* worldView, eoc::roll::stream::Stream* stream, Roll* roll, bool updateDebts, int32_t dc,
    bool side, float probability, int32_t critRange, bool maximize, StatsRollResult* result)
{
    if (IsClientThread()) {
        return next(self, env, roller, worldView, stream, roll, updateDebts, dc, side, probability, critRange, maximize, result);
    }

    Scope scope(Kind::Boosted, stream);
    return LogicalRoll(state_, scope, Kind::Boosted, stream, roller, roll, nullptr, dc, result, [&](int32_t difficulty) {
        return next(self, env, roller, worldView, stream, roll, updateDebts, difficulty, side, probability, critRange, maximize, result);
    });
}

int32_t RollEventHooks::OnRollAgainstDC(eoc::roll::stream::RollManager__RollDiceWithBoostsAgainstDCProc* next, eoc::roll::stream::RollManager* self,
    void* env, uint64_t streamKey, void* roller, void* worldView, Roll* roll, int32_t dc, bool side, uint8_t* critFlags,
    int32_t critRange, bool maximize, StatsRollResult* result)
{
    if (IsClientThread()) {
        return next(self, env, streamKey, roller, worldView, roll, dc, side, critFlags, critRange, maximize, result);
    }

    auto stream = FindStream(self, streamKey);
    Scope scope(Kind::AgainstDC, stream);
    return LogicalRoll(state_, scope, Kind::AgainstDC, stream, roller, roll, nullptr, dc, result, [&](int32_t difficulty) {
        return next(self, env, streamKey, roller, worldView, roll, difficulty, side, critFlags, critRange, maximize, result);
    });
}

END_NS()
