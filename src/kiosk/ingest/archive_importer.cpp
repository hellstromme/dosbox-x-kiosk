#include "config.h"

#if defined(C_KIOSK)

#include "archive_importer.h"
#include "image_crawler.h"
#include "signature_matcher.h"
#include "../kiosk_db.h"
#include "../machine/archetypes.h"
#include "logging.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <zlib.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// ============================================================================
// String & Path Helpers (C++14, Win32 UTF-8 + POSIX)
// ============================================================================

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

std::string ToUpperAscii(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    }
    return out;
}

std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool EndsWithLower(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return ToLowerAscii(s.substr(s.size() - suffix.size())) == suffix;
}

bool StartsWithLower(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    return ToLowerAscii(s.substr(0, prefix.size())) == prefix;
}

std::string NormalizeSlashes(const std::string& p) {
    std::string out = p;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\\') out[i] = '/';
    }
    while (out.size() > 1 && out.back() == '/') {
        if (out.size() == 3 && out[1] == ':') break; // keep "C:/"
        out.pop_back();
    }
    return out;
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return NormalizeSlashes(b);
    if (b.empty()) return NormalizeSlashes(a);
    std::string na = NormalizeSlashes(a);
    std::string nb = NormalizeSlashes(b);
    while (!nb.empty() && nb.front() == '/') nb.erase(nb.begin());
    if (na.back() == '/') return na + nb;
    return na + "/" + nb;
}

// Preserves native POSIX entry names (including literal backslashes) during filesystem traversal
std::string JoinNativePath(const std::string& dir, const std::string& entry_name) {
#if defined(_WIN32)
    return JoinPath(dir, entry_name);
#else
    if (dir.empty()) return entry_name;
    if (dir.back() == '/') return dir + entry_name;
    return dir + "/" + entry_name;
#endif
}

std::string BasenameOf(const std::string& p) {
    std::string np = NormalizeSlashes(p);
    size_t pos = np.find_last_of('/');
    return (pos == std::string::npos) ? np : np.substr(pos + 1);
}

std::string DirnameOf(const std::string& p) {
    std::string np = NormalizeSlashes(p);
    size_t pos = np.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return np.substr(0, pos);
}

std::string StemOf(const std::string& filename) {
    std::string base = BasenameOf(filename);
    size_t dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void WriteLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void WriteLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

std::string GenerateUniqueId(const std::string& prefix, const std::string& seed) {
    static uint32_t s_counter = 0;
    ++s_counter;
    std::ostringstream ss;
    ss << seed << "|" << static_cast<unsigned long long>(std::time(nullptr)) << "|" << s_counter;
    std::string combined = ss.str();
    uint32_t h1 = SignatureMatcher::ComputeCrc32(combined.data(), combined.size());
    std::string rev(combined.rbegin(), combined.rend());
    uint32_t h2 = SignatureMatcher::ComputeCrc32(rev.data(), rev.size());
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s-%08x-%04x", prefix.c_str(), h1, h2 & 0xFFFFU);
    return std::string(buf);
}

#if defined(_WIN32)
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        needed = MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        if (needed <= 0) return std::wstring();
        std::wstring w(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), &w[0], needed);
        return w;
    }
    std::wstring w(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], needed);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string s(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], needed, nullptr, nullptr);
    return s;
}
#endif

bool PathExists(const std::string& path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesW(Utf8ToWide(path).c_str());
    return (attr != INVALID_FILE_ATTRIBUTES);
#else
    struct stat st;
    return (stat(path.c_str(), &st) == 0);
#endif
}

bool IsDirectory(const std::string& path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesW(Utf8ToWide(path).c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0);
#else
    struct stat st;
    return (stat(path.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
#endif
}

uint64_t GetFileSizeBytes(const std::string& path) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(Utf8ToWide(path).c_str(), GetFileExInfoStandard, &fad)) return 0;
    ULARGE_INTEGER sz;
    sz.LowPart = fad.nFileSizeLow;
    sz.HighPart = fad.nFileSizeHigh;
    return sz.QuadPart;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
#endif
}

bool MakeDirSingle(const std::string& dir) {
    if (dir.empty() || IsDirectory(dir)) return true;
#if defined(_WIN32)
    if (CreateDirectoryW(Utf8ToWide(dir).c_str(), nullptr)) return true;
    return (GetLastError() == ERROR_ALREADY_EXISTS) && IsDirectory(dir);
#else
    if (mkdir(dir.c_str(), 0755) == 0) return true;
    return (errno == EEXIST) && IsDirectory(dir);
#endif
}

bool MakeDirRecursive(const std::string& path) {
    std::string np = NormalizeSlashes(path);
    if (np.empty() || IsDirectory(np)) return true;

    std::string cur;
    size_t pos = 0;
    if (np.size() >= 2 && std::isalpha(static_cast<unsigned char>(np[0])) && np[1] == ':') {
        cur = np.substr(0, 2);
        pos = 2;
        if (pos < np.size() && np[pos] == '/') {
            cur.push_back('/');
            ++pos;
        }
    } else if (!np.empty() && np[0] == '/') {
        cur = "/";
        pos = 1;
    }

    while (pos < np.size()) {
        size_t next = np.find('/', pos);
        std::string seg = (next == std::string::npos) ? np.substr(pos) : np.substr(pos, next - pos);
        if (!seg.empty()) {
            if (!cur.empty() && cur.back() != '/') cur.push_back('/');
            cur += seg;
            if (!MakeDirSingle(cur)) return false;
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return IsDirectory(np);
}

struct FsDirEntry {
    std::string name;
    bool is_dir = false;
};

bool ListDirEntries(const std::string& dir, std::vector<FsDirEntry>& out_entries) {
    out_entries.clear();
#if defined(_WIN32)
    std::wstring pattern = Utf8ToWide(JoinPath(dir, "*"));
    WIN32_FIND_DATAW ffd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        std::string name = WideToUtf8(ffd.cFileName);
        if (name == "." || name == "..") continue;
        // Skip symlinks / junctions / reparse points to keep folder staging strictly in-tree
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
        FsDirEntry e;
        e.name = name;
        e.is_dir = ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        out_entries.push_back(e);
    } while (FindNextFileW(h, &ffd));
    FindClose(h);
    return true;
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        std::string name(ent->d_name);
        if (name == "." || name == "..") continue;
        std::string full = JoinNativePath(dir, name);
        struct stat lst;
        if (lstat(full.c_str(), &lst) != 0) continue;
        if (S_ISLNK(lst.st_mode)) continue; // Skip symlinks
        if (!S_ISDIR(lst.st_mode) && !S_ISREG(lst.st_mode)) continue; // Regular files & dirs only
        FsDirEntry e;
        e.name = name;
        e.is_dir = S_ISDIR(lst.st_mode);
        out_entries.push_back(e);
    }
    closedir(d);
    return true;
#endif
}

void WalkFilesRecursive(const std::string& root, std::vector<std::string>& out_files, int depth = 0) {
    if (depth > 32) return;
    std::vector<FsDirEntry> entries;
    if (!ListDirEntries(root, entries)) return;
    for (size_t i = 0; i < entries.size(); ++i) {
        std::string full = JoinNativePath(root, entries[i].name);
        if (entries[i].is_dir) {
            WalkFilesRecursive(full, out_files, depth + 1);
        } else {
            out_files.push_back(full);
        }
    }
}

bool RemoveFileQuiet(const std::string& path) {
#if defined(_WIN32)
    std::wstring w = Utf8ToWide(path);
    SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(w.c_str()) != 0;
#else
    return unlink(path.c_str()) == 0;
#endif
}

bool RemoveTreeRecursive(const std::string& path, int depth = 0) {
    if (depth > 48 || !PathExists(path)) return true;
    if (!IsDirectory(path)) {
        return RemoveFileQuiet(path);
    }
    std::vector<FsDirEntry> entries;
    ListDirEntries(path, entries);
    for (size_t i = 0; i < entries.size(); ++i) {
        RemoveTreeRecursive(JoinNativePath(path, entries[i].name), depth + 1);
    }
#if defined(_WIN32)
    return RemoveDirectoryW(Utf8ToWide(path).c_str()) != 0;
#else
    return rmdir(path.c_str()) == 0;
#endif
}

bool ReadFileToBytes(const std::string& path, std::vector<uint8_t>& out_data, size_t max_bytes = 64U * 1024U * 1024U) {
    out_data.clear();
#if defined(_WIN32)
    FILE* f = _wfopen(Utf8ToWide(path).c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    long sz = std::ftell(f);
    if (sz < 0 || static_cast<size_t>(sz) > max_bytes) {
        std::fclose(f);
        return false;
    }
    std::fseek(f, 0, SEEK_SET);
    out_data.resize(static_cast<size_t>(sz));
    if (sz > 0 && std::fread(out_data.data(), 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        std::fclose(f);
        out_data.clear();
        return false;
    }
    std::fclose(f);
    return true;
}

bool ReadFilePrefixToBytes(const std::string& path, std::vector<uint8_t>& out_data, size_t max_bytes) {
    out_data.clear();
#if defined(_WIN32)
    FILE* f = _wfopen(Utf8ToWide(path).c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    out_data.resize(max_bytes);
    size_t got = std::fread(out_data.data(), 1, max_bytes, f);
    bool err = (std::ferror(f) != 0);
    std::fclose(f);
    if (err) {
        out_data.clear();
        return false;
    }
    out_data.resize(got);
    return true;
}

bool WriteBytesToFile(const std::string& path, const uint8_t* data, size_t size) {
    MakeDirRecursive(DirnameOf(path));
#if defined(_WIN32)
    FILE* f = _wfopen(Utf8ToWide(path).c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return false;
    bool ok = true;
    if (size > 0) {
        ok = (std::fwrite(data, 1, size, f) == size);
    }
    if (std::fclose(f) != 0) ok = false;
    return ok;
}

bool CopyFileStream(const std::string& src, const std::string& dst) {
    MakeDirRecursive(DirnameOf(dst));
#if defined(_WIN32)
    FILE* in = _wfopen(Utf8ToWide(src).c_str(), L"rb");
    if (!in) return false;
    FILE* out = _wfopen(Utf8ToWide(dst).c_str(), L"wb");
#else
    FILE* in = std::fopen(src.c_str(), "rb");
    if (!in) return false;
    FILE* out = std::fopen(dst.c_str(), "wb");
#endif
    if (!out) {
        std::fclose(in);
        return false;
    }
    uint8_t buf[65536];
    bool ok = true;
    for (;;) {
        size_t n = std::fread(buf, 1, sizeof(buf), in);
        if (n > 0) {
            if (std::fwrite(buf, 1, n, out) != n) {
                ok = false;
                break;
            }
        }
        if (n < sizeof(buf)) {
            if (std::ferror(in)) ok = false;
            break;
        }
    }
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    return ok;
}

bool CopyDirRecursive(const std::string& src_dir, const std::string& dst_dir, bool overwrite = true, int depth = 0) {
    if (depth > 32) return false;
    if (!MakeDirRecursive(dst_dir)) return false;
    std::vector<FsDirEntry> entries;
    if (!ListDirEntries(src_dir, entries)) return false;
    for (size_t i = 0; i < entries.size(); ++i) {
        std::string s = JoinNativePath(src_dir, entries[i].name);
        std::string d = JoinNativePath(dst_dir, entries[i].name);
        if (entries[i].is_dir) {
            if (!CopyDirRecursive(s, d, overwrite, depth + 1)) return false;
        } else {
            if (!overwrite && PathExists(d)) continue;
            if (!CopyFileStream(s, d)) return false;
        }
    }
    return true;
}

// Checks whether candidate path stays strictly inside root_dir (exact match or root_dir + "/" prefix).
bool IsPathInsideDir(const std::string& candidate, const std::string& root_dir) {
    if (candidate.empty() || root_dir.empty()) return false;
    std::string nc = NormalizeSlashes(candidate);
    std::string nr = NormalizeSlashes(root_dir);
#if defined(_WIN32)
    nc = ToLowerAscii(nc);
    nr = ToLowerAscii(nr);
#endif
    if (nc == nr) return true;
    if (nc.size() > nr.size() && nc.compare(0, nr.size(), nr) == 0) {
        if (nr.back() == '/' || nc[nr.size()] == '/') return true;
    }
    return false;
}

// Case-insensitive relative file resolution under base_dir, bounded inside optional sandbox_root.
std::string FindFileCaseInsensitive(
    const std::string& base_dir,
    const std::string& rel_path,
    const std::string& sandbox_root = ""
) {
    // Reject absolute paths in rel_path when sandbox_root is enforced
    std::string norm_rel = NormalizeSlashes(rel_path);
    if (!sandbox_root.empty()) {
        if (norm_rel.empty() || norm_rel[0] == '/' ||
            (norm_rel.size() >= 2 && std::isalpha(static_cast<unsigned char>(norm_rel[0])) && norm_rel[1] == ':')) {
            return "";
        }
    }

    std::vector<std::string> parts;
    std::stringstream ss(norm_rel);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty() || seg == ".") continue;
        parts.push_back(seg);
    }

    std::string cur = NormalizeSlashes(base_dir);
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] == "..") {
            std::string parent = DirnameOf(cur);
            if (!sandbox_root.empty() && !IsPathInsideDir(parent, sandbox_root)) {
                return "";
            }
            cur = parent;
            continue;
        }
        std::vector<FsDirEntry> entries;
        if (!ListDirEntries(cur, entries)) return "";
        std::string want = ToLowerAscii(parts[i]);
        bool found = false;
        for (size_t j = 0; j < entries.size(); ++j) {
            if (ToLowerAscii(entries[j].name) == want) {
                cur = JoinPath(cur, entries[j].name);
                found = true;
                break;
            }
        }
        if (!found) return "";
    }
    if (!sandbox_root.empty() && !IsPathInsideDir(cur, sandbox_root)) {
        return "";
    }
    return (PathExists(cur) && !IsDirectory(cur)) ? cur : "";
}

// ============================================================================
// Minimal JSON Field Parser for goggame-*.info
// ============================================================================

bool ParseJsonStringAt(const std::string& json, size_t& pos, std::string& out_str) {
    out_str.clear();
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') return true;
        if (c == '\\' && pos < json.size()) {
            char esc = json[pos++];
            switch (esc) {
                case '"':  out_str.push_back('"'); break;
                case '\\': out_str.push_back('\\'); break;
                case '/':  out_str.push_back('/'); break;
                case 'n':  out_str.push_back('\n'); break;
                case 'r':  out_str.push_back('\r'); break;
                case 't':  out_str.push_back('\t'); break;
                case 'u': {
                    if (pos + 4 <= json.size()) {
                        unsigned int cp = 0;
                        std::string hex = json.substr(pos, 4);
                        cp = static_cast<unsigned int>(std::strtoul(hex.c_str(), nullptr, 16));
                        pos += 4;
                        if (cp < 0x80) {
                            out_str.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            out_str.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out_str.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out_str.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out_str.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out_str.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                    }
                    break;
                }
                default: out_str.push_back(esc); break;
            }
        } else {
            out_str.push_back(c);
        }
    }
    return false;
}

void SkipJsonWs(const std::string& json, size_t& pos) {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
}

bool ExtractJsonStringField(const std::string& json, const std::string& key, std::string& out_val) {
    std::string needle = "\"" + key + "\"";
    size_t search_pos = 0;
    while ((search_pos = json.find(needle, search_pos)) != std::string::npos) {
        size_t pos = search_pos + needle.size();
        SkipJsonWs(json, pos);
        if (pos < json.size() && json[pos] == ':') {
            ++pos;
            SkipJsonWs(json, pos);
            if (pos < json.size() && json[pos] == '"') {
                return ParseJsonStringAt(json, pos, out_val);
            }
        }
        search_pos += needle.size();
    }
    return false;
}

bool ExtractJsonBoolField(const std::string& json, const std::string& key, bool& out_val) {
    std::string needle = "\"" + key + "\"";
    size_t search_pos = 0;
    while ((search_pos = json.find(needle, search_pos)) != std::string::npos) {
        size_t pos = search_pos + needle.size();
        SkipJsonWs(json, pos);
        if (pos < json.size() && json[pos] == ':') {
            ++pos;
            SkipJsonWs(json, pos);
            if (json.compare(pos, 4, "true") == 0) {
                out_val = true;
                return true;
            }
            if (json.compare(pos, 5, "false") == 0) {
                out_val = false;
                return true;
            }
        }
        search_pos += needle.size();
    }
    return false;
}

// ============================================================================
// Standalone Userspace FAT16/FAT32 Image Writer (Mode B HDD Injection)
// ============================================================================

class FatImageWriter {
public:
    FatImageWriter() = default;
    ~FatImageWriter() { Close(); }
    FatImageWriter(const FatImageWriter&) = delete;
    FatImageWriter& operator=(const FatImageWriter&) = delete;

