#include "config.h"

#if defined(C_KIOSK)

#include "boot_injector.h"
#include "kioskext_bin.h"
#include "logging.h"
#include "../fs/fat_volume.h"
#include "config_synthesizer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

constexpr size_t FAT12_1440K_SECTORS = 2880;
constexpr size_t FAT12_SECTOR_SIZE = 512;
constexpr size_t FAT12_1440K_BYTES = FAT12_1440K_SECTORS * FAT12_SECTOR_SIZE; // 1,474,560 bytes

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

bool HasBatchFileExtension(const std::string& path) {
    if (path.size() < 4) return false;
    std::string ext = ToUpperAscii(path.substr(path.size() - 4));
    return ext == ".BAT";
}

bool IsWinStartupCommandLine(const std::string& raw_line) {
    std::string s = TrimWhitespace(raw_line);
    if (s.empty()) return false;
    if (s[0] == '@') {
        s = TrimWhitespace(s.substr(1));
    }
    if (s.empty()) return false;
    if (s.size() >= 2 && s[0] == ':' && s[1] == ':') return false;

    size_t tok_end = s.find_first_of(" \t/");
    std::string first_tok = (tok_end == std::string::npos) ? s : s.substr(0, tok_end);
    if (!first_tok.empty() && first_tok.front() == '"' && first_tok.back() == '"' && first_tok.size() >= 2) {
        first_tok = first_tok.substr(1, first_tok.size() - 2);
    }

    std::string upper_tok = ToUpperAscii(first_tok);
    if (upper_tok == "REM") return false;

    size_t sep = upper_tok.find_last_of("\\/:");
    std::string base_cmd = (sep == std::string::npos) ? upper_tok : upper_tok.substr(sep + 1);
    return (base_cmd == "WIN" || base_cmd == "WIN.COM");
}

bool WriteBinaryFileExclusive(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::remove(path.c_str());
#if !defined(_WIN32)
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = ::open(path.c_str(), flags, 0600);
    if (fd < 0) {
        return false;
    }
    size_t total = 0;
    while (total < bytes.size()) {
        ssize_t n = ::write(fd, bytes.data() + total, bytes.size() - total);
        if (n <= 0) {
            ::close(fd);
            std::remove(path.c_str());
            return false;
        }
        total += static_cast<size_t>(n);
    }
    int rc = ::close(fd);
    return (rc == 0);
#else
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out.is_open()) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return !out.fail();
#endif
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

    // If AUTOEXEC.BAT invokes WIN / WIN.COM at startup, insert the Kiosk dispatch hook
    // immediately before that line so B:\KIOSKGO.BAT intercepts execution prior to booting
    // the default Windows desktop.
    size_t pos = 0;
    while (pos < cleaned.size()) {
        size_t line_end = cleaned.find('\n', pos);
        size_t next_pos = (line_end == std::string::npos) ? cleaned.size() : (line_end + 1);
        std::string line = cleaned.substr(pos, next_pos - pos);
        if (IsWinStartupCommandLine(line)) {
            std::string prefix = cleaned.substr(0, pos);
            if (!prefix.empty() && prefix.back() != '\n') {
                prefix += "\r\n";
            }
            return prefix + GetAutoexecHookBlock() + cleaned.substr(pos);
        }
        pos = next_pos;
    }

    if (!cleaned.empty() && cleaned.back() != '\n') {
        cleaned += "\r\n";
    }
    cleaned += GetAutoexecHookBlock();
    return cleaned;
}

