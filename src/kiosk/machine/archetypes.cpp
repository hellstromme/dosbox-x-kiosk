#include "config.h"

#if defined(C_KIOSK)

#include "archetypes.h"
#include <sstream>
#include <algorithm>

ArchetypeManager& ArchetypeManager::GetInstance() {
    static ArchetypeManager instance;
    return instance;
}

ArchetypeManager::ArchetypeManager() {
    InitProfiles();
}

void ArchetypeManager::InitProfiles() {
    m_profiles.clear();

    // 1. ARCH_XT_8088
    {
        ArchetypeProfile p;
        p.id = KioskArchetypes::XT_8088;
        p.display_name = "IBM PC/XT (8088, CGA, Tandy Sound)";
        p.era_description = "1981-1986 early DOS and PC booter era (4.77 MHz, 640KB)";
        p.tier = 1;
        p.cpu_type = "8088";
        p.cpu_core = "normal";
        p.cpu_cycles = "fixed 315";
        p.apm_bios = false;
        p.apm_version = "";
        p.isa_pnp = false;
        p.machine_type = "cga";
        p.mem_size_mb = 1;
        p.vmem_size_kb = 0;
        p.sound_card = "none";
        p.sound_base = "";
        p.sound_irq = 0;
        p.sound_dma = 0;
        p.sound_hdma = 0;
        p.opl_mode = "";
        p.opl_emu = "";
        p.gus_enabled = false;
        p.voodoo_enabled = false;
        p.mpu401_mode = "none";
        p.midi_device = "none";
        p.soundfont_path = "";
        p.dos_version = "3.30";
        p.xms = false;
        p.ems = false;
        p.umb = false;
        p.dual_ide = false;
        m_profiles.push_back(p);
    }

    // 2. ARCH_AT_286
    {
        ArchetypeProfile p;
        p.id = KioskArchetypes::AT_286;
        p.display_name = "IBM PC/AT (286, EGA, AdLib OPL2)";
        p.era_description = "1986-1989 mid DOS era, early Sierra & LucasArts (12 MHz, 2MB)";
        p.tier = 2;
        p.cpu_type = "286";
        p.cpu_core = "normal";
        p.cpu_cycles = "fixed 1500";
        p.apm_bios = true;
        p.apm_version = "1.0";
        p.isa_pnp = false;
        p.machine_type = "ega";
        p.mem_size_mb = 2;
        p.vmem_size_kb = 256;
        p.sound_card = "gb"; // Game Blaster / AdLib
        p.sound_base = "220";
        p.sound_irq = 7;
        p.sound_dma = 1;
        p.sound_hdma = 0;
        p.opl_mode = "opl2";
        p.opl_emu = "nuked";
        p.gus_enabled = false;
        p.voodoo_enabled = false;
        p.mpu401_mode = "none";
        p.midi_device = "none";
        p.soundfont_path = "";
        p.dos_version = "5.00";
        p.xms = true;
        p.ems = false;
        p.umb = false;
        p.dual_ide = false;
        m_profiles.push_back(p);
    }

    // 3. ARCH_386DX
    {
        ArchetypeProfile p;
        p.id = KioskArchetypes::DX_386;
        p.display_name = "Intel 386DX-33 (ET4000 SVGA, SB Pro 2, Roland MT-32)";
        p.era_description = "1990-1992 golden DOS / Win 3.1 era (33 MHz, 8MB)";
        p.tier = 3;
        p.cpu_type = "386";
        p.cpu_core = "dynamic";
        p.cpu_cycles = "fixed 8000";
        p.apm_bios = true;
        p.apm_version = "1.1";
        p.isa_pnp = false;
        p.machine_type = "svga_et4000";
        p.mem_size_mb = 8;
        p.vmem_size_kb = 1024;
        p.sound_card = "sbpro2";
        p.sound_base = "220";
        p.sound_irq = 7;
        p.sound_dma = 1;
        p.sound_hdma = 0;
        p.opl_mode = "dualopl2";
        p.opl_emu = "nuked";
        p.gus_enabled = false;
        p.voodoo_enabled = false;
        p.mpu401_mode = "intelligent";
        p.midi_device = "fluidsynth";
        p.soundfont_path = "/var/lib/dosbox-kiosk/soundfonts/SC-55.sf2";
        p.dos_version = "6.22";
        p.xms = true;
        p.ems = true;
        p.umb = true;
        p.dual_ide = true;
        m_profiles.push_back(p);
    }

    // 4. ARCH_486DX2
    {
        ArchetypeProfile p;
        p.id = KioskArchetypes::DX2_486;
        p.display_name = "Intel 486DX2-66 VLB (S3 Trio64, SB16 + GUS, CD-ROM)";
        p.era_description = "1993-1995 Doom & DOS/4GW protected mode peak (66 MHz, 16MB)";
        p.tier = 4;
        p.cpu_type = "486";
        p.cpu_core = "dynamic";
        p.cpu_cycles = "fixed 26800";
        p.apm_bios = true;
        p.apm_version = "1.1";
        p.isa_pnp = true;
        p.machine_type = "svga_s3";
        p.mem_size_mb = 16;
        p.vmem_size_kb = 2048;
        p.sound_card = "sb16";
        p.sound_base = "220";
        p.sound_irq = 7;
        p.sound_dma = 1;
        p.sound_hdma = 5;
        p.opl_mode = "opl3";
        p.opl_emu = "nuked";
        p.gus_enabled = true;
        p.voodoo_enabled = false;
        p.mpu401_mode = "intelligent";
        p.midi_device = "fluidsynth";
        p.soundfont_path = "/var/lib/dosbox-kiosk/soundfonts/SC-55.sf2";
        p.dos_version = "6.22";
        p.xms = true;
        p.ems = true;
        p.umb = true;
        p.dual_ide = true;
        m_profiles.push_back(p);
    }

    // 5. ARCH_PENTIUM_3DFX
    {
        ArchetypeProfile p;
        p.id = KioskArchetypes::PENTIUM_3DFX;
        p.display_name = "Pentium MMX + 3dfx Voodoo (Win95/Win98, Glide, SB16 Vibra)";
        p.era_description = "1996-1999 3D accelerated late DOS & Windows 9x era (64MB)";
        p.tier = 5;
        p.cpu_type = "pentium_mmx";
        p.cpu_core = "dynamic";
        p.cpu_cycles = "max 100% limit 75000";
        p.apm_bios = true;
        p.apm_version = "1.2";
        p.isa_pnp = true;
        p.machine_type = "svga_s3";
        p.mem_size_mb = 64;
        p.vmem_size_kb = 4096;
        p.sound_card = "sb16vibra";
        p.sound_base = "220";
        p.sound_irq = 5;
        p.sound_dma = 1;
        p.sound_hdma = 5;
        p.opl_mode = "opl3";
        p.opl_emu = "fast";
        p.gus_enabled = false;
        p.voodoo_enabled = true;
        p.mpu401_mode = "uart";
        p.midi_device = "fluidsynth";
        p.soundfont_path = "/var/lib/dosbox-kiosk/soundfonts/GM_GS.sf2";
        p.dos_version = "7.10";
        p.xms = true;
        p.ems = true;
        p.umb = true;
        p.dual_ide = true;
        m_profiles.push_back(p);
    }
}

