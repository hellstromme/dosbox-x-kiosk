#include "config.h"

#if defined(C_KIOSK)

#include "image_crawler.h"
#include "../kiosk_db.h"
#include "bios_disk.h"
#include "logging.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#include <direct.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace {

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

int16_t ReadLe16Signed(const uint8_t* p) {
    return static_cast<int16_t>(ReadLe16(p));
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

int32_t ReadLe32Signed(const uint8_t* p) {
    return static_cast<int32_t>(ReadLe32(p));
}

std::string ToUpperAscii(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    }
    return out;
}

std::string TrimAscii(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string GetUpperExtension(const std::string& filename) {
    size_t slash = filename.find_last_of("\\/");
    size_t start = (slash == std::string::npos) ? 0 : (slash + 1);
    size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot <= start) return "";
    return ToUpperAscii(filename.substr(dot));
}

std::string GetUpperStem(const std::string& filename) {
    size_t slash = filename.find_last_of("\\/");
    std::string base = (slash == std::string::npos) ? filename : filename.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        base = base.substr(0, dot);
    }
    return ToUpperAscii(base);
}

void AppendUtf8CodePoint(std::string& out, uint32_t cp) {
    if (cp == 0) return;
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

std::string Utf16LeToUtf8(const std::vector<uint16_t>& units) {
    std::string out;
    out.reserve(units.size());
    for (size_t i = 0; i < units.size(); ++i) {
        uint16_t u = units[i];
        if (u == 0x0000 || u == 0xFFFF) break;
        if (u >= 0xD800 && u <= 0xDBFF && (i + 1) < units.size()) {
            uint16_t low = units[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                uint32_t cp = 0x10000u + ((static_cast<uint32_t>(u - 0xD800u) << 10) | static_cast<uint32_t>(low - 0xDC00u));
                AppendUtf8CodePoint(out, cp);
                ++i;
                continue;
            }
        }
        AppendUtf8CodePoint(out, u);
    }
    return out;
}

uint8_t ComputeFatShortNameChecksum(const uint8_t raw_name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) {
        sum = static_cast<uint8_t>(((sum & 1u) ? 0x80u : 0x00u) + (sum >> 1) + raw_name[i]);
    }
    return sum;
}

std::string FormatShortName83(const uint8_t raw_name[11]) {
    std::string base(reinterpret_cast<const char*>(raw_name), 8);
    std::string ext(reinterpret_cast<const char*>(raw_name + 8), 3);
    while (!base.empty() && base.back() == ' ') base.pop_back();
    while (!ext.empty() && ext.back() == ' ') ext.pop_back();
    if (!ext.empty()) {
        return base + "." + ext;
    }
    return base;
}

bool EnsureParentDirsExist(const std::string& file_path) {
    size_t pos = file_path.find_last_of("\\/");
    if (pos == std::string::npos || pos == 0) return true;
    std::string dir = file_path.substr(0, pos);
    std::string cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        char c = dir[i];
        cur.push_back(c);
        if ((c == '\\' || c == '/') && cur.size() > 1) {
            if (cur.size() == 3 && cur[1] == ':') continue;
#if defined(_WIN32)
            _mkdir(cur.c_str());
#else
            mkdir(cur.c_str(), 0755);
#endif
        }
    }
#if defined(_WIN32)
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
    return true;
}

std::string JoinHostPath(const std::string& dir, const std::string& leaf) {
    if (dir.empty()) return leaf;
    char back = dir.back();
    if (back == '\\' || back == '/') return dir + leaf;
#if defined(_WIN32)
    return dir + "\\" + leaf;
#else
    return dir + "/" + leaf;
#endif
}

std::string GenerateDeterministicItemUuid(
    const std::string& machine_id,
    const std::string& working_dir,
    const std::string& executable
) {
    const std::string seed = machine_id + "|" + ToUpperAscii(working_dir) + "|" + ToUpperAscii(executable);
    const uint32_t h1 = SignatureMatcher::ComputeCrc32(seed.data(), seed.size());
    const std::string seed2 = seed + "|kiosk_scan_v1";
    const uint32_t h2 = SignatureMatcher::ComputeCrc32(seed2.data(), seed2.size());
    char buf[44];
    snprintf(buf, sizeof(buf), "scn-%08x-%04x-4%03x-8%03x-%08x",
                  h1,
                  (h2 >> 16) & 0xFFFFu,
                  (h2 >> 4) & 0x0FFFu,
                  (h1 ^ h2) & 0x0FFFu,
                  h1 ^ (h2 << 1));
    return std::string(buf);
}

std::string NormalizeGuestDir(const std::string& raw_dir) {
    std::string d = TrimAscii(raw_dir);
    for (size_t i = 0; i < d.size(); ++i) {
        if (d[i] == '/') d[i] = '\\';
    }
    while (d.size() > 3 && d.back() == '\\') {
        d.pop_back();
    }
    if (d.empty()) return "C:\\";
    if (d.size() >= 2 && d[1] == ':') {
        d[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(d[0])));
        if (d.size() == 2) d.push_back('\\');
        return d;
    }
    if (d[0] == '\\') return std::string("C:") + d;
    return std::string("C:\\") + d;
}

void SplitCommandLine(const std::string& cmd, std::string& out_exec, std::string& out_args) {
    std::string s = TrimAscii(cmd);
    out_exec.clear();
    out_args.clear();
    if (s.empty()) return;

    if (s[0] == '"') {
        size_t close_q = s.find('"', 1);
        if (close_q != std::string::npos) {
            out_exec = s.substr(1, close_q - 1);
            out_args = TrimAscii(s.substr(close_q + 1));
            return;
        }
    }

    size_t sp = s.find_first_of(" \t");
    if (sp == std::string::npos) {
        out_exec = s;
    } else {
        out_exec = s.substr(0, sp);
        out_args = TrimAscii(s.substr(sp + 1));
    }
}

void SplitGuestPath(const std::string& full_path, std::string& out_dir, std::string& out_file) {
    std::string p = TrimAscii(full_path);
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '/') p[i] = '\\';
    }
    size_t slash = p.find_last_of('\\');
    if (slash == std::string::npos) {
        out_dir = "C:\\";
        out_file = p;
    } else {
        out_dir = NormalizeGuestDir(p.substr(0, slash));
        out_file = p.substr(slash + 1);
    }
}

std::string ReadNullTermAscii(const uint8_t* data, size_t size, size_t offset) {
    if (!data || offset >= size) return "";
    size_t end = offset;
    while (end < size && data[end] != 0 && (end - offset) < 512) {
        ++end;
    }
    return std::string(reinterpret_cast<const char*>(data + offset), end - offset);
}

// Standard 16-color Windows/EGA palette (RGB) for Win 3.1 .GRP embedded icons
static const uint8_t kWin16Palette[16][3] = {
    {0x00, 0x00, 0x00}, {0x80, 0x00, 0x00}, {0x00, 0x80, 0x00}, {0x80, 0x80, 0x00},
    {0x00, 0x00, 0x80}, {0x80, 0x00, 0x80}, {0x00, 0x80, 0x80}, {0xC0, 0xC0, 0xC0},
    {0x80, 0x80, 0x80}, {0xFF, 0x00, 0x00}, {0x00, 0xFF, 0x00}, {0xFF, 0xFF, 0x00},
    {0x00, 0x00, 0xFF}, {0xFF, 0x00, 0xFF}, {0x00, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF}
};

// Decodes a Windows RT_ICON DIB (BITMAPINFOHEADER + optional palette + XOR + AND mask)
// or embedded PNG payload into 32-bit RGBA pixels.
bool DecodeIconPayloadToRgba(
    const uint8_t* payload,
    size_t payload_size,
    int& out_w,
    int& out_h,
    std::vector<uint8_t>& out_rgba,
    bool& out_is_raw_png
) {
    out_w = 0;
    out_h = 0;
    out_rgba.clear();
    out_is_raw_png = false;

    if (!payload || payload_size < 16) return false;

    // Check for PNG signature (Vista+ compressed icon)
    static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(payload, kPngSig, 8) == 0) {
        out_is_raw_png = true;
        return true;
    }

    if (payload_size < 40) return false;
    uint32_t bi_size = ReadLe32(payload + 0);
    if (bi_size < 40 || bi_size > payload_size) return false;

    int32_t bi_width = ReadLe32Signed(payload + 4);
    int32_t bi_height = ReadLe32Signed(payload + 8);
    uint16_t bi_bit_count = ReadLe16(payload + 14);
    uint32_t bi_compression = ReadLe32(payload + 16);
    uint32_t bi_clr_used = ReadLe32(payload + 32);

    if (bi_compression != 0) return false; // Only BI_RGB supported
    if (bi_width <= 0 || bi_width > 256) return false;
    if (bi_height == 0 || bi_height < -512 || bi_height > 512) return false;
    const bool top_down = (bi_height < 0);
    int32_t abs_h = top_down ? -bi_height : bi_height;
    int32_t icon_h = abs_h / 2;
    if (icon_h <= 0 || icon_h > 256) return false;

    if (bi_bit_count != 1 && bi_bit_count != 4 && bi_bit_count != 8 &&
        bi_bit_count != 24 && bi_bit_count != 32) {
        return false;
    }

    size_t palette_entries = 0;
    if (bi_bit_count <= 8) {
        const size_t max_entries = static_cast<size_t>(1) << bi_bit_count;
        palette_entries = (bi_clr_used > 0 && bi_clr_used <= max_entries)
                              ? static_cast<size_t>(bi_clr_used)
                              : max_entries;
    }
    const size_t palette_offset = bi_size;
    const size_t palette_bytes = palette_entries * 4U;
    if (palette_offset > payload_size || palette_bytes > (payload_size - palette_offset)) return false;

    const size_t xor_offset = palette_offset + palette_bytes;
    const size_t xor_stride = ((static_cast<size_t>(bi_width) * bi_bit_count + 31U) / 32U) * 4U;
    const size_t xor_bytes = xor_stride * static_cast<size_t>(icon_h);
    if (xor_bytes > (payload_size - xor_offset)) return false;

    const size_t and_offset = xor_offset + xor_bytes;
    const size_t and_stride = ((static_cast<size_t>(bi_width) + 31U) / 32U) * 4U;
    const size_t and_bytes = and_stride * static_cast<size_t>(icon_h);
    const bool has_and_mask = (and_bytes <= (payload_size - and_offset));

    out_w = static_cast<int>(bi_width);
    out_h = static_cast<int>(icon_h);
    out_rgba.assign(static_cast<size_t>(out_w) * static_cast<size_t>(out_h) * 4U, 0);

    bool any_32bpp_alpha = false;
    if (bi_bit_count == 32) {
        for (int y = 0; y < out_h && !any_32bpp_alpha; ++y) {
            const uint8_t* row = payload + xor_offset + static_cast<size_t>(y) * xor_stride;
            for (int x = 0; x < out_w; ++x) {
                if (row[static_cast<size_t>(x) * 4U + 3U] != 0) {
                    any_32bpp_alpha = true;
                    break;
                }
            }
        }
    }

    for (int y = 0; y < out_h; ++y) {
        const int src_y = top_down ? y : (out_h - 1 - y);
        const uint8_t* xor_row = payload + xor_offset + static_cast<size_t>(src_y) * xor_stride;
        const uint8_t* and_row = has_and_mask
                                     ? (payload + and_offset + static_cast<size_t>(src_y) * and_stride)
                                     : nullptr;

        for (int x = 0; x < out_w; ++x) {
            uint8_t r = 0, g = 0, b = 0, a = 0xFF;

            if (bi_bit_count == 1) {
                uint8_t idx = (xor_row[x >> 3] >> (7 - (x & 7))) & 1u;
                if (idx < palette_entries) {
                    const uint8_t* pe = payload + palette_offset + static_cast<size_t>(idx) * 4U;
                    b = pe[0]; g = pe[1]; r = pe[2];
                }
            } else if (bi_bit_count == 4) {
                uint8_t byte_val = xor_row[x >> 1];
                uint8_t idx = (x & 1) ? (byte_val & 0x0Fu) : ((byte_val >> 4) & 0x0Fu);
                if (idx < palette_entries) {
                    const uint8_t* pe = payload + palette_offset + static_cast<size_t>(idx) * 4U;
                    b = pe[0]; g = pe[1]; r = pe[2];
                }
            } else if (bi_bit_count == 8) {
                uint8_t idx = xor_row[x];
                if (idx < palette_entries) {
                    const uint8_t* pe = payload + palette_offset + static_cast<size_t>(idx) * 4U;
                    b = pe[0]; g = pe[1]; r = pe[2];
                }
            } else if (bi_bit_count == 24) {
                const uint8_t* px = xor_row + static_cast<size_t>(x) * 3U;
                b = px[0]; g = px[1]; r = px[2];
            } else if (bi_bit_count == 32) {
                const uint8_t* px = xor_row + static_cast<size_t>(x) * 4U;
                b = px[0]; g = px[1]; r = px[2];
                if (any_32bpp_alpha) {
                    a = px[3];
                }
            }

            if (has_and_mask && !(bi_bit_count == 32 && any_32bpp_alpha)) {
                uint8_t and_bit = (and_row[x >> 3] >> (7 - (x & 7))) & 1u;
                a = (and_bit != 0) ? 0x00 : 0xFF;
            }

            const size_t dst = (static_cast<size_t>(y) * static_cast<size_t>(out_w) + static_cast<size_t>(x)) * 4U;
            out_rgba[dst + 0] = r;
            out_rgba[dst + 1] = g;
            out_rgba[dst + 2] = b;
            out_rgba[dst + 3] = a;
        }
    }
    return true;
}

