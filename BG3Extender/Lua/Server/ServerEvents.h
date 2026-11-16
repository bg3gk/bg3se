#pragma once

BEGIN_NS(esv::lua)

struct GameStateChangedEventParams : public lua::EventBase
{
    esv::GameState FromState;
    esv::GameState ToState;
};

struct DealDamageEventParams : public lua::EventBase
{
    bg3se::stats::DealDamageFunctor* Functor;
    EntityHandle Caster;
    EntityHandle Target;
    glm::vec3 Position;
    bool IsFromItem;
    SpellIdWithPrototype* SpellId;
    int StoryActionId;
    ActionOriginator* Originator;
    HitDesc* Hit;
    [[bg3::legacy(DamageSums)]] AttackDesc* Attack;
    HitWith HitWith;
    EntityHandle Caster2;
    bg3se::SpellId* SpellId2;
};

struct DealtDamageEventParams : public DealDamageEventParams
{
    HitResult* Result;
};

struct BeforeDealDamageEventParams : public lua::EventBase
{
    HitDesc* Hit;
    [[bg3::legacy(DamageSums)]] AttackDesc* Attack;
};

struct ExecuteFunctorEventParams : public lua::EventBase
{
    bg3se::stats::Functors* Functor;
    bg3se::stats::ContextData* Params;
};

struct AfterExecuteFunctorEventParams : public lua::EventBase
{
    bg3se::stats::Functors* Functor;
    bg3se::stats::ContextData* Params;
    HitResult* Hit;
};

// A roll of the roll manager about to be made: a check, a save, an attack roll, damage dice,
// initiative, a heal - by any creature. DC, Advantage, Disadvantage and Bonus may be changed.
struct BeforeRollEventParams : public lua::EventBase
{
    // Who rolls; a null handle for dice that no creature rolls (a heal's dice, initiative)
    EntityHandle Roller;
    bg3se::stats::RollType RollType{ bg3se::stats::RollType::None };
    DiceSizeId DiceType{ DiceSizeId::D20 };
    uint8_t DiceCount{ 0 };
    int32_t Bonus{ 0 };
    bool Advantage{ false };
    bool Disadvantage{ false };
    // The difficulty the roll is made against, when it has one
    std::optional<int32_t> DC;
    // The roll stream the dice are drawn from
    bool HasStream{ false };
    RollStreamType Stream{ RollStreamType::Generic };
    uint64_t StreamId{ 0 };
    // What the roll is for, when the game says so at the moment of the roll: the two parties of
    // the condition or of the effect the roll belongs to (the caster or attacker and the target;
    // for a status its cause and its bearer), the spell and the status. Empty where it does not
    // apply or is not known (initiative, a roll the story asks for).
    EntityHandle Source;
    EntityHandle Target;
    FixedString Spell;
    FixedString Status;
    // Developer mode: the return address of the code that asked for the roll
    // ("bg3_dx11.exe+0x106CE8B") and the RVAs of the game's frames above it, the asker first
    STDString Caller;
    STDString CallerChain;
};

// The roll, made. Natural, Total and Critical may be changed.
struct RollEventParams : public BeforeRollEventParams
{
    // Every die drawn for the roll, in order: both dice of a d20, a die thrown again
    Array<int32_t> Dice;
    // The dice that count, without the bonus; the other die of a d20's pair
    int32_t Natural{ 0 };
    int32_t Discarded{ 0 };
    int32_t Total{ 0 };
    RollCritical Critical{ RollCritical::None };
    // The outcome was decided before the dice were drawn (Karmic Dice)
    bool Forced{ false };
};

// A roll changed after it was thrown: an interrupt set it, adjusted it or rerolled it
// (Portent, Shield, Lucky, Bardic Inspiration). Thrown once the change is written.
struct RollAdjustedEventParams : public lua::EventBase
{
    // The roll as the game goes on with it; nil for a roll that is no dice roll
    ConditionRoll* Roll{ nullptr };
    Guid RollUuid;
    int32_t DC{ 0 };
    int32_t OldNatural{ 0 };
    int32_t OldTotal{ 0 };
    RollCritical OldCritical{ RollCritical::None };
    int32_t Natural{ 0 };
    int32_t Total{ 0 };
    RollCritical Critical{ RollCritical::None };
    // Whether the change turned a failure into a success or a success into a failure
    bool OutcomeChanged{ false };
};

// Developer mode: one die drawn. Value may be changed.
struct RollDieEventParams : public lua::EventBase
{
    uint64_t Min{ 0 };
    uint64_t Max{ 0 };
    uint64_t Value{ 0 };
    STDString Caller;
    STDString CallerChain;
};

END_NS()
