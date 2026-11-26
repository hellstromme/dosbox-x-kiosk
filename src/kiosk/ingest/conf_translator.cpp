#include "config.h"

#if defined(C_KIOSK)

#include "conf_translator.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

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

std::string StripSurroundingQuotes(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

std::string ExtractBasenameLower(const std::string& path) {
    size_t sep = path.find_last_of("\\/");
    std::string base = (sep == std::string::npos) ? path : path.substr(sep + 1);
    return ToLowerAscii(base);
}

std::string ExtractDirname(const std::string& path) {
    size_t sep = path.find_last_of("\\/");
    if (sep == std::string::npos) return ".";
    if (sep == 0) return "/";
    return path.substr(0, sep);
}

bool EndsWithLower(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return ToLowerAscii(s.substr(s.size() - suffix.size())) == suffix;
}

bool StartsWithLower(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    return ToLowerAscii(s.substr(0, prefix.size())) == prefix;
}

std::string GetIniValue(
    const std::map<std::string, std::map<std::string, std::string>>& sections,
    const std::string& section,
    const std::string& key
) {
    auto sit = sections.find(section);
    if (sit == sections.end()) return "";
    auto kit = sit->second.find(key);
    if (kit == sit->second.end()) return "";
    return kit->second;
}

bool IsTruthyIni(const std::string& val) {
    std::string l = ToLowerAscii(Trim(val));
    return (l == "true" || l == "1" || l == "yes" || l == "on" || l == "auto");
}

bool IsExplicitTrueIni(const std::string& val) {
    std::string l = ToLowerAscii(Trim(val));
    return (l == "true" || l == "1" || l == "yes" || l == "on");
}

// Normalizes a DOS guest path (e.g. C:, \TOMB, subdir) into "C:\TOMB"
std::string UpdateGuestWorkingDir(const std::string& current_dir, const std::string& cd_arg) {
    std::string arg = Trim(StripSurroundingQuotes(cd_arg));
    if (arg.empty()) return current_dir;

    // Normalize slashes to backslash
    for (size_t i = 0; i < arg.size(); ++i) {
        if (arg[i] == '/') arg[i] = '\\';
    }

    char drive = 'C';
    std::string base_path;
    if (current_dir.size() >= 2 && std::isalpha(static_cast<unsigned char>(current_dir[0])) && current_dir[1] == ':') {
        drive = static_cast<char>(std::toupper(static_cast<unsigned char>(current_dir[0])));
        base_path = current_dir.substr(2);
    } else {
        base_path = current_dir;
    }

    if (arg.size() >= 2 && std::isalpha(static_cast<unsigned char>(arg[0])) && arg[1] == ':') {
        drive = static_cast<char>(std::toupper(static_cast<unsigned char>(arg[0])));
        arg = arg.substr(2);
        if (arg.empty()) arg = "\\";
    }

    std::vector<std::string> parts;
    if (!arg.empty() && arg[0] == '\\') {
        // Absolute from drive root
    } else {
        std::stringstream ss(base_path);
        std::string seg;
        while (std::getline(ss, seg, '\\')) {
            if (!seg.empty() && seg != ".") parts.push_back(seg);
        }
    }

    std::stringstream ss(arg);
    std::string seg;
    while (std::getline(ss, seg, '\\')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(ToUpperAscii(seg));
        }
    }

    std::string result;
    result.push_back(drive);
    result.push_back(':');
    if (parts.empty()) {
        result.push_back('\\');
    } else {
        for (size_t i = 0; i < parts.size(); ++i) {
            result.push_back('\\');
            result += parts[i];
        }
    }
    return result;
}

bool IsIgnoredAutoexecCommand(const std::string& lower_cmd) {
    static const char* kIgnored[] = {
        "echo", "@echo", "cls", "pause", "exit", "rem", "set", "path",
        "keyb", "mixer", "config", "boot", "ver", "prompt", "if", "goto",
        "del", "erase", "copy", "xcopy", "md", "mkdir", "rd", "rmdir",
        "subst", "rescan", "intro", "mem", "type", "dir", "vol", "label"
    };
    for (size_t i = 0; i < sizeof(kIgnored) / sizeof(kIgnored[0]); ++i) {
        if (lower_cmd == kIgnored[i]) return true;
    }
    return false;
}

