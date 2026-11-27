#include "config.h"

#if defined(C_KIOSK)

#include "view_disk_caddy.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

// Opens a path read-only without blocking on FIFOs/special files on POSIX.
FILE* OpenRegularFileReadOnly(const std::string& path) {
    if (path.empty()) return nullptr;
#if !defined(_WIN32)
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return nullptr;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return nullptr;
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    FILE* fp = fdopen(fd, "rb");
    if (!fp) {
        close(fd);
        return nullptr;
    }
    return fp;
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

std::string ToUpperAscii(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    }
    return out;
}

std::string TrimAsciiLabel(const char* raw, size_t max_len) {
    std::string s;
    s.reserve(max_len);
    for (size_t i = 0; i < max_len; ++i) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == 0) {
            break;
        }
        if (c >= 0x20 && c <= 0x7E) {
            s.push_back(static_cast<char>(c));
        } else {
            s.push_back(' ');
        }
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
        ++start;
    }
    return s.substr(start);
}

std::string ExtractFilenameStem(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string fname = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = fname.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        return fname.substr(0, dot);
    }
    return fname;
}

std::string ExtractExtensionLower(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string fname = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = fname.find_last_of('.');
    if (dot == std::string::npos) {
        return std::string();
    }
    std::string ext = fname.substr(dot);
    for (size_t i = 0; i < ext.size(); ++i) {
        ext[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));
    }
    return ext;
}

std::string ExtractParentDirectory(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return std::string();
    }
    return path.substr(0, slash + 1);
}

std::string TruncateWithEllipsis(const std::string& text, size_t max_chars) {
    if (text.size() <= max_chars || max_chars < 4) {
        return text;
    }
    return text.substr(0, max_chars - 2) + "..";
}

} // anonymous namespace

DiskCaddyView& DiskCaddyView::GetInstance() {
    static DiskCaddyView instance;
    return instance;
}

