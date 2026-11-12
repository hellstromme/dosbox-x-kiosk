#include "config.h"

#if defined(C_KIOSK)

#include "boot_injector.h"
#include "kioskext_bin.h"
#include "logging.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

constexpr size_t FAT12_1440K_SECTORS = 2880;
constexpr size_t FAT12_SECTOR_SIZE = 512;
constexpr size_t FAT12_1440K_BYTES = FAT12_1440K_SECTORS * FAT12_SECTOR_SIZE; // 1,474,560 bytes

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void WriteLe16(uint8_t* p, uint16_t val) {
    p[0] = static_cast<uint8_t>(val & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

void WriteLe32(uint8_t* p, uint32_t val) {
    p[0] = static_cast<uint8_t>(val & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

// Formats a filename like "KIOSKGO.BAT" into 11-byte space-padded uppercase "KIOSKGO BAT"
void FormatShortName83(const std::string& filename_8_3, uint8_t out_11[11]) {
    std::memset(out_11, ' ', 11);
    std::string base = filename_8_3;
    std::string ext;
    size_t dot_pos = filename_8_3.find('.');
    if (dot_pos != std::string::npos) {
        base = filename_8_3.substr(0, dot_pos);
        ext = filename_8_3.substr(dot_pos + 1);
    } else if (filename_8_3.size() == 11) {
        for (size_t i = 0; i < 11; ++i) {
            out_11[i] = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(filename_8_3[i])));
        }
        return;
    }

    for (size_t i = 0; i < base.size() && i < 8; ++i) {
        out_11[i] = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(base[i])));
    }
    for (size_t i = 0; i < ext.size() && i < 3; ++i) {
        out_11[8 + i] = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(ext[i])));
    }
}

void SetFat12Entry(uint8_t* fat_base, uint16_t cluster, uint16_t value) {
    size_t offset = static_cast<size_t>(cluster) + (static_cast<size_t>(cluster) / 2);
    if ((cluster & 1) == 0) {
        fat_base[offset] = static_cast<uint8_t>(value & 0xFF);
        fat_base[offset + 1] = static_cast<uint8_t>((fat_base[offset + 1] & 0xF0) | ((value >> 8) & 0x0F));
    } else {
        fat_base[offset] = static_cast<uint8_t>((fat_base[offset] & 0x0F) | ((value << 4) & 0xF0));
        fat_base[offset + 1] = static_cast<uint8_t>((value >> 4) & 0xFF);
    }
}

uint16_t GetFat12Entry(const uint8_t* fat_base, uint16_t cluster) {
    size_t offset = static_cast<size_t>(cluster) + (static_cast<size_t>(cluster) / 2);
    uint16_t raw = ReadLe16(fat_base + offset);
    if ((cluster & 1) == 0) {
        return raw & 0x0FFF;
    } else {
        return (raw >> 4) & 0x0FFF;
    }
}

bool IsValidFatBpb(const uint8_t vbr[512]) {
    if (vbr[510] != 0x55 || vbr[511] != 0xAA) return false;
    if (!((vbr[0] == 0xEB && vbr[2] == 0x90) || vbr[0] == 0xE9)) return false;

    uint16_t byts_per_sec = ReadLe16(vbr + 11);
    uint8_t sec_per_clus = vbr[13];
    uint16_t rsvd_sec_cnt = ReadLe16(vbr + 14);
    uint8_t num_fats = vbr[16];

    if (byts_per_sec != 512) return false;
    if (sec_per_clus == 0 || (sec_per_clus & (sec_per_clus - 1)) != 0) return false;
    if (rsvd_sec_cnt == 0) return false;
    if (num_fats == 0 || num_fats > 4) return false;

    uint32_t tot_sec = ReadLe16(vbr + 19);
    if (tot_sec == 0) tot_sec = ReadLe32(vbr + 32);

    uint32_t fat_sz = ReadLe16(vbr + 22);
    if (fat_sz == 0) fat_sz = ReadLe32(vbr + 36);

    return (tot_sec > 0 && fat_sz > 0);
}

std::string ToUpperAscii(const std::string& s) {
    std::string res = s;
    for (size_t i = 0; i < res.size(); ++i) {
        res[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(res[i])));
    }
    return res;
}

std::string TrimWhitespace(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

} // anonymous namespace

BootInjector& BootInjector::GetInstance() {
    static BootInjector instance;
    return instance;
}

const char* BootInjector::GetAutoexecHookMarker() {
    return "REM --- DOSBOX-X KIOSK DISPATCH HOOK (DO NOT REMOVE) ---";
}

