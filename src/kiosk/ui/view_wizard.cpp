#include "config.h"

#if defined(C_KIOSK)

#include "view_wizard.h"
#include "kiosk_ui.h"
#include "view_disk_caddy.h"
#include "../kiosk_main.h"
#include "../kiosk_db.h"
#include "../machine/archetypes.h"
#include "../machine/config_synthesizer.h"
#include "logging.h"
#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

bool EnsureDirectoryRecursive(const std::string& dir_path) {
    if (dir_path.empty()) return true;
#if defined(_WIN32)
    DWORD attrs = GetFileAttributesA(dir_path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    size_t slash = dir_path.find_last_of("\\/");
    if (slash != std::string::npos && slash > 0) {
        // Do not recurse into bare drive root like "C:"
        if (!(slash == 2 && dir_path[1] == ':')) {
            if (!EnsureDirectoryRecursive(dir_path.substr(0, slash))) {
                return false;
            }
        }
    }
    if (CreateDirectoryA(dir_path.c_str(), NULL) != 0) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    struct stat st;
    if (::stat(dir_path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    size_t slash = dir_path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        if (!EnsureDirectoryRecursive(dir_path.substr(0, slash))) {
            return false;
        }
    }
    if (::mkdir(dir_path.c_str(), 0755) == 0) {
        return true;
    }
    return errno == EEXIST;
#endif
}

bool EnsureParentDirectoryExists(const std::string& file_path) {
    size_t slash = file_path.find_last_of("\\/");
    if (slash == std::string::npos || slash == 0) {
        return true;
    }
    return EnsureDirectoryRecursive(file_path.substr(0, slash));
}

std::string SanitizeFilenameSlug(const std::string& input) {
    std::string out;
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        } else if (c == ' ' || c == '-' || c == '_') {
            if (!out.empty() && out.back() != '_') {
                out.push_back('_');
            }
        }
    }
    if (out.empty()) out = "retro_machine";
    return out;
}

} // anonymous namespace

ViewWizard& ViewWizard::GetInstance() {
    static ViewWizard instance;
    return instance;
}

ViewWizard::ViewWizard() {
    Reset();
}

void ViewWizard::Reset() {
    m_step = WizardStep::STEP_ARCHETYPE;
    m_draft = MachineRecord();
    m_draft.archetype_id = KioskArchetypes::DX_386;
    m_draft.archetype_tier = 3;
    m_draft.display_name = "My 386DX-33 Rig";
    m_draft.boot_mode = "INTEGRATED_DOS";
    m_draft.os_id.clear();
    m_draft.os_installed = false;
    m_hdd_size_mb = 250;
    m_name_edited_by_user = false;
    m_create_blank_hdd_file = true;
    m_installer_floppy_media_id.clear();
    m_installer_cd_media_id.clear();
    m_last_error.clear();

    std::snprintf(m_name_buf, sizeof(m_name_buf), "%s", m_draft.display_name.c_str());
    m_hdd_path_buf[0] = '\0';
    m_custom_chs_buf[0] = '\0';
}

bool ViewWizard::SetStep(WizardStep step) {
    if (step == WizardStep::STEP_OS_STORAGE && m_draft.display_name.empty()) {
        m_last_error = "Please enter a machine name before continuing.";
        return false;
    }
    if (step == WizardStep::STEP_REVIEW_CREATE) {
        if (!m_draft.os_id.empty()) {
            OSDistributionRecord os;
            if (KioskDatabase::GetInstance().IsOpen() &&
                KioskDatabase::GetInstance().GetOSDistribution(m_draft.os_id, os)) {
                std::string reason;
                if (!ArchetypeManager::GetInstance().ValidateMachineCompatibility(m_draft, os, reason)) {
                    m_last_error = reason;
                    return false;
                }
            }
        }
        if (m_draft.boot_mode == "REAL_HDD_BOOT") {
            if (m_create_blank_hdd_file) {
                m_draft.primary_hdd_chs = ComputeHddChsGeometry(m_hdd_size_mb);
            } else {
                // Imported existing HDD image: validate optional explicit CHS override, or empty for auto-detect
                std::string normalized_chs;
                if (!ParseAndNormalizeChs(m_custom_chs_buf, normalized_chs)) {
                    m_last_error = "Invalid CHS override format. Enter three positive integers 'C,H,S' (e.g., 1015,16,63) or leave blank.";
                    return false;
                }
                m_draft.primary_hdd_chs = normalized_chs;
            }
        }
    }
    m_last_error.clear();
    m_step = step;
    return true;
}

