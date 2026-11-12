#include <Extender/ScriptExtender.h>

/// <lua_module>Input</lua_module>
BEGIN_NS(ecl::lua::input)

void InjectKeyEvent(Uint32 type, Uint8 state, SDLScanCode key, std::optional<SDLKeyModifier> modifiers)
{
    SDL_Event evt;
    memset(&evt, 0, sizeof(evt));

    evt.type = type;
    evt.key.timestamp = SDL_GetTicks();
    evt.key.windowID = 0;
    evt.key.state = state;
    evt.key.repeat = 0;
    evt.key.keysym.scancode = (SDL_Scancode)key;
    // The key code as the keyboard layout gives it; SDL_SCANCODE_TO_KEYCODE() is only right for
    // keys without a character (arrows, F-keys), not for Tab, Return, Escape or letters
    evt.key.keysym.sym = SDL_GetKeyFromScancode((SDL_Scancode)key);
    evt.key.keysym.mod = modifiers ? (Uint16)*modifiers : 0;

    gExtender->GetClient().GetSDL().InjectEvent(evt);
}

void InjectKeyPress(SDLScanCode key, std::optional<SDLKeyModifier> modifiers)
{
    InjectKeyEvent(SDL_KEYDOWN, SDL_PRESSED, key, modifiers);
    InjectKeyEvent(SDL_KEYUP, SDL_RELEASED, key, modifiers);
}

void InjectKeyDown(SDLScanCode key, std::optional<SDLKeyModifier> modifiers)
{
    InjectKeyEvent(SDL_KEYDOWN, SDL_PRESSED, key, modifiers);
}

void InjectKeyUp(SDLScanCode key, std::optional<SDLKeyModifier> modifiers)
{
    InjectKeyEvent(SDL_KEYUP, SDL_RELEASED, key, modifiers);
}

// Types text as the keyboard would: into the focused IMGUI text field if there is one,
// otherwise to the game
void InjectText(StringView text)
{
    // An SDL text event carries at most 31 bytes; longer text is split on UTF-8 character boundaries
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto len = std::min<std::size_t>(text.size() - pos, SDL_TEXTINPUTEVENT_TEXT_SIZE - 1);
        while (len > 0 && pos + len < text.size() && ((uint8_t)text[pos + len] & 0xC0) == 0x80) {
            len--;
        }
        if (len == 0) break;

        SDL_Event evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = SDL_TEXTINPUT;
        evt.text.timestamp = SDL_GetTicks();
        memcpy(evt.text.text, text.data() + pos, len);
        gExtender->GetClient().GetSDL().InjectEvent(evt);
        pos += len;
    }
}

// Moves the mouse to a position in window pixels
void InjectMouseMove(int x, int y)
{
    SDL_Event evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = SDL_MOUSEMOTION;
    evt.motion.timestamp = SDL_GetTicks();
    evt.motion.x = x;
    evt.motion.y = y;
    gExtender->GetClient().GetSDL().InjectEvent(evt);
}

// button: 1 = left, 2 = middle, 3 = right. With a position the mouse is moved there first.
void InjectMouseButton(int button, bool pressed, std::optional<int> x, std::optional<int> y)
{
    if (x && y) {
        InjectMouseMove(*x, *y);
    }

    SDL_Event evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = pressed ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    evt.button.timestamp = SDL_GetTicks();
    evt.button.button = (Uint8)button;
    evt.button.state = pressed ? SDL_PRESSED : SDL_RELEASED;
    evt.button.clicks = 1;
    if (x && y) {
        evt.button.x = *x;
        evt.button.y = *y;
    }
    gExtender->GetClient().GetSDL().InjectEvent(evt);
}

void InjectMouseWheel(float x, float y)
{
    SDL_Event evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = SDL_MOUSEWHEEL;
    evt.wheel.timestamp = SDL_GetTicks();
    evt.wheel.x = (Sint32)x;
    evt.wheel.y = (Sint32)y;
    evt.wheel.preciseX = x;
    evt.wheel.preciseY = y;
    gExtender->GetClient().GetSDL().InjectEvent(evt);
}

bg3se::input::InputManager* GetInputManager()
{
    return *GetStaticSymbols().ls__gInputManager;
}


void RegisterInputLib()
{
    DECLARE_MODULE(Input, Client)
    BEGIN_MODULE()
    MODULE_FUNCTION(InjectKeyPress)
    MODULE_FUNCTION(InjectKeyDown)
    MODULE_FUNCTION(InjectKeyUp)
    MODULE_FUNCTION(InjectText)
    MODULE_FUNCTION(InjectMouseMove)
    MODULE_FUNCTION(InjectMouseButton)
    MODULE_FUNCTION(InjectMouseWheel)
    MODULE_FUNCTION(GetInputManager)
    END_MODULE()
}

END_NS()