bool ParseMountCommand(
    const std::vector<std::string>& tokens,
    bool is_imgmount,
    const std::string& conf_dir,
    ParsedMountDirective& out_dir
) {
    if (tokens.size() < 3) return false;

    // Find drive letter (e.g. "C", "C:", "d", "0", "1", "2")
    size_t idx = 1;
    std::string drv_tok = ToUpperAscii(tokens[idx]);
    if (drv_tok == "-U" || drv_tok == "/U") {
        // Unmount directive, ignore
        return false;
    }

    char drive_letter = 0;
    if (drv_tok.size() >= 1 && std::isalpha(static_cast<unsigned char>(drv_tok[0])) &&
        (drv_tok.size() == 1 || (drv_tok.size() == 2 && drv_tok[1] == ':'))) {
        drive_letter = drv_tok[0];
    } else if (drv_tok == "0") {
        drive_letter = 'A';
    } else if (drv_tok == "1") {
        drive_letter = 'B';
    } else if (drv_tok == "2") {
        drive_letter = 'C';
    } else {
        return false;
    }

    out_dir = ParsedMountDirective();
    out_dir.drive_letter = drive_letter;
    out_dir.is_imgmount = is_imgmount;

    for (size_t i = 2; i < tokens.size(); ++i) {
        std::string tok = tokens[i];
        std::string low = ToLowerAscii(tok);
        if (low == "-t" && i + 1 < tokens.size()) {
            std::string tval = ToLowerAscii(tokens[++i]);
            if (tval == "overlay") {
                out_dir.is_overlay = true;
            } else {
                out_dir.media_type = tval;
            }
        } else if (low == "-fs" && i + 1 < tokens.size()) {
            out_dir.fs_type = ToLowerAscii(tokens[++i]);
        } else if (low == "-label" || low == "-freesize" || low == "-size" ||
                   low == "-ide" || low == "-ioctl" || low == "-usecd" || low == "-io") {
            // Flags that may take an argument (except -ioctl which is standalone)
            if (low != "-ioctl" && i + 1 < tokens.size() && !tokens[i + 1].empty() && tokens[i + 1][0] != '-') {
                ++i;
            }
        } else if (!tok.empty() && tok[0] == '-') {
            // Unknown boolean switch
        } else {
            std::string clean_path = StripSurroundingQuotes(tok);
            if (!clean_path.empty()) {
                out_dir.raw_paths.push_back(clean_path);
                out_dir.resolved_paths.push_back(ConfTranslator::ResolveRelativeHostPath(conf_dir, clean_path));
            }
        }
    }

    return !out_dir.raw_paths.empty();
}

} // anonymous namespace

bool ConfTranslator::IsSecondaryProfileConf(const std::string& filename) {
    std::string lower = ExtractBasenameLower(filename);
    if (!EndsWithLower(lower, ".conf")) return false;
    static const char* kSecondarySuffixes[] = {
        "_settings.conf",
        "_setup.conf",
        "_server.conf",
        "_client.conf",
        "_multiplayer.conf",
        "_multi.conf",
        "_mp.conf",
        "_lan.conf",
        "_modem.conf",
        "_ipx.conf",
        "_editor.conf",
        "_manual.conf",
        "_config.conf"
    };
    for (size_t i = 0; i < sizeof(kSecondarySuffixes) / sizeof(kSecondarySuffixes[0]); ++i) {
        if (EndsWithLower(lower, kSecondarySuffixes[i])) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> ConfTranslator::TokenizeCommandLine(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_quotes = false;
    bool has_token = false;
    char quote_char = 0;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quotes) {
            if (c == quote_char) {
                in_quotes = false;
            } else {
                current.push_back(c);
            }
        } else {
            if (c == '"' || c == '\'') {
                in_quotes = true;
                has_token = true;
                quote_char = c;
            } else if (c == ' ' || c == '\t') {
                if (has_token) {
                    tokens.push_back(current);
                    current.clear();
                    has_token = false;
                }
            } else {
                current.push_back(c);
                has_token = true;
            }
        }
    }
    if (has_token) {
        tokens.push_back(current);
    }
    return tokens;
}

