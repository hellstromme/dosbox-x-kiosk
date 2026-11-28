#include "config.h"

#if defined(C_KIOSK)

#include "fat_volume.h"
#include "bios_disk.h"
#include "logging.h"

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <cwchar>
#include <cwctype>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

const uint32_t kSector = 512U;
const size_t kMaxFatCacheSectors = 16384U; // 8 MB of clean FAT sectors
const uint32_t kMaxChainClusters = 1U << 22;
const char kJournalMagic[8] = {'K', 'S', 'K', 'J', 'R', 'N', 'L', '2'};
const char kJournalEndMagic[8] = {'K', 'S', 'K', 'J', 'E', 'N', 'D', '2'};
const size_t kJournalHeaderSize = 8U + 8U + 4U;
const size_t kJournalEntrySize = 8U + 4U + kSector;
const size_t kJournalTrailerSize = 4U + 8U;
const uint64_t kMaxJournalBytes = 1ULL << 30;

std::atomic<int> g_commit_fault_after(-1);
std::atomic<bool> g_commit_fault_power_cut(true);

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadLe64(const uint8_t* p) {
    return static_cast<uint64_t>(ReadLe32(p)) | (static_cast<uint64_t>(ReadLe32(p + 4)) << 32);
}

void WriteLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
}

void WriteLe32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFU);
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t b[4];
    WriteLe32(b, v);
    out.insert(out.end(), b, b + 4);
}

void AppendLe64(std::vector<uint8_t>& out, uint64_t v) {
    AppendLe32(out, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
    AppendLe32(out, static_cast<uint32_t>(v >> 32));
}

uint32_t Crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

std::string ToUpperAscii(const std::string& s) {
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    }
    return out;
}

std::string TrimAscii(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80U) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

std::string Utf16ToUtf8(const std::vector<uint16_t>& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        uint32_t u = in[i];
        if (u == 0x0000U) break;
        if (u == 0xFFFFU) continue;
        if (u >= 0xD800U && u <= 0xDBFFU && i + 1 < in.size() && in[i + 1] >= 0xDC00U && in[i + 1] <= 0xDFFFU) {
            u = 0x10000U + ((u - 0xD800U) << 10) + (static_cast<uint32_t>(in[i + 1]) - 0xDC00U);
            ++i;
        } else if (u >= 0xD800U && u <= 0xDFFFU) {
            u = 0xFFFDU;
        }
        AppendUtf8(out, u);
    }
    return out;
}

std::vector<uint16_t> Utf8ToUtf16(const std::string& s) {
    std::vector<uint16_t> out;
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0xFFFDU;
        size_t len = 1;
        if (c < 0x80U) {
            cp = c;
        } else if ((c & 0xE0U) == 0xC0U && i + 1 < s.size()) {
            cp = ((c & 0x1FU) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3FU);
            len = 2;
        } else if ((c & 0xF0U) == 0xE0U && i + 2 < s.size()) {
            cp = ((c & 0x0FU) << 12) | ((static_cast<uint8_t>(s[i + 1]) & 0x3FU) << 6) |
                 (static_cast<uint8_t>(s[i + 2]) & 0x3FU);
            len = 3;
        } else if ((c & 0xF8U) == 0xF0U && i + 3 < s.size()) {
            cp = ((c & 0x07U) << 18) | ((static_cast<uint8_t>(s[i + 1]) & 0x3FU) << 12) |
                 ((static_cast<uint8_t>(s[i + 2]) & 0x3FU) << 6) | (static_cast<uint8_t>(s[i + 3]) & 0x3FU);
            len = 4;
        }
        if (cp >= 0x10000U) {
            cp -= 0x10000U;
            out.push_back(static_cast<uint16_t>(0xD800U + (cp >> 10)));
            out.push_back(static_cast<uint16_t>(0xDC00U + (cp & 0x3FFU)));
        } else {
            out.push_back(static_cast<uint16_t>(cp));
        }
        i += len;
    }
    return out;
}

// "NAME    EXT" -> "NAME.EXT"
std::string ShortNameToDisplay(const uint8_t* e) {
    std::string base(reinterpret_cast<const char*>(e), 8);
    std::string ext(reinterpret_cast<const char*>(e + 8), 3);
    while (!base.empty() && base[base.size() - 1] == ' ') base.erase(base.size() - 1);
    while (!ext.empty() && ext[ext.size() - 1] == ' ') ext.erase(ext.size() - 1);
    if (!base.empty() && static_cast<uint8_t>(base[0]) == 0x05U) base[0] = static_cast<char>(0xE5);
    return ext.empty() ? base : (base + "." + ext);
}

uint8_t ShortNameChecksum(const uint8_t sfn[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) {
        sum = static_cast<uint8_t>(((sum & 1U) ? 0x80U : 0U) + (sum >> 1) + sfn[i]);
    }
    return sum;
}

bool IsValid83Char(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc)) return true;
    return std::strchr("$%'-_@~`!(){}^#&", c) != nullptr && c != '\0';
}

// Builds an 8.3 short name; out_lossy reports whether the long name cannot round-trip.
void Format83Name(const std::string& name, uint8_t out_sfn[11], bool& out_lossy) {
    std::memset(out_sfn, ' ', 11);
    out_lossy = false;

    std::string clean = name;
    while (!clean.empty() && clean[0] == '.') {
        clean.erase(clean.begin());
        out_lossy = true;
    }
    const size_t dot = clean.find_last_of('.');
    const std::string base = (dot == std::string::npos) ? clean : clean.substr(0, dot);
    const std::string ext = (dot == std::string::npos) ? std::string() : clean.substr(dot + 1);

    std::string fbase;
    for (size_t i = 0; i < base.size(); ++i) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(base[i])));
        if (c == ' ' || c == '.') { out_lossy = true; continue; }
        if (IsValid83Char(c)) fbase.push_back(c);
        else { fbase.push_back('_'); out_lossy = true; }
    }
    std::string fext;
    for (size_t i = 0; i < ext.size(); ++i) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(ext[i])));
        if (c == ' ' || c == '.') { out_lossy = true; continue; }
        if (IsValid83Char(c)) fext.push_back(c);
        else { fext.push_back('_'); out_lossy = true; }
    }
    if (fbase.size() > 8) { fbase = fbase.substr(0, 8); out_lossy = true; }
    if (fext.size() > 3) { fext = fext.substr(0, 3); out_lossy = true; }
    if (fbase.empty()) { fbase = "FILE"; out_lossy = true; }

    std::memcpy(out_sfn, fbase.data(), fbase.size());
    std::memcpy(out_sfn + 8, fext.data(), fext.size());

    const std::string reconstructed = fbase + (fext.empty() ? std::string() : ("." + fext));
    if (ToUpperAscii(name) != reconstructed) out_lossy = true;
}

std::vector<std::string> SplitGuestPath(const std::string& p) {
    std::string s = TrimAscii(p);
    if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') {
        s = s.substr(2);
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '/') s[i] = '\\';
    }
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string seg;
    while (std::getline(ss, seg, '\\')) {
        if (!seg.empty() && seg != ".") out.push_back(seg);
    }
    return out;
}

void StampDosDateTime(uint8_t* ent) {
    // Fixed valid DOS timestamp (2026-01-01 12:00:00) keeps writes deterministic
    WriteLe16(ent + 14, 0x6000);
    WriteLe16(ent + 16, 0x5C21);
    WriteLe16(ent + 18, 0x5C21);
    WriteLe16(ent + 22, 0x6000);
    WriteLe16(ent + 24, 0x5C21);
}

// ---- Host file helpers (UTF-8 paths on Windows) ----
#if defined(_WIN32)
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring(s.begin(), s.end());
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
#endif

