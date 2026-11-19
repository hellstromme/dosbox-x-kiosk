#include "config.h"

#if defined(C_KIOSK)

#include "runtime_overlay.h"
#include "../kiosk_main.h"
#include "../kiosk_db.h"
#include "../../dos/drives.h"
#include "bios_disk.h"
#include "ide.h"
#include "mixer.h"
#include "keyboard.h"
#include "logging.h"
#include "imgui.h"
#include "SDL.h"

#include <cstdio>
#include <cctype>
#include <algorithm>

namespace {

std::string NormalizeDriveLetter(const std::string& drv) {
    if (drv.empty()) return "A";
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(drv[0])));
    if (c < 'A' || c > 'Z') return "A";
    return std::string(1, c);
}

std::string BasenameFromPath(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

bool PathsMatchNormalized(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = (a[i] == '\\') ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        char cb = (b[i] == '\\') ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) return false;
    }
    return true;
}

void EnrichMediaRecordFromInspector(MediaPoolRecord& media) {
    if (media.file_path.empty()) return;

    InspectedMediaInfo info;
    if (DiskCaddyView::InspectMediaFile(media.file_path, info)) {
        if (media.media_type.empty()) {
            media.media_type = info.media_type;
        }
        if (media.volume_label.empty() && !info.volume_label.empty()) {
            media.volume_label = info.volume_label;
        }
        if (media.label_title.empty()) {
            media.label_title = !info.label_title.empty() ? info.label_title : BasenameFromPath(media.file_path);
        }
        if (media.label_subtitle.empty()) {
            media.label_subtitle = info.label_subtitle;
        }
        if (info.has_redbook_audio) {
            media.has_redbook_audio = true;
        }
        media.physical_style = DiskCaddyView::ResolvePhysicalStyle(
            media.media_type,
            media.physical_style,
            media.id.empty() ? media.file_path : media.id
        );
    } else {
        if (media.media_type.empty()) {
            media.media_type = "FLOPPY_35_1440K";
        }
        if (media.label_title.empty()) {
            media.label_title = BasenameFromPath(media.file_path);
        }
        if (media.label_subtitle.empty()) {
            media.label_subtitle = DiskCaddyView::HumanReadableMediaTypeBadge(media.media_type);
        }
        media.physical_style = DiskCaddyView::ResolvePhysicalStyle(
            media.media_type,
            media.physical_style,
            media.id.empty() ? media.file_path : media.id
        );
    }
}

bool IsOpticalMediaType(const std::string& media_type) {
    return (media_type == "CDROM_ISO" || media_type == "CDROM_CUE");
}

} // anonymous namespace

RuntimeOverlay& RuntimeOverlay::GetInstance() {
    static RuntimeOverlay instance;
    return instance;
}

void RuntimeOverlay::ToggleViewMode() {
    m_view_mode = (m_view_mode == CaddyViewMode::SKEUOMORPHIC)
                      ? CaddyViewMode::LIST
                      : CaddyViewMode::SKEUOMORPHIC;
    LOG_MSG("KIOSK Overlay: Toggled CaddyViewMode to %s.",
            (m_view_mode == CaddyViewMode::SKEUOMORPHIC) ? "SKEUOMORPHIC" : "LIST");
}