bool WriteRawBytesToFile(const std::string& path, const uint8_t* data, size_t size) {
    if (path.empty() || !data || size == 0) return false;
    EnsureParentDirsExist(path);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t written = std::fwrite(data, 1, size, f);
    const bool closed_ok = (std::fclose(f) == 0);
    if (written != size || !closed_ok) {
        std::remove(path.c_str());
        return false;
    }
    return true;
}

struct SelectedIconVariant {
    uint16_t resource_id = 0;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    int score = -1;
};

// Selects the highest-scoring icon variant from a GRPICONDIR resource payload
bool SelectBestGrpIconVariant(
    const uint8_t* grp_data,
    size_t grp_size,
    uint16_t& out_best_nid
) {
    if (!grp_data || grp_size < 6) return false;
    uint16_t reserved = ReadLe16(grp_data + 0);
    uint16_t type = ReadLe16(grp_data + 2);
    uint16_t count = ReadLe16(grp_data + 4);
    if (reserved != 0 || type != 1 || count == 0) return false;

    int best_score = -1;
    uint16_t best_nid = 0;
    bool found = false;

    for (uint16_t i = 0; i < count; ++i) {
        size_t off = 6U + static_cast<size_t>(i) * 14U;
        if (off + 14U > grp_size) break;
        int w = grp_data[off + 0];
        int h = grp_data[off + 1];
        if (w == 0) w = 256;
        if (h == 0) h = 256;
        uint16_t bpp = ReadLe16(grp_data + off + 6);
        if (bpp == 0) {
            uint8_t cc = grp_data[off + 2];
            bpp = (cc == 2) ? 1 : ((cc == 16) ? 4 : 8);
        }
        uint16_t nid = ReadLe16(grp_data + off + 12);
        int score = (w * h) + (static_cast<int>(bpp) * 100);
        if (!found || score > best_score) {
            best_score = score;
            best_nid = nid;
            found = true;
        }
    }

    if (found) {
        out_best_nid = best_nid;
    }
    return found;
}

bool ExtractFromStandaloneIco(const uint8_t* data, size_t size, const std::string& out_png_path) {
    if (!data || size < 6) return false;
    uint16_t reserved = ReadLe16(data + 0);
    uint16_t type = ReadLe16(data + 2);
    uint16_t count = ReadLe16(data + 4);
    if (reserved != 0 || type != 1 || count == 0 || count > 256) return false;

    SelectedIconVariant best;
    for (uint16_t i = 0; i < count; ++i) {
        size_t off = 6U + static_cast<size_t>(i) * 16U;
        if (off + 16U > size) break;
        int w = data[off + 0];
        int h = data[off + 1];
        if (w == 0) w = 256;
        if (h == 0) h = 256;
        uint16_t bpp = ReadLe16(data + off + 6);
        if (bpp == 0) {
            uint8_t cc = data[off + 2];
            bpp = (cc == 2) ? 1 : ((cc == 16) ? 4 : 8);
        }
        uint32_t bytes_in_res = ReadLe32(data + off + 8);
        uint32_t img_off = ReadLe32(data + off + 12);
        if (img_off >= size || bytes_in_res == 0 || static_cast<uint64_t>(img_off) + bytes_in_res > size) {
            continue;
        }
        int score = (w * h) + (static_cast<int>(bpp) * 100);
        if (score > best.score) {
            best.score = score;
            best.data_offset = img_off;
            best.data_size = bytes_in_res;
        }
    }

    if (best.score < 0) return false;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool is_png = false;
    if (!DecodeIconPayloadToRgba(data + best.data_offset, best.data_size, w, h, rgba, is_png)) {
        return false;
    }
    if (is_png) {
        return WriteRawBytesToFile(out_png_path, data + best.data_offset, best.data_size);
    }
    return ImageCrawler::WriteRgbaPng(out_png_path, w, h, rgba);
}

bool ExtractFromNeBinary(
    const uint8_t* data,
    size_t size,
    size_t lfanew,
    const std::string& out_png_path,
    int preferred_icon_index
) {
    if (lfanew > size || (size - lfanew) < 0x40) return false;
    uint16_t rsrc_tab_rel = ReadLe16(data + lfanew + 0x24);
    if (rsrc_tab_rel > size - lfanew) return false;
    size_t rsrc_tab = lfanew + rsrc_tab_rel;
    if ((size - rsrc_tab) < 2) return false;

    uint16_t align_shift = ReadLe16(data + rsrc_tab);
    if (align_shift > 16) return false;

    struct NeResItem {
        uint16_t id;
        uint32_t file_off;
        uint32_t byte_len;
    };
    std::vector<NeResItem> group_icons;
    std::vector<NeResItem> raw_icons;

    size_t pos = rsrc_tab + 2;
    while ((size - pos) >= 2) {
        uint16_t type_id = ReadLe16(data + pos);
        if (type_id == 0) break;
        if ((size - pos) < 8) break;
        uint16_t count = ReadLe16(data + pos + 2);
        pos += 8;

        for (uint16_t i = 0; i < count; ++i) {
            if ((size - pos) < 12) return false;
            uint16_t rn_offset = ReadLe16(data + pos + 0);
            uint16_t rn_length = ReadLe16(data + pos + 2);
            uint16_t rn_id = ReadLe16(data + pos + 6);
            pos += 12;

            uint32_t abs_off = static_cast<uint32_t>(rn_offset) << align_shift;
            uint32_t abs_len = static_cast<uint32_t>(rn_length) << align_shift;
            if (static_cast<uint64_t>(abs_off) + abs_len > size) continue;

            NeResItem item;
            item.id = rn_id;
            item.file_off = abs_off;
            item.byte_len = abs_len;
            if (type_id == 0x800Eu) {
                group_icons.push_back(item);
            } else if (type_id == 0x8003u) {
                raw_icons.push_back(item);
            }
        }
    }

    if (raw_icons.empty()) return false;

    uint16_t target_nid = 0;
    bool have_target = false;
    if (!group_icons.empty()) {
        size_t grp_idx = (preferred_icon_index >= 0 && static_cast<size_t>(preferred_icon_index) < group_icons.size())
                             ? static_cast<size_t>(preferred_icon_index)
                             : 0;
        const NeResItem& grp = group_icons[grp_idx];
        have_target = SelectBestGrpIconVariant(data + grp.file_off, grp.byte_len, target_nid);
    }

    const NeResItem* chosen = nullptr;
    if (have_target) {
        for (size_t i = 0; i < raw_icons.size(); ++i) {
            if (raw_icons[i].id == static_cast<uint16_t>(0x8000u | target_nid) ||
                raw_icons[i].id == target_nid) {
                chosen = &raw_icons[i];
                break;
            }
        }
    }
    if (!chosen) {
        chosen = &raw_icons[0];
    }

    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool is_png = false;
    if (!DecodeIconPayloadToRgba(data + chosen->file_off, chosen->byte_len, w, h, rgba, is_png)) {
        return false;
    }
    if (is_png) {
        return WriteRawBytesToFile(out_png_path, data + chosen->file_off, chosen->byte_len);
    }
    return ImageCrawler::WriteRgbaPng(out_png_path, w, h, rgba);
}

struct PeSectionRange {
    uint32_t virtual_addr;
    uint32_t virtual_size;
    uint32_t raw_ptr;
    uint32_t raw_size;
};

bool PeRvaToFileOffset(const std::vector<PeSectionRange>& sections, uint32_t rva, size_t file_size, size_t& out_off) {
    for (size_t i = 0; i < sections.size(); ++i) {
        const PeSectionRange& s = sections[i];
        uint32_t span = (std::max)(s.virtual_size, s.raw_size);
        if (rva >= s.virtual_addr && (rva - s.virtual_addr) < span) {
            uint64_t off = static_cast<uint64_t>(s.raw_ptr) + static_cast<uint64_t>(rva - s.virtual_addr);
            if (off < file_size) {
                out_off = static_cast<size_t>(off);
                return true;
            }
        }
    }
    return false;
}

struct PeResourceEntry {
    uint32_t name_or_id = 0;
    bool is_dir = false;
    uint32_t offset = 0; // relative to resource root
};

bool ReadPeResourceDir(
    const uint8_t* data,
    size_t size,
    size_t rsrc_root_off,
    uint32_t rel_dir_off,
    std::vector<PeResourceEntry>& out_entries
) {
    out_entries.clear();
    if (rsrc_root_off > size || static_cast<uint64_t>(rel_dir_off) > (size - rsrc_root_off)) return false;
    const size_t dir_off = rsrc_root_off + static_cast<size_t>(rel_dir_off);
    if ((size - dir_off) < 16) return false;
    const uint8_t* dir = data + dir_off;
    uint16_t num_named = ReadLe16(dir + 12);
    uint16_t num_id = ReadLe16(dir + 14);
    uint32_t total = static_cast<uint32_t>(num_named) + static_cast<uint32_t>(num_id);
    if (total > 2048) total = 2048;

    const size_t entries_off = dir_off + 16;
    const size_t avail_bytes = size - entries_off;
    for (uint32_t i = 0; i < total; ++i) {
        const size_t rel_e = static_cast<size_t>(i) * 8U;
        if (rel_e > avail_bytes || (avail_bytes - rel_e) < 8U) break;
        const uint8_t* e = data + entries_off + rel_e;
        PeResourceEntry pe;
        pe.name_or_id = ReadLe32(e + 0);
        uint32_t otd = ReadLe32(e + 4);
        pe.is_dir = (otd & 0x80000000u) != 0;
        pe.offset = otd & 0x7FFFFFFFu;
        out_entries.push_back(pe);
    }
    return !out_entries.empty();
}

bool ResolvePeResourceDataLeaf(
    const uint8_t* data,
    size_t size,
    size_t rsrc_root_off,
    const std::vector<PeSectionRange>& sections,
    const PeResourceEntry& name_entry,
    size_t& out_file_off,
    size_t& out_data_size
) {
    uint32_t data_entry_rel = name_entry.offset;
    if (name_entry.is_dir) {
        std::vector<PeResourceEntry> langs;
        if (!ReadPeResourceDir(data, size, rsrc_root_off, name_entry.offset, langs) || langs.empty()) {
            return false;
        }
        if (langs[0].is_dir) return false;
        data_entry_rel = langs[0].offset;
    }

    if (rsrc_root_off > size || static_cast<uint64_t>(data_entry_rel) > (size - rsrc_root_off)) return false;
    const size_t leaf_off = rsrc_root_off + static_cast<size_t>(data_entry_rel);
    if ((size - leaf_off) < 16) return false;
    const uint8_t* leaf = data + leaf_off;
    uint32_t data_rva = ReadLe32(leaf + 0);
    uint32_t data_sz = ReadLe32(leaf + 4);
    if (data_sz == 0) return false;

    size_t file_off = 0;
    if (!PeRvaToFileOffset(sections, data_rva, size, file_off)) return false;
    if (static_cast<uint64_t>(data_sz) > (size - file_off)) return false;

    out_file_off = file_off;
    out_data_size = data_sz;
    return true;
}

