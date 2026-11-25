#ifndef DOSBOX_KIOSK_VIEW_CAROUSEL_H
#define DOSBOX_KIOSK_VIEW_CAROUSEL_H

#include "../kiosk_types.h"
#include "input_router.h"
#include <string>
#include <vector>

struct ImDrawList;

enum class KioskHubTab {
    GAMES = 0,
    APPLICATIONS,
    MACHINES,
    MEDIA_POOL,
    NEW_MACHINE,
    SETTINGS
};

class ViewCarousel {
public:
    static ViewCarousel& GetInstance();

    // Renders the active Big-Picture tab (GAMES, APPLICATIONS, MACHINES, MEDIA_POOL)
    void RenderGamesTab();
    void RenderApplicationsTab();
    void RenderMachinesTab();
    void RenderMediaPoolTab();

    // Unified input action handler for the currently active tab
    bool HandleAction(KioskHubTab active_tab, KioskInputAction action);

    // Search filter & selection helpers (also used by unit tests)
    void SetSearchFilter(const std::string& filter);
    std::string GetSearchFilter() const { return m_search_filter; }

    int GetSelectedGameIndex() const { return m_selected_game_idx; }
    int GetSelectedAppIndex() const { return m_selected_app_idx; }
    int GetSelectedMachineIndex() const { return m_selected_machine_idx; }

    // Pure data helpers (also used by unit tests)
    static std::vector<CatalogItemRecord> FilterAndSortCatalogItems(
        const std::vector<CatalogItemRecord>& all_items,
        const std::string& item_type,
        const std::string& search_filter
    );
    static bool IsMediaCompatibleWithDrive(const std::string& media_type, const std::string& target_drive);
    static int ComputeNextSlotOrder(const std::vector<CaddyBindingRecord>& bindings, const std::string& target_drive);

    void ResetTransientModals();

private:
    ViewCarousel() = default;

    void RenderCatalogCarousel(const std::string& item_type, int& selected_idx, float& scroll_offset);
    void DrawProceduralBoxArtCard(
        ImDrawList* draw_list,
        float x, float y, float w, float h,
        const CatalogItemRecord& item,
        const std::string& archetype_id,
        bool is_focused
    );
    void DrawProceduralMachineCase(
        ImDrawList* draw_list,
        float x, float y, float w, float h,
        const MachineRecord& machine,
        bool is_focused
    );
    void RenderCaddyManagerModal();
    void RenderDeleteConfirmModal();

    int m_selected_game_idx = 0;
    int m_selected_app_idx = 0;
    int m_selected_machine_idx = 0;
    float m_game_scroll_offset = 0.0f;
    float m_app_scroll_offset = 0.0f;
    float m_machine_scroll_offset = 0.0f;

    std::string m_search_filter;
    char m_search_buf[128] = {0};

    // Media Pool tab state
    CaddyViewMode m_media_view_mode = CaddyViewMode::SKEUOMORPHIC;
    char m_add_media_path_buf[512] = {0};

    // Session Caddy modal state
    bool m_show_caddy_modal = false;
    bool m_caddy_modal_is_item = false;
    std::string m_caddy_modal_target_id;
    std::string m_caddy_modal_target_name;
    std::vector<CaddyBindingRecord> m_caddy_modal_bindings;
    int m_caddy_modal_selected_drive = 0; // 0=A, 1=B, 2=D, 3=E
    int m_caddy_modal_selected_media = 0;

    // Delete confirmation modal state
    bool m_show_delete_modal = false;
    bool m_delete_modal_is_item = false;
    std::string m_delete_modal_target_id;
    std::string m_delete_modal_target_name;
};

#endif // DOSBOX_KIOSK_VIEW_CAROUSEL_H