    bool Open(const std::string& image_path, std::string& out_error) {
        Close();
#if defined(_WIN32)
        m_file = _wfopen(Utf8ToWide(image_path).c_str(), L"rb+");
#else
        m_file = std::fopen(image_path.c_str(), "rb+");
#endif
        if (!m_file) {
            out_error = "Failed to open HDD image for writing: " + image_path;
            return false;
        }

#if defined(_WIN32)
        _fseeki64(m_file, 0, SEEK_END);
        int64_t fsz = _ftelli64(m_file);
#else
        fseeko(m_file, 0, SEEK_END);
        int64_t fsz = static_cast<int64_t>(ftello(m_file));
#endif
        if (fsz < 512) {
            out_error = "HDD image is too small";
            Close();
            return false;
        }
        m_image_sectors = static_cast<uint64_t>(fsz) / 512ULL;

        uint8_t sec0[512];
        if (!ReadSector(0, sec0)) {
            out_error = "Failed to read sector 0 of HDD image";
            Close();
            return false;
        }

        uint32_t part_lba = 0;
        bool parsed = false;

        // Check MBR partition table first (if not a direct VBR)
        if (sec0[510] == 0x55 && sec0[511] == 0xAA) {
            for (int i = 0; i < 4; ++i) {
                const uint8_t* pe = sec0 + 446 + i * 16;
                uint8_t ptype = pe[4];
                uint32_t lba = ReadLe32(pe + 8);
                uint32_t secs = ReadLe32(pe + 12);
                if (ptype != 0 && lba > 0 && secs > 0 &&
                    static_cast<uint64_t>(lba) + static_cast<uint64_t>(secs) <= m_image_sectors) {
                    if (ParseBpb(lba, secs)) {
                        part_lba = lba;
                        parsed = true;
                        break;
                    }
                }
            }
        }
        if (!parsed) {
            if (!ParseBpb(0, m_image_sectors)) {
                out_error = "HDD image does not contain a valid FAT16 or FAT32 partition";
                Close();
                return false;
            }
            part_lba = 0;
        }

        if (m_fs_type == FilesystemType::FAT12) {
            out_error = "Mode B HDD injection requires a FAT16 or FAT32 partition";
            Close();
            return false;
        }

        m_part_lba = part_lba;
        m_open = true;
        m_modified = false;
        return true;
    }

    void Close() {
        if (m_file) {
            if (m_open && m_modified && m_fs_type == FilesystemType::FAT32 && m_fsinfo_sector > 0) {
                // Invalidate FAT32 FSInfo free/next counts so guest OS recalculates accurately
                uint8_t fsi[512];
                uint64_t fsi_lba = static_cast<uint64_t>(m_part_lba) + m_fsinfo_sector;
                if (fsi_lba < m_image_sectors && ReadSector(fsi_lba, fsi) &&
                    ReadLe32(fsi) == 0x41615252U && ReadLe32(fsi + 484) == 0x61417272U) {
                    WriteLe32(fsi + 488, 0xFFFFFFFFU);
                    WriteLe32(fsi + 492, 0xFFFFFFFFU);
                    WriteSector(fsi_lba, fsi);
                }
            }
            std::fflush(m_file);
            std::fclose(m_file);
            m_file = nullptr;
        }
        m_open = false;
        m_modified = false;
    }

    FilesystemType GetFilesystemType() const { return m_fs_type; }

    uint64_t ComputeFreeBytes() {
        if (!m_open) return 0;
        uint32_t free_clusters = 0;
        for (uint32_t c = 2; c < m_cluster_count + 2; ++c) {
            uint32_t val = 0;
            if (!GetFatEntry(c, val)) break;
            if (val == 0) ++free_clusters;
        }
        return static_cast<uint64_t>(free_clusters) * m_sectors_per_cluster * m_bytes_per_sector;
    }

    bool PathExistsInFat(const std::string& guest_path) {
        if (!m_open) return false;
        std::vector<std::string> segs = SplitGuestPath(guest_path);
        uint32_t cur_cluster = (m_fs_type == FilesystemType::FAT32) ? m_root_cluster : 0;
        for (size_t i = 0; i < segs.size(); ++i) {
            uint32_t next_cl = 0;
            bool is_dir = false;
            if (!FindEntryInDir(cur_cluster, segs[i], next_cl, is_dir)) {
                return false;
            }
            if (i + 1 < segs.size() && !is_dir) return false;
            cur_cluster = next_cl;
        }
        return true;
    }

    bool EnsureChildDirectory(
        uint32_t parent_cluster,
        const std::string& child_name,
        bool allow_lfn,
        uint32_t& out_child_cluster,
        std::string& out_error
    ) {
        uint32_t found_cluster = 0;
        bool is_dir = false;
        if (FindEntryInDir(parent_cluster, child_name, found_cluster, is_dir)) {
            if (!is_dir) {
                out_error = "Path component is a file, not a directory: " + child_name;
                return false;
            }
            out_child_cluster = found_cluster;
            return true;
        }
        return CreateSubdirectory(parent_cluster, child_name, allow_lfn, out_child_cluster, out_error);
    }

    bool EnsureDirectoryPath(const std::string& guest_path, bool allow_lfn, uint32_t& out_dir_cluster, std::string& out_error) {
        std::vector<std::string> segs = SplitGuestPath(guest_path);
        uint32_t cur_cluster = (m_fs_type == FilesystemType::FAT32) ? m_root_cluster : 0;

        for (size_t i = 0; i < segs.size(); ++i) {
            uint32_t found_cluster = 0;
            bool is_dir = false;
            if (FindEntryInDir(cur_cluster, segs[i], found_cluster, is_dir)) {
                if (!is_dir) {
                    out_error = "Path component is a file, not a directory: " + segs[i];
                    return false;
                }
                cur_cluster = found_cluster;
            } else {
                uint32_t new_cluster = 0;
                if (!CreateSubdirectory(cur_cluster, segs[i], allow_lfn, new_cluster, out_error)) {
                    return false;
                }
                cur_cluster = new_cluster;
            }
        }
        out_dir_cluster = cur_cluster;
        return true;
    }

    bool WriteFileToDir(
        uint32_t parent_cluster,
        const std::string& filename,
        const std::vector<uint8_t>& data,
        bool allow_lfn,
        std::string& out_error
    ) {
        // Reject colliding existing files/directories to avoid duplicate SFN/LFN corruption
        uint32_t existing_cl = 0;
        bool existing_dir = false;
        if (FindEntryInDir(parent_cluster, filename, existing_cl, existing_dir)) {
            out_error = existing_dir
                            ? ("Cannot overwrite directory with file: " + filename)
                            : ("Target file already exists on HDD image: " + filename);
            return false;
        }

        uint32_t bytes_per_cluster = m_sectors_per_cluster * m_bytes_per_sector;
        uint32_t needed_clusters = data.empty() ? 0 : static_cast<uint32_t>((data.size() + bytes_per_cluster - 1) / bytes_per_cluster);

        std::vector<uint32_t> chain;
        if (needed_clusters > 0) {
            if (!AllocateClusterChain(needed_clusters, chain)) {
                out_error = "Insufficient free clusters while writing " + filename;
                return false;
            }
            // Write file data to allocated clusters
            std::vector<uint8_t> cl_buf(bytes_per_cluster, 0);
            for (uint32_t i = 0; i < needed_clusters; ++i) {
                std::fill(cl_buf.begin(), cl_buf.end(), 0);
                size_t offset = static_cast<size_t>(i) * bytes_per_cluster;
                size_t copy_len = (std::min)(static_cast<size_t>(bytes_per_cluster), data.size() - offset);
                std::memcpy(cl_buf.data(), data.data() + offset, copy_len);
                if (!WriteCluster(chain[i], cl_buf.data())) {
                    FreeClusterChain(chain);
                    out_error = "Disk write error while writing " + filename;
                    return false;
                }
            }
        }

        uint32_t first_cluster = chain.empty() ? 0 : chain[0];
        if (!AddDirectoryEntry(parent_cluster, filename, 0x20, first_cluster, static_cast<uint32_t>(data.size()), allow_lfn, out_error)) {
            FreeClusterChain(chain);
            return false;
        }
        return true;
    }

private:
    bool ReadSector(uint64_t abs_lba, uint8_t out_sec[512]) {
        if (!m_file || abs_lba >= m_image_sectors) return false;
#if defined(_WIN32)
        if (_fseeki64(m_file, static_cast<int64_t>(abs_lba) * 512LL, SEEK_SET) != 0) return false;
#else
        if (fseeko(m_file, static_cast<off_t>(abs_lba) * 512, SEEK_SET) != 0) return false;
#endif
        return std::fread(out_sec, 1, 512, m_file) == 512;
    }

    bool WriteSector(uint64_t abs_lba, const uint8_t sec[512]) {
        if (!m_file || abs_lba >= m_image_sectors) return false;
#if defined(_WIN32)
        if (_fseeki64(m_file, static_cast<int64_t>(abs_lba) * 512LL, SEEK_SET) != 0) return false;
#else
        if (fseeko(m_file, static_cast<off_t>(abs_lba) * 512, SEEK_SET) != 0) return false;
#endif
        if (std::fwrite(sec, 1, 512, m_file) != 512) return false;
        m_modified = true;
        return true;
    }

    bool ParseBpb(uint32_t part_lba, uint64_t max_part_sectors = 0) {
        uint8_t vbr[512];
        if (!ReadSector(part_lba, vbr)) return false;
        if (vbr[510] != 0x55 || vbr[511] != 0xAA) return false;
        if (!((vbr[0] == 0xEB && vbr[2] == 0x90) || vbr[0] == 0xE9)) return false;

        uint16_t bps = ReadLe16(vbr + 11);
        uint8_t spc = vbr[13];
        uint16_t rsvd = ReadLe16(vbr + 14);
        uint8_t nfats = vbr[16];
        uint16_t root_cnt = ReadLe16(vbr + 17);
        uint32_t tot_sec = ReadLe16(vbr + 19);
        if (tot_sec == 0) tot_sec = ReadLe32(vbr + 32);
        uint32_t fat_sz = ReadLe16(vbr + 22);
        if (fat_sz == 0) fat_sz = ReadLe32(vbr + 36);

        if (bps != 512 || spc == 0 || (spc & (spc - 1)) != 0 || rsvd == 0 || nfats == 0 || nfats > 4) return false;
        if (tot_sec == 0 || fat_sz == 0) return false;
        if (max_part_sectors > 0 && static_cast<uint64_t>(tot_sec) > max_part_sectors) return false;
        if (static_cast<uint64_t>(part_lba) + static_cast<uint64_t>(tot_sec) > m_image_sectors) return false;

        uint64_t root_dir_secs = ((static_cast<uint64_t>(root_cnt) * 32ULL) + (bps - 1ULL)) / bps;
        uint64_t first_data = static_cast<uint64_t>(rsvd) + (static_cast<uint64_t>(nfats) * fat_sz) + root_dir_secs;
        if (first_data >= tot_sec) return false;

        uint64_t data_secs = static_cast<uint64_t>(tot_sec) - first_data;
        uint64_t clusters = data_secs / spc;
        if (clusters > 0x0FFFFFF0ULL) return false;

        m_bytes_per_sector = bps;
        m_sectors_per_cluster = spc;
        m_reserved_sectors = rsvd;
        m_num_fats = nfats;
        m_root_entry_count = root_cnt;
        m_fat_size_sectors = fat_sz;
        m_root_dir_sectors = static_cast<uint32_t>(root_dir_secs);
        m_first_data_sector = static_cast<uint32_t>(first_data);
        m_cluster_count = static_cast<uint32_t>(clusters);
        m_fsinfo_sector = 0;

        if (clusters < 4085) {
            m_fs_type = FilesystemType::FAT12;
        } else if (clusters < 65525) {
            m_fs_type = FilesystemType::FAT16;
            m_root_cluster = 0;
        } else {
            m_fs_type = FilesystemType::FAT32;
            m_root_cluster = ReadLe32(vbr + 44);
            if (m_root_cluster < 2) m_root_cluster = 2;
            uint16_t fsi = ReadLe16(vbr + 48);
            if (fsi > 0 && fsi < rsvd) m_fsinfo_sector = fsi;
        }
        return true;
    }

    uint64_t ClusterToLba(uint32_t cluster) const {
        return static_cast<uint64_t>(m_part_lba) +
               static_cast<uint64_t>(m_first_data_sector) +
               static_cast<uint64_t>(cluster - 2U) * static_cast<uint64_t>(m_sectors_per_cluster);
    }

    bool GetFatEntry(uint32_t cluster, uint32_t& out_val) {
        if (cluster < 2 || cluster >= m_cluster_count + 2) return false;
        uint64_t fat_offset = (m_fs_type == FilesystemType::FAT16)
                                  ? (static_cast<uint64_t>(cluster) * 2ULL)
                                  : (static_cast<uint64_t>(cluster) * 4ULL);
        uint64_t sec_idx = fat_offset / 512ULL;
        uint32_t ent_off = static_cast<uint32_t>(fat_offset % 512ULL);
        if (sec_idx >= m_fat_size_sectors) return false;
        uint8_t sec[512];
        if (!ReadSector(static_cast<uint64_t>(m_part_lba) + m_reserved_sectors + sec_idx, sec)) return false;
        if (m_fs_type == FilesystemType::FAT16) {
            out_val = ReadLe16(sec + ent_off);
        } else {
            out_val = ReadLe32(sec + ent_off) & 0x0FFFFFFFU;
        }
        return true;
    }

    bool SetFatEntry(uint32_t cluster, uint32_t val) {
        if (cluster < 2 || cluster >= m_cluster_count + 2) return false;
        uint64_t fat_offset = (m_fs_type == FilesystemType::FAT16)
                                  ? (static_cast<uint64_t>(cluster) * 2ULL)
                                  : (static_cast<uint64_t>(cluster) * 4ULL);
        uint64_t sec_idx = fat_offset / 512ULL;
        uint32_t ent_off = static_cast<uint32_t>(fat_offset % 512ULL);
        if (sec_idx >= m_fat_size_sectors) return false;

        for (uint32_t f = 0; f < m_num_fats; ++f) {
            uint64_t lba = static_cast<uint64_t>(m_part_lba) + m_reserved_sectors +
                           static_cast<uint64_t>(f) * m_fat_size_sectors + sec_idx;
            uint8_t sec[512];
            if (!ReadSector(lba, sec)) return false;
            if (m_fs_type == FilesystemType::FAT16) {
                WriteLe16(sec + ent_off, static_cast<uint16_t>(val & 0xFFFFU));
            } else {
                uint32_t old = ReadLe32(sec + ent_off);
                WriteLe32(sec + ent_off, (old & 0xF0000000U) | (val & 0x0FFFFFFFU));
            }
            if (!WriteSector(lba, sec)) return false;
        }
        return true;
    }

    void FreeClusterChain(const std::vector<uint32_t>& chain) {
        for (size_t i = 0; i < chain.size(); ++i) {
            SetFatEntry(chain[i], 0);
        }
    }

    bool IsEoc(uint32_t val) const {
        if (m_fs_type == FilesystemType::FAT16) return val >= 0xFFF8U;
        return (val & 0x0FFFFFFFU) >= 0x0FFFFFF8U;
    }

    uint32_t EocValue() const {
        return (m_fs_type == FilesystemType::FAT16) ? 0xFFFFU : 0x0FFFFFFFU;
    }

    bool AllocateClusterChain(uint32_t count, std::vector<uint32_t>& out_chain) {
        out_chain.clear();
        uint32_t search_start = m_alloc_hint;
        for (uint32_t c = search_start; c < m_cluster_count + 2 && out_chain.size() < count; ++c) {
            uint32_t v = 0;
            if (GetFatEntry(c, v) && v == 0) {
                out_chain.push_back(c);
            }
        }
        for (uint32_t c = 2; c < search_start && out_chain.size() < count; ++c) {
            uint32_t v = 0;
            if (GetFatEntry(c, v) && v == 0) {
                out_chain.push_back(c);
            }
        }
        if (out_chain.size() < count) {
            out_chain.clear();
            return false;
        }
        for (size_t i = 0; i < out_chain.size(); ++i) {
            uint32_t next_v = (i + 1 < out_chain.size()) ? out_chain[i + 1] : EocValue();
            if (!SetFatEntry(out_chain[i], next_v)) return false;
        }
        m_alloc_hint = out_chain.back() + 1;
        if (m_alloc_hint >= m_cluster_count + 2) m_alloc_hint = 2;
        return true;
    }

    bool ReadCluster(uint32_t cluster, uint8_t* out_buf) {
        uint64_t lba = ClusterToLba(cluster);
        for (uint32_t s = 0; s < m_sectors_per_cluster; ++s) {
            if (!ReadSector(lba + s, out_buf + s * 512U)) return false;
        }
        return true;
    }

    bool WriteCluster(uint32_t cluster, const uint8_t* buf) {
        uint64_t lba = ClusterToLba(cluster);
        for (uint32_t s = 0; s < m_sectors_per_cluster; ++s) {
            if (!WriteSector(lba + s, buf + s * 512U)) return false;
        }
        return true;
    }

    // Collects all sector LBAs belonging to a directory (cluster==0 means FAT16 fixed root)
    bool GetDirectorySectors(uint32_t dir_cluster, std::vector<uint64_t>& out_lbas, uint32_t& out_last_cluster) {
        out_lbas.clear();
        out_last_cluster = dir_cluster;
        if (dir_cluster == 0 && m_fs_type == FilesystemType::FAT16) {
            uint64_t start = static_cast<uint64_t>(m_part_lba) + m_reserved_sectors +
                             static_cast<uint64_t>(m_num_fats) * m_fat_size_sectors;
            for (uint32_t s = 0; s < m_root_dir_sectors; ++s) {
                out_lbas.push_back(start + s);
            }
            return true;
        }
        uint32_t cur = dir_cluster;
        std::set<uint32_t> visited;
        while (cur >= 2 && cur < m_cluster_count + 2 && visited.insert(cur).second) {
            out_last_cluster = cur;
            uint64_t base = ClusterToLba(cur);
            for (uint32_t s = 0; s < m_sectors_per_cluster; ++s) {
                out_lbas.push_back(base + s);
            }
            uint32_t nxt = 0;
            if (!GetFatEntry(cur, nxt)) break;
            if (IsEoc(nxt) || nxt == 0) break;
            cur = nxt;
        }
        return !out_lbas.empty();
    }