bool ExtractFromPeBinary(
    const uint8_t* data,
    size_t size,
    size_t lfanew,
    const std::string& out_png_path,
    int preferred_icon_index
) {
    if (lfanew > size || (size - lfanew) < 24) return false;
    uint16_t num_sections = ReadLe16(data + lfanew + 6);
    uint16_t opt_hdr_size = ReadLe16(data + lfanew + 20);
    size_t opt_off = lfanew + 24;
    if (opt_hdr_size < 96 || opt_hdr_size > (size - opt_off)) return false;

    uint16_t magic = ReadLe16(data + opt_off);
    size_t dd_rel = 0;
    if (magic == 0x010Bu) {
        dd_rel = 96;  // PE32
    } else if (magic == 0x020Bu) {
        dd_rel = 112; // PE32+
    } else {
        return false;
    }

    // Data Directory 2 = IMAGE_DIRECTORY_ENTRY_RESOURCE
    if (dd_rel > opt_hdr_size || (opt_hdr_size - dd_rel) < 24) return false;
    const size_t dd_off = opt_off + dd_rel;
    uint32_t rsrc_rva = ReadLe32(data + dd_off + 16);
    uint32_t rsrc_size = ReadLe32(data + dd_off + 20);
    if (rsrc_rva == 0 || rsrc_size == 0) return false;

    std::vector<PeSectionRange> sections;
    size_t sec_off = opt_off + opt_hdr_size;
    for (uint16_t i = 0; i < num_sections; ++i) {
        if ((size - sec_off) < 40) break;
        PeSectionRange sr;
        sr.virtual_size = ReadLe32(data + sec_off + 8);
        sr.virtual_addr = ReadLe32(data + sec_off + 12);
        sr.raw_size = ReadLe32(data + sec_off + 16);
        sr.raw_ptr = ReadLe32(data + sec_off + 20);
        sections.push_back(sr);
        sec_off += 40;
    }

    size_t rsrc_root_off = 0;
    if (!PeRvaToFileOffset(sections, rsrc_rva, size, rsrc_root_off)) return false;

    std::vector<PeResourceEntry> types;
    if (!ReadPeResourceDir(data, size, rsrc_root_off, 0, types)) return false;

    const PeResourceEntry* group_type = nullptr;
    const PeResourceEntry* icon_type = nullptr;
    for (size_t i = 0; i < types.size(); ++i) {
        if (!types[i].is_dir) continue;
        if (types[i].name_or_id == 14u) group_type = &types[i];
        if (types[i].name_or_id == 3u) icon_type = &types[i];
    }
    if (!icon_type) return false;

    std::vector<PeResourceEntry> icon_names;
    if (!ReadPeResourceDir(data, size, rsrc_root_off, icon_type->offset, icon_names) || icon_names.empty()) {
        return false;
    }

    uint16_t target_nid = 0;
    bool have_target = false;
    if (group_type) {
        std::vector<PeResourceEntry> grp_names;
        if (ReadPeResourceDir(data, size, rsrc_root_off, group_type->offset, grp_names) && !grp_names.empty()) {
            size_t grp_idx = (preferred_icon_index >= 0 && static_cast<size_t>(preferred_icon_index) < grp_names.size())
                                 ? static_cast<size_t>(preferred_icon_index)
                                 : 0;
            size_t grp_file_off = 0, grp_data_sz = 0;
            if (ResolvePeResourceDataLeaf(data, size, rsrc_root_off, sections, grp_names[grp_idx], grp_file_off, grp_data_sz)) {
                have_target = SelectBestGrpIconVariant(data + grp_file_off, grp_data_sz, target_nid);
            }
        }
    }

    const PeResourceEntry* chosen_icon = nullptr;
    if (have_target) {
        for (size_t i = 0; i < icon_names.size(); ++i) {
            if (icon_names[i].name_or_id == target_nid) {
                chosen_icon = &icon_names[i];
                break;
            }
        }
    }
    if (!chosen_icon) {
        chosen_icon = &icon_names[0];
    }

    size_t icon_file_off = 0, icon_data_sz = 0;
    if (!ResolvePeResourceDataLeaf(data, size, rsrc_root_off, sections, *chosen_icon, icon_file_off, icon_data_sz)) {
        return false;
    }

    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool is_png = false;
    if (!DecodeIconPayloadToRgba(data + icon_file_off, icon_data_sz, w, h, rgba, is_png)) {
        return false;
    }
    if (is_png) {
        return WriteRawBytesToFile(out_png_path, data + icon_file_off, icon_data_sz);
    }
    return ImageCrawler::WriteRgbaPng(out_png_path, w, h, rgba);
}

bool BatReferencesSiblingExe(
    const std::vector<uint8_t>& bat_bytes,
    const std::vector<std::string>& sibling_exes
) {
    if (bat_bytes.empty() || sibling_exes.empty()) return false;
    const size_t scan_len = (std::min)(bat_bytes.size(), static_cast<size_t>(8192));
    std::string text(reinterpret_cast<const char*>(bat_bytes.data()), scan_len);
    std::string upper_text = ToUpperAscii(text);

    for (size_t i = 0; i < sibling_exes.size(); ++i) {
        const std::string stem = GetUpperStem(sibling_exes[i]);
        if (stem.size() < 2) continue;
        size_t pos = upper_text.find(stem);
        while (pos != std::string::npos) {
            bool left_ok = (pos == 0 || !std::isalnum(static_cast<unsigned char>(upper_text[pos - 1])));
            size_t end_pos = pos + stem.size();
            bool right_ok = (end_pos >= upper_text.size() ||
                             !std::isalnum(static_cast<unsigned char>(upper_text[end_pos])));
            if (left_ok && right_ok) {
                return true;
            }
            pos = upper_text.find(stem, pos + 1);
        }
    }
    return false;
}

} // anonymous namespace

// ============================================================================
// FatReadOnlyVfs Implementation
// ============================================================================

FatReadOnlyVfs::~FatReadOnlyVfs() {
    Close();
}

void FatReadOnlyVfs::Close() {
    if (m_disk && m_owns_disk) {
        delete m_disk;
    }
    m_disk = nullptr;
    m_owns_disk = false;
    m_mounted = false;
}

bool FatReadOnlyVfs::OpenImageFile(const std::string& image_path) {
    Close();
    if (image_path.empty()) return false;

    const std::string ext = GetUpperExtension(image_path);
    if (ext == ".VHD") {
        imageDisk* vhd = nullptr;
        if (imageDiskVHD::Open(image_path.c_str(), /*readOnly=*/true, &vhd) != imageDiskVHD::OPEN_SUCCESS || !vhd) {
            return false;
        }
        vhd->kiosk_write_protected = true;
        vhd->kiosk_host_readonly = true;
        m_disk = vhd;
        m_owns_disk = true;
        m_write_protected = true;
        if (!MountPartition()) {
            Close();
            return false;
        }
        return true;
    }

    std::FILE* f = std::fopen(image_path.c_str(), "rb");
    if (!f) return false;

#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    __int64 file_size_signed = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    if (fseeko(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    off_t file_size_signed = ftello(f);
    fseeko(f, 0, SEEK_SET);
#endif

    if (file_size_signed < 512) {
        std::fclose(f);
        return false;
    }

    uint64_t file_size = static_cast<uint64_t>(file_size_signed);
    bool is_hdd = (file_size > 2949120ULL); // > 2.88MB floppy
    imageDisk* disk = new imageDisk(f, image_path.c_str(), file_size, is_hdd);
    disk->kiosk_write_protected = true;
    disk->kiosk_host_readonly = true;

    m_disk = disk;
    m_owns_disk = true;
    m_write_protected = true;
    if (!MountPartition()) {
        Close();
        return false;
    }
    return true;
}

bool FatReadOnlyVfs::OpenFromImageDisk(imageDisk* non_owning_disk) {
    Close();
    if (!non_owning_disk) return false;
    m_disk = non_owning_disk;
    m_owns_disk = false;
    m_write_protected = true;
    if (!MountPartition()) {
        Close();
        return false;
    }
    return true;
}

bool FatReadOnlyVfs::ReadSector(uint32_t abs_lba, uint8_t out_sector[512]) const {
    if (!m_disk || m_disk->getSectSize() != 512) return false;
    return m_disk->Read_AbsoluteSector(abs_lba, out_sector) == Int13Status::NoError;
}

bool FatReadOnlyVfs::MountPartition() {
    if (!m_disk || m_disk->getSectSize() != 512) return false;
    uint8_t sec0[512];
    if (!ReadSector(0, sec0)) return false;

    // 1. Check MBR partition table at 0x1BE if boot signature 0x55AA is present
    if (sec0[510] == 0x55 && sec0[511] == 0xAA) {
        uint32_t best_lba = 0;
        uint32_t best_lba_count = 0;
        bool best_active = false;
        for (int i = 0; i < 4; ++i) {
            const uint8_t* p = sec0 + 0x1BE + (i * 16);
            uint8_t boot_flag = p[0];
            uint8_t ptype = p[4];
            uint32_t lba_start = ReadLe32(p + 8);
            uint32_t lba_count = ReadLe32(p + 12);

            if (boot_flag != 0x00 && boot_flag != 0x80) continue;
            bool is_fat_type = (ptype == 0x01 || ptype == 0x04 || ptype == 0x06 ||
                                ptype == 0x0B || ptype == 0x0C || ptype == 0x0E);
            if (!is_fat_type || lba_start == 0 || lba_count == 0) continue;

            if (best_lba == 0 || (boot_flag == 0x80 && !best_active)) {
                best_lba = lba_start;
                best_lba_count = lba_count;
                best_active = (boot_flag == 0x80);
            }
        }
        if (best_lba > 0) {
            if (ParseBpb(best_lba, best_lba_count)) {
                m_mounted = true;
                return true;
            }
            return false;
        }
    }

    // 2. Fallback: superfloppy / unpartitioned FAT volume starting at LBA 0
    if (ParseBpb(0, 0)) {
        m_mounted = true;
        return true;
    }

    return false;
}

