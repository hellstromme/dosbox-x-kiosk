#ifndef DOSBOX_KIOSK_VIEW_CAROUSEL_H
#define DOSBOX_KIOSK_VIEW_CAROUSEL_H

#include "../kiosk_types.h"
#include "../ingest/archive_importer.h"
#include "input_router.h"
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
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
    enum class FilePickerTarget {
        IMPORT_ARCHIVE = 0,
        MEDIA_POOL = 1
    };

    enum class MediaAddStatus {
        ADDED = 0,
        ALREADY_EXISTS,
        INVALID_OR_MISSING
    };

    static std::vector<CatalogItemRecord> FilterAndSortCatalogItems(
        const std::vector<CatalogItemRecord>& all_items,
        const std::string& item_type,
        const std::string& search_filter
    );
    static bool IsMediaCompatibleWithDrive(const std::string& media_type, const std::string& target_drive);
    static int ComputeNextSlotOrder(const std::vector<CaddyBindingRecord>& bindings, const std::string& target_drive);
    static bool MatchesFilePickerFilter(const std::string& filename, FilePickerTarget target);
    static MediaAddStatus AddMediaFileToPool(const std::string& file_path, std::string& out_label, const std::string& media_group = "");
    static int AddMultipleMediaFilesToPool(const std::vector<std::string>& paths, int& out_already_exists, int& out_failed, const std::string& media_group = "");

    void SetLastMediaPickerDir(const std::string& dir) { m_last_media_picker_dir = dir; }
    std::string GetLastMediaPickerDir() const { return m_last_media_picker_dir; }
    std::string GetCurrentPickerDir() const { return m_picker_current_dir; }
    void OpenFilePickerForTest(const std::string& initial_dir, FilePickerTarget target) {
        OpenFilePicker(initial_dir, target);
    }

    void ResetTransientModals();
    void WaitForPendingImport();
    void PollImportCompletion();
    bool IsContextModalOpen() const { return m_show_context_modal; }

private:
    ViewCarousel() = default;
    ~ViewCarousel();

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
    void RenderContextActionsModal();
    void RenderCaddyManagerModal();
    void RenderDeleteConfirmModal();
    void RenderImportArchiveModal();
    void RenderFilePickerModal();
    void RenderEditMediaModal();
    void OpenEditMediaModal(const MediaPoolRecord& rec);
    void RenderRenameGroupModal();
    void RenderGroupPickerCombo(const char* combo_id, char* target_buf, size_t buf_size, const std::string& folder_suggestion);
    void OpenFilePicker(const std::string& initial_dir = "",
                        FilePickerTarget target = FilePickerTarget::IMPORT_ARCHIVE);
    void RefreshFilePickerEntries();

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
    char m_add_media_path_buf[1024] = {0};
    char m_media_search_buf[128] = {0};
    std::set<std::string> m_media_pool_selected_ids;
    char m_bulk_group_buf[128] = {0};

    // Rename / Merge Group modal state
    bool m_show_rename_group_modal = false;
    std::string m_rename_group_old_name;
    char m_rename_group_new_buf[128] = {0};

    // Edit Media Details modal state
    bool m_show_edit_media_modal = false;
    MediaPoolRecord m_edit_media_rec;
    char m_edit_media_group_buf[128] = {0};
    char m_edit_media_title_buf[128] = {0};
    char m_edit_media_subtitle_buf[256] = {0};
    bool m_edit_media_wp = false;

    // Session Caddy modal state
    bool m_show_caddy_modal = false;
    bool m_caddy_modal_is_item = false;
    std::string m_caddy_modal_target_id;
    std::string m_caddy_modal_target_name;
    std::vector<CaddyBindingRecord> m_caddy_modal_bindings;
    int m_caddy_modal_selected_drive = 0; // 0=A, 1=B, 2=D, 3=E
    int m_caddy_modal_selected_media = 0;

    // Context Actions modal state (M / Gamepad Start — controller-reachable Set Default / Remove / etc.)
    bool m_show_context_modal = false;
    bool m_context_modal_is_item = false;
    std::string m_context_modal_target_id;
    std::string m_context_modal_target_name;

    // Delete confirmation modal state
    bool m_show_delete_modal = false;
    bool m_delete_modal_is_item = false;
    std::string m_delete_modal_target_id;
    std::string m_delete_modal_target_name;

    // GOG / Steam archive import modal state
    bool m_show_import_modal = false;
    char m_import_path_buf[1024] = {0};
    int m_import_deploy_mode = 0; // 0 = Mode A (Appliance Container), 1 = Mode B (HDD Inject)
    int m_import_archetype_idx = 0; // 0 = Auto-Detect, 1..5 = XT_8088 .. PENTIUM_3DFX
    int m_import_target_machine_idx = 0;
    std::string m_import_item_type = "GAME";
    std::string m_import_status_msg;
    std::thread m_import_worker;
    std::mutex m_import_mutex;
    std::atomic<bool> m_import_in_progress{false};
    bool m_import_result_ready = false;
    IngestResult m_import_result;

    // In-Kiosk Dear ImGui file/folder picker state (no OS dialogs, stays fullscreen)
    struct FilePickerEntry {
        std::string name;
        std::string full_path;
        bool is_dir = false;
        uint64_t size_bytes = 0;
    };
    bool m_show_file_picker = false;
    bool m_picker_filter_ext = true;
    FilePickerTarget m_picker_target = FilePickerTarget::IMPORT_ARCHIVE;
    std::string m_picker_current_dir;
    std::string m_last_media_picker_dir;
    std::string m_last_import_picker_dir;
    int m_picker_selected_idx = -1;
    std::set<std::string> m_picker_multi_selected;
    std::string m_picker_status_msg;
    char m_picker_group_buf[128] = {0};
    std::vector<FilePickerEntry> m_picker_entries;
};

#endif // DOSBOX_KIOSK_VIEW_CAROUSEL_H
