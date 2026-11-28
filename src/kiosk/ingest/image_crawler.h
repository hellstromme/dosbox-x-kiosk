#ifndef DOSBOX_KIOSK_IMAGE_CRAWLER_H
#define DOSBOX_KIOSK_IMAGE_CRAWLER_H

#include "../kiosk_types.h"
#include "../fs/fat_volume.h"
#include "signature_matcher.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class imageDisk;

// Strictly read-only facade over the shared KioskFatVolume (src/kiosk/fs/fat_volume.h)
class FatReadOnlyVfs {
public:
    FatReadOnlyVfs() = default;
    ~FatReadOnlyVfs() = default;

    FatReadOnlyVfs(const FatReadOnlyVfs&) = delete;
    FatReadOnlyVfs& operator=(const FatReadOnlyVfs&) = delete;

    // Opens a raw .img/.ima or .vhd disk image strictly read-only.
    bool OpenImageFile(const std::string& image_path);

    // Wraps an already-open imageDisk without taking ownership (used for unit tests / in-memory disks).
    bool OpenFromImageDisk(imageDisk* non_owning_disk);

    void Close() { m_volume.Close(); }
    bool IsOpen() const { return m_volume.IsMounted(); }

    FilesystemType GetFilesystemType() const { return m_volume.GetFilesystemType(); }
    uint32_t GetPartitionLbaStart() const { return m_volume.GetPartitionLbaStart(); }
    uint32_t GetBytesPerSector() const { return m_volume.GetBytesPerSector(); }
    uint32_t GetSectorsPerCluster() const { return m_volume.GetSectorsPerCluster(); }
    uint32_t GetClusterCount() const { return m_volume.GetClusterCount(); }
    bool IsWriteProtected() const { return !m_volume.IsWritable(); }

    // Lists entries in the given directory (pass nullptr for root directory).
    bool ListDirectory(const FatDirEntry* dir, std::vector<FatDirEntry>& out_entries) const {
        return m_volume.ListDirectory(dir, out_entries);
    }

    // Case-insensitive path lookup (supports optional "C:\" prefix and LFN or 8.3 names).
    bool FindPath(const std::string& guest_path, FatDirEntry& out_entry) const {
        return m_volume.FindPath(guest_path, out_entry);
    }

    // Reads up to max_bytes of a file entry strictly read-only.
    bool ReadFile(
        const FatDirEntry& entry,
        std::vector<uint8_t>& out_data,
        size_t max_bytes = 16U * 1024U * 1024U
    ) const {
        return m_volume.ReadFile(entry, out_data, max_bytes);
    }

private:
    KioskFatVolume m_volume;
};

struct Win31GrpItem {
    std::string group_title;
    std::string title;
    std::string command_line;
    std::string executable;
    std::string arguments;
    std::string icon_path;
    uint16_t icon_index = 0;
    std::vector<uint8_t> embedded_rgba;
    int embedded_icon_w = 0;
    int embedded_icon_h = 0;
};

struct Win9xLnkInfo {
    std::string target_path;
    std::string working_dir;
    std::string arguments;
    std::string description;
    std::string icon_location;
    int32_t icon_index = 0;
};

class ImageCrawler {
public:
    struct CrawlResult {
        std::string title;
        std::string developer;
        int release_year = 0;
        std::string item_type = "GAME";                  // "GAME" or "APPLICATION"
        std::string launch_mechanism = "DOS_AUTOEXEC_INJECT";
        std::string working_dir;                         // e.g. "C:\DOOM2"
        std::string executable;                          // e.g. "DOOM2.EXE"
        std::string arguments;
        std::string executable_crc32;
        std::string icon_output_path;
        int score = 0;
        bool matched_signature = false;
    };

    struct CrawlSummary {
        int total_discovered = 0;
        int newly_inserted = 0;
        int skipped_duplicates = 0;
        int failed_inserts = 0;
        int icons_extracted = 0;
        int failed_icon_updates = 0;
        std::string error_message;
    };

    // Scans a raw/VHD read-only disk image (partition -> VFAT walker -> Win3.1/Win9x/DOS discovery)
    static std::vector<CrawlResult> ScanDiskImage(
        const std::string& hdd_img_path,
        const std::string& icon_cache_dir = "",
        const std::string& cache_namespace = ""
    );

    // Scans an already-mounted FatReadOnlyVfs (used directly by unit tests)
    static std::vector<CrawlResult> ScanMountedVfs(
        const FatReadOnlyVfs& vfs,
        const std::string& icon_cache_dir = "",
        const std::string& cache_namespace = "",
        std::string* out_error = nullptr
    );

    // Scans a registered machine's primary_hdd_path and persists deduplicated SCANNED items to KioskDatabase.
    static bool ScanAndCatalogMachine(
        const std::string& machine_id,
        const std::string& icon_cache_dir,
        CrawlSummary& out_summary
    );

    // Heuristic filters (public for unit testing)
    static bool IsPrunedSystemDirectory(const std::string& normalized_dir_path);
    static bool IsIgnoredUtilityBinary(const std::string& filename);
    static int ScoreDosCandidate(
        const std::string& filename,
        const std::string& parent_dir_name,
        const ExecutableFingerprint& fp,
        bool bat_calls_sibling_exe,
        bool has_exact_signature,
        bool is_ignore_signature
    );

    // Shortcut & Icon Extractors (public for unit testing)
    static bool ParseWin31GrpBuffer(const uint8_t* data, size_t size, std::vector<Win31GrpItem>& out_items);
    static bool ParseWin9xLnkBuffer(const uint8_t* data, size_t size, Win9xLnkInfo& out_info);
    static bool ExtractPeNeIcon(
        const uint8_t* file_data,
        size_t size,
        const std::string& out_png_path,
        int preferred_icon_index = 0
    );
    static bool WriteRgbaPng(
        const std::string& out_png_path,
        int width,
        int height,
        const std::vector<uint8_t>& rgba_pixels
    );

    // Resolves default icon cache directory (writable temp/cache path)
    static std::string GetDefaultIconCacheDir();
};

#endif // DOSBOX_KIOSK_IMAGE_CRAWLER_H
