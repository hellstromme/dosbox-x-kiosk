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
#include "logging.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    if (m_import_worker.joinable()) {
        m_import_worker.join();
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
    if (m_show_context_modal || m_show_caddy_modal || m_show_delete_modal || m_show_import_modal) {
        if (action == KioskInputAction::ACTION_BACK) {
            m_show_context_modal = false;
            m_show_caddy_modal = false;
            m_show_delete_modal = false;
            m_show_import_modal = false;
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
    RenderCatalogCarousel("GAME", m_selected_game_idx, m_game_scroll_offset);
    RenderImportArchiveModal();
}

void ViewCarousel::RenderApplicationsTab() {
    RenderCatalogCarousel("APPLICATION", m_selected_app_idx, m_app_scroll_offset);
    RenderImportArchiveModal();
}

void ViewCarousel::RenderMachinesTab() {
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
    KioskUI& ui = KioskUI::GetInstance();
    const KioskThemePalette& pal = ui.GetThemePalette();
    const std::vector<MediaPoolRecord>& media = ui.GetCachedMedia();

    // Add media image bar (adapts cleanly to 640x480)
    const float mp_right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Add Image:");
    ImGui::SameLine();
    const float input_w = (std::min)(360.0f, (std::max)(160.0f, ImGui::GetContentRegionAvail().x - 240.0f));
    ImGui::SetNextItemWidth(input_w);
    ImGui::InputTextWithHint("##AddMediaPath", "Path to .img, .ima, .iso, or .cue file...", m_add_media_path_buf, sizeof(m_add_media_path_buf));
    ImGui::SameLine();
    if (ImGui::Button("Inspect & Add")) {
        std::string path(m_add_media_path_buf);
        if (!path.empty()) {
            InspectedMediaInfo info;
            if (!DiskCaddyView::InspectMediaFile(path, info)) {
                ui.ShowToast("Could not inspect media file (missing or unsupported): " + path);
            } else {
                MediaPoolRecord rec;
                rec.id = GenerateSimpleMediaUuid(path);
                rec.file_path = path;
                rec.media_type = info.media_type;
                rec.physical_style = info.physical_style;
                rec.volume_label = info.volume_label;
                rec.label_title = info.label_title.empty() ? path : info.label_title;
                rec.label_subtitle = info.label_subtitle;
                rec.has_redbook_audio = info.has_redbook_audio;
                if (KioskDatabase::GetInstance().InsertMedia(rec)) {
                    m_add_media_path_buf[0] = '\0';
                    ui.InvalidateCatalogCache();
                    ui.ShowToast("Added '" + rec.label_title + "' to Global Media Pool");
                } else {
                    ui.ShowToast("Media already exists or could not be saved");
                }
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
        m_show_import_modal = true;
        m_import_item_type = "GAME";
        m_import_status_msg.clear();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (media.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
        ImGui::TextWrapped("Global Media Pool is empty. Add floppy (.img/.ima) or CD-ROM (.iso/.cue) images above to bind them to machines or swap them live with F12+D.");
        ImGui::PopStyleColor();
        RenderImportArchiveModal();
        return;
    }

    ImGui::BeginChild("##MediaPoolScroll", ImVec2(-1.0f, -1.0f), false);
    if (m_media_view_mode == CaddyViewMode::SKEUOMORPHIC) {
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const int cols = (std::max)(1, static_cast<int>(avail_w / 184.0f));
        for (size_t i = 0; i < media.size(); ++i) {
            if (i % static_cast<size_t>(cols) != 0) {
                ImGui::SameLine(0.0f, 14.0f);
            }
            DiskCardRenderParams params;
            params.media = media[i];
            params.target_drive = (media[i].media_type == "CDROM_ISO" || media[i].media_type == "CDROM_CUE") ? "D" : "A";
            params.slot_order = static_cast<int>(i) + 1;
            params.is_mounted_active = false;
            params.is_selected_focused = false;
            if (DiskCaddyView::GetInstance().RenderMediaCard(params)) {
                MediaPoolRecord toggled = media[i];
                toggled.is_write_protected = !toggled.is_write_protected;
                if (KioskDatabase::GetInstance().UpdateMedia(toggled)) {
                    ui.InvalidateCatalogCache();
                    ui.ShowToast(toggled.is_write_protected
                                     ? ("Write-protected '" + toggled.label_title + "'")
                                     : ("Write-enabled '" + toggled.label_title + "'"));
                }
            }
        }
    } else {
        if (ImGui::BeginTable("##MediaPoolTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < media.size(); ++i) {
                const MediaPoolRecord& rec = media[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(rec.label_title.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(DiskCaddyView::HumanReadableMediaTypeBadge(rec.media_type).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(rec.file_path.c_str());
                ImGui::TableSetColumnIndex(3);
                char del_id[48];
                std::snprintf(del_id, sizeof(del_id), "Remove##mp_%u", static_cast<unsigned int>(i));
                if (ImGui::SmallButton(del_id)) {
                    if (KioskDatabase::GetInstance().DeleteMedia(rec.id)) {
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
            ImGui::Text("[%s: Slot %d] %s (%s)",
                        b.target_drive.c_str(),
                        b.slot_order,
                        b.resolved_label.empty() ? b.resolved_file_path.c_str() : b.resolved_label.c_str(),
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
            const float combo_w = (std::min)(280.0f, (std::max)(120.0f, ImGui::GetContentRegionAvail().x - 120.0f));
            ImGui::SetNextItemWidth(combo_w);
            if (ImGui::BeginCombo("Media", chosen->label_title.c_str())) {
                for (size_t i = 0; i < compatible.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    const bool is_sel = (static_cast<int>(i) == m_caddy_modal_selected_media);
                    if (ImGui::Selectable(compatible[i]->label_title.c_str(), is_sel)) {
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
            OpenFilePicker(m_import_path_buf);
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

        // Poll background import worker completion without blocking the UI loop
        if (!m_import_in_progress.load()) {
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
            if (finished) {
                if (m_import_worker.joinable()) m_import_worker.join();
                if (done_res.success) {
                    ui.InvalidateCatalogCache();
                    ui.ShowToast("Imported '" + done_res.title + "' (" + done_res.archetype_id + ")");
                    m_import_path_buf[0] = '\0';
                    m_import_status_msg.clear();
                    m_show_import_modal = false;
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    return;
                } else {
                    m_import_status_msg = "Import failed: " + done_res.error_message;
                }
            }
        }

        const bool importing_now = m_import_in_progress.load();
        if (importing_now) {
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

        const bool can_import = !importing_now &&
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
            m_import_worker = std::thread([this, opts]() {
                IngestResult res = ArchiveImporter::ImportArchive(opts);
                {
                    std::lock_guard<std::mutex> lock(m_import_mutex);
                    m_import_result = res;
                    m_import_result_ready = true;
                }
                m_import_in_progress.store(false);
            });
        }
        if (!can_import) ImGui::EndDisabled();

        ImGui::SameLine();
        if (importing_now) ImGui::BeginDisabled();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 32.0f))) {
            m_show_import_modal = false;
            m_show_file_picker = false;
            ImGui::CloseCurrentPopup();
        }
        if (importing_now) ImGui::EndDisabled();

        RenderFilePickerModal();
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

} // anonymous namespace

void ViewCarousel::OpenFilePicker(const std::string& initial_dir) {
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
            if (!is_dir && m_picker_filter_archives && !PickerMatchesImportExt(name)) {
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
            if (!is_dir && m_picker_filter_archives && !PickerMatchesImportExt(name)) {
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
    static const char* kPickerPopupId = "Select Archive or Folder###KioskFilePickerModal";
    if (m_show_file_picker && !ImGui::IsPopupOpen(kPickerPopupId)) {
        ImGui::OpenPopup(kPickerPopupId);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp) {
        const float mw = (std::min)(620.0f, (std::max)(360.0f, vp->WorkSize.x - 24.0f));
        const float mh = (std::min)(440.0f, (std::max)(300.0f, vp->WorkSize.y - 24.0f));
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(kPickerPopupId, &m_show_file_picker, flags)) {
        if (!m_show_file_picker) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        const KioskThemePalette& pal = KioskUI::GetInstance().GetThemePalette();

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
        if (ImGui::Checkbox(".exe / .zip only", &m_picker_filter_archives)) {
            RefreshFilePickerEntries();
        }

        ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
        ImGui::TextWrapped("Location: %s", m_picker_current_dir.empty() ? "(Drives)" : m_picker_current_dir.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Scrollable directory / file listing
        const float list_h = (std::max)(120.0f, ImGui::GetContentRegionAvail().y - 46.0f);
        if (ImGui::BeginTable("##KioskFilePickerTable", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH,
                              ImVec2(-1.0f, list_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 64.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 84.0f);
            ImGui::TableHeadersRow();

            int navigate_idx = -1;
            int pick_file_idx = -1;

            for (size_t i = 0; i < m_picker_entries.size(); ++i) {
                const FilePickerEntry& entry = m_picker_entries[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                const char* badge = "[FILE]";
                if (entry.is_dir) {
                    badge = "[DIR]";
                } else {
                    std::string low = ToLowerCopy(entry.name);
                    if (low.size() >= 4 && low.substr(low.size() - 4) == ".zip") badge = "[ZIP]";
                    else if (low.size() >= 4 && low.substr(low.size() - 4) == ".exe") badge = "[EXE]";
                }

                const bool is_sel = (static_cast<int>(i) == m_picker_selected_idx);
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(badge, is_sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    m_picker_selected_idx = static_cast<int>(i);
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        if (entry.is_dir) navigate_idx = static_cast<int>(i);
                        else pick_file_idx = static_cast<int>(i);
                    }
                }
                ImGui::PopID();

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(entry.name.c_str());

                ImGui::TableSetColumnIndex(2);
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
                const std::string& picked = m_picker_entries[static_cast<size_t>(pick_file_idx)].full_path;
                if (picked.size() >= sizeof(m_import_path_buf)) {
                    m_import_status_msg = "Selected path exceeds maximum supported length.";
                } else {
                    std::snprintf(m_import_path_buf, sizeof(m_import_path_buf), "%s", picked.c_str());
                    m_import_status_msg.clear();
                }
                m_show_file_picker = false;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::Separator();
        ImGui::Spacing();

        const bool has_sel = (m_picker_selected_idx >= 0 &&
                              m_picker_selected_idx < static_cast<int>(m_picker_entries.size()));
        const bool sel_is_dir = has_sel && m_picker_entries[static_cast<size_t>(m_picker_selected_idx)].is_dir;

        if (!has_sel) ImGui::BeginDisabled();
        if (ImGui::Button(sel_is_dir ? "Open Folder" : "Select File", ImVec2(120.0f, 30.0f))) {
            const FilePickerEntry& chosen = m_picker_entries[static_cast<size_t>(m_picker_selected_idx)];
            if (chosen.is_dir) {
                m_picker_current_dir = chosen.full_path;
                RefreshFilePickerEntries();
            } else {
                if (chosen.full_path.size() >= sizeof(m_import_path_buf)) {
                    m_import_status_msg = "Selected path exceeds maximum supported length.";
                } else {
                    std::snprintf(m_import_path_buf, sizeof(m_import_path_buf), "%s", chosen.full_path.c_str());
                    m_import_status_msg.clear();
                }
                m_show_file_picker = false;
                ImGui::CloseCurrentPopup();
            }
        }
        if (!has_sel) ImGui::EndDisabled();

        ImGui::SameLine();
        const bool can_use_folder = !m_picker_current_dir.empty();
        if (!can_use_folder) ImGui::BeginDisabled();
        if (ImGui::Button("Use This Folder", ImVec2(135.0f, 30.0f))) {
            if (m_picker_current_dir.size() >= sizeof(m_import_path_buf)) {
                m_import_status_msg = "Selected folder path exceeds maximum supported length.";
            } else {
                std::snprintf(m_import_path_buf, sizeof(m_import_path_buf), "%s", m_picker_current_dir.c_str());
                m_import_status_msg.clear();
            }
            m_show_file_picker = false;
            ImGui::CloseCurrentPopup();
        }
        if (!can_use_folder) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(95.0f, 30.0f))) {
            m_show_file_picker = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

#endif // defined(C_KIOSK)