bool ViewWizard::SelectArchetype(const std::string& archetype_id) {
    const ArchetypeProfile* prof = ArchetypeManager::GetInstance().GetArchetype(archetype_id);
    if (!prof) {
        m_last_error = "Unknown archetype: " + archetype_id;
        return false;
    }
    m_draft.archetype_id = prof->id;
    m_draft.archetype_tier = prof->tier;

    if (!m_name_edited_by_user) {
        if (prof->id == KioskArchetypes::XT_8088)      m_draft.display_name = "1984 IBM PC/XT Rig";
        else if (prof->id == KioskArchetypes::AT_286)  m_draft.display_name = "1987 IBM PC/AT 286";
        else if (prof->id == KioskArchetypes::DX_386)  m_draft.display_name = "My 386DX-33 Rig";
        else if (prof->id == KioskArchetypes::DX2_486) m_draft.display_name = "1994 486DX2 Doom Rig";
        else                                           m_draft.display_name = "1998 Pentium 3dfx Rig";
        std::snprintf(m_name_buf, sizeof(m_name_buf), "%s", m_draft.display_name.c_str());
    }

    // Clear previously chosen OS if no longer compatible with the new tier
    if (!m_draft.os_id.empty() && KioskDatabase::GetInstance().IsOpen()) {
        OSDistributionRecord os;
        std::string reason;
        if (KioskDatabase::GetInstance().GetOSDistribution(m_draft.os_id, os) &&
            !ArchetypeManager::GetInstance().ValidateMachineCompatibility(m_draft, os, reason)) {
            m_draft.os_id.clear();
        }
    }
    m_last_error.clear();
    return true;
}

void ViewWizard::SetDisplayName(const std::string& name) {
    m_draft.display_name = name;
    m_name_edited_by_user = true;
    std::snprintf(m_name_buf, sizeof(m_name_buf), "%s", name.c_str());
}

void ViewWizard::SetBootMode(const std::string& boot_mode) {
    if (boot_mode == "REAL_HDD_BOOT" || boot_mode == "INTEGRATED_DOS") {
        m_draft.boot_mode = boot_mode;
    }
}

bool ViewWizard::SelectOSDistribution(const std::string& os_id, std::string& out_error) {
    if (os_id.empty()) {
        m_draft.os_id.clear();
        out_error.clear();
        return true;
    }
    OSDistributionRecord os;
    if (!KioskDatabase::GetInstance().IsOpen() ||
        !KioskDatabase::GetInstance().GetOSDistribution(os_id, os)) {
        out_error = "OS distribution not found: " + os_id;
        m_last_error = out_error;
        return false;
    }
    if (!ArchetypeManager::GetInstance().ValidateMachineCompatibility(m_draft, os, out_error)) {
        m_last_error = out_error;
        return false;
    }
    m_draft.os_id = os.id;
    m_draft.boot_mode = "REAL_HDD_BOOT";
    if (os.recommended_hdd_mb > 0) {
        m_hdd_size_mb = os.recommended_hdd_mb;
    }
    if (m_create_blank_hdd_file) {
        m_draft.primary_hdd_chs = ComputeHddChsGeometry(m_hdd_size_mb);
    }
    m_last_error.clear();
    out_error.clear();
    return true;
}

void ViewWizard::SetHddSizeMb(int size_mb) {
    if (size_mb < 10) size_mb = 10;
    if (size_mb > 4096) size_mb = 4096;
    m_hdd_size_mb = size_mb;
    if (m_create_blank_hdd_file) {
        m_draft.primary_hdd_chs = ComputeHddChsGeometry(m_hdd_size_mb);
    }
}

void ViewWizard::SetCreateBlankHddOnDisk(bool enable) {
    m_create_blank_hdd_file = enable;
    if (m_create_blank_hdd_file) {
        m_draft.primary_hdd_chs = ComputeHddChsGeometry(m_hdd_size_mb);
    } else {
        m_draft.primary_hdd_chs = m_custom_chs_buf;
    }
}

bool ViewWizard::ParseAndNormalizeChs(const std::string& input, std::string& out_normalized) {
    size_t first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        out_normalized.clear();
        return true; // Blank = IMGMOUNT auto-detect
    }
    size_t last = input.find_last_not_of(" \t\r\n");
    const std::string trimmed = input.substr(first, last - first + 1);

    int c = 0, h = 0, s = 0;
    char extra = '\0';
    if (std::sscanf(trimmed.c_str(), "%d , %d , %d %c", &c, &h, &s, &extra) != 3) {
        return false;
    }
    if (c < 1 || c > 65535 || h < 1 || h > 255 || s < 1 || s > 63) {
        return false;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d,%d,%d", c, h, s);
    out_normalized = buf;
    return true;
}

bool ViewWizard::SetCustomHddChsOverride(const std::string& chs) {
    std::snprintf(m_custom_chs_buf, sizeof(m_custom_chs_buf), "%s", chs.c_str());
    std::string normalized;
    if (!ParseAndNormalizeChs(chs, normalized)) {
        m_last_error = "Invalid CHS override format. Enter three positive integers 'C,H,S' (e.g., 1015,16,63) or leave blank.";
        return false;
    }
    m_last_error.clear();
    if (!m_create_blank_hdd_file) {
        m_draft.primary_hdd_chs = normalized;
    }
    return true;
}

void ViewWizard::SetPrimaryHddPath(const std::string& path) {
    m_draft.primary_hdd_path = path;
    std::snprintf(m_hdd_path_buf, sizeof(m_hdd_path_buf), "%s", path.c_str());
}

