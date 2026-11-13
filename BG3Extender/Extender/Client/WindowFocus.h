#pragma once

#include <GameDefinitions/Base/Base.h>

#include <string>
#include <vector>

BEGIN_NS(ecl::windowfocus)

// Who asked for the game's window to be brought to the front, and when.
//
// A copy of the game started by a tool should not take the keyboard from the person using the
// machine. Most of that is settled without any code here: the game is built on SDL, and SDL shows
// its window with SW_SHOWNA instead of SW_SHOW when it is told
// SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN, so showing the window activates nothing.
//
// Showing is not the only way in. SDL_RaiseWindow calls SetForegroundWindow outright, with no
// hint consulted and no condition, and the game is free to call it itself besides. Whether the
// call then succeeds is Windows' decision, by rules about who had the last input and whether the
// foreground lock has timed out - which is why a copy steals the keyboard when the machine is
// idle and behaves when somebody is working, and why it took so long to pin down.
//
// So the call is watched. Every attempt on the game's own window is recorded with the address of
// whoever made it, resolved to a module and an offset, which answers "who raises this window" with
// evidence rather than a guess. While `KeepInBackground` is on, the attempt is refused instead of
// passed on, and the record says so.
//
// Refusing is narrow on purpose: only SetForegroundWindow and SetActiveWindow, only from inside
// this process, only while the setting is on, and the window is never hidden, moved or disabled.
// A person who clicks the window still gets it.
struct Attempt
{
    double Seconds;        // since the hooks were installed
    uint64_t Window;       // the HWND asked for
    std::string Caller;    // module+0xRVA of the code that asked
    std::string Function;  // which call it was
    bool Refused;
};

// Installs the hooks. Safe to call once; later calls do nothing.
void EnableHooks();
void DisableHooks();

// Whether attempts to bring this copy's window to the front are refused rather than passed on.
//
// It is worth little on its own, and that is a measurement rather than an opinion: over a whole
// eighty-second start this game never once called SetForegroundWindow or SetActiveWindow. What
// actually puts a window in front is Windows itself - the first top-level window of a process
// when it is shown, and the next window in line when the one that had the keyboard goes away -
// and no call is made in either case, so there is nothing to refuse. The showing half is settled
// outside the game entirely, by SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN in its environment.
//
// So this stays for what it does reliably: it records. Every attempt is written down whether or
// not it is refused, which is how the above was learned and how it would be learned again if a
// patch to the game changed it.
//
// (WS_EX_NOACTIVATE was tried here and taken out again. It did stop a click from activating the
// window - and took the window out of the taskbar and made it impossible to raise by hand, which
// the owner noticed within a minute - while the system still handed it the foreground anyway. A
// style that costs the person their window and buys nothing is not a trade worth making.)
void KeepInBackground(bool enabled);
bool KeepingInBackground();

// Whether this copy's window is put where no screen reaches, the moment it is created.
//
// This is the answer to "it appears over my work", and it is the only one that is in time: the
// window is moved before it has ever been shown, so there is no frame in which it is on a screen.
// It stays a shown window, so everything that looks for it still finds it - a picture of the
// frame, a key posted to it, a deck listing the copies - it is simply nowhere a screen covers.
//
// For a copy a deck is about to take over, which is going to draw it in a tile anyway. A copy
// started to be looked at directly should not have this.
void KeepOffScreen(bool enabled);

// The game's own window, as it is created. Called from the SDL_CreateWindow hook, before the
// window has ever been shown - which is the only moment that is early enough to matter.
void WindowCreated(void* hwnd);

// What has been attempted so far, oldest first.
std::vector<Attempt> Attempts();

END_NS()
