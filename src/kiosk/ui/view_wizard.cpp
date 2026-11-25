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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

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
                // Imported existing HDD image: preserve image's own geometry (omit -size)
                m_draft.primary_hdd_chs.clear();
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
    if (os.recommended_hdd_mb > 0) {
        m_hdd_size_mb = os.recommended_hdd_mb;
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
        m_draft.primary_hdd_chs.clear();
    }
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

bool ViewWizard::CreateBlankHddImage(const std::string& file_path, int size_mb, std::string& out_chs) {
    if (file_path.empty()) return false;
    out_chs = ComputeHddChsGeometry(size_mb);

    int c = 0, h = 16, s = 63;
    if (std::sscanf(out_chs.c_str(), "%d,%d,%d", &c, &h, &s) != 3 || c <= 0) {
        return false;
    }
    const int64_t exact_bytes = static_cast<int64_t>(c) * static_cast<int64_t>(h) * static_cast<int64_t>(s) * 512LL;
    if (exact_bytes <= 0) {
        return false;
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
        return false;
    }
    LARGE_INTEGER li;
    li.QuadPart = exact_bytes;
    bool ok = (SetFilePointerEx(hFile, li, NULL, FILE_BEGIN) != 0) && (SetEndOfFile(hFile) != 0);
    CloseHandle(hFile);
    if (!ok) {
        std::remove(file_path.c_str());
    }
    return ok;
#else
    // Atomic exclusive creation (O_CREAT | O_EXCL) guarantees we never truncate an existing disk image
    int fd = ::open(file_path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        return false;
    }
    bool ok = (::ftruncate(fd, static_cast<off_t>(exact_bytes)) == 0);
    ::close(fd);
    if (!ok) {
        std::remove(file_path.c_str());
    }
    return ok;
#endif
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
    const char* temp_dir = std::getenv("LOCALAPPDATA");
    if (!temp_dir || !*temp_dir) temp_dir = std::getenv("TEMP");
    if (temp_dir && *temp_dir) {
        std::string base(temp_dir);
        if (base.back() != '\\' && base.back() != '/') base += '\\';
        return base + filename;
    }
    return filename;
#else
    return "/var/lib/dosbox-kiosk/machines/" + filename;
#endif
}