const ArchetypeProfile* ArchetypeManager::GetArchetype(const std::string& archetype_id) const {
    for (const auto& profile : m_profiles) {
        if (profile.id == archetype_id) {
            return &profile;
        }
    }
    return nullptr;
}

std::vector<ArchetypeProfile> ArchetypeManager::GetAllArchetypes() const {
    return m_profiles;
}

bool ArchetypeManager::ValidateMachineCompatibility(const MachineRecord& machine, const OSDistributionRecord& os, std::string& out_reason) const {
    const ArchetypeProfile* arch = GetArchetype(machine.archetype_id);
    if (!arch) {
        out_reason = "Unknown hardware archetype: " + machine.archetype_id;
        return false;
    }

    if (arch->tier < os.min_archetype_tier) {
        std::ostringstream ss;
        ss << "Operating system '" << os.display_name << "' requires Tier "
           << os.min_archetype_tier << "+ hardware, but machine archetype '"
           << arch->display_name << "' is Tier " << arch->tier << ".";
        out_reason = ss.str();
        return false;
    }

    // Specific architectural constraints
    if (os.os_family == "WIN9X" && arch->tier < 4) {
        out_reason = "Windows 9x requires at least a 486 (Tier 4) with 16MB+ RAM and APM 1.1+ support.";
        return false;
    }

    if (os.filesystem_type == "FAT32" && arch->tier < 4) {
        out_reason = "FAT32 filesystems require at least Tier 4 hardware.";
        return false;
    }

    return true;
}