std::string ViewWizard::ComputeHddChsGeometry(int size_mb) {
    if (size_mb < 10) size_mb = 10;
    if (size_mb > 4096) size_mb = 4096;

    // Standard BIOS/IDE geometry: 16 heads, 63 sectors/track, 512 bytes/sector (~504 KB/cyl)
    const int heads = 16;
    const int sectors = 63;
    const int64_t total_bytes = static_cast<int64_t>(size_mb) * 1024LL * 1024LL;
    const int64_t bytes_per_cyl = static_cast<int64_t>(heads) * static_cast<int64_t>(sectors) * 512LL;
    int cylinders = static_cast<int>(total_bytes / bytes_per_cyl);
    if (cylinders < 20) cylinders = 20;
    if (cylinders > 8192) cylinders = 8192;

    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d,%d,%d", cylinders, heads, sectors);
    return std::string(buf);
}

HddCreateStatus ViewWizard::CreateBlankHddImageEx(const std::string& file_path, int size_mb, std::string& out_chs) {
    if (file_path.empty()) return HddCreateStatus::IO_ERROR;
    out_chs = ComputeHddChsGeometry(size_mb);

    int c = 0, h = 16, s = 63;
    if (std::sscanf(out_chs.c_str(), "%d,%d,%d", &c, &h, &s) != 3 || c <= 0) {
        return HddCreateStatus::IO_ERROR;
    }
    const int64_t exact_bytes = static_cast<int64_t>(c) * static_cast<int64_t>(h) * static_cast<int64_t>(s) * 512LL;
    if (exact_bytes <= 0) {
        return HddCreateStatus::IO_ERROR;
    }

    if (!EnsureParentDirectoryExists(file_path)) {
        return HddCreateStatus::IO_ERROR;
    }

#if defined(_WIN32)
    // Atomic exclusive creation (CREATE_NEW) guarantees we never truncate an existing disk image
    HANDLE hFile = CreateFileA(
        file_path.c_str(),
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        return (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS)
                   ? HddCreateStatus::ALREADY_EXISTS
                   : HddCreateStatus::IO_ERROR;
    }
    LARGE_INTEGER li;
    li.QuadPart = exact_bytes;
    bool ok = (SetFilePointerEx(hFile, li, NULL, FILE_BEGIN) != 0) && (SetEndOfFile(hFile) != 0);
    CloseHandle(hFile);
    if (!ok) {
        std::remove(file_path.c_str());
        return HddCreateStatus::IO_ERROR;
    }
    return HddCreateStatus::OK;
#else
    // Atomic exclusive creation (O_CREAT | O_EXCL) guarantees we never truncate an existing disk image
    int fd = ::open(file_path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        return (errno == EEXIST) ? HddCreateStatus::ALREADY_EXISTS : HddCreateStatus::IO_ERROR;
    }
    bool ok = (::ftruncate(fd, static_cast<off_t>(exact_bytes)) == 0);
    ::close(fd);
    if (!ok) {
        std::remove(file_path.c_str());
        return HddCreateStatus::IO_ERROR;
    }
    return HddCreateStatus::OK;
#endif
}

bool ViewWizard::CreateBlankHddImage(const std::string& file_path, int size_mb, std::string& out_chs) {
    return CreateBlankHddImageEx(file_path, size_mb, out_chs) == HddCreateStatus::OK;
}

std::string ViewWizard::GenerateDefaultHddPath(const std::string& machine_name, const std::string& machine_id) const {
    const std::string slug = SanitizeFilenameSlug(machine_name);
    // Include unique suffix from machine_id so names like "Doom Rig" and "doom-rig" never collide
    std::string suffix = machine_id;
    if (suffix.size() > 13) {
        suffix = suffix.substr(0, 13);
    }
    const std::string filename = slug + "_" + SanitizeFilenameSlug(suffix) + "_hdd_c.img";
#if defined(_WIN32)
    const char* data_dir = std::getenv("LOCALAPPDATA");
    if (!data_dir || !*data_dir) data_dir = std::getenv("TEMP");
    if (data_dir && *data_dir) {
        std::string base(data_dir);
        if (base.back() != '\\' && base.back() != '/') base += '\\';
        return base + "DOSBox-Kiosk\\machines\\" + filename;
    }
    return filename;
#else
    // Prefer appliance directory if writable; otherwise fall back to XDG/HOME user data dir or /tmp
    if (::access("/var/lib/dosbox-kiosk/machines", W_OK) == 0 ||
        ::access("/var/lib/dosbox-kiosk", W_OK) == 0) {
        return "/var/lib/dosbox-kiosk/machines/" + filename;
    }
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        std::string base(xdg);
        if (base.back() != '/') base += '/';
        return base + "dosbox-kiosk/machines/" + filename;
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        std::string base(home);
        if (base.back() != '/') base += '/';
        return base + ".local/share/dosbox-kiosk/machines/" + filename;
    }
    return "/tmp/dosbox-kiosk-machines/" + filename;
#endif
}