std::string BootInjector::GetAutoexecHookBlock() {
    return std::string(GetAutoexecHookMarker()) + "\r\nIF EXIST B:\\KIOSKGO.BAT CALL B:\\KIOSKGO.BAT\r\n";
}

bool BootInjector::HasAutoexecHook(const std::string& content) {
    if (content.find(GetAutoexecHookMarker()) != std::string::npos) {
        return true;
    }
    std::string upper = ToUpperAscii(content);
    return upper.find("B:\\KIOSKGO.BAT") != std::string::npos;
}

std::string BootInjector::InjectAutoexecHookToString(const std::string& existing_content) {
    if (HasAutoexecHook(existing_content)) {
        return existing_content;
    }

    std::string cleaned = existing_content;
    // Strip trailing DOS EOF (0x1A / Ctrl-Z) characters before appending
    while (!cleaned.empty() && cleaned.back() == '\x1A') {
        cleaned.pop_back();
    }

    if (!cleaned.empty() && cleaned.back() != '\n') {
        cleaned += "\r\n";
    }
    cleaned += GetAutoexecHookBlock();
    return cleaned;
}

bool BootInjector::EnsureOneTimeAutoexecHook(const std::string& hdd_or_autoexec_path) const {
    if (hdd_or_autoexec_path.empty()) return false;

    std::fstream f(hdd_or_autoexec_path.c_str(), std::ios::in | std::ios::out | std::ios::binary);
    if (!f.is_open()) {
        return false;
    }

    f.seekg(0, std::ios::end);
    std::streamoff file_size_off = f.tellg();
    if (file_size_off < 0) return false;
    uint64_t file_size = static_cast<uint64_t>(file_size_off);

    uint8_t sec0[512];
    std::memset(sec0, 0, sizeof(sec0));
    if (file_size >= 512) {
        f.seekg(0, std::ios::beg);
        f.read(reinterpret_cast<char*>(sec0), 512);
        if (!f.good() || f.gcount() != 512) {
            return false;
        }
    }

    // Mode 1: Plain text host-side AUTOEXEC.BAT file (< 64 KB without a 0x55AA boot signature)
    if (file_size < 65536 && (file_size < 512 || sec0[510] != 0x55 || sec0[511] != 0xAA)) {
        f.clear();
        f.seekg(0, std::ios::beg);
        if (!f.good()) {
            return false;
        }
        std::string content(static_cast<size_t>(file_size), '\0');
        if (file_size > 0) {
            f.read(&content[0], static_cast<std::streamsize>(file_size));
            if (f.fail() || static_cast<uint64_t>(f.gcount()) != file_size) {
                return false;
            }
        }
        f.close();

        if (HasAutoexecHook(content)) {
            return true;
        }

        std::string updated = InjectAutoexecHookToString(content);
        std::ofstream out(hdd_or_autoexec_path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out.is_open()) return false;
        out.write(updated.data(), static_cast<std::streamsize>(updated.size()));
        return out.good();
    }

    if (file_size < 512 || sec0[510] != 0x55 || sec0[511] != 0xAA) {
        return false;
    }

    // Mode 2: Raw Disk Image (FAT12 / FAT16 / FAT32, Superfloppy or MBR-partitioned)
    uint64_t part_offset = 0;
    uint8_t vbr[512];
    bool found_vbr = false;

    if (IsValidFatBpb(sec0)) {
        part_offset = 0;
        std::memcpy(vbr, sec0, 512);
        found_vbr = true;
    } else {
        // Parse 4 MBR partition entries at 0x1BE..0x1FD
        for (int pass = 0; pass < 2 && !found_vbr; ++pass) {
            for (int i = 0; i < 4; ++i) {
                const uint8_t* pentry = sec0 + 0x1BE + (i * 16);
                uint8_t status = pentry[0];
                uint8_t ptype = pentry[4];
                uint32_t lba_start = ReadLe32(pentry + 8);
                uint32_t lba_count = ReadLe32(pentry + 12);

                if (ptype == 0 || lba_start == 0 || lba_count == 0) continue;
                // Pass 0 prefers active bootable partitions (0x80)
                if (pass == 0 && status != 0x80) continue;

                uint64_t cand_offset = static_cast<uint64_t>(lba_start) * 512ULL;
                if (cand_offset + 512ULL > file_size) continue;

                uint8_t cand_vbr[512];
                f.seekg(static_cast<std::streamoff>(cand_offset), std::ios::beg);
                f.read(reinterpret_cast<char*>(cand_vbr), 512);
                if (f.good() && IsValidFatBpb(cand_vbr)) {
                    part_offset = cand_offset;
                    std::memcpy(vbr, cand_vbr, 512);
                    found_vbr = true;
                    break;
                }
            }
        }
    }

    if (!found_vbr) {
        return false;
    }

    uint16_t byts_per_sec = ReadLe16(vbr + 11);
    uint8_t sec_per_clus = vbr[13];
    uint16_t rsvd_sec_cnt = ReadLe16(vbr + 14);
    uint8_t num_fats = vbr[16];
    uint16_t root_ent_cnt = ReadLe16(vbr + 17);
    uint32_t tot_sec = ReadLe16(vbr + 19);
    if (tot_sec == 0) tot_sec = ReadLe32(vbr + 32);
    uint16_t fat_sz_16 = ReadLe16(vbr + 22);
    uint32_t fat_sz = (fat_sz_16 != 0) ? fat_sz_16 : ReadLe32(vbr + 36);

    uint32_t root_dir_sectors = ((static_cast<uint32_t>(root_ent_cnt) * 32U) + (byts_per_sec - 1U)) / byts_per_sec;
    uint32_t first_data_sec = static_cast<uint32_t>(rsvd_sec_cnt) + (static_cast<uint32_t>(num_fats) * fat_sz) + root_dir_sectors;
    if (tot_sec <= first_data_sec) return false;

    uint32_t data_sec = tot_sec - first_data_sec;
    uint32_t count_of_clusters = data_sec / sec_per_clus;

    int fat_bits = 16;
    uint32_t root_clus = 2;
    if (fat_sz_16 == 0 || count_of_clusters >= 65525U) {
        fat_bits = 32;
        root_clus = ReadLe32(vbr + 44);
    } else if (count_of_clusters < 4085U) {
        fat_bits = 12;
    }

    uint64_t fat_offset = part_offset + static_cast<uint64_t>(rsvd_sec_cnt) * 512ULL;
    size_t fat_bytes = static_cast<size_t>(fat_sz) * 512U;
    if (fat_offset + fat_bytes > file_size) return false;

    std::vector<uint8_t> fat_buf(fat_bytes, 0);
    f.seekg(static_cast<std::streamoff>(fat_offset), std::ios::beg);
    f.read(reinterpret_cast<char*>(fat_buf.data()), static_cast<std::streamsize>(fat_bytes));
    if (!f.good()) return false;

    auto get_fat_entry = [&](uint32_t clus) -> uint32_t {
        if (fat_bits == 12) {
            size_t off = static_cast<size_t>(clus) + (static_cast<size_t>(clus) / 2U);
            if (off + 1 >= fat_buf.size()) return 0xFFFU;
            return GetFat12Entry(fat_buf.data(), static_cast<uint16_t>(clus));
        } else if (fat_bits == 16) {
            size_t off = static_cast<size_t>(clus) * 2U;
            if (off + 1 >= fat_buf.size()) return 0xFFFFU;
            return ReadLe16(fat_buf.data() + off);
        } else {
            size_t off = static_cast<size_t>(clus) * 4U;
            if (off + 3 >= fat_buf.size()) return 0x0FFFFFFFU;
            return ReadLe32(fat_buf.data() + off) & 0x0FFFFFFFU;
        }
    };

    bool fat_dirty = false;
    auto set_fat_entry = [&](uint32_t clus, uint32_t val) {
        fat_dirty = true;
        if (fat_bits == 12) {
            size_t off = static_cast<size_t>(clus) + (static_cast<size_t>(clus) / 2U);
            if (off + 1 < fat_buf.size()) {
                SetFat12Entry(fat_buf.data(), static_cast<uint16_t>(clus), static_cast<uint16_t>(val & 0x0FFFU));
            }
        } else if (fat_bits == 16) {
            size_t off = static_cast<size_t>(clus) * 2U;
            if (off + 1 < fat_buf.size()) {
                WriteLe16(fat_buf.data() + off, static_cast<uint16_t>(val & 0xFFFFU));
            }
        } else {
            size_t off = static_cast<size_t>(clus) * 4U;
            if (off + 3 < fat_buf.size()) {
                uint32_t prev_hi = ReadLe32(fat_buf.data() + off) & 0xF0000000U;
                WriteLe32(fat_buf.data() + off, prev_hi | (val & 0x0FFFFFFFU));
            }
        }
    };

    auto is_eof_cluster = [&](uint32_t val) -> bool {
        if (fat_bits == 12) return val >= 0x0FF8U;
        if (fat_bits == 16) return val >= 0xFFF8U;
        return val >= 0x0FFFFFF8U;
    };

    auto eof_marker = [&]() -> uint32_t {
        if (fat_bits == 12) return 0x0FFFU;
        if (fat_bits == 16) return 0xFFFFU;
        return 0x0FFFFFFFU;
    };

    auto alloc_free_cluster = [&]() -> uint32_t {
        uint32_t max_clus = count_of_clusters + 1U;
        for (uint32_t c = 2; c <= max_clus; ++c) {
            if (get_fat_entry(c) == 0) {
                set_fat_entry(c, eof_marker());
                return c;
            }
        }
        return 0;
    };

    auto cluster_to_offset = [&](uint32_t clus) -> uint64_t {
        uint64_t sector = static_cast<uint64_t>(first_data_sec) + static_cast<uint64_t>(clus - 2U) * static_cast<uint64_t>(sec_per_clus);
        return part_offset + sector * 512ULL;
    };

    // Scan root directory for "AUTOEXECBAT" or first free directory entry
    uint64_t found_dir_entry_offset = 0;
    uint8_t found_dir_entry[32];
    bool has_autoexec_entry = false;
    uint64_t free_dir_entry_offset = 0;

    auto inspect_dir_sector_buffer = [&](const uint8_t* buf, size_t buf_len, uint64_t base_disk_offset) {
        for (size_t off = 0; off + 32 <= buf_len; off += 32) {
            const uint8_t* ent = buf + off;
            if (ent[0] == 0x00 || ent[0] == 0xE5) {
                if (free_dir_entry_offset == 0) {
                    free_dir_entry_offset = base_disk_offset + off;
                }
                if (ent[0] == 0x00) break;
                continue;
            }
            uint8_t attr = ent[11];
            if (attr == 0x0F || (attr & 0x08) != 0 || (attr & 0x10) != 0) {
                continue;
            }
            if (std::memcmp(ent, "AUTOEXECBAT", 11) == 0) {
                found_dir_entry_offset = base_disk_offset + off;
                std::memcpy(found_dir_entry, ent, 32);
                has_autoexec_entry = true;
                return;
            }
        }
    };

    size_t clus_bytes = static_cast<size_t>(sec_per_clus) * 512U;
    if (fat_bits == 12 || fat_bits == 16) {
        uint64_t root_offset = part_offset + static_cast<uint64_t>(rsvd_sec_cnt + static_cast<uint32_t>(num_fats) * fat_sz) * 512ULL;
        size_t root_bytes = static_cast<size_t>(root_ent_cnt) * 32U;
        if (root_offset + root_bytes > file_size) return false;
        std::vector<uint8_t> root_buf(root_bytes, 0);
        f.seekg(static_cast<std::streamoff>(root_offset), std::ios::beg);
        f.read(reinterpret_cast<char*>(root_buf.data()), static_cast<std::streamsize>(root_bytes));
        inspect_dir_sector_buffer(root_buf.data(), root_bytes, root_offset);
    } else {
        uint32_t cur = root_clus;
        uint32_t last_root_clus = 0;
        size_t safety = 0;
        while (cur >= 2 && !is_eof_cluster(cur) && safety++ < 4096) {
            last_root_clus = cur;
            uint64_t c_off = cluster_to_offset(cur);
            if (c_off + clus_bytes > file_size) break;
            std::vector<uint8_t> c_buf(clus_bytes, 0);
            f.seekg(static_cast<std::streamoff>(c_off), std::ios::beg);
            f.read(reinterpret_cast<char*>(c_buf.data()), static_cast<std::streamsize>(clus_bytes));
            inspect_dir_sector_buffer(c_buf.data(), clus_bytes, c_off);
            if (has_autoexec_entry) break;
            cur = get_fat_entry(cur);
        }
        if (!has_autoexec_entry && free_dir_entry_offset == 0 && last_root_clus >= 2) {
            uint32_t new_root_clus = alloc_free_cluster();
            if (new_root_clus != 0) {
                uint64_t new_c_off = cluster_to_offset(new_root_clus);
                if (new_c_off + clus_bytes <= file_size) {
                    set_fat_entry(last_root_clus, new_root_clus);
                    set_fat_entry(new_root_clus, eof_marker());
                    std::vector<uint8_t> zero_clus(clus_bytes, 0);
                    f.seekp(static_cast<std::streamoff>(new_c_off), std::ios::beg);
                    f.write(reinterpret_cast<const char*>(zero_clus.data()), static_cast<std::streamsize>(clus_bytes));
                    free_dir_entry_offset = new_c_off;
                }
            }
        }
    }

    std::string target_content;
    std::vector<uint32_t> chain;

    if (has_autoexec_entry) {
        uint32_t fst_lo = ReadLe16(found_dir_entry + 26);
        uint32_t fst_hi = (fat_bits == 32) ? ReadLe16(found_dir_entry + 20) : 0;
        uint32_t fst_clus = (fst_hi << 16) | fst_lo;
        uint32_t existing_file_size = ReadLe32(found_dir_entry + 28);

        std::string existing_str;
        existing_str.reserve(existing_file_size);

        uint32_t cur = fst_clus;
        size_t bytes_left = existing_file_size;
        while (cur >= 2 && !is_eof_cluster(cur) && chain.size() < 65536) {
            chain.push_back(cur);
            if (bytes_left > 0) {
                uint64_t c_off = cluster_to_offset(cur);
                size_t to_read = (bytes_left < clus_bytes) ? bytes_left : clus_bytes;
                if (c_off + to_read > file_size) {
                    return false;
                }
                std::vector<char> chunk(to_read, 0);
                f.seekg(static_cast<std::streamoff>(c_off), std::ios::beg);
                f.read(chunk.data(), static_cast<std::streamsize>(to_read));
                if (f.fail() || static_cast<size_t>(f.gcount()) != to_read) {
                    return false;
                }
                existing_str.append(chunk.data(), to_read);
                bytes_left -= to_read;
            }
            cur = get_fat_entry(cur);
        }
        if (bytes_left != 0 || existing_str.size() != existing_file_size) {
            return false;
        }

        if (HasAutoexecHook(existing_str)) {
            return true; // Idempotent: 0 writes if hook is already present
        }

        target_content = InjectAutoexecHookToString(existing_str);
    } else {
        if (free_dir_entry_offset == 0) {
            return false;
        }
        target_content = GetAutoexecHookBlock();
        std::memset(found_dir_entry, 0, 32);
        std::memcpy(found_dir_entry, "AUTOEXECBAT", 11);
        found_dir_entry[11] = 0x20; // Archive attribute
        WriteLe16(found_dir_entry + 22, 0x0000); // Time
        WriteLe16(found_dir_entry + 24, 0x0021); // Date (1980-01-01)
        found_dir_entry_offset = free_dir_entry_offset;
    }

    size_t needed_clusters = (target_content.size() + clus_bytes - 1U) / clus_bytes;
    if (needed_clusters == 0) needed_clusters = 1;

    while (chain.size() < needed_clusters) {
        uint32_t new_c = alloc_free_cluster();
        if (new_c == 0) return false;
        if (!chain.empty()) {
            set_fat_entry(chain.back(), new_c);
        }
        set_fat_entry(new_c, eof_marker());
        chain.push_back(new_c);
    }

    // Write target_content across allocated cluster chain
    size_t written = 0;
    for (size_t i = 0; i < chain.size(); ++i) {
        uint64_t c_off = cluster_to_offset(chain[i]);
        if (c_off + clus_bytes > file_size) return false;
        std::vector<uint8_t> c_buf(clus_bytes, 0);
        if (written < target_content.size()) {
            size_t chunk = target_content.size() - written;
            if (chunk > clus_bytes) chunk = clus_bytes;
            std::memcpy(c_buf.data(), target_content.data() + written, chunk);
            written += chunk;
        }
        f.seekp(static_cast<std::streamoff>(c_off), std::ios::beg);
        f.write(reinterpret_cast<const char*>(c_buf.data()), static_cast<std::streamsize>(clus_bytes));
    }

    // Update directory entry
    uint32_t first_clus = chain.empty() ? 0 : chain[0];
    if (fat_bits == 32) {
        WriteLe16(found_dir_entry + 20, static_cast<uint16_t>((first_clus >> 16) & 0xFFFFU));
    }
    WriteLe16(found_dir_entry + 26, static_cast<uint16_t>(first_clus & 0xFFFFU));
    WriteLe32(found_dir_entry + 28, static_cast<uint32_t>(target_content.size()));

    f.seekp(static_cast<std::streamoff>(found_dir_entry_offset), std::ios::beg);
    f.write(reinterpret_cast<const char*>(found_dir_entry), 32);

    // Commit updated FAT tables if modified
    if (fat_dirty) {
        for (uint8_t fi = 0; fi < num_fats; ++fi) {
            uint64_t f_off = fat_offset + static_cast<uint64_t>(fi) * static_cast<uint64_t>(fat_bytes);
            f.seekp(static_cast<std::streamoff>(f_off), std::ios::beg);
            f.write(reinterpret_cast<const char*>(fat_buf.data()), static_cast<std::streamsize>(fat_bytes));
        }
    }

    f.flush();
    return f.good();
}