void RuntimeOverlay::OnEnterOverlay() {
    m_session_caddy.clear();
    m_global_pool_cache.clear();
    m_selected_caddy_index = 0;
    m_show_global_pool_drawer = false;
    m_status_message = "Select a diskette or CD-ROM to hot-swap, or press V / F2 / Gamepad Y to switch views.";

    const KioskConfigRecord cfg = KioskManager::GetInstance().GetConfig();
    m_view_mode = (cfg.default_caddy_view == "LIST") ? CaddyViewMode::LIST : CaddyViewMode::SKEUOMORPHIC;

    const LaunchContext ctx = KioskManager::GetInstance().GetCurrentLaunchContext();
    const std::string dispatch_path = KioskManager::GetInstance().GetActiveDispatchImagePath();
    const bool is_b_dispatch_reserved =
        ctx.is_real_hdd_boot && !ctx.catalog_item_id.empty() && !dispatch_path.empty();

    KioskDatabase& db = KioskDatabase::GetInstance();
    m_active_machine_name = "Default DOSBox-X Machine";
    m_active_item_title.clear();

    if (db.IsOpen()) {
        if (!ctx.machine_id.empty()) {
            MachineRecord mach;
            if (db.GetMachine(ctx.machine_id, mach) && !mach.display_name.empty()) {
                m_active_machine_name = mach.display_name;
            }
        }
        if (!ctx.catalog_item_id.empty()) {
            CatalogItemRecord item;
            if (db.GetItem(ctx.catalog_item_id, item) && !item.title.empty()) {
                m_active_item_title = item.title;
            }
        }

        std::vector<CaddyBindingRecord> bindings;
        if (!ctx.catalog_item_id.empty()) {
            bindings = db.GetCaddyBindingsForItem(ctx.catalog_item_id);
        }
        if (bindings.empty() && !ctx.machine_id.empty()) {
            bindings = db.GetCaddyBindingsForMachine(ctx.machine_id);
        }

        for (size_t i = 0; i < bindings.size(); ++i) {
            RuntimeCaddyEntry entry;
            entry.binding = bindings[i];
            entry.binding.target_drive = NormalizeDriveLetter(entry.binding.target_drive);
            if (is_b_dispatch_reserved && entry.binding.target_drive == "B") {
                entry.binding.target_drive = "A";
            }

            bool found_media = false;
            if (!entry.binding.media_id.empty()) {
                found_media = db.GetMedia(entry.binding.media_id, entry.media);
            }
            if (!found_media && !entry.binding.resolved_file_path.empty()) {
                found_media = db.GetMediaByPath(entry.binding.resolved_file_path, entry.media);
            }
            if (!found_media) {
                entry.media.id = entry.binding.media_id;
                entry.media.file_path = entry.binding.resolved_file_path;
                entry.media.media_type = entry.binding.resolved_media_type;
                entry.media.label_title = entry.binding.resolved_label;
                entry.media.physical_style.clear();
            } else if (entry.media.physical_style == "BEIGE_35" &&
                       entry.media.media_type != "FLOPPY_525_360K" &&
                       entry.media.media_type != "FLOPPY_525_1200K") {
                // Allow deterministic variety across Beige/Blue/Black if default style
                entry.media.physical_style.clear();
            }

            EnrichMediaRecordFromInspector(entry.media);
            m_session_caddy.push_back(entry);
        }

        m_global_pool_cache = db.GetAllMedia();
        for (size_t i = 0; i < m_global_pool_cache.size(); ++i) {
            EnrichMediaRecordFromInspector(m_global_pool_cache[i]);
        }
    }

    // Also synthesize caddy entries for any disks currently mounted in DriveManager (A:, B:, D:, E:)
    // that were not already listed in SQLite session_caddy
    const int check_drives[4] = { 0, 1, 3, 4 };
    for (int d = 0; d < 4; ++d) {
        const int drv_idx = check_drives[d];
        const std::string drv_letter(1, static_cast<char>('A' + drv_idx));
        const std::vector<std::string> mounted_paths = DriveManager::GetKioskDrivePaths(drv_idx);

        for (size_t p = 0; p < mounted_paths.size(); ++p) {
            const std::string& mpath = mounted_paths[p];
            if (mpath.empty()) continue;
            if (!dispatch_path.empty() && PathsMatchNormalized(mpath, dispatch_path)) {
                continue; // Hide internal ephemeral dispatch volume from user disk shelf
            }

            bool already_listed = false;
            for (size_t c = 0; c < m_session_caddy.size(); ++c) {
                if (PathsMatchNormalized(m_session_caddy[c].media.file_path, mpath) &&
                    m_session_caddy[c].binding.target_drive == drv_letter) {
                    already_listed = true;
                    break;
                }
            }
            if (!already_listed) {
                RuntimeCaddyEntry synth;
                synth.binding.target_drive = drv_letter;
                synth.binding.slot_order = static_cast<int>(p + 1);
                synth.binding.resolved_file_path = mpath;
                synth.media.file_path = mpath;
                synth.media.physical_style.clear();
                if (drv_idx >= 2) {
                    synth.media.media_type = "CDROM_ISO";
                }
                EnrichMediaRecordFromInspector(synth.media);
                m_session_caddy.push_back(synth);
            }
        }
    }

    MixerChannel* cd_chan = MIXER_FindChannel("CDAUDIO");
    if (cd_chan != nullptr) {
        m_cd_volume_percent = std::max(0.0f, std::min(100.0f, cd_chan->scale[0] * 100.0f));
    }

    RefreshDriveStatuses();
}