bool FatReadOnlyVfs::ParseBpb(uint32_t part_lba, uint32_t max_part_sectors) {
    uint8_t vbr[512];
    if (!ReadSector(part_lba, vbr)) return false;
    if (vbr[510] != 0x55 || vbr[511] != 0xAA) return false;
    if (!((vbr[0] == 0xEB && vbr[2] == 0x90) || vbr[0] == 0xE9)) return false;

    uint16_t byts_per_sec = ReadLe16(vbr + 11);
    uint8_t sec_per_clus = vbr[13];
    uint16_t rsvd_sec = ReadLe16(vbr + 14);
    uint8_t num_fats = vbr[16];
    uint16_t root_ent_cnt = ReadLe16(vbr + 17);
    uint32_t tot_sec = ReadLe16(vbr + 19);
    if (tot_sec == 0) tot_sec = ReadLe32(vbr + 32);
    uint32_t fat_sz = ReadLe16(vbr + 22);
    if (fat_sz == 0) fat_sz = ReadLe32(vbr + 36);

    if (byts_per_sec != 512) return false;
    if (sec_per_clus == 0 || (sec_per_clus & (sec_per_clus - 1)) != 0) return false;
    if (rsvd_sec == 0 || num_fats == 0 || num_fats > 4) return false;
    if (tot_sec == 0 || fat_sz == 0) return false;
    if (max_part_sectors > 0 && tot_sec > max_part_sectors) return false;
    if (static_cast<uint64_t>(part_lba) + static_cast<uint64_t>(tot_sec) > 0xFFFFFFFFULL) return false;

    uint32_t root_dir_secs = ((static_cast<uint32_t>(root_ent_cnt) * 32u) + (byts_per_sec - 1u)) / byts_per_sec;
    uint64_t first_data = static_cast<uint64_t>(rsvd_sec) +
                          (static_cast<uint64_t>(num_fats) * fat_sz) +
                          root_dir_secs;
    if (first_data >= tot_sec) return false;

    uint32_t data_secs = tot_sec - static_cast<uint32_t>(first_data);
    uint32_t clus_cnt = data_secs / sec_per_clus;
    if (clus_cnt == 0) return false;

    FilesystemType fs = FilesystemType::FAT16;
    uint32_t root_clus = 0;
    uint32_t active_fat = 0;
    if (clus_cnt < 4085u) {
        fs = FilesystemType::FAT12;
    } else if (clus_cnt < 65525u) {
        fs = FilesystemType::FAT16;
    } else {
        fs = FilesystemType::FAT32;
        uint16_t ext_flags = ReadLe16(vbr + 40);
        if (ext_flags & 0x0080u) {
            active_fat = static_cast<uint32_t>(ext_flags & 0x000Fu);
            if (active_fat >= num_fats) return false;
        }
        root_clus = ReadLe32(vbr + 44) & 0x0FFFFFFFu;
        if (root_clus < 2 || (root_clus - 2u) >= clus_cnt) return false;
    }

    m_part_lba = part_lba;
    m_bytes_per_sector = byts_per_sec;
    m_sectors_per_cluster = sec_per_clus;
    m_reserved_sectors = rsvd_sec;
    m_num_fats = num_fats;
    m_root_entry_count = root_ent_cnt;
    m_total_sectors = tot_sec;
    m_fat_size_sectors = fat_sz;
    m_root_dir_sectors = root_dir_secs;
    m_first_data_sector = static_cast<uint32_t>(first_data);
    m_cluster_count = clus_cnt;
    m_fs_type = fs;
    m_root_cluster = root_clus;
    m_active_fat_index = active_fat;
    return true;
}

uint32_t FatReadOnlyVfs::ClusterToLba(uint32_t cluster) const {
    return m_part_lba + m_first_data_sector + ((cluster - 2u) * m_sectors_per_cluster);
}

bool FatReadOnlyVfs::IsEndOfChain(uint32_t cluster) const {
    if (cluster < 2) return true;
    if (m_fs_type == FilesystemType::FAT12) return cluster >= 0xFF8u;
    if (m_fs_type == FilesystemType::FAT16) return cluster >= 0xFFF8u;
    return (cluster & 0x0FFFFFFFu) >= 0x0FFFFFF8u;
}

bool FatReadOnlyVfs::GetNextCluster(uint32_t cluster, uint32_t& out_next) const {
    if (!m_mounted || cluster < 2 || (cluster - 2u) >= m_cluster_count) {
        return false;
    }

    const uint32_t fat_lba = m_part_lba + m_reserved_sectors + (m_active_fat_index * m_fat_size_sectors);
    uint8_t sec[512];

    if (m_fs_type == FilesystemType::FAT12) {
        uint64_t byte_off = static_cast<uint64_t>(cluster) + (static_cast<uint64_t>(cluster) / 2ULL);
        uint64_t sec_idx = byte_off / 512ULL;
        uint32_t in_sec = static_cast<uint32_t>(byte_off % 512ULL);
        if (sec_idx >= m_fat_size_sectors || !ReadSector(fat_lba + static_cast<uint32_t>(sec_idx), sec)) {
            return false;
        }
        uint16_t raw = 0;
        if (in_sec == 511u) {
            uint8_t sec2[512];
            if (sec_idx + 1ULL >= m_fat_size_sectors || !ReadSector(fat_lba + static_cast<uint32_t>(sec_idx + 1ULL), sec2)) {
                return false;
            }
            raw = static_cast<uint16_t>(sec[511]) | (static_cast<uint16_t>(sec2[0]) << 8);
        } else {
            raw = ReadLe16(sec + in_sec);
        }
        out_next = (cluster & 1u) ? ((raw >> 4) & 0x0FFFu) : (raw & 0x0FFFu);
        return true;
    }

    if (m_fs_type == FilesystemType::FAT16) {
        uint64_t byte_off = static_cast<uint64_t>(cluster) * 2ULL;
        uint64_t sec_idx = byte_off / 512ULL;
        uint32_t in_sec = static_cast<uint32_t>(byte_off % 512ULL);
        if (sec_idx >= m_fat_size_sectors || !ReadSector(fat_lba + static_cast<uint32_t>(sec_idx), sec)) {
            return false;
        }
        out_next = ReadLe16(sec + in_sec);
        return true;
    }

    // FAT32
    uint64_t byte_off = static_cast<uint64_t>(cluster) * 4ULL;
    uint64_t sec_idx = byte_off / 512ULL;
    uint32_t in_sec = static_cast<uint32_t>(byte_off % 512ULL);
    if (sec_idx >= m_fat_size_sectors || !ReadSector(fat_lba + static_cast<uint32_t>(sec_idx), sec)) {
        return false;
    }
    out_next = ReadLe32(sec + in_sec) & 0x0FFFFFFFu;
    return true;
}

void FatReadOnlyVfs::ParseDirectoryBuffer(
    const std::vector<uint8_t>& buf,
    std::vector<FatDirEntry>& out_entries
) const {
    out_entries.clear();

    // Pending VFAT LFN assembly state
    bool lfn_active = false;
    uint8_t lfn_checksum = 0;
    uint8_t lfn_expected_ord = 0;
    std::vector<std::vector<uint16_t>> lfn_chunks; // indexed by (ord - 1)

    for (size_t off = 0; off + 32 <= buf.size(); off += 32) {
        const uint8_t* e = buf.data() + off;
        if (e[0] == 0x00) {
            break; // End of directory
        }
        if (e[0] == 0xE5) {
            lfn_active = false;
            lfn_chunks.clear();
            continue;
        }

        uint8_t attr = e[11];
        if ((attr & 0x3Fu) == 0x0Fu) {
            // VFAT Long Filename entry
            uint8_t ord = e[0];
            uint8_t seq = ord & 0x1Fu;
            bool is_last = (ord & 0x40u) != 0;
            uint8_t csum = e[13];

            if (seq == 0 || seq > 20) {
                lfn_active = false;
                lfn_chunks.clear();
                continue;
            }
            if (is_last) {
                lfn_chunks.assign(seq, std::vector<uint16_t>());
                lfn_checksum = csum;
                lfn_expected_ord = seq;
                lfn_active = true;
            } else if (!lfn_active || csum != lfn_checksum || seq != lfn_expected_ord) {
                lfn_active = false;
                lfn_chunks.clear();
                continue;
            }

            std::vector<uint16_t> chars(13, 0);
            for (int k = 0; k < 5; ++k)  chars[k]      = ReadLe16(e + 1 + k * 2);
            for (int k = 0; k < 6; ++k)  chars[5 + k]  = ReadLe16(e + 14 + k * 2);
            for (int k = 0; k < 2; ++k)  chars[11 + k] = ReadLe16(e + 28 + k * 2);
            lfn_chunks[seq - 1] = chars;
            lfn_expected_ord = static_cast<uint8_t>(seq - 1);
            continue;
        }

        // Skip volume label entries
        if (attr & 0x08u) {
            lfn_active = false;
            lfn_chunks.clear();
            continue;
        }

        std::string short_name = FormatShortName83(e);
        std::string full_name = short_name;

        if (lfn_active && lfn_expected_ord == 0 && ComputeFatShortNameChecksum(e) == lfn_checksum) {
            std::vector<uint16_t> combined;
            for (size_t c = 0; c < lfn_chunks.size(); ++c) {
                combined.insert(combined.end(), lfn_chunks[c].begin(), lfn_chunks[c].end());
            }
            std::string decoded = Utf16LeToUtf8(combined);
            if (!decoded.empty()) {
                full_name = decoded;
            }
        }
        lfn_active = false;
        lfn_chunks.clear();

        if (short_name == "." || short_name == "..") {
            continue;
        }

        FatDirEntry entry;
        entry.name = full_name;
        entry.short_name = ToUpperAscii(short_name);
        entry.is_directory = (attr & 0x10u) != 0;
        entry.attributes = attr;
        uint32_t hi = (m_fs_type == FilesystemType::FAT32) ? static_cast<uint32_t>(ReadLe16(e + 20)) : 0u;
        uint32_t lo = static_cast<uint32_t>(ReadLe16(e + 26));
        uint32_t raw_clus = (hi << 16) | lo;
        entry.first_cluster = (m_fs_type == FilesystemType::FAT32) ? (raw_clus & 0x0FFFFFFFu) : raw_clus;
        entry.file_size = ReadLe32(e + 28);
        out_entries.push_back(entry);
    }
}

bool FatReadOnlyVfs::ListDirectory(const FatDirEntry* dir, std::vector<FatDirEntry>& out_entries) const {
    out_entries.clear();
    if (!m_mounted) return false;

    std::vector<uint8_t> raw_dir;
    uint8_t sec[512];

    if (dir == nullptr && m_fs_type != FilesystemType::FAT32) {
        // Fixed root directory region for FAT12 / FAT16
        uint32_t root_lba = m_part_lba + m_reserved_sectors + (m_num_fats * m_fat_size_sectors);
        raw_dir.reserve(static_cast<size_t>(m_root_dir_sectors) * 512U);
        for (uint32_t s = 0; s < m_root_dir_sectors; ++s) {
            if (!ReadSector(root_lba + s, sec)) return false;
            raw_dir.insert(raw_dir.end(), sec, sec + 512);
        }
        ParseDirectoryBuffer(raw_dir, out_entries);
        return true;
    }

    uint32_t cur_cluster = (dir == nullptr) ? m_root_cluster : dir->first_cluster;
    if (cur_cluster < 2 || (cur_cluster - 2u) >= m_cluster_count) return false;

    std::unordered_set<uint32_t> visited;
    const size_t kMaxDirClusters = 2048;
    while (!IsEndOfChain(cur_cluster) && visited.size() < kMaxDirClusters) {
        if ((cur_cluster - 2u) >= m_cluster_count) return false;
        if (!visited.insert(cur_cluster).second) {
            return false; // Cycle protection
        }
        uint32_t lba = ClusterToLba(cur_cluster);
        for (uint32_t s = 0; s < m_sectors_per_cluster; ++s) {
            if (!ReadSector(lba + s, sec)) return false;
            raw_dir.insert(raw_dir.end(), sec, sec + 512);
        }
        uint32_t next_cluster = 0;
        if (!GetNextCluster(cur_cluster, next_cluster)) {
            return false; // FAT read error
        }
        cur_cluster = next_cluster;
    }

    ParseDirectoryBuffer(raw_dir, out_entries);
    return true;
}

bool FatReadOnlyVfs::FindPath(const std::string& guest_path, FatDirEntry& out_entry) const {
    if (!m_mounted || guest_path.empty()) return false;

    std::string p = TrimAscii(guest_path);
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '/') p[i] = '\\';
    }
    if (p.size() >= 2 && p[1] == ':') {
        const char drv = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
        if (drv != 'C') {
            return false; // Only drive-less or C: paths belong to the mounted primary HDD
        }
        p = p.substr(2);
    }

    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < p.size()) {
        while (pos < p.size() && p[pos] == '\\') ++pos;
        if (pos >= p.size()) break;
        size_t next = p.find('\\', pos);
        if (next == std::string::npos) {
            parts.push_back(p.substr(pos));
            break;
        }
        parts.push_back(p.substr(pos, next - pos));
        pos = next + 1;
    }

    if (parts.empty()) return false;

    FatDirEntry cur;
    bool has_cur = false;
    for (size_t i = 0; i < parts.size(); ++i) {
        std::vector<FatDirEntry> entries;
        if (!ListDirectory(has_cur ? &cur : nullptr, entries)) {
            return false;
        }
        const std::string target_upper = ToUpperAscii(parts[i]);
        bool matched = false;
        for (size_t j = 0; j < entries.size(); ++j) {
            if (ToUpperAscii(entries[j].name) == target_upper ||
                entries[j].short_name == target_upper) {
                cur = entries[j];
                has_cur = true;
                matched = true;
                break;
            }
        }
        if (!matched) return false;
        if (i + 1 < parts.size() && !cur.is_directory) {
            return false;
        }
    }

    out_entry = cur;
    return has_cur;
}