std::string BootInjector::SynthesizeKioskGoBat(
    const MachineRecord& machine,
    const CatalogItemRecord& item
) const {
    std::ostringstream ss;
    ss << "@ECHO OFF\r\n"
       << "CLS\r\n";

    // Parse optional leading drive letter (e.g., "C:\GAMES\DOOM2" or "D:\")
    std::string work_dir = TrimWhitespace(item.guest_working_dir);
    char drive_letter = 'C';
    if (work_dir.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(work_dir[0])) &&
        work_dir[1] == ':') {
        drive_letter = static_cast<char>(std::toupper(static_cast<unsigned char>(work_dir[0])));
        work_dir = work_dir.substr(2);
    }

    for (size_t i = 0; i < work_dir.size(); ++i) {
        if (work_dir[i] == '/') work_dir[i] = '\\';
    }

    ss << drive_letter << ":\r\n";
    if (!work_dir.empty()) {
        if (work_dir.find(' ') != std::string::npos && work_dir.front() != '"') {
            ss << "CD \"" << work_dir << "\"\r\n";
        } else {
            ss << "CD " << work_dir << "\r\n";
        }
    }

    std::string exe_str = TrimWhitespace(item.guest_executable);
    if (exe_str.find(' ') != std::string::npos && !exe_str.empty() && exe_str.front() != '"') {
        exe_str = "\"" + exe_str + "\"";
    }

    const bool is_windows_target =
        (item.launch_mechanism == "WIN3X_PROGMAN_HOOK" ||
         item.launch_mechanism == "WIN9X_RUNONCE_HOOK" ||
         (item.launch_mechanism != "DOS_AUTOEXEC_INJECT" &&
          item.launch_mechanism != "INTEGRATED_AUTOEXEC" &&
          machine.os_id.find("WIN") == 0));

    if (is_windows_target) {
        ss << "C:\\WINDOWS\\WIN.COM " << exe_str;
    } else {
        ss << exe_str;
    }

    if (!item.guest_arguments.empty()) {
        ss << " " << item.guest_arguments;
    }
    ss << "\r\n";

    if (item.return_on_exit) {
        ss << "B:\\KIOSKEXT.COM\r\n";
    }

    return ss.str();
}