    static std::vector<std::string> SplitGuestPath(const std::string& p) {
        std::string s = p;
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

    static bool IsValid83Char(char c) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) return true;
        const char* extra = "$%'-_@~`!(){}^#&";
        return (std::strchr(extra, c) != nullptr);
    }

    static bool Format83Name(const std::string& name, uint8_t out_sfn[11], bool& out_lossy) {
        std::memset(out_sfn, ' ', 11);
        out_lossy = false;

        std::string clean = name;
        while (!clean.empty() && clean.front() == '.') {
            clean.erase(clean.begin());
            out_lossy = true;
        }
        size_t dot = clean.find_last_of('.');
        std::string base = (dot == std::string::npos) ? clean : clean.substr(0, dot);
        std::string ext = (dot == std::string::npos) ? "" : clean.substr(dot + 1);

        std::string fbase;
        for (size_t i = 0; i < base.size(); ++i) {
            char c = static_cast<char>(std::toupper(static_cast<unsigned char>(base[i])));
            if (c == ' ' || c == '.') { out_lossy = true; continue; }
            if (IsValid83Char(c)) fbase.push_back(c);
            else { fbase.push_back('_'); out_lossy = true; }
        }
        std::string fext;
        for (size_t i = 0; i < ext.size(); ++i) {
            char c = static_cast<char>(std::toupper(static_cast<unsigned char>(ext[i])));
            if (c == ' ' || c == '.') { out_lossy = true; continue; }
            if (IsValid83Char(c)) fext.push_back(c);
            else { fext.push_back('_'); out_lossy = true; }
        }
        if (fbase.size() > 8) { fbase = fbase.substr(0, 8); out_lossy = true; }
        if (fext.size() > 3) { fext = fext.substr(0, 3); out_lossy = true; }
        if (fbase.empty()) { fbase = "FILE"; out_lossy = true; }

        std::memcpy(out_sfn, fbase.data(), fbase.size());
        std::memcpy(out_sfn + 8, fext.data(), fext.size());

        // Check if original name differs in case/chars
        std::string reconstructed = fbase + (fext.empty() ? "" : ("." + fext));
        if (ToUpperAscii(name) != reconstructed) out_lossy = true;
        return true;
    }

    static uint8_t ComputeSfnChecksum(const uint8_t sfn[11]) {
        uint8_t sum = 0;
        for (int i = 0; i < 11; ++i) {
            sum = static_cast<uint8_t>(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i]);
        }
        return sum;
    }

    bool FindEntryInDir(uint32_t dir_cluster, const std::string& target_name, uint32_t& out_first_cluster, bool& out_is_dir) {
        std::vector<uint64_t> lbas;
        uint32_t last_cl = 0;
        if (!GetDirectorySectors(dir_cluster, lbas, last_cl)) return false;

        std::string target_upper = ToUpperAscii(target_name);
        uint8_t target_sfn[11];
        bool lossy = false;
        Format83Name(target_name, target_sfn, lossy);

        std::string pending_lfn;
        bool has_pending_lfn = false;
        uint8_t pending_lfn_csum = 0;
        uint8_t sec[512];
        for (size_t s = 0; s < lbas.size(); ++s) {
            if (!ReadSector(lbas[s], sec)) return false;
            for (int e = 0; e < 16; ++e) {
                const uint8_t* ent = sec + e * 32;
                if (ent[0] == 0x00) return false;
                if (ent[0] == 0xE5) { pending_lfn.clear(); has_pending_lfn = false; continue; }
                uint8_t attr = ent[11];
                if (attr == 0x0F) {
                    // LFN entry
                    std::string chunk;
                    const int offsets[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
                    for (int k = 0; k < 13; ++k) {
                        uint16_t ch = ReadLe16(ent + offsets[k]);
                        if (ch == 0x0000 || ch == 0xFFFF) break;
                        chunk.push_back((ch < 128) ? static_cast<char>(ch) : '?');
                    }
                    if (ent[0] & 0x40) {
                        pending_lfn = chunk;
                        pending_lfn_csum = ent[13];
                        has_pending_lfn = true;
                    } else if (has_pending_lfn && ent[13] == pending_lfn_csum) {
                        pending_lfn = chunk + pending_lfn;
                    } else {
                        pending_lfn.clear();
                        has_pending_lfn = false;
                    }
                    continue;
                }
                if (attr & 0x08) { pending_lfn.clear(); has_pending_lfn = false; continue; } // Volume label

                bool match = false;
                if (has_pending_lfn && pending_lfn_csum == ComputeSfnChecksum(ent) &&
                    ToUpperAscii(pending_lfn) == target_upper) {
                    match = true;
                } else if (!lossy && std::memcmp(ent, target_sfn, 11) == 0) {
                    match = true;
                }
                pending_lfn.clear();
                has_pending_lfn = false;

                if (match) {
                    uint32_t hi = (m_fs_type == FilesystemType::FAT32) ? ReadLe16(ent + 20) : 0;
                    uint32_t lo = ReadLe16(ent + 26);
                    out_first_cluster = (hi << 16) | lo;
                    out_is_dir = ((attr & 0x10) != 0);
                    return true;
                }
            }
        }
        return false;
    }

    bool SfnExistsInDir(const std::vector<uint64_t>& lbas, const uint8_t sfn[11]) {
        uint8_t sec[512];
        for (size_t s = 0; s < lbas.size(); ++s) {
            if (!ReadSector(lbas[s], sec)) return false;
            for (int e = 0; e < 16; ++e) {
                const uint8_t* ent = sec + e * 32;
                if (ent[0] == 0x00) return false;
                if (ent[0] == 0xE5 || ent[11] == 0x0F || (ent[11] & 0x08)) continue;
                if (std::memcmp(ent, sfn, 11) == 0) return true;
            }
        }
        return false;
    }

    bool AddDirectoryEntry(
        uint32_t dir_cluster,
        const std::string& name,
        uint8_t attr,
        uint32_t first_cluster,
        uint32_t file_size,
        bool allow_lfn,
        std::string& out_error
    ) {
        std::vector<uint64_t> lbas;
        uint32_t last_cl = 0;
        if (!GetDirectorySectors(dir_cluster, lbas, last_cl)) {
            out_error = "Failed to enumerate directory sectors";
            return false;
        }

        // Enforce FAT 255-character LFN limit
        std::string clamped_name = (name.size() > 255U) ? name.substr(0, 255U) : name;

        uint8_t sfn[11];
        bool lossy = false;
        Format83Name(clamped_name, sfn, lossy);

        // Disambiguate SFN if collision exists
        if (SfnExistsInDir(lbas, sfn)) {
            for (int n = 1; n <= 9999; ++n) {
                char tilde[8];
                std::snprintf(tilde, sizeof(tilde), "~%d", n);
                size_t tlen = std::strlen(tilde);
                uint8_t cand[11];
                std::memcpy(cand, sfn, 11);
                size_t prefix_len = 8 - tlen;
                for (size_t k = 0; k < prefix_len; ++k) {
                    if (cand[k] == ' ') { prefix_len = k; break; }
                }
                std::memcpy(cand + prefix_len, tilde, tlen);
                for (size_t k = prefix_len + tlen; k < 8; ++k) cand[k] = ' ';
                if (!SfnExistsInDir(lbas, cand)) {
                    std::memcpy(sfn, cand, 11);
                    lossy = true;
                    break;
                }
            }
        }

        // Build raw 32-byte directory entry slots (optional LFN entries + 1 SFN entry)
        std::vector<std::vector<uint8_t>> raw_entries;
        if (allow_lfn && lossy) {
            uint8_t csum = ComputeSfnChecksum(sfn);
            size_t lfn_count = (clamped_name.size() + 12) / 13;
            for (size_t idx = lfn_count; idx >= 1; --idx) {
                std::vector<uint8_t> lfn(32, 0xFF);
                lfn[0] = static_cast<uint8_t>(idx | ((idx == lfn_count) ? 0x40 : 0x00));
                lfn[11] = 0x0F;
                lfn[12] = 0x00;
                lfn[13] = csum;
                lfn[26] = 0x00;
                lfn[27] = 0x00;
                const int offsets[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
                size_t base_ch = (idx - 1) * 13;
                for (int k = 0; k < 13; ++k) {
                    size_t ci = base_ch + static_cast<size_t>(k);
                    uint16_t u16 = 0xFFFF;
                    if (ci < clamped_name.size()) u16 = static_cast<uint8_t>(clamped_name[ci]);
                    else if (ci == clamped_name.size()) u16 = 0x0000;
                    WriteLe16(lfn.data() + offsets[k], u16);
                }
                raw_entries.push_back(lfn);
            }
        }

        std::vector<uint8_t> sfn_ent(32, 0);
        std::memcpy(sfn_ent.data(), sfn, 11);
        sfn_ent[11] = attr;
        // Fixed valid DOS date/time (2026-01-01 12:00:00)
        WriteLe16(sfn_ent.data() + 14, 0x6000);
        WriteLe16(sfn_ent.data() + 16, 0x5C21);
        WriteLe16(sfn_ent.data() + 18, 0x5C21);
        WriteLe16(sfn_ent.data() + 20, static_cast<uint16_t>((first_cluster >> 16) & 0xFFFFU));
        WriteLe16(sfn_ent.data() + 22, 0x6000);
        WriteLe16(sfn_ent.data() + 24, 0x5C21);
        WriteLe16(sfn_ent.data() + 26, static_cast<uint16_t>(first_cluster & 0xFFFFU));
        WriteLe32(sfn_ent.data() + 28, file_size);
        raw_entries.push_back(sfn_ent);

        // Find contiguous free slots in directory without leaving a 0x00 end-of-directory marker before new entries
        const size_t need = raw_entries.size();
        size_t total_slots = lbas.size() * 16;
        size_t run_start = 0;
        size_t run_len = 0;
        bool found_space = false;
        size_t eod_slot = total_slots;

        uint8_t sec[512];
        for (size_t slot = 0; slot < total_slots; ++slot) {
            if (slot % 16 == 0) {
                if (!ReadSector(lbas[slot / 16], sec)) return false;
            }
            uint8_t b0 = sec[(slot % 16) * 32];
            if (b0 == 0x00) {
                // End-of-directory marker: all remaining slots in this cluster chain are free
                eod_slot = (run_len > 0) ? run_start : slot;
                if (total_slots - eod_slot >= need) {
                    run_start = eod_slot;
                    found_space = true;
                }
                break;
            } else if (b0 == 0xE5) {
                if (run_len == 0) run_start = slot;
                ++run_len;
                if (run_len >= need) {
                    found_space = true;
                    break;
                }
            } else {
                run_len = 0;
            }
        }

        // Extend cluster-backed directory if full (starting at eod_slot so no 0x00 precedes new entries)
        if (!found_space) {
            if (dir_cluster == 0 && m_fs_type == FilesystemType::FAT16) {
                out_error = "FAT16 root directory is full";
                return false;
            }
            const size_t start_slot = eod_slot;
            const size_t remaining_existing = (start_slot < total_slots) ? (total_slots - start_slot) : 0;
            const size_t additional_slots = (need > remaining_existing) ? (need - remaining_existing) : need;
            const size_t slots_per_cluster = static_cast<size_t>(m_sectors_per_cluster) * 16U;
            const uint32_t extra_clusters = static_cast<uint32_t>((additional_slots + slots_per_cluster - 1U) / slots_per_cluster);
            std::vector<uint32_t> extra;
            if (!AllocateClusterChain(extra_clusters, extra)) {
                out_error = "Insufficient free space to grow directory";
                return false;
            }
            std::vector<uint8_t> zero_cl(m_sectors_per_cluster * 512U, 0);
            for (size_t c = 0; c < extra.size(); ++c) {
                if (!WriteCluster(extra[c], zero_cl.data())) {
                    FreeClusterChain(extra);
                    return false;
                }
            }
            if (!SetFatEntry(last_cl, extra[0])) {
                FreeClusterChain(extra);
                return false;
            }

            run_start = start_slot;
            for (size_t c = 0; c < extra.size(); ++c) {
                uint64_t base = ClusterToLba(extra[c]);
                for (uint32_t s = 0; s < m_sectors_per_cluster; ++s) {
                    lbas.push_back(base + s);
                }
            }
        }

        // Write entries into the reserved slots
        for (size_t i = 0; i < need; ++i) {
            size_t slot = run_start + i;
            uint64_t lba = lbas[slot / 16];
            if (!ReadSector(lba, sec)) return false;
            std::memcpy(sec + (slot % 16) * 32, raw_entries[i].data(), 32);
            if (!WriteSector(lba, sec)) return false;
        }
        return true;
    }

    bool CreateSubdirectory(
        uint32_t parent_cluster,
        const std::string& name,
        bool allow_lfn,
        uint32_t& out_new_cluster,
        std::string& out_error
    ) {
        std::vector<uint32_t> cl;
        if (!AllocateClusterChain(1, cl)) {
            out_error = "Insufficient free space to create directory " + name;
            return false;
        }
        out_new_cluster = cl[0];

        std::vector<uint8_t> buf(m_sectors_per_cluster * 512U, 0);
        // "." entry
        std::memset(buf.data(), ' ', 11);
        buf[0] = '.';
        buf[11] = 0x10;
        WriteLe16(buf.data() + 20, static_cast<uint16_t>((out_new_cluster >> 16) & 0xFFFFU));
        WriteLe16(buf.data() + 26, static_cast<uint16_t>(out_new_cluster & 0xFFFFU));

        // ".." entry
        uint32_t dotdot_cl = (m_fs_type == FilesystemType::FAT32 && parent_cluster == m_root_cluster) ? 0 : parent_cluster;
        std::memset(buf.data() + 32, ' ', 11);
        buf[32] = '.';
        buf[33] = '.';
        buf[32 + 11] = 0x10;
        WriteLe16(buf.data() + 32 + 20, static_cast<uint16_t>((dotdot_cl >> 16) & 0xFFFFU));
        WriteLe16(buf.data() + 32 + 26, static_cast<uint16_t>(dotdot_cl & 0xFFFFU));

        if (!WriteCluster(out_new_cluster, buf.data())) {
            FreeClusterChain(cl);
            out_error = "Failed to initialize subdirectory cluster";
            return false;
        }

        if (!AddDirectoryEntry(parent_cluster, name, 0x10, out_new_cluster, 0, allow_lfn, out_error)) {
            FreeClusterChain(cl);
            return false;
        }
        return true;
    }

    FILE* m_file = nullptr;
    bool m_open = false;
    bool m_modified = false;
    uint64_t m_image_sectors = 0;
    FilesystemType m_fs_type = FilesystemType::FAT16;
    uint32_t m_part_lba = 0;
    uint32_t m_bytes_per_sector = 512;
    uint32_t m_sectors_per_cluster = 1;
    uint32_t m_reserved_sectors = 1;
    uint32_t m_num_fats = 2;
    uint32_t m_root_entry_count = 0;
    uint32_t m_fat_size_sectors = 0;
    uint32_t m_root_dir_sectors = 0;
    uint32_t m_first_data_sector = 0;
    uint32_t m_cluster_count = 0;
    uint32_t m_root_cluster = 0;
    uint16_t m_fsinfo_sector = 0;
    uint32_t m_alloc_hint = 2;
};

// ============================================================================
// Subprocess Runner for innoextract (No Shell Interpolation)
// ============================================================================

#if defined(_WIN32)
std::wstring QuoteWin32Arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (size_t i = 0; i < arg.size(); ++i) {
        if (arg[i] == L'\\') {
            ++backslashes;
        } else if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}
#endif

bool RunProcessCaptured(
    const std::string& exe_path,
    const std::vector<std::string>& args,
    int& out_exit_code,
    std::string& out_output
) {
    out_exit_code = -1;
    out_output.clear();

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        out_output = "Failed to create process output pipe";
        return false;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdline = QuoteWin32Arg(Utf8ToWide(exe_path));
    for (size_t i = 0; i < args.size(); ++i) {
        cmdline.push_back(L' ');
        cmdline += QuoteWin32Arg(Utf8ToWide(args[i]));
    }

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');

    BOOL ok = CreateProcessW(
        nullptr,
        cmd_buf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );
    CloseHandle(write_pipe);
    if (!ok) {
        CloseHandle(read_pipe);
        out_output = "CreateProcessW failed for: " + exe_path;
        return false;
    }

    char buf[4096];
    DWORD bytes_read = 0;
    while (ReadFile(read_pipe, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
        if (out_output.size() < 65536) {
            out_output.append(buf, bytes_read);
        }
    }
    CloseHandle(read_pipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    out_exit_code = static_cast<int>(exit_code);
    return true;
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        out_output = "pipe() failed";
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        out_output = "fork() failed";
        return false;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe_path.c_str()));
        for (size_t i = 0; i < args.size(); ++i) {
            argv.push_back(const_cast<char*>(args[i].c_str()));
        }
        argv.push_back(nullptr);
        execvp(exe_path.c_str(), argv.data());
        _exit(127);
    }

    close(pipefd[1]);
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) {
            if (out_output.size() < 65536) {
                out_output.append(buf, static_cast<size_t>(n));
            }
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }
    close(pipefd[0]);

    int status = 0;
    pid_t wr = -1;
    do {
        wr = waitpid(pid, &status, 0);
    } while (wr < 0 && errno == EINTR);
    if (wr == pid && WIFEXITED(status)) {
        out_exit_code = WEXITSTATUS(status);
    } else {
        out_exit_code = -1;
    }
    return true;