void RuntimeOverlay::RefreshDriveStatuses() {
    m_drive_statuses.clear();

    const LaunchContext ctx = KioskManager::GetInstance().GetCurrentLaunchContext();
    const std::string dispatch_path = KioskManager::GetInstance().GetActiveDispatchImagePath();
    const bool is_b_dispatch_reserved =
        ctx.is_real_hdd_boot && !ctx.catalog_item_id.empty() && !dispatch_path.empty();

    bool include_drive_e = (DriveManager::GetKioskSlotCount(4) > 0);
    for (size_t i = 0; i < m_session_caddy.size(); ++i) {
        if (m_session_caddy[i].binding.target_drive == "E") {
            include_drive_e = true;
            break;
        }
    }

    std::vector<int> drives_to_show;
    drives_to_show.push_back(0); // A:
    drives_to_show.push_back(1); // B:
    drives_to_show.push_back(3); // D:
    if (include_drive_e) {
        drives_to_show.push_back(4); // E:
    }

    for (size_t i = 0; i < drives_to_show.size(); ++i) {
        const int drv = drives_to_show[i];
        RuntimeDriveSlotStatus st;
        st.drive_index = drv;
        st.drive_letter = std::string(1, static_cast<char>('A' + drv));
        st.is_optical = (drv >= 2);
        st.is_dispatch_reserved = (drv == 1 && is_b_dispatch_reserved);
        st.active_file_path = DriveManager::GetKioskActiveDiskPath(drv);
        st.current_slot_index = DriveManager::GetKioskCurrentSlot(drv);
        st.total_slots = DriveManager::GetKioskSlotCount(drv);

        if (st.is_optical) {
            uint8_t sk = 0, asc = 0, ascq = 0;
            int load_mode = 0;
            const bool has_atapi = Kiosk_IDE_GetATAPISense(static_cast<unsigned char>(drv), sk, asc, ascq, load_mode);
            const bool ejected = has_atapi && (load_mode == 0 /* LOAD_NO_DISC */);
            st.is_mounted = !st.active_file_path.empty() && !ejected;
            st.is_write_protected = true;
        } else {
            st.is_mounted = !st.active_file_path.empty() && (imageDiskList[drv] != NULL);
            st.is_write_protected = FDC_IsWriteProtected(drv);
        }

        if (st.is_dispatch_reserved) {
            st.active_label = "KIOSK_BOOT (System Dispatch)";
            st.media_type_badge = "SYSTEM DISPATCH - LOCKED";
        } else if (st.is_mounted) {
            // Find matching caddy record for friendly label/badge
            for (size_t c = 0; c < m_session_caddy.size(); ++c) {
                if (PathsMatchNormalized(m_session_caddy[c].media.file_path, st.active_file_path)) {
                    st.active_label = !m_session_caddy[c].media.label_title.empty()
                                          ? m_session_caddy[c].media.label_title
                                          : m_session_caddy[c].media.volume_label;
                    st.media_type_badge = DiskCaddyView::HumanReadableMediaTypeBadge(
                        m_session_caddy[c].media.media_type
                    );
                    m_session_caddy[c].media.is_write_protected = st.is_write_protected;
                    break;
                }
            }
            if (st.active_label.empty()) {
                st.active_label = BasenameFromPath(st.active_file_path);
            }
            if (st.media_type_badge.empty()) {
                st.media_type_badge = st.is_optical ? "CD-ROM ISO/CUE" : "Floppy Diskette";
            }
        } else {
            st.active_label = "(Empty / Door Open)";
            st.media_type_badge = st.is_optical ? "ATAPI Optical Drive" : "Floppy Disk Drive";
        }

        m_drive_statuses.push_back(st);
    }
}