std::string ArchetypeManager::GenerateArchetypeConfig(const std::string& archetype_id) const {
    const ArchetypeProfile* p = GetArchetype(archetype_id);
    if (!p) return "";

    std::ostringstream ss;
    ss << "# --- DOSBox-X Hardware Archetype Profile: " << p->id << " ---\n"
       << "# " << p->display_name << " (" << p->era_description << ")\n\n";

    // [dosbox]
    ss << "[dosbox]\n"
       << "machine             = " << p->machine_type << "\n"
       << "memsize             = " << p->mem_size_mb << "\n";
    if (p->vmem_size_kb > 0) {
        ss << "vmemsize            = " << p->vmem_size_kb << "\n";
    }
    if (p->tier >= 4) {
        ss << "vbe                 = true\n";
    }
    ss << "\n";

    // [cpu]
    ss << "[cpu]\n"
       << "core                = " << p->cpu_core << "\n"
       << "cputype             = " << p->cpu_type << "\n"
       << "cycles              = " << p->cpu_cycles << "\n";
    if (p->id == KioskArchetypes::XT_8088) {
        ss << "cycleup             = 50\n"
           << "cycledown           = 50\n";
    }
    ss << "apmbios             = " << (p->apm_bios ? "true" : "false") << "\n";
    if (p->apm_bios && !p->apm_version.empty()) {
        ss << "apmversion          = " << p->apm_version << "\n";
    }
    ss << "isapnpbios          = " << (p->isa_pnp ? "true" : "false") << "\n";
    if (p->id == KioskArchetypes::PENTIUM_3DFX) {
        ss << "integration device  = true\n";
    }
    ss << "\n";

    // [video]
    if (p->id == KioskArchetypes::XT_8088) {
        ss << "[video]\n"
           << "vga                 = false\n"
           << "cga_composite       = auto\n\n";
    }

    // [voodoo]
    if (p->voodoo_enabled) {
        ss << "[voodoo]\n"
           << "voodoo_card         = auto\n"
           << "voodoo_maxmem       = true\n"
           << "glide               = false\n"
           << "lfb                 = full\n\n";
    }

    // [sblaster]
    ss << "[sblaster]\n"
       << "sbtype              = " << p->sound_card << "\n";
    if (p->sound_card != "none") {
        ss << "sbbase              = " << p->sound_base << "\n"
           << "irq                 = " << p->sound_irq << "\n"
           << "dma                 = " << p->sound_dma << "\n";
        if (p->sound_hdma > 0) {
            ss << "hdma                = " << p->sound_hdma << "\n";
        }
        if (p->id == KioskArchetypes::DX2_486) {
            ss << "sbmixer             = true\n";
        } else if (p->id == KioskArchetypes::PENTIUM_3DFX) {
            ss << "enable speaker      = true\n";
        }
        if (!p->opl_mode.empty()) {
            ss << "oplmode             = " << p->opl_mode << "\n";
        }
        if (!p->opl_emu.empty()) {
            ss << "oplemu              = " << p->opl_emu << "\n";
        }
        if (p->id != KioskArchetypes::PENTIUM_3DFX) {
            ss << "oplrate             = 48000\n";
        }
    }
    ss << "\n";

    // [gus]
    ss << "[gus]\n"
       << "gus                 = " << (p->gus_enabled ? "true" : "false") << "\n";
    if (p->gus_enabled) {
        ss << "gusrate             = 48000\n"
           << "gusmemsize          = 1024\n"
           << "gusbase             = 240\n"
           << "gusirq              = 5\n"
           << "gusdma              = 3\n"
           << "ultradir            = C:\\ULTRASND\n";
    }
    ss << "\n";

    // [speaker]
    ss << "[speaker]\n"
       << "pcspeaker           = true\n";
    if (p->id == KioskArchetypes::XT_8088) {
        ss << "pcrate              = 48000\n"
           << "tandy               = auto\n"
           << "tandyrate           = 48000\n"
           << "disney              = false\n";
    } else if (p->id == KioskArchetypes::AT_286) {
        ss << "tandy               = off\n"
           << "disney              = true\n";
    } else {
        ss << "tandy               = off\n"
           << "disney              = false\n";
    }
    ss << "\n";

    // [midi]
    ss << "[midi]\n"
       << "mpu401              = " << p->mpu401_mode << "\n"
       << "mididevice          = " << p->midi_device << "\n";
    if (p->midi_device == "fluidsynth" && !p->soundfont_path.empty()) {
        ss << "fluid.soundfont     = " << p->soundfont_path << "\n";
    }
    ss << "\n";

    // [dos]
    ss << "[dos]\n"
       << "xms                 = " << (p->xms ? "true" : "false") << "\n"
       << "ems                 = " << (p->ems ? "true" : "false") << "\n";
    if (p->id == KioskArchetypes::DX_386) {
        ss << "ems system handle   = true\n";
    }
    ss << "umb                 = " << (p->umb ? "true" : "false") << "\n"
       << "ver                 = " << p->dos_version << "\n";
    if (p->id == KioskArchetypes::XT_8088) {
        ss << "minimum mcb free    = 0\n";
    } else if (p->id == KioskArchetypes::DX2_486) {
        ss << "automount           = false\n";
    } else if (p->id == KioskArchetypes::PENTIUM_3DFX) {
        ss << "fat32setversion     = auto\n";
    }
    ss << "\n";

    // [fdc, primary]
    ss << "[fdc, primary]\n"
       << "enable              = true\n";
    if (p->id == KioskArchetypes::XT_8088) {
        ss << "int13fakev86io      = false\n";
    } else if (p->tier >= 4) {
        ss << "int13fakev86io      = true\n";
    }
    ss << "\n";

    // [ide, primary]
    ss << "[ide, primary]\n"
       << "enable              = true\n";
    if (p->id == KioskArchetypes::XT_8088) {
        ss << "int13fakeio         = false\n";
    } else {
        ss << "int13fakeio         = true\n";
    }
    if (p->tier >= 4) {
        ss << "int13fakev86io      = true\n";
    }
    if (p->id == KioskArchetypes::PENTIUM_3DFX) {
        ss << "cdrom_insertion_delay = 1000\n";
    }
    ss << "\n";

    // [ide, secondary]
    if (p->dual_ide) {
        ss << "[ide, secondary]\n"
           << "enable              = true\n"
           << "int13fakeio         = true\n";
        if (p->tier >= 4) {
            ss << "int13fakev86io      = true\n";
        }
        ss << "\n";
    }

    if (p->id == KioskArchetypes::PENTIUM_3DFX) {
        ss << "[ne2000]\n"
           << "ne2000              = false\n\n";
    }

    return ss.str();
}

#endif // defined(C_KIOSK)
