#ifndef DOSBOX_KIOSK_CONF_TRANSLATOR_H
#define DOSBOX_KIOSK_CONF_TRANSLATOR_H

#include "../kiosk_types.h"
#include <map>
#include <string>
#include <vector>

struct ParsedMountDirective {
    char drive_letter = 'C';            // Uppercase A-Z
    bool is_imgmount = false;           // true for imgmount, false for mount
    bool is_overlay = false;            // true when -t overlay is specified
    std::string media_type;             // e.g. "iso", "cdrom", "floppy", "hdd", "dir"
    std::string fs_type;                // e.g. "iso", "fat", "none"
    std::vector<std::string> raw_paths; // Original paths from [autoexec]
    std::vector<std::string> resolved_paths; // Lexically resolved relative to conf_dir
};

struct ConfTranslationResult {
    std::string recommended_archetype = KioskArchetypes::DX_386;
    std::string custom_conf_overrides;

    // Resolved from [autoexec] mount C / -t overlay
    std::string resolved_game_root;
    std::vector<std::string> overlay_dirs;

    // Optical & floppy mount directives (drives A, B, D, E, etc.)
    std::vector<ParsedMountDirective> media_mounts;

    // Launch target extracted from [autoexec]
    std::string guest_working_dir;   // e.g. "C:\TOMB" or "C:\"
    std::string guest_executable;    // e.g. "TOMB.EXE" or "DOS4GW.EXE"
    std::string guest_arguments;     // e.g. "-nojoy" or "TOMB.EXE -nojoy"

    // Merged INI sections (lowercased section -> lowercased key -> trimmed value)
    std::map<std::string, std::map<std::string, std::string>> merged_sections;

    // Raw [autoexec] lines collected across merged configs
    std::vector<std::string> autoexec_lines;
};

class ConfTranslator {
public:
    // Parses a single dosbox.conf text buffer and merges into in_out_result.
    // conf_dir is the directory containing the .conf file (used to resolve relative mount paths).
    static bool ParseConfString(
        const std::string& content,
        const std::string& conf_dir,
        ConfTranslationResult& in_out_result
    );

    // Reads and merges multiple dosbox*.conf files (base .conf first, *_single.conf last).
    // Automatically skips secondary profile configs (*_settings.conf, *_server.conf, etc.).
    static bool ParseAndMergeConfFiles(
        const std::vector<std::string>& conf_paths,
        ConfTranslationResult& out_result
    );

    // Evaluates merged INI settings + payload binary hints to select one of the 5 Kiosk Archetypes.
    // Populates out_override_delta with any required per-game overrides (e.g. cycles = max).
    static std::string ScoreArchetype(
        const ConfTranslationResult& parsed,
        bool has_win9x_pe,
        bool has_dos_extender_or_vbe,
        std::string& out_override_delta
    );

    // Returns true for secondary GOG/Steam profile configs (*_settings.conf, *_server.conf, etc.)
    // that should not override primary game launch commands.
    static bool IsSecondaryProfileConf(const std::string& filename);

    // Tokenizes a DOSBox [autoexec] command line, honoring double quotes.
    static std::vector<std::string> TokenizeCommandLine(const std::string& line);

    // Lexically resolves a relative host path against base_dir, normalizing slashes to '/'.
    static std::string ResolveRelativeHostPath(const std::string& base_dir, const std::string& rel_path);

    // Parses numeric cycle count from strings like "fixed 12000", "15000", "auto", "max".
    // Returns -1 for "max", 0 for "auto"/unknown, or positive integer cycle count.
    static int ParseCyclesValue(const std::string& raw_cycles);
};

#endif // DOSBOX_KIOSK_CONF_TRANSLATOR_H
