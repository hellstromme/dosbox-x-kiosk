#include "config.h"

#if defined(C_KIOSK)

#include "config_synthesizer.h"
#include "archetypes.h"
#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>

ConfigSynthesizer& ConfigSynthesizer::GetInstance() {
    static ConfigSynthesizer instance;
    return instance;
}

std::string ConfigSynthesizer::GetDefaultBaseConfig() const {
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
        "mapperfile          = /var/lib/dosbox-kiosk/config/kiosk_mapper.map\n"
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
        "glshader            = crt-aperture.glsl\n\n";
}

std::string ConfigSynthesizer::SynthesizeAutoexec(
    const MachineRecord& machine,
    const CatalogItemRecord* item,
    const std::vector<CaddyBindingRecord>& caddy
) const {
    std::ostringstream ss;
    ss << "[autoexec]\n"
       << "@echo off\n";

    // Group caddy bindings by target drive ('A', 'B', 'D', 'E')
    std::map<std::string, std::vector<CaddyBindingRecord>> drive_bindings;
    for (const auto& b : caddy) {
        drive_bindings[b.target_drive].push_back(b);
    }
    for (auto& pair : drive_bindings) {
        std::sort(pair.second.begin(), pair.second.end(), [](const CaddyBindingRecord& a, const CaddyBindingRecord& b) {
            return a.slot_order < b.slot_order;
        });
    }

    if (machine.boot_mode == "REAL_HDD_BOOT") {
        // 1. Mount primary HDD
        if (!machine.primary_hdd_path.empty()) {
            ss << "# Mount primary boot hard drive on Primary IDE Master\n"
               << "IMGMOUNT 2 \"" << machine.primary_hdd_path << "\" -t hdd -fs none";
            if (!machine.primary_hdd_chs.empty()) {
                ss << " -size " << machine.primary_hdd_chs;
            }
            ss << " -ide 1m\n";
        }

        // 2. Mount floppy caddy (Drive A = 0, Drive B = 1)
        if (drive_bindings.find("A") != drive_bindings.end() && !drive_bindings["A"].empty()) {
            ss << "# Floppy Drive A: Session Caddy\n"
               << "IMGMOUNT 0";
            for (const auto& b : drive_bindings["A"]) {
                ss << " \"" << b.resolved_file_path << "\"";
            }
            ss << " -t fd -fs none\n";
        }

        if (drive_bindings.find("B") != drive_bindings.end() && !drive_bindings["B"].empty()) {
            ss << "# Floppy Drive B: Session Caddy\n"
               << "IMGMOUNT 1";
            for (const auto& b : drive_bindings["B"]) {
                ss << " \"" << b.resolved_file_path << "\"";
            }
            ss << " -t fd -fs none\n";
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

        // 4. Boot from C: MBR
        ss << "# Boot from primary hard drive\n"
           << "BOOT -l c\n";
    } else {
        // INTEGRATED_DOS mode
        // 1. Mount container or default C: drive
        if (item != nullptr && !item->host_container_dir.empty()) {
            ss << "# Mount container directory as C:\n"
               << "MOUNT C \"" << item->host_container_dir << "\"\n";
        } else if (!machine.primary_hdd_path.empty()) {
            ss << "# Mount machine directory as C:\n"
               << "MOUNT C \"" << machine.primary_hdd_path << "\"\n";
        }

        // 2. Mount floppies if queued (Drive A = 0, Drive B = 1)
        if (drive_bindings.find("A") != drive_bindings.end() && !drive_bindings["A"].empty()) {
            ss << "# Floppy Drive A: Session Caddy\n"
               << "IMGMOUNT 0";
            for (const auto& b : drive_bindings["A"]) {
                ss << " \"" << b.resolved_file_path << "\"";
            }
            ss << " -t fd -fs none\n";
        }

        if (drive_bindings.find("B") != drive_bindings.end() && !drive_bindings["B"].empty()) {
            ss << "# Floppy Drive B: Session Caddy\n"
               << "IMGMOUNT 1";
            for (const auto& b : drive_bindings["B"]) {
                ss << " \"" << b.resolved_file_path << "\"";
            }
            ss << " -t fd -fs none\n";
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
        }

        // 4. Dispatch executable
        if (item != nullptr && !item->guest_executable.empty()) {
            ss << "C:\n";
            if (!item->guest_working_dir.empty()) {
                ss << "CD " << item->guest_working_dir << "\n";
            }
            bool needs_quotes = item->guest_executable.find(' ') != std::string::npos;
            if (needs_quotes) {
                ss << "\"" << item->guest_executable << "\"";
            } else {
                ss << item->guest_executable;
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

    return ss.str();
}

std::string ConfigSynthesizer::SynthesizeConfig(
    const MachineRecord& machine,
    const CatalogItemRecord* item,
    const std::vector<CaddyBindingRecord>& caddy,
    const std::string& base_conf_path
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
    ss << SynthesizeAutoexec(machine, item, caddy) << "\n";

    return ss.str();
}

bool ConfigSynthesizer::WriteConfigFile(const std::string& config_content, const std::string& target_path) const {
    std::ofstream out(target_path.c_str(), std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out << config_content;
    out.close();
    return true;
}

#endif // defined(C_KIOSK)
