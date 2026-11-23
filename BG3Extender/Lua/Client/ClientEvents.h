#pragma once

BEGIN_NS(ecl::lua)

using namespace bg3se::lua;

struct GameStateChangedEventParams : public lua::EventBase
{
    ecl::GameState FromState;
    ecl::GameState ToState;
};

struct KeyInputEventParams : public lua::EventBase
{
    SDLKeyEvent Event;
    SDLScanCode Key;
    SDLKeyModifier Modifiers;
    bool Pressed;
    bool Repeat;
    // GateKit: the event was put in by Ext.Input.Inject* and did not come from a device
    bool Injected;
};

struct MouseButtonEventParams : public lua::EventBase
{
    int Button;
    bool Pressed;
    int Clicks;
    int X;
    int Y;
    bool Injected;
};

struct MouseWheelEventParams : public lua::EventBase
{
    float ScrollX;
    float ScrollY;
    int X;
    int Y;
    bool Injected;
};

struct ControllerAxisEventParams : public lua::EventBase
{
    int32_t DeviceId;
    SDLControllerAxis Axis;
    float Value;
};

struct ControllerButtonEventParams : public lua::EventBase
{
    int32_t DeviceId;
    SDLKeyEvent Event;
    SDLControllerButton Button;
    bool Pressed;
};

struct ViewportResizedEventParams : public lua::EventBase
{
    int Width;
    int Height;
};

// GateKit: the client loaded another set of modules than at its previous load (a change of the mod
// list applied by the game at a session's start): the module UUIDs before and now, in load order.
struct ModulesReloadedEventParams : public lua::EventBase
{
    Array<STDString> Previous;
    Array<STDString> Current;
};

// GateKit: a client timeline's playback state changed (eoc::TimelineDataComponent.PlaybackState, as
// the game's timeline system left it): the main menu's camera descent, a cinematic, a mod's own.
struct TimelinePlaybackEventParams : public lua::EventBase
{
    // The timeline resource's UUID and its type (the game's TimelineType).
    STDString Timeline;
    STDString Type;
    // The playback state before and now; -1: the timeline was not there / is gone.
    int32_t From;
    int32_t To;
};

// GateKit: the game showed or hid its loading screen (ls::LoadingScreenManager's visible flag).
struct LoadingScreenEventParams : public lua::EventBase
{
    bool Visible;
};

// GateKit: a state of the interface's state machine was entered or left (the loading screen's
// "Loading" left is when the player sees a load end).
struct UIStateChangedEventParams : public lua::EventBase
{
    STDString State;
    STDString Parent;
    int32_t Depth;
    bool Entered;
};

END_NS()