std::string ViewWizard::GenerateMachineUuid(const std::string& seed) {
    const uint32_t t = static_cast<uint32_t>(std::time(nullptr));
    static std::atomic<uint32_t> counter(0);
    const uint32_t seq = ++counter;
    std::ostringstream ss;
    ss << seed << "_" << t << "_" << seq;
    const uint32_t h1 = DiskCaddyView::ComputeStringCrc32(ss.str());
    const uint32_t h2 = DiskCaddyView::ComputeStringCrc32(ss.str() + "_mach");
    char buf[40];
    std::snprintf(buf, sizeof(buf), "mach-%08x-%04x-4%03x-8%03x-%08x",
                  h1,
                  (h2 >> 16) & 0xFFFFu,
                  (h2 >> 4) & 0x0FFFu,
                  (h1 ^ h2) & 0x0FFFu,
                  h2 ^ (seq * 2654435761u));
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
            if (m_draft.primary_hdd_path.empty()) {
                m_draft.primary_hdd_path = GenerateDefaultHddPath(m_draft.display_name, m_draft.id);
            }
            std::string created_chs;
            if (!CreateBlankHddImage(m_draft.primary_hdd_path, m_hdd_size_mb, created_chs)) {
                out_error = "Failed to create blank HDD image (file may already exist or path is invalid): " + m_draft.primary_hdd_path;
                m_last_error = out_error;
                return false;
            }
            m_draft.primary_hdd_chs = created_chs;
            m_draft.os_installed = false;
            created_new_hdd_file = true;
        } else {
            // Importing an existing HDD image: require existing file & preserve its native geometry
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
            m_draft.primary_hdd_chs.clear();
            m_draft.os_installed = true;
        }
    }

    if (!db.InsertMachine(m_draft)) {
        if (created_new_hdd_file) {
            std::remove(m_draft.primary_hdd_path.c_str());
        }
        out_error = "Failed to save machine record to catalog database.";
        m_last_error = out_error;
        return false;
    }

    // Bind optional installer media atomically (roll back machine if any binding fails)
    bool bindings_ok = true;
    if (!m_installer_floppy_media_id.empty()) {
        CaddyBindingRecord b;
        b.machine_id = m_draft.id;
        b.media_id = m_installer_floppy_media_id;
        b.target_drive = "A";
        b.slot_order = 1;
        if (!db.AddCaddyBinding(b)) {
            bindings_ok = false;
        }
    }
    if (bindings_ok && !m_installer_cd_media_id.empty()) {
        CaddyBindingRecord b;
        b.machine_id = m_draft.id;
        b.media_id = m_installer_cd_media_id;
        b.target_drive = "D";
        b.slot_order = 1;
        if (!db.AddCaddyBinding(b)) {
            bindings_ok = false;
        }
    }

    if (!bindings_ok) {
        db.DeleteMachine(m_draft.id);
        if (created_new_hdd_file) {
            std::remove(m_draft.primary_hdd_path.c_str());
        }
        out_error = "Failed to attach selected installer media; machine creation was rolled back.";
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
    ImGui::TextUnformatted(is_integrated ? "Optional Host C: Directory Path:" : "Primary Hard Disk Image Path (hdd_c.img):");
    ImGui::SetNextItemWidth((std::min)(480.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::InputTextWithHint("##WizHddPath", "Leave blank for automatic default path...", m_hdd_path_buf, sizeof(m_hdd_path_buf))) {
        m_draft.primary_hdd_path = m_hdd_path_buf;
    }
}

void ViewWizard::RenderStep3OsStorage() {
    const KioskThemePalette& pal = KioskUI::GetInstance().GetThemePalette();
    const std::vector<OSDistributionRecord>& os_list = KioskUI::GetInstance().GetCachedOSDistributions();
    const std::vector<MediaPoolRecord>& pool = KioskUI::GetInstance().GetCachedMedia();

    ImGui::Text("Select Target Operating System (Hardware Tier %d Compatibility Enforced):", m_draft.archetype_tier);
    ImGui::Spacing();

    ImGui::BeginChild("##WizardOsList", ImVec2(-1.0f, 175.0f), true);
    {
        const bool none_selected = m_draft.os_id.empty();
        if (ImGui::RadioButton("None / DOSBox-X Built-in DOS", none_selected)) {
            m_draft.os_id.clear();
        }

        for (size_t i = 0; i < os_list.size(); ++i) {
            const OSDistributionRecord& os = os_list[i];
            std::string reason;
            const bool compat = ArchetypeManager::GetInstance().ValidateMachineCompatibility(m_draft, os, reason);
            const bool selected = (m_draft.os_id == os.id);

            ImGui::BeginDisabled(!compat);
            char label[192];
            std::snprintf(label, sizeof(label), "%s  [%s | %s | Rec: %d MB]",
                          os.display_name.c_str(),
                          os.os_family.c_str(),
                          os.filesystem_type.c_str(),
                          os.recommended_hdd_mb);
            if (ImGui::RadioButton(label, selected)) {
                std::string err;
                SelectOSDistribution(os.id, err);
            }
            ImGui::EndDisabled();

            if (!compat) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.42f, 0.35f, 1.0f));
                ImGui::Text("(Requires Tier %d+)", os.min_archetype_tier);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("%s", reason.c_str());
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();

    if (m_draft.boot_mode == "REAL_HDD_BOOT") {
        if (ImGui::RadioButton("Create New Blank HDD Image", m_create_blank_hdd_file)) {
            SetCreateBlankHddOnDisk(true);
        }
        ImGui::SameLine(0.0f, 16.0f);
        if (ImGui::RadioButton("Import Existing HDD Image", !m_create_blank_hdd_file)) {
            SetCreateBlankHddOnDisk(false);
        }

        if (m_create_blank_hdd_file) {
            ImGui::Text("Hard Disk Capacity (MB):");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderInt("##WizHddSize", &m_hdd_size_mb, 20, 2048, "%d MB")) {
                m_draft.primary_hdd_chs = ComputeHddChsGeometry(m_hdd_size_mb);
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
            ImGui::Text("CHS: %s", ComputeHddChsGeometry(m_hdd_size_mb).c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
            ImGui::TextWrapped("Existing HDD image will keep its native MBR/partition geometry (DOSBox-X auto-detect).");
            ImGui::PopStyleColor();
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
            if (pool[i].id == m_installer_floppy_media_id) cur_floppy = pool[i].label_title;
            if (pool[i].id == m_installer_cd_media_id) cur_cd = pool[i].label_title;
        }
        if (ImGui::BeginCombo("Drive A: Floppy", cur_floppy.c_str())) {
            if (ImGui::Selectable("(None)##flp_none", m_installer_floppy_media_id.empty())) {
                m_installer_floppy_media_id.clear();
            }
            for (size_t i = 0; i < pool.size(); ++i) {
                if (pool[i].media_type.find("FLOPPY") != std::string::npos) {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(pool[i].label_title.c_str(), m_installer_floppy_media_id == pool[i].id)) {
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
                    if (ImGui::Selectable(pool[i].label_title.c_str(), m_installer_cd_media_id == pool[i].id)) {
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
        ImGui::Text("HDD Image:  %s (CHS: %s)", m_draft.primary_hdd_path.c_str(), m_draft.primary_hdd_chs.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Synthesized DOSBox-X Config Preview")) {
        const std::string preview = ConfigSynthesizer::GetInstance().SynthesizeConfig(m_draft);
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
