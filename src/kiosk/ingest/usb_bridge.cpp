#include "config.h"

#if defined(C_KIOSK)

#include "usb_bridge.h"
#include "../kiosk_db.h"
#include "../kiosk_host.h"
#include "../ui/kiosk_ui.h"
#include "../ui/view_disk_caddy.h"
#include "../ui/runtime_overlay.h"
#include "logging.h"
#include "SDL.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <cwchar>
#else
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/inotify.h>
#include <sys/mount.h>
#endif
#endif

namespace {

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

std::string GetFileExtensionLower(const std::string& filename) {
    const size_t slash = filename.find_last_of("\\/");
    const size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return "";
    }
    return ToLowerAscii(filename.substr(dot));
}

std::string GetBaseName(const std::string& path) {
    if (path.empty()) return "";
    size_t end = path.size();
    while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) {
        --end;
    }
    if (end == 0) return path;
    size_t slash = path.find_last_of("\\/", end - 1);
    if (slash == std::string::npos) return path.substr(0, end);
    return path.substr(slash + 1, end - slash - 1);
}

std::string BuildUsbMediaId(const std::string& file_path) {
    const uint32_t h1 = DiskCaddyView::ComputeStringCrc32(file_path);
    const uint32_t h2 = DiskCaddyView::ComputeStringCrc32("usb_media_" + file_path);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "usb-%08x-%08x", h1, h2);
    return std::string(buf);
}

#if defined(_WIN32)
std::wstring Utf8ToWideLocal(const std::string& s) {
    if (s.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], needed);
    return out;
}

std::string WideToUtf8Local(const std::wstring& ws) {
    if (ws.empty()) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), &out[0], needed, nullptr, nullptr);
    return out;
}

struct ScopedWinErrorMode {
    UINT prev_mode;
    ScopedWinErrorMode() {
        prev_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    }
    ~ScopedWinErrorMode() {
        SetErrorMode(prev_mode);
    }
};
#endif

struct DirEntryItem {
    std::string name;
    bool is_dir = false;
};

std::vector<DirEntryItem> ListDirectoryEntries(const std::string& dir_path, bool* out_ok = nullptr) {
    std::vector<DirEntryItem> entries;
    if (out_ok) *out_ok = false;
#if defined(_WIN32)
    ScopedWinErrorMode guard;
    std::string pattern = KioskHost::JoinPath(dir_path, "*");
    std::wstring wpattern = Utf8ToWideLocal(pattern);
    if (wpattern.empty()) return entries;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND && out_ok) *out_ok = true;
        return entries;
    }
    if (out_ok) *out_ok = true;
    do {
        if (std::wcscmp(fd.cFileName, L".") == 0 || std::wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue; // Do not follow symlinks / junctions on untrusted removable media
        }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0 &&
            (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0) {
            continue; // Skip System Volume Information / $RECYCLE.BIN
        }
        DirEntryItem item;
        item.name = WideToUtf8Local(fd.cFileName);
        item.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!item.name.empty()) {
            entries.push_back(item);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir_path.c_str());
    if (!d) return entries;
    bool read_error = false;
    for (;;) {
        errno = 0;
        struct dirent* ent = readdir(d);
        if (!ent) {
            if (errno != 0) read_error = true;
            break;
        }
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (ent->d_name[0] == '.') {
            continue;
        }
        DirEntryItem item;
        item.name = ent->d_name;
        const std::string full = KioskHost::JoinPath(dir_path, item.name);
        struct stat st;
        if (lstat(full.c_str(), &st) == 0 && (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) {
            item.is_dir = S_ISDIR(st.st_mode);
            entries.push_back(item);
        }
    }
    closedir(d);
    if (out_ok) *out_ok = !read_error;
#endif
    return entries;
}

