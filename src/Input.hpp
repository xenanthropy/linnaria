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

    // Gamepad state. The engine consumes nativeGamePadUpdate as a single
    // "snapshot per tick" -- mirrors the Java ShieldController -> Gamepad
    // -> AndroidInterface::fjAddGamePad pipeline.
    //
    // Two state regimes:
    //   * Axes (AxisX/Y/Z/RZ) -- HELD: the value persists until the key is
    //     released. Recomputed from a per-key boolean (PadKeys) so two
    //     opposing keys held simultaneously don't lose state when one is
    //     released ("hold A, hold D, release D -> resume walking left").
    //   * Buttons (A/B/X/Y/L*/R*/Dpad*/Start) -- PULSE: one frame of "1",
    //     then "0" until the key is released and pressed again. The engine
    //     re-reads the stored Gamepad on every internal tick and treats a
    //     non-zero button as "freshly pressed", so a HELD button would
    //     trigger that action every tick (menu flicker, hotbar auto-cycle).
    //     Pulse semantics emit one event per physical press regardless of
    //     hold duration.
    struct Pad {
        int A = 0, B = 0, X = 0, Y = 0;
        int L1 = 0, L2 = 0, L3 = 0;
        int R1 = 0, R2 = 0, R3 = 0;
        int DpadUp = 0, DpadDown = 0, DpadLeft = 0, DpadRight = 0;
        int Start = 0;
        float AxisX = 0.0f, AxisY = 0.0f;
        float AxisZ = 0.0f, AxisRZ = 0.0f;
    };

    // pad_state: only the axis fields are read out -- button fields here are
    // unused. The button fields of the snapshot we send come from pad_pulse.
    inline Pad pad_state;
    inline Pad pad_pulse;          // pending one-shot button pulses
    inline Pad pad_last_sent;
    inline bool pad_ever_sent = false;

    // Live held state for keys that need held semantics (axes, jump).
    struct PadKeys {
        bool w = false, a = false, s = false, d = false;
        bool jump = false;  // Space -- held so the engine can extend the jump
                            // for the full duration the key is pressed.
    };
    inline PadKeys pad_keys;

    // "Latest wins" tiebreak: when both opposing keys are held, the more
    // recently pressed one chooses direction. Cleared implicitly by
    // RecomputeAxes (it only consults these when both keys are held).
    inline SDL_Keycode last_horiz_press = 0; // SDLK_a or SDLK_d
    inline SDL_Keycode last_vert_press  = 0; // SDLK_w or SDLK_s

    inline void RecomputeAxes() {
        if (pad_keys.a && pad_keys.d) {
            pad_state.AxisX = (last_horiz_press == SDLK_d) ?  1.0f : -1.0f;
        } else if (pad_keys.d) {
            pad_state.AxisX =  1.0f;
        } else if (pad_keys.a) {
            pad_state.AxisX = -1.0f;
        } else {
            pad_state.AxisX = 0.0f;
        }
        // The engine expects +Y = up (matching the Java path, which ships
        // AxisY * -1 to invert Android's screen-down convention). So W
        // (player intends up) sends +1, S sends -1.
        if (pad_keys.w && pad_keys.s) {
            pad_state.AxisY = (last_vert_press == SDLK_s) ? -1.0f :  1.0f;
        } else if (pad_keys.w) {
            pad_state.AxisY =  1.0f;
        } else if (pad_keys.s) {
            pad_state.AxisY = -1.0f;
        } else {
            pad_state.AxisY = 0.0f;
        }
    }

    // SDL key -> pad action. WASD updates analog-stick axes (NOT the d-pad
    // -- the engine uses d-pad for menu/minimap, not motion). All other
    // mapped keys queue a single-frame pulse; key-release is intentionally
    // ignored for those, since the pulse self-clears on the next send.
    //
    // Edit this switch to remap; the SDLK_* constants make it self-documenting.
    inline bool ApplyPadKey(SDL_Keycode sym, bool down) {
        switch (sym) {
            // Movement (axes, held)
            case SDLK_w:
                pad_keys.w = down;
                if (down) last_vert_press = SDLK_w;
                RecomputeAxes();
                return true;
            case SDLK_s:
                pad_keys.s = down;
                if (down) last_vert_press = SDLK_s;
                RecomputeAxes();
                return true;
            case SDLK_a:
                pad_keys.a = down;
                if (down) last_horiz_press = SDLK_a;
                RecomputeAxes();
                return true;
            case SDLK_d:
                pad_keys.d = down;
                if (down) last_horiz_press = SDLK_d;
                RecomputeAxes();
                return true;
            // Jump -- HELD: A=1 every frame the key is down so the engine
            // can extend jump height for the duration of the hold. Toggle-
            // style actions (menu, inventory) below stay pulse so they
            // don't flicker.
            case SDLK_SPACE:
                pad_keys.jump = down;
                pad_state.A = down ? 1 : 0;
                return true;
            // Action / menu (one-frame pulses on key-down only)
            case SDLK_e:      if (down) pad_pulse.B     = 1; return true;
            case SDLK_f:      if (down) pad_pulse.X     = 1; return true;
            case SDLK_TAB:    if (down) pad_pulse.Y     = 1; return true;
            case SDLK_q:      if (down) pad_pulse.L1    = 1; return true;
            case SDLK_r:      if (down) pad_pulse.R1    = 1; return true;
            case SDLK_ESCAPE: if (down) pad_pulse.Start = 1; return true;
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

    // Push the current pad state to the engine via nativeGamePadUpdate.
    //
    // Composes the snapshot from two sources:
    //   * Axes -- from pad_state, the live held-direction signal.
    //   * Buttons -- from pad_pulse, the per-press one-shot queue (cleared
    //     after this call). On the *next* call, button fields will be 0,
    //     which differs from last_sent (which had them at 1), so the
    //     pulse-off update fires automatically and the engine sees a clean
    //     press-then-release pair.
    //
    // Called once per game tick (NOT per outer-loop iteration). Two sends
    // in the same inter-tick window would let the pulse-off overwrite the
    // pulse-on before the engine processed either; one-per-tick guarantees
    // the engine consumes each snapshot before the next overwrites it.
    //
    // Caller MUST guarantee main_thread_clean == true so that
    // ExecuteGameFunction's reuse of the main stack doesn't trample a paused
    // nativeOnUpdate frame.
    //
    // Arg ordering matches the Java OctarineBridge.nativeGamePadUpdate(...)
    // call in Gamepad.UpdateControllerData(); productVersion is the Shield's
    // value (1). L2 and R2 are typed `int` in Java but read as `float` by
    // native -- a known mobile-port mismatch that effectively zeros those
    // triggers. We match Java's bit pattern.
    inline void SendGamepadUpdate(Dynarmic::A32::Jit& cpu, GuestMemory& memory,
                                  ElfLoader& loader, uint32_t env_ptr) {
        Pad to_send{};
        // Axes from live state
        to_send.AxisX  = pad_state.AxisX;
        to_send.AxisY  = pad_state.AxisY;
        to_send.AxisZ  = pad_state.AxisZ;
        to_send.AxisRZ = pad_state.AxisRZ;
        // Held buttons (A = jump) from live state
        to_send.A         = pad_state.A;
        // Pulse buttons from the one-shot queue
        to_send.B         = pad_pulse.B;
        to_send.X         = pad_pulse.X;
        to_send.Y         = pad_pulse.Y;
        to_send.L1        = pad_pulse.L1;
        to_send.L2        = pad_pulse.L2;
        to_send.L3        = pad_pulse.L3;
        to_send.R1        = pad_pulse.R1;
        to_send.R2        = pad_pulse.R2;
        to_send.R3        = pad_pulse.R3;
        to_send.DpadUp    = pad_pulse.DpadUp;
        to_send.DpadDown  = pad_pulse.DpadDown;
        to_send.DpadLeft  = pad_pulse.DpadLeft;
        to_send.DpadRight = pad_pulse.DpadRight;
        to_send.Start     = pad_pulse.Start;

        if (pad_ever_sent && std::memcmp(&to_send, &pad_last_sent, sizeof(Pad)) == 0) {
            return;
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
                static_cast<uint32_t>(to_send.A),
                static_cast<uint32_t>(to_send.B),
                static_cast<uint32_t>(to_send.X),
                static_cast<uint32_t>(to_send.Y),
                static_cast<uint32_t>(to_send.L1),
                static_cast<uint32_t>(to_send.L2),
                static_cast<uint32_t>(to_send.L3),
                static_cast<uint32_t>(to_send.R1),
                static_cast<uint32_t>(to_send.R2),
                static_cast<uint32_t>(to_send.R3),
                static_cast<uint32_t>(to_send.DpadUp),
                static_cast<uint32_t>(to_send.DpadDown),
                static_cast<uint32_t>(to_send.DpadLeft),
                static_cast<uint32_t>(to_send.DpadRight),
                fbits(to_send.AxisX),
                fbits(to_send.AxisY),
                fbits(to_send.AxisZ),
                fbits(to_send.AxisRZ),
                static_cast<uint32_t>(to_send.Start),
            },
            /*verbose=*/false);

        pad_last_sent = to_send;
        pad_ever_sent = true;
        pad_pulse = {};   // consume: pulse-off will ship automatically next tick
    }
}