bool RuntimeOverlay::PerformHotSwap(
    const std::string& target_drive,
    const std::string& host_path,
    bool write_protected
) {
    if (host_path.empty()) {
        return false;
    }

    std::string drv_letter = NormalizeDriveLetter(target_drive);
    const LaunchContext ctx = KioskManager::GetInstance().GetCurrentLaunchContext();
    const std::string dispatch_path = KioskManager::GetInstance().GetActiveDispatchImagePath();
    if (drv_letter == "B" && ctx.is_real_hdd_boot && !ctx.catalog_item_id.empty() && !dispatch_path.empty()) {
        LOG_MSG("KIOSK Overlay: Drive B: is reserved for ephemeral dispatch volume; routing hot-swap of '%s' to Drive A:.",
                host_path.c_str());
        drv_letter = "A";
    }

    const int drive_idx = drv_letter[0] - 'A';
    const bool is_cdrom = (drive_idx >= 2);
    const bool final_wp = is_cdrom ? true : write_protected;

    if (!DriveManager::AppendAndSwapDisk(drive_idx, host_path, is_cdrom, final_wp)) {
        m_status_message = "Failed to mount '" + BasenameFromPath(host_path) + "' into Drive " + drv_letter + ":.";
        return false;
    }

    // Ensure disk is represented in m_session_caddy
    bool found_in_caddy = false;
    for (size_t i = 0; i < m_session_caddy.size(); ++i) {
        if (m_session_caddy[i].binding.target_drive == drv_letter &&
            PathsMatchNormalized(m_session_caddy[i].media.file_path, host_path)) {
            m_session_caddy[i].media.is_write_protected = final_wp;
            found_in_caddy = true;
            break;
        }
    }
    if (!found_in_caddy) {
        RuntimeCaddyEntry added;
        added.binding.target_drive = drv_letter;
        added.binding.slot_order = static_cast<int>(m_session_caddy.size() + 1);
        added.binding.resolved_file_path = host_path;
        added.media.file_path = host_path;
        added.media.is_write_protected = final_wp;
        added.media.physical_style.clear();
        added.is_from_global_pool = true;
        EnrichMediaRecordFromInspector(added.media);
        m_session_caddy.push_back(added);
    }

    RefreshDriveStatuses();
    m_status_message = "Inserted '" + BasenameFromPath(host_path) + "' into Drive " + drv_letter +
                       ": (" + (is_cdrom ? "ATAPI Unit Attention 0x06/0x28" : "FDC 0x3F7 DSKCHG 100ms") + ").";
    return true;
}

bool RuntimeOverlay::PerformHotSwapCaddySlot(size_t caddy_index) {
    if (caddy_index >= m_session_caddy.size()) {
        return false;
    }
    const RuntimeCaddyEntry& entry = m_session_caddy[caddy_index];
    m_selected_caddy_index = static_cast<int>(caddy_index);
    return PerformHotSwap(
        entry.binding.target_drive,
        entry.media.file_path,
        entry.media.is_write_protected
    );
}

bool RuntimeOverlay::EjectDrive(const std::string& target_drive) {
    const std::string drv_letter = NormalizeDriveLetter(target_drive);
    const int drive_idx = drv_letter[0] - 'A';

    for (size_t i = 0; i < m_drive_statuses.size(); ++i) {
        if (m_drive_statuses[i].drive_index == drive_idx && m_drive_statuses[i].is_dispatch_reserved) {
            m_status_message = "Drive B: is locked for the Kiosk system dispatch volume during this session.";
            return false;
        }
    }

    const bool is_cdrom = (drive_idx >= 2);
    if (!DriveManager::EjectDriveMedia(drive_idx, is_cdrom)) {
        return false;
    }

    RefreshDriveStatuses();
    m_status_message = "Ejected media from Drive " + drv_letter + ":.";
    return true;
}

