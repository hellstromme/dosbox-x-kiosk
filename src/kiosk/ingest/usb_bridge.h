#ifndef DOSBOX_KIOSK_USB_BRIDGE_H
#define DOSBOX_KIOSK_USB_BRIDGE_H

#include "../kiosk_types.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct UsbVolumeInfo {
    std::string mount_path;      // Normalized mount root (e.g. "E:\\" or "/run/media/kiosk/USB")
    std::string volume_label;    // Display label for UI & media_group ("USB: <label>")
    bool is_optical = false;     // True for CD/DVD drives
    int media_count = 0;         // Number of disk images indexed from this volume
    int archive_count = 0;       // Number of GOG/Steam archives discovered on this volume
    std::string documents_dir;   // Optional "<mount>/DOCUMENTS" or "<mount>/DOS_FILES" folder
};

struct UsbArchiveCandidate {
    std::string file_path;
    std::string mount_path;
    std::string display_name;
    bool is_gog_installer = false;
};

class KioskUsbBridge {
public:
    static KioskUsbBridge& GetInstance();

    // Initializes removable storage watching.
    // If watch_root is empty, uses platform defaults (/run/media/kiosk + /run/media/$USER on Linux;
    // Win32 GetLogicalDrives() on Windows).
    // Set enable_os_polling=false in headless unit tests to prevent host drive scans.
    bool Initialize(const std::string& watch_root = "", bool enable_os_polling = true);
    void Shutdown();
    bool IsInitialized() const { return m_initialized.load(); }

    // Non-blocking poll invoked periodically from Kiosk pre-boot & F12+D overlay loops
    void PollEventsNonBlocking();
    void RescanNow();

    // Safely ejects any active DOSBox-X drives pointing into mount_path, purges removable
    // media_pool rows, and requests OS dismount/sync.
    bool SafeUnmountDevice(const std::string& mount_path);

    // Monotonic revision incremented on mount/unmount so UI drawers refresh lazily (zero per-frame SQLite)
    uint64_t GetChangeRevision() const { return m_change_revision.load(); }

    // Cached volume & archive accessors (thread-safe, zero SQLite)
    std::vector<UsbVolumeInfo> GetActiveVolumes() const;
    std::vector<UsbArchiveCandidate> GetDiscoveredArchives() const;
    std::string GetPreferredUsbDocumentsDir() const;

    // Deterministic entry points (also used directly by unit tests)
    bool OnVolumeMounted(const std::string& mount_path,
                         const std::string& explicit_label = "",
                         bool is_optical = false,
                         bool force_rescan = false);
    bool OnVolumeRemoved(const std::string& mount_path);

    // Pure classification & normalization helpers
    static bool IsSupportedDiskImageExtension(const std::string& filename);
    static bool IsImportableArchiveFilename(const std::string& filename, bool* out_is_gog = nullptr);
    static std::string NormalizeMountPath(const std::string& path);
    static bool PathStartsWithMount(const std::string& file_path, const std::string& mount_path);
    static std::string DeriveVolumeLabelFromPath(const std::string& mount_path);

private:
    KioskUsbBridge();
    ~KioskUsbBridge();
    KioskUsbBridge(const KioskUsbBridge&) = delete;
    KioskUsbBridge& operator=(const KioskUsbBridge&) = delete;

    void PollLinuxInotify();
    void PollWindowsLogicalDrives();
    void RefreshLinuxWatchRoots();
    void ScanExistingLinuxMounts();

    void ScanVolumeRecursive(const std::string& dir,
                             const std::string& mount_root,
                             const std::string& volume_label,
                             int depth,
                             int max_depth,
                             std::vector<MediaPoolRecord>& out_media_records,
                             std::vector<UsbArchiveCandidate>& out_archives,
                             std::string& out_docs_dir);
    void EjectActiveDrivesUnderMount(const std::string& mount_path);
    void DispatchAsyncPoll(bool rescan_existing);
    void DrainPendingMainThreadActions();
    void JoinScanThread();

    mutable std::mutex m_mutex;
    std::atomic<bool> m_initialized;
    std::atomic<uint64_t> m_change_revision;
    std::atomic<bool> m_os_polling_enabled;
    std::atomic<bool> m_scan_running;
    std::atomic<bool> m_scan_cancel;
    std::atomic<bool> m_pending_forced_rescan;
    std::thread m_scan_thread;
    std::thread::id m_main_thread_id;
    uint32_t m_last_poll_ticks = 0;

    // Pending main-thread UI/emulation actions queued by the background worker
    bool m_pending_cache_invalidation = false;
    std::string m_pending_toast_msg;
    float m_pending_toast_sec = 0.0f;
    std::vector<std::string> m_pending_eject_mounts;

    // Linux inotify state
    int m_inotify_fd = -1;
    std::vector<std::string> m_watch_roots;
    struct WatchEntry {
        int wd = -1;
        std::string root_path;
    };
    std::vector<WatchEntry> m_watch_descriptors;

    // Windows logical drive bitmask & per-letter volume serial state
    uint32_t m_last_drive_mask = 0;
    uint32_t m_drive_serials[26];

    std::vector<UsbVolumeInfo> m_active_volumes;
    std::vector<UsbArchiveCandidate> m_discovered_archives;
};

#endif // DOSBOX_KIOSK_USB_BRIDGE_H
