#include "config.h"

#if defined(C_KIOSK)

#include "input_router.h"
#include "logging.h"
#include "SDL.h"

#include <algorithm>
#include <cstdlib>

// Out-of-line C++14 definitions for static constexpr class members
constexpr int16_t KioskInputRouter::AXIS_DEADZONE;
constexpr int16_t KioskInputRouter::AXIS_RELEASE_ZONE;
constexpr uint32_t KioskInputRouter::REPEAT_INITIAL_DELAY_MS;
constexpr uint32_t KioskInputRouter::REPEAT_INTERVAL_MS;

KioskInputRouter& KioskInputRouter::GetInstance() {
    static KioskInputRouter instance;
    return instance;
}

KioskInputRouter::~KioskInputRouter() {
    ShutdownControllers();
}

void KioskInputRouter::InitializeControllers() {
    if ((SDL_WasInit(SDL_INIT_GAMECONTROLLER) & SDL_INIT_GAMECONTROLLER) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
            LOG_MSG("KIOSK InputRouter: SDL_INIT_GAMECONTROLLER failed: %s", SDL_GetError());
            return;
        }
    }

    int num_joysticks = SDL_NumJoysticks();
    for (int i = 0; i < num_joysticks; ++i) {
        if (SDL_IsGameController(i)) {
            OpenControllerByDeviceIndex(i);
        }
    }
}

void KioskInputRouter::ShutdownControllers() {
    for (size_t i = 0; i < m_controllers.size(); ++i) {
        if (m_controllers[i] != nullptr) {
            SDL_GameControllerClose(m_controllers[i]);
        }
    }
    m_controllers.clear();
    m_axis_x_dir = 0;
    m_axis_y_dir = 0;
    m_axis_x_owner = -1;
    m_axis_y_owner = -1;
    m_last_hat_state = 0;
    m_action_queue.clear();
}

void KioskInputRouter::OpenControllerByDeviceIndex(int device_index) {
    if (!SDL_IsGameController(device_index)) {
        return;
    }
    SDL_JoystickID target_id = SDL_JoystickGetDeviceInstanceID(device_index);
    for (size_t i = 0; i < m_controllers.size(); ++i) {
        SDL_Joystick* joy = SDL_GameControllerGetJoystick(m_controllers[i]);
        if (joy && SDL_JoystickInstanceID(joy) == target_id) {
            return; // Already open
        }
    }
    SDL_GameController* ctrl = SDL_GameControllerOpen(device_index);
    if (ctrl != nullptr) {
        m_controllers.push_back(ctrl);
        const char* name = SDL_GameControllerName(ctrl);
        LOG_MSG("KIOSK InputRouter: Opened game controller '%s'", name ? name : "Unknown");
    }
}

void KioskInputRouter::CloseControllerByInstanceId(int32_t instance_id) {
    // Clear any held analog stick direction owned by the disconnecting controller
    if (m_axis_x_owner == instance_id) {
        m_axis_x_dir = 0;
        m_axis_x_owner = -1;
    }
    if (m_axis_y_owner == instance_id) {
        m_axis_y_dir = 0;
        m_axis_y_owner = -1;
    }

    for (std::vector<SDL_GameController*>::iterator it = m_controllers.begin();
         it != m_controllers.end(); ++it) {
        SDL_Joystick* joy = SDL_GameControllerGetJoystick(*it);
        if (joy && SDL_JoystickInstanceID(joy) == static_cast<SDL_JoystickID>(instance_id)) {
            SDL_GameControllerClose(*it);
            m_controllers.erase(it);
            LOG_MSG("KIOSK InputRouter: Closed disconnected game controller (id=%d)", instance_id);
            return;
        }
    }
}

void KioskInputRouter::PushAction(KioskInputAction action) {
    if (action == KioskInputAction::NONE) return;
    if (m_action_queue.size() >= 64) {
        m_action_queue.pop_front();
    }
    m_action_queue.push_back(action);
}

