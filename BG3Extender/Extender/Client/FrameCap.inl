#pragma once

#include <GameDefinitions/Base/Base.h>
#include <GameDefinitions/Symbols.h>

BEGIN_NS(ecl)

// How fast the game is allowed to draw, and the throttle it puts itself under when its window is
// not the one you are looking at.
//
// The engine has had this all along. `BaseApp` keeps three sixteen-bit numbers - the cap that was
// asked for, the cap that is in force, and the cap to fall back to when the window loses focus -
// and derives from them one number in its clock: how many microseconds a frame must last.
// `ls::GameClock::SetFrameCap` is the whole of it:
//
//     SetFrameCap(fps):  MicrosecondsPerFrame = fps ? (int64_t)(1000000.0f / fps) : 0
//
// and `BaseApp::SetFrameCap` picks which fps to hand it:
//
//     FrameCap = fps
//     if (GlobalSwitches->YieldOnLostFocus && !HasFocus) {
//         EffectiveFrameCap = FrameCap ? min(FrameCap, LostFocusFrameCap) : LostFocusFrameCap;
//     } else {
//         EffectiveFrameCap = FrameCap;
//     }
//     Clock.SetFrameCap(EffectiveFrameCap)
//
// Two things follow, and they are why nothing here hooks or patches anything:
//
//   * **LostFocusFrameCap = 0 turns the background throttle off**, whatever `YieldOnLostFocus`
//     says, because zero means "no cap" at every step of that calculation. The console command
//     `YieldOnLostFocus off` is a second way to the same place, and needs a byte of
//     `GlobalSwitches` whose offset nobody has checked; this needs only a field the Toolkit
//     exports an accessor for.
//   * **a cap that holds in the background is FrameCap and LostFocusFrameCap set together**, since
//     the smaller of the two wins when the window is not in front.
//
// Where all that was read: the Toolkit ships the same engine as DLLs that export their C++ classes
// by name, and these five accessors are each two or three instructions long (Baldur's Gate 3
// Toolkit 4.69.31.813, `GameEngine.dll`, `CoreLib.dll`):
//
//     BaseApp::GetFrameCap            movzx eax, word ptr [rcx+0x1C4]
//     BaseApp::SetFrameCap            mov word ptr [rcx+0x1C4], dx ... mov word ptr [rbx+0x1C6], dx
//     BaseApp::SetLostFocusFrameCap   mov word ptr [rcx+0x1C8], dx
//     BaseApp::GetHasFocus            [rcx+0x1C2]
//     BaseApp::GetClock               this+0xE0
//     ls::GameClock::SetFrameCap      [this+0x18] = fps ? 1000000.0f / fps : 0
//
// The game is a *different build* of that engine, and its `BaseApp` is the shorter of the two: the
// three caps sit at 0x14C rather than 0x1C4 and the clock's microseconds at 0xE8 rather than 0xF8.
// The offsets below are the game's, found by asking the running game where its own arithmetic comes
// out right - the only pair of a sixteen-bit cap and a sixty-four-bit microsecond count anywhere in
// the object for which `microseconds == 1000000.0f / cap` - and then read off against the order the
// Toolkit's accessors give.
//
// That same identity is the guard here. Before any write, `Probe` checks that it still holds; if it
// does not, the layout has moved and this does nothing at all rather than writing somewhere
// unknown.
struct FrameCapAccess
{
    // Game 4.76.31.656 (bg3.exe, bg3_dx11.exe). The Toolkit's own offsets, for comparison, are
    // 0x1C2 / 0x1C4 / 0x1C6 / 0x1C8 and 0xE0 + 0x18.
    static constexpr uintptr_t OffHasFocus = 0x14A;
    static constexpr uintptr_t OffFrameCap = 0x14C;
    static constexpr uintptr_t OffEffectiveFrameCap = 0x14E;
    static constexpr uintptr_t OffLostFocusFrameCap = 0x150;
    static constexpr uintptr_t OffClockMicrosecondsPerFrame = 0xE8;

    // The most frames a second anyone has a use for; a cap above this is refused as a typo.
    static constexpr uint32_t MaxFrameCap = 1000;

    uint8_t* App{ nullptr };

    static int64_t MicrosecondsFor(uint16_t fps)
    {
        return fps ? (int64_t)(1000000.0f / (float)fps) : 0;
    }

    template <class T>
    T& At(uintptr_t offset) const
    {
        return *(T*)(App + offset);
    }

    uint16_t FrameCap() const { return At<uint16_t>(OffFrameCap); }
    uint16_t EffectiveFrameCap() const { return At<uint16_t>(OffEffectiveFrameCap); }
    uint16_t LostFocusFrameCap() const { return At<uint16_t>(OffLostFocusFrameCap); }
    bool HasFocus() const { return At<uint8_t>(OffHasFocus) != 0; }
    int64_t MicrosecondsPerFrame() const { return At<int64_t>(OffClockMicrosecondsPerFrame); }

    // The app, if it is there and the layout above still describes it. Null otherwise.
    static std::optional<FrameCapAccess> Probe()
    {
        auto app = GetStaticSymbols().AppInstance;
        if (!app || !*app) return {};

        FrameCapAccess access{ (uint8_t*)*app };
        auto effective = access.EffectiveFrameCap();
        if (effective > MaxFrameCap) return {};
        if (access.FrameCap() > MaxFrameCap) return {};
        if (access.LostFocusFrameCap() > MaxFrameCap) return {};
        if (access.MicrosecondsPerFrame() != MicrosecondsFor(effective)) return {};
        return access;
    }

    // What `BaseApp::SetFrameCap` does, written out: the cap in force follows the two caps and the
    // focus, and the clock follows the cap in force.
    void Apply() const
    {
        uint16_t cap = FrameCap();
        uint16_t effective = cap;
        if (YieldOnLostFocus() && !HasFocus()) {
            uint16_t lost = LostFocusFrameCap();
            effective = (cap != 0 && lost >= cap) ? cap : lost;
        }

        At<uint16_t>(OffEffectiveFrameCap) = effective;
        At<int64_t>(OffClockMicrosecondsPerFrame) = MicrosecondsFor(effective);
    }

    void SetFrameCap(uint16_t fps) const
    {
        At<uint16_t>(OffFrameCap) = fps;
        Apply();
    }

    void SetLostFocusFrameCap(uint16_t fps) const
    {
        At<uint16_t>(OffLostFocusFrameCap) = fps;
        Apply();
    }

    // The engine's own switch for the background throttle, which the console command
    // `YieldOnLostFocus on|off` writes: a byte of GlobalSwitches at +0x23, which
    // `BaseApp::SetFrameCap` and `BaseApp::CommandYieldOnLostFocus` both read there. Only read
    // here, and only to say what the engine thinks: everything this file changes it changes
    // through the frame caps, which are fields the Toolkit names.
    static bool YieldOnLostFocus()
    {
        auto switches = GetStaticSymbols().GetGlobalSwitches();
        if (!switches) return true;

        auto flag = *((uint8_t*)switches + 0x23);
        // Not a flag any more if it ever stops being one; assume the engine throttles, which is
        // the cautious answer - a cap set for the background then still holds.
        return flag <= 1 ? flag != 0 : true;
    }
};

END_NS()