bool RuntimeOverlay::ToggleDriveWriteProtect(const std::string& target_drive) {
    const std::string drv_letter = NormalizeDriveLetter(target_drive);
    const int drive_idx = drv_letter[0] - 'A';
    if (drive_idx < 0 || drive_idx >= 2) {
        return false;
    }

    for (size_t i = 0; i < m_drive_statuses.size(); ++i) {
        if (m_drive_statuses[i].drive_index == drive_idx && m_drive_statuses[i].is_dispatch_reserved) {
            return false;
        }
    }

    const bool new_wp = !FDC_IsWriteProtected(drive_idx);
    FDC_SetWriteProtected(drive_idx, new_wp);
    if (drive_idx >= 0 && drive_idx < DOS_DRIVES && Drives[drive_idx] != NULL) {
        Drives[drive_idx]->readonly = new_wp;
    }

    const std::string active_path = DriveManager::GetKioskActiveDiskPath(drive_idx);
    for (size_t i = 0; i < m_session_caddy.size(); ++i) {
        if (PathsMatchNormalized(m_session_caddy[i].media.file_path, active_path)) {
            m_session_caddy[i].media.is_write_protected = new_wp;
        }
    }

    RefreshDriveStatuses();
    m_status_message = std::string("Drive ") + drv_letter + ": Write Protection " +
                       (new_wp ? "ENABLED (Read-Only)." : "DISABLED (Writable).");
    return true;
}

void RuntimeOverlay::SetCdAudioVolumePercent(float percent) {
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;
    m_cd_volume_percent = percent;

    MixerChannel* cd_chan = MIXER_FindChannel("CDAUDIO");
    if (cd_chan != nullptr) {
        const float scale = m_cd_volume_percent / 100.0f;
        cd_chan->SetScale(scale, scale);
    }
}

void RuntimeOverlay::SendCtrlAltDelToGuest() {
    LOG_MSG("KIOSK Overlay: Sending Ctrl+Alt+Del keystroke sequence to guest OS.");
    KEYBOARD_AddKey(KBD_leftctrl, true);
    KEYBOARD_AddKey(KBD_leftalt, true);
    KEYBOARD_AddKey(KBD_delete, true);
    KEYBOARD_AddKey(KBD_delete, false);
    KEYBOARD_AddKey(KBD_leftalt, false);
    KEYBOARD_AddKey(KBD_leftctrl, false);
    m_status_message = "Sent Ctrl+Alt+Del to guest OS.";
    KioskManager::GetInstance().ResumeEmulation();
}

bool RuntimeOverlay::HandleInputEvent(const SDL_Event& event) {
    if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
        const SDL_Keycode sym = event.key.keysym.sym;
        if (sym == SDLK_v || sym == SDLK_F2) {
            ToggleViewMode();
            return true;
        }
        if (!m_session_caddy.empty()) {
            if (sym == SDLK_LEFT) {
                m_selected_caddy_index = (m_selected_caddy_index <= 0)
                                             ? static_cast<int>(m_session_caddy.size() - 1)
                                             : (m_selected_caddy_index - 1);
                return true;
            }
            if (sym == SDLK_RIGHT) {
                m_selected_caddy_index = (m_selected_caddy_index + 1) % static_cast<int>(m_session_caddy.size());
                return true;
            }
            if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                PerformHotSwapCaddySlot(static_cast<size_t>(m_selected_caddy_index));
                return true;
            }
        }
    } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_Y) {
            ToggleViewMode();
            return true;
        }
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_B) {
            KioskManager::GetInstance().ResumeEmulation();
            return true;
        }
        if (!m_session_caddy.empty()) {
            if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
                m_selected_caddy_index = (m_selected_caddy_index <= 0)
                                             ? static_cast<int>(m_session_caddy.size() - 1)
                                             : (m_selected_caddy_index - 1);
                return true;
            }
            if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                m_selected_caddy_index = (m_selected_caddy_index + 1) % static_cast<int>(m_session_caddy.size());
                return true;
            }
            if (event.cbutton.button == SDL_CONTROLLER_BUTTON_A) {
                PerformHotSwapCaddySlot(static_cast<size_t>(m_selected_caddy_index));
                return true;
            }
        }
    }
    return false;
}

