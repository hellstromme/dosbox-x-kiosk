#include "config.h"

#if defined(C_KIOSK)

#include "signature_matcher.h"
#include "../kiosk_db.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

std::string ToUpperAscii(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    }
    return out;
}

std::string GetUpperExtension(const std::string& filename) {
    size_t slash = filename.find_last_of("\\/");
    size_t start = (slash == std::string::npos) ? 0 : (slash + 1);
    size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot <= start) {
        return "";
    }
    return ToUpperAscii(filename.substr(dot));
}

bool ContainsAsciiTokenCaseInsensitive(const uint8_t* data, size_t len, const char* needle) {
    if (!data || !needle || needle[0] == '\0') return false;
    const size_t nlen = std::strlen(needle);
    if (len < nlen) return false;

    for (size_t i = 0; i + nlen <= len; ++i) {
        bool match = true;
        for (size_t j = 0; j < nlen; ++j) {
            unsigned char c1 = static_cast<unsigned char>(std::toupper(data[i + j]));
            unsigned char c2 = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(needle[j])));
            if (c1 != c2) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool ClassifySecondaryHeader(const uint8_t* data, size_t size, size_t offset, KioskExecFormat& out_fmt) {
    if (offset < 0x1C || offset > size || (size - offset) < 2) {
        return false;
    }
    const uint8_t b0 = data[offset];
    const uint8_t b1 = data[offset + 1];

    if (b0 == 'P' && b1 == 'E' && (size - offset) >= 4 && data[offset + 2] == 0 && data[offset + 3] == 0) {
        out_fmt = KioskExecFormat::WIN32_PE;
        return true;
    }
    if (b0 == 'N' && b1 == 'E') {
        out_fmt = KioskExecFormat::WIN16_NE;
        return true;
    }
    if (b0 == 'L' && b1 == 'E') {
        out_fmt = KioskExecFormat::DOS_LE;
        return true;
    }
    if (b0 == 'L' && b1 == 'X') {
        out_fmt = KioskExecFormat::DOS_LX;
        return true;
    }
    if (b0 == 'B' && b1 == 'W') {
        out_fmt = KioskExecFormat::DOS_BW;
        return true;
    }
    return false;
}

bool TryExtractNeDescription(const uint8_t* data, size_t size, std::string& out_desc) {
    if (!data || size < 0x40) return false;
    if (data[0] != 'M' || data[1] != 'Z') return false;
    uint32_t lfanew_u32 = ReadLe32(data + 0x3C);
    if (lfanew_u32 < 0x40 || static_cast<uint64_t>(lfanew_u32) > size || (size - static_cast<size_t>(lfanew_u32)) < 0x40) {
        return false;
    }
    const size_t lfanew = static_cast<size_t>(lfanew_u32);
    if (data[lfanew] != 'N' || data[lfanew + 1] != 'E') return false;

    // NE header offset 0x2C: uint32_t absolute file offset to non-resident name table
    // NE header offset 0x20: uint16_t byte length of non-resident name table
    uint32_t nres_off_u32 = ReadLe32(data + lfanew + 0x2C);
    uint16_t nres_len = ReadLe16(data + lfanew + 0x20);
    if (nres_off_u32 == 0 || nres_len < 2) return false;
    if (static_cast<uint64_t>(nres_off_u32) > size || (size - static_cast<size_t>(nres_off_u32)) < 2) {
        return false;
    }
    const size_t nres_off = static_cast<size_t>(nres_off_u32);

    uint8_t str_len = data[nres_off];
    if (str_len == 0 || (size - nres_off - 1) < str_len) return false;

    std::string desc;
    desc.reserve(str_len);
    for (uint8_t i = 0; i < str_len; ++i) {
        unsigned char c = data[nres_off + 1 + i];
        if (c >= 0x20 && c < 0x7F) {
            desc.push_back(static_cast<char>(c));
        }
    }
    if (!desc.empty()) {
        out_desc = desc;
        return true;
    }
    return false;
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

} // anonymous namespace

uint32_t SignatureMatcher::UpdateCrc32(uint32_t crc_state, const void* data, size_t length) {
    struct CrcTable {
        uint32_t entries[256];
        CrcTable() {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) {
                    c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                entries[i] = c;
            }
        }
    };
    static const CrcTable kTable;

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = crc_state;
    for (size_t i = 0; i < length; ++i) {
        crc = kTable.entries[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

uint32_t SignatureMatcher::ComputeCrc32(const void* data, size_t length) {
    if (!data || length == 0) {
        return 0;
    }
    return UpdateCrc32(0xFFFFFFFFu, data, length) ^ 0xFFFFFFFFu;
}

std::string SignatureMatcher::FormatCrc32Hex(uint32_t crc) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", crc);
    return std::string(buf);
}

std::string SignatureMatcher::ExtractUpperBasename(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    return ToUpperAscii(base);
}

std::string SignatureMatcher::PrettifyTitle(const std::string& raw_name) {
    size_t slash = raw_name.find_last_of("\\/");
    std::string name = (slash == std::string::npos) ? raw_name : raw_name.substr(slash + 1);

    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        name = name.substr(0, dot);
    }

    bool all_upper = true;
    bool has_alpha = false;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '_' || name[i] == '-') {
            name[i] = ' ';
        }
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (std::isalpha(c)) {
            has_alpha = true;
            if (std::islower(c)) {
                all_upper = false;
            }
        }
    }

    name = TrimAscii(name);
    if (name.empty()) {
        return "Untitled";
    }

    if (has_alpha && all_upper && name.size() > 3) {
        bool cap_next = true;
        for (size_t i = 0; i < name.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(name[i]);
            if (std::isspace(c)) {
                cap_next = true;
            } else if (std::isalpha(c)) {
                name[i] = static_cast<char>(cap_next ? std::toupper(c) : std::tolower(c));
                cap_next = false;
            } else {
                cap_next = false;
            }
        }
    }
    return name;
}