bool BootInjector::BuildFat12ImageInMemory(
    const std::vector<DispatchFileEntry>& files,
    std::vector<uint8_t>& out_image_bytes,
    const std::string& volume_label
) const {
    if (files.size() > 223) {
        return false; // Root directory holds 224 entries (1 volume label + up to 223 files)
    }

    out_image_bytes.assign(FAT12_1440K_BYTES, 0);
    uint8_t* img = out_image_bytes.data();

    // 1. Sector 0: Boot Sector & BIOS Parameter Block (1.44MB 3.5" Microfloppy)
    img[0] = 0xEB;
    img[1] = 0x3C;
    img[2] = 0x90;
    std::memcpy(img + 3, "MSDOS5.0", 8);
    WriteLe16(img + 11, 512);  // BPB_BytsPerSec
    img[13] = 1;               // BPB_SecPerClus (1 sector = 512 bytes)
    WriteLe16(img + 14, 1);    // BPB_RsvdSecCnt (1 boot sector)
    img[16] = 2;               // BPB_NumFATs
    WriteLe16(img + 17, 224);  // BPB_RootEntCnt (14 sectors)
    WriteLe16(img + 19, 2880); // BPB_TotSec16 (1,474,560 bytes)
    img[21] = 0xF0;            // BPB_Media (3.5" HD floppy)
    WriteLe16(img + 22, 9);    // BPB_FATSz16 (9 sectors per FAT)
    WriteLe16(img + 24, 18);   // BPB_SecPerTrk
    WriteLe16(img + 26, 2);    // BPB_NumHeads
    WriteLe32(img + 28, 0);    // BPB_HiddSec
    WriteLe32(img + 32, 0);    // BPB_TotSec32

    // Extended BPB
    img[36] = 0x00;            // BS_DrvNum
    img[37] = 0x00;            // BS_Reserved1
    img[38] = 0x29;            // BS_BootSig
    WriteLe32(img + 39, 0x4B494F53U); // BS_VolID ("KIOS")

    uint8_t label_11[11];
    std::memset(label_11, ' ', 11);
    for (size_t i = 0; i < volume_label.size() && i < 11; ++i) {
        label_11[i] = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(volume_label[i])));
    }
    std::memcpy(img + 43, label_11, 11); // BS_VolLab
    std::memcpy(img + 54, "FAT12   ", 8); // BS_FilSysType
    img[510] = 0x55;
    img[511] = 0xAA;

    // 2. Sectors 1..9 (FAT #1) and Sectors 10..18 (FAT #2)
    uint8_t* fat1 = img + (1 * FAT12_SECTOR_SIZE);
    uint8_t* fat2 = img + (10 * FAT12_SECTOR_SIZE);
    fat1[0] = 0xF0;
    fat1[1] = 0xFF;
    fat1[2] = 0xFF;

    // 3. Sectors 19..32 (Root Directory, 224 entries * 32 bytes = 14 sectors)
    uint8_t* root_dir = img + (19 * FAT12_SECTOR_SIZE);
    // Entry 0: Volume Label
    std::memcpy(root_dir, label_11, 11);
    root_dir[11] = 0x08; // ATTR_VOLUME_ID
    WriteLe16(root_dir + 22, 0x0000);
    WriteLe16(root_dir + 24, 0x0021);

    // 4. Data Area begins at Sector 33 (Cluster 2)
    uint16_t next_cluster = 2;
    size_t dir_index = 1;

    for (size_t fi = 0; fi < files.size(); ++fi) {
        const DispatchFileEntry& file = files[fi];
        uint8_t* dent = root_dir + (dir_index * 32);
        FormatShortName83(file.filename_8_3, dent);
        dent[11] = 0x20; // ATTR_ARCHIVE
        WriteLe16(dent + 22, 0x0000); // Time
        WriteLe16(dent + 24, 0x0021); // Date

        if (file.data.empty()) {
            WriteLe16(dent + 26, 0);
            WriteLe32(dent + 28, 0);
            ++dir_index;
            continue;
        }

        size_t num_clusters = (file.data.size() + FAT12_SECTOR_SIZE - 1) / FAT12_SECTOR_SIZE;
        if (static_cast<size_t>(next_cluster) + num_clusters - 2 > 2847) {
            return false; // Out of disk space
        }

        uint16_t start_cluster = next_cluster;
        WriteLe16(dent + 26, start_cluster);
        WriteLe32(dent + 28, static_cast<uint32_t>(file.data.size()));

        for (size_t ci = 0; ci < num_clusters; ++ci) {
            uint16_t cur_cluster = static_cast<uint16_t>(start_cluster + ci);
            uint16_t next_val = (ci + 1 < num_clusters) ? static_cast<uint16_t>(cur_cluster + 1) : 0x0FFFU;
            SetFat12Entry(fat1, cur_cluster, next_val);

            size_t data_sector = 33 + static_cast<size_t>(cur_cluster - 2);
            size_t src_offset = ci * FAT12_SECTOR_SIZE;
            size_t copy_len = file.data.size() - src_offset;
            if (copy_len > FAT12_SECTOR_SIZE) copy_len = FAT12_SECTOR_SIZE;
            std::memcpy(img + (data_sector * FAT12_SECTOR_SIZE), file.data.data() + src_offset, copy_len);
        }

        next_cluster = static_cast<uint16_t>(start_cluster + num_clusters);
        ++dir_index;
    }

    // Mirror FAT #1 into FAT #2
    std::memcpy(fat2, fat1, 9 * FAT12_SECTOR_SIZE);
    return true;
}