#endif
}

// ============================================================================
// Floppy & Media Classification Helpers
// ============================================================================

std::string DetectFloppyMediaType(uint64_t file_size) {
    if (file_size == 368640ULL)  return "FLOPPY_525_360K";
    if (file_size == 737280ULL)  return "FLOPPY_35_720K";
    if (file_size == 1228800ULL) return "FLOPPY_525_1200K";
    if (file_size == 1474560ULL) return "FLOPPY_35_1440K";
    if (file_size == 2949120ULL) return "FLOPPY_35_2880K";
    return "";
}

bool IsAudioTrackExt(const std::string& path) {
    return EndsWithLower(path, ".ogg") ||
           EndsWithLower(path, ".mp3") ||
           EndsWithLower(path, ".wav") ||
           EndsWithLower(path, ".flac");
}

bool IsSafeRelativeZipPath(const std::string& raw_path, std::string& out_norm) {
    out_norm.clear();
    if (raw_path.empty()) return false;
    if (raw_path.find('\0') != std::string::npos) return false;

    std::string p = raw_path;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '\\') p[i] = '/';
    }
    if (p[0] == '/') return false;
    if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') return false;

    std::vector<std::string> parts;
    std::stringstream ss(p);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty()) continue;
        // Reject control characters and Win32-illegal filename characters (e.g. ':', '<', '>', '|', '?', '*')
        for (size_t c = 0; c < seg.size(); ++c) {
            unsigned char uc = static_cast<unsigned char>(seg[c]);
            if (uc < 0x20 || seg[c] == ':' || seg[c] == '<' || seg[c] == '>' ||
                seg[c] == '"' || seg[c] == '|' || seg[c] == '?' || seg[c] == '*') {
                return false;
            }
        }
        // Win32 normalizes away trailing spaces and dots (so ".. " or "..." can become "..")
        std::string win_norm = seg;
        while (!win_norm.empty() && (win_norm.back() == ' ' || win_norm.back() == '.')) {
            win_norm.pop_back();
        }
        if (seg == ".") continue;
        if (seg == ".." || win_norm.empty() || win_norm == "." || win_norm == "..") {
            return false; // Zip-Slip / Win32-normalized traversal attempt
        }
        // Also reject trailing spaces/dots on Win32-sensitive path segments
        if (seg.back() == ' ' || seg.back() == '.') {
            return false;
        }
        parts.push_back(seg);
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out_norm.push_back('/');
        out_norm += parts[i];
    }
    return !out_norm.empty();
}

} // anonymous namespace

// ============================================================================
// Public ArchiveImporter Implementation
// ============================================================================

std::string ArchiveImporter::GetDefaultKioskDataRoot() {
#if defined(_WIN32)
    const char* local_app = std::getenv("LOCALAPPDATA");
    if (local_app && *local_app) {
        return JoinPath(local_app, "DOSBox-Kiosk");
    }
    return ".";
#else
    if (IsDirectory("/var/lib/dosbox-kiosk") && access("/var/lib/dosbox-kiosk", W_OK) == 0) {
        return "/var/lib/dosbox-kiosk";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return JoinPath(home, ".local/share/dosbox-kiosk");
    }
    return ".";
#endif
}

std::string ArchiveImporter::MakeSlug(const std::string& raw_name, size_t max_len) {
    std::string slug;
    for (size_t i = 0; i < raw_name.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(raw_name[i]);
        if (std::isalnum(c)) {
            slug.push_back(static_cast<char>(std::tolower(c)));
            if (slug.size() >= max_len) break;
        }
    }
    if (slug.empty()) slug = "game";
    return slug;
}

std::string ArchiveImporter::FindInnoextractBinary(const std::string& override_path) {
    if (!override_path.empty() && PathExists(override_path) && !IsDirectory(override_path)) {
        return override_path;
    }

#if defined(_WIN32)
    const char* exe_name = "innoextract.exe";
    const char sep = ';';
#else
    const char* exe_name = "innoextract";
    const char sep = ':';
#endif

    const char* path_env = std::getenv("PATH");
    if (path_env && *path_env) {
        std::stringstream ss(path_env);
        std::string dir;
        while (std::getline(ss, dir, sep)) {
            if (dir.empty()) continue;
            std::string cand = JoinPath(dir, exe_name);
            if (PathExists(cand) && !IsDirectory(cand)) {
                return cand;
            }
        }
    }
    return "";
}

IngestSourceFormat ArchiveImporter::DetectSourceFormat(
    const std::string& path,
    const std::string& innoextract_override
) {
    if (path.empty() || !PathExists(path)) return IngestSourceFormat::UNKNOWN;
    if (IsDirectory(path)) return IngestSourceFormat::UNPACKED_DIR;

#if defined(_WIN32)
    FILE* f = _wfopen(Utf8ToWide(path).c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return IngestSourceFormat::UNKNOWN;

    uint8_t magic[4] = {0, 0, 0, 0};
    size_t n = std::fread(magic, 1, 4, f);
    if (n < 2) {
        std::fclose(f);
        return IngestSourceFormat::UNKNOWN;
    }

    // PKZIP signature
    if (n >= 4 && magic[0] == 'P' && magic[1] == 'K' &&
        ((magic[2] == 0x03 && magic[3] == 0x04) || (magic[2] == 0x05 && magic[3] == 0x06))) {
        std::fclose(f);
        return IngestSourceFormat::STEAM_ZIP;
    }

    // PE executable: scan for Inno Setup signature marker
    if (magic[0] == 'M' && magic[1] == 'Z') {
        std::fseek(f, 0, SEEK_SET);
        const char* kInnoMarker = "Inno Setup Setup Data";
        const size_t marker_len = std::strlen(kInnoMarker);
        std::vector<char> chunk(262144);
        size_t scanned = 0;
        const size_t kMaxScan = 4U * 1024U * 1024U; // First 4MB covers PE + Inno header
        std::string tail_overlap;
        bool found = false;

        while (scanned < kMaxScan) {
            size_t got = std::fread(chunk.data(), 1, chunk.size(), f);
            if (got == 0) break;
            std::string hay = tail_overlap + std::string(chunk.data(), got);
            if (hay.find(kInnoMarker) != std::string::npos) {
                found = true;
                break;
            }
            if (hay.size() >= marker_len) {
                tail_overlap = hay.substr(hay.size() - marker_len);
            }
            scanned += got;
        }
        std::fclose(f);

        if (found) return IngestSourceFormat::GOG_INNOSETUP;

        // Fallback: ask innoextract --info if installed
        std::string bin = FindInnoextractBinary(innoextract_override);
        if (!bin.empty()) {
            int rc = -1;
            std::string out;
            if (RunProcessCaptured(bin, {"--info", "--", path}, rc, out) && rc == 0) {
                return IngestSourceFormat::GOG_INNOSETUP;
            }
        }
        return IngestSourceFormat::UNKNOWN;
    }

    std::fclose(f);
    return IngestSourceFormat::UNKNOWN;
}

bool ArchiveImporter::ExtractZipArchive(
    const std::string& zip_path,
    const std::string& dest_dir,
    std::string& out_error
) {
    out_error.clear();
#if defined(_WIN32)
    FILE* f = _wfopen(Utf8ToWide(zip_path).c_str(), L"rb");
#else
    FILE* f = std::fopen(zip_path.c_str(), "rb");
#endif
    if (!f) {
        out_error = "Failed to open ZIP archive: " + zip_path;
        return false;
    }

#if defined(_WIN32)
    _fseeki64(f, 0, SEEK_END);
    int64_t file_size = _ftelli64(f);
#else
    fseeko(f, 0, SEEK_END);
    int64_t file_size = static_cast<int64_t>(ftello(f));
#endif
    if (file_size < 22) {
        std::fclose(f);
        out_error = "ZIP archive is too small or truncated";
        return false;
    }

    // Search last 65557 bytes for EOCD signature (0x06054b50)
    int64_t search_len = (std::min)(file_size, static_cast<int64_t>(65557));
    std::vector<uint8_t> tail(static_cast<size_t>(search_len));
#if defined(_WIN32)
    _fseeki64(f, file_size - search_len, SEEK_SET);
#else
    fseeko(f, static_cast<off_t>(file_size - search_len), SEEK_SET);
#endif
    if (std::fread(tail.data(), 1, tail.size(), f) != tail.size()) {
        std::fclose(f);
        out_error = "Failed to read ZIP End-of-Central-Directory";
        return false;
    }

    int64_t eocd_off = -1;
    for (int64_t i = search_len - 22; i >= 0; --i) {
        if (ReadLe32(tail.data() + i) == 0x06054b50U) {
            eocd_off = i;
            break;
        }
    }
    if (eocd_off < 0) {
        std::fclose(f);
        out_error = "Invalid ZIP archive: EOCD signature not found";
        return false;
    }

    const uint8_t* eocd = tail.data() + eocd_off;
    uint16_t entry_count = ReadLe16(eocd + 10);
    uint32_t cd_size = ReadLe32(eocd + 12);
    uint32_t cd_offset = ReadLe32(eocd + 16);

    if (entry_count > 50000 || static_cast<int64_t>(cd_offset) + static_cast<int64_t>(cd_size) > file_size) {
        std::fclose(f);
        out_error = "ZIP Central Directory is out of bounds or exceeds entry limit";
        return false;
    }

    std::vector<uint8_t> cd(cd_size);
#if defined(_WIN32)
    _fseeki64(f, static_cast<int64_t>(cd_offset), SEEK_SET);
#else
    fseeko(f, static_cast<off_t>(cd_offset), SEEK_SET);
#endif
    if (cd_size > 0 && std::fread(cd.data(), 1, cd_size, f) != cd_size) {
        std::fclose(f);
        out_error = "Failed to read ZIP Central Directory";
        return false;
    }

    struct ZipEntryMeta {
        std::string safe_path;
        bool is_dir = false;
        uint16_t method = 0;
        uint32_t crc32 = 0;
        uint32_t comp_size = 0;
        uint32_t uncomp_size = 0;
        uint32_t local_header_offset = 0;
    };

    std::vector<ZipEntryMeta> entries;
    entries.reserve(entry_count);

    size_t pos = 0;
    uint64_t total_uncomp = 0;
    bool saw_steam_backup_csd = false;
    bool saw_steam_backup_sis = false;

    for (uint16_t i = 0; i < entry_count; ++i) {
        if (pos + 46 > cd.size() || ReadLe32(cd.data() + pos) != 0x02014b50U) {
            std::fclose(f);
            out_error = "Corrupt ZIP Central Directory entry";
            return false;
        }
        uint16_t method = ReadLe16(cd.data() + pos + 10);
        uint32_t crc = ReadLe32(cd.data() + pos + 16);
        uint32_t csz = ReadLe32(cd.data() + pos + 20);
        uint32_t usz = ReadLe32(cd.data() + pos + 24);
        uint16_t nlen = ReadLe16(cd.data() + pos + 28);
        uint16_t elen = ReadLe16(cd.data() + pos + 30);
        uint16_t clen = ReadLe16(cd.data() + pos + 32);
        uint32_t lhoff = ReadLe32(cd.data() + pos + 42);

        if (pos + 46 + nlen + elen + clen > cd.size()) {
            std::fclose(f);
            out_error = "Truncated ZIP Central Directory entry";
            return false;
        }

        std::string raw_name(reinterpret_cast<const char*>(cd.data() + pos + 46), nlen);
        pos += 46 + nlen + elen + clen;

        bool is_dir = (!raw_name.empty() && (raw_name.back() == '/' || raw_name.back() == '\\'));
        std::string safe_rel;
        if (!IsSafeRelativeZipPath(raw_name, safe_rel)) {
            std::fclose(f);
            out_error = "Rejected unsafe ZIP path (Zip-Slip protection): " + raw_name;
            return false;
        }

        std::string low_base = ToLowerAscii(BasenameOf(safe_rel));
        if (low_base == "sku.sis") saw_steam_backup_sis = true;
        if (EndsWithLower(low_base, ".csd") || EndsWithLower(low_base, ".csm")) saw_steam_backup_csd = true;

        total_uncomp += usz;
        if (total_uncomp > 4ULL * 1024ULL * 1024ULL * 1024ULL) {
            std::fclose(f);
            out_error = "ZIP uncompressed size exceeds 4 GiB safety limit";
            return false;
        }

        ZipEntryMeta em;
        em.safe_path = safe_rel;
        em.is_dir = is_dir;
        em.method = method;
        em.crc32 = crc;
        em.comp_size = csz;
        em.uncomp_size = usz;
        em.local_header_offset = lhoff;
        entries.push_back(em);
    }

    if (saw_steam_backup_sis && saw_steam_backup_csd) {
        std::fclose(f);
        out_error = "Detected Steam encrypted backup format (sku.sis / .csd). "
                    "Please archive the installed steamapps/common/<Game> folder directly instead of using Steam's backup tool.";
        return false;
    }

    // Detect and strip redundant top-level wrapper folders (e.g., SteamApps/common/<Game>/ or single root folder)
    std::string strip_prefix;
    for (int pass = 0; pass < 3; ++pass) {
        std::string common_top;
        bool all_share_top = !entries.empty();
        for (size_t i = 0; i < entries.size(); ++i) {
            std::string rel = entries[i].safe_path;
            if (!strip_prefix.empty()) {
                if (!StartsWithLower(rel, ToLowerAscii(strip_prefix))) { all_share_top = false; break; }
                rel = rel.substr(strip_prefix.size());
                if (rel.empty()) continue;
            }
            size_t slash = rel.find('/');
            if (slash == std::string::npos) {
                if (!entries[i].is_dir) { all_share_top = false; break; }
                if (common_top.empty()) common_top = rel;
                else if (ToLowerAscii(common_top) != ToLowerAscii(rel)) { all_share_top = false; break; }
            } else {
                std::string top = rel.substr(0, slash);
                if (common_top.empty()) common_top = top;
                else if (ToLowerAscii(common_top) != ToLowerAscii(top)) { all_share_top = false; break; }
            }
        }
        if (all_share_top && !common_top.empty()) {
            strip_prefix += common_top + "/";
        } else {
            break;
        }
    }

    if (!MakeDirRecursive(dest_dir)) {
        std::fclose(f);
        out_error = "Failed to create extraction directory: " + dest_dir;
        return false;
    }

    // Extract each entry
    for (size_t i = 0; i < entries.size(); ++i) {
        const ZipEntryMeta& em = entries[i];
        std::string rel = em.safe_path;
        if (!strip_prefix.empty() && rel.size() >= strip_prefix.size()) {
            rel = rel.substr(strip_prefix.size());
        } else if (!strip_prefix.empty()) {
            continue;
        }
        if (rel.empty()) continue;

        std::string out_path = JoinPath(dest_dir, rel);
        if (em.is_dir) {
            MakeDirRecursive(out_path);
            continue;
        }

        // Read Local File Header
        uint8_t lfh[30];
#if defined(_WIN32)
        _fseeki64(f, static_cast<int64_t>(em.local_header_offset), SEEK_SET);
#else
        fseeko(f, static_cast<off_t>(em.local_header_offset), SEEK_SET);
#endif
        if (std::fread(lfh, 1, 30, f) != 30 || ReadLe32(lfh) != 0x04034b50U) {
            std::fclose(f);
            out_error = "Invalid ZIP local file header for: " + rel;
            return false;
        }
        uint16_t lf_nlen = ReadLe16(lfh + 26);
        uint16_t lf_elen = ReadLe16(lfh + 28);
        int64_t data_offset = static_cast<int64_t>(em.local_header_offset) + 30 + lf_nlen + lf_elen;
        if (data_offset + static_cast<int64_t>(em.comp_size) > file_size) {
            std::fclose(f);
            out_error = "Truncated ZIP entry data for: " + rel;
            return false;
        }

#if defined(_WIN32)
        _fseeki64(f, data_offset, SEEK_SET);
#else
        fseeko(f, static_cast<off_t>(data_offset), SEEK_SET);
#endif

        // Stream entry directly to disk in 64KB chunks (supports 600+ MiB CD images without large RAM allocations)
        if (em.method == 0 && em.comp_size != em.uncomp_size) {
            std::fclose(f);
            out_error = "Invalid Stored ZIP entry size mismatch for: " + rel;
            return false;
        }
        if (em.method != 0 && em.method != 8) {
            std::fclose(f);
            out_error = "Unsupported ZIP compression method (" + std::to_string(em.method) + ") for: " + rel;
            return false;
        }

        MakeDirRecursive(DirnameOf(out_path));
#if defined(_WIN32)
        FILE* out_f = _wfopen(Utf8ToWide(out_path).c_str(), L"wb");
#else
        FILE* out_f = std::fopen(out_path.c_str(), "wb");
#endif
        if (!out_f) {
            std::fclose(f);
            out_error = "Failed to open extracted file for writing: " + out_path;
            return false;
        }

        uint8_t in_chunk[65536];
        uint8_t out_chunk[65536];
        uLong crc_state = ::crc32(0L, Z_NULL, 0);
        uint64_t remaining_in = em.comp_size;
        uint64_t written_total = 0;
        bool stream_ok = true;

        if (em.method == 0) {
            while (remaining_in > 0) {
                size_t to_read = static_cast<size_t>((std::min)(remaining_in, static_cast<uint64_t>(sizeof(in_chunk))));
                if (std::fread(in_chunk, 1, to_read, f) != to_read) {
                    out_error = "Failed reading stored ZIP bytes for: " + rel;
                    stream_ok = false;
                    break;
                }
                if (std::fwrite(in_chunk, 1, to_read, out_f) != to_read) {
                    out_error = "Failed writing extracted file: " + out_path;
                    stream_ok = false;
                    break;
                }
                crc_state = ::crc32(crc_state, in_chunk, static_cast<uInt>(to_read));
                remaining_in -= to_read;
                written_total += to_read;
            }
        } else {
            z_stream zs;
            std::memset(&zs, 0, sizeof(zs));
            if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
                out_error = "zlib inflateInit2 failed for: " + rel;
                stream_ok = false;
            } else {
                int zrc = Z_OK;
                while (zrc != Z_STREAM_END) {
                    if (zs.avail_in == 0 && remaining_in > 0) {
                        size_t to_read = static_cast<size_t>((std::min)(remaining_in, static_cast<uint64_t>(sizeof(in_chunk))));
                        if (std::fread(in_chunk, 1, to_read, f) != to_read) {
                            out_error = "Failed reading deflated ZIP bytes for: " + rel;
                            stream_ok = false;
                            break;
                        }
                        zs.next_in = in_chunk;
                        zs.avail_in = static_cast<uInt>(to_read);
                        remaining_in -= to_read;
                    }
                    zs.next_out = out_chunk;
                    zs.avail_out = static_cast<uInt>(sizeof(out_chunk));
                    zrc = inflate(&zs, (remaining_in == 0) ? Z_FINISH : Z_NO_FLUSH);
                    size_t produced = sizeof(out_chunk) - zs.avail_out;
                    if (produced > 0) {
                        if (written_total + produced > em.uncomp_size) {
                            out_error = "Deflated ZIP entry exceeded declared uncompressed size: " + rel;
                            stream_ok = false;
                            break;
                        }
                        if (std::fwrite(out_chunk, 1, produced, out_f) != produced) {
                            out_error = "Failed writing extracted file: " + out_path;
                            stream_ok = false;
                            break;
                        }
                        crc_state = ::crc32(crc_state, out_chunk, static_cast<uInt>(produced));
                        written_total += produced;
                    }
                    if (zrc != Z_OK && zrc != Z_STREAM_END && zrc != Z_BUF_ERROR) {
                        out_error = "zlib inflate failed for: " + rel;
                        stream_ok = false;
                        break;
                    }
                    if (zrc == Z_BUF_ERROR && remaining_in == 0 && produced == 0) {
                        out_error = "Truncated deflate stream in ZIP entry: " + rel;
                        stream_ok = false;
                        break;
                    }
                }
                inflateEnd(&zs);
            }
        }

        if (std::fclose(out_f) != 0) {
            if (stream_ok) out_error = "Failed to flush extracted file: " + out_path;
            stream_ok = false;
        }
        if (!stream_ok || written_total != em.uncomp_size || static_cast<uint32_t>(crc_state) != em.crc32) {
            RemoveFileQuiet(out_path);
            std::fclose(f);
            if (out_error.empty()) {
                out_error = (written_total != em.uncomp_size)
                                ? ("Uncompressed size mismatch in ZIP entry: " + rel)
                                : ("CRC32 mismatch in ZIP entry: " + rel);
            }
            return false;
        }
    }

    std::fclose(f);
    return true;
}

