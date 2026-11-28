#include "config.h"

#if defined(C_KIOSK)

#include "view_watchdog.h"
#include "kiosk_ui.h"
#include "../kiosk_main.h"
#include "../kiosk_db.h"
#include "../machine/archetypes.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

std::string ShortArchetypeBadge(const std::string& archetype_id) {
    if (archetype_id == KioskArchetypes::XT_8088)      return "IBM PC/XT 8088";
    if (archetype_id == KioskArchetypes::AT_286)       return "IBM PC/AT 286";
    if (archetype_id == KioskArchetypes::DX_386)       return "386DX-33 SVGA";
    if (archetype_id == KioskArchetypes::DX2_486)      return "486DX2-66 VLB";
    if (archetype_id == KioskArchetypes::PENTIUM_3DFX) return "PENTIUM + 3DFX";
    return "386DX DEFAULT";
}

} // anonymous namespace

ViewWatchdog& ViewWatchdog::GetInstance() {
    static ViewWatchdog instance;
    return instance;
}

void ViewWatchdog::RefreshPreview() {
    const uint64_t target_gen = m_preview_generation.load(std::memory_order_acquire) +
                                KioskDatabase::GetInstance().GetChangeGeneration();
    KioskManager& mgr = KioskManager::GetInstance();
    KioskDatabase& db = KioskDatabase::GetInstance();
    const KioskConfigRecord cfg = mgr.GetConfig();
    const LaunchContext ctx = mgr.ResolveDefaultBootContext();

    m_preview = WatchdogTargetPreview();
    m_preview.total_timeout_sec = (cfg.boot_timeout_sec > 0) ? cfg.boot_timeout_sec : 30;

    if (!ctx.catalog_item_id.empty() && db.IsOpen()) {
        CatalogItemRecord item;
        if (db.GetItem(ctx.catalog_item_id, item)) {
            m_preview.title = item.title;
            m_preview.subtitle = (item.item_type == "APPLICATION")
                                     ? "Default Application Launch"
                                     : "Default Game Launch";
            MachineRecord mach;
            if (!item.machine_id.empty() && db.GetMachine(item.machine_id, mach)) {
                m_preview.archetype_badge = ShortArchetypeBadge(mach.archetype_id);
                m_preview.boot_mode_badge = (mach.boot_mode == "REAL_HDD_BOOT")
                                                ? "REAL HDD BOOT"
                                                : "INTEGRATED DOS";
            } else {
                m_preview.archetype_badge = "386DX DEFAULT";
                m_preview.boot_mode_badge = "INTEGRATED DOS";
            }
            m_loaded_preview_generation = target_gen;
            return;
        }
    }

    if (!ctx.machine_id.empty() && db.IsOpen()) {
        MachineRecord mach;
        if (db.GetMachine(ctx.machine_id, mach)) {
            m_preview.title = mach.display_name;
            m_preview.subtitle = "Default Retro Machine";
            m_preview.archetype_badge = ShortArchetypeBadge(mach.archetype_id);
            m_preview.boot_mode_badge = (mach.boot_mode == "REAL_HDD_BOOT")
                                            ? "REAL HDD BOOT"
                                            : "INTEGRATED DOS";
            m_loaded_preview_generation = target_gen;
            return;
        }
    }

    m_preview.title = "Quick Start: Default DOSBox-X";
    m_preview.subtitle = "Integrated DOS Shell with C: Drive";
    m_preview.archetype_badge = "386DX-33 SVGA";
    m_preview.boot_mode_badge = "INTEGRATED DOS";
    m_loaded_preview_generation = target_gen;
}

void ViewWatchdog::Render(float remaining_sec) {
    if (m_loaded_preview_generation != m_preview_generation.load(std::memory_order_acquire) +
                                       KioskDatabase::GetInstance().GetChangeGeneration()) {
        RefreshPreview();
    }

    const KioskThemePalette& pal = KioskUI::GetInstance().GetThemePalette();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 vp_pos = viewport ? viewport->WorkPos : ImVec2(0.0f, 0.0f);
    const ImVec2 vp_size = viewport ? viewport->WorkSize : ImVec2(1280.0f, 720.0f);

    const float panel_w = (std::min)(680.0f, (std::max)(320.0f, vp_size.x - 24.0f));
    const float panel_h = (std::min)(370.0f, (std::max)(260.0f, vp_size.y - 24.0f));

    ImGui::SetNextWindowPos(
        ImVec2(vp_pos.x + vp_size.x * 0.5f, vp_pos.y + vp_size.y * 0.5f),
        ImGuiCond_Always,
        ImVec2(0.5f, 0.5f)
    );
    ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, pal.card_bg);
    ImGui::PushStyleColor(ImGuiCol_Border, pal.accent);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 24.0f));

    if (ImGui::Begin("##KioskWatchdogScreen", nullptr, flags)) {
        // Header
        ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
        ImGui::SetWindowFontScale(1.45f);
        ImGui::Text("DOSBOX-X KIOSK EDITION");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
        ImGui::Text("Turn-Key Retro Computing Appliance  |  Auto-Boot Watchdog");
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Target Preview Card
        ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.bg_dark);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        if (ImGui::BeginChild("##WatchdogTargetCard", ImVec2(-1.0f, 118.0f), true)) {
            ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
            ImGui::Text("NEXT SCHEDULED BOOT TARGET");
            ImGui::PopStyleColor();
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Text, pal.text_primary);
            ImGui::SetWindowFontScale(1.30f);
            ImGui::TextUnformatted(m_preview.title.c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();

            ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
            ImGui::TextUnformatted(m_preview.subtitle.c_str());
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
            ImGui::Text("[%s]   [%s]",
                        m_preview.archetype_badge.c_str(),
                        m_preview.boot_mode_badge.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Spacing();

        // Animated Countdown Progress Bar
        const float total = static_cast<float>(m_preview.total_timeout_sec > 0 ? m_preview.total_timeout_sec : 30);
        float clamped = remaining_sec;
        if (clamped < 0.0f) clamped = 0.0f;
        if (clamped > total) clamped = total;
        const float fraction = (total > 0.0f) ? (clamped / total) : 0.0f;

        int secs = static_cast<int>(std::ceil(clamped));
        if (secs < 0) secs = 0;

        char overlay_buf[96];
        std::snprintf(overlay_buf, sizeof(overlay_buf), "Auto-booting in %d second%s...", secs, (secs == 1 ? "" : "s"));

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, pal.accent);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, pal.bg_dark);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 28.0f), overlay_buf);
        ImGui::PopStyleColor(2);

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, pal.text_muted);
        ImGui::TextWrapped("Press any key, move the mouse, or press any gamepad button to pause countdown and open the Big-Picture Kiosk Hub.");
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Spacing();

        // Direct action buttons
        const float btn_w = (ImGui::GetContentRegionAvail().x - 24.0f) / 3.0f;
        if (ImGui::Button("Boot Now", ImVec2(btn_w, 36.0f))) {
            KioskManager::GetInstance().TriggerDefaultBootAction();
        }
        ImGui::SameLine(0.0f, 12.0f);
        if (ImGui::Button("Open Kiosk Menu", ImVec2(btn_w, 36.0f))) {
            KioskManager::GetInstance().CancelWatchdog();
        }
        ImGui::SameLine(0.0f, 12.0f);
        if (ImGui::Button("Exit Kiosk", ImVec2(btn_w, 36.0f))) {
            KioskManager::GetInstance().RequestApplianceExit();
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

#endif // defined(C_KIOSK)
