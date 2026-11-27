#ifndef DOSBOX_KIOSK_VIEW_DISK_CADDY_H
#define DOSBOX_KIOSK_VIEW_DISK_CADDY_H

#include "../kiosk_types.h"
#include <string>
#include <vector>
#include <cstdint>

struct ImDrawList;

struct InspectedMediaInfo {
    std::string media_type = "FLOPPY_35_1440K";     // MediaType string
    std::string physical_style = "BEIGE_35";        // MediaPhysicalStyle string
    std::string volume_label;                       // Extracted from FAT BPB/Root or ISO9660 PVD
    std::string label_title;                        // Derived or explicit line 1
    std::string label_subtitle;                     // Derived geometry description (e.g., "3.5\" 1.44MB HD")
    bool has_redbook_audio = false;                 // True if CUE sheet contains AUDIO tracks
    uint64_t file_size_bytes = 0;
};

struct DiskCardRenderParams {
    MediaPoolRecord media;
    std::string target_drive = "A";                 // "A", "B", "D", "E"
    int slot_order = 1;
    bool is_mounted_active = false;                 // True if currently inserted in target_drive
    bool is_selected_focused = false;               // True if highlighted by keyboard/gamepad cursor
    float card_width = 168.0f;
    float card_height = 188.0f;
};

class DiskCaddyView {
public:
    static DiskCaddyView& GetInstance();

    // 1. Deterministic CRC32 & Visual Variety Helpers
    static uint32_t ComputeCrc32(const void* data, size_t length);
    static uint32_t ComputeStringCrc32(const std::string& str);

    // Deterministically resolves physical style ("CHARCOAL_525", "BEIGE_35", "BLUE_35", "BLACK_35", "JEWEL_CD", "GOLD_CD")
    // When a 3.5" floppy uses default/auto style, hashes id/path (CRC32 % 3) across Beige, Blue, and Black.
    static std::string ResolvePhysicalStyle(
        const std::string& media_type,
        const std::string& explicit_style,
        const std::string& seed_id_or_path
    );

    // Deterministic sticker rotation jitter in degrees within [-0.8f, +0.8f]
    static float ComputeStickerAngleJitterDeg(const std::string& seed_id_or_path);

    // 2. Non-Destructive Media Header & Volume Label Inspector
    // Inspects file size, FAT12/16 BPB (0x2B) / root volume entry, ISO9660 PVD (0x8028), or CUE sheet
    static bool InspectMediaFile(const std::string& file_path, InspectedMediaInfo& out_info);
    static bool ExtractFatVolumeLabelFromBytes(const uint8_t* data, size_t size, std::string& out_label);
    static bool ExtractIso9660VolumeLabelFromFile(const std::string& file_path, std::string& out_label);
    static bool DetectCueSheetRedbookAudio(const std::string& cue_path, std::string& out_first_bin_path);
    static bool ExtractCueReferencedFiles(const std::string& cue_path, std::vector<std::string>& out_raw_names, std::vector<std::string>& out_resolved_paths);
    static std::string HumanReadableMediaTypeBadge(const std::string& media_type);
    static std::string DeriveDefaultMediaGroup(const std::string& file_path);
    static std::string FormatMediaDisplayName(const MediaPoolRecord& media);

    // 3. ImGui Procedural Skeuomorphic Card Renderer
    // Returns true if the card was clicked/activated by the user in the current frame.
    bool RenderMediaCard(const DiskCardRenderParams& params);

private:
    DiskCaddyView() = default;

    void DrawFloppy525(ImDrawList* draw_list, float x, float y, float w, float h, const DiskCardRenderParams& params, uint32_t seed_hash);
    void DrawFloppy35(ImDrawList* draw_list, float x, float y, float w, float h, const DiskCardRenderParams& params, uint32_t seed_hash);
    void DrawCdJewelCase(ImDrawList* draw_list, float x, float y, float w, float h, const DiskCardRenderParams& params, uint32_t seed_hash);
    void DrawStickerTypography(ImDrawList* draw_list, float lx, float ly, float lw, float lh, const DiskCardRenderParams& params, uint32_t seed_hash);
};

#endif // DOSBOX_KIOSK_VIEW_DISK_CADDY_H