bool FatReadOnlyVfs::ReadFile(
    const FatDirEntry& entry,
    std::vector<uint8_t>& out_data,
    size_t max_bytes
) const {
    out_data.clear();
    if (!m_mounted || entry.is_directory) return false;
    if (entry.file_size == 0) return true;
    if (entry.first_cluster < 2) return false;

    const size_t target_bytes = (std::min)(static_cast<size_t>(entry.file_size), max_bytes);
    out_data.reserve(target_bytes);

    std::unordered_set<uint32_t> visited;
    uint32_t cur_cluster = entry.first_cluster;
    uint8_t sec[512];

    while (!IsEndOfChain(cur_cluster) && out_data.size() < target_bytes) {
        if ((cur_cluster - 2u) >= m_cluster_count) break;
        if (!visited.insert(cur_cluster).second) {
            break; // Cycle protection
        }
        uint32_t lba = ClusterToLba(cur_cluster);
        for (uint32_t s = 0; s < m_sectors_per_cluster && out_data.size() < target_bytes; ++s) {
            if (!ReadSector(lba + s, sec)) return false;
            size_t need = target_bytes - out_data.size();
            size_t take = (std::min)(static_cast<size_t>(512), need);
            out_data.insert(out_data.end(), sec, sec + take);
        }
        if (out_data.size() >= target_bytes) {
            break; // Satisfied capped or full read without needing next FAT lookup
        }
        uint32_t next_cluster = 0;
        if (!GetNextCluster(cur_cluster, next_cluster)) {
            return false;
        }
        cur_cluster = next_cluster;
    }

    return out_data.size() == target_bytes;
}

// ============================================================================
// ImageCrawler Implementation
// ============================================================================

bool ImageCrawler::IsPrunedSystemDirectory(const std::string& normalized_dir_path) {
    std::string p = ToUpperAscii(normalized_dir_path);
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '/') p[i] = '\\';
    }
    if (p.size() >= 2 && p[1] == ':') {
        p = p.substr(2);
    }
    if (p.empty() || p[0] != '\\') {
        p = "\\" + p;
    }

    static const char* kPrunedPrefixes[] = {
        "\\DOS",
        "\\FDOS",
        "\\FREEDOS",
        "\\ULTRASND",
        "\\SB16",
        "\\MT32",
        "\\MOUSE",
        "\\NET",
        "\\TEMP",
        "\\TMP",
        "\\RECYCLED",
        "\\DRIVERS",
        "\\WINDOWS\\SYSTEM",
        "\\WINDOWS\\SYSTEM32",
        "\\WINDOWS\\INF",
        "\\WINDOWS\\FONTS",
        "\\WINDOWS\\HELP",
        "\\WINDOWS\\TEMP",
        "\\WINDOWS\\COMMAND"
    };

    for (size_t i = 0; i < sizeof(kPrunedPrefixes) / sizeof(kPrunedPrefixes[0]); ++i) {
        const std::string pref(kPrunedPrefixes[i]);
        if (p == pref || (p.size() > pref.size() && p.compare(0, pref.size(), pref) == 0 && p[pref.size()] == '\\')) {
            return true;
        }
    }
    return false;
}

bool ImageCrawler::IsIgnoredUtilityBinary(const std::string& filename) {
    const std::string upper = SignatureMatcher::ExtractUpperBasename(filename);
    static const char* kBlocklist[] = {
        "DOS4GW.EXE",
        "DOS32A.EXE",
        "CWSDPMI.EXE",
        "RTM.EXE",
        "DPMI16BI.OVL",
        "SETUP.EXE",
        "INSTALL.EXE",
        "SETSOUND.EXE",
        "SNDSETUP.EXE",
        "SETSB.EXE",
        "CONFIG.EXE",
        "UVCONFIG.EXE",
        "UNIVBE.EXE",
        "LOADFIX.COM",
        "UNINST.EXE",
        "UNWISE.EXE",
        "README.COM",
        "README.EXE",
        "PKUNZIP.EXE",
        "ARJ.EXE",
        "LHA.EXE",
        "SMARTDRV.EXE",
        "EMM386.EXE",
        "MSCDEX.EXE",
        "COMMAND.COM",
        "AUTOEXEC.BAT"
    };
    for (size_t i = 0; i < sizeof(kBlocklist) / sizeof(kBlocklist[0]); ++i) {
        if (upper == kBlocklist[i]) {
            return true;
        }
    }
    return false;
}

int ImageCrawler::ScoreDosCandidate(
    const std::string& filename,
    const std::string& parent_dir_name,
    const ExecutableFingerprint& fp,
    bool bat_calls_sibling_exe,
    bool has_exact_signature,
    bool is_ignore_signature
) {
    if (is_ignore_signature) {
        return -10000;
    }

    int score = 0;
    if (has_exact_signature) {
        score += 1000;
    }

    const std::string stem = GetUpperStem(filename);
    const std::string parent_upper = ToUpperAscii(parent_dir_name);
    if (!stem.empty() && !parent_upper.empty() && stem == parent_upper) {
        score += 50;
    }

    if (fp.format == KioskExecFormat::BATCH_SCRIPT && bat_calls_sibling_exe) {
        score += 40;
    }

    if (fp.format == KioskExecFormat::DOS_LE ||
        fp.format == KioskExecFormat::DOS_LX ||
        fp.format == KioskExecFormat::DOS_BW ||
        fp.format == KioskExecFormat::WIN16_NE ||
        fp.format == KioskExecFormat::WIN32_PE) {
        score += 25;
    }

    // Demote configuration / setup / installer / readme / audio utilities
    static const char* kDemoteTokens[] = {
        "SETUP", "CFG", "CONFIG", "INST", "README", "UNINST", "SOUND", "AUDIO", "DIAG"
    };
    for (size_t i = 0; i < sizeof(kDemoteTokens) / sizeof(kDemoteTokens[0]); ++i) {
        if (stem.find(kDemoteTokens[i]) != std::string::npos) {
            score -= 500;
            break;
        }
    }

    return score;
}

static void StbAppendPngToVector(void* context, void* data, int size) {
    if (!context || !data || size <= 0) return;
    std::vector<uint8_t>* buf = static_cast<std::vector<uint8_t>*>(context);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    buf->insert(buf->end(), bytes, bytes + size);
}

bool ImageCrawler::WriteRgbaPng(
    const std::string& out_png_path,
    int width,
    int height,
    const std::vector<uint8_t>& rgba_pixels
) {
    if (out_png_path.empty() || width <= 0 || height <= 0) return false;
    if (rgba_pixels.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4U) {
        return false;
    }
    std::vector<uint8_t> encoded;
    if (stbi_write_png_to_func(StbAppendPngToVector, &encoded, width, height, 4, rgba_pixels.data(), width * 4) == 0 ||
        encoded.empty()) {
        return false;
    }
    return WriteRawBytesToFile(out_png_path, encoded.data(), encoded.size());
}

bool ImageCrawler::ExtractPeNeIcon(
    const uint8_t* file_data,
    size_t size,
    const std::string& out_png_path,
    int preferred_icon_index
) {
    if (!file_data || size < 6 || out_png_path.empty()) return false;

    // 1. Standalone .ICO file
    if (ReadLe16(file_data + 0) == 0 && ReadLe16(file_data + 2) == 1 && ReadLe16(file_data + 4) > 0) {
        return ExtractFromStandaloneIco(file_data, size, out_png_path);
    }

    // 2. MZ -> NE (16-bit) or PE (32-bit)
    if (size >= 0x40 && file_data[0] == 'M' && file_data[1] == 'Z') {
        uint32_t lfanew = ReadLe32(file_data + 0x3C);
        if (lfanew >= 0x40 && static_cast<uint64_t>(lfanew) <= (size - 4U)) {
            const size_t lf = static_cast<size_t>(lfanew);
            if (file_data[lf] == 'N' && file_data[lf + 1] == 'E') {
                return ExtractFromNeBinary(file_data, size, lf, out_png_path, preferred_icon_index);
            }
            if (file_data[lf] == 'P' && file_data[lf + 1] == 'E' &&
                file_data[lf + 2] == 0 && file_data[lf + 3] == 0) {
                return ExtractFromPeBinary(file_data, size, lf, out_png_path, preferred_icon_index);
            }
        }
    }

    return false;
}