bool ArchiveImporter::ExtractGogInstaller(
    const std::string& exe_path,
    const std::string& dest_dir,
    const std::string& innoextract_bin,
    std::string& out_error
) {
    out_error.clear();
    std::string bin = FindInnoextractBinary(innoextract_bin);
    if (bin.empty()) {
        out_error = "innoextract CLI not found in PATH. Install innoextract to unpack GOG offline installers.";
        return false;
    }
    if (!MakeDirRecursive(dest_dir)) {
        out_error = "Failed to create GOG staging directory: " + dest_dir;
        return false;
    }

    int rc = -1;
    std::string output;
    std::vector<std::string> args = {
        "--extract",
        "--lowercase",
        "--gog",
        "--exclude-temp",
        "--silent",
        "--output-dir",
        dest_dir,
        "--",
        exe_path
    };
    if (!RunProcessCaptured(bin, args, rc, output) || rc != 0) {
        out_error = "innoextract failed (exit " + std::to_string(rc) + "): " + Trim(output);
        return false;
    }
    return true;
}

bool ArchiveImporter::ParseGogInfoJson(const std::string& json_text, IngestMetadata& out_meta) {
    if (json_text.empty()) return false;

    std::string name, dev, rel_date, game_id;
    bool found_any = false;
    if (ExtractJsonStringField(json_text, "name", name) && !name.empty()) {
        out_meta.title = name;
        found_any = true;
    }
    if (ExtractJsonStringField(json_text, "gameId", game_id) && !game_id.empty()) {
        out_meta.gog_game_id = game_id;
        found_any = true;
    }
    if (ExtractJsonStringField(json_text, "developer", dev) && !dev.empty()) {
        out_meta.developer = dev;
    }
    if (ExtractJsonStringField(json_text, "releaseDate", rel_date) && rel_date.size() >= 4) {
        int yr = std::atoi(rel_date.substr(0, 4).c_str());
        if (yr >= 1975 && yr <= 2030) out_meta.release_year = yr;
    }

    // Parse playTasks array for primary game task
    size_t pt_pos = json_text.find("\"playTasks\"");
    if (pt_pos != std::string::npos) {
        size_t arr_start = json_text.find('[', pt_pos);
        size_t arr_end = (arr_start != std::string::npos) ? json_text.find(']', arr_start) : std::string::npos;
        if (arr_start != std::string::npos && arr_end != std::string::npos) {
            std::string arr = json_text.substr(arr_start + 1, arr_end - arr_start - 1);
            size_t obj_pos = 0;
            std::string fallback_path, fallback_args;
            while ((obj_pos = arr.find('{', obj_pos)) != std::string::npos) {
                size_t obj_end = arr.find('}', obj_pos);
                if (obj_end == std::string::npos) break;
                std::string obj = arr.substr(obj_pos, obj_end - obj_pos + 1);
                obj_pos = obj_end + 1;

                std::string tpath, targs, tcat;
                bool is_primary = false;
                ExtractJsonStringField(obj, "path", tpath);
                ExtractJsonStringField(obj, "arguments", targs);
                ExtractJsonStringField(obj, "category", tcat);
                ExtractJsonBoolField(obj, "isPrimary", is_primary);

                if (is_primary && !tpath.empty()) {
                    out_meta.primary_play_task_path = tpath;
                    out_meta.primary_play_task_args = targs;
                    break;
                }
                if (fallback_path.empty() && ToLowerAscii(tcat) == "game" && !tpath.empty()) {
                    fallback_path = tpath;
                    fallback_args = targs;
                }
            }
            if (out_meta.primary_play_task_path.empty() && !fallback_path.empty()) {
                out_meta.primary_play_task_path = fallback_path;
                out_meta.primary_play_task_args = fallback_args;
            }
        }
    }

    if (found_any) {
        out_meta.source_origin = "GOG_IMPORT";
    }
    return found_any;
}

bool ArchiveImporter::ExtractIcoToPng(const std::string& ico_path, const std::string& out_png_path) {
    std::vector<uint8_t> data;
    if (!ReadFileToBytes(ico_path, data, 16U * 1024U * 1024U)) return false;
    if (data.size() < 22) return false;

    uint16_t reserved = ReadLe16(data.data());
    uint16_t type = ReadLe16(data.data() + 2);
    uint16_t count = ReadLe16(data.data() + 4);
    if (reserved != 0 || type != 1 || count == 0) return false;
    if (6U + static_cast<size_t>(count) * 16U > data.size()) return false;

    // Pick largest resolution & highest bpp layer
    int best_idx = -1;
    uint64_t best_score = 0;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* ent = data.data() + 6 + i * 16;
        uint32_t w = (ent[0] == 0) ? 256U : ent[0];
        uint32_t h = (ent[1] == 0) ? 256U : ent[1];
        uint16_t bpp = ReadLe16(ent + 6);
        uint32_t sz = ReadLe32(ent + 8);
        uint32_t off = ReadLe32(ent + 12);
        if (sz == 0 || static_cast<uint64_t>(off) + sz > data.size()) continue;
        uint64_t score = static_cast<uint64_t>(w) * h * 100ULL + bpp;
        if (score > best_score) {
            best_score = score;
            best_idx = static_cast<int>(i);
        }
    }
    if (best_idx < 0) return false;

    const uint8_t* ent = data.data() + 6 + best_idx * 16;
    uint32_t img_sz = ReadLe32(ent + 8);
    uint32_t img_off = ReadLe32(ent + 12);
    const uint8_t* img = data.data() + img_off;

    // Embedded PNG frame (standard for 256x256 Vista+ icons in GOG .ico)
    static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (img_sz >= 8 && std::memcmp(img, kPngSig, 8) == 0) {
        return WriteBytesToFile(out_png_path, img, img_sz);
    }

    // DIB (BITMAPINFOHEADER) frame
    if (img_sz < 40) return false;
    uint32_t hdr_sz = ReadLe32(img);
    if (hdr_sz < 40 || hdr_sz > img_sz) return false;
    int32_t w = static_cast<int32_t>(ReadLe32(img + 4));
    int32_t h_full = static_cast<int32_t>(ReadLe32(img + 8));
    uint16_t bpp = ReadLe16(img + 14);
    uint32_t comp = ReadLe32(img + 16);
    if (comp != 0 || w <= 0 || w > 512 || h_full <= 0) return false;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) return false;
    int h = h_full / 2;
    if (h <= 0 || h > 512) return false;

    uint32_t clr_used = ReadLe32(img + 32);
    uint32_t pal_entries = 0;
    if (bpp <= 8) {
        uint32_t max_pal = 1U << bpp;
        pal_entries = (clr_used > 0 && clr_used <= max_pal) ? clr_used : max_pal;
    }
    size_t pal_bytes = static_cast<size_t>(pal_entries) * 4U;
    if (hdr_sz + pal_bytes > img_sz) return false;

    const uint8_t* palette = img + hdr_sz;
    const uint8_t* xor_bits = palette + pal_bytes;
    size_t xor_stride = ((static_cast<size_t>(w) * bpp + 31U) / 32U) * 4U;
    size_t xor_bytes = xor_stride * static_cast<size_t>(h);
    if (hdr_sz + pal_bytes + xor_bytes > img_sz) return false;

    const uint8_t* and_bits = xor_bits + xor_bytes;
    size_t and_stride = ((static_cast<size_t>(w) + 31U) / 32U) * 4U;
    bool has_and = (hdr_sz + pal_bytes + xor_bytes + and_stride * static_cast<size_t>(h) <= img_sz);

    std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4U, 0);
    for (int y = 0; y < h; ++y) {
        int src_y = h - 1 - y;
        const uint8_t* xrow = xor_bits + static_cast<size_t>(src_y) * xor_stride;
        const uint8_t* arow = has_and ? (and_bits + static_cast<size_t>(src_y) * and_stride) : nullptr;
        for (int x = 0; x < w; ++x) {
            uint8_t r = 0, g = 0, b = 0, a = 255;
            if (bpp == 32) {
                b = xrow[x * 4 + 0];
                g = xrow[x * 4 + 1];
                r = xrow[x * 4 + 2];
                a = xrow[x * 4 + 3];
            } else if (bpp == 24) {
                b = xrow[x * 3 + 0];
                g = xrow[x * 3 + 1];
                r = xrow[x * 3 + 2];
            } else if (bpp == 8) {
                uint8_t idx = xrow[x];
                if (idx < pal_entries) {
                    b = palette[idx * 4 + 0];
                    g = palette[idx * 4 + 1];
                    r = palette[idx * 4 + 2];
                }
            } else if (bpp == 4) {
                uint8_t byte = xrow[x / 2];
                uint8_t idx = (x & 1) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
                if (idx < pal_entries) {
                    b = palette[idx * 4 + 0];
                    g = palette[idx * 4 + 1];
                    r = palette[idx * 4 + 2];
                }
            } else if (bpp == 1) {
                uint8_t idx = (xrow[x / 8] >> (7 - (x & 7))) & 1;
                if (idx < pal_entries) {
                    b = palette[idx * 4 + 0];
                    g = palette[idx * 4 + 1];
                    r = palette[idx * 4 + 2];
                }
            }
            if (bpp != 32 && arow) {
                uint8_t mask_bit = (arow[x / 8] >> (7 - (x & 7))) & 1;
                if (mask_bit) a = 0;
            }
            size_t dst = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4U;
            rgba[dst + 0] = r;
            rgba[dst + 1] = g;
            rgba[dst + 2] = b;
            rgba[dst + 3] = a;
        }
    }

    return ImageCrawler::WriteRgbaPng(out_png_path, w, h, rgba);
}

