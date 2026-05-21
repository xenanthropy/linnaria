#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include <cstring>
#include <cstdint>
#include <string>
#include "GuestMemory.hpp"
#include "ElfLoader.hpp"
#include <dynarmic/interface/A32/a32.h>

// Forward decl -- actual definition lives in main.cpp. verbose=false
// suppresses the per-call "[Boot] Executing ..." banner so input dispatch
// doesn't drown the console at 60 Hz.
void ExecuteGameFunction(Dynarmic::A32::Jit& cpu, GuestMemory& memory, ElfLoader& loader,
                         const std::string& func_name, const std::vector<uint32_t>& args,
                         bool verbose = true);

namespace Input {

    // Octarine touch action codes (see OctarineView.onTouchEvent in the Java
    // bridge). They line up with Android MotionEvent.ACTION_DOWN/UP/MOVE
    // values, not the raw constants.
    enum TouchAction : int { DOWN = 0, UP = 1, MOVE = 2 };

    struct TouchEvt { int id; int action; float x; float y; float time; };
    struct KeyEvt   { int action; uint32_t unicode; int keyCode; };

    // Pending events accumulate here whenever SDL polls them and drain into
    // native calls at a safe point between guest ticks (see DrainPending).
    inline std::vector<TouchEvt> touch_queue;
    inline std::vector<KeyEvt>   key_queue;

    // Tracks left-mouse-down across motion events so we only synthesize
    // touch MOVE while the "finger" is actually down (matching Android,
    // which can't generate hover-MOVE on a touchscreen).
    inline bool mouse_held = false;

    // SDL keysym -> Android KeyEvent.KEYCODE_*. Values from the AOSP
    // KeyEvent source. Unmapped keys return 0 and are skipped at dispatch.
    inline int SDLKeyToAndroid(SDL_Keycode sym) {
        switch (sym) {
            // Letters
            case SDLK_a: return 29; case SDLK_b: return 30; case SDLK_c: return 31;
            case SDLK_d: return 32; case SDLK_e: return 33; case SDLK_f: return 34;
            case SDLK_g: return 35; case SDLK_h: return 36; case SDLK_i: return 37;
            case SDLK_j: return 38; case SDLK_k: return 39; case SDLK_l: return 40;
            case SDLK_m: return 41; case SDLK_n: return 42; case SDLK_o: return 43;
            case SDLK_p: return 44; case SDLK_q: return 45; case SDLK_r: return 46;
            case SDLK_s: return 47; case SDLK_t: return 48; case SDLK_u: return 49;
            case SDLK_v: return 50; case SDLK_w: return 51; case SDLK_x: return 52;
            case SDLK_y: return 53; case SDLK_z: return 54;
            // Digits (top row)
            case SDLK_0: return 7;  case SDLK_1: return 8;  case SDLK_2: return 9;
            case SDLK_3: return 10; case SDLK_4: return 11; case SDLK_5: return 12;
            case SDLK_6: return 13; case SDLK_7: return 14; case SDLK_8: return 15;
            case SDLK_9: return 16;
            // Whitespace / control
            case SDLK_SPACE:     return 62;
            case SDLK_RETURN:    return 66;
            case SDLK_BACKSPACE: return 67;
            case SDLK_TAB:       return 61;
            case SDLK_ESCAPE:    return 111;
            // Arrows
            case SDLK_LEFT:      return 21;
            case SDLK_RIGHT:     return 22;
            case SDLK_UP:        return 19;
            case SDLK_DOWN:      return 20;
            // Modifiers
            case SDLK_LSHIFT:    return 59;
            case SDLK_RSHIFT:    return 60;
            case SDLK_LCTRL:     return 113;
            case SDLK_RCTRL:     return 114;
            case SDLK_LALT:      return 57;
            case SDLK_RALT:      return 58;
            // Punctuation that has stable keycodes
            case SDLK_COMMA:     return 55;
            case SDLK_PERIOD:    return 56;
            case SDLK_MINUS:     return 69;
            case SDLK_EQUALS:    return 70;
            case SDLK_LEFTBRACKET:  return 71;
            case SDLK_RIGHTBRACKET: return 72;
            case SDLK_BACKSLASH: return 73;
            case SDLK_SEMICOLON: return 74;
            case SDLK_QUOTE:     return 75;
            case SDLK_SLASH:     return 76;
            default: return 0;
        }
    }