void KioskInputRouter::HandleAnalogAxis(
    int32_t instance_id,
    int axis_index,
    int16_t value,
    uint32_t now_ms,
    bool suppress_actions
) {
    if (axis_index == SDL_CONTROLLER_AXIS_LEFTX) {
        int new_dir = m_axis_x_dir;
        if (value <= -AXIS_DEADZONE) {
            new_dir = -1;
        } else if (value >= AXIS_DEADZONE) {
            new_dir = 1;
        } else if (std::abs(static_cast<int>(value)) < AXIS_RELEASE_ZONE) {
            if (m_axis_x_owner == instance_id || m_axis_x_owner == -1) {
                new_dir = 0;
            }
        }

        if (new_dir != m_axis_x_dir) {
            m_axis_x_dir = new_dir;
            m_axis_x_owner = (new_dir != 0) ? instance_id : -1;
            if (!suppress_actions && new_dir < 0) {
                PushAction(KioskInputAction::NAV_LEFT);
                m_axis_x_next_repeat_ms = now_ms + REPEAT_INITIAL_DELAY_MS;
            } else if (!suppress_actions && new_dir > 0) {
                PushAction(KioskInputAction::NAV_RIGHT);
                m_axis_x_next_repeat_ms = now_ms + REPEAT_INITIAL_DELAY_MS;
            }
        }
    } else if (axis_index == SDL_CONTROLLER_AXIS_LEFTY) {
        int new_dir = m_axis_y_dir;
        if (value <= -AXIS_DEADZONE) {
            new_dir = -1;
        } else if (value >= AXIS_DEADZONE) {
            new_dir = 1;
        } else if (std::abs(static_cast<int>(value)) < AXIS_RELEASE_ZONE) {
            if (m_axis_y_owner == instance_id || m_axis_y_owner == -1) {
                new_dir = 0;
            }
        }

        if (new_dir != m_axis_y_dir) {
            m_axis_y_dir = new_dir;
            m_axis_y_owner = (new_dir != 0) ? instance_id : -1;
            if (!suppress_actions && new_dir < 0) {
                PushAction(KioskInputAction::NAV_UP);
                m_axis_y_next_repeat_ms = now_ms + REPEAT_INITIAL_DELAY_MS;
            } else if (!suppress_actions && new_dir > 0) {
                PushAction(KioskInputAction::NAV_DOWN);
                m_axis_y_next_repeat_ms = now_ms + REPEAT_INITIAL_DELAY_MS;
            }
        }
    }
}

void KioskInputRouter::UpdateRepeat(uint32_t now_ms, bool suppress_actions) {
    if (suppress_actions) {
        return;
    }
    if (m_axis_x_dir != 0 && SDL_TICKS_PASSED(now_ms, m_axis_x_next_repeat_ms)) {
        PushAction(m_axis_x_dir < 0 ? KioskInputAction::NAV_LEFT : KioskInputAction::NAV_RIGHT);
        m_axis_x_next_repeat_ms = now_ms + REPEAT_INTERVAL_MS;
    }
    if (m_axis_y_dir != 0 && SDL_TICKS_PASSED(now_ms, m_axis_y_next_repeat_ms)) {
        PushAction(m_axis_y_dir < 0 ? KioskInputAction::NAV_UP : KioskInputAction::NAV_DOWN);
        m_axis_y_next_repeat_ms = now_ms + REPEAT_INTERVAL_MS;
    }
}