bool DirectoryExists(const std::string& path) {
    if (path.empty()) return false;
#if defined(_WIN32)
    ScopedWinErrorMode guard;
    std::wstring wp = Utf8ToWideLocal(path);
    DWORD attrs = GetFileAttributesW(wp.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
#else
    struct stat st;
    return (stat(path.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
#endif
}

} // anonymous namespace

KioskUsbBridge::KioskUsbBridge()
    : m_initialized(false),
      m_change_revision(0),
      m_os_polling_enabled(true),
      m_scan_running(false),
      m_scan_cancel(false),
      m_pending_forced_rescan(false),
      m_removal_generation(0),
      m_main_thread_id(std::this_thread::get_id()),
      m_last_poll_ticks(0),
      m_inotify_fd(-1),
      m_last_drive_mask(0) {
    std::memset(m_drive_serials, 0, sizeof(m_drive_serials));
}

KioskUsbBridge::~KioskUsbBridge() {
    Shutdown();
}

KioskUsbBridge& KioskUsbBridge::GetInstance() {
    static KioskUsbBridge instance;
    return instance;
}

std::string KioskUsbBridge::NormalizeMountPath(const std::string& path) {
    std::string out = path;
    while (out.size() > 1) {
        const char back = out[out.size() - 1];
#if defined(_WIN32)
        if (back != '/' && back != '\\') break;
        // Preserve Windows drive root trailing slash ("E:\")
        if (out.size() == 3 && out[1] == ':') break;
#else
        if (back != '/') break;
#endif
        out.pop_back();
    }
    return out;
}

bool KioskUsbBridge::PathStartsWithMount(const std::string& file_path, const std::string& mount_path) {
    const std::string norm_mount = NormalizeMountPath(mount_path);
    if (norm_mount.empty() || file_path.size() < norm_mount.size()) {
        return false;
    }

    std::string lhs = file_path;
    std::string rhs = norm_mount;
#if defined(_WIN32)
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i] == '\\') lhs[i] = '/';
    }
    for (size_t i = 0; i < rhs.size(); ++i) {
        if (rhs[i] == '\\') rhs[i] = '/';
    }
    lhs = ToLowerAscii(lhs);
    rhs = ToLowerAscii(rhs);
#endif
    if (lhs.compare(0, rhs.size(), rhs) != 0) {
        return false;
    }
    if (lhs.size() == rhs.size()) {
        return true;
    }
    if (!rhs.empty() && rhs[rhs.size() - 1] == '/') {
        return true;
    }
    return lhs[rhs.size()] == '/';
}

std::string KioskUsbBridge::DeriveVolumeLabelFromPath(const std::string& mount_path) {
    const std::string norm = NormalizeMountPath(mount_path);
    if (norm.empty()) return "USB_DRIVE";
#if defined(_WIN32)
    if (norm.size() >= 2 && norm[1] == ':' &&
        (norm.size() == 2 || (norm.size() == 3 && (norm[2] == '\\' || norm[2] == '/')))) {
        char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(norm[0])));
        return std::string("USB_DRIVE_") + letter;
    }
#endif
    const std::string base = GetBaseName(norm);
    return base.empty() ? "USB_DRIVE" : base;
}

bool KioskUsbBridge::IsSupportedDiskImageExtension(const std::string& filename) {
    const std::string ext = GetFileExtensionLower(filename);
    return (ext == ".img" || ext == ".ima" || ext == ".vfd" ||
            ext == ".iso" || ext == ".cue");
}

bool KioskUsbBridge::IsImportableArchiveFilename(const std::string& filename, bool* out_is_gog) {
    const std::string base = ToLowerAscii(GetBaseName(filename));
    const std::string ext = GetFileExtensionLower(base);
    if (ext == ".zip") {
        if (out_is_gog) *out_is_gog = false;
        return true;
    }
    if (ext == ".exe" && base.compare(0, 6, "setup_") == 0) {
        if (out_is_gog) *out_is_gog = true;
        return true;
    }
    return false;
}

bool KioskUsbBridge::Initialize(const std::string& watch_root, bool enable_os_polling) {
    Shutdown();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_main_thread_id = std::this_thread::get_id();
        m_os_polling_enabled = enable_os_polling;
        m_last_poll_ticks = 0;
        m_last_drive_mask = 0;
        std::memset(m_drive_serials, 0, sizeof(m_drive_serials));
        m_pending_cache_invalidation = false;
        m_pending_toast_msg.clear();
        m_pending_toast_sec = 0.0f;
        m_pending_eject_mounts.clear();
        m_watch_roots.clear();
        m_watch_descriptors.clear();
        m_active_volumes.clear();
        m_discovered_archives.clear();

        if (!watch_root.empty()) {
            m_watch_roots.push_back(NormalizeMountPath(watch_root));
        } else {
#if !defined(_WIN32)
            m_watch_roots.push_back("/run/media/kiosk");
            const char* user = std::getenv("USER");
            if (user && user[0] != '\0') {
                m_watch_roots.push_back(std::string("/run/media/") + user);
                m_watch_roots.push_back(std::string("/media/") + user);
            }
#endif
        }
    }

#if defined(__linux__)
    if (enable_os_polling) {
        m_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_inotify_fd < 0) {
            LOG_MSG("KIOSK USB: inotify_init1 failed (%d); falling back to directory polling", errno);
        }
    }
#endif

    m_initialized.store(true);

    if (enable_os_polling) {
        RescanNow();
    }

    LOG_MSG("KIOSK USB: Removable storage bridge initialized (OS polling: %s)",
            enable_os_polling ? "enabled" : "disabled");
    return true;
}

void KioskUsbBridge::JoinScanThread() {
    m_scan_cancel.store(true);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending_forced_rescan = false;
    }
    if (m_scan_thread.joinable()) {
        m_scan_thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_scan_running = false;
        m_pending_forced_rescan = false;
    }
    m_scan_cancel.store(false);
}