    // Translate one SDL event into a queued action. Safe to call any time;
    // does not touch guest state.
    inline void HandleSDLEvent(const SDL_Event& ev) {
        // The Java side passes 1000 / event.getEventTime() as the 5th arg
        // to nativeTouchEvent. Mirror that here so the guest sees a value
        // in the same shape, even if the game ends up ignoring it.
        auto time_field = []() -> float {
            uint32_t t = SDL_GetTicks();
            return t ? (1000.0f / static_cast<float>(t)) : 0.0f;
        };

        switch (ev.type) {
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    mouse_held = true;
                    touch_queue.push_back({0, DOWN,
                        static_cast<float>(ev.button.x),
                        static_cast<float>(ev.button.y),
                        time_field()});
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    mouse_held = false;
                    touch_queue.push_back({0, UP,
                        static_cast<float>(ev.button.x),
                        static_cast<float>(ev.button.y),
                        time_field()});
                }
                break;
            case SDL_MOUSEMOTION:
                if (mouse_held) {
                    touch_queue.push_back({0, MOVE,
                        static_cast<float>(ev.motion.x),
                        static_cast<float>(ev.motion.y),
                        time_field()});
                }
                break;
            case SDL_KEYDOWN: {
                if (ev.key.repeat) break;
                int code = SDLKeyToAndroid(ev.key.keysym.sym);
                if (code == 0) break;
                uint32_t unicode = (ev.key.keysym.sym < 128)
                    ? static_cast<uint32_t>(ev.key.keysym.sym) : 0u;
                key_queue.push_back({0, unicode, code});
                break;
            }
            case SDL_KEYUP: {
                int code = SDLKeyToAndroid(ev.key.keysym.sym);
                if (code == 0) break;
                uint32_t unicode = (ev.key.keysym.sym < 128)
                    ? static_cast<uint32_t>(ev.key.keysym.sym) : 0u;
                key_queue.push_back({1, unicode, code});
                break;
            }
        }
    }

    // Drain queued events into native calls. Caller MUST ensure the main
    // thread is at a clean return point (last halt was UserDefined1) so
    // that re-using the stack at CODE_BASE + MEMORY_SIZE - 0x100000 inside
    // ExecuteGameFunction doesn't trample a paused nativeOnUpdate frame.
    inline void DrainPending(Dynarmic::A32::Jit& cpu, GuestMemory& memory,
                             ElfLoader& loader, uint32_t env_ptr) {
        if (touch_queue.empty() && key_queue.empty()) return;

        auto float_bits = [](float f) -> uint32_t {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        };

        for (const auto& t : touch_queue) {
            ExecuteGameFunction(cpu, memory, loader,
                "Java_com_codeglue_terraria_OctarineBridge_nativeTouchEvent",
                { env_ptr, 0,
                  static_cast<uint32_t>(t.id),
                  static_cast<uint32_t>(t.action),
                  float_bits(t.x),
                  float_bits(t.y),
                  float_bits(t.time) },
                /*verbose=*/false);
        }
        touch_queue.clear();

        for (const auto& k : key_queue) {
            ExecuteGameFunction(cpu, memory, loader,
                "Java_com_codeglue_terraria_OctarineBridge_nativeKeyEvent",
                { env_ptr, 0,
                  static_cast<uint32_t>(k.action),
                  k.unicode,
                  static_cast<uint32_t>(k.keyCode) },
                /*verbose=*/false);
        }
        key_queue.clear();
    }
}