bool BootInjector::ReadFileFromFat12Image(
    const std::vector<uint8_t>& image_bytes,
    const std::string& filename_8_3,
    std::vector<uint8_t>& out_file_data
) const {
    out_file_data.clear();
    if (image_bytes.size() < FAT12_1440K_BYTES) {
        return false;
    }

    const uint8_t* img = image_bytes.data();
    if (img[510] != 0x55 || img[511] != 0xAA) {
        return false;
    }

    uint8_t target_83[11];
    FormatShortName83(filename_8_3, target_83);

    const uint8_t* fat1 = img + (1 * FAT12_SECTOR_SIZE);
    const uint8_t* root_dir = img + (19 * FAT12_SECTOR_SIZE);

    for (size_t i = 0; i < 224; ++i) {
        const uint8_t* dent = root_dir + (i * 32);
        if (dent[0] == 0x00) break;
        if (dent[0] == 0xE5) continue;
        if (dent[11] == 0x0F || (dent[11] & 0x08) != 0 || (dent[11] & 0x10) != 0) continue;

        if (std::memcmp(dent, target_83, 11) == 0) {
            uint16_t cur_cluster = ReadLe16(dent + 26);
            uint32_t file_size = ReadLe32(dent + 28);
            out_file_data.reserve(file_size);

            uint32_t remaining = file_size;
            size_t safety = 0;
            while (remaining > 0 && cur_cluster >= 2 && cur_cluster < 0x0FF8 && safety++ < 2880) {
                size_t data_sector = 33 + static_cast<size_t>(cur_cluster - 2);
                if ((data_sector + 1) * FAT12_SECTOR_SIZE > image_bytes.size()) {
                    return false;
                }
                size_t chunk = (remaining < FAT12_SECTOR_SIZE) ? remaining : FAT12_SECTOR_SIZE;
                const uint8_t* sector_ptr = img + (data_sector * FAT12_SECTOR_SIZE);
                out_file_data.insert(out_file_data.end(), sector_ptr, sector_ptr + chunk);
                remaining -= static_cast<uint32_t>(chunk);
                cur_cluster = GetFat12Entry(fat1, cur_cluster);
            }
            return remaining == 0;
        }
    }

    return false;
}