bool KioskInputRouter::ProcessSDLEvent(
    const SDL_Event& event,
    bool want_text_input,
    uint32_t now_ms,
    bool suppress_actions,
    bool suppress_confirm
) {
    if (now_ms == 0) {
        now_ms = SDL_GetTicks();
    }

    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED:
            OpenControllerByDeviceIndex(event.cdevice.which);
            return true;

        case SDL_CONTROLLERDEVICEREMOVED:
            CloseControllerByInstanceId(event.cdevice.which);
            return true;

        case SDL_CONTROLLERAXISMOTION:
            // Always update physical stick deflection/release state even when actions are suppressed
            HandleAnalogAxis(event.caxis.which, event.caxis.axis, event.caxis.value, now_ms, suppress_actions);
            return false;

        case SDL_JOYHATMOTION: {
            // Skip raw hat events if this instance is already managed by an open SDL_GameController
            if (SDL_GameControllerFromInstanceID(event.jhat.which) != nullptr) {
                return false;
            }
            const uint8_t val = event.jhat.value;
            const uint8_t newly_pressed = static_cast<uint8_t>(val & (~m_last_hat_state));
            m_last_hat_state = val;
            if (suppress_actions) {
                return false;
            }
            if ((newly_pressed & SDL_HAT_LEFT) != 0) PushAction(KioskInputAction::NAV_LEFT);
            if ((newly_pressed & SDL_HAT_RIGHT) != 0) PushAction(KioskInputAction::NAV_RIGHT);
            if ((newly_pressed & SDL_HAT_UP) != 0) PushAction(KioskInputAction::NAV_UP);
            if ((newly_pressed & SDL_HAT_DOWN) != 0) PushAction(KioskInputAction::NAV_DOWN);
            return newly_pressed != 0;
        }

        case SDL_KEYDOWN: {
            if (suppress_actions) {
                return false;
            }
            const SDL_Keycode sym = event.key.keysym.sym;
            const uint16_t mod = event.key.keysym.mod;

            // Escape always maps to ACTION_BACK even while editing text
            if (sym == SDLK_ESCAPE) {
                PushAction(KioskInputAction::ACTION_BACK);
                return true;
            }

            if (want_text_input) {
                return false;
            }

            switch (sym) {
                case SDLK_LEFT:
                case SDLK_a:
                    PushAction(KioskInputAction::NAV_LEFT);
                    return true;
                case SDLK_RIGHT:
                case SDLK_d:
                    PushAction(KioskInputAction::NAV_RIGHT);
                    return true;
                case SDLK_UP:
                case SDLK_w:
                    PushAction(KioskInputAction::NAV_UP);
                    return true;
                case SDLK_DOWN:
                case SDLK_s:
                    PushAction(KioskInputAction::NAV_DOWN);
                    return true;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE:
                    if (suppress_confirm) {
                        return false;
                    }
                    if (event.key.repeat == 0) {
                        PushAction(KioskInputAction::ACTION_CONFIRM);
                    }
                    return true;
                case SDLK_BACKSPACE:
                    if (event.key.repeat == 0) {
                        PushAction(KioskInputAction::ACTION_BACK);
                    }
                    return true;
                case SDLK_c:
                    if (event.key.repeat == 0) {
                        PushAction(KioskInputAction::ACTION_CADDY_QUEUE);
                    }
                    return true;
                case SDLK_v:
                case SDLK_F2:
                    if (event.key.repeat == 0) {
                        PushAction(KioskInputAction::ACTION_VIEW_TOGGLE);
                    }
                    return true;
                case SDLK_f:
                    if (event.key.repeat == 0) {
                        PushAction(KioskInputAction::ACTION_FAVORITE);
                    }
                    return true;
                case SDLK_m:
                    if (event.key.repeat == 0) {
                        PushAction(KioskInputAction::ACTION_CONTEXT_MENU);
                    }
                    return true;
                case SDLK_q:
                case SDLK_PAGEUP:
                    if (event.key.repeat == 0) {
                        PushAction(KioskInputAction::NAV_TAB_PREV);
                    }
                    return true;
                case SDLK_e:
                case SDLK_PAGEDOWN:
                    if (event.key.repeat == 0) {
                        PushAction(KioskInputAction::NAV_TAB_NEXT);
                    }
                    return true;
                case SDLK_TAB:
                    if (event.key.repeat == 0) {
                        if ((mod & KMOD_SHIFT) != 0) {
                            PushAction(KioskInputAction::NAV_TAB_PREV);
                        } else {
                            PushAction(KioskInputAction::NAV_TAB_NEXT);
                        }
                    }
                    return true;
                default:
                    return false;
            }
        }

        case SDL_CONTROLLERBUTTONDOWN: {
            if (suppress_actions) {
                return false;
            }
            switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                    PushAction(KioskInputAction::NAV_LEFT);
                    return true;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                    PushAction(KioskInputAction::NAV_RIGHT);
                    return true;
                case SDL_CONTROLLER_BUTTON_DPAD_UP:
                    PushAction(KioskInputAction::NAV_UP);
                    return true;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                    PushAction(KioskInputAction::NAV_DOWN);
                    return true;
                case SDL_CONTROLLER_BUTTON_A:
                    if (suppress_confirm) {
                        return false;
                    }
                    PushAction(KioskInputAction::ACTION_CONFIRM);
                    return true;
                case SDL_CONTROLLER_BUTTON_B:
                    PushAction(KioskInputAction::ACTION_BACK);
                    return true;
                case SDL_CONTROLLER_BUTTON_X:
                    PushAction(KioskInputAction::ACTION_CADDY_QUEUE);
                    return true;
                case SDL_CONTROLLER_BUTTON_Y:
                    PushAction(KioskInputAction::ACTION_VIEW_TOGGLE);
                    return true;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                    PushAction(KioskInputAction::NAV_TAB_PREV);
                    return true;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                    PushAction(KioskInputAction::NAV_TAB_NEXT);
                    return true;
                case SDL_CONTROLLER_BUTTON_BACK:
                    PushAction(KioskInputAction::ACTION_FAVORITE);
                    return true;
                case SDL_CONTROLLER_BUTTON_START:
                    PushAction(KioskInputAction::ACTION_CONTEXT_MENU);
                    return true;
                default:
                    return false;
            }
        }

        case SDL_JOYBUTTONDOWN: {
            if (suppress_actions) {
                return false;
            }
            // Only translate raw joystick buttons when no SDL_GameController is managing this instance
            if (SDL_GameControllerFromInstanceID(event.jbutton.which) != nullptr) {
                return false;
            }
            switch (event.jbutton.button) {
                case 0:
                    if (suppress_confirm) return false;
                    PushAction(KioskInputAction::ACTION_CONFIRM);
                    return true;
                case 1: PushAction(KioskInputAction::ACTION_BACK); return true;
                case 2: PushAction(KioskInputAction::ACTION_CADDY_QUEUE); return true;
                case 3: PushAction(KioskInputAction::ACTION_VIEW_TOGGLE); return true;
                default: return false;
            }
        }

        default:
            return false;
    }
}