std::string ViewWizard::GenerateMachineUuid(const std::string& seed) {
    static std::atomic<uint64_t> counter(0);
    const uint64_t seq = ++counter;
    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()
    );
    std::random_device rd;
    const uint64_t r1 = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    const uint64_t r2 = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    const uint64_t h = static_cast<uint64_t>(DiskCaddyView::ComputeStringCrc32(seed));
    const uint64_t hi = r1 ^ now_ns ^ (seq * 0x9E3779B97F4A7C15ULL);
    const uint64_t lo = r2 ^ (h << 32) ^ (seq * 0xBF58476D1CE4E5B9ULL);

    char buf[44];
    std::snprintf(buf, sizeof(buf), "mach-%08x-%04x-4%03x-%x%03x-%08x%04x",
                  static_cast<unsigned int>((hi >> 32) & 0xFFFFFFFFu),
                  static_cast<unsigned int>((hi >> 16) & 0xFFFFu),
                  static_cast<unsigned int>(hi & 0x0FFFu),
                  static_cast<unsigned int>(8u | ((lo >> 60) & 0x3u)),
                  static_cast<unsigned int>((lo >> 48) & 0x0FFFu),
                  static_cast<unsigned int>((lo >> 16) & 0xFFFFFFFFu),
                  static_cast<unsigned int>(lo & 0xFFFFu));
    return std::string(buf);
}

bool ViewWizard::CommitMachine(std::string& out_machine_id, std::string& out_error) {
    if (m_draft.display_name.empty()) {
        out_error = "Machine display name cannot be empty.";
        m_last_error = out_error;
        return false;
    }

    const ArchetypeProfile* prof = ArchetypeManager::GetInstance().GetArchetype(m_draft.archetype_id);
    if (!prof) {
        out_error = "Invalid hardware archetype selected.";
        m_last_error = out_error;
        return false;
    }
    m_draft.archetype_tier = prof->tier;

    KioskDatabase& db = KioskDatabase::GetInstance();
    if (!db.IsOpen()) {
        out_error = "Catalog database is not open.";
        m_last_error = out_error;
        return false;
    }

    if (!m_draft.os_id.empty()) {
        OSDistributionRecord os;
        if (db.GetOSDistribution(m_draft.os_id, os)) {
            if (!ArchetypeManager::GetInstance().ValidateMachineCompatibility(m_draft, os, out_error)) {
                m_last_error = out_error;
                return false;
            }
        }
    }

    // Pre-validate optional installer media IDs before creating files or inserting machine
    if (!m_installer_floppy_media_id.empty()) {
        MediaPoolRecord probe;
        if (!db.GetMedia(m_installer_floppy_media_id, probe)) {
            out_error = "Selected Drive A: installer floppy no longer exists in Media Pool.";
            m_last_error = out_error;
            return false;
        }
    }
    if (!m_installer_cd_media_id.empty()) {
        MediaPoolRecord probe;
        if (!db.GetMedia(m_installer_cd_media_id, probe)) {
            out_error = "Selected Drive D: installer CD-ROM no longer exists in Media Pool.";
            m_last_error = out_error;
            return false;
        }
    }

    m_draft.id = GenerateMachineUuid(m_draft.display_name);
    bool created_new_hdd_file = false;

    if (m_draft.boot_mode == "REAL_HDD_BOOT") {
        if (m_create_blank_hdd_file) {
            const bool auto_generated_path = m_draft.primary_hdd_path.empty();
            std::string created_chs;
            HddCreateStatus st = HddCreateStatus::IO_ERROR;

            if (auto_generated_path) {
                // Retry exclusive creation with fresh UUIDs only on actual file collision (ALREADY_EXISTS)
                for (int attempt = 0; attempt < 5; ++attempt) {
                    if (attempt > 0) {
                        m_draft.id = GenerateMachineUuid(m_draft.display_name);
                    }
                    m_draft.primary_hdd_path = GenerateDefaultHddPath(m_draft.display_name, m_draft.id);
                    st = CreateBlankHddImageEx(m_draft.primary_hdd_path, m_hdd_size_mb, created_chs);
                    if (st != HddCreateStatus::ALREADY_EXISTS) {
                        break;
                    }
                }
            } else {
                st = CreateBlankHddImageEx(m_draft.primary_hdd_path, m_hdd_size_mb, created_chs);
            }

            if (st == HddCreateStatus::ALREADY_EXISTS) {
                out_error = "HDD image file already exists (refusing to overwrite): " + m_draft.primary_hdd_path;
                m_last_error = out_error;
                return false;
            }
            if (st != HddCreateStatus::OK) {
                out_error = "Unable to create HDD image directory or file (check path permissions): " + m_draft.primary_hdd_path;
                m_last_error = out_error;
                return false;
            }
            m_draft.primary_hdd_chs = created_chs;
            m_draft.os_installed = false;
            created_new_hdd_file = true;
        } else {
            // Importing an existing HDD image: require existing file & validated optional CHS override
            if (m_draft.primary_hdd_path.empty()) {
                out_error = "Please specify an existing HDD image path or choose 'Create New Blank HDD Image'.";
                m_last_error = out_error;
                return false;
            }
            std::ifstream existing_probe(m_draft.primary_hdd_path.c_str(), std::ios::binary);
            if (!existing_probe.is_open()) {
                out_error = "Existing HDD image file not found: " + m_draft.primary_hdd_path;
                m_last_error = out_error;
                return false;
            }
            std::string normalized_chs;
            if (!ParseAndNormalizeChs(m_custom_chs_buf, normalized_chs)) {
                out_error = "Invalid CHS override format. Enter three positive integers 'C,H,S' (e.g., 1015,16,63) or leave blank.";
                m_last_error = out_error;
                return false;
            }
            m_draft.primary_hdd_chs = normalized_chs;
            m_draft.os_installed = true;
        }
    }

    // Build installer bindings list and persist machine + bindings atomically in a single SQLite transaction
    std::vector<CaddyBindingRecord> requested_bindings;
    if (!m_installer_floppy_media_id.empty()) {
        CaddyBindingRecord b;
        b.machine_id = m_draft.id;
        b.media_id = m_installer_floppy_media_id;
        b.target_drive = "A";
        b.slot_order = 1;
        requested_bindings.push_back(b);
    }
    if (!m_installer_cd_media_id.empty()) {
        CaddyBindingRecord b;
        b.machine_id = m_draft.id;
        b.media_id = m_installer_cd_media_id;
        b.target_drive = "D";
        b.slot_order = 1;
        requested_bindings.push_back(b);
    }

    if (!db.InsertMachineWithBindings(m_draft, requested_bindings)) {
        if (created_new_hdd_file) {
            std::remove(m_draft.primary_hdd_path.c_str());
        }
        out_error = "Failed to save machine and installer bindings atomically to catalog database.";
        m_last_error = out_error;
        return false;
    }

    out_machine_id = m_draft.id;
    out_error.clear();
    m_last_error.clear();

    KioskUI::GetInstance().InvalidateCatalogCache();
    LOG_MSG("KIOSK Wizard: Created machine '%s' (%s, %s)",
            m_draft.display_name.c_str(), m_draft.archetype_id.c_str(), m_draft.boot_mode.c_str());
    return true;
}