std::FILE* OpenHostFile(const std::string& path, const char* mode) {
#if defined(_WIN32)
    const std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(Utf8ToWide(path).c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool RemoveHostFile(const std::string& path) {
#if defined(_WIN32)
    return _wremove(Utf8ToWide(path).c_str()) == 0;
#else
    return std::remove(path.c_str()) == 0;
#endif
}

bool SeekHostFile(std::FILE* f, uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

int64_t HostFileSize(std::FILE* f) {
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0) return -1;
    return static_cast<int64_t>(_ftelli64(f));
#else
    if (fseeko(f, 0, SEEK_END) != 0) return -1;
    return static_cast<int64_t>(ftello(f));
#endif
}

bool SyncHostFile(std::FILE* f) {
    if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

// Makes a journal create/delete durable (POSIX directory entry); no-op on Windows.
void SyncParentDirectory(const std::string& path) {
#if !defined(_WIN32)
    const size_t slash = path.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? std::string(".") : path.substr(0, slash == 0 ? 1 : slash);
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd >= 0) {
        (void)fsync(fd);
        ::close(fd);
    }
#else
    (void)path;
#endif
}

// ---- Boot sector geometry ----
struct FatGeometry {
    FilesystemType type = FilesystemType::FAT16;
    uint32_t sectors_per_cluster = 0;
    uint32_t reserved = 0;
    uint32_t num_fats = 0;
    uint32_t total_sectors = 0;
    uint32_t fat_size = 0;
    uint32_t root_dir_sectors = 0;
    uint32_t first_data = 0;
    uint32_t clusters = 0;
    uint32_t root_cluster = 0;
    uint32_t active_fat = 0;
    bool mirroring = true;
    uint32_t fsinfo_sector = 0;
};

bool LooksLikeFatBootSector(const uint8_t* v) {
    return v[510] == 0x55 && v[511] == 0xAA && ((v[0] == 0xEB && v[2] == 0x90) || v[0] == 0xE9);
}

bool IsFatPartitionType(uint8_t t) {
    switch (t) {
        case 0x01: case 0x04: case 0x06: case 0x0B: case 0x0C: case 0x0E:  // FAT12/16/32 (CHS/LBA)
        case 0x11: case 0x14: case 0x16: case 0x1B: case 0x1C: case 0x1E:  // Hidden variants
            return true;
        default:
            return false;
    }
}

bool ParseFatGeometry(const uint8_t* vbr, uint32_t part_lba, uint32_t max_part_sectors,
                      uint64_t device_sectors, bool require_within_device, FatGeometry& g) {
    if (!LooksLikeFatBootSector(vbr)) return false;
    const uint16_t bps = ReadLe16(vbr + 11);
    const uint8_t spc = vbr[13];
    const uint16_t rsvd = ReadLe16(vbr + 14);
    const uint8_t nfats = vbr[16];
    const uint16_t root_ent = ReadLe16(vbr + 17);
    uint32_t tot = ReadLe16(vbr + 19);
    if (tot == 0) tot = ReadLe32(vbr + 32);
    const uint16_t fat16_size = ReadLe16(vbr + 22);
    const uint32_t fat_size = (fat16_size != 0) ? fat16_size : ReadLe32(vbr + 36);

    if (bps != kSector) return false;
    if (spc == 0 || (spc & (spc - 1)) != 0) return false;
    if (rsvd == 0 || nfats == 0 || nfats > 4) return false;
    if (tot == 0 || fat_size == 0) return false;
    if (max_part_sectors > 0 && tot > max_part_sectors) return false;
    if (static_cast<uint64_t>(part_lba) + tot > 0xFFFFFFFFULL) return false;
    if (require_within_device && static_cast<uint64_t>(part_lba) + tot > device_sectors) return false;

    const uint32_t root_dir_secs = (static_cast<uint32_t>(root_ent) * 32U + (kSector - 1U)) / kSector;
    const uint64_t first_data = static_cast<uint64_t>(rsvd) + static_cast<uint64_t>(nfats) * fat_size + root_dir_secs;
    if (first_data >= tot) return false;
    const uint32_t clusters = (tot - static_cast<uint32_t>(first_data)) / spc;
    if (clusters == 0) return false;

    g = FatGeometry();
    g.sectors_per_cluster = spc;
    g.reserved = rsvd;
    g.num_fats = nfats;
    g.total_sectors = tot;
    g.fat_size = fat_size;
    g.root_dir_sectors = root_dir_secs;
    g.first_data = static_cast<uint32_t>(first_data);
    g.clusters = clusters;

    // FATSz16 == 0 is the unambiguous FAT32 BPB marker; otherwise classify by cluster count.
    if (fat16_size == 0 || clusters >= 65525U) {
        g.type = FilesystemType::FAT32;
        const uint16_t ext_flags = ReadLe16(vbr + 40);
        if (ext_flags & 0x0080U) {
            g.mirroring = false;
            g.active_fat = ext_flags & 0x000FU;
            if (g.active_fat >= nfats) return false;
        }
        g.root_cluster = ReadLe32(vbr + 44) & 0x0FFFFFFFU;
        if (g.root_cluster < 2 || (g.root_cluster - 2U) >= clusters) return false;
        const uint16_t fsi = ReadLe16(vbr + 48);
        if (fsi > 0 && fsi < rsvd) g.fsinfo_sector = fsi;
    } else if (clusters < 4085U) {
        g.type = FilesystemType::FAT12;
    } else {
        g.type = FilesystemType::FAT16;
    }

    // Never address clusters beyond what one FAT copy can describe
    const uint32_t bits = (g.type == FilesystemType::FAT12) ? 12U : (g.type == FilesystemType::FAT16 ? 16U : 32U);
    const uint64_t capacity = (static_cast<uint64_t>(fat_size) * kSector * 8U) / bits;
    if (capacity < 3U) return false;
    if (static_cast<uint64_t>(g.clusters) + 2U > capacity) {
        g.clusters = static_cast<uint32_t>(capacity - 2U);
    }
    return true;
}

// ---- Per-image lock registry ----
// Serialises every open, commit and recovery of one image across threads (import worker,
// launch path, crawler): readers share, writers and recovery are exclusive.
// Canonical key so relative/absolute spellings (and POSIX symlinks) share one mutex.
std::string NormalizeLockKey(const std::string& path) {
#if defined(_WIN32)
    std::wstring wide = Utf8ToWide(path);
    wchar_t full[4096];
    const DWORD n = GetFullPathNameW(wide.c_str(), 4096, full, nullptr);
    if (n > 0 && n < 4096) wide.assign(full, n);
    std::string key;
    for (size_t i = 0; i < wide.size(); ++i) {
        const wchar_t c = (wide[i] == L'/') ? L'\\' : static_cast<wchar_t>(towlower(wide[i]));
        AppendUtf8(key, static_cast<uint32_t>(c));
    }
    return key;
#else
    char* resolved = realpath(path.c_str(), nullptr);
    if (!resolved) return path; // Not created yet: the raw spelling is all there is
    std::string key(resolved);
    std::free(resolved);
    return key;
#endif
}

std::shared_ptr<std::shared_timed_mutex> AcquireImageMutex(const std::string& path) {
    static std::mutex registry_mutex;
    static std::map<std::string, std::weak_ptr<std::shared_timed_mutex>> registry;
    std::lock_guard<std::mutex> guard(registry_mutex);
    if (registry.size() > 64) {
        for (std::map<std::string, std::weak_ptr<std::shared_timed_mutex>>::iterator it = registry.begin();
             it != registry.end();) {
            if (it->second.expired()) it = registry.erase(it);
            else ++it;
        }
    }
    std::weak_ptr<std::shared_timed_mutex>& slot = registry[NormalizeLockKey(path)];
    std::shared_ptr<std::shared_timed_mutex> mtx = slot.lock();
    if (!mtx) {
        mtx = std::make_shared<std::shared_timed_mutex>();
        slot = mtx;
    }
    return mtx;
}

const std::chrono::seconds kImageLockTimeout(30);

// Creates a brand-new file, refusing to follow a planted symlink or reuse an existing inode.
std::FILE* CreateHostFileExclusive(const std::string& path, int posix_mode) {
    RemoveHostFile(path);
#if defined(_WIN32)
    (void)posix_mode;
    // CREATE_NEW via Win32 (works on every CRT, unlike the C11 "x" fopen mode)
    HANDLE h = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        return nullptr;
    }
    std::FILE* f = _fdopen(fd, "wb");
    if (!f) _close(fd);
    return f;
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = ::open(path.c_str(), flags, posix_mode);
    if (fd < 0) return nullptr;
    std::FILE* f = fdopen(fd, "wb");
    if (!f) ::close(fd);
    return f;
#endif
}

bool HostFileExists(const std::string& path) {
    std::FILE* f = OpenHostFile(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// Moves an unusable journal aside so it is never applied and never blocks the image again.
void QuarantineJournalFile(const std::string& jpath, const char* reason) {
    const std::string bad = jpath + ".bad";
    RemoveHostFile(bad);
#if defined(_WIN32)
    const bool moved = MoveFileExW(Utf8ToWide(jpath).c_str(), Utf8ToWide(bad).c_str(),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    const bool moved = std::rename(jpath.c_str(), bad.c_str()) == 0;
#endif
    if (!moved) RemoveHostFile(jpath);
    SyncParentDirectory(jpath);
    LOG_MSG("KIOSK: FAT journal %s not applied (%s); moved to .bad", jpath.c_str(), reason);
}

// Journal v2 layout:
//   "KSKJRNL2" | u64 image_bytes | u32 count | count x { u64 lba | u32 crc32(new) | 512 original }
//   | u32 crc32(everything before) | "KSKJEND2"
// Caller holds the image's exclusive lock.
// trusted: the caller wrote this journal and held the exclusive lock throughout (in-process
// I/O failure), so every journaled sector is restored even if it was only partly written.
bool RecoverJournalLocked(const std::string& image_path, bool* out_rolled_back, bool trusted = false) {
    if (out_rolled_back) *out_rolled_back = false;
    const std::string jpath = image_path + ".kiosk-journal";
    std::FILE* jf = OpenHostFile(jpath, "rb");
    if (!jf) return true; // Nothing to recover

    std::vector<uint8_t> journal;
    const int64_t jsize = HostFileSize(jf);
    bool read_ok = jsize >= 0 && static_cast<uint64_t>(jsize) <= kMaxJournalBytes && SeekHostFile(jf, 0);
    if (read_ok) {
        journal.resize(static_cast<size_t>(jsize));
        read_ok = journal.empty() || std::fread(journal.data(), 1, journal.size(), jf) == journal.size();
    }
    std::fclose(jf);

    bool valid = read_ok && journal.size() >= kJournalHeaderSize + kJournalTrailerSize &&
                 std::memcmp(journal.data(), kJournalMagic, 8) == 0;
    uint32_t count = 0;
    if (valid) {
        count = ReadLe32(journal.data() + 16);
        const size_t body_bytes = journal.size() - kJournalHeaderSize - kJournalTrailerSize;
        // Overflow-safe: compare count against what the file can actually hold
        valid = body_bytes % kJournalEntrySize == 0 && count == body_bytes / kJournalEntrySize;
    }
    if (valid) {
        const size_t body = journal.size() - kJournalTrailerSize;
        valid = Crc32(journal.data(), body) == ReadLe32(journal.data() + body) &&
                std::memcmp(journal.data() + body + 4, kJournalEndMagic, 8) == 0;
    }
    if (!valid) {
        // Torn journal: the image is only modified after the journal is durable, so it is intact.
        LOG_MSG("KIOSK: Discarding incomplete FAT journal %s (image untouched)", jpath.c_str());
        if (!RemoveHostFile(jpath)) QuarantineJournalFile(jpath, "incomplete and not removable");
        SyncParentDirectory(jpath);
        return true;
    }

    std::FILE* img = OpenHostFile(image_path, "rb+");
    if (!img) {
        if (!HostFileExists(image_path)) {
            QuarantineJournalFile(jpath, "image no longer exists");
            return true;
        }
        LOG_MSG("KIOSK: Cannot open %s to roll back interrupted FAT write", image_path.c_str());
        return false;
    }
    const int64_t img_size = HostFileSize(img);
    if (img_size < 0 || static_cast<uint64_t>(img_size) != ReadLe64(journal.data() + 8)) {
        std::fclose(img);
        QuarantineJournalFile(jpath, "image size changed since the journal was written");
        return true;
    }
    const uint64_t img_sectors = static_cast<uint64_t>(img_size) / kSector;

    // Validate every sector before touching anything: each must hold either the journaled
    // original (already rolled back) or exactly the new contents this journal wrote.
    std::vector<uint32_t> to_restore;
    uint8_t current[kSector];
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* ent = journal.data() + kJournalHeaderSize + static_cast<size_t>(i) * kJournalEntrySize;
        const uint64_t lba = ReadLe64(ent);
        if (lba >= img_sectors || !SeekHostFile(img, lba * kSector) ||
            std::fread(current, 1, kSector, img) != kSector) {
            std::fclose(img);
            QuarantineJournalFile(jpath, "journal does not match this image");
            return true;
        }
        if (std::memcmp(current, ent + 12, kSector) == 0) continue;
        if (!trusted && Crc32(current, kSector) != ReadLe32(ent + 8)) {
            std::fclose(img);
            QuarantineJournalFile(jpath, "image was modified after the interrupted write");
            return true;
        }
        to_restore.push_back(i);
    }

    bool ok = true;
    for (size_t k = 0; k < to_restore.size() && ok; ++k) {
        const uint8_t* ent = journal.data() + kJournalHeaderSize + static_cast<size_t>(to_restore[k]) * kJournalEntrySize;
        ok = SeekHostFile(img, ReadLe64(ent) * kSector) && std::fwrite(ent + 12, 1, kSector, img) == kSector;
    }
    ok = ok && SyncHostFile(img);
    std::fclose(img);
    if (!ok) {
        LOG_MSG("KIOSK: Rolling back interrupted FAT write on %s failed", image_path.c_str());
        return false; // Journal kept: the next attempt resumes safely
    }
    RemoveHostFile(jpath);
    SyncParentDirectory(jpath);
    LOG_MSG("KIOSK: Rolled back interrupted FAT write on %s (%u of %u sectors)", image_path.c_str(),
            static_cast<unsigned>(to_restore.size()), count);
    if (out_rolled_back) *out_rolled_back = true;
    return true;
}

} // anonymous namespace

bool KioskWriteHostFileAtomically(const std::string& path, const std::string& contents) {
    const std::string tmp = path + ".kiosk-tmp";
    std::FILE* f = CreateHostFileExclusive(tmp, 0644);
    if (!f) return false;
    const bool written = (contents.empty() || std::fwrite(contents.data(), 1, contents.size(), f) == contents.size()) &&
                         SyncHostFile(f);
    std::fclose(f);
    if (!written) {
        RemoveHostFile(tmp);
        return false;
    }
#if defined(_WIN32)
    const bool renamed = MoveFileExW(Utf8ToWide(tmp).c_str(), Utf8ToWide(path).c_str(),
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    const bool renamed = std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
    if (!renamed) {
        RemoveHostFile(tmp);
        return false;
    }
    SyncParentDirectory(path);
    return true;
}

// ============================================================================
// KioskFileSectorDevice
// ============================================================================

KioskFileSectorDevice::~KioskFileSectorDevice() {
    Close();
}

std::string KioskFileSectorDevice::JournalPathFor(const std::string& image_path) {
    return image_path + ".kiosk-journal";
}

bool KioskFileSectorDevice::HasPendingJournal(const std::string& image_path) {
    return HostFileExists(JournalPathFor(image_path));
}

void KioskFileSectorDevice::SetCommitFaultInjection(int sectors_applied_before_abort, bool simulate_power_cut) {
    g_commit_fault_power_cut.store(simulate_power_cut);
    g_commit_fault_after.store(sectors_applied_before_abort);
}

bool KioskFileSectorDevice::RecoverPendingJournal(const std::string& image_path, bool* out_rolled_back,
                                                  bool wait_for_lock) {
    if (out_rolled_back) *out_rolled_back = false;
    std::shared_ptr<std::shared_timed_mutex> mtx = AcquireImageMutex(image_path);
    const bool locked = wait_for_lock ? mtx->try_lock_for(kImageLockTimeout) : mtx->try_lock();
    if (!locked) {
        LOG_MSG("KIOSK: Disk image %s is busy (another kiosk operation holds it)", image_path.c_str());
        return false;
    }
    const bool ok = RecoverJournalLocked(image_path, out_rolled_back);
    mtx->unlock();
    return ok;
}

void KioskFileSectorDevice::QuarantinePendingJournal(const std::string& image_path) {
    std::shared_ptr<std::shared_timed_mutex> mtx = AcquireImageMutex(image_path);
    if (!mtx->try_lock_for(kImageLockTimeout)) {
        LOG_MSG("KIOSK: Could not lock %s to quarantine a stale FAT journal (busy)", image_path.c_str());
        return;
    }
    if (HasPendingJournal(image_path)) {
        QuarantineJournalFile(JournalPathFor(image_path), "image was recreated");
    }
    mtx->unlock();
}

namespace {
std::mutex g_guest_pin_mutex;
std::shared_ptr<std::shared_timed_mutex> g_guest_pin;
} // anonymous namespace

bool KioskFileSectorDevice::PinForGuestSession(const std::string& image_path) {
    ReleaseGuestSessionPin();
    std::shared_ptr<std::shared_timed_mutex> mtx = AcquireImageMutex(image_path);
    if (!mtx->try_lock_shared()) return false;
    std::lock_guard<std::mutex> guard(g_guest_pin_mutex);
    g_guest_pin = mtx;
    return true;
}

void KioskFileSectorDevice::ReleaseGuestSessionPin() {
    std::lock_guard<std::mutex> guard(g_guest_pin_mutex);
    if (g_guest_pin) {
        g_guest_pin->unlock_shared();
        g_guest_pin.reset();
    }
}

bool KioskFileSectorDevice::Open(const std::string& path, bool writable, std::string* out_error) {
    Close();
    if (path.empty()) {
        if (out_error) *out_error = "Empty disk image path";
        return false;
    }
    m_image_mutex = AcquireImageMutex(path);
    const bool locked = writable ? m_image_mutex->try_lock_for(kImageLockTimeout)
                                 : m_image_mutex->try_lock_shared(); // Readers (often the UI thread) never wait
    if (!locked) {
        m_image_mutex.reset();
        if (out_error) *out_error = "Disk image is busy (another kiosk operation holds it): " + path;
        return false;
    }
    m_lock_exclusive = writable;

    if (writable) {
        if (!RecoverJournalLocked(path, nullptr)) {
            if (out_error) *out_error = "Disk image has an interrupted write that could not be rolled back: " + path;
            Close();
            return false;
        }
    } else if (HasPendingJournal(path)) {
        // Read-only opens never write; the launch path or a writable open rolls back first.
        if (out_error) *out_error = "Disk image has an interrupted write pending rollback: " + path;
        Close();
        return false;
    }

    m_file = OpenHostFile(path, writable ? "rb+" : "rb");
    if (!m_file) {
        if (out_error) *out_error = std::string("Failed to open disk image ") + (writable ? "for writing: " : ": ") + path;
        Close();
        return false;
    }
    const int64_t size = HostFileSize(m_file);
    if (size < static_cast<int64_t>(kSector)) {
        if (out_error) *out_error = "Disk image is too small: " + path;
        Close();
        return false;
    }
    m_path = path;
    m_file_bytes = static_cast<uint64_t>(size);
    m_sector_count = static_cast<uint64_t>(size) / kSector;
    m_writable = writable;
    m_failed = false;
    return true;
}

void KioskFileSectorDevice::Close() {
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }
    if (m_image_mutex) {
        if (m_lock_exclusive) m_image_mutex->unlock();
        else m_image_mutex->unlock_shared();
        m_image_mutex.reset();
    }
    m_sector_count = 0;
    m_writable = false;
}

void KioskFileSectorDevice::AbandonAfterFailedCommit() {
    Close();
}

bool KioskFileSectorDevice::ReadSectors(uint64_t lba, uint32_t count, uint8_t* out) {
    if (!m_file || count == 0 || lba + count > m_sector_count) return false;
    if (!SeekHostFile(m_file, lba * kSector)) return false;
    const size_t bytes = static_cast<size_t>(count) * kSector;
    return std::fread(out, 1, bytes, m_file) == bytes;
}

bool KioskFileSectorDevice::WriteSectorsDirect(uint64_t lba, uint32_t count, const uint8_t* in) {
    if (!m_file || !m_writable || m_failed || count == 0 || lba + count > m_sector_count) return false;
    if (!SeekHostFile(m_file, lba * kSector)) return false;
    const size_t bytes = static_cast<size_t>(count) * kSector;
    return std::fwrite(in, 1, bytes, m_file) == bytes;
}

bool KioskFileSectorDevice::CommitSectorsAtomically(const SectorMap& sectors) {
    if (!m_file || !m_writable || m_failed) return false;
    const std::string jpath = JournalPathFor(m_path);
    if (HostFileExists(jpath)) return false; // Never overwrite an undo log that may still be needed
    // Directly written (previously free) clusters must be durable before metadata points at them
    if (!SyncHostFile(m_file)) return false;
    if (sectors.empty()) return true;

    // 1. Undo journal: original contents of every sector about to change (+ CRC of the new contents)
    std::vector<uint8_t> journal;
    journal.reserve(kJournalHeaderSize + sectors.size() * kJournalEntrySize + kJournalTrailerSize);
    journal.insert(journal.end(), kJournalMagic, kJournalMagic + 8);
    AppendLe64(journal, m_file_bytes); // Exact size, so odd-tail images still recover
    AppendLe32(journal, static_cast<uint32_t>(sectors.size()));
    uint8_t original[kSector];
    for (SectorMap::const_iterator it = sectors.begin(); it != sectors.end(); ++it) {
        if (it->second.size() != kSector || !ReadSectors(it->first, 1, original)) return false;
        AppendLe64(journal, it->first);
        AppendLe32(journal, Crc32(it->second.data(), kSector));
        journal.insert(journal.end(), original, original + kSector);
    }
    AppendLe32(journal, Crc32(journal.data(), journal.size()));
    journal.insert(journal.end(), kJournalEndMagic, kJournalEndMagic + 8);
    if (journal.size() > kMaxJournalBytes) return false; // Recovery would treat it as torn

    std::FILE* jf = CreateHostFileExclusive(jpath, 0600);
    if (!jf) return false;
    const bool journal_ok = std::fwrite(journal.data(), 1, journal.size(), jf) == journal.size() && SyncHostFile(jf);
    std::fclose(jf);
    if (!journal_ok) {
        RemoveHostFile(jpath);
        return false;
    }
    SyncParentDirectory(jpath);

    // 2. Apply. A crash from here on leaves a complete journal that rolls the image back.
    int applied = 0;
    bool ok = true;
    for (SectorMap::const_iterator it = sectors.begin(); it != sectors.end() && ok; ++it) {
        const int fault_after = g_commit_fault_after.load();
        if (fault_after >= 0 && applied >= fault_after) {
            g_commit_fault_after.store(-1);
            if (g_commit_fault_power_cut.load()) {
                std::fflush(m_file);
                m_failed = true;
                return false; // Simulated power cut: the process "dies" with the journal in place
            }
            ok = false; // Simulated I/O error: handled like a real one below
            break;
        }
        ok = WriteSectorsDirect(it->first, 1, it->second.data());
        ++applied;
    }
    ok = ok && SyncHostFile(m_file);
    if (!ok) {
        // Roll back immediately (we hold the exclusive lock) and refuse further use
        std::fflush(m_file);
        m_failed = true;
        if (!RecoverJournalLocked(m_path, nullptr, /*trusted=*/true)) {
            LOG_MSG("KIOSK: FAT commit on %s failed and rollback is still pending (journal kept)", m_path.c_str());
        }
        return false;
    }

    // 3. Commit point: invalidate the journal header durably before unlinking it. If the
    // unlink is lost (no directory fsync on Windows), recovery sees a torn journal and keeps
    // the committed image instead of rolling it back.
    if (std::FILE* inval = OpenHostFile(jpath, "rb+")) {
        const uint8_t zeros[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        const bool ok_inval = SeekHostFile(inval, 0) && std::fwrite(zeros, 1, sizeof(zeros), inval) == sizeof(zeros) &&
                              SyncHostFile(inval);
        std::fclose(inval);
        if (!ok_inval) LOG_MSG("KIOSK: Could not invalidate FAT journal %s before removal", jpath.c_str());
    }
    RemoveHostFile(jpath);
    SyncParentDirectory(jpath);
    return true;
}

// ============================================================================
// KioskMemorySectorDevice / KioskImageDiskSectorDevice
// ============================================================================

bool KioskMemorySectorDevice::ReadSectors(uint64_t lba, uint32_t count, uint8_t* out) {
    if (count == 0 || lba + count > GetSectorCount()) return false;
    std::memcpy(out, m_ro->data() + lba * kSector, static_cast<size_t>(count) * kSector);
    return true;
}

bool KioskMemorySectorDevice::WriteSectorsDirect(uint64_t lba, uint32_t count, const uint8_t* in) {
    if (!m_rw || count == 0 || lba + count > GetSectorCount()) return false;
    std::memcpy(m_rw->data() + lba * kSector, in, static_cast<size_t>(count) * kSector);
    return true;
}

bool KioskMemorySectorDevice::CommitSectorsAtomically(const SectorMap& sectors) {
    if (!m_rw) return false;
    for (SectorMap::const_iterator it = sectors.begin(); it != sectors.end(); ++it) {
        if (it->second.size() != kSector || it->first >= GetSectorCount()) return false;
    }
    for (SectorMap::const_iterator it = sectors.begin(); it != sectors.end(); ++it) {
        std::memcpy(m_rw->data() + it->first * kSector, it->second.data(), kSector);
    }
    return true;
}

KioskImageDiskSectorDevice::KioskImageDiskSectorDevice(imageDisk* disk, bool owns_disk)
    : m_disk(disk), m_owns(owns_disk) {}

KioskImageDiskSectorDevice::~KioskImageDiskSectorDevice() {
    if (m_owns && m_disk) delete m_disk;
    m_disk = nullptr;
}

bool KioskImageDiskSectorDevice::IsUsable() const {
    return m_disk != nullptr && m_disk->getSectSize() == kSector;
}

uint64_t KioskImageDiskSectorDevice::GetSectorCount() const {
    return IsUsable() ? m_disk->getLBA() : 0;
}

bool KioskImageDiskSectorDevice::ReadSectors(uint64_t lba, uint32_t count, uint8_t* out) {
    if (!IsUsable() || count == 0 || lba + count > 0xFFFFFFFFULL) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (m_disk->Read_AbsoluteSector(static_cast<uint32_t>(lba + i), out + static_cast<size_t>(i) * kSector) !=
            Int13Status::NoError) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// KioskFatVolume: mounting
// ============================================================================

KioskFatVolume::~KioskFatVolume() {
    Close();
}

bool KioskFatVolume::SelectPartition(KioskSectorDevice& device, bool require_within_device,
                                     uint32_t& out_lba, uint32_t& out_sector_count) {
    uint8_t sec0[kSector];
    if (!device.ReadSectors(0, 1, sec0)) return false;
    const uint64_t dev_secs = device.GetSectorCount();

    FatGeometry g;
    if (ParseFatGeometry(sec0, 0, 0, dev_secs, require_within_device, g)) {
        out_lba = 0;
        out_sector_count = g.total_sectors;
        return true;
    }
    if (sec0[510] != 0x55 || sec0[511] != 0xAA) return false;

    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < 4; ++i) {
            const uint8_t* pe = sec0 + 0x1BE + i * 16;
            const uint8_t status = pe[0];
            if (status != 0x00 && status != 0x80) continue;
            if ((pass == 0) != (status == 0x80)) continue; // Active entries first
            const uint32_t lba = ReadLe32(pe + 8);
            const uint32_t count = ReadLe32(pe + 12);
            if (!IsFatPartitionType(pe[4]) || lba == 0 || count == 0) continue;

            uint8_t vbr[kSector];
            if (!device.ReadSectors(lba, 1, vbr)) continue;
            if (ParseFatGeometry(vbr, lba, count, dev_secs, require_within_device, g)) {
                out_lba = lba;
                out_sector_count = g.total_sectors;
                return true;
            }
            if (LooksLikeFatBootSector(vbr)) {
                return false; // A FAT volume is present but inconsistent: never act on another one
            }
        }
    }
    return false;
}

bool KioskFatVolume::MountInternal(KioskSectorDevice* device, bool writable, std::string* out_error) {
    m_device = device;
    m_mounted = false;
    m_writable = writable;
    DiscardChanges();

    if (!device) {
        if (out_error) *out_error = "No disk device";
        return false;
    }
    if (writable && !device->IsWritable()) {
        if (out_error) *out_error = "Disk image is not writable";
        return false;
    }
    uint32_t lba = 0;
    uint32_t count = 0;
    // Trimmed images (BPB extends past EOF) are accepted; writes stay inside the file below.
    if (!SelectPartition(*device, false, lba, count)) {
        if (out_error) *out_error = "Disk image does not contain a usable FAT12/FAT16/FAT32 volume";
        return false;
    }
    uint8_t vbr[kSector];
    FatGeometry g;
    if (!device->ReadSectors(lba, 1, vbr) ||
        !ParseFatGeometry(vbr, lba, lba == 0 ? 0 : count, device->GetSectorCount(), false, g)) {
        if (out_error) *out_error = "Failed to parse FAT boot sector";
        return false;
    }

    m_fs_type = g.type;
    m_part_lba = lba;
    m_sectors_per_cluster = g.sectors_per_cluster;
    m_reserved_sectors = g.reserved;
    m_num_fats = g.num_fats;
    m_total_sectors = g.total_sectors;
    m_fat_size_sectors = g.fat_size;
    m_root_dir_sectors = g.root_dir_sectors;
    m_first_data_sector = g.first_data;
    m_cluster_count = g.clusters;
    m_root_cluster = g.root_cluster;
    m_active_fat = g.active_fat;
    m_fat_mirroring = g.mirroring;
    m_fsinfo_sector = g.fsinfo_sector;
    m_alloc_hint = 2;
    m_alloc_limit = m_cluster_count;
    if (writable) {
        // Metadata regions must exist; only clusters physically inside the file are allocatable.
        const uint64_t data_start = static_cast<uint64_t>(lba) + g.first_data;
        if (data_start > device->GetSectorCount()) {
            if (out_error) *out_error = "Disk image is truncated before its data region";
            return false;
        }
        const uint64_t present = (device->GetSectorCount() - data_start) / g.sectors_per_cluster;
        if (present < m_alloc_limit) m_alloc_limit = static_cast<uint32_t>(present);
    }
    m_mounted = true;
    return true;
}

bool KioskFatVolume::Mount(KioskSectorDevice* device, std::string* out_error) {
    Close();
    return MountInternal(device, device != nullptr && device->IsWritable(), out_error);
}

bool KioskFatVolume::OpenImageFile(const std::string& path, bool writable, std::string* out_error) {
    Close();
    std::string ext = path.size() >= 4 ? ToUpperAscii(path.substr(path.size() - 4)) : std::string();
    if (ext == ".VHD") {
        if (writable) {
            if (out_error) *out_error = "VHD disk images cannot be modified by the kiosk FAT writer: " + path;
            return false;
        }
        imageDisk* vhd = nullptr;
        if (imageDiskVHD::Open(path.c_str(), /*readOnly=*/true, &vhd) != imageDiskVHD::OPEN_SUCCESS || !vhd) {
            if (out_error) *out_error = "Failed to open VHD image: " + path;
            return false;
        }
        vhd->kiosk_write_protected = true;
        vhd->kiosk_host_readonly = true;
        KioskImageDiskSectorDevice* dev = new KioskImageDiskSectorDevice(vhd, /*owns_disk=*/true);
        m_owned_device.reset(dev);
        if (!dev->IsUsable()) {
            if (out_error) *out_error = "VHD image does not use 512-byte sectors: " + path;
            Close();
            return false;
        }
    } else {
        KioskFileSectorDevice* dev = new KioskFileSectorDevice();
        m_owned_device.reset(dev);
        if (!dev->Open(path, writable, out_error)) {
            Close();
            return false;
        }
    }
    if (!MountInternal(m_owned_device.get(), writable, out_error)) {
        Close();
        return false;
    }
    return true;
}

bool KioskFatVolume::OpenImageDisk(imageDisk* non_owning_disk, std::string* out_error) {
    Close();
    KioskImageDiskSectorDevice* dev = new KioskImageDiskSectorDevice(non_owning_disk, /*owns_disk=*/false);
    m_owned_device.reset(dev);
    if (!dev->IsUsable()) {
        if (out_error) *out_error = "Disk does not use 512-byte sectors";
        Close();
        return false;
    }
    if (!MountInternal(dev, false, out_error)) {
        Close();
        return false;
    }
    return true;
}

void KioskFatVolume::Close() {
    DiscardChanges();
    m_mounted = false;
    m_writable = false;
    m_device = nullptr;
    m_owned_device.reset();
}

void KioskFatVolume::DiscardChanges() {
    // Directly written fresh clusters stay unreferenced (free) on disk: nothing to undo.
    m_dirty.clear();
    m_fat_cache.clear();
    m_fresh_clusters.clear();
    m_freed_clusters.clear();
    m_free_cluster_delta = 0;
}

// ============================================================================
// Sector, cluster and FAT primitives
// ============================================================================

bool KioskFatVolume::IsFatSector(uint64_t lba) const {
    const uint64_t start = static_cast<uint64_t>(m_part_lba) + m_reserved_sectors;
    return lba >= start && lba < start + static_cast<uint64_t>(m_num_fats) * m_fat_size_sectors;
}

bool KioskFatVolume::ReadSector(uint64_t lba, uint8_t out[512]) const {
    if (!m_mounted) return false;
    std::map<uint64_t, std::vector<uint8_t>>::const_iterator d = m_dirty.find(lba);
    if (d != m_dirty.end()) {
        std::memcpy(out, d->second.data(), kSector);
        return true;
    }
    if (!IsFatSector(lba)) {
        return m_device->ReadSectors(lba, 1, out);
    }
    std::map<uint64_t, std::vector<uint8_t>>::const_iterator c = m_fat_cache.find(lba);
    if (c != m_fat_cache.end()) {
        std::memcpy(out, c->second.data(), kSector);
        return true;
    }
    if (!m_device->ReadSectors(lba, 1, out)) return false;
    if (m_fat_cache.size() >= kMaxFatCacheSectors) m_fat_cache.erase(m_fat_cache.begin());
    m_fat_cache[lba].assign(out, out + kSector);
    return true;
}

bool KioskFatVolume::StageSector(uint64_t lba, const uint8_t in[512]) {
    if (!IsWritable()) return false;
    const uint64_t data_start = static_cast<uint64_t>(m_part_lba) + m_first_data_sector;
    if (lba >= data_start) {
        const uint64_t rel = (lba - data_start) / m_sectors_per_cluster;
        if (rel <= 0xFFFFFFFDULL && m_fresh_clusters.count(static_cast<uint32_t>(rel + 2U)) != 0) {
            return m_device->WriteSectorsDirect(lba, 1, in); // Unreferenced until commit
        }
    }
    m_dirty[lba].assign(in, in + kSector);
    m_fat_cache.erase(lba);
    return true;
}

uint64_t KioskFatVolume::ClusterToLba(uint32_t cluster) const {
    return static_cast<uint64_t>(m_part_lba) + m_first_data_sector +
           static_cast<uint64_t>(cluster - 2U) * m_sectors_per_cluster;
}

bool KioskFatVolume::IsValidCluster(uint32_t cluster) const {
    return cluster >= 2 && (cluster - 2U) < m_cluster_count;
}

bool KioskFatVolume::IsEndOfChain(uint32_t value) const {
    if (m_fs_type == FilesystemType::FAT12) return value >= 0x0FF8U;
    if (m_fs_type == FilesystemType::FAT16) return value >= 0xFFF8U;
    return (value & 0x0FFFFFFFU) >= 0x0FFFFFF8U;
}

uint32_t KioskFatVolume::EndOfChainValue() const {
    if (m_fs_type == FilesystemType::FAT12) return 0x0FFFU;
    if (m_fs_type == FilesystemType::FAT16) return 0xFFFFU;
    return 0x0FFFFFFFU;
}

uint32_t KioskFatVolume::ResolveDirCluster(uint32_t dir_handle) const {
    if (dir_handle == kRootDir && m_fs_type == FilesystemType::FAT32) return m_root_cluster;
    return dir_handle;
}

const uint8_t* KioskFatVolume::PeekFatSector(uint64_t lba) const {
    std::map<uint64_t, std::vector<uint8_t>>::const_iterator d = m_dirty.find(lba);
    if (d != m_dirty.end()) return d->second.data();
    std::map<uint64_t, std::vector<uint8_t>>::const_iterator c = m_fat_cache.find(lba);
    if (c != m_fat_cache.end()) return c->second.data();
    std::vector<uint8_t> buf(kSector);
    if (!m_device->ReadSectors(lba, 1, buf.data())) return nullptr;
    if (m_fat_cache.size() >= kMaxFatCacheSectors) m_fat_cache.erase(m_fat_cache.begin());
    std::vector<uint8_t>& slot = m_fat_cache[lba];
    slot.swap(buf);
    return slot.data();
}

bool KioskFatVolume::GetFatEntry(uint32_t cluster, uint32_t& out_value) const {
    if (!m_mounted || !IsValidCluster(cluster)) return false;
    const uint64_t fat_lba = static_cast<uint64_t>(m_part_lba) + m_reserved_sectors +
                             static_cast<uint64_t>(m_active_fat) * m_fat_size_sectors;

    if (m_fs_type == FilesystemType::FAT12) {
        const uint64_t off = static_cast<uint64_t>(cluster) + cluster / 2U;
        const uint64_t idx = off / kSector;
        const uint32_t in = static_cast<uint32_t>(off % kSector);
        if (idx >= m_fat_size_sectors) return false;
        const uint8_t* sec = PeekFatSector(fat_lba + idx);
        if (!sec) return false;
        uint16_t raw = 0;
        if (in == kSector - 1U) {
            const uint8_t lo = sec[kSector - 1U]; // Copy before the next peek may evict this sector
            if (idx + 1U >= m_fat_size_sectors) return false;
            const uint8_t* next = PeekFatSector(fat_lba + idx + 1U);
            if (!next) return false;
            raw = static_cast<uint16_t>(lo | (static_cast<uint16_t>(next[0]) << 8));
        } else {
            raw = ReadLe16(sec + in);
        }
        out_value = (cluster & 1U) ? static_cast<uint32_t>(raw >> 4) : static_cast<uint32_t>(raw & 0x0FFFU);
        return true;
    }

    const uint64_t off = static_cast<uint64_t>(cluster) * (m_fs_type == FilesystemType::FAT16 ? 2U : 4U);
    const uint64_t idx = off / kSector;
    const uint32_t in = static_cast<uint32_t>(off % kSector);
    if (idx >= m_fat_size_sectors) return false;
    const uint8_t* sec = PeekFatSector(fat_lba + idx);
    if (!sec) return false;
    out_value = (m_fs_type == FilesystemType::FAT16) ? ReadLe16(sec + in) : (ReadLe32(sec + in) & 0x0FFFFFFFU);
    return true;
}

bool KioskFatVolume::SetFatEntry(uint32_t cluster, uint32_t value) {
    if (!IsWritable() || !IsValidCluster(cluster)) return false;
    for (uint32_t f = 0; f < m_num_fats; ++f) {
        if (!m_fat_mirroring && f != m_active_fat) continue; // Only the active FAT is live
        const uint64_t fat_lba = static_cast<uint64_t>(m_part_lba) + m_reserved_sectors +
                                 static_cast<uint64_t>(f) * m_fat_size_sectors;
        uint8_t sec[kSector];

        if (m_fs_type == FilesystemType::FAT12) {
            const uint64_t off = static_cast<uint64_t>(cluster) + cluster / 2U;
            const uint64_t idx = off / kSector;
            const uint32_t in = static_cast<uint32_t>(off % kSector);
            if (idx >= m_fat_size_sectors || !ReadSector(fat_lba + idx, sec)) return false;
            const uint16_t v = static_cast<uint16_t>(value & 0x0FFFU);
            if (in == kSector - 1U) {
                uint8_t next[kSector];
                if (idx + 1U >= m_fat_size_sectors || !ReadSector(fat_lba + idx + 1U, next)) return false;
                uint16_t raw = static_cast<uint16_t>(sec[kSector - 1U] | (static_cast<uint16_t>(next[0]) << 8));
                raw = (cluster & 1U) ? static_cast<uint16_t>((raw & 0x000FU) | (v << 4))
                                     : static_cast<uint16_t>((raw & 0xF000U) | v);
                sec[kSector - 1U] = static_cast<uint8_t>(raw & 0xFFU);
                next[0] = static_cast<uint8_t>(raw >> 8);
                if (!StageSector(fat_lba + idx, sec) || !StageSector(fat_lba + idx + 1U, next)) return false;
            } else {
                uint16_t raw = ReadLe16(sec + in);
                raw = (cluster & 1U) ? static_cast<uint16_t>((raw & 0x000FU) | (v << 4))
                                     : static_cast<uint16_t>((raw & 0xF000U) | v);
                WriteLe16(sec + in, raw);
                if (!StageSector(fat_lba + idx, sec)) return false;
            }
            continue;
        }

        const uint64_t off = static_cast<uint64_t>(cluster) * (m_fs_type == FilesystemType::FAT16 ? 2U : 4U);
        const uint64_t idx = off / kSector;
        const uint32_t in = static_cast<uint32_t>(off % kSector);
        if (idx >= m_fat_size_sectors || !ReadSector(fat_lba + idx, sec)) return false;
        if (m_fs_type == FilesystemType::FAT16) {
            WriteLe16(sec + in, static_cast<uint16_t>(value & 0xFFFFU));
        } else {
            const uint32_t old = ReadLe32(sec + in);
            WriteLe32(sec + in, (old & 0xF0000000U) | (value & 0x0FFFFFFFU));
        }
        if (!StageSector(fat_lba + idx, sec)) return false;
    }
    return true;
}

bool KioskFatVolume::AllocateChain(uint32_t count, std::vector<uint32_t>& out_chain) {
    out_chain.clear();
    if (count == 0) return true;
    if (!IsWritable()) return false;
    const uint32_t end = m_alloc_limit + 2U;
    uint32_t c = (m_alloc_hint >= 2 && m_alloc_hint < end) ? m_alloc_hint : 2U;
    for (uint32_t scanned = 0; scanned < m_alloc_limit && out_chain.size() < count; ++scanned) {
        uint32_t v = 0;
        // Clusters freed during this transaction stay unused: on disk they still hold live data.
        if (GetFatEntry(c, v) && v == 0 && m_freed_clusters.count(c) == 0) {
            out_chain.push_back(c);
        }
        if (++c >= end) c = 2U;
    }
    if (out_chain.size() < count) {
        out_chain.clear();
        return false;
    }
    for (size_t i = 0; i < out_chain.size(); ++i) {
        const uint32_t next = (i + 1 < out_chain.size()) ? out_chain[i + 1] : EndOfChainValue();
        if (!SetFatEntry(out_chain[i], next)) {
            out_chain.clear();
            return false;
        }
        m_fresh_clusters.insert(out_chain[i]);
    }
    m_free_cluster_delta -= static_cast<int64_t>(out_chain.size());
    m_alloc_hint = out_chain.back() + 1U;
    if (m_alloc_hint >= end) m_alloc_hint = 2U;
    return true;
}

void KioskFatVolume::FreeChain(const std::vector<uint32_t>& chain) {
    for (size_t i = 0; i < chain.size(); ++i) {
        if (SetFatEntry(chain[i], 0)) {
            m_freed_clusters.insert(chain[i]);
            m_fresh_clusters.erase(chain[i]);
            ++m_free_cluster_delta;
        }
    }
}

bool KioskFatVolume::CollectChain(uint32_t first_cluster, std::vector<uint32_t>& out_chain) const {
    out_chain.clear();
    std::unordered_set<uint32_t> visited;
    uint32_t cur = first_cluster;
    while (true) {
        if (!IsValidCluster(cur) || !visited.insert(cur).second || out_chain.size() >= kMaxChainClusters) {
            return false;
        }
        out_chain.push_back(cur);
        uint32_t next = 0;
        if (!GetFatEntry(cur, next)) return false;
        if (IsEndOfChain(next)) return true;
        cur = next;
    }
}

bool KioskFatVolume::WriteClusterData(uint32_t cluster, const uint8_t* data) {
    const uint64_t lba = ClusterToLba(cluster);
    if (m_fresh_clusters.count(cluster) != 0) {
        return m_device->WriteSectorsDirect(lba, m_sectors_per_cluster, data);
    }
    for (uint32_t s = 0; s < m_sectors_per_cluster; ++s) {
        if (!StageSector(lba + s, data + static_cast<size_t>(s) * kSector)) return false;
    }
    return true;
}

// ============================================================================
// Directory reading
// ============================================================================

bool KioskFatVolume::GetDirectorySectors(uint32_t dir_handle, std::vector<uint64_t>& out_lbas,
                                         uint32_t& out_last_cluster) const {
    out_lbas.clear();
    out_last_cluster = 0;
    if (!m_mounted) return false;

    if (dir_handle == kRootDir && m_fs_type != FilesystemType::FAT32) {
        const uint64_t start = static_cast<uint64_t>(m_part_lba) + m_reserved_sectors +
                               static_cast<uint64_t>(m_num_fats) * m_fat_size_sectors;
        for (uint32_t s = 0; s < m_root_dir_sectors; ++s) out_lbas.push_back(start + s);
        return !out_lbas.empty();
    }

    std::vector<uint32_t> chain;
    if (!CollectChain(ResolveDirCluster(dir_handle), chain)) return false;
    for (size_t i = 0; i < chain.size(); ++i) {
        const uint64_t base = ClusterToLba(chain[i]);
        for (uint32_t s = 0; s < m_sectors_per_cluster; ++s) out_lbas.push_back(base + s);
    }
    out_last_cluster = chain.back();
    return true;
}

bool KioskFatVolume::ParseDirectorySectors(const std::vector<uint64_t>& lbas,
                                           std::vector<FatDirEntry>& out_entries) const {
    out_entries.clear();
    bool lfn_active = false;
    uint8_t lfn_checksum = 0;
    uint8_t lfn_expected_ord = 0;
    std::vector<std::vector<uint16_t>> lfn_chunks;
    uint8_t sec[kSector];

    for (size_t s = 0; s < lbas.size(); ++s) {
        if (!ReadSector(lbas[s], sec)) return false;
        for (uint32_t off = 0; off < kSector; off += 32U) {
            const uint8_t* e = sec + off;
            if (e[0] == 0x00) return true; // End of directory
            if (e[0] == 0xE5) {
                lfn_active = false;
                lfn_chunks.clear();
                continue;
            }
            const uint8_t attr = e[11];
            if ((attr & 0x3FU) == 0x0FU) {
                const uint8_t seq = e[0] & 0x1FU;
                const bool is_last = (e[0] & 0x40U) != 0;
                if (seq == 0 || seq > 20) {
                    lfn_active = false;
                    lfn_chunks.clear();
                    continue;
                }
                if (is_last) {
                    lfn_chunks.assign(seq, std::vector<uint16_t>());
                    lfn_checksum = e[13];
                    lfn_expected_ord = seq;
                    lfn_active = true;
                } else if (!lfn_active || e[13] != lfn_checksum || seq != lfn_expected_ord) {
                    lfn_active = false;
                    lfn_chunks.clear();
                    continue;
                }
                std::vector<uint16_t> chars(13, 0);
                for (int k = 0; k < 5; ++k) chars[k] = ReadLe16(e + 1 + k * 2);
                for (int k = 0; k < 6; ++k) chars[5 + k] = ReadLe16(e + 14 + k * 2);
                for (int k = 0; k < 2; ++k) chars[11 + k] = ReadLe16(e + 28 + k * 2);
                lfn_chunks[seq - 1] = chars;
                lfn_expected_ord = static_cast<uint8_t>(seq - 1);
                continue;
            }
            if (attr & 0x08U) { // Volume label
                lfn_active = false;
                lfn_chunks.clear();
                continue;
            }

            const std::string short_name = ShortNameToDisplay(e);
            std::string full_name = short_name;
            if (lfn_active && lfn_expected_ord == 0 && ShortNameChecksum(e) == lfn_checksum) {
                std::vector<uint16_t> combined;
                for (size_t c = 0; c < lfn_chunks.size(); ++c) {
                    combined.insert(combined.end(), lfn_chunks[c].begin(), lfn_chunks[c].end());
                }
                const std::string decoded = Utf16ToUtf8(combined);
                if (!decoded.empty()) full_name = decoded;
            }
            lfn_active = false;
            lfn_chunks.clear();
            if (short_name == "." || short_name == "..") continue;

            FatDirEntry entry;
            entry.name = full_name;
            entry.short_name = ToUpperAscii(short_name);
            entry.is_directory = (attr & 0x10U) != 0;
            entry.attributes = attr;
            const uint32_t hi = (m_fs_type == FilesystemType::FAT32) ? static_cast<uint32_t>(ReadLe16(e + 20)) : 0U;
            entry.first_cluster = ((hi << 16) | ReadLe16(e + 26)) & 0x0FFFFFFFU;
            entry.file_size = ReadLe32(e + 28);
            entry.entry_lba = lbas[s];
            entry.entry_offset = off;
            out_entries.push_back(entry);
        }
    }
    return true;
}

bool KioskFatVolume::ListDirectoryCluster(uint32_t dir_cluster, std::vector<FatDirEntry>& out_entries) const {
    out_entries.clear();
    if (!m_mounted) return false;

    if (dir_cluster == kRootDir && m_fs_type != FilesystemType::FAT32) {
        std::vector<uint64_t> lbas;
        uint32_t last = 0;
        return GetDirectorySectors(kRootDir, lbas, last) && ParseDirectorySectors(lbas, out_entries);
    }

    // Cluster-backed directory: stop at the end-of-directory marker so a damaged chain
    // beyond it does not hide valid entries.
    uint32_t cur = ResolveDirCluster(dir_cluster);
    std::unordered_set<uint32_t> visited;
    std::vector<uint64_t> lbas;
    uint8_t sec[kSector];
    const size_t kMaxDirClusters = 65536U;
    while (true) {
        if (!IsValidCluster(cur) || !visited.insert(cur).second || visited.size() > kMaxDirClusters) return false;
        const uint64_t base = ClusterToLba(cur);
        bool saw_eod = false;
        for (uint32_t s = 0; s < m_sectors_per_cluster && !saw_eod; ++s) {
            if (!ReadSector(base + s, sec)) return false;
            lbas.push_back(base + s);
            for (uint32_t off = 0; off < kSector; off += 32U) {
                if (sec[off] == 0x00) { saw_eod = true; break; }
            }
        }
        if (saw_eod) break;
        uint32_t next = 0;
        if (!GetFatEntry(cur, next)) return false;
        if (IsEndOfChain(next)) break;
        cur = next;
    }
    return ParseDirectorySectors(lbas, out_entries);
}

bool KioskFatVolume::ListDirectory(const FatDirEntry* dir, std::vector<FatDirEntry>& out_entries) const {
    out_entries.clear();
    if (dir == nullptr) return ListDirectoryCluster(kRootDir, out_entries);
    if (!dir->is_directory || !IsValidCluster(dir->first_cluster)) return false;
    return ListDirectoryCluster(dir->first_cluster, out_entries);
}

bool KioskFatVolume::FindInDirectory(uint32_t dir_handle, const std::string& name, FatDirEntry& out_entry) const {
    std::vector<FatDirEntry> entries;
    if (!ListDirectoryCluster(dir_handle, entries)) return false;
    const std::string target = ToUpperAscii(name);
    for (size_t i = 0; i < entries.size(); ++i) {
        if (ToUpperAscii(entries[i].name) == target || entries[i].short_name == target) {
            out_entry = entries[i];
            return true;
        }
    }
    return false;
}

bool KioskFatVolume::FindPath(const std::string& guest_path, FatDirEntry& out_entry) const {
    if (!m_mounted) return false;
    const std::string trimmed = TrimAscii(guest_path);
    if (trimmed.size() >= 2 && trimmed[1] == ':' &&
        std::toupper(static_cast<unsigned char>(trimmed[0])) != 'C') {
        return false; // Only drive-less or C: paths belong to the mounted primary volume
    }
    const std::vector<std::string> parts = SplitGuestPath(trimmed);
    if (parts.empty()) return false;

    uint32_t dir = kRootDir;
    FatDirEntry cur;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!FindInDirectory(dir, parts[i], cur)) return false;
        if (i + 1 < parts.size()) {
            if (!cur.is_directory || !IsValidCluster(cur.first_cluster)) return false;
            dir = cur.first_cluster;
        }
    }
    out_entry = cur;
    return true;
}