void RuntimeOverlay::RenderHeaderBar() {
    ImGui::TextColored(ImVec4(0.96f, 0.76f, 0.26f, 1.0f), "MACHINE: %s", m_active_machine_name.c_str());
    if (!m_active_item_title.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.86f, 0.96f, 1.0f), "  |  TITLE: %s", m_active_item_title.c_str());
    }

    const char* view_btn_label = (m_view_mode == CaddyViewMode::SKEUOMORPHIC)
                                     ? "View: Skeuomorphic Shelf [V / Y]"
                                     : "View: Compact List [V / Y]";
    const float right_controls_w = 430.0f;
    if (ImGui::GetContentRegionAvail().x > right_controls_w) {
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - right_controls_w);
    }

    if (ImGui::Button(view_btn_label, ImVec2(220.0f, 0.0f))) {
        ToggleViewMode();
    }
    ImGui::SameLine();
    const char* pool_label = m_show_global_pool_drawer ? "[-] Hide Media Pool" : "[+] Global Media / USB";
    if (ImGui::Button(pool_label, ImVec2(195.0f, 0.0f))) {
        m_show_global_pool_drawer = !m_show_global_pool_drawer;
    }
}

void RuntimeOverlay::RenderActiveDriveSlots() {
    if (m_drive_statuses.empty()) {
        return;
    }

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const size_t count = m_drive_statuses.size();
    const float card_w = std::max(210.0f, (avail_w - static_cast<float>(count - 1) * 10.0f) / static_cast<float>(count));

    for (size_t i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine();
        const RuntimeDriveSlotStatus& st = m_drive_statuses[i];

        std::string child_id = "##DriveSlotCard_" + st.drive_letter;
        ImGui::BeginChild(child_id.c_str(), ImVec2(card_w, 102.0f), true);

        ImVec4 hdr_color = st.is_optical
                               ? ImVec4(0.45f, 0.85f, 0.95f, 1.0f)
                               : ImVec4(0.95f, 0.78f, 0.32f, 1.0f);
        ImGui::TextColored(hdr_color, "DRIVE %s: [%s]", st.drive_letter.c_str(), st.media_type_badge.c_str());

        if (st.is_mounted) {
            ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f), "%s", st.active_label.c_str());
        } else {
            ImGui::TextDisabled("%s", st.active_label.c_str());
        }

        if (st.is_dispatch_reserved) {
            ImGui::TextDisabled("Reserved for post-exit KIOSKEXT hook");
        } else if (!st.is_optical) {
            std::string eject_btn = "Eject Door##ej_" + st.drive_letter;
            if (ImGui::SmallButton(eject_btn.c_str())) {
                EjectDrive(st.drive_letter);
            }
            ImGui::SameLine();
            std::string wp_btn = std::string(st.is_write_protected ? "WP: LOCKED" : "WP: Writable") +
                                 "##wp_" + st.drive_letter;
            if (ImGui::SmallButton(wp_btn.c_str())) {
                ToggleDriveWriteProtect(st.drive_letter);
            }
        } else {
            std::string eject_btn = "Eject Tray##ej_" + st.drive_letter;
            if (ImGui::SmallButton(eject_btn.c_str())) {
                EjectDrive(st.drive_letter);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(card_w - 115.0f);
            float vol = m_cd_volume_percent;
            std::string slider_id = "##cdvol_" + st.drive_letter;
            if (ImGui::SliderFloat(slider_id.c_str(), &vol, 0.0f, 100.0f, "CD Vol %.0f%%")) {
                SetCdAudioVolumePercent(vol);
            }
        }

        ImGui::EndChild();
    }
}

