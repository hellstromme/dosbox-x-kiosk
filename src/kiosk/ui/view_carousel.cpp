#include "config.h"

#if defined(C_KIOSK)

#include "view_carousel.h"
#include "kiosk_ui.h"
#include "view_disk_caddy.h"
#include "../kiosk_main.h"
#include "../kiosk_db.h"
#include "../machine/archetypes.h"
#include "../ingest/image_crawler.h"
#include "../ingest/archive_importer.h"
#include "../ingest/usb_bridge.h"
#include "logging.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

std::string ToLowerCopy(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

std::string ShortArchetypeTag(const std::string& archetype_id) {
    if (archetype_id == KioskArchetypes::XT_8088)      return "XT 8088";
    if (archetype_id == KioskArchetypes::AT_286)       return "AT 286";
    if (archetype_id == KioskArchetypes::DX_386)       return "386DX";
    if (archetype_id == KioskArchetypes::DX2_486)      return "486DX2";
    if (archetype_id == KioskArchetypes::PENTIUM_3DFX) return "PENTIUM 3DFX";
    return "DOS";
}

std::string GenerateSimpleMediaUuid(const std::string& seed) {
    const uint32_t h1 = DiskCaddyView::ComputeStringCrc32(seed);
    const uint32_t h2 = DiskCaddyView::ComputeStringCrc32(seed + "_media_pool");
    char buf[40];
    std::snprintf(buf, sizeof(buf), "med-%08x-%04x-4%03x-8%03x-%08x",
                  h1,
                  (h2 >> 16) & 0xFFFFu,
                  (h2 >> 4) & 0x0FFFu,
                  (h1 ^ h2) & 0x0FFFu,
                  h1 ^ (h2 << 1));
    return std::string(buf);
}

std::string FindMachineArchetype(const std::vector<MachineRecord>& machines, const std::string& machine_id) {
    for (size_t i = 0; i < machines.size(); ++i) {
        if (machines[i].id == machine_id) {
            return machines[i].archetype_id;
        }
    }
    return KioskArchetypes::DX_386;
}

ImU32 BoxArtAccentColor(uint32_t hash) {
    static const ImU32 kPalette[6] = {
        IM_COL32(198, 72, 48, 255),   // Classic Sierra / LucasArts terracotta
        IM_COL32(46, 114, 184, 255),  // Cobalt VGA blue
        IM_COL32(52, 148, 92, 255),   // Emerald DOS green
        IM_COL32(168, 118, 36, 255),  // Amber gold
        IM_COL32(134, 68, 168, 255),  // Synth purple
        IM_COL32(46, 142, 152, 255)   // Teal cyan
    };
    return kPalette[hash % 6u];
}

} // anonymous namespace

ViewCarousel& ViewCarousel::GetInstance() {
    static ViewCarousel instance;
    return instance;
}

ViewCarousel::~ViewCarousel() {
    WaitForPendingImport();
}

void ViewCarousel::WaitForPendingImport() {
    if (m_import_worker.joinable()) {
        m_import_worker.join();
    }
}

void ViewCarousel::PollImportCompletion() {
    if (m_import_in_progress.load()) return;

    bool finished = false;
    IngestResult done_res;
    {
        std::lock_guard<std::mutex> lock(m_import_mutex);
        if (m_import_result_ready) {
            finished = true;
            done_res = m_import_result;
            m_import_result_ready = false;
        }
    }
    if (!finished) return;

    if (m_import_worker.joinable()) {
        m_import_worker.join();
    }
    KioskUI& ui = KioskUI::GetInstance();
    if (done_res.success) {
        ui.InvalidateCatalogCache();
        ui.ShowToast("Imported '" + done_res.title + "' (" + done_res.archetype_id + ")");
        m_import_path_buf[0] = '\0';
        m_import_status_msg.clear();
        m_show_import_modal = false;
    } else {
        m_import_status_msg = "Import failed: " + done_res.error_message;
        ui.ShowToast("Import failed: " + done_res.error_message, 4.0f);
    }
}

void ViewCarousel::SetSearchFilter(const std::string& filter) {
    m_search_filter = filter;
    std::snprintf(m_search_buf, sizeof(m_search_buf), "%s", filter.c_str());
    m_selected_game_idx = 0;
    m_selected_app_idx = 0;
}

void ViewCarousel::ResetTransientModals() {
    m_show_context_modal = false;
    m_show_caddy_modal = false;
    m_show_delete_modal = false;
    m_show_import_modal = false;
    m_show_file_picker = false;
    m_show_edit_media_modal = false;
    m_show_rename_group_modal = false;
}

std::vector<CatalogItemRecord> ViewCarousel::FilterAndSortCatalogItems(
    const std::vector<CatalogItemRecord>& all_items,
    const std::string& item_type,
    const std::string& search_filter
) {
    const std::string q = ToLowerCopy(search_filter);
    std::vector<CatalogItemRecord> filtered;
    for (size_t i = 0; i < all_items.size(); ++i) {
        const CatalogItemRecord& item = all_items[i];
        if (!item_type.empty() && item.item_type != item_type) {
            continue;
        }
        if (!q.empty()) {
            const std::string title_low = ToLowerCopy(item.title);
            const std::string dev_low = ToLowerCopy(item.developer);
            if (title_low.find(q) == std::string::npos &&
                dev_low.find(q) == std::string::npos) {
                continue;
            }
        }
        filtered.push_back(item);
    }

    std::sort(filtered.begin(), filtered.end(),
              [](const CatalogItemRecord& a, const CatalogItemRecord& b) {
                  if (a.is_favorite != b.is_favorite) {
                      return a.is_favorite && !b.is_favorite;
                  }
                  if (a.last_launched_at != b.last_launched_at) {
                      return a.last_launched_at > b.last_launched_at;
                  }
                  return ToLowerCopy(a.title) < ToLowerCopy(b.title);
              });
    return filtered;
}

bool ViewCarousel::IsMediaCompatibleWithDrive(const std::string& media_type, const std::string& target_drive) {
    if (target_drive == "A" || target_drive == "B") {
        return media_type.find("FLOPPY_") == 0;
    }
    if (target_drive == "D" || target_drive == "E") {
        return (media_type == "CDROM_ISO" || media_type == "CDROM_CUE");
    }
    return false;
}

int ViewCarousel::ComputeNextSlotOrder(const std::vector<CaddyBindingRecord>& bindings, const std::string& target_drive) {
    int max_slot = 0;
    for (size_t i = 0; i < bindings.size(); ++i) {
        if (bindings[i].target_drive == target_drive && bindings[i].slot_order > max_slot) {
            max_slot = bindings[i].slot_order;
        }
    }
    return max_slot + 1;
}

bool ViewCarousel::HandleAction(KioskHubTab active_tab, KioskInputAction action) {
    if (m_show_file_picker) {
        if (action == KioskInputAction::ACTION_BACK) {
            m_show_file_picker = false;
            return true;
        }
        return false;
    }
    if (m_show_context_modal || m_show_caddy_modal || m_show_delete_modal || m_show_import_modal || m_show_edit_media_modal || m_show_rename_group_modal) {
        if (action == KioskInputAction::ACTION_BACK) {
            m_show_context_modal = false;
            m_show_caddy_modal = false;
            m_show_delete_modal = false;
            m_show_import_modal = false;
            m_show_edit_media_modal = false;
            m_show_rename_group_modal = false;
            return true;
        }
        return false;
    }

    KioskUI& ui = KioskUI::GetInstance();
    if (active_tab == KioskHubTab::GAMES || active_tab == KioskHubTab::APPLICATIONS) {
        const std::string type = (active_tab == KioskHubTab::GAMES) ? "GAME" : "APPLICATION";
        std::vector<CatalogItemRecord> items = FilterAndSortCatalogItems(ui.GetCachedItems(), type, m_search_filter);
        int& sel = (active_tab == KioskHubTab::GAMES) ? m_selected_game_idx : m_selected_app_idx;
        if (items.empty()) {
            if (action == KioskInputAction::ACTION_CONFIRM) {
                KioskManager::GetInstance().RequestDefaultDosboxLaunch();
                return true;
            }
            return false;
        }
        const int count = static_cast<int>(items.size());
        if (sel >= count) sel = count - 1;
        if (sel < 0) sel = 0;

        if (action == KioskInputAction::NAV_LEFT) {
            sel = (sel - 1 + count) % count;
            return true;
        }
        if (action == KioskInputAction::NAV_RIGHT) {
            sel = (sel + 1) % count;
            return true;
        }
        if (action == KioskInputAction::ACTION_CONFIRM) {
            KioskManager::GetInstance().RequestLaunchItem(items[static_cast<size_t>(sel)].id);
            return true;
        }
        if (action == KioskInputAction::ACTION_FAVORITE) {
            CatalogItemRecord updated = items[static_cast<size_t>(sel)];
            updated.is_favorite = !updated.is_favorite;
            if (KioskDatabase::GetInstance().UpdateCatalogItem(updated)) {
                ui.InvalidateCatalogCache();
                ui.ShowToast(updated.is_favorite ? ("Added '" + updated.title + "' to Favorites")
                                                 : ("Removed '" + updated.title + "' from Favorites"));
            }
            return true;
        }
        if (action == KioskInputAction::ACTION_CADDY_QUEUE) {
            const CatalogItemRecord& focused = items[static_cast<size_t>(sel)];
            m_show_caddy_modal = true;
            m_caddy_modal_is_item = true;
            m_caddy_modal_target_id = focused.id;
            m_caddy_modal_target_name = focused.title;
            m_caddy_modal_bindings = KioskDatabase::GetInstance().GetCaddyBindingsForItem(focused.id);
            return true;
        }
        if (action == KioskInputAction::ACTION_CONTEXT_MENU) {
            const CatalogItemRecord& focused = items[static_cast<size_t>(sel)];
            m_show_context_modal = true;
            m_context_modal_is_item = true;
            m_context_modal_target_id = focused.id;
            m_context_modal_target_name = focused.title;
            return true;
        }
    } else if (active_tab == KioskHubTab::MACHINES) {
        const std::vector<MachineRecord>& machines = ui.GetCachedMachines();
        // Virtual slot 0 = Quick Start Default DOSBox-X; 1..N = custom machines
        const int total = static_cast<int>(machines.size()) + 1;
        if (m_selected_machine_idx >= total) m_selected_machine_idx = total - 1;
        if (m_selected_machine_idx < 0) m_selected_machine_idx = 0;

        if (action == KioskInputAction::NAV_LEFT) {
            m_selected_machine_idx = (m_selected_machine_idx - 1 + total) % total;
            return true;
        }
        if (action == KioskInputAction::NAV_RIGHT) {
            m_selected_machine_idx = (m_selected_machine_idx + 1) % total;
            return true;
        }
        if (action == KioskInputAction::ACTION_CONFIRM) {
            if (m_selected_machine_idx == 0) {
                KioskManager::GetInstance().RequestDefaultDosboxLaunch();
            } else {
                const MachineRecord& mach = machines[static_cast<size_t>(m_selected_machine_idx - 1)];
                KioskManager::GetInstance().RequestLaunchMachine(mach.id);
            }
            return true;
        }
        if (action == KioskInputAction::ACTION_CADDY_QUEUE && m_selected_machine_idx > 0) {
            const MachineRecord& mach = machines[static_cast<size_t>(m_selected_machine_idx - 1)];
            m_show_caddy_modal = true;
            m_caddy_modal_is_item = false;
            m_caddy_modal_target_id = mach.id;
            m_caddy_modal_target_name = mach.display_name;
            m_caddy_modal_bindings = KioskDatabase::GetInstance().GetCaddyBindingsForMachine(mach.id);
            return true;
        }
        if (action == KioskInputAction::ACTION_CONTEXT_MENU && m_selected_machine_idx > 0) {
            const MachineRecord& mach = machines[static_cast<size_t>(m_selected_machine_idx - 1)];
            m_show_context_modal = true;
            m_context_modal_is_item = false;
            m_context_modal_target_id = mach.id;
            m_context_modal_target_name = mach.display_name;
            return true;
        }
    } else if (active_tab == KioskHubTab::MEDIA_POOL) {
        if (action == KioskInputAction::ACTION_VIEW_TOGGLE) {
            m_media_view_mode = (m_media_view_mode == CaddyViewMode::SKEUOMORPHIC)
                                    ? CaddyViewMode::LIST
                                    : CaddyViewMode::SKEUOMORPHIC;
            return true;
        }
    }

    return false;
}