bool KioskFatVolume::PathExists(const std::string& guest_path) const {
    FatDirEntry e;
    return FindPath(guest_path, e);
}

bool KioskFatVolume::ReadFile(const FatDirEntry& entry, std::vector<uint8_t>& out_data, size_t max_bytes) const {
    out_data.clear();
    if (!m_mounted || entry.is_directory) return false;
    if (entry.file_size == 0) return true;
    if (entry.first_cluster < 2) return false;

    const size_t target = (std::min)(static_cast<size_t>(entry.file_size), max_bytes);
    out_data.reserve(target);
    std::unordered_set<uint32_t> visited;
    uint32_t cur = entry.first_cluster;
    uint8_t sec[kSector];

    while (!IsEndOfChain(cur) && out_data.size() < target) {
        if (!IsValidCluster(cur)) return false;
        if (!visited.insert(cur).second) break; // Cycle protection
        const uint64_t lba = ClusterToLba(cur);
        for (uint32_t s = 0; s < m_sectors_per_cluster && out_data.size() < target; ++s) {
            if (!ReadSector(lba + s, sec)) return false;
            const size_t take = (std::min)(static_cast<size_t>(kSector), target - out_data.size());
            out_data.insert(out_data.end(), sec, sec + take);
        }
        if (out_data.size() >= target) break;
        uint32_t next = 0;
        if (!GetFatEntry(cur, next)) return false;
        if (!IsEndOfChain(next) && !IsValidCluster(next)) return false; // Free or out-of-range link
        cur = next;
    }
    return out_data.size() == target;
}