void RuntimeOverlay::RenderSkeuomorphicShelf() {
    ImGui::BeginChild("##SkeuomorphicShelfRegion", ImVec2(0.0f, m_show_global_pool_drawer ? 215.0f : -76.0f), true);

    if (m_session_caddy.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No disks are pre-assigned to this session caddy.");
        ImGui::TextDisabled("Click '[+] Global Media / USB' above to insert any floppy or CD-ROM image.");
        ImGui::EndChild();
        return;
    }

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float card_w = 168.0f;
    const float card_h = 188.0f;
    const float spacing = 14.0f;
    float cursor_x = 0.0f;

    for (size_t i = 0; i < m_session_caddy.size(); ++i) {
        const RuntimeCaddyEntry& entry = m_session_caddy[i];

        if (i > 0 && (cursor_x + card_w) <= avail_w) {
            ImGui::SameLine(0.0f, spacing);
        } else {
            cursor_x = 0.0f;
        }
        cursor_x += card_w + spacing;

        bool is_active_in_drive = false;
        for (size_t d = 0; d < m_drive_statuses.size(); ++d) {
            if (m_drive_statuses[d].drive_letter == entry.binding.target_drive &&
                m_drive_statuses[d].is_mounted &&
                PathsMatchNormalized(m_drive_statuses[d].active_file_path, entry.media.file_path)) {
                is_active_in_drive = true;
                break;
            }
        }

        DiskCardRenderParams params;
        params.media = entry.media;
        params.target_drive = entry.binding.target_drive;
        params.slot_order = entry.binding.slot_order > 0 ? entry.binding.slot_order : static_cast<int>(i + 1);
        params.is_mounted_active = is_active_in_drive;
        params.is_selected_focused = (static_cast<int>(i) == m_selected_caddy_index);
        params.card_width = card_w;
        params.card_height = card_h;

        if (DiskCaddyView::GetInstance().RenderMediaCard(params)) {
            PerformHotSwapCaddySlot(i);
        }
        if (params.is_selected_focused) {
            ImGui::SetItemDefaultFocus();
        }
    }

    ImGui::EndChild();
}

void RuntimeOverlay::RenderCompactListView() {
    ImGui::BeginChild("##CompactListViewRegion", ImVec2(0.0f, m_show_global_pool_drawer ? 215.0f : -76.0f), true);

    if (m_session_caddy.empty()) {
        ImGui::TextDisabled("No media in session caddy. Open '[+] Global Media / USB' to mount a disk.");
        ImGui::EndChild();
        return;
    }

    ImGuiTableFlags table_flags = ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##CaddyCompactTable", 7, table_flags)) {
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Drive", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Media Type", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Label / Title", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Volume ID", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < m_session_caddy.size(); ++i) {
            const RuntimeCaddyEntry& entry = m_session_caddy[i];
            ImGui::TableNextRow();

            bool is_active_in_drive = false;
            for (size_t d = 0; d < m_drive_statuses.size(); ++d) {
                if (m_drive_statuses[d].drive_letter == entry.binding.target_drive &&
                    m_drive_statuses[d].is_mounted &&
                    PathsMatchNormalized(m_drive_statuses[d].active_file_path, entry.media.file_path)) {
                    is_active_in_drive = true;
                    break;
                }
            }

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("#%d", entry.binding.slot_order > 0 ? entry.binding.slot_order : static_cast<int>(i + 1));

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s:", entry.binding.target_drive.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(DiskCaddyView::HumanReadableMediaTypeBadge(entry.media.media_type).c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(entry.media.label_title.c_str());

            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(entry.media.volume_label.empty() ? "-" : entry.media.volume_label.c_str());

            ImGui::TableSetColumnIndex(5);
            if (is_active_in_drive) {
                ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.48f, 1.0f), "ACTIVE");
            } else {
                ImGui::TextDisabled("In Caddy");
            }

            ImGui::TableSetColumnIndex(6);
            std::string btn = (is_active_in_drive ? "Re-Insert" : "Insert Now") +
                              std::string("##swap_row_") + std::to_string(i);
            if (ImGui::SmallButton(btn.c_str())) {
                PerformHotSwapCaddySlot(i);
            }
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();
}