std::string ConfTranslator::ResolveRelativeHostPath(const std::string& base_dir, const std::string& rel_path) {
    std::string norm_rel = rel_path;
    for (size_t i = 0; i < norm_rel.size(); ++i) {
        if (norm_rel[i] == '\\') norm_rel[i] = '/';
    }

    // Check if already absolute (POSIX '/' or Win32 'C:/')
    const bool is_abs_posix = (!norm_rel.empty() && norm_rel[0] == '/');
    const bool is_abs_win = (norm_rel.size() >= 2 &&
                             std::isalpha(static_cast<unsigned char>(norm_rel[0])) &&
                             norm_rel[1] == ':');

    std::string combined;
    if (is_abs_posix || is_abs_win || base_dir.empty()) {
        combined = norm_rel;
    } else {
        std::string norm_base = base_dir;
        for (size_t i = 0; i < norm_base.size(); ++i) {
            if (norm_base[i] == '\\') norm_base[i] = '/';
        }
        while (norm_base.size() > 1 && norm_base.back() == '/') {
            norm_base.pop_back();
        }
        combined = norm_base + "/" + norm_rel;
    }

    // Extract optional Windows drive prefix or leading slash
    std::string prefix;
    size_t start_idx = 0;
    if (combined.size() >= 2 && std::isalpha(static_cast<unsigned char>(combined[0])) && combined[1] == ':') {
        prefix = combined.substr(0, 2);
        start_idx = 2;
        if (start_idx < combined.size() && combined[start_idx] == '/') {
            prefix.push_back('/');
            ++start_idx;
        }
    } else if (!combined.empty() && combined[0] == '/') {
        prefix = "/";
        start_idx = 1;
    }

    std::vector<std::string> parts;
    std::stringstream ss(combined.substr(start_idx));
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (prefix.empty()) {
                parts.push_back("..");
            }
        } else {
            parts.push_back(seg);
        }
    }

    std::string result = prefix;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result.push_back('/');
        result += parts[i];
    }
    if (result.empty()) result = ".";
    return result;
}

int ConfTranslator::ParseCyclesValue(const std::string& raw_cycles) {
    std::string lower = ToLowerAscii(Trim(raw_cycles));
    if (lower.empty() || lower == "auto") return 0;
    if (lower.find("max") != std::string::npos) return -1;

    // Handle "fixed 15000", "15000", "auto 12000", "fixed 15000 100%"
    std::stringstream ss(lower);
    std::string tok;
    while (ss >> tok) {
        if (tok == "fixed" || tok == "auto" || tok == "limit") continue;
        // Check if tok starts with a digit
        if (!tok.empty() && std::isdigit(static_cast<unsigned char>(tok[0]))) {
            // Skip percentage tokens like "100%"
            if (tok.find('%') != std::string::npos) continue;
            errno = 0;
            char* endp = nullptr;
            long val = std::strtol(tok.c_str(), &endp, 10);
            if (errno == 0 && endp && *endp == '\0' && val > 0 && val <= INT_MAX) {
                return static_cast<int>(val);
            }
        }
    }
    return 0;
}