bool SignatureMatcher::ComputeExecutableFingerprint(
    const uint8_t* data,
    size_t size,
    const std::string& filename_hint,
    ExecutableFingerprint& out_fp,
    uint64_t true_file_size
) {
    out_fp = ExecutableFingerprint();
    if (!data || size == 0) {
        return false;
    }

    const uint64_t effective_size = (true_file_size > 0) ? true_file_size : static_cast<uint64_t>(size);
    out_fp.file_size_bytes = effective_size;
    const std::string ext = GetUpperExtension(filename_hint);

    if (ext == ".BAT") {
        out_fp.format = KioskExecFormat::BATCH_SCRIPT;
    } else if (size >= 2 && ((data[0] == 'M' && data[1] == 'Z') || (data[0] == 'Z' && data[1] == 'M'))) {
        out_fp.format = KioskExecFormat::DOS_MZ;

        // Detect common DOS extender stubs in the first 64 KB
        const size_t stub_scan_len = (std::min)(size, static_cast<size_t>(65536));
        if (ContainsAsciiTokenCaseInsensitive(data, stub_scan_len, "DOS/4GW") ||
            ContainsAsciiTokenCaseInsensitive(data, stub_scan_len, "DOS/4G") ||
            ContainsAsciiTokenCaseInsensitive(data, stub_scan_len, "DOS32A") ||
            ContainsAsciiTokenCaseInsensitive(data, stub_scan_len, "PMODE/W") ||
            ContainsAsciiTokenCaseInsensitive(data, stub_scan_len, "WSTUB") ||
            ContainsAsciiTokenCaseInsensitive(data, stub_scan_len, "CauseWay")) {
            out_fp.has_dos_extender_stub = true;
        }

        bool found_secondary = false;

        // 1. Check standard e_lfanew pointer at offset 0x3C
        if (size >= 0x40) {
            uint32_t lfanew = ReadLe32(data + 0x3C);
            if (lfanew >= 0x40 && static_cast<size_t>(lfanew) < size) {
                KioskExecFormat sec_fmt = KioskExecFormat::UNKNOWN;
                if (ClassifySecondaryHeader(data, size, static_cast<size_t>(lfanew), sec_fmt)) {
                    out_fp.format = sec_fmt;
                    out_fp.payload_offset = static_cast<size_t>(lfanew);
                    found_secondary = true;
                }
            }
        }

        // 2. Check end of MZ image: (e_cp * 512) - (512 - e_cblp)
        if (!found_secondary && size >= 0x1C) {
            uint16_t e_cblp = ReadLe16(data + 0x02);
            uint16_t e_cp = ReadLe16(data + 0x04);
            if (e_cp > 0) {
                uint64_t mz_end = static_cast<uint64_t>(e_cp) * 512ULL;
                if (e_cblp > 0 && e_cblp < 512) {
                    mz_end -= (512ULL - static_cast<uint64_t>(e_cblp));
                }
                if (mz_end >= 0x1C && mz_end + 2 <= static_cast<uint64_t>(size)) {
                    KioskExecFormat sec_fmt = KioskExecFormat::UNKNOWN;
                    if (ClassifySecondaryHeader(data, size, static_cast<size_t>(mz_end), sec_fmt)) {
                        out_fp.format = sec_fmt;
                        out_fp.payload_offset = static_cast<size_t>(mz_end);
                        found_secondary = true;
                    }
                }
            }
        }

        // If a BW (Watcom stub) header was found and contains a nested LE/LX header nearby,
        // keep payload_offset at the BW/LE boundary so the shared stub is stripped.
        if (out_fp.format == KioskExecFormat::DOS_LE ||
            out_fp.format == KioskExecFormat::DOS_LX ||
            out_fp.format == KioskExecFormat::DOS_BW) {
            out_fp.has_dos_extender_stub = true;
        }
    } else if (ext == ".COM") {
        out_fp.format = KioskExecFormat::RAW_COM;
    }

    // Compute 4KB fingerprint hash:
    // - For stubbed LE/LX/BW DOS extenders, strip the stub and hash 4KB starting at payload_offset.
    // - For all other binaries, hash [uint64_t LE file_size + first 4KB].
    const size_t kHashWindow = 4096;
    if ((out_fp.format == KioskExecFormat::DOS_LE ||
         out_fp.format == KioskExecFormat::DOS_LX ||
         out_fp.format == KioskExecFormat::DOS_BW) &&
        out_fp.payload_offset > 0 && out_fp.payload_offset < size) {
        const size_t avail = (std::min)(kHashWindow, size - out_fp.payload_offset);
        out_fp.crc32_value = ComputeCrc32(data + out_fp.payload_offset, avail);
    } else {
        uint8_t size_le[8];
        uint64_t sz = effective_size;
        for (int i = 0; i < 8; ++i) {
            size_le[i] = static_cast<uint8_t>((sz >> (i * 8)) & 0xFFu);
        }
        uint32_t crc = 0xFFFFFFFFu;
        crc = UpdateCrc32(crc, size_le, sizeof(size_le));
        crc = UpdateCrc32(crc, data, (std::min)(kHashWindow, size));
        out_fp.crc32_value = crc ^ 0xFFFFFFFFu;
    }

    out_fp.header_crc32 = FormatCrc32Hex(out_fp.crc32_value);
    return true;
}