bool KioskInputRouter::PollAction(KioskInputAction& out_action) {
    if (m_action_queue.empty()) {
        out_action = KioskInputAction::NONE;
        return false;
    }
    out_action = m_action_queue.front();
    m_action_queue.pop_front();
    return true;
}

void KioskInputRouter::ClearPendingActions() {
    m_action_queue.clear();
}

const char* KioskInputRouter::ActionToString(KioskInputAction action) {
    switch (action) {
        case KioskInputAction::NAV_LEFT:            return "NAV_LEFT";
        case KioskInputAction::NAV_RIGHT:           return "NAV_RIGHT";
        case KioskInputAction::NAV_UP:              return "NAV_UP";
        case KioskInputAction::NAV_DOWN:            return "NAV_DOWN";
        case KioskInputAction::NAV_TAB_PREV:        return "NAV_TAB_PREV";
        case KioskInputAction::NAV_TAB_NEXT:        return "NAV_TAB_NEXT";
        case KioskInputAction::ACTION_CONFIRM:      return "ACTION_CONFIRM";
        case KioskInputAction::ACTION_BACK:         return "ACTION_BACK";
        case KioskInputAction::ACTION_CADDY_QUEUE:  return "ACTION_CADDY_QUEUE";
        case KioskInputAction::ACTION_VIEW_TOGGLE:  return "ACTION_VIEW_TOGGLE";
        case KioskInputAction::ACTION_FAVORITE:     return "ACTION_FAVORITE";
        case KioskInputAction::ACTION_CONTEXT_MENU: return "ACTION_CONTEXT_MENU";
        default:                                    return "NONE";
    }
}

#endif // defined(C_KIOSK)