bool ArchiveImporter::HarvestMetadata(
    const std::string& sandbox_dir,
    IngestMetadata& out_meta,
    const std::string& icon_cache_dir,
    const std::string& item_id,
    std::string& out_icon_png_path
) {
    out_icon_png_path.clear();
    std::vector<std::string> files;
    WalkFilesRecursive(sandbox_dir, files);

    bool found = false;
    std::string best_ico;
    uint64_t best_ico_size = 0;
    std::string fallback_ico;
    uint64_t fallback_ico_size = 0;

    for (size_t i = 0; i < files.size(); ++i) {
        std::string base = ToLowerAscii(BasenameOf(files[i]));
        if (StartsWithLower(base, "goggame-") && EndsWithLower(base, ".info")) {
            std::vector<uint8_t> bytes;
            if (ReadFileToBytes(files[i], bytes, 1024U * 1024U)) {
                std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                if (ParseGogInfoJson(text, out_meta)) {
                    found = true;
                }
            }
        } else if (StartsWithLower(base, "goggame-") && EndsWithLower(base, ".ico")) {
            uint64_t sz = GetFileSizeBytes(files[i]);
            if (sz > best_ico_size) {
                best_ico_size = sz;
                best_ico = files[i];
            }
        } else if (EndsWithLower(base, ".ico")) {
            uint64_t sz = GetFileSizeBytes(files[i]);
            if (sz > fallback_ico_size) {
                fallback_ico_size = sz;
                fallback_ico = files[i];
            }
        } else if (base == "steam_appid.txt") {
            std::vector<uint8_t> bytes;
            if (ReadFileToBytes(files[i], bytes, 1024)) {
                std::string appid = Trim(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                if (!appid.empty()) {
                    out_meta.steam_appid = appid;
                    if (out_meta.source_origin != "GOG_IMPORT") {
                        out_meta.source_origin = "STEAM_IMPORT";
                    }
                    found = true;
                }
            }
        }
    }

    if (best_ico.empty()) {
        best_ico = fallback_ico;
    }
    if (!best_ico.empty()) {
        out_meta.icon_source_path = best_ico;
        if (!icon_cache_dir.empty() && !item_id.empty()) {
            MakeDirRecursive(icon_cache_dir);
            std::string png_path = JoinPath(icon_cache_dir, item_id + ".png");
            if (ExtractIcoToPng(best_ico, png_path)) {
                out_icon_png_path = png_path;
            }
        }
    }

    return found;
}

int ArchiveImporter::SanitizeWrapperCruft(const std::string& sandbox_dir) {
    static const char* kCruftDirs[] = {
        "dosbox", "__support", "__redist", "commonredist",
        "commonappdata", "directx", "_redist", "tmp"
    };
    static const char* kCruftExactFiles[] = {
        "dosbox.exe", "gogdosconfig.exe", "sdl.dll", "sdl_net.dll",
        "ddraw.dll", "unins000.exe", "unins000.dat", "galaxy.dll",
        "galaxy64.dll", "webcache.zip", "nglide_config.exe", "testapp.exe"
    };
    static const char* kCruftPrefixes[] = {
        "libogg", "libvorbis", "nglide", "3dfxspl"
    };

    int removed = 0;

    // Helper lambda via recursive walk on top-level & nested dirs
    std::vector<std::string> dir_queue;
    dir_queue.push_back(sandbox_dir);

    while (!dir_queue.empty()) {
        std::string cur = dir_queue.back();
        dir_queue.pop_back();

        std::vector<FsDirEntry> entries;
        if (!ListDirEntries(cur, entries)) continue;

        for (size_t i = 0; i < entries.size(); ++i) {
            std::string full = JoinPath(cur, entries[i].name);
            std::string low = ToLowerAscii(entries[i].name);

            if (entries[i].is_dir) {
                bool is_cruft_dir = false;
                for (size_t k = 0; k < sizeof(kCruftDirs) / sizeof(kCruftDirs[0]); ++k) {
                    if (low == kCruftDirs[k]) {
                        is_cruft_dir = true;
                        break;
                    }
                }
                if (is_cruft_dir) {
                    if (RemoveTreeRecursive(full)) ++removed;
                } else {
                    dir_queue.push_back(full);
                }
            } else {
                bool is_cruft_file = false;
                for (size_t k = 0; k < sizeof(kCruftExactFiles) / sizeof(kCruftExactFiles[0]); ++k) {
                    if (low == kCruftExactFiles[k]) {
                        is_cruft_file = true;
                        break;
                    }
                }
                if (!is_cruft_file && EndsWithLower(low, ".dll")) {
                    for (size_t k = 0; k < sizeof(kCruftPrefixes) / sizeof(kCruftPrefixes[0]); ++k) {
                        if (StartsWithLower(low, kCruftPrefixes[k])) {
                            is_cruft_file = true;
                            break;
                        }
                    }
                }
                if (is_cruft_file) {
                    if (RemoveFileQuiet(full)) ++removed;
                }
            }
        }
    }

    return removed;
}

std::string ArchiveImporter::ExtractIso9660VolumeLabel(const std::string& image_path) {
#if defined(_WIN32)
    FILE* f = _wfopen(Utf8ToWide(image_path).c_str(), L"rb");
#else
    FILE* f = std::fopen(image_path.c_str(), "rb");
#endif
    if (!f) return "";

    // Check 2048-byte cooked ISO (0x8000) and 2352-byte raw MODE1/2352 BIN (16 * 2352 + 16 = 0x9310)
    const int64_t offsets[2] = {0x8000LL, 0x9310LL};
    uint8_t pvd[80];
    std::string label;
    for (int i = 0; i < 2; ++i) {
#if defined(_WIN32)
        if (_fseeki64(f, offsets[i], SEEK_SET) != 0) continue;
#else
        if (fseeko(f, static_cast<off_t>(offsets[i]), SEEK_SET) != 0) continue;
#endif
        if (std::fread(pvd, 1, sizeof(pvd), f) == sizeof(pvd)) {
            if (pvd[0] == 0x01 && std::memcmp(pvd + 1, "CD001", 5) == 0) {
                std::string raw(reinterpret_cast<const char*>(pvd + 40), 32);
                label = Trim(raw);
                break;
            }
        }
    }
    std::fclose(f);
    return label;
}

bool ArchiveImporter::SanitizeCueSheet(
    const std::string& cue_content,
    const std::string& source_cue_dir,
    const std::string& dest_bin_basename,
    const std::string& audio_subdir_name,
    const std::string& dest_audio_dir,
    std::string& out_rewritten_cue,
    bool& out_has_redbook_audio,
    std::string& out_error,
    const std::string& sandbox_root,
    const std::string& dest_bin_dir,
    std::vector<std::string>* out_copied_source_bins,
    const std::string& fallback_primary_bin
) {
    out_rewritten_cue.clear();
    out_has_redbook_audio = false;
    out_error.clear();
    if (out_copied_source_bins) out_copied_source_bins->clear();

    const std::string contain_root = sandbox_root.empty() ? source_cue_dir : sandbox_root;
    std::map<std::string, std::string> bin_src_to_dest_base;
    std::map<std::string, std::string> audio_src_to_dest_name;
    std::set<std::string> used_audio_names;
    int bin_track_idx = 0;

    std::istringstream iss(cue_content);
    std::ostringstream oss;
    std::string raw_line;
    bool saw_file = false;

    while (std::getline(iss, raw_line)) {
        std::string trimmed = Trim(raw_line);
        if (trimmed.empty()) {
            oss << "\n";
            continue;
        }

        std::string upper = ToUpperAscii(trimmed);
        if (upper.find("TRACK ") == 0 && upper.find(" AUDIO") != std::string::npos) {
            out_has_redbook_audio = true;
            oss << raw_line << "\n";
            continue;
        }

        if (StartsWithLower(trimmed, "file ")) {
            saw_file = true;
            // Parse FILE "<path>" <TYPE> or FILE <path> <TYPE>
            std::string ref_path;
            std::string file_type;
            size_t first_q = trimmed.find('"');
            if (first_q != std::string::npos) {
                size_t second_q = trimmed.find('"', first_q + 1);
                if (second_q != std::string::npos) {
                    ref_path = trimmed.substr(first_q + 1, second_q - first_q - 1);
                    file_type = Trim(trimmed.substr(second_q + 1));
                }
            } else {
                std::vector<std::string> toks = ConfTranslator::TokenizeCommandLine(trimmed);
                if (toks.size() >= 3) {
                    ref_path = toks[1];
                    file_type = toks[2];
                }
            }

            if (ref_path.empty()) {
                oss << trimmed << "\n";
                continue;
            }
            if (file_type.empty()) file_type = "BINARY";

            if (IsAudioTrackExt(ref_path) || ToUpperAscii(file_type) == "MP3" || ToUpperAscii(file_type) == "WAVE" || ToUpperAscii(file_type) == "OGG") {
                std::string resolved = FindFileCaseInsensitive(source_cue_dir, ref_path, contain_root);
                if (resolved.empty()) {
                    resolved = FindFileCaseInsensitive(source_cue_dir, BasenameOf(ref_path), contain_root);
                }
                if (resolved.empty() && contain_root != source_cue_dir) {
                    resolved = FindFileCaseInsensitive(contain_root, BasenameOf(ref_path), contain_root);
                }
                if (resolved.empty()) {
                    out_error = "CUE references missing audio track: " + ref_path;
                    return false;
                }
                std::string audio_key = ToLowerAscii(NormalizeSlashes(resolved));
                std::string lower_audio;
                auto ait = audio_src_to_dest_name.find(audio_key);
                if (ait != audio_src_to_dest_name.end()) {
                    lower_audio = ait->second;
                } else {
                    std::string base_lower = ToLowerAscii(BasenameOf(resolved));
                    lower_audio = base_lower;
                    if (used_audio_names.count(lower_audio) > 0) {
                        std::string stem = StemOf(base_lower);
                        size_t dot = base_lower.find_last_of('.');
                        std::string ext = (dot == std::string::npos) ? "" : base_lower.substr(dot);
                        for (int dup = 2; dup <= 999; ++dup) {
                            std::string cand = stem + "_" + std::to_string(dup) + ext;
                            if (used_audio_names.count(cand) == 0) {
                                lower_audio = cand;
                                break;
                            }
                        }
                    }
                    used_audio_names.insert(lower_audio);
                    audio_src_to_dest_name[audio_key] = lower_audio;
                    if (!dest_audio_dir.empty()) {
                        MakeDirRecursive(dest_audio_dir);
                        if (!CopyFileStream(resolved, JoinPath(dest_audio_dir, lower_audio))) {
                            out_error = "Failed to copy audio track to media pool: " + resolved;
                            return false;
                        }
                    }
                }
                std::string cue_rel = audio_subdir_name.empty()
                                        ? lower_audio
                                        : (audio_subdir_name + "/" + lower_audio);
                oss << "FILE \"" << cue_rel << "\" " << file_type << "\n";
            } else {
                // Binary track: preserve every distinct binary file referenced in multi-BIN CUE sheets
                std::string resolved_bin = FindFileCaseInsensitive(source_cue_dir, ref_path, contain_root);
                if (resolved_bin.empty()) {
                    resolved_bin = FindFileCaseInsensitive(contain_root, BasenameOf(ref_path), contain_root);
                }
                if (resolved_bin.empty() && bin_track_idx == 0 && !fallback_primary_bin.empty()) {
                    resolved_bin = fallback_primary_bin;
                }
                std::string key = resolved_bin.empty() ? ToLowerAscii(ref_path) : ToLowerAscii(NormalizeSlashes(resolved_bin));
                std::string out_bin_name;
                auto bit = bin_src_to_dest_base.find(key);
                if (bit != bin_src_to_dest_base.end()) {
                    out_bin_name = bit->second;
                } else {
                    ++bin_track_idx;
                    if (bin_track_idx == 1) {
                        out_bin_name = ToLowerAscii(dest_bin_basename);
                    } else {
                        std::string stem = StemOf(dest_bin_basename);
                        out_bin_name = ToLowerAscii(stem + "_t" + std::to_string(bin_track_idx) + ".bin");
                    }
                    bin_src_to_dest_base[key] = out_bin_name;

                    if (!dest_bin_dir.empty()) {
                        if (resolved_bin.empty()) {
                            out_error = "CUE references missing binary track: " + ref_path;
                            return false;
                        }
                        if (!CopyFileStream(resolved_bin, JoinPath(dest_bin_dir, out_bin_name))) {
                            out_error = "Failed to copy CD binary track to media pool: " + resolved_bin;
                            return false;
                        }
                        if (out_copied_source_bins) {
                            out_copied_source_bins->push_back(resolved_bin);
                        }
                    }
                }
                oss << "FILE \"" << out_bin_name << "\" " << file_type << "\n";
            }
        } else {
            oss << trimmed << "\n";
        }
    }

    if (!saw_file) {
        out_error = "CUE/INST sheet contains no FILE directives";
        return false;
    }

    out_rewritten_cue = oss.str();
    return true;
}

bool ArchiveImporter::NormalizeMediaInSandbox(
    const std::string& sandbox_dir,
    const std::string& item_slug,
    const std::string& media_pool_dir,
    std::vector<NormalizedMediaEntry>& out_entries,
    std::string& out_error
) {
    out_entries.clear();
    out_error.clear();
    MakeDirRecursive(media_pool_dir);

    std::vector<std::string> all_files;
    WalkFilesRecursive(sandbox_dir, all_files);
    std::sort(all_files.begin(), all_files.end());

    std::set<std::string> consumed_files;
    std::vector<std::string> deferred_delete_files;
    int cd_index = 0;
    int floppy_index = 0;

    // Pass 1: Process .inst / .ins / .cue sheets (paired with .gog / .bin)
    for (size_t i = 0; i < all_files.size(); ++i) {
        const std::string& cue_path = all_files[i];
        if (!EndsWithLower(cue_path, ".inst") && !EndsWithLower(cue_path, ".ins") && !EndsWithLower(cue_path, ".cue")) {
            continue;
        }
        std::vector<uint8_t> cue_bytes;
        if (!ReadFileToBytes(cue_path, cue_bytes, 1024U * 1024U)) continue;
        std::string cue_text(reinterpret_cast<const char*>(cue_bytes.data()), cue_bytes.size());
        if (ToUpperAscii(cue_text).find("FILE ") == std::string::npos) continue;

        // Locate primary binary image referenced by first FILE ... BINARY line
        std::string src_cue_dir = DirnameOf(cue_path);
        std::string bin_src_path;
        std::istringstream iss(cue_text);
        std::string line;
        while (std::getline(iss, line)) {
            std::string tr = Trim(line);
            if (StartsWithLower(tr, "file ") && ToUpperAscii(tr).find("BINARY") != std::string::npos) {
                size_t q1 = tr.find('"');
                size_t q2 = (q1 != std::string::npos) ? tr.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    std::string ref = tr.substr(q1 + 1, q2 - q1 - 1);
                    bin_src_path = FindFileCaseInsensitive(src_cue_dir, ref, sandbox_dir);
                    if (bin_src_path.empty()) {
                        bin_src_path = FindFileCaseInsensitive(sandbox_dir, BasenameOf(ref), sandbox_dir);
                    }
                }
                break;
            }
        }
        if (bin_src_path.empty()) {
            // Fallback: same stem with .gog or .bin
            std::string stem = StemOf(cue_path);
            bin_src_path = FindFileCaseInsensitive(src_cue_dir, stem + ".gog", sandbox_dir);
            if (bin_src_path.empty()) bin_src_path = FindFileCaseInsensitive(src_cue_dir, stem + ".bin", sandbox_dir);
        }
        if (bin_src_path.empty()) continue;

        ++cd_index;
        std::string disc_stem = item_slug + "_cd" + std::to_string(cd_index);
        // Avoid overwriting existing media_pool files across sequels or re-imports
        if (PathExists(JoinPath(media_pool_dir, disc_stem + ".cue")) ||
            PathExists(JoinPath(media_pool_dir, disc_stem + ".bin"))) {
            for (int dup = 2; dup <= 999; ++dup) {
                std::string cand = item_slug + "_" + std::to_string(dup) + "_cd" + std::to_string(cd_index);
                if (!PathExists(JoinPath(media_pool_dir, cand + ".cue")) &&
                    !PathExists(JoinPath(media_pool_dir, cand + ".bin"))) {
                    disc_stem = cand;
                    break;
                }
            }
        }
        std::string bin_base = disc_stem + ".bin";
        std::string cue_base = disc_stem + ".cue";
        std::string audio_subdir = disc_stem + "_audio";
        std::string dest_bin = JoinPath(media_pool_dir, bin_base);
        std::string dest_cue = JoinPath(media_pool_dir, cue_base);
        std::string dest_audio = JoinPath(media_pool_dir, audio_subdir);

        std::string rewritten_cue;
        bool has_redbook = false;
        std::string cue_err;
        std::vector<std::string> copied_bins;
        if (!SanitizeCueSheet(cue_text, src_cue_dir, bin_base, audio_subdir, dest_audio,
                              rewritten_cue, has_redbook, cue_err,
                              sandbox_dir, media_pool_dir, &copied_bins, bin_src_path)) {
            out_error = cue_err;
            return false;
        }
        // Fallback if CUE referenced a missing stem but we located bin_src_path via stem fallback
        if (copied_bins.empty()) {
            if (!CopyFileStream(bin_src_path, dest_bin)) {
                out_error = "Failed to copy CD image to media pool: " + bin_src_path;
                return false;
            }
            copied_bins.push_back(bin_src_path);
        }
        if (!WriteBytesToFile(dest_cue, reinterpret_cast<const uint8_t*>(rewritten_cue.data()), rewritten_cue.size())) {
            out_error = "Failed to write normalized CUE sheet: " + dest_cue;
            return false;
        }

        consumed_files.insert(ToLowerAscii(NormalizeSlashes(cue_path)));
        deferred_delete_files.push_back(cue_path);
        for (size_t cb = 0; cb < copied_bins.size(); ++cb) {
            consumed_files.insert(ToLowerAscii(NormalizeSlashes(copied_bins[cb])));
            deferred_delete_files.push_back(copied_bins[cb]);
        }

        NormalizedMediaEntry entry;
        entry.original_rel_path = NormalizeSlashes(cue_path);
        entry.normalized_pool_path = dest_cue;
        entry.media_type = "CDROM_CUE";
        entry.volume_label = ExtractIso9660VolumeLabel(dest_bin);
        entry.has_redbook_audio = has_redbook;
        entry.media_pool_id = GenerateUniqueId("media", dest_cue);
        out_entries.push_back(entry);
    }

    // Delete consumed CUE/BIN files only after all CUE sheets are rewritten (handles shared BIN tracks)
    for (size_t d = 0; d < deferred_delete_files.size(); ++d) {
        RemoveFileQuiet(deferred_delete_files[d]);
    }

    // Pass 2: Standalone .gog / .iso images and floppy .img / .ima / .vfd images
    for (size_t i = 0; i < all_files.size(); ++i) {
        const std::string& fp = all_files[i];
        std::string norm_low = ToLowerAscii(NormalizeSlashes(fp));
        if (consumed_files.count(norm_low) > 0 || !PathExists(fp)) continue;

        if (EndsWithLower(fp, ".gog") || EndsWithLower(fp, ".iso")) {
            std::string vol = ExtractIso9660VolumeLabel(fp);
            // For .gog, require valid ISO9660 PVD magic (2048-byte cooked or 2352-byte MODE1 raw)
            if (EndsWithLower(fp, ".gog") && vol.empty()) {
                bool is_iso = false;
#if defined(_WIN32)
                FILE* f = _wfopen(Utf8ToWide(fp).c_str(), L"rb");
#else
                FILE* f = std::fopen(fp.c_str(), "rb");
#endif
                if (f) {
                    const int64_t iso_offsets[2] = {0x8000LL, 0x9310LL};
                    uint8_t sig[6];
                    for (int oi = 0; oi < 2; ++oi) {
#if defined(_WIN32)
                        if (_fseeki64(f, iso_offsets[oi], SEEK_SET) == 0 && std::fread(sig, 1, 6, f) == 6 &&
                            sig[0] == 0x01 && std::memcmp(sig + 1, "CD001", 5) == 0) {
                            is_iso = true;
                            break;
                        }
#else
                        if (fseeko(f, static_cast<off_t>(iso_offsets[oi]), SEEK_SET) == 0 && std::fread(sig, 1, 6, f) == 6 &&
                            sig[0] == 0x01 && std::memcmp(sig + 1, "CD001", 5) == 0) {
                            is_iso = true;
                            break;
                        }
#endif
                    }
                    std::fclose(f);
                }
                if (!is_iso) continue;
            }

            ++cd_index;
            std::string iso_base = item_slug + "_cd" + std::to_string(cd_index) + ".iso";
            if (PathExists(JoinPath(media_pool_dir, iso_base))) {
                for (int dup = 2; dup <= 999; ++dup) {
                    std::string cand = item_slug + "_" + std::to_string(dup) + "_cd" + std::to_string(cd_index) + ".iso";
                    if (!PathExists(JoinPath(media_pool_dir, cand))) {
                        iso_base = cand;
                        break;
                    }
                }
            }
            std::string dest_iso = JoinPath(media_pool_dir, iso_base);
            if (!CopyFileStream(fp, dest_iso)) {
                out_error = "Failed to copy ISO image to media pool: " + fp;
                return false;
            }
            RemoveFileQuiet(fp);

            NormalizedMediaEntry entry;
            entry.original_rel_path = NormalizeSlashes(fp);
            entry.normalized_pool_path = dest_iso;
            entry.media_type = "CDROM_ISO";
            entry.volume_label = vol;
            entry.has_redbook_audio = false;
            entry.media_pool_id = GenerateUniqueId("media", dest_iso);
            out_entries.push_back(entry);
        } else if (EndsWithLower(fp, ".img") || EndsWithLower(fp, ".ima") || EndsWithLower(fp, ".vfd")) {
            std::string flp_type = DetectFloppyMediaType(GetFileSizeBytes(fp));
            if (flp_type.empty()) continue;

            ++floppy_index;
            std::string flp_base = item_slug + "_disk" + std::to_string(floppy_index) + ".img";
            if (PathExists(JoinPath(media_pool_dir, flp_base))) {
                bool allocated = false;
                for (int dup = 2; dup <= 999; ++dup) {
                    std::string cand = item_slug + "_" + std::to_string(dup) + "_disk" + std::to_string(floppy_index) + ".img";
                    if (!PathExists(JoinPath(media_pool_dir, cand))) {
                        flp_base = cand;
                        allocated = true;
                        break;
                    }
                }
                if (!allocated) {
                    out_error = "Could not allocate a unique media_pool path for floppy image: " + fp;
                    return false;
                }
            }
            std::string dest_flp = JoinPath(media_pool_dir, flp_base);
            if (!CopyFileStream(fp, dest_flp)) {
                out_error = "Failed to copy floppy image to media pool: " + fp;
                return false;
            }
            RemoveFileQuiet(fp);

            NormalizedMediaEntry entry;
            entry.original_rel_path = NormalizeSlashes(fp);
            entry.normalized_pool_path = dest_flp;
            entry.media_type = flp_type;
            entry.has_redbook_audio = false;
            entry.media_pool_id = GenerateUniqueId("media", dest_flp);
            out_entries.push_back(entry);
        }
    }

    return true;
}