bool ConfTranslator::ParseConfString(
    const std::string& content,
    const std::string& conf_dir,
    ConfTranslationResult& in_out_result
) {
    // Strip UTF-8 BOM if present so first [section] header parses cleanly
    std::string clean_content = content;
    if (clean_content.size() >= 3 &&
        static_cast<unsigned char>(clean_content[0]) == 0xEF &&
        static_cast<unsigned char>(clean_content[1]) == 0xBB &&
        static_cast<unsigned char>(clean_content[2]) == 0xBF) {
        clean_content.erase(0, 3);
    }

    std::istringstream iss(clean_content);
    std::string raw_line;
    std::string current_section;
    std::string current_guest_dir = in_out_result.guest_working_dir.empty()
                                        ? "C:\\"
                                        : in_out_result.guest_working_dir;
    std::map<char, std::string> drive_cwds;
    if (current_guest_dir.size() >= 2 && std::isalpha(static_cast<unsigned char>(current_guest_dir[0])) && current_guest_dir[1] == ':') {
        drive_cwds[static_cast<char>(std::toupper(static_cast<unsigned char>(current_guest_dir[0])))] = current_guest_dir;
    }

    while (std::getline(iss, raw_line)) {
        std::string line = Trim(raw_line);
        if (line.empty()) continue;

        // Section header
        if (line.front() == '[' && line.back() == ']' && line.size() >= 3) {
            current_section = ToLowerAscii(Trim(line.substr(1, line.size() - 2)));
            continue;
        }

        if (current_section == "autoexec") {
            // Skip comments
            if (line[0] == '#' || (line.size() >= 2 && line[0] == ':' && line[1] == ':')) {
                continue;
            }
            if (line[0] == ':') {
                // Batch label
                continue;
            }
            in_out_result.autoexec_lines.push_back(line);

            // Strip leading '@'
            std::string exec_line = line;
            if (!exec_line.empty() && exec_line[0] == '@') {
                exec_line = Trim(exec_line.substr(1));
            }
            if (exec_line.empty()) continue;

            std::vector<std::string> tokens = TokenizeCommandLine(exec_line);
            if (tokens.empty()) continue;

            std::string cmd_lower = ToLowerAscii(tokens[0]);

            // Handle "call <cmd> ..."
            if (cmd_lower == "call" && tokens.size() >= 2) {
                tokens.erase(tokens.begin());
                cmd_lower = ToLowerAscii(tokens[0]);
            }

            // Handle drive letter switch (e.g., "c:" or "d:"), restoring saved per-drive CWD
            if (cmd_lower.size() == 2 && std::isalpha(static_cast<unsigned char>(cmd_lower[0])) && cmd_lower[1] == ':') {
                if (current_guest_dir.size() >= 2 && current_guest_dir[1] == ':') {
                    drive_cwds[static_cast<char>(std::toupper(static_cast<unsigned char>(current_guest_dir[0])))] = current_guest_dir;
                }
                char drv = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd_lower[0])));
                auto it = drive_cwds.find(drv);
                current_guest_dir = (it != drive_cwds.end()) ? it->second : (std::string(1, drv) + ":\\");
                continue;
            }

            // Handle cd / chdir (including "cd..", "cd\foo", and drive-qualified "cd C:SUB" / "cd /d C:SUB")
            if (cmd_lower == "cd" || cmd_lower == "chdir") {
                if (tokens.size() >= 2) {
                    size_t arg_idx = 1;
                    bool switch_drive = false;
                    if (ToLowerAscii(tokens[arg_idx]) == "/d" && tokens.size() >= 3) {
                        switch_drive = true;
                        ++arg_idx;
                    }
                    std::string cd_arg = StripSurroundingQuotes(tokens[arg_idx]);
                    char active_drv = (current_guest_dir.size() >= 2 && current_guest_dir[1] == ':')
                                          ? static_cast<char>(std::toupper(static_cast<unsigned char>(current_guest_dir[0])))
                                          : 'C';
                    char target_drv = active_drv;
                    if (cd_arg.size() >= 2 && std::isalpha(static_cast<unsigned char>(cd_arg[0])) && cd_arg[1] == ':') {
                        target_drv = static_cast<char>(std::toupper(static_cast<unsigned char>(cd_arg[0])));
                    }
                    auto it = drive_cwds.find(target_drv);
                    std::string base_for_target = (it != drive_cwds.end())
                                                      ? it->second
                                                      : (std::string(1, target_drv) + ":\\");
                    std::string updated = UpdateGuestWorkingDir(base_for_target, cd_arg);
                    drive_cwds[target_drv] = updated;
                    if (switch_drive || target_drv == active_drv) {
                        current_guest_dir = updated;
                    }
                }
                continue;
            }
            if (StartsWithLower(cmd_lower, "cd\\") || StartsWithLower(cmd_lower, "cd/") || cmd_lower == "cd..") {
                std::string arg = tokens[0].substr(2);
                current_guest_dir = UpdateGuestWorkingDir(current_guest_dir, arg);
                if (current_guest_dir.size() >= 2 && current_guest_dir[1] == ':') {
                    drive_cwds[static_cast<char>(std::toupper(static_cast<unsigned char>(current_guest_dir[0])))] = current_guest_dir;
                }
                continue;
            }

            // Handle mount / imgmount
            if (cmd_lower == "mount" || cmd_lower == "imgmount") {
                ParsedMountDirective dir;
                if (ParseMountCommand(tokens, cmd_lower == "imgmount", conf_dir, dir)) {
                    if (!dir.is_imgmount && dir.drive_letter == 'C') {
                        if (dir.is_overlay) {
                            for (size_t p = 0; p < dir.resolved_paths.size(); ++p) {
                                in_out_result.overlay_dirs.push_back(dir.resolved_paths[p]);
                            }
                        } else if (!dir.resolved_paths.empty()) {
                            in_out_result.resolved_game_root = dir.resolved_paths[0];
                        }
                    } else {
                        in_out_result.media_mounts.push_back(dir);
                    }
                }
                continue;
            }

            if (IsIgnoredAutoexecCommand(cmd_lower)) {
                continue;
            }

            // Skip batch control constructs or redirects like "> nul"
            if (cmd_lower == "lh" || cmd_lower == "loadhigh") {
                if (tokens.size() >= 2) {
                    tokens.erase(tokens.begin());
                    cmd_lower = ToLowerAscii(tokens[0]);
                } else {
                    continue;
                }
            }

            // Treat as executable invocation
            std::string exec_tok = tokens[0];
            // If executable has a path prefix (e.g. "subdir\game.exe"), split directory into guest_working_dir
            // unless it's a wrapper like dos4gw.exe or loadfix
            size_t slash_pos = exec_tok.find_last_of("\\/");
            std::string effective_dir = current_guest_dir;
            std::string effective_exe = exec_tok;
            if (slash_pos != std::string::npos) {
                std::string dir_part = (slash_pos == 0) ? "\\" : exec_tok.substr(0, slash_pos);
                // Preserve root backslash for drive-rooted paths like "C:\GAME.EXE"
                if (slash_pos == 2 && exec_tok.size() >= 3 && exec_tok[1] == ':') {
                    dir_part = exec_tok.substr(0, 3);
                }
                effective_exe = exec_tok.substr(slash_pos + 1);
                if (!dir_part.empty()) {
                    effective_dir = UpdateGuestWorkingDir(current_guest_dir, dir_part);
                }
            }

            // Reassemble arguments (stopping before shell redirection '>' or '|')
            std::string args;
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (!tokens[i].empty() && (tokens[i][0] == '>' || tokens[i][0] == '|')) {
                    break;
                }
                if (!args.empty()) args.push_back(' ');
                if (tokens[i].empty() || tokens[i].find(' ') != std::string::npos) {
                    args += "\"" + tokens[i] + "\"";
                } else {
                    args += tokens[i];
                }
            }

            if (!effective_exe.empty()) {
                in_out_result.guest_working_dir = effective_dir;
                in_out_result.guest_executable = ToUpperAscii(effective_exe);
                in_out_result.guest_arguments = args;
            }
        } else {
            // Standard INI section: skip comments
            if (line[0] == '#' || line[0] == ';') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = ToLowerAscii(Trim(line.substr(0, eq)));
            std::string val = Trim(line.substr(eq + 1));
            // Strip inline '#' comments from INI values
            size_t hash = val.find('#');
            if (hash != std::string::npos) {
                val = Trim(val.substr(0, hash));
            }
            if (!current_section.empty() && !key.empty()) {
                in_out_result.merged_sections[current_section][key] = val;
            }
        }
    }

    if (in_out_result.guest_working_dir.empty()) {
        in_out_result.guest_working_dir = current_guest_dir;
    }

    return true;
}

