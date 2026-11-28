#ifndef DOSBOX_KIOSK_HOST_H
#define DOSBOX_KIOSK_HOST_H

#include <string>
#include <cstdint>

enum class KioskHostProfile {
    DESKTOP_BIG_PICTURE = 0,
    APPLIANCE_KMSDRM
};

class KioskHost {
public:
    // Deployment profile detection (Buildroot/KMSDRM appliance vs Desktop Big-Picture)
    static KioskHostProfile DetectHostProfile();
    static const char* GetHostProfileDisplayName(KioskHostProfile profile);

    // Cross-platform data root & standardized storage hierarchy
    // Appliance Linux: /var/lib/dosbox-kiosk
    // Desktop Windows: %LOCALAPPDATA%\DOSBox-Kiosk (fallback %APPDATA%, then .)
    // Desktop Linux/macOS: $XDG_DATA_HOME/dosbox-kiosk or $HOME/.local/share/dosbox-kiosk
    static std::string GetDataRootDir();
    static std::string GetConfigDir(const std::string& root_override = "");
    static std::string GetMediaPoolDir(const std::string& root_override = "");
    static std::string GetOSInstallersDir(const std::string& root_override = "");
    static std::string GetIngestStagingDir(const std::string& root_override = "");
    static std::string GetMachinesDir(const std::string& root_override = "");
    static std::string GetIconCacheDir(const std::string& root_override = "");

    static std::string GetDefaultDatabasePath(const std::string& root_override = "");
    static std::string GetDefaultBaseConfPath(const std::string& root_override = "");
    static std::string GetDefaultMapperPath(const std::string& root_override = "");

    // Ensures all standard Kiosk subdirectories exist on disk
    static bool EnsureDataDirectories(const std::string& root_override = "");
    static bool EnsureDirectoryRecursive(const std::string& dir_path);
    static bool FileExists(const std::string& file_path);
    // Returns true ONLY when the OS confirms the path does not exist (returns false on EACCES / permission errors)
    static bool IsFileConfirmedMissing(const std::string& file_path);
    static std::string JoinPath(const std::string& base, const std::string& child);

    // Strips trailing separators (keeps "/" and Windows drive roots such as "E:\\").
    static std::string NormalizeDirPath(const std::string& path);
    // True when file_path equals dir_path or lies beneath it (separator-aware; on Windows
    // case-insensitive and '/' == '\\'). Used for USB mount containment checks.
    static bool PathIsWithin(const std::string& file_path, const std::string& dir_path);

    // Optional systemd sd_notify() supervision (no-op unless HAVE_LIBSYSTEMD is defined)
    static bool IsSystemdSupportCompiled();
    static void NotifySystemdReady();
    static void NotifySystemdStopping();
    static void PingSystemdWatchdog(uint32_t min_interval_ms = 5000);
};

#endif // DOSBOX_KIOSK_HOST_H