uint32_t DiskCaddyView::ComputeCrc32(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint32_t>(bytes[i]);
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

uint32_t DiskCaddyView::ComputeStringCrc32(const std::string& str) {
    if (str.empty()) {
        return 0u;
    }
    return ComputeCrc32(str.data(), str.size());
}

std::string DiskCaddyView::ResolvePhysicalStyle(
    const std::string& media_type,
    const std::string& explicit_style,
    const std::string& seed_id_or_path
) {
    if (media_type == "FLOPPY_525_360K" || media_type == "FLOPPY_525_1200K") {
        return "CHARCOAL_525";
    }
    if (media_type == "CDROM_ISO" || media_type == "CDROM_CUE") {
        if (explicit_style == "GOLD_CD") {
            return "GOLD_CD";
        }
        return "JEWEL_CD";
    }

    // 3.5" microfloppy styles
    if (explicit_style == "BEIGE_35" || explicit_style == "BLUE_35" || explicit_style == "BLACK_35") {
        return explicit_style;
    }
    if (!explicit_style.empty() && explicit_style != "AUTO") {
        return "BEIGE_35";
    }

    uint32_t hash = ComputeStringCrc32(seed_id_or_path);
    switch (hash % 3u) {
        case 0:  return "BEIGE_35";
        case 1:  return "BLUE_35";
        default: return "BLACK_35";
    }
}

float DiskCaddyView::ComputeStickerAngleJitterDeg(const std::string& seed_id_or_path) {
    if (seed_id_or_path.empty()) {
        return 0.0f;
    }
    uint32_t hash = ComputeStringCrc32(seed_id_or_path);
    float normalized = static_cast<float>(hash & 0xFFFFu) / 65535.0f; // [0.0, 1.0]
    float angle = -0.8f + (normalized * 1.6f);
    if (angle < -0.8f) angle = -0.8f;
    if (angle > 0.8f) angle = 0.8f;
    return angle;
}

bool DiskCaddyView::ExtractFatVolumeLabelFromBytes(const uint8_t* data, size_t size, std::string& out_label) {
    out_label.clear();
    if (!data || size < 512) {
        return false;
    }

    const uint16_t bytes_per_sec = static_cast<uint16_t>(data[0x0B]) |
                                   (static_cast<uint16_t>(data[0x0C]) << 8);
    const uint16_t rsvd_sec = static_cast<uint16_t>(data[0x0E]) |
                              (static_cast<uint16_t>(data[0x0F]) << 8);
    const uint8_t num_fats = data[0x10];
    const uint16_t root_ent_cnt = static_cast<uint16_t>(data[0x11]) |
                                  (static_cast<uint16_t>(data[0x12]) << 8);
    const uint16_t fat_sz_16 = static_cast<uint16_t>(data[0x16]) |
                               (static_cast<uint16_t>(data[0x17]) << 8);

    // 1. Check Root Directory for an explicit Volume Label entry (DIR_Attr == 0x08)
    if (bytes_per_sec >= 128 && bytes_per_sec <= 4096 &&
        rsvd_sec > 0 && num_fats > 0 && fat_sz_16 > 0 && root_ent_cnt > 0) {
        const uint64_t root_dir_offset_u64 =
            (static_cast<uint64_t>(rsvd_sec) + static_cast<uint64_t>(num_fats) * fat_sz_16) * bytes_per_sec;
        if (root_dir_offset_u64 <= static_cast<uint64_t>(size)) {
            for (uint16_t i = 0; i < root_ent_cnt; ++i) {
                const uint64_t entry_ofs_u64 = root_dir_offset_u64 + static_cast<uint64_t>(i) * 32u;
                if (entry_ofs_u64 > static_cast<uint64_t>(size)) {
                    break;
                }
                const size_t entry_ofs = static_cast<size_t>(entry_ofs_u64);
                if (size - entry_ofs < 32u) {
                    break;
                }
                const uint8_t first_byte = data[entry_ofs];
                if (first_byte == 0x00) {
                    break; // End of root directory
                }
                if (first_byte == 0xE5) {
                    continue; // Deleted entry
                }
                const uint8_t attr = data[entry_ofs + 11];
                if (attr == 0x0F) {
                    continue; // LFN entry
                }
                if ((attr & 0x08) != 0 && (attr & 0x10) == 0) {
                    std::string candidate = TrimAsciiLabel(reinterpret_cast<const char*>(data + entry_ofs), 11);
                    if (!candidate.empty() && ToUpperAscii(candidate) != "NO NAME") {
                        out_label = candidate;
                        return true;
                    }
                }
            }
        }
    }

    // 2. Fallback to FAT12/16 Extended BPB Volume Label at offset 0x2B (when BootSig at 0x26 is 0x28 or 0x29)
    if (data[0x26] == 0x28 || data[0x26] == 0x29) {
        std::string bpb_label = TrimAsciiLabel(reinterpret_cast<const char*>(data + 0x2B), 11);
        if (!bpb_label.empty() && ToUpperAscii(bpb_label) != "NO NAME") {
            out_label = bpb_label;
            return true;
        }
    }

    // 3. Fallback to FAT32 Extended BPB Volume Label at offset 0x47 (when BootSig at 0x42 is 0x28 or 0x29)
    if (root_ent_cnt == 0 && fat_sz_16 == 0 && (data[0x42] == 0x28 || data[0x42] == 0x29)) {
        std::string fat32_label = TrimAsciiLabel(reinterpret_cast<const char*>(data + 0x47), 11);
        if (!fat32_label.empty() && ToUpperAscii(fat32_label) != "NO NAME") {
            out_label = fat32_label;
            return true;
        }
    }

    return false;
}

bool DiskCaddyView::ExtractIso9660VolumeLabelFromFile(const std::string& file_path, std::string& out_label) {
    out_label.clear();
    if (file_path.empty()) {
        return false;
    }

    FILE* fp = OpenRegularFileReadOnly(file_path);
    if (!fp) {
        return false;
    }

    // Probe candidate sector 16 offsets:
    // - 0x8000: Standard 2048-byte cooked ISO9660 sector 16 (16 * 2048)
    // - 0x9310: Raw 2352-byte Mode 1 BIN sector 16 (16 * 2352 + 16)
    // - 0x9318: Raw 2352-byte Mode 2 Form 1 BIN sector 16 (16 * 2352 + 24)
    const long candidate_offsets[3] = { 0x8000L, 0x9310L, 0x9318L };
    uint8_t pvd_header[72];

    for (size_t i = 0; i < 3; ++i) {
        if (std::fseek(fp, candidate_offsets[i], SEEK_SET) == 0) {
            if (std::fread(pvd_header, 1, sizeof(pvd_header), fp) == sizeof(pvd_header)) {
                if (pvd_header[0] == 0x01 && std::memcmp(pvd_header + 1, "CD001", 5) == 0) {
                    std::string label = TrimAsciiLabel(reinterpret_cast<const char*>(pvd_header + 40), 32);
                    std::fclose(fp);
                    if (!label.empty()) {
                        out_label = label;
                        return true;
                    }
                    return false;
                }
            }
        }
    }

    std::fclose(fp);
    return false;
}

bool DiskCaddyView::DetectCueSheetRedbookAudio(const std::string& cue_path, std::string& out_first_bin_path) {
    out_first_bin_path.clear();
    if (cue_path.empty()) {
        return false;
    }

    FILE* fp = OpenRegularFileReadOnly(cue_path);
    if (!fp) {
        return false;
    }

    const std::string parent_dir = ExtractParentDirectory(cue_path);
    bool has_audio = false;
    char line_buf[1024];

    while (std::fgets(line_buf, sizeof(line_buf), fp) != nullptr) {
        std::string line(line_buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        std::string upper = ToUpperAscii(line);

        // Parse FILE "<filename>" BINARY
        if (out_first_bin_path.empty()) {
            size_t file_pos = upper.find("FILE");
            if (file_pos != std::string::npos &&
                (file_pos == 0 || std::isspace(static_cast<unsigned char>(upper[file_pos - 1])))) {
                size_t q1 = line.find('"', file_pos + 4);
                if (q1 != std::string::npos) {
                    size_t q2 = line.find('"', q1 + 1);
                    if (q2 != std::string::npos && q2 > q1 + 1) {
                        std::string rel = line.substr(q1 + 1, q2 - q1 - 1);
                        if (rel.find(':') != std::string::npos || (!rel.empty() && (rel[0] == '/' || rel[0] == '\\'))) {
                            out_first_bin_path = rel;
                        } else {
                            out_first_bin_path = parent_dir + rel;
                        }
                    }
                }
            }
        }

        // Check for TRACK <N> AUDIO
        std::istringstream iss(upper);
        std::string tok1, tok2, tok3;
        if (iss >> tok1 >> tok2 >> tok3) {
            if (tok1 == "TRACK" && tok3 == "AUDIO") {
                has_audio = true;
            }
        }
    }

    std::fclose(fp);
    return has_audio;
}

std::string DiskCaddyView::HumanReadableMediaTypeBadge(const std::string& media_type) {
    if (media_type == "FLOPPY_525_360K")  return "5.25\" 360KB DD";
    if (media_type == "FLOPPY_525_1200K") return "5.25\" 1.2MB HD";
    if (media_type == "FLOPPY_35_720K")   return "3.5\" 720KB DD";
    if (media_type == "FLOPPY_35_1440K")  return "3.5\" 1.44MB HD";
    if (media_type == "FLOPPY_35_2880K")  return "3.5\" 2.88MB ED";
    if (media_type == "CDROM_ISO")        return "CD-ROM ISO9660";
    if (media_type == "CDROM_CUE")        return "CD-ROM BIN/CUE";
    if (media_type == "HDD_IMG")          return "Fixed Disk";
    return "3.5\" 1.44MB HD";
}

bool DiskCaddyView::InspectMediaFile(const std::string& file_path, InspectedMediaInfo& out_info) {
    out_info = InspectedMediaInfo();
    if (file_path.empty()) {
        return false;
    }

    out_info.label_title = ExtractFilenameStem(file_path);
    const std::string ext = ExtractExtensionLower(file_path);

    if (ext == ".cue") {
        out_info.media_type = "CDROM_CUE";
        out_info.physical_style = "JEWEL_CD";
        std::string first_bin_path;
        out_info.has_redbook_audio = DetectCueSheetRedbookAudio(file_path, first_bin_path);

        FILE* fp = OpenRegularFileReadOnly(file_path);
        if (!fp) {
            return false;
        }
        std::fseek(fp, 0, SEEK_END);
        long sz = std::ftell(fp);
        std::fclose(fp);
        if (sz > 0) {
            out_info.file_size_bytes = static_cast<uint64_t>(sz);
        }

        if (!first_bin_path.empty()) {
            ExtractIso9660VolumeLabelFromFile(first_bin_path, out_info.volume_label);
        }
        out_info.label_subtitle = out_info.has_redbook_audio
                                      ? "CD-ROM + Redbook Audio"
                                      : HumanReadableMediaTypeBadge(out_info.media_type);
        if (!out_info.volume_label.empty() && out_info.label_title.empty()) {
            out_info.label_title = out_info.volume_label;
        }
        return true;
    }

    FILE* fp = OpenRegularFileReadOnly(file_path);
    if (!fp) {
        return false;
    }

#if defined(_WIN32)
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return false;
    }
    const int64_t file_sz = _ftelli64(fp);
#else
    if (fseeko(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return false;
    }
    const int64_t file_sz = static_cast<int64_t>(ftello(fp));
#endif
    if (file_sz < 0) {
        std::fclose(fp);
        return false;
    }
    out_info.file_size_bytes = static_cast<uint64_t>(file_sz);

    if (ext == ".iso" || ext == ".bin" || out_info.file_size_bytes > 4u * 1024u * 1024u) {
        std::fclose(fp);
        out_info.media_type = (ext == ".bin") ? "CDROM_CUE" : "CDROM_ISO";
        out_info.physical_style = "JEWEL_CD";
        ExtractIso9660VolumeLabelFromFile(file_path, out_info.volume_label);
        out_info.label_subtitle = HumanReadableMediaTypeBadge(out_info.media_type);
        return true;
    }

    // Read up to 64 KB to inspect FAT12/16 BPB & Root Directory
    const size_t probe_len = static_cast<size_t>(std::min<uint64_t>(out_info.file_size_bytes, 65536u));
    std::vector<uint8_t> header_buf(probe_len, 0);
    std::fseek(fp, 0, SEEK_SET);
    size_t got = std::fread(header_buf.data(), 1, probe_len, fp);
    std::fclose(fp);

    if (got >= 512) {
        ExtractFatVolumeLabelFromBytes(header_buf.data(), got, out_info.volume_label);
    }

    const uint64_t kb = out_info.file_size_bytes / 1024u;
    if (kb <= 400u) {
        out_info.media_type = "FLOPPY_525_360K";
        out_info.physical_style = "CHARCOAL_525";
    } else if (kb <= 800u) {
        out_info.media_type = "FLOPPY_35_720K";
        out_info.physical_style = ResolvePhysicalStyle(out_info.media_type, "", file_path);
    } else if (kb <= 1240u) {
        out_info.media_type = "FLOPPY_525_1200K";
        out_info.physical_style = "CHARCOAL_525";
    } else if (kb <= 1920u) {
        out_info.media_type = "FLOPPY_35_1440K";
        out_info.physical_style = ResolvePhysicalStyle(out_info.media_type, "", file_path);
    } else {
        out_info.media_type = "FLOPPY_35_2880K";
        out_info.physical_style = "BLACK_35";
    }

    out_info.label_subtitle = HumanReadableMediaTypeBadge(out_info.media_type);
    return true;
}

std::string DiskCaddyView::DeriveDefaultMediaGroup(const std::string& file_path) {
    if (file_path.empty()) return "";
    size_t last_sep = file_path.find_last_of("/\\");
    if (last_sep == std::string::npos || last_sep == 0) return "";
    std::string parent = file_path.substr(0, last_sep);
    while (parent.size() > 1 && (parent.back() == '/' || parent.back() == '\\')) {
        parent.pop_back();
    }
    // Skip bare drive root like "C:"
    if (parent.size() <= 2 && parent.size() >= 2 && parent[1] == ':') return "";

    size_t prev_sep = parent.find_last_of("/\\");
    std::string folder = (prev_sep == std::string::npos) ? parent : parent.substr(prev_sep + 1);
    if (folder.empty()) return "";

    // Ignore generic host/staging directory names so standalone images stay cleanly Ungrouped
    std::string low = folder;
    for (size_t i = 0; i < low.size(); ++i) {
        low[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(low[i])));
    }
    if (low == "media_pool" || low == "downloads" || low == "desktop" ||
        low == "temp" || low == "tmp" || low == "documents") {
        return "";
    }
    return folder;
}

std::string DiskCaddyView::FormatMediaDisplayName(const MediaPoolRecord& media) {
    const std::string title = !media.label_title.empty()
                                  ? media.label_title
                                  : (!media.volume_label.empty()
                                         ? media.volume_label
                                         : ExtractFilenameStem(media.file_path));
    if (!media.media_group.empty()) {
        return "[" + media.media_group + "] " + title;
    }
    return title;
}

void DiskCaddyView::DrawStickerTypography(
    ImDrawList* draw_list,
    float lx,
    float ly,
    float lw,
    float lh,
    const DiskCardRenderParams& params,
    uint32_t seed_hash
) {
    if (!draw_list) return;

    // Era-colored top stripe on paper sticker
    ImU32 stripe_col = IM_COL32(180, 68, 42, 255); // Warm rust-red
    if (params.media.media_type == "FLOPPY_35_1440K" || params.media.media_type == "FLOPPY_35_2880K") {
        stripe_col = IM_COL32(42, 108, 178, 255); // High-density blue stripe
    } else if (params.media.media_type == "CDROM_ISO" || params.media.media_type == "CDROM_CUE") {
        stripe_col = IM_COL32(36, 138, 130, 255); // Teal CD booklet stripe
    } else if ((seed_hash & 1u) != 0u) {
        stripe_col = IM_COL32(204, 132, 34, 255); // Amber stripe
    }

    const float stripe_h = std::max(10.0f, lh * 0.20f);
    draw_list->AddRectFilled(ImVec2(lx, ly), ImVec2(lx + lw, ly + stripe_h), stripe_col, 3.0f);

    // Header badge text inside stripe: Media Group (if set) or Target Drive & Slot
    std::string stripe_text;
    if (!params.media.media_group.empty()) {
        stripe_text = TruncateWithEllipsis(params.media.media_group, 18);
    } else {
        char header_buf[48];
        std::snprintf(header_buf, sizeof(header_buf), "%s: DISK #%d",
                      params.target_drive.empty() ? "A" : params.target_drive.c_str(),
                      params.slot_order);
        stripe_text = header_buf;
    }
    draw_list->AddText(ImVec2(lx + 5.0f, ly + 1.0f), IM_COL32(255, 250, 240, 255), stripe_text.c_str());

    // Optional ruled lines for DOT_MATRIX / HANDWRITTEN styles
    if (params.media.label_font_style != "PRINTED_OEM") {
        const ImU32 rule_col = IM_COL32(205, 198, 180, 180);
        for (float ry = ly + stripe_h + 15.0f; ry < ly + lh - 4.0f; ry += 14.0f) {
            draw_list->AddLine(ImVec2(lx + 4.0f, ry), ImVec2(lx + lw - 4.0f, ry), rule_col, 1.0f);
        }
    }

    // Primary Title (Line 1)
    std::string title = params.media.label_title;
    if (title.empty()) {
        title = !params.media.volume_label.empty()
                    ? params.media.volume_label
                    : ExtractFilenameStem(params.media.file_path);
    }
    title = TruncateWithEllipsis(title, 18);

    ImU32 ink_col = IM_COL32(28, 30, 36, 255);
    if (params.media.label_font_style == "HANDWRITTEN") {
        ink_col = IM_COL32(24, 48, 128, 255); // Ballpoint blue ink
    }
    draw_list->AddText(ImVec2(lx + 5.0f, ly + stripe_h + 3.0f), ink_col, title.c_str());

    // Subtitle / Geometry (Line 2)
    std::string subtitle = params.media.label_subtitle;
    if (subtitle.empty()) {
        subtitle = HumanReadableMediaTypeBadge(params.media.media_type);
    }
    subtitle = TruncateWithEllipsis(subtitle, 20);
    draw_list->AddText(ImVec2(lx + 5.0f, ly + stripe_h + 17.0f), IM_COL32(82, 84, 90, 255), subtitle.c_str());

    // Volume Label (Line 3, if present)
    if (!params.media.volume_label.empty() && lh >= 52.0f) {
        std::string vol = "VOL: " + TruncateWithEllipsis(params.media.volume_label, 14);
        draw_list->AddText(ImVec2(lx + 5.0f, ly + stripe_h + 31.0f), IM_COL32(65, 92, 65, 255), vol.c_str());
    }
}

void DiskCaddyView::DrawFloppy525(
    ImDrawList* draw_list,
    float x,
    float y,
    float w,
    float h,
    const DiskCardRenderParams& params,
    uint32_t seed_hash
) {
    // Layer 1: Flexible 5.25" Charcoal outer jacket with subtle border
    const ImU32 jacket_col = IM_COL32(38, 40, 44, 255);
    const ImU32 jacket_border = IM_COL32(72, 76, 82, 255);
    draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), jacket_col, 4.0f);
    draw_list->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), jacket_border, 4.0f, 0, 1.5f);

    // Layer 2: Hardware details
    // Right-edge write-protect notch
    const float notch_y = y + h * 0.24f;
    const float notch_h = 12.0f;
    const float notch_w = 7.0f;
    if (params.media.is_write_protected) {
        // Metallic silver write-protect adhesive tab covering the notch
        draw_list->AddRectFilled(
            ImVec2(x + w - notch_w - 2.0f, notch_y - 1.0f),
            ImVec2(x + w, notch_y + notch_h + 1.0f),
            IM_COL32(198, 204, 212, 255),
            1.0f
        );
    } else {
        // Open dark cutout notch on right edge
        draw_list->AddRectFilled(
            ImVec2(x + w - notch_w, notch_y),
            ImVec2(x + w + 1.0f, notch_y + notch_h),
            IM_COL32(14, 15, 18, 255),
            0.0f
        );
    }

    // Center drive spindle hub ring & inner dark cutout
    const float cx = x + w * 0.50f;
    const float cy = y + h * 0.54f;
    const float hub_r = std::min(w, h) * 0.16f;
    draw_list->AddCircleFilled(ImVec2(cx, cy), hub_r + 3.5f, IM_COL32(55, 58, 64, 255), 28);
    draw_list->AddCircleFilled(ImVec2(cx, cy), hub_r, IM_COL32(18, 19, 22, 255), 28);
    draw_list->AddCircle(ImVec2(cx, cy), hub_r, IM_COL32(88, 92, 98, 255), 28, 1.2f);

    // Small index timing hole to the lower-right of the spindle hub
    draw_list->AddCircleFilled(
        ImVec2(cx + hub_r + 10.0f, cy + hub_r * 0.65f),
        3.5f,
        IM_COL32(18, 19, 22, 255),
        12
    );

    // Bottom vertical oval read/write head access slot exposing brown magnetic oxide media
    const float slot_w = w * 0.13f;
    const float slot_top = cy + hub_r + 7.0f;
    const float slot_bot = y + h - 8.0f;
    if (slot_bot > slot_top) {
        draw_list->AddRectFilled(
            ImVec2(cx - slot_w * 0.5f, slot_top),
            ImVec2(cx + slot_w * 0.5f, slot_bot),
            IM_COL32(62, 41, 31, 255),
            slot_w * 0.5f
        );
        draw_list->AddRect(
            ImVec2(cx - slot_w * 0.5f, slot_top),
            ImVec2(cx + slot_w * 0.5f, slot_bot),
            IM_COL32(25, 25, 28, 255),
            slot_w * 0.5f
        );
    }

    // Layer 3 & 4: Top-left aged cream paper sticker with deterministic jitter offset
    const float jitter = ComputeStickerAngleJitterDeg(
        params.media.id.empty() ? params.media.file_path : params.media.id
    );
    const float lx = x + 10.0f;
    const float ly = y + 8.0f + jitter * 1.2f;
    const float lw = w - 24.0f;
    const float lh = std::min(56.0f, h * 0.31f);

    draw_list->AddRectFilled(ImVec2(lx, ly), ImVec2(lx + lw, ly + lh), IM_COL32(244, 239, 224, 255), 3.0f);
    draw_list->AddRect(ImVec2(lx, ly), ImVec2(lx + lw, ly + lh), IM_COL32(168, 160, 142, 255), 3.0f);

    DrawStickerTypography(draw_list, lx, ly, lw, lh, params, seed_hash);
}

