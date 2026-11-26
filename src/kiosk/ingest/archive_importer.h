#ifndef DOSBOX_KIOSK_ARCHIVE_IMPORTER_H
#define DOSBOX_KIOSK_ARCHIVE_IMPORTER_H

#include "../kiosk_types.h"
#include "conf_translator.h"
#include <cstdint>
#include <string>
#include <vector>

enum class IngestSourceFormat {
    UNKNOWN = 0,
    GOG_INNOSETUP,
    STEAM_ZIP,
    UNPACKED_DIR
};

enum class IngestDeployMode {
    APPLIANCE_CONTAINER = 0, // Mode A: INTEGRATED_DOS container under machines/containers/<uuid>/
    HDD_INJECT               // Mode B: Inject into REAL_HDD_BOOT machine's hdd_c.img (C:\GAMES\<SLUG>\)
};

struct IngestOptions {
    std::string source_path;                                   // Path to setup_*.exe, *.zip, or folder
    IngestDeployMode deploy_mode = IngestDeployMode::APPLIANCE_CONTAINER;
    std::string target_machine_id;                             // Required for HDD_INJECT; optional for Mode A
    std::string archetype_override;                            // Optional Mode A override (e.g. ARCH_AT_286); empty = Auto-Detect
    std::string kiosk_data_root;                               // Defaults to platform kiosk data dir
    std::string innoextract_binary;                            // Optional override; auto-discovered in PATH
    std::string item_type = "GAME";                            // "GAME" or "APPLICATION"
};

struct IngestMetadata {
    std::string title;
    std::string developer;
    int release_year = 0;
    std::string gog_game_id;
    std::string steam_appid;
    std::string primary_play_task_path;
    std::string primary_play_task_args;
    std::string icon_source_path;
    std::string source_origin = "STEAM_IMPORT";                // "GOG_IMPORT" or "STEAM_IMPORT"
};

struct NormalizedMediaEntry {
    std::string original_rel_path;
    std::string normalized_pool_path;
    std::string media_type;                                    // CDROM_ISO, CDROM_CUE, FLOPPY_*
    std::string volume_label;
    bool has_redbook_audio = false;
    std::string media_pool_id;
};

struct IngestResult {
    bool success = false;
    std::string error_message;
    std::string catalog_item_id;
    std::string machine_id;
    std::string archetype_id;
    std::string title;
    std::string deployed_path;                                 // Container dir (Mode A) or C:\GAMES\<SLUG> (Mode B)
    int media_registered_count = 0;
    int caddy_bindings_count = 0;
    int config_files_patched = 0;
};

class ArchiveImporter {
public:
    // End-to-end 4-stage ingestion pipeline (Unpack -> Normalize -> Translate -> Deploy & Catalog)
    static IngestResult ImportArchive(const IngestOptions& options);

    // Stage 1A: Format detection & unpacking
    static IngestSourceFormat DetectSourceFormat(
        const std::string& path,
        const std::string& innoextract_override = ""
    );
    static std::string FindInnoextractBinary(const std::string& override_path = "");
    static bool ExtractZipArchive(
        const std::string& zip_path,
        const std::string& dest_dir,
        std::string& out_error
    );
    static bool ExtractGogInstaller(
        const std::string& exe_path,
        const std::string& dest_dir,
        const std::string& innoextract_bin,
        std::string& out_error
    );

    // Stage 1B & 1C: Metadata harvesting, icon conversion & wrapper cruft sanitization
    static bool ParseGogInfoJson(const std::string& json_text, IngestMetadata& out_meta);
    static bool ExtractIcoToPng(const std::string& ico_path, const std::string& out_png_path);
    static bool HarvestMetadata(
        const std::string& sandbox_dir,
        IngestMetadata& out_meta,
        const std::string& icon_cache_dir,
        const std::string& item_id,
        std::string& out_icon_png_path
    );
    static int SanitizeWrapperCruft(const std::string& sandbox_dir);

    // Stage 2: Optical & floppy media normalizer
    static std::string ExtractIso9660VolumeLabel(const std::string& image_path);
    static bool SanitizeCueSheet(
        const std::string& cue_content,
        const std::string& source_cue_dir,
        const std::string& dest_bin_basename,
        const std::string& audio_subdir_name,
        const std::string& dest_audio_dir,
        std::string& out_rewritten_cue,
        bool& out_has_redbook_audio,
        std::string& out_error
    );
    static bool NormalizeMediaInSandbox(
        const std::string& sandbox_dir,
        const std::string& item_slug,
        const std::string& media_pool_dir,
        std::vector<NormalizedMediaEntry>& out_entries,
        std::string& out_error
    );

    // Stage 4B helpers: Mode B config path patching & userspace FAT16/FAT32 HDD injection
    static int PatchGameConfigPaths(
        const std::string& game_root_dir,
        const std::string& guest_install_dir
    );
    static bool InjectDirectoryIntoFatImage(
        const std::string& hdd_img_path,
        const std::string& host_source_dir,
        const std::string& guest_dest_dir,
        bool allow_vfat_lfn,
        std::string& out_error
    );

    // Utility helpers
    static std::string GetDefaultKioskDataRoot();
    static std::string MakeSlug(const std::string& raw_name, size_t max_len = 8);
    static std::string GetOrCreateContainerMachine(
        const std::string& archetype_id,
        const std::string& custom_conf_overrides = ""
    );
};

#endif // DOSBOX_KIOSK_ARCHIVE_IMPORTER_H
