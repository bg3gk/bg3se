#pragma once

#include <Extender/Client/WindowFocus.h>
#include <CoreLib/Wrappers.h>
#include <detours.h>
#include <mutex>

// The hooks themselves live in bg3se, beside every other one: WrappableFunction's gHook is a
// static member of a template of that namespace, and a static member can only be defined in the
// namespace its class belongs to. Everything else belongs in ecl::windowfocus.
BEGIN_SE()

using SetForegroundWindowProc = BOOL(HWND);
using SetActiveWindowProc = HWND(HWND);

enum class SetForegroundWindowHookTag {};
enum class SetActiveWindowHookTag {};
using SetForegroundWindowHookType = WrappableFunction<SetForegroundWindowHookTag, SetForegroundWindowProc>;
using SetActiveWindowHookType = WrappableFunction<SetActiveWindowHookTag, SetActiveWindowProc>;
SetForegroundWindowHookType* SetForegroundWindowHookType::gHook;
SetActiveWindowHookType* SetActiveWindowHookType::gHook;

END_SE()

BEGIN_NS(ecl::windowfocus)

// GameDefinitions/Base/WindowsSlim.h trims windows.h with NOWINSTYLES and NOWINOFFSETS, so the
// window styles and the long-pointer accessors are not declared anywhere in this tree. They are
// written out here rather than widening that header for everybody: three documented constants and
// two exports of user32, each with the value and signature Microsoft publishes.
extern "C" {
    __declspec(dllimport) LONG_PTR __stdcall GetWindowLongPtrW(HWND window, int index);
    __declspec(dllimport) LONG_PTR __stdcall SetWindowLongPtrW(HWND window, int index, LONG_PTR value);
    __declspec(dllimport) BOOL __stdcall SetWindowPos(HWND window, HWND after, int x, int y,
        int cx, int cy, UINT flags);
}

constexpr int GWLExStyle = -20;                       // GWL_EXSTYLE
constexpr LONG_PTR WsExToolWindow = 0x00000080L;      // WS_EX_TOOLWINDOW: and so out of the taskbar
constexpr LONG_PTR WsExAppWindow = 0x00040000L;       // WS_EX_APPWINDOW: and so into it
constexpr UINT SwpNoSize = 0x0001, SwpNoZOrder = 0x0004, SwpNoActivate = 0x0010, SwpNoOwnerZOrder = 0x0200;

// Far enough that no arrangement of screens reaches it. The deck uses the same number from the
// outside; this is the same move made early enough that the window is never seen at all.
constexpr int FarAway = 30000;

namespace
{
    SetForegroundWindowHookType gSetForegroundWindowHook;
    SetActiveWindowHookType gSetActiveWindowHook;
    HMODULE gUser32{ nullptr };
    bool gInstalled{ false };
    bool gQuiet{ false };
    bool gOffScreen{ false };
    HWND gWindow{ nullptr };

    std::mutex gRecordMutex;
    std::vector<Attempt> gAttempts;
    LARGE_INTEGER gStart{};
    LARGE_INTEGER gFrequency{};

    // The most that is kept. A copy that asks a thousand times has told us everything it is going
    // to; what matters is the first few, which is where the answer to "who" lives.
    constexpr size_t MaxAttempts = 64;

    double SecondsSinceStart()
    {
        if (gFrequency.QuadPart == 0) return 0.0;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (double)(now.QuadPart - gStart.QuadPart) / (double)gFrequency.QuadPart;
    }

