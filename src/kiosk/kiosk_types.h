#ifndef DOSBOX_KIOSK_TYPES_H
#define DOSBOX_KIOSK_TYPES_H

#include <string>
#include <cstdint>

enum class KioskState {
    DISABLED = 0,
    STATE_KIOSK_BOOT,
    STATE_COUNTDOWN_IDLE,
    STATE_SHELL_INTERACTIVE,
    STATE_SYNTHESIZE_LAUNCH,
    STATE_EMULATION_RUNNING,
    STATE_RUNTIME_OVERLAY
};

enum class KioskBootAction {
    DEFAULT_DOSBOX,
    LAST_PLAYED,
    SPECIFIC_MACHINE,
    SPECIFIC_ITEM
};

enum class CaddyViewMode {
    SKEUOMORPHIC,
    LIST
};

struct LaunchContext {
    std::string machine_id;
    std::string catalog_item_id;
    bool is_real_hdd_boot = false;
    std::string custom_disk_path;
};

#endif // DOSBOX_KIOSK_TYPES_H
