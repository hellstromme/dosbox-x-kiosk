#ifndef DOSBOX_KIOSK_BOOT_INJECTOR_H
#define DOSBOX_KIOSK_BOOT_INJECTOR_H

#include "kiosk_types.h"
#include <string>
#include <vector>
#include <cstdint>

#if defined(C_KIOSK)

struct DispatchFileEntry {
    std::string filename_8_3; // e.g., "KIOSKGO.BAT", "KIOSKEXT.COM"
    std::vector<uint8_t> data;
};

class BootInjector {
public:
    static BootInjector& GetInstance();

    // Returns canonical one-time hook marker and command lines for C:\AUTOEXEC.BAT
    static const char* GetAutoexecHookMarker();
    static std::string GetAutoexecHookBlock();

    // Idempotently appends the one-time B:\KIOSKGO.BAT hook to an AUTOEXEC.BAT string
    static std::string InjectAutoexecHookToString(const std::string& existing_content);
    static bool HasAutoexecHook(const std::string& content);

    // One-time guest boot hooker for a raw FAT12/FAT16/FAT32 HDD image file or host file
    bool EnsureOneTimeAutoexecHook(const std::string& hdd_or_autoexec_path) const;

    // Synthesizes B:\KIOSKGO.BAT content for DOS, Win3.1x, or Win9x targets
    // (using C:\WINDOWS\WIN.COM <guest_executable> [guest_arguments] for Windows targets with zero SYSTEM.INI mutation)
    std::string SynthesizeKioskGoBat(
        const MachineRecord& machine,
        const CatalogItemRecord& item
    ) const;

    // Builds a complete 1.44MB (1,474,560-byte) FAT12 floppy image in memory
    // containing the provided root directory files (up to 223 entries).
    bool BuildFat12ImageInMemory(
        const std::vector<DispatchFileEntry>& files,
        std::vector<uint8_t>& out_image_bytes,
        const std::string& volume_label = "KIOSKDISP  "
    ) const;

    // Reads a file back from an in-memory FAT12 image (used for verification & hooks)
    bool ReadFileFromFat12Image(
        const std::vector<uint8_t>& image_bytes,
        const std::string& filename_8_3,
        std::vector<uint8_t>& out_file_data
    ) const;

    // Generates the active dispatch floppy image on disk for a Carousel item launch (Mode A)
    bool BuildDispatchVolume(
        const std::string& output_image_path,
        const MachineRecord& machine,
        const CatalogItemRecord& item
    ) const;

    // Generates an idle dispatch floppy image (KIOSKEXT.COM only, no KIOSKGO.BAT)
    // for Machine-Only boots (Mode B) so BIOS reports 2 floppy drives and skips B: phantom prompts
    bool BuildIdleDispatchVolume(
        const std::string& output_image_path
    ) const;

    // Resolves default temporary path for kiosk_dispatch.img
    std::string GetDefaultDispatchImagePath() const;

    // Removes temporary dispatch image file on session end
    bool CleanupDispatchVolume(const std::string& image_path) const;

private:
    BootInjector() = default;
};

#endif // defined(C_KIOSK)

#endif // DOSBOX_KIOSK_BOOT_INJECTOR_H