bool ViewWizard::HandleAction(KioskInputAction action) {
    if (action == KioskInputAction::ACTION_BACK) {
        if (m_step == WizardStep::STEP_IDENTITY_BOOT) { m_step = WizardStep::STEP_ARCHETYPE; return true; }
        if (m_step == WizardStep::STEP_OS_STORAGE)    { m_step = WizardStep::STEP_IDENTITY_BOOT; return true; }
        if (m_step == WizardStep::STEP_REVIEW_CREATE) { m_step = WizardStep::STEP_OS_STORAGE; return true; }
    }
    return false;
}

void ViewWizard::RenderStepHeader() {
    const KioskThemePalette& pal = KioskUI::GetInstance().GetThemePalette();
    static const char* kStepLabels[4] = {
        "1. Hardware Archetype",
        "2. Identity & Boot Mode",
        "3. OS & Storage",
        "4. Review & Create"
    };

    const float hdr_right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    for (int i = 0; i < 4; ++i) {
        const bool is_cur = (static_cast<int>(m_step) == i);
        const bool is_done = (static_cast<int>(m_step) > i);
        char step_txt[64];
        std::snprintf(step_txt, sizeof(step_txt), is_cur ? "[ %s ]" : "  %s  ", kStepLabels[i]);
        const float item_w = ImGui::CalcTextSize(step_txt).x;
        if (i > 0 && (ImGui::GetItemRectMax().x + 10.0f + item_w <= hdr_right)) {
            ImGui::SameLine(0.0f, 10.0f);
        }
        ImVec4 col = is_cur ? pal.accent : (is_done ? pal.text_primary : pal.text_muted);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(step_txt);
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
    ImGui::Spacing();
}

void ViewWizard::RenderStep1Archetype() {
    const KioskThemePalette& pal = KioskUI::GetInstance().GetThemePalette();
    const std::vector<ArchetypeProfile> archetypes = ArchetypeManager::GetInstance().GetAllArchetypes();

    ImGui::TextWrapped("Choose a period-accurate PC hardware archetype. DOSBox-X will synthesize the CPU, video card, sound hardware, and bus configuration automatically.");
    ImGui::Spacing();

    ImGui::BeginChild("##WizardArchetypeList", ImVec2(-1.0f, 285.0f), false);
    for (size_t i = 0; i < archetypes.size(); ++i) {
        const ArchetypeProfile& p = archetypes[i];
        const bool selected = (m_draft.archetype_id == p.id);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? pal.accent_dim : pal.card_bg);
        ImGui::PushStyleColor(ImGuiCol_Border, selected ? pal.accent : pal.card_border);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);

        char child_id[48];
        std::snprintf(child_id, sizeof(child_id), "##arch_opt_%u", static_cast<unsigned int>(i));
        if (ImGui::BeginChild(child_id, ImVec2(-1.0f, 54.0f), true)) {
            if (ImGui::RadioButton(p.display_name.c_str(), selected)) {
                SelectArchetype(p.id);
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
            ImGui::Text("  (Tier %d | %d MB RAM | %s | %s)",
                        p.tier, p.mem_size_mb, p.machine_type.c_str(), p.sound_card.c_str());
            ImGui::PopStyleColor();

            ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
            ImGui::TextUnformatted(p.era_description.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    ImGui::EndChild();
}

void ViewWizard::RenderStep2IdentityBoot() {
    const KioskThemePalette& pal = KioskUI::GetInstance().GetThemePalette();

    ImGui::Text("Machine Display Name:");
    ImGui::SetNextItemWidth(420.0f);
    if (ImGui::InputText("##WizMachineName", m_name_buf, sizeof(m_name_buf))) {
        m_draft.display_name = m_name_buf;
        m_name_edited_by_user = true;
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Text("Boot Mode:");
    ImGui::Spacing();

    const bool is_integrated = (m_draft.boot_mode == "INTEGRATED_DOS");
    if (ImGui::RadioButton("Integrated DOS (Fast Container Mode)", is_integrated)) {
        SetBootMode("INTEGRATED_DOS");
    }
    ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
    ImGui::Indent(24.0f);
    ImGui::TextWrapped("Uses DOSBox-X's built-in DOS kernel with a host directory mounted as C:. Instant boot (<0.5s), ideal for most DOS games.");
    ImGui::Unindent(24.0f);
    ImGui::PopStyleColor();

    ImGui::Spacing();
    if (ImGui::RadioButton("Real Hard Disk Boot (MS-DOS / FreeDOS / Windows 9x)", !is_integrated)) {
        SetBootMode("REAL_HDD_BOOT");
    }
    ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
    ImGui::Indent(24.0f);
    ImGui::TextWrapped("Boots a raw FAT12/FAT16/FAT32 hard disk image (hdd_c.img) via BIOS MBR. Required for authentic MS-DOS 6.22, Windows 3.11, or Windows 95/98 installations.");
    ImGui::Unindent(24.0f);
    ImGui::PopStyleColor();

    ImGui::Spacing();
    if (is_integrated) {
        ImGui::TextUnformatted("Optional Host C: Directory Path:");
        ImGui::SetNextItemWidth((std::min)(480.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::InputTextWithHint("##WizHddPath", "Leave blank for automatic default path...", m_hdd_path_buf, sizeof(m_hdd_path_buf))) {
            m_draft.primary_hdd_path = m_hdd_path_buf;
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
        ImGui::TextWrapped("Next step (3. OS & Storage) lets you create a new blank hard disk image (.img) with custom capacity or attach an existing HDD image.");
        ImGui::PopStyleColor();
    }
}

void ViewWizard::RenderStep3OsStorage() {
    const KioskThemePalette& pal = KioskUI::GetInstance().GetThemePalette();
    const std::vector<OSDistributionRecord>& os_list = KioskUI::GetInstance().GetCachedOSDistributions();
    const std::vector<MediaPoolRecord>& pool = KioskUI::GetInstance().GetCachedMedia();

    ImGui::Text("Select Target Operating System (Hardware Tier %d Compatibility Enforced):", m_draft.archetype_tier);
    ImGui::Spacing();

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const int os_cols = (avail_w >= 680.0f) ? 2 : 1;
    const float os_list_h = (os_cols == 2) ? 156.0f : 185.0f;

    ImGui::BeginChild("##WizardOsList", ImVec2(-1.0f, os_list_h), true);
    {
        if (ImGui::BeginTable("##WizardOsGrid", os_cols, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            const bool none_selected = m_draft.os_id.empty();
            if (ImGui::RadioButton("None / DOSBox-X Built-in DOS", none_selected)) {
                m_draft.os_id.clear();
            }

            for (size_t i = 0; i < os_list.size(); ++i) {
                ImGui::TableNextColumn();
                const OSDistributionRecord& os = os_list[i];
                std::string reason;
                const bool compat = ArchetypeManager::GetInstance().ValidateMachineCompatibility(m_draft, os, reason);
                const bool selected = (m_draft.os_id == os.id);

                ImGui::BeginDisabled(!compat);
                char label[192];
                std::snprintf(label, sizeof(label), "%s [%s | %d MB]##os_%u",
                              os.display_name.c_str(),
                              os.filesystem_type.c_str(),
                              os.recommended_hdd_mb,
                              static_cast<unsigned int>(i));
                if (ImGui::RadioButton(label, selected)) {
                    std::string err;
                    SelectOSDistribution(os.id, err);
                }
                ImGui::EndDisabled();

                if (!compat) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.42f, 0.35f, 1.0f));
                    ImGui::Text("(Tier %d+)", os.min_archetype_tier);
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("%s", reason.c_str());
                    }
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Hard Disk Image (Drive C:):");

    if (m_draft.boot_mode != "REAL_HDD_BOOT") {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
        ImGui::TextWrapped("Currently set to Integrated DOS (host folder C:). Select an OS above or click below to create a virtual hard disk image (.img).");
        ImGui::PopStyleColor();
        if (ImGui::Button("Create Virtual Hard Disk Image (Switch to Real HDD Boot)")) {
            SetBootMode("REAL_HDD_BOOT");
            SetCreateBlankHddOnDisk(true);
        }
    } else {
        if (ImGui::RadioButton("Create New Blank HDD Image", m_create_blank_hdd_file)) {
            SetCreateBlankHddOnDisk(true);
        }
        ImGui::SameLine(0.0f, 16.0f);
        if (ImGui::RadioButton("Use Existing HDD Image", !m_create_blank_hdd_file)) {
            SetCreateBlankHddOnDisk(false);
        }

        if (m_create_blank_hdd_file) {
            ImGui::Text("Presets:");
            static const int kPresets[6] = {20, 120, 250, 500, 1024, 2048};
            static const char* kPresetLabels[6] = {"20 MB", "120 MB", "250 MB", "500 MB", "1 GB", "2 GB"};
            for (int p = 0; p < 6; ++p) {
                ImGui::SameLine();
                const bool active = (m_hdd_size_mb == kPresets[p]);
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));
                }
                if (ImGui::SmallButton(kPresetLabels[p])) {
                    SetHddSizeMb(kPresets[p]);
                }
                if (active) {
                    ImGui::PopStyleColor(2);
                }
            }

            ImGui::Text("Capacity:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderInt("##WizHddSize", &m_hdd_size_mb, 10, 4096, "%d MB")) {
                m_draft.primary_hdd_chs = ComputeHddChsGeometry(m_hdd_size_mb);
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
            ImGui::Text("CHS: %s", ComputeHddChsGeometry(m_hdd_size_mb).c_str());
            ImGui::PopStyleColor();

            ImGui::Text("Image File Path:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth((std::min)(420.0f, ImGui::GetContentRegionAvail().x));
            if (ImGui::InputTextWithHint("##WizNewHddPath", "Auto-create in Kiosk machines folder (or enter custom .img path)...", m_hdd_path_buf, sizeof(m_hdd_path_buf))) {
                m_draft.primary_hdd_path = m_hdd_path_buf;
            }

            ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
            ImGui::TextWrapped("A new %d MB blank HDD image (CHS %s) will be created automatically when you finish.",
                               m_hdd_size_mb, ComputeHddChsGeometry(m_hdd_size_mb).c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::Text("Existing HDD Image Path:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth((std::min)(420.0f, ImGui::GetContentRegionAvail().x));
            if (ImGui::InputTextWithHint("##WizExistingHddPath", "Path to existing hdd_c.img...", m_hdd_path_buf, sizeof(m_hdd_path_buf))) {
                m_draft.primary_hdd_path = m_hdd_path_buf;
            }
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::InputTextWithHint("Optional CHS Override", "e.g. 1015,16,63 (blank = auto)", m_custom_chs_buf, sizeof(m_custom_chs_buf))) {
                SetCustomHddChsOverride(m_custom_chs_buf);
            }
        }
    }

    if (!pool.empty()) {
        ImGui::Spacing();
        ImGui::Text("Optional Installer Media Binding (from Global Media Pool):");
        const float combo_w = (std::min)(240.0f, (std::max)(140.0f, ImGui::GetContentRegionAvail().x * 0.42f));
        ImGui::SetNextItemWidth(combo_w);
        std::string cur_floppy = m_installer_floppy_media_id.empty() ? "(None)" : m_installer_floppy_media_id;
        std::string cur_cd = m_installer_cd_media_id.empty() ? "(None)" : m_installer_cd_media_id;
        for (size_t i = 0; i < pool.size(); ++i) {
            if (pool[i].id == m_installer_floppy_media_id) cur_floppy = DiskCaddyView::FormatMediaDisplayName(pool[i]);
            if (pool[i].id == m_installer_cd_media_id) cur_cd = DiskCaddyView::FormatMediaDisplayName(pool[i]);
        }
        if (ImGui::BeginCombo("Drive A: Floppy", cur_floppy.c_str())) {
            if (ImGui::Selectable("(None)##flp_none", m_installer_floppy_media_id.empty())) {
                m_installer_floppy_media_id.clear();
            }
            for (size_t i = 0; i < pool.size(); ++i) {
                if (pool[i].media_type.find("FLOPPY") != std::string::npos) {
                    ImGui::PushID(static_cast<int>(i));
                    const std::string disp = DiskCaddyView::FormatMediaDisplayName(pool[i]);
                    if (ImGui::Selectable(disp.c_str(), m_installer_floppy_media_id == pool[i].id)) {
                        m_installer_floppy_media_id = pool[i].id;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(combo_w);
        if (ImGui::BeginCombo("Drive D: CD-ROM", cur_cd.c_str())) {
            if (ImGui::Selectable("(None)##cd_none", m_installer_cd_media_id.empty())) {
                m_installer_cd_media_id.clear();
            }
            for (size_t i = 0; i < pool.size(); ++i) {
                if (pool[i].media_type == "CDROM_ISO" || pool[i].media_type == "CDROM_CUE") {
                    ImGui::PushID(static_cast<int>(i));
                    const std::string disp = DiskCaddyView::FormatMediaDisplayName(pool[i]);
                    if (ImGui::Selectable(disp.c_str(), m_installer_cd_media_id == pool[i].id)) {
                        m_installer_cd_media_id = pool[i].id;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndCombo();
        }
    }
}

void ViewWizard::RenderStep4ReviewCreate() {
    const KioskThemePalette& pal = KioskUI::GetInstance().GetThemePalette();
    const ArchetypeProfile* prof = ArchetypeManager::GetInstance().GetArchetype(m_draft.archetype_id);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.card_bg);
    ImGui::BeginChild("##WizReviewSummary", ImVec2(-1.0f, 128.0f), true);
    ImGui::Text("Name:       %s", m_draft.display_name.c_str());
    ImGui::Text("Archetype:  %s (Tier %d)", prof ? prof->display_name.c_str() : m_draft.archetype_id.c_str(), m_draft.archetype_tier);
    ImGui::Text("Boot Mode:  %s", m_draft.boot_mode.c_str());
    ImGui::Text("Target OS:  %s", m_draft.os_id.empty() ? "DOSBox-X Integrated DOS" : m_draft.os_id.c_str());
    if (m_draft.boot_mode == "REAL_HDD_BOOT") {
        if (m_create_blank_hdd_file) {
            const std::string target_desc = m_draft.primary_hdd_path.empty()
                                                ? "(Auto-generated in Kiosk machines folder)"
                                                : m_draft.primary_hdd_path;
            ImGui::Text("HDD Image:  [NEW %d MB Blank] %s (CHS: %s)",
                        m_hdd_size_mb, target_desc.c_str(), m_draft.primary_hdd_chs.c_str());
        } else {
            ImGui::Text("HDD Image:  [Existing] %s (CHS: %s)",
                        m_draft.primary_hdd_path.c_str(),
                        m_draft.primary_hdd_chs.empty() ? "Auto-detect" : m_draft.primary_hdd_chs.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Synthesized DOSBox-X Config Preview")) {
        MachineRecord preview_draft = m_draft;
        if (preview_draft.boot_mode == "REAL_HDD_BOOT" && m_create_blank_hdd_file && preview_draft.primary_hdd_path.empty()) {
            preview_draft.primary_hdd_path = "<auto_generated_hdd_c.img>";
        }
        const std::string preview = ConfigSynthesizer::GetInstance().SynthesizeConfig(preview_draft);
        ImGui::BeginChild("##WizConfPreview", ImVec2(-1.0f, 125.0f), true);
        ImGui::TextUnformatted(preview.c_str());
        ImGui::EndChild();
    }
}

void ViewWizard::Render() {
    RenderStepHeader();

    if (!m_last_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.35f, 1.0f));
        ImGui::TextWrapped("Error: %s", m_last_error.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    switch (m_step) {
        case WizardStep::STEP_ARCHETYPE:     RenderStep1Archetype(); break;
        case WizardStep::STEP_IDENTITY_BOOT: RenderStep2IdentityBoot(); break;
        case WizardStep::STEP_OS_STORAGE:    RenderStep3OsStorage(); break;
        case WizardStep::STEP_REVIEW_CREATE: RenderStep4ReviewCreate(); break;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Navigation footer
    if (m_step != WizardStep::STEP_ARCHETYPE) {
        if (ImGui::Button("< Back", ImVec2(110.0f, 34.0f))) {
            m_step = static_cast<WizardStep>(static_cast<int>(m_step) - 1);
            m_last_error.clear();
        }
        ImGui::SameLine();
    }

    if (m_step != WizardStep::STEP_REVIEW_CREATE) {
        if (ImGui::Button("Next >", ImVec2(120.0f, 34.0f))) {
            SetStep(static_cast<WizardStep>(static_cast<int>(m_step) + 1));
        }
    } else {
        if (ImGui::Button("Create Machine", ImVec2(155.0f, 34.0f))) {
            std::string new_id, err;
            if (CommitMachine(new_id, err)) {
                KioskUI::GetInstance().ShowToast("Created machine '" + m_draft.display_name + "'");
                Reset();
                KioskUI::GetInstance().SetActiveTab(KioskHubTab::MACHINES);
            }
        }
        ImGui::SameLine();
        const bool blank_without_installer =
            (m_draft.boot_mode == "REAL_HDD_BOOT" && m_create_blank_hdd_file &&
             m_installer_floppy_media_id.empty() && m_installer_cd_media_id.empty());
        ImGui::BeginDisabled(blank_without_installer);
        if (ImGui::Button("Create & Boot Now", ImVec2(170.0f, 34.0f))) {
            std::string new_id, err;
            if (CommitMachine(new_id, err)) {
                Reset();
                KioskManager::GetInstance().RequestLaunchMachine(new_id);
            }
        }
        ImGui::EndDisabled();
        if (blank_without_installer && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Attach an OS installer floppy (Drive A:) or CD-ROM (Drive D:) in Step 3 to boot a blank HDD.");
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset Wizard", ImVec2(120.0f, 34.0f))) {
        Reset();
    }
}

#endif // defined(C_KIOSK)
