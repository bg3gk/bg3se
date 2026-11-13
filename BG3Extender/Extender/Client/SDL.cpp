#include <stdafx.h>
#include <Extender/ScriptExtender.h>
#include <Extender/Client/ExtensionStateClient.h>
#include <Extender/Client/SDLManager.h>
#include <CoreLib/Wrappers.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <detours.h>
#include <SDL_syswm.h>
#include <Extender/Client/WindowFocus.inl>

BEGIN_SE()

SDLManager::SDLManager()
{}

SDLManager::~SDLManager()
{
    DestroyUI();
    DisableHooks();
}

void SDLManager::EnableHooks()
{
    // Before the window exists, because the first attempt to bring it to the front can come at
    // any moment after that. What it does is settled by KeepWindowInBackground; watching costs
    // nothing either way, and the record is what answers "who raises this window".
    ecl::windowfocus::EnableHooks();
    ecl::windowfocus::KeepInBackground(gExtender->GetConfig().KeepWindowInBackground);
    ecl::windowfocus::KeepOffScreen(gExtender->GetConfig().KeepWindowOffScreen);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    CreateWindowHook_.Wrap(ResolveFunctionTrampoline(&SDL_CreateWindow));
    PollEventHook_.Wrap(ResolveFunctionTrampoline(&SDL_PollEvent));
    IsTextInputActiveHook_.Wrap(ResolveFunctionTrampoline(&SDL_IsTextInputActive));
    StartTextInputHook_.Wrap(ResolveFunctionTrampoline(&SDL_StartTextInput));
    StopTextInputHook_.Wrap(ResolveFunctionTrampoline(&SDL_StopTextInput));
    DetourTransactionCommit();

    CreateWindowHook_.SetPostHook(&SDLManager::SDLCreateWindowHooked, this);
    PollEventHook_.SetWrapper(&SDLManager::SDLPollEventHooked, this);
    IsTextInputActiveHook_.SetPostHook(&SDLManager::SDLIsTextInputActiveHooked, this);
    StartTextInputHook_.SetWrapper(&SDLManager::SDLStartTextInputHooked, this);
    StopTextInputHook_.SetWrapper(&SDLManager::SDLStopTextInputHooked, this);
}

void SDLManager::DisableHooks()
{
    ecl::windowfocus::DisableHooks();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    CreateWindowHook_.Unwrap();
    PollEventHook_.Unwrap();
    StartTextInputHook_.Unwrap();
    StopTextInputHook_.Unwrap();
    DetourTransactionCommit();
}

void SDLManager::InitializeUI()
{
    if (enableUI_) return;

    ImGui_ImplSDL2_InitForVulkan(window_);
    enableUI_ = true;
}

void SDLManager::DestroyUI()
{
    if (!enableUI_) return;

    enableUI_ = false;
    ImGui_ImplSDL2_Shutdown();
}

void SDLManager::NewFrame()
{
    ImGui_ImplSDL2_NewFrame();
}

void SDLManager::InjectEvent(SDL_Event const& evt)
{
    std::lock_guard _(injectedEventMutex_);
    injectedEvents_.push_back(evt);
}

void SDLManager::SDLCreateWindowHooked(const char* title,
    int x, int y, int w,
    int h, uint32_t flags,
    SDL_Window* window)
{
    window_ = window;

    // The window exists and has never been shown: SDL creates it without WS_VISIBLE and shows it
    // later. That makes this the one moment at which the window can be told not to be a candidate
    // for activation in time to matter - a window shown for the first time is activated by the
    // system itself, with nobody calling anything.
    if (window != nullptr) {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(window, &info) == SDL_TRUE && info.subsystem == SDL_SYSWM_WINDOWS) {
            ecl::windowfocus::WindowCreated(info.info.win.window);
        }
    }
}

int SDLManager::SDLPollEventHooked(SDLPollEventProc* wrapped, SDL_Event* event)
{
    while (!injectedEvents_.empty()) {
        {
            std::lock_guard _(injectedEventMutex_);
            if (injectedEvents_.empty()) break;
            *event = *injectedEvents_.begin();
            injectedEvents_.ordered_remove_at(0);
        }

        // An injected event the UI takes (typing into a text field, a click on a window)
        // is not passed on to the game, the same as a real one
        if (!InjectedEventConsumedByUI(event)) {
            return 1;
        }
    }

    int result = wrapped(event);
    result = SDLPollEventSEH(event, result);

    return result;
}

// Shows an injected event to the extender UI. Lua input events are not raised for it:
// the event came from Lua in the first place.
bool SDLManager::InjectedEventConsumedByUI(SDL_Event* event)
{
    if (!enableUI_) return false;

    bool keyboard = event->type == SDL_KEYDOWN || event->type == SDL_KEYUP || event->type == SDL_TEXTINPUT;
    bool mouse = event->type == SDL_MOUSEMOTION || event->type == SDL_MOUSEBUTTONDOWN
        || event->type == SDL_MOUSEBUTTONUP || event->type == SDL_MOUSEWHEEL;
    if (!keyboard && !mouse) return false;

    // The backend ignores events that do not name its window; injected ones may carry no window
    SDL_Event uiEvent = *event;
    auto windowId = SDL_GetWindowID(window_);
    switch (uiEvent.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: uiEvent.key.windowID = windowId; break;
    case SDL_TEXTINPUT: uiEvent.text.windowID = windowId; break;
    case SDL_MOUSEMOTION: uiEvent.motion.windowID = windowId; break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: uiEvent.button.windowID = windowId; break;
    case SDL_MOUSEWHEEL: uiEvent.wheel.windowID = windowId; break;
    }

    {
        std::lock_guard _(mutex_);
        ImGui_ImplSDL2_ProcessEvent(&uiEvent);
    }

    ImGuiIO& io = ImGui::GetIO();
    return (keyboard && io.WantTextInput) || (mouse && io.WantCaptureMouse);
}

int SDLManager::SDLPollEventSEH(SDL_Event* event, int result)
{
    BEGIN_GUARDED()
    return SDLPollEventInternal(event, result);
    END_GUARDED()
    return 0;
}

int SDLManager::SDLPollEventInternal(SDL_Event* event, int result)
{
    gExtender->OnSDLEvent(event);

    if (enableUI_ && result == 1) {
        {
            std::lock_guard _(mutex_);
            ImGui_ImplSDL2_ProcessEvent(event);
        }

        ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput
            && (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP || event->type == SDL_TEXTEDITING || event->type == SDL_TEXTINPUT)) {
            result = 0;
        }

        if (io.WantCaptureMouse
            && (event->type == SDL_MOUSEMOTION || event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP || event->type == SDL_MOUSEWHEEL)) {
            result = 0;
        }
    }

    if (result == 1 && gExtender->GetClient().HasExtensionState()) {
        ecl::ExtensionState::Get().OnInputEvent(event, result);
    }

    return result;
}

void SDLManager::SDLIsTextInputActiveHooked(SDL_bool active)
{
    if (enableUI_ && ImGui::GetIO().WantTextInput && !active) {
        SDL_StartTextInput();
    }
}

void SDLManager::SDLStartTextInputHooked(SDLStartTextInputProc* wrapped)
{
    wrapped();
}

void SDLManager::SDLStopTextInputHooked(SDLStartTextInputProc* wrapped)
{
    if (enableUI_ && ImGui::GetIO().WantTextInput) {
        if (!SDL_IsTextInputActive()) {
            SDL_StartTextInput();
        }
    } else {
        wrapped();
    }
}

END_SE()