void KioskUsbBridge::Shutdown() {
    JoinScanThread();
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(__linux__)
    if (m_inotify_fd >= 0) {
        close(m_inotify_fd);
        m_inotify_fd = -1;
    }
#endif
    m_watch_descriptors.clear();
    m_watch_roots.clear();
    m_active_volumes.clear();
    m_discovered_archives.clear();
    m_last_drive_mask = 0;
    std::memset(m_drive_serials, 0, sizeof(m_drive_serials));
    m_initialized.store(false);
}

void KioskUsbBridge::DispatchAsyncPoll(bool rescan_existing) {
    if (!m_initialized.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_scan_running) {
            if (rescan_existing) {
                m_pending_forced_rescan = true;
            }
            return;
        }
        m_scan_running = true;
        m_pending_forced_rescan = false;
    }
    if (m_scan_thread.joinable()) {
        m_scan_thread.join();
    }
    m_scan_cancel.store(false);
    m_scan_thread = std::thread([this, rescan_existing]() {
        bool current_rescan = rescan_existing;
        for (;;) {
            if (m_os_polling_enabled.load() && !m_scan_cancel.load()) {
#if defined(_WIN32)
                PollWindowsLogicalDrives();
#elif defined(__linux__)
                if (current_rescan) {
                    RefreshLinuxWatchRoots();
                    ScanExistingLinuxMounts();
                } else {
                    PollLinuxInotify();
                }
#endif
            }
            if (current_rescan && !m_scan_cancel.load()) {
                const std::vector<UsbVolumeInfo> active = GetActiveVolumes();
                for (size_t i = 0; i < active.size() && !m_scan_cancel.load(); ++i) {
                    OnVolumeMounted(active[i].mount_path, active[i].volume_label, active[i].is_optical, true);
                }
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_pending_forced_rescan && !m_scan_cancel.load()) {
                m_pending_forced_rescan = false;
                current_rescan = true;
                continue;
            }
            m_scan_running = false;
            break;
        }
    });
}

void KioskUsbBridge::DrainPendingMainThreadActions() {
    bool do_invalidate = false;
    std::string toast_msg;
    float toast_sec = 0.0f;
    std::vector<std::string> eject_mounts;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        do_invalidate = m_pending_cache_invalidation;
        m_pending_cache_invalidation = false;
        toast_msg.swap(m_pending_toast_msg);
        toast_sec = m_pending_toast_sec;
        m_pending_toast_sec = 0.0f;
        eject_mounts.swap(m_pending_eject_mounts);
    }

    for (size_t i = 0; i < eject_mounts.size(); ++i) {
        EjectActiveDrivesUnderMount(eject_mounts[i]);
    }
    if (do_invalidate) {
        KioskUI::GetInstance().InvalidateCatalogCache();
    }
    if (!toast_msg.empty()) {
        KioskUI::GetInstance().ShowToast(toast_msg, toast_sec);
    }
}

void KioskUsbBridge::PollEventsNonBlocking() {
    if (!m_initialized.load()) {
        return;
    }

    DrainPendingMainThreadActions();

    if (!m_os_polling_enabled.load()) {
        return;
    }

    const uint32_t now_ticks = SDL_GetTicks();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_last_poll_ticks != 0 && static_cast<uint32_t>(now_ticks - m_last_poll_ticks) < 500u) {
            return;
        }
        m_last_poll_ticks = now_ticks;
    }

    DispatchAsyncPoll(false);
}

void KioskUsbBridge::RescanNow() {
    if (!m_initialized.load()) {
        return;
    }
    DispatchAsyncPoll(true);
}

void KioskUsbBridge::RefreshLinuxWatchRoots() {
#if defined(__linux__)
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_inotify_fd < 0) return;
    for (size_t i = 0; i < m_watch_roots.size(); ++i) {
        const std::string& root = m_watch_roots[i];
        bool already_watched = false;
        for (size_t j = 0; j < m_watch_descriptors.size(); ++j) {
            if (m_watch_descriptors[j].root_path == root) {
                already_watched = true;
                break;
            }
        }
        if (!already_watched && DirectoryExists(root)) {
            int wd = inotify_add_watch(m_inotify_fd, root.c_str(),
                                       IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
            if (wd >= 0) {
                WatchEntry we;
                we.wd = wd;
                we.root_path = root;
                m_watch_descriptors.push_back(we);
            }
        }
    }
#endif
}