bool ConfTranslator::ParseAndMergeConfFiles(
    const std::vector<std::string>& conf_paths,
    ConfTranslationResult& out_result
) {
    out_result = ConfTranslationResult();

    // Order base .conf files before *_single.conf so _single.conf [autoexec] takes precedence
    std::vector<std::string> base_confs;
    std::vector<std::string> single_confs;
    for (size_t i = 0; i < conf_paths.size(); ++i) {
        const std::string& p = conf_paths[i];
        if (IsSecondaryProfileConf(p)) continue;
        std::string low = ExtractBasenameLower(p);
        if (EndsWithLower(low, "_single.conf")) {
            single_confs.push_back(p);
        } else {
            base_confs.push_back(p);
        }
    }
    std::sort(base_confs.begin(), base_confs.end());
    std::sort(single_confs.begin(), single_confs.end());

    std::vector<std::string> ordered = base_confs;
    ordered.insert(ordered.end(), single_confs.begin(), single_confs.end());

    bool parsed_any = false;
    for (size_t i = 0; i < ordered.size(); ++i) {
        std::ifstream f(ordered[i].c_str(), std::ios::in | std::ios::binary);
        if (!f.is_open()) continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        if (ParseConfString(ss.str(), ExtractDirname(ordered[i]), out_result)) {
            parsed_any = true;
        }
    }

    if (parsed_any) {
        std::string override_delta;
        out_result.recommended_archetype = ScoreArchetype(out_result, false, false, override_delta);
        out_result.custom_conf_overrides = override_delta;
    }

    return parsed_any;
}