bool ImageCrawler::ParseWin31GrpBuffer(
    const uint8_t* data,
    size_t size,
    std::vector<Win31GrpItem>& out_items
) {
    out_items.clear();
    if (!data || size < 34) return false;
    if (std::memcmp(data, "PMCC", 4) != 0) return false;

    // Native Windows 3.1 GROUPHEADER layout (34 bytes before rgiItems[]):
    // +0: cIdentifier[4], +4: wCheckSum(2), +6: cbGroup(2), +8: nCmdShow(2),
    // +10: rcNormal[4](8), +18: ptMin[2](4), +22: pName(2), +24: wLogPixelsX(2),
    // +26: wLogPixelsY(2), +28: bBitsPerPixel(1), +29: bPlanes(1),
    // +30: wReserved(2), +32: cItems(2), +34: rgiItems[cItems]
    uint16_t p_group_name = ReadLe16(data + 22);
    uint16_t c_items = ReadLe16(data + 32);
    std::string group_title = ReadNullTermAscii(data, size, p_group_name);

    if (static_cast<size_t>(c_items) * 2U > (size - 34U)) return false;

    for (uint16_t i = 0; i < c_items; ++i) {
        uint16_t item_off = ReadLe16(data + 34U + static_cast<size_t>(i) * 2U);
        if (item_off == 0) continue;
        if (static_cast<size_t>(item_off) + 24U > size) continue;

        const uint8_t* it = data + item_off;
        // Win31ItemData layout (24 bytes):
        // +0: pt[2] (4B), +4: iIcon (2B), +6: cbHeader (2B), +8: cbANDPlane (2B),
        // +10: cbXORPlane (2B), +12: pHeader (2B), +14: pANDPlane (2B), +16: pXORPlane (2B),
        // +18: pName (2B), +20: pCommand (2B), +22: pIconPath (2B)
        Win31GrpItem rec;
        rec.group_title = group_title;
        rec.icon_index = ReadLe16(it + 4);
        uint16_t cb_header = ReadLe16(it + 6);
        uint16_t cb_and = ReadLe16(it + 8);
        uint16_t cb_xor = ReadLe16(it + 10);
        uint16_t p_header = ReadLe16(it + 12);
        uint16_t p_and = ReadLe16(it + 14);
        uint16_t p_xor = ReadLe16(it + 16);
        uint16_t p_name = ReadLe16(it + 18);
        uint16_t p_command = ReadLe16(it + 20);
        uint16_t p_icon_path = ReadLe16(it + 22);

        rec.title = ReadNullTermAscii(data, size, p_name);
        rec.command_line = ReadNullTermAscii(data, size, p_command);
        rec.icon_path = ReadNullTermAscii(data, size, p_icon_path);
        SplitCommandLine(rec.command_line, rec.executable, rec.arguments);

        if (rec.executable.empty()) continue;
        if (rec.title.empty()) {
            rec.title = SignatureMatcher::PrettifyTitle(rec.executable);
        }

        // Decode embedded Win 3.1 cached icon (CURSORSHAPE: 12-byte header + AND + XOR)
        if (cb_header >= 12 && cb_and > 0 && cb_xor > 0 &&
            static_cast<size_t>(p_header) + 12U <= size &&
            static_cast<size_t>(p_and) + cb_and <= size &&
            static_cast<size_t>(p_xor) + cb_xor <= size) {
            const uint8_t* hdr = data + p_header;
            uint16_t w = ReadLe16(hdr + 4);
            uint16_t h = ReadLe16(hdr + 6);
            uint16_t width_bytes = ReadLe16(hdr + 8);
            uint8_t planes = hdr[10];
            uint8_t bpp = hdr[11];

            if (w > 0 && w <= 64 && h > 0 && h <= 64) {
                const uint8_t* and_bits = data + p_and;
                const uint8_t* xor_bits = data + p_xor;
                const size_t and_stride = (w + 7u) / 8u;

                if (and_stride * h <= cb_and) {
                    rec.embedded_icon_w = w;
                    rec.embedded_icon_h = h;
                    rec.embedded_rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4U, 0);

                    for (int y = 0; y < h; ++y) {
                        for (int x = 0; x < w; ++x) {
                            uint8_t idx = 0;
                            if (planes == 1 && bpp == 4) {
                                size_t row_bytes = (width_bytes > 0) ? width_bytes : ((w + 1u) / 2u);
                                size_t pos = static_cast<size_t>(y) * row_bytes + static_cast<size_t>(x >> 1);
                                if (pos < cb_xor) {
                                    idx = (x & 1) ? (xor_bits[pos] & 0x0Fu) : ((xor_bits[pos] >> 4) & 0x0Fu);
                                }
                            } else if (planes == 4 && bpp == 1) {
                                size_t plane_stride = (width_bytes > 0) ? width_bytes : ((w + 7u) / 8u);
                                size_t plane_size = plane_stride * h;
                                for (int pl = 0; pl < 4; ++pl) {
                                    size_t pos = static_cast<size_t>(pl) * plane_size +
                                                 static_cast<size_t>(y) * plane_stride +
                                                 static_cast<size_t>(x >> 3);
                                    if (pos < cb_xor) {
                                        uint8_t bit = (xor_bits[pos] >> (7 - (x & 7))) & 1u;
                                        idx |= static_cast<uint8_t>(bit << pl);
                                    }
                                }
                            } else if (bpp == 1) {
                                size_t row_bytes = (width_bytes > 0) ? width_bytes : ((w + 7u) / 8u);
                                size_t pos = static_cast<size_t>(y) * row_bytes + static_cast<size_t>(x >> 3);
                                if (pos < cb_xor) {
                                    idx = ((xor_bits[pos] >> (7 - (x & 7))) & 1u) ? 15u : 0u;
                                }
                            }

                            uint8_t and_bit = (and_bits[static_cast<size_t>(y) * and_stride + static_cast<size_t>(x >> 3)] >> (7 - (x & 7))) & 1u;
                            size_t dst = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4U;
                            rec.embedded_rgba[dst + 0] = kWin16Palette[idx & 0x0Fu][0];
                            rec.embedded_rgba[dst + 1] = kWin16Palette[idx & 0x0Fu][1];
                            rec.embedded_rgba[dst + 2] = kWin16Palette[idx & 0x0Fu][2];
                            rec.embedded_rgba[dst + 3] = (and_bit != 0) ? 0x00 : 0xFF;
                        }
                    }
                }
            }
        }

        out_items.push_back(rec);
    }

    return true;
}

bool ImageCrawler::ParseWin9xLnkBuffer(
    const uint8_t* data,
    size_t size,
    Win9xLnkInfo& out_info
) {
    out_info = Win9xLnkInfo();
    if (!data || size < 0x4C) return false;

    uint32_t header_size = ReadLe32(data + 0);
    if (header_size != 0x0000004Cu) return false;

    static const uint8_t kLinkClsid[16] = {
        0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
    };
    if (std::memcmp(data + 4, kLinkClsid, 16) != 0) return false;

    uint32_t link_flags = ReadLe32(data + 0x14);
    out_info.icon_index = ReadLe32Signed(data + 0x38);

    size_t pos = 0x4C;

    // 1. Optional LinkTargetIDList (bit 0)
    if (link_flags & 0x01u) {
        if ((size - pos) < 2) return false;
        uint16_t idlist_size = ReadLe16(data + pos);
        pos += 2;
        if (idlist_size > (size - pos)) return false;
        pos += idlist_size;
    }

    // 2. Optional LinkInfo (bit 1)
    if (link_flags & 0x02u) {
        if ((size - pos) < 4) return false;
        uint32_t li_size = ReadLe32(data + pos);
        if (li_size < 0x1C || static_cast<uint64_t>(li_size) > (size - pos)) return false;
        const size_t li_end = pos + static_cast<size_t>(li_size);

        uint32_t li_hdr_size = ReadLe32(data + pos + 4);
        uint32_t li_flags = ReadLe32(data + pos + 8);
        uint32_t local_base_off = ReadLe32(data + pos + 16);
        uint32_t common_suffix_off = ReadLe32(data + pos + 24);

        if (li_flags & 0x01u) {
            std::string base_path;
            std::string suffix;
            if (li_hdr_size >= 0x24 && (li_end - pos) >= 0x24) {
                uint32_t local_base_uni_off = ReadLe32(data + pos + 28);
                if (local_base_uni_off > 0 && local_base_uni_off < li_size) {
                    std::vector<uint16_t> u16;
                    for (size_t u = pos + local_base_uni_off; (li_end - u) >= 2; u += 2) {
                        uint16_t ch = ReadLe16(data + u);
                        if (ch == 0) break;
                        u16.push_back(ch);
                    }
                    base_path = Utf16LeToUtf8(u16);
                }
                uint32_t common_suffix_uni_off = ReadLe32(data + pos + 32);
                if (common_suffix_uni_off > 0 && common_suffix_uni_off < li_size) {
                    std::vector<uint16_t> u16;
                    for (size_t u = pos + common_suffix_uni_off; (li_end - u) >= 2; u += 2) {
                        uint16_t ch = ReadLe16(data + u);
                        if (ch == 0) break;
                        u16.push_back(ch);
                    }
                    suffix = Utf16LeToUtf8(u16);
                }
            }
            if (base_path.empty() && local_base_off > 0 && local_base_off < li_size) {
                base_path = ReadNullTermAscii(data, li_end, pos + local_base_off);
            }
            if (suffix.empty() && common_suffix_off > 0 && common_suffix_off < li_size) {
                suffix = ReadNullTermAscii(data, li_end, pos + common_suffix_off);
            }
            if (!base_path.empty()) {
                if (!suffix.empty() && base_path.back() != '\\' && suffix.front() != '\\') {
                    out_info.target_path = base_path + "\\" + suffix;
                } else {
                    out_info.target_path = base_path + suffix;
                }
            }
        }
        pos = li_end;
    }

    // 3. Optional StringData blocks
    const bool is_unicode = (link_flags & 0x80u) != 0;
    auto read_string_data = [&](std::string& out_str) -> bool {
        if ((size - pos) < 2) return false;
        uint16_t char_count = ReadLe16(data + pos);
        pos += 2;
        if (is_unicode) {
            size_t byte_len = static_cast<size_t>(char_count) * 2U;
            if (byte_len > (size - pos)) return false;
            std::vector<uint16_t> u16;
            u16.reserve(char_count);
            for (uint16_t i = 0; i < char_count; ++i) {
                u16.push_back(ReadLe16(data + pos + static_cast<size_t>(i) * 2U));
            }
            out_str = Utf16LeToUtf8(u16);
            pos += byte_len;
        } else {
            if (char_count > (size - pos)) return false;
            out_str.assign(reinterpret_cast<const char*>(data + pos), char_count);
            pos += char_count;
        }
        return true;
    };

    if (link_flags & 0x04u) { // HasName
        if (!read_string_data(out_info.description)) return false;
    }
    if (link_flags & 0x08u) { // HasRelativePath
        std::string rel;
        if (!read_string_data(rel)) return false;
        if (out_info.target_path.empty() && !rel.empty()) {
            out_info.target_path = rel;
        }
    }
    if (link_flags & 0x10u) { // HasWorkingDir
        if (!read_string_data(out_info.working_dir)) return false;
    }
    if (link_flags & 0x20u) { // HasArguments
        if (!read_string_data(out_info.arguments)) return false;
    }
    if (link_flags & 0x40u) { // HasIconLocation
        if (!read_string_data(out_info.icon_location)) return false;
    }

    return !out_info.target_path.empty();
}

std::string ImageCrawler::GetDefaultIconCacheDir() {
#if defined(_WIN32)
    const char* local_app = std::getenv("LOCALAPPDATA");
    if (local_app && local_app[0] != '\0') {
        return std::string(local_app) + "\\DOSBox-Kiosk\\cache\\icons";
    }
    const char* tmp = std::getenv("TEMP");
    if (tmp && tmp[0] != '\0') {
        return std::string(tmp) + "\\dosbox-kiosk-icons";
    }
    return ".\\kiosk_icons";
#else
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0] != '\0') {
        return std::string(xdg) + "/dosbox-kiosk/icons";
    }
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.cache/dosbox-kiosk/icons";
    }
    return "/tmp/dosbox-kiosk-icons";
#endif
}

std::string ResolveRelativeGuestPath(const std::string& base_dir, const std::string& raw_target) {
    std::string t = TrimAscii(raw_target);
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '/') t[i] = '\\';
    }
    if (t.empty()) return "";

    // Absolute path with drive letter or leading backslash
    if ((t.size() >= 2 && t[1] == ':') || t[0] == '\\') {
        return (t[0] == '\\') ? (std::string("C:") + t) : t;
    }

    // Relative path: split base_dir + raw_target and normalize '.' / '..'
    std::string combined = NormalizeGuestDir(base_dir);
    if (combined.back() != '\\') combined.push_back('\\');
    combined += t;

    std::string drive_prefix = "C:";
    std::string rest = combined;
    if (combined.size() >= 2 && combined[1] == ':') {
        drive_prefix = combined.substr(0, 2);
        rest = combined.substr(2);
    }

    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < rest.size()) {
        while (pos < rest.size() && rest[pos] == '\\') ++pos;
        if (pos >= rest.size()) break;
        size_t next = rest.find('\\', pos);
        std::string seg = (next == std::string::npos) ? rest.substr(pos) : rest.substr(pos, next - pos);
        pos = (next == std::string::npos) ? rest.size() : (next + 1);
        if (seg == "." || seg.empty()) continue;
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(seg);
        }
    }

    std::string out = drive_prefix;
    if (parts.empty()) {
        out.push_back('\\');
    } else {
        for (size_t i = 0; i < parts.size(); ++i) {
            out.push_back('\\');
            out += parts[i];
        }
    }
    return out;
}

std::vector<ImageCrawler::CrawlResult> ImageCrawler::ScanDiskImage(
    const std::string& hdd_img_path,
    const std::string& icon_cache_dir,
    const std::string& cache_namespace
) {
    FatReadOnlyVfs vfs;
    if (!vfs.OpenImageFile(hdd_img_path)) {
        return std::vector<CrawlResult>();
    }
    return ScanMountedVfs(vfs, icon_cache_dir, cache_namespace, nullptr);
}

