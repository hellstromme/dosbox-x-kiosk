#ifndef DOSBOX_KIOSK_VIEW_WATCHDOG_H
#define DOSBOX_KIOSK_VIEW_WATCHDOG_H

#include "../kiosk_types.h"
#include <string>

struct WatchdogTargetPreview {
    std::string title;
    std::string subtitle;
    std::string archetype_badge;
    std::string boot_mode_badge;
    int total_timeout_sec = 30;
};

class ViewWatchdog {
public:
    static ViewWatchdog& GetInstance();

    // Resolves the default boot target once from KioskManager/KioskDatabase (zero per-frame SQLite)
    void RefreshPreview();
    void InvalidatePreview() { m_preview_loaded = false; }

    // Renders the full-screen STATE_COUNTDOWN_IDLE autoboot watchdog overlay
    void Render(float remaining_sec);

    const WatchdogTargetPreview& GetCachedPreview() const { return m_preview; }

private:
    ViewWatchdog() = default;

    bool m_preview_loaded = false;
    WatchdogTargetPreview m_preview;
};

#endif // DOSBOX_KIOSK_VIEW_WATCHDOG_H
