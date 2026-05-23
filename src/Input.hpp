#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include <deque>
#include <cstring>
#include <cstdint>
#include <string>
#include "GuestMemory.hpp"
#include "ElfLoader.hpp"
#include "Pacing.hpp"
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
    // deque so DrainPending can pop one from the front per frame -- the
    // game's native side (fjAddTouchEvent) appends to a fixed-size internal
    // queue that nativeOnUpdate drains each frame, so we must NOT dispatch
    // a burst or that buffer overflows and smashes a return address.
    inline std::deque<TouchEvt> touch_queue;
    inline std::deque<KeyEvt>   key_queue;

    // Tracks left-mouse-down across motion events so we only synthesize
    // touch MOVE while the "finger" is actually down (matching Android,
    // which can't generate hover-MOVE on a touchscreen).
    inline bool mouse_held = false;

    // Gamepad state. Mirrors the Java ShieldController fields one-for-one;
    // shipped to the engine via nativeGamePadUpdate (which builds a fresh
    // Gamepad each call, populates it, and feeds AndroidInterface::fjAddGamePad).
    // We emulate the Shield path because it's pure-Java keystroke aggregation
    // and the engine is already wired to consume it.
    struct Pad {
        int A = 0, B = 0, X = 0, Y = 0;
        int L1 = 0, L2 = 0, L3 = 0;
        int R1 = 0, R2 = 0, R3 = 0;
        int DpadUp = 0, DpadDown = 0, DpadLeft = 0, DpadRight = 0;
        int Start = 0;
        float AxisX = 0.0f, AxisY = 0.0f;   // left stick
        float AxisZ = 0.0f, AxisRZ = 0.0f;  // right stick
    };
    inline Pad pad_state;
    inline Pad pad_last_sent;
    inline bool pad_ever_sent = false;

    // Returns true if this key drives a gamepad field (regardless of whether
    // we changed state -- holding the key down past auto-repeat re-sets the
    // same value, but the on-change gate in SendGamepadUpdate filters dupes).
    // WASD drives both the d-pad and the analog stick: the engine tends to
    // use the stick for actor motion and the d-pad for UI nav, so feed both
    // and let it pick.
    inline bool ApplyPadKey(SDL_Keycode sym, bool down) {
        const int b = down ? 1 : 0;
        const float a_pos = down ?  1.0f : 0.0f;
        const float a_neg = down ? -1.0f : 0.0f;
        switch (sym) {
            case SDLK_w: pad_state.DpadUp    = b; pad_state.AxisY = a_neg; return true;
            case SDLK_s: pad_state.DpadDown  = b; pad_state.AxisY = a_pos; return true;
            case SDLK_a: pad_state.DpadLeft  = b; pad_state.AxisX = a_neg; return true;
            case SDLK_d: pad_state.DpadRight = b; pad_state.AxisX = a_pos; return true;
            case SDLK_SPACE:  pad_state.A     = b; return true;  // jump
            case SDLK_e:      pad_state.B     = b; return true;  // interact / use
            case SDLK_f:      pad_state.X     = b; return true;
            case SDLK_TAB:    pad_state.Y     = b; return true;  // inventory
            case SDLK_q:      pad_state.L1    = b; return true;  // hotbar prev
            case SDLK_r:      pad_state.R1    = b; return true;  // hotbar next
            case SDLK_ESCAPE: pad_state.Start = b; return true;  // pause
            default: return false;
        }
    }

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
    // does not touch guest state. Events are dropped (not queued) until the
    // game finishes booting -- see Pacing::input_enabled.
    inline void HandleSDLEvent(const SDL_Event& ev) {
        if (!Pacing::input_enabled.load(std::memory_order_relaxed)) {
            // Make sure we don't strand mouse_held=true if the user clicked
            // mid-boot: a stale "held" would synthesize a phantom MOVE the
            // instant input enables.
            if (ev.type == SDL_MOUSEBUTTONUP) mouse_held = false;
            return;
        }

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
                    // Coalesce: if the newest queued event is already a MOVE
                    // for this pointer, overwrite its position rather than
                    // appending. A fast drag generates far more motion events
                    // than the game can drain per frame; without this the
                    // queue (and the game's internal one) would burst.
                    if (!touch_queue.empty()
                        && touch_queue.back().action == MOVE
                        && touch_queue.back().id == 0) {
                        touch_queue.back().x = static_cast<float>(ev.motion.x);
                        touch_queue.back().y = static_cast<float>(ev.motion.y);
                        touch_queue.back().time = time_field();
                    } else {
                        touch_queue.push_back({0, MOVE,
                            static_cast<float>(ev.motion.x),
                            static_cast<float>(ev.motion.y),
                            time_field()});
                    }
                }
                break;
            case SDL_KEYDOWN: {
                if (ev.key.repeat) break;
                ApplyPadKey(ev.key.keysym.sym, true);

                int code = SDLKeyToAndroid(ev.key.keysym.sym);
                if (code == 0) break;

                uint32_t unicode;
                if (code == 66) {
                    unicode = '\n';
                    code = 0;
                } else if (ev.key.keysym.sym < 128) {
                    unicode = static_cast<uint32_t>(ev.key.keysym.sym);
                } else {
                    unicode = 0u;
                }

                key_queue.push_back({0, unicode, code});
                break;
            }
            case SDL_KEYUP: {
                ApplyPadKey(ev.key.keysym.sym, false);

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
    //
    // Dispatches AT MOST ONE touch and one key per call. DrainPending runs
    // once per main-loop iteration, and the main loop ticks nativeOnUpdate
    // right after -- so one-per-iteration guarantees the game drains its
    // internal touch queue between each fjAddTouchEvent. Dispatching the
    // whole backlog at once (the old behavior) overflowed that fixed-size
    // queue, corrupting a saved return address -> wild jump -> freeze.
    inline void DrainPending(Dynarmic::A32::Jit& cpu, GuestMemory& memory,
                             ElfLoader& loader, uint32_t env_ptr) {
        auto float_bits = [](float f) -> uint32_t {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        };

        if (!touch_queue.empty()) {
            const TouchEvt t = touch_queue.front();
            touch_queue.pop_front();
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

        if (!key_queue.empty()) {
            const KeyEvt k = key_queue.front();
            key_queue.pop_front();
            ExecuteGameFunction(cpu, memory, loader,
                "Java_com_codeglue_terraria_OctarineBridge_nativeKeyEvent",
                { env_ptr, 0,
                  static_cast<uint32_t>(k.action),
                  k.unicode,
                  static_cast<uint32_t>(k.keyCode) },
                /*verbose=*/false);
        }
    }

    // Push the current pad state to the engine via nativeGamePadUpdate. The
    // native side constructs a fresh Gamepad each call, calls SetConnected(true)
    // unconditionally, and feeds AndroidInterface::fjAddGamePad -- so there's
    // no "controller registration" handshake; every call is self-contained.
    //
    // Caller MUST guarantee main_thread_clean == true (same gate as DrainPending).
    //
    // We send only when state changed since the last call. The engine stores
    // the most recent Gamepad and uses it on subsequent frames, so a held key
    // produces continuous motion without re-sends.
    //
    // Arg ordering matches the Java OctarineBridge.nativeGamePadUpdate(...) call
    // in Gamepad.UpdateControllerData(). productVersion is the Shield's value (1).
    // Note: L2 and R2 are typed `int` in Java but read as `float` by the native
    // side -- a known mobile-port type mismatch that effectively zeroes those
    // triggers. We match Java's behavior and pass int bit-patterns.
    inline void SendGamepadUpdate(Dynarmic::A32::Jit& cpu, GuestMemory& memory,
                                  ElfLoader& loader, uint32_t env_ptr) {
        if (pad_ever_sent && std::memcmp(&pad_state, &pad_last_sent, sizeof(Pad)) == 0) {
            return; // nothing changed
        }

        auto fbits = [](float f) -> uint32_t {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        };

        ExecuteGameFunction(cpu, memory, loader,
            "Java_com_codeglue_terraria_OctarineBridge_nativeGamePadUpdate",
            {
                env_ptr,
                0u,                              // jclass (unused by native)
                1u,                              // productVersion (Shield)
                0u,                              // 2nd Java arg, clobbered on entry
                static_cast<uint32_t>(pad_state.A),
                static_cast<uint32_t>(pad_state.B),
                static_cast<uint32_t>(pad_state.X),
                static_cast<uint32_t>(pad_state.Y),
                static_cast<uint32_t>(pad_state.L1),
                static_cast<uint32_t>(pad_state.L2),
                static_cast<uint32_t>(pad_state.L3),
                static_cast<uint32_t>(pad_state.R1),
                static_cast<uint32_t>(pad_state.R2),
                static_cast<uint32_t>(pad_state.R3),
                static_cast<uint32_t>(pad_state.DpadUp),
                static_cast<uint32_t>(pad_state.DpadDown),
                static_cast<uint32_t>(pad_state.DpadLeft),
                static_cast<uint32_t>(pad_state.DpadRight),
                fbits(pad_state.AxisX),
                fbits(pad_state.AxisY),
                fbits(pad_state.AxisZ),
                fbits(pad_state.AxisRZ),
                static_cast<uint32_t>(pad_state.Start),
            },
            /*verbose=*/false);

        pad_last_sent = pad_state;
        pad_ever_sent = true;
    }
}
