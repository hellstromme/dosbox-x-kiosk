#ifndef DOSBOX_KIOSK_MAIN_H
#define DOSBOX_KIOSK_MAIN_H

#include "kiosk_types.h"
#include <string>
#include <memory>
#include <atomic>
#include <mutex>

typedef union SDL_Event SDL_Event;

class KioskManager {
public:
    static KioskManager& GetInstance();

    bool Initialize(const std::string& db_path, const std::string& base_conf);
    void RunMainLoop();
    void Shutdown();

    // State getters/setters
    KioskState GetState() const { return m_state.load(); }
    void SetState(KioskState state) { m_state.store(state); }

    bool IsKioskModeActive() const { return m_kiosk_active.load(); }
    void SetKioskModeActive(bool active) { m_kiosk_active.store(active); }

    bool IsSkipCountdown() const { return m_skip_countdown.load(); }
    void SetSkipCountdown(bool skip) { m_skip_countdown.store(skip); }

    // Configuration & Watchdog
    KioskConfigRecord GetConfig() const;
    float GetWatchdogRemainingSec() const;
    void SetWatchdogRemainingSec(float sec);
    void UpdateWatchdog(float delta_sec);
    void CancelWatchdog();

    // Input & Pre-Boot Routing
    bool HandlePreBootInputEvent(const SDL_Event& event);
    static bool IsIntentionalUserInput(const SDL_Event& event);

    // Default Boot Target Resolution & Launch Requests
    LaunchContext ResolveDefaultBootContext() const;
    bool TriggerDefaultBootAction();

    LaunchContext GetCurrentLaunchContext() const;
    void SetLaunchContext(const LaunchContext& ctx);

    bool RequestLaunchMachine(const std::string& machine_id, const std::string& custom_disk_path = "");
    bool RequestLaunchItem(const std::string& item_id);
    bool RequestDefaultDosboxLaunch();

    // Launch Synthesis & Session Execution
    bool SynthesizeAndPrepareLaunch(std::string& out_synthesized_conf);
    std::string GetLastSynthesizedConfig() const;
    std::string GetActiveDispatchImagePath() const;
    std::string GetSessionConfPath() const;
    std::string GetDefaultSessionConfPath() const;

    // Runtime Overlay & Guest Return Lifecycle
    void EnterRuntimeOverlay();
    void ResumeEmulation();
    void AbortSessionToShell();
    void OnGuestShutdown(KioskShutdownReason reason);

    static bool RelaunchIntoInteractiveShell(
        const char* argv0,
        const std::string& db_path = "",
        const std::string& conf_path = ""
    );

    void RequestApplianceExit();
    bool IsExitRequested() const { return m_exit_requested.load(); }

    KioskShutdownReason GetLastShutdownReason() const { return m_last_shutdown_reason.load(); }
    int GetCompletedSessionCount() const { return m_completed_sessions.load(); }

private:
    KioskManager();
    ~KioskManager();
    KioskManager(const KioskManager&) = delete;
    KioskManager& operator=(const KioskManager&) = delete;

    void CleanupSessionArtifactsLocked();

    mutable std::mutex m_mutex;
    std::atomic<KioskState> m_state;
    KioskConfigRecord m_config;
    LaunchContext m_launch_ctx;
    std::string m_base_conf_path;
    std::string m_last_synthesized_conf;
    std::string m_active_dispatch_image_path;
    std::string m_session_conf_path;
    float m_watchdog_timer = 30.0f;
    bool m_initialized = false;
    std::atomic<bool> m_kiosk_active;
    std::atomic<bool> m_skip_countdown;
    std::atomic<bool> m_exit_requested;
    std::atomic<KioskShutdownReason> m_last_shutdown_reason;
    std::atomic<int> m_completed_sessions;
};

#endif // DOSBOX_KIOSK_MAIN_H
