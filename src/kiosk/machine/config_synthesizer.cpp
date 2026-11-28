#include "config.h"

#if defined(C_KIOSK)

#include "config_synthesizer.h"
#include "archetypes.h"
#include "../kiosk_db.h"
#include "../kiosk_host.h"
#include "../kiosk_main.h"
#include "../ingest/usb_bridge.h"
#include "logging.h"
#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>
#include <cctype>

ConfigSynthesizer& ConfigSynthesizer::GetInstance() {
    static ConfigSynthesizer instance;
    return instance;
}

bool ConfigSynthesizer::IsSafeAutoexecMountPath(const std::string& path) {
    if (path.empty()) return false;
    for (size_t i = 0; i < path.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(path[i]);
        // Reject control chars (including \r, \n, 0x1A DOS EOF), quotes, and '%' (expanded by BatchFile::ReadLine)
        if (ch < 0x20u || ch == '"' || ch == '%') {
            return false;
        }
    }
    return true;
}

bool ConfigSynthesizer::IsSafeGuestDosPath(const std::string& path) {
    for (size_t i = 0; i < path.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(path[i]);
        if (ch >= 0x80u || std::isalnum(ch)) continue; // DOS codepage characters are plain filename bytes
        switch (ch) {
            case '\\': case '.': case '_': case '-': case '~': case '!': case '#': case '$':
            case '\'': case '(': case ')': case '@': case '^': case '{': case '}': case ' ':
                continue;
            default:
                return false; // rejects ':', '"', '%', '|', '<', '>', '&', '/', control chars
        }
    }
    return true;
}

bool ConfigSynthesizer::IsSafeGuestExecutable(const std::string& exe) {
    if (exe.empty() || exe[0] == '\\' || exe[exe.size() - 1] == '\\' || !IsSafeGuestDosPath(exe)) return false;
    // Bare names ("TIE") are legal: the shell resolves .COM/.EXE/.BAT in the given directory.
    // A space is only meaningful inside a real filename, so require an extension then.
    std::string ext = exe.size() >= 4 ? exe.substr(exe.size() - 4) : std::string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const bool has_known_ext = ext == ".EXE" || ext == ".COM" || ext == ".BAT";
    return has_known_ext || exe.find(' ') == std::string::npos;
}

bool ConfigSynthesizer::IsLaunchableGuestDrive(char drive_letter) {
    // Only drives the kiosk [autoexec] itself mounts. Never Z: (DOSBox-X built-in programs).
    const char d = static_cast<char>(std::toupper(static_cast<unsigned char>(drive_letter)));
    return d == 'A' || d == 'B' || d == 'C' || d == 'D' || d == 'E';
}

bool ConfigSynthesizer::IsSafeGuestArguments(const std::string& args) {
    bool in_quotes = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(args[i]);
        if (ch < 0x20u || ch == '%' || ch == '|' || ch == '<' || ch == '>' || ch == '&') {
            return false;
        }
        if (ch == '"') in_quotes = !in_quotes;
    }
    return !in_quotes;
}