void RuntimeOverlay::RenderGlobalMediaPoolDrawer() {
    if (!m_show_global_pool_drawer) {
        return;
    }

    ImGui::BeginChild("##GlobalMediaPoolDrawer", ImVec2(0.0f, -76.0f), true);
    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.30f, 1.0f),
                       "Global Media Pool & Removable USB Images (%u available):",
                       static_cast<unsigned int>(m_global_pool_cache.size()));
    ImGui::Separator();

    if (m_global_pool_cache.empty()) {
        ImGui::TextDisabled("No media registered in Global Media Pool.");
    } else {
        for (size_t i = 0; i < m_global_pool_cache.size(); ++i) {
            const MediaPoolRecord& m = m_global_pool_cache[i];
            const bool is_cd = IsOpticalMediaType(m.media_type);

            if (m.is_removable_usb) {
                ImGui::TextColored(ImVec4(0.40f, 0.90f, 0.95f, 1.0f), "[USB]");
                ImGui::SameLine();
            }
            ImGui::Text("%s  (%s)  [%s]",
                        m.label_title.c_str(),
                        DiskCaddyView::HumanReadableMediaTypeBadge(m.media_type).c_str(),
                        m.volume_label.empty() ? BasenameFromPath(m.file_path).c_str() : m.volume_label.c_str());

            ImGui::SameLine();
            if (!is_cd) {
                std::string btn_a = "Insert A:##pool_a_" + std::to_string(i);
                if (ImGui::SmallButton(btn_a.c_str())) {
                    PerformHotSwap("A", m.file_path, m.is_write_protected);
                }
                ImGui::SameLine();
                std::string btn_b = "Insert B:##pool_b_" + std::to_string(i);
                if (ImGui::SmallButton(btn_b.c_str())) {
                    PerformHotSwap("B", m.file_path, m.is_write_protected);
                }
            } else {
                std::string btn_d = "Insert D: (CD)##pool_d_" + std::to_string(i);
                if (ImGui::SmallButton(btn_d.c_str())) {
                    PerformHotSwap("D", m.file_path, true);
                }
            }
        }
    }

    ImGui::EndChild();
}

void RuntimeOverlay::RenderFooterBar() {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.80f, 0.84f, 0.90f, 1.0f), "Status: %s", m_status_message.c_str());
    ImGui::Spacing();

    if (ImGui::Button("Resume Game [Esc / F12+D]", ImVec2(220.0f, 34.0f))) {
        KioskManager::GetInstance().ResumeEmulation();
    }
    ImGui::SameLine();
    if (ImGui::Button("Send Ctrl+Alt+Del Reboot", ImVec2(210.0f, 34.0f))) {
        SendCtrlAltDelToGuest();
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit to Kiosk Hub", ImVec2(190.0f, 34.0f))) {
        KioskManager::GetInstance().AbortSessionToShell();
    }
}

void RuntimeOverlay::Render() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport) {
        // Dimmed backdrop over paused guest OpenGL framebuffer
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        if (bg) {
            bg->AddRectFilled(
                viewport->Pos,
                ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
                IM_COL32(8, 10, 14, 185)
            );
        }

        const float win_w = std::max(720.0f, std::min(1160.0f, viewport->WorkSize.x * 0.92f));
        const float win_h = std::max(480.0f, std::min(760.0f, viewport->WorkSize.y * 0.90f));
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("DOSBox-X Kiosk - Runtime Media Caddy & Disk Swapper", nullptr, flags)) {
        RenderHeaderBar();
        ImGui::Separator();
        RenderActiveDriveSlots();
        ImGui::Spacing();

        if (m_view_mode == CaddyViewMode::SKEUOMORPHIC) {
            RenderSkeuomorphicShelf();
        } else {
            RenderCompactListView();
        }

        RenderGlobalMediaPoolDrawer();
        RenderFooterBar();
    }
    ImGui::End();
}

#endif // defined(C_KIOSK)