void KioskUsbBridge::ScanExistingLinuxMounts() {
#if defined(__linux__)
    std::vector<std::string> roots;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        roots = m_watch_roots;
    }

    std::vector<std::string> seen_mounts;
    std::vector<std::string> failed_roots;
    for (size_t i = 0; i < roots.size(); ++i) {
        struct stat parent_st;
        if (stat(roots[i].c_str(), &parent_st) != 0) {
            if (errno != ENOENT && errno != ENOTDIR) {
                failed_roots.push_back(roots[i]);
            }
            continue;
        }
        if (!S_ISDIR(parent_st.st_mode)) continue;
        const bool have_parent_st = true;
        bool list_ok = false;
        std::vector<DirEntryItem> subdirs = ListDirectoryEntries(roots[i], &list_ok);
        if (!list_ok) {
            failed_roots.push_back(roots[i]);
            continue;
        }
        for (size_t j = 0; j < subdirs.size(); ++j) {
            if (!subdirs[j].is_dir) continue;
            const std::string mp = NormalizeMountPath(KioskHost::JoinPath(roots[i], subdirs[j].name));
            // Wait until the filesystem is actually mounted (st_dev differs from parent mount root)
            if (have_parent_st) {
                struct stat child_st;
                if (stat(mp.c_str(), &child_st) != 0 || child_st.st_dev == parent_st.st_dev) {
                    continue;
                }
            }
            seen_mounts.push_back(mp);
            OnVolumeMounted(mp, subdirs[j].name, false, false);
        }
    }

    // Detect any previously active volumes that disappeared (skip roots whose opendir failed)
    std::vector<std::string> stale;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (size_t i = 0; i < m_active_volumes.size(); ++i) {
            bool under_failed_root = false;
            for (size_t f = 0; f < failed_roots.size(); ++f) {
                if (PathStartsWithMount(m_active_volumes[i].mount_path, failed_roots[f])) {
                    under_failed_root = true;
                    break;
                }
            }
            if (under_failed_root) continue;
            if (std::find(seen_mounts.begin(), seen_mounts.end(), m_active_volumes[i].mount_path) == seen_mounts.end()) {
                stale.push_back(m_active_volumes[i].mount_path);
            }
        }
    }
    for (size_t i = 0; i < stale.size(); ++i) {
        OnVolumeRemoved(stale[i]);
    }
#endif
}

void KioskUsbBridge::PollLinuxInotify() {
#if defined(__linux__)
    RefreshLinuxWatchRoots();

    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        fd = m_inotify_fd;
    }
    if (fd < 0) {
        ScanExistingLinuxMounts();
        return;
    }

    alignas(struct inotify_event) char buf[4096];
    for (;;) {
        ssize_t len = read(fd, buf, sizeof(buf));
        if (len <= 0) {
            break;
        }
        const char* ptr = buf;
        while (ptr < buf + len) {
            const struct inotify_event* ev = reinterpret_cast<const struct inotify_event*>(ptr);
            if (ev->len > 0 && (ev->mask & IN_ISDIR)) {
                std::string root_path;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (size_t i = 0; i < m_watch_descriptors.size(); ++i) {
                        if (m_watch_descriptors[i].wd == ev->wd) {
                            root_path = m_watch_descriptors[i].root_path;
                            break;
                        }
                    }
                }
                if (!root_path.empty()) {
                    const std::string mount_path = NormalizeMountPath(KioskHost::JoinPath(root_path, ev->name));
                    if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
                        OnVolumeRemoved(mount_path);
                    }
                }
            }
            ptr += sizeof(struct inotify_event) + ev->len;
        }
    }
    // Always reconcile mounted subdirs via st_dev check so mkdir-before-mount never races
    ScanExistingLinuxMounts();
#endif
}