std::vector<ImageCrawler::CrawlResult> ImageCrawler::ScanMountedVfs(
    const FatReadOnlyVfs& vfs,
    const std::string& icon_cache_dir,
    const std::string& cache_namespace,
    std::string* out_error
) {
    std::vector<CrawlResult> results;
    if (out_error) out_error->clear();
    if (!vfs.IsOpen()) {
        if (out_error) *out_error = "FAT filesystem is not mounted";
        return results;
    }

    // Verify root directory is readable up front so truncated images report an error
    std::vector<FatDirEntry> root_probe;
    if (!vfs.ListDirectory(nullptr, root_probe)) {
        if (out_error) *out_error = "Failed to read FAT root directory (image may be truncated or corrupted)";
        return results;
    }

    std::set<std::string> seen_targets; // key: UPPER(full_target_path)
    std::set<uint32_t> seen_clusters;   // deduplicates 8.3 vs LFN aliases to the same FAT file

    auto reserve_fat_target = [&](const std::string& target_key, const FatDirEntry& fe) -> bool {
        if (fe.first_cluster >= 2 && seen_clusters.find(fe.first_cluster) != seen_clusters.end()) {
            return false;
        }
        if (!seen_targets.insert(target_key).second) {
            return false;
        }
        if (fe.first_cluster >= 2) {
            seen_clusters.insert(fe.first_cluster);
        }
        return true;
    };

    auto make_icon_path = [&](const std::string& seed_key) -> std::string {
        if (icon_cache_dir.empty()) return "";
        const std::string namespaced_seed = cache_namespace.empty()
                                                ? seed_key
                                                : (cache_namespace + "|" + seed_key);
        uint32_t h = SignatureMatcher::ComputeCrc32(namespaced_seed.data(), namespaced_seed.size());
        return JoinHostPath(icon_cache_dir, "icon_" + SignatureMatcher::FormatCrc32Hex(h) + ".png");
    };

    auto try_extract_icon_from_vfs_file = [&](const std::string& vfs_path, int icon_idx, const std::string& out_png) -> bool {
        if (out_png.empty() || vfs_path.empty()) return false;
        FatDirEntry fe;
        if (!vfs.FindPath(vfs_path, fe) || fe.is_directory) return false;
        std::vector<uint8_t> bytes;
        if (!vfs.ReadFile(fe, bytes, 8U * 1024U * 1024U)) return false;
        return ExtractPeNeIcon(bytes.data(), bytes.size(), out_png, icon_idx);
    };

    // ------------------------------------------------------------------------
    // 1. Windows 95/98 Start Menu (.LNK) Discovery
    // ------------------------------------------------------------------------
    FatDirEntry start_menu_dir;
    if (vfs.FindPath("\\WINDOWS\\Start Menu\\Programs", start_menu_dir) && start_menu_dir.is_directory) {
        struct LnkDirNode {
            FatDirEntry entry;
            std::string guest_dir;
            int depth;
        };
        std::vector<LnkDirNode> dir_stack;
        dir_stack.push_back(LnkDirNode{start_menu_dir, "C:\\WINDOWS\\Start Menu\\Programs", 0});

        while (!dir_stack.empty()) {
            LnkDirNode cur = dir_stack.back();
            dir_stack.pop_back();
            if (cur.depth > 6) continue;

            std::vector<FatDirEntry> entries;
            if (!vfs.ListDirectory(&cur.entry, entries)) continue;

            for (size_t i = 0; i < entries.size(); ++i) {
                const FatDirEntry& e = entries[i];
                if (e.is_directory) {
                    dir_stack.push_back(LnkDirNode{e, cur.guest_dir + "\\" + e.name, cur.depth + 1});
                    continue;
                }
                if (GetUpperExtension(e.name) != ".LNK") continue;

                std::vector<uint8_t> lnk_bytes;
                if (!vfs.ReadFile(e, lnk_bytes, 65536)) continue;

                Win9xLnkInfo lnk;
                if (!ParseWin9xLnkBuffer(lnk_bytes.data(), lnk_bytes.size(), lnk)) continue;

                std::string resolved_target = ResolveRelativeGuestPath(cur.guest_dir, lnk.target_path);
                if (resolved_target.empty()) continue;

                std::string dir_part, file_part;
                SplitGuestPath(resolved_target, dir_part, file_part);
                if (file_part.empty() || IsIgnoredUtilityBinary(file_part)) continue;

                std::string ext = GetUpperExtension(file_part);
                if (ext != ".EXE" && ext != ".COM" && ext != ".BAT") continue;

                FatDirEntry target_fe;
                if (!vfs.FindPath(resolved_target, target_fe) || target_fe.is_directory) {
                    continue; // Skip broken shortcuts whose target does not exist on the image
                }

                std::string work_dir = lnk.working_dir.empty() ? dir_part : NormalizeGuestDir(lnk.working_dir);
                std::string canonical_target = (dir_part.back() == '\\') ? (dir_part + file_part) : (dir_part + "\\" + file_part);
                std::string target_key = ToUpperAscii(canonical_target);
                if (!reserve_fat_target(target_key, target_fe)) continue;

                CrawlResult cr;
                cr.title = !lnk.description.empty() ? lnk.description : SignatureMatcher::PrettifyTitle(e.name);
                cr.working_dir = work_dir;
                // Preserve full target path if working directory differs from executable's directory
                cr.executable = (ToUpperAscii(work_dir) == ToUpperAscii(dir_part)) ? file_part : canonical_target;
                cr.arguments = lnk.arguments;
                cr.launch_mechanism = "WIN9X_RUNONCE_HOOK";
                cr.item_type = "APPLICATION";

                // Inspect target binary for fingerprint & icon
                std::vector<uint8_t> bin;
                if (vfs.ReadFile(target_fe, bin, 8U * 1024U * 1024U)) {
                    ExecutableFingerprint fp;
                    if (SignatureMatcher::ComputeExecutableFingerprint(bin.data(), bin.size(), file_part, fp, target_fe.file_size)) {
                        cr.executable_crc32 = fp.header_crc32;
                        SoftwareSignatureRecord sig;
                        if (SignatureMatcher::MatchSignature(file_part, fp, sig)) {
                            if (sig.item_type == "IGNORE_SYSTEM") continue;
                            cr.title = sig.detected_title;
                            cr.item_type = sig.item_type;
                            cr.matched_signature = true;
                        }
                    }
                    std::string icon_png = make_icon_path(target_key);
                    if (!icon_png.empty()) {
                        bool got_icon = false;
                        if (!lnk.icon_location.empty()) {
                            std::string resolved_icon = ResolveRelativeGuestPath(cur.guest_dir, lnk.icon_location);
                            got_icon = try_extract_icon_from_vfs_file(resolved_icon, lnk.icon_index, icon_png);
                        }
                        if (!got_icon) {
                            got_icon = ExtractPeNeIcon(bin.data(), bin.size(), icon_png, lnk.icon_index);
                        }
                        if (got_icon) cr.icon_output_path = icon_png;
                    }
                }
                results.push_back(cr);
            }
        }
    }

    // ------------------------------------------------------------------------
    // 2. Windows 3.1 Program Manager (.GRP via PROGMAN.INI) Discovery
    // ------------------------------------------------------------------------
    FatDirEntry progman_ini;
    if (vfs.FindPath("\\WINDOWS\\PROGMAN.INI", progman_ini) && !progman_ini.is_directory) {
        std::vector<uint8_t> ini_bytes;
        if (vfs.ReadFile(progman_ini, ini_bytes, 65536)) {
            std::string ini_text(reinterpret_cast<const char*>(ini_bytes.data()), ini_bytes.size());
            std::istringstream iss(ini_text);
            std::string line;
            bool in_groups = false;
            std::vector<std::string> grp_paths;

            while (std::getline(iss, line)) {
                std::string t = TrimAscii(line);
                if (t.empty() || t[0] == ';') continue;
                if (t.front() == '[' && t.back() == ']') {
                    in_groups = (ToUpperAscii(t) == "[GROUPS]");
                    continue;
                }
                if (in_groups) {
                    size_t eq = t.find('=');
                    if (eq != std::string::npos) {
                        std::string gp = TrimAscii(t.substr(eq + 1));
                        if (!gp.empty()) grp_paths.push_back(gp);
                    }
                }
            }

            for (size_t g = 0; g < grp_paths.size(); ++g) {
                FatDirEntry grp_fe;
                if (!vfs.FindPath(grp_paths[g], grp_fe) || grp_fe.is_directory) continue;
                std::vector<uint8_t> grp_bytes;
                if (!vfs.ReadFile(grp_fe, grp_bytes, 262144)) continue;

                std::vector<Win31GrpItem> items;
                if (!ParseWin31GrpBuffer(grp_bytes.data(), grp_bytes.size(), items)) continue;

                for (size_t k = 0; k < items.size(); ++k) {
                    const Win31GrpItem& it = items[k];
                    std::string dir_part, file_part;
                    SplitGuestPath(it.executable, dir_part, file_part);
                    if (file_part.empty() || IsIgnoredUtilityBinary(file_part)) continue;

                    FatDirEntry target_fe;
                    if (!vfs.FindPath(it.executable, target_fe) || target_fe.is_directory) {
                        continue; // Skip uninstalled / missing Program Manager targets
                    }

                    std::string target_key = ToUpperAscii(dir_part + "\\" + file_part);
                    if (!reserve_fat_target(target_key, target_fe)) continue;

                    CrawlResult cr;
                    cr.title = it.title;
                    cr.working_dir = dir_part;
                    cr.executable = file_part;
                    cr.arguments = it.arguments;
                    cr.launch_mechanism = "WIN3X_PROGMAN_HOOK";
                    cr.item_type = (ToUpperAscii(it.group_title).find("GAME") != std::string::npos) ? "GAME" : "APPLICATION";

                    // Check fingerprint & IGNORE_SYSTEM signature before extracting icon PNG
                    std::vector<uint8_t> bin;
                    if (vfs.ReadFile(target_fe, bin, 262144)) {
                        ExecutableFingerprint fp;
                        bool fp_ok = SignatureMatcher::ComputeExecutableFingerprint(bin.data(), bin.size(), file_part, fp, target_fe.file_size);
                        if (!fp_ok && target_fe.file_size > bin.size()) {
                            std::vector<uint8_t> full_bin;
                            if (vfs.ReadFile(target_fe, full_bin, 8U * 1024U * 1024U)) {
                                fp_ok = SignatureMatcher::ComputeExecutableFingerprint(full_bin.data(), full_bin.size(), file_part, fp, target_fe.file_size);
                            }
                        }
                        if (fp_ok) {
                            cr.executable_crc32 = fp.header_crc32;
                            SoftwareSignatureRecord sig;
                            if (SignatureMatcher::MatchSignature(file_part, fp, sig)) {
                                if (sig.item_type == "IGNORE_SYSTEM") continue;
                                cr.title = sig.detected_title;
                                cr.item_type = sig.item_type;
                                cr.matched_signature = true;
                            }
                        }
                    }

                    std::string icon_png = make_icon_path(target_key);
                    if (!icon_png.empty()) {
                        bool got_icon = false;
                        if (!it.embedded_rgba.empty() && it.embedded_icon_w > 0 && it.embedded_icon_h > 0) {
                            got_icon = WriteRgbaPng(icon_png, it.embedded_icon_w, it.embedded_icon_h, it.embedded_rgba);
                        }
                        if (!got_icon && !it.icon_path.empty()) {
                            got_icon = try_extract_icon_from_vfs_file(it.icon_path, it.icon_index, icon_png);
                        }
                        if (!got_icon) {
                            got_icon = try_extract_icon_from_vfs_file(it.executable, it.icon_index, icon_png);
                        }
                        if (got_icon) cr.icon_output_path = icon_png;
                    }

                    results.push_back(cr);
                }
            }
        }
    }

    // ------------------------------------------------------------------------
    // 3. Recursive DOS Directory Tree Walker + Per-Directory Main Binary Scorer
    // ------------------------------------------------------------------------
    struct DirNode {
        bool is_root = true;
        FatDirEntry entry;
        std::string guest_path; // e.g. "C:\GAMES\DOOM2"
        std::string dir_name;   // e.g. "DOOM2"
        int depth = 0;
    };

    std::vector<DirNode> stack;
    DirNode root_node;
    root_node.is_root = true;
    root_node.guest_path = "C:\\";
    root_node.dir_name = "";
    root_node.depth = 0;
    stack.push_back(root_node);

    const int kMaxDepth = 6;
    while (!stack.empty()) {
        DirNode node = stack.back();
        stack.pop_back();

        std::vector<FatDirEntry> entries;
        if (!vfs.ListDirectory(node.is_root ? nullptr : &node.entry, entries)) {
            continue;
        }

        // Collect subdirectories and candidate executables in this directory
        std::vector<FatDirEntry> exec_files;
        std::vector<std::string> sibling_exes;
        std::string companion_ico_path;

        for (size_t i = 0; i < entries.size(); ++i) {
            const FatDirEntry& e = entries[i];
            if (e.is_directory) {
                if (node.depth < kMaxDepth) {
                    std::string child_path = (node.guest_path == "C:\\")
                                                 ? ("C:\\" + e.name)
                                                 : (node.guest_path + "\\" + e.name);
                    if (!IsPrunedSystemDirectory(child_path)) {
                        // Also skip \WINDOWS root for raw DOS walk (handled by .GRP/.LNK above)
                        if (ToUpperAscii(child_path) != "C:\\WINDOWS") {
                            DirNode child;
                            child.is_root = false;
                            child.entry = e;
                            child.guest_path = child_path;
                            child.dir_name = e.name;
                            child.depth = node.depth + 1;
                            stack.push_back(child);
                        }
                    }
                }
                continue;
            }

            std::string ext = GetUpperExtension(e.name);
            if (ext == ".ICO" && companion_ico_path.empty()) {
                companion_ico_path = (node.guest_path == "C:\\")
                                         ? ("C:\\" + e.name)
                                         : (node.guest_path + "\\" + e.name);
            } else if (ext == ".EXE" || ext == ".COM" || ext == ".BAT") {
                if (ext == ".EXE") {
                    sibling_exes.push_back(e.name);
                }
                if (!IsIgnoredUtilityBinary(e.name)) {
                    exec_files.push_back(e);
                }
            }
        }

        // Do not index loose binaries sitting directly in C:\ root unless they match software_signatures
        if (exec_files.empty()) continue;

        struct ScoredCandidate {
            FatDirEntry entry;
            ExecutableFingerprint fp;
            SoftwareSignatureRecord sig;
            bool has_sig = false;
            int score = 0;
            std::vector<uint8_t> sample_bytes;
        };

        std::vector<ScoredCandidate> scored;
        for (size_t i = 0; i < exec_files.size(); ++i) {
            const FatDirEntry& fe = exec_files[i];
            std::vector<uint8_t> bytes;
            // Read up to 256KB for fingerprinting & metadata scraping (full read later if NE/PE icon needed)
            if (!vfs.ReadFile(fe, bytes, 262144)) continue;

            ExecutableFingerprint fp;
            if (!SignatureMatcher::ComputeExecutableFingerprint(bytes.data(), bytes.size(), fe.name, fp, fe.file_size)) {
                if (fe.file_size > bytes.size()) {
                    std::vector<uint8_t> full_bytes;
                    if (vfs.ReadFile(fe, full_bytes, 8U * 1024U * 1024U) &&
                        SignatureMatcher::ComputeExecutableFingerprint(full_bytes.data(), full_bytes.size(), fe.name, fp, fe.file_size)) {
                        bytes.swap(full_bytes);
                    } else {
                        continue;
                    }
                } else {
                    continue;
                }
            }

            SoftwareSignatureRecord sig;
            bool has_sig = SignatureMatcher::MatchSignature(fe.name, fp, sig);
            bool is_ignore_sig = (has_sig && sig.item_type == "IGNORE_SYSTEM");

            if (node.is_root && !has_sig) {
                continue;
            }

            bool bat_calls_exe = (fp.format == KioskExecFormat::BATCH_SCRIPT) &&
                                 BatReferencesSiblingExe(bytes, sibling_exes);

            int sc = ScoreDosCandidate(fe.name, node.dir_name, fp, bat_calls_exe, has_sig, is_ignore_sig);
            if (sc <= -500) {
                continue; // Explicitly ignored or demoted setup/config utility
            }

            ScoredCandidate cand;
            cand.entry = fe;
            cand.fp = fp;
            cand.sig = sig;
            cand.has_sig = has_sig;
            cand.score = sc;
            cand.sample_bytes.swap(bytes);
            scored.push_back(cand);
        }

        if (scored.empty()) continue;

        // Sort descending by score, then descending by file size as tie-breaker
        std::sort(scored.begin(), scored.end(),
                  [](const ScoredCandidate& a, const ScoredCandidate& b) {
                      if (a.score != b.score) return a.score > b.score;
                      return a.entry.file_size > b.entry.file_size;
                  });

        // Keep top-scoring candidate per directory, plus any additional distinct software_signatures hits
        for (size_t i = 0; i < scored.size(); ++i) {
            if (i > 0 && !scored[i].has_sig) {
                break;
            }
            const ScoredCandidate& c = scored[i];
            std::string target_key = ToUpperAscii(node.guest_path + "\\" + c.entry.name);
            if (!reserve_fat_target(target_key, c.entry)) continue;

            CrawlResult cr;
            cr.working_dir = node.guest_path;
            cr.executable = c.entry.name;
            cr.executable_crc32 = c.fp.header_crc32;
            cr.score = c.score;
            cr.matched_signature = c.has_sig;

            if (c.fp.format == KioskExecFormat::WIN16_NE) {
                cr.launch_mechanism = "WIN3X_PROGMAN_HOOK";
            } else if (c.fp.format == KioskExecFormat::WIN32_PE) {
                cr.launch_mechanism = "WIN9X_RUNONCE_HOOK";
            } else {
                cr.launch_mechanism = "DOS_AUTOEXEC_INJECT";
            }

            if (c.has_sig) {
                cr.title = c.sig.detected_title;
                cr.item_type = (c.sig.item_type == "APPLICATION") ? "APPLICATION" : "GAME";
            } else {
                ScrapedMetadata meta;
                std::string fallback_label = !node.dir_name.empty() ? node.dir_name : c.entry.name;
                SignatureMatcher::ScrapeFallbackMetadata(
                    c.sample_bytes.data(), c.sample_bytes.size(), fallback_label, meta);
                cr.title = meta.title;
                cr.developer = meta.developer;
                cr.release_year = meta.release_year;
                cr.item_type = (c.fp.format == KioskExecFormat::WIN16_NE || c.fp.format == KioskExecFormat::WIN32_PE)
                                   ? "APPLICATION"
                                   : "GAME";
            }

            // Attempt icon extraction (NE/PE embedded icon, or companion .ICO in same folder)
            std::string icon_png = make_icon_path(target_key);
            if (!icon_png.empty()) {
                bool got_icon = false;
                if (c.fp.format == KioskExecFormat::WIN16_NE || c.fp.format == KioskExecFormat::WIN32_PE) {
                    std::vector<uint8_t> full_bin;
                    if (c.entry.file_size <= c.sample_bytes.size()) {
                        got_icon = ExtractPeNeIcon(c.sample_bytes.data(), c.sample_bytes.size(), icon_png, 0);
                    } else if (vfs.ReadFile(c.entry, full_bin, 8U * 1024U * 1024U)) {
                        got_icon = ExtractPeNeIcon(full_bin.data(), full_bin.size(), icon_png, 0);
                    }
                }
                if (!got_icon && !companion_ico_path.empty()) {
                    got_icon = try_extract_icon_from_vfs_file(companion_ico_path, 0, icon_png);
                }
                if (got_icon) {
                    cr.icon_output_path = icon_png;
                }
            }

            results.push_back(cr);
        }
    }

    return results;
}