bool SignatureMatcher::MatchSignature(
    const std::string& exec_filename,
    const ExecutableFingerprint& fp,
    SoftwareSignatureRecord& out_sig
) {
    if (exec_filename.empty() || fp.header_crc32.empty()) {
        return false;
    }
    const std::string upper_name = ExtractUpperBasename(exec_filename);
    return KioskDatabase::GetInstance().FindSoftwareSignature(upper_name, fp.header_crc32, out_sig);
}

void SignatureMatcher::ScrapeFallbackMetadata(
    const uint8_t* data,
    size_t size,
    const std::string& fallback_name,
    ScrapedMetadata& out_meta
) {
    out_meta = ScrapedMetadata();
    out_meta.title = PrettifyTitle(fallback_name);

    if (!data || size == 0) {
        return;
    }

    // 1. Check NE module description string (often contains clean product title)
    std::string ne_desc;
    if (TryExtractNeDescription(data, size, ne_desc) && ne_desc.size() >= 3) {
        out_meta.title = TrimAscii(ne_desc);
    }

    // 2. Scan first 128KB for ASCII Copyright / (C) strings to harvest year & developer
    const size_t scan_limit = (std::min)(size, static_cast<size_t>(131072));
    for (size_t i = 0; i + 10 < scan_limit; ++i) {
        bool is_copy = false;
        size_t after_marker = i;

        if ((data[i] == 'C' || data[i] == 'c') &&
            i + 9 < scan_limit &&
            (data[i + 1] == 'o' || data[i + 1] == 'O') &&
            (data[i + 2] == 'p' || data[i + 2] == 'P') &&
            (data[i + 3] == 'y' || data[i + 3] == 'Y') &&
            (data[i + 4] == 'r' || data[i + 4] == 'R') &&
            (data[i + 5] == 'i' || data[i + 5] == 'I') &&
            (data[i + 6] == 'g' || data[i + 6] == 'G') &&
            (data[i + 7] == 'h' || data[i + 7] == 'H') &&
            (data[i + 8] == 't' || data[i + 8] == 'T')) {
            is_copy = true;
            after_marker = i + 9;
        } else if (data[i] == '(' && i + 3 < scan_limit &&
                   (data[i + 1] == 'C' || data[i + 1] == 'c') &&
                   data[i + 2] == ')') {
            is_copy = true;
            after_marker = i + 3;
        }

        if (!is_copy) continue;

        // Extract printable snippet up to 80 chars
        std::string snippet;
        for (size_t j = after_marker; j < scan_limit && snippet.size() < 80; ++j) {
            unsigned char c = data[j];
            if (c < 0x20 || c >= 0x7F) break;
            snippet.push_back(static_cast<char>(c));
        }

        // Look for a 4-digit retro release year (1980..2005)
        int year_found = 0;
        size_t year_end = 0;
        for (size_t k = 0; k + 4 <= snippet.size(); ++k) {
            if (std::isdigit(static_cast<unsigned char>(snippet[k])) &&
                std::isdigit(static_cast<unsigned char>(snippet[k + 1])) &&
                std::isdigit(static_cast<unsigned char>(snippet[k + 2])) &&
                std::isdigit(static_cast<unsigned char>(snippet[k + 3]))) {
                int y = (snippet[k] - '0') * 1000 +
                        (snippet[k + 1] - '0') * 100 +
                        (snippet[k + 2] - '0') * 10 +
                        (snippet[k + 3] - '0');
                if (y >= 1980 && y <= 2005) {
                    year_found = y;
                    year_end = k + 4;
                    break;
                }
            }
        }

        if (year_found > 0) {
            out_meta.release_year = year_found;
            // Developer text typically follows the year (skipping punctuation/spaces/ranges like "-1995")
            size_t dev_start = year_end;
            while (dev_start < snippet.size() &&
                   (std::isspace(static_cast<unsigned char>(snippet[dev_start])) ||
                    snippet[dev_start] == ',' || snippet[dev_start] == '-' ||
                    snippet[dev_start] == ':' || std::isdigit(static_cast<unsigned char>(snippet[dev_start])))) {
                ++dev_start;
            }
            if (dev_start < snippet.size()) {
                std::string dev = TrimAscii(snippet.substr(dev_start));
                // Strip trailing "All rights reserved" if present
                std::string dev_upper = ToUpperAscii(dev);
                size_t arr_pos = dev_upper.find("ALL RIGHTS");
                if (arr_pos != std::string::npos) {
                    dev = TrimAscii(dev.substr(0, arr_pos));
                }
                while (!dev.empty() && (dev.back() == '.' || dev.back() == ',' || dev.back() == ';')) {
                    dev.pop_back();
                }
                if (dev.size() >= 2 && dev.size() <= 48) {
                    out_meta.developer = dev;
                }
            }
            break;
        }
    }
}

#endif // defined(C_KIOSK)