static bool IsDecimalToken(const std::string& s) {
    if (s.empty() || s.size() > 6) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

// Emits a quoted host path, or a REM marker when the path cannot be emitted safely.
static bool EmitQuotedHostPath(std::ostringstream& ss, const std::string& path) {
    if (!ConfigSynthesizer::IsSafeAutoexecMountPath(path)) {
        LOG_MSG("KIOSK: Rejected unsafe host path for [autoexec] mount (length %u)", static_cast<unsigned>(path.size()));
        return false;
    }
    ss << " \"" << path << "\"";
    return true;
}

std::string ConfigSynthesizer::GetDefaultBaseConfig() const {
    const std::string mapper_path = KioskHost::GetDefaultMapperPath();
    return
        "# --- DOSBox-X Kiosk Appliance Base Configuration ---\n"
        "[sdl]\n"
        "fullscreen          = true\n"
        "fulldouble          = true\n"
        "fullresolution      = desktop\n"
        "output              = openglpp\n"
        "autolock            = true\n"
        "sensitivity         = 100\n"
        "waitonerror         = false\n"
        "mapperfile          = " + mapper_path + "\n"
        "\n"
        "[dosbox]\n"
        "title               = DOSBox-X Kiosk Appliance\n"
        "fastbioslogo        = true\n"
        "startbanner         = false\n"
        "quit warning        = false\n"
        "working directory option = default\n"
        "\n"
        "[render]\n"
        "frameskip           = 0\n"
        "aspect              = true\n"
        "char9               = true\n"
        "scaler              = none\n"
        "glshader            = crt-aperture.glsl\n"
        "\n"
        "[dos]\n"
        "automount           = false\n"
        "automountall        = false\n"
        "mountwarning        = false\n\n";
}

std::string ConfigSynthesizer::SynthesizeAutoexec(
    const MachineRecord& machine,
    const CatalogItemRecord* item,
    const std::vector<CaddyBindingRecord>& caddy,
    const std::string& dispatch_image_path,
    bool boot_installer_media
) const {
    std::ostringstream ss;
    ss << "[autoexec]\n"
       << "@echo off\n";

    // Internally generated, but validated like every other host path before emission
    std::string safe_dispatch_path = dispatch_image_path;
    if (!safe_dispatch_path.empty() && !IsSafeAutoexecMountPath(safe_dispatch_path)) {
        LOG_MSG("KIOSK: Rejected unsafe dispatch image path");
        safe_dispatch_path.clear();
    }

    // Group caddy bindings by target drive ('A', 'B', 'D', 'E'), verifying USB path containment
    std::map<std::string, std::vector<CaddyBindingRecord>> drive_bindings;
    for (const auto& b : caddy) {
        if (!IsSafeAutoexecMountPath(b.resolved_file_path)) {
            LOG_MSG("KIOSK: Rejected unsafe caddy media path for drive %s", b.target_drive.c_str());
            continue; // Quotes, '%' or control chars would break out of the quoted IMGMOUNT argument
        }
        if (!b.media_id.empty() && KioskDatabase::GetInstance().IsOpen()) {
            MediaPoolRecord mrec;
            if (KioskDatabase::GetInstance().GetMedia(b.media_id, mrec) && mrec.is_removable_usb) {
                if (!KioskUsbBridge::GetInstance().IsPathContainedInActiveRemovableMount(b.resolved_file_path)) {
                    continue; // Reject escaped USB media path before emitting IMGMOUNT
                }
            }
        }
        drive_bindings[b.target_drive].push_back(b);
    }
    for (auto& pair : drive_bindings) {
        std::sort(pair.second.begin(), pair.second.end(), [](const CaddyBindingRecord& a, const CaddyBindingRecord& b) {
            return a.slot_order < b.slot_order;
        });
    }

    if (machine.boot_mode == "REAL_HDD_BOOT") {
        // 1. Mount primary HDD
        if (!machine.primary_hdd_path.empty() && IsSafeAutoexecMountPath(machine.primary_hdd_path)) {
            ss << "# Mount primary boot hard drive on Primary IDE Master\n"
               << "IMGMOUNT 2";
            EmitQuotedHostPath(ss, machine.primary_hdd_path);
            ss << " -t hdd -fs none";
            if (!machine.primary_hdd_chs.empty()) {
                // Parse primary_hdd_chs (C,H,S format) and convert to -size 512,S,H,C
                std::vector<std::string> tokens;
                std::stringstream chs_ss(machine.primary_hdd_chs);
                std::string token;
                while (std::getline(chs_ss, token, ',')) {
                    token.erase(0, token.find_first_not_of(" \t\r\n"));
                    token.erase(token.find_last_not_of(" \t\r\n") + 1);
                    if (!token.empty()) tokens.push_back(token);
                }
                if (tokens.size() == 3 && IsDecimalToken(tokens[0]) &&
                    IsDecimalToken(tokens[1]) && IsDecimalToken(tokens[2])) {
                    // tokens: C, H, S -> IMGMOUNT -size 512,S,H,C
                    ss << " -size 512," << tokens[2] << "," << tokens[1] << "," << tokens[0];
                } else {
                    LOG_MSG("KIOSK: Ignoring malformed primary_hdd_chs geometry override");
                }
            }
            ss << " -ide 1m\n";
        } else if (!machine.primary_hdd_path.empty()) {
            LOG_MSG("KIOSK: Rejected unsafe primary HDD path for machine %s", machine.id.c_str());
        }

        // 2. Mount floppy caddy (Drive A = 0, Drive B = 1)
        // In Mode A (Carousel Item launch on REAL_HDD_BOOT), Drive B: (IMGMOUNT 1) is
        // reserved exclusively for the ephemeral dispatch floppy (B:\KIOSKGO.BAT + B:\KIOSKEXT.COM)
        // so the boot hook and post-exit return script are never shadowed or ejected.
        // Any B: caddy bindings are coalesced into Drive A:'s multi-disk swap queue.
        const bool reserve_b_for_dispatch = (item != nullptr && !safe_dispatch_path.empty());
        std::vector<CaddyBindingRecord> floppy_a_queue = drive_bindings["A"];
        if (reserve_b_for_dispatch && !drive_bindings["B"].empty()) {
            const auto& b_list = drive_bindings["B"];
            floppy_a_queue.insert(floppy_a_queue.end(), b_list.begin(), b_list.end());
        }

        if (!floppy_a_queue.empty()) {
            ss << "# Floppy Drive A: Session Caddy\n"
               << "IMGMOUNT 0";
            for (const auto& b : floppy_a_queue) {
                ss << " \"" << b.resolved_file_path << "\"";
            }
            ss << " -t floppy -fs none\n";
        }

        if (reserve_b_for_dispatch) {
            ss << "# Ephemeral Kiosk Dispatch Floppy (Drive B:)\n"
               << "IMGMOUNT 1 \"" << safe_dispatch_path << "\" -t floppy -fs none\n";
        } else if (drive_bindings.find("B") != drive_bindings.end() && !drive_bindings["B"].empty()) {
            ss << "# Floppy Drive B: Session Caddy\n"
               << "IMGMOUNT 1";
            for (const auto& b : drive_bindings["B"]) {
                ss << " \"" << b.resolved_file_path << "\"";
            }
            ss << " -t floppy -fs none\n";
        } else if (!safe_dispatch_path.empty()) {
            ss << "# Ephemeral Kiosk Dispatch Floppy (Drive B:)\n"
               << "IMGMOUNT 1 \"" << safe_dispatch_path << "\" -t floppy -fs none\n";
        }

        // 3. Mount optical caddy (Drive D on Secondary IDE Master, Drive E on Secondary IDE Slave)
        if (drive_bindings.find("D") != drive_bindings.end() && !drive_bindings["D"].empty()) {
            ss << "# Optical Drive D: on Secondary IDE Master\n"
               << "IMGMOUNT D";
            for (const auto& b : drive_bindings["D"]) {
                ss << " \"" << b.resolved_file_path << "\"";
            }
            ss << " -t iso -fs iso -ide 2m\n";
        }

        if (drive_bindings.find("E") != drive_bindings.end() && !drive_bindings["E"].empty()) {
            ss << "# Optical Drive E: on Secondary IDE Slave\n"
               << "IMGMOUNT E";
            for (const auto& b : drive_bindings["E"]) {
                ss << " \"" << b.resolved_file_path << "\"";
            }
            ss << " -t iso -fs iso -ide 2s\n";
        }

        // 4. Boot from installer media (pending OS install) or primary C: MBR
        if (boot_installer_media && item == nullptr) {
            if (!floppy_a_queue.empty()) {
                ss << "# Boot OS installer from Floppy Drive A:\n"
                   << "BOOT -l a\n";
            } else if (drive_bindings.find("D") != drive_bindings.end() && !drive_bindings["D"].empty()) {
                // El Torito CD-ROM boot uses BOOT -el-torito D: (BOOT -l d boots 2nd HDD)
                ss << "# Boot OS installer from El Torito Optical Drive D:\n"
                   << "BOOT -el-torito D:\n";
            } else {
                ss << "# Boot from primary hard drive\n"
                   << "BOOT -l c\n";
            }
        } else {
            ss << "# Boot from primary hard drive\n"
               << "BOOT -l c\n";
        }
    } else {
        // INTEGRATED_DOS mode
        // 1. Mount container or default C: drive
        if (item != nullptr && !item->host_container_dir.empty()) {
            if (IsSafeAutoexecMountPath(item->host_container_dir)) {
                ss << "# Mount container directory as C:\n"
                   << "MOUNT C";
                EmitQuotedHostPath(ss, item->host_container_dir);
                ss << "\n";
            } else {
                LOG_MSG("KIOSK: Rejected unsafe container directory for item %s", item->id.c_str());
            }
        } else if (!machine.primary_hdd_path.empty() && IsSafeAutoexecMountPath(machine.primary_hdd_path)) {
            ss << "# Mount machine directory as C:\n"
               << "MOUNT C";
            EmitQuotedHostPath(ss, machine.primary_hdd_path);
            ss << "\n";
        } else if (!machine.primary_hdd_path.empty()) {
            LOG_MSG("KIOSK: Rejected unsafe machine directory for machine %s", machine.id.c_str());
        }

        // 2. Mount floppies if queued (Drive A and Drive B with DOS filesystem)
        if (drive_bindings.find("A") != drive_bindings.end() && !drive_bindings["A"].empty()) {
            ss << "# Floppy Drive A: Session Caddy\n"
               << "IMGMOUNT A";
            for (const auto& b : drive_bindings["A"]) {
                ss << " \"" << b.resolved_file_path << "\"";
            }
            ss << " -t floppy\n";
        }

        if (drive_bindings.find("B") != drive_bindings.end() && !drive_bindings["B"].empty()) {
            ss << "# Floppy Drive B: Session Caddy\n"
               << "IMGMOUNT B";
            for (const auto& b : drive_bindings["B"]) {
                ss << " \"" << b.resolved_file_path << "\"";
            }
            ss << " -t floppy\n";
        }

        // 3. Mount CD-ROMs if queued (Drive D and Drive E)
        if (drive_bindings.find("D") != drive_bindings.end() && !drive_bindings["D"].empty()) {
            ss << "# Mount optical media to D:\n"
               << "IMGMOUNT D";
            for (const auto& b : drive_bindings["D"]) {
                ss << " \"" << b.resolved_file_path << "\"";
            }
            ss << " -t iso\n";
        }

        if (drive_bindings.find("E") != drive_bindings.end() && !drive_bindings["E"].empty()) {
            ss << "# Mount auxiliary optical media to E:\n"
               << "IMGMOUNT E";
            for (const auto& b : drive_bindings["E"]) {
                ss << " \"" << b.resolved_file_path << "\"";
            }
            ss << " -t iso\n";
        } else if (KioskManager::GetInstance().GetConfig().auto_mount_usb_docs) {
            const std::string usb_docs = KioskUsbBridge::GetInstance().GetPreferredUsbDocumentsDir();
            if (IsSafeAutoexecMountPath(usb_docs)) {
                ss << "# Auto-mount removable USB DOCUMENTS folder as E:\n"
                   << "MOUNT E \"" << usb_docs << "\"\n";
            }
        }

        // 4. Dispatch executable
        // Working dir, executable and arguments may originate from an imported (untrusted)
        // dosbox.conf, and this [autoexec] runs in the DOSBox-X shell, which can MOUNT host
        // paths. Unsafe launches are dropped, never sanitised into something else.
        if (item != nullptr && !item->guest_executable.empty()) {
            std::string work_dir = item->guest_working_dir;
            char drive_letter = 'C';
            if (work_dir.size() >= 2 &&
                std::isalpha(static_cast<unsigned char>(work_dir[0])) &&
                work_dir[1] == ':') {
                drive_letter = static_cast<char>(std::toupper(static_cast<unsigned char>(work_dir[0])));
                work_dir = work_dir.substr(2);
            }
            work_dir.erase(0, work_dir.find_first_not_of('\\'));
            while (!work_dir.empty() && work_dir[work_dir.size() - 1] == '\\') {
                work_dir.erase(work_dir.size() - 1);
            }

            if (!IsLaunchableGuestDrive(drive_letter) ||
                !IsSafeGuestDosPath(work_dir) ||
                !IsSafeGuestExecutable(item->guest_executable) ||
                !IsSafeGuestArguments(item->guest_arguments)) {
                LOG_MSG("KIOSK: Rejected unsafe launch command for item %s", item->id.c_str());
                // Return to the kiosk shell instead of leaving an interactive DOSBox-X prompt
                ss << "ECHO Kiosk: launch command rejected (unsafe characters).\n"
                   << "EXIT\n";
            } else {
                ss << drive_letter << ":\n";
                if (!work_dir.empty()) {
                    ss << "CD \\" << work_dir << "\n";
                }
                // Absolute guest path: a bare name such as MOUNT.COM must never fall
                // through the PATH search to the DOSBox-X Z:\ built-in programs.
                std::string exe_path(1, drive_letter);
                exe_path += ":\\";
                if (!work_dir.empty()) exe_path += work_dir + "\\";
                exe_path += item->guest_executable;
                if (exe_path.find(' ') != std::string::npos) {
                    ss << "\"" << exe_path << "\"";
                } else {
                    ss << exe_path;
                }
                if (!item->guest_arguments.empty()) {
                    ss << " " << item->guest_arguments;
                }
                ss << "\n";
                if (item->return_on_exit) {
                    ss << "EXIT\n";
                }
            }
        }
    }

    return ss.str();
}

std::string ConfigSynthesizer::SynthesizeConfig(
    const MachineRecord& machine,
    const CatalogItemRecord* item,
    const std::vector<CaddyBindingRecord>& caddy,
    const std::string& base_conf_path,
    const std::string& dispatch_image_path,
    bool boot_installer_media
) const {
    std::ostringstream ss;

    // 1. Layer 1: Base Kiosk Configuration
    std::string base_conf;
    if (!base_conf_path.empty()) {
        std::ifstream f(base_conf_path.c_str());
        if (f.is_open()) {
            std::stringstream buf;
            buf << f.rdbuf();
            base_conf = buf.str();
        }
    }
    if (base_conf.empty()) {
        base_conf = GetDefaultBaseConfig();
    }
    ss << base_conf << "\n";

    // 2. Layer 2: Archetype Delta
    std::string arch_conf = ArchetypeManager::GetInstance().GenerateArchetypeConfig(machine.archetype_id);
    if (!arch_conf.empty()) {
        ss << arch_conf << "\n";
    }

    // 3. Layer 3: Machine Custom Overrides
    if (!machine.custom_conf_overrides.empty()) {
        ss << "# --- Machine Custom Overrides ---\n"
           << machine.custom_conf_overrides << "\n\n";
    }

    // 4. Layer 4: Dynamic [autoexec]
    ss << SynthesizeAutoexec(machine, item, caddy, dispatch_image_path, boot_installer_media) << "\n";

    return ss.str();
}

bool ConfigSynthesizer::WriteConfigFile(const std::string& config_content, const std::string& target_path) const {
    std::ofstream out(target_path.c_str(), std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out << config_content;
    if (!out.good()) {
        out.close();
        return false;
    }
    out.close();
    return !out.fail();
}

#endif // defined(C_KIOSK)