// ============================================================================
// Writing
// ============================================================================

uint64_t KioskFatVolume::ComputeFreeBytes() const {
    if (!m_mounted) return 0;
    uint64_t free_clusters = 0;
    const uint32_t end = (m_writable ? m_alloc_limit : m_cluster_count) + 2U;
    for (uint32_t c = 2; c < end; ++c) {
        uint32_t v = 0;
        if (!GetFatEntry(c, v)) break;
        if (v == 0 && m_freed_clusters.count(c) == 0) ++free_clusters;
    }
    return free_clusters * m_sectors_per_cluster * kSector;
}

bool KioskFatVolume::ShortNameExists(const std::vector<uint64_t>& lbas, const uint8_t sfn[11]) const {
    uint8_t sec[kSector];
    for (size_t s = 0; s < lbas.size(); ++s) {
        if (!ReadSector(lbas[s], sec)) return false;
        for (uint32_t off = 0; off < kSector; off += 32U) {
            const uint8_t* e = sec + off;
            if (e[0] == 0x00) return false;
            if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08U)) continue;
            if (std::memcmp(e, sfn, 11) == 0) return true;
        }
    }
    return false;
}

bool KioskFatVolume::AddDirectoryEntry(uint32_t dir_handle, const std::string& name, uint8_t attr,
                                       uint32_t first_cluster, uint32_t file_size, bool allow_lfn,
                                       std::string& out_error) {
    std::vector<uint64_t> lbas;
    uint32_t last_cluster = 0;
    if (!GetDirectorySectors(dir_handle, lbas, last_cluster)) {
        out_error = "Failed to enumerate directory sectors";
        return false;
    }

    std::vector<uint16_t> name16 = Utf8ToUtf16(name);
    if (name16.size() > 255U) name16.resize(255U);

    uint8_t sfn[11];
    bool lossy = false;
    Format83Name(name, sfn, lossy);
    if (ShortNameExists(lbas, sfn)) {
        bool resolved = false;
        for (int n = 1; n <= 9999 && !resolved; ++n) {
            const std::string tilde = "~" + std::to_string(n);
            const size_t tlen = tilde.size();
            uint8_t cand[11];
            std::memcpy(cand, sfn, 11);
            size_t prefix_len = 8 - tlen;
            for (size_t k = 0; k < prefix_len; ++k) {
                if (cand[k] == ' ') { prefix_len = k; break; }
            }
            std::memcpy(cand + prefix_len, tilde.data(), tlen);
            for (size_t k = prefix_len + tlen; k < 8; ++k) cand[k] = ' ';
            if (!ShortNameExists(lbas, cand)) {
                std::memcpy(sfn, cand, 11);
                lossy = true;
                resolved = true;
            }
        }
        if (!resolved) {
            out_error = "Could not generate a unique 8.3 name for " + name;
            return false;
        }
    }

    // Raw 32-byte slots: optional LFN entries (highest ordinal first), then the SFN entry
    std::vector<std::vector<uint8_t>> slots;
    if (allow_lfn && lossy) {
        const uint8_t csum = ShortNameChecksum(sfn);
        const size_t lfn_count = (name16.size() + 12U) / 13U;
        static const int kOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (size_t idx = lfn_count; idx >= 1; --idx) {
            std::vector<uint8_t> lfn(32, 0xFF);
            lfn[0] = static_cast<uint8_t>(idx | ((idx == lfn_count) ? 0x40U : 0x00U));
            lfn[11] = 0x0F;
            lfn[12] = 0x00;
            lfn[13] = csum;
            lfn[26] = 0x00;
            lfn[27] = 0x00;
            const size_t base = (idx - 1U) * 13U;
            for (int k = 0; k < 13; ++k) {
                const size_t ci = base + static_cast<size_t>(k);
                uint16_t u = 0xFFFFU;
                if (ci < name16.size()) u = name16[ci];
                else if (ci == name16.size()) u = 0x0000U;
                WriteLe16(lfn.data() + kOffsets[k], u);
            }
            slots.push_back(lfn);
        }
    }
    std::vector<uint8_t> sfn_ent(32, 0);
    std::memcpy(sfn_ent.data(), sfn, 11);
    sfn_ent[11] = attr;
    StampDosDateTime(sfn_ent.data());
    if (m_fs_type == FilesystemType::FAT32) {
        WriteLe16(sfn_ent.data() + 20, static_cast<uint16_t>((first_cluster >> 16) & 0xFFFFU));
    }
    WriteLe16(sfn_ent.data() + 26, static_cast<uint16_t>(first_cluster & 0xFFFFU));
    WriteLe32(sfn_ent.data() + 28, file_size);
    slots.push_back(sfn_ent);

    // Contiguous free run, never leaving a 0x00 end marker before the new entries
    const size_t need = slots.size();
    const size_t total_slots = lbas.size() * 16U;
    size_t run_start = 0;
    size_t run_len = 0;
    bool found = false;
    size_t eod_slot = total_slots;
    uint8_t sec[kSector];
    for (size_t slot = 0; slot < total_slots; ++slot) {
        if (slot % 16U == 0 && !ReadSector(lbas[slot / 16U], sec)) {
            out_error = "Failed to read directory sector";
            return false;
        }
        const uint8_t b0 = sec[(slot % 16U) * 32U];
        if (b0 == 0x00) {
            eod_slot = (run_len > 0) ? run_start : slot;
            if (total_slots - eod_slot >= need) {
                run_start = eod_slot;
                found = true;
            }
            break;
        } else if (b0 == 0xE5) {
            if (run_len == 0) run_start = slot;
            if (++run_len >= need) {
                found = true;
                break;
            }
        } else {
            run_len = 0;
        }
    }

    if (!found) {
        if (dir_handle == kRootDir && m_fs_type != FilesystemType::FAT32) {
            out_error = "Root directory is full";
            return false;
        }
        const size_t start_slot = eod_slot;
        const size_t remaining = (start_slot < total_slots) ? (total_slots - start_slot) : 0;
        const size_t additional = need - remaining;
        const size_t per_cluster = static_cast<size_t>(m_sectors_per_cluster) * 16U;
        const uint32_t extra_count = static_cast<uint32_t>((additional + per_cluster - 1U) / per_cluster);
        std::vector<uint32_t> extra;
        if (!AllocateChain(extra_count, extra)) {
            out_error = "Insufficient free space to grow directory";
            return false;
        }
        const std::vector<uint8_t> zero(static_cast<size_t>(m_sectors_per_cluster) * kSector, 0);
        for (size_t c = 0; c < extra.size(); ++c) {
            if (!WriteClusterData(extra[c], zero.data())) {
                FreeChain(extra);
                out_error = "Failed to initialize directory cluster";
                return false;
            }
        }
        if (!SetFatEntry(last_cluster, extra[0])) {
            FreeChain(extra);
            out_error = "Failed to link directory cluster";
            return false;
        }
        for (size_t c = 0; c < extra.size(); ++c) {
            const uint64_t base = ClusterToLba(extra[c]);
            for (uint32_t s = 0; s < m_sectors_per_cluster; ++s) lbas.push_back(base + s);
        }
        run_start = start_slot;
    }

    for (size_t i = 0; i < need; ++i) {
        const size_t slot = run_start + i;
        const uint64_t lba = lbas[slot / 16U];
        if (!ReadSector(lba, sec)) {
            out_error = "Failed to read directory sector";
            return false;
        }
        std::memcpy(sec + (slot % 16U) * 32U, slots[i].data(), 32);
        if (!StageSector(lba, sec)) {
            out_error = "Failed to stage directory sector";
            return false;
        }
    }
    return true;
}