void KioskUsbBridge::PollWindowsLogicalDrives() {
#if defined(_WIN32)
    ScopedWinErrorMode guard;
    const DWORD raw_mask = GetLogicalDrives();
    uint32_t active_removable_mask = 0;

    struct DriveProbe {
        int bit;
        std::string root;
        std::string label;
        uint32_t serial;
        bool is_optical;
    };
    std::vector<DriveProbe> current_drives;
    uint32_t current_serials[26];
    std::memset(current_serials, 0, sizeof(current_serials));

    // Start at C: (index 2) to avoid slow legacy floppy A:/B: polls on older hardware
    for (int i = 2; i < 26; ++i) {
        if ((raw_mask & (1u << i)) == 0) continue;
        wchar_t wroot[4] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0' };
        const UINT dtype = GetDriveTypeW(wroot);
        if (dtype != DRIVE_REMOVABLE && dtype != DRIVE_CDROM) {
            continue;
        }

        // Verify media is actually present (empty card readers / optical drives fail GetVolumeInformationW)
        wchar_t vol_name[MAX_PATH] = {0};
        DWORD serial = 0;
        if (!GetVolumeInformationW(wroot, vol_name, MAX_PATH, &serial, nullptr, nullptr, nullptr, 0)) {
            continue;
        }

        active_removable_mask |= (1u << i);
        current_serials[i] = static_cast<uint32_t>(serial);
        DriveProbe dp;
        dp.bit = i;
        dp.root = std::string(1, static_cast<char>('A' + i)) + ":\\";
        dp.label = WideToUtf8Local(vol_name);
        if (dp.label.empty()) {
            dp.label = DeriveVolumeLabelFromPath(dp.root);
        }
        dp.serial = static_cast<uint32_t>(serial);
        dp.is_optical = (dtype == DRIVE_CDROM);
        current_drives.push_back(dp);
    }

    uint32_t prev_mask = 0;
    uint32_t prev_serials[26];
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        prev_mask = m_last_drive_mask;
        std::memcpy(prev_serials, m_drive_serials, sizeof(prev_serials));
        m_last_drive_mask = active_removable_mask;
        std::memcpy(m_drive_serials, current_serials, sizeof(m_drive_serials));
    }

    // Handle removals and media swaps (serial change on same drive letter) first
    const uint32_t removed_mask = prev_mask & ~active_removable_mask;
    uint32_t swapped_mask = 0;
    for (int i = 2; i < 26; ++i) {
        const bool was_present = (prev_mask & (1u << i)) != 0;
        const bool now_present = (active_removable_mask & (1u << i)) != 0;
        if ((removed_mask & (1u << i)) != 0) {
            std::string root = std::string(1, static_cast<char>('A' + i)) + ":\\";
            OnVolumeRemoved(root);
        } else if (was_present && now_present && prev_serials[i] != current_serials[i]) {
            swapped_mask |= (1u << i);
            std::string root = std::string(1, static_cast<char>('A' + i)) + ":\\";
            OnVolumeRemoved(root);
        }
    }

    // Handle newly arrived or swapped drives
    const uint32_t added_or_swapped_mask = (active_removable_mask & ~prev_mask) | swapped_mask;
    for (size_t k = 0; k < current_drives.size(); ++k) {
        const int bit = current_drives[k].bit;
        if (bit >= 0 && bit < 26 && (added_or_swapped_mask & (1u << bit)) != 0) {
            if (!OnVolumeMounted(current_drives[k].root, current_drives[k].label, current_drives[k].is_optical)) {
                // Retry on next poll if transient arrival failed before becoming tracked
                std::lock_guard<std::mutex> lock(m_mutex);
                m_last_drive_mask &= ~(1u << bit);
                m_drive_serials[bit] = 0;
            }
        }
    }
#endif
}

void KioskUsbBridge::ScanVolumeRecursive(
    const std::string& dir,
    const std::string& mount_root,
    const std::string& volume_label,
    int depth,
    int max_depth,
    std::vector<MediaPoolRecord>& out_media_records,
    std::vector<UsbArchiveCandidate>& out_archives,
    std::string& out_docs_dir
) {
    if (depth > max_depth || m_scan_cancel.load()) return;

    std::vector<DirEntryItem> entries = ListDirectoryEntries(dir);
    for (size_t i = 0; i < entries.size(); ++i) {
        if (m_scan_cancel.load()) break;
        const std::string full_path = KioskHost::JoinPath(dir, entries[i].name);
        if (entries[i].is_dir) {
            if (depth == 0 && out_docs_dir.empty()) {
                const std::string lower_name = ToLowerAscii(entries[i].name);
                if (lower_name == "documents" || lower_name == "dos_files") {
                    out_docs_dir = full_path;
                }
            }
            if (depth < max_depth) {
                ScanVolumeRecursive(full_path, mount_root, volume_label,
                                    depth + 1, max_depth,
                                    out_media_records, out_archives, out_docs_dir);
            }
            continue;
        }

        if (IsSupportedDiskImageExtension(entries[i].name)) {
            MediaPoolRecord existing;
            if (KioskDatabase::GetInstance().IsOpen() &&
                !KioskDatabase::GetInstance().GetMediaByPath(full_path, existing)) {
                InspectedMediaInfo info;
                if (DiskCaddyView::InspectMediaFile(full_path, info)) {
                    MediaPoolRecord rec;
                    rec.id = BuildUsbMediaId(full_path);
                    rec.file_path = full_path;
                    rec.media_type = info.media_type;
                    rec.physical_style = info.physical_style;
                    rec.volume_label = info.volume_label;
                    rec.label_title = info.label_title.empty() ? entries[i].name : info.label_title;
                    rec.label_subtitle = info.label_subtitle;
                    rec.label_font_style = "DOT_MATRIX";
                    rec.has_redbook_audio = info.has_redbook_audio;
                    rec.is_write_protected = false;
                    rec.is_removable_usb = true;
                    rec.media_group = "USB: " + volume_label;
                    out_media_records.push_back(rec);
                }
            }
            continue;
        }

        bool is_gog = false;
        if (IsImportableArchiveFilename(entries[i].name, &is_gog)) {
            UsbArchiveCandidate cand;
            cand.file_path = full_path;
            cand.mount_path = mount_root;
            cand.display_name = entries[i].name;
            cand.is_gog_installer = is_gog;
            out_archives.push_back(cand);
        }
    }
}