void DiskCaddyView::DrawFloppy35(
    ImDrawList* draw_list,
    float x,
    float y,
    float w,
    float h,
    const DiskCardRenderParams& params,
    uint32_t seed_hash
) {
    // Layer 1: Hard plastic 3.5" shell (Beige, Cobalt Blue, or Matte Black)
    const std::string style = ResolvePhysicalStyle(
        params.media.media_type,
        params.media.physical_style,
        params.media.id.empty() ? params.media.file_path : params.media.id
    );

    ImU32 shell_col = IM_COL32(216, 210, 193, 255);   // BEIGE_35
    ImU32 shell_border = IM_COL32(158, 152, 136, 255);
    ImU32 recess_col = IM_COL32(194, 188, 172, 255);
    if (style == "BLUE_35") {
        shell_col = IM_COL32(38, 86, 158, 255);       // BLUE_35
        shell_border = IM_COL32(72, 124, 198, 255);
        recess_col = IM_COL32(28, 68, 128, 255);
    } else if (style == "BLACK_35") {
        shell_col = IM_COL32(36, 39, 44, 255);        // BLACK_35
        shell_border = IM_COL32(82, 86, 94, 255);
        recess_col = IM_COL32(26, 28, 32, 255);
    }

    draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), shell_col, 5.0f);
    draw_list->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), shell_border, 5.0f, 0, 1.5f);

    // Chamfered top-right insertion notch visual accent
    draw_list->AddTriangleFilled(
        ImVec2(x + w - 12.0f, y),
        ImVec2(x + w, y),
        ImVec2(x + w, y + 12.0f),
        IM_COL32(18, 20, 24, 220)
    );

    // Layer 2: Top recessed area & brushed-metal sliding shutter
    const float shutter_recess_x = x + w * 0.22f;
    const float shutter_recess_w = w * 0.56f;
    const float shutter_h = h * 0.30f;
    draw_list->AddRectFilled(
        ImVec2(shutter_recess_x, y + 2.0f),
        ImVec2(shutter_recess_x + shutter_recess_w, y + shutter_h),
        recess_col,
        2.0f
    );

    const float metal_x = x + w * 0.26f;
    const float metal_w = w * 0.42f;
    draw_list->AddRectFilled(
        ImVec2(metal_x, y + 1.0f),
        ImVec2(metal_x + metal_w, y + shutter_h - 1.0f),
        IM_COL32(186, 194, 204, 255),
        2.0f
    );
    draw_list->AddRect(
        ImVec2(metal_x, y + 1.0f),
        ImVec2(metal_x + metal_w, y + shutter_h - 1.0f),
        IM_COL32(118, 124, 134, 255),
        2.0f
    );

    // Rectangular window cutout inside the metal shutter
    const float win_x = metal_x + metal_w * 0.58f;
    const float win_w = metal_w * 0.26f;
    draw_list->AddRectFilled(
        ImVec2(win_x, y + 7.0f),
        ImVec2(win_x + win_w, y + shutter_h - 6.0f),
        shell_col,
        1.0f
    );

    // Bottom-left write-protect tab indicator window
    const float wp_x = x + 7.0f;
    const float wp_y = y + h - 16.0f;
    if (params.media.is_write_protected) {
        // Open hole (write-protected) with red indicator outline
        draw_list->AddRectFilled(ImVec2(wp_x, wp_y), ImVec2(wp_x + 9.0f, wp_y + 9.0f), IM_COL32(16, 16, 20, 255), 1.0f);
        draw_list->AddRect(ImVec2(wp_x, wp_y), ImVec2(wp_x + 9.0f, wp_y + 9.0f), IM_COL32(220, 80, 70, 255), 1.0f);
    } else {
        // Closed plastic slider (writable)
        draw_list->AddRectFilled(ImVec2(wp_x, wp_y), ImVec2(wp_x + 9.0f, wp_y + 9.0f), IM_COL32(50, 54, 60, 255), 1.0f);
    }

    // HD hole on bottom-right for 1.44MB / 2.88MB floppies
    if (params.media.media_type == "FLOPPY_35_1440K" || params.media.media_type == "FLOPPY_35_2880K") {
        draw_list->AddRectFilled(
            ImVec2(x + w - 16.0f, wp_y),
            ImVec2(x + w - 7.0f, wp_y + 9.0f),
            IM_COL32(24, 26, 30, 255),
            1.0f
        );
    }

    // Layer 3 & 4: Bottom recessed paper sticker decal
    const float jitter = ComputeStickerAngleJitterDeg(
        params.media.id.empty() ? params.media.file_path : params.media.id
    );
    const float lx = x + 16.0f;
    const float ly = y + shutter_h + 8.0f + jitter * 1.2f;
    const float lw = w - 32.0f;
    const float lh = h - shutter_h - 24.0f;

    draw_list->AddRectFilled(ImVec2(lx, ly), ImVec2(lx + lw, ly + lh), IM_COL32(246, 241, 226, 255), 3.0f);
    draw_list->AddRect(ImVec2(lx, ly), ImVec2(lx + lw, ly + lh), IM_COL32(160, 154, 138, 255), 3.0f);

    DrawStickerTypography(draw_list, lx, ly, lw, lh, params, seed_hash);
}