bool KioskFatVolume::CreateSubdirectory(uint32_t parent_dir, const std::string& name, bool allow_lfn,
                                        uint32_t& out_cluster, std::string& out_error) {
    std::vector<uint32_t> cl;
    if (!AllocateChain(1, cl)) {
        out_error = "Insufficient free space to create directory " + name;
        return false;
    }
    out_cluster = cl[0];

    std::vector<uint8_t> buf(static_cast<size_t>(m_sectors_per_cluster) * kSector, 0);
    std::memset(buf.data(), ' ', 11);
    buf[0] = '.';
    buf[11] = 0x10;
    StampDosDateTime(buf.data());
    std::memset(buf.data() + 32, ' ', 11);
    buf[32] = '.';
    buf[33] = '.';
    buf[32 + 11] = 0x10;
    StampDosDateTime(buf.data() + 32);
    if (m_fs_type == FilesystemType::FAT32) {
        WriteLe16(buf.data() + 20, static_cast<uint16_t>((out_cluster >> 16) & 0xFFFFU));
        WriteLe16(buf.data() + 32 + 20, static_cast<uint16_t>((parent_dir >> 16) & 0xFFFFU));
    }
    WriteLe16(buf.data() + 26, static_cast<uint16_t>(out_cluster & 0xFFFFU));
    WriteLe16(buf.data() + 32 + 26, static_cast<uint16_t>(parent_dir & 0xFFFFU)); // 0 = root

    if (!WriteClusterData(out_cluster, buf.data())) {
        FreeChain(cl);
        out_error = "Failed to initialize subdirectory cluster";
        return false;
    }
    if (!AddDirectoryEntry(parent_dir, name, 0x10, out_cluster, 0, allow_lfn, out_error)) {
        FreeChain(cl);
        return false;
    }
    return true;
}