bool KioskUsbBridge::OnVolumeMounted(const std::string& mount_path,
                                     const std::string& explicit_label,
                                     bool is_optical,
                                     bool force_rescan) {
    const std::string norm_path = NormalizeMountPath(mount_path);
    if (norm_path.empty() || !DirectoryExists(norm_path)) {
        return false;
    }

    bool already_tracked = false;
    uint64_t start_removal_gen = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        start_removal_gen = m_removal_generation;
        for (size_t i = 0; i < m_active_volumes.size(); ++i) {
            if (PathStartsWithMount(m_active_volumes[i].mount_path, norm_path) &&
                PathStartsWithMount(norm_path, m_active_volumes[i].mount_path)) {
                already_tracked = true;
                break;
            }
        }
    }
    if (already_tracked && !force_rescan) {
        return false;
    }

    const std::string label = explicit_label.empty()
                                  ? DeriveVolumeLabelFromPath(norm_path)
                                  : explicit_label;

    std::vector<MediaPoolRecord> candidate_media;
    std::vector<UsbArchiveCandidate> archives;
    std::string docs_dir;
    // Scan top 3 directory levels (depths 0..3); live hotplug invokes this on the worker thread
    ScanVolumeRecursive(norm_path, norm_path, label, 0, 3, candidate_media, archives, docs_dir);

    int media_added = 0;
    bool published = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_scan_cancel.load() || m_removal_generation != start_removal_gen) {
            return false; // A removal or shutdown occurred while scan was in flight; drop stale results
        }
        if (already_tracked) {
            bool still_tracked = false;
            size_t tracked_idx = 0;
            for (size_t i = 0; i < m_active_volumes.size(); ++i) {
                if (PathStartsWithMount(m_active_volumes[i].mount_path, norm_path) &&
                    PathStartsWithMount(norm_path, m_active_volumes[i].mount_path)) {
                    still_tracked = true;
                    tracked_idx = i;
                    break;
                }
            }
            if (!still_tracked) {
                return false; // Volume was removed while rescan was in flight; drop stale results
            }
            if (KioskDatabase::GetInstance().IsOpen()) {
                for (size_t k = 0; k < candidate_media.size(); ++k) {
                    if (KioskDatabase::GetInstance().InsertMedia(candidate_media[k])) {
                        ++media_added;
                    }
                }
            }
            m_active_volumes[tracked_idx].media_count += media_added;
            m_active_volumes[tracked_idx].archive_count = static_cast<int>(archives.size());
            m_active_volumes[tracked_idx].documents_dir = docs_dir;
            for (std::vector<UsbArchiveCandidate>::iterator it = m_discovered_archives.begin();
                 it != m_discovered_archives.end(); ) {
                if (PathStartsWithMount(it->mount_path, norm_path)) {
                    it = m_discovered_archives.erase(it);
                } else {
                    ++it;
                }
            }
            m_discovered_archives.insert(m_discovered_archives.end(), archives.begin(), archives.end());
            published = true;
        } else {
            if (KioskDatabase::GetInstance().IsOpen()) {
                for (size_t k = 0; k < candidate_media.size(); ++k) {
                    if (KioskDatabase::GetInstance().InsertMedia(candidate_media[k])) {
                        ++media_added;
                    }
                }
            }
            UsbVolumeInfo vol;
            vol.mount_path = norm_path;
            vol.volume_label = label;
            vol.is_optical = is_optical;
            vol.media_count = media_added;
            vol.archive_count = static_cast<int>(archives.size());
            vol.documents_dir = docs_dir;
            m_active_volumes.push_back(vol);
            m_discovered_archives.insert(m_discovered_archives.end(), archives.begin(), archives.end());
            published = true;
        }
    }
    if (!published) {
        return false;
    }
    m_change_revision.fetch_add(1);

    const bool on_main_thread = (std::this_thread::get_id() == m_main_thread_id);
    std::string toast_msg;
    float toast_sec = 0.0f;
    if (!already_tracked) {
        if (!archives.empty()) {
            std::ostringstream msg;
            msg << "Found " << archives.size() << " importable game archive"
                << (archives.size() == 1 ? "" : "s") << " on USB (" << label << ") — open Media Pool to Import.";
            toast_msg = msg.str();
            toast_sec = 5.0f;
        } else if (media_added > 0) {
            std::ostringstream msg;
            msg << "USB Drive '" << label << "' connected (" << media_added
                << " disk image" << (media_added == 1 ? "" : "s") << " added to Media Pool).";
            toast_msg = msg.str();
            toast_sec = 4.0f;
        }
    }

    if (on_main_thread) {
        if (media_added > 0) {
            KioskUI::GetInstance().InvalidateCatalogCache();
        }
        if (!toast_msg.empty()) {
            KioskUI::GetInstance().ShowToast(toast_msg, toast_sec);
        }
    } else {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (media_added > 0) {
            m_pending_cache_invalidation = true;
        }
        if (!toast_msg.empty()) {
            m_pending_toast_msg = toast_msg;
            m_pending_toast_sec = toast_sec;
        }
    }

    LOG_MSG("KIOSK USB: %s '%s' (%s) — %d new disk images, %u archives",
            already_tracked ? "Rescanned" : "Mounted",
            norm_path.c_str(), label.c_str(), media_added, static_cast<unsigned int>(archives.size()));
    return true;
}