int ArchiveImporter::PatchGameConfigPaths(
    const std::string& game_root_dir,
    const std::string& guest_install_dir,
    std::string* out_error
) {
    if (out_error) out_error->clear();
    std::string target_prefix = guest_install_dir;
    for (size_t i = 0; i < target_prefix.size(); ++i) {
        if (target_prefix[i] == '/') target_prefix[i] = '\\';
    }
    if (!target_prefix.empty() && target_prefix.back() != '\\') {
        target_prefix.push_back('\\');
    }
    std::string target_upper = ToUpperAscii(target_prefix);

    std::vector<std::string> files;
    WalkFilesRecursive(game_root_dir, files);

    int patched_count = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const std::string& fp = files[i];
        if (!EndsWithLower(fp, ".ini") && !EndsWithLower(fp, ".cfg")) continue;
        uint64_t sz = GetFileSizeBytes(fp);
        if (sz == 0 || sz >= 65536ULL) continue;

        std::vector<uint8_t> bytes;
        if (!ReadFileToBytes(fp, bytes, 65536U)) continue;

        // Skip binary files containing NUL bytes
        bool is_binary = false;
        for (size_t b = 0; b < bytes.size(); ++b) {
            if (bytes[b] == 0) { is_binary = true; break; }
        }
        if (is_binary) continue;

        std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::string out;
        out.reserve(text.size() + 64);
        bool modified = false;

        size_t pos = 0;
        while (pos < text.size()) {
            // Look for "C:\" or "c:\"
            if (pos + 2 < text.size() &&
                (text[pos] == 'C' || text[pos] == 'c') &&
                text[pos + 1] == ':' &&
                text[pos + 2] == '\\') {
                // Ensure not part of an identifier (e.g. "PC:\")
                bool boundary_ok = (pos == 0) || !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
                // Avoid double-patching if already pointing to target_prefix
                bool already_patched = (pos + target_upper.size() <= text.size()) &&
                                       (ToUpperAscii(text.substr(pos, target_upper.size())) == target_upper);
                if (boundary_ok && !already_patched) {
                    out += target_prefix;
                    pos += 3;
                    modified = true;
                    continue;
                }
            }
            out.push_back(text[pos++]);
        }

        if (modified) {
            if (!WriteBytesToFile(fp, reinterpret_cast<const uint8_t*>(out.data()), out.size())) {
                if (out_error) *out_error = "Failed to write patched config file: " + fp;
                return -1;
            }
            ++patched_count;
        }
    }

    return patched_count;
}

bool ArchiveImporter::InjectDirectoryIntoFatImage(
    const std::string& hdd_img_path,
    const std::string& host_source_dir,
    const std::string& guest_dest_dir,
    bool allow_vfat_lfn,
    std::string& out_error,
    std::string* out_actual_guest_dir
) {
    out_error.clear();
    if (out_actual_guest_dir) *out_actual_guest_dir = guest_dest_dir;
    if (!IsDirectory(host_source_dir)) {
        out_error = "Source directory does not exist: " + host_source_dir;
        return false;
    }

    // 1. Collect all host files and compute total required bytes (+15% safety margin)
    std::vector<std::string> all_files;
    WalkFilesRecursive(host_source_dir, all_files);
    uint64_t total_bytes = 0;
    for (size_t i = 0; i < all_files.size(); ++i) {
        total_bytes += GetFileSizeBytes(all_files[i]);
    }
    // Add 15% safety margin + 64KB metadata overhead
    uint64_t required_bytes = total_bytes + (total_bytes * 15ULL) / 100ULL + 65536ULL;

    FatImageWriter writer;
    if (!writer.Open(hdd_img_path, out_error)) {
        return false;
    }

    uint64_t free_bytes = writer.ComputeFreeBytes();
    if (required_bytes > free_bytes) {
        out_error = "Insufficient free space on HDD image (need " +
                    std::to_string(required_bytes / 1024ULL) + " KB including 15% safety margin, available " +
                    std::to_string(free_bytes / 1024ULL) + " KB)";
        return false;
    }

    // Honor allow_vfat_lfn on both FAT16 (Win95) and FAT32 (Win95 OSR2 / Win98)
    const bool use_lfn = allow_vfat_lfn;
    if (out_actual_guest_dir) *out_actual_guest_dir = guest_dest_dir;

    uint32_t root_dest_cluster = 0;
    if (!writer.EnsureDirectoryPath(guest_dest_dir, use_lfn, root_dest_cluster, out_error)) {
        return false;
    }

    // Cache source relative directory -> FAT cluster so every file from one long-named directory
    // lands in the same FAT subdirectory even when LFN is disabled.
    std::map<std::string, uint32_t> dir_cluster_cache;
    dir_cluster_cache[""] = root_dest_cluster;

    std::string norm_src = NormalizeSlashes(host_source_dir);
    for (size_t i = 0; i < all_files.size(); ++i) {
        std::string norm_f = NormalizeSlashes(all_files[i]);
        if (norm_f.size() <= norm_src.size() + 1) continue;
        std::string rel = norm_f.substr(norm_src.size() + 1);

        std::string rel_dir = DirnameOf(rel);
        if (rel_dir == ".") rel_dir.clear();
        std::string fname = BasenameOf(rel);
        uint32_t target_cl = root_dest_cluster;
        if (!rel_dir.empty()) {
            auto cit = dir_cluster_cache.find(rel_dir);
            if (cit != dir_cluster_cache.end()) {
                target_cl = cit->second;
            } else {
                std::stringstream ss(rel_dir);
                std::string seg;
                std::string acc_rel;
                uint32_t cur_cl = root_dest_cluster;
                while (std::getline(ss, seg, '/')) {
                    if (seg.empty() || seg == ".") continue;
                    std::string next_rel = acc_rel.empty() ? seg : (acc_rel + "/" + seg);
                    auto nit = dir_cluster_cache.find(next_rel);
                    if (nit != dir_cluster_cache.end()) {
                        cur_cl = nit->second;
                    } else {
                        uint32_t child_cl = 0;
                        if (!writer.EnsureChildDirectory(cur_cl, seg, use_lfn, child_cl, out_error)) {
                            return false;
                        }
                        cur_cl = child_cl;
                        dir_cluster_cache[next_rel] = cur_cl;
                    }
                    acc_rel = next_rel;
                }
                target_cl = cur_cl;
            }
        }

        std::vector<uint8_t> bytes;
        if (!ReadFileToBytes(all_files[i], bytes, 256U * 1024U * 1024U)) {
            out_error = "Failed to read file for HDD injection: " + all_files[i];
            return false;
        }
        if (!writer.WriteFileToDir(target_cl, fname, bytes, use_lfn, out_error)) {
            return false;
        }
    }

    writer.Close();
    return true;
}

