#include "config.h"

#if defined(C_KIOSK)

#include "kiosk_host.h"
#include "logging.h"
#include "SDL.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <cwchar>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#endif

#if defined(HAVE_LIBSYSTEMD)
#include <systemd/sd-daemon.h>
#endif

namespace {

#if defined(_WIN32)
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], needed);
    return out;
}

std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), &out[0], needed, nullptr, nullptr);
    return out;
}

bool EnsureDirRecursiveWide(const std::wstring& dir_path) {
    if (dir_path.empty()) return false;
    std::wstring cur;
    for (size_t i = 0; i < dir_path.size(); ++i) {
        wchar_t c = dir_path[i];
        cur.push_back(c);
        if ((c == L'\\' || c == L'/') && cur.size() > 1) {
            if (cur.size() == 3 && cur[1] == L':') continue;
            _wmkdir(cur.c_str());
        }
    }
    _wmkdir(dir_path.c_str());
    DWORD attrs = GetFileAttributesW(dir_path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
}
#else
bool EqualsIgnoreCaseAscii(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0');
}
#endif

} // anonymous namespace

KioskHostProfile KioskHost::DetectHostProfile() {
#if defined(_WIN32)
    return KioskHostProfile::DESKTOP_BIG_PICTURE;
#else
    const char* force_appliance = std::getenv("DOSBOX_KIOSK_APPLIANCE");
    if (force_appliance && (std::strcmp(force_appliance, "1") == 0 ||
                            EqualsIgnoreCaseAscii(force_appliance, "true"))) {
        return KioskHostProfile::APPLIANCE_KMSDRM;
    }

    const char* video_drv = std::getenv("SDL_VIDEODRIVER");
    if (video_drv && EqualsIgnoreCaseAscii(video_drv, "kmsdrm")) {
        return KioskHostProfile::APPLIANCE_KMSDRM;
    }

    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const bool has_desktop_session =
        (display && display[0] != '\0') || (wayland && wayland[0] != '\0');
    if (!has_desktop_session && access("/var/lib/dosbox-kiosk", W_OK) == 0) {
        return KioskHostProfile::APPLIANCE_KMSDRM;
    }

    return KioskHostProfile::DESKTOP_BIG_PICTURE;
#endif
}

const char* KioskHost::GetHostProfileDisplayName(KioskHostProfile profile) {
    switch (profile) {
        case KioskHostProfile::APPLIANCE_KMSDRM:
            return "Buildroot KMSDRM Appliance";
        case KioskHostProfile::DESKTOP_BIG_PICTURE:
        default:
            return "Desktop Big-Picture";
    }
}

namespace {
std::string HostToLowerAscii(const std::string& s) {
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
    return out;
}
} // anonymous namespace