std::string ConfTranslator::ScoreArchetype(
    const ConfTranslationResult& parsed,
    bool has_win9x_pe,
    bool has_dos_extender_or_vbe,
    std::string& out_override_delta
) {
    out_override_delta.clear();

    const std::string machine_val = ToLowerAscii(GetIniValue(parsed.merged_sections, "dosbox", "machine"));
    const std::string memsize_val = Trim(GetIniValue(parsed.merged_sections, "dosbox", "memsize"));
    int memsize_mb = 0;
    if (!memsize_val.empty() && std::isdigit(static_cast<unsigned char>(memsize_val[0]))) {
        errno = 0;
        char* endp = nullptr;
        long parsed_mem = std::strtol(memsize_val.c_str(), &endp, 10);
        if (errno == 0 && endp && *endp == '\0' && parsed_mem > 0 && parsed_mem <= INT_MAX) {
            memsize_mb = static_cast<int>(parsed_mem);
        }
    }

    const std::string cycles_val = GetIniValue(parsed.merged_sections, "cpu", "cycles");
    const int cycles_num = ParseCyclesValue(cycles_val);
    const std::string cputype_val = ToLowerAscii(GetIniValue(parsed.merged_sections, "cpu", "cputype"));

    const std::string glide_val = GetIniValue(parsed.merged_sections, "glide", "glide");
    const std::string voodoo_val = ToLowerAscii(GetIniValue(parsed.merged_sections, "voodoo", "voodoo_card"));
    const std::string voodoo_legacy = GetIniValue(parsed.merged_sections, "pci", "voodoo");

    const std::string sbtype_val = ToLowerAscii(GetIniValue(parsed.merged_sections, "sblaster", "sbtype"));
    const std::string gus_val = GetIniValue(parsed.merged_sections, "gus", "gus");
    const std::string ems_val = GetIniValue(parsed.merged_sections, "dos", "ems");

    const bool wants_3dfx =
        IsExplicitTrueIni(glide_val) ||
        (!voodoo_val.empty() && voodoo_val != "false" && voodoo_val != "none") ||
        IsExplicitTrueIni(voodoo_legacy) ||
        (cputype_val.find("pentium") != std::string::npos);

    // Tier 5: ARCH_PENTIUM_3DFX
    if (has_win9x_pe || wants_3dfx || memsize_mb >= 32) {
        return KioskArchetypes::PENTIUM_3DFX;
    }

    // Tier 4: ARCH_486DX2
    if (cycles_num == -1 || cycles_num > 15000 || has_dos_extender_or_vbe ||
        IsExplicitTrueIni(gus_val) || cputype_val.find("486") != std::string::npos) {
        if (cycles_num == -1 || cycles_num > 40000) {
            out_override_delta = "[cpu]\ncycles = max\n";
        }
        return KioskArchetypes::DX2_486;
    }

    // Tier 3: ARCH_386DX
    if ((cycles_num >= 3000 && cycles_num <= 15000) ||
        StartsWithLower(machine_val, "svga") ||
        StartsWithLower(sbtype_val, "sbpro") ||
        sbtype_val == "sb16" ||
        IsExplicitTrueIni(ems_val) ||
        cputype_val.find("386") != std::string::npos) {
        return KioskArchetypes::DX_386;
    }

    // Tier 2: ARCH_AT_286
    if (machine_val == "ega" || (cycles_num >= 600 && cycles_num < 3000) ||
        cputype_val.find("286") != std::string::npos) {
        return KioskArchetypes::AT_286;
    }

    // Tier 1: ARCH_XT_8088
    if (machine_val == "cga" || machine_val == "hercules" || machine_val == "tandy" ||
        machine_val == "pcjr" || (cycles_num > 0 && cycles_num < 600) ||
        cputype_val.find("8086") != std::string::npos || cputype_val.find("8088") != std::string::npos) {
        return KioskArchetypes::XT_8088;
    }

    // Default safe middle-ground for unclassified DOS games
    return KioskArchetypes::DX_386;
}

#endif // defined(C_KIOSK)