IngestResult ArchiveImporter::ImportArchive(const IngestOptions& options) {
    IngestResult result;

    KioskDatabase& db = KioskDatabase::GetInstance();
    if (!db.IsOpen()) {
        result.error_message = "Catalog database is not open; cannot persist imported game";
        return result;
    }

    if (options.source_path.empty() || !PathExists(options.source_path)) {
        result.error_message = "Source archive or directory not found: " + options.source_path;
        return result;
    }

    const std::string data_root = options.kiosk_data_root.empty()
                                    ? GetDefaultKioskDataRoot()
                                    : NormalizeSlashes(options.kiosk_data_root);
    const std::string job_uuid = GenerateUniqueId("job", options.source_path);
    const std::string sandbox_dir = JoinPath(JoinPath(data_root, "ingest_staging"), job_uuid);
    const std::string preserved_save_dir = sandbox_dir + "_save";
    const std::string preserved_overlay_dir = sandbox_dir + "_ovl";
    const std::string media_pool_dir = JoinPath(data_root, "media_pool");
    const std::string icon_cache_dir = JoinPath(JoinPath(data_root, "cache"), "icons");
    const std::string containers_dir = JoinPath(JoinPath(data_root, "machines"), "containers");

    // Ensure sandbox & temporary overlay cleanup on every exit path
    struct SandboxGuard {
        std::string dir;
        std::string save_dir;
        std::string ovl_dir;
        ~SandboxGuard() {
            if (!ovl_dir.empty()) RemoveTreeRecursive(ovl_dir);
            if (!save_dir.empty()) RemoveTreeRecursive(save_dir);
            if (!dir.empty()) RemoveTreeRecursive(dir);
        }
    } sandbox_guard{sandbox_dir, preserved_save_dir, preserved_overlay_dir};

    // --- Stage 1A: Detect & Unpack ---
    IngestSourceFormat fmt = DetectSourceFormat(options.source_path, options.innoextract_binary);
    if (fmt == IngestSourceFormat::UNKNOWN) {
        result.error_message = "Unsupported archive format (expected GOG Inno Setup .exe, Steam .zip, or unpacked folder): " + options.source_path;
        return result;
    }

    if (fmt == IngestSourceFormat::GOG_INNOSETUP) {
        if (!ExtractGogInstaller(options.source_path, sandbox_dir, options.innoextract_binary, result.error_message)) {
            return result;
        }
    } else if (fmt == IngestSourceFormat::STEAM_ZIP) {
        if (!ExtractZipArchive(options.source_path, sandbox_dir, result.error_message)) {
            return result;
        }
    } else {
        if (!CopyDirRecursive(options.source_path, sandbox_dir)) {
            result.error_message = "Failed to stage unpacked directory: " + options.source_path;
            return result;
        }
    }

    const std::string item_id = GenerateUniqueId("item", options.source_path);

    // --- Stage 1B: Harvest Metadata, Icons & .conf files BEFORE Sanitization ---
    IngestMetadata meta;
    meta.source_origin = (fmt == IngestSourceFormat::GOG_INNOSETUP) ? "GOG_IMPORT" : "STEAM_IMPORT";
    std::string icon_png_path;
    HarvestMetadata(sandbox_dir, meta, icon_cache_dir, item_id, icon_png_path);

    if (meta.title.empty()) {
        meta.title = SignatureMatcher::PrettifyTitle(StemOf(options.source_path));
    }
    const std::string item_slug = MakeSlug(meta.title, 8);

    // Collect DOSBox configuration files (dosbox*.conf, including __support/app/dosbox*.conf)
    std::vector<std::string> all_sandbox_files;
    WalkFilesRecursive(sandbox_dir, all_sandbox_files);
    std::vector<std::string> conf_files;
    for (size_t i = 0; i < all_sandbox_files.size(); ++i) {
        std::string base = ToLowerAscii(BasenameOf(all_sandbox_files[i]));
        if (StartsWithLower(base, "dosbox") && EndsWithLower(base, ".conf") && !ConfTranslator::IsSecondaryProfileConf(base)) {
            conf_files.push_back(all_sandbox_files[i]);
        }
    }

    ConfTranslationResult conf_res;
    bool has_conf = ConfTranslator::ParseAndMergeConfFiles(conf_files, conf_res);

    // Preserve __support/save/ (non-overwriting) and explicit -t overlay dirs (overlay precedence) before sanitizer deletes __support/
    std::string support_save_dir = JoinPath(JoinPath(sandbox_dir, "__support"), "save");
    if (IsDirectory(support_save_dir)) {
        if (!CopyDirRecursive(support_save_dir, preserved_save_dir)) {
            result.error_message = "Failed to preserve __support/save data before sanitization";
            return result;
        }
    }
    for (size_t i = 0; i < conf_res.overlay_dirs.size(); ++i) {
        if (IsPathInsideDir(conf_res.overlay_dirs[i], sandbox_dir) && IsDirectory(conf_res.overlay_dirs[i])) {
            if (!CopyDirRecursive(conf_res.overlay_dirs[i], preserved_overlay_dir, true)) {
                result.error_message = "Failed to preserve overlay directory: " + conf_res.overlay_dirs[i];
                return result;
            }
        }
    }

    // --- Stage 1C: Wrapper & Cruft Sanitization ---
    SanitizeWrapperCruft(sandbox_dir);

    // Determine true Game Root inside sandbox (with strict directory-boundary containment check)
    std::string game_root = sandbox_dir;
    if (has_conf && !conf_res.resolved_game_root.empty() &&
        IsPathInsideDir(conf_res.resolved_game_root, sandbox_dir) &&
        IsDirectory(conf_res.resolved_game_root)) {
        game_root = conf_res.resolved_game_root;
    } else if (IsDirectory(JoinPath(sandbox_dir, "app"))) {
        // Check if actual game executables live under sandbox_dir/app
        std::vector<FsDirEntry> root_entries;
        ListDirEntries(sandbox_dir, root_entries);
        bool root_has_exe = false;
        for (size_t i = 0; i < root_entries.size(); ++i) {
            if (!root_entries[i].is_dir &&
                (EndsWithLower(root_entries[i].name, ".exe") || EndsWithLower(root_entries[i].name, ".com"))) {
                root_has_exe = true;
                break;
            }
        }
        if (!root_has_exe) {
            game_root = JoinPath(sandbox_dir, "app");
        }
    }

    // Merge __support/save (without overwriting base files) and explicit -t overlays (with overlay precedence)
    if (IsDirectory(preserved_save_dir)) {
        if (!CopyDirRecursive(preserved_save_dir, game_root, false)) {
            result.error_message = "Failed to merge preserved save files into game root";
            return result;
        }
        RemoveTreeRecursive(preserved_save_dir);
    }
    if (IsDirectory(preserved_overlay_dir)) {
        if (!CopyDirRecursive(preserved_overlay_dir, game_root, true)) {
            result.error_message = "Failed to apply overlay files into game root";
            return result;
        }
        RemoveTreeRecursive(preserved_overlay_dir);
    }

    // Remove harvested goggame-* metadata files & .conf files from game_root so they don't clutter DOS drive
    {
        std::vector<std::string> cleanup_files;
        WalkFilesRecursive(game_root, cleanup_files);
        for (size_t i = 0; i < cleanup_files.size(); ++i) {
            std::string b = ToLowerAscii(BasenameOf(cleanup_files[i]));
            if ((StartsWithLower(b, "goggame-") && (EndsWithLower(b, ".info") || EndsWithLower(b, ".ico") ||
                                                    EndsWithLower(b, ".hashdb") || EndsWithLower(b, ".script"))) ||
                (StartsWithLower(b, "dosbox") && EndsWithLower(b, ".conf"))) {
                RemoveFileQuiet(cleanup_files[i]);
            }
        }
    }

    // --- Stage 2: Optical & Floppy Media Normalizer ---
    std::vector<NormalizedMediaEntry> norm_media;
    if (!NormalizeMediaInSandbox(sandbox_dir, item_slug, media_pool_dir, norm_media, result.error_message)) {
        return result;
    }

    // Register normalized media in KioskDatabase::media_pool
    for (size_t i = 0; i < norm_media.size(); ++i) {
        MediaPoolRecord mrec;
        mrec.id = norm_media[i].media_pool_id;
        mrec.file_path = norm_media[i].normalized_pool_path;
        mrec.media_type = norm_media[i].media_type;
        mrec.physical_style = StartsWithLower(norm_media[i].media_type, "cdrom") ? "JEWEL_CD" : "BEIGE_35";
        mrec.volume_label = norm_media[i].volume_label;
        mrec.label_title = meta.title;
        mrec.label_subtitle = "Disc " + std::to_string(i + 1);
        mrec.has_redbook_audio = norm_media[i].has_redbook_audio;
        if (!db.InsertMedia(mrec)) {
            result.error_message = "Failed to register normalized media in catalog.db: " + mrec.file_path;
            return result;
        }
        ++result.media_registered_count;
    }

    // --- Stage 3: Inspect Payloads for Win9x PE / DOS Extenders & Score Primary Executable ---
    bool saw_win9x_pe = false;
    bool saw_dos_exec = false;
    bool has_dos_extender = false;
    std::string best_dos_exe;
    std::string best_dos_work_dir = "C:\\";
    std::string best_dos_crc;
    int best_dos_score = -100000;

    std::string best_win_exe;
    std::string best_win_work_dir = "C:\\";
    std::string best_win_crc;
    int best_win_score = -100000;

    std::vector<std::string> game_files;
    WalkFilesRecursive(game_root, game_files);
    const std::string norm_game_root = NormalizeSlashes(game_root);

    for (size_t i = 0; i < game_files.size(); ++i) {
        std::string base_upper = ToUpperAscii(BasenameOf(game_files[i]));
        if (base_upper == "DOS4GW.EXE" || base_upper == "DOS32A.EXE" || base_upper == "CWSDPMI.EXE" ||
            base_upper == "UVCONFIG.EXE" || base_upper == "UNIVBE.EXE") {
            has_dos_extender = true;
            continue;
        }
        if (EndsWithLower(base_upper, ".exe") || EndsWithLower(base_upper, ".com") || EndsWithLower(base_upper, ".bat")) {
            std::vector<uint8_t> head;
            uint64_t fsz = GetFileSizeBytes(game_files[i]);
            if (ReadFilePrefixToBytes(game_files[i], head, 262144U) && !head.empty()) {
                ExecutableFingerprint fp;
                if (SignatureMatcher::ComputeExecutableFingerprint(head.data(), head.size(), base_upper, fp, fsz)) {
                    const bool is_win_pe = (fp.format == KioskExecFormat::WIN32_PE);
                    const bool is_dos_bin = (fp.format == KioskExecFormat::DOS_MZ ||
                                             fp.format == KioskExecFormat::DOS_LE ||
                                             fp.format == KioskExecFormat::DOS_LX ||
                                             fp.format == KioskExecFormat::DOS_BW ||
                                             fp.format == KioskExecFormat::RAW_COM ||
                                             fp.format == KioskExecFormat::BATCH_SCRIPT);
                    if (is_win_pe) saw_win9x_pe = true;
                    if (is_dos_bin && !ImageCrawler::IsIgnoredUtilityBinary(base_upper)) saw_dos_exec = true;
                    if (fp.has_dos_extender_stub || fp.format == KioskExecFormat::DOS_LE || fp.format == KioskExecFormat::DOS_LX) {
                        has_dos_extender = true;
                    }

                    if (!ImageCrawler::IsIgnoredUtilityBinary(base_upper)) {
                        // Compute guest working directory relative to game_root (e.g. "C:\ATLANTIS")
                        std::string norm_f = NormalizeSlashes(game_files[i]);
                        std::string rel_dir;
                        if (norm_f.size() > norm_game_root.size() + 1) {
                            std::string rel = norm_f.substr(norm_game_root.size() + 1);
                            std::string d = DirnameOf(rel);
                            if (d != "." && !d.empty()) rel_dir = d;
                        }
                        std::string guest_dir = "C:\\";
                        if (!rel_dir.empty()) {
                            std::string dos_sub = ToUpperAscii(rel_dir);
                            for (size_t k = 0; k < dos_sub.size(); ++k) {
                                if (dos_sub[k] == '/') dos_sub[k] = '\\';
                            }
                            guest_dir += dos_sub;
                        }
                        std::string parent_name = rel_dir.empty() ? BasenameOf(norm_game_root) : BasenameOf(rel_dir);

                        SoftwareSignatureRecord sig;
                        bool has_sig = db.IsOpen() && SignatureMatcher::MatchSignature(base_upper, fp, sig);
                        bool is_ignore_sig = has_sig && (sig.item_type == "IGNORE_SYSTEM");
                        int score = ImageCrawler::ScoreDosCandidate(
                            base_upper, parent_name, fp, false, has_sig, is_ignore_sig);

                        // Prefer valid 8.3 DOS filenames over long Windows wrapper names (e.g. "Indiana Jones ... .exe")
                        if (base_upper.find(' ') != std::string::npos || StemOf(base_upper).size() > 8) {
                            score -= 80;
                        }
                        // Mild preference for larger main game binaries over tiny stubs
                        if (fsz >= 32768ULL) score += 20;
                        // Penalize .BAT scripts that hardcode drive switches (e.g. "d:\n cd \games\...")
                        if (fp.format == KioskExecFormat::BATCH_SCRIPT) {
                            std::string bat_text(reinterpret_cast<const char*>(head.data()), head.size());
                            std::istringstream bss(bat_text);
                            std::string bline;
                            while (std::getline(bss, bline)) {
                                std::string t = Trim(bline);
                                if (t.size() == 2 && std::isalpha(static_cast<unsigned char>(t[0])) && t[1] == ':') {
                                    score -= 120;
                                    break;
                                }
                            }
                        }

                        if (is_dos_bin) {
                            if (score > best_dos_score) {
                                best_dos_score = score;
                                best_dos_exe = base_upper;
                                best_dos_work_dir = guest_dir;
                                best_dos_crc = fp.header_crc32;
                            }
                        } else if (is_win_pe) {
                            if (score > best_win_score) {
                                best_win_score = score;
                                best_win_exe = base_upper;
                                best_win_work_dir = guest_dir;
                                best_win_crc = fp.header_crc32;
                            }
                        }
                    }
                }
            }
        }
    }

    // Only classify as Win9x PE if no real DOS game executable was found (ignores Steam Win32 launcher wrappers)
    const bool has_win9x_pe = saw_win9x_pe && !saw_dos_exec;

    std::string override_delta;
    std::string archetype_id;
    if (!options.archetype_override.empty() &&
        ArchetypeManager::GetInstance().GetArchetype(options.archetype_override) != nullptr) {
        archetype_id = options.archetype_override;
    } else {
        archetype_id = ConfTranslator::ScoreArchetype(conf_res, has_win9x_pe, has_dos_extender, override_delta);
    }
    result.archetype_id = archetype_id;

    // Resolve primary executable & working dir
    std::string guest_exe = conf_res.guest_executable;
    std::string guest_args = conf_res.guest_arguments;
    std::string guest_work_dir = conf_res.guest_working_dir.empty() ? "C:\\" : conf_res.guest_working_dir;
    std::string fallback_exe_crc = !best_dos_crc.empty() ? best_dos_crc : best_win_crc;
    if (guest_exe.empty()) {
        if (!best_dos_exe.empty()) {
            guest_exe = best_dos_exe;
            guest_work_dir = best_dos_work_dir;
        } else if (!best_win_exe.empty()) {
            if (options.deploy_mode == IngestDeployMode::APPLIANCE_CONTAINER) {
                result.error_message = "No DOS executable found (only Windows PE binaries were detected). "
                                       "Use Mode B with a Windows 9x machine for Windows games.";
                return result;
            }
            guest_exe = best_win_exe;
            guest_work_dir = best_win_work_dir;
        } else {
            result.error_message = "No launchable executable (.exe, .com, .bat) found in imported archive";
            return result;
        }
    }

    // Build ordered CaddyBindingRecords from [autoexec] imgmount directives (prefer resolved path match, then basename)
    std::vector<CaddyBindingRecord> caddy_bindings;
    std::set<std::string> bound_media_ids;
    std::map<std::string, int> drive_slot_counters;

    for (size_t m = 0; m < conf_res.media_mounts.size(); ++m) {
        const ParsedMountDirective& md = conf_res.media_mounts[m];
        std::string raw_drv(1, md.drive_letter);
        for (size_t p = 0; p < md.raw_paths.size(); ++p) {
            std::string ref_exact = (p < md.resolved_paths.size()) ? NormalizeSlashes(md.resolved_paths[p]) : "";
            std::string ref_resolved = ToLowerAscii(ref_exact);
            std::string ref_base = ToLowerAscii(BasenameOf(md.raw_paths[p]));
            int matched_idx = -1;
            // 1. Exact resolved path match (for case-sensitive POSIX hosts)
            if (!ref_exact.empty()) {
                for (size_t nm = 0; nm < norm_media.size(); ++nm) {
                    if (bound_media_ids.count(norm_media[nm].media_pool_id) > 0) continue;
                    if (norm_media[nm].original_rel_path == ref_exact) {
                        matched_idx = static_cast<int>(nm);
                        break;
                    }
                }
            }
            // 2. Case-folded resolved path match
            if (matched_idx < 0 && !ref_resolved.empty()) {
                for (size_t nm = 0; nm < norm_media.size(); ++nm) {
                    if (bound_media_ids.count(norm_media[nm].media_pool_id) > 0) continue;
                    if (ToLowerAscii(norm_media[nm].original_rel_path) == ref_resolved) {
                        matched_idx = static_cast<int>(nm);
                        break;
                    }
                }
            }
            // 3. Basename fallback
            if (matched_idx < 0) {
                for (size_t nm = 0; nm < norm_media.size(); ++nm) {
                    if (bound_media_ids.count(norm_media[nm].media_pool_id) > 0) continue;
                    if (ToLowerAscii(BasenameOf(norm_media[nm].original_rel_path)) == ref_base) {
                        matched_idx = static_cast<int>(nm);
                        break;
                    }
                }
            }
            if (matched_idx >= 0) {
                const NormalizedMediaEntry& nme = norm_media[static_cast<size_t>(matched_idx)];
                bound_media_ids.insert(nme.media_pool_id);
                // Enforce caddy_bindings CHECK (target_drive IN ('A', 'B', 'D', 'E'))
                std::string drv = raw_drv;
                if (drv != "A" && drv != "B" && drv != "D" && drv != "E") {
                    drv = StartsWithLower(nme.media_type, "cdrom") ? "D" : "A";
                }
                CaddyBindingRecord cb;
                cb.catalog_item_id = item_id;
                cb.media_id = nme.media_pool_id;
                cb.target_drive = drv;
                cb.slot_order = ++drive_slot_counters[drv];
                caddy_bindings.push_back(cb);
            }
        }
    }
    // Bind any remaining normalized media not explicitly matched by [autoexec]
    for (size_t nm = 0; nm < norm_media.size(); ++nm) {
        if (bound_media_ids.insert(norm_media[nm].media_pool_id).second) {
            std::string drv = StartsWithLower(norm_media[nm].media_type, "cdrom") ? "D" : "A";
            CaddyBindingRecord cb;
            cb.catalog_item_id = item_id;
            cb.media_id = norm_media[nm].media_pool_id;
            cb.target_drive = drv;
            cb.slot_order = ++drive_slot_counters[drv];
            caddy_bindings.push_back(cb);
        }
    }

    // --- Stage 4: Dual-Mode Target Deployment ---
    CatalogItemRecord item;
    item.id = item_id;
    item.item_type = options.item_type.empty() ? "GAME" : options.item_type;
    item.title = meta.title;
    item.developer = meta.developer;
    item.release_year = meta.release_year;
    item.source_origin = meta.source_origin;
    item.guest_executable = guest_exe;
    item.guest_arguments = guest_args;
    item.return_on_exit = true;
    item.icon_cache_path = icon_png_path;
    item.executable_crc32 = fallback_exe_crc;

    if (options.deploy_mode == IngestDeployMode::APPLIANCE_CONTAINER) {
        // Mode A: Appliance Container (INTEGRATED_DOS)
        std::string container_path = JoinPath(containers_dir, item_id);
        if (!CopyDirRecursive(game_root, container_path)) {
            result.error_message = "Failed to deploy game to container directory: " + container_path;
            return result;
        }

        // Find or create an INTEGRATED_DOS machine template for this archetype
        std::string machine_id = options.target_machine_id;
        if (machine_id.empty() && db.IsOpen()) {
            machine_id = GetOrCreateContainerMachine(archetype_id, override_delta);
            if (machine_id.empty()) {
                result.error_message = "Failed to create INTEGRATED_DOS template machine in catalog.db";
                return result;
            }
        }

        item.machine_id = machine_id;
        item.launch_mechanism = "INTEGRATED_AUTOEXEC";
        item.host_container_dir = container_path;
        item.guest_working_dir = guest_work_dir;
        result.deployed_path = container_path;
    } else {
        // Mode B: Guest OS Hard Disk Injection (REAL_HDD_BOOT)
        if (options.target_machine_id.empty()) {
            result.error_message = "Mode B (HDD Injection) requires a target_machine_id";
            return result;
        }
        MachineRecord target_mach;
        if (!db.IsOpen() || !db.GetMachine(options.target_machine_id, target_mach)) {
            result.error_message = "Target machine not found in catalog.db: " + options.target_machine_id;
            return result;
        }
        if (target_mach.boot_mode != "REAL_HDD_BOOT" || target_mach.primary_hdd_path.empty()) {
            result.error_message = "Target machine is not a REAL_HDD_BOOT machine with a valid primary_hdd_path";
            return result;
        }

        // Select an unused sibling directory under C:\GAMES BEFORE patching configs or writing files
        std::string base_slug = ToUpperAscii(item_slug);
        std::string guest_install_dir = "C:\\GAMES\\" + base_slug;
        {
            FatImageWriter probe;
            std::string probe_err;
            if (probe.Open(target_mach.primary_hdd_path, probe_err) && probe.PathExistsInFat(guest_install_dir)) {
                bool found_free = false;
                for (int dup = 2; dup <= 99; ++dup) {
                    std::string suffix = std::to_string(dup);
                    std::string prefix = base_slug.substr(0, (std::min)(base_slug.size(), static_cast<size_t>(8) - suffix.size()));
                    std::string cand = "C:\\GAMES\\" + prefix + suffix;
                    if (!probe.PathExistsInFat(cand)) {
                        guest_install_dir = cand;
                        found_free = true;
                        break;
                    }
                }
                if (!found_free) {
                    result.error_message = "Could not find an unused C:\\GAMES\\<SLUG> folder on HDD image";
                    return result;
                }
            }
        }

        std::string patch_err;
        result.config_files_patched = PatchGameConfigPaths(game_root, guest_install_dir, &patch_err);
        if (result.config_files_patched < 0) {
            result.error_message = patch_err.empty() ? "Failed to patch game configuration files" : patch_err;
            return result;
        }

        bool allow_lfn = (target_mach.archetype_tier >= 5) || has_win9x_pe;
        std::string actual_guest_dir;
        if (!InjectDirectoryIntoFatImage(target_mach.primary_hdd_path, game_root, guest_install_dir, allow_lfn, result.error_message, &actual_guest_dir)) {
            return result;
        }

        // Compute guest_working_dir relative to actual_guest_dir
        std::string sub_dir;
        if (guest_work_dir.size() > 3 && guest_work_dir[1] == ':' && guest_work_dir[2] == '\\') {
            sub_dir = guest_work_dir.substr(3);
        }
        item.machine_id = target_mach.id;
        item.launch_mechanism = has_win9x_pe ? "WIN9X_RUNONCE_HOOK" : "DOS_AUTOEXEC_INJECT";
        item.guest_working_dir = sub_dir.empty() ? actual_guest_dir : (actual_guest_dir + "\\" + sub_dir);
        result.deployed_path = actual_guest_dir;
    }

    // Helper to roll back deployed Mode A container & DB rows if final publication fails
    auto rollback_publication = [&](bool delete_item_row) {
        if (delete_item_row) {
            db.DeleteCatalogItem(item.id);
        }
        for (size_t i = 0; i < norm_media.size(); ++i) {
            db.DeleteMedia(norm_media[i].media_pool_id);
        }
        if (options.deploy_mode == IngestDeployMode::APPLIANCE_CONTAINER && !item.host_container_dir.empty()) {
            RemoveTreeRecursive(item.host_container_dir);
        }
    };

    // Persist CatalogItemRecord and CaddyBindingRecords
    if (!db.InsertCatalogItem(item)) {
        rollback_publication(false);
        result.error_message = "Failed to insert imported CatalogItemRecord into catalog.db";
        return result;
    }
    for (size_t i = 0; i < caddy_bindings.size(); ++i) {
        caddy_bindings[i].machine_id.clear();
        if (!db.AddCaddyBinding(caddy_bindings[i])) {
            rollback_publication(true);
            result.error_message = "Failed to persist caddy binding for imported item";
            return result;
        }
        ++result.caddy_bindings_count;
    }

    result.success = true;
    result.catalog_item_id = item.id;
    result.machine_id = item.machine_id;
    result.title = item.title;
    LOG_MSG("KIOSK: Imported '%s' (%s) -> %s [%s]",
            result.title.c_str(),
            item.source_origin.c_str(),
            result.deployed_path.c_str(),
            result.archetype_id.c_str());
    return result;
}

std::string ArchiveImporter::GetOrCreateContainerMachine(
    const std::string& archetype_id,
    const std::string& custom_conf_overrides
) {
    KioskDatabase& db = KioskDatabase::GetInstance();
    if (!db.IsOpen()) return "";

    std::vector<MachineRecord> machines = db.GetAllMachines();
    for (size_t i = 0; i < machines.size(); ++i) {
        if (machines[i].boot_mode == "INTEGRATED_DOS" &&
            machines[i].archetype_id == archetype_id &&
            machines[i].custom_conf_overrides == custom_conf_overrides) {
            return machines[i].id;
        }
    }

    MachineRecord tpl;
    tpl.id = GenerateUniqueId("mach", archetype_id);
    const ArchetypeProfile* prof = ArchetypeManager::GetInstance().GetArchetype(archetype_id);
    tpl.display_name = prof ? (prof->display_name + " (Container)") : (archetype_id + " (Container)");
    tpl.archetype_id = archetype_id;
    tpl.archetype_tier = prof ? prof->tier : 3;
    tpl.boot_mode = "INTEGRATED_DOS";
    tpl.os_installed = true;
    tpl.custom_conf_overrides = custom_conf_overrides;
    if (!db.InsertMachine(tpl)) {
        return "";
    }
    return tpl.id;
}

#endif // defined(C_KIOSK)