std::string KioskHost::NormalizeDirPath(const std::string& path) {
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

bool KioskHost::PathIsWithin(const std::string& file_path, const std::string& mount_path) {
    const std::string norm_mount = NormalizeDirPath(mount_path);
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
    lhs = HostToLowerAscii(lhs);
    rhs = HostToLowerAscii(rhs);
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

std::string KioskHost::JoinPath(const std::string& base, const std::string& child) {
    if (base.empty()) return child;
    if (child.empty()) return base;
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    const char last = base[base.size() - 1];
    if (last == '/' || last == '\\') {
        return base + child;
    }
    return base + sep + child;
}

bool KioskHost::EnsureDirectoryRecursive(const std::string& dir_path) {
    if (dir_path.empty()) return false;
#if defined(_WIN32)
    return EnsureDirRecursiveWide(Utf8ToWide(dir_path));
#else
    std::string cur;
    for (size_t i = 0; i < dir_path.size(); ++i) {
        char c = dir_path[i];
        cur.push_back(c);
        if ((c == '/' || c == '\\') && cur.size() > 1) {
            mkdir(cur.c_str(), 0755);
        }
    }
    mkdir(dir_path.c_str(), 0755);
    struct stat st;
    return (stat(dir_path.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
#endif
}

bool KioskHost::FileExists(const std::string& file_path) {
    if (file_path.empty()) return false;
#if defined(_WIN32)
    std::wstring wp = Utf8ToWide(file_path);
    if (wp.empty()) return false;
    DWORD attrs = GetFileAttributesW(wp.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
#else
    struct stat st;
    return (stat(file_path.c_str(), &st) == 0) && S_ISREG(st.st_mode);
#endif
}

bool KioskHost::IsFileConfirmedMissing(const std::string& file_path) {
    if (file_path.empty()) return true;
#if defined(_WIN32)
    std::wstring wp = Utf8ToWide(file_path);
    if (wp.empty()) return false;
    DWORD attrs = GetFileAttributesW(wp.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return false; // Path exists (file or directory)
    }
    const DWORD err = GetLastError();
    return (err == ERROR_FILE_NOT_FOUND ||
            err == ERROR_PATH_NOT_FOUND ||
            err == ERROR_NOT_READY ||
            err == ERROR_DEV_NOT_EXIST);
#else
    struct stat st;
    if (stat(file_path.c_str(), &st) == 0) {
        return false; // Path exists
    }
    return (errno == ENOENT || errno == ENOTDIR);
#endif
}

std::string KioskHost::GetDataRootDir() {
#if defined(_WIN32)
    const wchar_t* local_app = _wgetenv(L"LOCALAPPDATA");
    if (local_app && local_app[0] != L'\0') {
        std::string utf8 = WideToUtf8(std::wstring(local_app) + L"\\DOSBox-Kiosk");
        if (!utf8.empty()) return utf8;
    }
    const wchar_t* app_data = _wgetenv(L"APPDATA");
    if (app_data && app_data[0] != L'\0') {
        std::string utf8 = WideToUtf8(std::wstring(app_data) + L"\\DOSBox-Kiosk");
        if (!utf8.empty()) return utf8;
    }
    return ".";
#else
    if (access("/var/lib/dosbox-kiosk", W_OK) == 0) {
        return "/var/lib/dosbox-kiosk";
    }
    const char* xdg_data = std::getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0] != '\0') {
        return JoinPath(xdg_data, "dosbox-kiosk");
    }
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return JoinPath(JoinPath(JoinPath(home, ".local"), "share"), "dosbox-kiosk");
    }
    return ".";
#endif
}

std::string KioskHost::GetConfigDir(const std::string& root_override) {
    const std::string root = root_override.empty() ? GetDataRootDir() : root_override;
    return JoinPath(root, "config");
}

std::string KioskHost::GetMediaPoolDir(const std::string& root_override) {
    const std::string root = root_override.empty() ? GetDataRootDir() : root_override;
    return JoinPath(root, "media_pool");
}

std::string KioskHost::GetOSInstallersDir(const std::string& root_override) {
    const std::string root = root_override.empty() ? GetDataRootDir() : root_override;
    return JoinPath(root, "os_installers");
}

std::string KioskHost::GetIngestStagingDir(const std::string& root_override) {
    const std::string root = root_override.empty() ? GetDataRootDir() : root_override;
    return JoinPath(root, "ingest_staging");
}

std::string KioskHost::GetMachinesDir(const std::string& root_override) {
    const std::string root = root_override.empty() ? GetDataRootDir() : root_override;
    return JoinPath(root, "machines");
}

std::string KioskHost::GetIconCacheDir(const std::string& root_override) {
    const std::string root = root_override.empty() ? GetDataRootDir() : root_override;
    return JoinPath(JoinPath(root, "cache"), "icons");
}

std::string KioskHost::GetDefaultDatabasePath(const std::string& root_override) {
    const std::string root = root_override.empty() ? GetDataRootDir() : root_override;
    if (root_override.empty() && root == ".") {
        return JoinPath(".", "dosbox-kiosk-catalog.db");
    }
    return JoinPath(root, "catalog.db");
}

std::string KioskHost::GetDefaultBaseConfPath(const std::string& root_override) {
    return JoinPath(GetConfigDir(root_override), "kiosk_base.conf");
}

std::string KioskHost::GetDefaultMapperPath(const std::string& root_override) {
    return JoinPath(GetConfigDir(root_override), "kiosk_mapper.map");
}

bool KioskHost::EnsureDataDirectories(const std::string& root_override) {
    const std::string root = root_override.empty() ? GetDataRootDir() : root_override;
    bool ok = EnsureDirectoryRecursive(root);
    ok = EnsureDirectoryRecursive(GetConfigDir(root)) && ok;
    ok = EnsureDirectoryRecursive(GetMediaPoolDir(root)) && ok;
    ok = EnsureDirectoryRecursive(GetOSInstallersDir(root)) && ok;
    ok = EnsureDirectoryRecursive(GetIngestStagingDir(root)) && ok;
    ok = EnsureDirectoryRecursive(GetMachinesDir(root)) && ok;
    ok = EnsureDirectoryRecursive(GetIconCacheDir(root)) && ok;
    return ok;
}

bool KioskHost::IsSystemdSupportCompiled() {
#if defined(HAVE_LIBSYSTEMD)
    return true;
#else
    return false;
#endif
}

void KioskHost::NotifySystemdReady() {
#if defined(HAVE_LIBSYSTEMD)
    sd_notify(0, "READY=1");
#endif
}

void KioskHost::NotifySystemdStopping() {
#if defined(HAVE_LIBSYSTEMD)
    sd_notify(0, "STOPPING=1");
#endif
}

void KioskHost::PingSystemdWatchdog(uint32_t min_interval_ms) {
#if defined(HAVE_LIBSYSTEMD)
    static std::atomic<uint32_t> last_ping_ms(0);
    const uint32_t now_ms = SDL_GetTicks();
    uint32_t prev_ms = last_ping_ms.load(std::memory_order_relaxed);
    if (prev_ms != 0 && static_cast<uint32_t>(now_ms - prev_ms) < min_interval_ms) {
        return;
    }
    if (last_ping_ms.compare_exchange_strong(prev_ms, now_ms, std::memory_order_relaxed)) {
        sd_notify(0, "WATCHDOG=1");
    }
#else
    (void)min_interval_ms;
#endif
}

#endif // defined(C_KIOSK)
