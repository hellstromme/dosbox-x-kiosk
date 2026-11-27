#ifndef DOSBOX_KIOSK_RUNTIME_OVERLAY_H
#define DOSBOX_KIOSK_RUNTIME_OVERLAY_H

#include "../kiosk_types.h"
#include "view_disk_caddy.h"
#include <string>
#include <vector>

typedef union SDL_Event SDL_Event;

struct RuntimeDriveSlotStatus {
    std::string drive_letter;          // "A", "B", "D", "E"
    int drive_index = 0;               // 0=A, 1=B, 3=D, 4=E
    bool is_optical = false;           // false=Floppy, true=ATAPI CD-ROM
    bool is_mounted = false;
    bool is_write_protected = false;
    bool is_dispatch_reserved = false; // True for B: during REAL_HDD_BOOT item launch
    std::string active_file_path;
    std::string active_label;
    std::string media_type_badge;
    int current_slot_index = 0;
    int total_slots = 0;
};

struct RuntimeCaddyEntry {
    CaddyBindingRecord binding;
    MediaPoolRecord media;
    bool is_from_global_pool = false;
};

class RuntimeOverlay {
public:
    static RuntimeOverlay& GetInstance();

    // Called once when entering STATE_RUNTIME_OVERLAY to snapshot DB caddy,
    // global media pool, and active drive states (Zero per-frame SQLite queries)
    void OnEnterOverlay();
    void RefreshDriveStatuses();

    // Renders the dimmed background and full Runtime Media Caddy modal window
    void Render();

    // Handles overlay-specific keyboard/gamepad shortcuts (V/F2/Y view toggle, etc.)
    bool HandleInputEvent(const SDL_Event& event);

    // View mode (SKEUOMORPHIC vs LIST)
    CaddyViewMode GetViewMode() const { return m_view_mode; }
    void SetViewMode(CaddyViewMode mode) { m_view_mode = mode; }
    void ToggleViewMode();

    // Programmatic hot-swap, eject, write-protect & audio controls (also used by unit tests)
    bool PerformHotSwap(const std::string& target_drive, const std::string& host_path, bool write_protected = false);
    bool PerformHotSwapCaddySlot(size_t caddy_index);
    bool EjectDrive(const std::string& target_drive);
    bool ToggleDriveWriteProtect(const std::string& target_drive);
    void SendCtrlAltDelToGuest();

    float GetCdAudioVolumePercent() const { return m_cd_volume_percent; }
    void SetCdAudioVolumePercent(float percent);

    const std::vector<RuntimeDriveSlotStatus>& GetDriveStatuses() const { return m_drive_statuses; }
    const std::vector<RuntimeCaddyEntry>& GetSessionCaddy() const { return m_session_caddy; }
    const std::string& GetLastStatusMessage() const { return m_status_message; }

private:
    RuntimeOverlay() = default;

    void RenderHeaderBar();
    void RenderActiveDriveSlots();
    void RenderSkeuomorphicShelf();
    void RenderCompactListView();
    void RenderGlobalMediaPoolDrawer();
    void RenderFooterBar();

    CaddyViewMode m_view_mode = CaddyViewMode::SKEUOMORPHIC;
    std::vector<RuntimeDriveSlotStatus> m_drive_statuses;
    std::vector<RuntimeCaddyEntry> m_session_caddy;
    std::vector<MediaPoolRecord> m_global_pool_cache;
    uint64_t m_usb_change_revision = 0;
    bool m_show_global_pool_drawer = false;
    int m_selected_caddy_index = 0;
    float m_cd_volume_percent = 100.0f;
    std::string m_active_machine_name;
    std::string m_active_item_title;
    std::string m_status_message;
};

#endif // DOSBOX_KIOSK_RUNTIME_OVERLAY_H