void ViewCarousel::DrawProceduralBoxArtCard(
    ImDrawList* draw_list,
    float x, float y, float w, float h,
    const CatalogItemRecord& item,
    const std::string& archetype_id,
    bool is_focused
) {
    const KioskThemePalette& pal = KioskUI::GetInstance().GetThemePalette();
    const uint32_t hash = DiskCaddyView::ComputeStringCrc32(item.id.empty() ? item.title : item.id);
    const ImU32 cover_accent = BoxArtAccentColor(hash);
    const ImU32 border_col = is_focused
                                 ? ImGui::ColorConvertFloat4ToU32(pal.accent)
                                 : IM_COL32(78, 82, 92, 220);

    // Drop shadow
    draw_list->AddRectFilled(
        ImVec2(x + 5.0f, y + 6.0f),
        ImVec2(x + w + 5.0f, y + h + 6.0f),
        IM_COL32(0, 0, 0, is_focused ? 150 : 90),
        8.0f
    );

    // Outer retro big-box body
    draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(28, 30, 36, 255), 8.0f);

    // Top colored cover header band
    const float banner_h = h * 0.42f;
    draw_list->AddRectFilled(
        ImVec2(x + 2.0f, y + 2.0f),
        ImVec2(x + w - 2.0f, y + banner_h),
        cover_accent,
        6.0f,
        ImDrawFlags_RoundCornersTop
    );

    // Subtle geometric scan stripe on cover art
    for (float sy = y + 12.0f; sy < y + banner_h - 6.0f; sy += 8.0f) {
        draw_list->AddLine(
            ImVec2(x + 6.0f, sy),
            ImVec2(x + w - 6.0f, sy),
            IM_COL32(255, 255, 255, 22),
            1.0f
        );
    }

    // Archetype badge pill in top-left
    const std::string tag = ShortArchetypeTag(archetype_id);
    const ImVec2 tag_sz = ImGui::CalcTextSize(tag.c_str());
    draw_list->AddRectFilled(
        ImVec2(x + 10.0f, y + 10.0f),
        ImVec2(x + 10.0f + tag_sz.x + 12.0f, y + 10.0f + tag_sz.y + 6.0f),
        IM_COL32(12, 14, 18, 210),
        4.0f
    );
    draw_list->AddText(ImVec2(x + 16.0f, y + 13.0f), IM_COL32(240, 240, 240, 255), tag.c_str());

    // Favorite badge in top-right
    if (item.is_favorite) {
        const char* fav = "* FAV";
        const ImVec2 fsz = ImGui::CalcTextSize(fav);
        draw_list->AddRectFilled(
            ImVec2(x + w - fsz.x - 18.0f, y + 10.0f),
            ImVec2(x + w - 8.0f, y + 10.0f + fsz.y + 6.0f),
            IM_COL32(215, 162, 24, 235),
            4.0f
        );
        draw_list->AddText(ImVec2(x + w - fsz.x - 13.0f, y + 13.0f), IM_COL32(18, 16, 12, 255), fav);
    }

    // Title & metadata inside lower half (clipped to card width)
    draw_list->PushClipRect(ImVec2(x + 10.0f, y + banner_h + 6.0f), ImVec2(x + w - 10.0f, y + h - 8.0f), true);
    draw_list->AddText(ImVec2(x + 12.0f, y + banner_h + 10.0f), IM_COL32(245, 245, 245, 255), item.title.c_str());

    std::string meta = !item.developer.empty() ? item.developer : item.guest_executable;
    if (item.release_year > 0) {
        char ybuf[16];
        std::snprintf(ybuf, sizeof(ybuf), " (%d)", item.release_year);
        meta += ybuf;
    }
    draw_list->AddText(ImVec2(x + 12.0f, y + banner_h + 34.0f), IM_COL32(170, 176, 186, 255), meta.c_str());

    char plays_buf[48];
    std::snprintf(plays_buf, sizeof(plays_buf), "Plays: %d", item.play_count);
    draw_list->AddText(ImVec2(x + 12.0f, y + h - 26.0f), IM_COL32(140, 148, 160, 255), plays_buf);
    draw_list->PopClipRect();

    // Border highlight
    draw_list->AddRect(
        ImVec2(x, y),
        ImVec2(x + w, y + h),
        border_col,
        8.0f,
        0,
        is_focused ? 3.0f : 1.5f
    );
}

void ViewCarousel::DrawProceduralMachineCase(
    ImDrawList* draw_list,
    float x, float y, float w, float h,
    const MachineRecord& machine,
    bool is_focused
) {
    const KioskThemePalette& pal = KioskUI::GetInstance().GetThemePalette();
    const ImU32 border_col = is_focused
                                 ? ImGui::ColorConvertFloat4ToU32(pal.accent)
                                 : IM_COL32(78, 82, 92, 220);

    // Card background
    draw_list->AddRectFilled(ImVec2(x + 4.0f, y + 5.0f), ImVec2(x + w + 4.0f, y + h + 5.0f), IM_COL32(0, 0, 0, 120), 8.0f);
    draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(28, 30, 36, 255), 8.0f);

    // Case illustration zone
    const float cx = x + w * 0.5f;
    const float art_top = y + 16.0f;
    const ImU32 beige_case = IM_COL32(214, 206, 186, 255);
    const ImU32 beige_dark = IM_COL32(174, 166, 148, 255);
    const ImU32 drive_slot = IM_COL32(42, 44, 48, 255);

    const bool is_tower = (machine.archetype_id == KioskArchetypes::DX2_486 ||
                           machine.archetype_id == KioskArchetypes::PENTIUM_3DFX);

    if (!is_tower) {
        // Classic horizontal desktop chassis (XT / AT / 386DX)
        const float bw = w - 42.0f;
        const float bh = 68.0f;
        const float bx = cx - bw * 0.5f;
        const float by = art_top + 22.0f;
        draw_list->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), beige_case, 4.0f);
        draw_list->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), beige_dark, 4.0f, 0, 2.0f);

        // Drive bays on right
        draw_list->AddRectFilled(ImVec2(bx + bw - 68.0f, by + 12.0f), ImVec2(bx + bw - 12.0f, by + 26.0f), drive_slot, 2.0f);
        draw_list->AddRectFilled(ImVec2(bx + bw - 68.0f, by + 34.0f), ImVec2(bx + bw - 12.0f, by + 48.0f), drive_slot, 2.0f);

        if (machine.archetype_id == KioskArchetypes::XT_8088) {
            // Iconic red IBM XT paddle switch
            draw_list->AddRectFilled(ImVec2(bx + 12.0f, by + 14.0f), ImVec2(bx + 26.0f, by + 32.0f), IM_COL32(204, 48, 42, 255), 2.0f);
        } else if (machine.archetype_id == KioskArchetypes::AT_286) {
            // Keylock + green power LED
            draw_list->AddCircleFilled(ImVec2(bx + 20.0f, by + 24.0f), 6.0f, IM_COL32(110, 110, 115, 255));
            draw_list->AddCircleFilled(ImVec2(bx + 36.0f, by + 24.0f), 3.5f, IM_COL32(64, 220, 96, 255));
        } else {
            // 386DX green "33" MHz 7-segment display
            draw_list->AddRectFilled(ImVec2(bx + 12.0f, by + 16.0f), ImVec2(bx + 44.0f, by + 36.0f), IM_COL32(16, 24, 18, 255), 2.0f);
            draw_list->AddText(ImVec2(bx + 19.0f, by + 19.0f), IM_COL32(60, 245, 95, 255), "33");
        }
    } else {
        // Mid/ATX Tower chassis (486DX2 / Pentium 3dfx)
        const float tw = 74.0f;
        const float th = 104.0f;
        const float tx = cx - tw * 0.5f;
        const float ty = art_top + 2.0f;
        const ImU32 tower_col = (machine.archetype_id == KioskArchetypes::PENTIUM_3DFX)
                                    ? IM_COL32(224, 222, 214, 255)
                                    : beige_case;
        draw_list->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), tower_col, 6.0f);
        draw_list->AddRect(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), beige_dark, 6.0f, 0, 2.0f);

        // 5.25" optical bay + 3.5" floppy bay
        draw_list->AddRectFilled(ImVec2(tx + 10.0f, ty + 12.0f), ImVec2(tx + tw - 10.0f, ty + 24.0f), drive_slot, 2.0f);
        draw_list->AddRectFilled(ImVec2(tx + 16.0f, ty + 30.0f), ImVec2(tx + tw - 16.0f, ty + 39.0f), drive_slot, 2.0f);

        if (machine.archetype_id == KioskArchetypes::DX2_486) {
            draw_list->AddRectFilled(ImVec2(tx + 18.0f, ty + 48.0f), ImVec2(tx + 56.0f, ty + 66.0f), IM_COL32(18, 24, 18, 255), 2.0f);
            draw_list->AddText(ImVec2(tx + 28.0f, ty + 50.0f), IM_COL32(255, 176, 40, 255), "66");
        } else {
            // 3dfx Voodoo badge
            draw_list->AddRectFilled(ImVec2(tx + 14.0f, ty + 50.0f), ImVec2(tx + tw - 14.0f, ty + 68.0f), IM_COL32(36, 78, 164, 255), 3.0f);
            draw_list->AddText(ImVec2(tx + 22.0f, ty + 52.0f), IM_COL32(255, 220, 80, 255), "3dfx");
        }

        // Power button + LED
        draw_list->AddCircleFilled(ImVec2(tx + 22.0f, ty + th - 16.0f), 5.0f, IM_COL32(120, 124, 130, 255));
        draw_list->AddCircleFilled(ImVec2(tx + 38.0f, ty + th - 16.0f), 3.0f, IM_COL32(64, 220, 96, 255));
    }

    // Machine text block
    const float text_y = y + 128.0f;
    draw_list->PushClipRect(ImVec2(x + 10.0f, text_y), ImVec2(x + w - 10.0f, y + h - 8.0f), true);
    draw_list->AddText(ImVec2(x + 12.0f, text_y), IM_COL32(245, 245, 245, 255), machine.display_name.c_str());

    const std::string arch_tag = ShortArchetypeTag(machine.archetype_id);
    draw_list->AddText(ImVec2(x + 12.0f, text_y + 22.0f), ImGui::ColorConvertFloat4ToU32(pal.accent), arch_tag.c_str());

    const char* mode_str = (machine.boot_mode == "REAL_HDD_BOOT") ? "Real HDD Boot" : "Integrated DOS";
    draw_list->AddText(ImVec2(x + 12.0f, text_y + 42.0f), IM_COL32(165, 172, 182, 255), mode_str);
    draw_list->PopClipRect();

    draw_list->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), border_col, 8.0f, 0, is_focused ? 3.0f : 1.5f);
}

