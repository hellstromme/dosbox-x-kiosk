#ifndef DOSBOX_KIOSK_MAIN_H
#define DOSBOX_KIOSK_MAIN_H

#include "kiosk_types.h"
#include <string>
#include <memory>

class KioskManager {
public:
    static KioskManager& GetInstance();

    bool Initialize(const std::string& db_path, const std::string& base_conf);
    void RunMainLoop();
    void Shutdown();

    KioskState GetState() const { return m_state; }
    void SetState(KioskState state) { m_state = state; }

    const LaunchContext& GetCurrentLaunchContext() const { return m_launch_ctx; }
    void SetLaunchContext(const LaunchContext& ctx) { m_launch_ctx = ctx; }

    bool IsKioskModeActive() const { return m_kiosk_active; }
    void SetKioskModeActive(bool active) { m_kiosk_active = active; }

private:
    KioskManager() = default;
    ~KioskManager();
    KioskManager(const KioskManager&) = delete;
    KioskManager& operator=(const KioskManager&) = delete;

    KioskState m_state = KioskState::DISABLED;
    LaunchContext m_launch_ctx;
    float m_watchdog_timer = 30.0f;
    bool m_initialized = false;
    bool m_kiosk_active = false;
};

#endif // DOSBOX_KIOSK_MAIN_H