bool BootInjector::BuildDispatchVolume(
    const std::string& output_image_path,
    const MachineRecord& machine,
    const CatalogItemRecord& item
) const {
    if (output_image_path.empty()) return false;

    std::string kioskgo_str = SynthesizeKioskGoBat(machine, item);

    DispatchFileEntry bat_entry;
    bat_entry.filename_8_3 = "KIOSKGO.BAT";
    bat_entry.data.assign(kioskgo_str.begin(), kioskgo_str.end());

    DispatchFileEntry com_entry;
    com_entry.filename_8_3 = "KIOSKEXT.COM";
    com_entry.data.assign(KIOSKEXT_COM_BYTES, KIOSKEXT_COM_BYTES + KIOSKEXT_COM_SIZE);

    std::vector<DispatchFileEntry> entries;
    entries.push_back(bat_entry);
    entries.push_back(com_entry);

    std::vector<uint8_t> image_bytes;
    if (!BuildFat12ImageInMemory(entries, image_bytes)) {
        return false;
    }

    std::ofstream out(output_image_path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out.is_open()) {
        LOG_MSG("KIOSK: Failed to write dispatch floppy image to '%s'", output_image_path.c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(image_bytes.data()), static_cast<std::streamsize>(image_bytes.size()));
    out.close();
    return !out.fail();
}

bool BootInjector::BuildIdleDispatchVolume(
    const std::string& output_image_path
) const {
    if (output_image_path.empty()) return false;

    DispatchFileEntry com_entry;
    com_entry.filename_8_3 = "KIOSKEXT.COM";
    com_entry.data.assign(KIOSKEXT_COM_BYTES, KIOSKEXT_COM_BYTES + KIOSKEXT_COM_SIZE);

    std::vector<DispatchFileEntry> entries;
    entries.push_back(com_entry);

    std::vector<uint8_t> image_bytes;
    if (!BuildFat12ImageInMemory(entries, image_bytes)) {
        return false;
    }

    std::ofstream out(output_image_path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out.is_open()) {
        LOG_MSG("KIOSK: Failed to write idle dispatch floppy image to '%s'", output_image_path.c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(image_bytes.data()), static_cast<std::streamsize>(image_bytes.size()));
    out.close();
    return !out.fail();
}

std::string BootInjector::GetDefaultDispatchImagePath() const {
#if defined(_WIN32)
    const char* temp_dir = std::getenv("TEMP");
    if (!temp_dir || !*temp_dir) {
        temp_dir = std::getenv("TMP");
    }
    if (temp_dir && *temp_dir) {
        std::string path(temp_dir);
        if (path.back() != '\\' && path.back() != '/') {
            path += '\\';
        }
        return path + "kiosk_dispatch.img";
    }
    return "kiosk_dispatch.img";
#else
    const char* xdg_dir = std::getenv("XDG_RUNTIME_DIR");
    if (xdg_dir && *xdg_dir) {
        std::string path(xdg_dir);
        if (path.back() != '/') path += '/';
        return path + "kiosk_dispatch.img";
    }
    return "/tmp/kiosk_dispatch.img";
#endif
}

bool BootInjector::CleanupDispatchVolume(const std::string& image_path) const {
    if (image_path.empty()) return false;
    return std::remove(image_path.c_str()) == 0;
}

#endif // defined(C_KIOSK)