void KioskUsbBridge::EjectActiveDrivesUnderMount(const std::string& mount_path) {
    RuntimeOverlay::GetInstance().RefreshDriveStatuses();
    std::vector<std::string> drives_to_eject;
    {
        const std::vector<RuntimeDriveSlotStatus>& statuses = RuntimeOverlay::GetInstance().GetDriveStatuses();
        for (size_t i = 0; i < statuses.size(); ++i) {
            if (statuses[i].is_mounted && !statuses[i].active_file_path.empty() &&
                PathStartsWithMount(statuses[i].active_file_path, mount_path)) {
                drives_to_eject.push_back(statuses[i].drive_letter);
            }
        }
    }
    for (size_t i = 0; i < drives_to_eject.size(); ++i) {
        LOG_MSG("KIOSK USB: Ejecting Drive %s: backed by removed volume '%s'",
                drives_to_eject[i].c_str(), mount_path.c_str());
        RuntimeOverlay::GetInstance().EjectDrive(drives_to_eject[i]);
    }
}

bool KioskUsbBridge::OnVolumeRemoved(const std::string& mount_path) {
    const std::string norm_path = NormalizeMountPath(mount_path);
    if (norm_path.empty()) return false;

    std::string removed_label;
    bool was_tracked = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_removal_generation;
        for (std::vector<UsbVolumeInfo>::iterator it = m_active_volumes.begin();
             it != m_active_volumes.end(); ) {
            if (PathStartsWithMount(it->mount_path, norm_path) &&
                PathStartsWithMount(norm_path, it->mount_path)) {
                removed_label = it->volume_label;
                it = m_active_volumes.erase(it);
                was_tracked = true;
            } else {
                ++it;
            }
        }

        for (std::vector<UsbArchiveCandidate>::iterator it = m_discovered_archives.begin();
             it != m_discovered_archives.end(); ) {
            if (PathStartsWithMount(it->mount_path, norm_path)) {
                it = m_discovered_archives.erase(it);
            } else {
                ++it;
            }
        }
    }

    const bool on_main_thread = (std::this_thread::get_id() == m_main_thread_id);
    // Safely release any active DOSBox-X image handles before purging DB records
    if (on_main_thread) {
        EjectActiveDrivesUnderMount(norm_path);
    } else {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending_eject_mounts.push_back(norm_path);
    }

    int deleted_rows = 0;
    if (KioskDatabase::GetInstance().IsOpen()) {
        deleted_rows = KioskDatabase::GetInstance().DeleteRemovableUsbMediaByPrefix(norm_path);
    }

    if (was_tracked || deleted_rows > 0) {
        m_change_revision.fetch_add(1);
        const std::string label = removed_label.empty() ? DeriveVolumeLabelFromPath(norm_path) : removed_label;
        const std::string msg = "Removable drive '" + label + "' disconnected.";
        if (on_main_thread) {
            KioskUI::GetInstance().InvalidateCatalogCache();
            KioskUI::GetInstance().ShowToast(msg, 3.5f);
        } else {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending_cache_invalidation = true;
            m_pending_toast_msg = msg;
            m_pending_toast_sec = 3.5f;
        }
        LOG_MSG("KIOSK USB: Removed '%s' (%d media_pool entries purged)", norm_path.c_str(), deleted_rows);
        return true;
    }

    return false;
}