void DiskCaddyView::DrawCdJewelCase(
    ImDrawList* draw_list,
    float x,
    float y,
    float w,
    float h,
    const DiskCardRenderParams& params,
    uint32_t seed_hash
) {
    const std::string style = ResolvePhysicalStyle(
        params.media.media_type,
        params.media.physical_style,
        params.media.id.empty() ? params.media.file_path : params.media.id
    );

    // Layer 1: Jewel Case Outer Frame + Ribbed Left Spine
    const float spine_w = std::max(14.0f, w * 0.11f);
    draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + spine_w, y + h), IM_COL32(34, 36, 40, 255), 2.0f);
    for (float ry = y + 4.0f; ry < y + h - 4.0f; ry += 5.0f) {
        draw_list->AddLine(ImVec2(x + 2.0f, ry), ImVec2(x + spine_w - 2.0f, ry), IM_COL32(62, 66, 72, 255), 1.0f);
    }

    // Clear polycarbonate tray body
    draw_list->AddRectFilled(ImVec2(x + spine_w, y), ImVec2(x + w, y + h), IM_COL32(28, 34, 42, 245), 3.0f);
    draw_list->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(196, 212, 228, 200), 3.0f, 0, 1.5f);

    // Layer 2: Optical Disc inside the jewel case
    const float disc_cx = x + spine_w + (w - spine_w) * 0.50f;
    const float disc_cy = y + h * 0.36f;
    const float disc_r = std::min((w - spine_w) * 0.38f, h * 0.28f);

    const ImU32 disc_base = (style == "GOLD_CD")
                                ? IM_COL32(214, 178, 86, 255)
                                : IM_COL32(192, 206, 218, 255);
    const ImU32 disc_sheen = (style == "GOLD_CD")
                                 ? IM_COL32(245, 218, 128, 255)
                                 : IM_COL32(130, 210, 232, 255);

    draw_list->AddCircleFilled(ImVec2(disc_cx, disc_cy), disc_r, disc_base, 36);
    draw_list->AddCircle(ImVec2(disc_cx, disc_cy), disc_r * 0.72f, disc_sheen, 32, 2.0f);
    draw_list->AddCircle(ImVec2(disc_cx, disc_cy), disc_r, IM_COL32(240, 248, 255, 220), 36, 1.2f);

    // Center clamping ring & spindle hole
    draw_list->AddCircleFilled(ImVec2(disc_cx, disc_cy), disc_r * 0.28f, IM_COL32(72, 82, 94, 255), 20);
    draw_list->AddCircleFilled(ImVec2(disc_cx, disc_cy), disc_r * 0.15f, IM_COL32(24, 28, 34, 255), 16);

    // Diagonal polycarbonate specular highlight line
    draw_list->AddLine(
        ImVec2(x + spine_w + 6.0f, y + 6.0f),
        ImVec2(x + w - 10.0f, y + h * 0.45f),
        IM_COL32(255, 255, 255, 45),
        2.0f
    );

    // Layer 3 & 4: Booklet label card at bottom of jewel case
    const float lx = x + spine_w + 6.0f;
    const float ly = y + h * 0.56f;
    const float lw = (w - spine_w) - 12.0f;
    const float lh = h * 0.40f;

    draw_list->AddRectFilled(ImVec2(lx, ly), ImVec2(lx + lw, ly + lh), IM_COL32(244, 242, 234, 255), 3.0f);
    draw_list->AddRect(ImVec2(lx, ly), ImVec2(lx + lw, ly + lh), IM_COL32(140, 150, 160, 255), 3.0f);

    DrawStickerTypography(draw_list, lx, ly, lw, lh, params, seed_hash);

    // Redbook Audio badge on top-right of jewel case if has_redbook_audio
    if (params.media.has_redbook_audio) {
        const float bx = x + w - 54.0f;
        const float by = y + 6.0f;
        draw_list->AddRectFilled(ImVec2(bx, by), ImVec2(bx + 48.0f, by + 18.0f), IM_COL32(212, 154, 28, 240), 3.0f);
        draw_list->AddText(ImVec2(bx + 5.0f, by + 2.0f), IM_COL32(18, 18, 22, 255), "AUDIO");
    }
}