void ViewCarousel::RenderCatalogCarousel(const std::string& item_type, int& selected_idx, float& scroll_offset) {
    KioskUI& ui = KioskUI::GetInstance();
    const KioskThemePalette& pal = ui.GetThemePalette();
    const std::vector<MachineRecord>& machines = ui.GetCachedMachines();
    std::vector<CatalogItemRecord> items = FilterAndSortCatalogItems(ui.GetCachedItems(), item_type, m_search_filter);

    // Search & filter bar
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Filter:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::InputTextWithHint("##CarouselSearch", "Search titles or developers...", m_search_buf, sizeof(m_search_buf))) {
        m_search_filter = m_search_buf;
        selected_idx = 0;
    }
    if (!m_search_filter.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Clear##ClearSearch")) {
            SetSearchFilter("");
        }
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
    ImGui::Text("(%u %s)",
                static_cast<unsigned int>(items.size()),
                (item_type == "GAME") ? "games" : "applications");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Import GOG / Steam")) {
        m_show_import_modal = true;
        m_import_item_type = item_type.empty() ? "GAME" : item_type;
        m_import_status_msg.clear();
    }

    ImGui::Spacing();

    if (items.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.card_bg);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        if (ImGui::BeginChild("##CarouselEmptyState", ImVec2(-1.0f, 260.0f), true)) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
            ImGui::SetWindowFontScale(1.25f);
            ImGui::TextUnformatted(item_type == "GAME"
                                       ? "No Games in Your Kiosk Library Yet"
                                       : "No Applications in Your Kiosk Library Yet");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Create a retro machine with the New Machine Wizard, mount disk images from the Media Pool, "
                "or boot Default DOSBox-X right away. Once software is scanned or imported, it will appear here "
                "in the Big-Picture cover-flow carousel."
            );
            ImGui::Spacing();
            ImGui::Spacing();
            if (ImGui::Button("Boot Default DOSBox-X", ImVec2(210.0f, 36.0f))) {
                KioskManager::GetInstance().RequestDefaultDosboxLaunch();
            }
            ImGui::SameLine();
            if (ImGui::Button("Open New Machine Wizard", ImVec2(210.0f, 36.0f))) {
                ui.SetActiveTab(KioskHubTab::NEW_MACHINE);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }

    const int count = static_cast<int>(items.size());
    if (selected_idx >= count) selected_idx = count - 1;
    if (selected_idx < 0) selected_idx = 0;

    // Smoothly interpolate horizontal cover-flow scroll toward selected card
    const float card_w = 200.0f;
    const float card_h = 232.0f;
    const float spacing = 22.0f;
    const float step_w = card_w + spacing;
    const float target_scroll = static_cast<float>(selected_idx) * step_w;
    scroll_offset += (target_scroll - scroll_offset) * 0.22f;
    if (std::fabs(target_scroll - scroll_offset) < 0.5f) {
        scroll_offset = target_scroll;
    }

    // Cover-flow strip area
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float strip_h = 266.0f;
    ImGui::BeginChild("##CoverFlowStrip", ImVec2(avail.x, strip_h), false, ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 win_pos = ImGui::GetWindowPos();
        const float center_x = win_pos.x + avail.x * 0.5f;
        const float base_y = win_pos.y + 20.0f;

        for (int i = 0; i < count; ++i) {
            const bool is_focused = (i == selected_idx);
            const float rel_x = static_cast<float>(i) * step_w - scroll_offset;
            const float draw_x = center_x + rel_x - card_w * 0.5f;
            const float draw_y = is_focused ? (base_y - 10.0f) : (base_y + 6.0f);

            if (draw_x + card_w < win_pos.x - 40.0f || draw_x > win_pos.x + avail.x + 40.0f) {
                continue;
            }

            const CatalogItemRecord& item = items[static_cast<size_t>(i)];
            const std::string arch_id = FindMachineArchetype(machines, item.machine_id);
            DrawProceduralBoxArtCard(draw_list, draw_x, draw_y, card_w, card_h, item, arch_id, is_focused);

            // Invisible hit button for mouse selection / double-click launch
            ImGui::SetCursorScreenPos(ImVec2(draw_x, draw_y));
            char btn_id[64];
            std::snprintf(btn_id, sizeof(btn_id), "##card_%s_%d", item_type.c_str(), i);
            if (ImGui::InvisibleButton(btn_id, ImVec2(card_w, card_h))) {
                if (selected_idx == i) {
                    KioskManager::GetInstance().RequestLaunchItem(item.id);
                } else {
                    selected_idx = i;
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // Focused Item Detail & Action Panel
    const CatalogItemRecord& focused = items[static_cast<size_t>(selected_idx)];
    const std::string focused_arch = FindMachineArchetype(machines, focused.machine_id);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.card_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    if (ImGui::BeginChild("##FocusedItemDetail", ImVec2(-1.0f, 148.0f), true)) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
        ImGui::SetWindowFontScale(1.25f);
        ImGui::TextUnformatted(focused.title.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
        ImGui::Text("  [%s]  %s",
                    ShortArchetypeTag(focused_arch).c_str(),
                    focused.launch_mechanism.c_str());
        ImGui::PopStyleColor();

        ImGui::Text("Executable: %s%s%s   |   Plays: %d",
                    focused.guest_working_dir.c_str(),
                    focused.guest_working_dir.empty() ? "" : "\\",
                    focused.guest_executable.c_str(),
                    focused.play_count);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float detail_right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        auto wrap_if_needed = [&](float next_w) {
            if (ImGui::GetItemRectMax().x + 8.0f + next_w <= detail_right) {
                ImGui::SameLine(0.0f, 8.0f);
            }
        };

        if (ImGui::Button("Launch Now [Enter/A]", ImVec2(175.0f, 34.0f))) {
            KioskManager::GetInstance().RequestLaunchItem(focused.id);
        }
        wrap_if_needed(130.0f);
        if (ImGui::Button(focused.is_favorite ? "Unfavorite [F]" : "Favorite [F]", ImVec2(130.0f, 34.0f))) {
            CatalogItemRecord updated = focused;
            updated.is_favorite = !updated.is_favorite;
            if (KioskDatabase::GetInstance().UpdateCatalogItem(updated)) {
                ui.InvalidateCatalogCache();
                ui.ShowToast(updated.is_favorite ? "Added to Favorites" : "Removed from Favorites");
            }
        }
        wrap_if_needed(145.0f);
        if (ImGui::Button("Session Caddy [C]", ImVec2(145.0f, 34.0f))) {
            m_show_caddy_modal = true;
            m_caddy_modal_is_item = true;
            m_caddy_modal_target_id = focused.id;
            m_caddy_modal_target_name = focused.title;
            m_caddy_modal_bindings = KioskDatabase::GetInstance().GetCaddyBindingsForItem(focused.id);
        }
        wrap_if_needed(155.0f);
        if (ImGui::Button("Set as Default Boot", ImVec2(155.0f, 34.0f))) {
            KioskConfigRecord cfg = ui.GetCachedConfig();
            cfg.default_boot_action = "SPECIFIC_ITEM";
            cfg.default_target_uuid = focused.id;
            if (KioskDatabase::GetInstance().UpdateConfig(cfg)) {
                KioskManager::GetInstance().ReloadConfig();
                ui.InvalidateCatalogCache();
                ui.ShowToast("Set '" + focused.title + "' as default auto-boot target");
            }
        }
        wrap_if_needed(90.0f);
        if (ImGui::Button("Remove", ImVec2(90.0f, 34.0f))) {
            m_show_delete_modal = true;
            m_delete_modal_is_item = true;
            m_delete_modal_target_id = focused.id;
            m_delete_modal_target_name = focused.title;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    RenderContextActionsModal();
    RenderCaddyManagerModal();
    RenderDeleteConfirmModal();
}

void ViewCarousel::RenderGamesTab() {
    PollImportCompletion();
    RenderCatalogCarousel("GAME", m_selected_game_idx, m_game_scroll_offset);
    RenderImportArchiveModal();
}

void ViewCarousel::RenderApplicationsTab() {
    PollImportCompletion();
    RenderCatalogCarousel("APPLICATION", m_selected_app_idx, m_app_scroll_offset);
    RenderImportArchiveModal();
}

void ViewCarousel::RenderMachinesTab() {
    PollImportCompletion();
    KioskUI& ui = KioskUI::GetInstance();
    const KioskThemePalette& pal = ui.GetThemePalette();
    const std::vector<MachineRecord>& machines = ui.GetCachedMachines();

    const int total = static_cast<int>(machines.size()) + 1; // index 0 = Quick Start Default DOSBox-X
    if (m_selected_machine_idx >= total) m_selected_machine_idx = total - 1;
    if (m_selected_machine_idx < 0) m_selected_machine_idx = 0;

    ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
    ImGui::Text("Select a retro computer to boot cleanly to its DOS prompt or Windows desktop (%u custom machines).",
                static_cast<unsigned int>(machines.size()));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("+ New Machine Wizard")) {
        ui.SetActiveTab(KioskHubTab::NEW_MACHINE);
    }

    ImGui::Spacing();

    const float card_w = 210.0f;
    const float card_h = 204.0f;
    const float spacing = 22.0f;
    const float step_w = card_w + spacing;
    const float target_scroll = static_cast<float>(m_selected_machine_idx) * step_w;
    m_machine_scroll_offset += (target_scroll - m_machine_scroll_offset) * 0.22f;
    if (std::fabs(target_scroll - m_machine_scroll_offset) < 0.5f) {
        m_machine_scroll_offset = target_scroll;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##MachinesStrip", ImVec2(avail.x, 240.0f), false, ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 win_pos = ImGui::GetWindowPos();
        const float center_x = win_pos.x + avail.x * 0.5f;
        const float base_y = win_pos.y + 18.0f;

        MachineRecord default_mach;
        default_mach.id = "default";
        default_mach.display_name = "Default DOSBox-X";
        default_mach.archetype_id = KioskArchetypes::DX_386;
        default_mach.boot_mode = "INTEGRATED_DOS";

        for (int i = 0; i < total; ++i) {
            const bool is_focused = (i == m_selected_machine_idx);
            const float rel_x = static_cast<float>(i) * step_w - m_machine_scroll_offset;
            const float draw_x = center_x + rel_x - card_w * 0.5f;
            const float draw_y = is_focused ? (base_y - 8.0f) : (base_y + 6.0f);

            if (draw_x + card_w < win_pos.x - 40.0f || draw_x > win_pos.x + avail.x + 40.0f) {
                continue;
            }

            const MachineRecord& mach = (i == 0) ? default_mach : machines[static_cast<size_t>(i - 1)];
            DrawProceduralMachineCase(draw_list, draw_x, draw_y, card_w, card_h, mach, is_focused);

            ImGui::SetCursorScreenPos(ImVec2(draw_x, draw_y));
            char btn_id[48];
            std::snprintf(btn_id, sizeof(btn_id), "##mach_card_%d", i);
            if (ImGui::InvisibleButton(btn_id, ImVec2(card_w, card_h))) {
                if (m_selected_machine_idx == i) {
                    if (i == 0) {
                        KioskManager::GetInstance().RequestDefaultDosboxLaunch();
                    } else {
                        KioskManager::GetInstance().RequestLaunchMachine(mach.id);
                    }
                } else {
                    m_selected_machine_idx = i;
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // Detail panel for selected machine
    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.card_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    if (ImGui::BeginChild("##MachineDetailPanel", ImVec2(-1.0f, 152.0f), true)) {
        if (m_selected_machine_idx == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
            ImGui::SetWindowFontScale(1.22f);
            ImGui::Text("Quick Start: Default DOSBox-X Shell");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
            ImGui::TextWrapped("Boots standard DOSBox-X integrated DOS immediately with zero setup required.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button("Boot Default DOSBox-X [Enter/A]", ImVec2(230.0f, 34.0f))) {
                KioskManager::GetInstance().RequestDefaultDosboxLaunch();
            }
            ImGui::SameLine();
            if (ImGui::Button("+ Create Custom Machine", ImVec2(200.0f, 34.0f))) {
                ui.SetActiveTab(KioskHubTab::NEW_MACHINE);
            }
        } else {
            const MachineRecord& mach = machines[static_cast<size_t>(m_selected_machine_idx - 1)];
            const ArchetypeProfile* prof = ArchetypeManager::GetInstance().GetArchetype(mach.archetype_id);

            ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
            ImGui::SetWindowFontScale(1.22f);
            ImGui::TextUnformatted(mach.display_name.c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();

            if (prof) {
                ImGui::Text("Hardware: %s   |   RAM: %d MB   |   OS: %s",
                            prof->display_name.c_str(),
                            prof->mem_size_mb,
                            mach.os_id.empty() ? "Integrated DOS" : mach.os_id.c_str());
            }
            if (!mach.primary_hdd_path.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
                ImGui::Text("Primary Storage: %s %s",
                            mach.primary_hdd_path.c_str(),
                            mach.primary_hdd_chs.empty() ? "" : ("(CHS " + mach.primary_hdd_chs + ")").c_str());
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const float mach_right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            auto wrap_mach = [&](float next_w) {
                if (ImGui::GetItemRectMax().x + 8.0f + next_w <= mach_right) {
                    ImGui::SameLine(0.0f, 8.0f);
                }
            };

            if (ImGui::Button("Boot Machine [Enter/A]", ImVec2(175.0f, 34.0f))) {
                KioskManager::GetInstance().RequestLaunchMachine(mach.id);
            }
            if (!mach.primary_hdd_path.empty()) {
                wrap_mach(135.0f);
                if (ImGui::Button("Scan Software", ImVec2(135.0f, 34.0f))) {
                    ImageCrawler::CrawlSummary summary;
                    if (ImageCrawler::ScanAndCatalogMachine(mach.id, "", summary)) {
                        char toast_buf[192];
                        if (summary.newly_inserted > 0) {
                            std::snprintf(toast_buf, sizeof(toast_buf),
                                          "Scanned '%s': +%d new title%s (%d icon%s)",
                                          mach.display_name.c_str(),
                                          summary.newly_inserted,
                                          summary.newly_inserted == 1 ? "" : "s",
                                          summary.icons_extracted,
                                          summary.icons_extracted == 1 ? "" : "s");
                        } else if (summary.skipped_duplicates > 0) {
                            if (summary.failed_icon_updates > 0) {
                                std::snprintf(toast_buf, sizeof(toast_buf),
                                              "Scanned '%s': %d already cataloged (%d icon update%s failed)",
                                              mach.display_name.c_str(),
                                              summary.skipped_duplicates,
                                              summary.failed_icon_updates,
                                              summary.failed_icon_updates == 1 ? "" : "s");
                            } else {
                                std::snprintf(toast_buf, sizeof(toast_buf),
                                              "Scanned '%s': all %d title%s already cataloged",
                                              mach.display_name.c_str(),
                                              summary.skipped_duplicates,
                                              summary.skipped_duplicates == 1 ? "" : "s");
                            }
                        } else {
                            std::snprintf(toast_buf, sizeof(toast_buf),
                                          "Scanned '%s': no new software found",
                                          mach.display_name.c_str());
                        }
                        ui.ShowToast(toast_buf, 4.0f);
                    } else if (summary.newly_inserted > 0 && summary.failed_inserts > 0) {
                        char toast_buf[192];
                        std::snprintf(toast_buf, sizeof(toast_buf),
                                      "Scanned '%s': +%d added, %d failed to save",
                                      mach.display_name.c_str(),
                                      summary.newly_inserted,
                                      summary.failed_inserts);
                        ui.ShowToast(toast_buf, 4.0f);
                    } else {
                        ui.ShowToast("Scan failed: " + summary.error_message, 4.0f);
                    }
                }
            }
            wrap_mach(145.0f);
            if (ImGui::Button("Session Caddy [C]", ImVec2(145.0f, 34.0f))) {
                m_show_caddy_modal = true;
                m_caddy_modal_is_item = false;
                m_caddy_modal_target_id = mach.id;
                m_caddy_modal_target_name = mach.display_name;
                m_caddy_modal_bindings = KioskDatabase::GetInstance().GetCaddyBindingsForMachine(mach.id);
            }
            wrap_mach(155.0f);
            if (ImGui::Button("Set as Default Boot", ImVec2(155.0f, 34.0f))) {
                KioskConfigRecord cfg = ui.GetCachedConfig();
                cfg.default_boot_action = "SPECIFIC_MACHINE";
                cfg.default_target_uuid = mach.id;
                if (KioskDatabase::GetInstance().UpdateConfig(cfg)) {
                    KioskManager::GetInstance().ReloadConfig();
                    ui.InvalidateCatalogCache();
                    ui.ShowToast("Set '" + mach.display_name + "' as default auto-boot machine");
                }
            }
            wrap_mach(90.0f);
            if (ImGui::Button("Delete", ImVec2(90.0f, 34.0f))) {
                m_show_delete_modal = true;
                m_delete_modal_is_item = false;
                m_delete_modal_target_id = mach.id;
                m_delete_modal_target_name = mach.display_name;
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    RenderContextActionsModal();
    RenderCaddyManagerModal();
    RenderDeleteConfirmModal();
}

void ViewCarousel::RenderMediaPoolTab() {
    PollImportCompletion();
    KioskUI& ui = KioskUI::GetInstance();
    const KioskThemePalette& pal = ui.GetThemePalette();
    const std::vector<MediaPoolRecord>& media = ui.GetCachedMedia();

    // Add media image bar (adapts cleanly to 640x480)
    const float mp_right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Add Image:");
    ImGui::SameLine();
    const float input_w = (std::min)(340.0f, (std::max)(140.0f, ImGui::GetContentRegionAvail().x - 320.0f));
    ImGui::SetNextItemWidth(input_w);
    ImGui::InputTextWithHint("##AddMediaPath", "Path to .img, .ima, .iso, or .cue file...", m_add_media_path_buf, sizeof(m_add_media_path_buf));
    ImGui::SameLine();
    if (ImGui::Button("Browse & Add...##MediaPoolBrowse")) {
        OpenFilePicker(m_add_media_path_buf, FilePickerTarget::MEDIA_POOL);
    }
    if (ImGui::GetItemRectMax().x + 8.0f + 110.0f <= mp_right) {
        ImGui::SameLine(0.0f, 8.0f);
    }
    if (ImGui::Button("Inspect & Add")) {
        std::string path(m_add_media_path_buf);
        if (!path.empty()) {
            std::string label;
            MediaAddStatus st = AddMediaFileToPool(path, label);
            if (st == MediaAddStatus::ADDED) {
                size_t slash = path.find_last_of("\\/");
                if (slash != std::string::npos) {
                    std::string parent = (slash == 2 && path[1] == ':') ? path.substr(0, 3) : path.substr(0, slash);
                    if (!parent.empty()) m_last_media_picker_dir = parent;
                }
                m_add_media_path_buf[0] = '\0';
                ui.InvalidateCatalogCache();
                ui.ShowToast("Added '" + label + "' to Global Media Pool");
            } else if (st == MediaAddStatus::ALREADY_EXISTS) {
                ui.ShowToast("Media already exists in Global Media Pool");
            } else {
                ui.ShowToast("Could not inspect media file (missing or unsupported): " + path);
            }
        }
    }
    if (ImGui::GetItemRectMax().x + 8.0f + 130.0f <= mp_right) {
        ImGui::SameLine(0.0f, 8.0f);
    }
    if (ImGui::Button(m_media_view_mode == CaddyViewMode::SKEUOMORPHIC ? "View: Visual [V]" : "View: List [V]")) {
        m_media_view_mode = (m_media_view_mode == CaddyViewMode::SKEUOMORPHIC)
                                ? CaddyViewMode::LIST
                                : CaddyViewMode::SKEUOMORPHIC;
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Button("Import GOG / Steam##MediaPool")) {
        m_show_file_picker = false;
        m_show_import_modal = true;
        m_import_item_type = "GAME";
        m_import_status_msg.clear();
    }

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Filter Pool:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##MediaPoolFilter", "Filter by set/group, label, description...", m_media_search_buf, sizeof(m_media_search_buf));
    if (m_media_search_buf[0] != '\0') {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##MediaFilterClear")) {
            m_media_search_buf[0] = '\0';
        }
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
    ImGui::TextUnformatted("Tip: Click any disk card to edit its Set/Group, Label, or Description.");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Removable USB / Optical Drives strip (zero per-frame SQLite; cached by KioskUsbBridge)
    const std::vector<UsbVolumeInfo> usb_volumes = KioskUsbBridge::GetInstance().GetActiveVolumes();
    const std::vector<UsbArchiveCandidate> usb_archives = KioskUsbBridge::GetInstance().GetDiscoveredArchives();
    if (!usb_volumes.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.14f, 0.18f, 0.90f));
        const float strip_h = (std::min)(160.0f, 34.0f * static_cast<float>(usb_volumes.size()) + 38.0f);
        ImGui::BeginChild("##UsbVolumesStrip", ImVec2(0.0f, strip_h), true);
        ImGui::TextColored(ImVec4(0.40f, 0.90f, 0.95f, 1.0f), "Removable USB / Optical Drives (%u connected)",
                           static_cast<unsigned int>(usb_volumes.size()));
        ImGui::SameLine();
        if (ImGui::SmallButton("Rescan Drives##UsbRescan")) {
            KioskUsbBridge::GetInstance().RescanNow();
            ui.InvalidateCatalogCache();
        }
        if (!usb_archives.empty()) {
            ImGui::SameLine();
            char imp_lbl[96];
            snprintf(imp_lbl, sizeof(imp_lbl), "Import USB Archive (%u)##UsbImportBtn",
                     static_cast<unsigned int>(usb_archives.size()));
            if (ImGui::SmallButton(imp_lbl)) {
                snprintf(m_import_path_buf, sizeof(m_import_path_buf), "%s", usb_archives[0].file_path.c_str());
                m_show_file_picker = false;
                m_show_import_modal = true;
                m_import_item_type = "GAME";
                m_import_status_msg.clear();
            }
        }
        for (size_t vi = 0; vi < usb_volumes.size(); ++vi) {
            const UsbVolumeInfo& v = usb_volumes[vi];
            ImGui::PushID(static_cast<int>(vi));
            ImGui::Text("  [%s] %s (%s) — %d image(s), %d archive(s)",
                        v.is_optical ? "CD/DVD" : "USB",
                        v.volume_label.c_str(),
                        v.mount_path.c_str(),
                        v.media_count,
                        v.archive_count);
            ImGui::SameLine();
            if (ImGui::SmallButton("Safe Eject##UsbEject")) {
                if (!KioskUsbBridge::GetInstance().SafeUnmountDevice(v.mount_path)) {
                    ui.ShowToast("Disconnected '" + v.volume_label + "' from Kiosk (OS dismount may require manual eject).", 4.0f);
                }
                ui.InvalidateCatalogCache();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    if (media.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
        ImGui::TextWrapped("Global Media Pool is empty. Click 'Browse & Add...' above to select one or more floppy (.img/.ima) or CD-ROM (.iso/.cue) images and assign them to a named Set/Group.");
        ImGui::PopStyleColor();
        RenderImportArchiveModal();
        RenderEditMediaModal();
        if (m_picker_target == FilePickerTarget::MEDIA_POOL) {
            RenderFilePickerModal();
        }
        return;
    }

    // Filter media and group by media_group ("Ungrouped" sorted last)
    const std::string mq = ToLowerCopy(m_media_search_buf);
    std::map<std::string, std::vector<const MediaPoolRecord*> > grouped;
    std::vector<const MediaPoolRecord*> ungrouped;
    std::vector<const MediaPoolRecord*> filtered_flat;
    for (size_t i = 0; i < media.size(); ++i) {
        const MediaPoolRecord& rec = media[i];
        if (!mq.empty()) {
            const bool match =
                ToLowerCopy(rec.media_group).find(mq) != std::string::npos ||
                ToLowerCopy(rec.label_title).find(mq) != std::string::npos ||
                ToLowerCopy(rec.label_subtitle).find(mq) != std::string::npos ||
                ToLowerCopy(rec.file_path).find(mq) != std::string::npos;
            if (!match) continue;
        }
        filtered_flat.push_back(&rec);
        if (rec.media_group.empty()) {
            ungrouped.push_back(&rec);
        } else {
            grouped[rec.media_group].push_back(&rec);
        }
    }

    ImGui::BeginChild("##MediaPoolScroll", ImVec2(-1.0f, -1.0f), false);
    if (filtered_flat.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
        ImGui::TextUnformatted("No media images match the current filter.");
        ImGui::PopStyleColor();
    } else if (m_media_view_mode == CaddyViewMode::SKEUOMORPHIC) {
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const int cols = (std::max)(1, static_cast<int>(avail_w / 184.0f));

        auto render_group_cards = [&](const std::string& grp_title, const std::string& raw_group_key, const std::vector<const MediaPoolRecord*>& items) {
            if (items.empty()) return;
            char hdr[192];
            std::snprintf(hdr, sizeof(hdr), "%s (%u image%s)###mp_grp_%s",
                          grp_title.c_str(),
                          static_cast<unsigned int>(items.size()),
                          items.size() == 1 ? "" : "s",
                          grp_title.c_str());
            if (ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen)) {
                char ren_btn[96];
                std::snprintf(ren_btn, sizeof(ren_btn), "Rename / Merge Group...##ren_%s", grp_title.c_str());
                if (ImGui::SmallButton(ren_btn)) {
                    m_rename_group_old_name = raw_group_key;
                    std::snprintf(m_rename_group_new_buf, sizeof(m_rename_group_new_buf), "%s", raw_group_key.c_str());
                    m_show_rename_group_modal = true;
                }
                ImGui::Spacing();
                for (size_t i = 0; i < items.size(); ++i) {
                    if (i % static_cast<size_t>(cols) != 0) {
                        ImGui::SameLine(0.0f, 14.0f);
                    }
                    const MediaPoolRecord& rec = *items[i];
                    DiskCardRenderParams params;
                    params.media = rec;
                    params.target_drive = (rec.media_type == "CDROM_ISO" || rec.media_type == "CDROM_CUE") ? "D" : "A";
                    params.slot_order = static_cast<int>(i) + 1;
                    params.is_mounted_active = false;
                    params.is_selected_focused = false;
                    if (DiskCaddyView::GetInstance().RenderMediaCard(params)) {
                        OpenEditMediaModal(rec);
                    }
                }
                ImGui::Spacing();
            }
        };

        for (std::map<std::string, std::vector<const MediaPoolRecord*> >::const_iterator it = grouped.begin();
             it != grouped.end(); ++it) {
            render_group_cards(it->first, it->first, it->second);
        }
        render_group_cards("Ungrouped", "", ungrouped);
    } else {
        // Bulk-select & assign group toolbar for List view
        if (ImGui::SmallButton("Select All##mp_sel_all")) {
            for (size_t i = 0; i < filtered_flat.size(); ++i) {
                m_media_pool_selected_ids.insert(filtered_flat[i]->id);
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Sel##mp_sel_clr")) {
            m_media_pool_selected_ids.clear();
        }
        if (!m_media_pool_selected_ids.empty()) {
            // Suggest folder name from the first selected media record
            std::string bulk_folder_hint;
            for (size_t i = 0; i < filtered_flat.size(); ++i) {
                if (m_media_pool_selected_ids.find(filtered_flat[i]->id) != m_media_pool_selected_ids.end()) {
                    bulk_folder_hint = DiskCaddyView::DeriveDefaultMediaGroup(filtered_flat[i]->file_path);
                    if (!bulk_folder_hint.empty()) break;
                }
            }
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
            ImGui::Text("Selected (%u) -> Group:", static_cast<unsigned int>(m_media_pool_selected_ids.size()));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputTextWithHint("##BulkGroupInput", "New or existing group...", m_bulk_group_buf, sizeof(m_bulk_group_buf));
            ImGui::SameLine();
            RenderGroupPickerCombo("##BulkGroupPick", m_bulk_group_buf, sizeof(m_bulk_group_buf), bulk_folder_hint);
            ImGui::SameLine();
            if (ImGui::SmallButton("Assign Group##mp_bulk_apply")) {
                std::vector<std::string> ids(m_media_pool_selected_ids.begin(), m_media_pool_selected_ids.end());
                const std::string target_grp(m_bulk_group_buf);
                if (KioskDatabase::GetInstance().AssignMediaGroupBatch(ids, target_grp)) {
                    ui.InvalidateCatalogCache();
                    char tbuf[160];
                    std::snprintf(tbuf, sizeof(tbuf), "Assigned %u image%s to '%s'",
                                  static_cast<unsigned int>(ids.size()),
                                  ids.size() == 1 ? "" : "s",
                                  target_grp.empty() ? "Ungrouped" : target_grp.c_str());
                    ui.ShowToast(tbuf);
                    m_media_pool_selected_ids.clear();
                }
            }
        }
        ImGui::Spacing();

        if (ImGui::BeginTable("##MediaPoolTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Sel", ImGuiTableColumnFlags_WidthFixed, 34.0f);
            ImGui::TableSetupColumn("Set / Group", ImGuiTableColumnFlags_WidthFixed, 155.0f);
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 125.0f);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < filtered_flat.size(); ++i) {
                const MediaPoolRecord& rec = *filtered_flat[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool is_checked = (m_media_pool_selected_ids.find(rec.id) != m_media_pool_selected_ids.end());
                char chk_id[48];
                std::snprintf(chk_id, sizeof(chk_id), "##mp_chk_%u", static_cast<unsigned int>(i));
                if (ImGui::Checkbox(chk_id, &is_checked)) {
                    if (is_checked) m_media_pool_selected_ids.insert(rec.id);
                    else m_media_pool_selected_ids.erase(rec.id);
                }

                ImGui::TableSetColumnIndex(1);
                if (rec.media_group.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
                    ImGui::TextUnformatted("Ungrouped");
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
                    ImGui::TextUnformatted(rec.media_group.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(rec.label_title.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(rec.label_subtitle.c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(DiskCaddyView::HumanReadableMediaTypeBadge(rec.media_type).c_str());
                ImGui::TableSetColumnIndex(5);
                char edit_id[48];
                std::snprintf(edit_id, sizeof(edit_id), "Edit##mp_ed_%u", static_cast<unsigned int>(i));
                if (ImGui::SmallButton(edit_id)) {
                    OpenEditMediaModal(rec);
                }
                ImGui::SameLine();
                char del_id[48];
                std::snprintf(del_id, sizeof(del_id), "Remove##mp_%u", static_cast<unsigned int>(i));
                if (ImGui::SmallButton(del_id)) {
                    if (KioskDatabase::GetInstance().DeleteMedia(rec.id)) {
                        m_media_pool_selected_ids.erase(rec.id);
                        ui.InvalidateCatalogCache();
                        ui.ShowToast("Removed media from Global Pool");
                    }
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    RenderImportArchiveModal();
    RenderEditMediaModal();
    RenderRenameGroupModal();
    if (m_picker_target == FilePickerTarget::MEDIA_POOL) {
        RenderFilePickerModal();
    }
}

void ViewCarousel::RenderContextActionsModal() {
    static const char* kCtxPopupId = "Actions###KioskContextActionsModal";
    if (m_show_context_modal && !ImGui::IsPopupOpen(kCtxPopupId)) {
        ImGui::OpenPopup(kCtxPopupId);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp) {
        const float mw = (std::min)(420.0f, (std::max)(280.0f, vp->WorkSize.x - 24.0f));
        const float mh = (std::min)(345.0f, (std::max)(240.0f, vp->WorkSize.y - 24.0f));
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(kCtxPopupId, &m_show_context_modal, flags)) {
        if (!m_show_context_modal) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        KioskUI& ui = KioskUI::GetInstance();
        ImGui::TextUnformatted(m_context_modal_target_name.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button(m_context_modal_is_item ? "Launch Now" : "Boot Machine", ImVec2(-1.0f, 32.0f))) {
            if (m_context_modal_is_item) {
                KioskManager::GetInstance().RequestLaunchItem(m_context_modal_target_id);
            } else {
                KioskManager::GetInstance().RequestLaunchMachine(m_context_modal_target_id);
            }
            m_show_context_modal = false;
            ImGui::CloseCurrentPopup();
        }

        if (m_context_modal_is_item) {
            if (ImGui::Button("Toggle Favorite", ImVec2(-1.0f, 32.0f))) {
                CatalogItemRecord rec;
                if (KioskDatabase::GetInstance().GetItem(m_context_modal_target_id, rec)) {
                    rec.is_favorite = !rec.is_favorite;
                    if (KioskDatabase::GetInstance().UpdateCatalogItem(rec)) {
                        ui.InvalidateCatalogCache();
                        ui.ShowToast(rec.is_favorite ? "Added to Favorites" : "Removed from Favorites");
                    }
                }
                m_show_context_modal = false;
                ImGui::CloseCurrentPopup();
            }

            // Allow switching Hardware Archetype for INTEGRATED_AUTOEXEC container items
            const std::vector<CatalogItemRecord>& cached_items = ui.GetCachedItems();
            const CatalogItemRecord* cur_item = nullptr;
            for (size_t i = 0; i < cached_items.size(); ++i) {
                if (cached_items[i].id == m_context_modal_target_id) {
                    cur_item = &cached_items[i];
                    break;
                }
            }
            if (cur_item && cur_item->launch_mechanism == "INTEGRATED_AUTOEXEC") {
                static const char* kArchLabels[5] = {
                    "XT 8088 (4.77 MHz, CGA)",
                    "AT 286 (12 MHz, EGA, AdLib)",
                    "386DX (33 MHz, SVGA, SB Pro 2)",
                    "486DX2 (66 MHz, S3, SB16+GUS)",
                    "Pentium MMX + 3dfx Voodoo"
                };
                static const char* kArchIds[5] = {
                    KioskArchetypes::XT_8088,
                    KioskArchetypes::AT_286,
                    KioskArchetypes::DX_386,
                    KioskArchetypes::DX2_486,
                    KioskArchetypes::PENTIUM_3DFX
                };
                int cur_arch_idx = 2;
                const std::vector<MachineRecord>& cached_machs = ui.GetCachedMachines();
                for (size_t m = 0; m < cached_machs.size(); ++m) {
                    if (cached_machs[m].id == cur_item->machine_id) {
                        for (int a = 0; a < 5; ++a) {
                            if (cached_machs[m].archetype_id == kArchIds[a]) cur_arch_idx = a;
                        }
                        break;
                    }
                }
                ImGui::Spacing();
                ImGui::Text("Hardware Configuration:");
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##CtxChangeArchetype", &cur_arch_idx, kArchLabels, 5)) {
                    std::string new_mach_id = ArchiveImporter::GetOrCreateContainerMachine(kArchIds[cur_arch_idx]);
                    if (!new_mach_id.empty()) {
                        CatalogItemRecord updated = *cur_item;
                        updated.machine_id = new_mach_id;
                        if (KioskDatabase::GetInstance().UpdateCatalogItem(updated)) {
                            ui.InvalidateCatalogCache();
                            ui.ShowToast(std::string("Updated hardware to ") + kArchLabels[cur_arch_idx]);
                        }
                    }
                }
            }
        } else {
            if (ImGui::Button("Scan Disk for Software", ImVec2(-1.0f, 32.0f))) {
                ImageCrawler::CrawlSummary summary;
                if (ImageCrawler::ScanAndCatalogMachine(m_context_modal_target_id, "", summary)) {
                    char toast_buf[192];
                    if (summary.newly_inserted > 0) {
                        std::snprintf(toast_buf, sizeof(toast_buf),
                                      "Scanned '%s': +%d new title%s (%d icon%s)",
                                      m_context_modal_target_name.c_str(),
                                      summary.newly_inserted,
                                      summary.newly_inserted == 1 ? "" : "s",
                                      summary.icons_extracted,
                                      summary.icons_extracted == 1 ? "" : "s");
                    } else {
                        std::snprintf(toast_buf, sizeof(toast_buf),
                                      "Scanned '%s': no new software found (%d already cataloged)",
                                      m_context_modal_target_name.c_str(),
                                      summary.skipped_duplicates);
                    }
                    ui.ShowToast(toast_buf, 4.0f);
                } else if (summary.newly_inserted > 0 && summary.failed_inserts > 0) {
                    char toast_buf[192];
                    std::snprintf(toast_buf, sizeof(toast_buf),
                                  "Scanned '%s': +%d added, %d failed to save",
                                  m_context_modal_target_name.c_str(),
                                  summary.newly_inserted,
                                  summary.failed_inserts);
                    ui.ShowToast(toast_buf, 4.0f);
                } else {
                    ui.ShowToast("Scan failed: " + summary.error_message, 4.0f);
                }
                m_show_context_modal = false;
                ImGui::CloseCurrentPopup();
            }
        }

        if (ImGui::Button("Session Media Caddy", ImVec2(-1.0f, 32.0f))) {
            m_show_context_modal = false;
            ImGui::CloseCurrentPopup();
            m_show_caddy_modal = true;
            m_caddy_modal_is_item = m_context_modal_is_item;
            m_caddy_modal_target_id = m_context_modal_target_id;
            m_caddy_modal_target_name = m_context_modal_target_name;
            m_caddy_modal_bindings = m_context_modal_is_item
                                         ? KioskDatabase::GetInstance().GetCaddyBindingsForItem(m_context_modal_target_id)
                                         : KioskDatabase::GetInstance().GetCaddyBindingsForMachine(m_context_modal_target_id);
        }

        if (ImGui::Button("Set as Default Boot", ImVec2(-1.0f, 32.0f))) {
            KioskConfigRecord cfg = ui.GetCachedConfig();
            cfg.default_boot_action = m_context_modal_is_item ? "SPECIFIC_ITEM" : "SPECIFIC_MACHINE";
            cfg.default_target_uuid = m_context_modal_target_id;
            if (KioskDatabase::GetInstance().UpdateConfig(cfg)) {
                KioskManager::GetInstance().ReloadConfig();
                ui.InvalidateCatalogCache();
                ui.ShowToast("Set '" + m_context_modal_target_name + "' as default auto-boot target");
            }
            m_show_context_modal = false;
            ImGui::CloseCurrentPopup();
        }

        if (ImGui::Button(m_context_modal_is_item ? "Remove from Catalog..." : "Delete Machine...", ImVec2(-1.0f, 32.0f))) {
            m_show_context_modal = false;
            ImGui::CloseCurrentPopup();
            m_show_delete_modal = true;
            m_delete_modal_is_item = m_context_modal_is_item;
            m_delete_modal_target_id = m_context_modal_target_id;
            m_delete_modal_target_name = m_context_modal_target_name;
        }

        ImGui::Spacing();
        if (ImGui::Button("Cancel [Esc/B]", ImVec2(-1.0f, 30.0f))) {
            m_show_context_modal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void ViewCarousel::RenderCaddyManagerModal() {
    static const char* kPopupId = "Session Media Caddy###CaddyManagerModal";
    if (m_show_caddy_modal && !ImGui::IsPopupOpen(kPopupId)) {
        ImGui::OpenPopup(kPopupId);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp) {
        const float mw = (std::min)(580.0f, (std::max)(320.0f, vp->WorkSize.x - 24.0f));
        const float mh = (std::min)(390.0f, (std::max)(240.0f, vp->WorkSize.y - 24.0f));
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(kPopupId, &m_show_caddy_modal, flags)) {
        if (!m_show_caddy_modal) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        const std::vector<MediaPoolRecord>& pool = KioskUI::GetInstance().GetCachedMedia();

        ImGui::Text("Target: %s   |   Bound Disks (%u):",
                    m_caddy_modal_target_name.c_str(),
                    static_cast<unsigned int>(m_caddy_modal_bindings.size()));
        ImGui::BeginChild("##BoundCaddyList", ImVec2(-1.0f, 170.0f), true);
        for (size_t i = 0; i < m_caddy_modal_bindings.size(); ++i) {
            const CaddyBindingRecord& b = m_caddy_modal_bindings[i];
            std::string display_name = b.resolved_label.empty() ? b.resolved_file_path : b.resolved_label;
            for (size_t p = 0; p < pool.size(); ++p) {
                if (pool[p].id == b.media_id) {
                    display_name = DiskCaddyView::FormatMediaDisplayName(pool[p]);
                    break;
                }
            }
            ImGui::Text("[%s: Slot %d] %s (%s)",
                        b.target_drive.c_str(),
                        b.slot_order,
                        display_name.c_str(),
                        b.resolved_media_type.c_str());
            ImGui::SameLine();
            char rm_btn[32];
            std::snprintf(rm_btn, sizeof(rm_btn), "Remove##cb_%d", b.id);
            if (ImGui::SmallButton(rm_btn)) {
                if (KioskDatabase::GetInstance().DeleteCaddyBinding(b.id)) {
                    m_caddy_modal_bindings = m_caddy_modal_is_item
                                                 ? KioskDatabase::GetInstance().GetCaddyBindingsForItem(m_caddy_modal_target_id)
                                                 : KioskDatabase::GetInstance().GetCaddyBindingsForMachine(m_caddy_modal_target_id);
                    break;
                }
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        static const char* kDrives[4] = {"A", "B", "D", "E"};
        if (m_caddy_modal_selected_drive < 0 || m_caddy_modal_selected_drive >= 4) {
            m_caddy_modal_selected_drive = 0;
        }
        const std::string active_drive = kDrives[m_caddy_modal_selected_drive];

        // Filter Global Media Pool strictly to media compatible with active_drive
        std::vector<const MediaPoolRecord*> compatible;
        for (size_t i = 0; i < pool.size(); ++i) {
            if (IsMediaCompatibleWithDrive(pool[i].media_type, active_drive)) {
                compatible.push_back(&pool[i]);
            }
        }

        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::Combo("Drive", &m_caddy_modal_selected_drive, kDrives, 4)) {
            m_caddy_modal_selected_media = 0;
        }
        ImGui::SameLine();

        if (compatible.empty()) {
            ImGui::TextWrapped((active_drive == "A" || active_drive == "B")
                                   ? "No floppy images in Media Pool for Drive A:/B:."
                                   : "No CD-ROM images in Media Pool for Drive D:/E:.");
        } else {
            if (m_caddy_modal_selected_media < 0 || m_caddy_modal_selected_media >= static_cast<int>(compatible.size())) {
                m_caddy_modal_selected_media = 0;
            }
            const MediaPoolRecord* chosen = compatible[static_cast<size_t>(m_caddy_modal_selected_media)];
            const std::string chosen_display = DiskCaddyView::FormatMediaDisplayName(*chosen);
            // Count compatible disks in the same media_group (for 1-click "Attach Set")
            std::vector<const MediaPoolRecord*> same_group;
            if (!chosen->media_group.empty()) {
                for (size_t i = 0; i < compatible.size(); ++i) {
                    if (compatible[i]->media_group == chosen->media_group) {
                        same_group.push_back(compatible[i]);
                    }
                }
            }
            const float right_btns_w = (same_group.size() > 1) ? 215.0f : 110.0f;
            const float combo_w = (std::min)(300.0f, (std::max)(120.0f, ImGui::GetContentRegionAvail().x - right_btns_w));
            ImGui::SetNextItemWidth(combo_w);
            if (ImGui::BeginCombo("Media", chosen_display.c_str())) {
                for (size_t i = 0; i < compatible.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    const bool is_sel = (static_cast<int>(i) == m_caddy_modal_selected_media);
                    std::string item_lbl = DiskCaddyView::FormatMediaDisplayName(*compatible[i]);
                    if (!compatible[i]->label_subtitle.empty()) {
                        item_lbl += "  (" + compatible[i]->label_subtitle + ")";
                    }
                    if (ImGui::Selectable(item_lbl.c_str(), is_sel)) {
                        m_caddy_modal_selected_media = static_cast<int>(i);
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Attach")) {
                chosen = compatible[static_cast<size_t>(m_caddy_modal_selected_media)];
                if (IsMediaCompatibleWithDrive(chosen->media_type, active_drive)) {
                    CaddyBindingRecord nb;
                    if (m_caddy_modal_is_item) {
                        nb.catalog_item_id = m_caddy_modal_target_id;
                    } else {
                        nb.machine_id = m_caddy_modal_target_id;
                    }
                    nb.media_id = chosen->id;
                    nb.target_drive = active_drive;
                    nb.slot_order = ComputeNextSlotOrder(m_caddy_modal_bindings, active_drive);
                    if (KioskDatabase::GetInstance().AddCaddyBinding(nb)) {
                        m_caddy_modal_bindings = m_caddy_modal_is_item
                                                     ? KioskDatabase::GetInstance().GetCaddyBindingsForItem(m_caddy_modal_target_id)
                                                     : KioskDatabase::GetInstance().GetCaddyBindingsForMachine(m_caddy_modal_target_id);
                        KioskUI::GetInstance().ShowToast("Attached disk to Drive " + nb.target_drive + ":");
                    }
                }
            }
            if (same_group.size() > 1) {
                ImGui::SameLine();
                char set_btn[48];
                std::snprintf(set_btn, sizeof(set_btn), "Attach Set (%u)",
                              static_cast<unsigned int>(same_group.size()));
                if (ImGui::Button(set_btn)) {
                    int attached = 0;
                    for (size_t g = 0; g < same_group.size(); ++g) {
                        bool already_bound = false;
                        for (size_t b = 0; b < m_caddy_modal_bindings.size(); ++b) {
                            if (m_caddy_modal_bindings[b].media_id == same_group[g]->id &&
                                m_caddy_modal_bindings[b].target_drive == active_drive) {
                                already_bound = true;
                                break;
                            }
                        }
                        if (already_bound) continue;
                        CaddyBindingRecord nb;
                        if (m_caddy_modal_is_item) nb.catalog_item_id = m_caddy_modal_target_id;
                        else nb.machine_id = m_caddy_modal_target_id;
                        nb.media_id = same_group[g]->id;
                        nb.target_drive = active_drive;
                        nb.slot_order = ComputeNextSlotOrder(m_caddy_modal_bindings, active_drive);
                        if (KioskDatabase::GetInstance().AddCaddyBinding(nb)) {
                            m_caddy_modal_bindings.push_back(nb);
                            ++attached;
                        }
                    }
                    m_caddy_modal_bindings = m_caddy_modal_is_item
                                                 ? KioskDatabase::GetInstance().GetCaddyBindingsForItem(m_caddy_modal_target_id)
                                                 : KioskDatabase::GetInstance().GetCaddyBindingsForMachine(m_caddy_modal_target_id);
                    if (attached > 0) {
                        char tbuf[128];
                        std::snprintf(tbuf, sizeof(tbuf), "Attached %d disk%s from '%s' to Drive %s:",
                                      attached, attached == 1 ? "" : "s",
                                      chosen->media_group.c_str(), active_drive.c_str());
                        KioskUI::GetInstance().ShowToast(tbuf);
                    } else {
                        KioskUI::GetInstance().ShowToast("All disks in '" + chosen->media_group + "' are already attached.");
                    }
                }
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(110.0f, 30.0f))) {
            m_show_caddy_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ViewCarousel::RenderDeleteConfirmModal() {
    static const char* kDeletePopupId = "Confirm Removal###KioskDeleteModal";
    if (m_show_delete_modal && !ImGui::IsPopupOpen(kDeletePopupId)) {
        ImGui::OpenPopup(kDeletePopupId);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp) {
        const float mw = (std::min)(420.0f, (std::max)(280.0f, vp->WorkSize.x - 24.0f));
        const float mh = (std::min)(165.0f, (std::max)(140.0f, vp->WorkSize.y - 24.0f));
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(kDeletePopupId, &m_show_delete_modal, flags)) {
        if (!m_show_delete_modal) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        ImGui::TextWrapped("Remove '%s' from the Kiosk catalog? Disk image files on host storage will not be deleted.",
                           m_delete_modal_target_name.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Confirm Remove", ImVec2(140.0f, 32.0f))) {
            bool ok = m_delete_modal_is_item
                          ? KioskDatabase::GetInstance().DeleteCatalogItem(m_delete_modal_target_id)
                          : KioskDatabase::GetInstance().DeleteMachine(m_delete_modal_target_id);
            if (ok) {
                KioskUI::GetInstance().InvalidateCatalogCache();
                KioskUI::GetInstance().ShowToast("Removed '" + m_delete_modal_target_name + "'");
            }
            m_show_delete_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 32.0f))) {
            m_show_delete_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ViewCarousel::RenderImportArchiveModal() {
    static const char* kImportPopupId = "Import GOG / Steam Archive###KioskImportModal";
    if (m_show_import_modal && !ImGui::IsPopupOpen(kImportPopupId)) {
        ImGui::OpenPopup(kImportPopupId);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp) {
        const float mw = (std::min)(560.0f, (std::max)(340.0f, vp->WorkSize.x - 24.0f));
        const float mh = (std::min)(370.0f, (std::max)(280.0f, vp->WorkSize.y - 24.0f));
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(kImportPopupId, &m_show_import_modal, flags)) {
        if (!m_show_import_modal) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        KioskUI& ui = KioskUI::GetInstance();
        const KioskThemePalette& pal = ui.GetThemePalette();

        ImGui::TextWrapped("Import a GOG offline installer (setup_*.exe), a Steam DOS/Win9x .zip archive, or an unpacked game folder.");
        ImGui::Spacing();

        ImGui::Text("Archive / Folder Path:");
        const float browse_w = 115.0f;
        ImGui::SetNextItemWidth((std::max)(140.0f, ImGui::GetContentRegionAvail().x - browse_w - 8.0f));
        ImGui::InputTextWithHint("##ImportSourcePath", "e.g. setup_game.exe or steam_game.zip", m_import_path_buf, sizeof(m_import_path_buf));
        ImGui::SameLine();
        if (ImGui::Button("Browse...##ImportBrowse", ImVec2(browse_w, 0.0f))) {
            OpenFilePicker(m_import_path_buf, FilePickerTarget::IMPORT_ARCHIVE);
        }
        ImGui::Spacing();

        ImGui::Text("Deployment Mode:");
        ImGui::RadioButton("Mode A: Appliance Container (Instant Integrated DOS)", &m_import_deploy_mode, 0);
        ImGui::RadioButton("Mode B: Inject into Machine HDD (REAL_HDD_BOOT)", &m_import_deploy_mode, 1);

        static const char* kImportArchLabels[6] = {
            "Auto-Detect (from .conf / binaries)",
            "XT 8088 (4.77 MHz, CGA, 640KB)",
            "AT 286 (12 MHz, EGA, AdLib, 2MB)",
            "386DX (33 MHz, SVGA, SB Pro 2, 8MB)",
            "486DX2 (66 MHz, S3 Trio64, SB16+GUS, 16MB)",
            "Pentium MMX + 3dfx Voodoo (64MB)"
        };
        static const char* kImportArchIds[6] = {
            "",
            KioskArchetypes::XT_8088,
            KioskArchetypes::AT_286,
            KioskArchetypes::DX_386,
            KioskArchetypes::DX2_486,
            KioskArchetypes::PENTIUM_3DFX
        };
        if (m_import_deploy_mode == 0) {
            ImGui::Spacing();
            ImGui::Text("Hardware Configuration:");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::Combo("##ImportArchetype", &m_import_archetype_idx, kImportArchLabels, 6);
        }

        std::vector<MachineRecord> hdd_machines;
        const std::vector<MachineRecord>& all_machines = ui.GetCachedMachines();
        for (size_t i = 0; i < all_machines.size(); ++i) {
            if (all_machines[i].boot_mode == "REAL_HDD_BOOT" && !all_machines[i].primary_hdd_path.empty()) {
                hdd_machines.push_back(all_machines[i]);
            }
        }

        if (m_import_deploy_mode == 1) {
            if (hdd_machines.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
                ImGui::TextWrapped("No REAL_HDD_BOOT machines available. Create one in + New Machine first.");
                ImGui::PopStyleColor();
            } else {
                if (m_import_target_machine_idx < 0 || m_import_target_machine_idx >= static_cast<int>(hdd_machines.size())) {
                    m_import_target_machine_idx = 0;
                }
                ImGui::SetNextItemWidth(300.0f);
                if (ImGui::BeginCombo("Target Machine", hdd_machines[static_cast<size_t>(m_import_target_machine_idx)].display_name.c_str())) {
                    for (size_t i = 0; i < hdd_machines.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(hdd_machines[i].display_name.c_str(), static_cast<int>(i) == m_import_target_machine_idx)) {
                            m_import_target_machine_idx = static_cast<int>(i);
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }
        }

        // Poll completion inside modal as well so modal closes cleanly on finish
        PollImportCompletion();
        if (!m_show_import_modal) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        const bool importing_now = m_import_in_progress.load();
        bool result_pending = false;
        {
            std::lock_guard<std::mutex> lock(m_import_mutex);
            result_pending = m_import_result_ready;
        }

        if (importing_now || result_pending) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
            ImGui::TextUnformatted("Importing archive in background... please wait.");
            ImGui::PopStyleColor();
        } else if (!m_import_status_msg.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
            ImGui::TextWrapped("%s", m_import_status_msg.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool can_import = !importing_now && !result_pending &&
                                (m_import_path_buf[0] != '\0') &&
                                (m_import_deploy_mode == 0 || !hdd_machines.empty());
        if (!can_import) ImGui::BeginDisabled();
        if (ImGui::Button("Start Import", ImVec2(140.0f, 32.0f))) {
            if (m_import_worker.joinable()) m_import_worker.join();
            IngestOptions opts;
            opts.source_path = m_import_path_buf;
            opts.item_type = m_import_item_type.empty() ? "GAME" : m_import_item_type;
            opts.deploy_mode = (m_import_deploy_mode == 1)
                                   ? IngestDeployMode::HDD_INJECT
                                   : IngestDeployMode::APPLIANCE_CONTAINER;
            if (m_import_deploy_mode == 0 && m_import_archetype_idx > 0 && m_import_archetype_idx < 6) {
                opts.archetype_override = kImportArchIds[m_import_archetype_idx];
            }
            if (m_import_deploy_mode == 1 && !hdd_machines.empty()) {
                opts.target_machine_id = hdd_machines[static_cast<size_t>(m_import_target_machine_idx)].id;
            }
            m_import_status_msg.clear();
            m_import_in_progress.store(true);
            try {
                m_import_worker = std::thread([this, opts]() {
                    IngestResult res;
                    try {
                        res = ArchiveImporter::ImportArchive(opts);
                    } catch (const std::exception& e) {
                        res.success = false;
                        res.error_message = std::string("Unexpected error during import: ") + e.what();
                    } catch (...) {
                        res.success = false;
                        res.error_message = "Unknown exception during archive import";
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_import_mutex);
                        m_import_result = res;
                        m_import_result_ready = true;
                    }
                    m_import_in_progress.store(false);
                });
            } catch (const std::exception& e) {
                m_import_in_progress.store(false);
                m_import_status_msg = std::string("Failed to start import thread: ") + e.what();
            } catch (...) {
                m_import_in_progress.store(false);
                m_import_status_msg = "Failed to start import thread";
            }
        }
        if (!can_import) ImGui::EndDisabled();

        ImGui::SameLine();
        if (importing_now || result_pending) ImGui::BeginDisabled();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 32.0f))) {
            m_show_import_modal = false;
            m_show_file_picker = false;
            ImGui::CloseCurrentPopup();
        }
        if (importing_now || result_pending) ImGui::EndDisabled();

        if (m_picker_target == FilePickerTarget::IMPORT_ARCHIVE) {
            RenderFilePickerModal();
        }
        ImGui::EndPopup();
    }
}

namespace {

#if defined(_WIN32)
std::wstring PickerUtf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string PickerWideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}
#endif

bool PickerIsDirectory(const std::string& path) {
    if (path.empty()) return false;
#if defined(_WIN32)
    DWORD attr = GetFileAttributesW(PickerUtf8ToWide(path).c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0);
#else
    struct stat st;
    return (stat(path.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
#endif
}

std::string PickerGetCwd() {
#if defined(_WIN32)
    wchar_t buf[1024];
    DWORD n = GetCurrentDirectoryW(1024, buf);
    if (n > 0 && n < 1024) return PickerWideToUtf8(std::wstring(buf, n));
    return "C:\\";
#else
    char buf[1024];
    if (getcwd(buf, sizeof(buf)) != nullptr) return std::string(buf);
    return "/";
#endif
}

std::string PickerGetHomeDir() {
#if defined(_WIN32)
    const char* up = std::getenv("USERPROFILE");
    if (up && *up) return std::string(up);
    return PickerGetCwd();
#else
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home);
    return "/";
#endif
}

std::string PickerNormalizeDir(const std::string& p) {
    if (p.empty()) return "";
    std::string out = p;
#if defined(_WIN32)
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '/') out[i] = '\\';
    }
    while (out.size() > 3 && out.back() == '\\') out.pop_back();
    if (out.size() == 2 && out[1] == ':') out.push_back('\\');
#else
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\\') out[i] = '/';
    }
    while (out.size() > 1 && out.back() == '/') out.pop_back();
#endif
    return out;
}

std::string PickerParentDir(const std::string& p) {
    std::string np = PickerNormalizeDir(p);
    if (np.empty()) return "";
#if defined(_WIN32)
    // At drive root like "C:\", go up to Drive List ("")
    if (np.size() <= 3 && np.size() >= 2 && np[1] == ':') return "";
    size_t pos = np.find_last_of('\\');
    if (pos == std::string::npos) return "";
    if (pos == 2 && np[1] == ':') return np.substr(0, 3);
    return np.substr(0, pos);
#else
    if (np == "/") return "/";
    size_t pos = np.find_last_of('/');
    if (pos == std::string::npos || pos == 0) return "/";
    return np.substr(0, pos);
#endif
}

std::string PickerJoin(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
#if defined(_WIN32)
    if (dir.back() == '\\' || dir.back() == '/') return dir + name;
    return dir + "\\" + name;
#else
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
#endif
}

bool PickerMatchesImportExt(const std::string& filename) {
    std::string low = ToLowerCopy(filename);
    if (low.size() >= 4) {
        std::string ext = low.substr(low.size() - 4);
        if (ext == ".exe" || ext == ".zip") return true;
    }
    return false;
}

bool PickerMatchesMediaExt(const std::string& filename) {
    std::string low = ToLowerCopy(filename);
    if (low.size() >= 4) {
        std::string ext = low.substr(low.size() - 4);
        if (ext == ".img" || ext == ".ima" || ext == ".iso" ||
            ext == ".cue" || ext == ".bin" || ext == ".vfd" || ext == ".flp") {
            return true;
        }
    }
    return false;
}

std::string PickerFormatSize(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ULL * 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%u B", static_cast<unsigned int>(bytes));
    }
    return std::string(buf);
}

std::string NormalizePathKey(const std::string& p) {
    std::string out = PickerNormalizeDir(p);
#if defined(_WIN32)
    out = ToLowerCopy(out);
#endif
    return out;
}

} // anonymous namespace

ViewCarousel::MediaAddStatus ViewCarousel::AddMediaFileToPool(
    const std::string& file_path,
    std::string& out_label,
    const std::string& media_group
) {
    out_label.clear();
    if (file_path.empty()) return MediaAddStatus::INVALID_OR_MISSING;

    InspectedMediaInfo info;
    if (!DiskCaddyView::InspectMediaFile(file_path, info)) {
        return MediaAddStatus::INVALID_OR_MISSING;
    }

    MediaPoolRecord rec;
    rec.id = GenerateSimpleMediaUuid(file_path);
    rec.file_path = file_path;
    rec.media_type = info.media_type;
    rec.physical_style = info.physical_style;
    rec.volume_label = info.volume_label;
    rec.label_title = info.label_title.empty() ? file_path : info.label_title;
    rec.label_subtitle = info.label_subtitle;
    rec.media_group = !media_group.empty() ? media_group : DiskCaddyView::DeriveDefaultMediaGroup(file_path);
    rec.has_redbook_audio = info.has_redbook_audio;
    out_label = DiskCaddyView::FormatMediaDisplayName(rec);

    if (!KioskDatabase::GetInstance().InsertMedia(rec)) {
        return MediaAddStatus::ALREADY_EXISTS;
    }
    return MediaAddStatus::ADDED;
}

int ViewCarousel::AddMultipleMediaFilesToPool(
    const std::vector<std::string>& paths,
    int& out_already_exists,
    int& out_failed,
    const std::string& media_group
) {
    out_already_exists = 0;
    out_failed = 0;
    int added = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        std::string label;
        MediaAddStatus st = AddMediaFileToPool(paths[i], label, media_group);
        if (st == MediaAddStatus::ADDED) {
            ++added;
        } else if (st == MediaAddStatus::ALREADY_EXISTS) {
            ++out_already_exists;
        } else {
            ++out_failed;
        }
    }
    if (added > 0) {
        KioskUI::GetInstance().InvalidateCatalogCache();
    }
    return added;
}

bool ViewCarousel::MatchesFilePickerFilter(const std::string& filename, FilePickerTarget target) {
    if (target == FilePickerTarget::MEDIA_POOL) {
        return PickerMatchesMediaExt(filename);
    }
    return PickerMatchesImportExt(filename);
}

void ViewCarousel::OpenFilePicker(const std::string& initial_dir, FilePickerTarget target) {
    m_picker_target = target;
    m_picker_filter_ext = true;
    m_picker_multi_selected.clear();
    m_picker_status_msg.clear();

    std::string start_dir;
    if (!initial_dir.empty()) {
        std::string norm = PickerNormalizeDir(initial_dir);
        if (PickerIsDirectory(norm)) {
            start_dir = norm;
        } else if (norm.find_last_of("\\/") != std::string::npos) {
            std::string parent = PickerParentDir(norm);
            if (!parent.empty() && PickerIsDirectory(parent)) {
                start_dir = parent;
            }
        }
    }
    if (start_dir.empty()) {
        const std::string& remembered = (target == FilePickerTarget::MEDIA_POOL)
                                            ? m_last_media_picker_dir
                                            : m_last_import_picker_dir;
        if (!remembered.empty() && PickerIsDirectory(remembered)) {
            start_dir = remembered;
        }
    }
    if (start_dir.empty()) {
        start_dir = PickerGetCwd();
    }
    m_picker_current_dir = start_dir;
    m_picker_selected_idx = -1;
    RefreshFilePickerEntries();
    m_show_file_picker = true;
}

void ViewCarousel::RefreshFilePickerEntries() {
    m_picker_entries.clear();
    m_picker_selected_idx = -1;
    m_picker_multi_selected.clear();

    if (!m_picker_current_dir.empty()) {
        if (m_picker_target == FilePickerTarget::MEDIA_POOL) {
            m_last_media_picker_dir = m_picker_current_dir;
            std::string derived = DiskCaddyView::DeriveDefaultMediaGroup(PickerJoin(m_picker_current_dir, "disk.img"));
            std::snprintf(m_picker_group_buf, sizeof(m_picker_group_buf), "%s", derived.c_str());
        } else {
            m_last_import_picker_dir = m_picker_current_dir;
        }
    } else if (m_picker_target == FilePickerTarget::MEDIA_POOL) {
        m_picker_group_buf[0] = '\0';
    }

#if defined(_WIN32)
    if (m_picker_current_dir.empty()) {
        // Drive root listing (C:\, D:\, ...)
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (mask & (1U << i)) {
                std::string drv;
                drv.push_back(static_cast<char>('A' + i));
                drv += ":\\";
                FilePickerEntry e;
                e.name = drv;
                e.full_path = drv;
                e.is_dir = true;
                m_picker_entries.push_back(e);
            }
        }
        return;
    }

    std::wstring pattern = PickerUtf8ToWide(PickerJoin(m_picker_current_dir, "*"));
    WIN32_FIND_DATAW ffd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &ffd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string name = PickerWideToUtf8(ffd.cFileName);
            if (name == "." || name == "..") continue;
            bool is_dir = ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
            if (!is_dir && m_picker_filter_ext && !MatchesFilePickerFilter(name, m_picker_target)) {
                continue;
            }
            FilePickerEntry e;
            e.name = name;
            e.full_path = PickerJoin(m_picker_current_dir, name);
            e.is_dir = is_dir;
            ULARGE_INTEGER sz;
            sz.LowPart = ffd.nFileSizeLow;
            sz.HighPart = ffd.nFileSizeHigh;
            e.size_bytes = is_dir ? 0ULL : sz.QuadPart;
            m_picker_entries.push_back(e);
        } while (FindNextFileW(h, &ffd));
        FindClose(h);
    }
#else
    if (m_picker_current_dir.empty()) {
        m_picker_current_dir = "/";
    }
    DIR* d = opendir(m_picker_current_dir.c_str());
    if (d) {
        struct dirent* ent = nullptr;
        while ((ent = readdir(d)) != nullptr) {
            std::string name(ent->d_name);
            if (name == "." || name == "..") continue;
            std::string full = PickerJoin(m_picker_current_dir, name);
            struct stat st;
            bool is_dir = false;
            uint64_t sz = 0;
            if (stat(full.c_str(), &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                sz = is_dir ? 0ULL : static_cast<uint64_t>(st.st_size);
            }
            if (!is_dir && m_picker_filter_ext && !MatchesFilePickerFilter(name, m_picker_target)) {
                continue;
            }
            FilePickerEntry e;
            e.name = name;
            e.full_path = full;
            e.is_dir = is_dir;
            e.size_bytes = sz;
            m_picker_entries.push_back(e);
        }
        closedir(d);
    }
#endif

    std::sort(m_picker_entries.begin(), m_picker_entries.end(),
              [](const FilePickerEntry& a, const FilePickerEntry& b) {
                  if (a.is_dir != b.is_dir) return a.is_dir;
                  return ToLowerCopy(a.name) < ToLowerCopy(b.name);
              });
}

void ViewCarousel::RenderFilePickerModal() {
    const bool is_media_mode = (m_picker_target == FilePickerTarget::MEDIA_POOL);
    const char* popup_id = is_media_mode
                               ? "Add Disk Images to Media Pool###KioskFilePickerModal"
                               : "Select Archive or Folder###KioskFilePickerModal";
    if (m_show_file_picker && !ImGui::IsPopupOpen(popup_id)) {
        ImGui::OpenPopup(popup_id);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp) {
        const float mw = (std::min)(680.0f, (std::max)(380.0f, vp->WorkSize.x - 24.0f));
        const float mh = (std::min)(470.0f, (std::max)(310.0f, vp->WorkSize.y - 24.0f));
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
    }

    auto apply_picked_path = [&](const std::string& picked, bool is_folder) {
        if (picked.size() >= sizeof(m_import_path_buf)) {
            m_import_status_msg = is_folder
                                      ? "Selected folder path exceeds maximum supported length."
                                      : "Selected path exceeds maximum supported length.";
        } else {
            std::snprintf(m_import_path_buf, sizeof(m_import_path_buf), "%s", picked.c_str());
            m_import_status_msg.clear();
        }
    };

    auto add_media_batch = [&](const std::vector<std::string>& paths) {
        if (paths.empty()) return;
        int already = 0;
        int failed = 0;
        const std::string grp(m_picker_group_buf);
        const int added = AddMultipleMediaFilesToPool(paths, already, failed, grp);
        char msg[192];
        if (added > 0 && already == 0 && failed == 0) {
            std::snprintf(msg, sizeof(msg), "Added %d disk image%s to Media Pool.",
                          added, added == 1 ? "" : "s");
        } else if (added > 0) {
            std::snprintf(msg, sizeof(msg), "Added %d image%s (%d already in pool, %d unsupported).",
                          added, added == 1 ? "" : "s", already, failed);
        } else if (already > 0 && failed == 0) {
            std::snprintf(msg, sizeof(msg), "%d selected image%s already in Media Pool.",
                          already, already == 1 ? "" : "s");
        } else {
            std::snprintf(msg, sizeof(msg), "Could not add selected file%s (unsupported or unreadable).",
                          paths.size() == 1 ? "" : "s");
        }
        m_picker_status_msg = msg;
        KioskUI::GetInstance().ShowToast(msg);
        m_picker_multi_selected.clear();
    };

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(popup_id, &m_show_file_picker, flags)) {
        if (!m_show_file_picker) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        KioskUI& ui = KioskUI::GetInstance();
        const KioskThemePalette& pal = ui.GetThemePalette();

        // Build lookup set of paths already in Global Media Pool (zero SQLite queries — uses cache)
        std::set<std::string> existing_pool_paths;
        if (is_media_mode) {
            const std::vector<MediaPoolRecord>& pool = ui.GetCachedMedia();
            for (size_t i = 0; i < pool.size(); ++i) {
                existing_pool_paths.insert(NormalizePathKey(pool[i].file_path));
            }
        }

        // Top navigation bar
        if (ImGui::Button("Up (..)")) {
            m_picker_current_dir = PickerParentDir(m_picker_current_dir);
            RefreshFilePickerEntries();
        }
        ImGui::SameLine();
#if defined(_WIN32)
        if (ImGui::Button("Drives")) {
            m_picker_current_dir.clear();
            RefreshFilePickerEntries();
        }
#else
        if (ImGui::Button("/")) {
            m_picker_current_dir = "/";
            RefreshFilePickerEntries();
        }
#endif
        ImGui::SameLine();
        if (ImGui::Button("Home")) {
            m_picker_current_dir = PickerGetHomeDir();
            RefreshFilePickerEntries();
        }
        ImGui::SameLine();
        if (ImGui::Button("App Dir")) {
            m_picker_current_dir = PickerGetCwd();
            RefreshFilePickerEntries();
        }
        ImGui::SameLine();
        const char* filter_label = is_media_mode ? "Disk images only" : ".exe / .zip only";
        if (ImGui::Checkbox(filter_label, &m_picker_filter_ext)) {
            RefreshFilePickerEntries();
        }

        ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
        ImGui::TextWrapped("Location: %s", m_picker_current_dir.empty() ? "(Drives)" : m_picker_current_dir.c_str());
        ImGui::PopStyleColor();
        if (is_media_mode) {
            const std::string cur_folder_hint =
                m_picker_current_dir.empty()
                    ? ""
                    : DiskCaddyView::DeriveDefaultMediaGroup(PickerJoin(m_picker_current_dir, "disk.img"));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Set / Group:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputTextWithHint("##PickerMediaGroup", "e.g. MS-DOS 6.22, WordPerfect51...", m_picker_group_buf, sizeof(m_picker_group_buf));
            ImGui::SameLine();
            RenderGroupPickerCombo("##PickerGroupPick", m_picker_group_buf, sizeof(m_picker_group_buf), cur_folder_hint);
        }
        ImGui::Separator();

        // Count addable (not-yet-in-pool) files in the current directory for media mode
        std::vector<std::string> addable_files_in_dir;
        if (is_media_mode) {
            for (size_t i = 0; i < m_picker_entries.size(); ++i) {
                if (!m_picker_entries[i].is_dir &&
                    existing_pool_paths.find(NormalizePathKey(m_picker_entries[i].full_path)) == existing_pool_paths.end()) {
                    addable_files_in_dir.push_back(m_picker_entries[i].full_path);
                }
            }
        }

        // Scrollable directory / file listing
        const float footer_reserve = m_picker_status_msg.empty() ? 48.0f : 72.0f;
        const float list_h = (std::max)(120.0f, ImGui::GetContentRegionAvail().y - footer_reserve);
        const int col_count = is_media_mode ? 4 : 3;
        if (ImGui::BeginTable("##KioskFilePickerTable", col_count,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH,
                              ImVec2(-1.0f, list_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            if (is_media_mode) {
                ImGui::TableSetupColumn("Sel", ImGuiTableColumnFlags_WidthFixed, 34.0f);
            }
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 64.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 84.0f);
            ImGui::TableHeadersRow();

            int navigate_idx = -1;
            int pick_file_idx = -1;

            for (size_t i = 0; i < m_picker_entries.size(); ++i) {
                const FilePickerEntry& entry = m_picker_entries[i];
                const bool in_pool = is_media_mode && !entry.is_dir &&
                                     (existing_pool_paths.find(NormalizePathKey(entry.full_path)) != existing_pool_paths.end());
                ImGui::TableNextRow();
                int col = 0;

                if (is_media_mode) {
                    ImGui::TableSetColumnIndex(col++);
                    if (!entry.is_dir) {
                        const bool checked = (m_picker_multi_selected.find(entry.full_path) != m_picker_multi_selected.end());
                        ImGui::PushStyleColor(ImGuiCol_Text, checked ? pal.accent : pal.text_muted);
                        ImGui::TextUnformatted(checked ? "[x]" : "[ ]");
                        ImGui::PopStyleColor();
                    }
                }

                ImGui::TableSetColumnIndex(col++);

                const char* badge = "[FILE]";
                if (entry.is_dir) {
                    badge = "[DIR]";
                } else {
                    std::string low = ToLowerCopy(entry.name);
                    if (low.size() >= 4) {
                        std::string ext = low.substr(low.size() - 4);
                        if (ext == ".zip")      badge = "[ZIP]";
                        else if (ext == ".exe") badge = "[EXE]";
                        else if (ext == ".iso") badge = "[ISO]";
                        else if (ext == ".cue") badge = "[CUE]";
                        else if (ext == ".bin") badge = "[BIN]";
                        else if (ext == ".img") badge = "[IMG]";
                        else if (ext == ".ima") badge = "[IMA]";
                        else if (ext == ".vfd") badge = "[VFD]";
                        else if (ext == ".flp") badge = "[FLP]";
                    }
                }

                const bool is_sel = is_media_mode
                                        ? ((!entry.is_dir && m_picker_multi_selected.find(entry.full_path) != m_picker_multi_selected.end()) ||
                                           (entry.is_dir && static_cast<int>(i) == m_picker_selected_idx))
                                        : (static_cast<int>(i) == m_picker_selected_idx);
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(badge, is_sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    m_picker_selected_idx = static_cast<int>(i);
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        if (entry.is_dir) {
                            navigate_idx = static_cast<int>(i);
                        } else {
                            pick_file_idx = static_cast<int>(i);
                        }
                    } else if (is_media_mode && !entry.is_dir) {
                        if (m_picker_multi_selected.find(entry.full_path) != m_picker_multi_selected.end()) {
                            m_picker_multi_selected.erase(entry.full_path);
                        } else {
                            m_picker_multi_selected.insert(entry.full_path);
                        }
                    }
                }
                ImGui::PopID();

                ImGui::TableSetColumnIndex(col++);
                if (in_pool) {
                    ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
                    ImGui::Text("%s  [IN POOL]", entry.name.c_str());
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextUnformatted(entry.name.c_str());
                }

                ImGui::TableSetColumnIndex(col++);
                if (!entry.is_dir) {
                    std::string sz_str = PickerFormatSize(entry.size_bytes);
                    ImGui::TextUnformatted(sz_str.c_str());
                } else {
                    ImGui::TextUnformatted("--");
                }
            }
            ImGui::EndTable();

            if (navigate_idx >= 0) {
                m_picker_current_dir = m_picker_entries[static_cast<size_t>(navigate_idx)].full_path;
                RefreshFilePickerEntries();
            } else if (pick_file_idx >= 0) {
                const std::string picked = m_picker_entries[static_cast<size_t>(pick_file_idx)].full_path;
                if (is_media_mode) {
                    std::vector<std::string> single(1, picked);
                    add_media_batch(single);
                } else {
                    apply_picked_path(picked, false);
                    m_show_file_picker = false;
                    ImGui::CloseCurrentPopup();
                }
            }
        }

        if (!m_picker_status_msg.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
            ImGui::TextUnformatted(m_picker_status_msg.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Separator();
        ImGui::Spacing();

        const bool has_sel = (m_picker_selected_idx >= 0 &&
                              m_picker_selected_idx < static_cast<int>(m_picker_entries.size()));
        const bool sel_is_dir = has_sel && m_picker_entries[static_cast<size_t>(m_picker_selected_idx)].is_dir;

        if (is_media_mode) {
            if (sel_is_dir && m_picker_multi_selected.empty()) {
                if (ImGui::Button("Open Folder", ImVec2(120.0f, 30.0f))) {
                    m_picker_current_dir = m_picker_entries[static_cast<size_t>(m_picker_selected_idx)].full_path;
                    RefreshFilePickerEntries();
                }
            } else {
                const int sel_count = static_cast<int>(m_picker_multi_selected.size());
                char add_btn[48];
                if (sel_count > 0) {
                    std::snprintf(add_btn, sizeof(add_btn), "Add Selected (%d)", sel_count);
                } else {
                    std::snprintf(add_btn, sizeof(add_btn), "Add Selected");
                }
                if (sel_count == 0) ImGui::BeginDisabled();
                if (ImGui::Button(add_btn, ImVec2(145.0f, 30.0f))) {
                    std::vector<std::string> batch(m_picker_multi_selected.begin(), m_picker_multi_selected.end());
                    add_media_batch(batch);
                }
                if (sel_count == 0) ImGui::EndDisabled();
            }

            ImGui::SameLine();
            const bool can_select_all = !addable_files_in_dir.empty();
            if (!can_select_all && m_picker_multi_selected.empty()) ImGui::BeginDisabled();
            if (m_picker_multi_selected.size() == addable_files_in_dir.size() && !m_picker_multi_selected.empty()) {
                if (ImGui::Button("Clear Sel", ImVec2(95.0f, 30.0f))) {
                    m_picker_multi_selected.clear();
                }
            } else {
                if (ImGui::Button("Select All", ImVec2(95.0f, 30.0f))) {
                    m_picker_multi_selected.clear();
                    for (size_t i = 0; i < addable_files_in_dir.size(); ++i) {
                        m_picker_multi_selected.insert(addable_files_in_dir[i]);
                    }
                }
            }
            if (!can_select_all && m_picker_multi_selected.empty()) ImGui::EndDisabled();

            ImGui::SameLine();
            char add_all_btn[64];
            std::snprintf(add_all_btn, sizeof(add_all_btn), "Add All in Folder (%u)",
                          static_cast<unsigned int>(addable_files_in_dir.size()));
            if (addable_files_in_dir.empty()) ImGui::BeginDisabled();
            if (ImGui::Button(add_all_btn, ImVec2(180.0f, 30.0f))) {
                add_media_batch(addable_files_in_dir);
            }
            if (addable_files_in_dir.empty()) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Done", ImVec2(85.0f, 30.0f))) {
                m_show_file_picker = false;
                ImGui::CloseCurrentPopup();
            }
        } else {
            if (!has_sel) ImGui::BeginDisabled();
            if (ImGui::Button(sel_is_dir ? "Open Folder" : "Select File", ImVec2(120.0f, 30.0f))) {
                const FilePickerEntry& chosen = m_picker_entries[static_cast<size_t>(m_picker_selected_idx)];
                if (chosen.is_dir) {
                    m_picker_current_dir = chosen.full_path;
                    RefreshFilePickerEntries();
                } else {
                    apply_picked_path(chosen.full_path, false);
                    m_show_file_picker = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            if (!has_sel) ImGui::EndDisabled();

            ImGui::SameLine();
            const bool can_use_folder = !m_picker_current_dir.empty();
            if (!can_use_folder) ImGui::BeginDisabled();
            if (ImGui::Button("Use This Folder", ImVec2(135.0f, 30.0f))) {
                apply_picked_path(m_picker_current_dir, true);
                m_show_file_picker = false;
                ImGui::CloseCurrentPopup();
            }
            if (!can_use_folder) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(95.0f, 30.0f))) {
                m_show_file_picker = false;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();
    }
}

void ViewCarousel::OpenEditMediaModal(const MediaPoolRecord& rec) {
    m_edit_media_rec = rec;
    std::snprintf(m_edit_media_group_buf, sizeof(m_edit_media_group_buf), "%s", rec.media_group.c_str());
    std::snprintf(m_edit_media_title_buf, sizeof(m_edit_media_title_buf), "%s", rec.label_title.c_str());
    std::snprintf(m_edit_media_subtitle_buf, sizeof(m_edit_media_subtitle_buf), "%s", rec.label_subtitle.c_str());
    m_edit_media_wp = rec.is_write_protected;
    m_show_edit_media_modal = true;
}

void ViewCarousel::RenderEditMediaModal() {
    static const char* kEditPopupId = "Edit Media Details###KioskEditMediaModal";
    if (m_show_edit_media_modal && !ImGui::IsPopupOpen(kEditPopupId)) {
        ImGui::OpenPopup(kEditPopupId);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp) {
        const float mw = (std::min)(520.0f, (std::max)(320.0f, vp->WorkSize.x - 24.0f));
        const float mh = (std::min)(305.0f, (std::max)(240.0f, vp->WorkSize.y - 24.0f));
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(kEditPopupId, &m_show_edit_media_modal, flags)) {
        if (!m_show_edit_media_modal) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        KioskUI& ui = KioskUI::GetInstance();
        const KioskThemePalette& pal = ui.GetThemePalette();

        ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
        ImGui::TextWrapped("File: %s", m_edit_media_rec.file_path.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Set / Group Name:");
        const std::string file_folder_hint = DiskCaddyView::DeriveDefaultMediaGroup(m_edit_media_rec.file_path);
        ImGui::SetNextItemWidth((std::max)(160.0f, ImGui::GetContentRegionAvail().x - 118.0f));
        ImGui::InputTextWithHint("##EditMediaGroup", "e.g. MS-DOS 6.22, WordPerfect51...", m_edit_media_group_buf, sizeof(m_edit_media_group_buf));
        ImGui::SameLine();
        RenderGroupPickerCombo("##EditGroupPick", m_edit_media_group_buf, sizeof(m_edit_media_group_buf), file_folder_hint);

        ImGui::Text("Disk Label / Title:");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##EditMediaTitle", "e.g. Disk 1 - Setup", m_edit_media_title_buf, sizeof(m_edit_media_title_buf));

        ImGui::Text("Description / Subtitle:");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##EditMediaSubtitle", "e.g. Bootable Installer Disk 1 of 3", m_edit_media_subtitle_buf, sizeof(m_edit_media_subtitle_buf));

        ImGui::Checkbox("Write-Protected (Read-Only)", &m_edit_media_wp);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Save Changes", ImVec2(130.0f, 30.0f))) {
            MediaPoolRecord updated = m_edit_media_rec;
            updated.media_group = m_edit_media_group_buf;
            if (m_edit_media_title_buf[0] != '\0') {
                updated.label_title = m_edit_media_title_buf;
            }
            updated.label_subtitle = m_edit_media_subtitle_buf;
            updated.is_write_protected = m_edit_media_wp;
            if (KioskDatabase::GetInstance().UpdateMedia(updated)) {
                ui.InvalidateCatalogCache();
                ui.ShowToast("Updated '" + DiskCaddyView::FormatMediaDisplayName(updated) + "'");
            }
            m_show_edit_media_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove from Pool", ImVec2(145.0f, 30.0f))) {
            if (KioskDatabase::GetInstance().DeleteMedia(m_edit_media_rec.id)) {
                ui.InvalidateCatalogCache();
                ui.ShowToast("Removed '" + m_edit_media_rec.label_title + "' from Media Pool");
            }
            m_show_edit_media_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(95.0f, 30.0f))) {
            m_show_edit_media_modal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void ViewCarousel::RenderGroupPickerCombo(
    const char* combo_id,
    char* target_buf,
    size_t buf_size,
    const std::string& folder_suggestion
) {
    if (!target_buf || buf_size == 0) return;

    KioskUI& ui = KioskUI::GetInstance();
    const std::vector<MediaPoolRecord>& media = ui.GetCachedMedia();
    const std::vector<OSDistributionRecord>& os_dists = ui.GetCachedOSDistributions();

    std::set<std::string> existing_groups;
    for (size_t i = 0; i < media.size(); ++i) {
        if (!media[i].media_group.empty()) {
            existing_groups.insert(media[i].media_group);
        }
    }

    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo(combo_id, "Pick...")) {
        // 1. Containing folder suggestion (e.g. "WordPerfect51")
        if (!folder_suggestion.empty()) {
            ImGui::BeginDisabled();
            ImGui::Selectable("-- Folder Name --", false);
            ImGui::EndDisabled();
            std::string folder_lbl = "Folder: " + folder_suggestion + "##grp_folder";
            if (ImGui::Selectable(folder_lbl.c_str(), std::string(target_buf) == folder_suggestion)) {
                std::snprintf(target_buf, buf_size, "%s", folder_suggestion.c_str());
            }
            ImGui::Separator();
        }

        // 2. Existing groups in Global Media Pool + Ungrouped
        ImGui::BeginDisabled();
        ImGui::Selectable("-- Existing Groups --", false);
        ImGui::EndDisabled();
        for (std::set<std::string>::const_iterator it = existing_groups.begin();
             it != existing_groups.end(); ++it) {
            std::string item_id = *it + "##grp_exist_" + *it;
            if (ImGui::Selectable(item_id.c_str(), std::string(target_buf) == *it)) {
                std::snprintf(target_buf, buf_size, "%s", it->c_str());
            }
        }
        if (ImGui::Selectable("(Ungrouped)##grp_none", target_buf[0] == '\0')) {
            target_buf[0] = '\0';
        }

        // 3. Seeded OS Presets
        if (!os_dists.empty()) {
            ImGui::Separator();
            ImGui::BeginDisabled();
            ImGui::Selectable("-- OS Presets --", false);
            ImGui::EndDisabled();
            for (size_t i = 0; i < os_dists.size(); ++i) {
                const std::string& os_name = os_dists[i].display_name;
                std::string item_id = os_name + "##grp_os_" + os_dists[i].id;
                if (ImGui::Selectable(item_id.c_str(), std::string(target_buf) == os_name)) {
                    std::snprintf(target_buf, buf_size, "%s", os_name.c_str());
                }
            }
        }
        ImGui::EndCombo();
    }
}

void ViewCarousel::RenderRenameGroupModal() {
    static const char* kRenamePopupId = "Rename / Merge Media Group###KioskRenameGroupModal";
    if (m_show_rename_group_modal && !ImGui::IsPopupOpen(kRenamePopupId)) {
        ImGui::OpenPopup(kRenamePopupId);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp) {
        const float mw = (std::min)(500.0f, (std::max)(320.0f, vp->WorkSize.x - 24.0f));
        const float mh = (std::min)(215.0f, (std::max)(180.0f, vp->WorkSize.y - 24.0f));
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(kRenamePopupId, &m_show_rename_group_modal, flags)) {
        if (!m_show_rename_group_modal) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        KioskUI& ui = KioskUI::GetInstance();
        const KioskThemePalette& pal = ui.GetThemePalette();

        // Derive a folder hint from the first media item in the group being renamed
        std::string folder_hint;
        const std::vector<MediaPoolRecord>& pool = ui.GetCachedMedia();
        for (size_t i = 0; i < pool.size(); ++i) {
            if (pool[i].media_group == m_rename_group_old_name) {
                folder_hint = DiskCaddyView::DeriveDefaultMediaGroup(pool[i].file_path);
                if (!folder_hint.empty()) break;
            }
        }

        ImGui::Text("Current Group: %s",
                    m_rename_group_old_name.empty() ? "Ungrouped" : m_rename_group_old_name.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
        ImGui::TextWrapped("Enter a new name or pick an existing group to merge all disks into it. Leave blank for Ungrouped.");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        ImGui::Text("New Group Name:");
        ImGui::SetNextItemWidth((std::max)(160.0f, ImGui::GetContentRegionAvail().x - 118.0f));
        ImGui::InputTextWithHint("##RenameGroupNew", "New or existing group name...", m_rename_group_new_buf, sizeof(m_rename_group_new_buf));
        ImGui::SameLine();
        RenderGroupPickerCombo("##RenameGroupPick", m_rename_group_new_buf, sizeof(m_rename_group_new_buf), folder_hint);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Apply to Group", ImVec2(140.0f, 30.0f))) {
            const std::string new_name(m_rename_group_new_buf);
            if (KioskDatabase::GetInstance().RenameMediaGroup(m_rename_group_old_name, new_name)) {
                ui.InvalidateCatalogCache();
                ui.ShowToast("Updated group to '" + (new_name.empty() ? std::string("Ungrouped") : new_name) + "'");
            }
            m_show_rename_group_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(95.0f, 30.0f))) {
            m_show_rename_group_modal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

#endif // defined(C_KIOSK)
