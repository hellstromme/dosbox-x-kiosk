#ifndef DOSBOX_KIOSK_SIGNATURE_MATCHER_H
#define DOSBOX_KIOSK_SIGNATURE_MATCHER_H

#include "../kiosk_types.h"
#include <cstddef>
#include <cstdint>
#include <string>

enum class KioskExecFormat {
    UNKNOWN = 0,
    RAW_COM,
    BATCH_SCRIPT,
    DOS_MZ,
    DOS_LE,
    DOS_LX,
    DOS_BW,
    WIN16_NE,
    WIN32_PE
};

struct ExecutableFingerprint {
    KioskExecFormat format = KioskExecFormat::UNKNOWN;
    bool has_dos_extender_stub = false;
    size_t payload_offset = 0;       // Offset of LE/LX/BW/NE/PE header if present
    uint64_t file_size_bytes = 0;
    uint32_t crc32_value = 0;
    std::string header_crc32;        // 8-char uppercase hex string
};

struct ScrapedMetadata {
    std::string title;
    std::string developer;
    int release_year = 0;
};

class SignatureMatcher {
public:
    // Standard IEEE 802.3 CRC32 (incremental & one-shot)
    static uint32_t UpdateCrc32(uint32_t crc_state, const void* data, size_t length);
    static uint32_t ComputeCrc32(const void* data, size_t length);
    static std::string FormatCrc32Hex(uint32_t crc);

    // Inspects executable bytes, strips DOS/4GW / Watcom extender stubs when present,
    // and computes the 4KB header CRC32 fingerprint.
    // Pass true_file_size > 0 when data is a truncated prefix of a larger file.
    static bool ComputeExecutableFingerprint(
        const uint8_t* data,
        size_t size,
        const std::string& filename_hint,
        ExecutableFingerprint& out_fp,
        uint64_t true_file_size = 0
    );

    // Tier 1: Query KioskDatabase::software_signatures by uppercase basename + header_crc32.
    static bool MatchSignature(
        const std::string& exec_filename,
        const ExecutableFingerprint& fp,
        SoftwareSignatureRecord& out_sig
    );

    // Tier 2: Heuristic fallback metadata scraper (NE description / Copyright string scan).
    static void ScrapeFallbackMetadata(
        const uint8_t* data,
        size_t size,
        const std::string& fallback_name,
        ScrapedMetadata& out_meta
    );

    // Converts a raw filename or folder stem (e.g. "DOOM2.EXE", "sim_city") into a readable title.
    static std::string PrettifyTitle(const std::string& raw_name);

    // Returns uppercase basename without directory path (handles '\' and '/').
    static std::string ExtractUpperBasename(const std::string& path);
};

#endif // DOSBOX_KIOSK_SIGNATURE_MATCHER_H