bool KioskFatVolume::EnsureChildDirectory(uint32_t parent_dir, const std::string& name, bool allow_lfn,
                                          uint32_t& out_child_dir, std::string& out_error) {
    if (!IsWritable()) {
        out_error = "Volume is not writable";
        return false;
    }
    FatDirEntry existing;
    if (FindInDirectory(parent_dir, name, existing)) {
        if (!existing.is_directory || !IsValidCluster(existing.first_cluster)) {
            out_error = "Path component is a file, not a directory: " + name;
            return false;
        }
        out_child_dir = existing.first_cluster;
        return true;
    }
    return CreateSubdirectory(parent_dir, name, allow_lfn, out_child_dir, out_error);
}

bool KioskFatVolume::EnsureDirectoryPath(const std::string& guest_path, bool allow_lfn,
                                         uint32_t& out_dir_cluster, std::string& out_error) {
    const std::vector<std::string> segs = SplitGuestPath(guest_path);
    uint32_t cur = kRootDir;
    for (size_t i = 0; i < segs.size(); ++i) {
        uint32_t next = 0;
        if (!EnsureChildDirectory(cur, segs[i], allow_lfn, next, out_error)) return false;
        cur = next;
    }
    out_dir_cluster = cur;
    return true;
}

bool KioskFatVolume::AddFile(uint32_t parent_dir, const std::string& name, const std::vector<uint8_t>& data,
                             bool allow_lfn, std::string& out_error) {
    if (!IsWritable()) {
        out_error = "Volume is not writable";
        return false;
    }
    FatDirEntry existing;
    if (FindInDirectory(parent_dir, name, existing)) {
        out_error = existing.is_directory ? ("Cannot overwrite directory with file: " + name)
                                          : ("Target file already exists on HDD image: " + name);
        return false;
    }
    const size_t cluster_bytes = static_cast<size_t>(m_sectors_per_cluster) * kSector;
    const uint32_t needed = static_cast<uint32_t>((data.size() + cluster_bytes - 1U) / cluster_bytes);
    std::vector<uint32_t> chain;
    if (!AllocateChain(needed, chain)) {
        out_error = "Insufficient free clusters while writing " + name;
        return false;
    }
    std::vector<uint8_t> buf(cluster_bytes, 0);
    for (size_t i = 0; i < chain.size(); ++i) {
        std::fill(buf.begin(), buf.end(), 0);
        const size_t off = i * cluster_bytes;
        std::memcpy(buf.data(), data.data() + off, (std::min)(cluster_bytes, data.size() - off));
        if (!WriteClusterData(chain[i], buf.data())) {
            FreeChain(chain);
            out_error = "Disk write error while writing " + name;
            return false;
        }
    }
    const uint32_t first = chain.empty() ? 0 : chain[0];
    if (!AddDirectoryEntry(parent_dir, name, 0x20, first, static_cast<uint32_t>(data.size()), allow_lfn, out_error)) {
        FreeChain(chain);
        return false;
    }
    return true;
}

