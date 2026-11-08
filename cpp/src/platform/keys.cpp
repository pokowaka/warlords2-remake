#include "platform/keys.hpp"

#include <set>

#include <SDL.h>

namespace keys {

namespace {
std::set<std::string> down;
}

void Press(const std::string& name) {
    down.insert(name);
}
void Release(const std::string& name) {
    down.erase(name);
}
void Clear() {
    down.clear();
}

bool IsDown(std::initializer_list<const char*> names) {
    for (const char* name : names)
        if (down.count(name)) return true;
    return false;
}
bool IsDown(const std::string& name) {
    return down.count(name) > 0;
}

std::string LoveName(int sdl_keycode) {
    switch (sdl_keycode) {
    case SDLK_LSHIFT:
        return "lshift";
    case SDLK_RSHIFT:
        return "rshift";
    case SDLK_LCTRL:
        return "lctrl";
    case SDLK_RCTRL:
        return "rctrl";
    case SDLK_LALT:
        return "lalt";
    case SDLK_RALT:
        return "ralt";
    case SDLK_LGUI:
        return "lgui";
    case SDLK_RGUI:
        return "rgui";
    case SDLK_RETURN:
        return "return";
    case SDLK_KP_ENTER:
        return "kpenter";
    case SDLK_ESCAPE:
        return "escape";
    case SDLK_BACKSPACE:
        return "backspace";
    case SDLK_TAB:
        return "tab";
    case SDLK_SPACE:
        return "space";
    case SDLK_DELETE:
        return "delete";
    case SDLK_INSERT:
        return "insert";
    case SDLK_HOME:
        return "home";
    case SDLK_END:
        return "end";
    case SDLK_PAGEUP:
        return "pageup";
    case SDLK_PAGEDOWN:
        return "pagedown";
    case SDLK_UP:
        return "up";
    case SDLK_DOWN:
        return "down";
    case SDLK_LEFT:
        return "left";
    case SDLK_RIGHT:
        return "right";
    case SDLK_KP_PLUS:
        return "kp+";
    case SDLK_KP_MINUS:
        return "kp-";
    case SDLK_KP_MULTIPLY:
        return "kp*";
    case SDLK_KP_DIVIDE:
        return "kp/";
    case SDLK_KP_PERIOD:
        return "kp.";
    case SDLK_KP_EQUALS:
        return "kp=";
    case SDLK_KP_0:
        return "kp0";
    case SDLK_KP_1:
        return "kp1";
    case SDLK_KP_2:
        return "kp2";
    case SDLK_KP_3:
        return "kp3";
    case SDLK_KP_4:
        return "kp4";
    case SDLK_KP_5:
        return "kp5";
    case SDLK_KP_6:
        return "kp6";
    case SDLK_KP_7:
        return "kp7";
    case SDLK_KP_8:
        return "kp8";
    case SDLK_KP_9:
        return "kp9";
    case SDLK_CAPSLOCK:
        return "capslock";
    case SDLK_NUMLOCKCLEAR:
        return "numlock";
    default:
        break;
    }
    if (sdl_keycode >= SDLK_F1 && sdl_keycode <= SDLK_F12)
        return "f" + std::to_string(sdl_keycode - SDLK_F1 + 1);
    if (sdl_keycode >= 32 && sdl_keycode < 127) {
        char chr = static_cast<char>(sdl_keycode);
        if (chr >= 'A' && chr <= 'Z') chr = static_cast<char>(chr - 'A' + 'a');
        return std::string(1, chr);
    }
    return "";
}

}  // namespace keys