bool KioskUsbBridge::SafeUnmountDevice(const std::string& mount_path) {
    const std::string norm_path = NormalizeMountPath(mount_path);
    if (norm_path.empty()) return false;

    // 1. Release any active DOSBox-X image handles before requesting OS dismount
    EjectActiveDrivesUnderMount(norm_path);

#if defined(__linux__)
    sync();
    if (umount2(norm_path.c_str(), 0) == 0) {
        OnVolumeRemoved(norm_path);
        LOG_MSG("KIOSK USB: Cleanly unmounted '%s'", norm_path.c_str());
        return true;
    }
    LOG_MSG("KIOSK USB: umount2('%s') returned errno=%d (keeping volume visible for retry)",
            norm_path.c_str(), errno);
    return false;
#elif defined(_WIN32)
    if (norm_path.size() >= 2 && norm_path[1] == ':') {
        const wchar_t drive_letter = static_cast<wchar_t>(std::toupper(static_cast<unsigned char>(norm_path[0])));
        wchar_t dev_path[16];
        std::swprintf(dev_path, 16, L"\\\\.\\%c:", drive_letter);
        HANDLE hVol = CreateFileW(dev_path, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (hVol == INVALID_HANDLE_VALUE) {
            LOG_MSG("KIOSK USB: CreateFileW('%c:') failed (%lu); volume remains active for retry",
                    static_cast<char>(drive_letter), static_cast<unsigned long>(GetLastError()));
            return false;
        }

        DWORD bytes = 0;
        const bool flushed = (FlushFileBuffers(hVol) != 0);
        const bool locked = flushed && (DeviceIoControl(hVol, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr) != 0);
        bool ejected = false;
        if (locked) {
            const bool dismounted = (DeviceIoControl(hVol, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr) != 0);
            if (dismounted) {
                ejected = (DeviceIoControl(hVol, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &bytes, nullptr) != 0);
            }
        }
        CloseHandle(hVol);

        if (ejected) {
            OnVolumeRemoved(norm_path);
            LOG_MSG("KIOSK USB: Cleanly locked, dismounted, and ejected '%s'", norm_path.c_str());
            return true;
        }
        LOG_MSG("KIOSK USB: Safe eject of '%s' rejected by OS (volume may be in use); keeping retryable",
                norm_path.c_str());
        return false;
    }
    return false;
#else
    return false;
#endif
}

std::vector<UsbVolumeInfo> KioskUsbBridge::GetActiveVolumes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_active_volumes;
}

std::vector<UsbArchiveCandidate> KioskUsbBridge::GetDiscoveredArchives() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_discovered_archives;
}

std::string KioskUsbBridge::GetPreferredUsbDocumentsDir() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = 0; i < m_active_volumes.size(); ++i) {
        if (!m_active_volumes[i].documents_dir.empty()) {
            return m_active_volumes[i].documents_dir;
        }
    }
    return "";
}

bool KioskUsbBridge::IsPathContainedInActiveRemovableMount(const std::string& file_path) const {
    if (file_path.empty()) return false;
    std::vector<UsbVolumeInfo> vols = GetActiveVolumes();
    if (vols.empty()) return false;

    std::string resolved_file;
#if defined(_WIN32)
    std::wstring wpath = Utf8ToWideLocal(file_path);
    if (wpath.empty()) return false;
    HANDLE h = CreateFileW(wpath.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info;
    const bool is_reg = (GetFileInformationByHandle(h, &info) != 0) &&
                        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
    typedef DWORD (WINAPI *GetFinalPathNameByHandleWFn)(HANDLE, LPWSTR, DWORD, DWORD);
    static GetFinalPathNameByHandleWFn pGetFinalPath =
        reinterpret_cast<GetFinalPathNameByHandleWFn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetFinalPathNameByHandleW"));
    wchar_t final_buf[MAX_PATH * 4];
    DWORD n = 0;
    if (is_reg && pGetFinalPath) {
        n = pGetFinalPath(h, final_buf, MAX_PATH * 4, 0 /* FILE_NAME_NORMALIZED */);
    } else if (is_reg) {
        n = GetFullPathNameW(wpath.c_str(), MAX_PATH * 4, final_buf, nullptr);
    }
    CloseHandle(h);
    if (!is_reg || n == 0 || n >= MAX_PATH * 4) return false;
    std::wstring wfinal(final_buf, n);
    if (wfinal.compare(0, 4, L"\\\\?\\") == 0) {
        wfinal = wfinal.substr(4);
    }
    resolved_file = WideToUtf8Local(wfinal);
#else
    char resolved_buf[PATH_MAX];
    if (!realpath(file_path.c_str(), resolved_buf)) return false;
    struct stat st;
    if (stat(resolved_buf, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    resolved_file = resolved_buf;
#endif

    for (size_t i = 0; i < vols.size(); ++i) {
        std::string resolved_root = vols[i].mount_path;
#if !defined(_WIN32)
        char root_buf[PATH_MAX];
        if (realpath(vols[i].mount_path.c_str(), root_buf)) {
            resolved_root = root_buf;
        }
#endif
        if (PathStartsWithMount(resolved_file, resolved_root)) {
            return true;
        }
    }
    return false;
}

#endif // defined(C_KIOSK)