bool KioskFatVolume::WriteFileReplacing(uint32_t parent_dir, const std::string& name,
                                        const std::vector<uint8_t>& data, std::string& out_error) {
    if (!IsWritable()) {
        out_error = "Volume is not writable";
        return false;
    }
    FatDirEntry entry;
    if (!FindInDirectory(parent_dir, name, entry)) {
        return AddFile(parent_dir, name, data, false, out_error);
    }
    if (entry.is_directory) {
        out_error = "Cannot overwrite directory with file: " + name;
        return false;
    }

    const size_t cluster_bytes = static_cast<size_t>(m_sectors_per_cluster) * kSector;
    const size_t needed = (data.size() + cluster_bytes - 1U) / cluster_bytes;
    std::vector<uint32_t> chain;
    if (entry.first_cluster >= 2 && !CollectChain(entry.first_cluster, chain)) {
        out_error = "Existing file has a corrupt cluster chain: " + name;
        return false;
    }
    if (chain.size() < needed) {
        std::vector<uint32_t> extra;
        if (!AllocateChain(static_cast<uint32_t>(needed - chain.size()), extra)) {
            out_error = "Insufficient free clusters while writing " + name;
            return false;
        }
        if (!chain.empty() && !SetFatEntry(chain.back(), extra[0])) {
            out_error = "Failed to extend cluster chain of " + name;
            return false;
        }
        chain.insert(chain.end(), extra.begin(), extra.end());
    } else if (chain.size() > needed) {
        const std::vector<uint32_t> tail(chain.begin() + static_cast<std::ptrdiff_t>(needed), chain.end());
        if (needed > 0 && !SetFatEntry(chain[needed - 1U], EndOfChainValue())) {
            out_error = "Failed to truncate cluster chain of " + name;
            return false;
        }
        FreeChain(tail);
        chain.resize(needed);
    }

    std::vector<uint8_t> buf(cluster_bytes, 0);
    for (size_t i = 0; i < chain.size(); ++i) {
        std::fill(buf.begin(), buf.end(), 0);
        const size_t off = i * cluster_bytes;
        std::memcpy(buf.data(), data.data() + off, (std::min)(cluster_bytes, data.size() - off));
        if (!WriteClusterData(chain[i], buf.data())) {
            out_error = "Disk write error while writing " + name;
            return false;
        }
    }

    uint8_t sec[kSector];
    if (!ReadSector(entry.entry_lba, sec)) {
        out_error = "Failed to read directory entry of " + name;
        return false;
    }
    uint8_t* e = sec + entry.entry_offset;
    const uint32_t first = chain.empty() ? 0 : chain[0];
    if (m_fs_type == FilesystemType::FAT32) {
        WriteLe16(e + 20, static_cast<uint16_t>((first >> 16) & 0xFFFFU));
    }
    WriteLe16(e + 26, static_cast<uint16_t>(first & 0xFFFFU));
    WriteLe32(e + 28, static_cast<uint32_t>(data.size()));
    if (!StageSector(entry.entry_lba, sec)) {
        out_error = "Failed to stage directory entry of " + name;
        return false;
    }
    return true;
}