    // "GameEngine.dll+0x1a2b3c" for an address inside a loaded module, or its hex if it is in none.
    std::string Describe(void* address)
    {
        HMODULE module{ nullptr };
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)address, &module) && module != nullptr) {
            wchar_t path[MAX_PATH];
            auto length = GetModuleFileNameW(module, path, MAX_PATH);
            if (length > 0) {
                std::wstring file(path, length);
                auto slash = file.find_last_of(L'\\');
                if (slash != std::wstring::npos) file = file.substr(slash + 1);
                char line[MAX_PATH + 32];
                sprintf_s(line, "%S+0x%llx", file.c_str(),
                    (unsigned long long)((uintptr_t)address - (uintptr_t)module));
                return line;
            }
        }
        char line[32];
        sprintf_s(line, "0x%llx", (unsigned long long)(uintptr_t)address);
        return line;
    }

    // Who asked, as far up the stack as it takes to leave our own code.
    //
    // The obvious `_ReturnAddress()` is no use here: a detoured call arrives through the
    // extender's own dispatcher, so the immediate caller is always this DLL. The stack is walked
    // instead and the first few frames outside this module are reported, innermost first - which
    // is SDL2.dll or the game itself, the thing worth knowing.
    std::string Callers()
    {
        void* frames[24];
        auto taken = CaptureStackBackTrace(1, 24, frames, nullptr);
        HMODULE self{ nullptr };
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&SecondsSinceStart, &self);
        std::string out;
        int shown = 0;
        for (USHORT i = 0; i < taken && shown < 3; i++) {
            HMODULE module{ nullptr };
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)frames[i], &module);
            if (module != nullptr && module == self) continue;
            if (!out.empty()) out += " < ";
            out += Describe(frames[i]);
            shown++;
        }
        return out.empty() ? "unknown" : out;
    }

    void Record(char const* function, HWND window, bool refused)
    {
        std::lock_guard _(gRecordMutex);
        if (gAttempts.size() >= MaxAttempts) return;
        gAttempts.push_back(Attempt{ SecondsSinceStart(), (uint64_t)(uintptr_t)window,
            Callers(), function, refused });
    }

    BOOL SetForegroundWindowHooked(SetForegroundWindowProc* wrapped, HWND window)
    {
        auto refuse = gQuiet;
        Record("SetForegroundWindow", window, refuse);
        // FALSE is what Windows itself returns when it refuses a foreground change, which it does
        // often and for its own reasons, so every caller already has to cope with it.
        if (refuse) return FALSE;
        return wrapped(window);
    }

    HWND SetActiveWindowHooked(SetActiveWindowProc* wrapped, HWND window)
    {
        auto refuse = gQuiet;
        Record("SetActiveWindow", window, refuse);
        if (refuse) return nullptr;
        return wrapped(window);
    }
}

void EnableHooks()
{
    if (gInstalled) return;
    gUser32 = LoadLibraryW(L"user32.dll");
    if (gUser32 == nullptr) {
        ERR("Could not load user32.dll; the game's window is on its own");
        return;
    }

    QueryPerformanceFrequency(&gFrequency);
    QueryPerformanceCounter(&gStart);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    gSetForegroundWindowHook.Wrap(gUser32, "SetForegroundWindow");
    gSetActiveWindowHook.Wrap(gUser32, "SetActiveWindow");
    DetourTransactionCommit();

    gSetForegroundWindowHook.SetWrapper(&SetForegroundWindowHooked);
    gSetActiveWindowHook.SetWrapper(&SetActiveWindowHooked);
    gInstalled = true;
}

void DisableHooks()
{
    if (!gInstalled) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    gSetForegroundWindowHook.Unwrap();
    gSetActiveWindowHook.Unwrap();
    DetourTransactionCommit();
    gInstalled = false;
}

namespace
{
    // Out of sight: off every screen and out of the taskbar, with the window left shown so that
    // everything which looks for it still finds it. Nothing is hidden and nothing is disabled -
    // a hidden window cannot be found by the tools that read a copy, and a disabled one cannot be
    // played in.
    void ApplyPlace()
    {
        if (gWindow == nullptr || !IsWindow(gWindow) || !gOffScreen) return;
        auto style = GetWindowLongPtrW(gWindow, GWLExStyle);
        SetWindowLongPtrW(gWindow, GWLExStyle, (style & ~WsExAppWindow) | WsExToolWindow);
        SetWindowPos(gWindow, nullptr, FarAway, FarAway, 0, 0,
            SwpNoSize | SwpNoZOrder | SwpNoActivate | SwpNoOwnerZOrder);
    }
}

void KeepInBackground(bool enabled)
{
    gQuiet = enabled;
}

bool KeepingInBackground()
{
    return gQuiet;
}

void KeepOffScreen(bool enabled)
{
    gOffScreen = enabled;
    ApplyPlace();
}

void WindowCreated(void* hwnd)
{
    gWindow = (HWND)hwnd;
    ApplyPlace();
}

std::vector<Attempt> Attempts()
{
    std::lock_guard _(gRecordMutex);
    return gAttempts;
}

END_NS()