bool BootInjector::EnsureOneTimeAutoexecHook(const std::string& hdd_or_autoexec_path) const {
    if (hdd_or_autoexec_path.empty()) return false;

    // Mode 1: plain host-side AUTOEXEC.BAT (explicit .BAT extension, < 64 KB, no 0x55AA boot signature)
    if (HasBatchFileExtension(hdd_or_autoexec_path)) {
        std::ifstream in(hdd_or_autoexec_path.c_str(), std::ios::in | std::ios::binary | std::ios::ate);
        if (!in.is_open()) return false;
        const std::streamoff size = in.tellg();
        // Size check first: a disk image that merely ends in .BAT must never be slurped whole
        std::string content;
        if (size >= 0 && size < 65536) {
            in.seekg(0, std::ios::beg);
            content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (in.bad()) return false;
        }
        in.close();
        const bool has_boot_sig = content.size() >= 512 &&
                                  static_cast<uint8_t>(content[510]) == 0x55 &&
                                  static_cast<uint8_t>(content[511]) == 0xAA;
        if (size >= 0 && size < 65536 && !has_boot_sig) {
            if (HasAutoexecHook(content)) {
                return true;
            }
            const std::string updated = InjectAutoexecHookToString(content);
            return KioskWriteHostFileAtomically(hdd_or_autoexec_path, updated);
        }
    }

    // Mode 2: raw FAT12/FAT16/FAT32 disk image (superfloppy or MBR-partitioned) via the
    // shared journaled FAT layer: the image is either fully hooked or left untouched.
    KioskFatVolume volume;
    std::string err;
    std::string upper_ext = hdd_or_autoexec_path.size() >= 4 ? ToUpperAscii(hdd_or_autoexec_path.substr(hdd_or_autoexec_path.size() - 4)) : "";
    if (upper_ext == ".VHD") {
        // The kiosk FAT writer never modifies VHD containers, but an image that is already
        // hooked can still boot: verify read-only and refuse only if the hook is missing.
        FatDirEntry hooked;
        std::vector<uint8_t> bytes;
        if (volume.OpenImageFile(hdd_or_autoexec_path, /*writable=*/false, &err) &&
            volume.FindPath("\\AUTOEXEC.BAT", hooked) && !hooked.is_directory &&
            volume.ReadFile(hooked, bytes, 1024U * 1024U) &&
            HasAutoexecHook(std::string(bytes.begin(), bytes.end()))) {
            return true;
        }
        LOG_MSG("KIOSK: AUTOEXEC hook missing on VHD image %s; VHDs cannot be modified by the kiosk",
                hdd_or_autoexec_path.c_str());
        return false;
    }
    if (!volume.OpenImageFile(hdd_or_autoexec_path, /*writable=*/true, &err)) {
        LOG_MSG("KIOSK: AUTOEXEC hook skipped: %s", err.c_str());
        return false;
    }

    FatDirEntry autoexec;
    std::string existing;
    const bool has_autoexec = volume.FindPath("\\AUTOEXEC.BAT", autoexec);
    if (has_autoexec) {
        if (autoexec.is_directory) return false;
        std::vector<uint8_t> bytes;
        if (!volume.ReadFile(autoexec, bytes, 1024U * 1024U) || bytes.size() != autoexec.file_size) {
            LOG_MSG("KIOSK: AUTOEXEC hook skipped: cannot read C:\\AUTOEXEC.BAT");
            return false;
        }
        existing.assign(bytes.begin(), bytes.end());
        if (HasAutoexecHook(existing)) {
            return true; // Idempotent: zero writes when the hook is already present
        }
    }

    const std::string target = has_autoexec ? InjectAutoexecHookToString(existing) : GetAutoexecHookBlock();
    const std::vector<uint8_t> target_bytes(target.begin(), target.end());
    if (!volume.WriteFileReplacing(KioskFatVolume::kRootDir, "AUTOEXEC.BAT", target_bytes, err) ||
        !volume.Commit(&err)) {
        LOG_MSG("KIOSK: AUTOEXEC hook failed: %s", err.c_str());
        return false;
    }
    return true;
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

    // Fields may come from imported (untrusted) metadata: never let them inject batch lines,
    // %expansion%, redirection or extra commands into the guest's B:\KIOSKGO.BAT.
    const std::string exe_raw = TrimWhitespace(item.guest_executable);
    std::string dir_check = work_dir;
    if (dir_check.size() >= 2 && dir_check.front() == '"' && dir_check.back() == '"') {
        dir_check = dir_check.substr(1, dir_check.size() - 2);
    }
    if (!std::isalpha(static_cast<unsigned char>(drive_letter)) ||
        !ConfigSynthesizer::IsSafeGuestDosPath(dir_check) ||
        !ConfigSynthesizer::IsSafeGuestExecutable(exe_raw) ||
        !ConfigSynthesizer::IsSafeGuestArguments(item.guest_arguments)) {
        LOG_MSG("KIOSK: Rejected unsafe launch command for item %s in KIOSKGO.BAT", item.id.c_str());
        ss << "ECHO Kiosk: launch command rejected (unsafe characters).\r\n";
        if (item.return_on_exit) {
            ss << "B:\\KIOSKEXT.COM\r\n";
        }
        return ss.str();
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
    KioskMemorySectorDevice device(image_bytes);
    KioskFatVolume volume;
    FatDirEntry entry;
    if (!volume.Mount(&device) || !volume.FindPath("\\" + filename_8_3, entry) || entry.is_directory) {
        return false;
    }
    return volume.ReadFile(entry, out_file_data, image_bytes.size());
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

    if (!WriteBinaryFileExclusive(output_image_path, image_bytes)) {
        LOG_MSG("KIOSK: Failed to write dispatch floppy image to '%s'", output_image_path.c_str());
        return false;
    }
    return true;
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

    if (!WriteBinaryFileExclusive(output_image_path, image_bytes)) {
        LOG_MSG("KIOSK: Failed to write idle dispatch floppy image to '%s'", output_image_path.c_str());
        return false;
    }
    return true;
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
        std::ostringstream oss;
        oss << path << "kiosk_dispatch_" << static_cast<unsigned long>(::getpid()) << ".img";
        return oss.str();
    }
    std::ostringstream dir_ss;
    dir_ss << "/tmp/dosbox-kiosk-" << static_cast<unsigned long>(::geteuid())
           << "-" << static_cast<unsigned long>(::getpid());
    std::string priv_dir = dir_ss.str();
    (void)::mkdir(priv_dir.c_str(), 0700);
    return priv_dir + "/kiosk_dispatch.img";
#endif
}

bool BootInjector::CleanupDispatchVolume(const std::string& image_path) const {
    if (image_path.empty()) return false;
    bool removed = (std::remove(image_path.c_str()) == 0);
#if !defined(_WIN32)
    if (image_path.compare(0, 18, "/tmp/dosbox-kiosk-") == 0) {
        size_t slash = image_path.find_last_of('/');
        if (slash != std::string::npos && slash > 5) {
            std::string parent_dir = image_path.substr(0, slash);
            (void)::rmdir(parent_dir.c_str());
        }
    }
#endif
    return removed;
}

#endif // defined(C_KIOSK)
