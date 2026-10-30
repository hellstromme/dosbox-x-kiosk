#ifndef DOSBOX_KIOSK_TYPES_H
#define DOSBOX_KIOSK_TYPES_H

#include <string>
#include <vector>
#include <cstdint>

// Archetype identifier constants
namespace KioskArchetypes {
    constexpr const char* XT_8088      = "ARCH_XT_8088";
    constexpr const char* AT_286       = "ARCH_AT_286";
    constexpr const char* DX_386       = "ARCH_386DX";
    constexpr const char* DX2_486      = "ARCH_486DX2";
    constexpr const char* PENTIUM_3DFX = "ARCH_PENTIUM_3DFX";
}

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

enum class OSFamily {
    MSDOS,
    FREEDOS,
    WIN3X,
    WIN9X
};

enum class FilesystemType {
    FAT12,
    FAT16,
    FAT32
};

enum class InstallStrategy {
    FLOPPY_SEQUENCE,
    BOOTABLE_CD,
    WIN9X_CAB_PRECOPY,
    PREBUILT_IMAGE
};

enum class MediaType {
    FLOPPY_525_360K,
    FLOPPY_525_1200K,
    FLOPPY_35_720K,
    FLOPPY_35_1440K,
    FLOPPY_35_2880K,
    CDROM_ISO,
    CDROM_CUE,
    HDD_IMG
};

enum class MediaPhysicalStyle {
    CHARCOAL_525,
    BEIGE_35,
    BLUE_35,
    BLACK_35,
    JEWEL_CD,
    GOLD_CD
};

enum class LabelFontStyle {
    DOT_MATRIX,
    HANDWRITTEN,
    PRINTED_OEM
};

enum class ItemType {
    GAME,
    APPLICATION
};

enum class SourceOrigin {
    SCANNED,
    GOG_IMPORT,
    STEAM_IMPORT,
    MANUAL
};

enum class LaunchMechanism {
    INTEGRATED_AUTOEXEC,
    DOS_AUTOEXEC_INJECT,
    WIN3X_PROGMAN_HOOK,
    WIN9X_RUNONCE_HOOK
};

enum class BootMode {
    INTEGRATED_DOS,
    REAL_HDD_BOOT
};

// --- Record Structs ---

struct KioskConfigRecord {
    int id = 1;
    int boot_timeout_sec = 30;
    std::string default_boot_action = "DEFAULT_DOSBOX";
    std::string default_target_uuid;
    std::string overlay_hotkey = "F12+D";
    std::string default_caddy_view = "SKEUOMORPHIC";
    bool auto_mount_usb_docs = true;
    std::string theme_name = "crt_amber";
};

struct MediaPoolRecord {
    std::string id;
    std::string file_path;
    std::string file_sha1;
    std::string media_type;      // matches MediaType enum name
    std::string physical_style;  // matches MediaPhysicalStyle enum name
    std::string volume_label;
    std::string label_title;
    std::string label_subtitle;
    std::string label_font_style; // matches LabelFontStyle enum name
    bool has_redbook_audio = false;
    bool is_write_protected = false;
    bool is_removable_usb = false;
    std::string created_at;
};

struct OSDistributionRecord {
    std::string id;
    std::string display_name;
    std::string os_family;        // "MSDOS", "FREEDOS", "WIN3X", "WIN9X"
    int min_archetype_tier = 1;
    int recommended_hdd_mb = 250;
    std::string filesystem_type;  // "FAT12", "FAT16", "FAT32"
    std::string install_strategy; // "FLOPPY_SEQUENCE", "BOOTABLE_CD", "WIN9X_CAB_PRECOPY", "PREBUILT_IMAGE"
    std::string boot_floppy_media_id;
    std::string install_cd_media_id;
    std::string unattended_answer_file;
};

struct MachineRecord {
    std::string id;
    std::string display_name;
    std::string archetype_id;
    int archetype_tier = 1;
    std::string os_id;
    bool os_installed = false;
    std::string boot_mode = "INTEGRATED_DOS"; // "INTEGRATED_DOS" or "REAL_HDD_BOOT"
    std::string primary_hdd_path;
    std::string primary_hdd_chs;
    std::string custom_conf_overrides;        // JSON or INI delta
    std::string last_booted_at;
    std::string created_at;
};

struct CatalogItemRecord {
    std::string id;
    std::string machine_id;
    std::string item_type;        // "GAME" or "APPLICATION"
    std::string title;
    std::string developer;
    int release_year = 0;
    std::string source_origin = "SCANNED"; // "SCANNED", "GOG_IMPORT", etc.
    std::string launch_mechanism = "INTEGRATED_AUTOEXEC";
    std::string host_container_dir;
    std::string guest_working_dir;
    std::string guest_executable;
    std::string guest_arguments;
    bool return_on_exit = true;
    std::string icon_cache_path;
    std::string screenshot_cache_path;
    std::string executable_crc32;
    int play_count = 0;
    std::string last_launched_at;
    bool is_favorite = false;
};

struct CaddyBindingRecord {
    int id = 0;
    std::string machine_id;
    std::string catalog_item_id;
    std::string media_id;
    std::string target_drive = "A"; // "A", "B", "D", "E"
    int slot_order = 1;
    // Resolved media path (populated on joins / query helpers)
    std::string resolved_file_path;
    std::string resolved_media_type;
    std::string resolved_label;
};

struct SoftwareSignatureRecord {
    std::string exec_filename;
    int64_t file_size_bytes = 0;
    std::string header_crc32;
    std::string detected_title;
    std::string item_type; // "GAME", "APPLICATION", "IGNORE_SYSTEM"
    std::string recommended_archetype;
};

struct LaunchContext {
    std::string machine_id;
    std::string catalog_item_id;
    bool is_real_hdd_boot = false;
    std::string custom_disk_path;
};

struct ArchetypeProfile {
    std::string id;
    std::string display_name;
    std::string era_description;
    int tier = 1; // 1 to 5
    // Hardware specs
    std::string cpu_type;
    std::string cpu_core;
    std::string cpu_cycles;
    bool apm_bios = false;
    std::string apm_version;
    bool isa_pnp = false;
    std::string machine_type;
    int mem_size_mb = 1;
    int vmem_size_kb = 0;
    std::string sound_card;
    std::string sound_base;
    int sound_irq = 7;
    int sound_dma = 1;
    int sound_hdma = 5;
    std::string opl_mode;
    std::string opl_emu;
    bool gus_enabled = false;
    bool voodoo_enabled = false;
    std::string mpu401_mode;
    std::string midi_device;
    std::string soundfont_path;
    std::string dos_version;
    bool xms = false;
    bool ems = false;
    bool umb = false;
    bool dual_ide = false;
};

#endif // DOSBOX_KIOSK_TYPES_H