bool DiskCaddyView::RenderMediaCard(const DiskCardRenderParams& params) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!draw_list) {
        return false;
    }

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float w = params.card_width;
    const float h = params.card_height;

    std::string btn_id = "##MediaCard_" + params.target_drive + "_" +
                         std::to_string(params.slot_order) + "_" +
                         (params.media.id.empty() ? params.media.file_path : params.media.id);
    const bool clicked = ImGui::InvisibleButton(btn_id.c_str(), ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused() || params.is_selected_focused;

    // Subtle lift when hovered or focused
    const float lift_y = (hovered || focused) ? -3.0f : 0.0f;
    const float x = cursor.x;
    const float y = cursor.y + lift_y;

    // Drop shadow
    draw_list->AddRectFilled(
        ImVec2(x + 3.0f, y + 4.0f),
        ImVec2(x + w + 3.0f, y + h + 4.0f),
        IM_COL32(0, 0, 0, 120),
        6.0f
    );

    const uint32_t seed_hash = ComputeStringCrc32(
        params.media.id.empty() ? params.media.file_path : params.media.id
    );

    if (params.media.media_type == "FLOPPY_525_360K" || params.media.media_type == "FLOPPY_525_1200K") {
        DrawFloppy525(draw_list, x, y, w, h, params, seed_hash);
    } else if (params.media.media_type == "CDROM_ISO" || params.media.media_type == "CDROM_CUE") {
        DrawCdJewelCase(draw_list, x, y, w, h, params, seed_hash);
    } else {
        DrawFloppy35(draw_list, x, y, w, h, params, seed_hash);
    }

    // Active / Mounted glow border & badge
    if (params.is_mounted_active) {
        draw_list->AddRect(
            ImVec2(x - 2.0f, y - 2.0f),
            ImVec2(x + w + 2.0f, y + h + 2.0f),
            IM_COL32(72, 228, 118, 255),
            6.0f,
            0,
            2.5f
        );
        std::string badge = "IN DRIVE " + params.target_drive + ":";
        draw_list->AddRectFilled(
            ImVec2(x + 6.0f, y + 6.0f),
            ImVec2(x + 88.0f, y + 23.0f),
            IM_COL32(28, 148, 64, 240),
            3.0f
        );
        draw_list->AddText(ImVec2(x + 11.0f, y + 7.0f), IM_COL32(245, 255, 245, 255), badge.c_str());
    } else if (hovered || focused) {
        draw_list->AddRect(
            ImVec2(x - 2.0f, y - 2.0f),
            ImVec2(x + w + 2.0f, y + h + 2.0f),
            IM_COL32(250, 188, 52, 255),
            6.0f,
            0,
            2.0f
        );
    }

    return clicked;
}

#endif // defined(C_KIOSK)