bool ImageCrawler::ScanAndCatalogMachine(
    const std::string& machine_id,
    const std::string& icon_cache_dir,
    CrawlSummary& out_summary
) {
    out_summary = CrawlSummary();
    KioskDatabase& db = KioskDatabase::GetInstance();
    if (!db.IsOpen() || machine_id.empty()) {
        out_summary.error_message = "Database not open or invalid machine ID";
        return false;
    }

    MachineRecord mach;
    if (!db.GetMachine(machine_id, mach)) {
        out_summary.error_message = "Machine not found in catalog";
        return false;
    }
    if (mach.primary_hdd_path.empty()) {
        out_summary.error_message = "Machine has no primary HDD image configured";
        return false;
    }

    FatReadOnlyVfs vfs;
    if (!vfs.OpenImageFile(mach.primary_hdd_path)) {
        out_summary.error_message = "Could not open or mount FAT partition in HDD image: " + mach.primary_hdd_path;
        return false;
    }

    const std::string cache_dir = icon_cache_dir.empty() ? GetDefaultIconCacheDir() : icon_cache_dir;
    std::string scan_error;
    std::vector<CrawlResult> discovered = ScanMountedVfs(vfs, cache_dir, machine_id, &scan_error);
    if (!scan_error.empty()) {
        out_summary.error_message = scan_error;
        return false;
    }
    out_summary.total_discovered = static_cast<int>(discovered.size());

    // Build existing-items lookup map for deduplication & icon backfill
    std::vector<CatalogItemRecord> existing = db.GetItemsForMachine(machine_id);
    std::vector<std::pair<std::string, size_t>> existing_index;
    existing_index.reserve(existing.size());
    for (size_t i = 0; i < existing.size(); ++i) {
        std::string key = ToUpperAscii(NormalizeGuestDir(existing[i].guest_working_dir) + "\\" + existing[i].guest_executable);
        existing_index.push_back(std::make_pair(key, i));
    }

    auto find_existing_idx = [&](const std::string& key) -> int {
        for (size_t j = 0; j < existing_index.size(); ++j) {
            if (existing_index[j].first == key) {
                return static_cast<int>(existing_index[j].second);
            }
        }
        return -1;
    };

    for (size_t i = 0; i < discovered.size(); ++i) {
        const CrawlResult& cr = discovered[i];
        std::string key = ToUpperAscii(NormalizeGuestDir(cr.working_dir) + "\\" + cr.executable);
        int exist_idx = find_existing_idx(key);
        if (exist_idx >= 0) {
            out_summary.skipped_duplicates++;
            // Backfill icon_cache_path if this SCANNED item previously lacked an icon
            CatalogItemRecord& ex = existing[static_cast<size_t>(exist_idx)];
            if (ex.source_origin == "SCANNED" && ex.icon_cache_path.empty() && !cr.icon_output_path.empty()) {
                ex.icon_cache_path = cr.icon_output_path;
                if (db.UpdateCatalogItem(ex)) {
                    out_summary.icons_extracted++;
                }
            }
            continue;
        }

        CatalogItemRecord rec;
        rec.id = GenerateDeterministicItemUuid(machine_id, cr.working_dir, cr.executable);
        rec.machine_id = machine_id;
        rec.item_type = (cr.item_type == "APPLICATION") ? "APPLICATION" : "GAME";
        rec.title = cr.title;
        rec.developer = cr.developer;
        rec.release_year = cr.release_year;
        rec.source_origin = "SCANNED";
        rec.launch_mechanism = cr.launch_mechanism;
        rec.guest_working_dir = NormalizeGuestDir(cr.working_dir);
        rec.guest_executable = cr.executable;
        rec.guest_arguments = cr.arguments;
        rec.return_on_exit = true;
        rec.icon_cache_path = cr.icon_output_path;
        rec.executable_crc32 = cr.executable_crc32;

        if (db.InsertCatalogItem(rec)) {
            out_summary.newly_inserted++;
            if (!rec.icon_cache_path.empty()) {
                out_summary.icons_extracted++;
            }
            existing.push_back(rec);
            existing_index.push_back(std::make_pair(key, existing.size() - 1));
        } else {
            out_summary.failed_inserts++;
        }
    }

    LOG_MSG("KIOSK: Scanned machine '%s' (%s): discovered=%d, inserted=%d, duplicates=%d, failed=%d, icons=%d",
            mach.display_name.c_str(),
            mach.primary_hdd_path.c_str(),
            out_summary.total_discovered,
            out_summary.newly_inserted,
            out_summary.skipped_duplicates,
            out_summary.failed_inserts,
            out_summary.icons_extracted);

    if (out_summary.failed_inserts > 0) {
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf), "Failed to save %d of %d discovered title(s)",
                 out_summary.failed_inserts, out_summary.total_discovered);
        out_summary.error_message = err_buf;
        return false;
    }
    return true;
}

#endif // defined(C_KIOSK)
