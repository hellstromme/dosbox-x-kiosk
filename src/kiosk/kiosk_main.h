#ifndef DOSBOX_KIOSK_MAIN_H
#define DOSBOX_KIOSK_MAIN_H

#include "kiosk_types.h"
#include <string>
#include <memory>
#include <atomic>

class KioskManager {
public:
    static KioskManager& GetInstance();

    bool Initialize(const std::string& db_path, const std::string& base_conf);
    void RunMainLoop();
    void Shutdown();

    KioskState GetState() const { return m_state.load(); }
    void SetState(KioskState state) { m_state.store(state); }

    const LaunchContext& GetCurrentLaunchContext() const { return m_launch_ctx; }
    void SetLaunchContext(const LaunchContext& ctx) { m_launch_ctx = ctx; }

    bool IsKioskModeActive() const { return m_kiosk_active.load(); }
    void SetKioskModeActive(bool active) { m_kiosk_active.store(active); }

private:
    KioskManager();
    ~KioskManager();
    KioskManager(const KioskManager&) = delete;
    KioskManager& operator=(const KioskManager&) = delete;

    std::atomic<KioskState> m_state;
    LaunchContext m_launch_ctx;
    float m_watchdog_timer = 30.0f;
    bool m_initialized = false;
    std::atomic<bool> m_kiosk_active;
};

#endif // DOSBOX_KIOSK_MAIN_H