bool KioskFatVolume::StageFsInfoUpdate() {
    if (m_fs_type != FilesystemType::FAT32 || m_fsinfo_sector == 0) return true;
    const uint64_t lba = static_cast<uint64_t>(m_part_lba) + m_fsinfo_sector;
    uint8_t fsi[kSector];
    if (!ReadSector(lba, fsi)) return true; // Optional structure; guests recompute when absent
    if (ReadLe32(fsi) != 0x41615252U || ReadLe32(fsi + 484) != 0x61417272U) return true;

    const uint32_t free_count = ReadLe32(fsi + 488);
    if (free_count != 0xFFFFFFFFU) {
        int64_t updated = static_cast<int64_t>(free_count) + m_free_cluster_delta;
        if (updated < 0 || updated > static_cast<int64_t>(m_cluster_count)) {
            updated = 0xFFFFFFFFLL; // Inconsistent hint: mark unknown so the guest recounts
        }
        WriteLe32(fsi + 488, static_cast<uint32_t>(updated));
    }
    WriteLe32(fsi + 492, m_alloc_hint);
    return StageSector(lba, fsi);
}

bool KioskFatVolume::Commit(std::string* out_error) {
    if (!IsWritable()) {
        if (out_error) *out_error = "Volume is not writable";
        return false;
    }
    if (m_dirty.empty() && m_fresh_clusters.empty() && m_freed_clusters.empty()) return true;
    if (!StageFsInfoUpdate() || !m_device->CommitSectorsAtomically(m_dirty)) {
        // The device has rolled back (or, on a simulated power cut, left its journal). The
        // staged state no longer matches the disk, so this volume must not be used again.
        if (out_error) *out_error = "Failed to commit FAT changes to disk image";
        m_device->AbandonAfterFailedCommit();
        DiscardChanges();
        m_mounted = false;
        m_writable = false;
        return false;
    }
    m_dirty.clear();
    m_fat_cache.clear();
    m_fresh_clusters.clear();
    m_freed_clusters.clear();
    m_free_cluster_delta = 0;
    return true;
}

#endif // defined(C_KIOSK)
