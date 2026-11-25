#ifndef DOSBOX_KIOSK_INPUT_ROUTER_H
#define DOSBOX_KIOSK_INPUT_ROUTER_H

#include <cstdint>
#include <deque>
#include <vector>

typedef union SDL_Event SDL_Event;
struct _SDL_GameController;
typedef struct _SDL_GameController SDL_GameController;

enum class KioskInputAction {
    NONE = 0,
    NAV_LEFT,
    NAV_RIGHT,
    NAV_UP,
    NAV_DOWN,
    NAV_TAB_PREV,
    NAV_TAB_NEXT,
    ACTION_CONFIRM,
    ACTION_BACK,
    ACTION_CADDY_QUEUE,
    ACTION_VIEW_TOGGLE,
    ACTION_FAVORITE,
    ACTION_CONTEXT_MENU
};

class KioskInputRouter {
public:
    static KioskInputRouter& GetInstance();

    // Opens/closes SDL2 game controllers for pre-boot navigation
    void InitializeControllers();
    void ShutdownControllers();
    size_t GetConnectedControllerCount() const { return m_controllers.size(); }

    // Translates an SDL_Event into zero or more KioskInputActions.
    // Physical state (hotplug, analog stick releases, hat state) is always tracked even when
    // suppress_actions is true; suppress_confirm blocks ACTION_CONFIRM when an ImGui widget has focus.
    bool ProcessSDLEvent(
        const SDL_Event& event,
        bool want_text_input = false,
        uint32_t now_ms = 0,
        bool suppress_actions = false,
        bool suppress_confirm = false
    );

    // Generates key-repeat navigation actions for held analog sticks
    void UpdateRepeat(uint32_t now_ms, bool suppress_actions = false);

    // Queue inspection
    bool PollAction(KioskInputAction& out_action);
    size_t GetPendingActionCount() const { return m_action_queue.size(); }
    void ClearPendingActions();

    static const char* ActionToString(KioskInputAction action);

private:
    KioskInputRouter() = default;
    ~KioskInputRouter();
    KioskInputRouter(const KioskInputRouter&) = delete;
    KioskInputRouter& operator=(const KioskInputRouter&) = delete;

    void PushAction(KioskInputAction action);
    void HandleAnalogAxis(int32_t instance_id, int axis_index, int16_t value, uint32_t now_ms, bool suppress_actions);
    void OpenControllerByDeviceIndex(int device_index);
    void CloseControllerByInstanceId(int32_t instance_id);

    std::deque<KioskInputAction> m_action_queue;
    std::vector<SDL_GameController*> m_controllers;

    // Analog stick debounce/repeat state (tracked per owning controller instance ID)
    int m_axis_x_dir = 0; // -1, 0, +1
    int m_axis_y_dir = 0; // -1, 0, +1
    int32_t m_axis_x_owner = -1;
    int32_t m_axis_y_owner = -1;
    uint32_t m_axis_x_next_repeat_ms = 0;
    uint32_t m_axis_y_next_repeat_ms = 0;
    uint8_t m_last_hat_state = 0;

    static constexpr int16_t AXIS_DEADZONE = 16000;
    static constexpr int16_t AXIS_RELEASE_ZONE = 8000;
    static constexpr uint32_t REPEAT_INITIAL_DELAY_MS = 350;
    static constexpr uint32_t REPEAT_INTERVAL_MS = 120;
};

#endif // DOSBOX_KIOSK_INPUT_ROUTER_H
